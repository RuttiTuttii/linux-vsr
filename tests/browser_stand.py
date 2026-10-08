#!/usr/bin/env python3
# isolated browser video fixture stand
# usage: python3 tests/browser_stand.py --seconds 12 --browser /usr/bin/zen-browser

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[1]
VIDEO_MARKERS = (
    "ycbcr", "vUv_", "vUV_", "sample_yuv", "ps_quad_yuv",
    "debiased", "nv12", "p010", "samplerexternaloes",
)


def reset_directory(path):
    # preserve the requested root while removing only its old contents
    path.mkdir(parents=True, exist_ok=True)
    for child in path.iterdir():
        if child.is_dir() and not child.is_symlink():
            shutil.rmtree(child)
        else:
            child.unlink()


def find_browser(requested):
    # prefer the browser with the already verified WebRender path
    candidates = ["/opt/zen-browser-bin/zen-bin", "zen-browser", "firefox",
                  "chromium", "brave", "google-chrome-stable"]
    if requested:
        candidate = Path(requested)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
        from shutil import which
        resolved = which(requested)
        if resolved:
            return resolved
        raise FileNotFoundError(f"browser not executable: {requested}")
    from shutil import which
    for candidate in candidates:
        if candidate.startswith("/") and Path(candidate).is_file():
            return candidate
        resolved = which(candidate)
        if resolved:
            return resolved
    raise FileNotFoundError("no supported browser found")


def make_fixture(video_path):
    # generate a short local 4:2:0 clip with edges and motion; no network needed
    command = ["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi",
               "-i", "testsrc2=size=640x360:rate=30:duration=8", "-an",
               "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
               str(video_path)]
    subprocess.run(command, check=True)


def write_page(page_path, video_path):
    # autoplay muted video starts without a browser gesture
    source = quote(video_path.as_uri(), safe=":/")
    page_path.write_text(
        "<!doctype html><meta charset='utf-8'>\n"
        "<title>linux-vsr browser fixture</title>\n"
        "<body style='margin:0;background:#111'>\n"
        f"<video id='fixture' src='{source}' autoplay muted loop playsinline "
        "style='width:100vw;height:100vh;object-fit:contain'></video>\n"
        "<script>document.getElementById('fixture').play().catch(()=>{});</script>\n",
        encoding="utf-8",
    )


def shader_summary(dump_dir):
    # classify only shader payloads; stats files are diagnostic metadata
    payloads = [p for p in dump_dir.glob("*.glsl") if p.is_file()]
    video = []
    patched = []
    for path in payloads:
        text = path.read_text(errors="replace").lower()
        if any(marker in text for marker in VIDEO_MARKERS):
            video.append(path)
        if "sample_luma_cas" in text:
            patched.append(path)
    return payloads, video, patched


def run_browser(browser, output, seconds, mode, headless, strict):
    # prepare all inputs and per-run output paths
    reset_directory(output)
    video_path = output / "fixture.mp4"
    page_path = output / "fixture.html"
    profile = output / "profile"
    dump_dir = output / "shader-dumps"
    profile.mkdir(parents=True, exist_ok=True)
    for marker in ("lock", ".parentlock", "crashes", "minidumps", "compatibility.ini"):
        marker_path = profile / marker
        if marker_path.is_dir():
            shutil.rmtree(marker_path)
        else:
            marker_path.unlink(missing_ok=True)
    dump_dir.mkdir()
    make_fixture(video_path)
    write_page(page_path, video_path)
    config = output / "config.ini"
    config.write_text("enable=1\ndebug=1\nwatermark=1\nmode=" + mode + "\nbackend=auto\n")

    name = Path(browser).name.lower()
    browser_args = []
    if "firefox" in name or "zen" in name:
        browser_args = ["--new-instance", "--profile", str(profile), "--no-remote"]
        if headless:
            browser_args.append("--headless")
    else:
        browser_args = [f"--user-data-dir={profile}", "--no-first-run",
                        "--no-default-browser-check", "--autoplay-policy=no-user-gesture-required"]
        if headless:
            browser_args.append("--headless=new")
    browser_args.append(page_path.as_uri())

    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    env.update({
        "VSR_CONFIG": str(config), "VSR_MODE": mode, "VSR_DEBUG": "1",
        "VSR_DUMP": "1", "VSR_DUMP_DIR": str(dump_dir),
        "VSR_HIT_LOG": str(output / "hits.log"),
        "VSR_COMPILE_LOG": str(output / "compile-errors.log"),
        "VSR_CHROMIUM_GL": "1",
    })
    command = [str(ROOT / "bin/linux-vsr"), "launch", browser, *browser_args]
    browser_log = (output / "browser.log").open("w", encoding="utf-8")
    started = time.monotonic()
    process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=browser_log,
                               stderr=subprocess.STDOUT, start_new_session=True)
    try:
        time.sleep(seconds)
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
        browser_log.close()

    payloads, video, patched = shader_summary(dump_dir)
    hits = (output / "hits.log").read_text(errors="replace").splitlines() if (output / "hits.log").exists() else []
    errors = ((output / "compile-errors.log").read_text(errors="replace").splitlines()
              if (output / "compile-errors.log").exists() else [])
    report = {
        "browser": browser,
        "mode": mode,
        "seconds": round(time.monotonic() - started, 2),
        "browser_exit": process.returncode,
        "browser_started": True,
        "shader_dumps": len(payloads),
        "video_candidates": len(video),
        "patched_shaders": len(patched),
        "hits": len(hits),
        "compile_errors": len(errors),
        "paths": {"browser_log": str(output / "browser.log"),
                  "shader_dumps": str(dump_dir), "hits": str(output / "hits.log")},
    }
    if errors:
        report["verdict"] = "FAIL_COMPILE_ERRORS"
    elif hits:
        report["verdict"] = "PASS_PATCHED"
    elif video:
        report["verdict"] = "OBSERVED_VIDEO_NO_HIT"
    elif payloads:
        report["verdict"] = "OBSERVED_SHADERS_NO_VIDEO_MARKER"
    else:
        report["verdict"] = "NO_SHADER_DUMPS"
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if strict and report["verdict"] != "PASS_PATCHED":
        return 2
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="run an isolated local browser video fixture")
    parser.add_argument("--browser", help="browser binary or PATH name")
    parser.add_argument("--output", default="/tmp/linux-vsr-browser-stand")
    parser.add_argument("--seconds", type=float, default=12)
    parser.add_argument("--mode", choices=("cas", "easu", "directional", "off"), default="cas")
    parser.add_argument("--headless", action="store_true", help="startup-only headless smoke; GPU hits are not expected")
    parser.add_argument("--strict", action="store_true", help="return nonzero unless a shader hit is recorded")
    args = parser.parse_args(argv)
    try:
        browser = find_browser(args.browser)
        return run_browser(browser, Path(args.output), args.seconds, args.mode, args.headless, args.strict)
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        print(f"browser stand failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
