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
EXE = IDENTITY["PACKAGE_STEM"] + ".exe"  # 0.1.8: Tally.exe
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
    make_pe(payload / EXE, ["avcodec-62.dll", "WinSparkle.dll", "USER32.dll"])
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
        images = {"payload/" + EXE: {"imports": ["hidden.dll"], "delay_imports": []},
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

    def test_recorder_appcast_marks_the_automatic_update(self):
        enc = ET.parse(self.bundle / "appcast.xml").find("./channel/item/enclosure")
        self.assertEqual(enc.get(validate.SPARKLE + "installerArguments"), "/SILENT /SP- /NORESTART /AUTOUPDATE=1")

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

    def test_second_executable_in_payload_is_rejected(self):
        # 0.1.8 rename: a Recorder.exe left in the reused package build directory must not ship next to Tally.exe.
        make_pe(self.bundle / "payload/Recorder.exe", ["KERNEL32.dll"])
        refresh_manifest(self.bundle)
        self.assertTrue(any("executables other than the app" in error for error in self.check()["errors"]))

    def test_required_notice_cannot_be_omitted(self):
        (self.bundle / "payload/licenses/NOTICE.txt").unlink()
        refresh_manifest(self.bundle)
        self.assertTrue(any("NOTICE.txt" in error for error in self.check()["errors"]))

    def test_appcast_cross_app_url_is_rejected(self):
        path = self.bundle / "appcast.xml"
        path.write_text(path.read_text(encoding="utf-8").replace(IDENTITY["RELEASE_BASE_URL"], "https://github.com/owner/enqueue/releases/download/"), encoding="utf-8")
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
        # The versions themselves move with every Enqueue/LiveMix release; what must not change is where they
        # come from (CMakeLists.txt) and the release identifiers.
        cmake = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        expected = {"enqueue": re.search(r"project\(Enqueue\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", cmake).group(1),
                    "livemix": re.search(r'set\(LIVEMIX_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"', cmake).group(1)}
        for key, prefix, repo, remote in (
            ("enqueue", "v", "dnakrhs2-crypto/enqueue", "origin"),
            ("livemix", "livemix-v", "dnakrhs2-crypto/livemix", "livemix")):
            app = release.APPS[key]
            self.assertEqual((app["tag_prefix"], app["repo"], app["remote"]), (prefix, repo, remote))
            with mock.patch.object(release, "APP", app):
                self.assertEqual(release.read_version(), expected[key])
                self.assertRegex(release.read_version(), r"^[0-9]+\.[0-9]+\.[0-9]+$")

    def test_legacy_build_commands_stay_identical(self):
        for key in ("enqueue", "livemix"):
            with mock.patch.object(release, "APP", release.APPS[key]), mock.patch.object(release, "run") as run:
                release.build("local", False)
                self.assertEqual(run.call_args_list, [
                    mock.call(["cmake", "--preset", "local"]),
                    mock.call(["cmake", "--build", "--preset", "local-release", "--target", release.APPS[key]["target"],
                               "EnqueueTests", "--", "-m", "-v:m", "-nologo"]),
                    # only the app's own suites since 5ec12fe (Recorder's ctest entries need RecorderTests.exe)
                    mock.call(["ctest", "--preset", "local-release", "-R", release.APPS[key]["ctest_filter"]])])

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
                self.assertEqual(enc.get(validate.SPARKLE + "installerArguments"), "/SILENT /SP- /NORESTART /AUTOUPDATE=1")
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
                # GitHub as create_release sees it: no release for the tag yet, then the published one with its files
                stack.enter_context(mock.patch.object(release, "release_exists", return_value=False))
                stack.enter_context(mock.patch.object(release, "release_state",
                                                      return_value=(False, {installer.name, "appcast.xml"})))
                other = stack.enter_context(mock.patch.object(release, "check_other_work"))   # OtherWorkTests cover it
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
                other.assert_called_once_with(key, False)   # looked at before building

    def test_site_only_does_not_build(self):
        for key in ("enqueue", "livemix"):
            with mock.patch.object(sys, "argv", ["release.py", "--app", key, "--site-only"]), \
                 mock.patch.object(release, "deploy_site") as deploy, mock.patch.object(release, "build") as build:
                release.main()
                deploy.assert_called_once_with(release.SITE_REPO, None)
                build.assert_not_called()

    def _deploy_site_dry(self, directory, confirmed):
        work = Path(directory) / "work"
        work.mkdir()
        def command(cmd, **kwargs):
            if cmd[:2] == ["git", "clone"]:
                (Path(cmd[-1]) / ".git").mkdir(parents=True)
            return "" # no changes, so no push
        with mock.patch.dict(release.APPS["recorder"], {"publication_confirmed": confirmed}), \
             mock.patch.object(release, "APP", release.APPS["enqueue"]), \
             mock.patch.object(release.tempfile, "mkdtemp", return_value=str(work)), \
             mock.patch.object(release, "latest_from_github", return_value=None) as latest, \
             mock.patch.object(release, "run", side_effect=command), \
             mock.patch.object(release.shutil, "rmtree"):
            release.deploy_site(release.SITE_REPO, None)
        return work, [c.args[1] for c in latest.call_args_list]

    def test_site_deploy_never_queries_or_copies_unconfirmed_recorder(self):
        with tempfile.TemporaryDirectory() as directory:
            work, queried = self._deploy_site_dry(directory, confirmed=False)
            self.assertEqual(queried, [release.APPS[k]["repo"] for k in ("enqueue", "livemix")])
            self.assertFalse((work / ("pages/" + IDENTITY["SITE_DIR"])).exists())  # the app page; site/recorder/ keeps a redirect
            self.assertTrue((work / "pages/livemix/index.html").is_file())

    def test_site_deploy_includes_confirmed_recorder(self):
        with tempfile.TemporaryDirectory() as directory:
            work, queried = self._deploy_site_dry(directory, confirmed=True)
            self.assertEqual(queried, [release.APPS[k]["repo"] for k in ("enqueue", "livemix", "recorder")])
            self.assertTrue((work / ("pages/" + IDENTITY["SITE_DIR"] + "/index.html")).is_file())
            self.assertTrue((work / ("pages/" + IDENTITY["SITE_DIR"] + "/notes.html")).is_file())


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
        always = [["--package-only", "--publish"], ["--package-only", "--site-only"],
                  ["--package-only", "--skip-build"], ["--package-only", "--skip-tests"],
                  ["--publish", "--repo", "real-owner/recorder"], ["--package-only", "--repo", "real-owner/recorder"],
                  ["--publish", "--allow-dirty"], ["--publish", "--skip-tests"]]
        unconfirmed_only = [["--site-only"], ["--publish"]]
        for confirmed, cases in ((True, always), (False, always + unconfirmed_only)):
            for flags in cases:
                with self.subTest(confirmed=confirmed, flags=flags), mock.patch.dict(release.APPS["recorder"], {"publication_confirmed": confirmed}), \
                     mock.patch.object(sys, "argv", ["release.py", "--app", "recorder", *flags]), \
                     mock.patch.object(release, "run") as run, mock.patch.object(release, "package_recorder") as package, \
                     mock.patch.object(release, "publish_recorder") as publish, mock.patch.object(release, "deploy_site") as site, \
                     redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    release.main()
                run.assert_not_called()
                package.assert_not_called()
                publish.assert_not_called()
                site.assert_not_called()

    def test_confirmed_identity_routes_site_only_and_publish(self):
        with mock.patch.dict(release.APPS["recorder"], {"publication_confirmed": True}), \
             mock.patch.object(sys, "argv", ["release.py", "--app", "recorder", "--site-only"]), \
             mock.patch.object(release, "run") as run, mock.patch.object(release, "package_recorder") as package, \
             mock.patch.object(release, "deploy_site") as site:
            release.main()
            site.assert_called_once_with(release.SITE_REPO, None)
            package.assert_not_called()
            run.assert_not_called()
        candidate = {"tag": "recorder-v9.9.9"}
        order = []
        with mock.patch.dict(release.APPS["recorder"], {"publication_confirmed": True}), \
             mock.patch.object(sys, "argv", ["release.py", "--app", "recorder", "--publish"]), \
             mock.patch.object(release, "run", return_value="") as run, \
             mock.patch.object(release, "check_other_work", side_effect=lambda app, checked: order.append(("other", app, checked))), \
             mock.patch.object(release, "recorder_tag_preflight", side_effect=lambda tag: order.append(("preflight", tag))), \
             mock.patch.object(release, "package_recorder", side_effect=lambda a: order.append("package") or candidate), \
             mock.patch.object(release, "publish_recorder", side_effect=lambda a, c: order.append(("publish", c))):
            release.main()
            self.assertEqual(order, [("other", "recorder", False), ("preflight", "recorder-v" + IDENTITY["VERSION"]), "package", ("publish", candidate)])
            self.assertEqual(list(run.call_args_list[0].args[0]), ["git", "status", "--porcelain"])
        with mock.patch.dict(release.APPS["recorder"], {"publication_confirmed": True}), \
             mock.patch.object(sys, "argv", ["release.py", "--app", "recorder", "--publish"]), \
             mock.patch.object(release, "run", return_value=""), \
             mock.patch.object(release, "recorder_tag_preflight", side_effect=SystemExit("tag mismatch")), \
             mock.patch.object(release, "package_recorder") as package, mock.patch.object(release, "publish_recorder") as publish, \
             redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            release.main()
        package.assert_not_called()
        publish.assert_not_called()
        with mock.patch.dict(release.APPS["recorder"], {"publication_confirmed": True}), \
             mock.patch.object(sys, "argv", ["release.py", "--app", "recorder", "--publish"]), \
             mock.patch.object(release, "run", return_value=" M tools/release.py\n"), mock.patch.object(release, "package_recorder") as package, \
             mock.patch.object(release, "publish_recorder") as publish, redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            release.main()
        package.assert_not_called()
        publish.assert_not_called()

    def test_build_discovers_final_target_output_and_runs_all_registered_tests(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(release, "ROOT", Path(directory)), \
             mock.patch.dict(os.environ, {"CMAKE": "cmake.exe"}), mock.patch.object(release, "recorder_run") as run:
            root = Path(directory)
            build = root / "out/recorder-package-build/local"
            exe = build / ("future/output/" + EXE)
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
                (payload / EXE).write_bytes(b"fixture")
                return payload, [validate.file_record(payload / EXE, output)]
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


class NetworkRetryTests(unittest.TestCase):
    """2026-09-29: a timed-out gh request left LiveMix 0.12.0 tagged without its release - network calls retry."""

    def failure(self, stderr=""):
        return subprocess.CalledProcessError(1, "gh", stderr=stderr)

    def setUp(self):
        patcher = mock.patch.object(release.time, "sleep")
        self.sleep = patcher.start()
        self.addCleanup(patcher.stop)
        out = redirect_stdout(io.StringIO())
        out.__enter__()
        self.addCleanup(out.__exit__, None, None, None)

    def test_a_glitch_is_tried_again(self):
        with mock.patch.object(release, "run", side_effect=[self.failure(), "ok"]) as run:
            self.assertEqual(release.run_network(["git", "push", "origin", "main"], capture=True), "ok")
        self.assertEqual(run.call_count, 2)
        self.sleep.assert_called_once_with(release.NETWORK_RETRY_WAIT)

    def test_it_gives_up_after_the_last_attempt(self):
        with mock.patch.object(release, "run", side_effect=self.failure()) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                release.run_network(["git", "push", "origin", "main"])
        self.assertEqual(run.call_count, release.NETWORK_ATTEMPTS)

    def test_an_answer_is_not_retried(self):
        with mock.patch.object(release, "run", side_effect=self.failure("release not found")) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                release.run_network(["gh", "release", "view"], capture=True,
                                    give_up=lambda e: "release not found" in (e.stderr or "").lower())
        self.assertEqual(run.call_count, 1)
        self.sleep.assert_not_called()

    def test_before_retry_cleans_up_a_half_attempt(self):
        cleaned = []
        with mock.patch.object(release, "run", side_effect=[self.failure(), ""]):
            release.run_network(["git", "clone"], before_retry=lambda: cleaned.append(True))
        self.assertEqual(cleaned, [True])

    def fake_github(self, create_fails=1, exists_before=False, draft_states=None):
        """A GitHub for create_release: 'create' fails the first N times but (after the first) leaves the release
        behind as a draft; 'view --json tagName' answers whether it exists; 'view --json isDraft,assets' reports it."""
        state = {"exists": exists_before, "draft": False, "creates": 0, "assets": set()}
        drafts = list(draft_states or [])
        calls = []

        def fake_run(cmd, cwd=None, capture=False):
            cmd = list(map(str, cmd))
            calls.append(cmd[1:3] + ([cmd[-1]] if cmd[1:3] == ["release", "view"] else []))
            if cmd[1:3] == ["release", "create"]:
                state["creates"] += 1
                assets = [c for c in cmd[4:] if not c.startswith("--") and c not in ("o/r", "t")]
                if state["creates"] <= create_fails:
                    state.update(exists=True, draft=True, assets={Path(assets[0]).name})   # half done, answer lost
                    raise self.failure()
                state.update(exists=True, draft=False, assets={Path(a).name for a in assets})
                return ""
            if cmd[1:3] == ["release", "view"] and cmd[-1] == "tagName":
                if not state["exists"]:
                    raise self.failure("release not found")
                return "{}"
            if cmd[1:3] == ["release", "view"]:
                draft = drafts.pop(0) if drafts else state["draft"]
                return json.dumps({"isDraft": draft, "assets": [{"name": n} for n in sorted(state["assets"])]})
            if cmd[1:3] == ["release", "upload"]:
                state["assets"] |= {Path(c).name for c in cmd[4:] if not c.startswith("--") and c != "o/r"}
                return ""
            if cmd[1:3] == ["release", "edit"]:
                state["draft"] = False
                return ""
            return ""

        return fake_run, calls, state

    def test_a_first_create_that_works_is_checked_once(self):
        fake_run, calls, state = self.fake_github(create_fails=0)
        with mock.patch.object(release, "run", side_effect=fake_run):
            release.create_release("gh", "v9.9.9", ["a.exe", "appcast.xml"], "o/r", ["--title", "t", "--verify-tag"])
        self.assertEqual(calls, [["release", "view", "tagName"], ["release", "create"], ["release", "view", "isDraft,assets"]])
        self.sleep.assert_not_called()

    def test_a_create_whose_answer_was_lost_is_finished_not_made_twice(self):
        fake_run, calls, state = self.fake_github(create_fails=1)   # left behind as a draft with part of the files
        with mock.patch.object(release, "run", side_effect=fake_run):
            release.create_release("gh", "livemix-v9.9.9", ["a.exe", "appcast.xml"], "o/r", ["--title", "t", "--verify-tag"])
        self.assertEqual(calls, [["release", "view", "tagName"], ["release", "create"], ["release", "view", "tagName"],
                                 ["release", "upload"], ["release", "edit"], ["release", "view", "isDraft,assets"]])
        self.assertFalse(state["draft"])
        self.assertEqual(state["assets"], {"a.exe", "appcast.xml"})
        self.assertEqual(state["creates"], 1)

    def test_a_create_that_never_arrived_is_made_again(self):
        fake_run, calls, state = self.fake_github(create_fails=0)
        original = fake_run
        first = {"done": False}

        def lost_before_github(cmd, cwd=None, capture=False):
            if list(map(str, cmd[1:3])) == ["release", "create"] and not first["done"]:
                first["done"] = True
                calls.append(["release", "create"])
                raise self.failure()   # never reached GitHub: nothing was made
            return original(cmd, cwd, capture)

        with mock.patch.object(release, "run", side_effect=lost_before_github):
            release.create_release("gh", "v9.9.9", ["a.exe"], "o/r", ["--verify-tag"])
        self.assertEqual(calls, [["release", "view", "tagName"], ["release", "create"], ["release", "view", "tagName"],
                                 ["release", "create"], ["release", "view", "isDraft,assets"]])

    def test_a_release_that_was_already_there_is_not_touched(self):
        fake_run, calls, state = self.fake_github(create_fails=0, exists_before=True)
        with mock.patch.object(release, "run", side_effect=fake_run):
            with self.assertRaises(SystemExit) as stopped:
                release.create_release("gh", "v9.9.9", ["a.exe"], "o/r", ["--verify-tag"])
        self.assertIn("already exists", str(stopped.exception))
        self.assertEqual(calls, [["release", "view", "tagName"]])   # no create, no upload, no edit

    def test_a_lookup_that_times_out_is_not_taken_as_no_release(self):
        calls = []

        def timing_out(cmd, cwd=None, capture=False):
            calls.append(list(map(str, cmd[1:3])))
            raise self.failure("Post https://api.github.com/graphql: i/o timeout")

        with mock.patch.object(release, "run", side_effect=timing_out):
            with self.assertRaises(subprocess.CalledProcessError):
                release.create_release("gh", "v9.9.9", ["a.exe"], "o/r", ["--verify-tag"])
        self.assertEqual(calls, [["release", "view"]] * release.NETWORK_ATTEMPTS)   # never created, never uploaded

    def test_a_release_still_a_draft_is_not_reported_done(self):
        fake_run, calls, state = self.fake_github(create_fails=0, draft_states=[True, True, True])
        with mock.patch.object(release, "run", side_effect=fake_run):
            with self.assertRaises(subprocess.CalledProcessError):
                release.create_release("gh", "v9.9.9", ["a.exe"], "o/r", ["--verify-tag"])
        self.assertEqual(self.sleep.call_count, release.NETWORK_ATTEMPTS - 1)


class OtherWorkTests(unittest.TestCase):
    """The CEO (2026-09-28): several changes to one app under way at once go out once, after the last one."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.main_tree, self.other = base / "gocue", base / "gocue-volume"
        for tree in (self.main_tree, self.other):
            tree.mkdir()
        (self.other / "livemix").mkdir()
        self.edited = self.other / "livemix" / "Edited.cpp"
        self.edited.write_text("x")
        self.log_lines = ""
        self.status_lines = ""

    def fake_run(self, cmd, cwd=None, capture=False):
        cmd = list(map(str, cmd))
        if cmd[:2] == ["git", "log"]:
            return self.log_lines
        if cmd[:2] == ["git", "status"]:
            return self.status_lines if str(cwd) == str(self.other) else ""
        return ""

    def check(self, app="livemix"):
        trees = [{"worktree": str(self.main_tree), "HEAD": "aaa", "branch": "refs/heads/main"},
                 {"worktree": str(self.other), "HEAD": "bbb", "branch": "refs/heads/volume-cue"}]
        with mock.patch.object(release, "ROOT", self.main_tree), \
             mock.patch.object(release, "list_worktrees", return_value=trees), \
             mock.patch.object(release, "run", side_effect=self.fake_run) as run:
            found = release.other_work_in_progress(app)
        return found, run

    def test_nothing_under_way_lets_the_release_go(self):
        found, run = self.check()
        self.assertEqual(found, [])
        logs = [list(map(str, c.args[0])) for c in run.call_args_list if list(map(str, c.args[0]))[:2] == ["git", "log"]]
        self.assertEqual(len(logs), 1)                  # only the other worktree is looked at
        self.assertIn("HEAD...bbb", logs[0])
        self.assertIn("--cherry-pick", logs[0])         # a cherry-picked commit is not work still to come
        self.assertIn("livemix/", logs[0])              # this app's files only
        self.assertIn(":(glob)installer/LiveMix*", logs[0])   # matches LiveMix.iss / LiveMix.messages.iss

    def test_recent_commits_elsewhere_are_reported(self):
        self.log_lines = "1a2b3c4 09-29 00:20 Volume cue / big view\n5d6e7f8 09-28 23:10 start\n"
        found, _ = self.check()
        self.assertEqual(len(found), 1)
        self.assertIn("gocue-volume [volume-cue]", found[0])
        self.assertIn("2 commit(s)", found[0])

    def test_recent_uncommitted_changes_are_reported_old_ones_are_not(self):
        self.status_lines = " M livemix/Edited.cpp\0"
        found, _ = self.check()
        self.assertEqual(len(found), 1)
        self.assertIn("livemix/Edited.cpp", found[0])
        old = release.time.time() - (release.IN_PROGRESS_HOURS + 1) * 3600
        os.utime(self.edited, (old, old))                # an abandoned edit from days ago is not work in progress
        found, _ = self.check()
        self.assertEqual(found, [])

    def test_a_deletion_counts_by_its_folder_time(self):
        gone = self.other / "livemix" / "Removed.cpp"   # deleted, not committed: no file to take a time from
        self.status_lines = " D livemix/Removed.cpp\0"
        found, _ = self.check()
        self.assertEqual(len(found), 1)                  # the folder was just touched
        old = release.time.time() - (release.IN_PROGRESS_HOURS + 1) * 3600
        os.utime(self.other / "livemix", (old, old))     # deleted days ago: not work in progress
        found, _ = self.check()
        self.assertEqual(found, [])
        self.assertFalse(gone.exists())

    def test_untracked_files_are_listed_one_by_one_and_renames_parse(self):
        (self.other / "livemix" / "newdir").mkdir()
        (self.other / "livemix" / "newdir" / "Fresh.cpp").write_text("y")
        self.status_lines = "?? livemix/newdir/Fresh.cpp\0R  livemix/Renamed.cpp\0livemix/Old.cpp\0"
        (self.other / "livemix" / "Renamed.cpp").write_text("z")
        found, _ = self.check()
        self.assertEqual(len(found), 1)
        self.assertIn("livemix/newdir/Fresh.cpp", found[0])
        self.assertIn("livemix/Renamed.cpp", found[0])
        self.assertNotIn("Old.cpp", found[0])            # a rename's old name is not a change of its own

    def test_the_check_stops_the_release_unless_it_was_decided(self):
        with mock.patch.object(release, "other_work_in_progress", return_value=["C:/gocue-volume [volume-cue]: 3 commit(s)"]):
            with self.assertRaises(SystemExit) as stopped:
                release.check_other_work("enqueue", False)
            self.assertIn("gocue-volume", str(stopped.exception))
            with redirect_stdout(io.StringIO()) as out:
                release.check_other_work("enqueue", True)
            self.assertIn("--other-work-checked", out.getvalue())
        with mock.patch.object(release, "other_work_in_progress", return_value=[]):
            release.check_other_work("enqueue", False)   # nothing under way: silent


if __name__ == "__main__":
    unittest.main()
