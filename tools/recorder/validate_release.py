#!/usr/bin/env python3
"""Offline Recorder bundle checks for round 34; never installs or launches the app.

Exit 0: technical checks and recorded release gates pass; 1: invalid bundle;
2: technically valid candidate with unresolved release gates. Supply a trusted
--public-key out of band; a key inside a bundle proves consistency, not identity.
"""
import argparse
import base64
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile

if __package__:
    from .audit_ffmpeg import sha256, write_json, pe_imports, dependency_closure, configuration_errors
else:
    from audit_ffmpeg import sha256, write_json, pe_imports, dependency_closure, configuration_errors

SPARKLE = "{http://www.andymatuschak.org/xml-namespaces/sparkle}"
REQUIRED_NOTICES = {"LGPL-3.0.txt", "GPL-3.0.txt", "NOTICE.txt", "THIRD-PARTY.md",
                    "WinSparkle-COPYING.txt", "WinSparkle-COPYING.expat.txt", "release-gates.json"}
REQUIRED_GATES = {"exact-corresponding-source", "third-party-notices", "JUCE-contract", "ASIO-contract",
                  "AVC-contract", "AAC-contract", "public-identity-URLs", "round28-shutdown-and-about-wiring",
                  "clean-PC-install-update"}


def safe_path(root, relative):
    if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative:
        raise ValueError("unsafe bundle path: " + str(relative))
    part = PurePosixPath(relative)
    if part.is_absolute() or part.as_posix() != relative or any(p in (".", "..") for p in relative.split("/")):
        raise ValueError("unsafe bundle path: " + relative)
    root = Path(root).resolve()
    path = root.joinpath(*part.parts)
    if not path.resolve().is_relative_to(root):
        raise ValueError("bundle path escapes root: " + relative)
    for parent in [path, *path.parents]:
        if parent == root:
            break
        if parent.is_symlink() or (hasattr(parent, "is_junction") and parent.is_junction()):
            raise ValueError("bundle symlink/junction: " + relative)
    return path


def file_record(path, root):
    return {"path": Path(path).relative_to(root).as_posix(), "sha256": sha256(path), "size": Path(path).stat().st_size}


def verify_signature(tool, public_key, signature, installer):
    if len(base64.b64decode(public_key, validate=True)) != 32:
        raise ValueError("EdDSA public key must decode to 32 bytes")
    if len(base64.b64decode(signature, validate=True)) != 64:
        raise ValueError("EdDSA signature must decode to 64 bytes")
    if not tool:
        raise ValueError("winsparkle-tool is required for real EdDSA verification (--winsparkle-tool / WINSPARKLE_DIR)")
    result = subprocess.run([str(tool), "verify", "--public-key", public_key, "--signature", signature, str(installer)],
                            capture_output=True, timeout=60, env=dict(os.environ))
    if result.returncode != 0:
        raise ValueError("WinSparkle EdDSA verification failed")


def validate_sources(archive):
    """Check a release-owner assembled ZIP without extracting untrusted paths.

    Human provenance/rebuild review remains a release-gates.json requirement.
    """
    with zipfile.ZipFile(archive) as source:
        names = [n for n in source.namelist() if not n.endswith("/")]
        if len(names) != len(set(n.lower() for n in names)):
            raise ValueError("duplicate source archive member")
        for name in names:
            safe_path(Path(archive).parent, name)
        metadata = json.loads(source.read("source-manifest.json"))
        if metadata.get("schema_version") != 1 or not re.fullmatch(r"7ba069f4f1[0-9a-f]{30}", metadata.get("ffmpeg_commit", "")):
            raise ValueError("source archive needs full FFmpeg commit matching 7ba069f4f1")
        for key in ("btbn_recipe_commit", "toolchain_image_digest", "toolchain_versions", "components", "rebuild_instructions"):
            if not metadata.get(key):
                raise ValueError("source provenance missing: " + key)
        if not re.fullmatch(r"[0-9a-f]{40}", metadata["btbn_recipe_commit"]):
            raise ValueError("BtbN build recipe must be a full immutable commit")
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", metadata["toolchain_image_digest"]):
            raise ValueError("toolchain container must have an immutable digest")
        records = metadata["files"]
        if {r["path"] for r in records} != set(names) - {"source-manifest.json"} or len(records) != len(names) - 1:
            raise ValueError("source manifest file set mismatch")
        if metadata["rebuild_instructions"] not in names or metadata.get("sbom") not in names:
            raise ValueError("source archive must contain rebuild instructions and SBOM")
        for component in metadata["components"]:
            if not component.get("name") or not component.get("version"):
                raise ValueError("source component lacks name/version")
            for field in ("source_paths", "license_paths"):
                if not component.get(field) or any(p not in names for p in component[field]):
                    raise ValueError("source component lacks archived " + field)
        import hashlib
        for record in records:
            data = source.read(record["path"])
            if hashlib.sha256(data).hexdigest() != record["sha256"] or len(data) != record["size"]:
                raise ValueError("source member hash/size mismatch: " + record["path"])
        return metadata


def validate_bundle(bundle, tool=None, trusted_public_key=""):
    errors, blockers = [], []
    report = {"schema_version": 1, "signature_verified": False, "errors": errors, "blockers": blockers,
              "scope": "offline files/signature only; does not certify installation, runtime, contracts or source provenance"}
    try:
        bundle = Path(bundle).resolve()
        manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
        if manifest.get("schema_version") != 1 or manifest.get("app") != "recorder":
            raise ValueError("not a Recorder release manifest")
        records = manifest["files"]
        names = [r["path"] for r in records]
        if len(names) != len(set(n.lower() for n in names)):
            raise ValueError("duplicate manifest path")
        for record in records:
            path = safe_path(bundle, record["path"])
            if not path.is_file() or path.stat().st_size != record["size"] or sha256(path) != record["sha256"]:
                raise ValueError("missing/changed manifest file: " + record["path"])
        required = {manifest["installer"], manifest["executable"], "appcast.xml", "latest.json", "notes.html",
                    "ffmpeg.lock.json", "ffmpeg-audit.json", "public-key.txt", "payload/release-links.json"}
        required |= {"payload/licenses/" + n for n in REQUIRED_NOTICES}
        if not required <= set(names):
            raise ValueError("manifest omits required files: " + str(sorted(required - set(names))))
        expected_payload = {n for n in names if n.startswith("payload/")}
        actual_payload = {p.relative_to(bundle).as_posix() for p in (bundle / "payload").rglob("*") if p.is_file()}
        if expected_payload != actual_payload:
            raise ValueError("payload contains unmanifested or missing files")
        lock = json.loads((bundle / "ffmpeg.lock.json").read_text(encoding="utf-8"))
        if lock["license"] != "LGPL-3.0-or-later":
            raise ValueError("unexpected FFmpeg license")
        config_errors = configuration_errors(lock["runtime"])
        if config_errors:
            raise ValueError("invalid locked FFmpeg configuration/license: " + str(config_errors))
        for name, record in lock["files"]["dlls"].items():
            dll = bundle / "payload" / Path(name).name
            if not dll.is_file() or sha256(dll) != record["sha256"]:
                raise ValueError("FFmpeg DLL differs from lock: " + name)
        audit = json.loads((bundle / "ffmpeg-audit.json").read_text(encoding="utf-8"))
        if (audit.get("status") != "PASS" or audit.get("errors") or audit.get("unpinned_latest") != 0
                or audit.get("missing_dependency_dlls") != 0 or audit.get("archive_sha256") != lock["archive"]["sha256"]
                or audit.get("runtime") != lock["runtime"]):
            raise ValueError("SDK audit evidence does not match lock")
        images = {n: pe_imports(bundle / n) for n in sorted(actual_payload) if Path(n).suffix.lower() in (".dll", ".exe")}
        if any(p["machine"] != "0x8664" for p in images.values()):
            raise ValueError("non-x64 payload image")
        if "payload/WinSparkle.dll" not in images or manifest["executable"] not in images:
            raise ValueError("missing app or WinSparkle PE")
        other_executables = sorted(n for n in images if n.lower().endswith(".exe") and n != manifest["executable"])
        if other_executables:
            raise ValueError("payload contains executables other than the app: " + str(other_executables))
        closure = dependency_closure(images)
        if closure["missing"]:
            raise ValueError("missing payload dependency DLLs: " + str(closure["missing"]))
        report["dependency_closure"] = closure
        report["dlls"] = sorted(n for n in images if n.lower().endswith(".dll"))
        enclosure = ET.parse(bundle / "appcast.xml").findall("./channel/item/enclosure")
        if len(enclosure) != 1:
            raise ValueError("candidate appcast must contain exactly one enclosure")
        enclosure = enclosure[0]
        latest = json.loads((bundle / "latest.json").read_text(encoding="utf-8"))
        identity = manifest["identity"]
        if (manifest["executable"] != "payload/" + identity["PACKAGE_STEM"] + ".exe"
                or manifest["installer"] != identity["PACKAGE_STEM"] + "-Setup-" + identity["VERSION"] + ".exe"):
            raise ValueError("installer/executable names differ from ProductIdentity")
        tag = identity["TAG_PREFIX"] + manifest["version"]
        expected_url = identity["RELEASE_BASE_URL"] + tag + "/" + manifest["installer"]
        source_name = identity["SOURCES_STEM"] + "-" + manifest["version"] + ".zip"
        source_url = identity["RELEASE_BASE_URL"] + tag + "/" + source_name
        links = json.loads((bundle / "payload/release-links.json").read_text(encoding="utf-8"))
        if (latest.get("source_url") != source_url or links.get("source_url") != source_url
                or links.get("version") != manifest["version"]
                or (manifest.get("source_archive") and manifest["source_archive"] != source_name)):
            raise ValueError("same-release source links differ from ProductIdentity")
        installer = safe_path(bundle, manifest["installer"])
        if (manifest["version"] != identity["VERSION"] or enclosure.get(SPARKLE + "version") != manifest["version"]
                or enclosure.get("length") != str(installer.stat().st_size) or enclosure.get("url") != expected_url
                or latest.get("url") != expected_url or latest.get("tag") != tag
                or latest.get("version") != manifest["version"] or latest.get("size") != installer.stat().st_size
                or ET.parse(bundle / "appcast.xml").findtext("./channel/link") != identity["APPCAST_URL"]):
            raise ValueError("appcast/latest/identity/installer mismatch")
        if not urllib.parse.urlsplit(expected_url).scheme == "https":
            raise ValueError("non-HTTPS download URL")
        key = (bundle / "public-key.txt").read_text(encoding="ascii").strip()
        if key != manifest["embedded_public_key"] or (trusted_public_key and key != trusted_public_key):
            raise ValueError("signing key differs from app/trusted public key")
        verify_signature(tool, key, enclosure.get(SPARKLE + "edSignature", ""), installer)
        report["signature_verified"] = True
        if not trusted_public_key:
            blockers.append("trusted public key not supplied out of band")
        if identity["PUBLICATION_CONFIRMED"] != "1" or ".invalid" in expected_url or "UNCONFIRMED" in identity["RELEASE_REPO"]:
            blockers.append("public identity/URLs not confirmed")
        gates = json.loads((bundle / "payload/licenses/release-gates.json").read_text(encoding="utf-8"))["gates"]
        if {gate["id"] for gate in gates} != REQUIRED_GATES or len(gates) != len(REQUIRED_GATES):
            raise ValueError("release gate list missing/duplicated")
        blockers.extend(gate["id"] for gate in gates if gate.get("status") != "CONFIRMED" or not gate.get("evidence"))
        source = manifest.get("source_archive")
        if source:
            if source not in names:
                raise ValueError("source archive omitted from manifest")
            source_path = safe_path(bundle, source)
            source_metadata = validate_sources(source_path)
            provenance = lock.get("corresponding_source", {})
            if (provenance.get("status") != "CONFIRMED" or provenance.get("archive_sha256") != sha256(source_path)
                    or provenance.get("btbn_recipe_commit") != source_metadata["btbn_recipe_commit"]
                    or provenance.get("toolchain_image_digest") != source_metadata["toolchain_image_digest"]):
                blockers.append("source archive provenance not confirmed in FFmpeg lock")
        else:
            blockers.append("same-release source archive missing")
        report["version"] = manifest["version"]
    except (OSError, ValueError, KeyError, TypeError, AttributeError, ET.ParseError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        errors.append(str(error))
    report["technical_status"] = "FAIL" if errors else "PASS"
    report["status"] = "FAIL" if errors else "BLOCKED" if blockers else "PASS"
    report["publishable"] = report["status"] == "PASS"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--winsparkle-tool", default="")
    parser.add_argument("--public-key", default=os.environ.get("RECORDER_EDDSA_PUBLIC_KEY", ""))
    args = parser.parse_args()
    base = os.environ.get("WINSPARKLE_DIR", "")
    tool = args.winsparkle_tool or (str(Path(base) / "bin/winsparkle-tool.exe") if base else shutil.which("winsparkle-tool"))
    report = validate_bundle(args.bundle, tool, args.public_key)
    write_json(args.report, report)
    print(json.dumps(report, ensure_ascii=True))
    return 1 if report["errors"] else 2 if report["blockers"] else 0


if __name__ == "__main__":
    sys.exit(main())
