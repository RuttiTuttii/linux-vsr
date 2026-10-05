#include "vsr/config.h"
#include "vsr/logger.h"
#include "vsr/hook.h"
#include <unistd.h>

// constructor executed when library is preloaded into application process
__attribute__((constructor))
static void vsr_init(void) {
    // load configuration settings from environment
    vsr_config_t *cfg = vsr_config_get();
    vsr_config_load(cfg);

    // initialize logger subsystem
    vsr_log_init(cfg->debug);

    // log activation notice
    if (cfg->enabled) {
        vsr_log_debug("native video super resolution loaded (pid: %d, sharpness: %.2f)", getpid(), cfg->sharpness);
    }

    // pre-bind function hooks
    vsr_hook_init();
}

// destructor executed on process termination or library unload
__attribute__((destructor))
static void vsr_fini(void) {
    // perform teardown cleanup
    vsr_log_debug("unloading vsr hook from pid %d", getpid());
}
