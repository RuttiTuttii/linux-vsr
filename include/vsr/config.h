#ifndef VSR_CONFIG_H
#define VSR_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "vsr/safety.h"

// configuration state structure
typedef struct {
    bool enabled;
    bool debug;
    float sharpness;
    bool watermark_enabled;
    float watermark_opacity;
    float watermark_size;
    char mode[VSR_MAX_MODE_LEN];
    char model_path[VSR_MAX_PATH_LEN];
} vsr_config_t;

// retrieve global configuration instance
vsr_config_t* vsr_config_get(void);

// load configuration from environment variables
void vsr_config_load(vsr_config_t *cfg);

// set default configuration values
void vsr_config_init_defaults(vsr_config_t *cfg);

// validate and clamp configuration values
bool vsr_config_validate(vsr_config_t *cfg);

// resolve default config file path
bool vsr_config_default_path(char *out, size_t out_len);

// load configuration from ini-like key=value file
bool vsr_config_load_file(vsr_config_t *cfg, const char *path);

// save configuration to key=value file
bool vsr_config_save(const vsr_config_t *cfg, const char *path);

// reload global configuration from file and env
bool vsr_config_reload(void);

#endif // VSR_CONFIG_H
