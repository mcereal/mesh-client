#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_app_run_mode {
    MESH_APP_RUN_SINGLE_POLL = 0,
    MESH_APP_RUN_FOREGROUND
};

struct mesh_app_config {
    enum mesh_app_run_mode run_mode;
    int idle_timeout_ms;
    bool enable_ble;
    char preferred_ble_device[64];
};

struct mesh_app_config mesh_app_config_default(void);
void mesh_app_config_apply_env_overrides(struct mesh_app_config *config);

#ifdef __cplusplus
}
#endif
