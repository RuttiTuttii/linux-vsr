#include "vsr/backend.h"
#include "vsr/safety.h"
#include <stdio.h>
#include <string.h>

// copy a backend name into a bounded output buffer
static bool vsr_backend_copy(char *out, size_t out_len, const char *name) {
    // validate output and source
    if (!out || out_len == 0 || !name || name[0] == '\0') {
        return false;
    }
    int written = snprintf(out, out_len, "%s", name);
    return written > 0 && (size_t)written < out_len;
}

// inspect DRM vendor identifiers without starting external helper processes
static const char *vsr_backend_detect_drm(void) {
    // scan the first few DRM cards for a known vendor id
    for (unsigned int card = 0; card < 16; card++) {
        char path[64] = {0};
        int written = snprintf(path, sizeof(path), "/sys/class/drm/card%u/device/vendor", card);
        if (written <= 0 || (size_t)written >= sizeof(path)) {
            continue;
        }
        FILE *fp = fopen(path, "r");
        if (!fp) {
            continue;
        }
        char vendor[32] = {0};
        if (fgets(vendor, sizeof(vendor), fp)) {
            fclose(fp);
            if (strstr(vendor, "0x10de") != NULL) {
                return "nvidia";
            }
            if (strstr(vendor, "0x1002") != NULL) {
                return "amd";
            }
        } else {
            fclose(fp);
        }
    }
    return "generic";
}

// resolve explicit selection or detect the active DRM vendor
bool vsr_backend_resolve(const char *requested, char *out, size_t out_len) {
    // validate output buffer
    if (!out || out_len == 0) {
        return false;
    }
    // honor explicit backend requests
    if (requested && requested[0] != '\0' && strcmp(requested, "auto") != 0) {
        if (strcmp(requested, "amd") != 0 && strcmp(requested, "nvidia") != 0
            && strcmp(requested, "generic") != 0) {
            vsr_safety_set_error("unknown backend requested");
            return false;
        }
        return vsr_backend_copy(out, out_len, requested);
    }
    // auto mode uses DRM vendor identifiers
    return vsr_backend_copy(out, out_len, vsr_backend_detect_drm());
}
