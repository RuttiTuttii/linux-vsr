#include "vsr/logger.h"
#include "vsr/safety.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

// internal debug flag storage
static bool g_log_debug = false;

// internal rate limit state for info messages
static long long g_last_info_ms = 0;
static int g_info_suppressed = 0;

// get monotonic time in milliseconds
static long long vsr_log_now_ms(void) {
    // query monotonic clock
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    // convert to milliseconds with overflow guard
    long long sec_ms = (long long)ts.tv_sec * 1000LL;
    long long nsec_ms = (long long)(ts.tv_nsec / 1000000LL);
    return sec_ms + nsec_ms;
}

// initialize logging level based on debug flag
void vsr_log_init(bool debug_enabled) {
    // store verbosity flag
    g_log_debug = debug_enabled;
    // reset rate limit state
    g_last_info_ms = 0;
    g_info_suppressed = 0;
}

// output informational log line with green prefix
void vsr_log_info(const char *fmt, ...) {
    // validate format string
    if (!fmt) {
        return;
    }
    // bound format length to avoid log spam
    if (vsr_safe_strlen(fmt, 2048) >= 2048) {
        return;
    }
    // apply rate limit of one message per 2 seconds
    long long now = vsr_log_now_ms();
    if (now != 0 && g_last_info_ms != 0 && (now - g_last_info_ms) < 2000) {
        // count suppressed duplicates
        g_info_suppressed++;
        return;
    }
    // update timestamp marker
    g_last_info_ms = now;
    // print stylized tag
    fprintf(stderr, "\033[1;32m[linux-vsr]\033[0m ");
    // format and print user message
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    // report suppressed count when present
    if (g_info_suppressed > 0) {
        fprintf(stderr, " (suppressed %d similar)", g_info_suppressed);
        g_info_suppressed = 0;
    }
    // ensure trailing newline
    fprintf(stderr, "\n");
}

// output debug log line if debug mode is active
void vsr_log_debug(const char *fmt, ...) {
    // check if debug output is allowed
    if (!g_log_debug) {
        return;
    }
    // validate format string
    if (!fmt) {
        return;
    }
    // bound format length
    if (vsr_safe_strlen(fmt, 2048) >= 2048) {
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
    // validate format string
    if (!fmt) {
        fmt = "unknown error";
    }
    // bound format length
    if (vsr_safe_strlen(fmt, 4096) >= 4096) {
        fmt = "log message too long, truncated";
    }
    // print stylized red error tag
    fprintf(stderr, "\033[1;31m[linux-vsr error]\033[0m ");
    // format and write error text
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    // append newline
    fprintf(stderr, "\n");
    // store generic marker for diagnostics
    vsr_safety_set_error("see stderr for details");
}
