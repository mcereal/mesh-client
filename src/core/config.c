#include "mesh/core/config.h"

#include "mesh/utils/env.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mesh_app_config mesh_app_config_default(void) {
    struct mesh_app_config config;
    config.run_mode = MESH_APP_RUN_SINGLE_POLL;
    config.idle_timeout_ms = 1000;
    config.enable_ble = true;
    config.preferred_ble_device[0] = '\0';
    config.enable_serial = true;
    config.preferred_serial_device[0] = '\0';
    return config;
}

static void lowercase(char *buffer) {
    for (; *buffer != '\0'; ++buffer) {
        *buffer = (char)tolower((unsigned char)*buffer);
    }
}

/* MESHCLIENT_DISABLE_<X>: truthy turns the transport off, so the flag is the negation of the
   `enable_*` field it lands in. */
static void apply_disable_override(const char *env_name, const char *label, bool *enabled) {
    *enabled = !mesh_env_bool(env_name, label, !*enabled);
}

void mesh_app_config_apply_env_overrides(struct mesh_app_config *config) {
    if (config == NULL) {
        return;
    }

    const char *run_mode_env = getenv("MESHCLIENT_RUN_MODE");
    if (run_mode_env != NULL && run_mode_env[0] != '\0') {
        char mode[32];
        mesh_str_copy(mode, sizeof mode, run_mode_env);
        lowercase(mode);
        if (strcmp(mode, "foreground") == 0) {
            config->run_mode = MESH_APP_RUN_FOREGROUND;
        } else if (strcmp(mode, "single_poll") == 0) {
            config->run_mode = MESH_APP_RUN_SINGLE_POLL;
        } else {
            mesh_log_warn("config", "Unknown run mode '%s', using default", run_mode_env);
        }
    }

    config->idle_timeout_ms =
        (int)mesh_env_int("MESHCLIENT_IDLE_TIMEOUT_MS", INT_MIN, INT_MAX, config->idle_timeout_ms);

    apply_disable_override("MESHCLIENT_DISABLE_BLE", "BLE", &config->enable_ble);
    apply_disable_override("MESHCLIENT_DISABLE_SERIAL", "serial", &config->enable_serial);

    const char *preferred_env = getenv("MESHCLIENT_PREFERRED_BLE_DEVICE");
    if (preferred_env != NULL) {
        mesh_str_copy(config->preferred_ble_device, sizeof config->preferred_ble_device,
                      preferred_env);
    }

    const char *preferred_serial_env = getenv("MESHCLIENT_PREFERRED_SERIAL_DEVICE");
    if (preferred_serial_env != NULL) {
        mesh_str_copy(config->preferred_serial_device, sizeof config->preferred_serial_device,
                      preferred_serial_env);
    }
}
