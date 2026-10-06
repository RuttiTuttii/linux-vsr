#!/usr/bin/env python3
# drive test browser to youtube watch page for relay observation
# usage: /tmp/vsr-neural/bin/python neural/bidi/drive_watch.py [url]

import sys
import time

WATCH = "https://www.youtube.com/watch?v=aqz-KE-bpKQ"


def main():
    # late import keeps module importable without dep
    import os

    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
    from spike_attach import Bidi

    # pick target url from args
    target = sys.argv[1] if len(sys.argv) > 1 else WATCH
    # connect and open session
    bidi = Bidi("127.0.0.1", 9222, "/session")
    bidi.cmd("session.new", {"capabilities": {}})
    # find first browsing context
    tree = bidi.cmd("browsingContext.getTree", {})
    ctx = tree["contexts"][0]["context"]
    # navigate to watch page
    bidi.cmd("browsingContext.navigate", {"context": ctx, "url": target})
    print("navigated, waiting for player...")
    # wait for player bootstrap and segment fetches
    time.sleep(30)
    print("drive done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
