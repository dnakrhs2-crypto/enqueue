# -*- coding: utf-8 -*-
"""LiveMix 합방 시험: 가짜 요청·loopback UDP 및 --serve 실제 HTTP 왕복.
python -m unittest test_livemix_collab -v
"""
import io
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing, redirect_stdout
from email.message import Message
from unittest import mock

# Import itself restores persisted rooms. Never let even test collection read the
# production file; stop its writer before this temporary directory is removed.
with tempfile.TemporaryDirectory() as _import_dir:
    with mock.patch.dict(os.environ, {"LIVEMIX_ROOMS_STATE": str(Path(_import_dir) / "rooms.json")}):
        with redirect_stdout(io.StringIO()):
            if __package__:
                from . import livemix_collab as lc
            else:
                import livemix_collab as lc
            lc.reset_for_tests()

HOST_IP, GUEST_IP = "203.0.113.17", "198.51.100.28"
HOST_PORT = 12000


class H:
    """BaseHTTPRequestHandler 중 모듈이 사용하는 부분만 제공한다."""
    def __init__(self, path, body=None, *, raw=None, client_ip=HOST_IP, xff=None):
        self.path = path
        if raw is None:
            raw = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.headers = Message()
        self.headers["Content-Type"] = "application/json; charset=utf-8"
        self.headers["Content-Length"] = str(len(raw))
        if xff is not None:
            self.headers["X-Forwarded-For"] = xff
        self.client_address = (client_ip, 54321)
        self.rfile, self.wfile = io.BytesIO(raw), io.BytesIO()
        self.status = None
        self.response_headers = {}

    def send_response(self, status):
        self.status = status

    def send_header(self, key, value):
        self.response_headers[key] = value

    def end_headers(self):
        pass


class CollabTest(unittest.TestCase):
    def setUp(self):
        self.now = 1000000.0
        lc.reset_for_tests(now=lambda: self.now)
        self.addCleanup(lc.reset_for_tests)
        self.probe_patch = mock.patch.object(lc, "_start_probe")
        self.start_probe = self.probe_patch.start()
        self.addCleanup(self.probe_patch.stop)
        self.logs = io.StringIO()
        self.enterContext(redirect_stdout(self.logs))

    def response(self, h, status=200):
        raw = h.wfile.getvalue()
        self.assertEqual(h.status, status, (h.path, raw))
        self.assertEqual(h.response_headers["Content-Type"], "application/json; charset=utf-8")
        self.assertEqual(h.response_headers["Content-Length"], str(len(raw)))
        self.assertEqual(h.response_headers["Cache-Control"], "no-store")
        return json.loads(raw.decode("utf-8"))

    def send(self, h, status=200):
        lc.handle_post(h)
        return self.response(h, status)

    def post(self, action, body, status=200, **request):
        return self.send(H(lc.PREFIX + action, body, **request), status)

    def create(self, *, client_ip=HOST_IP, xff=None, **fields):
        body = {"nick": "다현", "inst": "host-PC-01", "port": HOST_PORT, "lan": ["192.168.1.23"]}
        body.update(fields)
        return self.post("create", body, client_ip=client_ip, xff=xff)

    def host_post(self, action, room, status=200, **fields):
        body = dict(code=room["code"], hostToken=room["hostToken"], **fields)
        return self.post(action, body, status)

    def open_room(self, **fields):
        room = self.create(**fields)
        self.assertEqual(self.host_post("confirm", room, reachable=True), {"ok": True})
        return room

    def join(self, room, status=200, *, nick="하늘", inst="guest-PC-02", client_ip=GUEST_IP, xff=None):
        return self.post("join", {"code": room["code"], "nick": nick, "inst": inst}, status,
                         client_ip=client_ip, xff=xff)

    def test_create_confirm_join_and_forwarded_ip(self):
        room = self.create(client_ip="127.0.0.1", xff="  203.0.113.17  , 192.0.2.99")
        self.assertEqual(set(room), {"code", "session", "hostToken", "publicIp", "nonce"})
        self.assertRegex(room["code"], r"^[0-9]{4}$")
        self.assertRegex(room["session"], r"^[0-9a-f]{16}$")
        self.assertRegex(room["hostToken"], r"^[0-9a-f]{32}$")
        self.assertRegex(room["nonce"], r"^[0-9a-f]{32}$")
        self.assertEqual(room["publicIp"], HOST_IP)
        self.start_probe.assert_called_once_with(HOST_IP, HOST_PORT, room["nonce"])
        self.assertEqual(self.host_post("confirm", room, reachable=True), {"ok": True})
        self.assertEqual(self.join(room), {"session": room["session"], "hostNick": "다현",
                         "candidates": [{"ip": HOST_IP, "port": HOST_PORT}], "sameNetwork": False})

    def test_same_network_private_lan_candidates_first(self):
        room = self.open_room(lan=["192.168.1.23", "10.2.3.4", "172.16.5.6", "8.8.8.8",
                                   "127.0.0.1", "169.254.1.2", "::1", "not-an-ip"])
        joined = self.join(room, xff=" 203.0.113.17 , 192.0.2.99")
        self.assertTrue(joined["sameNetwork"])
        self.assertEqual(joined["candidates"], [{"ip": ip, "port": HOST_PORT} for ip in
                                              ("192.168.1.23", "10.2.3.4", "172.16.5.6", HOST_IP)])
        other = self.join(room)
        self.assertFalse(other["sameNetwork"])
        self.assertEqual(other["candidates"], [{"ip": HOST_IP, "port": HOST_PORT}])

    def test_same_network_without_lan(self):
        room = self.open_room(lan=[])
        joined = self.join(room, client_ip=HOST_IP)
        self.assertTrue(joined["sameNetwork"])
        self.assertEqual(joined["candidates"], [{"ip": HOST_IP, "port": HOST_PORT}])

    def test_probe_three_exact_packets_on_loopback(self):
        self.probe_patch.stop()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.bind(("127.0.0.1", 0))
            receiver.settimeout(3)
            room = self.create(client_ip="127.0.0.1", port=receiver.getsockname()[1])
            self.assertEqual(room["publicIp"], "127.0.0.1")
            expected = (struct.pack('<IBBHQIIQHH', 0x42434D4C, 1, 7, 0, 0, 0, 0, 0, 0, 16)
                        + bytes.fromhex(room["nonce"]))
            packets = [receiver.recvfrom(1024)[0] for _ in range(3)]
        for packet in packets:
            self.assertEqual(len(packet), 36 + 16)
            self.assertEqual(packet, expected)
            self.assertEqual(struct.unpack('<IBBHQIIQHH', packet[:36]),
                             (0x42434D4C, 1, 7, 0, 0, 0, 0, 0, 0, 16))
            self.assertEqual(packet[36:].hex(), room["nonce"])

    def test_checking_and_unknown_room(self):
        room = self.create()
        self.assertEqual(self.join(room, 425), {"error": "checking"})
        self.host_post("heartbeat", room, connected=False)
        self.assertEqual(self.join(room, 425), {"error": "checking"})
        missing = {"code": "0000" if room["code"] != "0000" else "0001"}
        self.assertEqual(self.join(missing, 404), {"error": "room"})

    def test_failed_confirm_closes_room(self):
        room = self.create()
        self.assertEqual(self.host_post("confirm", room, reachable=False, reason="probe timeout"), {"ok": True})
        self.assertEqual(self.join(room, 410), {"error": "closed"})
        self.assertEqual(self.host_post("confirm", room, 410, reachable=True), {"error": "closed"})

    def test_close(self):
        room = self.open_room()
        self.join(room)
        self.assertEqual(self.host_post("close", room), {"ok": True})
        self.assertEqual(self.join(room, 410), {"error": "closed"})

    def test_wrong_host_token_for_all_host_actions(self):
        room = self.create()
        for action, fields in (("confirm", {"reachable": True}), ("heartbeat", {"connected": True}), ("close", {})):
            with self.subTest(action=action):
                body = dict(code=room["code"], hostToken="wrong-token", **fields)
                self.assertEqual(self.post(action, body, 403), {"error": "host"})
        self.assertEqual(self.join(room, 425), {"error": "checking"})
        self.host_post("confirm", room, reachable=True)
        self.join(room)

    def test_connected_room_full_same_inst_rejoins_and_disconnect_frees_it(self):
        room = self.open_room()
        first = self.join(room)
        self.assertEqual(self.host_post("heartbeat", room, connected=True), {"ok": True})
        self.assertEqual(self.join(room, 409, inst="other-PC"), {"error": "full"})
        self.assertEqual(self.join(room, nick="하늘 새 이름"), first)
        self.assertEqual(self.host_post("heartbeat", room, connected=False), {"ok": True})
        self.join(room, inst="other-PC")
        self.host_post("heartbeat", room, connected=True)
        self.assertEqual(self.join(room, 409), {"error": "full"})
        self.join(room, inst="other-PC")

    def test_room_expires_at_45_seconds_and_join_does_not_refresh_it(self):
        room = self.open_room()
        self.now += 44
        self.join(room)
        self.now += 1
        self.assertEqual(self.join(room, 410), {"error": "closed"})
        self.assertEqual(self.host_post("heartbeat", room, 410, connected=False), {"error": "closed"})

    def test_heartbeat_refreshes_host_deadline(self):
        room = self.open_room()
        self.now += 40
        self.host_post("heartbeat", room, connected=False)
        self.now += 44
        self.join(room)
        self.now += 1
        self.assertEqual(self.join(room, 410), {"error": "closed"})

    def test_active_and_closed_codes_skipped_then_reused_after_ten_minutes(self):
        # 사용 가능한 코드 중 맨 앞을 골라, 우연에 기대지 않고 재사용을 확인한다.
        with mock.patch.object(lc.secrets, "choice", side_effect=lambda codes: codes[0]):
            room = self.open_room()
            active = self.create()
            self.assertNotEqual(active["code"], room["code"])
            self.now += 45
            self.assertEqual(self.join(room, 410), {"error": "closed"})
            replacement = self.create()
            self.assertNotIn(replacement["code"], (room["code"], active["code"]))
            self.now += 599
            self.assertEqual(self.join(room, 410), {"error": "closed"})
            self.now += 1
            reused = self.open_room()
            self.assertEqual(reused["code"], room["code"])
            self.assertNotEqual(reused["session"], room["session"])
            self.assertNotEqual(reused["hostToken"], room["hostToken"])
            self.join(reused)

    def test_500_room_limit_and_capacity_released_by_close_and_expiry(self):
        rooms = [self.create() for _ in range(500)]
        self.assertEqual(len({room["code"] for room in rooms}), 500)
        body = {"nick": "추가 방장", "inst": "extra-PC", "port": HOST_PORT, "lan": []}
        self.assertEqual(self.post("create", body, 503), {"error": "busy"})
        self.assertEqual(self.start_probe.call_count, 500)
        self.host_post("close", rooms[0])
        replacement = self.create()
        self.assertNotEqual(replacement["code"], rooms[0]["code"])
        self.now += 45
        self.create()

    def test_ipv6_or_invalid_public_ip_rejected(self):
        room = self.open_room()
        create_body = {"nick": "다현", "inst": "host-PC", "port": HOST_PORT, "lan": []}
        join_body = {"code": room["code"], "nick": "하늘", "inst": "guest-PC"}
        for action, body in (("create", create_body), ("join", join_body)):
            for xff in ("2001:db8::1, 203.0.113.17", "::ffff:127.0.0.1", "not-ip"):
                with self.subTest(action=action, xff=xff):
                    self.assertEqual(self.post(action, body, 400, xff=xff), {"error": "ipv4"})
            self.assertEqual(self.post(action, body, 400, client_ip="::1"), {"error": "ipv4"})

    def test_invalid_port(self):
        for port in (1023, 65536, -1, "12000", 12000.5, True, None):
            with self.subTest(port=port):
                body = {"nick": "다현", "inst": "host-PC", "port": port, "lan": []}
                self.assertEqual(self.post("create", body, 400), {"error": "bad"})
        self.start_probe.assert_not_called()

    def test_invalid_nick_and_inst_on_create_and_join(self):
        room = self.open_room()
        invalid = {"nick": ("", "가" * 41, " \n\x00 ", 123, None),
                   "inst": ("", "a" * 65, "pc_name", " pc ", "한글", 123, None)}
        for action in ("create", "join"):
            for key, values in invalid.items():
                for value in values:
                    with self.subTest(action=action, key=key, value=value):
                        body = {"code": room["code"], "nick": "하늘", "inst": "guest-PC", "port": HOST_PORT, "lan": []}
                        body[key] = value
                        self.assertEqual(self.post(action, body, 400), {"error": "bad"})

    def test_valid_limits_and_nick_control_characters_removed(self):
        for port in (1024, 65535):
            with self.subTest(port=port):
                room = self.open_room(nick="가" * 40, inst="a" * 64, port=port, lan=[])
                self.assertEqual(self.join(room)["hostNick"], "가" * 40)
        room = self.open_room(nick=" \t다\x00현\x7f\x85\n ")
        self.assertEqual(self.join(room)["hostNick"], "다현")

    def test_invalid_lan_upnp_booleans_reason_and_code(self):
        for fields in ({"lan": "192.168.1.2"}, {"lan": ["192.168.1.2"] * 9}, {"upnp": None},
                       {"upnp": {"found": "yes"}}, {"upnp": {"mapped": 1}}, {"upnp": {"externalIp": 123}}):
            with self.subTest(fields=fields):
                body = dict(nick="다현", inst="host-PC", port=HOST_PORT)
                body.update(fields)
                self.assertEqual(self.post("create", body, 400), {"error": "bad"})
        room = self.create()
        for action, key in (("confirm", "reachable"), ("heartbeat", "connected")):
            for value in (None, 1, "true"):
                with self.subTest(action=action, value=value):
                    self.assertEqual(self.host_post(action, room, 400, **{key: value}), {"error": "bad"})
        self.assertEqual(self.host_post("confirm", room, 400, reachable=False, reason=123), {"error": "bad"})
        for code in ("123", "12345", "１２３４", 1234, None):
            with self.subTest(code=code):
                self.assertEqual(self.join({"code": code}, 400), {"error": "bad"})
        self.assertEqual(self.join(room, 425), {"error": "checking"})

    def test_body_limit_is_16_kib_of_utf8_bytes(self):
        body = {"nick": "다현", "inst": "host-PC", "port": HOST_PORT, "lan": []}
        raw = json.dumps(body, ensure_ascii=False).encode("utf-8")
        exact = raw + b" " * (16 * 1024 - len(raw))
        self.send(H(lc.PREFIX + "create", raw=exact))
        too_large = H(lc.PREFIX + "create", raw=exact + b" ")
        self.assertEqual(self.send(too_large, 400), {"error": "bad"})
        self.assertEqual(too_large.rfile.tell(), 0)

    def test_invalid_json_and_content_length(self):
        for raw in (b"{", b"[]", b"null", b'"text"', b"\xff", b""):
            with self.subTest(raw=raw):
                self.assertEqual(self.send(H(lc.PREFIX + "create", raw=raw), 400), {"error": "bad"})
        for length in (None, "not-a-number", "-1", "0"):
            with self.subTest(length=length):
                h = H(lc.PREFIX + "create", {})
                del h.headers["Content-Length"]
                if length is not None:
                    h.headers["Content-Length"] = length
                self.assertEqual(self.send(h, 400), {"error": "bad"})
        truncated = H(lc.PREFIX + "create", {})
        truncated.headers.replace_header("Content-Length", "3")
        self.assertEqual(self.send(truncated, 400), {"error": "bad"})

    def test_routing_and_standalone_handlers_without_starting_a_server(self):
        self.assertTrue(lc.handles(lc.PREFIX + "create?from=app"))
        self.assertFalse(lc.handles("/api/lsetter/collab/create"))
        self.assertFalse(lc.handles("/api/livemix/collab-other/create"))
        for path in (lc.PREFIX, lc.PREFIX + "unknown", "/api/other/create"):
            self.assertEqual(self.send(H(path, {}), 404), {"error": "path"})
        h = H(lc.PREFIX + "create?from=app", {"nick": "다현", "inst": "host-PC", "port": HOST_PORT})
        lc._StandaloneHandler.do_POST(h)
        self.response(h)
        ping = H("/api/ping", {})
        lc._StandaloneHandler.do_GET(ping)
        self.assertEqual(self.response(ping), {"ok": True})
        unknown = H("/api/other", {})
        lc._StandaloneHandler.do_GET(unknown)
        self.assertEqual(self.response(unknown, 404), {"error": "path"})

    def test_event_logs_preserve_nicks_ips_and_upnp(self):
        upnp = {"found": True, "mapped": True, "externalIp": "192.0.2.88"}
        room = self.create(upnp=upnp)
        self.host_post("confirm", room, reachable=True, reason="수신\n확인")
        self.join(room)
        self.host_post("close", room)
        lines = self.logs.getvalue().splitlines()
        self.assertEqual(len(lines), 4)
        events = [json.loads(line.removeprefix("[livemix_collab] ")) for line in lines]
        self.assertEqual([event["action"] for event in events], ["create", "confirm", "join", "close"])
        self.assertEqual([event["nick"] for event in events], ["다현", "다현", "하늘", "다현"])
        self.assertEqual([event["publicIp"] for event in events], [HOST_IP, HOST_IP, GUEST_IP, HOST_IP])
        self.assertEqual(events[0]["upnp"], upnp)
        self.assertEqual(events[1]["reason"], "수신\n확인")
        self.start_probe.assert_called_once_with(HOST_IP, HOST_PORT, room["nonce"])

    # ── 홀 펀칭 (10/16 두 PC 시험: 독산 공유기 UPnP 꺼짐, 두 공유기 모두 구멍 뚫기 됨) ──
    def punch_join(self, room, **fields):
        body = {"code": room["code"], "nick": "하늘", "inst": "guest-PC-02"}
        body.update(fields)
        return self.post("join", body, client_ip=GUEST_IP)

    def test_punch_keeps_room_open_and_heartbeat_returns_guest_for_30_seconds(self):
        room = self.create(stun={"ip": HOST_IP, "port": 40001})
        self.assertEqual(self.host_post("confirm", room, reachable=False, reason="UPnP", punch=True), {"ok": True, "punch": True})
        self.assertEqual(self.host_post("heartbeat", room, connected=False), {"ok": True})   # no guest yet
        joined = self.punch_join(room, stun={"ip": GUEST_IP, "port": 50001}, port=50002)
        # the host's router changed its port: the public port and the STUN mapping both
        self.assertEqual(joined["candidates"], [{"ip": HOST_IP, "port": HOST_PORT}, {"ip": HOST_IP, "port": 40001}])
        self.assertEqual(self.host_post("heartbeat", room, connected=False),
                         {"ok": True, "guest": {"candidates": [{"ip": GUEST_IP, "port": 50001}, {"ip": GUEST_IP, "port": 50002}]}})
        self.now += 31
        self.assertEqual(self.host_post("heartbeat", room, connected=True), {"ok": True})
        logs = [json.loads(line.removeprefix("[livemix_collab] ")) for line in self.logs.getvalue().splitlines()]
        self.assertEqual(logs[0]["stun"], {"ip": HOST_IP, "port": 40001})
        self.assertTrue(logs[1]["punch"])
        self.assertEqual(logs[2]["stun"], {"ip": GUEST_IP, "port": 50001})

    def test_closed_port_without_stun_or_punch_still_closes_the_room(self):
        room = self.create()
        self.assertEqual(self.host_post("confirm", room, reachable=False, punch=True), {"ok": True})
        self.join(room, status=410)
        room = self.create(stun={"ip": HOST_IP, "port": HOST_PORT})
        self.assertEqual(self.host_post("confirm", room, reachable=False), {"ok": True})   # 0.15.2: no punch
        self.join(room, status=410)

    def test_reachable_room_with_stun_answers_like_before(self):
        room = self.open_room(stun={"ip": HOST_IP, "port": HOST_PORT})
        joined = self.punch_join(room, stun={"ip": GUEST_IP, "port": 50001})
        self.assertEqual(joined["candidates"], [{"ip": HOST_IP, "port": HOST_PORT}])   # same as the public port: once
        self.assertEqual(self.host_post("heartbeat", room, connected=False), {"ok": True})   # no punching needed

    def test_invalid_stun_punch_and_guest_port(self):
        base = {"nick": "다현", "inst": "host-PC-01", "port": HOST_PORT}
        for stun in ("1.2.3.4:5", {"ip": "::1", "port": 5}, {"ip": "1.2.3.4", "port": 0}, {"ip": "1.2.3.4", "port": "5"},
                     {"port": 5}, {"ip": "1.2.3.4", "port": True}):
            self.assertEqual(self.post("create", dict(base, stun=stun), 400, client_ip=HOST_IP), {"error": "bad"})
        room = self.create(stun={"ip": HOST_IP, "port": 40001})
        self.assertEqual(self.host_post("confirm", room, 400, reachable=False, punch="yes"), {"error": "bad"})
        room = self.open_room()
        for port in (0, 70000, "5", True):
            self.assertEqual(self.post("join", {"code": room["code"], "nick": "하늘", "inst": "g", "port": port}, 400,
                                       client_ip=GUEST_IP), {"error": "bad"})


class CollabV2Test(unittest.TestCase):
    setUp = CollabTest.setUp
    response = CollabTest.response
    send = CollabTest.send
    post = CollabTest.post

    def client(self, number=1):
        return dict(apiVersion=2, appVersion="0.17.0", protocols=[1, 2],
                    capabilities=lc.CAPABILITIES[:], connectionId="%032x" % number, wireRate=48000)

    def create(self, number=1, **fields):
        body = dict(self.client(number), nick="방장", inst="same-profile", port=HOST_PORT)
        body.update(fields)
        return self.post("create", body)

    def host(self, action, room, status=200, **fields):
        return self.post(action, dict(apiVersion=2, code=room["code"], hostToken=room["hostToken"], **fields), status)

    def open(self, number=1, **fields):
        room = self.create(number, **fields)
        self.host("confirm", room, reachable=True)
        return room

    def join(self, room, number=2, status=200, **fields):
        body = dict(self.client(number), code=room["code"], nick="네이티브", inst="same-profile", port=13000 + number)
        body.update(fields)
        return self.post("join", body, status, client_ip=GUEST_IP)

    def beat(self, room, members=()):
        return self.host("heartbeat", room, connected=bool(members), memberStates=list(members))

    def state(self, member, state="connected", epoch=1):
        return dict(participantId=member["participantId"], connectionGeneration=member["connectionGeneration"],
                    state=state, streamEpoch=epoch)

    def invite(self, room):
        return self.host("web/invite", room)

    def request(self, room, invite, number=10, status=200):
        return self.post("web/request", dict(apiVersion=2, code=room["code"], inviteToken=invite["inviteToken"],
                inviteGeneration=invite["inviteGeneration"], action="join", nick="웹 손님", connectionId="%032x" % number,
                protocols=[2], capabilities=lc.CAPABILITIES[:]), status, client_ip="2001:db8::42")

    def accept(self, room, request, status=200):
        return self.host("web/accept", room, status, requestId=request["requestId"])

    def events(self, room, cursor="0", status=200, **auth):
        return self.post("web/events", dict(apiVersion=2, code=room["code"], cursor=cursor, **auth), status)

    def signal(self, room, accepted, signal, sid=1, status=200):
        return self.post("web/signal", dict(apiVersion=2, code=room["code"],
                connectionToken=accepted["connectionToken"], connectionGeneration=accepted["connectionGeneration"],
                signalId=sid, signal=signal), status)

    def web_pair(self, room):
        req = self.request(room, self.invite(room))
        host = self.accept(room, req)
        events = self.events(room, req["cursor"], requestToken=req["requestToken"])
        web = next(e["data"] for e in events["events"] if e["type"] == "accepted")
        return req, host, web

    # ── 중계(TURN) 접속 정보 (10/28: 사무실 PC 웹 -> 도이 집 방장, 직접 연결 실패) ──
    RELAY = [{"urls": ["stun:stun.cloudflare.com:3478", "stun:stun.cloudflare.com:53"]},
             {"urls": ["turn:turn.cloudflare.com:3478?transport=udp", "turns:turn.cloudflare.com:443?transport=tcp",
                       "turn:turn.cloudflare.com:53?transport=udp", "turn:turn.cloudflare.com:80?transport=tcp",
                       "turns:turn.cloudflare.com:5349?transport=tcp", "http://evil.example/", 7],
              "username": "u" * 64, "credential": "c" * 64},
             {"urls": "turn:turn.cloudflare.com:3478?transport=udp"},                      # a relay without credentials
             "nonsense"]

    def test_clean_ice_keeps_only_safe_urls_and_complete_relays(self):
        self.assertEqual(lc._clean_ice(self.RELAY), [
            {"urls": ["stun:stun.cloudflare.com:3478"]},
            {"urls": ["turn:turn.cloudflare.com:3478?transport=udp", "turns:turn.cloudflare.com:443?transport=tcp"],
             "username": "u" * 64, "credential": "c" * 64}])
        self.assertEqual(lc._clean_ice(None), [])

    def test_an_accepted_pair_receives_relay_credentials_both_ways(self):
        servers = lc._clean_ice(self.RELAY)
        with lc._turn_lock:
            lc._turn.update(servers=servers, expires=time.time() + lc.TURN_TTL_SEC)
        room = self.open()
        _, host, web = self.web_pair(room)
        self.assertEqual(host["iceServers"], servers)
        self.assertEqual(web["iceServers"], servers)
        self.assertNotIn("iceServers", self.events(room, hostToken=room["hostToken"])["events"][0]["data"]["room"])

    def test_without_relay_credentials_an_accept_is_unchanged(self):
        room = self.open()
        _, host, web = self.web_pair(room)
        self.assertNotIn("iceServers", host)
        self.assertNotIn("iceServers", web)

    def test_credentials_close_to_expiry_are_not_handed_out(self):
        with lc._turn_lock:
            lc._turn.update(servers=lc._clean_ice(self.RELAY), expires=time.time() + 1800)
        room = self.open()
        _, host, _ = self.web_pair(room)
        self.assertNotIn("iceServers", host)

    def test_a_web_seat_outlives_the_pages_twenty_second_wait(self):
        room = self.open()
        _, host, _ = self.web_pair(room)
        self.assertGreaterEqual(host["reservationExpiresAtMs"] - int(self.now * 1000), 25000)

    def test_three_native_same_profile_retries_and_private_addresses(self):
        room = self.open()
        self.assertEqual(self.create()["code"], room["code"])
        a, b = self.join(room), self.join(room, 3)
        self.assertEqual([m["participantId"] for m in b["members"]], [1, 2, 3])
        self.assertEqual(len(b["reservations"]), 2)
        self.assertEqual(b["roomMode"], "v2")
        self.assertEqual(self.join(room)["participantId"], a["participantId"])
        self.assertEqual(self.join(room, 4, 409), {"error": "full"})
        self.assertNotIn("connectionId", json.dumps(b))
        beat = self.beat(room, [self.state(a), self.state(b)])
        self.assertEqual(beat["reservations"], [])
        self.assertEqual(len(beat["peers"]), 2)
        self.assertEqual(beat["peers"][0]["candidates"], [{"ip": GUEST_IP, "port": 13002}])
        revision = beat["revision"]
        self.assertEqual(self.beat(room, [self.state(a), self.state(b)])["revision"], revision)

    def test_native_and_web_both_entry_orders(self):
        for web_first in (False, True):
            with self.subTest(web_first=web_first):
                room = self.open(100 + int(web_first))
                if not web_first:
                    self.join(room, 200)
                _, host, _ = self.web_pair(room)
                if web_first:
                    self.join(room, 201)
                self.assertEqual(len(self.beat(room)["members"]), 3)
                self.signal(room, host, {"type": "connected"})
                self.assertEqual(self.join(room, 202, 409), {"error": "full"})

    def test_full_native_room_rejects_web_accept(self):
        room = self.open()
        req = self.request(room, self.invite(room))
        self.join(room)
        self.join(room, 3)
        self.assertEqual(self.accept(room, req, 409), {"error": "full"})

    def test_second_web_accept_rejected_and_retry_does_not_extend_reservation(self):
        room = self.open()
        invite = self.invite(room)
        one, two = self.request(room, invite), self.request(room, invite, 11)
        first = self.accept(room, one)
        self.now += 5
        self.assertEqual(self.accept(room, one), first)
        self.assertEqual(self.accept(room, two, 409), {"error": "web_full"})
        self.assertEqual(self.request(room, invite), one)

    def test_concurrent_join_accept_reserves_only_remaining_seat(self):
        room = self.open()
        self.join(room)
        req = self.request(room, self.invite(room))
        barrier = threading.Barrier(3)
        def submit(action, body):
            h = H(lc.PREFIX + action, body)
            barrier.wait()
            lc.handle_post(h)
            return h.status
        native = dict(self.client(3), code=room["code"], nick="동시", inst="same-profile", port=14000)
        web = dict(apiVersion=2, code=room["code"], hostToken=room["hostToken"], requestId=req["requestId"])
        with ThreadPoolExecutor(2) as pool:
            futures = [pool.submit(submit, "join", native), pool.submit(submit, "web/accept", web)]
            barrier.wait()
            self.assertEqual(sorted(f.result(5) for f in futures), [200, 409])
        members = self.beat(room)["members"]
        self.assertEqual(len(members), 3)
        self.assertLessEqual(sum(m["kind"] == "web" for m in members), 1)

    def test_concurrent_accepts_keep_one_web(self):
        room = self.open()
        invite = self.invite(room)
        requests = [self.request(room, invite, n) for n in (10, 11)]
        barrier = threading.Barrier(2)
        def accept(req):
            h = H(lc.PREFIX + "web/accept", dict(apiVersion=2, code=room["code"], hostToken=room["hostToken"], requestId=req["requestId"]))
            barrier.wait()
            lc.handle_post(h)
            return h.status
        with ThreadPoolExecutor(2) as pool:
            self.assertEqual(sorted(pool.map(accept, requests)), [200, 409])
        self.assertEqual(len(self.beat(room)["members"]), 2)

    def test_reissue_invalidates_pending_only(self):
        room = self.open()
        invite = self.invite(room)
        pending = self.request(room, invite)
        req = self.request(room, invite, 11)
        accepted = self.accept(room, req)
        newer = self.invite(room)
        self.assertEqual(newer["inviteGeneration"], invite["inviteGeneration"] + 1)
        self.assertEqual(self.request(room, invite, 12, 410), {"error": "invite"})
        self.assertEqual(self.accept(room, pending, 409), {"error": "request_state"})
        self.events(room, requestToken=pending["requestToken"], status=410)
        self.signal(room, accepted, {"type": "offer", "sdp": "v=0\r\n"})

    def test_pending_and_reservation_expire_and_ids_are_not_reused(self):
        room = self.open()
        invite = self.invite(room)
        req = self.request(room, invite)
        self.now += 40
        self.beat(room)
        self.now += 20
        self.assertEqual(self.accept(room, req, 410), {"error": "expired"})
        req = self.request(room, invite, 11)
        first = self.accept(room, req)
        self.now += 15
        self.assertEqual(self.accept(room, req), first)   # a web seat outlives the page's 20 s wait
        self.beat(room)
        self.now += lc.WEB_RESERVATION_SEC - 15
        self.assertEqual(self.accept(room, req, 410), {"error": "expired"})
        self.signal(room, first, {"type": "connected"}, status=410)
        self.beat(room)
        native = self.join(room)
        self.now += 15
        newer = self.join(room, 3)
        self.assertGreater(newer["participantId"], native["participantId"])
        self.assertEqual(self.join(room, 2, 410), {"error": "expired"})

    def test_kick_leave_and_native_left_reuse_seats(self):
        room = self.open()
        native = self.join(room)
        _, host, web = self.web_pair(room)
        self.signal(room, host, {"type": "connected"})
        self.host("web/kick", room, participantId=host["participantId"], connectionGeneration=host["connectionGeneration"])
        self.signal(room, web, {"type": "answer", "sdp": "old"}, status=410)
        _, host2, web2 = self.web_pair(room)
        self.assertGreater(host2["participantId"], host["participantId"])
        self.post("web/leave", dict(apiVersion=2, code=room["code"], connectionToken=web2["connectionToken"], connectionGeneration=web2["connectionGeneration"]))
        self.beat(room, [self.state(native, "left")])
        self.assertEqual(len(self.join(room, 3)["members"]), 2)

    def test_legacy_mix_downgrades_then_restores_v2(self):
        room = self.open()
        old = dict(code=room["code"], nick="구 앱", inst="old")
        joined = self.post("join", old)
        self.assertEqual(list(joined), ["session", "hostNick", "candidates", "sameNetwork"])
        beat = self.beat(room)
        self.assertEqual(beat["roomMode"], "legacy2")
        self.host("web/invite", room, 409)
        self.join(room, 3, 409)
        self.beat(room, [self.state(beat["members"][1], "left")])
        self.assertEqual(self.beat(room)["roomMode"], "v2")
        self.invite(room)
        self.join(room)
        self.join(room, 3)
        self.assertEqual(self.post("join", old, 409), {"error": "full"})

    def test_new_guest_in_old_host_room_and_capability_intersection(self):
        room = self.post("create", dict(nick="old", inst="old", port=12000))
        self.post("confirm", dict(code=room["code"], hostToken=room["hostToken"], reachable=True))
        self.assertEqual(self.join(room)["roomMode"], "legacy2")
        self.join(room, 3, 409)
        self.host("web/invite", room, 409)
        self.post("heartbeat", dict(code=room["code"], hostToken=room["hostToken"], connected=True))
        self.post("heartbeat", dict(code=room["code"], hostToken=room["hostToken"], connected=False))
        self.join(room, 3)
        modern = self.open(99, capabilities=lc.CAPABILITIES[:3])
        self.assertEqual(self.join(modern)["capabilities"], lc.CAPABILITIES[:3])
        self.host("web/invite", modern, 409)

    def test_native_unavailable_keeps_web_and_epochs_update(self):
        room = self.create()
        confirmed = self.host("confirm", room, reachable=False, reason="no UDP")
        self.assertEqual(confirmed["nativeState"], "unavailable")
        self.assertEqual(self.join(room, 2, 409), {"error": "native_unavailable"})
        _, host, _ = self.web_pair(room)
        beat = self.beat(room, [self.state(host, epoch=2)])
        self.assertEqual(beat["members"][1]["streamEpoch"], 2)
        self.host("heartbeat", room, 409, connected=True, memberStates=[self.state(host)])

    def test_heartbeat_skips_a_member_that_already_ended(self):
        # A web guest whose connection never forms leaves while the host still lists it as connecting: the host's next
        # heartbeat succeeds and its room snapshot drops the guest, instead of a 409 that ends the host's room.
        room = self.open()
        _, host, web = self.web_pair(room)
        self.post("web/leave", dict(apiVersion=2, code=room["code"], connectionToken=web["connectionToken"],
                                    connectionGeneration=web["connectionGeneration"]))
        beat = self.beat(room, [self.state(host, state="connecting")])
        self.assertNotIn(host["participantId"], [m["participantId"] for m in beat["members"]])
        # An ID/generation the server never issued is still refused.
        self.host("heartbeat", room, 409, connected=False,
                  memberStates=[dict(participantId=9, connectionGeneration=99, state="connecting", streamEpoch=1)])

    def test_role_tokens_and_signals_are_private_and_deduplicated(self):
        room = self.open()
        req, host, web = self.web_pair(room)
        self.assertNotEqual(host["connectionToken"], web["connectionToken"])
        self.signal(room, web, {"type": "offer", "sdp": "bad"}, status=403)
        self.signal(room, host, {"type": "answer", "sdp": "bad"}, status=403)
        self.signal(room, web, {"type": "connected"}, status=403)
        before = self.events(room, hostToken=room["hostToken"])["cursor"]
        first = self.signal(room, host, {"type": "offer", "sdp": "v=0\r\n"})
        self.assertEqual(self.signal(room, host, {"type": "offer", "sdp": "v=0\r\n"}), first)
        events = self.events(room, before, connectionToken=web["connectionToken"], connectionGeneration=web["connectionGeneration"])
        self.assertEqual(len(events["events"]), 1)
        self.assertEqual(events["events"][0]["data"]["from"], "host")
        self.signal(room, web, {"type": "answer", "sdp": "v=0\r\n"})
        reply = self.events(room, before, connectionToken=host["connectionToken"], connectionGeneration=host["connectionGeneration"])
        self.assertEqual([e["data"]["from"] for e in reply["events"]], ["web"])
        with mock.patch.object(lc, "EVENT_WAIT_SEC", 0):
            self.assertEqual(self.events(room, before, hostToken=room["hostToken"])["events"], [])
            request_events = self.events(room, req["cursor"], requestToken=req["requestToken"])
            self.assertNotIn("signal", [e["type"] for e in request_events["events"]])
        self.signal(room, dict(host, connectionGeneration=99), {"type": "connected"}, 2, 409)

    def test_limits_and_event_pages(self):
        room = self.open()
        invite = self.invite(room)
        requests = [self.request(room, invite, n) for n in range(10, 18)]
        self.request(room, invite, 18, 429)
        host = self.accept(room, requests[0])
        web = next(e["data"] for e in self.events(room, requestToken=requests[0]["requestToken"])["events"] if e["type"] == "accepted")
        before = self.events(room, hostToken=room["hostToken"])["cursor"]
        self.signal(room, host, {"type": "offer", "sdp": "x" * 65537}, status=400)
        self.signal(room, host, {"type": "offer", "sdp": "x" * 65536})
        self.signal(room, host, {"type": "offer", "sdp": "another"}, 2, 400)
        candidate = dict(type="candidate", candidate="x" * 2048, sdpMid=None, sdpMLineIndex=None)
        self.signal(room, host, dict(candidate, candidate="x" * 2049), 2, 400)
        self.signal(room, host, dict(candidate, sdpMid="x" * 65), 2, 400)
        self.signal(room, host, dict(candidate, sdpMLineIndex=True), 2, 400)
        for n in range(2, 66):
            self.signal(room, host, candidate, n)
        self.signal(room, host, candidate, 66, 400)
        auth = dict(connectionToken=web["connectionToken"], connectionGeneration=web["connectionGeneration"])
        page1 = self.events(room, before, **auth)
        page2 = self.events(room, page1["cursor"], **auth)
        page3 = self.events(room, page2["cursor"], **auth)
        self.assertEqual([len(p["events"]) for p in (page1, page2, page3)], [32, 32, 1])
        self.assertEqual(self.events(room, before, **auth), page1)

    def test_reject_cancel_and_validation(self):
        room = self.open()
        invite = self.invite(room)
        req = self.request(room, invite)
        self.host("web/reject", room, requestId=req["requestId"])
        self.accept(room, req, 409)
        req = self.request(room, invite, 11)
        self.post("web/leave", dict(apiVersion=2, code=room["code"], requestToken=req["requestToken"]))
        self.accept(room, req, 409)
        for version in (True, "2", 3):
            self.post("create", dict(self.client(), apiVersion=version, nick="a", inst="a", port=12000), 400)
        for cursor in (0, "-1", "18446744073709551616"):
            self.events(room, cursor, 400, hostToken=room["hostToken"])
        self.host("heartbeat", room, 400, connected=False, memberStates=[dict(participantId=True)])
        self.post("web/request", dict(apiVersion=2, code=room["code"], inviteToken=invite["inviteToken"], inviteGeneration=invite["inviteGeneration"], action="unknown"), 400)

    def test_longpoll_releases_global_lock_and_close_wakes_it(self):
        room, other = self.open(), self.open(99)
        cursor = self.events(room, hostToken=room["hostToken"])["cursor"]
        condition = lc._rooms[room["code"]]["condition"]
        waiting = threading.Event()
        original_wait = condition.wait
        def wait(timeout):
            waiting.set()
            return original_wait(timeout)
        with mock.patch.object(condition, "wait", side_effect=wait), ThreadPoolExecutor(2) as pool:
            poll = pool.submit(self.events, room, cursor, hostToken=room["hostToken"])
            self.assertTrue(waiting.wait(5))
            beat = pool.submit(self.beat, other).result(5)
            self.assertEqual(beat["code"], other["code"])
            self.beat(room)
            self.assertFalse(poll.done())
            self.host("close", room)
            self.assertIn("closed", [e["data"].get("reason") for e in poll.result(5)["events"]])
        self.events(room, cursor, 410, hostToken=room["hostToken"])

    def test_waiting_request_poll_receives_invalidation_then_token_expires(self):
        room = self.open()
        req = self.request(room, self.invite(room))
        condition = lc._rooms[room["code"]]["condition"]
        waiting, original_wait = threading.Event(), condition.wait
        def wait(timeout):
            waiting.set()
            return original_wait(timeout)
        with mock.patch.object(condition, "wait", side_effect=wait), ThreadPoolExecutor(1) as pool:
            poll = pool.submit(self.events, room, req["cursor"], requestToken=req["requestToken"])
            self.assertTrue(waiting.wait(5))
            self.invite(room)
            self.assertIn("ended", [e["type"] for e in poll.result(5)["events"]])
        self.events(room, req["cursor"], 410, requestToken=req["requestToken"])

    def test_cors_exact_origin_errors_text_plain_and_ipv6(self):
        room = self.open()
        invite = self.invite(room)
        for origin in (lc.WEB_ORIGIN, None, "null", "http://dnakrhs2-crypto.github.io", "https://xn--jb0byyo90f.com", lc.WEB_ORIGIN + ":443", lc.WEB_ORIGIN + ".evil"):
            for valid in (True, False):
                h = H(lc.PREFIX + "web/request", dict(apiVersion=2, code=room["code"], inviteToken=invite["inviteToken"] if valid else "old",
                        inviteGeneration=invite["inviteGeneration"], action="inspect"), client_ip="::1", xff="2001:db8::1")
                h.headers.replace_header("Content-Type", "text/plain")
                if origin is not None:
                    h.headers["Origin"] = origin
                self.send(h, 200 if valid else 410)
                self.assertEqual(h.response_headers.get("Access-Control-Allow-Origin"), lc.WEB_ORIGIN if origin == lc.WEB_ORIGIN else None)
                self.assertNotIn("Access-Control-Allow-Credentials", h.response_headers)
        self.assertEqual(len(self.beat(room)["members"]), 1)
        oversized = H(lc.PREFIX + "web/request", raw=b" " * (lc.MAX_WEB_BODY + 1))
        self.send(oversized, 400)
        self.assertEqual(oversized.rfile.tell(), 0)

    def test_legacy_response_bytes_and_headers(self):
        with mock.patch.object(lc.secrets, "choice", return_value="1234"), mock.patch.object(lc.secrets, "token_hex", side_effect=["a" * 16, "b" * 32, "c" * 32]):
            h = H(lc.PREFIX + "create", dict(nick="다현", inst="host", port=12000))
            self.send(h)
        expected = ('{"code": "1234", "session": "' + "a" * 16 + '", "hostToken": "' + "b" * 32
                    + '", "publicIp": "203.0.113.17", "nonce": "' + "c" * 32 + '"}').encode()
        self.assertEqual(h.wfile.getvalue(), expected)
        self.assertEqual(set(h.response_headers), {"Content-Type", "Content-Length", "Cache-Control"})
        for action, fields in (("confirm", dict(reachable=True)), ("heartbeat", dict(connected=False)), ("close", {})):
            h = H(lc.PREFIX + action, dict(code="1234", hostToken="b" * 32, **fields))
            self.send(h)
            self.assertEqual(h.wfile.getvalue(), b'{"ok": true}')

    def test_legacy_punch_bytes_after_repeated_disconnected_heartbeats(self):
        room = self.post("create", dict(nick="Old", inst="host", port=12000, stun=dict(ip=HOST_IP, port=40000)))
        auth = dict(code=room["code"], hostToken=room["hostToken"])
        self.post("confirm", dict(auth, reachable=False, punch=True))
        self.post("join", dict(code=room["code"], nick="Old guest", inst="guest", port=13000), client_ip=GUEST_IP)
        expected = b'{"ok": true, "guest": {"candidates": [{"ip": "198.51.100.28", "port": 13000}]}}'
        for connected in (True, False, False):
            h = H(lc.PREFIX + "heartbeat", dict(auth, connected=connected))
            self.send(h)
            self.assertEqual(h.wfile.getvalue(), expected)
        for action, fields in (("confirm", dict(reachable="invalid")), ("heartbeat", dict(connected=1)), ("join", dict(nick="", inst="id"))):
            self.assertEqual(self.post(action, dict(code="xxxx", **fields), 400), {"error": "bad"})

    def test_two_concurrent_native_joins_fill_distinct_reservations(self):
        room = self.open()
        barrier = threading.Barrier(2)
        def join(number):
            barrier.wait()
            return self.join(room, number)
        with ThreadPoolExecutor(2) as pool:
            results = list(pool.map(join, (2, 3)))
        self.assertEqual(sorted(r["participantId"] for r in results), [2, 3])
        self.assertEqual(len(self.beat(room)["reservations"]), 2)

    def test_http_response_write_does_not_hold_global_lock(self):
        room = self.open()
        h = H(lc.PREFIX + "heartbeat", dict(apiVersion=2, code=room["code"], hostToken=room["hostToken"],
                                           connected=False, memberStates=[]))
        acquired = []
        write = h.wfile.write
        def unlocked_write(payload):
            free = lc._lock.acquire(blocking=False)
            acquired.append(free)
            if free:
                lc._lock.release()
            return write(payload)
        with mock.patch.object(h.wfile, "write", side_effect=unlocked_write):
            self.send(h)
        self.assertEqual(acquired, [True])


class CollabPersistenceTest(unittest.TestCase):
    response = CollabTest.response
    send = CollabTest.send
    post = CollabTest.post
    client = CollabV2Test.client
    create = CollabV2Test.create
    host = CollabV2Test.host
    open = CollabV2Test.open
    join = CollabV2Test.join
    beat = CollabV2Test.beat
    state = CollabV2Test.state
    invite = CollabV2Test.invite
    request = CollabV2Test.request
    accept = CollabV2Test.accept
    events = CollabV2Test.events
    signal = CollabV2Test.signal
    web_pair = CollabV2Test.web_pair

    def setUp(self):
        CollabTest.setUp(self)
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.path = self.directory / "state" / "rooms.json"
        self.addCleanup(lc.reset_for_tests)  # join the writer before removing its directory
        self.enterContext(mock.patch.object(lc, "EVENT_WAIT_SEC", 0))

    def enable(self):
        lc.reset_for_tests(now=lambda: self.now, state_path=self.path)

    def save(self):
        lc._save_state()
        self.assertTrue(self.path.is_file())
        return json.loads(self.path.read_text(encoding="utf-8"))

    def restart(self, downtime=120):
        self.save()
        self.now += downtime
        self.enable()
        self.assertEqual((lc._rooms, lc._closed, lc._closed_rooms), ({}, {}, {}))
        lc._load_state()

    def test_restart_preserves_tokens_cursors_signals_and_connected_guests(self):
        self.enable()
        room = self.open()
        native = self.join(room)
        req, host, web = self.web_pair(room)
        self.signal(room, host, dict(type="connected"))
        self.beat(room, [self.state(native), self.state(host)])
        cursor = self.events(room, hostToken=room["hostToken"])["cursor"]
        auth = dict(connectionToken=web["connectionToken"], connectionGeneration=web["connectionGeneration"])
        self.assertEqual(self.events(room, cursor, **auth)["events"], [])
        offer = dict(type="offer", sdp="v=0\r\n")
        reply = self.signal(room, host, offer, 2)
        pending = self.request(room, self.invite(room), 11)
        expected_host = self.events(room, cursor, hostToken=room["hostToken"])
        expected_web = self.events(room, cursor, **auth)
        old_condition = lc._rooms[room["code"]]["condition"]
        self.restart()
        restored = lc._rooms[room["code"]]
        self.assertIsNot(restored["condition"], old_condition)
        self.assertEqual(restored["seen"], self.now)
        self.assertEqual(len(self.beat(room, [self.state(native), self.state(host)])["members"]), 3)
        # The offline pending request expires normally; events issued before it are unchanged.
        got = self.events(room, cursor, hostToken=room["hostToken"])
        self.assertEqual(got["events"][:len(expected_host["events"])], expected_host["events"])
        self.assertEqual(self.events(room, cursor, **auth)["events"], expected_web["events"])
        self.assertEqual(self.signal(room, host, offer, 2), reply)  # integer reply-cache keys
        self.assertEqual(self.accept(room, req), host)
        request_events = self.events(room, req["cursor"], requestToken=req["requestToken"])
        self.assertIn(web, [e["data"] for e in request_events["events"] if e["type"] == "accepted"])
        self.accept(room, pending, 410)
        before = got["cursor"]
        new_req = self.request(room, self.invite(room), 12)
        new_events = self.events(room, before, hostToken=room["hostToken"])
        self.assertIn(new_req["requestId"], [e["data"].get("requestId") for e in new_events["events"]])
        self.assertTrue(all(int(e["id"]) > int(before) for e in new_events["events"]))
        self.signal(room, web, dict(type="answer", sdp="v=0\r\n"))
        host_events = self.events(room, before, connectionToken=host["connectionToken"],
                                  connectionGeneration=host["connectionGeneration"])
        self.assertIn("signal", [e["type"] for e in host_events["events"]])
        self.assertEqual(self.create()["code"], room["code"])

    def test_restart_allows_native_and_web_rejoins_without_reusing_ids(self):
        self.enable()
        room = self.open()
        native = self.join(room)
        invite = self.invite(room)
        req = self.request(room, invite)
        host = self.accept(room, req)
        web = next(e["data"] for e in self.events(room, req["cursor"], requestToken=req["requestToken"])["events"]
                   if e["type"] == "accepted")
        self.signal(room, host, dict(type="connected"))
        self.beat(room, [self.state(native, "left")])
        self.restart()
        self.assertEqual(self.join(room, 2, 410), {"error": "expired"})
        newer = self.join(room, 3)
        self.assertGreater(newer["participantId"], host["participantId"])
        self.assertGreater(newer["connectionGeneration"], host["connectionGeneration"])
        self.post("web/leave", dict(apiVersion=2, code=room["code"], connectionToken=web["connectionToken"],
                                   connectionGeneration=web["connectionGeneration"]))
        again = self.accept(room, self.request(room, invite, 11))
        self.assertGreater(again["participantId"], newer["participantId"])
        self.assertGreater(again["connectionGeneration"], newer["connectionGeneration"])
        self.signal(room, again, dict(type="connected"))
        self.assertEqual(len(self.beat(room, [self.state(newer)])["members"]), 3)
        self.assertGreater(self.invite(room)["inviteGeneration"], invite["inviteGeneration"])

    def test_legacy_heartbeat_restores_guest_alias_and_detached_guest(self):
        for modern_guest in (False, True):
            with self.subTest(modern_guest=modern_guest):
                self.enable()
                room = self.post("create", dict(nick="old host", inst="old", port=HOST_PORT,
                                                stun=dict(ip=HOST_IP, port=40000)))
                auth = dict(code=room["code"], hostToken=room["hostToken"])
                self.post("confirm", dict(auth, reachable=False, punch=True))
                if modern_guest:
                    self.join(room)
                else:
                    self.post("join", dict(code=room["code"], nick="old guest", inst="guest", port=13000),
                              client_ip=GUEST_IP)
                self.post("heartbeat", dict(auth, connected=True))
                model = lc._rooms[room["code"]]
                pid = next(iter(model["nativePeers"]))
                self.assertIs(model["guest"], model["nativePeers"][pid])
                saved = self.save()["rooms"][room["code"]]
                self.assertEqual(saved["guestParticipantId"], pid)
                self.assertNotIn("guest", saved)  # only one copy of the shared dict
                self.restart()
                model = lc._rooms[room["code"]]
                self.assertIs(model["guest"], model["nativePeers"][pid])
                self.assertEqual(self.post("heartbeat", dict(auth, connected=True)), {"ok": True})
                self.post("heartbeat", dict(auth, connected=False))
                self.assertEqual(model["nativePeers"], {})
                self.assertEqual(model["guest"] is None, modern_guest)
                # Legacy punch retains a detached guest after disconnect; keep that too.
                guest = model["guest"]
                self.restart(0)
                self.assertEqual(lc._rooms[room["code"]]["guest"], guest)
                self.post("join", dict(code=room["code"], nick="rejoin", inst="guest", port=13000))

    def test_closed_codes_events_and_code_allocation_survive_restart(self):
        self.enable()
        with mock.patch.object(lc.secrets, "choice", side_effect=lambda choices: choices[0]):
            room, closed = self.open(), self.open(99)
            self.host("close", closed)
            ended = lc._closed_rooms[closed["code"]]["events"][:]
            self.restart()
            self.assertEqual(lc._closed_rooms[closed["code"]]["events"], ended)
            self.assertEqual(self.join(closed, status=410), {"error": "closed"})
            self.events(closed, "1", 410, hostToken=closed["hostToken"])
            self.assertEqual(self.join(dict(code="9999"), status=404), {"error": "room"})
            new = self.open(100)
            self.assertEqual([room["code"], closed["code"], new["code"]], ["0000", "0001", "0002"])
            self.now += lc.CLOSED_KEEP - 120
            # GC may mutate state even when the requested room returns an error.
            self.join(dict(code="9999"), status=404)
            saved = self.save()
            self.assertNotIn(closed["code"], saved["closed"])
            self.assertNotIn(closed["code"], saved["closedRooms"])

    def test_pending_request_can_be_accepted_after_short_restart(self):
        self.enable()
        room = self.open()
        invite = self.invite(room)
        req = self.request(room, invite)
        self.restart(10)
        self.assertEqual(self.request(room, invite), req)
        self.assertEqual(lc._rooms[room["code"]]["pendingWebRequests"][req["requestId"]]["expiresAtMs"], req["expiresAtMs"])
        host = self.accept(room, req)
        events = self.events(room, req["cursor"], requestToken=req["requestToken"])
        self.assertIn(host["connectionGeneration"], [e["data"].get("connectionGeneration") for e in events["events"]])

    def test_offline_pending_and_reservations_keep_their_deadlines(self):
        self.enable()
        room = self.open()
        native = self.join(room)
        invite = self.invite(room)
        accepted_req, pending = self.request(room, invite), self.request(room, invite, 11)
        accepted = self.accept(room, accepted_req)
        deadlines = {pid: r["expiresAtMs"] for pid, r in lc._rooms[room["code"]]["reservations"].items()}
        self.restart()
        restored = lc._rooms[room["code"]]
        self.assertEqual({pid: r["expiresAtMs"] for pid, r in restored["reservations"].items()}, deadlines)
        self.assertEqual(restored["pendingWebRequests"][pending["requestId"]]["expiresAtMs"], pending["expiresAtMs"])
        self.assertEqual(len(self.beat(room)["members"]), 1)
        self.accept(room, pending, 410)
        self.accept(room, accepted_req, 410)
        self.signal(room, accepted, dict(type="connected"), status=410)
        self.assertGreater(self.join(room, 3)["participantId"], native["participantId"])

    def test_missing_corrupt_and_old_files_start_empty_with_one_log(self):
        self.enable()
        room = self.open()
        saved = self.save()
        for content in (None, "{broken", "[]", "{}", json.dumps(dict(saved, version=99)),
                        json.dumps(dict(saved, savedAt=self.now - 901))):
            with self.subTest(content=content):
                self.enable()
                if content is None:
                    self.path.unlink(missing_ok=True)
                else:
                    self.path.write_text(content, encoding="utf-8")
                with mock.patch.object(lc, "_log") as log:
                    lc._load_state()
                log.assert_called_once()
                self.assertEqual((lc._rooms, lc._closed, lc._closed_rooms), ({}, {}, {}))
                self.assertEqual(self.join(room, status=404), {"error": "room"})
        with mock.patch.object(Path, "read_text", side_effect=PermissionError), mock.patch.object(lc, "_log") as log:
            lc._load_state()
        log.assert_called_once()
        self.assertEqual((lc._rooms, lc._closed, lc._closed_rooms), ({}, {}, {}))
        self.path.write_text(json.dumps(dict(saved, savedAt=self.now - 900)), encoding="utf-8")
        lc._load_state()
        self.beat(room)

    def test_reset_disables_disk_access_until_a_test_supplies_a_path(self):
        self.enable()
        room = self.open()
        self.save()
        lc.reset_for_tests(now=lambda: self.now)
        with mock.patch.object(Path, "read_text", side_effect=AssertionError("unexpected state read")), \
             mock.patch.object(lc, "_write_state", side_effect=AssertionError("unexpected state write")):
            lc._load_state()
            self.open()
            lc._save_state()
        self.assertIsNone(lc._state_path)
        self.enable()
        lc._load_state()
        self.beat(room)

    def test_background_write_does_not_hold_room_lock_or_block_heartbeat(self):
        self.enable()
        room = self.open()
        entered, release = threading.Event(), threading.Event()
        observed = []
        write = lc._write_state
        def slow_write(path, payload):
            free = lc._lock.acquire(blocking=False)
            observed.append((free, threading.current_thread() is threading.main_thread()))
            if free:
                lc._lock.release()
            entered.set()
            if not release.wait(5):
                raise TimeoutError("test did not release writer")
            write(path, payload)
        with mock.patch.object(lc, "_write_state", side_effect=slow_write):
            lc._start_state_writer()
            try:
                self.assertTrue(entered.wait(3))
                with ThreadPoolExecutor(1) as pool:
                    self.assertEqual(pool.submit(self.beat, room).result(2)["code"], room["code"])
            finally:
                release.set()
                lc._stop_state_writer()
        self.assertEqual(observed, [(True, False)])
        self.assertTrue(self.path.is_file())

    def test_changes_coalesce_at_one_second_and_replace_complete_json(self):
        self.enable()
        room = self.open()
        self.save()
        previous = self.path.read_text(encoding="utf-8")
        replaced = threading.Event()
        replacements = []
        replace = lc.os.replace
        def atomic_replace(source, destination):
            payload = json.loads(Path(source).read_text(encoding="utf-8"))
            replacements.append((time.monotonic(), payload, Path(destination).read_text(encoding="utf-8")))
            replace(source, destination)
            replaced.set()
        self.now += 10
        self.beat(room)
        with mock.patch.object(lc.os, "replace", side_effect=atomic_replace):
            lc._start_state_writer()
            try:
                self.assertTrue(replaced.wait(3))
                replaced.clear()
                self.now += 10
                self.invite(room)
                self.beat(room)
                self.assertTrue(replaced.wait(3))
            finally:
                lc._stop_state_writer()
        self.assertEqual(len(replacements), 2)
        self.assertEqual(replacements[0][2], previous)
        self.assertGreaterEqual(replacements[1][0] - replacements[0][0], 1)
        final = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(final, replacements[1][1])
        self.assertEqual(final["rooms"][room["code"]]["seen"], self.now)
        self.assertEqual(final["rooms"][room["code"]]["inviteGeneration"], 1)

    def test_write_failure_is_logged_and_requests_still_succeed(self):
        self.enable()
        # A directory cannot be replaced by a file (PermissionError on Windows).
        self.path.mkdir(parents=True)
        room = self.open()
        self.beat(room)
        with mock.patch.object(lc, "_log") as log:
            lc._save_state()
        log.assert_called_once()
        self.beat(room)
        self.join(room)
        self.request(room, self.invite(room))

    def test_host_event_snapshot_is_saved_without_turn_cache(self):
        self.enable()
        room = self.open()
        self.save()
        lc._turn.update(servers=[dict(urls="stun:unused", credential="cache-only-secret")], expires=self.now + 10000)
        cursor = self.events(room, hostToken=room["hostToken"])["cursor"]
        saved = self.save()
        self.assertEqual(saved["rooms"][room["code"]]["eventId"], int(cursor))
        self.assertNotIn("cache-only-secret", self.path.read_text(encoding="utf-8"))
        self.restart(0)
        self.assertIsNone(lc._turn["servers"])

    def test_import_restores_and_starts_writer_at_default_or_override_path(self):
        # Exercise production import, without calling any persistence helper.
        script = '''
import json, time
from pathlib import Path
import livemix_collab as lc
if not lc._rooms:
    lc._start_probe = lambda *a: None
    with lc._lock:
        lc._native("create", dict(nick="import", inst="import", port=12000),
                   type("H", (), {"headers": {}, "client_address": ("127.0.0.1", 1)})(), time.time(), False)
code = next(iter(lc._rooms))
with lc._lock:
    lc._native("heartbeat", dict(code=code, hostToken=lc._rooms[code]["hostToken"], connected=False), None, time.time(), False)
print("ROOM=" + code, flush=True)
time.sleep(2)
'''
        for override in (False, True):
            with self.subTest(override=override):
                local = self.directory / ("override" if override else "default")
                state_path = local / ("custom.json" if override else "LiveMixRoomServer/rooms.json")
                env = dict(os.environ, LOCALAPPDATA=str(local))
                env.pop("LIVEMIX_ROOMS_STATE", None)
                if override:
                    env["LIVEMIX_ROOMS_STATE"] = str(state_path)
                def run():
                    result = subprocess.run([sys.executable, "-B", "-c", script], env=env,
                                            cwd=Path(lc.__file__).parent, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    return next(line for line in result.stdout.splitlines() if line.startswith("ROOM="))
                first = run()
                self.assertTrue(state_path.is_file())
                saved = json.loads(state_path.read_text(encoding="utf-8"))
                self.assertIn(first[5:], saved["rooms"])
                self.assertEqual(run(), first)

    def test_serve_restores_host_heartbeat_from_saved_file(self):
        self.enable()
        self.now = time.time()
        room = self.open()
        self.save()
        port = StandaloneHTTPTest.start_server(self, self.path)
        with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as conn:
            conn.request("POST", lc.PREFIX + "heartbeat", json.dumps(dict(apiVersion=2, code=room["code"],
                         hostToken=room["hostToken"], connected=False, memberStates=[])))
            response = conn.getresponse()
            payload = response.read()
            self.assertEqual(response.status, 200, payload)
            self.assertEqual(json.loads(payload)["session"], room["session"])

    def test_restored_web_invite_warms_relay_credentials_before_the_first_accept(self):
        # A restored invite never passes web/invite, where the relay credentials are warmed. Without a warm-up at
        # load the first accept after a restart hands out no relay, so a web guest that needs TURN cannot connect.
        servers = lc._clean_ice(CollabV2Test.RELAY)
        self.enable()
        room = self.open()
        invite = self.invite(room)
        self.save()
        self.now += 120
        self.enable()
        def fetched():   # stands in for the Cloudflare request: tests never fetch real relay credentials
            with lc._turn_lock:
                lc._turn.update(servers=servers, expires=time.time() + lc.TURN_TTL_SEC, fetching=False)
        with lc._turn_lock:
            lc._turn["failedAt"] = 0.0
        with mock.patch.object(lc, "_fetch_turn", fetched):
            lc._load_state()
            deadline = time.monotonic() + 2
            while lc._turn["servers"] is None and time.monotonic() < deadline:
                time.sleep(0.01)
            host = self.accept(room, self.request(room, invite))
            self.assertEqual(host["iceServers"], servers)

    def test_serve_without_override_never_reads_or_writes_the_live_file(self):
        # A local test server (--serve) runs on the same PC as the live server: it must not restore the live
        # rooms nor overwrite the live file with test rooms.
        self.enable()
        self.now = time.time()
        room = self.open()
        self.save()
        local = self.directory / "localappdata"
        live = local / "LiveMixRoomServer" / "rooms.json"
        live.parent.mkdir(parents=True)
        live.write_bytes(self.path.read_bytes())
        before = live.read_bytes()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = dict(os.environ, LOCALAPPDATA=str(local), LIVEMIX_TURN_KEY_FILE=str(self.directory / "missing-turn-key.json"))
        env.pop("LIVEMIX_ROOMS_STATE", None)
        process = subprocess.Popen([sys.executable, str(Path(lc.__file__).resolve()), "--serve", "127.0.0.1:%d" % port],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.addCleanup(lambda: (process.terminate(), process.wait(timeout=5)))
        deadline = time.monotonic() + 10
        while True:
            try:
                with socket.create_connection(("127.0.0.1", port), 1):
                    break
            except OSError:
                if time.monotonic() >= deadline or process.poll() is not None:
                    self.fail("standalone server did not start")
                time.sleep(0.05)
        def post(action, body):
            with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as conn:
                conn.request("POST", lc.PREFIX + action, json.dumps(body))
                response = conn.getresponse()
                return response.status, json.loads(response.read())
        status, _ = post("heartbeat", dict(apiVersion=2, code=room["code"], hostToken=room["hostToken"],
                                           connected=False, memberStates=[]))
        self.assertEqual(status, 404)   # the live room was not restored into the test server
        status, created = post("create", dict(self.client(7), nick="serve", inst="serve", port=12001))
        self.assertEqual(status, 200, created)
        time.sleep(lc.STATE_SAVE_SEC + 1)
        self.assertEqual(live.read_bytes(), before)
        self.assertEqual(sorted(p.name for p in live.parent.iterdir()), ["rooms.json"])


class StandaloneHTTPTest(unittest.TestCase):
    def start_server(self, state_path):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        process = subprocess.Popen([sys.executable, str(Path(lc.__file__).resolve()), "--serve", "127.0.0.1:%d" % port],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=dict(os.environ, LIVEMIX_ROOMS_STATE=str(state_path),
                         LIVEMIX_TURN_KEY_FILE=str(state_path.parent / "missing-turn-key.json")),
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.addCleanup(lambda: (process.terminate(), process.wait(timeout=5)))
        deadline = time.monotonic() + 10
        while True:
            try:
                with socket.create_connection(("127.0.0.1", port), 1):
                    break
            except OSError:
                if time.monotonic() >= deadline or process.poll() is not None:
                    self.fail("standalone server did not start")
                time.sleep(0.05)
        return port

    def test_real_http_create_invite_accept_signals_and_leave(self):
        directory = self.enterContext(tempfile.TemporaryDirectory())
        port = self.start_server(Path(directory) / "rooms.json")
        def post(endpoint, **body):
            with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as conn:
                conn.request("POST", lc.PREFIX + endpoint, json.dumps(dict(apiVersion=2, **body), ensure_ascii=False).encode(),
                             {"Content-Type": "text/plain", "Origin": lc.WEB_ORIGIN})
                response = conn.getresponse()
                payload = response.read()
                self.assertEqual(response.status, 200, payload)
                self.assertEqual(response.getheader("Access-Control-Allow-Origin"), lc.WEB_ORIGIN)
                self.assertEqual(int(response.getheader("Content-Length")), len(payload))
                return json.loads(payload)
        room = post("create", nick="실제 HTTP 방장", inst="http-host", port=12000, appVersion="0.17.0", protocols=[1, 2],
                    capabilities=lc.CAPABILITIES, connectionId="1" * 32, wireRate=48000)
        code, token = room["code"], room["hostToken"]
        post("confirm", code=code, hostToken=token, reachable=False)
        invite = post("web/invite", code=code, hostToken=token)
        req = post("web/request", code=code, inviteToken=invite["inviteToken"], inviteGeneration=invite["inviteGeneration"],
                   action="join", nick="실제 웹", connectionId="2" * 32, protocols=[2], capabilities=lc.CAPABILITIES)
        host = post("web/accept", code=code, hostToken=token, requestId=req["requestId"])
        events = post("web/events", code=code, requestToken=req["requestToken"], cursor=req["cursor"])
        web = next(e["data"] for e in events["events"] if e["type"] == "accepted")
        auths = [dict(connectionToken=a["connectionToken"], connectionGeneration=a["connectionGeneration"]) for a in (host, web)]
        for sender, receiver, kind in ((0, 1, "offer"), (1, 0, "answer")):
            signal = dict(type=kind, sdp="v=0\r\ns=LiveMix\r\n")
            post("web/signal", code=code, **auths[sender], signalId=1, signal=signal)
            events = post("web/events", code=code, **auths[receiver], cursor="0")
            self.assertIn(signal, [e["data"].get("signal") for e in events["events"]])
        post("web/signal", code=code, **auths[0], signalId=2, signal=dict(type="connected"))
        post("web/leave", code=code, **auths[1])
        beat = post("heartbeat", code=code, hostToken=token, connected=False, memberStates=[])
        self.assertEqual(len(beat["members"]), 1)
        post("close", code=code, hostToken=token)


if __name__ == "__main__":
    unittest.main(verbosity=2)
