#include "vsr/backend.h"
#include <assert.h>
#include <string.h>

// unit test for explicit backend selection
void test_backend_explicit(void) {
    char out[16] = {0};
    assert(vsr_backend_resolve("nvidia", out, sizeof(out)) == true);
    assert(strcmp(out, "nvidia") == 0);
    assert(vsr_backend_resolve("amd", out, sizeof(out)) == true);
    assert(strcmp(out, "amd") == 0);
    assert(vsr_backend_resolve("bogus", out, sizeof(out)) == false);
}

// unit test for automatic backend detection
void test_backend_auto(void) {
    char out[16] = {0};
    assert(vsr_backend_resolve("auto", out, sizeof(out)) == true);
    assert(strcmp(out, "amd") == 0 || strcmp(out, "nvidia") == 0 || strcmp(out, "generic") == 0);
    assert(vsr_backend_resolve(NULL, out, sizeof(out)) == true);
}

// unit test for output buffer guards
void test_backend_guards(void) {
    char out[4] = {0};
    assert(vsr_backend_resolve("nvidia", out, sizeof(out)) == false);
    assert(vsr_backend_resolve("nvidia", NULL, 0) == false);
}
