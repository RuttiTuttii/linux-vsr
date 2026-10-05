#include "vsr/config.h"
#include "vsr/logger.h"
#include "vsr/hook.h"
#include "vsr/safety.h"
#include <unistd.h>
#include <string.h>

// constructor executed when library is preloaded into application process
__attribute__((constructor))
static void vsr_init(void) {
    // clear stale thread-local error
    vsr_safety_clear_error();
    // load configuration settings from environment
    vsr_config_t *cfg = vsr_config_get();
    // handle null singleton defensively
    if (!cfg) {
        return;
    }
    // populate defaults plus file and env
    vsr_config_load(cfg);
    // initialize logger subsystem
    vsr_log_init(cfg->debug);
    // log activation notice with mode details
    if (cfg->enabled) {
        vsr_log_debug("native video super resolution loaded (pid: %d, mode: %s, sharpness: %.2f, watermark: %d)",
            (int)getpid(), cfg->mode, (double)cfg->sharpness, cfg->watermark_enabled ? 1 : 0);
    } else {
        vsr_log_debug("vsr preloaded but disabled (pid: %d)", (int)getpid());
    }
    // pre-bind function hooks
    vsr_hook_init();
}

// destructor executed on process termination or library unload
__attribute__((destructor))
static void vsr_fini(void) {
    // perform teardown cleanup safely
    vsr_log_debug("unloading vsr hook from pid %d", (int)getpid());
    // clear error state
    vsr_safety_clear_error();
}
