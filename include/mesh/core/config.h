#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_app_run_mode { MESH_APP_RUN_SINGLE_POLL = 0, MESH_APP_RUN_FOREGROUND };

struct mesh_app_config {
    enum mesh_app_run_mode run_mode;
    int idle_timeout_ms;
    bool enable_ble;
    char preferred_ble_device[64];
    bool enable_serial;
    /* sysfs interface id ("1-1:1.1") or device node ("/dev/ttyUSB0"). */
    char preferred_serial_device[64];
    bool enable_tcp;
    /*
     * A numeric address with an optional port: "192.168.1.50", "192.168.1.50:4403" or
     * "[fd00::1]:4403". Empty means the network link has nothing to connect to, which is its
     * resting state - a network cannot be scanned for radios.
     *
     * 64 matches MESH_TCP_TARGET_MAX, declared separately for the reason the other two are:
     * this header is reached from everywhere and mesh/transport/tcp.h drags in the generated
     * protobuf headers behind it. tcp_target_max_agrees_with_the_config is what holds the two
     * honest.
     */
    char preferred_tcp_host[64];
};

struct mesh_app_config mesh_app_config_default(void);
void mesh_app_config_apply_env_overrides(struct mesh_app_config *config);

#ifdef __cplusplus
}
#endif
