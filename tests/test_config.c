#include "vsr/config.h"
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

// unit test for default configuration state
void test_config_defaults(void) {
    // initialize config structure
    vsr_config_t cfg;
    vsr_config_init_defaults(&cfg);
    // assert defaults are set as expected
    assert(cfg.enabled == true);
    assert(cfg.debug == true);
    assert(fabsf(cfg.sharpness - 0.15f) < 0.001f);
    // assert watermark defaults
    assert(cfg.watermark_enabled == true);
    assert(fabsf(cfg.watermark_opacity - 0.45f) < 0.001f);
    assert(fabsf(cfg.watermark_size - 0.06f) < 0.001f);
    // assert mode default
    assert(strcmp(cfg.mode, "cas") == 0);
    // assert automatic backend selection
    assert(strcmp(cfg.backend, "auto") == 0);
    // assert null guards do not crash
    vsr_config_init_defaults(NULL);
    vsr_config_load(NULL);
    assert(vsr_config_validate(NULL) == false);
}

// unit test for environment variable overrides
void test_config_env_overrides(void) {
    // isolate file config by pointing to missing path
    setenv("VSR_CONFIG", "/tmp/vsr-test-missing-config.ini", 1);
    // set environment variables
    setenv("VSR_ENABLE", "0", 1);
    setenv("VSR_DEBUG", "0", 1);
    setenv("VSR_SHARPNESS", "0.35", 1);
    setenv("VSR_WATERMARK", "0", 1);
    setenv("VSR_MODE", "easu", 1);
    setenv("VSR_BACKEND", "nvidia", 1);
    // load configuration
    vsr_config_t cfg;
    vsr_config_load(&cfg);
    // assert variables are parsed correctly
    assert(cfg.enabled == false);
    assert(cfg.debug == false);
    assert(fabsf(cfg.sharpness - 0.35f) < 0.001f);
    assert(cfg.watermark_enabled == false);
    assert(strcmp(cfg.mode, "easu") == 0);
    assert(strcmp(cfg.backend, "nvidia") == 0);
    // accept the explicit directional sharpen mode
    strncpy(cfg.mode, "directional", sizeof(cfg.mode) - 1);
    assert(vsr_config_validate(&cfg) == true);
    assert(strcmp(cfg.mode, "directional") == 0);
    // clean up environment
    unsetenv("VSR_ENABLE");
    unsetenv("VSR_DEBUG");
    unsetenv("VSR_SHARPNESS");
    unsetenv("VSR_WATERMARK");
    unsetenv("VSR_MODE");
    unsetenv("VSR_BACKEND");
    unsetenv("VSR_CONFIG");
}

// unit test for invalid env values fallback
void test_config_invalid_env(void) {
    // isolate file config
    setenv("VSR_CONFIG", "/tmp/vsr-test-missing-config.ini", 1);
    // set invalid values
    setenv("VSR_SHARPNESS", "not-a-number", 1);
    setenv("VSR_ENABLE", "maybe", 1);
    setenv("VSR_MODE", "unknown_mode_xyz", 1);
    setenv("VSR_BACKEND", "unknown_backend_xyz", 1);
    // load configuration
    vsr_config_t cfg;
    vsr_config_load(&cfg);
    // assert fallback to safe defaults
    assert(fabsf(cfg.sharpness - 0.15f) < 0.001f);
    assert(cfg.enabled == true);
    assert(strcmp(cfg.mode, "cas") == 0);
    assert(strcmp(cfg.backend, "auto") == 0);
    // clean up environment
    unsetenv("VSR_SHARPNESS");
    unsetenv("VSR_ENABLE");
    unsetenv("VSR_MODE");
    unsetenv("VSR_BACKEND");
    unsetenv("VSR_CONFIG");
}

// unit test for file save and load roundtrip
void test_config_file_roundtrip(void) {
    // build temp path
    char tmpl[] = "/tmp/vsr-test-XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);
    // prepare config with custom values
    vsr_config_t src;
    vsr_config_init_defaults(&src);
    src.enabled = false;
    src.debug = false;
    src.sharpness = 0.31f;
    src.watermark_enabled = true;
    src.watermark_opacity = 0.7f;
    src.watermark_size = 0.08f;
    strncpy(src.mode, "easu", sizeof(src.mode) - 1);
    strncpy(src.backend, "amd", sizeof(src.backend) - 1);
    // save to file
    assert(vsr_config_save(&src, tmpl) == true);
    // load into fresh struct
    vsr_config_t dst;
    vsr_config_init_defaults(&dst);
    assert(vsr_config_load_file(&dst, tmpl) == true);
    // assert roundtrip preserved
    assert(dst.enabled == false);
    assert(dst.debug == false);
    assert(fabsf(dst.sharpness - 0.31f) < 0.001f);
    assert(strcmp(dst.mode, "easu") == 0);
    assert(strcmp(dst.backend, "amd") == 0);
    assert(fabsf(dst.watermark_opacity - 0.7f) < 0.001f);
    // test missing file returns false safely
    assert(vsr_config_load_file(&dst, "/tmp/vsr-definitely-missing-12345.ini") == false);
    // test null guards
    assert(vsr_config_load_file(NULL, tmpl) == false);
    assert(vsr_config_load_file(&dst, NULL) == false);
    assert(vsr_config_save(NULL, tmpl) == false);
    assert(vsr_config_save(&dst, NULL) == false);
    // cleanup temp file
    unlink(tmpl);
}

// unit test for validation clamping
void test_config_validate(void) {
    // prepare out of range config
    vsr_config_t cfg;
    vsr_config_init_defaults(&cfg);
    cfg.sharpness = 99.0f;
    cfg.watermark_opacity = -5.0f;
    cfg.watermark_size = 99.0f;
    // validate clamps values
    vsr_config_validate(&cfg);
    assert(cfg.sharpness <= 0.50f);
    assert(cfg.watermark_opacity >= 0.05f && cfg.watermark_opacity <= 1.0f);
    assert(cfg.watermark_size >= 0.02f && cfg.watermark_size <= 0.15f);
    // test default path resolver
    char out[1024] = {0};
    setenv("VSR_CONFIG", "/tmp/custom.ini", 1);
    assert(vsr_config_default_path(out, sizeof(out)) == true);
    assert(strcmp(out, "/tmp/custom.ini") == 0);
    unsetenv("VSR_CONFIG");
    // test null out guard
    assert(vsr_config_default_path(NULL, 0) == false);
}
