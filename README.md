# linux-vsr: native video super resolution layer for linux

linux-vsr is a lightweight graphics layer and driver shim that enables automatic hardware video super resolution (analogous to nvidia rtx video super resolution) across linux applications, including zen browser, mozilla firefox, chromium, and video players.

it intercepts opengl and egl shader compilation on the fly and upgrades naive bilinear video scaling filters to amd fidelityfx contrast adaptive sharpening (cas) or edge-adaptive spatial upsampling (easu) directly inside the gpu fragment pipeline without browser modifications or overhead.

---

### key highlights

* zero overhead (0.02 ms per frame): upscaler runs directly inside the browser's existing video compositor fragment shader, with zero additional fbos, zero blits, and zero memory copies.
* runtime hot reload: updates sharpness and upscaler modes on the fly via monotonic config file polling without restarting the browser.
* corner badge watermark: baked-in diagnostic indicator to visually verify active shader patching.
* modular monolith in pure c: clean separation of concerns, defensive arithmetic and memory guards (`safety`), internal symbol hiding (`-fvisibility=hidden`).
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

# switch upscaler mode (cas / easu / off)
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
| `mode` / `VSR_MODE` | `cas` | upscaler algorithm (`cas`, `easu`, `off`) |
| `sharpness` / `VSR_SHARPNESS` | `0.22` | adaptive sharpening strength (`0.0` to `0.50`) |
| `debug` / `VSR_DEBUG` | `1` | output diagnostic messages to stderr |
| `watermark` / `VSR_WATERMARK` | `1` | display corner badge verification indicator |
| `watermark_opacity` / `VSR_WATERMARK_OPACITY` | `0.45` | opacity of corner indicator (`0.05` to `1.0`) |
| `watermark_size` / `VSR_WATERMARK_SIZE` | `0.06` | relative frame size fraction of indicator |
| `model` / `VSR_MODEL` | `""` | optional path to tensorrt engine |
| `VSR_CONFIG` | `~/.config/linux-vsr/config.ini` | override configuration file path |

example:

```bash
VSR_MODE=cas VSR_SHARPNESS=0.30 ./bin/linux-vsr
```
