#include "vsr/config.h"
#include <assert.h>
#include <stdlib.h>
#include <math.h>

// unit test for default configuration state
void test_config_defaults(void) {
    // initialize config structure
    vsr_config_t cfg;
    vsr_config_init_defaults(&cfg);

    // assert defaults are set as expected
    assert(cfg.enabled == true);
    assert(cfg.debug == true);
    assert(fabsf(cfg.sharpness - 0.22f) < 0.001f);
}

// unit test for environment variable overrides
void test_config_env_overrides(void) {
    // set environment variables
    setenv("VSR_ENABLE", "0", 1);
    setenv("VSR_DEBUG", "0", 1);
    setenv("VSR_SHARPNESS", "0.35", 1);

    // load configuration
    vsr_config_t cfg;
    vsr_config_load(&cfg);

    // assert variables are parsed correctly
    assert(cfg.enabled == false);
    assert(cfg.debug == false);
    assert(fabsf(cfg.sharpness - 0.35f) < 0.001f);

    // clean up environment
    unsetenv("VSR_ENABLE");
    unsetenv("VSR_DEBUG");
    unsetenv("VSR_SHARPNESS");
}
