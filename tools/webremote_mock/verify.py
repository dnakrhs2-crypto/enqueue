"""Browser checks for the normal operator flows and the protocol's connection gates.

python tools/webremote_mock/verify.py
python tools/webremote_mock/verify.py --browser webkit
Uses the same stdlib fixture as shots.py; no production testing hooks.
"""

from __future__ import annotations

import argparse
import copy
import json
import re

from playwright.sync_api import expect, sync_playwright

from shots import PROFILES, connected, control, fixture_server, inspect, row, start_drag


def messages(base, kind):
    return [message for message in inspect(base)["messages"] if message["t"] == kind]


def until(page, predicate, timeout=5000):
    for _ in range(timeout // 50):
        if predicate():
            return
        page.wait_for_timeout(50)
    raise AssertionError("Condition did not become true")


def reset(page, base):
    control(base, reset=True, frozen=True)
    page.goto(base + "/#k=test")
    connected(page)
    page.locator("#list-scroll").evaluate("el => { el.scrollTop = 0; }")


def center(locator):
    expect(locator).to_be_visible()
    rect = locator.bounding_box()
    assert rect
    return rect["x"] + rect["width"] / 2, rect["y"] + rect["height"] / 2


def assert_layout(page):
    result = page.evaluate("""() => {
      const root = document.getElementById('remote');
      const controls = [...document.querySelectorAll('button')].filter(el => !el.hidden && el.getClientRects().length);
      const small = controls.filter(el => { const r = el.getBoundingClientRect(); return r.width < 47.9 || r.height < 47.9; })
        .map(el => ({text:el.textContent, cls:el.className, w:el.getBoundingClientRect().width, h:el.getBoundingClientRect().height}));
      return {small, overflow:root.scrollWidth > innerWidth || document.documentElement.scrollWidth > innerWidth,
        height:root.getBoundingClientRect().height, viewport:innerHeight};
    }""")
    assert not result["small"], result
    assert not result["overflow"], result
    assert abs(result["height"] - result["viewport"]) <= 1, result


def operator_flows(browser, base, engine):
    context = browser.new_context(**PROFILES["phone"])
    page = context.new_page()
    errors = []
    external = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.on("request", lambda request: external.append(request.url) if not request.url.startswith(base) else None)
    reset(page, base)
    assert_layout(page)

    # GO runs at pointerdown, then releases exactly once with the same touch ID.
    x, y = center(page.locator("#go"))
    page.mouse.move(x, y)
    page.mouse.down()
    until(page, lambda: len(messages(base, "go")) == 1)
    assert messages(base, "go")[0]["down"] is True
    expect(page.locator("#go")).to_have_class(re.compile("is-pressed"))
    page.mouse.up()
    until(page, lambda: len(messages(base, "go")) == 2)
    pair = messages(base, "go")
    assert [item["down"] for item in pair] == [True, False] and pair[0]["touch"] == pair[1]["touch"]
    assert any(item["id"] == "c2" for item in inspect(base)["state"]["playing"])
    page.touchscreen.tap(x, y)
    until(page, lambda: len(messages(base, "go")) == 4)
    assert messages(base, "go")[2]["touch"] != pair[0]["touch"]

    reset(page, base)
    active_before = inspect(base)["state"]["playing"]
    row(page, "c11").click()
    until(page, lambda: inspect(base)["lists"]["playhead"]["main"] == "c11")
    expect(row(page, "c11")).to_have_class(re.compile("is-next"))
    assert inspect(base)["state"]["playing"] == active_before, "List selection must never fire"
    before = len(messages(base, "playhead"))
    x, y = center(row(page, "c12"))
    page.mouse.move(x, y)
    page.mouse.down()
    page.mouse.move(x + 20, y, steps=4)
    page.mouse.move(x, y, steps=4)
    page.mouse.up()
    page.wait_for_timeout(100)
    assert len(messages(base, "playhead")) == before, "A moved pointer is not a tap"
    page.mouse.down()
    page.wait_for_timeout(550)
    page.mouse.up()
    assert len(messages(base, "playhead")) == before, "A long press is not a tap"

    if engine == "chromium":
        cdp = context.new_cdp_session(page)
        x, y = center(row(page, "c2"))
        cdp.send("Input.dispatchTouchEvent", {"type": "touchStart", "touchPoints": [{"x": x, "y": y, "id": 1}]})
        for offset in (20, 45, 80, 120):
            cdp.send("Input.dispatchTouchEvent", {"type": "touchMove", "touchPoints": [{"x": x, "y": y - offset, "id": 1}]})
            page.wait_for_timeout(30)
        cdp.send("Input.dispatchTouchEvent", {"type": "touchEnd", "touchPoints": []})
        page.wait_for_timeout(200)
        assert page.locator("#list-scroll").evaluate("el => el.scrollTop") > 0
        assert len(messages(base, "playhead")) == before
        cdp.detach()

    # Pause/resume and cancellation retain the exact active instance and wait identity.
    reset(page, base)
    page.locator('[data-local="active"]').click()
    assert messages(base, "tab") == []
    card = page.locator('.active-card[data-cue="c11"]')
    card.get_by_role("button", name="일시정지", exact=True).click()
    expect(card).to_have_class(re.compile("is-pause"))
    held = card.locator(".active-time").inner_text()
    page.wait_for_timeout(220)
    assert card.locator(".active-time").inner_text() == held
    card.get_by_role("button", name="재개", exact=True).click()
    expect(card).to_have_class(re.compile("is-play"))
    card.get_by_role("button", name="×", exact=True).click()
    expect(card).to_have_count(0)
    wait = page.locator('.active-card.is-wait[data-cue="c3"]')
    wait.get_by_role("button", name="×", exact=True).click()
    expect(wait).to_have_count(0)
    assert messages(base, "wait")[-1] | {"id": 0} == dict(t="wait", id=0, cue="c3", kind="pre", startId=1)

    # Cart placement is slot-based; the close control must not fire a new cue.
    page.locator('[data-tab="fx"]').click()
    expect(page.locator("#cart-scroll")).to_be_visible()
    assert page.locator("#cart-scroll").evaluate("el => el.scrollWidth <= el.clientWidth")
    fx = page.locator('.cart-cell[data-cue="fx1"]')
    fx.locator(".cart-fire").tap()
    expect(fx).to_have_class(re.compile("is-play"))
    fire_count = len(messages(base, "fire"))
    fx.locator(".cart-stop").tap()
    expect(fx.locator(".cart-stop")).to_be_hidden()
    assert len(messages(base, "fire")) == fire_count

    # Two panic presses remain available while the first fade is in progress.
    reset(page, base)
    page.locator("#panic").click()
    expect(page.locator("#panic")).to_be_enabled()
    page.locator("#panic").click()
    until(page, lambda: not inspect(base)["state"]["playing"])
    assert len([item for item in messages(base, "cmd") if item["cmd"] == "panic"]) == 2
    reset(page, base)
    page.locator('[data-command="pause"]').click()
    until(page, lambda: all(item["st"] == "pause" for item in inspect(base)["state"]["playing"]))
    page.locator('[data-command="fade"]').click()
    until(page, lambda: all(item["st"] == "fade" for item in inspect(base)["state"]["playing"]))

    # A drop before a child gets that child's parent; show mode removes all handles.
    reset(page, base)
    page.locator('[data-show="false"]').click()
    expect(row(page, "c2").locator(".drag-handle")).to_be_visible()
    start_drag(page)
    page.mouse.up()
    until(page, lambda: bool(messages(base, "move")))
    move = messages(base, "move")[-1]
    assert (move["cue"], move["before"], move["parent"]) == ("c2", "c11", "g1")
    until(page, lambda: next(item for item in inspect(base)["lists"]["cues"]["main"] if item["id"] == "c2")["parent"] == "g1")
    expect(row(page, "c2")).not_to_have_class(re.compile("pending-move"))
    page.locator('[data-show="true"]').click()
    expect(row(page, "c2").locator(".drag-handle")).to_be_hidden()

    reset(page, base)
    page.locator('[data-show="false"]').click()
    expect(row(page, "g1").locator(".drag-handle")).to_be_visible()
    x, y = center(row(page, "g1").locator(".drag-handle"))
    target = row(page, "c12").bounding_box()
    page.mouse.move(x, y)
    page.mouse.down()
    page.mouse.move(x, target["y"] + 8, steps=4)
    expect(page.locator("#drop-line")).to_be_hidden()
    page.mouse.up()
    assert not messages(base, "move"), "A group cannot be dropped inside itself"

    # Dragging near the lower edge scrolls to the end, preserving a group's children.
    x, y = center(row(page, "g1").locator(".drag-handle"))
    bounds = page.locator("#list-scroll").bounding_box()
    page.mouse.move(x, y)
    page.mouse.down()
    page.mouse.move(x, bounds["y"] + bounds["height"] - 2, steps=8)
    page.wait_for_timeout(850)
    assert page.locator("#list-scroll").evaluate("el => el.scrollTop") > 0
    page.mouse.up()
    until(page, lambda: bool(messages(base, "move")))
    move = messages(base, "move")[-1]
    assert move["before"] is None and move["parent"] is None
    ids = [item["id"] for item in inspect(base)["lists"]["cues"]["main"]]
    assert ids[-4:] == ["g1", "c11", "c12", "c13"]

    reset(page, base)
    page.locator('[data-show="false"]').click()
    expect(row(page, "c2").locator(".drag-handle")).to_be_visible()
    original = [item["id"] for item in inspect(base)["lists"]["cues"]["main"]]
    control(base, reject={"t": "move", "msg": "쇼 모드"})
    start_drag(page)
    page.mouse.up()
    expect(page.locator("#toast")).to_have_text("쇼 모드")
    expect(row(page, "c2")).not_to_have_class(re.compile("pending-move"))
    assert [item["id"] for item in inspect(base)["lists"]["cues"]["main"]] == original
    expect(page.locator("#toast")).to_be_hidden(timeout=4000)

    # Running time decreases; a paused item keeps its authoritative remaining time.
    reset(page, base)
    control(base, frozen=False)
    before = row(page, "c11").locator(".cue-time").inner_text()
    paused = row(page, "c13").locator(".cue-time").inner_text()
    page.wait_for_timeout(450)
    assert row(page, "c11").locator(".cue-time").inner_text() != before
    assert row(page, "c13").locator(".cue-time").inner_text() == paused
    row(page, "c4").click()
    expect(row(page, "c4")).to_have_class(re.compile("is-next"))
    page.locator("#go").tap()
    until(page, lambda: any(item["id"] == "c4" for item in inspect(base)["state"]["playing"]))
    assert row(page, "c4").locator(".cue-time").inner_text() == "∞"

    # Loss of server traffic locks all controls and then recovers without replaying a command.
    reset(page, base)
    control(base, silence=4.5)
    expect(page.locator("#connection-label")).to_have_text("다시 연결 중", timeout=5000)
    assert page.locator("button:not(:disabled)").count() == 0
    page.locator("#go").dispatch_event("pointerdown", dict(isPrimary=True, button=0, pointerId=9))
    connected(page)
    assert not messages(base, "go")
    control(base, bye="newkey")
    expect(page.locator("#connection-label")).to_have_text("연결 끊김")
    expect(page.locator("#connection-notice")).to_have_text("QR을 다시 찍어 주세요")
    page.wait_for_timeout(800)
    assert inspect(base)["clients"] == 0
    page.goto(base + "/#k=wrong")
    expect(page.locator("#connection-label")).to_have_text("연결 끊김")
    assert page.url.endswith("#k=wrong")

    assert not errors, errors
    assert not external, external
    context.close()
    print("PASS operator flows, real touch/scroll, move, playback, disconnect", flush=True)


def protocol_gates(browser, base):
    """Control incoming frame order without adding any hooks to app.js."""
    control(base, reset=True, frozen=True)
    fixture = inspect(base)
    context = browser.new_context(**PROFILES["tablet-landscape"])
    context.add_init_script("Object.defineProperty(navigator, 'maxTouchPoints', {get: () => 5})")
    page = context.new_page()
    received, routes = [], []

    def route(ws):
        routes.append(ws)
        ws.on_message(lambda message: received.append(json.loads(message)))

    page.route_web_socket("**/ws?*", route)
    page.goto(base + "/#k=test")
    until(page, lambda: bool(routes))
    ws = routes[-1]

    def frame(data):
        ws.send(json.dumps(data, ensure_ascii=False))
        page.wait_for_timeout(20)

    frame(dict(t="hello", proto=1, server="test", pc="STUDIO-A", project="토요 라이브", app="test"))
    until(page, lambda: bool(received))
    assert received[0]["t"] == "hi" and received[0]["id"] == 1
    assert received[0]["device"] == "iPad · 사파리"
    frame(fixture["lists"])
    assert page.locator("button:not(:disabled)").count() == 0, "Hello/lists do not unlock before state"
    frame(fixture["state"])
    connected(page)
    # Returning from a hidden page waits for both ack and a later state.
    page.evaluate("window.dispatchEvent(new PageTransitionEvent('pageshow', {persisted:true}))")
    until(page, lambda: any(item["t"] == "ping" for item in received))
    ping = received[-1]
    expect(page.locator("#go")).to_be_disabled()
    frame(fixture["state"])
    expect(page.locator("#go")).to_be_disabled()
    frame(dict(t="ack", id=ping["id"], ok=True))
    expect(page.locator("#go")).to_be_disabled()
    frame(fixture["state"])
    connected(page)

    # Mode and moved order stay authoritative; an ack alone never reorders the list.
    page.locator('[data-show="false"]').click()
    assert not page.locator("#remote").evaluate("el => el.classList.contains('mode-edit')")
    edit_lists = copy.deepcopy(fixture["lists"])
    edit_lists["mode"] = "edit"
    edit_lists["gen"] += 1
    frame(edit_lists)
    start_drag(page)
    page.mouse.up()
    until(page, lambda: received[-1]["t"] == "move")
    move = received[-1]
    original = page.locator("#cue-list .cue-row").evaluate_all("els => els.map(el => el.dataset.cue)")
    expect(row(page, "c2")).to_have_class(re.compile("pending-move"))
    frame(dict(t="ack", id=move["id"], ok=True))
    assert page.locator("#cue-list .cue-row").evaluate_all("els => els.map(el => el.dataset.cue)") == original
    changed = copy.deepcopy(edit_lists)
    changed["gen"] += 1
    cues = changed["cues"]["main"]
    moved = next(item for item in cues if item["id"] == "c2")
    cues.remove(moved)
    moved.update(parent="g1", depth=1)
    cues.insert(1, moved)
    frame(changed)
    expect(row(page, "c2")).not_to_have_class(re.compile("pending-move"))

    # Local interpolation between packets, including pause and endless duration.
    playing_state = copy.deepcopy(fixture["state"])
    playing_state["mode"] = "edit"
    frame(playing_state)
    before = row(page, "c11").locator(".cue-time").inner_text()
    paused = row(page, "c13").locator(".cue-time").inner_text()
    page.wait_for_timeout(350)
    assert row(page, "c11").locator(".cue-time").inner_text() != before
    assert row(page, "c13").locator(".cue-time").inner_text() == paused
    assert [item["id"] for item in received] == list(range(1, len(received) + 1))
    assert len([item for item in received if item["t"] == "move"]) == 1
    context.close()
    print("PASS handshake, iPad UA, resume ack/state gate, authoritative mode/order, interpolation", flush=True)


def responsive_layouts(browser, base):
    context = browser.new_context(**PROFILES["tablet-landscape"])
    page = context.new_page()
    reset(page, base)
    for width, height in ((1180, 746), (944, 597), (787, 497), (900, 600), (820, 1106), (700, 800), (699, 800), (390, 664), (320, 568)):
        page.set_viewport_size(dict(width=width, height=height))
        assert_layout(page)
        if width >= 900 and width > height:
            expect(page.locator("#active-panel")).to_be_visible()
            expect(page.locator('[data-local="active"]')).to_be_hidden()
        else:
            expect(page.locator("#active-panel")).to_be_hidden()
            expect(page.locator('[data-local="active"]')).to_be_visible()
    page.set_viewport_size(dict(width=820, height=1106))
    page.locator('[data-local="active"]').click()
    expect(page.locator("#content")).to_be_hidden()
    page.set_viewport_size(dict(width=1180, height=746))
    expect(page.locator("#content")).to_be_visible()
    expect(page.locator("#active-panel")).to_be_visible()
    page.set_viewport_size(dict(width=820, height=1106))
    expect(page.locator("#content")).to_be_hidden()

    # Long data stays inside its scroll region, including many project tabs and sparse cart slots.
    reset(page, base)
    fixture = inspect(base)["lists"]
    fixture["tabs"] += [dict(id=f"extra{i}", name="효과음 카트 " + "긴 이름 " * 8, kind="cart", cols=4, rows=3) for i in range(6)]
    fixture["cues"]["main"] += [dict(id=f"long{i}", no=str(8 + i), name="게스트 소개 " * 20,
                                    kind="audio", depth=0, parent=None, len="1:23.4", slot=None, armed=True, color="") for i in range(60)]
    fixture["cues"]["fx"] = [fixture["cues"]["fx"][0], fixture["cues"]["fx"][5]]
    control(base, lists=fixture)
    until(page, lambda: page.locator("#cue-list .cue-row").count() == 70)
    for width, height in ((1180, 746), (944, 597), (787, 497), (390, 664)):
        page.set_viewport_size(dict(width=width, height=height))
        assert_layout(page)
        page.locator("#list-scroll").evaluate("el => { el.scrollTop = el.scrollHeight; }")
        assert page.locator("#list-scroll").evaluate("el => el.scrollWidth <= el.clientWidth")
    page.locator('[data-tab="fx"]').click()
    expect(page.locator(".cart-cell.is-empty")).to_have_count(10)
    assert page.locator('.cart-cell[data-slot="5"] .cart-name').inner_text() == "띵동"
    assert_layout(page)
    context.close()
    print("PASS breakpoints/rotation, 100/125/150% equivalent CSS widths, long lists/names/tabs, sparse cart", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", choices=["chromium", "webkit"], default="chromium")
    parser.add_argument("--port", type=int, default=49831)
    args = parser.parse_args()
    with fixture_server(args.port) as base, sync_playwright() as playwright:
        browser = getattr(playwright, args.browser).launch(headless=True)
        try:
            operator_flows(browser, base, args.browser)
            protocol_gates(browser, base)
            responsive_layouts(browser, base)
        finally:
            browser.close()
    print(f"PASS all checks ({args.browser})", flush=True)


if __name__ == "__main__":
    main()
