#!/usr/bin/env python3
"""Offline audit of the frozen SDK. No downloads; PE imports include delay imports.

--write-lock is an explicit maintainer operation, never used by CMake/release.py.
An SDK audit PASS is not a corresponding-source or contract approval.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOCK = ROOT / "recorder/third_party/ffmpeg.lock.json"
PINNED_VERSION = "n8.1.2-51-g7ba069f4f1-20260908"
PINNED_ARCHIVE_SHA256 = "b74c95a1976622f93f9c3ce73551683f4167646b155d9bfe9f2e132e5503e7eb"
# Explicit OS/driver boundaries, never 'whatever happens to exist in System32'.
SYSTEM_DLLS = set("""advapi32.dll avicap32.dll avrt.dll bcrypt.dll bcryptprimitives.dll
 cabinet.dll cfgmgr32.dll comctl32.dll comdlg32.dll crypt32.dll cryptbase.dll cryptsp.dll
 d2d1.dll d3d11.dll d3d12.dll d3d9.dll d3dcompiler_47.dll dbghelp.dll dcomp.dll
 dnsapi.dll dwmapi.dll dwrite.dll dxgi.dll dxva2.dll gdi32.dll glu32.dll hid.dll
 imm32.dll iphlpapi.dll kernel32.dll kernelbase.dll mf.dll mfplat.dll mfreadwrite.dll
 mpr.dll msvcrt.dll msimg32.dll mswsock.dll ncrypt.dll netapi32.dll normaliz.dll
 ntdll.dll ole32.dll oleacc.dll oleaut32.dll opengl32.dll powrprof.dll propsys.dll
 psapi.dll rpcrt4.dll secur32.dll setupapi.dll shell32.dll shlwapi.dll strmiids.dll
 user32.dll userenv.dll usp10.dll ucrtbase.dll uxtheme.dll version.dll winhttp.dll
 wininet.dll winmm.dll winspool.drv wintrust.dll ws2_32.dll wtsapi32.dll""".split())
DRIVER_DLLS = {"nvcuda.dll", "nvencodeapi64.dll"}


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def dependency_kind(name):
    name = name.lower()
    if name in SYSTEM_DLLS or name.startswith(("api-ms-win-", "ext-ms-win-")):
        return "windows"
    if name in DRIVER_DLLS:
        return "driver"
    return "bundled"


def pe_imports(path):
    """Read PE32/PE32+ import and delay-import tables without executing the image."""
    data = Path(path).read_bytes()

    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError(f"truncated PE: {path}")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError(f"not a PE image: {path}")
    pe = unpack("<I", 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"invalid PE signature: {path}")
    machine, count = unpack("<HH", pe + 4)
    optional_size = unpack("<H", pe + 20)[0]
    optional = pe + 24
    magic = unpack("<H", optional)[0]
    if magic not in (0x10b, 0x20b):
        raise ValueError("unsupported PE optional header")
    is64 = magic == 0x20b
    image_base = unpack("<Q" if is64 else "<I", optional + (24 if is64 else 28))[0]
    directories = optional + (112 if is64 else 96)
    directory_count = unpack("<I", directories - 4)[0]
    sections = []
    for i in range(count):
        virtual_size, virtual, raw_size, raw = unpack("<IIII", optional + optional_size + 40 * i + 8)
        sections.append((virtual, max(virtual_size, raw_size), raw, raw_size))

    def offset(rva):
        for virtual, size, raw, raw_size in sections:
            if virtual <= rva < virtual + size and rva - virtual < raw_size:
                return raw + rva - virtual
        raise ValueError(f"PE RVA outside raw sections: {rva:x}")

    def dll_name(rva):
        start = offset(rva)
        end = data.find(b"\0", start, start + 512)
        if end < 0:
            raise ValueError("unterminated PE import name")
        name = data[start:end].decode("ascii").lower()
        if not name or "/" in name or "\\" in name or ":" in name:
            raise ValueError("invalid PE import name")
        return name

    imports, delay = set(), set()
    for index, width, dest in ((1, 20, imports), (13, 32, delay)):
        if directory_count <= index:
            continue
        rva, size = unpack("<II", directories + 8 * index)
        if not rva:
            continue
        start = offset(rva)
        terminated = False
        for pos in range(start, start + size, width):
            fields = unpack("<" + "I" * (width // 4), pos)
            if not any(fields):
                terminated = True
                break
            name_rva = fields[3] if index == 1 else fields[1]
            if index == 13 and not fields[0] & 1:
                name_rva -= image_base
            dest.add(dll_name(name_rva))
        if not terminated:
            raise ValueError("unterminated PE import table")
    return {"machine": hex(machine), "imports": sorted(imports), "delay_imports": sorted(delay)}


def dependency_closure(images):
    names = {str(Path(name)).replace("\\", "/").lower() for name in images}
    # SDK images share bin/; installed payload images share payload/. A DLL in
    # an unrelated plugin subdirectory cannot satisfy an application's import.
    parents = [Path(name).parent for name in images]
    common = Path(os.path.commonpath([str(p) for p in parents])) if parents else Path(".")
    missing, system, drivers = set(), set(), set()
    for image, record in images.items():
        for name in record["imports"] + record["delay_imports"]:
            candidates = {str(parent / name).replace("\\", "/").lower() for parent in (Path(image).parent, common)}
            if candidates & names:
                continue
            kind = dependency_kind(name)
            (system if kind == "windows" else drivers if kind == "driver" else missing).add(name)
    return {"missing": sorted(missing), "windows": sorted(system), "drivers": sorted(drivers)}


def inventory(sdk):
    groups = {
        "headers": sorted(p for p in (sdk / "include").rglob("*") if p.is_file()),
        "import_libraries": sorted(p for p in (sdk / "lib").rglob("*") if p.is_file()),
        "dlls": sorted((sdk / "bin").glob("*.dll")),
        "executables": sorted((sdk / "bin").glob("*.exe")),
        "license_files": [sdk / "LICENSE.txt"],
    }
    result = {}
    for group, paths in groups.items():
        result[group] = {}
        for path in paths:
            record = {"sha256": sha256(path), "size": path.stat().st_size}
            if group in ("dlls", "executables"):
                record.update(pe_imports(path))
            result[group][path.relative_to(sdk).as_posix()] = record
    if any(not result[key] for key in groups):
        raise ValueError("incomplete SDK inventory")
    return result


def verify_archive_files(archive, files):
    with zipfile.ZipFile(archive) as package:
        for group in files.values():
            for relative, record in group.items():
                matches = [n for n in package.namelist() if n == relative or n.endswith("/" + relative)]
                if len(matches) != 1 or hashlib.sha256(package.read(matches[0])).hexdigest() != record["sha256"]:
                    raise ValueError("SDK file differs from frozen archive: " + relative)


def runtime_info(sdk, dlls):
    def run(*args):
        return subprocess.run([str(sdk / "bin/ffmpeg.exe"), *args], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", check=True, timeout=20).stdout
    version_text, license_text = run("-version"), run("-L")
    version = re.search(r"^ffmpeg version (\S+)", version_text)
    compiler = re.search(r"^built with (.+)$", version_text, re.M)
    configuration = re.search(r"^configuration: (.+)$", version_text, re.M)
    if not all((version, compiler, configuration)):
        raise ValueError("missing FFmpeg runtime metadata")
    result = {"version": version[1], "compiler": compiler[1],
              "configuration": shlex.split(configuration[1]), "license_output": license_text.strip(),
              "libraries": {}}
    if os.name != "nt":
        raise ValueError("runtime SDK audit requires Windows")
    with os.add_dll_directory(str(sdk / "bin")):
        for name in dlls:
            prefix = Path(name).stem.split("-")[0]
            if prefix not in {"avcodec", "avformat", "avutil", "avdevice", "avfilter", "swscale", "swresample"}:
                continue
            library = ctypes.CDLL(str(sdk / name))
            values = {}
            for field in ("configuration", "license", "version"):
                fn = getattr(library, prefix + "_" + field)
                fn.argtypes = []
                fn.restype = ctypes.c_uint if field == "version" else ctypes.c_char_p
                value = fn()
                values[field] = value if field == "version" else value.decode("utf-8")
            values["configuration_sha256"] = hashlib.sha256(values.pop("configuration").encode()).hexdigest()
            result["libraries"][name] = values
    return result


def configuration_errors(runtime):
    options = set(runtime["configuration"])
    required = {"--enable-shared", "--disable-static", "--enable-version3",
                "--disable-libx264", "--disable-libx265", "--disable-libfdk-aac"}
    errors = ["missing configure option: " + option for option in sorted(required - options)]
    forbidden = ("--enable-gpl", "--enable-nonfree", "--enable-libx264", "--enable-libx265", "--enable-libfdk-aac")
    errors += ["forbidden configure option: " + option for option in options
               if any(option == flag or option.startswith(flag + "=") for flag in forbidden)]
    if ("Lesser General Public License" not in runtime["license_output"]
            or "either version 3" not in runtime["license_output"]
            or "any later version" not in runtime["license_output"]):
        errors.append("runtime -L is not LGPL v3-or-later")
    for name, lib in runtime["libraries"].items():
        if lib["license"] != "LGPL version 3 or later":
            errors.append("unexpected library license: " + name)
    return errors


def audit(sdk, lock, expected_version, archive=None, copy_dlls=None):
    sdk = Path(sdk).resolve()
    errors = []
    observed = inventory(sdk)
    for group, records in observed.items():
        if records != lock["files"].get(group):
            errors.append("inventory differs from lock: " + group)
    archive = Path(archive) if archive else sdk.parent / lock["archive"]["filename"]
    archive_sha = sha256(archive)
    if archive_sha != lock["archive"]["sha256"]:
        errors.append("archive SHA-256 differs from lock")
    images = {**observed["dlls"], **observed["executables"]}
    closure = dependency_closure(images)
    if closure["missing"]:
        errors.append("missing dependency DLLs: " + ", ".join(closure["missing"]))
    if any(rec["machine"] != "0x8664" for rec in images.values()):
        errors.append("SDK contains a non-x64 PE")
    copied = sorted(Path(p).name.lower() for p in copy_dlls) if copy_dlls is not None else None
    expected_copies = sorted(Path(p).name.lower() for p in lock["files"]["dlls"])
    if copied is not None and copied != expected_copies:
        errors.append("CMake DLL copy list differs from lock")
    runtime = None
    # Never load an altered SDK to find out whether it is trustworthy.
    if not errors:
        runtime = runtime_info(sdk, observed["dlls"])
        errors.extend(configuration_errors(runtime))
        if runtime != lock["runtime"] or runtime["version"] != expected_version:
            errors.append("runtime version/configuration/license differs from lock/expected version")
    unpinned = int(bool(lock["archive"].get("url") and "latest" in lock["archive"]["url"]
                        and not re.fullmatch(r"[0-9a-f]{64}", lock["archive"].get("sha256", ""))))
    if unpinned:
        errors.append("unpinned latest archive URL")
    return {"schema_version": 1, "status": "FAIL" if errors else "PASS", "errors": errors,
            "sdk": str(sdk), "archive_sha256": archive_sha, "version": expected_version,
            "unpinned_latest": unpinned, "missing_dependency_dlls": len(closure["missing"]),
            "dependency_closure": closure, "dll_count": len(observed["dlls"]),
            "header_count": len(observed["headers"]), "runtime": runtime,
            "mingw_runtime_dlls": lock["mingw_runtime_dlls"],
            "corresponding_source_status": lock["corresponding_source"]["status"],
            "scope": "SDK integrity only; source, contract, driver and clean-PC gates remain separate"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--expected-version", default=PINNED_VERSION)
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--copy-dll", action="append", default=None)
    parser.add_argument("--write-lock", action="store_true")
    args = parser.parse_args()
    try:
        if args.write_lock:
            if args.expected_version != PINNED_VERSION or not args.archive:
                raise ValueError("lock creation requires the pinned version and --archive")
            if sha256(args.archive) != PINNED_ARCHIVE_SHA256:
                raise ValueError("archive differs from the design's frozen SHA-256")
            files = inventory(args.sdk)
            verify_archive_files(args.archive, files)
            runtime = runtime_info(args.sdk.resolve(), files["dlls"])
            errors = configuration_errors(runtime)
            if errors or runtime["version"] != args.expected_version:
                raise ValueError(str(errors) + " version=" + runtime["version"])
            images = {**files["dlls"], **files["executables"]}
            lock = {"schema_version": 1, "license": "LGPL-3.0-or-later",
                    "archive": {"filename": args.archive.name, "sha256": sha256(args.archive),
                                "url": None, "url_status": "unverified; local frozen archive only"},
                    "ffmpeg_commit": "7ba069f4f1", "runtime": runtime, "files": files,
                    "dependency_closure": dependency_closure(images),
                    "mingw_runtime_dlls": sorted({Path(n).name for n in images
                        if re.match(r"lib(gcc|stdc\+\+|winpthread|gomp|ssp)", Path(n).name, re.I)}),
                    "corresponding_source": {"status": "UNVERIFIED", "archive_sha256": None,
                        "btbn_recipe_commit": None, "toolchain_image_digest": None,
                        "reason": "Exact third-party sources, patches and build provenance not acquired"}}
            write_json(args.lock, lock)
        lock = json.loads(args.lock.read_text(encoding="utf-8"))
        report = audit(args.sdk, lock, args.expected_version, args.archive, args.copy_dll)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, zipfile.BadZipFile, struct.error, subprocess.SubprocessError) as error:
        report = {"status": "FAIL", "errors": [str(error)]}
    write_json(args.report, report)
    print(json.dumps({k: v for k, v in report.items() if k not in ("runtime", "dependency_closure")}, ensure_ascii=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
