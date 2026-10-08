#include "vsr/config.h"
#include "vsr/safety.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <locale.h>

// global configuration singleton
static vsr_config_t g_config;

// return pointer to static configuration instance
vsr_config_t* vsr_config_get(void) {
    // expose global singleton address
    return &g_config;
}

// set safe default values
void vsr_config_init_defaults(vsr_config_t *cfg) {
    // handle null pointer safely
    if (!cfg) {
        vsr_safety_set_error("config null pointer in init_defaults");
        return;
    }
    // enable vsr by default
    cfg->enabled = true;
    // enable diagnostic logging by default
    cfg->debug = true;
    // set balanced default sharpness level
    cfg->sharpness = 0.15f;
    // enable watermark indicator by default
    cfg->watermark_enabled = true;
    // set semi-transparent default opacity
    cfg->watermark_opacity = 0.45f;
    // set small corner badge size as fraction of frame
    cfg->watermark_size = 0.06f;
    // set default upscaler mode
    strncpy(cfg->mode, "cas", sizeof(cfg->mode) - 1);
    cfg->mode[sizeof(cfg->mode) - 1] = '\0';
    // select hardware backend automatically unless overridden
    strncpy(cfg->backend, "auto", sizeof(cfg->backend) - 1);
    cfg->backend[sizeof(cfg->backend) - 1] = '\0';
    // clear optional model path
    cfg->model_path[0] = '\0';
}

// validate and clamp configuration values
bool vsr_config_validate(vsr_config_t *cfg) {
    // handle null pointer safely
    if (!cfg) {
        vsr_safety_set_error("config null pointer in validate");
        return false;
    }
    bool valid = true;
    // clamp sharpness into safe range
    cfg->sharpness = vsr_safe_clamp_float(cfg->sharpness, 0.0f, 0.50f);
    // clamp watermark opacity into visible range
    cfg->watermark_opacity = vsr_safe_clamp_float(cfg->watermark_opacity, 0.05f, 1.0f);
    // clamp watermark size into tiny badge range
    cfg->watermark_size = vsr_safe_clamp_float(cfg->watermark_size, 0.02f, 0.15f);
    // normalize mode string to lowercase
    for (size_t i = 0; cfg->mode[i] != '\0' && i < sizeof(cfg->mode); i++) {
        cfg->mode[i] = (char)tolower((unsigned char)cfg->mode[i]);
    }
    // validate mode value
    if (strcmp(cfg->mode, "cas") != 0 && strcmp(cfg->mode, "easu") != 0
        && strcmp(cfg->mode, "directional") != 0 && strcmp(cfg->mode, "off") != 0) {
        // fallback to safe default on unknown mode
        strncpy(cfg->mode, "cas", sizeof(cfg->mode) - 1);
        cfg->mode[sizeof(cfg->mode) - 1] = '\0';
        vsr_safety_set_error("unknown vsr mode, fallback to cas");
        valid = false;
    }
    // normalize backend string to lowercase
    for (size_t i = 0; cfg->backend[i] != '\0' && i < sizeof(cfg->backend); i++) {
        cfg->backend[i] = (char)tolower((unsigned char)cfg->backend[i]);
    }
    // validate backend selection
    if (strcmp(cfg->backend, "auto") != 0 && strcmp(cfg->backend, "amd") != 0
        && strcmp(cfg->backend, "nvidia") != 0 && strcmp(cfg->backend, "generic") != 0) {
        strncpy(cfg->backend, "auto", sizeof(cfg->backend) - 1);
        cfg->backend[sizeof(cfg->backend) - 1] = '\0';
        vsr_safety_set_error("unknown vsr backend, fallback to auto");
        valid = false;
    }
    // validate model path length
    size_t model_len = vsr_safe_strlen(cfg->model_path, sizeof(cfg->model_path));
    if (model_len >= sizeof(cfg->model_path)) {
        cfg->model_path[0] = '\0';
        vsr_safety_set_error("model path too long, cleared");
        valid = false;
    }
    return valid;
}

// resolve default config file path
bool vsr_config_default_path(char *out, size_t out_len) {
    // validate output buffer
    if (!out || out_len == 0) {
        return false;
    }
    // check explicit config override first
    const char *explicit_path = getenv("VSR_CONFIG");
    if (explicit_path && explicit_path[0] != '\0') {
        // copy explicit path with truncation guard
        strncpy(out, explicit_path, out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
    // resolve xdg base directory or home fallback
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    // build xdg-based path when available
    if (xdg && xdg[0] != '\0') {
        int written = snprintf(out, out_len, "%s/linux-vsr/config.ini", xdg);
        // validate snprintf result
        if (written <= 0 || (size_t)written >= out_len) {
            return false;
        }
        return true;
    }
    // build home-based path as fallback
    if (home && home[0] != '\0') {
        int written = snprintf(out, out_len, "%s/.config/linux-vsr/config.ini", home);
        // validate snprintf result
        if (written <= 0 || (size_t)written >= out_len) {
            return false;
        }
        return true;
    }
    return false;
}

// trim leading and trailing whitespace in place
static char *vsr_config_trim(char *str) {
    // handle null input safely
    if (!str) {
        return NULL;
    }
    // skip leading spaces
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    // handle empty string case
    if (*str == '\0') {
        return str;
    }
    // trim trailing spaces
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

// parse boolean value from string
static bool vsr_config_parse_bool(const char *val, bool *out) {
    // validate pointers
    if (!val || !out) {
        return false;
    }
    // accept truthy variants
    if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "on") == 0) {
        *out = true;
        return true;
    }
    // accept falsy variants
    if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0 || strcmp(val, "no") == 0 || strcmp(val, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

// parse float value with range check
static bool vsr_config_parse_float(const char *val, float min_v, float max_v, float *out) {
    // validate pointers
    if (!val || !out) {
        return false;
    }
    // reject empty string
    if (val[0] == '\0') {
        return false;
    }
    // convert with end pointer check
    char *endptr = NULL;
    float parsed = strtof(val, &endptr);
    // reject non-numeric input
    if (endptr == val || (endptr && *endptr != '\0')) {
        return false;
    }
    // validate finite range
    if (!vsr_safe_float_in_range(parsed, min_v, max_v)) {
        return false;
    }
    *out = parsed;
    return true;
}

// apply single key=value pair to config
static void vsr_config_apply_key(vsr_config_t *cfg, const char *key, const char *val) {
    // validate inputs
    if (!cfg || !key || !val) {
        return;
    }
    // match enable flag
    if (strcmp(key, "enable") == 0 || strcmp(key, "enabled") == 0) {
        vsr_config_parse_bool(val, &cfg->enabled);
        return;
    }
    // match debug flag
    if (strcmp(key, "debug") == 0) {
        vsr_config_parse_bool(val, &cfg->debug);
        return;
    }
    // match sharpness value
    if (strcmp(key, "sharpness") == 0) {
        vsr_config_parse_float(val, 0.0f, 0.50f, &cfg->sharpness);
        return;
    }
    // match watermark enable flag
    if (strcmp(key, "watermark") == 0 || strcmp(key, "watermark_enabled") == 0) {
        vsr_config_parse_bool(val, &cfg->watermark_enabled);
        return;
    }
    // match watermark opacity
    if (strcmp(key, "watermark_opacity") == 0) {
        vsr_config_parse_float(val, 0.05f, 1.0f, &cfg->watermark_opacity);
        return;
    }
    // match watermark size
    if (strcmp(key, "watermark_size") == 0) {
        vsr_config_parse_float(val, 0.02f, 0.15f, &cfg->watermark_size);
        return;
    }
    // match upscaler mode
    if (strcmp(key, "mode") == 0) {
        // copy with truncation guard
        strncpy(cfg->mode, val, sizeof(cfg->mode) - 1);
        cfg->mode[sizeof(cfg->mode) - 1] = '\0';
        return;
    }
    // match hardware backend selection
    if (strcmp(key, "backend") == 0) {
        // copy with truncation guard
        strncpy(cfg->backend, val, sizeof(cfg->backend) - 1);
        cfg->backend[sizeof(cfg->backend) - 1] = '\0';
        return;
    }
    // match optional model path
    if (strcmp(key, "model") == 0 || strcmp(key, "model_path") == 0) {
        // copy with truncation guard
        strncpy(cfg->model_path, val, sizeof(cfg->model_path) - 1);
        cfg->model_path[sizeof(cfg->model_path) - 1] = '\0';
        return;
    }
    // ignore unknown keys silently
}

// load configuration from ini-like key=value file
bool vsr_config_load_file(vsr_config_t *cfg, const char *path) {
    // validate inputs
    if (!cfg || !path || path[0] == '\0') {
        return false;
    }
    // reject overlong paths
    if (vsr_safe_strlen(path, VSR_MAX_PATH_LEN + 1) > VSR_MAX_PATH_LEN) {
        vsr_safety_set_error("config path too long");
        return false;
    }
    // open file for reading
    FILE *fp = fopen(path, "r");
    // missing file is not fatal, caller may use defaults
    if (!fp) {
        return false;
    }
    // read line by line with bounded buffer
    char line[VSR_MAX_CONFIG_LINE];
    while (fgets(line, sizeof(line), fp)) {
        // detect truncated overlong lines
        size_t linelen = strlen(line);
        if (linelen > 0 && line[linelen - 1] != '\n' && !feof(fp)) {
            // skip remainder of overlong line
            int ch = 0;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) {
            }
            continue;
        }
        // trim whitespace
        char *trimmed = vsr_config_trim(line);
        // skip empty lines and comments
        if (!trimmed || trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        // skip section headers like [vsr]
        if (trimmed[0] == '[') {
            continue;
        }
        // split key and value on first delimiter
        char *sep = strchr(trimmed, '=');
        if (!sep) {
            sep = strchr(trimmed, ':');
        }
        if (!sep) {
            continue;
        }
        // terminate key and advance to value
        *sep = '\0';
        char *key = vsr_config_trim(trimmed);
        char *val = vsr_config_trim(sep + 1);
        // validate split result
        if (!key || !val || key[0] == '\0') {
            continue;
        }
        // normalize key to lowercase
        for (char *p = key; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        // apply parsed pair
        vsr_config_apply_key(cfg, key, val);
    }
    // close file handle
    fclose(fp);
    // validate final state
    vsr_config_validate(cfg);
    return true;
}

// save configuration to key=value file
bool vsr_config_save(const vsr_config_t *cfg, const char *path) {
    // validate inputs
    if (!cfg || !path || path[0] == '\0') {
        vsr_safety_set_error("config save null input");
        return false;
    }
    // open file for writing
    FILE *fp = fopen(path, "w");
    // handle open failure
    if (!fp) {
        vsr_safety_set_error("cannot open config for writing");
        return false;
    }
    // pin numeric locale so saved decimals always use dots
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t saved_locale = (locale_t)0;
    if (c_locale != (locale_t)0) {
        saved_locale = uselocale(c_locale);
    }
    // write header comment
    fprintf(fp, "# linux-vsr configuration (generated)\n");
    // write core toggles
    fprintf(fp, "enable=%d\n", cfg->enabled ? 1 : 0);
    fprintf(fp, "debug=%d\n", cfg->debug ? 1 : 0);
    fprintf(fp, "sharpness=%.4f\n", (double)cfg->sharpness);
    // write upscaler mode
    fprintf(fp, "mode=%s\n", cfg->mode);
    // write hardware backend selection
    fprintf(fp, "backend=%s\n", cfg->backend);
    // write watermark block
    fprintf(fp, "watermark=%d\n", cfg->watermark_enabled ? 1 : 0);
    fprintf(fp, "watermark_opacity=%.4f\n", (double)cfg->watermark_opacity);
    fprintf(fp, "watermark_size=%.4f\n", (double)cfg->watermark_size);
    // write optional model path
    if (cfg->model_path[0] != '\0') {
        fprintf(fp, "model=%s\n", cfg->model_path);
    }
    // restore thread locale before closing
    if (c_locale != (locale_t)0) {
        uselocale(saved_locale);
        freelocale(c_locale);
    }
    // close file handle
    fclose(fp);
    return true;
}

// helper to read env with fallback prefix
static const char *vsr_config_getenv2(const char *primary, const char *fallback) {
    // try primary name first
    const char *val = getenv(primary);
    if (val) {
        return val;
    }
    // try legacy fallback name
    if (fallback) {
        return getenv(fallback);
    }
    return NULL;
}

// parse environment variables and populate configuration structure
void vsr_config_load(vsr_config_t *cfg) {
    // handle null pointer safely
    if (!cfg) {
        vsr_safety_set_error("config null pointer in load");
        return;
    }
    // initialize baseline defaults
    vsr_config_init_defaults(cfg);
    // try loading from default file location
    char cfg_path[VSR_MAX_PATH_LEN];
    if (vsr_config_default_path(cfg_path, sizeof(cfg_path))) {
        // ignore missing file, env still applies
        vsr_config_load_file(cfg, cfg_path);
    }
    // check if vsr is explicitly disabled
    const char *enable_env = vsr_config_getenv2("VSR_ENABLE", "ZEN_VSR_ENABLE");
    if (enable_env) {
        bool parsed = false;
        if (vsr_config_parse_bool(enable_env, &parsed)) {
            cfg->enabled = parsed;
        }
    }
    // check if debug output is disabled
    const char *debug_env = vsr_config_getenv2("VSR_DEBUG", "ZEN_VSR_DEBUG");
    if (debug_env) {
        bool parsed = false;
        if (vsr_config_parse_bool(debug_env, &parsed)) {
            cfg->debug = parsed;
        }
    }
    // check custom sharpness setting
    const char *sharp_env = vsr_config_getenv2("VSR_SHARPNESS", "ZEN_VSR_SHARPNESS");
    if (sharp_env) {
        float val = 0.0f;
        if (vsr_config_parse_float(sharp_env, 0.0f, 0.50f, &val)) {
            cfg->sharpness = val;
        }
    }
    // check watermark toggle
    const char *wm_env = getenv("VSR_WATERMARK");
    if (wm_env) {
        bool parsed = false;
        if (vsr_config_parse_bool(wm_env, &parsed)) {
            cfg->watermark_enabled = parsed;
        }
    }
    // check watermark opacity override
    const char *wm_op = getenv("VSR_WATERMARK_OPACITY");
    if (wm_op) {
        float val = 0.0f;
        if (vsr_config_parse_float(wm_op, 0.05f, 1.0f, &val)) {
            cfg->watermark_opacity = val;
        }
    }
    // check watermark size override
    const char *wm_sz = getenv("VSR_WATERMARK_SIZE");
    if (wm_sz) {
        float val = 0.0f;
        if (vsr_config_parse_float(wm_sz, 0.02f, 0.15f, &val)) {
            cfg->watermark_size = val;
        }
    }
    // check upscaler mode override
    const char *mode_env = getenv("VSR_MODE");
    if (mode_env && mode_env[0] != '\0') {
        strncpy(cfg->mode, mode_env, sizeof(cfg->mode) - 1);
        cfg->mode[sizeof(cfg->mode) - 1] = '\0';
    }
    // check hardware backend override
    const char *backend_env = getenv("VSR_BACKEND");
    if (backend_env && backend_env[0] != '\0') {
        strncpy(cfg->backend, backend_env, sizeof(cfg->backend) - 1);
        cfg->backend[sizeof(cfg->backend) - 1] = '\0';
    }
    // check optional model path override
    const char *model_env = getenv("VSR_MODEL");
    if (model_env && model_env[0] != '\0') {
        strncpy(cfg->model_path, model_env, sizeof(cfg->model_path) - 1);
        cfg->model_path[sizeof(cfg->model_path) - 1] = '\0';
    }
    // validate final merged state
    vsr_config_validate(cfg);
    // clear stale error on success path
    vsr_safety_clear_error();
}

// reload global configuration from file and env
bool vsr_config_reload(void) {
    // reload into global singleton
    vsr_config_load(&g_config);
    return true;
}
