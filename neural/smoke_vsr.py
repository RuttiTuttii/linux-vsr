#!/usr/bin/env python3
# linux-vsr neural smoke: run real rtx vsr models via nvidia vfx sdk
# usage: /tmp/vsr-neural/bin/python neural/smoke_vsr.py [--w 1280] [--h 720] [--scale 2]
# proves video super resolution inference on local gpu, saves before/after png

import argparse
import sys
import time


# pick available cuda tensor backend for dlpack exchange
def pick_backend():
    # try pytorch first for widest compatibility
    try:
        import torch
        if torch.cuda.is_available():
            return ("torch", torch)
    except Exception:
        pass
    # fall back to cupy for lighter footprint
    try:
        import cupy
        return ("cupy", cupy)
    except Exception:
        pass
    return (None, None)


# build synthetic test frame with sharp edges and text-like bars
def make_test_frame(backend_mod, name, w, h):
    # create gradient plus bars pattern in float32 range 0..1
    if name == "torch":
        import torch
        y, x = torch.meshgrid(
            torch.linspace(0, 1, h, device="cuda"),
            torch.linspace(0, 1, w, device="cuda"),
            indexing="ij",
        )
        r = x
        g = y
        b = ((x * 8) % 1.0 + (y * 8) % 1.0) / 2.0
        frame = torch.stack([r, g, b]).float()
        return frame.contiguous()
    # cupy path with same pattern
    import cupy as cp
    y, x = cp.meshgrid(
        cp.linspace(0, 1, h), cp.linspace(0, 1, w), indexing="ij"
    )
    r = x
    g = y
    b = ((x * 8) % 1.0 + (y * 8) % 1.0) / 2.0
    frame = cp.stack([r, g, b]).astype(cp.float32)
    return cp.ascontiguousarray(frame)


# save float rgb frame to png for visual check
def save_png(frame, name, path):
    # move channels to host as uint8 hwc
    if name == "torch":
        arr = (frame.detach().clamp(0, 1).permute(1, 2, 0).cpu().numpy() * 255).astype("uint8")
    else:
        arr = (frame.transpose(1, 2, 0).get().clip(0, 1) * 255).astype("uint8")
    # write via pillow when present, else raw ppm fallback
    try:
        from PIL import Image
        Image.fromarray(arr).save(path)
    except Exception:
        h, w, _ = arr.shape
        with open(path.replace(".png", ".ppm"), "wb") as fp:
            fp.write(f"P6\n{w} {h}\n255\n".encode())
            fp.write(arr.tobytes())
        print(f"pillow missing, wrote ppm instead of {path}")


def main():
    # parse smoke options
    ap = argparse.ArgumentParser()
    ap.add_argument("--w", type=int, default=1280)
    ap.add_argument("--h", type=int, default=720)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--quality", default="HIGH")
    ap.add_argument("--outdir", default="/tmp/vsr_neural_out")
    args = ap.parse_args()

    # resolve tensor backend
    name, mod = pick_backend()
    if name is None:
        print("no cuda tensor backend (need torch or cupy)", file=sys.stderr)
        return 2

    # import vfx bindings lazily after backend check
    try:
        from nvvfx import VideoSuperRes
    except Exception as ex:
        print(f"cannot import nvvfx: {ex}", file=sys.stderr)
        return 3

    # resolve quality enum by name with safe fallback
    qlevel = getattr(VideoSuperRes.QualityLevel, args.quality, None)
    if qlevel is None:
        print(f"unknown quality {args.quality}, using HIGH")
        qlevel = VideoSuperRes.QualityLevel.HIGH

    # build input frame on gpu
    import os
    os.makedirs(args.outdir, exist_ok=True)
    frame = make_test_frame(mod, name, args.w, args.h)
    save_png(frame, name, f"{args.outdir}/input_{args.w}x{args.h}.png")

    # create effect and set output size
    vsr = VideoSuperRes(quality=qlevel)
    vsr.output_width = args.w * args.scale
    vsr.output_height = args.h * args.scale

    # load models and warm up once (untimed)
    t0 = time.perf_counter()
    vsr.load()
    load_ms = (time.perf_counter() - t0) * 1000.0
    _ = vsr.run(frame)
    if name == "torch":
        mod.cuda.synchronize()
    else:
        mod.cuda.runtime.deviceSynchronize()

    # timed runs for stable measurement
    runs, dt = 10, 0.0
    out = None
    for _ in range(runs):
        t0 = time.perf_counter()
        out = vsr.run(frame)
        if name == "torch":
            mod.cuda.synchronize()
        else:
            mod.cuda.runtime.deviceSynchronize()
        dt += time.perf_counter() - t0

    # convert result copy before effect reuse
    if name == "torch":
        import torch
        result = torch.from_dlpack(out.image).clone()
    else:
        import cupy as cp
        result = cp.from_dlpack(out.image).copy()

    # save upscaled output for visual check
    save_png(result, name, f"{args.outdir}/output_{args.w*args.scale}x{args.h*args.scale}.png")

    # report timings and shapes
    print(f"backend={name} load_ms={load_ms:.0f} avg_ms={dt/runs*1000:.2f} "
          f"in=3x{args.h}x{args.w} out={tuple(result.shape)}")
    print(f"files in {args.outdir}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
