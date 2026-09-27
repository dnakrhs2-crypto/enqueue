# -*- coding: utf-8 -*-
"""Minimal obs-websocket v5 client for the LiveMix OBS tests (no auth - the portable test OBS has none).

    from obsws import Obs
    with Obs(port=4466) as obs:
        print(obs.call("GetVersion"))
"""
import json, itertools
import websocket  # websocket-client


class ObsError(RuntimeError):
    pass


class Obs:
    def __init__(self, host="127.0.0.1", port=4466, timeout=10.0):
        self.url = "ws://%s:%d" % (host, port)
        self.timeout = timeout
        self.ws = None
        self.ids = itertools.count(1)

    def __enter__(self):
        self.ws = websocket.create_connection(self.url, timeout=self.timeout)
        hello = json.loads(self.ws.recv())
        if hello.get("op") != 0:
            raise ObsError("no Hello: %r" % hello)
        if hello["d"].get("authentication"):
            raise ObsError("this OBS asks for a password - use the portable test OBS")
        self.ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1, "eventSubscriptions": 0}}))
        identified = json.loads(self.ws.recv())
        if identified.get("op") != 2:
            raise ObsError("not identified: %r" % identified)
        return self

    def __exit__(self, *exc):
        if self.ws is not None:
            self.ws.close()

    def call(self, request_type, data=None, check=True):
        rid = str(next(self.ids))
        msg = {"op": 6, "d": {"requestType": request_type, "requestId": rid}}
        if data is not None:
            msg["d"]["requestData"] = data
        self.ws.send(json.dumps(msg))
        while True:
            reply = json.loads(self.ws.recv())
            if reply.get("op") == 7 and reply["d"].get("requestId") == rid:
                status = reply["d"]["requestStatus"]
                if check and not status.get("result"):
                    raise ObsError("%s failed: %s %s" % (request_type, status.get("code"), status.get("comment", "")))
                return reply["d"].get("responseData", {})
