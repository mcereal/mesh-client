#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/ble.h"

#include "mesh/config.h"
#include "mesh/log.h"

#include "mesh/transport/ble_bluez.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

enum mesh_ble_state {
    MESH_BLE_STATE_DISABLED = 0,
    MESH_BLE_STATE_IDLE,
    MESH_BLE_STATE_WAITING_FOR_BLUEZ,
    MESH_BLE_STATE_WAITING_FOR_ADAPTER,
    MESH_BLE_STATE_READY,
};

struct mesh_ble_transport_state {
    enum mesh_ble_state state;
    bool client_initialised;
    bool discovery_active;
    char adapter_path[128];
    struct mesh_bluez_client bluez;
    struct mesh_bluez_device_info devices[16];
    size_t device_count;
    int refresh_timer_fd;
    struct mesh_event_loop *loop;
};

static const char *mesh_ble_state_to_string(enum mesh_ble_state state) {
    switch (state) {
        case MESH_BLE_STATE_DISABLED:
            return "disabled";
        case MESH_BLE_STATE_IDLE:
            return "inactive";
        case MESH_BLE_STATE_WAITING_FOR_BLUEZ:
            return "waiting-for-bluez";
        case MESH_BLE_STATE_WAITING_FOR_ADAPTER:
            return "waiting-for-adapter";
        case MESH_BLE_STATE_READY:
            return "running";
    }
    return "unknown";
}

static size_t mesh_ble_refresh_devices_internal(struct mesh_transport *transport);
static void mesh_ble_tick(struct mesh_transport *transport) {
    mesh_ble_refresh_devices_internal(transport);
}

static int mesh_ble_refresh_timer_callback(int fd, uint32_t events, void *userdata) {
    (void)events;
    struct mesh_transport *transport = (struct mesh_transport *)userdata;
    if (transport == NULL) {
        return 0;
    }
    uint64_t expirations = 0;
    ssize_t read_result = read(fd, &expirations, sizeof(expirations));
    if (read_result < 0 && errno != EAGAIN) {
        mesh_log_warn("ble", "refresh timer read failed: %s", strerror(errno));
    }
    mesh_ble_refresh_devices_internal(transport);
    return 0;
}

static int mesh_ble_setup_refresh_timer(struct mesh_transport *transport, struct mesh_ble_transport_state *state,
                                        struct mesh_event_loop *loop) {
    if (loop == NULL) {
        return 0;
    }

    state->refresh_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (state->refresh_timer_fd < 0) {
        mesh_log_warn("ble", "timerfd_create failed: %s", strerror(errno));
        return -errno;
    }

    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = 5;
    spec.it_interval.tv_sec = 5;
    if (timerfd_settime(state->refresh_timer_fd, 0, &spec, NULL) < 0) {
        mesh_log_warn("ble", "timerfd_settime failed: %s", strerror(errno));
        close(state->refresh_timer_fd);
        state->refresh_timer_fd = -1;
        return -errno;
    }

    int add_result = mesh_event_loop_add_fd(loop, state->refresh_timer_fd, EPOLLIN, mesh_ble_refresh_timer_callback,
                                            transport);
    if (add_result < 0) {
        mesh_log_warn("ble", "Failed to add refresh timer fd: %d", add_result);
        close(state->refresh_timer_fd);
        state->refresh_timer_fd = -1;
        return add_result;
    }

    state->loop = loop;
    return 0;
}

static void mesh_ble_teardown_refresh_timer(struct mesh_ble_transport_state *state) {
    if (state->refresh_timer_fd >= 0) {
        if (state->loop != NULL) {
            mesh_event_loop_remove_fd(state->loop, state->refresh_timer_fd);
        }
        close(state->refresh_timer_fd);
        state->refresh_timer_fd = -1;
    }
    state->loop = NULL;
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
    state->discovery_active = false;
    state->adapter_path[0] = '\0';
    state->device_count = 0;
    state->refresh_timer_fd = -1;
    state->loop = NULL;

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

    char adapter_path[sizeof(state->adapter_path)];
    int adapter_result = mesh_bluez_client_find_adapter(&state->bluez, adapter_path, sizeof(adapter_path));
    if (adapter_result < 0) {
        if (adapter_result == -ENODEV) {
            mesh_log_warn("ble", "No BlueZ adapters available; waiting for device");
            state->state = MESH_BLE_STATE_WAITING_FOR_ADAPTER;
        } else if (adapter_result == -ENOTCONN) {
            mesh_log_warn("ble", "BlueZ client disconnected before adapter search");
            state->state = MESH_BLE_STATE_WAITING_FOR_BLUEZ;
        } else if (adapter_result == -ENOSYS) {
            mesh_log_warn("ble", "Adapter search unsupported on this build");
            state->state = MESH_BLE_STATE_WAITING_FOR_BLUEZ;
        } else {
            mesh_log_warn("ble", "Adapter search failed: %s", strerror(-adapter_result));
            state->state = MESH_BLE_STATE_WAITING_FOR_ADAPTER;
        }
        return 0;
    }

    snprintf(state->adapter_path, sizeof(state->adapter_path), "%s", adapter_path);
    int discovery_result = mesh_bluez_client_start_discovery(&state->bluez, state->adapter_path);
    if (discovery_result < 0) {
        mesh_log_warn("ble", "StartDiscovery failed on %s: %s", state->adapter_path,
                      strerror(-discovery_result));
        state->state = MESH_BLE_STATE_WAITING_FOR_ADAPTER;
        return 0;
    }

    state->discovery_active = true;
    mesh_ble_refresh_devices_internal(transport);

    if (mesh_ble_setup_refresh_timer(transport, state, loop) < 0) {
        mesh_log_debug("ble", "Refresh timer unavailable; continuing without periodic updates");
    }

    state->state = MESH_BLE_STATE_READY;
    if (config->preferred_ble_device[0] != '\0') {
        mesh_log_info("ble", "Attempting to connect to preferred device '%s'", config->preferred_ble_device);
    } else {
        mesh_log_info("ble", "Scanning for Meshtastic nodes via %s", state->adapter_path);
    }

    // TODO: hook into BlueZ via D-Bus and register descriptors with the event loop.
    return 0;
}

static void mesh_ble_stop(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (state->discovery_active && state->adapter_path[0] != '\0') {
        int stop_result = mesh_bluez_client_stop_discovery(&state->bluez, state->adapter_path);
        if (stop_result < 0) {
            mesh_log_warn("ble", "StopDiscovery failed on %s: %s", state->adapter_path,
                          strerror(-stop_result));
        }
    }

    mesh_ble_teardown_refresh_timer(state);

    if (state->client_initialised) {
        mesh_bluez_client_shutdown(&state->bluez);
        state->client_initialised = false;
    }

    state->state = MESH_BLE_STATE_IDLE;
    state->discovery_active = false;
    state->adapter_path[0] = '\0';
    state->device_count = 0;
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
    .tick = mesh_ble_tick,
};

size_t mesh_ble_transport_get_devices(struct mesh_transport *transport, struct mesh_bluez_device_info *out,
                                      size_t capacity) {
    if (transport == NULL || out == NULL || capacity == 0U) {
        return 0U;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    size_t to_copy = state->device_count;
    if (to_copy > capacity) {
        to_copy = capacity;
    }
    for (size_t i = 0; i < to_copy; ++i) {
        out[i] = state->devices[i];
    }
    return to_copy;
}

const struct mesh_bluez_device_info *mesh_ble_transport_devices(struct mesh_transport *transport, size_t *count) {
    if (transport == NULL || count == NULL) {
        if (count != NULL) {
            *count = 0U;
        }
        return NULL;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    *count = state->device_count;
    return state->devices;
}

static size_t mesh_ble_refresh_devices_internal(struct mesh_transport *transport) {
    if (transport == NULL) {
        return 0U;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    size_t device_count = 0;
    int list_result = mesh_bluez_client_list_meshtastic(&state->bluez, state->devices,
                                                        sizeof(state->devices) / sizeof(state->devices[0]),
                                                        &device_count);
    if (list_result < 0) {
        mesh_log_debug("ble", "Device enumeration failed: %s", strerror(-list_result));
        state->device_count = 0;
        return 0U;
    }

    if (device_count != state->device_count) {
        mesh_log_info("ble", "Discovered %zu meshtastic device(s)", device_count);
    }
    state->device_count = device_count;
    for (size_t i = 0; i < state->device_count; ++i) {
        mesh_log_debug("ble", "  %s (%s) RSSI=%d", state->devices[i].name, state->devices[i].address,
                       (int)state->devices[i].rssi);
    }
    return state->device_count;
}

size_t mesh_ble_transport_refresh_devices(struct mesh_transport *transport) {
    return mesh_ble_refresh_devices_internal(transport);
}

struct mesh_transport *mesh_ble_transport(void) {
    static struct mesh_ble_transport_state state;
    static struct mesh_transport transport = {
        .name = "ble",
        .state = &state,
        .ops = &k_ble_ops,
    };
    return &transport;
}
