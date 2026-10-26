# -*- coding: utf-8 -*-
"""LiveMix 합방 시험: 가짜 HTTP 요청과 loopback UDP만 사용하며 서버를 실행하지 않는다.
python -m unittest test_livemix_collab -v
"""
import io
import json
import socket
import struct
import unittest
from contextlib import redirect_stdout
from email.message import Message
from unittest import mock

import livemix_collab as lc

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
