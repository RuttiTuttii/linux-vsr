// vsr_rt neural tests, gpu path runs only with sdk present
// usage: VSR_RT_SDK_DIR=... LD_LIBRARY_PATH=... ./build/vsr_rt_test
// without sdk only cpu-side guards are checked, gpu part skips cleanly

#include "vsr_rt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// unit test for cpu-side guards without gpu
static void test_rt_guards(void) {
    // null output slot rejected
    assert(vsr_rt_init(NULL, VSR_RT_HIGH) == VSR_RT_ERR_ARGS);
    // bad quality values rejected
    vsr_rt_session_t *session = NULL;
    assert(vsr_rt_init(&session, -1) == VSR_RT_ERR_ARGS);
    assert(vsr_rt_init(&session, 99) == VSR_RT_ERR_ARGS);
    assert(session == NULL);
    // null session calls rejected
    assert(vsr_rt_load(NULL, 100, 100) == VSR_RT_ERR_ARGS);
    assert(vsr_rt_upscale(NULL, 1, 10, 10, 2) == VSR_RT_ERR_ARGS);
    // null destroy safe
    vsr_rt_destroy(NULL);
    // geometry getters tolerate nulls
    vsr_rt_in_geometry(NULL, NULL, NULL, NULL);
    vsr_rt_out_geometry(NULL, NULL, NULL, NULL);
    // error strings non-empty
    assert(strlen(vsr_rt_strerror(VSR_RT_OK)) > 0);
    assert(strlen(vsr_rt_strerror(VSR_RT_ERR_RUN)) > 0);
    // sdk path accessor safe
    assert(vsr_rt_sdk_path() != NULL);
}

// unit test for bad dimensions rejected without inference
static void test_rt_bad_dims(vsr_rt_session_t *session) {
    // zero dims rejected
    assert(vsr_rt_load(session, 0, 100) == VSR_RT_ERR_ARGS);
    assert(vsr_rt_upscale(session, 1, 0, 10, 2) == VSR_RT_ERR_ARGS);
    assert(vsr_rt_upscale(session, 0, 10, 10, 2) == VSR_RT_ERR_ARGS);
}

int main(void) {
    // run cpu-side guard tests always
    printf("running vsr_rt guard tests...\n");
    test_rt_guards();
    printf("  [pass] guard tests\n");
    // check sdk presence for gpu path
    const char *sdk_dir = getenv("VSR_RT_SDK_DIR");
    if (!sdk_dir || !sdk_dir[0]) {
        printf("  [skip] gpu tests need VSR_RT_SDK_DIR\n");
        printf("all vsr_rt tests completed (cpu only)\n");
        return 0;
    }
    // init session against real sdk
    printf("running vsr_rt gpu tests...\n");
    vsr_rt_session_t *session = NULL;
    vsr_rt_status_t status = vsr_rt_init(&session, VSR_RT_HIGH);
    assert(status == VSR_RT_OK && session != NULL);
    // validate bad dims handling on live session
    test_rt_bad_dims(session);
    printf("  [pass] bad dims tests\n");
    // load tiny geometry for fast check
    status = vsr_rt_load(session, 128, 128);
    assert(status == VSR_RT_OK);
    // verify geometry getters report bound values
    int fmt = 0, type = 0;
    unsigned layout = 0;
    vsr_rt_out_geometry(session, &fmt, &type, &layout);
    assert(fmt == 6 && type == 1 && layout == 0);
    printf("  [pass] load and geometry tests\n");
    // upscale with null pointers must fail without crashing
    assert(vsr_rt_upscale(session, 0, 64, 64, 1) == VSR_RT_ERR_ARGS);
    printf("  [pass] upscale guard tests\n");
    // release session cleanly
    vsr_rt_destroy(session);
    printf("all vsr_rt tests completed successfully\n");
    return 0;
}
