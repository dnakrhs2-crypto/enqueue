"""Tests for release.py's --builder (2026-10-18 roomc build server). No build, no SSH: the readiness check and the
roomc build are fakes.  python tools/test_release_builder.py"""
import importlib.util
import pathlib
import sys
import unittest

sys.dont_write_bytecode = True   # tools/__pycache__/release.cpython-312.pyc is tracked: importing must not dirty the tree
spec = importlib.util.spec_from_file_location("release", pathlib.Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class PickBuilderTests(unittest.TestCase):
    def test_local_never_asks_roomc(self):
        def ready():
            raise AssertionError("local must not touch the network")
        self.assertEqual(release.pick_builder("local", ready), "local")

    def test_auto_uses_roomc_when_it_answers_else_this_pc(self):
        self.assertEqual(release.pick_builder("auto", lambda: True), "roomc")
        self.assertEqual(release.pick_builder("auto", lambda: False), "local")

    def test_roomc_stops_when_it_does_not_answer(self):
        self.assertEqual(release.pick_builder("roomc", lambda: True), "roomc")
        with self.assertRaises(SystemExit):
            release.pick_builder("roomc", lambda: False)


class GateCommandTests(unittest.TestCase):
    def setUp(self):
        self.saved = release.APP
        release.APP = release.APPS["livemix"]

    def tearDown(self):
        release.APP = self.saved

    def test_build_chains_on_success_and_never_exits(self):
        command = release.roomc_build_command("local")
        self.assertNotIn("exit", command)
        self.assertNotIn("ctest", command)
        parts = command.split(" ; if ($LASTEXITCODE -eq 0) { ")
        self.assertEqual(len(parts), 2)
        self.assertIn("cmake.exe' --preset local", parts[0])
        self.assertIn("--build --preset local-release --target LiveMix LiveMixCollabProbe EnqueueTests -- /m /v:m /nologo", parts[1])
        self.assertTrue(command.endswith(" }"))

    def test_only_livemix_builds_the_web_join_probe(self):
        release.APP = release.APPS["enqueue"]
        self.assertEqual(release.build_targets(), ["Enqueue", "EnqueueTests"])
        release.APP = release.APPS["livemix"]
        self.assertEqual(release.build_targets(), ["LiveMix", "LiveMixCollabProbe", "EnqueueTests"])

    def test_gate_runs_this_apps_tests(self):
        self.assertEqual(release.roomc_ctest_command("local"),
                         "& '%s\\ctest.exe' --preset local-release -R '^EnqueueUnitTests$'" % release.ROOMC_CMAKE_BIN)


class BuildOnRoomcTests(unittest.TestCase):
    """The build shares roomc, the gate runs alone (exclusive), then the Release folder comes back."""

    def run_build(self, skip_tests, codes, web_ok=True):
        calls, codes = [], list(codes)

        class FakeRbuild:
            HOST = "roomc"

            @staticmethod
            def cmd_sync(args):
                calls.append(("sync", args.working))

            @staticmethod
            def cmd_run(args):
                calls.append(("run", "ctest" in args.command[0], args.exclusive, args.light))
                return codes.pop(0)

        saved_module, saved_fetch, saved_app = sys.modules.get("rbuild"), release.fetch_roomc_release, release.APP
        saved_gate = release.web_join_gate
        sys.modules["rbuild"] = FakeRbuild
        release.fetch_roomc_release = lambda rbuild: calls.append("fetch")
        release.web_join_gate = lambda: calls.append("web gate") if web_ok else sys.exit("web join gate failed")
        release.APP = release.APPS["livemix"]
        try:
            release.build_on_roomc("local", skip_tests, True)
        finally:
            release.fetch_roomc_release, release.APP, release.web_join_gate = saved_fetch, saved_app, saved_gate
            if saved_module is None:
                sys.modules.pop("rbuild")
            else:
                sys.modules["rbuild"] = saved_module
        return calls

    def test_build_then_exclusive_gate_then_fetch_then_web_join(self):
        self.assertEqual(self.run_build(False, [0, 0]),
                         [("sync", True), ("run", False, False, False), ("run", True, True, False), "fetch", "web gate"])

    def test_skip_tests_builds_and_fetches_without_a_gate(self):
        self.assertEqual(self.run_build(True, [0]), [("sync", True), ("run", False, False, False), "fetch"])

    def test_a_failed_gate_stops_before_the_fetch(self):
        with self.assertRaises(SystemExit):
            self.run_build(False, [0, 8])

    def test_a_failed_web_join_stops_the_release(self):
        with self.assertRaises(SystemExit):
            self.run_build(False, [0, 0], web_ok=False)


class BuildDispatchTests(unittest.TestCase):
    def test_roomc_choice_skips_the_local_build(self):
        calls = []
        saved = release.run, release.pick_builder, release.build_on_roomc
        release.run = lambda cmd, *a, **k: calls.append(("local", cmd[0]))
        release.pick_builder = lambda choice: "roomc" if choice != "local" else "local"
        release.build_on_roomc = lambda preset, skip_tests, working: calls.append(("roomc", preset, skip_tests, working))
        try:
            release.build("local", False, 0, 1.0, "auto", working=True)
            self.assertEqual(calls, [("roomc", "local", False, True)])
        finally:
            release.run, release.pick_builder, release.build_on_roomc = saved


if __name__ == "__main__":
    unittest.main(verbosity=2)
