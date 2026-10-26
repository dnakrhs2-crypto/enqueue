"""R12a: run selected native filters on an invisible desktop with disposable settings."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--filter', action='append')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    for key, part in [('TEMP', 'temp'), ('TMP', 'temp'), ('USERPROFILE', 'profile'),
                      ('APPDATA', 'profile/AppData/Roaming'), ('LOCALAPPDATA', 'profile/AppData/Local')]:
        directory = output / part
        directory.mkdir(parents=True, exist_ok=True)
        env[key] = str(directory)
    (output / 'profile/Documents').mkdir(parents=True, exist_ok=True)
    # Explicit per-engine peer names in the existing tests must remain independent.
    for key in ('LIVEMIX_OBS_PEER_RING', 'LIVEMIX_OBS_PEER2_RING'):
        env.pop(key, None)
    env.update(LIVEMIX_ALLOW_MULTI='1', LIVEMIX_COLLAB_LOOPBACK='1',
               LIVEMIX_OBS_RING='Local\\LiveMixR12a.' + uuid.uuid4().hex)
    executable = ROOT / 'build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe'
    harness = Path('C:/Users/claude/tools/claude_harness/run_tests_hidden_desktop.py')
    if not harness.exists():
        harness = ROOT / 'build/r12a-harness/run_tests_hidden_desktop.py'
    results = []
    for label in args.filter or ['LiveMix Collab', 'LiveMix WebGuest', 'LiveMix ObsMetadata', 'LiveMix Recorder']:
        log = output / (label.replace(' ', '-') + '.log')
        completed = subprocess.run([sys.executable, str(harness), '--timeout', '600',
                                    str(executable), str(log), label], cwd=ROOT, env=env,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        text = log.read_text(encoding='utf-8-sig', errors='replace') if log.exists() else ''
        counts = re.search(r'(\d+) passed, (\d+) failed', text)
        passed, failed = (int(counts[1]), int(counts[2])) if counts else (0, 0)
        row = {'filter': label, 'exit': completed.returncode, 'passed': passed, 'failed': failed,
               'ok': completed.returncode == 0 and passed > 0 and failed == 0, 'log': log.name}
        results.append(row)
        print(json.dumps(row), flush=True)
    (output / 'result.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return 0 if all(row['ok'] for row in results) else 1


if __name__ == '__main__':
    sys.exit(main())
