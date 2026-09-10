"""Release regression and failure-path tests; no publishing subprocess is allowed."""
import argparse
import base64
from contextlib import ExitStack, redirect_stdout, redirect_stderr
import copy
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
import release
from recorder import audit_ffmpeg as audit
from recorder import validate_release as validate

REPO = TOOLS.parent
IDENTITY = release.recorder_identity()
INSTALLER_NAME = IDENTITY["PACKAGE_STEM"] + "-Setup-" + IDENTITY["VERSION"] + ".exe"
NOTES_RELATIVE = "docs/release-notes/recorder/" + IDENTITY["VERSION"] + ".html"
KEY = base64.b64encode(b"k" * 32).decode()
SIGNATURE = base64.b64encode(b"s" * 64).decode()


def make_pe(path, imports=(), delay_imports=()):
    """Independent small PE32+ fixture with real import-table layout."""
    data = bytearray(8192)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 1)
    struct.pack_into("<H", data, 0x94, 240)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20b)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<IIII", data, optional + 240 + 8, 0x1800, 0x1000, 0x1800, 0x200)
    name_offset = 0x800
    for index, start, width, names in ((1, 0x200, 20, imports), (13, 0x400, 32, delay_imports)):
        if names:
            struct.pack_into("<II", data, optional + 112 + index * 8, start + 0xe00, width * (len(names) + 1))
        for i, name in enumerate(names):
            encoded = name.encode("ascii") + b"\0"
            data[name_offset:name_offset + len(encoded)] = encoded
            if index == 1:
                struct.pack_into("<I", data, start + i * width + 12, name_offset + 0xe00)
            else:
                struct.pack_into("<II", data, start + i * width, 1, name_offset + 0xe00)
            name_offset += len(encoded)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def refresh_manifest(bundle):
    path = bundle / "manifest.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    manifest["files"] = [validate.file_record(p, bundle) for p in sorted(bundle.rglob("*")) if p.is_file() and p != path]
    audit.write_json(path, manifest)


def candidate(bundle):
    payload = bundle / "payload"
    make_pe(payload / "Recorder.exe", ["avcodec-62.dll", "WinSparkle.dll", "USER32.dll"])
    make_pe(payload / "WinSparkle.dll", ["KERNEL32.dll"])
    make_pe(payload / "avcodec-62.dll", ["avutil-60.dll"], ["api-ms-win-crt-runtime-l1-1-0.dll"])
    make_pe(payload / "avutil-60.dll", ["KERNEL32.dll"])
    shutil.copytree(REPO / "recorder/licenses", payload / "licenses")
    runtime = json.loads((REPO / "recorder/third_party/ffmpeg.lock.json").read_text(encoding="utf-8"))["runtime"]
    lock = {"license": "LGPL-3.0-or-later", "runtime": runtime, "archive": {"sha256": "a" * 64},
            "files": {"dlls": {"bin/" + name: {"sha256": audit.sha256(payload / name)}
                              for name in ("avcodec-62.dll", "avutil-60.dll")}}}
    audit.write_json(bundle / "ffmpeg.lock.json", lock)
    audit.write_json(bundle / "ffmpeg-audit.json", {"status": "PASS", "errors": [], "unpinned_latest": 0,
                     "missing_dependency_dlls": 0, "archive_sha256": "a" * 64, "runtime": runtime})
    audit.write_json(payload / "release-links.json", {"version": IDENTITY["VERSION"],
        "source_url": IDENTITY["RELEASE_BASE_URL"] + IDENTITY["TAG_PREFIX"] + IDENTITY["VERSION"] + "/"
        + IDENTITY["SOURCES_STEM"] + "-" + IDENTITY["VERSION"] + ".zip"})
    installer = bundle / INSTALLER_NAME
    make_pe(installer)
    with mock.patch.object(release, "APP", release.APPS["recorder"]):
        release.write_recorder_metadata(bundle, IDENTITY, installer, SIGNATURE,
            REPO / NOTES_RELATIVE, KEY)
    return bundle


class PeAuditTests(unittest.TestCase):
    def test_normal_and_delay_imports(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.dll"
            make_pe(path, ["AVUTIL-60.dll", "KERNEL32.dll"], ["libwinpthread-1.dll"])
            result = audit.pe_imports(path)
            self.assertEqual(result["machine"], "0x8664")
            self.assertEqual(result["imports"], ["avutil-60.dll", "kernel32.dll"])
            self.assertEqual(result["delay_imports"], ["libwinpthread-1.dll"])
            closure = audit.dependency_closure({"sample.dll": result})
            self.assertEqual(closure["missing"], ["avutil-60.dll", "libwinpthread-1.dll"])

    def test_truncated_pe_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.dll"
            path.write_bytes(b"MZ")
            with self.assertRaises(ValueError):
                audit.pe_imports(path)

    def test_dll_in_unrelated_subdirectory_cannot_satisfy_import(self):
        empty = {"imports": [], "delay_imports": []}
        images = {"payload/Recorder.exe": {"imports": ["hidden.dll"], "delay_imports": []},
                  "payload/plugins/hidden.dll": empty}
        self.assertEqual(audit.dependency_closure(images)["missing"], ["hidden.dll"])

    def test_forbidden_configuration_and_license(self):
        runtime = copy.deepcopy(json.loads((REPO / "recorder/third_party/ffmpeg.lock.json").read_text())["runtime"])
        self.assertEqual(audit.configuration_errors(runtime), [])
        runtime["configuration"].append("--enable-gpl")
        self.assertIn("forbidden configure option: --enable-gpl", audit.configuration_errors(runtime))
        runtime["configuration"].append("--enable-nonfree=yes")
        self.assertTrue(any("nonfree" in error for error in audit.configuration_errors(runtime)))

    def test_copy_list_mismatch_prevents_runtime_execution(self):
        lock = json.loads((REPO / "recorder/third_party/ffmpeg.lock.json").read_text())
        with mock.patch.object(audit, "inventory", return_value=lock["files"]), \
             mock.patch.object(audit, "sha256", return_value=lock["archive"]["sha256"]), \
             mock.patch.object(audit, "runtime_info") as runtime:
            result = audit.audit(Path("sdk"), lock, audit.PINNED_VERSION, copy_dlls=["avcodec-62.dll"])
            self.assertEqual(result["status"], "FAIL")
            self.assertIn("CMake DLL copy list differs from lock", result["errors"])
            runtime.assert_not_called()


class BundleValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.bundle = candidate(Path(self.temp.name))
        self.verify = mock.patch.object(validate, "verify_signature").start()
        self.addCleanup(mock.patch.stopall)

    def check(self):
        return validate.validate_bundle(self.bundle, "tool", KEY)

    def test_candidate_is_technically_valid_but_not_publishable(self):
        result = self.check()
        self.assertEqual(result["technical_status"], "PASS", result)
        self.assertEqual(result["status"], "BLOCKED")
        self.assertFalse(result["publishable"])
        self.assertIn("JUCE-contract", result["blockers"])
        self.assertIn("same-release source archive missing", result["blockers"])
        self.verify.assert_called_once()

    def test_tampered_installer_does_not_reach_signature_check(self):
        with (self.bundle / INSTALLER_NAME).open("ab") as stream:
            stream.write(b"tampered")
        self.assertEqual(self.check()["status"], "FAIL")
        self.verify.assert_not_called()

    def test_tampered_signature_reaches_real_verification_contract(self):
        path = self.bundle / "appcast.xml"
        path.write_text(path.read_text(encoding="utf-8").replace(SIGNATURE, base64.b64encode(b"x" * 64).decode()), encoding="utf-8")
        refresh_manifest(self.bundle)
        self.verify.side_effect = ValueError("WinSparkle EdDSA verification failed")
        self.assertIn("WinSparkle EdDSA verification failed", self.check()["errors"])

    def test_changed_dll_cannot_be_hidden_by_regenerating_manifest(self):
        (self.bundle / "payload/avcodec-62.dll").write_bytes(b"changed")
        refresh_manifest(self.bundle)
        self.assertTrue(any("FFmpeg DLL differs" in error for error in self.check()["errors"]))

    def test_additional_transitive_dependency_must_ship(self):
        make_pe(self.bundle / "payload/WinSparkle.dll", ["missing-runtime.dll"])
        refresh_manifest(self.bundle)
        self.assertTrue(any("missing-runtime.dll" in error for error in self.check()["errors"]))

    def test_unmanifested_payload_file_is_rejected(self):
        (self.bundle / "payload/extra.dll").write_bytes(b"extra")
        self.assertEqual(self.check()["status"], "FAIL")

    def test_required_notice_cannot_be_omitted(self):
        (self.bundle / "payload/licenses/NOTICE.txt").unlink()
        refresh_manifest(self.bundle)
        self.assertTrue(any("NOTICE.txt" in error for error in self.check()["errors"]))

    def test_appcast_cross_app_url_is_rejected(self):
        path = self.bundle / "appcast.xml"
        path.write_text(path.read_text(encoding="utf-8").replace("recorder.invalid/releases/download", "github.com/owner/enqueue/releases/download"), encoding="utf-8")
        refresh_manifest(self.bundle)
        self.assertTrue(any("mismatch" in error for error in self.check()["errors"]))

    def test_manifest_cannot_escape_bundle(self):
        for value in ("../secret", "C:/secret", "\\\\server\\secret", "/absolute", "payload/../secret"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate.safe_path(self.bundle, value)

    def test_wrong_trusted_key_is_rejected(self):
        result = validate.validate_bundle(self.bundle, "tool", base64.b64encode(b"z" * 32).decode())
        self.assertEqual(result["status"], "FAIL")

    def test_source_zip_without_provenance_is_rejected(self):
        path = self.bundle / "sources.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("source-manifest.json", json.dumps({"schema_version": 1, "ffmpeg_commit": "7ba069f4f1"}))
        with self.assertRaises(ValueError):
            validate.validate_sources(path)

    def test_legacy_preview_bytes_are_preserved(self):
        for relative in ("index.html", "livemix/index.html", "style.css", "CNAME"):
            self.assertEqual((self.bundle / "site" / relative).read_bytes(), (REPO / "site" / relative).read_bytes())


class LegacyRegressionTests(unittest.TestCase):
    def test_existing_apps_and_version_sources_are_unchanged(self):
        for key, prefix, repo, remote, version in (
            ("enqueue", "v", "dnakrhs2-crypto/enqueue", "origin", "0.9.7"),
            ("livemix", "livemix-v", "dnakrhs2-crypto/livemix", "livemix", "0.9.0")):
            app = release.APPS[key]
            self.assertEqual((app["tag_prefix"], app["repo"], app["remote"]), (prefix, repo, remote))
            with mock.patch.object(release, "APP", app):
                self.assertEqual(release.read_version(), version)

    def test_legacy_build_commands_stay_identical(self):
        for key in ("enqueue", "livemix"):
            with mock.patch.object(release, "APP", release.APPS[key]), mock.patch.object(release, "run") as run:
                release.build("local", False)
                self.assertEqual(run.call_args_list, [
                    mock.call(["cmake", "--preset", "local"]),
                    mock.call(["cmake", "--build", "--preset", "local-release", "--target", release.APPS[key]["target"],
                               "EnqueueTests", "--", "-m", "-v:m", "-nologo"]),
                    mock.call(["ctest", "--preset", "local-release"])])

    def test_legacy_appcast_and_latest_schema(self):
        for key in ("enqueue", "livemix"):
            app = release.APPS[key]
            with self.subTest(app=key), tempfile.TemporaryDirectory() as directory, mock.patch.object(release, "APP", app):
                installer = Path(directory) / (app["name"] + "-Setup-1.2.3.exe")
                installer.write_bytes(b"installer")
                url, feed = release.write_appcast(Path(directory) / "appcast.xml", app["repo"], "1.2.3", installer, SIGNATURE, "<p>notes</p>")
                expected = "https://github.com/" + app["repo"] + "/releases/download/" + app["tag_prefix"] + "1.2.3/" + installer.name
                self.assertEqual(url, expected)
                self.assertEqual(feed, "https://github.com/" + app["repo"] + "/releases/latest/download/appcast.xml")
                enc = ET.parse(Path(directory) / "appcast.xml").find("./channel/item/enclosure")
                self.assertEqual(enc.get(validate.SPARKLE + "edSignature"), SIGNATURE)
                github = {"tagName": app["tag_prefix"] + "1.2.3", "publishedAt": "2026-09-09T00:00:00Z", "assets": [
                    {"name": installer.name, "url": expected, "size": 9}, {"name": app["fixed"], "url": "unused", "size": 9}]}
                with mock.patch.object(release, "run", return_value=json.dumps(github)), \
                     mock.patch.object(release, "read_feedback_link", return_value="feedback"):
                    latest = release.latest_from_github("gh", app["repo"], app)
                self.assertEqual(latest, {"version": "1.2.3", "tag": app["tag_prefix"] + "1.2.3", "url": expected,
                    "size": 9, "date": "2026-09-09T00:00:00Z", "latest_url": "https://github.com/" + app["repo"] +
                    "/releases/latest/download/" + app["fixed"], "feedback": "feedback"})

    def test_legacy_publish_and_skip_site_keep_their_remote_and_assets(self):
        for key in ("enqueue", "livemix"):
            with self.subTest(app=key), tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
                root = Path(directory)
                app = release.APPS[key]
                source = root / "build/vs2022" / app["artefacts"] / "Release"
                source.mkdir(parents=True)
                for name in (app["exe"], "WinSparkle.dll"):
                    (source / name).write_bytes(b"fixture")
                installer = root / (app["name"] + "-Setup-1.2.3.exe")
                installer.write_bytes(b"fixture")
                (root / "installer/output").mkdir(parents=True)
                keyfile = root / "private-key.pem"
                keyfile.write_text("test only")
                for name in ("yt-dlp.exe", "qjs.exe", "lame.exe", "libsndfile-1.dll", "LICENSES.txt"):
                    (root / name).write_text("fixture")
                stack.enter_context(mock.patch.object(release, "ROOT", root))
                stack.enter_context(mock.patch.object(release, "read_version", return_value="1.2.3"))
                stack.enter_context(mock.patch.object(release, "find_iscc", return_value="ISCC"))
                stack.enter_context(mock.patch.object(release, "find_winsparkle_tool", return_value="sign-tool"))
                stack.enter_context(mock.patch.object(release, "find_gh", return_value="gh"))
                stack.enter_context(mock.patch.object(release, "make_installer", return_value=installer))
                stack.enter_context(mock.patch.object(release, "sign", return_value=SIGNATURE))
                run = stack.enter_context(mock.patch.object(release, "run", return_value=""))
                site = stack.enter_context(mock.patch.object(release, "deploy_site"))
                stack.enter_context(mock.patch.dict(os.environ, {"GITHUB_REF_NAME": "", "GOCUE_GITHUB_REPO": ""}))
                stack.enter_context(mock.patch.object(sys, "argv", ["release.py", "--app", key, "--publish", "--skip-site",
                    "--key", str(keyfile), "--tools-dir", str(root)]))
                with redirect_stdout(io.StringIO()):
                    release.main()
                commands = [list(map(str, call.args[0])) for call in run.call_args_list]
                self.assertIn(["git", "push", app["remote"], "HEAD:main"], commands)
                self.assertIn(["git", "push", app["remote"], app["tag_prefix"] + "1.2.3"], commands)
                uploads = [c for c in commands if c[:3] == ["gh", "release", "upload"]]
                self.assertEqual(len(uploads), 1)
                self.assertIn(app["repo"], uploads[0])
                self.assertTrue(any(c.endswith(app["fixed"]) for c in uploads[0]))
                site.assert_not_called()

    def test_site_only_does_not_build(self):
        for key in ("enqueue", "livemix"):
            with mock.patch.object(sys, "argv", ["release.py", "--app", key, "--site-only"]), \
                 mock.patch.object(release, "deploy_site") as deploy, mock.patch.object(release, "build") as build:
                release.main()
                deploy.assert_called_once_with(release.SITE_REPO, None)
                build.assert_not_called()

    def test_site_deploy_never_queries_or_copies_unconfirmed_recorder(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory) / "work"
            work.mkdir()
            def command(cmd, **kwargs):
                if cmd[:2] == ["git", "clone"]:
                    (Path(cmd[-1]) / ".git").mkdir(parents=True)
                return "" # no changes, so no push
            with mock.patch.object(release, "APP", release.APPS["enqueue"]), \
                 mock.patch.object(release.tempfile, "mkdtemp", return_value=str(work)), \
                 mock.patch.object(release, "latest_from_github", return_value=None) as latest, \
                 mock.patch.object(release, "run", side_effect=command), \
                 mock.patch.object(release.shutil, "rmtree"):
                release.deploy_site(release.SITE_REPO, None)
            self.assertEqual([c.args[1] for c in latest.call_args_list], [release.APPS[k]["repo"] for k in ("enqueue", "livemix")])
            self.assertFalse((work / "pages/recorder").exists())
            self.assertTrue((work / "pages/livemix/index.html").is_file())


class RecorderReleaseRoutingTests(unittest.TestCase):
    def test_identity_is_the_packaging_source(self):
        app = release.APPS["recorder"]
        self.assertEqual((app["tag_prefix"], app["repo"], app["remote"], app["appcast"], app["site_dir"]),
            tuple(IDENTITY[k] for k in ("TAG_PREFIX", "RELEASE_REPO", "RELEASE_REMOTE", "APPCAST_URL", "SITE_DIR")))

    def test_recorder_package_only_never_reaches_git_or_site(self):
        with mock.patch.object(sys, "argv", ["release.py", "--app", "recorder", "--preset", "local", "--package-only"]), \
             mock.patch.object(release, "package_recorder") as package, mock.patch.object(release, "run") as run, \
             mock.patch.object(release, "deploy_site") as site:
            release.main()
            package.assert_called_once()
            run.assert_not_called()
            site.assert_not_called()

    def test_conflicting_flags_and_placeholder_publication_fail_before_actions(self):
        cases = [["--package-only", "--publish"], ["--package-only", "--site-only"],
                 ["--package-only", "--skip-build"], ["--package-only", "--skip-tests"],
                 ["--publish", "--repo", "real-owner/recorder"], ["--site-only"],
                 ["--package-only", "--repo", "real-owner/recorder"]]
        for flags in cases:
            with self.subTest(flags=flags), mock.patch.object(sys, "argv", ["release.py", "--app", "recorder", *flags]), \
                 mock.patch.object(release, "run") as run, mock.patch.object(release, "package_recorder") as package, \
                 redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                release.main()
            run.assert_not_called()
            package.assert_not_called()

    def test_build_discovers_final_target_output_and_runs_all_registered_tests(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(release, "ROOT", Path(directory)), \
             mock.patch.dict(os.environ, {"CMAKE": "cmake.exe"}), mock.patch.object(release, "recorder_run") as run:
            root = Path(directory)
            build = root / "out/recorder-package-build/local"
            exe = build / "future/output/Recorder.exe"
            exe.parent.mkdir(parents=True)
            exe.write_bytes(b"fixture")
            def configure(cmd, **kwargs):
                if "--preset" in cmd:
                    audit.write_json(build / "recorder-package-Release.json", {"exe": str(exe), "public_key": KEY})
            run.side_effect = configure
            description = release.recorder_build(argparse.Namespace(preset="local"), KEY)
            self.assertEqual(description["exe"], str(exe))
            commands = [list(map(str, c.args[0])) for c in run.call_args_list]
            self.assertFalse(any("--target" in c for c in commands))
            self.assertTrue(any("--no-tests=error" in c for c in commands))
            self.assertTrue(any("unittest" in c for c in commands))

    def test_package_pipeline_orders_build_test_stage_iscc_sign_and_stays_local(self):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            root = Path(directory)
            for relative in ("recorder/src/app/ProductIdentity.h", NOTES_RELATIVE):
                dest = root / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO / relative, dest)
            key = root / "ephemeral.pem"
            key.write_text("fixture")
            args = argparse.Namespace(preset="local", winsparkle_dir="tool-root", key=str(key), notes="", source_bundle="")
            order = []
            stack.enter_context(mock.patch.object(release, "ROOT", root))
            stack.enter_context(mock.patch.object(release, "APP", release.APPS["recorder"]))
            stack.enter_context(mock.patch.object(release, "find_iscc", return_value="ISCC"))
            stack.enter_context(mock.patch.object(release, "find_winsparkle_tool", return_value="sign-tool"))
            def build(*unused):
                order.append("build+ctest+unittest")
                return {"public_key": KEY}
            def stage(description, output, identity):
                order.append("audit+stage")
                payload = output / "payload"
                payload.mkdir()
                (payload / "Recorder.exe").write_bytes(b"fixture")
                return payload, [validate.file_record(payload / "Recorder.exe", output)]
            def local_run(cmd, **kwargs):
                if cmd[1] == "public-key":
                    return KEY
                self.assertEqual(cmd[0], "ISCC")
                order.append("ISCC")
                output = Path(next(c[len("/DOutputDir="):] for c in cmd if str(c).startswith("/DOutputDir=")))
                (output / INSTALLER_NAME).write_bytes(b"installer")
                return ""
            stack.enter_context(mock.patch.object(release, "recorder_build", side_effect=build))
            stack.enter_context(mock.patch.object(release, "stage_recorder", side_effect=stage))
            stack.enter_context(mock.patch.object(release, "recorder_run", side_effect=local_run))
            stack.enter_context(mock.patch.object(release, "sign", side_effect=lambda *a: order.append("sign") or SIGNATURE))
            stack.enter_context(mock.patch.object(release, "write_recorder_metadata", side_effect=lambda *a: order.append("metadata")))
            stack.enter_context(mock.patch.object(validate, "validate_bundle", side_effect=lambda *a: order.append("validate") or
                {"errors": [], "technical_status": "PASS", "status": "BLOCKED", "blockers": ["fixture"]}))
            run = stack.enter_context(mock.patch.object(release, "run"))
            site = stack.enter_context(mock.patch.object(release, "deploy_site"))
            with redirect_stdout(io.StringIO()):
                release.package_recorder(args)
            self.assertEqual(order, ["build+ctest+unittest", "audit+stage", "ISCC", "sign", "metadata", "validate"])
            run.assert_not_called()
            site.assert_not_called()


class RealWinSparkleSignatureTests(unittest.TestCase):
    def test_signature_and_changed_installer_with_actual_tool(self):
        tool = release.find_winsparkle_tool(os.environ.get("WINSPARKLE_DIR", ""))
        if not tool:
            self.skipTest("set WINSPARKLE_DIR for actual EdDSA verification")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private = root / "ephemeral-test-key.pem"
            subprocess.run([str(tool), "generate-key", "--file", str(private)], check=True, capture_output=True)
            public = subprocess.run([str(tool), "public-key", "--private-key-file", str(private)],
                                    check=True, capture_output=True, text=True).stdout
            key = re.search(r"[A-Za-z0-9+/]{43}=", public)[0]
            installer = root / "test-only.exe"
            make_pe(installer)
            with redirect_stdout(io.StringIO()):
                signature = release.sign(tool, private, installer)
            validate.verify_signature(tool, key, signature, installer)
            with installer.open("ab") as stream:
                stream.write(b"changed")
            with self.assertRaises(ValueError):
                validate.verify_signature(tool, key, signature, installer)

    def test_complete_candidate_with_real_signature_and_out_of_band_key(self):
        tool = release.find_winsparkle_tool(os.environ.get("WINSPARKLE_DIR", ""))
        if not tool:
            self.skipTest("set WINSPARKLE_DIR for actual bundle verification")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = root / "candidate"
            bundle.mkdir()
            candidate(bundle)
            private = root / "ephemeral-test-key.pem" # outside the bundle
            subprocess.run([str(tool), "generate-key", "--file", str(private)], check=True, capture_output=True)
            public = subprocess.run([str(tool), "public-key", "--private-key-file", str(private)],
                                    check=True, capture_output=True, text=True).stdout
            key = re.search(r"[A-Za-z0-9+/]{43}=", public)[0]
            with redirect_stdout(io.StringIO()):
                signature = release.sign(tool, private, bundle / INSTALLER_NAME)
            appcast = bundle / "appcast.xml"
            appcast.write_text(appcast.read_text(encoding="utf-8").replace(SIGNATURE, signature), encoding="utf-8")
            (bundle / "public-key.txt").write_text(key + "\n", encoding="ascii")
            manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
            manifest["embedded_public_key"] = key
            audit.write_json(bundle / "manifest.json", manifest)
            refresh_manifest(bundle)
            result = validate.validate_bundle(bundle, tool, key)
            self.assertEqual(result["status"], "BLOCKED", result)
            self.assertTrue(result["signature_verified"])
            self.assertFalse(result["publishable"])
            self.assertFalse(any("private" in r["path"] for r in manifest["files"]))
            checked = subprocess.run([sys.executable, "-B", str(TOOLS / "recorder/validate_release.py"),
                "--bundle", str(bundle), "--report", str(root / "release.json"),
                "--winsparkle-tool", str(tool), "--public-key", key], capture_output=True, env=dict(os.environ))
            self.assertEqual(checked.returncode, 2, checked.stderr)
            self.assertTrue(json.loads((root / "release.json").read_text(encoding="utf-8"))["signature_verified"])


if __name__ == "__main__":
    unittest.main()
