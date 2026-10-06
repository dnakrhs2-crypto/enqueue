"""Tests for release.py's quiet-moment start of the ctest gate (2026-10-06). No build, no ctest, no real waiting:
the idle reader, sleep and clock are fakes, and build()'s run() is recorded.  python tools/test_release_gate_idle.py"""
import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("release", pathlib.Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class Clock:
    def __init__(self):
        self.t = 0.0
        self.sleeps = []

    def now(self):
        return self.t

    def sleep(self, s):
        self.sleeps.append(s)
        self.t += s


def wait(idles, idle_s=20, max_min=30):
    clock, seq = Clock(), iter(idles)
    last = [None]

    def idle_fn():
        last[0] = next(seq, last[0])
        return last[0]
    return release.wait_for_gate_idle(idle_s, max_min, idle_fn, clock.sleep, clock.now), clock


class GateIdleTests(unittest.TestCase):
    def test_already_quiet_starts_without_waiting(self):
        result, clock = wait([25.0])
        self.assertEqual(result, "idle")
        self.assertEqual(clock.sleeps, [])

    def test_waits_until_quiet(self):
        result, clock = wait([1.0, 3.0, 12.0, 21.0])
        self.assertEqual(result, "idle")
        self.assertEqual(len(clock.sleeps), 3)
        self.assertTrue(all(0.5 <= s <= 5.0 for s in clock.sleeps))

    def test_boundary_exactly_the_idle_time_starts(self):
        self.assertEqual(wait([19.9, 20.0])[0], "idle")

    def test_never_quiet_starts_anyway_after_max(self):
        result, clock = wait([0.5] * 10000, max_min=1)
        self.assertEqual(result, "timeout")
        self.assertGreaterEqual(clock.t, 60)
        self.assertLess(clock.t, 66)

    def test_off_and_unknown_never_block(self):
        self.assertEqual(wait([0.0], idle_s=0)[0], "off")
        self.assertEqual(wait([None])[0], "unknown")

    def test_real_reader_returns_seconds(self):
        idle = release.seconds_since_input()
        self.assertTrue(idle is None or idle >= 0)

    def test_build_waits_right_before_ctest_only_when_testing(self):
        calls = []
        saved = release.run, release.wait_for_gate_idle
        release.run = lambda cmd, *a, **k: calls.append(cmd[0])
        release.wait_for_gate_idle = lambda idle_s, max_min: calls.append(("wait", idle_s, max_min))
        try:
            release.build("local", False, 15, 2.0)
            self.assertEqual(calls, ["cmake", "cmake", ("wait", 15, 2.0), "ctest"])
            calls.clear()
            release.build("local", True)
            self.assertEqual(calls, ["cmake", "cmake"])
        finally:
            release.run, release.wait_for_gate_idle = saved

    def test_command_line_defaults(self):
        self.assertEqual((release.GATE_IDLE_S, release.GATE_IDLE_MAX_MIN), (20, 30.0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
