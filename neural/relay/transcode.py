#!/usr/bin/env python3
# segment transcode: dash media bytes in, upscaled dash media bytes out
# decode with ffmpeg, upscale frames with nvidia vfx, encode back with timing

import os
import subprocess
import sys

# import box helpers from same directory
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boxes


# thin neural wrapper around vfx bindings for numpy arrays
class Vsr:
    def __init__(self, scale, quality="HIGH"):
        # late import keeps module importable without gpu stack
        import cupy as cp
        from nvvfx import VideoSuperRes

        # store scale and resolve quality enum
        self.scale = scale
        self.cp = cp
        qlevel = getattr(VideoSuperRes.QualityLevel, quality, VideoSuperRes.QualityLevel.HIGH)
        # create effect instance
        self.effect = VideoSuperRes(quality=qlevel)
        self.loaded_dims = None

    # upscale float32 hwc array in range 0..1
    def upscale_array(self, arr):
        # read input geometry from array shape
        h, w, _ = arr.shape
        ow, oh = w * self.scale, h * self.scale
        # reload effect when output size changed
        if self.loaded_dims != (ow, oh):
            self.effect.output_width = ow
            self.effect.output_height = oh
            self.effect.load()
            self.loaded_dims = (ow, oh)
        # move frame to device as channels-first tensor
        frame = self.cp.asarray(arr.transpose(2, 0, 1))
        frame = self.cp.ascontiguousarray(frame.astype(self.cp.float32))
        # run inference and synchronize stream
        out = self.effect.run(frame)
        self.cp.cuda.runtime.deviceSynchronize()
        # copy result back to host hwc array
        res = self.cp.from_dlpack(out.image).copy()
        return res.get().transpose(1, 2, 0)


# run ffmpeg helper with piped input and output
def run_ffmpeg(args, data):
    # launch process with pipes
    proc = subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error"] + args,
        input=data,
        capture_output=True,
    )
    # raise on encode failure
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {proc.stderr[:200]!r}")
    return proc.stdout


# decode segment bytes to rgb frames plus geometry
def decode_segment(data, hwaccel=None):
    # probe stream geometry first
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "stream=width,height,avg_frame_rate",
         "-of", "csv", "-"],
        input=data,
        capture_output=True,
    )
    # parse first stream line for dims
    w = h = 0
    fps = 30.0
    for line in probe.stdout.decode(errors="replace").splitlines():
        parts = line.strip().split(",")
        if len(parts) >= 4 and parts[0] == "stream":
            w, h = int(parts[1]), int(parts[2])
            if "/" in parts[3]:
                num, den = parts[3].split("/")
                fps = float(num) / float(den) if float(den) else 30.0
            break
    if not w or not h:
        raise RuntimeError("cannot probe segment geometry")
    # build decode args with optional hwaccel
    args = []
    if hwaccel:
        args += ["-hwaccel", hwaccel]
    args += ["-i", "pipe:0", "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"]
    # decode frames to raw rgb bytes
    raw = run_ffmpeg(args, data)
    # validate frame buffer size
    frame = w * h * 3
    assert len(raw) % frame == 0 and len(raw) > 0, "bad raw size"
    return (raw, w, h, fps)


# build init segment for codec at target resolution
def build_init(codec, out_w, out_h, encoder="h264_nvenc"):
    # fall back to software encoder when nvenc missing
    if encoder == "h264_nvenc":
        probe = subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi",
             "-i", "nullsrc=size=16x16:rate=1:duration=0.1",
             "-c:v", "h264_nvenc", "-f", "null", "-"],
            capture_output=True,
        )
        if probe.returncode != 0:
            encoder = "libx264"
    # encode single black frame as fragmented mp4
    args = [
        "-f", "lavfi", "-i", f"color=size={out_w}x{out_h}:rate=10:duration=0.1",
        "-frames:v", "1", "-c:v", encoder,
        "-movflags", "+empty_moov+default_base_moof+frag_keyframe",
        "-f", "mp4", "pipe:1",
    ]
    data = run_ffmpeg(args, b"")
    # keep init part before first moof only
    init, _ = boxes.split_init_media(data)
    return init


# transcode media segment bytes with neural upscale
def transcode_segment(data, init, scale, vsr, encoder="h264_nvenc"):
    # split timing info from boxes
    timescale = boxes.read_timescale(init)
    base, _ = boxes.read_tfdt(data, timescale)
    dur = boxes.read_trun_duration(data, timescale)
    if base is None or not dur or not timescale:
        raise RuntimeError("segment timing unreadable")
    # decode original frames with init prepended for probe
    raw, w, h, _ = decode_segment(init + data)
    # derive frame count and output geometry
    frame = w * h * 3
    count = len(raw) // frame
    ow, oh = w * scale, h * scale
    fps = count / dur
    # upscale frames through neural effect
    import numpy as np

    out_frames = []
    for i in range(count):
        # convert frame to float rgb array
        arr = np.frombuffer(raw[i * frame:(i + 1) * frame], dtype="uint8")
        arr = arr.reshape(h, w, 3).astype("float32") / 255.0
        out_frames.append(vsr.upscale_array(arr))
    # pack upscaled frames back to raw bytes
    packed = b"".join(
        (frame_arr.clip(0, 1) * 255).astype("uint8").tobytes() for frame_arr in out_frames
    )
    # encode with preserved base timestamp and duration
    args = [
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{ow}x{oh}",
        "-framerate", f"{fps:.3f}", "-i", "pipe:0",
        "-c:v", encoder, "-g", str(count),
        "-output_ts_offset", f"{base / timescale:.3f}",
        "-frag_duration", str(int(dur * 1000000)),
        "-movflags", "+empty_moov+default_base_moof+frag_keyframe",
        "-f", "mp4", "pipe:1",
    ]
    try:
        return run_ffmpeg(args, packed)
    except RuntimeError:
        # retry once with software encoder fallback
        if encoder != "libx264":
            args[args.index(encoder)] = "libx264"
            return run_ffmpeg(args, packed)
        raise
