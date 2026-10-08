#!/usr/bin/env python3
# vsr relay daemon: neural upscale of browser video segments in the same tab
# usage: /tmp/vsr-neural/bin/python neural/relay/vsr_relay.py [--port 9222] [--dry-run]
# attaches to running zen over bidi, transcodes googlevideo video ranges

import base64
import json
import subprocess
import sys
import time
import urllib.parse
import urllib.request

PORT = 9222
CACHE_LIMIT = 200


# minimal sync bidi client over raw transport
class Bidi:
    def __init__(self, host, port, path):
        # late import keeps module importable without dep
        import os

        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bidi"))
        from _ws import RawWs

        self.ws = RawWs(host, port, path, timeout=30)
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


# fetch original bytes forwarding range headers
def fetch_original(url, headers):
    # forward range and user agent only
    fwd = {}
    for key in ("Range", "range", "User-Agent", "user-agent"):
        if key in headers:
            fwd["Range" if "ange" in key else "User-Agent"] = headers[key]
            break
    # build request with forwarded headers
    req = urllib.request.Request(url, headers=fwd or {"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=20) as fp:
            return (fp.status, dict(fp.headers.items()), fp.read())
    except urllib.error.HTTPError as ex:
        return (ex.code, dict(ex.headers.items()), ex.read())
    except Exception as ex:
        print(f"fetch failed: {ex}")
        return (None, {}, b"")


# slice bytes for range request, return body status and headers
def slice_range(body, req_headers, content_type):
    # find range header value
    raw = req_headers.get("Range") or req_headers.get("range")
    if not raw or not raw.startswith("bytes="):
        return (body, 200, {"Content-Type": content_type, "Content-Length": str(len(body))})
    # parse first byte range spec
    try:
        spec = raw[len("bytes="):].split(",")[0].strip()
        start_s, end_s = spec.split("-")
        start = int(start_s) if start_s else 0
        end = int(end_s) if end_s else len(body) - 1
        end = min(end, len(body) - 1)
    except ValueError:
        return (body, 200, {"Content-Type": content_type, "Content-Length": str(len(body))})
    # slice payload and describe range
    part = body[start:end + 1]
    headers = {
        "Content-Type": content_type,
        "Content-Length": str(len(part)),
        "Content-Range": f"bytes {start}-{end}/{len(body)}",
        "Accept-Ranges": "bytes",
    }
    return (part, 206, headers)


# map mime marker to codec and container pair
def codec_for_url(url):
    # inspect mime query marker
    if "webm" in url:
        return ("vp9", "webm")
    return ("h264", "mp4")


# decide whether url carries video bytes worth transcoding
def is_video_url(url):
    # inspect mime query marker for video kinds
    query = urllib.parse.urlparse(url).query
    params = urllib.parse.parse_qs(query)
    mime = params.get("mime", [""])[0]
    return "video/" in mime


# format headers for bidi bytes value shape
def bidi_headers(headers):
    # convert mapping to name and typed value pairs
    out = []
    for name, value in headers.items():
        # skip hop headers the browser must compute
        if name.lower() in ("content-length", "connection", "transfer-encoding"):
            continue
        out.append({"name": name, "value": {"type": "string", "value": str(value)}})
    return out


def main():
    # parse daemon options
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--quality", default="HIGH")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    # lazy imports keep startup light
    sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
    import boxes
    import transcode

    # create shared neural effect lazily on first segment
    vsr = None

    def get_vsr():
        # reuse singleton across segments
        nonlocal vsr
        if vsr is None:
            vsr = transcode.Vsr(scale=args.scale, quality=args.quality)
        return vsr

    # per-stream init cache keyed by stream base url
    init_cache = {}
    # transcoded segment cache keyed by full url
    seg_cache = {}

    # connect with retries until browser agent ready
    bidi = None
    for attempt in range(30):
        try:
            candidate = Bidi("127.0.0.1", args.port, "/session")
            candidate.cmd("session.new", {"capabilities": {}})
            bidi = candidate
            break
        except Exception as ex:
            print(f"attach try {attempt + 1}: {str(ex)[:80]}")
            time.sleep(2)
    if not bidi:
        print("browser agent not reachable", file=sys.stderr)
        return 2
    print(f"attached to browser on port {args.port}")
    # install intercept for all requests, daemon filters video hosts
    bidi.cmd(
        "network.addIntercept",
        {"phases": ["beforeRequestSent"]},
    )
    print("intercept active, filtering googlevideo in daemon")
    # subscribe to interception events
    bidi.cmd("session.subscribe", {"events": ["network.beforeRequestSent"]})
    print("relay running, dry_run=", args.dry_run)
    # serve loop over paused requests
    bidi.ws.sock.settimeout(0.5)
    while True:
        try:
            msg = bidi.ws.recv_text()
        except Exception:
            continue
        data = json.loads(msg)
        # skip non-interception frames
        if data.get("method") != "network.beforeRequestSent":
            continue
        params = data.get("params", {})
        request = params.get("request", {})
        url = request.get("url", "")
        rid = request.get("request")
        if not rid:
            continue
        # extract request headers once for range slicing
        req_headers = {}
        for h in request.get("headers", []):
            if isinstance(h.get("value"), dict):
                req_headers[h["name"]] = h["value"]["value"]
        # passthrough anything outside video hosts instantly
        if "googlevideo" not in url or not is_video_url(url):
            try:
                bidi.cmd("network.continueRequest", {"request": rid})
            except Exception:
                pass
            continue
        # resolve codec pair and content type
        codec, container = codec_for_url(url)
        content_type = f"video/{container}"
        # dry run only logs video urls
        if args.dry_run:
            print("video:", url[:100])
            try:
                bidi.cmd("network.continueRequest", {"request": rid})
            except Exception:
                pass
            continue
        # only h264 mp4 streams carry through v1 transcoder
        if codec != "h264":
            try:
                bidi.cmd("network.continueRequest", {"request": rid})
            except Exception:
                pass
            continue
        # serve cached segment when present
        if url in seg_cache:
            part, status, headers = slice_range(seg_cache[url], req_headers, content_type)
            try:
                bidi.cmd(
                    "network.provideResponse",
                    {
                        "request": rid,
                        "statusCode": status,
                        "headers": bidi_headers(headers),
                        "body": {"type": "base64", "value": base64.b64encode(part).decode()},
                    },
                )
            except Exception as ex:
                print("cached fulfill failed:", str(ex)[:100])
            continue
        # fetch original bytes from origin
        status, headers, body = fetch_original(url, req_headers)
        # passthrough on fetch failure
        if not status or not body:
            try:
                bidi.cmd("network.continueRequest", {"request": rid})
            except Exception:
                pass
            continue
        # split init and media parts
        init_part, media_part = boxes.split_init_media(body)
        # derive stream key without range for init cache
        stream_key = url.split("&sq=")[0].split("&range=")[0]
        try:
            if not media_part:
                # init request, build fresh init at target size
                if stream_key not in init_cache:
                    # probe original dims from init bytes
                    probe = subprocess.run(
                        ["ffprobe", "-v", "error", "-show_entries", "stream=width,height",
                         "-of", "csv", "-"],
                        input=body,
                        capture_output=True,
                    )
                    w = h = 0
                    for line in probe.stdout.decode(errors="replace").splitlines():
                        parts = line.strip().split(",")
                        if len(parts) >= 3 and parts[0] == "stream":
                            w, h = int(parts[1]), int(parts[2])
                            break
                    if not w or not h:
                        raise RuntimeError("init probe failed")
                    init_cache[stream_key] = transcode.build_init(
                        codec, w * args.scale, h * args.scale
                    )
                full_body = init_cache[stream_key]
            else:
                # media request, transcode with cached init
                if stream_key not in init_cache:
                    # learn original init on first media hit
                    init_cache[stream_key] = init_part
                full_body = transcode.transcode_segment(
                    media_part, init_cache[stream_key], args.scale, get_vsr()
                )
        except Exception as ex:
            print("transcode failed, passing through:", str(ex)[:120])
            try:
                bidi.cmd("network.continueRequest", {"request": rid})
            except Exception:
                pass
            continue
        # cache full body and fulfill sliced range
        if len(seg_cache) >= CACHE_LIMIT:
            seg_cache.pop(next(iter(seg_cache)))
        seg_cache[url] = full_body
        part, out_status, out_headers = slice_range(full_body, req_headers, content_type)
        try:
            bidi.cmd(
                "network.provideResponse",
                {
                    "request": rid,
                    "statusCode": out_status,
                    "headers": bidi_headers(out_headers),
                    "body": {"type": "base64", "value": base64.b64encode(part).decode()},
                },
            )
            print(f"served {len(part)}b for {url[:80]}")
        except Exception as ex:
            print("fulfill failed:", str(ex)[:100])


if __name__ == "__main__":
    sys.exit(main())
