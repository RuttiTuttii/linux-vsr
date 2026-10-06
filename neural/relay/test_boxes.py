#!/usr/bin/env python3
# unit tests for mp4 box surgery without gpu or network
# usage: /tmp/vsr-neural/bin/python neural/relay/test_boxes.py

import os
import subprocess
import sys
import tempfile

# import module under test from same directory
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boxes


# run ffmpeg helper producing fragmented mp4 test asset
def make_fragmented(path):
    # generate two second test pattern as fragmented mp4
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10:duration=2",
        "-c:v", "libx264", "-g", "10",
        "-movflags", "+empty_moov+default_base_moof+frag_keyframe",
        path,
    ]
    assert subprocess.run(cmd).returncode == 0, "ffmpeg asset failed"


# test box walk finds ftyp and moov siblings
def test_walk(path):
    # read generated asset bytes
    with open(path, "rb") as fp:
        data = fp.read()
    # collect top-level box kinds
    kinds = [k for k, _, _ in boxes.walk_boxes(data)]
    assert "ftyp" in kinds, f"ftyp missing: {kinds}"
    assert "moov" in kinds, f"moov missing: {kinds}"
    assert "moof" in kinds, f"moof missing: {kinds}"
    print("  [pass] walk finds ftyp moov moof")


# test init and media split boundary
def test_split(path):
    # read generated asset bytes
    with open(path, "rb") as fp:
        data = fp.read()
    # split on first moof marker
    init, media = boxes.split_init_media(data)
    assert init and media, "split produced empty part"
    assert len(init) + len(media) == len(data), "split loses bytes"
    # init part must not contain moof boxes
    assert b"moof" not in init, "init contains moof"
    # media part must start with moof box
    assert media[4:8] == b"moof", "media does not start with moof"
    print("  [pass] split init and media")


# test timescale extraction from init
def test_timescale(path):
    # read generated asset bytes
    with open(path, "rb") as fp:
        data = fp.read()
    # split init part only
    init, _ = boxes.split_init_media(data)
    # extract track timescale value
    scale = boxes.read_timescale(init)
    assert scale and scale > 0, f"bad timescale: {scale}"
    print(f"  [pass] timescale={scale}")


# test trun duration matches asset length roughly
def test_trun(path):
    # read generated asset bytes
    with open(path, "rb") as fp:
        data = fp.read()
    # split media part only
    _, media = boxes.split_init_media(data)
    # extract timescale for conversion
    init, _ = boxes.split_init_media(data)
    scale = boxes.read_timescale(init)
    # sum trun durations across fragments
    total = 0.0
    off = 0
    while off < len(media):
        box = boxes.read_box(media, off)
        assert box, "truncated media box"
        _, size, _ = box
        dur = boxes.read_trun_duration(media[off:off + size], scale)
        if dur:
            total += dur
        off += size
    # two second asset must measure near two seconds
    assert 1.5 < total < 2.5, f"bad duration: {total}"
    print(f"  [pass] trun duration={total:.2f}s")


# test tfdt base time reads on media part
def test_tfdt(path):
    # read generated asset bytes
    with open(path, "rb") as fp:
        data = fp.read()
    # split media and init parts
    init, media = boxes.split_init_media(data)
    scale = boxes.read_timescale(init)
    # read first fragment base time
    base, _ = boxes.read_tfdt(media, scale)
    assert base is not None, "tfdt missing"
    print(f"  [pass] tfdt base={base}")


def main():
    # build temp asset for tests
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "frag.mp4")
        make_fragmented(path)
        print("running box surgery tests...")
        test_walk(path)
        test_split(path)
        test_timescale(path)
        test_trun(path)
        test_tfdt(path)
    print("all box tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
