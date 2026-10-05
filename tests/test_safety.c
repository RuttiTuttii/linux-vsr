#include "vsr/safety.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

// unit test for safe allocation guards
void test_safety_alloc(void) {
    // reject zero size
    assert(vsr_safe_malloc(0) == NULL);
    // reject oversized request
    assert(vsr_safe_malloc(VSR_MAX_OUTPUT_SIZE + 1) == NULL);
    // accept normal allocation
    void *p = vsr_safe_malloc(64);
    assert(p != NULL);
    free(p);
    // test calloc overflow path
    assert(vsr_safe_calloc(0, 10) == NULL);
    assert(vsr_safe_calloc(10, 0) == NULL);
    // test calloc huge overflow
    assert(vsr_safe_calloc((size_t)-1, 2) == NULL);
    // test normal calloc
    void *q = vsr_safe_calloc(4, 16);
    assert(q != NULL);
    free(q);
}

// unit test for safe string helpers
void test_safety_strings(void) {
    // handle null strlen
    assert(vsr_safe_strlen(NULL, 100) == 0);
    // measure bounded length
    assert(vsr_safe_strlen("hello", 100) == 5);
    // enforce bound truncation
    assert(vsr_safe_strlen("hello", 3) == 3);
    // test strdup null guard
    assert(vsr_safe_strdup(NULL, 100) == NULL);
    // test strdup normal path
    char *d = vsr_safe_strdup("vsr", 100);
    assert(d != NULL);
    assert(strcmp(d, "vsr") == 0);
    free(d);
    // test strdup overlong rejection
    assert(vsr_safe_strdup("toolong", 3) == NULL);
}

// unit test for checked arithmetic
void test_safety_math(void) {
    // test valid addition
    size_t out = 0;
    assert(vsr_safe_add_size(10, 20, &out) && out == 30);
    // test addition overflow
    assert(!vsr_safe_add_size((size_t)-1, 1, &out));
    // test null out pointer
    assert(!vsr_safe_add_size(1, 2, NULL));
    // test valid multiplication
    assert(vsr_safe_mul_size(6, 7, &out) && out == 42);
    // test zero fast path
    assert(vsr_safe_mul_size(0, (size_t)-1, &out) && out == 0);
    // test multiplication overflow
    assert(!vsr_safe_mul_size((size_t)-1, 2, &out));
    // test null out pointer
    assert(!vsr_safe_mul_size(1, 2, NULL));
}

// unit test for float validation
void test_safety_float(void) {
    // accept in-range value
    assert(vsr_safe_float_in_range(0.22f, 0.0f, 0.50f));
    // reject out of range
    assert(!vsr_safe_float_in_range(0.99f, 0.0f, 0.50f));
    // reject nan
    assert(!vsr_safe_float_in_range(NAN, 0.0f, 1.0f));
    // reject infinity
    assert(!vsr_safe_float_in_range(INFINITY, 0.0f, 1.0f));
    // test clamp lower
    assert(vsr_safe_clamp_float(-1.0f, 0.0f, 1.0f) == 0.0f);
    // test clamp upper
    assert(vsr_safe_clamp_float(2.0f, 0.0f, 1.0f) == 1.0f);
    // test clamp nan fallback
    assert(vsr_safe_clamp_float(NAN, 0.0f, 1.0f) == 0.0f);
    // test clamp passthrough
    assert(fabsf(vsr_safe_clamp_float(0.5f, 0.0f, 1.0f) - 0.5f) < 0.0001f);
}

// unit test for error slot
void test_safety_error_slot(void) {
    // set and read error
    vsr_safety_set_error("test error");
    assert(strcmp(vsr_safety_last_error(), "test error") == 0);
    // clear error
    vsr_safety_clear_error();
    assert(vsr_safety_last_error()[0] == '\0');
    // handle null set safely
    vsr_safety_set_error(NULL);
    assert(vsr_safety_last_error()[0] == '\0');
}
