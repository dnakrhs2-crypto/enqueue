"""Capture the actual offline UI at the three requested device sizes.

python tools/webremote_mock/shots.py
python tools/webremote_mock/shots.py --browser webkit --output tools/webremote_mock/shots/webkit
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import threading
from urllib.request import Request, urlopen

from playwright.sync_api import expect, sync_playwright

from mock_server import MockServer


HERE = Path(__file__).resolve().parent
PROFILES = {
    "phone": dict(viewport=dict(width=390, height=664), is_mobile=True, has_touch=True, device_scale_factor=3,
                  user_agent="Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Version/18.0 Mobile/15E148 Safari/604.1"),
    "tablet-landscape": dict(viewport=dict(width=1180, height=746), is_mobile=True, has_touch=True, device_scale_factor=1,
                             user_agent="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) AppleWebKit/605.1.15 Version/18.0 Safari/605.1.15"),
    "tablet-portrait": dict(viewport=dict(width=820, height=1106), is_mobile=True, has_touch=True, device_scale_factor=1,
                            user_agent="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) AppleWebKit/605.1.15 Version/18.0 Safari/605.1.15"),
}


@contextmanager
def fixture_server(port=49831):
    # An in-process thread needs no visible helper window on Windows.
    server = MockServer(("127.0.0.1", port), frozen=True)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    base = f"http://127.0.0.1:{server.server_port}"
    print(f"{base}/#k=test", flush=True)
    try:
        yield base
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


def control(base, **data):
    request = Request(base + "/__mock__/control", data=json.dumps(data).encode("utf-8"),
                      headers={"Content-Type": "application/json"})
    with urlopen(request, timeout=5) as response:
        return json.load(response)


def inspect(base):
    with urlopen(base + "/__mock__/inspect", timeout=5) as response:
        return json.load(response)


def connected(page):
    expect(page.locator("#connection-label")).to_have_text("연결됨")
    expect(page.locator("#go")).to_be_enabled()


def row(page, cue_id):
    return page.locator(f'#cue-list .cue-row[data-cue="{cue_id}"]')


def start_drag(page, cue_id="c2", before="c11"):
    page.locator("#list-scroll").evaluate("el => { el.scrollTop = 0; }")
    handle = row(page, cue_id).locator(".drag-handle")
    handle.scroll_into_view_if_needed()
    box = handle.bounding_box()
    target = row(page, before).bounding_box()
    page.mouse.move(box["x"] + 24, box["y"] + 24)
    page.mouse.down()
    page.mouse.move(box["x"] + 24, target["y"] + 8, steps=6)
    try:
        expect(page.locator("#drop-line")).to_be_visible()
    except AssertionError:
        (HERE / "shots").mkdir(exist_ok=True)
        page.screenshot(path=str(HERE / "shots" / "drag-failure.png"))
        print(page.evaluate("""() => ({root:document.getElementById('remote').className,
          ghost:!!document.querySelector('.drag-ghost'),
          scroll:document.getElementById('list-scroll').scrollTop,
          rows:[...document.querySelectorAll('#cue-list .cue-row')].map(e => ({id:e.dataset.cue,y:e.getBoundingClientRect().y}))})"""), flush=True)
        raise


def capture(browser, base, output):
    results = []
    for name, options in PROFILES.items():
        control(base, reset=True, frozen=True)
        context = browser.new_context(**options)
        if name.startswith("tablet"):
            context.add_init_script("Object.defineProperty(navigator, 'maxTouchPoints', {get: () => 5})")
        page = context.new_page()
        errors, external = [], []
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.on("request", lambda request: external.append(request.url) if not request.url.startswith(base) else None)
        page.goto(base + "/#k=test")
        connected(page)

        def shot(state):
            filename = f"{name}-{state}.png"
            page.screenshot(path=str(output / filename), animations="disabled")
            results.append(dict(file=filename, viewport=options["viewport"], dpr=options["device_scale_factor"]))
            print(filename, flush=True)

        shot("default")
        page.locator('[data-show="false"]').click()
        expect(page.locator("#remote")).to_have_class("remote is-connected mode-edit")
        shot("edit")
        start_drag(page)
        shot("dragging")
        page.mouse.up()
        expect(row(page, "c2")).not_to_have_class(re.compile("pending-move"))
        # Restore the reference order before the other screenshots.
        control(base, reset=True, frozen=True)
        expect(page.locator('[data-show="true"]')).to_have_attribute("aria-pressed", "true")
        shot("show")
        page.locator('[data-tab="fx"]').click()
        expect(page.locator("#cart-scroll")).to_be_visible()
        expect(page.locator(".cart-cell")).to_have_count(12)
        shot("cart")
        if name != "tablet-landscape":
            page.locator('[data-tab="main"]').click()
            expect(page.locator("#list-scroll")).to_be_visible()
            page.locator('[data-local="active"]').click()
            expect(page.locator("#active-panel")).to_be_visible()
            shot("active")
        page.locator('[data-tab="main"]').click()
        expect(page.locator("#list-scroll")).to_be_visible()
        control(base, silence=8)
        expect(page.locator("#connection-label")).to_have_text("다시 연결 중", timeout=5500)
        expect(page.locator("#go")).to_be_disabled()
        shot("reconnecting")
        assert not errors, errors
        assert not external, external
        context.close()
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=49831)
    parser.add_argument("--browser", choices=["chromium", "webkit"], default="chromium")
    parser.add_argument("--output", type=Path, default=HERE / "shots")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with fixture_server(args.port) as base, sync_playwright() as playwright:
        browser = getattr(playwright, args.browser).launch(headless=True)
        try:
            results = capture(browser, base, args.output)
            (args.output / "results.json").write_text(json.dumps(dict(browser=args.browser, captures=results),
                                                                ensure_ascii=False, indent=2), encoding="utf-8")
        finally:
            browser.close()
    print(f"Saved {len(results)} captures to {args.output}", flush=True)


if __name__ == "__main__":
    main()
