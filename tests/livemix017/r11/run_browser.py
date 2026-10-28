"""R11 actual-page UI flow, keyboard, copy, layout and paired mock captures.

Headless only; one Chromium launch, sandbox enabled, no launch/new_page retries.
The only replaced production module is guest-client.js, via a Playwright route.
"""
import argparse
import asyncio
from contextlib import contextmanager
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import threading
import traceback
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
MOCK = Path(r"C:\Users\claude\tools\claude_harness\lm_webrec\mock\webguest.html")
ERROR = "초대 주소가 바뀌었거나 방이 닫혔습니다."
MIC_ERROR, FULL_ERROR, REJECTED_ERROR, CONNECTION_ERROR = ("마이크를 쓸 수 없습니다.", "방이 꽉 찼습니다.",
                                                           "방장이 거절했습니다.", "연결하지 못했습니다.")
PERMISSION_ERROR = "마이크 권한이 없습니다."
MIC_SWITCH = "이 마이크를 쓸 수 없습니다. 다른 마이크를 고르세요."
ERROR_FOR = {"network": CONNECTION_ERROR, "microphone_denied": PERMISSION_ERROR, "microphone_missing": MIC_ERROR,
             "microphone_busy": MIC_ERROR, "web_full": FULL_ERROR, "full": FULL_ERROR, "rejected": REJECTED_ERROR}
NICKNAME_KEY = "livemix.webjoin.nickname"
VOLUME_PREFIX = "livemix.webjoin.volume."
FRAGMENT = "#v=2&code=4827&invite=" + "ab" * 32 + "&generation=1"
STATES = ("entry", "waiting", "connected", "error")
SIZES = {"phone": (390, 760), "pc": (960, 600)}
COPY = ["ON", "LiveMix", "닉네임", "마이크", "헤드폰을 쓰고 들어오세요.", "들어가기",
        "방장이 수락하면 연결됩니다", "취소", "연결됨", "마이크 ON", "마이크 OFF", "나가기", ERROR,
        MIC_ERROR, FULL_ERROR, REJECTED_ERROR, CONNECTION_ERROR, PERMISSION_ERROR, MIC_SWITCH]


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def translate_path(self, path):
        key = unquote(urlsplit(path).path)
        if key.startswith("/reference/"):
            return str(self.server.references.get(key, ROOT / "__missing_reference__"))
        return super().translate_path(path)


@contextmanager
def serve(mock):
    # These read-only mounts retain the original relative CSS imports.
    harness = mock.parents[2]
    references = {
        "/reference/lm_webrec/mock/webguest.html": mock,
        "/reference/lm_webrec/mock/webrec.css": mock.with_name("webrec.css"),
        "/reference/lm_collab/mock/common.css": harness / "lm_collab/mock/common.css",
        "/reference/layoutmock_1004/lm/tokens.css": harness / "layoutmock_1004/lm/tokens.css",
        "/reference/layoutmock_1004/lm/components.css": harness / "layoutmock_1004/lm/components.css",
    }
    http = ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(ROOT)))
    http.daemon_threads = True
    http.references = references
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{http.server_port}"
    finally:
        http.shutdown()
        http.server_close()
        thread.join(timeout=2)


class Suite:
    def __init__(self, browser, base, result, output):
        self.browser, self.base, self.result, self.output = browser, base, result, output
        self.fake = (HERE / "fake-guest-client.js").read_text(encoding="utf-8")
        self.errors = []

    async def context(self, storage=None, failure=None):
        context = await self.browser.new_context(viewport={"width": 390, "height": 760}, device_scale_factor=1)
        context.on("page", lambda page: page.on("pageerror", lambda error: self.errors.append(str(error))))

        async def route(request):
            parsed = urlsplit(request.request.url)
            if parsed.path.endswith("/guest-client.js") and not parsed.query:
                await request.fulfill(status=200, content_type="text/javascript; charset=utf-8", body=self.fake)
            else:
                await request.continue_()

        await context.route("**/*", route)
        if storage:
            # The new page's first about:blank document denies localStorage; seed only the served page.
            await context.add_init_script("if (location.protocol === 'http:') for (const [key, value] of Object.entries(" +
                                          json.dumps(storage) + ")) localStorage.setItem(key, value);")
        if failure == "getter":
            await context.add_init_script("Object.defineProperty(window, 'localStorage', {get() {throw new Error('SecurityError');}});")
        elif failure == "read":
            await context.add_init_script("Storage.prototype.getItem = () => {throw new Error('SecurityError');};")
        elif failure == "write":
            await context.add_init_script("Storage.prototype.setItem = () => {throw new Error('QuotaExceededError');};")
        return context

    async def page(self, context, config=None, size=(390, 760), fragment=FRAGMENT):
        page = await context.new_page()
        await page.set_viewport_size({"width": size[0], "height": size[1]})
        await page.add_init_script("window.r11Fixture = " + json.dumps(config or {}, ensure_ascii=False) + ";")
        await page.goto(self.base + "/livemix/webjoin/" + fragment)
        await page.wait_for_function("['entry', 'error'].includes(document.querySelector('#app').dataset.screen)")
        return page

    async def join(self, page, nick="하늘"):
        if nick is not None:
            await page.locator("#nickname").fill(nick)
        await page.locator("#join").click()
        await page.wait_for_function("document.querySelector('#app').dataset.screen === 'waiting'")

    async def connected(self, page, nick="하늘"):
        await self.join(page, nick)
        await page.evaluate("r11.accept()")
        await page.wait_for_function("document.querySelector('#app').dataset.screen === 'connected'")

    async def calls(self, page, method):
        return await page.evaluate("method => r11.calls.filter(call => call.method === method)", method)

    async def copy(self, page, names=None):
        allowed = COPY + (names or ["하람", "윤슬", "하늘", "하람 님의 합방", "휴대폰 마이크", "마이크(Shure MV7)"])
        evidence = await page.evaluate("""allowed => {
            const outside = [];
            const permitted = text => allowed.includes(text) || /^(100|[0-9]{1,2})%$/.test(text);
            const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
            while (walker.nextNode()) {
                const node = walker.currentNode, text = node.textContent.trim().replace(/\\s+/g, ' ');
                const parent = node.parentElement;
                if (text && parent && parent.getClientRects().length && !parent.closest('[hidden]') && !permitted(text)) outside.push(text);
            }
            for (const element of document.querySelectorAll('[aria-label], [title], [placeholder], [alt]')) {
                for (const attr of ['aria-label', 'title', 'placeholder', 'alt']) {
                    const text = element.getAttribute(attr);
                    if (text && !permitted(text)) outside.push(attr + ': ' + text);
                }
            }
            return {outside, title: document.title, text: document.body.innerText, url: location.href};
        }""", allowed)
        assert not evidence["outside"], evidence["outside"]
        assert evidence["title"] == "LiveMix"
        for forbidden in ("유튜브다운.com", "곰튀김.com", "xn--2j1bo5s81gjmag88b.com", "xn--jb0byyo90f.com"):
            assert forbidden not in evidence["text"] + evidence["url"], forbidden

    async def layout(self, page):
        evidence = await page.evaluate("""() => {
            const col = document.querySelector('.col').getBoundingClientRect();
            const viewport = document.documentElement.clientWidth;
            const outside = [...document.querySelectorAll('button, input:not([type=range]), .pblk, .wcard')]
                .filter(el => el.getClientRects().length && !el.closest('[hidden]'))
                .map(el => ({id: el.id || el.className, r: el.getBoundingClientRect()}))
                .filter(({r}) => r.left < -1 || r.right > viewport + 1)
                .map(({id}) => id);
            return {width: document.documentElement.scrollWidth, viewport, outside,
                col: {left: col.left, right: col.right, width: col.width},
                zoom: Number(getComputedStyle(document.body).zoom),
                sliders: [...document.querySelectorAll('.slider')].map(el => el.getBoundingClientRect().height)};
        }""")
        assert evidence["width"] <= evidence["viewport"] + 1, evidence
        assert not evidence["outside"], evidence
        assert evidence["col"]["width"] <= 420 * evidence["zoom"] + 1, evidence
        assert abs(evidence["col"]["left"] + evidence["col"]["right"] - evidence["viewport"]) <= 2, evidence
        assert all(height >= 44 * evidence["zoom"] - 1 for height in evidence["sliders"]), evidence
        return evidence

    async def case(self, name, run):
        before = len(self.errors)
        try:
            details = await run()
            assert len(self.errors) == before, self.errors[before:]
            self.result["tests"].append({"name": name, "ok": True, **(details or {})})
            print("PASS", name, flush=True)
        except Exception:
            self.result["tests"].append({"name": name, "ok": False, "error": traceback.format_exc()})
            print("FAIL", name, flush=True)

    async def entry_flow(self):
        from playwright.async_api import expect
        context = await self.context()
        try:
            page = await self.page(context)
            assert [call["method"] for call in await page.evaluate("r11.calls")] == ["create", "inspect"]
            await expect(page.locator("#room-title")).to_have_text("하람 님의 합방")
            await expect(page.locator("#join")).to_be_disabled()
            await page.locator("#nickname").fill(" 하늘 ")
            await page.keyboard.press("Tab")
            await expect(page.locator("#microphone")).to_be_focused()
            await page.keyboard.press("Enter")
            await expect(page.locator("#microphone")).to_have_attribute("aria-expanded", "true")
            await self.copy(page)
            assert not await self.calls(page, "requestJoin")
            await page.keyboard.press("End")
            await page.keyboard.press("Enter")
            await expect(page.locator("#microphone-name")).to_have_text("마이크(Shure MV7)")
            await expect(page.locator("#microphone")).to_be_enabled()
            await expect(page.locator("#microphone")).to_be_focused()
            await page.keyboard.press("Tab")
            await expect(page.locator("#join")).to_be_focused()
            await page.keyboard.press("Enter")
            await expect(page.locator("#waiting")).to_be_visible()
            assert [call["deviceId"] for call in await self.calls(page, "prepareMicrophone")] == [None, "shure"]
            assert (await self.calls(page, "requestJoin"))[0]["nickname"] == "하늘"
            methods = [call["method"] for call in await page.evaluate("r11.calls")]
            assert methods.index("prepareMicrophone") < methods.index("requestJoin")
            await self.copy(page)
        finally:
            await context.close()

    async def cancel_flow(self):
        from playwright.async_api import expect
        context = await self.context()
        try:
            page = await self.page(context)
            await self.join(page)
            await page.locator("#cancel").focus()
            await page.keyboard.press("Enter")
            await expect(page.locator("#entry")).to_be_visible()
            await expect(page.locator("#nickname")).to_have_value("하늘")
            assert len(await self.calls(page, "cancel")) == 1
            assert len(await self.calls(page, "dispose")) == 1
            assert len(await self.calls(page, "inspect")) == 2
            await self.join(page, None)
            assert len(await self.calls(page, "prepareMicrophone")) == 2
        finally:
            await context.close()

    async def volume_flow(self):
        from playwright.async_api import expect
        context = await self.context()
        try:
            page = await self.page(context)
            await self.connected(page)
            await expect(page.locator("#people input")).to_have_count(2)
            calls = await self.calls(page, "setPeerVolume")
            assert [(call["participantId"], call["gain"], call["rowVisible"]) for call in calls] == [(1, 1, False), (2, 1, False)]
            await page.locator("#volume-1").focus()
            await page.keyboard.press("Home")
            await expect(page.locator("#volume-1")).to_have_attribute("aria-valuetext", "0%")
            assert (await self.calls(page, "setPeerVolume"))[-1]["gain"] == 0
            await page.keyboard.press("End")
            await page.keyboard.press("ArrowLeft")
            await expect(page.locator("#volume-1")).to_have_value("99")
            await page.keyboard.press("Tab")
            await expect(page.locator("#volume-2")).to_be_focused()
            # A real pointer event changes only the other person's native range.
            slider = await page.locator("#volume-2").bounding_box()
            await page.mouse.click(slider["x"] + slider["width"] / 2, slider["y"] + slider["height"] / 2)
            value = int(await page.locator("#volume-2").input_value())
            assert 49 <= value <= 51, value
            await expect(page.locator("#volume-1")).to_have_value("99")
            last = (await self.calls(page, "setPeerVolume"))[-1]
            assert (last["participantId"], last["gain"]) == (2, value / 100)
            await page.locator("#volume-2").press("Home")
            await self.copy(page)
            await page.close()
            # Same nickname, new participantId: memory must not be keyed by id.
            members = [{"participantId": 11, "nick": "하람", "kind": "native", "state": "connected"},
                       {"participantId": 22, "nick": "윤슬", "kind": "native", "state": "connected"},
                       {"participantId": 33, "nick": "하늘", "kind": "web", "state": "connected"}]
            reopened = await self.page(context, {"members": members, "selfId": 33})
            await expect(reopened.locator("#nickname")).to_have_value("하늘")
            await self.connected(reopened, None)
            await expect(reopened.locator("#volume-11")).to_have_value("99")
            await expect(reopened.locator("#volume-22")).to_have_value("0")
            assert [(c["participantId"], c["gain"], c["rowVisible"]) for c in await self.calls(reopened, "setPeerVolume")] == [(11, .99, False), (22, 0, False)]
        finally:
            await context.close()

    async def roster_flow(self):
        from playwright.async_api import expect
        context = await self.context(storage={VOLUME_PREFIX + "윤슬": "35"})
        try:
            # Self deliberately has the host's nickname; exclude by id only.
            members = [{"participantId": 1, "nick": "하람", "kind": "native", "state": "connected"},
                       {"participantId": 3, "nick": "하람", "kind": "web", "state": "connected"}]
            page = await self.page(context, {"members": members})
            await self.connected(page)
            await expect(page.locator("#people input")).to_have_count(1)
            await page.locator("#volume-1").focus()
            members.insert(1, {"participantId": 8, "nick": "윤슬", "kind": "native", "state": "connecting"})
            await page.evaluate("members => r11.roster(members)", members)
            await expect(page.locator("#volume-1")).to_be_focused()
            await expect(page.locator("#volume-8")).to_have_value("35")
            await expect(page.locator('[data-participant-id="8"] .st')).to_be_hidden()
            members[1]["state"] = "connected"
            await page.evaluate("members => r11.roster(members)", members)
            await expect(page.locator('[data-participant-id="8"] .st')).to_be_visible()
            assert len(await self.calls(page, "setPeerVolume")) == 2
            await page.evaluate("members => r11.roster(members)", [members[0], members[2]])
            await expect(page.locator("#volume-8")).to_have_count(0)
            await expect(page.locator("#volume-1")).to_be_focused()
            await self.copy(page)
        finally:
            await context.close()

    async def microphone_and_end(self, remote):
        from playwright.async_api import expect
        context = await self.context()
        try:
            page = await self.page(context)
            await self.connected(page)
            await page.locator("#volume-2").focus()
            await page.keyboard.press("Tab")
            await expect(page.locator("#mic-toggle")).to_be_focused()
            await page.keyboard.press("Space")
            await expect(page.locator("#mic-toggle")).to_have_attribute("aria-pressed", "false")
            await expect(page.locator("#mic-text")).to_have_text("마이크 OFF")
            await page.evaluate("r11.meter(.9)")
            await expect(page.locator("#mic-meter")).to_have_attribute("aria-valuetext", "0%")
            await self.copy(page)
            await page.keyboard.press("Space")
            await page.evaluate("r11.meter(.46)")
            await expect(page.locator("#mic-meter")).to_have_attribute("aria-valuetext", "46%")
            assert [c["enabled"] for c in await self.calls(page, "setMicEnabled")] == [False, True]
            if remote:
                await page.evaluate("r11.state('ended')")
                await expect(page.locator("#error-message")).to_have_text(ERROR)
            else:
                await page.keyboard.press("Tab")
                await expect(page.locator("#leave")).to_be_focused()
                await page.keyboard.press("Enter")
                await expect(page.locator("#app")).to_have_attribute("data-screen", "ended")
                assert len(await self.calls(page, "leave")) == 1
                assert len(await self.calls(page, "dispose")) == 1
            await expect(page.locator("#people input")).to_have_count(0)
            await expect(page.locator("#connected")).to_be_hidden()
            await expect(page.locator("#mic-meter")).to_have_attribute("aria-valuetext", "0%")
            await self.copy(page)
        finally:
            await context.close()

    async def error_flow(self, stage, code):
        from playwright.async_api import expect
        context = await self.context()
        try:
            config = {stage: code} if stage != "state" else {}
            page = await self.page(context, config)
            if stage == "microphoneError" or stage == "joinError":
                await page.locator("#nickname").fill("하늘")
                await page.locator("#join").click()
            elif stage == "state":
                await self.join(page)
                await page.evaluate("state => r11.state(state)", code)
            # The client's generic 'error' state and rethrow follow the coded failure; the coded text must stay.
            await expect(page.locator("#error-message")).to_have_text(ERROR_FOR.get(code, ERROR))
            await expect(page.locator("#error-message")).to_be_focused()
            if stage in ("inspectError", "microphoneError"):
                assert not await self.calls(page, "requestJoin")
            await self.copy(page)
        finally:
            await context.close()

    async def held_microphone(self, via_list):
        """A PC whose default input another program holds stays on the entry screen and picks another input."""
        from playwright.async_api import expect
        context = await self.context()
        try:
            devices = [{"deviceId": "default", "kind": "audioinput", "label": "기본값 - 라인(오디오 인터페이스)"},
                       {"deviceId": "interface", "kind": "audioinput", "label": "라인(오디오 인터페이스)"},
                       {"deviceId": "webcam", "kind": "audioinput", "label": "마이크(웹캠)"}]
            page = await self.page(context, {"heldMicrophones": [None, "default", "interface"], "devices": devices})
            await page.locator("#nickname").fill("하늘")
            if via_list:
                await page.locator("#microphone").click()
                await expect(page.locator("#microphone-options li")).to_have_count(3)
            else:
                await page.locator("#join").click()
                await expect(page.locator("#microphone")).to_be_focused()
            await expect(page.locator("#app")).to_have_attribute("data-screen", "entry")
            await expect(page.locator("#microphone-error")).to_have_text(MIC_SWITCH)
            await expect(page.locator("#microphone")).to_have_attribute("aria-invalid", "true")
            await expect(page.locator("#microphone-name")).to_have_text("기본값 - 라인(오디오 인터페이스)")
            assert not await self.calls(page, "requestJoin")
            await self.copy(page, ["하람 님의 합방"] + [device["label"] for device in devices])

            async def list_below_message():
                # The open list starts below the message instead of covering it (design check 10/29).
                message = await page.locator("#microphone-error").bounding_box()
                listed = await page.locator("#microphone-options").bounding_box()
                assert message and listed and listed["y"] >= message["y"] + message["height"], (message, listed)

            if via_list:
                await list_below_message()
            else:
                # Joining again tries the same microphone; opening the list after it does not.
                await page.locator("#join").click()
                await expect(page.locator("#microphone-error")).to_have_text(MIC_SWITCH)
                await expect(page.locator("#microphone-error")).to_be_visible()
                await page.locator("#microphone").click()
                await expect(page.locator("#microphone-options li")).to_have_count(3)
                await list_below_message()
            prepared = [call["deviceId"] for call in await self.calls(page, "prepareMicrophone")]
            assert prepared == ([None] if via_list else [None, None]), prepared
            await page.locator("#microphone-options li", has_text="마이크(웹캠)").click()
            await expect(page.locator("#microphone-name")).to_have_text("마이크(웹캠)")
            await expect(page.locator("#microphone-error")).to_be_hidden()
            await expect(page.locator("#microphone")).to_have_attribute("aria-invalid", "false")
            await page.locator("#join").click()
            await expect(page.locator("#waiting")).to_be_visible()
            assert (await self.calls(page, "prepareMicrophone"))[-1]["deviceId"] == "webcam"
            assert len(await self.calls(page, "requestJoin")) == 1
        finally:
            await context.close()

    async def invalid_invite(self):
        from playwright.async_api import expect
        context = await self.context()
        try:
            page = await self.page(context, fragment="#v=2&code=4827")
            await expect(page.locator("#error-message")).to_have_text(ERROR)
            assert not await self.calls(page, "create")
            await self.copy(page)
        finally:
            await context.close()

    async def reissued_invite(self):
        from playwright.async_api import expect
        context = await self.context()
        try:
            page = await self.page(context, {"inspectError": "invite"})
            await expect(page.locator("#error-message")).to_have_text(ERROR)
            # The host's new address differs only in the fragment, so pasting it into this tab does not reload the page.
            await page.evaluate("delete r11.config.inspectError")
            await page.evaluate("hash => { location.hash = hash; }", "#v=2&code=5931&invite=" + "cd" * 32 + "&generation=2")
            await expect(page.locator("#entry")).to_be_visible()
            await expect(page.locator("#room-title")).to_have_text("하람 님의 합방")
            assert [c["code"] for c in await self.calls(page, "inspect")] == ["4827", "5931"]
            await self.copy(page)
        finally:
            await context.close()

    async def design_fixes(self):
        from playwright.async_api import expect
        context = await self.context()
        try:
            # 1366x768 at 125% leaves about 1093x478 CSS px of page; the opened device list must be fully on screen.
            page = await self.page(context, size=(1093, 478))
            await expect(page.locator("#entry")).to_be_visible()
            join = page.locator("#join")
            assert await join.evaluate("e => e.disabled && getComputedStyle(e).opacity") == "0.45"
            await page.locator("#nickname").fill("하늘")
            assert await join.evaluate("e => !e.disabled && getComputedStyle(e).opacity") == "1"
            await page.locator("#microphone").click()
            await page.wait_for_function("document.querySelector('#microphone').getAttribute('aria-expanded') === 'true'")
            box = await page.locator("#microphone-options").evaluate("e => [e.getBoundingClientRect().bottom, innerHeight]")
            assert box[0] <= box[1] + .5, box
            await page.locator("#microphone").press("Escape")
            await self.join(page)
            await page.evaluate("r11.accept()")
            await page.locator("#volume-1").focus()
            await page.keyboard.press("Tab")
            await page.keyboard.press("Shift+Tab")
            assert await page.locator("#volume-1").evaluate("e => getComputedStyle(e.closest('.slider')).outlineOffset") == "-2px"
        finally:
            await context.close()

    async def storage_flow(self, failure):
        from playwright.async_api import expect
        context = await self.context(failure=failure)
        try:
            page = await self.page(context)
            await self.connected(page)
            await expect(page.locator("#volume-1")).to_have_value("100")
            await page.locator("#volume-1").press("Home")
            await expect(page.locator("#volume-1")).to_have_value("0")
            assert (await self.calls(page, "setPeerVolume"))[-1]["gain"] == 0
            await page.reload()
            await page.wait_for_function("document.querySelector('#app').dataset.screen === 'entry'")
            await self.connected(page)
            await expect(page.locator("#volume-1")).to_have_value("100")
            await self.copy(page)
        finally:
            await context.close()

    async def disposal(self):
        context = await self.context()
        try:
            page = await self.page(context)
            await self.connected(page)
            await page.evaluate("window.dispatchEvent(new PageTransitionEvent('pagehide'))")
            assert len(await self.calls(page, "dispose")) == 1
        finally:
            await context.close()

    async def long_content(self):
        context = await self.context()
        try:
            host = "아주긴방장닉네임" * 5
            peer = "LongNicknameWithoutSpaces" + "가" * 16
            device = "마이크(Shure MV7) " * 8
            members = [{"participantId": 1, "nick": host, "kind": "native", "state": "connected"},
                       {"participantId": 2, "nick": peer, "kind": "native", "state": "connected"},
                       {"participantId": 3, "nick": "하늘", "kind": "web", "state": "connected"}]
            devices = [{"deviceId": str(i), "kind": "audioinput", "label": device + str(i)} for i in range(12)]
            page = await self.page(context, {"hostNick": host, "members": members, "devices": devices}, size=(320, 760))
            await page.locator("#nickname").fill("가" * 40)
            await page.locator("#microphone").click()
            await page.wait_for_function("document.querySelector('#microphone').getAttribute('aria-expanded') === 'true'")
            await page.locator("#microphone").press("End")
            await self.layout(page)
            await page.locator("#microphone").press("Enter")
            await page.locator("#join").click()
            await page.wait_for_function("document.querySelector('#app').dataset.screen === 'waiting'")
            await page.evaluate("r11.accept()")
            for size in [(320, 760), (390, 760), (960, 600)]:
                await page.set_viewport_size({"width": size[0], "height": size[1]})
                for zoom in (1, 1.25, 1.5):
                    await page.evaluate("zoom => document.body.style.zoom = zoom", zoom)
                    await self.layout(page)
                    boxes = await page.evaluate("""() => [...document.querySelectorAll('.pblk')].map(row => {
                        const name = row.querySelector('.nick').getBoundingClientRect();
                        const status = row.querySelector('.st').getBoundingClientRect();
                        const bottom = row.querySelector('.bot').getBoundingClientRect();
                        return {nameRight: name.right, statusLeft: status.left, nameBottom: name.bottom, bottomTop: bottom.top};
                    })""")
                    assert all(b["nameRight"] <= b["statusLeft"] and b["nameBottom"] <= b["bottomTop"] + 1 for b in boxes), boxes
            return {"layoutCombinations": 9, "deviceOptions": 12, "maxRemotePeople": 2}
        finally:
            await context.close()

    async def capture_state(self, size_name, state):
        context = await self.context()
        size = SIZES[size_name]
        try:
            page = await self.page(context, size=size)
            if state == "entry":
                await page.locator("#nickname").fill("하늘")
                await page.locator("#microphone").click()
                await page.wait_for_function("document.querySelector('#microphone').getAttribute('aria-expanded') === 'true'")
                if size_name == "pc":
                    await page.locator("#microphone").press("End")
                    await page.locator("#microphone").press("Enter")
                    await page.wait_for_function("!document.querySelector('#microphone').disabled")
                else:
                    await page.locator("#microphone").press("Escape")
            elif state in ("waiting", "connected"):
                await self.join(page)
                if state == "connected":
                    await page.evaluate("r11.accept()")
            else:
                await page.evaluate("r11.error('invite')")
            # Clear keyboard focus decoration for the visual baseline only.
            await page.locator("#brand").focus()
            await self.copy(page)
            await self.layout(page)
            relative = f"actual/{size_name}-{state}.png"
            await page.screenshot(path=str(self.output / relative), full_page=False)
            self.result["captures"].append(relative)
            for zoom in (1.25, 1.5):
                await page.evaluate("zoom => document.body.style.zoom = zoom", zoom)
                await self.layout(page)
            await page.evaluate("document.body.style.zoom = 1")
            await page.set_viewport_size({"width": 320, "height": 760})
            await self.layout(page)

            reference = await context.new_page()
            await reference.set_viewport_size({"width": size[0], "height": size[1]})
            await reference.goto(self.base + "/reference/lm_webrec/mock/webguest.html")
            await reference.evaluate("""({index, desktopEntry}) => {
                const original = desktopEntry ? document.querySelector('.desk .web') : document.querySelectorAll('.phone .web')[index];
                const web = original.cloneNode(true);
                document.body.className = '';
                document.body.style.cssText = 'margin:0;padding:0;width:100%;height:100vh';
                document.body.replaceChildren(web);
            }""", {"index": STATES.index(state), "desktopEntry": size_name == "pc" and state == "entry"})
            await reference.screenshot(path=str(self.output / f"reference/{size_name}-{state}.png"))
            from PIL import Image
            with Image.open(self.output / f"reference/{size_name}-{state}.png") as left, Image.open(self.output / relative) as right:
                assert left.size == right.size == size
                pair = Image.new("RGB", (size[0] * 2, size[1]))
                pair.paste(left, (0, 0))
                pair.paste(right, (size[0], 0))
                pair.save(self.output / f"compare/{size_name}-{state}.png")
            return {"size": list(size), "state": state, "layoutCombinations": 4}
        finally:
            await context.close()

    async def run(self):
        await self.case("entry permission, device choice and keyboard join order", self.entry_flow)
        await self.case("cancel, dispose, re-inspect and explicit rejoin", self.cancel_flow)
        await self.case("per-person pointer/keyboard volume and reopen with new ids", self.volume_flow)
        await self.case("two/three-person roster, remembered gain and focus retention", self.roster_flow)
        await self.case("mic OFF/ON, meter and local leave", lambda: self.microphone_and_end(False))
        await self.case("host ended clears people, controls and meter", lambda: self.microphone_and_end(True))
        for stage, code in [("inspectError", "invite"), ("inspectError", "closed"), ("inspectError", "network"),
                            ("microphoneError", "microphone_denied"), ("microphoneError", "microphone_missing"),
                            ("microphoneError", "microphone_busy"), ("joinError", "web_full"),
                            ("joinError", "full"), ("state", "rejected"), ("state", "ended")]:
            await self.case(f"approved error copy: {stage}/{code}", lambda s=stage, c=code: self.error_flow(s, c))
        await self.case("held default microphone: join keeps the entry and another input is picked", lambda: self.held_microphone(False))
        await self.case("held default microphone: the list opens and another input is picked", lambda: self.held_microphone(True))
        await self.case("invalid fragment stops before inspect", self.invalid_invite)
        await self.case("a reissued invite pasted into the same tab opens its room", self.reissued_invite)
        await self.case("short window device list, disabled join and slider focus ring", self.design_fixes)
        for failure in ("getter", "read", "write"):
            await self.case(f"localStorage {failure} failure keeps UI usable", lambda f=failure: self.storage_flow(f))
        await self.case("pagehide calls dispose", self.disposal)
        await self.case("long names/devices, 320/390/960 widths, 100/125/150 percent", self.long_content)
        for size_name in SIZES:
            for state in STATES:
                await self.case(f"mock pair and copy/layout: {size_name}/{state}", lambda n=size_name, s=state: self.capture_state(n, s))
        self.result["pageErrors"] = self.errors


async def run(args, result, base):
    from playwright.async_api import async_playwright
    async with async_playwright() as playwright:
        result["stage"] = "launch"
        result["launchAttempts"] = 1
        browser = await playwright.chromium.launch(headless=True, channel=args.channel, chromium_sandbox=True)
        try:
            result["browserVersion"] = browser.version
            # Stop on the known sandbox failure before creating any test contexts.
            result["stage"] = "new_page"
            probe_context = await browser.new_context()
            await probe_context.new_page()
            await probe_context.close()
            result["stage"] = "tests"
            await Suite(browser, base, result, args.output).run()
        finally:
            await browser.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("chrome", "msedge"), default=None)
    parser.add_argument("--mock", type=Path, default=MOCK)
    parser.add_argument("--output", type=Path, default=HERE / "artifacts/browser")
    args = parser.parse_args()
    args.output = args.output.resolve()
    if not args.output.is_relative_to(HERE):
        parser.error("R11 output must stay inside tests/livemix017/r11/")
    for directory in ("", "actual", "reference", "compare"):
        (args.output / directory).mkdir(parents=True, exist_ok=True)
    result = {"ok": False, "complete": False, "tests": [], "captures": [], "launchAttempts": 0, "stage": "setup"}
    try:
        if args.mock.with_suffix(".png").is_file():
            shutil.copyfile(args.mock.with_suffix(".png"), args.output / "reference/approved-board.png")
        with serve(args.mock) as base:
            asyncio.run(asyncio.wait_for(run(args, result, base), timeout=240))
        result["ok"] = bool(result["tests"]) and all(test["ok"] for test in result["tests"])
        result["complete"] = result["ok"] and len(result["captures"]) == 8
        result["stage"] = "finished"
        if result["complete"]:
            entries = "\n".join(f'<p>{name} {state}: 시안 / 실제</p><img src="{name}-{state}.png" width="{size[0] * 2}" height="{size[1]}">'
                                for name, size in SIZES.items() for state in STATES)
            (args.output / "compare/index.html").write_text('<!doctype html><html lang="ko"><meta charset="utf-8"><title>R11 비교</title>' + entries + "</html>", encoding="utf-8")
    except Exception as error:
        result["error"] = traceback.format_exc()
        result["blocked"] = result["stage"] in ("launch", "new_page")
        result["errorType"] = type(error).__name__
        print(result["error"], flush=True)
    result["passed"] = sum(test["ok"] for test in result["tests"])
    result["failed"] = sum(not test["ok"] for test in result["tests"])
    (args.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("ok", "complete", "passed", "failed", "stage", "launchAttempts")}), flush=True)
    return 0 if result["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
