#!/usr/bin/env python3
# unit tests for relay request and response helpers
# usage: python3 neural/relay/test_relay.py

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vsr_relay


def test_full_body():
    body, status, headers = vsr_relay.slice_range(b"abc", {}, "video/mp4")
    assert body == b"abc"
    assert status == 200
    assert headers["Accept-Ranges"] == "bytes"
    print("  [pass] full body response")


def test_explicit_range():
    body, status, headers = vsr_relay.slice_range(
        b"abc", {"Range": "bytes=1-2"}, "video/mp4"
    )
    assert body == b"bc"
    assert status == 206
    assert headers["Content-Range"] == "bytes 1-2/3"
    print("  [pass] explicit range")


def test_open_and_suffix_ranges():
    body, status, headers = vsr_relay.slice_range(
        b"abc", {"Range": "bytes=1-"}, "video/mp4"
    )
    assert (body, status) == (b"bc", 206)
    assert headers["Content-Range"] == "bytes 1-2/3"

    body, status, headers = vsr_relay.slice_range(
        b"abc", {"Range": "bytes=-2"}, "video/mp4"
    )
    assert (body, status) == (b"bc", 206)
    assert headers["Content-Range"] == "bytes 1-2/3"
    print("  [pass] open and suffix ranges")


def test_invalid_ranges():
    for value in ("bytes=9-10", "bytes=2-1", "bytes=-0", "bytes=nope"):
        body, status, headers = vsr_relay.slice_range(
            b"abc", {"Range": value}, "video/mp4"
        )
        assert body == b""
        assert status == 416
        assert headers["Content-Range"] == "bytes */3"
    print("  [pass] invalid ranges")


def main():
    print("running relay helper tests...")
    test_full_body()
    test_explicit_range()
    test_open_and_suffix_ranges()
    test_invalid_ranges()
    print("all relay helper tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
