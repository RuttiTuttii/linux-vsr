#include "vsr/shaders.h"
#include "vsr/safety.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// internal buffer size for generated code
#define VSR_SHADER_BUF 8192

// build watermark glsl snippet into buffer
static int vsr_shader_append_watermark(char *buf, size_t buf_len, size_t offset, float opacity, float size_frac) {
    // validate buffer state
    if (!buf || offset >= buf_len) {
        return -1;
    }
    // clamp params defensively
    opacity = vsr_safe_clamp_float(opacity, 0.05f, 1.0f);
    size_frac = vsr_safe_clamp_float(size_frac, 0.02f, 0.15f);
    // format badge helper with baked constants
    int written = snprintf(buf + offset, buf_len - offset,
        "\n// === linux-vsr: corner badge (upscale indicator) ===\n"
        "float vsr_badge_mask(vec2 uv, vec4 bounds) {\n"
        "    vec2 ext = bounds.zw - bounds.xy;\n"
        "    ext = max(ext, vec2(1e-6));\n"
        "    vec2 rel = (uv - bounds.xy) / ext;\n"
        "    float margin = 0.015;\n"
        "    float bw = %.4ff;\n"
        "    float bh = %.4ff * 0.32;\n"
        "    vec2 lo = vec2(1.0 - margin - bw, 1.0 - margin - bh);\n"
        "    vec2 hi = vec2(1.0 - margin, 1.0 - margin);\n"
        "    if (rel.x < lo.x || rel.x > hi.x || rel.y < lo.y || rel.y > hi.y) { return 0.0; }\n"
        "    vec2 f = (rel - lo) / max(vec2(bw, bh), vec2(1e-6));\n"
        "    float bar = step(f.x, 0.22) + step(abs(f.x - 0.5), 0.11) + step(0.78, f.x);\n"
        "    bar = clamp(bar, 0.0, 1.0);\n"
        "    float edge = step(0.06, f.x) * step(f.x, 0.94) * step(0.12, f.y) * step(f.y, 0.88);\n"
        "    return clamp(bar * edge + 0.25 * (1.0 - edge), 0.0, 1.0);\n"
        "}\n"
        "float vsr_apply_badge(float luma, vec2 uv, vec4 bounds) {\n"
        "    float m = vsr_badge_mask(uv, bounds);\n"
        "    float stripe = 0.85 + 0.15 * step(0.5, fract((uv.x + uv.y) * 400.0));\n"
        "    float target = mix(1.0, 0.0, step(0.5, luma));\n"
        "    return mix(luma, target * stripe, m * %.4ff);\n"
        "}\n",
        (double)size_frac, (double)size_frac, (double)opacity);
    // validate snprintf result
    if (written <= 0 || (size_t)written >= buf_len - offset) {
        vsr_safety_set_error("watermark snippet truncated");
        return -1;
    }
    return written;
}

// allocate and generate cas glsl function with specified sharpness weight
char* vsr_shader_generate_cas(float sharpness) {
    // delegate to full generator with defaults
    return vsr_shader_generate_cas_full(sharpness, false, 0.45f, 0.06f);
}

// generate cas with watermark badge baked in
char* vsr_shader_generate_cas_full(float sharpness, bool watermark, float opacity, float size_frac) {
    // sanitize sharpness input
    sharpness = vsr_safe_clamp_float(sharpness, 0.0f, 0.50f);
    // sanitize badge params
    opacity = vsr_safe_clamp_float(opacity, 0.05f, 1.0f);
    size_frac = vsr_safe_clamp_float(size_frac, 0.02f, 0.15f);
    // allocate buffer for shader code
    char *buffer = (char *)vsr_safe_malloc(VSR_SHADER_BUF);
    // handle allocation failure
    if (!buffer) {
        return NULL;
    }
    // format base cas function
    int written = snprintf(buffer, VSR_SHADER_BUF,
        "\n// === linux-vsr: contrast adaptive sharpening (fidelityfx cas) ===\n"
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
        "    res = clamp(res / (4.0 * w + 1.0), 0.0, 1.0);\n",
        (double)sharpness);
    // validate snprintf result
    if (written <= 0 || (size_t)written >= VSR_SHADER_BUF) {
        free(buffer);
        vsr_safety_set_error("cas snippet truncated");
        return NULL;
    }
    // append watermark tail when enabled
    if (watermark) {
        // generate badge helpers
        int wm = vsr_shader_append_watermark(buffer, VSR_SHADER_BUF, (size_t)written, opacity, size_frac);
        // handle generation failure
        if (wm < 0) {
            free(buffer);
            return NULL;
        }
        written += wm;
        // append badge apply and function close
        int tail = snprintf(buffer + written, VSR_SHADER_BUF - (size_t)written,
            "    res = vsr_apply_badge(res, uv, bounds);\n"
            "    return res;\n"
            "}\n// === end linux-vsr ===\n\n");
        // validate tail write
        if (tail <= 0 || (size_t)tail >= VSR_SHADER_BUF - (size_t)written) {
            free(buffer);
            vsr_safety_set_error("cas tail truncated");
            return NULL;
        }
    } else {
        // append plain return when badge disabled
        int tail = snprintf(buffer + written, VSR_SHADER_BUF - (size_t)written,
            "    return res;\n"
            "}\n// === end linux-vsr ===\n\n");
        // validate tail write
        if (tail <= 0 || (size_t)tail >= VSR_SHADER_BUF - (size_t)written) {
            free(buffer);
            vsr_safety_set_error("cas tail truncated");
            return NULL;
        }
    }
    return buffer;
}

// generate easu with watermark badge baked in
char* vsr_shader_generate_easu_full(bool watermark, float opacity, float size_frac) {
    // sanitize badge params
    opacity = vsr_safe_clamp_float(opacity, 0.05f, 1.0f);
    size_frac = vsr_safe_clamp_float(size_frac, 0.02f, 0.15f);
    // allocate buffer for shader code
    char *buffer = (char *)vsr_safe_malloc(VSR_SHADER_BUF);
    // handle allocation failure
    if (!buffer) {
        return NULL;
    }
    // format directional easu base
    int written = snprintf(buffer, VSR_SHADER_BUF,
        "\n// === linux-vsr: edge-adaptive spatial filter ===\n"
        "float sample_luma_cas(sampler2D tex, vec2 uv, vec4 bounds) {\n"
        "    vec2 size = vec2(textureSize(tex, 0));\n"
        "    vec2 texel = 1.0 / max(size, vec2(1.0));\n"
        "    float tc = texture(tex, clamp(uv + vec2(0.0, -texel.y), bounds.xy, bounds.zw)).r;\n"
        "    float bc = texture(tex, clamp(uv + vec2(0.0,  texel.y), bounds.xy, bounds.zw)).r;\n"
        "    float ml = texture(tex, clamp(uv + vec2(-texel.x, 0.0), bounds.xy, bounds.zw)).r;\n"
        "    float mr = texture(tex, clamp(uv + vec2( texel.x, 0.0), bounds.xy, bounds.zw)).r;\n"
        "    float cc = texture(tex, uv).r;\n"
        "    float gx = abs(mr - ml);\n"
        "    float gy = abs(bc - tc);\n"
        "    float wx = 1.0 / (gx + 0.001);\n"
        "    float wy = 1.0 / (gy + 0.001);\n"
        "    float res = (ml * wx + mr * wx + tc * wy + bc * wy + 2.0 * cc) / (2.0 * (wx + wy) + 2.0);\n"
        "    res = clamp(res, 0.0, 1.0);\n");
    // validate snprintf result
    if (written <= 0 || (size_t)written >= VSR_SHADER_BUF) {
        free(buffer);
        vsr_safety_set_error("easu snippet truncated");
        return NULL;
    }
    // append watermark tail when enabled
    if (watermark) {
        // generate badge helpers
        int wm = vsr_shader_append_watermark(buffer, VSR_SHADER_BUF, (size_t)written, opacity, size_frac);
        // handle generation failure
        if (wm < 0) {
            free(buffer);
            return NULL;
        }
        written += wm;
        // append badge apply and close
        int tail = snprintf(buffer + written, VSR_SHADER_BUF - (size_t)written,
            "    res = vsr_apply_badge(res, uv, bounds);\n"
            "    return res;\n"
            "}\n// === end linux-vsr ===\n\n");
        // validate tail write
        if (tail <= 0 || (size_t)tail >= VSR_SHADER_BUF - (size_t)written) {
            free(buffer);
            vsr_safety_set_error("easu tail truncated");
            return NULL;
        }
    } else {
        // append plain return when badge disabled
        int tail = snprintf(buffer + written, VSR_SHADER_BUF - (size_t)written,
            "    return res;\n"
            "}\n// === end linux-vsr ===\n\n");
        // validate tail write
        if (tail <= 0 || (size_t)tail >= VSR_SHADER_BUF - (size_t)written) {
            free(buffer);
            vsr_safety_set_error("easu tail truncated");
            return NULL;
        }
    }
    return buffer;
}

// dispatch generator by mode name (cas/easu/off)
char* vsr_shader_generate_upscaler(const char *mode, float sharpness, bool watermark, float opacity, float size_frac) {
    // handle null mode as default cas
    if (!mode || mode[0] == '\0') {
        return vsr_shader_generate_cas_full(sharpness, watermark, opacity, size_frac);
    }
    // dispatch cas branch
    if (strcmp(mode, "cas") == 0) {
        return vsr_shader_generate_cas_full(sharpness, watermark, opacity, size_frac);
    }
    // dispatch easu branch
    if (strcmp(mode, "easu") == 0) {
        return vsr_shader_generate_easu_full(watermark, opacity, size_frac);
    }
    // handle off mode as null (no injection)
    if (strcmp(mode, "off") == 0) {
        return NULL;
    }
    // fallback to cas on unknown input
    return vsr_shader_generate_cas_full(sharpness, watermark, opacity, size_frac);
}
