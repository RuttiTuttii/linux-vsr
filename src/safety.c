#include "vsr/safety.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// thread-local storage for last error message
static _Thread_local char g_last_error[256] = {0};

// debug: get last error string
const char *vsr_safety_last_error(void) {
    // return pointer to thread-local buffer
    return g_last_error;
}

// debug: set last error string
void vsr_safety_set_error(const char *msg) {
    // handle null input safely
    if (!msg) {
        g_last_error[0] = '\0';
        return;
    }
    // copy with truncation guard
    strncpy(g_last_error, msg, sizeof(g_last_error) - 1);
    // ensure null termination
    g_last_error[sizeof(g_last_error) - 1] = '\0';
}

// debug: clear last error string
void vsr_safety_clear_error(void) {
    // reset error buffer
    g_last_error[0] = '\0';
}

// memory: safe malloc with zeroed memory and limit check
void *vsr_safe_malloc(size_t size) {
    // reject zero and oversized requests
    if (size == 0 || size > VSR_MAX_OUTPUT_SIZE) {
        vsr_safety_set_error("allocation size out of range");
        return NULL;
    }
    // allocate memory from heap
    void *ptr = malloc(size);
    // handle out of memory condition
    if (!ptr) {
        vsr_safety_set_error("out of memory");
        return NULL;
    }
    return ptr;
}

// memory: safe calloc wrapper with overflow check
void *vsr_safe_calloc(size_t count, size_t size) {
    // reject zero arguments
    if (count == 0 || size == 0) {
        vsr_safety_set_error("calloc zero size");
        return NULL;
    }
    // check multiplication overflow
    size_t total = 0;
    if (!vsr_safe_mul_size(count, size, &total)) {
        vsr_safety_set_error("calloc size overflow");
        return NULL;
    }
    // check against global output limit
    if (total > VSR_MAX_OUTPUT_SIZE) {
        vsr_safety_set_error("calloc size exceeds limit");
        return NULL;
    }
    // allocate zeroed memory
    void *ptr = calloc(count, size);
    // handle out of memory condition
    if (!ptr) {
        vsr_safety_set_error("out of memory");
        return NULL;
    }
    return ptr;
}

// memory: safe string duplication with length limit
char *vsr_safe_strdup(const char *src, size_t max_len) {
    // validate input pointer
    if (!src) {
        vsr_safety_set_error("strdup null input");
        return NULL;
    }
    // measure bounded string length
    size_t len = vsr_safe_strlen(src, max_len);
    // reject empty or unterminated strings
    if (len >= max_len) {
        vsr_safety_set_error("string too long or unterminated");
        return NULL;
    }
    // allocate destination buffer
    char *dup = (char *)vsr_safe_malloc(len + 1);
    // handle allocation failure
    if (!dup) {
        return NULL;
    }
    // copy payload with terminator
    memcpy(dup, src, len);
    dup[len] = '\0';
    return dup;
}

// memory: safe string length with upper bound
size_t vsr_safe_strlen(const char *str, size_t max_len) {
    // handle null input safely
    if (!str) {
        return 0;
    }
    // scan for terminator within bound
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }
    return len;
}

// math: checked addition for sizes
bool vsr_safe_add_size(size_t a, size_t b, size_t *out) {
    // validate output pointer
    if (!out) {
        return false;
    }
    // detect unsigned overflow
    if (a > ((size_t)-1) - b) {
        return false;
    }
    // store result
    *out = a + b;
    return true;
}

// math: checked multiplication for sizes
bool vsr_safe_mul_size(size_t a, size_t b, size_t *out) {
    // validate output pointer
    if (!out) {
        return false;
    }
    // handle zero fast path
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    // detect unsigned overflow
    if (a > ((size_t)-1) / b) {
        return false;
    }
    // store result
    *out = a * b;
    return true;
}

// validate: check float is finite and in range
bool vsr_safe_float_in_range(float val, float min_val, float max_val) {
    // reject nan and infinities
    if (!isfinite(val)) {
        return false;
    }
    // check lower and upper bounds
    if (val < min_val || val > max_val) {
        return false;
    }
    return true;
}

// validate: clamp float into range
float vsr_safe_clamp_float(float val, float min_val, float max_val) {
    // handle nan by returning minimum
    if (!isfinite(val)) {
        return min_val;
    }
    // clamp lower bound
    if (val < min_val) {
        return min_val;
    }
    // clamp upper bound
    if (val > max_val) {
        return max_val;
    }
    return val;
}
