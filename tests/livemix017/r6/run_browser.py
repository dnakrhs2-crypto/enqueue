"""Headless R6 audio + public client HTTP/WebRTC, then the unchanged R1 native probe runner.

No launch retries, no sandbox weakening, no visible windows. A TargetClosedError
at new_page stops the run, preserving the command for execution outside Codex.
"""
import argparse
import asyncio
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import threading
import traceback
import urllib.error
import urllib.request
import wave

from run_http import ROOT, HIDDEN, server

CAPS = ['packet_v2', 'roster_ack', 'star_3', 'pcm16_mono', 'web_guest']

def post(base, path, body):
    request = urllib.request.Request(base + path, json.dumps({'apiVersion': 2, **body}).encode(), {'Content-Type': 'text/plain'})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)

def fixture(path):
    with wave.open(str(path), 'wb') as wav:
        wav.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        wav.writeframes(b''.join(struct.pack('<h', round(0.2 * 32767 * math.sin(2 * math.pi * 440 * i / 48000))) for i in range(48000 * 10)))

class Handler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        if not self.path.startswith('/r2/'):
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers['Content-Length']))
        self.server.posts.append({'path': self.path, 'contentType': self.headers.get('Content-Type')})
        request = urllib.request.Request(self.server.api + self.path[4:], body, {'Content-Type': self.headers.get('Content-Type', 'text/plain')})
        try:
            response = urllib.request.urlopen(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read()
            self.send_response(response.status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

async def run(args, result, base, site):
    from playwright.async_api import async_playwright
    async with async_playwright() as playwright:
        browser = None
        try:
            browser = await playwright.chromium.launch(headless=True, channel=args.channel, chromium_sandbox=True,
                args=['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
                      f'--use-file-for-fake-audio-capture={args.output.resolve() / "microphone.wav"}',
                      '--autoplay-policy=no-user-gesture-required'])
            result['browserVersion'] = browser.version
            context = await browser.new_context(permissions=['microphone'])
            # Exactly one attempt; see the R1 sandbox finding.
            page = await context.new_page()
            errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            await page.goto(f'{site}/tests/livemix017/r6/entry.html')
            await page.wait_for_function('!!window.r6')
            result['audio'] = []
            if not args.connection_only:
                for rate in (48000, 44100):
                    print(f'Audio: wire rate {rate}, independent drift {args.drift_seconds}s', flush=True)
                    result['audio'].append(await page.evaluate('config => r6.audioTest(config)', {'rate': rate, 'driftSeconds': args.drift_seconds}))
                    (args.output / f'audio-{rate}.json').write_text(json.dumps(result['audio'][-1], indent=2), encoding='utf-8')
            result['connections'] = []
            for relay, remote_close in ((False, False), (True, False), (False, True)):
                room = post(base, 'create', {'nick': 'R6 host', 'inst': 'r6-browser-host', 'port': 38472,
                    'appVersion': '0.17.0', 'protocols': [1, 2], 'capabilities': CAPS,
                    'connectionId': __import__('secrets').token_hex(16), 'wireRate': 48000})
                try:
                    post(base, 'confirm', {'code': room['code'], 'hostToken': room['hostToken'], 'reachable': False})
                    invite = post(base, 'web/invite', {'code': room['code'], 'hostToken': room['hostToken']})
                    config = {'apiBase': site + '/r2/', 'code': room['code'], 'hostToken': room['hostToken'],
                        'invite': {'code': room['code'], 'inviteToken': invite['inviteToken'], 'inviteGeneration': invite['inviteGeneration']},
                        'forceRelay': relay, 'remoteClose': remote_close}
                    print(f'Connection: relay={relay}, remote close={remote_close}', flush=True)
                    result['connections'].append(await page.evaluate('config => r6.connectionTest(config)', config))
                finally:
                    post(base, 'close', {'code': room['code'], 'hostToken': room['hostToken']})
            result['pageErrors'] = errors
            if errors:
                raise RuntimeError(str(errors))
            result['ok'] = True
            result['complete'] = not args.connection_only and not args.skip_probe
        except Exception:
            if browser and 'page' in locals() and not page.is_closed():
                try:
                    result['lastEvidence'] = await page.evaluate('window.r6Evidence')
                except Exception:
                    pass
            raise
        finally:
            if browser:
                await browser.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, default=ROOT / 'build/vs2022/LiveMixWebGuestProbe_artefacts/Release/LiveMixWebGuestProbe.exe')
    parser.add_argument('--channel', choices=('chrome', 'msedge'), default=None)
    parser.add_argument('--drift-seconds', type=int, default=120)
    parser.add_argument('--connection-only', action='store_true')
    parser.add_argument('--skip-probe', action='store_true', help='Record native probe as not run')
    parser.add_argument('--output', type=Path, default=Path('tests/livemix017/r6/artifacts/browser'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture(args.output / 'microphone.wav')
    result = {'ok': False, 'nativeProbe': 'not run'}
    try:
        with server(args.output) as base:
            http = ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(ROOT)))
            http.api, http.posts = base, []
            threading.Thread(target=http.serve_forever, daemon=True).start()
            try:
                asyncio.run(asyncio.wait_for(run(args, result, base, f'http://127.0.0.1:{http.server_port}'), timeout=2*args.drift_seconds+180))
                result['httpPosts'] = http.posts
            finally:
                http.shutdown()
                http.server_close()
        if not args.skip_probe:
            if not args.probe.is_file():
                raise FileNotFoundError(f'Fetch roomc probe: {args.probe}')
            command = [sys.executable, str(ROOT / 'tests/livemix017/r1/run_browser.py'), '--probe', str(args.probe.resolve()), '--output', str(args.output / 'r1-probe')]
            if args.channel:
                command += ['--channel', args.channel]
            completed = subprocess.run(command, cwd=ROOT, creationflags=HIDDEN, timeout=60)
            result['nativeProbe'] = json.loads((args.output / 'r1-probe/result.json').read_text(encoding='utf-8'))
            if completed.returncode:
                raise RuntimeError('R1 native channel probe failed')
        else:
            result['complete'] = False
    except Exception as error:
        result['ok'] = False
        result['error'] = f'{type(error).__name__}: {error}'
        result['traceback'] = traceback.format_exc()
        result['blockedBySandbox'] = 'TargetClosedError' in type(error).__name__ or 'Target page, context or browser has been closed' in str(error)
        print(result['error'], file=sys.stderr)
    (args.output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'ok': result['ok'], 'blockedBySandbox': result.get('blockedBySandbox', False), 'evidence': str(args.output.resolve())}))
    return 0 if result['ok'] else 1

if __name__ == '__main__':
    sys.exit(main())
