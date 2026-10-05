#ifndef VSR_PATCHER_H
#define VSR_PATCHER_H

#include <stdbool.h>
#include <stddef.h>

// inspect whether given shader source contains webrender yuv pipeline code
bool vsr_patcher_is_target_shader(const char *source);

// replace all instances of target substring with replacement string
char* vsr_patcher_replace_all(const char *orig, const char *target, const char *replacement);

// transform webrender shader by injecting cas upscaling function
char* vsr_patcher_inject_upscaler(const char *source, float sharpness);

// merge glshadersource input chunks into a single null-terminated string
char* vsr_patcher_combine_chunks(int count, const char *const *string, const int *length, size_t *out_len);

#endif // VSR_PATCHER_H
