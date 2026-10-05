#include "vsr/config.h"
#include <stdlib.h>
#include <string.h>

static vsr_config_t g_config;

// return pointer to static configuration instance
vsr_config_t* vsr_config_get(void) {
    return &g_config;
}

// set safe default values
void vsr_config_init_defaults(vsr_config_t *cfg) {
    // enable vsr by default
    cfg->enabled = true;
    
    // enable diagnostic logging by default
    cfg->debug = true;
    
    // set balanced default sharpness level
    cfg->sharpness = 0.22f;
}

// parse environment variables and populate configuration structure
void vsr_config_load(vsr_config_t *cfg) {
    // initialize baseline defaults
    vsr_config_init_defaults(cfg);

    // check if vsr is explicitly disabled
    const char *enable_env = getenv("VSR_ENABLE");
    if (!enable_env) {
        enable_env = getenv("ZEN_VSR_ENABLE");
    }
    if (enable_env && (strcmp(enable_env, "0") == 0 || strcmp(enable_env, "false") == 0)) {
        cfg->enabled = false;
    }

    // check if debug output is disabled
    const char *debug_env = getenv("VSR_DEBUG");
    if (!debug_env) {
        debug_env = getenv("ZEN_VSR_DEBUG");
    }
    if (debug_env && (strcmp(debug_env, "0") == 0 || strcmp(debug_env, "false") == 0)) {
        cfg->debug = false;
    }

    // check custom sharpness setting
    const char *sharp_env = getenv("VSR_SHARPNESS");
    if (!sharp_env) {
        sharp_env = getenv("ZEN_VSR_SHARPNESS");
    }
    if (sharp_env) {
        char *endptr = NULL;
        float val = strtof(sharp_env, &endptr);
        // validate float range between 0.0 and 0.50
        if (endptr != sharp_env && val >= 0.0f && val <= 0.50f) {
            cfg->sharpness = val;
        }
    }
}
