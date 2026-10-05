#include "vsr/patcher.h"
#include "vsr/shaders.h"
#include "vsr/safety.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// check if shader contains signatures of webrender yuv video decoder
bool vsr_patcher_is_target_shader(const char *source) {
    // validate input pointer
    if (!source) {
        return false;
    }
    // bound input length to avoid scanning huge buffers
    size_t len = vsr_safe_strlen(source, VSR_MAX_SHADER_SIZE + 1);
    // reject empty or oversized input
    if (len == 0 || len > VSR_MAX_SHADER_SIZE) {
        return false;
    }
    // skip already patched shaders for idempotency
    if (strstr(source, "sample_luma_cas") != NULL) {
        return false;
    }
    // skip our own marker to avoid recursion
    if (strstr(source, "linux-vsr") != NULL && strstr(source, "sample_yuv") == NULL) {
        return false;
    }
    // verify existence of sample_yuv function
    bool has_sample_yuv = (strstr(source, "sample_yuv") != NULL);
    // verify existence of luma texture sampling call
    bool has_luma_sample = (strstr(source, "TEX_SAMPLE(sColor0, uv_y).r") != NULL);
    // require both signatures
    return has_sample_yuv && has_luma_sample;
}

// replace all instances of target string inside input string
char* vsr_patcher_replace_all(const char *orig, const char *target, const char *replacement) {
    // validate input pointers
    if (!orig || !target) {
        vsr_safety_set_error("replace null input");
        return NULL;
    }
    // bound input lengths
    size_t orig_len = vsr_safe_strlen(orig, VSR_MAX_SHADER_SIZE + 1);
    size_t target_len = vsr_safe_strlen(target, 512);
    // validate lengths
    if (orig_len == 0 || orig_len > VSR_MAX_SHADER_SIZE) {
        vsr_safety_set_error("replace orig length invalid");
        return NULL;
    }
    if (target_len == 0 || target_len > 511) {
        vsr_safety_set_error("replace target length invalid");
        return NULL;
    }
    // handle null replacement as empty string
    if (!replacement) {
        replacement = "";
    }
    // bound replacement length
    size_t replacement_len = vsr_safe_strlen(replacement, VSR_MAX_SHADER_SIZE + 1);
    if (replacement_len > VSR_MAX_SHADER_SIZE) {
        vsr_safety_set_error("replace replacement too long");
        return NULL;
    }
    // count total occurrences with cap
    size_t count = 0;
    const char *scan = orig;
    while ((scan = strstr(scan, target)) != NULL) {
        count++;
        // enforce replacement count limit
        if (count > VSR_MAX_REPLACE_COUNT) {
            vsr_safety_set_error("replace count exceeds limit");
            return NULL;
        }
        scan += target_len;
    }
    // compute size delta safely
    size_t new_len = 0;
    if (replacement_len >= target_len) {
        // growing replacement path
        size_t diff = replacement_len - target_len;
        size_t growth = 0;
        // check multiplication overflow
        if (!vsr_safe_mul_size(diff, count, &growth)) {
            vsr_safety_set_error("replace size overflow");
            return NULL;
        }
        // check addition overflow
        if (!vsr_safe_add_size(orig_len, growth, &new_len)) {
            vsr_safety_set_error("replace size overflow");
            return NULL;
        }
    } else {
        // shrinking replacement path
        size_t diff = target_len - replacement_len;
        size_t shrink = 0;
        // check multiplication overflow
        if (!vsr_safe_mul_size(diff, count, &shrink)) {
            vsr_safety_set_error("replace size overflow");
            return NULL;
        }
        // shrinking cannot underflow when count is correct
        if (shrink > orig_len) {
            vsr_safety_set_error("replace shrink underflow");
            return NULL;
        }
        new_len = orig_len - shrink;
    }
    // enforce global output limit
    if (new_len > VSR_MAX_OUTPUT_SIZE) {
        vsr_safety_set_error("replace output exceeds limit");
        return NULL;
    }
    // allocate output buffer
    char *result = (char *)vsr_safe_malloc(new_len + 1);
    // handle allocation failure
    if (!result) {
        return NULL;
    }
    // fast path when no matches found
    if (count == 0) {
        // copy original verbatim
        memcpy(result, orig, orig_len + 1);
        return result;
    }
    // copy segments and insert replacements
    char *dest = result;
    scan = orig;
    size_t remaining = count;
    while (remaining-- > 0) {
        // locate next match
        const char *match = strstr(scan, target);
        // handle unexpected miss defensively
        if (!match) {
            break;
        }
        size_t prefix_len = (size_t)(match - scan);
        // copy preceding chunk
        memcpy(dest, scan, prefix_len);
        dest += prefix_len;
        // copy replacement string
        if (replacement_len > 0) {
            memcpy(dest, replacement, replacement_len);
            dest += replacement_len;
        }
        // advance scan pointer
        scan = match + target_len;
    }
    // copy trailing part of the string
    size_t tail_len = orig_len - (size_t)(scan - orig);
    memcpy(dest, scan, tail_len);
    dest[tail_len] = '\0';
    return result;
}

// transform shader source code by injecting cas upscaling filter
char* vsr_patcher_inject_upscaler(const char *source, float sharpness) {
    // delegate to full version with safe defaults
    return vsr_patcher_inject_upscaler_full(source, "cas", sharpness, false, 0.45f, 0.06f);
}

// transform shader with full mode and watermark control
char* vsr_patcher_inject_upscaler_full(const char *source, const char *mode, float sharpness, bool watermark, float opacity, float size_frac) {
    // validate source pointer
    if (!source) {
        vsr_safety_set_error("inject null source");
        return NULL;
    }
    // bound source length
    size_t src_len = vsr_safe_strlen(source, VSR_MAX_SHADER_SIZE + 1);
    // reject empty or oversized shaders
    if (src_len == 0 || src_len > VSR_MAX_SHADER_SIZE) {
        vsr_safety_set_error("inject source length invalid");
        return NULL;
    }
    // sanitize mode input
    if (!mode || mode[0] == '\0') {
        mode = "cas";
    }
    // sanitize numeric params
    sharpness = vsr_safe_clamp_float(sharpness, 0.0f, 0.50f);
    opacity = vsr_safe_clamp_float(opacity, 0.05f, 1.0f);
    size_frac = vsr_safe_clamp_float(size_frac, 0.02f, 0.15f);
    // handle off mode as no-op
    if (strncmp(mode, "off", VSR_MAX_MODE_LEN) == 0) {
        return NULL;
    }
    // skip already patched content
    if (strstr(source, "sample_luma_cas") != NULL) {
        return NULL;
    }
    // require injection anchor present
    if (strstr(source, "vec4 sample_yuv(") == NULL) {
        vsr_safety_set_error("inject anchor not found");
        return NULL;
    }
    // require sampling line present
    if (strstr(source, "ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;") == NULL) {
        vsr_safety_set_error("inject sampling line not found");
        return NULL;
    }
    // detect bounds variable for watermark path
    bool has_bounds = (strstr(source, "uv_bounds_y") != NULL);
    // disable watermark when bounds missing to keep shader compilable
    bool use_watermark = watermark && has_bounds;
    // generate glsl upscaler function
    char *upscaler_func = vsr_shader_generate_upscaler(mode, sharpness, use_watermark, opacity, size_frac);
    // handle generation failure
    if (!upscaler_func) {
        return NULL;
    }
    // construct insertion marker string safely
    size_t func_len = vsr_safe_strlen(upscaler_func, VSR_MAX_SHADER_SIZE);
    const char *anchor = "vec4 sample_yuv(";
    size_t anchor_len = strlen(anchor);
    // compute marker length with overflow check
    size_t marker_len = 0;
    if (!vsr_safe_add_size(func_len, anchor_len, &marker_len) || !vsr_safe_add_size(marker_len, 1, &marker_len)) {
        free(upscaler_func);
        vsr_safety_set_error("inject marker size overflow");
        return NULL;
    }
    // enforce marker limit
    if (marker_len > VSR_MAX_SHADER_SIZE) {
        free(upscaler_func);
        vsr_safety_set_error("inject marker too large");
        return NULL;
    }
    // allocate marker buffer
    char *marker_str = (char *)vsr_safe_malloc(marker_len);
    // handle allocation failure
    if (!marker_str) {
        free(upscaler_func);
        return NULL;
    }
    // build marker payload
    memcpy(marker_str, upscaler_func, func_len);
    memcpy(marker_str + func_len, anchor, anchor_len + 1);
    // release generator buffer
    free(upscaler_func);
    // inject helper function right before sample_yuv definition
    char *with_func = vsr_patcher_replace_all(source, anchor, marker_str);
    // release marker buffer
    free(marker_str);
    // handle injection failure
    if (!with_func) {
        return NULL;
    }
    // replace standard texture sampling with adaptive luma sampling
    char *final_src = vsr_patcher_replace_all(
        with_func,
        "ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;",
        "ycbcr_sample.x = sample_luma_cas(sColor0, uv_y, uv_bounds_y);"
    );
    // release intermediate buffer
    free(with_func);
    // validate final output
    if (!final_src) {
        return NULL;
    }
    // enforce final size limit
    size_t final_len = vsr_safe_strlen(final_src, VSR_MAX_OUTPUT_SIZE + 1);
    if (final_len == 0 || final_len > VSR_MAX_OUTPUT_SIZE) {
        free(final_src);
        vsr_safety_set_error("inject final size invalid");
        return NULL;
    }
    return final_src;
}

// combine array of glsl string chunks into one contiguous buffer
char* vsr_patcher_combine_chunks(int count, const char *const *string, const int *length, size_t *out_len) {
    // init output length to zero
    if (out_len) {
        *out_len = 0;
    }
    // validate chunk count
    if (count <= 0 || count > VSR_MAX_CHUNKS) {
        vsr_safety_set_error("combine chunk count invalid");
        return NULL;
    }
    // validate string array pointer
    if (!string) {
        vsr_safety_set_error("combine null string array");
        return NULL;
    }
    // calculate total string length safely
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        size_t chunk_len = 0;
        // prefer explicit length when positive
        if (length && length[i] > 0) {
            // bound explicit length
            if ((size_t)length[i] > VSR_MAX_SHADER_SIZE) {
                vsr_safety_set_error("combine chunk too large");
                return NULL;
            }
            // require non-null data for explicit length
            if (!string[i]) {
                vsr_safety_set_error("combine null chunk with length");
                return NULL;
            }
            chunk_len = (size_t)length[i];
        } else if (string[i]) {
            // measure null-terminated chunk
            chunk_len = vsr_safe_strlen(string[i], VSR_MAX_SHADER_SIZE + 1);
            // reject unterminated chunks
            if (chunk_len > VSR_MAX_SHADER_SIZE) {
                vsr_safety_set_error("combine chunk unterminated or too large");
                return NULL;
            }
        } else {
            // skip null chunks gracefully
            continue;
        }
        // accumulate with overflow check
        size_t next = 0;
        if (!vsr_safe_add_size(total, chunk_len, &next)) {
            vsr_safety_set_error("combine total overflow");
            return NULL;
        }
        total = next;
        // enforce global shader limit early
        if (total > VSR_MAX_SHADER_SIZE) {
            vsr_safety_set_error("combine total exceeds limit");
            return NULL;
        }
    }
    // update output length
    if (out_len) {
        *out_len = total;
    }
    // reject empty payload
    if (total == 0) {
        return NULL;
    }
    // allocate memory for combined string
    char *buf = (char *)vsr_safe_malloc(total + 1);
    // handle allocation failure
    if (!buf) {
        return NULL;
    }
    // concatenate each chunk into buffer
    char *ptr = buf;
    for (int i = 0; i < count; i++) {
        size_t l = 0;
        // resolve chunk length again
        if (length && length[i] > 0) {
            l = (size_t)length[i];
        } else if (string[i]) {
            l = vsr_safe_strlen(string[i], VSR_MAX_SHADER_SIZE + 1);
            if (l > VSR_MAX_SHADER_SIZE) {
                l = 0;
            }
        }
        // copy non-empty chunks
        if (string && string[i] && l > 0) {
            memcpy(ptr, string[i], l);
            ptr += l;
        }
    }
    // terminate buffer
    *ptr = '\0';
    return buf;
}
