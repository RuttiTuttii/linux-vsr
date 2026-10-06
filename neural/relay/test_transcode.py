#!/usr/bin/env python3
# unit test for segment transcode on synthetic asset with real inference
# usage: VSR_RT_SDK_DIR=... /tmp/vsr-neural/bin/python neural/relay/test_transcode.py

import os
import subprocess
import sys
import tempfile

# import modules under test from same directory
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boxes
import transcode


# build fragmented test asset on disk
def make_asset(path):
    # generate two second fragmented mp4
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10:duration=2",
        "-c:v", "libx264", "-g", "10",
        "-movflags", "+empty_moov+default_base_moof+frag_keyframe",
        path,
    ]
    assert subprocess.run(cmd).returncode == 0, "ffmpeg asset failed"


# test decode yields expected frame count
def test_decode(path):
    # read asset bytes
    with open(path, "rb") as fp:
        data = fp.read()
    # decode all frames
    raw, w, h, fps = transcode.decode_segment(data)
    # validate geometry and count
    assert (w, h) == (320, 240), f"bad dims {w}x{h}"
    assert len(raw) // (w * h * 3) == 20, "bad frame count"
    print(f"  [pass] decode {w}x{h} {fps:.1f}fps")


# test full segment transcode doubles dims with valid boxes
def test_roundtrip(path):
    # read asset bytes
    with open(path, "rb") as fp:
        data = fp.read()
    # split init and media parts
    init, media = boxes.split_init_media(data)
    # create neural effect once
    vsr = transcode.Vsr(scale=2, quality="LOW")
    # transcode media part only
    out = transcode.transcode_segment(media, init, 2, vsr)
    # validate output boxes parse cleanly
    kinds = [k for k, _, _ in boxes.walk_boxes(out)]
    assert "moof" in kinds, f"no moof in output: {kinds}"
    # build fresh init at target size and probe combined stream
    our_init = transcode.build_init("h264", 640, 480)
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "stream=width,height",
         "-of", "csv", "-"],
        input=our_init + out,
        capture_output=True,
    )
    text = probe.stdout.decode(errors="replace")
    assert "640" in text and "480" in text, f"bad dims: {text}"
    # validate output duration preserved
    scale = boxes.read_timescale(init)
    dur = boxes.read_trun_duration(out, scale)
    assert dur and 1.5 < dur < 2.5, f"bad duration: {dur}"
    print(f"  [pass] roundtrip 640x480 dur={dur:.2f}s")


def main():
    # build temp asset for tests
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "frag.mp4")
        make_asset(path)
        print("running transcode tests...")
        test_decode(path)
        test_roundtrip(path)
    print("all transcode tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
