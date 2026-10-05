#include "vsr/shaders.h"
#include "vsr/safety.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// unit test for basic cas generation
void test_shaders_cas_basic(void) {
    // generate default cas
    char *s = vsr_shader_generate_cas(0.22f);
    assert(s != NULL);
    // verify core function present
    assert(strstr(s, "sample_luma_cas") != NULL);
    // verify sharpness baked in
    assert(strstr(s, "0.2200") != NULL);
    free(s);
    // test nan sharpness clamped safely
    char *nan_s = vsr_shader_generate_cas(NAN);
    assert(nan_s != NULL);
    free(nan_s);
    // test infinite sharpness clamped safely
    char *inf_s = vsr_shader_generate_cas(INFINITY);
    assert(inf_s != NULL);
    free(inf_s);
}

// unit test for cas watermark badge
void test_shaders_cas_watermark(void) {
    // generate with badge enabled
    char *s = vsr_shader_generate_cas_full(0.22f, true, 0.45f, 0.06f);
    assert(s != NULL);
    // verify badge helpers present
    assert(strstr(s, "vsr_badge_mask") != NULL);
    assert(strstr(s, "vsr_apply_badge") != NULL);
    free(s);
    // generate without badge
    char *plain = vsr_shader_generate_cas_full(0.22f, false, 0.45f, 0.06f);
    assert(plain != NULL);
    // verify badge absent when disabled
    assert(strstr(plain, "vsr_badge_mask") == NULL);
    free(plain);
}

// unit test for easu generator
void test_shaders_easu(void) {
    // generate easu with badge
    char *s = vsr_shader_generate_easu_full(true, 0.5f, 0.06f);
    assert(s != NULL);
    // verify shared entry name for patcher compat
    assert(strstr(s, "sample_luma_cas") != NULL);
    assert(strstr(s, "vsr_apply_badge") != NULL);
    free(s);
    // generate easu without badge
    char *plain = vsr_shader_generate_easu_full(false, 0.5f, 0.06f);
    assert(plain != NULL);
    assert(strstr(plain, "vsr_badge_mask") == NULL);
    free(plain);
}

// unit test for mode dispatch
void test_shaders_dispatch(void) {
    // dispatch cas mode
    char *cas = vsr_shader_generate_upscaler("cas", 0.22f, false, 0.45f, 0.06f);
    assert(cas != NULL);
    free(cas);
    // dispatch easu mode
    char *easu = vsr_shader_generate_upscaler("easu", 0.22f, false, 0.45f, 0.06f);
    assert(easu != NULL);
    free(easu);
    // dispatch off mode returns null
    char *off = vsr_shader_generate_upscaler("off", 0.22f, false, 0.45f, 0.06f);
    assert(off == NULL);
    // dispatch null mode falls back to cas
    char *nullmode = vsr_shader_generate_upscaler(NULL, 0.22f, false, 0.45f, 0.06f);
    assert(nullmode != NULL);
    free(nullmode);
    // dispatch unknown falls back to cas
    char *unk = vsr_shader_generate_upscaler("unknown_xyz", 0.22f, false, 0.45f, 0.06f);
    assert(unk != NULL);
    free(unk);
}
