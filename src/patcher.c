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
    // match legacy firefox format (pre-130)
    bool legacy = (strstr(source, "sample_yuv") != NULL)
        && (strstr(source, "TEX_SAMPLE(sColor0, uv_y).r") != NULL);
    // match modern firefox/zen format (157+, webrender nagle)
    // real desktop glsl uses vUv_Y/vUvBounds_Y, some builds use vUV_y variant
    bool has_uv_pair = (strstr(source, "vUv_Y") != NULL && strstr(source, "vUvBounds_Y") != NULL)
        || (strstr(source, "vUV_y") != NULL && strstr(source, "vUVBounds_y") != NULL);
    // require a fragment-stage luma sample, not only vertex-stage varyings
    bool has_luma_sample = (strstr(source, "texture (sColor0") != NULL)
        || (strstr(source, "texture(sColor0") != NULL);
    bool modern = (strstr(source, "ycbcr") != NULL || strstr(source, "Ycbcr") != NULL
            || strstr(source, "ps_quad_yuv") != NULL || strstr(source, "Debiased") != NULL)
        && has_uv_pair
        && has_luma_sample
        && (strstr(source, "sColor0") != NULL);
    // accept either pipeline
    return legacy || modern;
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
    if (strcmp(mode, "off") == 0) {
        return NULL;
    }
    // skip already patched content
    if (strstr(source, "sample_luma_cas") != NULL) {
        return NULL;
    }
    // detect legacy pipeline first
    bool is_legacy = (strstr(source, "vec4 sample_yuv(") != NULL)
        && (strstr(source, "ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;") != NULL);
    // detect modern firefox/zen pipeline with real identifier pairs
    bool has_real_pair = (strstr(source, "vUv_Y") != NULL) && (strstr(source, "vUvBounds_Y") != NULL);
    bool has_alt_pair = (strstr(source, "vUV_y") != NULL) && (strstr(source, "vUVBounds_y") != NULL);
    bool is_modern = !is_legacy
        && (strstr(source, "ycbcr") != NULL || strstr(source, "Ycbcr") != NULL
            || strstr(source, "ps_quad_yuv") != NULL || strstr(source, "Debiased") != NULL)
        && (has_real_pair || has_alt_pair)
        && (strstr(source, "sColor0") != NULL);
    // reject unknown layout
    if (!is_legacy && !is_modern) {
        vsr_safety_set_error("inject anchor not found");
        return NULL;
    }
    // handle legacy path with exact anchors
    if (is_legacy) {
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
    // handle modern path with whitespace-tolerant luma replacement
    // generate glsl upscaler function with new bounds names
    char *upscaler_func = vsr_shader_generate_upscaler(mode, sharpness, watermark, opacity, size_frac);
    // handle generation failure
    if (!upscaler_func) {
        return NULL;
    }
    // find version header end to keep #version first
    size_t insert_at = 0;
    if (strncmp(source, "#version", 8) == 0) {
        // locate end of first line
        const char *nl = strchr(source, '\n');
        if (nl) {
            insert_at = (size_t)(nl - source) + 1;
        }
    }
    // bound function length
    size_t func_len = vsr_safe_strlen(upscaler_func, VSR_MAX_SHADER_SIZE);
    // compute combined length safely
    size_t staged_len = 0;
    if (!vsr_safe_add_size(src_len, func_len, &staged_len) || !vsr_safe_add_size(staged_len, 1, &staged_len)) {
        free(upscaler_func);
        vsr_safety_set_error("inject staged size overflow");
        return NULL;
    }
    // enforce staged limit
    if (staged_len > VSR_MAX_OUTPUT_SIZE) {
        free(upscaler_func);
        vsr_safety_set_error("inject staged too large");
        return NULL;
    }
    // allocate staged buffer with function spliced in
    char *staged = (char *)vsr_safe_malloc(staged_len);
    // handle allocation failure
    if (!staged) {
        free(upscaler_func);
        return NULL;
    }
    // copy head part
    memcpy(staged, source, insert_at);
    // copy upscaler body
    memcpy(staged + insert_at, upscaler_func, func_len);
    // copy tail part
    memcpy(staged + insert_at + func_len, source + insert_at, src_len - insert_at + 1);
    // release generator buffer
    free(upscaler_func);
    // scan staged buffer for texture(sColor0, ...) luma fetches
    // use dynamic output that grows only when replacements found
    size_t staged_cur = vsr_safe_strlen(staged, VSR_MAX_OUTPUT_SIZE + 1);
    // pick uv and bounds identifiers present in this shader
    const char *uv_name = "vUv_Y";
    const char *bounds_name = "vUvBounds_Y";
    if (strstr(staged, "vUv_Y") == NULL && strstr(staged, "vUV_y") != NULL) {
        uv_name = "vUV_y";
        bounds_name = "vUVBounds_y";
    }
    // build replacement snippet for modern pipeline
    char cas_call[128] = {0};
    int cas_written = snprintf(cas_call, sizeof(cas_call), "sample_luma_cas(sColor0, %s, %s)", uv_name, bounds_name);
    // validate replacement format
    if (cas_written <= 0 || (size_t)cas_written >= sizeof(cas_call)) {
        free(staged);
        vsr_safety_set_error("inject cas call format failed");
        return NULL;
    }
    size_t cas_len = (size_t)cas_written;
    // first pass: count replacements to size output
    size_t replaced = 0;
    const char *scan = staged;
    while ((scan = strstr(scan, "texture")) != NULL) {
        // skip whitespace after keyword
        const char *p = scan + 7;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        // require opening paren and sColor0 sampler
        if (*p != '(') {
            scan += 7;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "sColor0", 7) != 0) {
            scan += 7;
            continue;
        }
        replaced++;
        scan = p + 7;
        // guard runaway counts
        if (replaced > VSR_MAX_REPLACE_COUNT) {
            break;
        }
    }
    // fallback when no luma fetch found
    if (replaced == 0) {
        free(staged);
        vsr_safety_set_error("inject modern luma fetch not found");
        return NULL;
    }
    // second pass: build output with paren-aware replacement
    // note: cas call can be longer than the original fetch, size with headroom
    size_t out_cap = 0;
    size_t headroom = 0;
    if (!vsr_safe_mul_size(replaced, cas_len + 1, &headroom) || !vsr_safe_add_size(staged_cur, headroom, &out_cap) || !vsr_safe_add_size(out_cap, 1, &out_cap)) {
        free(staged);
        vsr_safety_set_error("inject output size overflow");
        return NULL;
    }
    // enforce global output limit
    if (out_cap > VSR_MAX_OUTPUT_SIZE) {
        free(staged);
        vsr_safety_set_error("inject output exceeds limit");
        return NULL;
    }
    // allocate output buffer with growth headroom
    char *final_src = (char *)vsr_safe_malloc(out_cap);
    // handle allocation failure
    if (!final_src) {
        free(staged);
        return NULL;
    }
    // copy with replacement loop
    const char *src_ptr = staged;
    char *dst_ptr = final_src;
    // count scalar replacements for no-op detection
    size_t scalar_done = 0;
    while (*src_ptr) {
        // track whether current position was replaced
        bool consumed = false;
        // detect texture keyword at this position
        if (strncmp(src_ptr, "texture", 7) == 0) {
            // probe ahead for sColor0 pattern
            const char *p = src_ptr + 7;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '(') {
                const char *q = p + 1;
                while (*q == ' ' || *q == '\t') {
                    q++;
                }
                if (strncmp(q, "sColor0", 7) == 0) {
                    // find matching close paren with nesting
                    const char *r = p;
                    int depth = 0;
                    while (*r) {
                        if (*r == '(') {
                            depth++;
                        } else if (*r == ')') {
                            depth--;
                            if (depth == 0) {
                                break;
                            }
                        }
                        r++;
                    }
                    // validate paren match found
                    if (*r == ')' && depth == 0) {
                        // inspect trailing accessor to keep type scalar
                        const char *after = r + 1;
                        // allow whitespace between call and accessor
                        while (*after == ' ' || *after == '\t') {
                            after++;
                        }
                        // accept scalar luma accessors and consume them
                        size_t extra = 0;
                        bool scalar = false;
                        if (after[0] == '.' && (after[1] == 'x' || after[1] == 'r')) {
                            // ensure accessor is not a longer swizzle prefix
                            char third = after[2];
                            bool boundary = (third == '\0') || (third != '.' && (third < 'a' || third > 'z')
                                && (third < 'A' || third > 'Z') && (third < '0' || third > '9') && third != '_');
                            if (boundary) {
                                extra = (size_t)(after - (r + 1)) + 2;
                                scalar = true;
                            }
                            // packed swizzle otherwise, leave fetch untouched
                        } else if (after[0] != '.') {
                            // bare call without accessor yields float texture texel, replace directly
                            scalar = true;
                        }
                        // packed or vector accessor otherwise, leave fetch untouched
                        if (scalar) {
                            // guard output capacity before emitting
                            if ((size_t)(dst_ptr - final_src) + cas_len + 1 > out_cap) {
                                free(staged);
                                free(final_src);
                                vsr_safety_set_error("inject output capacity exceeded");
                                return NULL;
                            }
                            // emit cas call instead of texture fetch
                            memcpy(dst_ptr, cas_call, cas_len);
                            dst_ptr += cas_len;
                            // advance past original call and consumed accessor
                            src_ptr = r + 1 + extra;
                            scalar_done++;
                            consumed = true;
                        }
                    }
                }
            }
        }
        // copy single byte when not replaced
        if (!consumed) {
            // guard capacity for single byte
            if ((size_t)(dst_ptr - final_src) + 2 > out_cap) {
                free(staged);
                free(final_src);
                vsr_safety_set_error("inject output capacity exceeded");
                return NULL;
            }
            *dst_ptr++ = *src_ptr++;
        }
    }
    // terminate output
    *dst_ptr = '\0';
    // release staged buffer
    free(staged);
    // treat zero scalar replacements as no-op to avoid shipping dead code
    if (scalar_done == 0) {
        free(final_src);
        vsr_safety_set_error("inject no scalar luma fetch replaced");
        return NULL;
    }
    // validate final size
    size_t final_len = (size_t)(dst_ptr - final_src);
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
