#!/usr/bin/env python3
# bidi spike 2: watch real video traffic through intercept
# usage: /tmp/vsr-neural/bin/python neural/bidi/spike_watch.py [url]
# installs url-pattern intercept, navigates to test page, logs hits

import json
import sys
import time

PORT = 9222
TEST_URL = "https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/720/Big_Buck_Bunny_720_10s_1MB.mp4"


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
        import json

        # serialize command frame only
        self.ws.send_text(
            json.dumps({"id": self.next_id, "method": method, "params": params or {}})
        )
        return self.next_id


def main():
    # pick target page from args
    target = sys.argv[1] if len(sys.argv) > 1 else TEST_URL
    # connect and open session
    bidi = Bidi("127.0.0.1", PORT, "/session")
    bidi.cmd("session.new", {"capabilities": {}})
    # find first browsing context
    tree = bidi.cmd("browsingContext.getTree", {})
    ctx = tree["contexts"][0]["context"]
    # navigate to target page before intercepting
    bidi.cmd("browsingContext.navigate", {"context": ctx, "url": target})
    print("navigated, installing intercept...")
    # install intercept for video hosts
    try:
        res = bidi.cmd(
            "network.addIntercept",
            {
                "phases": ["beforeRequestSent"],
                "urlPatterns": [
                    {"type": "pattern", "protocol": "https", "hostname": "test-videos.co.uk"},
                    {"type": "pattern", "protocol": "https", "hostname": "googlevideo.com"},
                ],
            },
        )
        print("intercept:", res.get("intercept"))
    except Exception as ex:
        print("addIntercept failed:", str(ex)[:160])
        return 2
    # subscribe to interception events
    bidi.cmd("session.subscribe", {"events": ["network.beforeRequestSent"]})
    # reload page without blocking so paused requests keep flowing
    bidi.send("browsingContext.navigate", {"context": ctx, "url": target})
    print("reloaded, watching 20s...")
    # collect interception events, continuing each paused request
    deadline = time.time() + 20.0
    bidi.ws.sock.settimeout(0.5)
    hits = 0
    while time.time() < deadline:
        try:
            msg = bidi.ws.recv_text()
        except Exception:
            continue
        data = json.loads(msg)
        # collect late navigate response as informational
        if data.get("id") and "method" not in data:
            continue
        # handle paused intercepted requests
        if data.get("method") != "network.beforeRequestSent":
            continue
        params = data.get("params", {})
        request = params.get("request", {})
        # log request url
        url = request.get("url", "?")
        print("HIT:", url[:120])
        hits += 1
        # resume paused request unchanged
        try:
            bidi.cmd("network.continueRequest", {"request": request.get("request")})
        except Exception as ex:
            print("continue failed:", str(ex)[:100])
    print(f"total hits: {hits}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
