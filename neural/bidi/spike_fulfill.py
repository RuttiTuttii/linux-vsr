#!/usr/bin/env python3
# bidi spike 3: fulfill intercepted request with synthetic bytes
# usage: /tmp/vsr-neural/bin/python neural/bidi/spike_fulfill.py
# proves browser accepts daemon-provided response bodies

import base64
import json
import sys
import time

PORT = 9222
TARGET = "https://httpbin.org/image/png"

# one pixel png payload for substitution
PIXEL_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="
)


# minimal sync bidi client over raw transport
class Bidi:
    def __init__(self, host, port, path):
        # late import keeps module importable without dep
        import os

        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from _ws import RawWs

        self.ws = RawWs(host, port, path, timeout=15)
        self.next_id = 0

    # send command and wait for matching id response
    def cmd(self, method, params=None):
        # allocate command id
        self.next_id += 1
        return self.ws.cmd(self.next_id, method, params)

    # send command without waiting for response
    def send(self, method, params=None):
        # allocate command id
        self.next_id += 1
        self.ws.send_text(
            json.dumps({"id": self.next_id, "method": method, "params": params or {}})
        )
        return self.next_id


def main():
    # connect and open session
    bidi = Bidi("127.0.0.1", PORT, "/session")
    bidi.cmd("session.new", {"capabilities": {}})
    # find first browsing context
    tree = bidi.cmd("browsingContext.getTree", {})
    ctx = tree["contexts"][0]["context"]
    # install intercept for httpbin host
    try:
        res = bidi.cmd(
            "network.addIntercept",
            {
                "phases": ["beforeRequestSent"],
                "urlPatterns": [{"type": "pattern", "protocol": "https", "hostname": "httpbin.org"}],
            },
        )
        print("intercept:", res.get("intercept"))
    except Exception as ex:
        print("addIntercept failed:", str(ex)[:160])
        return 2
    # subscribe to interception and completion events
    bidi.cmd("session.subscribe", {"events": ["network.beforeRequestSent", "network.responseCompleted"]})
    # navigate without blocking so pauses keep flowing
    bidi.send("browsingContext.navigate", {"context": ctx, "url": TARGET})
    print("navigated, watching 20s...")
    # serve synthetic favicon, continue everything else
    deadline = time.time() + 20.0
    bidi.ws.sock.settimeout(0.5)
    fulfilled = 0
    continued = 0
    while time.time() < deadline:
        try:
            msg = bidi.ws.recv_text()
        except Exception:
            continue
        data = json.loads(msg)
        # log completed responses for acceptance proof
        if data.get("method") == "network.responseCompleted":
            info = data.get("params", {}).get("response", {})
            print("completed:", info.get("status", "?"), str(info.get("url", "?"))[:80])
            continue
        # skip non-interception frames
        if data.get("method") != "network.beforeRequestSent":
            continue
        params = data.get("params", {})
        request = params.get("request", {})
        url = request.get("url", "")
        rid = request.get("request")
        # fulfill image body with synthetic png bytes
        if "image/png" in url and rid:
            try:
                bidi.cmd(
                    "network.provideResponse",
                    {
                        "request": rid,
                        "statusCode": 200,
                        "headers": [
                            {"name": "Content-Type", "value": {"type": "string", "value": "image/png"}},
                            {"name": "Content-Length", "value": {"type": "string", "value": str(len(PIXEL_PNG))}},
                        ],
                        "body": {"type": "base64", "value": base64.b64encode(PIXEL_PNG).decode()},
                    },
                )
                print("fulfilled:", url[:80])
                fulfilled += 1
            except Exception as ex:
                print("provideResponse failed:", str(ex)[:160])
                return 3
        elif rid:
            # resume all other requests unchanged
            try:
                bidi.cmd("network.continueRequest", {"request": rid})
                continued += 1
            except Exception:
                pass
    print(f"fulfilled={fulfilled} continued={continued}")
    return 0 if fulfilled else 1


if __name__ == "__main__":
    sys.exit(main())
