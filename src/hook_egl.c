#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <unistd.h>

typedef void (*PFNGLSHADERSOURCEPROC)(unsigned int shader, int count, const char *const *string, const int *length);
typedef void (*PFNGLCOMPILESHADERPROC)(unsigned int shader);
typedef void* (*PFNEGLGETPROCADDRESSPROC)(const char *procname);

static PFNGLSHADERSOURCEPROC real_glShaderSource = NULL;
static PFNGLCOMPILESHADERPROC real_glCompileShader = NULL;
static PFNEGLGETPROCADDRESSPROC real_eglGetProcAddress = NULL;

static float g_sharpness = 0.22f;
static bool g_debug = true;
static bool g_enabled = true;

static void init_hooks(void) {
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

static char* replace_string(const char *orig, const char *rep, const char *with) {
    if (!orig || !rep) return NULL;
    size_t len_rep = strlen(rep);
    if (len_rep == 0) return NULL;
    if (!with) with = "";
    size_t len_with = strlen(with);

    const char *ins = orig;
    int count = 0;
    while ((ins = strstr(ins, rep)) != NULL) {
        count++;
        ins += len_rep;
    }

    size_t new_len = strlen(orig) + (len_with - len_rep) * count + 1;
    char *result = malloc(new_len);
    if (!result) return NULL;

    char *tmp = result;
    ins = orig;
    while (count--) {
        const char *found = strstr(ins, rep);
        size_t len_front = found - ins;
        memcpy(tmp, ins, len_front);
        tmp += len_front;
        memcpy(tmp, with, len_with);
        tmp += len_with;
        ins = found + len_rep;
    }
    strcpy(tmp, ins);
    return result;
}

__attribute__((visibility("default")))
void glShaderSource(unsigned int shader, int count, const char *const *string, const int *length) {
    init_hooks();

    if (!g_enabled) {
        if (real_glShaderSource) real_glShaderSource(shader, count, string, length);
        return;
    }

    size_t total_len = 0;
    for (int i = 0; i < count; i++) {
        if (length && length[i] > 0) {
            total_len += (size_t)length[i];
        } else if (string[i]) {
            total_len += strlen(string[i]);
        }
    }

    if (total_len == 0) {
        if (real_glShaderSource) real_glShaderSource(shader, count, string, length);
        return;
    }

    char *combined = malloc(total_len + 1);
    if (!combined) {
        if (real_glShaderSource) real_glShaderSource(shader, count, string, length);
        return;
    }

    char *dst = combined;
    for (int i = 0; i < count; i++) {
        size_t l = (length && length[i] > 0) ? (size_t)length[i] : (string[i] ? strlen(string[i]) : 0);
        if (string[i] && l > 0) {
            memcpy(dst, string[i], l);
            dst += l;
        }
    }
    *dst = '\0';

    // Check if this is the WebRender YUV video shader
    bool is_yuv_shader = (strstr(combined, "sample_yuv") != NULL) &&
                         (strstr(combined, "TEX_SAMPLE(sColor0, uv_y).r") != NULL);

    if (is_yuv_shader) {
        if (g_debug) {
            fprintf(stderr, "\033[1;32m[ZEN-VSR]\033[0m Intercepted WebRender YUV shader! Injecting FSR/CAS Native Upscaler (sharpness: %.2f)...\n", g_sharpness);
        }

        char shader_func[2048];
        snprintf(shader_func, sizeof(shader_func),
            "\n// === ZEN-VSR: Contrast Adaptive Sharpening (FidelityFX CAS) ===\n"
            "float sample_luma_cas(sampler2D tex, vec2 uv, vec4 bounds) {\n"
            "    vec2 size = vec2(textureSize(tex, 0));\n"
            "    vec2 texel = 1.0 / max(size, vec2(1.0));\n"
            "    float e = texture(tex, uv).r;\n"
            "    float b = texture(tex, clamp(uv + vec2(0.0, -texel.y), bounds.xy, bounds.zw)).r;\n"
            "    float d = texture(tex, clamp(uv + vec2(-texel.x, 0.0), bounds.xy, bounds.zw)).r;\n"
            "    float f = texture(tex, clamp(uv + vec2( texel.x, 0.0), bounds.xy, bounds.zw)).r;\n"
            "    float h = texture(tex, clamp(uv + vec2(0.0,  texel.y), bounds.xy, bounds.zw)).r;\n"
            "    float a = texture(tex, clamp(uv + vec2(-texel.x, -texel.y), bounds.xy, bounds.zw)).r;\n"
            "    float c = texture(tex, clamp(uv + vec2( texel.x, -texel.y), bounds.xy, bounds.zw)).r;\n"
            "    float g = texture(tex, clamp(uv + vec2(-texel.x,  texel.y), bounds.xy, bounds.zw)).r;\n"
            "    float i = texture(tex, clamp(uv + vec2( texel.x,  texel.y), bounds.xy, bounds.zw)).r;\n"
            "    float mn = min(min(min(d, e), min(f, b)), h);\n"
            "    float mn2 = min(min(min(mn, a), min(c, g)), i);\n"
            "    mn = mn + mn2;\n"
            "    float mx = max(max(max(d, e), max(f, b)), h);\n"
            "    float mx2 = max(max(max(mx, a), max(c, g)), i);\n"
            "    mx = mx + mx2;\n"
            "    float amp = clamp(min(mn, 2.0 - mx) / max(mx, 0.001), 0.0, 1.0);\n"
            "    float w = -sqrt(amp) * %.4ff;\n"
            "    float res = (b + d + f + h) * w + e;\n"
            "    return clamp(res / (4.0 * w + 1.0), 0.0, 1.0);\n"
            "}\n"
            "// === END ZEN-VSR ===\n\n",
            g_sharpness
        );

        size_t inject_len = strlen(shader_func) + strlen("vec4 sample_yuv(") + 1;
        char *inject_str = malloc(inject_len);
        if (inject_str) {
            snprintf(inject_str, inject_len, "%svec4 sample_yuv(", shader_func);
            char *with_func = replace_string(combined, "vec4 sample_yuv(", inject_str);
            free(inject_str);

            if (with_func) {
                char *final_src = replace_string(with_func,
                    "ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;",
                    "ycbcr_sample.x = sample_luma_cas(sColor0, uv_y, uv_bounds_y);");
                free(with_func);

                if (final_src) {
                    const char *src_ptr = final_src;
                    int new_len = (int)strlen(final_src);
                    if (real_glShaderSource) {
                        real_glShaderSource(shader, 1, &src_ptr, &new_len);
                    }
                    free(final_src);
                    free(combined);
                    if (g_debug) {
                        fprintf(stderr, "\033[1;32m[ZEN-VSR]\033[0m Successfully hooked WebRender video pipeline!\n");
                    }
                    return;
                }
            }
        }
    }

    // Default passthrough
    if (real_glShaderSource) {
        const char *src_ptr = combined;
        int l = (int)total_len;
        real_glShaderSource(shader, 1, &src_ptr, &l);
    }
    free(combined);
}

__attribute__((visibility("default")))
void glCompileShader(unsigned int shader) {
    init_hooks();
    if (real_glCompileShader) {
        real_glCompileShader(shader);
    }
}

__attribute__((visibility("default")))
void* eglGetProcAddress(const char *procname) {
    init_hooks();
    if (procname) {
        if (strcmp(procname, "glShaderSource") == 0) {
            return (void*)glShaderSource;
        }
        if (strcmp(procname, "glCompileShader") == 0) {
            return (void*)glCompileShader;
        }
    }
    if (real_eglGetProcAddress) {
        return real_eglGetProcAddress(procname);
    }
    return dlsym(RTLD_NEXT, procname);
}

__attribute__((constructor))
static void zen_vsr_init(void) {
    const char *sharp_env = getenv("ZEN_VSR_SHARPNESS");
    if (sharp_env) {
        float val = strtof(sharp_env, NULL);
        if (val >= 0.0f && val <= 0.5f) {
            g_sharpness = val;
        }
    }

    const char *debug_env = getenv("ZEN_VSR_DEBUG");
    if (debug_env && strcmp(debug_env, "0") == 0) {
        g_debug = false;
    }

    const char *enable_env = getenv("ZEN_VSR_ENABLE");
    if (enable_env && strcmp(enable_env, "0") == 0) {
        g_enabled = false;
    }

    if (g_debug && g_enabled) {
        fprintf(stderr, "\033[1;36m[ZEN-VSR]\033[0m Native Video Super Resolution Hook Loaded (PID: %d, Sharpness: %.2f)\n", getpid(), g_sharpness);
    }
    init_hooks();
}
