#include "vsr/patcher.h"
#include "vsr/shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// check if shader contains signatures of webrender yuv video decoder
bool vsr_patcher_is_target_shader(const char *source) {
    if (!source) {
        return false;
    }

    // verify existence of sample_yuv function
    bool has_sample_yuv = (strstr(source, "sample_yuv") != NULL);
    
    // verify existence of luma texture sampling call
    bool has_luma_sample = (strstr(source, "TEX_SAMPLE(sColor0, uv_y).r") != NULL);

    return has_sample_yuv && has_luma_sample;
}

// replace all instances of target string inside input string
char* vsr_patcher_replace_all(const char *orig, const char *target, const char *replacement) {
    if (!orig || !target) {
        return NULL;
    }

    size_t target_len = strlen(target);
    if (target_len == 0) {
        return NULL;
    }

    if (!replacement) {
        replacement = "";
    }
    size_t replacement_len = strlen(replacement);

    // count total occurrences of target
    int count = 0;
    const char *scan = orig;
    while ((scan = strstr(scan, target)) != NULL) {
        count++;
        scan += target_len;
    }

    // calculate memory size needed for output string
    size_t new_len = strlen(orig) + (replacement_len - target_len) * count + 1;
    char *result = malloc(new_len);
    if (!result) {
        return NULL;
    }

    // copy segments and insert replacements
    char *dest = result;
    scan = orig;
    while (count--) {
        const char *match = strstr(scan, target);
        size_t prefix_len = (size_t)(match - scan);
        
        // copy preceding chunk
        memcpy(dest, scan, prefix_len);
        dest += prefix_len;
        
        // copy replacement string
        memcpy(dest, replacement, replacement_len);
        dest += replacement_len;
        
        // advance scan pointer
        scan = match + target_len;
    }

    // copy trailing part of the string
    strcpy(dest, scan);
    return result;
}

// transform shader source code by injecting cas upscaling filter
char* vsr_patcher_inject_upscaler(const char *source, float sharpness) {
    if (!source) {
        return NULL;
    }

    // generate glsl cas function
    char *cas_func = vsr_shader_generate_cas(sharpness);
    if (!cas_func) {
        return NULL;
    }

    // construct insertion marker string
    size_t marker_len = strlen(cas_func) + strlen("vec4 sample_yuv(") + 1;
    char *marker_str = malloc(marker_len);
    if (!marker_str) {
        free(cas_func);
        return NULL;
    }
    snprintf(marker_str, marker_len, "%svec4 sample_yuv(", cas_func);
    free(cas_func);

    // inject helper function right before sample_yuv definition
    char *with_func = vsr_patcher_replace_all(source, "vec4 sample_yuv(", marker_str);
    free(marker_str);

    if (!with_func) {
        return NULL;
    }

    // replace standard texture sampling with adaptive luma cas sampling
    char *final_src = vsr_patcher_replace_all(
        with_func,
        "ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;",
        "ycbcr_sample.x = sample_luma_cas(sColor0, uv_y, uv_bounds_y);"
    );

    free(with_func);
    return final_src;
}

// combine array of glsl string chunks into one contiguous buffer
char* vsr_patcher_combine_chunks(int count, const char *const *string, const int *length, size_t *out_len) {
    // calculate total string length
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (length && length[i] > 0) {
            total += (size_t)length[i];
        } else if (string && string[i]) {
            total += strlen(string[i]);
        }
    }

    if (out_len) {
        *out_len = total;
    }

    if (total == 0) {
        return NULL;
    }

    // allocate memory for combined string
    char *buf = malloc(total + 1);
    if (!buf) {
        return NULL;
    }

    // concatenate each chunk into buffer
    char *ptr = buf;
    for (int i = 0; i < count; i++) {
        size_t l = (length && length[i] > 0) ? (size_t)length[i] : (string[i] ? strlen(string[i]) : 0);
        if (string && string[i] && l > 0) {
            memcpy(ptr, string[i], l);
            ptr += l;
        }
    }
    *ptr = '\0';

    return buf;
}
