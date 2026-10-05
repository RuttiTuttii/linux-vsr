#define _GNU_SOURCE
#include "vsr/hook.h"
#include "vsr/config.h"
#include "vsr/logger.h"
#include "vsr/patcher.h"
#include "vsr/safety.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

// function pointer types for intercepted gl calls
typedef void (*PFNGLSHADERSOURCEPROC)(unsigned int shader, int count, const char *const *string, const int *length);
typedef void (*PFNGLCOMPILESHADERPROC)(unsigned int shader);
typedef void* (*PFNEGLGETPROCADDRESSPROC)(const char *procname);
typedef void* (*PFNGLXGETPROCADDRESSPROC)(const char *procname);

// cached original function pointers
static PFNGLSHADERSOURCEPROC real_glShaderSource = NULL;
static PFNGLCOMPILESHADERPROC real_glCompileShader = NULL;
static PFNEGLGETPROCADDRESSPROC real_eglGetProcAddress = NULL;
static PFNGLXGETPROCADDRESSPROC real_glXGetProcAddress = NULL;

// one-time init guard for thread safety
static pthread_once_t g_hook_once = PTHREAD_ONCE_INIT;

// bypass flag for driver probe helpers (never disturb capability detection)
static bool g_vsr_bypass = false;

// detect helper processes that must see pristine gl dispatch
static void vsr_hook_detect_bypass(void) {
    // read own command line safely
    FILE *fp = fopen("/proc/self/cmdline", "r");
    // handle unreadable cmdline gracefully
    if (!fp) {
        return;
    }
    // scan first 512 bytes for probe markers
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    // handle empty cmdline
    if (n == 0) {
        return;
    }
    // skip interposition inside gl capability probes
    if (strstr(buf, "glxtest") != NULL) {
        g_vsr_bypass = true;
    }
}

// cached real dlsym pointer for direct handle lookups
static void *(*real_dlsym_fn)(void *, const char *) = NULL;

// resolve real dlsym via versioned lookup (dlvsym itself is not hooked)
static void vsr_hook_resolve_dlsym(void) {
    // use raw dlvsym to bypass our own dlsym interposition
    if (!real_dlsym_fn) {
        real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
    // fallback to unversioned next lookup when versioned fails
    if (!real_dlsym_fn) {
        real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    }
}

// cached config file state for hot reload
static long long g_last_cfg_check_ms = 0;
static long long g_last_cfg_mtime = 0;
static char g_cfg_path[VSR_MAX_PATH_LEN] = {0};
static bool g_cfg_path_init = false;

// get monotonic time in milliseconds
static long long vsr_hook_now_ms(void) {
    // query monotonic clock
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    // convert to milliseconds
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

// poll config file for hot reload without restart
static void vsr_hook_maybe_reload(void) {
    // throttle checks to once per second
    long long now = vsr_hook_now_ms();
    if (now != 0 && g_last_cfg_check_ms != 0 && (now - g_last_cfg_check_ms) < 1000) {
        return;
    }
    // update throttle marker
    g_last_cfg_check_ms = now;
    // resolve config path once
    if (!g_cfg_path_init) {
        // query default path
        if (!vsr_config_default_path(g_cfg_path, sizeof(g_cfg_path))) {
            return;
        }
        // mark resolved
        g_cfg_path_init = true;
    }
    // stat config file
    struct stat st = {0};
    if (stat(g_cfg_path, &st) != 0) {
        return;
    }
    // compare mtime with cache
    long long mtime = (long long)st.st_mtime;
    if (g_last_cfg_mtime == 0) {
        // init baseline without reload
        g_last_cfg_mtime = mtime;
        return;
    }
    // reload when file changed
    if (mtime != g_last_cfg_mtime) {
        // update cache first to avoid loops
        g_last_cfg_mtime = mtime;
        // reload global config
        vsr_config_reload();
        // refresh logger verbosity
        vsr_config_t *cfg = vsr_config_get();
        if (cfg) {
            vsr_log_init(cfg->debug);
            vsr_log_info("vsr config hot-reloaded (mode: %s, sharpness: %.2f)", cfg->mode, (double)cfg->sharpness);
        }
    }
}

// cached explicit handles for system gl libraries
static void *g_egl_handle = NULL;
static void *g_gl_handle = NULL;

// force-load system gl libraries so late lookups always have a target
static void vsr_hook_force_gl_libs(void) {
    // dlopen is not hooked, safe to call here
    if (!g_egl_handle) {
        g_egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_gl_handle) {
        g_gl_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_gl_handle) {
        g_gl_handle = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    }
}

// resolve original function pointers using dlsym
static void vsr_hook_resolve(void) {
    // detect probe helpers before touching dispatch
    vsr_hook_detect_bypass();
    // resolve real lookup first to avoid recursion
    vsr_hook_resolve_dlsym();
    // fall back to direct next lookup when bootstrap failed
    void *(*lookup)(void *, const char *) = real_dlsym_fn;
    // handle bootstrap failure gracefully
    if (!lookup) {
        return;
    }
    // ensure system libraries are present for next-order search
    vsr_hook_force_gl_libs();
    // clear errors before resolving
    dlerror();
    // resolve base glShaderSource symbol
    real_glShaderSource = (PFNGLSHADERSOURCEPROC)lookup(RTLD_NEXT, "glShaderSource");
    // resolve compile symbol
    real_glCompileShader = (PFNGLCOMPILESHADERPROC)lookup(RTLD_NEXT, "glCompileShader");
    // resolve egl lookup symbol
    real_eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)lookup(RTLD_NEXT, "eglGetProcAddress");
    // resolve glx lookup symbol for x11 paths
    real_glXGetProcAddress = (PFNGLXGETPROCADDRESSPROC)lookup(RTLD_NEXT, "glXGetProcAddress");
    // fall back to explicit handles when next-order search missed
    if (!real_eglGetProcAddress && g_egl_handle) {
        real_eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)lookup(g_egl_handle, "eglGetProcAddress");
    }
    if (!real_glShaderSource && g_gl_handle) {
        real_glShaderSource = (PFNGLSHADERSOURCEPROC)lookup(g_gl_handle, "glShaderSource");
    }
    if (!real_glCompileShader && g_gl_handle) {
        real_glCompileShader = (PFNGLCOMPILESHADERPROC)lookup(g_gl_handle, "glCompileShader");
    }
    // ignore dlerror here, null pointers are handled per call
}

// retry missing pointers when libraries load lazily after first call
static void vsr_hook_retry_missing(void) {
    // skip when lookup itself unavailable
    if (!real_dlsym_fn) {
        return;
    }
    // ensure system libraries are present before retrying
    vsr_hook_force_gl_libs();
    // retry each missing entry, racy writes are idempotent
    if (!real_glShaderSource) {
        real_glShaderSource = (PFNGLSHADERSOURCEPROC)real_dlsym_fn(RTLD_NEXT, "glShaderSource");
        if (!real_glShaderSource && g_gl_handle) {
            real_glShaderSource = (PFNGLSHADERSOURCEPROC)real_dlsym_fn(g_gl_handle, "glShaderSource");
        }
    }
    if (!real_glCompileShader) {
        real_glCompileShader = (PFNGLCOMPILESHADERPROC)real_dlsym_fn(RTLD_NEXT, "glCompileShader");
        if (!real_glCompileShader && g_gl_handle) {
            real_glCompileShader = (PFNGLCOMPILESHADERPROC)real_dlsym_fn(g_gl_handle, "glCompileShader");
        }
    }
    if (!real_eglGetProcAddress) {
        real_eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)real_dlsym_fn(RTLD_NEXT, "eglGetProcAddress");
        if (!real_eglGetProcAddress && g_egl_handle) {
            real_eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)real_dlsym_fn(g_egl_handle, "eglGetProcAddress");
        }
    }
    if (!real_glXGetProcAddress) {
        real_glXGetProcAddress = (PFNGLXGETPROCADDRESSPROC)real_dlsym_fn(RTLD_NEXT, "glXGetProcAddress");
    }
}

// initialize original function pointers using dlsym
void vsr_hook_init(void) {
    // guard against reentrant init via dlopen constructors in the same thread
    static __thread int in_init = 0;
    if (in_init) {
        return;
    }
    in_init = 1;
    // run resolver exactly once across threads
    pthread_once(&g_hook_once, vsr_hook_resolve);
    in_init = 0;
}

// forward original shader source without modification
static void vsr_hook_forward_source(unsigned int shader, int count, const char *const *string, const int *length) {
    // validate cached pointer before call
    if (real_glShaderSource) {
        // forward to driver implementation
        real_glShaderSource(shader, count, string, length);
    }
}

// dump counters to avoid flooding disk
static int g_dump_count = 0;
static long g_shader_total = 0;

// dump interesting shaders to /tmp for diagnosis
static void vsr_hook_maybe_dump(unsigned int shader, const char *combined) {
    // check opt-in env once
    static int dump_enabled = -1;
    static int dump_all = -1;
    if (dump_enabled < 0) {
        // read env flag
        const char *env = getenv("VSR_DUMP");
        dump_enabled = (env && (env[0] == '1' || env[0] == 'y' || env[0] == 't')) ? 1 : 0;
        // read full dump flag
        const char *env_all = getenv("VSR_DUMP_ALL");
        dump_all = (env_all && (env_all[0] == '1' || env_all[0] == 'y' || env_all[0] == 't')) ? 1 : 0;
    }
    // count every intercepted call for stats
    long total = __atomic_add_fetch(&g_shader_total, 1, __ATOMIC_RELAXED);
    // skip file work when disabled
    if ((!dump_enabled && !dump_all) || !combined) {
        return;
    }
    // detect video markers
    bool has_video = strstr(combined, "ycbcr") || strstr(combined, "vUV_y")
        || strstr(combined, "sample_yuv") || strstr(combined, "sColor0");
    // in video-only mode skip non-video shaders
    if (!dump_all && !has_video) {
        // update stats file every 50 calls so empty dumps still leave trace
        if ((total % 50) == 0) {
            mkdir("/tmp/vsr_shaders", 0755);
            char spath[128] = {0};
            snprintf(spath, sizeof(spath), "/tmp/vsr_shaders/stats_%d.log", (int)getpid());
            FILE *sfp = fopen(spath, "w");
            if (sfp) {
                fprintf(sfp, "pid=%d total=%ld video=0 note=no-video-markers-yet\n", (int)getpid(), total);
                fclose(sfp);
            }
        }
        return;
    }
    // cap dumps per process
    int cap = dump_all ? 50 : 20;
    if (__atomic_fetch_add(&g_dump_count, 1, __ATOMIC_RELAXED) >= cap) {
        return;
    }
    // ensure dump directory exists
    mkdir("/tmp/vsr_shaders", 0755);
    // build dump path
    char path[128] = {0};
    snprintf(path, sizeof(path), "/tmp/vsr_shaders/shader_%d_%u.glsl", (int)getpid(), shader);
    // write combined source to file
    FILE *fp = fopen(path, "w");
    if (fp) {
        // write payload
        fwrite(combined, 1, vsr_safe_strlen(combined, VSR_MAX_SHADER_SIZE), fp);
        fclose(fp);
        vsr_log_info("dumped shader %u to %s (total=%ld video=%d)", shader, path, total, has_video ? 1 : 0);
    }
    // update stats file
    char spath[128] = {0};
    snprintf(spath, sizeof(spath), "/tmp/vsr_shaders/stats_%d.log", (int)getpid());
    FILE *sfp = fopen(spath, "w");
    if (sfp) {
        fprintf(sfp, "pid=%d total=%ld dumped=%d\n", (int)getpid(), total, g_dump_count);
        fclose(sfp);
    }
}

// snapshot config values safely for hook thread
static void vsr_hook_snapshot_config(bool *enabled, char *mode, size_t mode_len, float *sharpness, bool *watermark, float *opacity, float *size_frac) {
    // validate outputs
    if (!enabled || !mode || !sharpness || !watermark || !opacity || !size_frac) {
        return;
    }
    // read global singleton
    vsr_config_t *cfg = vsr_config_get();
    // handle null singleton defensively
    if (!cfg) {
        *enabled = false;
        return;
    }
    // copy scalar flags
    *enabled = cfg->enabled;
    *sharpness = vsr_safe_clamp_float(cfg->sharpness, 0.0f, 0.50f);
    *watermark = cfg->watermark_enabled;
    *opacity = vsr_safe_clamp_float(cfg->watermark_opacity, 0.05f, 1.0f);
    *size_frac = vsr_safe_clamp_float(cfg->watermark_size, 0.02f, 0.15f);
    // copy mode string safely
    snprintf(mode, mode_len, "%s", cfg->mode);
}

// intercepted glShaderSource function
__attribute__((visibility("default")))
void glShaderSource(unsigned int shader, int count, const char *const *string, const int *length) {
    // ensure function pointers are bound
    vsr_hook_init();
    // retry lazy libraries that loaded after first call
    vsr_hook_retry_missing();
    // in bypass mode forward directly without inspection
    if (g_vsr_bypass) {
        vsr_hook_forward_source(shader, count, string, length);
        return;
    }
    // poll config file for runtime changes
    vsr_hook_maybe_reload();
    // handle missing original gracefully
    if (!real_glShaderSource) {
        vsr_safety_set_error("glShaderSource original missing");
        return;
    }
    // validate shader handle
    if (shader == 0) {
        vsr_hook_forward_source(shader, count, string, length);
        return;
    }
    // snapshot configuration for this call
    bool enabled = false;
    char mode[VSR_MAX_MODE_LEN] = {0};
    float sharpness = 0.22f;
    bool watermark = false;
    float opacity = 0.45f;
    float size_frac = 0.06f;
    // copy config values
    vsr_hook_snapshot_config(&enabled, mode, sizeof(mode), &sharpness, &watermark, &opacity, &size_frac);
    // if vsr is disabled, forward call directly
    if (!enabled) {
        vsr_hook_forward_source(shader, count, string, length);
        return;
    }
    // handle off mode as passthrough
    if (strcmp(mode, "off") == 0) {
        vsr_hook_forward_source(shader, count, string, length);
        return;
    }
    // validate input array before combining
    if (count <= 0 || count > VSR_MAX_CHUNKS || !string) {
        vsr_hook_forward_source(shader, count, string, length);
        return;
    }
    // combine incoming shader source chunks
    size_t total_len = 0;
    char *combined = vsr_patcher_combine_chunks(count, string, length, &total_len);
    // handle combine failure by forwarding original
    if (!combined) {
        vsr_hook_forward_source(shader, count, string, length);
        return;
    }
    // dump video-like shaders when VSR_DUMP=1
    vsr_hook_maybe_dump(shader, combined);
    // inspect if shader matches webrender yuv video pipeline
    if (vsr_patcher_is_target_shader(combined)) {
        // log interception with rate limit
        vsr_log_info("intercepted video shader, injecting %s upscaler (sharpness: %.2f)", mode, (double)sharpness);
        // inject upscaler shader code
        char *patched = vsr_patcher_inject_upscaler_full(combined, mode, sharpness, watermark, opacity, size_frac);
        // handle successful patch
        if (patched) {
            // bound patched length
            size_t patched_len = vsr_safe_strlen(patched, VSR_MAX_OUTPUT_SIZE + 1);
            if (patched_len > 0 && patched_len <= VSR_MAX_OUTPUT_SIZE) {
                const char *src_ptr = patched;
                int new_len = (int)patched_len;
                // forward patched single chunk
                real_glShaderSource(shader, 1, &src_ptr, &new_len);
                // release buffers
                free(patched);
                free(combined);
                // log success in debug mode
                vsr_log_debug("patched video pipeline shader (%zu -> %zu bytes)", total_len, patched_len);
                return;
            }
            // release oversized patch
            free(patched);
        } else {
            // log injection skip in debug mode
            vsr_log_debug("shader matched but injection skipped: %s", vsr_safety_last_error());
        }
    }
    // pass unmodified original to preserve driver semantics
    vsr_hook_forward_source(shader, count, string, length);
    // release combine buffer
    free(combined);
}

// intercepted glShaderSourceARB for compatibility profiles
__attribute__((visibility("default")))
void glShaderSourceARB(unsigned int shader, int count, const char *const *string, const int *length) {
    // delegate to main implementation
    glShaderSource(shader, count, string, length);
}

// intercepted glCompileShader function
__attribute__((visibility("default")))
void glCompileShader(unsigned int shader) {
    // ensure function pointers are bound
    vsr_hook_init();
    // retry lazy libraries that loaded after first call
    vsr_hook_retry_missing();
    // invoke real implementation when available
    if (real_glCompileShader) {
        real_glCompileShader(shader);
    }
}

// intercepted eglGetProcAddress function for wayland and egl applications
__attribute__((visibility("default")))
void* eglGetProcAddress(const char *procname) {
    // ensure function pointers are bound
    vsr_hook_init();
    // retry lazy libraries that loaded after first call
    vsr_hook_retry_missing();
    // in bypass mode forward directly
    if (g_vsr_bypass && real_eglGetProcAddress) {
        return real_eglGetProcAddress(procname);
    }
    // validate input string with bound
    if (procname && vsr_safe_strlen(procname, 256) < 256) {
        // redirect glShaderSource resolution
        if (strcmp(procname, "glShaderSource") == 0) {
            return (void*)glShaderSource;
        }
        // redirect arb variant resolution
        if (strcmp(procname, "glShaderSourceARB") == 0) {
            return (void*)glShaderSourceARB;
        }
        // redirect glCompileShader resolution
        if (strcmp(procname, "glCompileShader") == 0) {
            return (void*)glCompileShader;
        }
    }
    // query original egl procedure address
    if (real_eglGetProcAddress) {
        return real_eglGetProcAddress(procname);
    }
    // fallback to direct symbol lookup
    if (procname) {
        return dlsym(RTLD_NEXT, procname);
    }
    return NULL;
}

// intercepted glXGetProcAddress for x11 paths
__attribute__((visibility("default")))
void* glXGetProcAddress(const char *procname) {
    // ensure function pointers are bound
    vsr_hook_init();
    // retry lazy libraries that loaded after first call
    vsr_hook_retry_missing();
    // in bypass mode forward directly
    if (g_vsr_bypass && real_glXGetProcAddress) {
        return real_glXGetProcAddress(procname);
    }
    // validate input string with bound
    if (procname && vsr_safe_strlen(procname, 256) < 256) {
        // redirect glShaderSource resolution
        if (strcmp(procname, "glShaderSource") == 0) {
            return (void*)glShaderSource;
        }
        // redirect arb variant resolution
        if (strcmp(procname, "glShaderSourceARB") == 0) {
            return (void*)glShaderSourceARB;
        }
        // redirect glCompileShader resolution
        if (strcmp(procname, "glCompileShader") == 0) {
            return (void*)glCompileShader;
        }
    }
    // query original glx procedure address
    if (real_glXGetProcAddress) {
        return real_glXGetProcAddress(procname);
    }
    // fallback to direct symbol lookup
    if (procname && real_dlsym_fn) {
        return real_dlsym_fn(RTLD_NEXT, procname);
    }
    return NULL;
}

// intercepted glXGetProcAddressARB alias for compatibility
__attribute__((visibility("default")))
void* glXGetProcAddressARB(const char *procname) {
    // delegate to main glx handler
    return glXGetProcAddress(procname);
}

// intercepted eglGetProcAddressKHR alias for wayland drivers
__attribute__((visibility("default")))
void* eglGetProcAddressKHR(const char *procname) {
    // delegate to main egl handler
    return eglGetProcAddress(procname);
}

// intercepted dlsym to catch direct libGL handle lookups
__attribute__((visibility("default")))
void* dlsym(void *handle, const char *symbol) {
    // lazily resolve real lookup via unhooked versioned symbol
    if (!real_dlsym_fn) {
        real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
    // fallback to newer version tag
    if (!real_dlsym_fn) {
        real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    }
    // handle missing resolver gracefully
    if (!real_dlsym_fn) {
        return NULL;
    }
    // in bypass mode forward everything untouched
    if (g_vsr_bypass) {
        return real_dlsym_fn(handle, symbol);
    }
    // ensure probe detection ran (no once here to avoid loader deadlock)
    static int bypass_decided = 0;
    if (!bypass_decided) {
        vsr_hook_detect_bypass();
        bypass_decided = 1;
    }
    // recheck bypass after detection
    if (g_vsr_bypass) {
        return real_dlsym_fn(handle, symbol);
    }
    // redirect shader entry points resolved via explicit handles
    if (strcmp(symbol, "glShaderSource") == 0) {
        return (void*)glShaderSource;
    }
    if (strcmp(symbol, "glShaderSourceARB") == 0) {
        return (void*)glShaderSourceARB;
    }
    if (strcmp(symbol, "glCompileShader") == 0) {
        return (void*)glCompileShader;
    }
    // redirect loader entry points so apps cannot bypass via real dispatch
    if (strcmp(symbol, "eglGetProcAddress") == 0) {
        return (void*)eglGetProcAddress;
    }
    if (strcmp(symbol, "eglGetProcAddressKHR") == 0) {
        return (void*)eglGetProcAddressKHR;
    }
    if (strcmp(symbol, "glXGetProcAddress") == 0) {
        return (void*)glXGetProcAddress;
    }
    if (strcmp(symbol, "glXGetProcAddressARB") == 0) {
        return (void*)glXGetProcAddressARB;
    }
    // forward all other lookups
    return real_dlsym_fn(handle, symbol);
}
