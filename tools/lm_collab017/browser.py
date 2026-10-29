"""R6 public client, real headless Chromium and fake microphone. One sandbox attempt."""
from __future__ import annotations

import functools
from http.server import ThreadingHTTPServer
from pathlib import Path
import sys
import threading
import wave
from urllib.parse import urlsplit, parse_qs

from audio import signal, signal_range

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True  # importing R6 must not create files in another round's directory
sys.path.insert(0, str(ROOT / 'tests/livemix017/r6'))
from run_browser import Handler as R6Handler  # unchanged R6 HTTP forwarding


class Handler(R6Handler):
    def do_POST(self):
        if urlsplit(self.path).path != '/capture':
            return super().do_POST()
        size = int(self.headers.get('Content-Length', 0))
        if not 0 < size <= 8 * 1024 * 1024:
            self.send_error(400)
            return
        offset = int(parse_qs(urlsplit(self.path).query).get('offset', ['0'])[0])
        actual = self.server.capture.stat().st_size if self.server.capture.exists() else 0
        if offset != actual:
            self.send_error(409, 'capture offset mismatch')
            return
        with self.server.capture.open('ab') as f:
            remaining = size
            while remaining:
                data = self.rfile.read(min(remaining, 1024*1024))
                if not data:
                    raise EOFError('browser capture truncated')
                f.write(data)
                remaining -= len(data)
        self.send_response(200)
        self.send_header('Content-Length', '0')
        self.end_headers()


def write_microphone(path: Path, seconds: int) -> Path:
    """The web voice (3,700 Hz + markers) as a 48 kHz mono WAV, written in 10 s pieces without a seam."""
    with wave.open(str(path), 'wb') as wav:
        wav.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        for first in range(0, 48000*seconds, 48000*10):
            pcm = signal_range(48000, first, min(48000*10, 48000*seconds-first), 3)[:, 0]
            wav.writeframes((pcm*32767).round().astype('<i2').tobytes())
    return path


class BrowserPool:
    def __init__(self, output: Path, channel: str | None, fixture_seconds: int = 10):
        self.output, self.channel = output, channel
        # 10/29: Chrome's fake microphone replays its file from the start when it ends, with a short gap the host
        # then hears as a 10-20 ms dropout every file length (exactly every 10 s with the old 10 s file). A soak run
        # passes a file longer than the whole run so the measured voice never crosses that seam.
        self.fixture_seconds = max(10, int(fixture_seconds))
        self.playwright = self.browser = None
        self.blocked = None

    def launch(self):
        if self.blocked:
            raise RuntimeError(self.blocked)
        if self.browser:
            return
        from playwright.sync_api import sync_playwright
        fixture = write_microphone(self.output / 'web-microphone.wav', self.fixture_seconds)
        try:
            self.playwright = sync_playwright().start()
            self.browser = self.playwright.chromium.launch(headless=True, channel=self.channel, chromium_sandbox=True,
                args=['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--mute-audio', '--disable-gpu',
                      f'--use-file-for-fake-audio-capture={fixture.resolve()}', '--autoplay-policy=no-user-gesture-required'])
        except Exception as error:
            self.blocked = f'{type(error).__name__}: {error}'
            raise

    def close(self):
        if self.browser:
            self.browser.close()
        if self.playwright:
            self.playwright.stop()
        self.browser = self.playwright = None


class WebGuest:
    def __init__(self, pool: BrowserPool, output: Path, api: str, invite: str, loopback=True, reflect=False, force_relay=False):
        self.output, self.pool = output, pool
        self.page = self.context = self.site = self.thread = None
        self.result = None
        output.mkdir()
        try:
            pool.launch()
            self.site = ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(ROOT)))
            self.site.daemon_threads = True
            self.site.api, self.site.posts = api, []
            self.site.capture = output / 'capture.f32'
            self.thread = threading.Thread(target=self.site.serve_forever, daemon=True)
            self.thread.start()
            self.context = pool.browser.new_context(permissions=['microphone'])
            self.context.add_init_script('window.__soakForceRelay = ' + ('true;' if force_relay else 'false;') +
                                         (Path(__file__).with_name('rtc_observer.js')).read_text(encoding='utf-8'))
            self.page = self.context.new_page()  # exactly one attempt on sandbox TargetClosedError
            site = f'http://127.0.0.1:{self.site.server_port}'
            self.page.goto(site + '/tools/lm_collab017/entry.html')
            self.page.wait_for_function('!!window.r9')
            self.page.evaluate('c=>r9.start(c)', {'invite': invite, 'apiBase': site+'/r2/', 'loopback': loopback, 'reflect': reflect})
            self.page.wait_for_function("r9.evidence().connectMs || r9.evidence().errors.length", timeout=30000)
            if self.page.evaluate('r9.evidence().errors.length'):
                raise RuntimeError(str(self.page.evaluate('r9.evidence().errors')))
        except Exception as error:
            if 'TargetClosedError' in type(error).__name__ or 'Target page, context or browser has been closed' in str(error):
                pool.blocked = f'{type(error).__name__}: {error}'
            self.close()
            raise

    def mark(self):
        return {'frame': self.page.evaluate('r9.mark()'), 'session': self.page.evaluate('r9.evidence().rosters.at(-1)')}

    def gain(self, person, gain):
        self.page.evaluate('([p,g])=>r9.gain(p,g)', [person, gain])

    def status(self, detailed=False):
        return self.page.evaluate('(d)=>r9.snapshot(d)', detailed)

    def finish_capture(self):
        return self.page.evaluate('r9.finishCapture()')

    def close(self):
        try:
            if self.page and not self.page.is_closed():
                try:
                    self.result = self.page.evaluate('r9.stop()')
                    self.result['browserVersion'] = self.pool.browser.version
                except Exception as error:
                    self.result = {'ok': False, 'error': f'{type(error).__name__}: {error}'}
                import json
                (self.output/'result.json').write_text(json.dumps(self.result, ensure_ascii=False, indent=2), encoding='utf-8')
        finally:
            if self.context:
                self.context.close()
            if self.site:
                self.site.shutdown(); self.site.server_close()
            if self.thread:
                self.thread.join(5)
            self.context = self.site = self.thread = self.page = None


if __name__ == '__main__':
    import argparse
    import json
    import time
    parser=argparse.ArgumentParser(description='One headless Chromium sandbox check; never retry TargetClosedError')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--channel',choices=['chrome','msedge'])
    args=parser.parse_args();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=False)
    pool=BrowserPool(args.output,args.channel);result={'ok':False,'blockedBySandbox':False};started=time.monotonic()
    try:
        pool.launch()
        context=pool.browser.new_context(permissions=['microphone'])
        page=context.new_page()
        page.goto('about:blank')
        result.update(ok=True,browserVersion=pool.browser.version)
        context.close()
    except Exception as error:
        result['error']=f'{type(error).__name__}: {error}'
        result['blockedBySandbox']='TargetClosedError' in type(error).__name__ or 'Target page, context or browser has been closed' in str(error)
    finally:
        pool.close();result['elapsedSeconds']=time.monotonic()-started
        (args.output/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False));sys.exit(0 if result['ok'] else 1)
