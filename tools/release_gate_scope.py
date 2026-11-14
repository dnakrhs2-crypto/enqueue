"""Select the release gate by comparing a complete dev_cycle snapshot with release content.

The CLI only inspects source/history; it never builds or runs a release.
"""
import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CYCLES_DIR = Path(r"C:\Users\claude\tools\claude_harness\cycles")
SKIP_FILE = "tests/release_gate_skip.txt"
FULL_FILTER = "^EnqueueUnitTests$"
SHORT_FILTER = "^EnqueueGateShortTests$"
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
STALE_LIST = "목록이 낡음 — `--write` 후 커밋"
VERSION_PATTERNS = {
    "enqueue": re.compile(r"project\(Enqueue\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)"),
    "livemix": re.compile(r'set\(LIVEMIX_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"'),
}
NOTES_DIRS = {"enqueue": "docs/release-notes", "livemix": "docs/release-notes/livemix"}
DESKTOP_PATTERN = re.compile(
    r"isForegroundProcess|isForegroundOrEmbeddedProcess|GetForegroundWindow|SetForegroundWindow|"
    r"BringWindowToTop|SetActiveWindow|GetActiveWindow|\bSetFocus\b|AttachThreadInput|ShortcutRouter|isActiveWindow|toFront|"
    r"PopupMenu|ComboBox|showPopup|isPopupActive|showMenuAsync|CallOutBox|BubbleComponent|TooltipWindow|AlertWindow|MessageBox|"
    r"KeyboardFocus|getCurrentlyFocusedComponent|focusLost|focusGained|focusOfChildComponentChanged|"
    r"keyPressed|keyStateChanged|KeyPress\b|KeyPressMappingSet|ApplicationCommandManager|isKeyCurrentlyDown|"
    r"getCurrentModifiersRealtime|GetAsyncKeyState|GetKeyState|RegisterHotKey|SendInput|keybd_event|WM_(SYS)?KEY(DOWN|UP)|WM_CHAR|"
    r"enterModalState|runModalLoop|exitModalState|ModalComponentManager|isCurrentlyBlockedByAnotherModalComponent|launchAsync|DialogWindow|"
    r"addToDesktop|isOnDesktop|DocumentWindow|ResizableWindow|TopLevelWindow|setVisible\s*\(\s*true\s*\)|"
    r"ComponentPeer|getPeer\s*\(|\bHWND\b|FindWindow|WM_ACTIVATE|"
    r"getMainMouseSource|getMousePosition|setScreenPosition|SetCursorPos|GetCursorPos|mouse_event"
)
BUILD_PC_PATTERN = re.compile(
    r"MP3|Mp3|mp3|MediaFoundation|WindowsMediaAudioFormat|\bWMA\b|WASAPI|Wasapi|"
    r"JUCE_ASIO|IASIO|VST2|JUCE_PLUGINHOST_VST\b|WinSparkle"
)
# Only plain literal names/categories: expressions and escaped/non-ASCII names stay in the gate.
SUITE_PATTERN = re.compile(r'\bUnitTest\s*\(\s*"([^"\\\r\n]+)"\s*,\s*"(?:Enqueue|LiveMix)"\s*\)')
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*"([^"\r\n]+)"', re.M)


def skip_suites(tests_dir):
    """ASCII literal suites without desktop/build-PC patterns in source or tests-local headers."""
    tests_dir = Path(tests_dir).resolve()

    def source_with_headers(path, seen):
        if path in seen:
            return ""
        seen.add(path)
        source = path.read_text(encoding="utf-8-sig")
        texts = [source]
        for include in INCLUDE_PATTERN.findall(source):
            for candidate in (path.parent / include, tests_dir / include):
                header = candidate.resolve()
                if (header.is_relative_to(tests_dir) and header.is_file()
                        and header.suffix.lower() in (".h", ".hpp", ".hh", ".hxx", ".inl")):
                    texts.append(source_with_headers(header, seen))
                    break
        return "\n".join(texts)

    skipped, retained = set(), set()
    for path in sorted(tests_dir.rglob("*.cpp")):
        source = path.read_text(encoding="utf-8-sig")
        names = {name for name in SUITE_PATTERN.findall(source) if name.isascii()}
        if names:
            combined = source_with_headers(path, set())
            destination = retained if DESKTOP_PATTERN.search(combined) or BUILD_PC_PATTERN.search(combined) else skipped
            destination.update(names)
    # A name shared with a desktop-sensitive suite must never exclude that suite either.
    return sorted(skipped - retained)


def skip_file_text(tests_dir):
    return ("# Release gate: suites covered by a matching full dev_cycle snapshot.\n"
            "# Retain desktop patterns: foreground, popups, focus, keys, modal dialogs, windows and mouse position.\n"
            "# Retain build-PC patterns: MP3/Media Foundation/WMA, WASAPI, JUCE ASIO, VST2 and WinSparkle.\n"
            "# Generated from tests/*.cpp and recursively included tests-local headers.\n"
            "# Regenerate: python tools/release_gate_scope.py --write (then commit this file).\n"
            "# Verify: python tools/release_gate_scope.py --check\n"
            + "".join(name + "\n" for name in skip_suites(tests_dir)))


@dataclass(frozen=True)
class GateDecision:
    short: bool
    reason: str
    skip_count: int = 0
    trace: tuple = ()

    @property
    def ctest_filter(self):
        return SHORT_FILTER if self.short else FULL_FILTER

    def __str__(self):
        if self.short:
            return f"GATE_SCOPE: short - skip {self.skip_count} suites, about 6 min ({self.reason})"
        return f"GATE_SCOPE: full - about 10 min ({self.reason})"


def _git(root, *args, check=True, env=None, raw=False):
    result = subprocess.run(["git", *args], cwd=root, capture_output=True, env=env)
    if not raw:
        result.stdout = result.stdout.decode("utf-8")
    result.stderr = result.stderr.decode("utf-8", errors="replace")
    if check and result.returncode:
        raise ValueError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result


def _cycle_problem(record):
    stages = record.get("stages")
    full = stages.get("full") if isinstance(stages, dict) else None
    if not isinstance(full, dict) or full.get("status") != "passed":
        return "full stage did not pass"
    steps = full.get("steps")
    if not isinstance(steps, list) or not steps or any(
            not isinstance(step, dict) or step.get("test_filter") is not None or step.get("exit_code") != 0
            for step in steps):
        return "full steps are missing, filtered or failed"
    start, end = record.get("fingerprint_start"), record.get("fingerprint_end")
    if (record.get("stale") is not False or not isinstance(start, dict) or not isinstance(end, dict)
            or not start.get("sha256") or start["sha256"] != end.get("sha256")):
        return "stale run or different fingerprints"
    if not isinstance(start.get("head"), str) or not re.fullmatch(r"[0-9a-fA-F]{40,64}", start["head"]):
        return "cycle has no commit hash"
    if not isinstance(start.get("diff_sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", start["diff_sha256"]):
        return "cycle has no valid diff hash"
    untracked = start.get("untracked")
    if not isinstance(untracked, list) or any(
            not isinstance(item, dict) or not isinstance(item.get("path"), str) or not item["path"]
            or not isinstance(item.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])
            for item in untracked):
        return "cycle has invalid untracked file hashes"
    return None


def _tested_tree(root, fingerprint, cycle_dir, env):
    """Rebuild tracked content using only the caller's temporary index/object directory."""
    diff = cycle_dir / "fingerprint_start.diff"
    if fingerprint["diff_sha256"] != EMPTY_SHA256:
        if not diff.is_file():
            raise ValueError("fingerprint_start.diff is missing")
        if hashlib.sha256(diff.read_bytes()).hexdigest() != fingerprint["diff_sha256"]:
            raise ValueError("fingerprint_start.diff SHA-256 mismatch")
    _git(root, "read-tree", fingerprint["head"], env=env)
    if fingerprint["diff_sha256"] != EMPTY_SHA256:
        _git(root, "apply", "--cached", "--binary", "--whitespace=nowarn", str(diff), env=env)
    return _git(root, "write-tree", env=env).stdout.strip()


def _release_changes(root, tree, head, app, current, untracked, env):
    """Only version/current notes and byte-identical additions recorded as untracked may differ."""
    fields = _git(root, "diff", "--name-status", "-z", "--no-renames", tree, head, "--", env=env).stdout.split("\0")
    changes = list(zip(fields[0:-1:2], fields[1::2]))
    paths = [path for _, path in changes]
    match = VERSION_PATTERNS[app].search(current)
    if not match:
        return paths, "target commit has no app version"
    notes = f"{NOTES_DIRS[app]}/{match.group(1)}.html"
    hashes = {item["path"]: item["sha256"] for item in untracked}
    for status, path in changes:
        if path == notes and status in ("A", "M"):
            continue
        if path == "CMakeLists.txt" and status == "M":
            continue
        if status == "A" and path in hashes:
            continue
        return paths, f"change outside this app's version/current notes or recorded additions: {status} {path}"
    if ("M", "CMakeLists.txt") in changes:
        previous = _git(root, "show", f"{tree}:CMakeLists.txt", env=env).stdout
        old_match = VERSION_PATTERNS[app].search(previous)
        if (not old_match or previous[:old_match.start(1)] + previous[old_match.end(1):]
                != current[:match.start(1)] + current[match.end(1):]):
            return paths, "CMakeLists.txt changed beyond this app's version numbers"
    for path, digest in hashes.items():
        if ("A", path) not in changes:
            return paths, f"untracked file missing from target additions: {path}"
        if path == notes:  # Current release notes may be edited after the cycle.
            continue
        for mode in ("blob", "--filters"):
            content = _git(root, "cat-file", mode, f"{head}:{path}", env=env, raw=True, check=False)
            if content.returncode == 0 and hashlib.sha256(content.stdout).hexdigest() == digest:
                break
        else:
            return paths, f"untracked file content changed: {path}"
    return paths, None


def decide(root, app, working=False, force_full=False, cycles_dir=None, rev="HEAD"):
    """Fail closed unless a complete tested snapshot covers the target's non-release content."""
    root = Path(root)
    trace = []

    def full(reason):
        return GateDecision(False, reason, trace=tuple(trace))

    if working:
        return full("working tree build (--allow-dirty / --package-only)")
    if force_full:
        return full("full gate requested")
    if app not in VERSION_PATTERNS:
        return full("app does not use the short gate")
    cycles_dir = Path(cycles_dir if cycles_dir is not None else os.environ.get("RELEASE_GATE_CYCLES_DIR", DEFAULT_CYCLES_DIR))
    try:
        head = _git(root, "rev-parse", "--verify", f"{rev}^{{commit}}").stdout.strip()
        trace.extend((f"Revision: {rev} -> {head} ({app})", f"Cycles: {cycles_dir}"))
        current = _git(root, "show", f"{head}:CMakeLists.txt").stdout
        common = Path(_git(root, "rev-parse", "--path-format=absolute", "--git-common-dir").stdout.strip())
        records = []
        for path in sorted(cycles_dir.glob("*/*/summary.json")):
            label = "\\".join(path.parts[-3:-1])
            try:
                record = json.loads(path.read_text(encoding="utf-8-sig"))
                if not isinstance(record, dict):
                    raise ValueError("summary is not an object")
                ended = datetime.fromisoformat(record["ended_at"])
                if ended.tzinfo is None:
                    ended = ended.replace(tzinfo=timezone.utc)
                records.append((ended, label, record, path.parent))
            except (OSError, ValueError, KeyError, TypeError) as error:
                trace.append(f"Ignore {label}: invalid summary ({error})")
        trace.append(f"Records: {len(records)} readable, newest first")
        last_reason = "no complete cycle matches the target content (see --explain)"
        nearest = None  # why the newest complete cycle did not match: the reason a reader of the release log needs
        with tempfile.TemporaryDirectory(prefix="release-gate-snapshot-") as temporary:
            scratch = Path(temporary)
            (scratch / "objects").mkdir()
            env = dict(os.environ, GIT_INDEX_FILE=str(scratch / "index"),
                       GIT_OBJECT_DIRECTORY=str(scratch / "objects"),
                       GIT_ALTERNATE_OBJECT_DIRECTORIES=str(common / "objects"))
            for ended, label, record, cycle_dir in sorted(records, key=lambda item: (item[0], item[1]), reverse=True):
                started = time.perf_counter()
                problem = _cycle_problem(record)
                if problem:
                    trace.append(f"Cycle {label} ({ended.isoformat()}): {problem}")
                    last_reason = problem
                    continue
                fingerprint = record["fingerprint_start"]
                commit = fingerprint["head"]
                try:
                    tree = _tested_tree(root, fingerprint, cycle_dir, env)
                    paths, problem = _release_changes(root, tree, head, app, current, fingerprint["untracked"], env)
                    trace.append(f"Diff cycle {label} ({tree[:10]})..{head[:10]}: {', '.join(paths) or '(none)'}")
                except (OSError, ValueError) as error:
                    problem = f"cannot restore/compare tested tree: {error}"
                elapsed = time.perf_counter() - started
                if problem:
                    trace.append(f"Cycle {label}: {problem} ({elapsed:.3f}s)")
                    last_reason = problem
                    nearest = nearest or f"newest complete cycle {label}: {problem}"
                    continue
                trace.append(f"Cycle {label}: MATCH; full passed, stable fingerprints; base {commit[:10]} ({elapsed:.3f}s)")
                trace.append("Changes: only this app's version/current notes after verifying recorded untracked additions (or no changes)")
                generated = skip_file_text(root / "tests")
                committed = _git(root, "show", f"{head}:{SKIP_FILE}", check=False)
                if committed.returncode or committed.stdout != generated:
                    trace.append(f"Skip list: {STALE_LIST} ({rev}:{SKIP_FILE})")
                    return full(STALE_LIST)
                count = len(skip_suites(root / "tests"))
                trace.append(f"Skip list: committed list matches generated list ({count} suites)")
                reason = (f"cycle {label} passed the whole suite on snapshot based on {commit[:10]}; "
                          f"only version/notes or verified additions: {', '.join(paths) or 'no changes'}")
                return GateDecision(True, reason, count, tuple(trace))
        return full(nearest or last_reason)
    except (OSError, ValueError) as error:
        trace.append(f"Inspection failed: {error}")
        return full(f"cannot verify gate scope: {error}")


def print_decision(decision, explain=False):
    # Windows redirected stdout defaults to cp949, which cannot encode the stale-list explanation.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if explain:
        for line in decision.trace:
            print(line)
    print(decision, flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--write", action="store_true", help="regenerate tests/release_gate_skip.txt")
    actions.add_argument("--check", action="store_true", help="compare the checked-out skip file with source")
    actions.add_argument("--explain", action="store_true", help="inspect a commit and cycle evidence without building")
    parser.add_argument("--app", choices=tuple(VERSION_PATTERNS), default="enqueue")
    parser.add_argument("--rev", default="HEAD", help="commit or release tag to inspect with --explain (default HEAD)")
    args = parser.parse_args(argv)
    if args.explain:
        decision = decide(ROOT, args.app, rev=args.rev)
        print_decision(decision, explain=True)
        return 0
    expected = skip_file_text(ROOT / "tests")
    path = ROOT / SKIP_FILE
    if args.write:
        # Keep an existing checkout's newline/BOM convention.
        previous = path.read_bytes() if path.exists() else b""
        newline = "\r\n" if b"\r\n" in previous else "\n"
        encoding = "utf-8-sig" if previous.startswith(b"\xef\xbb\xbf") else "utf-8"
        path.write_text(expected, encoding=encoding, newline=newline)
        print(f"Wrote {SKIP_FILE}: {len(skip_suites(ROOT / 'tests'))} suites")
        return 0
    actual = path.read_text(encoding="utf-8-sig") if path.exists() else ""
    if actual != expected:
        print("".join(difflib.unified_diff(actual.splitlines(True), expected.splitlines(True),
                                         fromfile=SKIP_FILE, tofile="generated")), end="")
        return 1
    print(f"OK: {SKIP_FILE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
