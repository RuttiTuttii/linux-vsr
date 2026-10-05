#define _GNU_SOURCE
#include "vsr/hook.h"
#include "vsr/config.h"
#include "vsr/logger.h"
#include "vsr/patcher.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef void (*PFNGLSHADERSOURCEPROC)(unsigned int shader, int count, const char *const *string, const int *length);
typedef void (*PFNGLCOMPILESHADERPROC)(unsigned int shader);
typedef void* (*PFNEGLGETPROCADDRESSPROC)(const char *procname);

static PFNGLSHADERSOURCEPROC real_glShaderSource = NULL;
static PFNGLCOMPILESHADERPROC real_glCompileShader = NULL;
static PFNEGLGETPROCADDRESSPROC real_eglGetProcAddress = NULL;

// initialize original function pointers using dlsym
void vsr_hook_init(void) {
    if (!real_glShaderSource) {
        real_glShaderSource = (PFNGLSHADERSOURCEPROC)dlsym(RTLD_NEXT, "glShaderSource");
    }
    if (!real_glCompileShader) {
        real_glCompileShader = (PFNGLCOMPILESHADERPROC)dlsym(RTLD_NEXT, "glCompileShader");
    }
    if (!real_eglGetProcAddress) {
        real_eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)dlsym(RTLD_NEXT, "eglGetProcAddress");
    }
}

// intercepted glShaderSource function
__attribute__((visibility("default")))
void glShaderSource(unsigned int shader, int count, const char *const *string, const int *length) {
    // ensure function pointers are bound
    vsr_hook_init();

    vsr_config_t *cfg = vsr_config_get();

    // if vsr is disabled, forward call directly
    if (!cfg->enabled) {
        if (real_glShaderSource) {
            real_glShaderSource(shader, count, string, length);
        }
        return;
    }

    // combine incoming shader source chunks
    size_t total_len = 0;
    char *combined = vsr_patcher_combine_chunks(count, string, length, &total_len);
    if (!combined) {
        if (real_glShaderSource) {
            real_glShaderSource(shader, count, string, length);
        }
        return;
    }

    // inspect if shader matches webrender yuv video pipeline
    if (vsr_patcher_is_target_shader(combined)) {
        vsr_log_info("intercepted video rendering shader, injecting hardware cas upscaler (sharpness: %.2f)", cfg->sharpness);

        // inject upscaler shader code
        char *patched = vsr_patcher_inject_upscaler(combined, cfg->sharpness);
        if (patched) {
            const char *src_ptr = patched;
            int new_len = (int)strlen(patched);

            if (real_glShaderSource) {
                real_glShaderSource(shader, 1, &src_ptr, &new_len);
            }

            free(patched);
            free(combined);
            vsr_log_debug("successfully hooked and patched video pipeline shader");
            return;
        }
    }

    // pass unmodified shader string to driver
    if (real_glShaderSource) {
        const char *src_ptr = combined;
        int l = (int)total_len;
        real_glShaderSource(shader, 1, &src_ptr, &l);
    }
    free(combined);
}

// intercepted glCompileShader function
__attribute__((visibility("default")))
void glCompileShader(unsigned int shader) {
    // ensure function pointers are bound
    vsr_hook_init();

    // invoke real implementation
    if (real_glCompileShader) {
        real_glCompileShader(shader);
    }
}

// intercepted eglGetProcAddress function for wayland and egl applications
__attribute__((visibility("default")))
void* eglGetProcAddress(const char *procname) {
    // ensure function pointers are bound
    vsr_hook_init();

    if (procname) {
        // redirect glShaderSource resolution
        if (strcmp(procname, "glShaderSource") == 0) {
            return (void*)glShaderSource;
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
    return dlsym(RTLD_NEXT, procname);
}
