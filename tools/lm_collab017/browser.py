"""R6 public client, real headless Chromium and fake microphone. One sandbox attempt."""
from __future__ import annotations

import functools
from http.server import ThreadingHTTPServer
from pathlib import Path
import sys
import threading
import wave

from audio import signal

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True  # importing R6 must not create files in another round's directory
sys.path.insert(0, str(ROOT / 'tests/livemix017/r6'))
from run_browser import Handler as R6Handler  # unchanged R6 HTTP forwarding


class Handler(R6Handler):
    def do_POST(self):
        if self.path != '/capture':
            return super().do_POST()
        size = int(self.headers.get('Content-Length', 0))
        if not 0 < size <= 512 * 1024 * 1024:
            self.send_error(400)
            return
        with self.server.capture.open('wb') as f:
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


class BrowserPool:
    def __init__(self, output: Path, channel: str | None):
        self.output, self.channel = output, channel
        self.playwright = self.browser = None
        self.blocked = None

    def launch(self):
        if self.blocked:
            raise RuntimeError(self.blocked)
        if self.browser:
            return
        from playwright.sync_api import sync_playwright
        fixture = self.output / 'web-microphone.wav'
        with wave.open(str(fixture), 'wb') as wav:
            wav.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            pcm = signal(48000, 48000*10, 3)[:, 0]
            wav.writeframes((pcm*32767).round().astype('<i2').tobytes())
        try:
            self.playwright = sync_playwright().start()
            self.browser = self.playwright.chromium.launch(headless=True, channel=self.channel, chromium_sandbox=True,
                args=['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--mute-audio',
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
    def __init__(self, pool: BrowserPool, output: Path, api: str, invite: str, loopback=True, reflect=False):
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
