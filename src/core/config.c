#include "mesh/config.h"

#include "mesh/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mesh_app_config mesh_app_config_default(void) {
    struct mesh_app_config config;
    config.run_mode = MESH_APP_RUN_SINGLE_POLL;
    config.idle_timeout_ms = 1000;
    config.enable_ble = true;
    config.preferred_ble_device[0] = '\0';
    return config;
}

static int parse_int(const char *value, int fallback) {
    if (value == NULL || *value == '\0') {
        return fallback;
    }

    char *endptr = NULL;
    long parsed = strtol(value, &endptr, 10);
    if (endptr == value || *endptr != '\0') {
        mesh_log_warn("config", "Invalid integer '%s', keeping %d", value, fallback);
        return fallback;
    }
    return (int)parsed;
}

static void lowercase(char *buffer) {
    for (; *buffer != '\0'; ++buffer) {
        *buffer = (char)tolower((unsigned char)*buffer);
    }
}

void mesh_app_config_apply_env_overrides(struct mesh_app_config *config) {
    if (config == NULL) {
        return;
    }

    const char *run_mode_env = getenv("MESHCLIENT_RUN_MODE");
    if (run_mode_env != NULL && run_mode_env[0] != '\0') {
        char mode[32];
        snprintf(mode, sizeof mode, "%s", run_mode_env);
        lowercase(mode);
        if (strcmp(mode, "foreground") == 0) {
            config->run_mode = MESH_APP_RUN_FOREGROUND;
        } else if (strcmp(mode, "single_poll") == 0) {
            config->run_mode = MESH_APP_RUN_SINGLE_POLL;
        } else {
            mesh_log_warn("config", "Unknown run mode '%s', using default", run_mode_env);
        }
    }

    const char *timeout_env = getenv("MESHCLIENT_IDLE_TIMEOUT_MS");
    if (timeout_env != NULL) {
        config->idle_timeout_ms = parse_int(timeout_env, config->idle_timeout_ms);
    }

    const char *disable_ble_env = getenv("MESHCLIENT_DISABLE_BLE");
    if (disable_ble_env != NULL && disable_ble_env[0] != '\0') {
        char value[8];
        snprintf(value, sizeof value, "%s", disable_ble_env);
        lowercase(value);
        if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0) {
            config->enable_ble = false;
        } else if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0) {
            config->enable_ble = true;
        } else {
            mesh_log_warn("config", "Unknown boolean '%s', keeping BLE %s", disable_ble_env,
                          config->enable_ble ? "enabled" : "disabled");
        }
    }

    const char *preferred_env = getenv("MESHCLIENT_PREFERRED_BLE_DEVICE");
    if (preferred_env != NULL) {
        strncpy(config->preferred_ble_device, preferred_env, sizeof(config->preferred_ble_device) - 1U);
        config->preferred_ble_device[sizeof(config->preferred_ble_device) - 1U] = '\0';
    }
}
