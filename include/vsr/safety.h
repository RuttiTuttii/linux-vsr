#ifndef VSR_SAFETY_H
#define VSR_SAFETY_H

#include <stddef.h>
#include <stdbool.h>

// debug: get last error string
const char *vsr_safety_last_error(void);

// debug: set last error string
void vsr_safety_set_error(const char *msg);

// debug: clear last error string
void vsr_safety_clear_error(void);

// memory: safe malloc with zeroed memory and limit check
void *vsr_safe_malloc(size_t size);

// memory: safe calloc wrapper with overflow check
void *vsr_safe_calloc(size_t count, size_t size);

// memory: safe string duplication with length limit
char *vsr_safe_strdup(const char *src, size_t max_len);

// memory: safe string length with upper bound
size_t vsr_safe_strlen(const char *str, size_t max_len);

// math: checked addition for sizes
bool vsr_safe_add_size(size_t a, size_t b, size_t *out);

// math: checked multiplication for sizes
bool vsr_safe_mul_size(size_t a, size_t b, size_t *out);

// validate: check float is finite and in range
bool vsr_safe_float_in_range(float val, float min_val, float max_val);

// validate: clamp float into range
float vsr_safe_clamp_float(float val, float min_val, float max_val);

// limits: global safety constants
#define VSR_MAX_SHADER_SIZE (8u * 1024u * 1024u)
#define VSR_MAX_OUTPUT_SIZE (16u * 1024u * 1024u)
#define VSR_MAX_CHUNKS 1024
#define VSR_MAX_REPLACE_COUNT 10000
#define VSR_MAX_CONFIG_LINE 1024
#define VSR_MAX_PATH_LEN 1024
#define VSR_MAX_MODE_LEN 16

#endif // VSR_SAFETY_H
