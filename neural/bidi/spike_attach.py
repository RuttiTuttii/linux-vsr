#!/usr/bin/env python3
# bidi spike 1: attach to zen over webdriver bidi, observe network events
# usage: /tmp/vsr-neural/bin/python neural/bidi/spike_attach.py
# proves daemon-to-browser attach without extensions or forks

import json
import sys
import time

PORT = 9222


# minimal sync bidi client over raw transport
class Bidi:
    def __init__(self, host, port, path):
        # late import keeps module importable without dep
        try:
            from neural.bidi._ws import RawWs
        except ImportError:
            import os
            import sys
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from _ws import RawWs
        self.ws = RawWs(host, port, path, timeout=10)
        self.next_id = 0
        self.session = None

    # send command and wait for matching id response
    def cmd(self, method, params=None):
        # allocate command id
        self.next_id += 1
        return self.ws.cmd(self.next_id, method, params)

    # drain pending async events without blocking long
    def drain(self, seconds=2.0):
        # set short socket timeout for polling
        self.ws.sock.settimeout(0.5)
        deadline = time.time() + seconds
        seen = 0
        while time.time() < deadline:
            try:
                msg = self.ws.recv_text()
            except Exception:
                continue
            data = json.loads(msg)
            if "method" in data:
                print(f"event: {data['method']}")
                seen += 1
        self.ws.sock.settimeout(10)
        return seen


def main():
    # firefox remote agent speaks bidi on fixed session path
    print("bidi: 127.0.0.1:9222/session")
    # connect and open session
    bidi = Bidi("127.0.0.1", PORT, "/session")
    res = bidi.cmd("session.new", {"capabilities": {}})
    bidi.session = res.get("sessionId")
    print("session:", bidi.session)
    # subscribe to network events
    bidi.cmd("session.subscribe", {"events": ["network.responseCompleted"]})
    print("subscribed, collecting events...")
    # drain events while caller drives the browser
    seen = bidi.drain(seconds=8.0)
    print(f"observed {seen} network events")
    # close session cleanly
    try:
        bidi.cmd("session.end", {})
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
