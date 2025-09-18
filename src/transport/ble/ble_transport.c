#include "mesh/transport/ble.h"

#include "mesh/config.h"
#include "mesh/log.h"
#include "mesh/transport/ble_bluez.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

enum mesh_ble_state {
    MESH_BLE_STATE_DISABLED = 0,
    MESH_BLE_STATE_IDLE,
    MESH_BLE_STATE_WAITING_FOR_BLUEZ,
    MESH_BLE_STATE_READY,
};

struct mesh_ble_transport_state {
    enum mesh_ble_state state;
    bool client_initialised;
    struct mesh_bluez_client bluez;
};

static const char *mesh_ble_state_to_string(enum mesh_ble_state state) {
    switch (state) {
        case MESH_BLE_STATE_DISABLED:
            return "disabled";
        case MESH_BLE_STATE_IDLE:
            return "inactive";
        case MESH_BLE_STATE_WAITING_FOR_BLUEZ:
            return "waiting-for-bluez";
        case MESH_BLE_STATE_READY:
            return "running";
    }
    return "unknown";
}

static int mesh_ble_start(struct mesh_transport *transport, const struct mesh_app_config *config,
                          struct mesh_event_loop *loop) {
    (void)loop;

    if (transport == NULL || config == NULL) {
        return -EINVAL;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    state->state = MESH_BLE_STATE_IDLE;
    state->client_initialised = false;

    if (!config->enable_ble) {
        mesh_log_info("ble", "BLE transport disabled by configuration");
        state->state = MESH_BLE_STATE_DISABLED;
        return 0;
    }

    const int init_result = mesh_bluez_client_init(&state->bluez);
    if (init_result < 0) {
        if (init_result == -ENOSYS) {
            mesh_log_warn("ble", "BLE transport built without D-Bus support; skipping BlueZ startup");
            state->state = MESH_BLE_STATE_IDLE;
            return 0;
        }

        mesh_log_warn("ble", "Failed to initialise BlueZ client: %s", strerror(-init_result));
        state->state = MESH_BLE_STATE_IDLE;
        return 0;
    }

    state->client_initialised = true;
    state->state = MESH_BLE_STATE_WAITING_FOR_BLUEZ;

    const int ready_result = mesh_bluez_client_check_ready(&state->bluez);
    if (ready_result < 0) {
        if (ready_result == -ENODEV) {
            mesh_log_warn("ble", "BlueZ service not present; BLE transport idle");
            state->state = MESH_BLE_STATE_WAITING_FOR_BLUEZ;
        } else if (ready_result == -ENOTCONN) {
            mesh_log_warn("ble", "BlueZ client not connected");
        } else if (ready_result == -ENOSYS) {
            mesh_log_warn("ble", "BlueZ readiness check unsupported on this build");
        } else {
            mesh_log_warn("ble", "Error talking to BlueZ: %s", strerror(-ready_result));
        }
        return 0;
    }

    state->state = MESH_BLE_STATE_READY;
    if (config->preferred_ble_device[0] != '\0') {
        mesh_log_info("ble", "Attempting to connect to preferred device '%s'", config->preferred_ble_device);
    } else {
        mesh_log_info("ble", "BlueZ detected; ready to scan for Meshtastic nodes (stub)");
    }

    // TODO: hook into BlueZ via D-Bus and register descriptors with the event loop.
    return 0;
}

static void mesh_ble_stop(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (state->client_initialised) {
        mesh_bluez_client_shutdown(&state->bluez);
        state->client_initialised = false;
    }

    state->state = MESH_BLE_STATE_IDLE;
    mesh_log_info("ble", "BLE transport stopped");
}

static const char *mesh_ble_status(const struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return "unknown";
    }

    const struct mesh_ble_transport_state *state = (const struct mesh_ble_transport_state *)transport->state;
    return mesh_ble_state_to_string(state->state);
}

static const struct mesh_transport_ops k_ble_ops = {
    .start = mesh_ble_start,
    .stop = mesh_ble_stop,
    .status = mesh_ble_status,
};

struct mesh_transport *mesh_ble_transport(void) {
    static struct mesh_ble_transport_state state;
    static struct mesh_transport transport = {
        .name = "ble",
        .state = &state,
        .ops = &k_ble_ops,
    };
    return &transport;
}
