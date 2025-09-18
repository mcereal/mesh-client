#include "mesh/transport/ble.h"

#include "mesh/config.h"
#include "mesh/log.h"

#include <stdbool.h>
#include <string.h>

struct mesh_ble_transport_state {
    bool running;
};

static int mesh_ble_start(struct mesh_transport *transport, const struct mesh_app_config *config,
                          struct mesh_event_loop *loop) {
    (void)loop;

    if (transport == NULL || config == NULL) {
        return -1;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (!config->enable_ble) {
        mesh_log_info("ble", "BLE transport disabled by configuration");
        state->running = false;
        return 0;
    }

    state->running = true;
    if (config->preferred_ble_device[0] != '\0') {
        mesh_log_info("ble", "Attempting to connect to preferred device '%s'", config->preferred_ble_device);
    } else {
        mesh_log_info("ble", "Scanning for nearby Meshtastic nodes (stub)");
    }

    // TODO: hook into BlueZ via D-Bus and register descriptors with the event loop.
    return 0;
}

static void mesh_ble_stop(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (!state->running) {
        return;
    }

    state->running = false;
    mesh_log_info("ble", "BLE transport stopped");
}

static const char *mesh_ble_status(const struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return "unknown";
    }

    const struct mesh_ble_transport_state *state = (const struct mesh_ble_transport_state *)transport->state;
    return state->running ? "running" : "stopped";
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
