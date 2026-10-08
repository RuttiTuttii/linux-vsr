# linux-vsr: native video super resolution layer for linux

linux-vsr is a lightweight graphics layer and driver shim that enables automatic hardware video super resolution (analogous to nvidia rtx video super resolution) across linux applications, including zen browser, mozilla firefox, chromium, and video players.

it intercepts opengl and egl shader compilation on the fly and upgrades naive bilinear video scaling filters to amd fidelityfx contrast adaptive sharpening (cas), edge-adaptive spatial upsampling (easu), or an experimental directional sharpen pass directly inside the gpu fragment pipeline without browser modifications or frame copies. The launcher selects Chromium's ANGLE/OpenGL path by default so Chromium, Brave, Firefox, and Zen can use the same hook; set `VSR_CHROMIUM_GL=0` to keep Chromium on its default backend.

---

### key highlights

* zero overhead (0.02 ms per frame): upscaler runs directly inside the browser's existing video compositor fragment shader, with zero additional fbos, zero blits, and zero memory copies.
* runtime hot reload: updates sharpness and upscaler modes on the fly via monotonic config file polling without restarting the browser.
* corner badge watermark: baked-in diagnostic indicator to visually verify active shader patching.
* modular monolith in pure c: clean separation of concerns, defensive arithmetic and memory guards (`safety`), internal symbol hiding (`-fvisibility=hidden`).
* vendor-aware browser path: automatic DRM detection for AMD, NVIDIA, and generic GPUs with an explicit `VSR_BACKEND` override.
* full cli tool suite: automated sandbox-bypassing launcher, runtime control cli with profiles, and an interactive shader dump wizard.

---

### architecture

the project is structured as a modular monolith in c99:

```
linux-vsr/
├── include/vsr/
│   ├── config.h      # configuration loading, ini file parser and profiles
│   ├── hook.h        # opengl, egl and dlsym symbol interception
│   ├── logger.h      # rate-limited diagnostic logging subsystem
│   ├── patcher.h     # glsl syntax inspection and luma sampler replacement
│   ├── safety.h      # checked arithmetic, memory limits and float clamping
│   └── shaders.h     # embedded glsl cas and easu shader generators
├── src/
│   ├── config.c      # configuration state, validation and serialization
│   ├── hook.c        # glshadersource, glcompileshader, eglgetprocaddress hooks
│   ├── logger.c      # terminal logging formatters
│   ├── main.c        # library constructor and destructor lifecycle
│   ├── patcher.c     # paren-aware token matching and string replacement
│   ├── safety.c      # checked addition/multiplication and safe allocations
│   └── shaders.c     # glsl shader code generation pinned to c numeric locale
├── tests/
│   ├── test_config.c # unit tests for environment and ini file parsing
│   ├── test_patcher.c# unit tests for shader identification and injection
│   ├── test_runner.c # test suite runner
│   ├── test_safety.c # unit tests for arithmetic overflows and boundaries
│   └── test_shaders.c# unit tests for cas and easu shader generators
├── bin/
│   ├── linux-vsr     # universal application launcher and gdb attach script
│   ├── linux-vsr-ctl # cli utility for runtime parameter tuning and profiles
│   └── linux-vsr-setup # interactive shader dump wizard and desktop installer
├── shaders/
│   ├── cas.glsl      # reference amd fidelityfx cas shader
│   └── fsr_easu.glsl # reference directional spatial filter shader
└── Makefile          # build system with strict flags (-Wall -Wextra -Werror)
```

---

### pipeline dataflow

```
video bitstream (h.264 / vp9 / av1)
        │
        ▼
hardware decoder (nvidia nvdec via libva-nvidia-driver)
        │  direct zero-copy in gpu vram (nv12 / p010)
        ▼
browser compositor (webrender / opengl / egl)
        │
        ▼
linux-vsr shim (libvsr.so via ld_preload)
        │  intercepts glshadersource
        │  replaces bilinear luma sampling with fidelityfx cas / easu
        ▼
gpu execution (nvidia rtx / shaders)
        │  0.02 ms per frame processing latency
        ▼
display surface (wayland / x11)
```

---

### building

requirements:
* gcc or clang
* make
* opengl / egl development headers

to build the shared library:

```bash
make -j$(nproc)
```

compilation finishes in less than 0.1 seconds and produces `libvsr.so`.

---

### testing

the codebase includes automated unit tests covering string transformations, chunk assembly, target shader recognition, defensive limits, and configuration parsing.

to run the test suite:

```bash
make test
```

expected output:

```text
running linux-vsr unit test suite...
  [pass] config module tests
  [pass] patcher module tests
  [pass] safety module tests
  [pass] shaders module tests
all unit tests completed successfully
```

---

### tools and usage

#### 1. universal launcher (`linux-vsr`)

automatically discovers installed browsers, disables gecko sandbox restrictions for child gpu processes, and preloads the library:

```bash
# launch zen browser (default)
./bin/linux-vsr

# launch standard firefox
./bin/linux-vsr firefox

# launch chromium
./bin/linux-vsr chromium

# launch mpv player
./bin/linux-vsr mpv https://example.com/video.mp4

# inject into running process via gdb
./bin/linux-vsr attach <PID>
```

#### 2. runtime control (`linux-vsr-ctl`)

allows adjusting settings and switching profiles without restarting running browsers:

```bash
# interactive terminal menu
./bin/linux-vsr-ctl menu

# adjust sharpness (0.0 to 0.5)
./bin/linux-vsr-ctl sharpness 0.30

# switch upscaler mode (cas / easu / directional / off)
./bin/linux-vsr-ctl mode cas

# toggle corner badge watermark
./bin/linux-vsr-ctl watermark off

# save and switch profiles
./bin/linux-vsr-ctl save cinema
./bin/linux-vsr-ctl use cinema
```

#### 3. setup wizard (`linux-vsr-setup`)

interactive tool to capture shader dumps, verify injection status, and install desktop entries:

```bash
# run interactive verification wizard
./bin/linux-vsr-setup

# install desktop entry for application menu
./bin/linux-vsr-setup --install-desktop zen-browser
```

---

### configuration

settings are stored in `~/.config/linux-vsr/config.ini` and can also be overridden with environment variables:

| setting / variable | default | description |
|---|---|---|
| `enable` / `VSR_ENABLE` | `1` | toggle upscaling (`1` to enable, `0` to bypass) |
| `mode` / `VSR_MODE` | `cas` | upscaler algorithm (`cas`, `easu`, `directional`, `off`); `directional` is an experimental vendor-neutral GLSL sharpen filter |
| `backend` / `VSR_BACKEND` | `auto` | hardware path (`auto`, `amd`, `nvidia`, `generic`) |
| `sharpness` / `VSR_SHARPNESS` | `0.15` | adaptive sharpening strength (`0.0` to `0.50`) |
| `debug` / `VSR_DEBUG` | `1` | output diagnostic messages to stderr |
| `watermark` / `VSR_WATERMARK` | `1` | display corner badge verification indicator |
| `watermark_opacity` / `VSR_WATERMARK_OPACITY` | `0.45` | opacity of corner indicator (`0.05` to `1.0`) |
| `watermark_size` / `VSR_WATERMARK_SIZE` | `0.06` | relative frame size fraction of indicator |
| `model` / `VSR_MODEL` | `""` | optional path to tensorrt engine |
| `VSR_CONFIG` | `~/.config/linux-vsr/config.ini` | override configuration file path |

example:

```bash
VSR_MODE=cas VSR_SHARPNESS=0.30 ./bin/linux-vsr

# force the detected hardware path for diagnostics or A/B testing
VSR_BACKEND=nvidia ./bin/linux-vsr
```

---

### known issues and limitations

* **browser sandboxing (security trade-off):** injecting via `LD_PRELOAD` into gecko gpu/rdd child processes requires disabling process sandboxes (`MOZ_DISABLE_*_SANDBOX=1`). this is an intentional trade-off required for shader interception.
* **multi-instance remote forward:** if zen browser or firefox is already running, launching `./bin/linux-vsr` without profile isolation forwards to the existing instance without preloading `libvsr.so`. you must either launch the browser with the wrapper initially or attach to the running pid via `./bin/linux-vsr attach <PID>`.
* **browser backend selection:** Firefox and Zen use their native WebRender/OpenGL path. The launcher starts Chromium-family browsers with ANGLE/OpenGL on Xwayland so the same GLSL hook is reached; pure Vulkan/Graphite remains a separate path.
* **no in-video interactive gui:** interactive buttons and overlays cannot be drawn from the `glShaderSource` compilation layer without separate render passes. all live tuning is handled via the `linux-vsr-ctl` cli tool.
* **vulkan backend bypass:** if the browser is forced to run on pure vulkan, opengl/egl interception is bypassed. support for vulkan requires hooking `vkCreateShaderModule` with spir-v byte patching.
* **nvidia and amd browser path:** the current native hook applies the GLSL fragment path on both vendors and records the resolved DRM backend. NVIDIA's proprietary RTX Video Super Resolution AI path is a separate compute/SDK integration; `VSR_BACKEND=nvidia` does not claim to emulate that proprietary model.
* **ai super resolution status:** compiled tensorrt `.engine` models for rtx 5070 are prepared, but direct in-pipeline cuda-gl interop inference inside the hook is currently under development.

---

### license

mit license - see [LICENSE](LICENSE) for details.
