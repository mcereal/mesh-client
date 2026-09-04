#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/ble.h"

#include "mesh/config.h"
#include "mesh/log.h"

#include "mesh/transport/ble_bluez.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define MESH_BLE_MAX_OUTBOUND_PACKETS 8U
/*
 * FromRadio reads are synchronous D-Bus round trips (~60 ms each on the Brick). Read at most this
 * many per event-loop turn, then wake the loop via eventfd to continue, so UI input, timers and
 * disconnects keep flowing during a large NodeDB sync.
 */
#define MESH_BLE_READS_PER_TURN 4U
/* Transient ReadValue failures are retried with exponential backoff, then the link is dropped. */
#define MESH_BLE_DRAIN_RETRY_BASE_MS 250U
#define MESH_BLE_DRAIN_MAX_FAILURES 5U
/* Device-list refresh from tick() is rate limited; the 5 s timerfd is the steady-state refresher.
 */
#define MESH_BLE_TICK_REFRESH_MIN_MS 1000U
/* BlueZ's Connect returns as soon as the link is up; the GATT characteristics only appear once
   service discovery finishes, which can take several seconds when nothing is cached. */
#define MESH_BLE_SERVICES_POLL_MS 250U
/* How often Device1.Connected is re-read while linked; a dropped radio is noticed within this. */
#define MESH_BLE_LINK_POLL_MS 2000U
#define MESH_BLE_SERVICES_TIMEOUT_MS 20000U
/* How long to wait for BlueZ to answer Device1.Connect before giving up on the attempt. */
#define MESH_BLE_CONNECT_TIMEOUT_MS 30000U

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

/* One ToRadio protobuf. Meshtastic BLE has no stream framing: one packet per GATT write. */
struct mesh_ble_outbound_packet {
    size_t length;
    uint32_t packet_id; /* message log id to fail if this never reaches the radio; 0 = none */
    uint8_t data[MESH_BLE_MAX_PACKET_SIZE];
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
    int drain_wake_fd; /* eventfd: continue a FromRadio drain on the next loop turn */
    uint64_t last_refresh_ms;
    struct mesh_event_loop *loop;
    enum mesh_ble_link_state link_state;
    char connected_address[32];
    char connected_device_path[128];
    bool notifications_enabled;
    uint64_t connect_started_ms;    /* Device1.Connect returned; link_state is CONNECTING */
    uint64_t next_services_poll_ms; /* earliest next ServicesResolved poll */
    bool services_wait_logged;
    bool connect_pending;       /* Device1.Connect sent, reply not yet seen */
    uint64_t next_link_poll_ms; /* earliest next Device1.Connected check while CONNECTED */
    bool drain_pending;         /* more FromRadio packets may be waiting */
    uint64_t drain_retry_at_ms; /* earliest time to run the pending drain (0 = now) */
    unsigned drain_failures;    /* consecutive ReadValue failures */
    struct mesh_bluez_meshtastic_chars chars;
    size_t frames_received;
    size_t bytes_received;
    struct mesh_ble_outbound_packet write_queue[MESH_BLE_MAX_OUTBOUND_PACKETS];
    size_t write_queue_head;
    size_t write_queue_len;
    /* The Meshtastic conversation itself (handshake, nodes, messages, settings). The link
       attaches to it while connected and feeds it every FromRadio packet. */
    /* Owned only when nothing was injected; `session` is what the code uses. */
    struct mesh_session own_session;
    struct mesh_session *session;
    /* Why the last connect attempt failed, in words, waiting to be shown once. A connect that
       returned 0 can still fail several seconds later from tick(), so the errno the caller saw
       cannot explain it. */
    char last_error[MESH_TRANSPORT_ERROR_MAX];
};

/* Records a failure for the UI to pick up. First one wins until it is read, so the message the
   user sees is the thing that actually went wrong rather than whatever cleanup reported last. */
static void mesh_ble_set_error(struct mesh_ble_transport_state *state, const char *fmt, ...) {
    if (state == NULL || state->last_error[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(state->last_error, sizeof state->last_error, fmt, args);
    va_end(args);
}

/* A short label for a node: the last four hex digits are what the Meshtastic apps show and what
   is printed on the case, so "6DDA" beats the full MAC in a 64-column toast. */
static const char *mesh_ble_short_label(const char *address) {
    if (address == NULL || address[0] == '\0') {
        return "node";
    }
    const size_t len = strlen(address);
    /* "FB:17:7C:37:6D:DA" -> "6D:DA"; anything shorter is used as-is. */
    return (len > 5U) ? address + (len - 5U) : address;
}

/* Plain English for the errnos bluez_client maps its D-Bus errors onto. */
static const char *mesh_ble_connect_failure_text(int err) {
    switch (err) {
    case -EACCES:
        return "needs pairing (PIN mode)";
    case -ETIMEDOUT:
        return "did not answer; is it in range?";
    case -EBUSY:
        return "already connecting";
    case -ENOENT:
        return "is not in range";
    case -ENOTCONN:
        return "Bluetooth is not ready";
    default:
        return "connect failed";
    }
}

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
static void mesh_ble_clear_write_queue(struct mesh_ble_transport_state *state);
static int mesh_ble_queue_packet(struct mesh_ble_transport_state *state, const uint8_t *packet,
                                 size_t len, uint32_t packet_id);
static int mesh_ble_session_send(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id);
static int mesh_ble_flush_write_queue(struct mesh_ble_transport_state *state);
static void mesh_ble_drain_from_radio(struct mesh_ble_transport_state *state);
static void mesh_ble_schedule_drain(struct mesh_ble_transport_state *state, uint64_t delay_ms);
static void mesh_ble_reset_link(struct mesh_ble_transport_state *state, const char *reason);
static void mesh_ble_notification_handler(const uint8_t *data, size_t len, void *userdata);
static void mesh_ble_poll_connecting(struct mesh_ble_transport_state *state);
static int mesh_ble_complete_connect(struct mesh_ble_transport_state *state);

static uint64_t mesh_ble_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void mesh_ble_tick(struct mesh_transport *transport) {
    if (transport == NULL) {
        return;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    uint64_t now = mesh_ble_now_ms();
    if (state != NULL && state->client_initialised) {
        mesh_bluez_client_process(&state->bluez);
        (void)mesh_ble_flush_write_queue(state);
        if (state->link_state == MESH_BLE_LINK_CONNECTING) {
            mesh_ble_poll_connecting(state);
        } else if (state->link_state == MESH_BLE_LINK_CONNECTED &&
                   now >= state->next_link_poll_ms) {
            state->next_link_poll_ms = now + MESH_BLE_LINK_POLL_MS;
            (void)mesh_ble_transport_check_link(transport);
        }
        if (state->link_state == MESH_BLE_LINK_CONNECTED) {
            mesh_session_tick(state->session, now);
        }
        /* Fallback for the eventfd wake, and the path that services delayed retries. */
        if (state->drain_pending && now >= state->drain_retry_at_ms) {
            mesh_ble_drain_from_radio(state);
        }
    }

    if (state == NULL || now - state->last_refresh_ms >= MESH_BLE_TICK_REFRESH_MIN_MS) {
        mesh_ble_refresh_devices_internal(transport);
        if (state != NULL) {
            state->last_refresh_ms = now;
        }
    }
}

static int mesh_ble_drain_wake_callback(int fd, uint32_t events, void *userdata) {
    (void)events;
    struct mesh_transport *transport = (struct mesh_transport *)userdata;
    if (transport == NULL) {
        return 0;
    }
    uint64_t value = 0;
    ssize_t read_result = read(fd, &value, sizeof(value));
    if (read_result < 0 && errno != EAGAIN) {
        mesh_log_warn("ble", "drain wake read failed: %s", strerror(errno));
    }
    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (state != NULL && state->drain_pending && mesh_ble_now_ms() >= state->drain_retry_at_ms) {
        mesh_ble_drain_from_radio(state);
    }
    return 0;
}

static int mesh_ble_setup_drain_wake(struct mesh_transport *transport,
                                     struct mesh_ble_transport_state *state,
                                     struct mesh_event_loop *loop) {
    if (loop == NULL) {
        return 0;
    }
    state->drain_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (state->drain_wake_fd < 0) {
        mesh_log_warn("ble", "eventfd create failed: %s", strerror(errno));
        return -errno;
    }
    int add_result = mesh_event_loop_add_fd(loop, state->drain_wake_fd, EPOLLIN,
                                            mesh_ble_drain_wake_callback, transport);
    if (add_result < 0) {
        mesh_log_warn("ble", "Failed to add drain wake fd: %d", add_result);
        close(state->drain_wake_fd);
        state->drain_wake_fd = -1;
        return add_result;
    }
    return 0;
}

static void mesh_ble_teardown_drain_wake(struct mesh_ble_transport_state *state) {
    if (state->drain_wake_fd >= 0) {
        if (state->loop != NULL) {
            mesh_event_loop_remove_fd(state->loop, state->drain_wake_fd);
        }
        close(state->drain_wake_fd);
        state->drain_wake_fd = -1;
    }
}

static void mesh_ble_schedule_drain(struct mesh_ble_transport_state *state, uint64_t delay_ms) {
    state->drain_pending = true;
    state->drain_retry_at_ms = delay_ms == 0U ? 0U : mesh_ble_now_ms() + delay_ms;
    if (delay_ms == 0U && state->drain_wake_fd >= 0) {
        uint64_t one = 1U;
        if (write(state->drain_wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
            mesh_log_warn("ble", "drain wake write failed: %s", strerror(errno));
        }
    }
    /* Delayed retries are picked up by tick(), which runs every loop turn. */
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

static int mesh_ble_setup_refresh_timer(struct mesh_transport *transport,
                                        struct mesh_ble_transport_state *state,
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

    int add_result = mesh_event_loop_add_fd(loop, state->refresh_timer_fd, EPOLLIN,
                                            mesh_ble_refresh_timer_callback, transport);
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
    state->drain_wake_fd = -1;
    state->last_refresh_ms = 0U;
    state->drain_retry_at_ms = 0U;
    state->drain_failures = 0U;
    state->loop = loop;
    state->link_state = MESH_BLE_LINK_DISCONNECTED;
    state->connected_address[0] = '\0';
    state->connected_device_path[0] = '\0';
    state->notifications_enabled = false;
    memset(&state->chars, 0, sizeof(state->chars));
    state->frames_received = 0U;
    state->bytes_received = 0U;
    /* The app hands every link the same session; standalone (tests, --list-devices) each link
       falls back to its own and initialises it here. */
    if (state->session == NULL) {
        state->session = &state->own_session;
    }
    if (state->session == &state->own_session) {
        mesh_session_init(state->session);
    }
    mesh_ble_clear_write_queue(state);

    if (!config->enable_ble) {
        mesh_log_info("ble", "BLE transport disabled by configuration");
        state->state = MESH_BLE_STATE_DISABLED;
        return 0;
    }

    const int init_result = mesh_bluez_client_init(&state->bluez);
    if (init_result < 0) {
        if (init_result == -ENOSYS) {
            mesh_log_warn("ble",
                          "BLE transport built without D-Bus support; skipping BlueZ startup");
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
    int adapter_result =
        mesh_bluez_client_find_adapter(&state->bluez, adapter_path, sizeof(adapter_path));
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
    if (mesh_ble_setup_drain_wake(transport, state, loop) < 0) {
        mesh_log_debug("ble", "Drain wake unavailable; FromRadio drains continue from tick()");
    }

    state->state = MESH_BLE_STATE_READY;
    if (config->preferred_ble_device[0] != '\0') {
        mesh_log_info("ble", "Attempting to connect to preferred device '%s'",
                      config->preferred_ble_device);
    } else {
        mesh_log_info("ble", "Scanning for Meshtastic nodes via %s", state->adapter_path);
    }

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
    mesh_ble_teardown_drain_wake(state);

    if (state->client_initialised) {
        /* A CONNECTING link is up at the controller even though setup never finished. */
        if (state->link_state != MESH_BLE_LINK_DISCONNECTED &&
            state->connected_device_path[0] != '\0') {
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
    state->drain_pending = false;
    memset(&state->chars, 0, sizeof(state->chars));
    state->frames_received = 0U;
    state->bytes_received = 0U;
    mesh_session_detach(state->session);
    mesh_ble_clear_write_queue(state);
    state->loop = NULL;
    mesh_log_info("ble", "BLE transport stopped");
}

static const char *mesh_ble_status(const struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return "unknown";
    }

    const struct mesh_ble_transport_state *state =
        (const struct mesh_ble_transport_state *)transport->state;
    if (state->state == MESH_BLE_STATE_READY) {
        if (state->link_state == MESH_BLE_LINK_CONNECTING) {
            return "connecting";
        }
        if (state->link_state == MESH_BLE_LINK_CONNECTED) {
            return "connected";
        }
    }
    return mesh_ble_state_to_string(state->state);
}

static void mesh_ble_set_session(struct mesh_transport *transport, struct mesh_session *session) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    ((struct mesh_ble_transport_state *)transport->state)->session = session;
}

static bool mesh_ble_take_error(struct mesh_transport *transport, char *out, size_t out_len) {
    if (transport == NULL || transport->state == NULL || out == NULL || out_len == 0U) {
        return false;
    }
    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (state->last_error[0] == '\0') {
        return false;
    }
    snprintf(out, out_len, "%s", state->last_error);
    state->last_error[0] = '\0';
    return true;
}

static const struct mesh_transport_ops k_ble_ops = {
    .start = mesh_ble_start,
    .stop = mesh_ble_stop,
    .status = mesh_ble_status,
    .tick = mesh_ble_tick,
    .set_session = mesh_ble_set_session,
    .take_error = mesh_ble_take_error,
};

static bool mesh_ble_format_device_path(const struct mesh_ble_transport_state *state,
                                        const char *address, char *out_path, size_t out_len) {
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

size_t mesh_ble_transport_get_devices(struct mesh_transport *transport,
                                      struct mesh_bluez_device_info *out, size_t capacity) {
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

const struct mesh_bluez_device_info *mesh_ble_transport_devices(struct mesh_transport *transport,
                                                                size_t *count) {
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
    int list_result = mesh_bluez_client_list_meshtastic(
        &state->bluez, state->devices, sizeof(state->devices) / sizeof(state->devices[0]),
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
        mesh_log_debug("ble", "  %s (%s) RSSI=%d", state->devices[i].name,
                       state->devices[i].address, (int)state->devices[i].rssi);
    }
    return state->device_count;
}

/* Drops everything still queued. Messages among them never reached the radio, so their
   delivery state becomes FAILED rather than staying PENDING forever. */
static void mesh_ble_clear_write_queue(struct mesh_ble_transport_state *state) {
    if (state == NULL) {
        return;
    }

    while (state->write_queue_len > 0U) {
        struct mesh_ble_outbound_packet *packet = &state->write_queue[state->write_queue_head];
        mesh_session_packet_failed(state->session, packet->packet_id);
        packet->packet_id = 0U;
        state->write_queue_head = (state->write_queue_head + 1U) % MESH_BLE_MAX_OUTBOUND_PACKETS;
        state->write_queue_len--;
    }
    state->write_queue_head = 0U;
    state->write_queue_len = 0U;
}

/* A GATT write that fails means the link is gone in practice (BlueZ says "Not connected"), so
   the link is reset here and auto-connect takes it from there. Returns the write error. */
static int mesh_ble_flush_write_queue(struct mesh_ble_transport_state *state) {
    if (state == NULL || state->write_queue_len == 0U || !state->client_initialised ||
        state->chars.toradio_path[0] == '\0') {
        return 0;
    }

    while (state->write_queue_len > 0U) {
        struct mesh_ble_outbound_packet *packet = &state->write_queue[state->write_queue_head];

        int result = mesh_bluez_client_write(&state->bluez, state->chars.toradio_path,
                                             MESH_BLE_TORADIO_UUID, packet->data, packet->length);
        if (result < 0) {
            mesh_log_warn("ble", "ToRadio write failed: %d; dropping link", result);
            mesh_ble_reset_link(state, "write failed");
            return result;
        }

        packet->packet_id = 0U;
        state->write_queue_head = (state->write_queue_head + 1U) % MESH_BLE_MAX_OUTBOUND_PACKETS;
        state->write_queue_len--;
    }
    return 0;
}

static int mesh_ble_queue_packet(struct mesh_ble_transport_state *state, const uint8_t *packet,
                                 size_t len, uint32_t packet_id) {
    if (state == NULL || packet == NULL || len == 0U) {
        return -EINVAL;
    }

    if (len > MESH_BLE_MAX_PACKET_SIZE) {
        mesh_log_warn("ble", "ToRadio packet of %zu bytes exceeds %u byte limit", len,
                      (unsigned)MESH_BLE_MAX_PACKET_SIZE);
        return -EMSGSIZE;
    }

    if (state->write_queue_len >= MESH_BLE_MAX_OUTBOUND_PACKETS) {
        mesh_log_warn("ble", "write queue full, dropping %zu byte packet", len);
        return -ENOSPC;
    }

    size_t index =
        (state->write_queue_head + state->write_queue_len) % MESH_BLE_MAX_OUTBOUND_PACKETS;
    struct mesh_ble_outbound_packet *slot = &state->write_queue[index];
    slot->length = len;
    slot->packet_id = packet_id;
    memcpy(slot->data, packet, len);
    state->write_queue_len++;

    return mesh_ble_flush_write_queue(state);
}

/* The session's send path while this link is up: one GATT write per ToRadio protobuf. */
static int mesh_ble_session_send(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id) {
    return mesh_ble_queue_packet((struct mesh_ble_transport_state *)ctx, packet, len, packet_id);
}

/*
 * Pull everything the node has queued. Meshtastic serves one FromRadio protobuf per read and an
 * empty value once the FIFO is drained; FromNum only tells us that there is something to read.
 */
static void mesh_ble_drain_from_radio(struct mesh_ble_transport_state *state) {
    if (state == NULL || !state->client_initialised ||
        state->link_state != MESH_BLE_LINK_CONNECTED || state->chars.fromradio_path[0] == '\0') {
        return;
    }

    state->drain_pending = false;
    state->drain_retry_at_ms = 0U;
    uint8_t packet[MESH_BLE_MAX_PACKET_SIZE];
    for (size_t i = 0; i < MESH_BLE_READS_PER_TURN; ++i) {
        size_t len = 0U;
        int result = mesh_bluez_client_read(&state->bluez, state->chars.fromradio_path, packet,
                                            sizeof(packet), &len);
        if (result < 0) {
            state->drain_failures += 1U;
            if (state->drain_failures >= MESH_BLE_DRAIN_MAX_FAILURES) {
                mesh_log_error("ble", "FromRadio read failed %u times in a row (%d); dropping link",
                               state->drain_failures, result);
                mesh_ble_reset_link(state, "FromRadio unreadable");
                return;
            }
            /* The FromNum notification already told us a packet is waiting; do not lose it. */
            uint64_t delay = (uint64_t)MESH_BLE_DRAIN_RETRY_BASE_MS << (state->drain_failures - 1U);
            mesh_log_warn("ble", "FromRadio read failed (%d); retrying in %" PRIu64 " ms", result,
                          delay);
            mesh_ble_schedule_drain(state, delay);
            return;
        }
        state->drain_failures = 0U;
        if (len == 0U) {
            return; /* FIFO drained */
        }

        state->frames_received += 1U;
        state->bytes_received += len;
        mesh_log_debug("ble", "FromRadio packet (%zu bytes)", len);
        mesh_session_handle_from_radio(state->session, packet, len);
    }

    /* Budget for this turn spent; yield to the event loop and come straight back. */
    mesh_ble_schedule_drain(state, 0U);
}

static void mesh_ble_notification_handler(const uint8_t *data, size_t len, void *userdata) {
    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)userdata;
    if (state == NULL || data == NULL || len == 0U) {
        return;
    }

    if (state->link_state != MESH_BLE_LINK_CONNECTED) {
        return;
    }

    /* FromNum carries a little-endian uint32 packet counter; the value itself is only informative.
     */
    uint32_t from_num = 0U;
    for (size_t i = 0; i < len && i < 4U; ++i) {
        from_num |= (uint32_t)data[i] << (8U * i);
    }
    mesh_log_trace("ble", "FromNum notification (%u)", from_num);
    mesh_ble_drain_from_radio(state);
}

size_t mesh_ble_transport_refresh_devices(struct mesh_transport *transport) {
    return mesh_ble_refresh_devices_internal(transport);
}

int mesh_ble_transport_connect(struct mesh_transport *transport, const char *address) {
    if (transport == NULL || address == NULL) {
        return -EINVAL;
    }

    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    /* A new attempt supersedes whatever the last one failed with. */
    state->last_error[0] = '\0';

    if (!state->client_initialised) {
        mesh_ble_set_error(state, "Bluetooth is unavailable");
        return -ENOTCONN;
    }

    if (state->state != MESH_BLE_STATE_READY) {
        mesh_ble_set_error(state, "Bluetooth is not ready yet");
        return -EAGAIN;
    }

    if (state->link_state == MESH_BLE_LINK_CONNECTED &&
        strcmp(state->connected_address, address) == 0) {
        return -EALREADY;
    }
    if (state->link_state == MESH_BLE_LINK_CONNECTING) {
        return strcmp(state->connected_address, address) == 0 ? -EINPROGRESS : -EBUSY;
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
        mesh_ble_set_error(state, "%s is not in range", mesh_ble_short_label(address));
        return -ENOENT;
    }

    char device_path[sizeof(state->connected_device_path)];
    if (!mesh_ble_format_device_path(state, address, device_path, sizeof(device_path))) {
        return -EINVAL;
    }

    state->link_state = MESH_BLE_LINK_CONNECTING;
    int result = mesh_bluez_client_connect_begin(&state->bluez, device_path);
    if (result < 0) {
        state->link_state = MESH_BLE_LINK_DISCONNECTED;
        mesh_ble_set_error(state, "%s: %s", mesh_ble_short_label(address),
                           mesh_ble_connect_failure_text(result));
        return result;
    }

    snprintf(state->connected_address, sizeof(state->connected_address), "%s", address);
    snprintf(state->connected_device_path, sizeof(state->connected_device_path), "%s", device_path);
    state->connect_pending = true;
    state->connect_started_ms = mesh_ble_now_ms();
    state->next_services_poll_ms = 0U;
    state->services_wait_logged = false;

    /* Device1.Connect can take BlueZ many seconds (or the full 25 s D-Bus timeout when the node
       does not answer), so the reply is collected from tick(). With the mock it completes here,
       and so does a bonded node whose GATT database BlueZ already holds. */
    mesh_ble_poll_connecting(state);
    return 0;
}

/* Runs while link_state is CONNECTING: completes the connect once BlueZ reports
   ServicesResolved, and drops the link if discovery errors out or overruns the timeout. */
static void mesh_ble_poll_connecting(struct mesh_ble_transport_state *state) {
    if (state == NULL || state->link_state != MESH_BLE_LINK_CONNECTING) {
        return;
    }

    uint64_t now = mesh_ble_now_ms();

    if (state->connect_pending) {
        int connect_result = 0;
        int poll = mesh_bluez_client_connect_poll(&state->bluez, &connect_result);
        if (poll == 0) {
            if (now - state->connect_started_ms >= MESH_BLE_CONNECT_TIMEOUT_MS) {
                mesh_log_warn("ble", "%s: no reply to Connect after %u ms",
                              state->connected_address, MESH_BLE_CONNECT_TIMEOUT_MS);
                mesh_ble_set_error(state, "%s did not answer; is it in range?",
                                   mesh_ble_short_label(state->connected_address));
                mesh_ble_reset_link(state, "connect timed out");
            }
            return;
        }
        state->connect_pending = false;
        if (poll < 0 || connect_result < 0) {
            mesh_ble_set_error(state, "%s: %s", mesh_ble_short_label(state->connected_address),
                               mesh_ble_connect_failure_text(connect_result));
            mesh_ble_reset_link(state, "connect failed");
            return;
        }
        /* Link is up; service discovery starts now, so time it from here. */
        state->connect_started_ms = now;
    }

    if (now < state->next_services_poll_ms) {
        return;
    }
    state->next_services_poll_ms = now + MESH_BLE_SERVICES_POLL_MS;

    bool resolved = false;
    int result =
        mesh_bluez_client_services_resolved(&state->bluez, state->connected_device_path, &resolved);
    if (result < 0) {
        mesh_log_warn("ble", "ServicesResolved query failed for %s (%d)", state->connected_address,
                      result);
        mesh_ble_set_error(state, "%s: service discovery failed",
                           mesh_ble_short_label(state->connected_address));
        mesh_ble_reset_link(state, "service discovery failed");
        return;
    }

    if (!resolved) {
        if (now - state->connect_started_ms >= MESH_BLE_SERVICES_TIMEOUT_MS) {
            mesh_log_warn("ble", "%s: GATT services still unresolved after %u ms",
                          state->connected_address, MESH_BLE_SERVICES_TIMEOUT_MS);
            mesh_ble_set_error(state, "%s: no GATT services; try re-pairing",
                               mesh_ble_short_label(state->connected_address));
            mesh_ble_reset_link(state, "service discovery timed out");
            return;
        }
        if (!state->services_wait_logged) {
            mesh_log_info("ble", "Link to %s is up; waiting for GATT service discovery",
                          state->connected_address);
            state->services_wait_logged = true;
        }
        return;
    }

    if (mesh_ble_complete_connect(state) < 0) {
        mesh_ble_reset_link(state, "characteristic setup failed");
    }
}

static int mesh_ble_complete_connect(struct mesh_ble_transport_state *state) {
    const char *address = state->connected_address;
    const char *device_path = state->connected_device_path;

    struct mesh_bluez_meshtastic_chars chars;
    int result =
        mesh_bluez_client_find_meshtastic_characteristics(&state->bluez, device_path, &chars);
    if (result < 0) {
        mesh_log_warn("ble", "%s does not expose the Meshtastic service characteristics (%d)",
                      address, result);
        mesh_ble_set_error(state, "%s is not a Meshtastic node", mesh_ble_short_label(address));
        return result;
    }
    mesh_log_debug("ble", "ToRadio %s", chars.toradio_path);
    mesh_log_debug("ble", "FromRadio %s", chars.fromradio_path);
    mesh_log_debug("ble", "FromNum %s", chars.fromnum_path);

    result = mesh_bluez_client_subscribe(&state->bluez, chars.fromnum_path, MESH_BLE_FROMNUM_UUID);
    if (result < 0) {
        mesh_log_warn("ble", "FromNum StartNotify failed (%d); is the node paired?", result);
        /* The overwhelmingly common cause, and the only one the user can act on: a node in PIN
           pairing mode has to be bonded with BlueZ out of band before its characteristics will
           notify. Say so instead of printing an errno. */
        if (result == -EACCES) {
            mesh_ble_set_error(state, "%s needs pairing (PIN mode)", mesh_ble_short_label(address));
        } else {
            mesh_ble_set_error(state, "%s: could not subscribe (%d)", mesh_ble_short_label(address),
                               result);
        }
        return result;
    }

    state->link_state = MESH_BLE_LINK_CONNECTED;
    state->notifications_enabled = true;
    state->chars = chars;
    state->drain_pending = false;
    state->drain_retry_at_ms = 0U;
    state->drain_failures = 0U;
    state->frames_received = 0U;
    state->bytes_received = 0U;
    mesh_bluez_client_process(&state->bluez);
    mesh_session_attach(state->session, mesh_ble_session_send, state);
    int handshake_result = mesh_session_begin_handshake(state->session);
    if (handshake_result < 0) {
        mesh_log_warn("ble", "Failed to request config sync: %d", handshake_result);
    }
    mesh_log_info("ble", "Connected to %s", address);
    /* The node may already have packets queued, and a FromNum notify can race the subscription. */
    mesh_ble_drain_from_radio(state);
    return 0;
}

static void mesh_ble_reset_link(struct mesh_ble_transport_state *state, const char *reason) {
    if (state == NULL) {
        return;
    }
    if (state->client_initialised && state->connected_device_path[0] != '\0') {
        int result = mesh_bluez_client_disconnect(&state->bluez, state->connected_device_path);
        if (result < 0) {
            mesh_log_debug("ble", "Disconnect during link reset returned %d", result);
        }
    }
    if (state->connect_pending) {
        mesh_bluez_client_connect_cancel(&state->bluez);
    }
    state->link_state = MESH_BLE_LINK_DISCONNECTED;
    state->connect_pending = false;
    state->next_link_poll_ms = 0U;
    state->notifications_enabled = false;
    state->drain_pending = false;
    state->drain_retry_at_ms = 0U;
    state->drain_failures = 0U;
    state->connected_address[0] = '\0';
    state->connected_device_path[0] = '\0';
    memset(&state->chars, 0, sizeof(state->chars));
    mesh_session_detach(state->session);
    mesh_ble_clear_write_queue(state);
    mesh_log_info("ble", "Disconnected from Meshtastic node (%s)", reason);
}

int mesh_ble_transport_check_link(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -EINVAL;
    }
    struct mesh_ble_transport_state *state = (struct mesh_ble_transport_state *)transport->state;
    if (!state->client_initialised || state->link_state != MESH_BLE_LINK_CONNECTED ||
        state->connected_device_path[0] == '\0') {
        return -ENOTCONN;
    }

    bool connected = true;
    int result =
        mesh_bluez_client_device_connected(&state->bluez, state->connected_device_path, &connected);
    if (result < 0) {
        mesh_log_debug("ble", "Could not read Device1.Connected: %d", result);
        return result;
    }
    if (connected) {
        return 1;
    }
    mesh_log_warn("ble", "BlueZ reports %s disconnected", state->connected_address);
    mesh_ble_reset_link(state, "link dropped");
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
    state->connected_device_path[0] =
        '\0'; /* already disconnected; reset_link must not repeat it */
    mesh_ble_reset_link(state, "requested");
    return 0;
}

struct mesh_ble_transport_stats mesh_ble_transport_stats(struct mesh_transport *transport) {
    struct mesh_ble_transport_stats stats = {0U, 0U};
    if (transport == NULL || transport->state == NULL) {
        return stats;
    }

    const struct mesh_ble_transport_state *state =
        (const struct mesh_ble_transport_state *)transport->state;
    stats.frames_received = state->frames_received;
    stats.bytes_received = state->bytes_received;
    return stats;
}

bool mesh_ble_transport_is_connecting(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return false;
    }
    const struct mesh_ble_transport_state *state =
        (const struct mesh_ble_transport_state *)transport->state;
    return state->link_state == MESH_BLE_LINK_CONNECTING;
}

const char *mesh_ble_transport_connected_address(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return NULL;
    }

    const struct mesh_ble_transport_state *state =
        (const struct mesh_ble_transport_state *)transport->state;
    if (state->link_state != MESH_BLE_LINK_CONNECTED || state->connected_address[0] == '\0') {
        return NULL;
    }
    return state->connected_address;
}

struct mesh_session *mesh_ble_transport_session(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return NULL;
    }
    return ((struct mesh_ble_transport_state *)transport->state)->session;
}

const struct mesh_radio_settings *mesh_ble_transport_settings(struct mesh_transport *transport) {
    return mesh_session_settings(mesh_ble_transport_session(transport));
}

int mesh_ble_transport_refresh_settings(struct mesh_transport *transport) {
    struct mesh_session *session = mesh_ble_transport_session(transport);
    return session != NULL ? mesh_session_refresh_settings(session) : -EINVAL;
}

int mesh_ble_transport_write_settings(struct mesh_transport *transport,
                                      const struct mesh_admin_request *write) {
    struct mesh_session *session = mesh_ble_transport_session(transport);
    return session != NULL ? mesh_session_write_settings(session, write) : -EINVAL;
}

int mesh_ble_transport_send_packet(struct mesh_transport *transport, const uint8_t *packet,
                                   size_t len) {
    struct mesh_session *session = mesh_ble_transport_session(transport);
    return session != NULL ? mesh_session_send_packet(session, packet, len) : -EINVAL;
}

int mesh_ble_transport_send_text(struct mesh_transport *transport, uint32_t dest, uint8_t channel,
                                 const char *text, bool want_ack, uint32_t *out_packet_id) {
    struct mesh_session *session = mesh_ble_transport_session(transport);
    if (session == NULL) {
        return -EINVAL;
    }
    return mesh_session_send_text(session, dest, channel, text, want_ack, out_packet_id);
}

const struct mesh_message_log *mesh_ble_transport_messages(struct mesh_transport *transport) {
    return mesh_session_messages(mesh_ble_transport_session(transport));
}

struct mesh_handshake_status mesh_ble_transport_handshake_status(struct mesh_transport *transport) {
    struct mesh_handshake_status status;
    const struct mesh_session *session = mesh_ble_transport_session(transport);
    if (session == NULL) {
        memset(&status, 0, sizeof status);
        return status;
    }
    status = session->handshake;
    if (status.node_count > MESH_SESSION_MAX_NODES) {
        status.node_count = MESH_SESSION_MAX_NODES;
    }
    if (status.channel_count > MESH_SESSION_MAX_CHANNELS) {
        status.channel_count = MESH_SESSION_MAX_CHANNELS;
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
