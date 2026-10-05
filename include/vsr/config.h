#ifndef VSR_CONFIG_H
#define VSR_CONFIG_H

#include <stdbool.h>

// configuration state structure
typedef struct {
    bool enabled;
    bool debug;
    float sharpness;
} vsr_config_t;

// retrieve global configuration instance
vsr_config_t* vsr_config_get(void);

// load configuration from environment variables
void vsr_config_load(vsr_config_t *cfg);

// set default configuration values
void vsr_config_init_defaults(vsr_config_t *cfg);

#endif // VSR_CONFIG_H
