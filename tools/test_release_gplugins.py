"""Default plugin packaging checks, using temporary folders only.

python tools/test_release_gplugins.py
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
from gplugins_stage import content_id, lock_for, sha256, stage, write_json

spec = importlib.util.spec_from_file_location("release", Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class GPluginsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gplugins-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "installer").mkdir()
        self.lock = self.root / "installer/gplugins.lock.json"
        self.out = self.root / "payload"
        self.vst3 = self.out / "VST3"
        self.bundle = "G-Test.vst3"
        self.file = self.vst3 / self.bundle / "Contents/x86_64-win/G-Test.vst3"
        self.file.parent.mkdir(parents=True)
        self.file.write_bytes(b"test plugin bytes")
        self.files = [{"path": self.file.relative_to(self.vst3).as_posix(),
                       "size": self.file.stat().st_size, "sha256": sha256(self.file)}]
        self.payload = {"schema": 1, "id": content_id(self.files), "version": "1.2.3",
                        "source_list_sha256": "a" * 64, "bundles": [self.bundle], "files": self.files}
        write_json(self.vst3 / "bundled_plugins.json", self.payload)
        write_json(self.lock, lock_for(self.payload))
        self.addCleanup(patch.stopall)
        patch.object(release, "ROOT", self.root).start()
        patch.object(release, "APP", release.APPS["livemix"]).start()

    def check(self, directory=None):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            release.check_gplugins(directory or self.out)
        return output.getvalue()

    def test_valid_payload(self):
        self.assertIn("GPLUGINS: 1 plugins, 1.2.3, id ", self.check())

    def test_changed_file(self):
        self.file.write_bytes(b"different content")
        with self.assertRaisesRegex(SystemExit, "SHA-256 mismatch"):
            self.check()

    def test_missing_file(self):
        self.file.unlink()
        with self.assertRaisesRegex(SystemExit, "missing files"):
            self.check()

    def test_extra_file(self):
        (self.vst3 / "extra.dll").write_bytes(b"extra")
        with self.assertRaisesRegex(SystemExit, "unlisted files"):
            self.check()

    def test_different_lock(self):
        write_json(self.lock, {**lock_for(self.payload), "id": "b" * 64})
        with self.assertRaisesRegex(SystemExit, "lock.json mismatch"):
            self.check()

    def source_manifest(self):
        build = self.root / "build"
        source = build / "GTest_artefacts/Release/VST3" / self.files[0]["path"]
        source.parent.mkdir(parents=True)
        source.write_bytes(self.file.read_bytes())
        manifest = self.root / "installer_manifest.json"
        write_json(manifest, {"schema_version": 1, "version": self.payload["version"],
                             "source_build": str(build), "source_list_sha256": "a" * 64,
                             "bundles": [{"bundle": self.bundle, "target": "GTest", "files": self.files}]})
        return manifest, source

    def test_stage_then_release_check_and_replace(self):
        manifest, source = self.source_manifest()
        out = self.root / "staged"
        with contextlib.redirect_stdout(io.StringIO()) as output:
            stage(manifest, out, self.lock)
        self.assertTrue(output.getvalue().startswith("GPLUGINS_STAGED id="))
        self.check(out)
        self.assertEqual((out / "VST3" / self.files[0]["path"]).stat().st_mtime_ns, source.stat().st_mtime_ns)
        with self.assertRaisesRegex(ValueError, "already exists"):
            stage(manifest, out)
        (out / "obsolete").write_bytes(b"old")
        with contextlib.redirect_stdout(io.StringIO()):
            stage(manifest, out, self.lock, replace=True)
        self.assertFalse((out / "obsolete").exists())
        self.check(out)

    def test_source_mismatch_writes_nothing(self):
        manifest, source = self.source_manifest()
        out = self.root / "staged"
        original = source.read_bytes()
        for problem in ("changed", "missing", "extra"):
            with self.subTest(problem=problem):
                source.write_bytes(original)
                if problem == "changed":
                    source.write_bytes(b"changed")
                elif problem == "missing":
                    source.unlink()
                else:
                    source.with_name("extra.dll").write_bytes(b"extra")
                before = self.lock.read_bytes()
                with self.assertRaises(ValueError):
                    stage(manifest, out, self.lock)
                self.assertFalse(out.exists())
                self.assertEqual(self.lock.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
