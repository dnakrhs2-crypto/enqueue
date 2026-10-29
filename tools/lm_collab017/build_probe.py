"""Use rb.py's sync/fetch contract, with an SSH-only build and no snapshot commit."""
import base64
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, 'C:/Users/claude/tools/build_server')
import rb
import rbuild

HIDDEN = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS


def sync(args):
    # rb normally creates a snapshot commit and runs in the logged-on desktop.
    # This run explicitly prohibits both. Sync HEAD, then copy only ProbeV2.cpp.
    args.working = False
    args.commit = 'HEAD'
    original_sync(args)
    source = ROOT / 'tools/lm_collab017/ProbeV2.cpp'
    subprocess.run(rbuild.SCP + [str(source), f'{rbuild.USER}@{rbuild.HOST}:{source.as_posix()}'],
                   check=True, creationflags=HIDDEN)


def run(args):
    script = """
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
Set-Location 'C:/Users/claude/gocue-soak'
git diff --exit-code HEAD -- livemix/src livemix/webjoin server
if ($LASTEXITCODE) { throw 'Product code differs from HEAD' }
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --preset vs2022 -DFETCHCONTENT_SOURCE_DIR_JUCE=C:/Users/claude/JUCE
if ($LASTEXITCODE) { throw 'Configure failed' }
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --build --preset vs2022-release --target LiveMixCollabProbe --parallel 2
exit $LASTEXITCODE
"""
    encoded = base64.b64encode(script.encode('utf-16-le')).decode('ascii')
    return subprocess.run(rbuild.SSH + ['powershell -NoProfile -NonInteractive -EncodedCommand ' + encoded],
                          creationflags=HIDDEN).returncode


if __name__ == '__main__':
    # Read this already-authorized checkout without changing global git config.
    config_count = int(os.environ.get('GIT_CONFIG_COUNT', '0'))
    os.environ.update({ 'GIT_CONFIG_COUNT': str(config_count + 1),
                       f'GIT_CONFIG_KEY_{config_count}': 'safe.directory',
                       f'GIT_CONFIG_VALUE_{config_count}': 'C:/Users/claude/JUCE' })
    for command in (['git', 'diff', '--exit-code', '5545f809', '--', 'livemix/src', 'livemix/webjoin', 'server'],):
        subprocess.run(command, cwd=ROOT, check=True, creationflags=HIDDEN)
    original_sync = rbuild.cmd_sync
    rbuild.cmd_sync, rbuild.cmd_run = sync, run
    sys.argv = ['rb.py', '--fetch', 'build/vs2022/LiveMixCollabProbe_artefacts/Release', str(ROOT), '--', 'SSH-only LiveMixCollabProbe build']
    sys.exit(rb.main())
