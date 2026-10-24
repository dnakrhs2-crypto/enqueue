"""Local-only web remote fixture. Python standard library; never shipped in the app.

Run: python tools/webremote_mock/mock_server.py
The /__mock__/ endpoints are fixture controls for shots.py and verify.py only.
"""

from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import mimetypes
from pathlib import Path
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


WEB_ROOT = Path(__file__).resolve().parents[2] / "src" / "remote" / "web"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def cue(cue_id, no, name, length, *, kind="audio", depth=0, parent=None, slot=None):
    return dict(id=cue_id, no=no, name=name, kind=kind, depth=depth, parent=parent,
                len=length, armed=True, color="", slot=slot)


class Model:
    def __init__(self):
        self.lock = threading.RLock()
        self.gen = 0
        self.seq = 0
        self.reset()

    def reset(self, frozen=False):
        with self.lock:
            self.gen += 1
            self.mode = "show"
            self.active = "main"
            self.pc = "STUDIO-A"
            self.project = "토요 라이브"
            self.tabs = [dict(id="main", name="메인 큐 리스트", kind="list", cols=0, rows=0),
                         dict(id="fx", name="효과음 카트", kind="cart", cols=4, rows=3)]
            self.cues = {
                "main": [cue("g1", "1", "오프닝", "0:40.0", kind="group"),
                         cue("c11", "1.1", "객석 앰비언스", "0:40.0", depth=1, parent="g1"),
                         cue("c12", "1.2", "오프닝 테마", "0:24.0", depth=1, parent="g1"),
                         cue("c13", "1.3", "인트로 보이스", "0:12.0", depth=1, parent="g1"),
                         cue("c2", "2", "타이틀 시그널", "0:12.0"),
                         cue("c3", "3", "진행자 입장", "0:32.0"),
                         cue("c4", "4", "토크 배경음", "∞"),
                         cue("c5", "5", "코너 전환 징글", "0:04.5"),
                         cue("c6", "6", "게스트 소개", "0:18.0"),
                         cue("c7", "7", "엔딩 테마", "2:41.0")],
                "fx": [cue(f"fx{i}", str(i + 1), name, "0:20.0" if i == 0 else "0:05.0", slot=i)
                       for i, name in enumerate(["박수", "함성", "웃음", "드럼롤", "짠", "띵동",
                                                "정답", "오답", "두둥", "바람 소리", "휘파람", "경적"])]}
            self.playhead = {"main": "c2", "fx": None}
            self.playing = []
            self.waits = [dict(cue="c3", no="3", name="진행자 입장", kind="pre", remain=2.4, startId=1)]
            self.order = 0
            self.panic = False
            self.last_panic = 0
            self.go_locked = False
            self.frozen = frozen
            self.last_tick = time.monotonic()
            self.start("c11", remain=32.8)
            self.start("c12", remain=16.8)
            self.start("c13", remain=5.2, status="pause")
            self.start("fx0", remain=18.5)

    def find(self, cue_id):
        return next((item for items in self.cues.values() for item in items if item["id"] == cue_id), None)

    @staticmethod
    def duration(item):
        if item["len"] in ("∞", ""):
            return -1.0
        minutes, seconds = item["len"].split(":")
        return int(minutes) * 60 + float(seconds)

    def start(self, cue_id, remain=None, status="play"):
        item = self.find(cue_id)
        if not item:
            return False
        if item["kind"] == "group":
            for child in list(self.cues["main"]):
                if child["parent"] == cue_id:
                    self.start(child["id"])
            return True
        duration = self.duration(item)
        remain = duration if remain is None else remain
        self.order += 1
        self.playing.append(dict(id=cue_id, no=item["no"], name=item["name"], remain=remain,
                                 elapsed=0 if duration < 0 else duration - remain,
                                 dur=duration, progress=0 if duration <= 0 else (duration - remain) / duration,
                                 st=status, order=self.order))
        return True

    def tick(self):
        now = time.monotonic()
        delta = now - self.last_tick
        self.last_tick = now
        if not self.frozen:
            for item in self.playing[:]:
                if item["st"] == "pause":
                    continue
                item["elapsed"] += delta
                if item["remain"] >= 0:
                    item["remain"] = max(0, item["remain"] - delta)
                if item["st"] == "fade":
                    item["fade_left"] = item.get("fade_left", 1.5) - delta
                if item["remain"] == 0 or item.get("fade_left", 1) <= 0:
                    self.playing.remove(item)
            for item in self.waits[:]:
                item["remain"] = max(0, item["remain"] - delta)
                if item["remain"] == 0:
                    self.waits.remove(item)
                    self.start(item["cue"])
            if not self.playing:
                self.panic = False
        for item in self.playing:
            item["progress"] = min(1, item["elapsed"] / item["dur"]) if item["dur"] > 0 else 0

    def lists(self):
        return dict(t="lists", gen=self.gen, active=self.active, mode=self.mode,
                    tabs=self.tabs, cues=self.cues, playhead=self.playhead)

    def state(self):
        self.seq += 1
        next_cue = self.find(self.playhead.get(self.active))
        fields = ("id", "no", "name", "remain", "elapsed", "dur", "progress", "st", "order")
        return dict(t="state", seq=self.seq, mode=self.mode,
                    next={key: next_cue[key] for key in ("id", "no", "name", "len")} if next_cue else None,
                    playing=[{key: item[key] for key in fields} for item in self.playing], waits=self.waits,
                    counts=dict(playing=len(self.playing),
                                paused=sum(item["st"] == "pause" for item in self.playing), waiting=len(self.waits)),
                    panic=self.panic, goLocked=self.go_locked)

    def move(self, message):
        if self.mode != "edit":
            return "쇼 모드"
        items = self.cues.get(message.get("tab"), [])
        item = next((item for item in items if item["id"] == message.get("cue")), None)
        if not item:
            return "큐 없음"
        index = items.index(item)
        end = index + 1
        while end < len(items) and items[end]["depth"] > item["depth"]:
            end += 1
        block = items[index:end]
        block_ids = {item["id"] for item in block}
        before_id, parent_id = message.get("before"), message.get("parent")
        if before_id in block_ids or parent_id in block_ids:
            return "이동할 수 없음"
        remaining = items[:index] + items[end:]
        before = next((item for item in remaining if item["id"] == before_id), None)
        parent = next((item for item in remaining if item["id"] == parent_id), None)
        if (before_id and not before) or (parent_id and (not parent or parent["kind"] != "group")):
            return "이동할 수 없음"
        if before and before["parent"] != parent_id:
            return "이동할 수 없음"
        if before:
            target = remaining.index(before)
        elif parent:
            target = remaining.index(parent) + 1
            while target < len(remaining) and remaining[target]["depth"] > parent["depth"]:
                target += 1
        else:
            target = len(remaining)
        depth_change = (parent["depth"] + 1 if parent else 0) - item["depth"]
        item["parent"] = parent_id
        for moved in block:
            moved["depth"] += depth_change
        self.cues[message["tab"]] = remaining[:target] + block + remaining[target:]
        # PC renumbers a list after a move. Keep IDs stable, including children and live cards.
        counters = {}
        numbers = {}
        for moved in self.cues[message["tab"]]:
            parent_id = moved["parent"]
            counters[parent_id] = counters.get(parent_id, 0) + 1
            moved["no"] = f"{numbers[parent_id]}.{counters[parent_id]}" if parent_id else str(counters[parent_id])
            numbers[moved["id"]] = moved["no"]
        for live in self.playing:
            live["no"] = self.find(live["id"])["no"]
        for wait in self.waits:
            wait["no"] = self.find(wait["cue"])["no"]
        return None

    def request(self, message, client):
        """Returns (error text or None, whether the structural snapshot changed)."""
        kind = message.get("t")
        changed = False
        if kind == "hi":
            client.device = message.get("device", "")
        elif kind == "ping":
            pass
        elif kind == "mode":
            self.mode = "show" if message["show"] else "edit"
            changed = True
        elif kind == "tab":
            if message.get("tab") not in self.cues:
                return "탭 없음", False
            self.active = message["tab"]
            changed = True
        elif kind == "playhead":
            tab = message.get("tab")
            if not any(item["id"] == message.get("cue") for item in self.cues.get(tab, [])):
                return "큐 없음", False
            self.playhead[tab] = message["cue"]
            changed = True
        elif kind == "go":
            touch = message.get("touch")
            if message.get("down"):
                if touch not in client.touches and not self.go_locked:
                    client.touches.add(touch)
                    cue_id = self.playhead.get(self.active)
                    if cue_id:
                        self.start(cue_id)
                        items = self.cues[self.active]
                        index = next(i for i, item in enumerate(items) if item["id"] == cue_id)
                        end = index + 1
                        if items[index]["kind"] == "group":
                            while end < len(items) and items[end]["depth"] > items[index]["depth"]:
                                end += 1
                        self.playhead[self.active] = items[end]["id"] if end < len(items) else None
                        changed = True
            else:
                client.touches.discard(touch)
        elif kind == "cmd":
            command = message.get("cmd")
            if command == "pause":
                pause = any(item["st"] != "pause" for item in self.playing)
                for item in self.playing:
                    item["st"] = "pause" if pause else "play"
            elif command == "fade":
                for item in self.playing:
                    item.update(st="fade", fade_left=1.5)
            elif command == "panic":
                now = time.monotonic()
                if now - self.last_panic <= .5:
                    self.playing.clear()
                    self.panic = False
                else:
                    for item in self.playing:
                        item.update(st="fade", fade_left=1.5)
                    self.panic = True
                self.waits.clear()
                self.last_panic = now
        elif kind == "cue":
            item = next((item for item in self.playing if item["id"] == message.get("cue")
                         and item["order"] == message.get("order")), None)
            if not item:
                return "큐 없음", False
            if message.get("op") == "stop":
                self.playing.remove(item)
            else:
                item["st"] = "pause" if message.get("op") == "pause" else "play"
                item.pop("fade_left", None)
        elif kind == "wait":
            self.waits = [item for item in self.waits if not (item["cue"] == message.get("cue")
                          and item["kind"] == message.get("kind") and item["startId"] == message.get("startId"))]
        elif kind == "fire":
            if not self.start(message.get("cue")):
                return "큐 없음", False
        elif kind == "stopcue":
            self.playing = [item for item in self.playing if item["id"] != message.get("cue")]
        elif kind == "move":
            error = self.move(message)
            if error:
                return error, False
            changed = True
        else:
            return "요청 오류", False
        if changed:
            self.gen += 1
        return None, changed


class Client:
    def __init__(self, handler):
        self.handler = handler
        self.lock = threading.Lock()
        self.last_id = 0
        self.last_ack = None
        self.touches = set()
        self.device = ""

    def frame(self, payload, opcode=1):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        length = len(payload)
        prefix = bytes([0x80 | opcode])
        if length < 126:
            prefix += bytes([length])
        elif length <= 65535:
            prefix += b"\x7e" + struct.pack("!H", length)
        else:
            prefix += b"\x7f" + struct.pack("!Q", length)
        with self.lock:
            self.handler.connection.sendall(prefix + payload)

    def send(self, message):
        self.frame(json.dumps(message, ensure_ascii=False, separators=(",", ":")))

    def close(self):
        self.touches.clear()
        try:
            self.handler.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


class MockServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, frozen=False):
        super().__init__(address, Handler)
        self.model = Model()
        self.model.frozen = frozen
        self.clients = set()
        self.messages = []
        self.silent_until = 0
        self.last_state = 0
        self.reject_next = None
        self.stopping = threading.Event()
        self.ticker = threading.Thread(target=self.tick, daemon=True)
        self.ticker.start()

    def broadcast(self, message):
        if time.monotonic() < self.silent_until:
            return
        for client in list(self.clients):
            try:
                client.send(message)
            except OSError:
                self.clients.discard(client)
                client.close()

    def tick(self):
        while not self.stopping.wait(.1):
            with self.model.lock:
                self.model.tick()
                now = time.monotonic()
                if self.model.playing or self.model.waits or now - self.last_state >= 1:
                    self.broadcast(self.model.state())
                    self.last_state = now

    def server_close(self):
        self.stopping.set()
        with self.model.lock:
            for client in list(self.clients):
                client.close()
        super().server_close()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: MockServer

    def log_message(self, *_args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass  # Closing/reloading a browser may reset its idle HTTP keep-alive socket.

    def response(self, status, data, content_type="application/json; charset=utf-8"):
        if not isinstance(data, bytes):
            data = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urlsplit(self.path)
        path = parsed.path
        if path == "/ws":
            self.websocket(parse_qs(parsed.query).get("k", [""])[0])
            return
        if path == "/__mock__/inspect":
            with self.server.model.lock:
                self.response(200, dict(lists=self.server.model.lists(), state=self.server.model.state(),
                                        messages=self.server.messages, clients=len(self.server.clients),
                                        devices=[client.device for client in self.server.clients]))
            return
        if path == "/":
            file = WEB_ROOT / "index.html"
        elif path in ("/app.css", "/app.js"):
            file = WEB_ROOT / path[1:]
        elif path.startswith("/icons/") and path.endswith(".svg") and Path(path).name == path[len("/icons/"):]:
            file = WEB_ROOT / "icons" / Path(path).name
        else:
            self.response(404, {})
            return
        if not file.is_file():
            self.response(404, {})
            return
        mime = {".js": "text/javascript", ".css": "text/css", ".html": "text/html", ".svg": "image/svg+xml"}.get(file.suffix)
        self.response(200, file.read_bytes(), (mime or mimetypes.guess_type(file.name)[0]) + "; charset=utf-8")

    def do_POST(self):
        if urlsplit(self.path).path != "/__mock__/control":
            self.response(404, {})
            return
        data = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        with self.server.model.lock:
            model = self.server.model
            if data.get("reset"):
                model.reset(data.get("frozen", False))
                self.server.messages.clear()
                self.server.silent_until = 0
                self.server.reject_next = None
                for client in self.server.clients:
                    client.touches.clear()
            if "frozen" in data:
                model.frozen = data["frozen"]
            if "reject" in data:
                self.server.reject_next = data["reject"]
            if "silence" in data:
                self.server.silent_until = time.monotonic() + data["silence"]
            if "goLocked" in data:
                model.go_locked = data["goLocked"]
            # Fixture variations exercise long lists/names, empty slots and waits without adding app UI.
            if "lists" in data:
                for name in ("tabs", "cues", "playhead", "active", "mode"):
                    if name in data["lists"]:
                        setattr(model, name, copy.deepcopy(data["lists"][name]))
                model.gen += 1
            if "state" in data:
                for name in ("playing", "waits"):
                    if name in data["state"]:
                        setattr(model, name, copy.deepcopy(data["state"][name]))
            if "bye" in data:
                for client in list(self.server.clients):
                    client.send(dict(t="bye", reason=data["bye"]))
                    client.close()
            self.server.broadcast(model.lists())
            self.server.broadcast(model.state())
        self.response(200, {"ok": True})

    def read_frame(self):
        header = self.rfile.read(2)
        if len(header) != 2:
            return None
        first, second = header
        length = second & 127
        if length == 126:
            length = struct.unpack("!H", self.rfile.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.rfile.read(8))[0]
        if length > 1024 * 1024:
            return None
        mask = self.rfile.read(4) if second & 128 else None
        payload = self.rfile.read(length)
        if len(payload) != length:
            return None
        if mask:
            payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        return bool(first & 128), first & 15, payload

    def websocket(self, key):
        ws_key = self.headers.get("Sec-WebSocket-Key")
        if not ws_key or self.headers.get("Upgrade", "").lower() != "websocket":
            self.response(400, {})
            return
        accept = base64.b64encode(hashlib.sha1((ws_key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        client = Client(self)
        try:
            if key != "test":
                client.send(dict(t="bye", reason="key"))
                return
            with self.server.model.lock:
                model = self.server.model
                if time.monotonic() < self.server.silent_until:
                    return
                self.server.clients.add(client)
                if time.monotonic() >= self.server.silent_until:
                    client.send(dict(t="hello", proto=1, server="mock-studio-a", pc=model.pc, project=model.project, app="mock"))
                    client.send(model.lists())
                    client.send(model.state())
            chunks = bytearray()
            while not self.server.stopping.is_set():
                frame = self.read_frame()
                if frame is None:
                    break
                final, opcode, payload = frame
                if opcode == 8:
                    client.frame(payload, 8)
                    break
                if opcode == 9:
                    client.frame(payload, 10)
                    continue
                if opcode not in (0, 1):
                    continue
                if opcode == 1:
                    chunks.clear()
                chunks.extend(payload)
                if not final:
                    continue
                message = json.loads(chunks.decode("utf-8"))
                with self.server.model.lock:
                    if time.monotonic() < self.server.silent_until:
                        continue
                    request_id = message.get("id", 0)
                    if request_id <= client.last_id:
                        client.send(client.last_ack if request_id == client.last_id else dict(t="ack", id=request_id, ok=True))
                        continue
                    client.last_id = request_id
                    self.server.messages.append(message)
                    self.server.messages[:] = self.server.messages[-1000:]
                    reject = self.server.reject_next
                    if reject and reject["t"] == message.get("t"):
                        error, changed = reject["msg"], False
                        self.server.reject_next = None
                    else:
                        error, changed = model.request(message, client)
                    ack = dict(t="ack", id=request_id, ok=not error)
                    if error:
                        ack["msg"] = error
                    client.last_ack = ack
                    client.send(ack)
                    if changed:
                        self.server.broadcast(model.lists())
                    # ping itself only acknowledges; the regular ticker supplies the next state.
                    if message.get("t") != "ping":
                        self.server.broadcast(model.state())
        except (OSError, ValueError, KeyError, struct.error):
            pass
        finally:
            with self.server.model.lock:
                self.server.clients.discard(client)
                client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=49831)
    parser.add_argument("--frozen", action="store_true", help="Keep fixture time fixed for screenshots")
    args = parser.parse_args()
    server = MockServer(("127.0.0.1", args.port), frozen=args.frozen)
    print(f"http://127.0.0.1:{server.server_port}/#k=test", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
