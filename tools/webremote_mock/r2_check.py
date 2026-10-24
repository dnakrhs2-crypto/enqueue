"""Reproduce the R2 design findings through the existing mock protocol.

python -B tools/webremote_mock/r2_check.py
python -B tools/webremote_mock/r2_check.py --browser webkit --port 49832
"""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import re

from playwright.sync_api import expect, sync_playwright

from mock_server import cue
from shots import PROFILES, connected, control, fixture_server, inspect, row, start_drag


NAMES = [
    "오프닝 VCR 배경음악 (메인 테마 오케스트라 편곡, 2분 30초 버전)",
    "사회자 등장 팡파르 Fanfare for the Common Man (Live Edit)",
    "객석 앰비언스 루프 (하우스 오픈용)",
    "1부 엔딩 전체 출연진 인사 BGM",
    "게스트 소개 징글", "코너 전환 효과음",
    "인터미션 안내 방송 (휴대전화 전원 및 촬영 금지 안내)",
    "커튼콜 음악 앵콜 대기", "타이틀 시그널",
    "드라마 OST 메들리 (피아노 솔로 + 현악 4중주 협연 라이브 녹음)",
]


def fixture():
    tabs = [dict(id="main", name="메인 큐 리스트", kind="list", cols=0, rows=0),
            dict(id="fx", name="효과음 카트", kind="cart", cols=6, rows=4),
            dict(id="act1", name="1부 본공연 음악", kind="list", cols=0, rows=0),
            dict(id="act2", name="2부 본공연 음악 오케스트라와 게스트 소개", kind="list", cols=0, rows=0),
            dict(id="inter", name="인터미션 BGM", kind="list", cols=0, rows=0),
            dict(id="curtain", name="커튼콜 앵콜 카트", kind="cart", cols=4, rows=3)]
    main = []
    for group in range(1, 41):
        if group % 4 == 1:
            gid = f"g{group}"
            main.append(cue(gid, str(group), f"{group}장 " + NAMES[0 if group == 13 else group % len(NAMES)], "3:12.0", kind="group"))
            for child in range(1, (12 if group in (13, 21) else 5) + 1):
                cid, no = f"{gid}c{child}", f"{group}.{child}"
                if group == 21 and child == 3:
                    main.append(cue(cid, no, "효과음 묶음 " + NAMES[child], "0:45.0", kind="group", depth=1, parent=gid))
                    for nested in range(1, 12):
                        main.append(cue(f"{cid}d{nested}", f"{no}.{nested}", NAMES[(child + nested) % len(NAMES)],
                                        "0:05.5", depth=2, parent=cid))
                else:
                    main.append(cue(cid, no, NAMES[(group + child) % len(NAMES)], "1:05.0", depth=1, parent=gid))
        else:
            main.append(cue(f"t{group}", str(group), NAMES[group % len(NAMES)], "2:41.0"))
    number = 41
    while len(main) < 160:
        main.append(cue(f"t{number}", str(number), NAMES[number % len(NAMES)], "2:41.0"))
        number += 1
    fx = [cue(f"fx{i}", str(i + 1), NAMES[i % len(NAMES)], "0:20.0", slot=i) for i in range(24)]
    cues = dict(main=main, fx=fx, curtain=[cue(f"cc{i}", str(i + 1), NAMES[i % len(NAMES)], "0:05.0", slot=i) for i in range(12)])
    for tab in ("act1", "act2", "inter"):
        cues[tab] = [cue(f"{tab}{i}", str(i + 1), NAMES[i % len(NAMES)], "1:05.0") for i in range(15)]
    lists = dict(tabs=tabs, cues=cues, active="main", mode="show",
                 playhead=dict(main="t2", fx=None, act1="act10", act2="act20", inter="inter0", curtain=None))
    by_id = {item["id"]: item for items in cues.values() for item in items}
    playing = []
    for order, (cid, status) in enumerate([("g1c1", "play"), ("g1c2", "play"), ("g1c3", "pause"),
                                         ("g5c1", "pause"), ("g5c2", "pause"), ("t3", "fade"),
                                         ("g21c3d10", "play"), ("fx0", "play")], 1):
        item = by_id[cid]
        playing.append(dict(id=cid, no=item["no"], name=item["name"], remain=32.8, elapsed=7.2,
                            dur=40, progress=.18, st=status, order=order))
    waits = [dict(cue=cid, no=by_id[cid]["no"], name=by_id[cid]["name"], kind="pre", remain=2.4, startId=i)
             for i, cid in enumerate(("g9c1", "g13"), 1)]
    return lists, dict(playing=playing, waits=waits)


def load(page, base):
    lists, state = fixture()
    control(base, reset=True, frozen=True, pc="DESKTOP-4F7K2Q9", project="토요 라이브 오케스트라와 게스트 소개",
            lists=lists, state=state)
    if page.url == base + "/#k=test":
        page.reload()
    else:
        page.goto(base + "/#k=test")
    connected(page)
    expect(page.locator("#cue-list .cue-row")).to_have_count(160)
    return lists, state


def measurements(page):
    return page.evaluate("""() => {
      const box = el => { const r=el.getBoundingClientRect(); return {x:r.x,y:r.y,w:r.width,h:r.height,right:r.right,bottom:r.bottom}; };
      const shown = el => el.getClientRects().length && !el.hidden;
      const tabs=document.getElementById('tabs'), active=tabs.querySelector('[data-local]');
      const selected=tabs.querySelector('[aria-selected="true"]');
      const rows=[...document.querySelectorAll('#cue-list .cue-row')];
      const numbers=rows.map(row=>row.querySelector('.cue-number'));
      const spill=rows.filter(row=>[...row.querySelectorAll('.cue-name > *')].some(child=>{
        if (!shown(child)) return false;
        const r=box(row), c=box(child); return c.y<r.y-1 || c.bottom>r.bottom+1;
      })).map(el=>el.dataset.cue);
      const small=[...document.querySelectorAll('button')].filter(shown).filter(el=>{ const r=box(el); return r.w<47.9||r.h<47.9; }).map(el=>el.className);
      return {width:innerWidth,height:innerHeight,numberClips:numbers.filter(el=>el.scrollWidth>el.clientWidth+1).map(el=>el.textContent),
        numberWidths:[...new Set(numbers.map(el=>box(el).w))],rowSpill:spill,small,
        pageOverflow:document.documentElement.scrollWidth>innerWidth || document.getElementById('remote').scrollWidth>innerWidth,
        list:box(document.getElementById('list-scroll')), content:box(document.getElementById('content')),
        tabs:box(tabs),selected:selected?box(selected):null,local:shown(active)?box(active):null,
        tabScroll:tabs.scrollLeft,tabFade:document.getElementById('tabs-strip').className,
        pc:box(document.getElementById('pc-name')),identity:box(document.getElementById('project-identity')),
        activePanel:shown(document.getElementById('active-panel'))?box(document.getElementById('active-panel')):null,
        nextFont:getComputedStyle(document.querySelector('.next-title')).fontSize,
        go:box(document.getElementById('go')),notice:box(document.getElementById('connection-notice'))};
    }""")


def basic_checks(page):
    result = measurements(page)
    assert not result["numberClips"], result
    assert not result["rowSpill"], result
    assert not result["small"], result
    assert not result["pageOverflow"], result
    assert len(result["numberWidths"]) <= 1, result
    return result


def visible_head(page, cue_id):
    target = row(page, cue_id).bounding_box()
    bounds = page.locator("#list-scroll").bounding_box()
    assert target["y"] >= bounds["y"] - 1 and target["y"] + target["height"] <= bounds["y"] + bounds["height"] + 1, (target, bounds)


def run(browser, base, engine, output):
    output.mkdir(parents=True, exist_ok=True)
    results = []
    context = browser.new_context(**PROFILES["phone"])
    context.add_init_script("""window.frameCalls=0; const nativeFrame=window.requestAnimationFrame;
      window.requestAnimationFrame=callback=>nativeFrame.call(window, time=>{window.frameCalls++; callback(time);});""")
    page = context.new_page()
    errors = []
    page.on("pageerror", lambda error: errors.append(str(error)))

    def shot(name):
        result = basic_checks(page)
        page.screenshot(path=str(output / f"{name}.png"), animations="disabled")
        results.append(dict(file=f"{name}.png", **result))
        print(f"PASS {name}", flush=True)

    try:
        lists, state = load(page, base)
        assert page.locator("html").get_attribute("translate") == "no"
        for cid, status in (("g1", "play"), ("g5", "pause"), ("g9", "wait"), ("g21c3", "play"), ("g21", "play")):
            expect(row(page, cid)).to_have_class(re.compile(f"is-{status}"))
        # A waiting group with playing children stays green, and clearing children clears the group.
        with_parent_wait = copy.deepcopy(state)
        with_parent_wait["waits"].append(dict(cue="g1", no="1", name="오프닝", kind="pre", remain=2.4, startId=3))
        control(base, state=with_parent_wait)
        expect(row(page, "g1")).to_have_class(re.compile("is-play"))
        page.locator("#list-scroll").evaluate("el=>{el.scrollTop=0}")
        shot("phone-groups")

        control(base, lists={"playhead": {**lists["playhead"], "main": "g21c3d10"}})
        expect(row(page, "g21c3d10")).to_have_class(re.compile("is-next"))
        visible_head(page, "g21c3d10")
        shot("phone-long-numbers")
        page.locator('[data-show="false"]').click()
        expect(page.locator("#remote")).to_have_class(re.compile("mode-edit"))
        shot("phone-long-numbers-edit")

        # The same head on subsequent lists must preserve an operator's scroll position.
        page.locator("#list-scroll").evaluate("el=>{el.scrollTop=0}")
        control(base, lists={"mode": "edit"})
        page.wait_for_timeout(150)
        assert page.locator("#list-scroll").evaluate("el=>el.scrollTop") == 0
        start_drag(page, "g1c3", "g1c1")
        ghost = page.locator(".drag-ghost")
        assert ghost.evaluate("el=>getComputedStyle(el).opacity") == "1"
        assert ghost.evaluate("el=>getComputedStyle(el).backgroundColor").startswith("rgb(")
        shot("phone-opaque-drag")
        before_scroll = page.locator("#list-scroll").evaluate("el=>el.scrollTop")
        control(base, lists={"playhead": {**lists["playhead"], "main": "t60"}})
        expect(row(page, "t60")).to_have_class(re.compile("is-next"))
        assert page.locator("#list-scroll").evaluate("el=>el.scrollTop") == before_scroll
        page.mouse.up()

        # Finger scrolling is not pulled to a head changed from the PC.
        page.locator("#list-scroll").evaluate("el=>el.dispatchEvent(new Event('touchstart',{bubbles:true}))")
        control(base, lists={"playhead": {**lists["playhead"], "main": "t59"}})
        expect(row(page, "t59")).to_have_class(re.compile("is-next"))
        assert page.locator("#list-scroll").evaluate("el=>el.scrollTop") == before_scroll
        page.locator("#list-scroll").evaluate("""el=>{const e=new Event('touchend',{bubbles:true}); Object.defineProperty(e,'touches',{value:[]}); el.dispatchEvent(e)}""")
        page.wait_for_timeout(180)
        for _ in range(10):
            previous = inspect(base)["lists"]["playhead"]["main"]
            page.locator("#go").tap()
            page.wait_for_function("old=>document.querySelector('#cue-list .is-next')?.dataset.cue!==old", arg=previous)
            visible_head(page, inspect(base)["lists"]["playhead"]["main"])
        shot("phone-go-follow")

        load(page, base)
        control(base, lists={"playhead": {**lists["playhead"], "main": "g13"}})
        expect(row(page, "g13")).to_have_class(re.compile("is-next"))
        expect(page.locator(".next-title")).to_have_class(re.compile("is-compact"))
        assert measurements(page)["nextFont"] == "17px"
        assert row(page, "g13").locator(".cue-wait").evaluate("el=>el.scrollWidth<=el.clientWidth")
        shot("phone-wait-name")
        page.locator('[data-local="active"]').click()
        expect(page.locator("#active-panel")).to_be_visible()
        shot("phone-active-names")

        for name, width, height in (("tablet-portrait",820,1106), ("tablet-landscape",1180,746),
                                    ("ipad-1080",1080,810), ("narrow-900",900,600),
                                    ("phone-landscape",844,390), ("short-853",853,345), ("wide-1920",1920,945)):
            page.set_viewport_size(dict(width=width,height=height))
            load(page, base)
            page.locator("#list-scroll").evaluate("el=>{el.scrollTop=0}")
            result = basic_checks(page)
            if 900 <= width <= 1099:
                assert result["pc"]["right"] <= result["identity"]["right"] + 1, result
            if height <= 560:
                assert result["go"]["h"] == 112 and result["list"]["h"] >= height - 193, result
            if width == 1920:
                assert result["activePanel"]["w"] == 480, result
            shot(f"{name}-list")
            control(base, lists={"active": "inter"})
            expect(page.locator('[data-tab="inter"]')).to_have_attribute("aria-selected", "true")
            result = measurements(page)
            visible_right = result["local"]["x"] if result["local"] else result["tabs"]["right"]
            assert result["selected"]["x"] >= result["tabs"]["x"] - 1 and result["selected"]["right"] <= visible_right + 1, result
            if result["local"]:
                assert abs(result["local"]["right"] - result["tabs"]["right"]) <= 1, result
            shot(f"{name}-selected-tab")
            if name in ("tablet-landscape", "phone-landscape"):
                control(base, lists={"active": "fx"})
                expect(page.locator("#cart-scroll")).to_be_visible()
                expect(page.locator(".cart-cell")).to_have_count(24)
                shot(f"{name}-cart-six-columns")

        # Pause-only and idle state schedule no frames; playing resumes, a connection lock stops them.
        page.set_viewport_size(dict(width=1080,height=810))
        load(page, base)
        for waiting_state in (dict(playing=[], waits=[]), dict(playing=[state["playing"][2]], waits=[])):
            control(base, state=waiting_state)
            page.wait_for_timeout(160)
            calls = page.evaluate("frameCalls")
            page.wait_for_timeout(350)
            assert page.evaluate("frameCalls") == calls
        expect(row(page, "g1")).to_have_class(re.compile("is-pause"))
        control(base, state=state)
        page.wait_for_timeout(160)
        calls = page.evaluate("frameCalls")
        page.wait_for_timeout(180)
        assert page.evaluate("frameCalls") > calls
        control(base, silence=8)
        expect(page.locator("#connection-label")).to_have_text("다시 연결 중", timeout=5500)
        calls = page.evaluate("frameCalls")
        page.wait_for_timeout(350)
        assert page.evaluate("frameCalls") == calls
        result = measurements(page)
        assert abs(result["notice"]["x"] + result["notice"]["w"]/2 - result["go"]["x"] - result["go"]["w"]/2) < 1
        assert abs(result["notice"]["y"] + result["notice"]["h"]/2 - result["go"]["y"] - result["go"]["h"]/2) < 1
        shot("ipad-1080-reconnecting")
        page.set_viewport_size(dict(width=844,height=390))
        shot("phone-landscape-reconnecting")
        assert not errors, errors
        (output / "results.json").write_text(json.dumps(dict(browser=engine, cues=160, tabs=6, cartColumns=6,
                                                             captures=results, errors=errors), ensure_ascii=False, indent=2), encoding="utf-8")
    except Exception:
        page.screenshot(path=str(output / "failure.png"))
        print(json.dumps(measurements(page), ensure_ascii=False), flush=True)
        raise
    finally:
        context.close()
    print(f"PASS R2 {engine}: {len(results)} captures", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", choices=["chromium", "webkit"], default="chromium")
    parser.add_argument("--port", type=int, default=49831)
    args = parser.parse_args()
    output = Path(__file__).resolve().parent / "shots" / "r2" / args.browser
    with fixture_server(args.port) as base, sync_playwright() as playwright:
        browser = getattr(playwright, args.browser).launch(headless=True)
        try:
            run(browser, base, args.browser, output)
        finally:
            browser.close()


if __name__ == "__main__":
    main()
