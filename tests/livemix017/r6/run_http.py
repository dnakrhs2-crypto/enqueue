"""Run the R6 JS signaling client against an unmodified standalone R2 server."""
import argparse
import contextlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
HIDDEN = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0

def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

@contextlib.contextmanager
def server(output):
    port = free_port()
    with (output / 'r2-server.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([sys.executable, str(ROOT / 'server/ytdown/livemix_collab.py'), '--serve', f'127.0.0.1:{port}'],
                                   cwd=ROOT, stdout=log, stderr=log, creationflags=HIDDEN)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError('R2 server exited; see r2-server.log')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError('R2 server startup timeout')
            yield f'http://127.0.0.1:{port}/api/livemix/collab/'
        finally:
            process.terminate()
            process.wait(timeout=10)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('tests/livemix017/r6/artifacts/http'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with server(args.output) as base:
        return subprocess.run(['node', str(Path(__file__).with_name('http.mjs')), base, str((args.output / 'result.json').resolve())],
                              cwd=ROOT, creationflags=HIDDEN, timeout=60).returncode

if __name__ == '__main__':
    sys.exit(main())
