"""Release scope evidence and command wiring; all history/cycle data is temporary."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from tools import release_gate_scope as scope

spec = importlib.util.spec_from_file_location("release_gate_test_release", Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ScopeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="release-gate-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "repo"
        self.root.mkdir()
        self.cycles = self.base / "cycles"
        self.git("init", "-q")
        self.git("config", "user.name", "Release gate test")
        self.git("config", "user.email", "release-gate@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.write("CMakeLists.txt", 'project(Enqueue VERSION 1.2.3 LANGUAGES CXX)\nset(LIVEMIX_VERSION "4.5.6")\n')
        self.write("tests/Offline.cpp", 'UnitTest ("Offline", "Enqueue") {}\n')
        self.write("tests/Desktop.cpp", 'UnitTest ("Desktop", "LiveMix") {} // SetFocus\n')
        self.write(scope.SKIP_FILE, scope.skip_file_text(self.root / "tests"))
        self.write("src/code.cpp", "original\n")
        self.commit = self.commit_all()
        self.record = {
            "ended_at": "2026-11-14T02:00:00+00:00", "stale": False,
            "fingerprint_start": {"head": self.commit, "sha256": "same",
                                  "diff_sha256": scope.EMPTY_SHA256, "untracked": []},
            "fingerprint_end": {"head": self.commit, "sha256": "same"},
            "stages": {"full": {"status": "passed", "steps": [{"test_filter": None, "exit_code": 0}]}},
        }
        self.write_record()

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True,
                              encoding="utf-8").stdout.strip()

    def write(self, path, text):
        destination = self.root / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text, encoding="utf-8", newline="\n")

    def commit_all(self):
        self.git("add", "-A")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def write_record(self, record=None, round_name="r01"):
        path = self.cycles / "fixture" / round_name / "summary.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(record if record is not None else self.record), encoding="utf-8")
        return path

    def record_working(self):
        fingerprint = {"head": self.git("rev-parse", "HEAD"), "untracked": []}
        diff = subprocess.check_output(["git", "diff", "HEAD", "--binary", "--no-ext-diff", "--no-textconv"], cwd=self.root)
        fingerprint["diff_sha256"] = hashlib.sha256(diff).hexdigest()
        paths = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=self.root)
        for raw in paths.split(b"\0"):
            if raw:
                path = raw.decode("utf-8")
                fingerprint["untracked"].append({"path": path, "sha256": hashlib.sha256((self.root / path).read_bytes()).hexdigest()})
        fingerprint["sha256"] = hashlib.sha256(json.dumps(fingerprint, sort_keys=True).encode()).hexdigest()
        self.record["fingerprint_start"] = fingerprint
        self.record["fingerprint_end"] = copy.deepcopy(fingerprint)
        self.write_record().with_name("fingerprint_start.diff").write_bytes(diff)

    def dirty_release(self, app="enqueue", crlf=False):
        if crlf:
            self.git("config", "core.autocrlf", "true")
            self.write(".gitattributes", "* text=auto\n")
            self.commit_all()
        self.write("src/code.cpp", "tested change\nsecond line\n")
        (self.root / "src/new.cpp").write_bytes(b"new file\r\nsecond line\r\n" if crlf else b"new file\nsecond line\n")
        version = "1.2.4" if app == "enqueue" else "4.5.7"
        self.write(f"{scope.NOTES_DIRS[app]}/{version}.html", "<p>Draft notes from the cycle</p>\n")
        self.record_working()
        self.commit_all()
        self.release_changes(app)

    def release_changes(self, app="enqueue"):
        text = (self.root / "CMakeLists.txt").read_text(encoding="utf-8")
        old, new = ("1.2.3", "1.2.4") if app == "enqueue" else ("4.5.6", "4.5.7")
        self.write("CMakeLists.txt", text.replace(old, new))
        self.write(f"{scope.NOTES_DIRS[app]}/{new}.html", "<p>Release notes</p>\n")
        self.commit_all()

    def decide(self, app="enqueue", **kwargs):
        return scope.decide(self.root, app, cycles_dir=self.cycles, **kwargs)

    def assert_full(self, decision, reason):
        self.assertFalse(decision.short)
        self.assertEqual(decision.ctest_filter, scope.FULL_FILTER)
        self.assertIn(reason, decision.reason)

    def test_enqueue_version_and_current_notes_are_short(self):
        self.release_changes()
        decision = self.decide()
        self.assertTrue(decision.short)
        self.assertEqual(decision.ctest_filter, scope.SHORT_FILTER)
        self.assertEqual(decision.skip_count, 1)
        self.assertIn("fixture\\r01", str(decision))
        self.assertIn(self.commit[:10], str(decision))
        self.assertIn("docs/release-notes/1.2.4.html", str(decision))

    def test_livemix_version_and_current_notes_are_short(self):
        self.release_changes("livemix")
        self.assertTrue(self.decide("livemix").short)

    def test_same_commit_is_short(self):
        self.assertTrue(self.decide().short)

    def test_modifying_current_notes_is_short(self):
        self.release_changes()
        self.write("docs/release-notes/1.2.4.html", "<p>Updated notes</p>\n")
        self.commit_all()
        self.assertTrue(self.decide().short)

    def test_code_change_is_full(self):
        self.release_changes()
        self.write("src/code.cpp", "changed\n")
        self.commit_all()
        self.assert_full(self.decide(), "src/code.cpp")

    def test_other_apps_version_is_full_for_each_app(self):
        original = (self.root / "CMakeLists.txt").read_text(encoding="utf-8")
        self.write("CMakeLists.txt", original.replace("4.5.6", "4.5.7"))
        self.commit_all()
        self.assert_full(self.decide("enqueue"), "CMakeLists.txt")
        self.git("reset", "--hard", self.commit)  # this temporary fixture only
        self.write("CMakeLists.txt", original.replace("1.2.3", "1.2.4"))
        self.commit_all()
        self.assert_full(self.decide("livemix"), "CMakeLists.txt")

    def test_cmake_changes_outside_version_digits_are_full(self):
        self.release_changes()
        self.write("CMakeLists.txt", (self.root / "CMakeLists.txt").read_text().replace("LANGUAGES", " LANGUAGES"))
        self.commit_all()
        self.assert_full(self.decide(), "beyond")

    def test_other_document_is_full(self):
        self.write("docs/guide.md", "new guide\n")
        self.commit_all()
        self.assert_full(self.decide(), "docs/guide.md")

    def test_old_notes_are_full(self):
        self.write("docs/release-notes/1.2.2.html", "old notes\n")
        self.commit_all()
        self.assert_full(self.decide(), "1.2.2.html")

    def test_deleted_notes_are_full(self):
        self.release_changes()
        self.record["fingerprint_start"]["head"] = self.git("rev-parse", "HEAD")
        self.write_record()
        (self.root / "docs/release-notes/1.2.4.html").unlink()
        self.commit_all()
        self.assert_full(self.decide(), "D docs/release-notes/1.2.4.html")

    def test_renamed_notes_are_full(self):
        self.release_changes()
        self.record["fingerprint_start"]["head"] = self.git("rev-parse", "HEAD")
        self.write_record()
        self.write("CMakeLists.txt", (self.root / "CMakeLists.txt").read_text().replace("1.2.4", "1.2.5"))
        self.git("mv", "docs/release-notes/1.2.4.html", "docs/release-notes/1.2.5.html")
        self.commit_all()
        self.assert_full(self.decide(), "D docs/release-notes/1.2.4.html")

    def test_invalid_diff_hash_is_full(self):
        self.record["fingerprint_start"]["diff_sha256"] = "dirty"
        self.write_record()
        self.assert_full(self.decide(), "diff hash")

    def test_invalid_untracked_hashes_are_full(self):
        self.record["fingerprint_start"]["untracked"] = ["new.cpp"]
        self.write_record()
        self.assert_full(self.decide(), "untracked file hashes")

    def test_different_fingerprints_are_full(self):
        self.record["fingerprint_end"]["sha256"] = "different"
        self.write_record()
        self.assert_full(self.decide(), "fingerprints")

    def test_stale_cycle_is_full(self):
        self.record["stale"] = True
        self.write_record()
        self.assert_full(self.decide(), "stale")

    def test_skipped_full_stage_is_full(self):
        self.record["stages"]["full"]["status"] = "skipped"
        self.write_record()
        self.assert_full(self.decide(), "did not pass")

    def test_any_filtered_or_failed_full_step_is_full(self):
        for step in ({"test_filter": "Offline", "exit_code": 0}, {"test_filter": None, "exit_code": 1}):
            with self.subTest(step=step):
                self.record["stages"]["full"]["steps"] = [{"exit_code": 0}, step]
                self.write_record()
                self.assert_full(self.decide(), "steps")

    def test_full_stage_without_steps_is_full(self):
        self.record["stages"]["full"]["steps"] = []
        self.write_record()
        self.assert_full(self.decide(), "steps")

    def test_non_ancestor_with_different_content_is_full(self):
        self.git("checkout", "-qb", "side")
        self.write("src/code.cpp", "side branch\n")
        side = self.commit_all()
        self.git("checkout", "--detach", self.commit)
        self.record["fingerprint_start"]["head"] = side
        self.write_record()
        self.assert_full(self.decide(), "src/code.cpp")

    def test_non_ancestor_with_same_content_is_short(self):
        self.git("-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", "different history, same tree")
        self.record["fingerprint_start"]["head"] = self.git("rev-parse", "HEAD")
        self.git("checkout", "--detach", self.commit)
        self.write_record()
        self.assertTrue(self.decide().short)

    def test_dirty_cycle_then_source_commit_then_version_commit_is_short(self):
        self.dirty_release()
        self.assertTrue(self.decide().short)

    def test_livemix_dirty_cycle_is_short(self):
        self.dirty_release("livemix")
        self.assertTrue(self.decide("livemix").short)

    def test_untracked_crlf_matches_checkout_filters_after_text_auto_commit(self):
        self.dirty_release(crlf=True)
        expected = next(item["sha256"] for item in self.record["fingerprint_start"]["untracked"] if item["path"] == "src/new.cpp")
        blob = subprocess.check_output(["git", "cat-file", "blob", "HEAD:src/new.cpp"], cwd=self.root)
        self.assertNotEqual(hashlib.sha256(blob).hexdigest(), expected)
        self.assertTrue(self.decide().short)

    def test_one_line_changed_after_dirty_cycle_is_full(self):
        self.dirty_release()
        self.write("src/code.cpp", "tested change\nchanged after testing\n")
        self.commit_all()
        self.assert_full(self.decide(), "src/code.cpp")

    def test_untracked_file_omitted_from_commit_is_full(self):
        self.dirty_release()
        (self.root / "src/new.cpp").unlink()
        self.commit_all()
        self.assert_full(self.decide(), "untracked file missing")

    def test_untracked_file_changed_in_commit_is_full(self):
        self.dirty_release()
        self.write("src/new.cpp", "not what the cycle tested\n")
        self.commit_all()
        self.assert_full(self.decide(), "untracked file content changed")

    def test_untracked_notes_may_change_but_must_be_committed(self):
        self.dirty_release()
        self.assertTrue(self.decide().short)  # Draft notes were edited by release_changes().
        (self.root / "docs/release-notes/1.2.4.html").unlink()
        self.commit_all()
        self.assert_full(self.decide(), "untracked file missing")

    def test_missing_or_tampered_diff_skips_to_another_record(self):
        self.dirty_release()
        older = copy.deepcopy(self.record)
        older["ended_at"] = "2026-11-14T01:00:00Z"
        older["fingerprint_start"].update(head=self.git("rev-parse", "HEAD"), diff_sha256=scope.EMPTY_SHA256, untracked=[])
        self.write_record(older, "r00")
        diff = self.cycles / "fixture/r01/fingerprint_start.diff"
        diff.unlink()
        for contents, reason in ((None, "is missing"), (b"tampered patch", "SHA-256 mismatch")):
            with self.subTest(reason=reason):
                if contents is not None:
                    diff.write_bytes(contents)
                decision = self.decide()
                self.assertTrue(decision.short)
                self.assertIn("fixture\\r00", decision.reason)
                self.assertIn(reason, "\n".join(decision.trace))

    def test_unapplicable_diff_is_rejected(self):
        self.dirty_release()
        invalid = b"not a git patch\n"
        self.record["fingerprint_start"]["diff_sha256"] = hashlib.sha256(invalid).hexdigest()
        self.write_record().with_name("fingerprint_start.diff").write_bytes(invalid)
        self.assert_full(self.decide(), "cannot restore")

    def test_binary_diff_and_isolated_objects_and_index(self):
        self.write("src/data.bin", "initial\0binary")
        self.commit_all()
        (self.root / "src/data.bin").write_bytes(b"tested\0binary\xff")
        # The restored tracked-only tree never existed as a commit: the source commit also adds this file.
        self.write("src/binary-helper.h", "new helper\n")
        self.record_working()
        self.commit_all()
        self.release_changes()
        objects = self.root / ".git/objects"
        before = {str(path.relative_to(objects)): path.read_bytes() for path in objects.rglob("*") if path.is_file()}
        index = (self.root / ".git/index").read_bytes()
        with patch.object(scope.tempfile, "tempdir", str(self.base)):
            self.assertTrue(self.decide().short)
        self.assertEqual(before, {str(path.relative_to(objects)): path.read_bytes() for path in objects.rglob("*") if path.is_file()})
        self.assertEqual(index, (self.root / ".git/index").read_bytes())
        self.assertFalse(list(self.base.glob("release-gate-snapshot-*")))

    def test_explain_rev_uses_tag_even_when_head_content_changed(self):
        self.dirty_release()
        self.git("tag", "v1.2.4")
        self.write("src/code.cpp", "later work\n")
        self.commit_all()
        self.assertFalse(self.decide().short)
        self.assertTrue(self.decide(rev="v1.2.4").short)
        with patch.object(scope, "ROOT", self.root), patch.dict(os.environ, {"RELEASE_GATE_CYCLES_DIR": str(self.cycles)}), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(scope.main(["--explain", "--rev", "v1.2.4"]), 0)
        self.assertIn("Revision: v1.2.4", output.getvalue())
        self.assertIn("MATCH; full passed", output.getvalue())

    def test_historical_rev_without_list_reports_matching_cycle_then_stale_list(self):
        self.git("rm", scope.SKIP_FILE)
        self.record["fingerprint_start"]["head"] = self.commit_all()
        self.write_record()
        self.git("tag", "without-list")
        self.write(scope.SKIP_FILE, scope.skip_file_text(self.root / "tests"))
        self.commit_all()
        decision = self.decide(rev="without-list")
        self.assert_full(decision, scope.STALE_LIST)
        self.assertIn("MATCH; full passed", "\n".join(decision.trace))

    def test_missing_commit_is_skipped(self):
        other = copy.deepcopy(self.record)
        other["ended_at"] = "2026-11-14T03:00:00Z"
        other["fingerprint_start"]["head"] = "a" * 40
        self.write_record(other, "r02")
        self.assertTrue(self.decide().short)
        self.assertIn("fixture\\r01", self.decide().reason)

    def test_uncommitted_skip_list_is_full_even_after_write(self):
        self.git("rm", scope.SKIP_FILE)
        self.record["fingerprint_start"]["head"] = self.commit_all()
        self.write_record()
        with patch.object(scope, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(scope.main(["--write"]), 0)
            self.assertEqual(scope.main(["--check"]), 0)
        self.assert_full(self.decide(), scope.STALE_LIST)

    def test_stale_committed_list_is_full(self):
        self.write(scope.SKIP_FILE, "# old list\n")
        self.record["fingerprint_start"]["head"] = self.commit_all()
        self.write_record()
        self.assert_full(self.decide(), scope.STALE_LIST)

    def test_working_and_force_full_do_not_read_history(self):
        for kwargs in ({"working": True}, {"force_full": True}):
            with self.subTest(kwargs=kwargs), patch.object(scope, "_git", side_effect=AssertionError("read history")):
                self.assertFalse(self.decide(**kwargs).short)

    def test_latest_eligible_cycle_is_selected_by_end_time(self):
        newer = copy.deepcopy(self.record)
        newer["ended_at"] = "2026-11-14T12:30:00+09:00"  # 03:30 UTC
        self.write_record(newer, "r00")
        self.assertIn("fixture\\r00", self.decide().reason)

    def test_latest_ineligible_cycle_does_not_hide_older_evidence(self):
        newer = copy.deepcopy(self.record)
        newer["ended_at"] = "2026-11-14T04:00:00Z"
        newer["stale"] = True
        self.write_record(newer, "r02")
        self.assertIn("fixture\\r01", self.decide().reason)

    def test_broken_json_is_ignored(self):
        self.write_record(round_name="broken").write_text("{broken", encoding="utf-8")
        decision = self.decide()
        self.assertTrue(decision.short)
        self.assertIn("invalid summary", "\n".join(decision.trace))

    def test_environment_cycles_directory_and_no_records(self):
        with patch.dict(os.environ, {"RELEASE_GATE_CYCLES_DIR": str(self.cycles)}):
            self.assertTrue(scope.decide(self.root, "enqueue").short)
        self.assertFalse(scope.decide(self.root, "enqueue", cycles_dir=self.base / "absent").short)

    def test_check_reports_diff_and_write_repairs_it(self):
        self.write(scope.SKIP_FILE, "# stale\n")
        with patch.object(scope, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(scope.main(["--check"]), 1)
            self.assertEqual(scope.main(["--write"]), 0)
            self.assertEqual(scope.main(["--check"]), 0)
        self.assertIn("--- tests/release_gate_skip.txt", output.getvalue())
        self.assertIn("+Offline", output.getvalue())


class SuiteRulesTests(unittest.TestCase):
    def test_build_pc_patterns_in_source_and_recursive_headers_remain(self):
        patterns = ["MP3", "Mp3", "mp3", "MediaFoundation", "WindowsMediaAudioFormat", "WMA", "WASAPI",
                    "Wasapi", "JUCE_ASIO", "IASIO", "VST2", "JUCE_PLUGINHOST_VST", "WinSparkle"]
        with tempfile.TemporaryDirectory(prefix="gate-pc-suites-") as folder:
            tests = Path(folder)
            for index, pattern in enumerate(patterns):
                (tests / f"Direct{index}.cpp").write_text(f'UnitTest ("Direct {index}", "Enqueue") {{}} // {pattern}', encoding="utf-8")
                (tests / f"Via{index}.cpp").write_text(f'#include "First{index}.h"\nUnitTest ("Via {index}", "LiveMix") {{}}', encoding="utf-8")
                (tests / f"First{index}.h").write_text(f'#include "Second{index}.h"', encoding="utf-8")
                (tests / f"Second{index}.h").write_text(f'// {pattern}', encoding="utf-8")
            (tests / "Clock.cpp").write_text('UnitTest ("Collab clock", "LiveMix") {} // ASIO block size', encoding="utf-8")
            self.assertEqual(scope.skip_suites(tests), ["Collab clock"])

    def test_source_headers_names_and_categories(self):
        with tempfile.TemporaryDirectory(prefix="gate-suites-") as folder:
            tests = Path(folder) / "tests"
            (tests / "helpers").mkdir(parents=True)
            files = {
                "Pure.cpp": 'UnitTest ("Z pure", "Enqueue") {} UnitTest ("A pure", "LiveMix") {}',
                "Desktop.cpp": 'UnitTest ("Desktop", "Enqueue") {} // GetForegroundWindow',
                "Headers.cpp": '#include "helpers/First.h"\nUnitTest ("Via header", "LiveMix") {}',
                "helpers/First.h": '#include "Second.hpp"\n',
                "helpers/Second.hpp": '#include "First.h"\n// ComboBox',
                "Korean.cpp": 'UnitTest ("UI scale (글씨·화면 크기)", "Enqueue") {}',
                "Computed.cpp": 'UnitTest (live ? "Live" : "Enqueue", "Enqueue") {}',
                "Other.cpp": 'UnitTest ("MIDI load", "EnqueueMidiLoad") {}',
            }
            for file, text in files.items():
                (tests / file).write_text(text, encoding="utf-8")
            self.assertEqual(scope.skip_suites(tests), ["A pure", "Z pure"])


class ReleaseWiringTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(patch.stopall)
        patch.object(release, "APP", release.APPS["livemix"]).start()
        patch.object(release, "FULL_GATE", False).start()
        self.web = patch.object(release, "web_join_gate").start()

    def test_local_short_and_full_commands_keep_idle_slot_web_order(self):
        for short in (True, False):
            with self.subTest(short=short):
                events = []

                @contextlib.contextmanager
                def slot(what):
                    events.append(("slot", what))
                    yield
                    events.append("free")

                decision = scope.GateDecision(short, "fixture", 1)
                with patch.object(release.release_gate_scope, "decide", return_value=decision) as choose, \
                        patch.object(release, "run", side_effect=lambda command: events.append(command)), \
                        patch.object(release, "wait_for_gate_idle", side_effect=lambda a, b: events.append(("wait", a, b))), \
                        patch.object(release, "heavy_slot", side_effect=slot), \
                        patch.object(release, "web_join_gate", side_effect=lambda: events.append("web")), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    release.build("local", False, 15, 2.0)
                choose.assert_called_once_with(release.ROOT, "livemix", False, False)
                self.assertEqual(events[-5:], [("wait", 15, 2.0), ("slot", "release test gate LiveMix"),
                                             ["ctest", "--preset", "local-release", "-R", decision.ctest_filter], "free", "web"])
                self.assertEqual(output.getvalue().count("GATE_SCOPE:"), 1)

    def test_roomc_short_and_full_commands_keep_exclusive_fetch_web_order(self):
        for short in (True, False):
            with self.subTest(short=short):
                events = []
                fake = SimpleNamespace(HOST="fixture", cmd_sync=lambda args: events.append(("sync", args.working)),
                                       cmd_run=lambda args: events.append((args.command[0], args.exclusive, args.light)) or 0)
                decision = scope.GateDecision(short, "fixture", 1)
                with patch.dict(sys.modules, {"rbuild": fake}), \
                        patch.object(release.release_gate_scope, "decide", return_value=decision) as choose, \
                        patch.object(release, "fetch_roomc_release", side_effect=lambda r: events.append("fetch")), \
                        patch.object(release, "web_join_gate", side_effect=lambda: events.append("web")), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    release.build_on_roomc("local", False, False)
                choose.assert_called_once_with(release.ROOT, "livemix", False, False)
                self.assertEqual(events[0], ("sync", False))
                self.assertEqual(events[1][1:], (False, False))
                self.assertEqual(events[2], (release.roomc_ctest_command("local", decision.ctest_filter), True, False))
                self.assertEqual(events[3:], ["fetch", "web"])
                self.assertEqual(output.getvalue().count("GATE_SCOPE:"), 1)

    def test_skip_tests_never_decides_scope_on_either_builder(self):
        fake = SimpleNamespace(HOST="fixture", cmd_sync=lambda args: None, cmd_run=lambda args: 0)
        with patch.object(release.release_gate_scope, "decide", side_effect=AssertionError("scope consulted")), \
                patch.object(release, "run"), patch.object(release, "heavy_slot", return_value=contextlib.nullcontext()), \
                patch.dict(sys.modules, {"rbuild": fake}), patch.object(release, "fetch_roomc_release"), \
                contextlib.redirect_stdout(io.StringIO()):
            release.build("local", True)
            release.build_on_roomc("local", True, False)
        self.web.assert_not_called()

    def test_imported_build_defaults_to_full_without_real_history(self):
        with patch.object(release, "FULL_GATE", True), patch.object(scope, "_git", side_effect=AssertionError("real history")), \
                patch.object(release, "run") as run, patch.object(release, "wait_for_gate_idle"), \
                patch.object(release, "heavy_slot", return_value=contextlib.nullcontext()), \
                contextlib.redirect_stdout(io.StringIO()):
            release.build("local", False, 15, 2.0)
        self.assertEqual(run.call_args.args[0][-1], scope.FULL_FILTER)

    def test_cli_sets_full_gate_option_without_building(self):
        for flag in ([], ["--full-gate"]):
            with self.subTest(flag=flag), patch.object(sys, "argv", ["release.py", "--site-only", *flag]), \
                    patch.object(release, "deploy_site"):
                release.main()
                self.assertEqual(release.FULL_GATE, bool(flag))

    def test_working_is_forwarded_on_both_builders(self):
        fake = SimpleNamespace(HOST="fixture", cmd_sync=lambda args: None, cmd_run=lambda args: 0)
        with patch.object(release.release_gate_scope, "decide", wraps=scope.decide) as choose, \
                patch.object(scope, "_git", side_effect=AssertionError("history on working build")), \
                patch.object(release, "run"), patch.object(release, "wait_for_gate_idle"), \
                patch.object(release, "heavy_slot", return_value=contextlib.nullcontext()), \
                patch.dict(sys.modules, {"rbuild": fake}), patch.object(release, "fetch_roomc_release"), \
                contextlib.redirect_stdout(io.StringIO()):
            release.build("local", False, working=True)
            release.build_on_roomc("local", False, True)
        self.assertEqual(choose.call_count, 2)
        for call in choose.call_args_list:
            self.assertEqual(call.args[2:], (True, False))


if __name__ == "__main__":
    unittest.main()
