#define _GNU_SOURCE
#include "vsr/hook.h"
#include "vsr/config.h"
#include "vsr/logger.h"
#include "vsr/patcher.h"
#include "vsr/safety.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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

// resolve original function pointers using dlsym
static void vsr_hook_resolve(void) {
    // clear errors before resolving
    dlerror();
    // resolve base glShaderSource symbol
    real_glShaderSource = (PFNGLSHADERSOURCEPROC)dlsym(RTLD_NEXT, "glShaderSource");
    // resolve compile symbol
    real_glCompileShader = (PFNGLCOMPILESHADERPROC)dlsym(RTLD_NEXT, "glCompileShader");
    // resolve egl lookup symbol
    real_eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)dlsym(RTLD_NEXT, "eglGetProcAddress");
    // resolve glx lookup symbol for x11 paths
    real_glXGetProcAddress = (PFNGLXGETPROCADDRESSPROC)dlsym(RTLD_NEXT, "glXGetProcAddress");
    // ignore dlerror here, null pointers are handled per call
}

// initialize original function pointers using dlsym
void vsr_hook_init(void) {
    // run resolver exactly once across threads
    pthread_once(&g_hook_once, vsr_hook_resolve);
}

// forward original shader source without modification
static void vsr_hook_forward_source(unsigned int shader, int count, const char *const *string, const int *length) {
    // validate cached pointer before call
    if (real_glShaderSource) {
        // forward to driver implementation
        real_glShaderSource(shader, count, string, length);
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
    strncpy(mode, cfg->mode, mode_len - 1);
    mode[mode_len - 1] = '\0';
}

// intercepted glShaderSource function
__attribute__((visibility("default")))
void glShaderSource(unsigned int shader, int count, const char *const *string, const int *length) {
    // ensure function pointers are bound
    vsr_hook_init();
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
    if (strncmp(mode, "off", sizeof(mode)) == 0) {
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
    if (procname) {
        return dlsym(RTLD_NEXT, procname);
    }
    return NULL;
}
