# -*- coding: utf-8 -*-
"""LiveMix 합방: 최대 3명 방·주소 교환·웹 시그널링. 구 요청은 2인 호환.

POST /api/livemix/collab/{create,confirm,heartbeat,join,close}
로그인 없이 hostToken으로 방장 요청을 확인한다. 오디오는 중계하지 않는다.
방장은 10초마다 heartbeat를 보내며, 45초 동안 없으면 방을 닫는다.
닫힌 코드는 10분간 410으로 응답한다. 방 상태를 로컬 파일에 저장해 서버 재시작 때 복원한다.

홀 펀칭(2026-10-16, 0.15.3~): 방장·참가자가 STUN으로 알아낸 자기 공인 주소(stun)를 보낸다. 방장 포트 시험이
실패해도 confirm에 punch가 오고 방장 stun이 있으면 방을 열어 두고, 방장 heartbeat 응답에 참가자 주소를 실어 준다
(방장이 먼저 두드려 길을 연다). stun·punch가 없는 0.15.2 이하 요청과 응답은 그대로다.

로컬 종단 시험용 실행: python livemix_collab.py --serve 127.0.0.1:8899
운영 연결: import livemix_collab 후 handles(path) / handle_post(h)로 위임.
"""
import argparse
import copy
import ipaddress
import json
import re
import secrets
import socket
import struct
import threading
import os
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

PREFIX = "/api/livemix/collab/"
MAX_BODY = 16 * 1024
MAX_WEB_BODY = 131072
WEB_ORIGIN = "https://dnakrhs2-crypto.github.io"  # 웹 참여 페이지 = GitHub Pages 기본 주소(10/26 gom, 영문만)
EVENT_WAIT_SEC = 20
PENDING_SEC, RESERVATION_SEC = 60, 15
# A web guest connects through both routers (ICE), slower than a LiveMix guest's direct UDP: its page waits 20 s,
# so its seat must outlive that (10/28: a 15 s seat would end a join the page was still waiting for).
WEB_RESERVATION_SEC = 30
CAPABILITIES = ["packet_v2", "roster_ack", "star_3", "pcm16_mono", "web_guest"]
MAX_ROOMS = 500
HOST_DROP_SEC = 45
CLOSED_KEEP = 600
STATE_SAVE_SEC, STATE_MAX_AGE_SEC = 1, 15 * 60
GUEST_PUNCH_SEC = 30              # heartbeat가 참가자 주소를 돌려주는 시간(참가 뒤)
CODE_RE = re.compile(r"[0-9]{4}")
INST_RE = re.compile(r"[A-Za-z0-9-]{1,64}")
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f-\x9f]")
LAN_NETWORKS = tuple(ipaddress.IPv4Network(net) for net in
                     ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))

_lock = threading.Lock()
_rooms = {}                       # code → 방
_closed = {}                      # code → 닫힌 시각
_closed_rooms = {}                # events live for the same 600 seconds as closed codes
_now = time.time                  # reset_for_tests(now=...)로 시험 시계 주입
_state_path = None
_state_dirty = False
_state_thread = None
_state_stop = None


class Fail(Exception):
    def __init__(self, code, error):
        super().__init__(error)
        self.code, self.error = code, error


def _log(action, **fields):
    # The wall time to the millisecond (10/28): a web join's steps are judged against the page's 10 s connect limit.
    t = time.time()
    fields["t"] = time.strftime("%m-%d %H:%M:%S", time.localtime(t)) + ".%03d" % int(t * 1000 % 1000)
    print("[livemix_collab] " + json.dumps(dict(action=action, **fields), ensure_ascii=False), flush=True)


# Persistence uses one writer. Only the JSON snapshot takes _lock; directory
# creation, file writes and atomic replacement all happen after releasing it.
def _mark_state_dirty():
    """Called with _lock held, including mutations made by event polling / GC."""
    global _state_dirty
    if _state_path is not None:
        _state_dirty = True


def _stored_room(room):
    # Keep the room's tokens, counters, events and accepted-pair replies intact.
    # The global TURN cache is separate and is never part of this snapshot.
    saved = {key: value for key, value in room.items() if key != "condition"}
    saved["retiredConnections"] = sorted(room["retiredConnections"])
    saved["events"] = [[event, sorted(targets)] for event, targets in room["events"]]
    # Legacy hosts share this dict with nativePeers. _end_member tests identity;
    # two JSON copies would leave a departed modern guest attached to the host.
    for pid, peer in room["nativePeers"].items():
        if room["guest"] is peer:
            del saved["guest"]
            saved["guestParticipantId"] = pid
            break
    return saved


def _restored_room(saved, now):
    room = dict(saved)
    for key in ("members", "reservations", "nativePeers", "connections"):
        room[key] = {int(k): value for k, value in room[key].items()}
    for connection in room["connections"].values():
        for sent in connection["signals"].values():
            sent["replies"] = {int(k): value for k, value in sent["replies"].items()}
    room["retiredConnections"] = set(room["retiredConnections"])
    room["events"] = [(event, {tuple(target) for target in targets}) for event, targets in room["events"]]
    if "guestParticipantId" in room:
        room["guest"] = room["nativePeers"][room.pop("guestParticipantId")]
    room["condition"] = threading.Condition()
    room["seen"] = now  # downtime is not a missed host heartbeat; absolute seat/request deadlines stay intact
    return room


def _load_state():
    """Startup only (or after reset_for_tests); decode fully before installing."""
    if _state_path is None:
        return
    rooms, closed, closed_rooms = {}, {}, {}
    try:
        saved = json.loads(_state_path.read_text(encoding="utf-8"))
        now = _now()
        if saved["version"] != 1:
            raise ValueError("state version")
        if now - saved["savedAt"] > STATE_MAX_AGE_SEC:
            _log("state_ignored", reason="stale")
        else:
            rooms = {code: _restored_room(room, now) for code, room in saved["rooms"].items()}
            closed = {code: float(at) for code, at in saved["closed"].items()}
            closed_rooms = {code: _restored_room(room, now) for code, room in saved["closedRooms"].items()}
            _log("state_restored", rooms=len(rooms), closed=len(closed), savedSecondsAgo=round(now - saved["savedAt"], 1))
    except Exception as e:
        rooms, closed, closed_rooms = {}, {}, {}
        _log("state_ignored", error=type(e).__name__)
    with _lock:
        _rooms.clear()
        _rooms.update(rooms)
        _closed.clear()
        _closed.update(closed)
        _closed_rooms.clear()
        _closed_rooms.update(closed_rooms)
    if any(room.get("inviteToken") for room in rooms.values()):
        _turn_servers()   # a restored invite skips web/invite's warm-up: fetch the relay before its first accept


def _write_state(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(payload, encoding="utf-8")
    os.replace(temporary, path)


def _save_state():
    global _state_dirty
    try:
        with _lock:
            if _state_path is None or not _state_dirty:
                return
            path = _state_path
            payload = json.dumps(dict(version=1, savedAt=_now(),
                    rooms={code: _stored_room(room) for code, room in _rooms.items()},
                    closed=_closed,
                    closedRooms={code: _stored_room(room) for code, room in _closed_rooms.items()}),
                    ensure_ascii=False, separators=(",", ":"), allow_nan=False)
            _state_dirty = False
        _write_state(path, payload)
    except Exception as e:
        # A failed save never fails an HTTP request. The next mutation schedules another save.
        _log("state_write_failed", error=type(e).__name__)


def _state_writer(stop):
    while not stop.wait(STATE_SAVE_SEC):
        _save_state()


def _start_state_writer():
    global _state_thread, _state_stop
    if _state_path is not None and _state_thread is None:
        _state_stop = threading.Event()
        _state_thread = threading.Thread(target=_state_writer, args=(_state_stop,), name="livemix-rooms", daemon=True)
        _state_thread.start()


def _stop_state_writer():
    global _state_thread, _state_stop
    if _state_thread is not None:
        _state_stop.set()
        _state_thread.join()  # outside _lock; no old write can outlive a test's temporary directory
        _state_thread = _state_stop = None


# ── 정리 (_lock 안에서) ───────────────────────────────────────────────────
def _close(code, now, reason):
    room = _rooms.pop(code, None)
    if room is not None:
        if "members" in room:
            _end_room(room)
            _closed_rooms[code] = room
        _closed[code] = now
        _mark_state_dirty()
        _log("close", code=code, nick=room["nick"], publicIp=room["publicIp"], reason=reason)
    return room


def _gc(now):
    for code, room in list(_rooms.items()):
        if now - room["seen"] >= HOST_DROP_SEC:
            _close(code, now, "timeout")
        elif "members" in room:
            _expire(room, now)
    for code in [code for code, closed_at in _closed.items() if now - closed_at >= CLOSED_KEEP]:
        del _closed[code]
        _closed_rooms.pop(code, None)
        _mark_state_dirty()


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


def _who(h):
    """Diagnostics only (10/28 web join logging): the caller's address and browser. Never raises, never refuses."""
    try:
        forwarded = h.headers.get("X-Forwarded-For")
        ip = forwarded.split(",", 1)[0].strip() if forwarded else h.client_address[0]
        return dict(publicIp=ip, ua=(h.headers.get("User-Agent") or "")[:200])
    except Exception:
        return dict(publicIp=None, ua=None)


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
    _mark_state_dirty()
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
        _mark_state_dirty()
    else:
        _close(code, now, reason or "unreachable")
    return {"ok": True, "punch": True} if keep else {"ok": True}


def _heartbeat(body, h, now):
    code, connected = _code(body), _bool(body, "connected")
    room = _room(code)
    _host(room, body.get("hostToken"))
    room["seen"], room["connected"] = now, connected
    _mark_state_dirty()
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


# v2 state mutations run under _lock, including the complete join/accept transaction.
# Each room's condition is independent; neither waiting nor writing HTTP holds _lock.
def _uint(value, minimum=1, maximum=0xffffffff):
    if type(value) is not int or not minimum <= value <= maximum:
        raise Fail(400, "bad")
    return value


def _text(value, limit, allow_empty=True):
    if not isinstance(value, str):
        raise Fail(400, "bad")
    try:
        size = len(value.encode("utf-8"))
    except UnicodeEncodeError:
        raise Fail(400, "bad")
    if size > limit or (not allow_empty and not size):
        raise Fail(400, "bad")
    return value


def _hex(value, length):
    if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{%d}" % length, value):
        raise Fail(400, "bad")
    return value


def _client(body, web=False):
    protocols, caps = body.get("protocols"), body.get("capabilities")
    if not isinstance(protocols, list) or any(type(p) is not int for p in protocols):
        raise Fail(400, "bad")
    if protocols not in ([[2]] if web else [[1], [1, 2]]):
        raise Fail(400, "bad")
    if (not isinstance(caps, list) or len(caps) > 5
            or any(not isinstance(c, str) for c in caps)
            or len(set(caps)) != len(caps)):
        raise Fail(400, "bad")
    version = "" if web else _text(body.get("appVersion"), 32, False)
    if not version.isascii():
        raise Fail(400, "bad")
    rate = 48000 if web else _uint(body.get("wireRate"))
    if rate not in (44100, 48000):
        raise Fail(400, "bad")
    return dict(appVersion=version, protocols=protocols, capabilities=caps,
                connectionId=_hex(body.get("connectionId"), 32), wireRate=rate)


def _legacy_client():
    return dict(appVersion="", protocols=[1], capabilities=[], connectionId="", wireRate=48000)


def _supports(member, web=False):
    if member["kind"] == "native":
        version = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)(?:[-+].*)?", member["appVersion"])
        if version is None or tuple(map(int, version.groups())) < (0, 17, 0):
            return False
    return 2 in member["protocols"] and set(CAPABILITIES if web else CAPABILITIES[:3]) <= set(member["capabilities"])


def _member(pid, generation, nick, kind, client):
    return dict(participantId=pid, kind=kind, nick=nick, appVersion=client["appVersion"],
                protocols=client["protocols"], capabilities=client["capabilities"],
                connectionGeneration=generation, streamEpoch=1, state="connected" if pid == 1 else "connecting")


def _init_model(room, client, modern):
    room.update(modern=modern, members={1: _member(1, 1, room["nick"], "native", client)},
                reservations={}, nativePeers={}, retiredConnections=set(), hostConnectionId=client["connectionId"],
                wireRate=client["wireRate"], revision=1, nextId=2, nextGeneration=2,
                nativeState="checking", pendingWebRequests={}, connections={},
                inviteToken=None, inviteGeneration=0, events=[], eventId=0,
                condition=threading.Condition(), ended=False)


def _mode(room):
    return "v2" if all(_supports(m) for m in room["members"].values()) else "legacy2"


def _snapshot(room):
    caps = set(CAPABILITIES)
    for member in room["members"].values():
        caps.intersection_update(member["capabilities"])
    return copy.deepcopy(dict(apiVersion=2, code=room["code"], session=room["session"],
                roomMode=_mode(room), wireRate=room["wireRate"], revision=room["revision"],
                capabilities=[c for c in CAPABILITIES if c in caps], nativeState=room["nativeState"],
                members=list(room["members"].values()), reservations=list(room["reservations"].values())))


def _event(room, kind, data, targets):
    room["eventId"] += 1
    event = dict(id=str(room["eventId"]), type=kind, data=copy.deepcopy(data))
    room["events"].append((event, set(targets)))
    _mark_state_dirty()
    with room["condition"]:
        room["condition"].notify_all()
    return event


def _connection_targets(connection):
    return [("connection", connection["generation"], role) for role in ("host", "web")]


def _changed(room):
    room["revision"] += 1
    targets = [("host",)]
    for connection in room["connections"].values():
        if connection["active"]:
            targets.extend(_connection_targets(connection))
    _event(room, "room", dict(room=_snapshot(room)), targets)


def _request_state(room, request, state):
    request["state"] = state
    _event(room, "requestState", dict(requestId=request["requestId"], state=state),
           [("host",), ("request", request["requestId"])])


def _end_request(room, request, reason):
    _log("web_request_end", code=room["code"], rid=request["requestId"][:8], reason=reason)
    _request_state(room, request, reason)
    _event(room, "ended", dict(requestId=request["requestId"], reason=reason),
           [("host",), ("request", request["requestId"])])


def _end_member(room, pid, reason):
    member = room["members"].pop(pid)
    if reason != "closed":   # a closing room is logged once as "close"
        _log("member_end", code=room["code"], pid=pid, kind=member["kind"], state=member["state"], reason=reason)
    room["reservations"].pop(pid, None)
    peer = room["nativePeers"].pop(pid, None)
    if peer:
        room["retiredConnections"].add(peer["connectionId"])
        if room["guest"] is peer and (room["modern"] or peer["connectionId"]):
            room["guest"] = None
    data = dict(participantId=pid, connectionGeneration=member["connectionGeneration"], reason=reason)
    targets = [("host",)]
    if member["kind"] == "web":
        connection = room["connections"][member["connectionGeneration"]]
        connection["active"] = False
        req = room["pendingWebRequests"][connection["requestId"]]
        _request_state(room, req, reason)
        data["requestId"] = req["requestId"]
        targets += [("request", req["requestId"])] + _connection_targets(connection)
    _event(room, "ended", data, targets)
    _changed(room)


def _end_room(room):
    room["ended"], room["inviteToken"] = True, None
    for pid in list(room["members"]):
        if pid != 1:
            _end_member(room, pid, "closed")
    for request in room["pendingWebRequests"].values():
        if request["state"] == "pending":
            _end_request(room, request, "closed")
    _event(room, "ended", dict(reason="closed"), [("host",)])


def _expire(room, now):
    for request in room["pendingWebRequests"].values():
        if request["state"] == "pending" and now * 1000 >= request["expiresAtMs"]:
            _end_request(room, request, "expired")
    for pid, reservation in list(room["reservations"].items()):
        if now * 1000 >= reservation["expiresAtMs"]:
            _end_member(room, pid, "expired")


def _reserve(room, nick, kind, client, now):
    pid, generation = room["nextId"], room["nextGeneration"]
    room["nextId"] += 1
    room["nextGeneration"] += 1
    member = _member(pid, generation, nick, kind, client)
    room["members"][pid] = member
    room["reservations"][pid] = dict(participantId=pid, kind=kind, connectionGeneration=generation,
                                    expiresAtMs=int((now + (WEB_RESERVATION_SEC if kind == "web" else RESERVATION_SEC)) * 1000))
    return member


def _self_fields(member):
    return {key: member[key] for key in ("participantId", "connectionGeneration", "streamEpoch")}


def _native_reply(room, member, public_ip):
    same = public_ip == room["publicIp"]
    candidates = [dict(ip=ip, port=room["port"]) for ip in
                  (room["lan"] + [room["publicIp"]] if same else [room["publicIp"]])]
    if room["stun"] and room["stun"] not in candidates:
        candidates.append(room["stun"])
    return dict(session=room["session"], hostNick=room["nick"], candidates=candidates, sameNetwork=same)


def _native_join(body, h, now, v2):
    nick, inst = _nick(body), _inst(body)
    stun, port, ip = _stun(body), _guest_port(body), _public_ip(h)
    client = _client(body) if v2 else _legacy_client()
    if v2:
        _text(nick, 160, False)
    room = _room(_code(body))
    if room["nativeState"] == "checking":
        raise Fail(425, "checking")
    if room["nativeState"] == "unavailable":
        raise Fail(409, "native_unavailable" if v2 else "full")
    for pid, peer in room["nativePeers"].items():
        if ((v2 and peer["connectionId"] == client["connectionId"])
                or (not v2 and not peer["connectionId"] and peer["inst"] == inst)):
            result = _native_reply(room, room["members"][pid], ip)
            if v2:
                result.update(_snapshot(room), **_self_fields(room["members"][pid]))
            return result
    candidate = _member(0, 0, nick, "native", client)
    if v2 and client["connectionId"] in room["retiredConnections"]:
        raise Fail(410, "expired")
    if v2 and client["connectionId"] == room["hostConnectionId"]:
        raise Fail(409, "generation")
    has_web = any(m["kind"] == "web" for m in room["members"].values())
    compatible = _supports(candidate) and _mode(room) == "v2"
    if len(room["members"]) >= 3:
        raise Fail(409, "full")
    if (len(room["members"]) >= 2 and not compatible) or (has_web and not _supports(candidate, True)):
        raise Fail(409, "incompatible" if v2 else "full")
    member = _reserve(room, nick, "native", client, now)
    _log("join_v2", code=room["code"], pid=member["participantId"], nick=nick, publicIp=ip, hostIp=room["publicIp"],
         appVersion=client["appVersion"], stun=stun is not None, port=port)
    peer = dict(nick=nick, inst=inst, publicIp=ip, stun=stun, port=port, joinedAt=now,
                connectionId=client["connectionId"])
    room["nativePeers"][member["participantId"]] = peer
    if not room["modern"]:
        room["guest"] = peer
    _changed(room)
    result = _native_reply(room, member, ip)
    if v2:
        result.update(_snapshot(room), **_self_fields(member))
    return result


def _native(action, body, h, now, v2):
    if action == "create":
        client = _client(body) if v2 else _legacy_client()
        if v2:
            _text(_nick(body), 160, False)
            _text(_upnp(body.get("upnp", {}))["externalIp"], 45)
            for room in _rooms.values():
                if room["hostConnectionId"] == client["connectionId"]:
                    return dict(code=room["code"], session=room["session"], hostToken=room["hostToken"],
                                publicIp=room["publicIp"], nonce=room["nonce"],
                                **{k: v for k, v in _snapshot(room).items() if k not in ("code", "session")},
                                **_self_fields(room["members"][1]))
        result = _create(body, h, now)
        room = _rooms[result["code"]]
        _init_model(room, client, v2)
        if v2:
            result.update(_snapshot(room), **_self_fields(room["members"][1]))
        return result
    if not v2 and action in ("confirm", "heartbeat", "close"):
        # Keep legacy validation order as well as its success/error bytes.
        result = ROUTES[action](body, h, now)
        room = _rooms.get(body["code"])
        if room is not None and action == "confirm":
            state = "punch" if room["punch"] else "reachable"
            if room["nativeState"] != state:
                room["nativeState"] = state
                _changed(room)
        if room is not None and action == "heartbeat" and not room["modern"]:
            for pid in list(room["nativePeers"]):
                member = room["members"][pid]
                if body["connected"] and member["state"] != "connected":
                    member["state"] = "connected"
                    room["reservations"].pop(pid, None)
                    _changed(room)
                elif not body["connected"] and member["state"] == "connected":
                    _end_member(room, pid, "left")
        return result
    if not v2 and action == "join":
        _code(body), _nick(body), _inst(body), _stun(body), _guest_port(body), _public_ip(h)
    room = _room(_code(body))
    if action == "join":
        if not v2 and not room["modern"]:
            # Preserve legacy-only replacement/rejoin semantics. An extended guest's
            # outstanding reservation still consumes the single legacy guest seat.
            if any(p["connectionId"] for p in room["nativePeers"].values()):
                raise Fail(409, "full")
            result = _join(body, h, now)
            for pid in list(room["nativePeers"]):
                _end_member(room, pid, "left")
            m = _reserve(room, _nick(body), "native", _legacy_client(), now)
            room["guest"]["connectionId"] = ""
            room["nativePeers"][m["participantId"]] = room["guest"]
            # Legacy-only clients never confirm individual reservations.
            room["reservations"].clear()
            _changed(room)
            return result
        return _native_join(body, h, now, v2)
    _host(room, body.get("hostToken"))
    if action == "confirm":
        reachable = _bool(body, "reachable")
        _text(body.get("reason", ""), 256)
        punch = body.get("punch", False)
        if type(punch) is not bool:
            raise Fail(400, "bad")
        keep = not reachable and punch and room["stun"] is not None
        room["state"], room["punch"] = "open", keep
        _mark_state_dirty()
        result = dict(ok=True, punch=keep)
        if not room["ended"]:
            state = "punch" if room["punch"] else ("reachable" if body["reachable"] else "unavailable")
            if state != room["nativeState"]:
                room["nativeState"] = state
                _changed(room)
        if v2:
            result.update(_snapshot(room))
        return result
    if action == "heartbeat":
        _bool(body, "connected")
        states = body.get("memberStates")
        if not isinstance(states, list) or len(states) > 2:
            raise Fail(400, "bad")
        seen = set()
        current = []
        for state in states:
            if not isinstance(state, dict):
                raise Fail(400, "bad")
            pid = _uint(state.get("participantId"), 2)
            generation = _uint(state.get("connectionGeneration"))
            _uint(state.get("streamEpoch"))
            if state.get("state") not in ("connecting", "connected", "left") or pid in seen:
                raise Fail(400, "bad")
            seen.add(pid)
            member = room["members"].get(pid)
            if member is None or member["connectionGeneration"] != generation:
                # Issued earlier but already ended (a web guest that never connected and left, an expired reservation,
                # a rejoin under a new generation): the host's view is stale. Skip it so this reply's room corrects
                # the host; a 409 here would keep refusing every heartbeat and end the room. Never issued stays 409.
                if pid >= room["nextId"] or generation >= room["nextGeneration"]:
                    raise Fail(409, "generation")
                continue
            if state["streamEpoch"] < member["streamEpoch"]:
                raise Fail(409, "generation")
            current.append(state)
        room["seen"], room["connected"] = now, body["connected"]
        _mark_state_dirty()
        for state in current:
            pid = state["participantId"]
            if state["state"] == "left":
                _end_member(room, pid, "left")
                continue
            member = room["members"][pid]
            if member["state"] != state["state"] or member["streamEpoch"] != state["streamEpoch"]:
                if member["state"] != state["state"]:
                    _log("member_state", code=room["code"], pid=pid, kind=member["kind"], state=state["state"], by="heartbeat")
                member.update(state=state["state"], streamEpoch=state["streamEpoch"])
                if state["state"] == "connected":
                    room["reservations"].pop(pid, None)
                _changed(room)
        peers = []
        for pid, peer in room["nativePeers"].items():
            candidates = [peer["stun"]] if peer["stun"] else []
            address = dict(ip=peer["publicIp"], port=peer["port"])
            if peer["port"] is not None and address not in candidates:
                candidates.append(address)
            peers.append(dict(participantId=pid, connectionId=peer["connectionId"], candidates=candidates))
        return dict(ok=True, **_snapshot(room), peers=peers)
    result = _close_request(body, h, now)
    if v2:
        result["apiVersion"] = 2
    return result


# ── 중계(TURN) 접속 정보 (10/28) ─────────────────────────────────────────
# Two routers that would not open a direct path for the web join (10/28 22:32: office PC -> 도이's home, ICE failed)
# connect through Cloudflare's relay. Only an accepted pair receives short-lived credentials; the long-term key stays
# on this PC outside the repository. Fetched in the background, never under _lock; any failure leaves STUN as before.
TURN_KEY_FILE = os.environ.get("LIVEMIX_TURN_KEY_FILE", r"C:\Users\claude\SDKs\gocue_release\cf_turn.json")
TURN_TTL_SEC, TURN_REFRESH_SEC, TURN_RETRY_SEC = 172800, 86400, 60   # 48 h credentials, renewed below 24 h left
_turn_lock = threading.Lock()
_turn = dict(servers=None, expires=0.0, fetching=False, failedAt=0.0)


def _clean_ice(servers):
    """Browser- and libjuice-safe ICE servers: stun/turn/turns URLs only, never port 53 (browsers block it)."""
    cleaned = []
    for server in servers if isinstance(servers, list) else []:
        if not isinstance(server, dict):
            continue
        urls = server.get("urls")
        urls = [urls] if isinstance(urls, str) else urls if isinstance(urls, list) else []
        urls = [u for u in urls if isinstance(u, str) and re.fullmatch(r"(stun|turns?):[A-Za-z0-9.\-]+:\d+(\?transport=(udp|tcp))?", u)
                and not re.search(r":53(\?|$)", u)]
        # Each relay address costs every network adapter of the browser a candidate (64 per side): keep UDP 3478 and
        # TLS 443 (for a network that blocks UDP) of the relay, every STUN address.
        urls = [u for u in urls if u.startswith("stun:") or u.endswith(":3478?transport=udp") or
                (u.startswith("turns:") and u.endswith(":443?transport=tcp"))][:4]
        if not urls:
            continue
        entry = dict(urls=urls)
        for key in ("username", "credential"):
            if isinstance(server.get(key), str) and 0 < len(server[key]) <= 256:
                entry[key] = server[key]
        if any(u.startswith("turn") for u in urls) and len(entry) != 3:
            continue
        cleaned.append(entry)
    return cleaned[:4]


def _fetch_turn():
    try:
        key = json.loads(Path(TURN_KEY_FILE).read_text(encoding="utf-8"))
        request = urllib.request.Request(
            "https://rtc.live.cloudflare.com/v1/turn/keys/%s/credentials/generate-ice-servers" % key["keyId"],
            data=json.dumps({"ttl": TURN_TTL_SEC}).encode(), method="POST",
            headers={"Authorization": "Bearer " + key["apiToken"], "Content-Type": "application/json",
                     "User-Agent": "LiveMix-RoomServer/1.0"})   # Cloudflare refuses Python's default agent (403)
        with urllib.request.urlopen(request, timeout=10) as response:
            servers = _clean_ice(json.loads(response.read()).get("iceServers"))
        if not any(u.startswith("turn") for s in servers for u in s["urls"]):
            raise ValueError("no relay")
        with _turn_lock:
            _turn.update(servers=servers, expires=time.time() + TURN_TTL_SEC, fetching=False)
        _log("turn_credentials", servers=len(servers), hours=TURN_TTL_SEC // 3600)
    except Exception as e:   # a missing key, network or answer: STUN only
        with _turn_lock:
            _turn.update(fetching=False, failedAt=time.time())
        _log("turn_failed", error=type(e).__name__)


def _turn_servers():
    """The current relay credentials (more than an hour left) or None; starts a background renewal when due."""
    now = time.time()
    with _turn_lock:
        servers, expires = _turn["servers"], _turn["expires"]
        due = ((servers is None or expires - now < TURN_REFRESH_SEC) and not _turn["fetching"]
               and now - _turn["failedAt"] >= TURN_RETRY_SEC)
        if due:
            _turn["fetching"] = True
    if due:
        threading.Thread(target=_fetch_turn, name="livemix-turn", daemon=True).start()
    return copy.deepcopy(servers) if servers and expires - now > 3600 else None


def _web_compatible(room):
    if not all(_supports(m, True) for m in room["members"].values()):
        raise Fail(409, "incompatible")


def _token_equal(value, expected):
    return isinstance(value, str) and value.isascii() and expected is not None and secrets.compare_digest(value, expected)


def _request_by_id(room, body):
    request = room["pendingWebRequests"].get(_hex(body.get("requestId"), 32))
    if request is None:
        raise Fail(404, "request")
    return request


def _request_auth(room, token):
    for request in room["pendingWebRequests"].values():
        if _token_equal(token, request["requestToken"]):
            if request["state"] not in ("pending", "accepted", "connected"):
                raise Fail(410, "expired")
            return request
    raise Fail(403, "token")


def _connection_auth(room, body):
    generation = _uint(body.get("connectionGeneration"))
    for connection in room["connections"].values():
        for role in ("host", "web"):
            if _token_equal(body.get("connectionToken"), connection[role + "Token"]):
                if not connection["active"]:
                    raise Fail(410, "expired")
                if connection["generation"] != generation:
                    raise Fail(409, "generation")
                return connection, role
    raise Fail(403, "token")


def _accepted(room, request, role):
    connection = room["connections"][request["generation"]]
    result = dict(apiVersion=2, requestId=request["requestId"], state="accepted",
                  participantId=connection["participantId"], streamEpoch=1,
                  connectionGeneration=connection["generation"], connectionToken=connection[role + "Token"],
                  reservationExpiresAtMs=connection["expiresAtMs"], room=connection["acceptedRoom"])
    if connection.get("iceServers"):
        result["iceServers"] = copy.deepcopy(connection["iceServers"])
    return result


def _web(action, body, h, now):
    room = _room(_code(body))
    if action in ("invite", "accept", "reject", "kick"):
        _host(room, body.get("hostToken"))
    if action == "invite":
        _web_compatible(room)
        _turn_servers()   # warm the relay credentials before anyone is accepted
        for request in room["pendingWebRequests"].values():
            if request["state"] == "pending":
                _end_request(room, request, "invalidated")
        room["inviteGeneration"] += 1
        room["inviteToken"] = secrets.token_hex(32)
        _mark_state_dirty()
        return dict(apiVersion=2, inviteToken=room["inviteToken"], inviteGeneration=room["inviteGeneration"],
                    hostNick=room["nick"], session=room["session"], wireRate=room["wireRate"], revision=room["revision"])
    if action == "request":
        generation = _uint(body.get("inviteGeneration"))
        if not _token_equal(body.get("inviteToken"), room["inviteToken"]) or generation != room["inviteGeneration"]:
            raise Fail(410, "invite")
        _web_compatible(room)
        if body.get("action") == "inspect":
            _log("web_inspect", code=room["code"], hostIp=room["publicIp"], **_who(h))
            return dict(apiVersion=2, hostNick=room["nick"], session=room["session"], wireRate=room["wireRate"],
                        roomMode=_mode(room), revision=room["revision"])
        if body.get("action") != "join":
            raise Fail(400, "bad")
        client, nick = _client(body, True), _text(_nick(body), 160, False)
        if not _supports(_member(0, 0, nick, "web", client), True):
            raise Fail(409, "incompatible")
        for req in room["pendingWebRequests"].values():
            if req["connectionId"] == client["connectionId"] and req["inviteGeneration"] == generation:
                if req["state"] == "expired":
                    raise Fail(410, "expired")
                if req["state"] not in ("pending", "accepted", "connected"):
                    raise Fail(409, "request_state")
                return req["reply"]
        if sum(r["state"] == "pending" for r in room["pendingWebRequests"].values()) >= 8:
            raise Fail(429, "requests")
        rid, token = secrets.token_hex(16), secrets.token_hex(32)
        expires = int((now + PENDING_SEC) * 1000)
        _event(room, "request", dict(requestId=rid, nick=nick, expiresAtMs=expires), [("host",)])
        reply = dict(apiVersion=2, requestId=rid, requestToken=token, state="pending", expiresAtMs=expires,
                     hostNick=room["nick"], session=room["session"], cursor=str(room["eventId"]))
        room["pendingWebRequests"][rid] = dict(requestId=rid, requestToken=token, state="pending",
                nick=nick, expiresAtMs=expires, connectionId=client["connectionId"], client=client,
                inviteGeneration=generation, reply=reply)
        _log("web_request", code=room["code"], rid=rid[:8], nick=nick, hostIp=room["publicIp"], **_who(h))
        return reply
    if action in ("accept", "reject"):
        req = _request_by_id(room, body)
        if req["state"] == "expired":
            raise Fail(410, "expired")
        if action == "accept" and req["state"] in ("accepted", "connected"):
            return _accepted(room, req, "host")
        if req["state"] != "pending":
            raise Fail(409, "request_state")
        if action == "reject":
            _end_request(room, req, "rejected")
            return dict(ok=True, apiVersion=2, requestId=req["requestId"], state="rejected")
        _web_compatible(room)
        if any(m["kind"] == "web" for m in room["members"].values()):
            raise Fail(409, "web_full")
        if len(room["members"]) >= 3:
            raise Fail(409, "full")
        member = _reserve(room, req["nick"], "web", req["client"], now)
        pid, generation = member["participantId"], member["connectionGeneration"]
        req["generation"] = generation
        connection = dict(participantId=pid, generation=generation, requestId=req["requestId"], active=True,
                          hostToken=secrets.token_hex(32), webToken=secrets.token_hex(32),
                          expiresAtMs=room["reservations"][pid]["expiresAtMs"],
                          signals={role: dict(last=0, replies={}, candidates=0, sdp=False) for role in ("host", "web")})
        room["connections"][generation] = connection
        connection["iceServers"] = _turn_servers()   # the pair's relay, only for an accepted pair
        _request_state(room, req, "accepted")
        _changed(room)
        connection["acceptedRoom"] = _snapshot(room)
        _event(room, "accepted", _accepted(room, req, "web"), [("request", req["requestId"])])
        _log("web_accept", code=room["code"], rid=req["requestId"][:8], pid=pid, gen=generation)
        return _accepted(room, req, "host")
    if action == "kick":
        pid, generation = _uint(body.get("participantId"), 2), _uint(body.get("connectionGeneration"))
        member = room["members"].get(pid)
        if member is None or member["connectionGeneration"] != generation:
            raise Fail(409, "generation")
        _end_member(room, pid, "kicked")
    elif action == "leave":
        if "requestToken" in body:
            req = _request_auth(room, body["requestToken"])
            if req["state"] != "pending":
                raise Fail(409, "request_state")
            _end_request(room, req, "cancelled")
        else:
            connection, _ = _connection_auth(room, body)
            _end_member(room, connection["participantId"], "left")
    elif action == "signal":
        return _signal(room, body)
    return dict(ok=True, apiVersion=2)


def _candidate_kind(signal):
    """Diagnostic summary of an ICE candidate without its addresses: protocol/type/family (mdns = a .local name)."""
    parts = (signal.get("candidate") or "").split()
    if len(parts) < 8:
        return None
    address = parts[4]
    family = "mdns" if address.endswith(".local") else ("ipv6" if ":" in address else "ipv4")
    return "%s/%s/%s" % (parts[2].lower(), parts[7], family)


def _signal(room, body):
    connection, role = _connection_auth(room, body)
    sid = _uint(body.get("signalId"))
    sent = connection["signals"][role]
    if sid in sent["replies"]:
        return sent["replies"][sid]
    if sid <= sent["last"]:
        raise Fail(400, "bad")
    signal = body.get("signal")
    if not isinstance(signal, dict):
        raise Fail(400, "bad")
    kind = signal.get("type")
    if kind in ("offer", "answer"):
        if role != ("host" if kind == "offer" else "web"):
            raise Fail(403, "token")
        _text(signal.get("sdp"), 65536)
        if sent["sdp"]:
            raise Fail(400, "bad")
        signal = dict(type=kind, sdp=signal["sdp"])
        sent["sdp"] = True
    elif kind == "candidate":
        _text(signal.get("candidate"), 2048)
        if "sdpMid" not in signal or "sdpMLineIndex" not in signal:
            raise Fail(400, "bad")
        if signal["sdpMid"] is not None:
            _text(signal["sdpMid"], 64)
        if signal["sdpMLineIndex"] is not None:
            _uint(signal["sdpMLineIndex"], 0, 65535)
        if sent["candidates"] >= 64:
            raise Fail(400, "bad")
        signal = {k: signal[k] for k in ("type", "candidate", "sdpMid", "sdpMLineIndex")}
        sent["candidates"] += 1
    elif kind == "connected":
        if role != "host":
            raise Fail(403, "token")
        member = room["members"][connection["participantId"]]
        if member["state"] != "connected":
            member["state"] = "connected"
            room["reservations"].pop(member["participantId"], None)
            room["pendingWebRequests"][connection["requestId"]]["state"] = "connected"
            _changed(room)
        signal = dict(type=kind)
    elif kind == "endOfCandidates":
        signal = dict(type=kind)
    else:
        raise Fail(400, "bad")
    sent["last"] = sid
    _log("web_signal", code=room["code"], gen=connection["generation"], role=role, kind=kind, cand=_candidate_kind(signal))
    result = dict(ok=True, apiVersion=2, signalId=sid)
    sent["replies"][sid] = result
    _event(room, "signal", dict(connectionGeneration=connection["generation"], signalId=sid,
                               **{"from": role}, signal=signal),
           [("connection", connection["generation"], "web" if role == "host" else "host")])
    return result


def _events(body):
    cursor = body.get("cursor")
    if not isinstance(cursor, str) or not re.fullmatch(r"0|[1-9][0-9]{0,19}", cursor) or int(cursor) > 0xffffffffffffffff:
        raise Fail(400, "bad")
    cursor = int(cursor)
    with _lock:
        _gc(_now())
        room = _room(_code(body))
        keys = [key for key in ("hostToken", "requestToken", "connectionToken") if key in body]
        if len(keys) != 1:
            raise Fail(403, "token")
        if keys[0] == "hostToken":
            _host(room, body["hostToken"])
            target = ("host",)
            if cursor == 0:
                pending = [{key: r[key] for key in ("requestId", "nick", "expiresAtMs")}
                           for r in room["pendingWebRequests"].values() if r["state"] == "pending"]
                event = _event(room, "snapshot", dict(room=_snapshot(room), pendingWebRequests=pending), [target])
                return dict(apiVersion=2, cursor=event["id"], events=[event])
        elif keys[0] == "requestToken":
            target = ("request", _request_auth(room, body["requestToken"])["requestId"])
        else:
            connection, role = _connection_auth(room, body)
            target = ("connection", connection["generation"], role)
    deadline = time.monotonic() + EVENT_WAIT_SEC
    while True:
        with _lock:
            now = _now()
            _gc(now)
            events = [event for event, targets in room["events"] if int(event["id"]) > cursor and target in targets][:32]
            next_cursor = events[-1]["id"] if len(events) == 32 else str(room["eventId"])
            remaining = deadline - time.monotonic()
            if events or room["ended"] or remaining <= 0:
                return dict(apiVersion=2, cursor=next_cursor, events=events)
            expiries = [room["seen"] + HOST_DROP_SEC]
            expiries += [r["expiresAtMs"] / 1000 for r in room["pendingWebRequests"].values() if r["state"] == "pending"]
            expiries += [r["expiresAtMs"] / 1000 for r in room["reservations"].values()]
            delay = min(remaining, max(0, min(expiries) - now))
            condition = room["condition"]
            condition.acquire()  # no lost notification between reading events and starting wait
        try:
            condition.wait(delay)  # _lock is not held here
        finally:
            condition.release()


def handles(path):
    return urlparse(path).path.startswith(PREFIX)


def _send(h, code, data):
    body = json.dumps(data, ensure_ascii=False).encode("utf-8")
    try:
        h.send_response(code)
        h.send_header("Content-Type", "application/json; charset=utf-8")
        h.send_header("Content-Length", str(len(body)))
        h.send_header("Cache-Control", "no-store")
        if h.headers.get("Origin") == WEB_ORIGIN:
            h.send_header("Access-Control-Allow-Origin", WEB_ORIGIN)
            h.send_header("Vary", "Origin")
        h.end_headers()
        h.wfile.write(body)
    except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError, TimeoutError):
        pass


def handle_post(h):
    path = urlparse(h.path).path
    action = path[len(PREFIX):] if path.startswith(PREFIX) else ""
    web = action.startswith("web/")
    if action not in ROUTES and action not in {"web/" + name for name in
            ("invite", "request", "accept", "reject", "signal", "events", "kick", "leave")}:
        return _send(h, 404, {"error": "path"})
    try:
        n = int(h.headers.get("Content-Length") or 0)
    except (TypeError, ValueError):
        n = 0
    if not 0 < n <= (MAX_WEB_BODY if web else MAX_BODY):
        return _send(h, 400, {"error": "bad"})
    try:
        raw = h.rfile.read(n)
        body = json.loads(raw.decode("utf-8")) if len(raw) == n else None
    except Exception:
        body = None
    if not isinstance(body, dict):
        return _send(h, 400, {"error": "bad"})
    try:
        v2 = "apiVersion" in body
        if (v2 and (type(body["apiVersion"]) is not int or body["apiVersion"] != 2)) or (web and not v2):
            raise Fail(400, "bad")
        if action == "web/events":
            result = _events(body)
        else:
            with _lock:
                now = _now()
                _gc(now)
                result = (_web(action[4:], body, h, now) if web else _native(action, body, h, now, v2))
                result = copy.deepcopy(result)
        return _send(h, 200, result)
    except Fail as e:
        if action not in ("web/events", "heartbeat"):
            # Refusals only (no tokens): which step failed for whom, to follow a real failed join.
            _log("refused", path=action, status=e.code, error=e.error, code=body.get("code") if isinstance(body.get("code"), str) else None,
                 publicIp=_who(h)["publicIp"])
        return _send(h, e.code, {"error": e.error})


def reset_for_tests(now=None, state_path=None):
    """Clear memory and disable disk access, unless a test supplies its own temporary path.

    Does not load or start a writer; persistence tests opt in explicitly. The test
    module must also set LIVEMIX_ROOMS_STATE to a temporary path before importing.
    """
    global _now, _state_path, _state_dirty
    _stop_state_writer()
    with _lock:
        _rooms.clear()
        _closed.clear()
        _closed_rooms.clear()
        _now = time.time if now is None else now
        _state_path = Path(state_path) if state_path is not None else None
        _state_dirty = False
    with _turn_lock:   # tests never fetch real relay credentials; a test that needs them sets _turn itself
        _turn.update(servers=None, expires=0.0, fetching=False, failedAt=float("inf"))


class _StandaloneHandler(BaseHTTPRequestHandler):
    # 운영 서버는 NAS 중계를 거쳐 HTTP/1.1로 답한다. 시험 서버가 HTTP/1.0이면 앱(WinINet)이 동시 연결을 4개까지 열어
    # 방장 롱폴 2개 뒤에 연결 정보가 묶이는 결함(10/27 웹 참가 실패)이 시험에서 안 보였다 — 운영과 같게.
    protocol_version = "HTTP/1.1"

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


# web_server.py's import (the live server) keeps rooms in the default file. A --serve process is a local test
# server on the same PC: it must never read or overwrite the live file, so it saves only to LIVEMIX_ROOMS_STATE.
if os.environ.get("LIVEMIX_ROOMS_STATE"):
    _state_path = Path(os.environ["LIVEMIX_ROOMS_STATE"])
elif __name__ != "__main__":
    _state_path = Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local") / "LiveMixRoomServer" / "rooms.json"
_load_state()
_start_state_writer()


if __name__ == "__main__":
    main()
