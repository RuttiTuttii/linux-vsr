# linux-vsr design notes

this document records the target architecture, the current boundary of the
implementation, and the experiments required before a feature is called
browser-ready. It is more concrete than a wish list: every larger idea has an
entry point, a fallback, and an acceptance signal.

## product target

linux-vsr should make video enhancement appear automatically when a supported
browser is opened through the launcher. The browser keeps owning decoding,
audio, seeking, subtitles, color management, and presentation. linux-vsr
changes only the video scaling stage and can return to the original pipeline
without restarting the browser.

the target has three levels:

1. **shader level:** low-latency spatial enhancement inside the existing GPU
   compositor, without copying frames through the CPU;
2. **compute level:** a proper Vulkan/GL compute pass for browsers without a
   patchable GLSL fragment shader;
3. **AI level:** an optional NVIDIA VFX SDK or other vendor runtime connected
   through a separate GPU-owned frame path. A detected NVIDIA card never means
   that AI VSR is active.

the first level is the reliable baseline. The second and third levels are
additive: when they are missing or fail, the browser keeps playing with the
original compositor or the shader fallback.

## current status

| path | current state | next proof |
|---|---|---|
| Firefox/Zen WebRender + OpenGL | verified with local video and hit-log | add more browser release fixtures |
| AMD OpenGL | same GLSL path, DRM vendor detected | validate on a real AMD machine |
| NVIDIA OpenGL | verified on RTX 5070, backend recorded | measure frame cost and HDR behavior |
| Chromium/Brave launcher | ANGLE/OpenGL route is available | collect and patch Skia/ANGLE video shaders |
| pure Vulkan | safe SPIR-V parser only | implement an official Vulkan layer |
| NVIDIA AI VFX | separate neural runtime for mpv | connect GPU frames without browser copies |
| browser test stand | local MP4, isolated profile, JSON report | add more browser matrix jobs |

the browser stand is the source of truth for a local integration claim:

    python3 tests/browser_stand.py --browser /usr/bin/zen-browser \
      --seconds 12 --output /tmp/linux-vsr-browser-stand

it creates a short 4:2:0 MP4 with ffmpeg, opens it from a local HTML page,
launches an isolated profile, and writes report.json, browser.log, shader
dumps, compile errors, and a per-run hit-log. The stand never reuses the
global /tmp/vsr_hits.log, so an old successful run cannot create a new false
positive. The strict option is for a machine where the selected browser path
is already expected to produce a hit.

the report distinguishes:

- PASS_PATCHED: a target shader was patched and the hit-log contains this run;
- FAIL_COMPILE_ERRORS: the driver rejected a generated shader;
- OBSERVED_VIDEO_NO_HIT: a video shader was seen but the patcher did not match;
- OBSERVED_SHADERS_NO_VIDEO_MARKER: graphics started but no video marker was seen;
- NO_SHADER_DUMPS: the browser did not reach the intercepted API.

## layers and responsibilities

### launcher

bin/linux-vsr owns process setup, config location, browser selection, and
child-process environment. It must not contain shader policy. The launcher
preloads libvsr.so, passes explicit user overrides through, and gives
Chromium-family browsers an opt-out ANGLE/OpenGL route with
VSR_CHROMIUM_GL=0.

Firefox-family sandboxes currently need the existing trade-off so the library
reaches GPU/content children. A future unsafe-browser-hook option should make
that trade-off visible and keep the safe default untouched for non-browser
commands.

### hook

libvsr.so resolves real GL entry points, combines source chunks, checks whether
a fragment shader resembles a video YUV pass, and forwards either the original
source or a bounded replacement. It must remain fail-open:

- missing symbols forward or skip;
- malformed source is never rewritten;
- unknown layouts are forwarded;
- allocation and output limits are enforced;
- compile failures are logged and do not stop the browser.

diagnostic locations are configurable per run through VSR_DUMP_DIR,
VSR_HIT_LOG, and VSR_COMPILE_LOG. This is required for parallel browser tests
and for debugging a running desktop without corrupting another run's evidence.

### patcher

the patcher should operate on signatures, not browser version numbers. Each
signature has a positive video marker, an exact luma sample anchor, a fragment
stage check, a replacement preserving texture bounds and coordinate names, and
a regression fixture taken from a real dump.

every new signature needs a negative fixture. A broad substring match that can
touch a color-correction or UI shader is a regression even when the generated
GLSL compiles.

### shader generator

generators emit bounded GLSL using a C numeric locale. cas is the established
adaptive sharpen path, easu is the directional spatial path, and directional
is an experimental vendor-neutral sharpen filter. The directional name is
intentional: it is not a claim that a short fragment function is the complete
NVIDIA NIS SDK implementation.

the full NVIDIA Image Scaling SDK is a compute-shader integration with
coefficients, configuration buffers, dispatch geometry, and HDR mode. Its
official GLSL sources are a possible future backend, but copying the name into
the mode string is not a substitute for integrating those resources.

## vendor contract

VSR_BACKEND=auto|amd|nvidia|generic has two meanings:

- selection tells policy which implementation is allowed;
- detection records which DRM vendor was actually found.

the current code implements detection and records it in the hit-log. It does
not pretend that the same GLSL function is NVIDIA RTX AI or an AMD proprietary
driver feature.

the next backend contract should expose capabilities:

    vendor=nvidia
    glsl_hook=available
    vulkan_layer=missing
    vfx_ai=missing
    dmabuf_interop=unknown

the launcher and CLI can then report why a mode was selected or downgraded.
auto should choose the fastest verified path, not the most ambitious one.

## browser strategy

### Firefox and Zen

keep the current WebRender/OpenGL path as the reference implementation. The
local fixture stand demonstrates the desired loop: decode a local YUV clip,
compile a YUV fragment, patch it, and record one hit without network traffic.

future work is to collect dumps from stable and nightly releases, add NV12/P010
and both texture-coordinate orientations, test rotation and crop, and verify
hot reload while the clip is playing.

### Chromium and derivatives

there are three choices:

1. **ANGLE/OpenGL route.** Lowest effort and reuses the current hook. The
   launcher already exposes it. The missing work is a clean local video dump
   and Skia/ANGLE signatures.
2. **ANGLE/Vulkan or Graphite layer.** More native on Wayland, but requires a
   Vulkan layer or a browser-supported post-process hook.
3. **network relay or extension.** Useful for neural experiments, but it
   changes bytes or replaces the player and is not a native compositor path.
   Keep it optional, never as the default VSR claim.

the recommended order is 1, then 2. A Chromium result is accepted only when
the stand reports a current-run hit and zero compile errors.

### pure Vulkan

the previous raw vkGetInstanceProcAddr interposition crashed Chromium and is
not an acceptable foundation. The safe plan is an official Vulkan layer:

1. publish a layer manifest with a unique layer name and library path;
2. implement instance/device dispatch tables by following the loader chain;
3. intercept vkCreateShaderModule only after dispatch is initialized;
4. parse SPIR-V header, bounds, execution model, and decorations defensively;
5. pass unknown modules byte-for-byte unchanged;
6. initially log fragment modules without rewriting them;
7. add a transformation only with a real Chromium module fixture and a
   validation pass through spirv-val or the driver;
8. provide VSR_VULKAN_LAYER=0 as a complete opt-out.

SPIR-V patching is not string replacement. A transform may need to identify
sampled YUV images, preserve descriptor bindings, add a helper, and rewrite
the final luma value. If this cannot be proven, the module is forwarded.

## NVIDIA AI bridge

NVIDIA's VFX documentation describes Linux VSR as a GPU-buffer filter with
quality, denoise, deblur, and high-bitrate modes. The existing neural runtime
is a useful starting point because it loads the SDK lazily and keeps a model
in VRAM for mpv.

the browser bridge should not call AI inference from glShaderSource. A
practical design is:

    browser video texture
            |
            +-- GL/Vulkan interop or dmabuf export
            v
    vsr-ai helper process
            |  persistent VFX session, bounded queue, CUDA stream
            v
    GPU output texture / dmabuf
            |
            v
    browser compositor

required pieces are frame ownership and fences, one persistent session per GPU
process, a one or two frame queue, NV12/P010/RGBA/BGRA negotiation, shader
fallback, and a licensing note because the NVIDIA runtime is not MIT code.

the first useful milestone is a standalone dmabuf-to-VFX probe. Only after it
processes a synthetic frame and returns a fence should it be connected to a
browser. Until then, linux-vsr-play remains the honest AI entry point.

## AMD path

AMD support should stay vendor-neutral where possible. CAS/FSR-style GLSL is
portable and should be validated on AMD hardware rather than selected only by
string name. A future AMD path can use Vulkan compute, subgroup and FP16
checks, VAAPI/DMABUF negotiation, optional FidelityFX sources, and per-GPU
tuning after timestamp measurements.

an optional AMD component must never break fallback. generic remains valid for
the same fragment shader.

## performance and quality

replace fixed latency claims with measurements: GPU timestamps, frame time
p50/p95/p99, dropped and late frames, dimensions and pixel format, compile time,
recompile count, VRAM, and AI queue depth.

quality fixtures should include text on gradients, diagonal lines, thin UI
borders, film grain, compression blocks, dark scenes, HDR/PQ, and chroma edges.

the report should store config, vendor, browser build, driver string, mode, and
commit SHA. Images belong in ignored output; summaries belong in CI artifacts.

## security and failure policy

LD_PRELOAD and sandbox changes are high-impact process controls. The launcher
must show browser and backend before exec, preserve explicit opt-outs, avoid
changes for unrelated applications, use private 0700 diagnostic directories,
write config atomically, never execute URL or shader contents as shell code,
and fail open to the original browser pipeline.

AI and relay connectors need their own opt-in switch. A network relay must
validate HTTPS googlevideo origins, preserve range semantics, and never become
the hidden default for ordinary browsing.

## acceptance gates

a backend is browser-ready only when:

1. local fixture stand produces a current-run hit;
2. generated shader or module validates;
3. browser plays at least ten seconds without a crash;
4. compile-error log is empty;
5. off mode produces no hit and no rewrite;
6. malformed and non-video shaders remain byte-for-byte unchanged;
7. private diagnostic directories work;
8. performance is measured on the target GPU;
9. documentation states the exact limitation.

## implementation order

1. keep the fixture stand green and add stable Firefox/Zen fixtures;
2. capture Chromium ANGLE/OpenGL local-video dumps and add signatures;
3. add capability reporting and timestamp measurements;
4. implement a logging-only Vulkan layer;
5. validate one real Chromium SPIR-V module before a transform;
6. build the standalone NVIDIA dmabuf/VFX probe;
7. connect AI frames only behind explicit opt-in;
8. add AMD hardware validation and compare fallback quality;
9. package desktop install, uninstall, and versioned diagnostics.

## references

- https://docs.nvidia.com/maxine/vfx/1.2.0.0/Filters/VideoSuperResolution.html
- https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- https://github.com/KhronosGroup/Vulkan-Loader
