#ifndef VSR_LOGGER_H
#define VSR_LOGGER_H

#include <stdbool.h>

// define log verbosity levels
typedef enum {
    VSR_LOG_LEVEL_NONE = 0,
    VSR_LOG_LEVEL_INFO = 1,
    VSR_LOG_LEVEL_DEBUG = 2
} vsr_log_level_t;

// initialize logger subsystem
void vsr_log_init(bool debug_enabled);

// print informational message
void vsr_log_info(const char *fmt, ...);

// print debug diagnostic message
void vsr_log_debug(const char *fmt, ...);

// print error notification
void vsr_log_error(const char *fmt, ...);

#endif // VSR_LOGGER_H
