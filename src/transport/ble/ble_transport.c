#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/ble.h"

#include "mesh/config.h"
#include "mesh/log.h"

#include "mesh/transport/ble_bluez.h"
#include "mesh/proto/framing.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define MESH_BLE_DEFAULT_MTU 20U
#define MESH_BLE_MAX_OUTBOUND_SEGMENTS 8U
#define MESH_BLE_MAX_SEGMENT_SIZE 244U

enum mesh_ble_state {
    MESH_BLE_STATE_DISABLED = 0,
    MESH_BLE_STATE_IDLE,
    MESH_BLE_STATE_WAITING_FOR_BLUEZ,
    MESH_BLE_STATE_WAITING_FOR_ADAPTER,
    MESH_BLE_STATE_READY,
};

enum mesh_ble_link_state {
    MESH_BLE_LINK_DISCONNECTED = 0,
    MESH_BLE_LINK_CONNECTING,
    MESH_BLE_LINK_CONNECTED,
};

struct mesh_ble_outbound_segment {
    size_t length;
    size_t offset;
    uint8_t data[MESH_BLE_MAX_SEGMENT_SIZE];
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
    enum mesh_ble_link_state link_state;
    char connected_address[32];
    char connected_device_path[128];
    bool notifications_enabled;
    char rx_char_path[128];
    char tx_char_path[128];
    uint8_t rx_buffer[1024];
    size_t rx_buffer_len;
    size_t frames_received;
    size_t bytes_received;
    uint32_t next_config_request_id;
    struct mesh_ble_handshake_status handshake;
    size_t write_mtu;
    struct mesh_ble_outbound_segment write_queue[MESH_BLE_MAX_OUTBOUND_SEGMENTS];
    size_t write_queue_head;
    size_t write_queue_len;
};

static const char *k_mesh_ble_nus_rx_uuid __attribute__((unused)) = MESH_BLE_NUS_RX_UUID;
static const char *k_mesh_ble_nus_tx_uuid = MESH_BLE_NUS_TX_UUID;

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
static void mesh_ble_reset_handshake(struct mesh_ble_transport_state *state);
static int mesh_ble_begin_handshake(struct mesh_ble_transport_state *state);
static void mesh_ble_handle_from_radio(struct mesh_ble_transport_state *state, const uint8_t *payload, size_t len);
static void mesh_ble_store_node_summary(struct mesh_ble_transport_state *state, const meshtastic_NodeInfo *info);
static void mesh_ble_handle_log_record(const meshtastic_LogRecord *record);
static void mesh_ble_clear_write_queue(struct mesh_ble_transport_state *state);
static size_t mesh_ble_effective_mtu(const struct mesh_ble_transport_state *state);
static int mesh_ble_queue_frame(struct mesh_ble_transport_state *state, const uint8_t *frame, size_t len);
static void mesh_ble_flush_write_queue(struct mesh_ble_transport_state *state);
static void mesh_ble_process_rx_buffer(struct mesh_ble_transport_state *state);
static void mesh_ble_notification_handler(const uint8_t *data, size_t len, void *userdata);
static void mesh_ble_tick(struct mesh_transport *transport) {
    if (transport == NULL) {
        return;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (state != NULL && state->client_initialised) {
        mesh_bluez_client_process(&state->bluez);
        mesh_ble_flush_write_queue(state);
    }

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
}

static int mesh_ble_start(struct mesh_transport *transport, const struct mesh_app_config *config,
                          struct mesh_event_loop *loop) {
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
    state->loop = loop;
    state->link_state = MESH_BLE_LINK_DISCONNECTED;
    state->connected_address[0] = '\0';
    state->connected_device_path[0] = '\0';
    state->notifications_enabled = false;
    state->rx_char_path[0] = '\0';
    state->tx_char_path[0] = '\0';
    state->rx_buffer_len = 0U;
    state->frames_received = 0U;
    state->bytes_received = 0U;
    state->next_config_request_id = 1U;
    mesh_ble_reset_handshake(state);
    state->write_mtu = MESH_BLE_DEFAULT_MTU;
    mesh_ble_clear_write_queue(state);

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

    if (loop != NULL) {
        int attach_result = mesh_bluez_client_attach_loop(&state->bluez, loop);
        if (attach_result < 0) {
            mesh_log_warn("ble", "Failed to attach BlueZ client to event loop: %d", attach_result);
        }
    }

    mesh_bluez_client_set_notification_handler(&state->bluez, mesh_ble_notification_handler, state);

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
        if (state->link_state == MESH_BLE_LINK_CONNECTED) {
            mesh_bluez_client_disconnect(&state->bluez, state->connected_device_path);
        }
        mesh_bluez_client_set_notification_handler(&state->bluez, NULL, NULL);
        mesh_bluez_client_detach_loop(&state->bluez);
        mesh_bluez_client_shutdown(&state->bluez);
        state->client_initialised = false;
    }

    state->state = MESH_BLE_STATE_IDLE;
    state->discovery_active = false;
    state->adapter_path[0] = '\0';
    state->device_count = 0;
    state->link_state = MESH_BLE_LINK_DISCONNECTED;
    state->connected_address[0] = '\0';
    state->connected_device_path[0] = '\0';
    state->notifications_enabled = false;
    state->rx_char_path[0] = '\0';
    state->tx_char_path[0] = '\0';
    state->rx_buffer_len = 0U;
    state->frames_received = 0U;
    state->bytes_received = 0U;
    state->next_config_request_id = 1U;
    mesh_ble_reset_handshake(state);
    mesh_ble_clear_write_queue(state);
    state->write_mtu = MESH_BLE_DEFAULT_MTU;
    state->loop = NULL;
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

static bool mesh_ble_format_device_path(const struct mesh_ble_transport_state *state, const char *address,
                                        char *out_path, size_t out_len) {
    if (state->adapter_path[0] == '\0' || address == NULL || out_path == NULL) {
        return false;
    }
    char address_copy[32];
    snprintf(address_copy, sizeof(address_copy), "%s", address);
    for (char *c = address_copy; *c != '\0'; ++c) {
        if (*c == ':') {
            *c = '_';
        }
    }
    snprintf(out_path, out_len, "%s/dev_%s", state->adapter_path, address_copy);
    return true;
}

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

static void mesh_ble_reset_handshake(struct mesh_ble_transport_state *state) {
    if (state == NULL) {
        return;
    }

    memset(&state->handshake, 0, sizeof(state->handshake));
}

static void mesh_ble_clear_write_queue(struct mesh_ble_transport_state *state) {
    if (state == NULL) {
        return;
    }

    state->write_queue_head = 0U;
    state->write_queue_len = 0U;
}

static size_t mesh_ble_effective_mtu(const struct mesh_ble_transport_state *state) {
    if (state == NULL || state->write_mtu == 0U) {
        return MESH_BLE_DEFAULT_MTU;
    }
    if (state->write_mtu > MESH_BLE_MAX_SEGMENT_SIZE) {
        return MESH_BLE_MAX_SEGMENT_SIZE;
    }
    return state->write_mtu;
}

static void mesh_ble_flush_write_queue(struct mesh_ble_transport_state *state) {
    if (state == NULL || state->write_queue_len == 0U || !state->client_initialised || state->rx_char_path[0] == '\0') {
        return;
    }

    while (state->write_queue_len > 0U) {
        struct mesh_ble_outbound_segment *segment =
            &state->write_queue[(state->write_queue_head) % MESH_BLE_MAX_OUTBOUND_SEGMENTS];

        size_t remaining = segment->length - segment->offset;
        if (remaining == 0U) {
            state->write_queue_head = (state->write_queue_head + 1U) % MESH_BLE_MAX_OUTBOUND_SEGMENTS;
            state->write_queue_len--;
            continue;
        }

        int result = mesh_bluez_client_write(&state->bluez, state->rx_char_path, MESH_BLE_NUS_RX_UUID,
                                             segment->data + segment->offset, remaining);
        if (result < 0) {
            mesh_log_warn("ble", "write failed: %d", result);
            mesh_ble_clear_write_queue(state);
            return;
        }

        state->write_queue_head = (state->write_queue_head + 1U) % MESH_BLE_MAX_OUTBOUND_SEGMENTS;
        state->write_queue_len--;
    }
}

static int mesh_ble_queue_frame(struct mesh_ble_transport_state *state, const uint8_t *frame, size_t len) {
    if (state == NULL || frame == NULL || len == 0U) {
        return -EINVAL;
    }

    size_t mtu = mesh_ble_effective_mtu(state);
    size_t position = 0U;

    while (position < len) {
        size_t chunk = len - position;
        if (chunk > mtu) {
            chunk = mtu;
        }

        if (state->write_queue_len >= MESH_BLE_MAX_OUTBOUND_SEGMENTS) {
            mesh_log_warn("ble", "write queue full, dropping %zu bytes", len - position);
            return -ENOSPC;
        }

        size_t index = (state->write_queue_head + state->write_queue_len) % MESH_BLE_MAX_OUTBOUND_SEGMENTS;
        struct mesh_ble_outbound_segment *segment = &state->write_queue[index];
        segment->length = chunk;
        segment->offset = 0U;
        memcpy(segment->data, frame + position, chunk);
        state->write_queue_len++;
        position += chunk;
    }

    mesh_ble_flush_write_queue(state);
    return 0;
}

static void mesh_ble_store_node_summary(struct mesh_ble_transport_state *state, const meshtastic_NodeInfo *info) {
    if (state == NULL || info == NULL) {
        return;
    }

    if (state->handshake.node_count > MESH_BLE_MAX_NODE_SUMMARY) {
        state->handshake.node_count = MESH_BLE_MAX_NODE_SUMMARY;
    }

    size_t index = state->handshake.node_count;
    for (size_t i = 0; i < state->handshake.node_count; ++i) {
        if (state->handshake.nodes[i].node_id == info->num) {
            index = i;
            break;
        }
    }

    if (index == state->handshake.node_count) {
        if (state->handshake.node_count >= MESH_BLE_MAX_NODE_SUMMARY) {
            mesh_log_warn("ble", "Node cache full, dropping node %u", info->num);
            return;
        }
        state->handshake.node_count += 1U;
    }

    struct mesh_ble_node_summary *summary = &state->handshake.nodes[index];
    memset(summary, 0, sizeof(*summary));
    summary->node_id = info->num;
    summary->last_heard = info->last_heard;
    summary->snr = info->snr;
    summary->via_mqtt = info->via_mqtt;
    summary->has_hops_away = info->has_hops_away;
    summary->hops_away = info->hops_away;

    if (info->has_user) {
        snprintf(summary->long_name, sizeof(summary->long_name), "%s", info->user.long_name);
        snprintf(summary->short_name, sizeof(summary->short_name), "%s", info->user.short_name);
    } else {
        summary->long_name[0] = '\0';
        summary->short_name[0] = '\0';
    }

    mesh_log_debug("ble", "Cached node %u (%s)", summary->node_id,
                   summary->short_name[0] != '\0' ? summary->short_name : summary->long_name);
}

static void mesh_ble_handle_log_record(const meshtastic_LogRecord *record) {
    if (record == NULL) {
        return;
    }

    char message[sizeof(record->message) + 1U];
    memcpy(message, record->message, sizeof(record->message));
    message[sizeof(record->message)] = '\0';

    const char *component = "ble.log";

    switch (record->level) {
        case meshtastic_LogRecord_Level_CRITICAL:
        case meshtastic_LogRecord_Level_ERROR:
            mesh_log_error(component, "%s", message);
            break;
        case meshtastic_LogRecord_Level_WARNING:
            mesh_log_warn(component, "%s", message);
            break;
        case meshtastic_LogRecord_Level_INFO:
            mesh_log_info(component, "%s", message);
            break;
        case meshtastic_LogRecord_Level_DEBUG:
            mesh_log_debug(component, "%s", message);
            break;
        case meshtastic_LogRecord_Level_TRACE:
        case meshtastic_LogRecord_Level_UNSET:
        default:
            mesh_log_trace(component, "%s", message);
            break;
    }
}

static void mesh_ble_handle_from_radio(struct mesh_ble_transport_state *state, const uint8_t *payload, size_t len) {
    if (state == NULL || payload == NULL || len == 0U) {
        return;
    }

    meshtastic_FromRadio message = meshtastic_FromRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(payload, len);
    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &message)) {
        mesh_log_warn("ble", "Failed to decode FromRadio: %s", PB_GET_ERROR(&stream));
        return;
    }

    switch (message.which_payload_variant) {
        case meshtastic_FromRadio_my_info_tag:
            state->handshake.has_my_info = true;
            state->handshake.my_info = message.my_info;
            mesh_log_info("ble", "MyNodeInfo: node=%u, node_count=%u", message.my_info.my_node_num,
                          message.my_info.nodedb_count);
            break;
        case meshtastic_FromRadio_node_info_tag:
            mesh_ble_store_node_summary(state, &message.node_info);
            break;
        case meshtastic_FromRadio_config_tag:
            state->handshake.has_config = true;
            state->handshake.config = message.config;
            mesh_log_debug("ble", "Received config fragment");
            break;
        case meshtastic_FromRadio_config_complete_id_tag:
            state->handshake.config_complete_id = message.config_complete_id;
            if (state->handshake.request_in_flight &&
                message.config_complete_id == state->handshake.request_id) {
                state->handshake.request_in_flight = false;
                state->handshake.config_complete = true;
                mesh_log_info("ble", "Config sync complete for request %u", message.config_complete_id);
            } else {
                mesh_log_debug("ble", "Received config_complete_id=%u (pending=%s request=%u)",
                               message.config_complete_id, state->handshake.request_in_flight ? "yes" : "no",
                               state->handshake.request_id);
            }
            break;
        case meshtastic_FromRadio_log_record_tag:
            mesh_ble_handle_log_record(&message.log_record);
            break;
        default:
            mesh_log_debug("ble", "Ignoring FromRadio payload tag %" PRIu32, (uint32_t)message.which_payload_variant);
            break;
    }
}

static int mesh_ble_begin_handshake(struct mesh_ble_transport_state *state) {
    if (state == NULL) {
        return -EINVAL;
    }

    if (!state->client_initialised || state->rx_char_path[0] == '\0') {
        return -ENOTCONN;
    }

    uint32_t request_id = state->next_config_request_id++;
    if (state->next_config_request_id == 0U) {
        state->next_config_request_id = 1U;
    }
    if (request_id == 0U) {
        request_id = state->next_config_request_id++;
        if (state->next_config_request_id == 0U) {
            state->next_config_request_id = 1U;
        }
    }

    mesh_ble_reset_handshake(state);
    state->handshake.request_in_flight = true;
    state->handshake.request_id = request_id;

    meshtastic_ToRadio request = meshtastic_ToRadio_init_default;
    request.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    request.want_config_id = request_id;

    uint8_t payload[64];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &request)) {
        mesh_log_error("ble", "Failed to encode want_config: %s", PB_GET_ERROR(&stream));
        state->handshake.request_in_flight = false;
        return -EIO;
    }

    size_t payload_len = stream.bytes_written;
    uint8_t frame[sizeof(payload) + 8U];
    size_t frame_len = 0U;
    int frame_result = mesh_proto_frame_encode(payload, payload_len, frame, sizeof(frame), &frame_len);
    if (frame_result < 0) {
        mesh_log_error("ble", "Failed to frame want_config payload: %d", frame_result);
        state->handshake.request_in_flight = false;
        return frame_result;
    }

    int queue_result = mesh_ble_queue_frame(state, frame, frame_len);
    if (queue_result < 0) {
        mesh_log_error("ble", "Failed to queue want_config request: %d", queue_result);
        state->handshake.request_in_flight = false;
        return queue_result;
    }

    mesh_log_info("ble", "Requested config sync (request_id=%u)", request_id);
    return 0;
}

static void mesh_ble_process_rx_buffer(struct mesh_ble_transport_state *state) {
    if (state == NULL) {
        return;
    }

    size_t offset = 0U;
    while (offset < state->rx_buffer_len) {
        size_t available = state->rx_buffer_len - offset;
        uint32_t payload_len = 0U;
        size_t header_len = 0U;
        int result = mesh_proto_varint_decode(state->rx_buffer + offset, available, &payload_len, &header_len);
        if (result < 0) {
            if (available >= 5U) {
                mesh_log_warn("ble", "Dropping invalid BLE frame header");
                state->rx_buffer_len = 0U;
            }
            break;
        }

        size_t total_len = header_len + (size_t)payload_len;
        if (total_len > available) {
            if (total_len > sizeof(state->rx_buffer)) {
                mesh_log_warn("ble", "Frame length %zu exceeds buffer capacity", total_len);
                state->rx_buffer_len = 0U;
            }
            break;
        }

        const uint8_t *payload_ptr = state->rx_buffer + offset + header_len;
        mesh_ble_handle_from_radio(state, payload_ptr, payload_len);
        state->frames_received += 1U;
        state->bytes_received += payload_len;
        mesh_log_debug("ble", "Received protobuf frame (%u bytes)", (unsigned)payload_len);

        offset += total_len;
    }

    if (offset > 0U && offset <= state->rx_buffer_len) {
        size_t remaining = state->rx_buffer_len - offset;
        if (remaining > 0U) {
            memmove(state->rx_buffer, state->rx_buffer + offset, remaining);
        }
        state->rx_buffer_len = remaining;
    }
}

static void mesh_ble_notification_handler(const uint8_t *data, size_t len, void *userdata) {
    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)userdata;
    if (state == NULL || data == NULL || len == 0U) {
        return;
    }

    if (state->link_state != MESH_BLE_LINK_CONNECTED) {
        return;
    }

    if (len > sizeof(state->rx_buffer) - state->rx_buffer_len) {
        mesh_log_warn("ble", "RX buffer overflow, dropping %zu byte notification", len);
        state->rx_buffer_len = 0U;
        return;
    }

    memcpy(state->rx_buffer + state->rx_buffer_len, data, len);
    state->rx_buffer_len += len;
    mesh_ble_process_rx_buffer(state);
}

size_t mesh_ble_transport_refresh_devices(struct mesh_transport *transport) {
    return mesh_ble_refresh_devices_internal(transport);
}

int mesh_ble_transport_connect(struct mesh_transport *transport, const char *address) {
    if (transport == NULL || address == NULL) {
        return -EINVAL;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (!state->client_initialised) {
        return -ENOTCONN;
    }

    if (state->state != MESH_BLE_STATE_READY) {
        return -EAGAIN;
    }

    if (state->link_state == MESH_BLE_LINK_CONNECTED && strcmp(state->connected_address, address) == 0) {
        return -EALREADY;
    }

    const struct mesh_bluez_device_info *devices = state->devices;
    bool found = false;
    for (size_t i = 0; i < state->device_count; ++i) {
        if (strcmp(devices[i].address, address) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        return -ENOENT;
    }

    char device_path[sizeof(state->connected_device_path)];
    if (!mesh_ble_format_device_path(state, address, device_path, sizeof(device_path))) {
        return -EINVAL;
    }

    state->link_state = MESH_BLE_LINK_CONNECTING;
    int result = mesh_bluez_client_connect(&state->bluez, device_path);
    if (result < 0) {
        state->link_state = MESH_BLE_LINK_DISCONNECTED;
        return result;
    }

    char rx_path[sizeof(state->rx_char_path)];
    char tx_path[sizeof(state->tx_char_path)];
    result = mesh_bluez_client_find_nus_characteristics(&state->bluez, device_path, rx_path, sizeof(rx_path), tx_path,
                                                        sizeof(tx_path));
    if (result < 0) {
        mesh_bluez_client_disconnect(&state->bluez, device_path);
        state->link_state = MESH_BLE_LINK_DISCONNECTED;
        return result;
    }

    result = mesh_bluez_client_subscribe(&state->bluez, tx_path, k_mesh_ble_nus_tx_uuid);
    if (result < 0) {
        mesh_bluez_client_disconnect(&state->bluez, device_path);
        state->link_state = MESH_BLE_LINK_DISCONNECTED;
        return result;
    }

    state->link_state = MESH_BLE_LINK_CONNECTED;
    state->notifications_enabled = true;
    snprintf(state->connected_address, sizeof(state->connected_address), "%s", address);
    snprintf(state->connected_device_path, sizeof(state->connected_device_path), "%s", device_path);
    snprintf(state->rx_char_path, sizeof(state->rx_char_path), "%s", rx_path);
    snprintf(state->tx_char_path, sizeof(state->tx_char_path), "%s", tx_path);
    state->rx_buffer_len = 0U;
    state->frames_received = 0U;
    state->bytes_received = 0U;
    mesh_bluez_client_process(&state->bluez);
    int handshake_result = mesh_ble_begin_handshake(state);
    if (handshake_result < 0) {
        mesh_log_warn("ble", "Failed to request config sync: %d", handshake_result);
    }
    mesh_log_info("ble", "Connected to %s", address);
    return 0;
}

int mesh_ble_transport_disconnect(struct mesh_transport *transport) {
    if (transport == NULL) {
        return -EINVAL;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (state->link_state != MESH_BLE_LINK_CONNECTED) {
        return -ENOTCONN;
    }

    int result = mesh_bluez_client_disconnect(&state->bluez, state->connected_device_path);
    if (result < 0) {
        return result;
    }

    state->link_state = MESH_BLE_LINK_DISCONNECTED;
    state->notifications_enabled = false;
    state->connected_address[0] = '\0';
    state->connected_device_path[0] = '\0';
    state->rx_char_path[0] = '\0';
    state->tx_char_path[0] = '\0';
    state->rx_buffer_len = 0U;
    mesh_ble_reset_handshake(state);
    mesh_ble_clear_write_queue(state);
    state->write_mtu = MESH_BLE_DEFAULT_MTU;
    mesh_log_info("ble", "Disconnected from Meshtastic node");
    return 0;
}

struct mesh_ble_transport_stats mesh_ble_transport_stats(struct mesh_transport *transport) {
    struct mesh_ble_transport_stats stats = {0U, 0U};
    if (transport == NULL || transport->state == NULL) {
        return stats;
    }

    const struct mesh_ble_transport_state *state = (const struct mesh_ble_transport_state *)transport->state;
    stats.frames_received = state->frames_received;
    stats.bytes_received = state->bytes_received;
    return stats;
}

int mesh_ble_transport_send_frame(struct mesh_transport *transport, const uint8_t *frame, size_t len) {
    if (transport == NULL || transport->state == NULL) {
        return -EINVAL;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    return mesh_ble_queue_frame(state, frame, len);
}

struct mesh_ble_handshake_status mesh_ble_transport_handshake_status(struct mesh_transport *transport) {
    struct mesh_ble_handshake_status status;
    memset(&status, 0, sizeof(status));

    if (transport == NULL || transport->state == NULL) {
        return status;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    status = state->handshake;
    if (status.node_count > MESH_BLE_MAX_NODE_SUMMARY) {
        status.node_count = MESH_BLE_MAX_NODE_SUMMARY;
    }
    return status;
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
