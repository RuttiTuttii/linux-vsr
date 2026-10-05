#include "vsr/logger.h"
#include <stdio.h>
#include <stdarg.h>

static bool g_log_debug = false;

// initialize logging level based on debug flag
void vsr_log_init(bool debug_enabled) {
    g_log_debug = debug_enabled;
}

// output informational log line with green prefix
void vsr_log_info(const char *fmt, ...) {
    // print stylized tag
    fprintf(stderr, "\033[1;32m[linux-vsr]\033[0m ");
    
    // format and print user message
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    
    // ensure trailing newline
    fprintf(stderr, "\n");
}

// output debug log line if debug mode is active
void vsr_log_debug(const char *fmt, ...) {
    // check if debug output is allowed
    if (!g_log_debug) {
        return;
    }

    // print stylized cyan debug tag
    fprintf(stderr, "\033[1;36m[linux-vsr debug]\033[0m ");
    
    // format message
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    
    // append newline
    fprintf(stderr, "\n");
}

// output error notification with red prefix
void vsr_log_error(const char *fmt, ...) {
    // print stylized red error tag
    fprintf(stderr, "\033[1;31m[linux-vsr error]\033[0m ");
    
    // format and write error text
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    
    // append newline
    fprintf(stderr, "\n");
}
