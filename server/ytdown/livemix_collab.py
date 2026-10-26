# -*- coding: utf-8 -*-
"""LiveMix 2인 합방: 메모리 방·주소 교환과 UDP 포트 시험만 담당한다.

POST /api/livemix/collab/{create,confirm,heartbeat,join,close}
로그인 없이 hostToken으로 방장 요청을 확인한다. 오디오는 중계하지 않는다.
방장은 10초마다 heartbeat를 보내며, 45초 동안 없으면 방을 닫는다.
닫힌 코드는 10분간 410으로 응답한다. 서버를 다시 켜면 메모리 상태는 사라진다.

홀 펀칭(2026-10-16, 0.15.3~): 방장·참가자가 STUN으로 알아낸 자기 공인 주소(stun)를 보낸다. 방장 포트 시험이
실패해도 confirm에 punch가 오고 방장 stun이 있으면 방을 열어 두고, 방장 heartbeat 응답에 참가자 주소를 실어 준다
(방장이 먼저 두드려 길을 연다). stun·punch가 없는 0.15.2 이하 요청과 응답은 그대로다.

로컬 종단 시험용 실행: python livemix_collab.py --serve 127.0.0.1:8899
운영 연결: import livemix_collab 후 handles(path) / handle_post(h)로 위임.
"""
import argparse
import ipaddress
import json
import re
import secrets
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

PREFIX = "/api/livemix/collab/"
MAX_BODY = 16 * 1024
MAX_ROOMS = 500
HOST_DROP_SEC = 45
CLOSED_KEEP = 600
GUEST_PUNCH_SEC = 30              # heartbeat가 참가자 주소를 돌려주는 시간(참가 뒤)
CODE_RE = re.compile(r"[0-9]{4}")
INST_RE = re.compile(r"[A-Za-z0-9-]{1,64}")
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f-\x9f]")
LAN_NETWORKS = tuple(ipaddress.IPv4Network(net) for net in
                     ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))

_lock = threading.Lock()
_rooms = {}                       # code → 방
_closed = {}                      # code → 닫힌 시각
_now = time.time                  # reset_for_tests(now=...)로 시험 시계 주입


class Fail(Exception):
    def __init__(self, code, error):
        super().__init__(error)
        self.code, self.error = code, error


def _log(action, **fields):
    print("[livemix_collab] " + json.dumps(dict(action=action, **fields), ensure_ascii=False), flush=True)


# ── 정리 (_lock 안에서) ───────────────────────────────────────────────────
def _close(code, now, reason):
    room = _rooms.pop(code, None)
    if room is not None:
        _closed[code] = now
        _log("close", code=code, nick=room["nick"], publicIp=room["publicIp"], reason=reason)
    return room


def _gc(now):
    for code, room in list(_rooms.items()):
        if now - room["seen"] >= HOST_DROP_SEC:
            _close(code, now, "timeout")
    for code in [code for code, closed_at in _closed.items() if now - closed_at >= CLOSED_KEEP]:
        del _closed[code]


def _new_code():
    available = ["%04d" % n for n in range(10000)
                 if "%04d" % n not in _rooms and "%04d" % n not in _closed]
    if not available:
        raise Fail(503, "busy")
    return secrets.choice(available)


# ── 값 검사 ───────────────────────────────────────────────────────────────
def _nick(body):
    nick = body.get("nick")
    if not isinstance(nick, str):
        raise Fail(400, "bad")
    nick = CONTROL_RE.sub("", nick).strip()
    if not 1 <= len(nick) <= 40:
        raise Fail(400, "bad")
    return nick


def _inst(body):
    inst = body.get("inst")
    if not isinstance(inst, str) or not INST_RE.fullmatch(inst):
        raise Fail(400, "bad")
    return inst


def _code(body):
    code = body.get("code")
    if not isinstance(code, str) or not CODE_RE.fullmatch(code):
        raise Fail(400, "bad")
    return code


def _bool(body, key):
    value = body.get(key)
    if type(value) is not bool:
        raise Fail(400, "bad")
    return value


def _public_ip(h):
    forwarded = h.headers.get("X-Forwarded-For")
    value = forwarded.split(",", 1)[0].strip() if forwarded is not None else h.client_address[0]
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError:
        raise Fail(400, "ipv4")


def _lan(value):
    if not isinstance(value, list) or len(value) > 8:
        raise Fail(400, "bad")
    result = []
    for value_ip in value:
        if not isinstance(value_ip, str):
            continue
        try:
            ip = ipaddress.IPv4Address(value_ip)
        except ipaddress.AddressValueError:
            continue
        if any(ip in network for network in LAN_NETWORKS):
            result.append(str(ip))
    return result


def _upnp(value):
    if not isinstance(value, dict):
        raise Fail(400, "bad")
    found, mapped, external_ip = value.get("found", False), value.get("mapped", False), value.get("externalIp", "")
    if type(found) is not bool or type(mapped) is not bool or not isinstance(external_ip, str):
        raise Fail(400, "bad")
    return {"found": found, "mapped": mapped, "externalIp": external_ip}


def _stun(body):
    """선택 항목 {"ip": IPv4, "port": 1..65535} — 없으면 None."""
    value = body.get("stun")
    if value is None:
        return None
    if not isinstance(value, dict) or type(value.get("port")) is not int or not 1 <= value["port"] <= 65535:
        raise Fail(400, "bad")
    try:
        ip = str(ipaddress.IPv4Address(value.get("ip")))
    except (ipaddress.AddressValueError, TypeError):
        raise Fail(400, "bad")
    return {"ip": ip, "port": value["port"]}


def _guest_port(body):
    port = body.get("port")
    if port is None:
        return None
    if type(port) is not int or not 1 <= port <= 65535:
        raise Fail(400, "bad")
    return port


def _room(code):
    room = _rooms.get(code)
    if room is None:
        raise Fail(410, "closed") if code in _closed else Fail(404, "room")
    return room


def _host(room, token):
    if not isinstance(token, str) or not token.isascii() or not secrets.compare_digest(token, room["hostToken"]):
        raise Fail(403, "host")


# ── UDP probe (방 잠금·HTTP 응답과 독립) ───────────────────────────────────
def _probe(public_ip, port, nonce):
    packet = struct.pack('<IBBHQIIQHH', 0x42434D4C, 1, 7, 0, 0, 0, 0, 0, 0, 16) + bytes.fromhex(nonce)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            started = time.monotonic()
            for offset in (0.0, 0.15, 0.30):
                delay = started + offset - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                try:
                    udp.sendto(packet, (public_ip, port))
                except OSError:
                    pass                         # 도달 여부는 방장의 confirm으로 결정
    except OSError:
        pass


def _start_probe(public_ip, port, nonce):
    threading.Thread(target=_probe, args=(public_ip, port, nonce), daemon=True).start()


# ── 요청 (handle_post가 _lock과 만료 정리를 담당) ──────────────────────────
def _create(body, h, now):
    nick, inst = _nick(body), _inst(body)
    port = body.get("port")
    if type(port) is not int or not 1024 <= port <= 65535:
        raise Fail(400, "bad")
    lan, upnp, stun = _lan(body.get("lan", [])), _upnp(body.get("upnp", {})), _stun(body)
    public_ip = _public_ip(h)
    if len(_rooms) >= MAX_ROOMS:
        raise Fail(503, "busy")
    code = _new_code()
    session, host_token, nonce = secrets.token_hex(8), secrets.token_hex(16), secrets.token_hex(16)
    _rooms[code] = {"code": code, "nick": nick, "inst": inst, "port": port, "lan": lan, "upnp": upnp,
                    "publicIp": public_ip, "session": session, "hostToken": host_token, "nonce": nonce,
                    "state": "checking", "seen": now, "connected": False, "guest": None, "stun": stun, "punch": False}
    _log("create", code=code, nick=nick, inst=inst, publicIp=public_ip, port=port, lan=lan, upnp=upnp,
         **({"stun": stun} if stun else {}))
    _start_probe(public_ip, port, nonce)
    return {"code": code, "session": session, "hostToken": host_token, "publicIp": public_ip, "nonce": nonce}


def _confirm(body, h, now):
    code, reachable = _code(body), _bool(body, "reachable")
    reason = body.get("reason", "")
    if not isinstance(reason, str):
        raise Fail(400, "bad")
    punch = body.get("punch", False)
    if type(punch) is not bool:
        raise Fail(400, "bad")
    room = _room(code)
    _host(room, body.get("hostToken"))
    # 포트가 닫혀도 방장 공인 주소(stun)를 알면 방을 열어 둔다: 방장이 참가자 주소로 먼저 두드린다(홀 펀칭)
    keep = not reachable and punch and room["stun"] is not None
    _log("confirm", code=code, nick=room["nick"], publicIp=room["publicIp"], reachable=reachable, reason=reason,
         **({"punch": True} if keep else {}))
    if reachable or keep:
        room["state"], room["punch"] = "open", keep
    else:
        _close(code, now, reason or "unreachable")
    return {"ok": True, "punch": True} if keep else {"ok": True}


def _heartbeat(body, h, now):
    code, connected = _code(body), _bool(body, "connected")
    room = _room(code)
    _host(room, body.get("hostToken"))
    room["seen"], room["connected"] = now, connected
    guest = room["guest"]
    if room["punch"] and guest is not None and now - guest["joinedAt"] < GUEST_PUNCH_SEC:
        candidates = [guest["stun"]] if guest["stun"] else []
        if guest["port"] is not None and {"ip": guest["publicIp"], "port": guest["port"]} not in candidates:
            candidates.append({"ip": guest["publicIp"], "port": guest["port"]})
        if candidates:
            return {"ok": True, "guest": {"candidates": candidates}}
    return {"ok": True}


def _join(body, h, now):
    code, nick, inst = _code(body), _nick(body), _inst(body)
    stun, guest_port = _stun(body), _guest_port(body)
    public_ip = _public_ip(h)
    room = _room(code)
    if room["state"] == "checking":
        raise Fail(425, "checking")
    guest = room["guest"]
    if room["connected"] and guest is not None and guest["inst"] != inst:
        raise Fail(409, "full")
    room["guest"] = {"nick": nick, "inst": inst, "publicIp": public_ip, "stun": stun, "port": guest_port, "joinedAt": now}
    same_network = public_ip == room["publicIp"]
    ips = room["lan"] + [room["publicIp"]] if same_network else [room["publicIp"]]
    candidates = [{"ip": ip, "port": room["port"]} for ip in ips]
    if room["stun"] and room["stun"] not in candidates:   # 방장 공유기가 포트 번호를 바꾼 경우
        candidates.append(room["stun"])
    _log("join", code=code, nick=nick, inst=inst, publicIp=public_ip, hostIp=room["publicIp"],
         **({"stun": stun} if stun else {}), **({"guestPort": guest_port} if guest_port else {}))
    return {"session": room["session"], "hostNick": room["nick"], "candidates": candidates, "sameNetwork": same_network}


def _close_request(body, h, now):
    code = _code(body)
    room = _room(code)
    _host(room, body.get("hostToken"))
    _close(code, now, "host")
    return {"ok": True}


ROUTES = {"create": _create, "confirm": _confirm, "heartbeat": _heartbeat, "join": _join, "close": _close_request}


def handles(path):
    return urlparse(path).path.startswith(PREFIX)


def _send(h, code, data):
    body = json.dumps(data, ensure_ascii=False).encode("utf-8")
    try:
        h.send_response(code)
        h.send_header("Content-Type", "application/json; charset=utf-8")
        h.send_header("Content-Length", str(len(body)))
        h.send_header("Cache-Control", "no-store")
        h.end_headers()
        h.wfile.write(body)
    except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError, TimeoutError):
        pass


def handle_post(h):
    path = urlparse(h.path).path
    fn = ROUTES.get(path[len(PREFIX):]) if path.startswith(PREFIX) else None
    if fn is None:
        return _send(h, 404, {"error": "path"})
    try:
        n = int(h.headers.get("Content-Length") or 0)
    except (TypeError, ValueError):
        n = 0
    if not 0 < n <= MAX_BODY:
        return _send(h, 400, {"error": "bad"})
    try:
        raw = h.rfile.read(n)
        body = json.loads(raw.decode("utf-8")) if len(raw) == n else None
    except Exception:
        body = None
    if not isinstance(body, dict):
        return _send(h, 400, {"error": "bad"})
    try:
        with _lock:
            now = _now()
            _gc(now)
            result = fn(body, h, now)
        return _send(h, 200, result)
    except Fail as e:
        return _send(h, e.code, {"error": e.error})


def reset_for_tests(now=None):
    """본보기처럼 메모리만 초기화한다. now로 인자 없는 시계 함수를 주입할 수 있다."""
    global _now
    with _lock:
        _rooms.clear()
        _closed.clear()
        _now = time.time if now is None else now


class _StandaloneHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        return handle_post(self)

    def do_GET(self):
        if urlparse(self.path).path == "/api/ping":
            return _send(self, 200, {"ok": True})
        return _send(self, 404, {"error": "path"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serve", required=True, metavar="IP:PORT", help="예: 127.0.0.1:8899")
    args = parser.parse_args()
    try:
        host, port_text = args.serve.rsplit(":", 1)
        host, port = str(ipaddress.IPv4Address(host)), int(port_text)
        if not 1 <= port <= 65535:
            raise ValueError
    except ValueError:
        parser.error("--serve는 IPv4:PORT 형식이어야 합니다")
    with ThreadingHTTPServer((host, port), _StandaloneHandler) as server:
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
