#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/serial.h"

#include "mesh/core/config.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/utils/log.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define MESH_SERIAL_MAX_OUTBOUND_PACKETS 8U
#define MESH_SERIAL_READ_CHUNK 1024U
/* Reads per event-loop turn. A NodeDB sync arrives as a burst; bound it so UI input still flows. */
#define MESH_SERIAL_READS_PER_TURN 8U
/* Rescan of sysfs from tick() is rate limited; a hotplugged node shows up within this. */
#define MESH_SERIAL_SCAN_INTERVAL_MS 3000U
/*
 * The radio may be asleep, and its own frame parser may be mid-packet from whatever was on the
 * port before us. The Meshtastic clients send a run of bare START2 bytes, which cannot complete
 * a frame and so forces a resync, then pause before the first real packet.
 */
#define MESH_SERIAL_WAKE_BYTES 32U
#define MESH_SERIAL_WAKE_SETTLE_MS 100U

enum mesh_serial_state {
    MESH_SERIAL_STATE_DISABLED = 0,
    MESH_SERIAL_STATE_IDLE,
    MESH_SERIAL_STATE_READY,
};

enum mesh_serial_link_state {
    MESH_SERIAL_LINK_DISCONNECTED = 0,
    MESH_SERIAL_LINK_WAKING,
    MESH_SERIAL_LINK_CONNECTED,
};

/* One framed ToRadio packet, header included, with a cursor for partial writes. */
struct mesh_serial_outbound_packet {
    size_t length;
    size_t sent;
    uint32_t packet_id; /* message log id to fail if this never reaches the radio; 0 = none */
    uint8_t data[MESH_STREAM_FRAME_HEADER_LEN + MESH_STREAM_FRAME_MAX_PAYLOAD];
};

struct mesh_serial_transport_state {
    enum mesh_serial_state state;
    enum mesh_serial_link_state link_state;
    struct mesh_event_loop *loop;
    struct mesh_serial_device_info devices[MESH_SERIAL_MAX_DEVICES];
    size_t device_count;
    uint64_t last_scan_ms;

    int fd;
    bool fd_registered;
    bool want_write; /* EPOLLOUT is armed because the write queue has a remainder */
    struct mesh_serial_device_info connected;
    uint64_t wake_done_at_ms;

    struct mesh_stream_parser parser;
    size_t frames_received;
    size_t bytes_received;

    struct mesh_serial_outbound_packet write_queue[MESH_SERIAL_MAX_OUTBOUND_PACKETS];
    size_t write_queue_head;
    size_t write_queue_len;

    /* The Meshtastic conversation itself. The link attaches to it once the port is awake. */
    /* Owned only when nothing was injected; `session` is what the code uses. */
    struct mesh_session own_session;
    struct mesh_session *session;
    /* Why the last connect attempt failed, in words, waiting to be shown once. */
    char last_error[MESH_TRANSPORT_ERROR_MAX];
};

/* Records a failure for the UI to pick up. First one wins until it is read. */
static void mesh_serial_set_error(struct mesh_serial_transport_state *state, const char *fmt, ...) {
    if (state == NULL || state->last_error[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(state->last_error, sizeof state->last_error, fmt, args);
    va_end(args);
}

static const char *mesh_serial_state_to_string(enum mesh_serial_state state) {
    switch (state) {
    case MESH_SERIAL_STATE_DISABLED:
        return "disabled";
    case MESH_SERIAL_STATE_IDLE:
        return "no-ports";
    case MESH_SERIAL_STATE_READY:
        return "running";
    }
    return "unknown";
}

static void mesh_serial_reset_link(struct mesh_serial_transport_state *state, const char *reason);
static int mesh_serial_flush_write_queue(struct mesh_serial_transport_state *state);
static size_t mesh_serial_scan_internal(struct mesh_serial_transport_state *state);

/* ------------------------------------------------------------------ write queue */

static void mesh_serial_clear_write_queue(struct mesh_serial_transport_state *state) {
    for (size_t i = 0; i < state->write_queue_len; ++i) {
        const size_t index = (state->write_queue_head + i) % MESH_SERIAL_MAX_OUTBOUND_PACKETS;
        const uint32_t packet_id = state->write_queue[index].packet_id;
        if (packet_id != 0U) {
            mesh_session_packet_failed(state->session, packet_id);
        }
    }
    state->write_queue_head = 0U;
    state->write_queue_len = 0U;
}

static int mesh_serial_queue_packet(struct mesh_serial_transport_state *state,
                                    const uint8_t *packet, size_t len, uint32_t packet_id) {
    if (state->write_queue_len >= MESH_SERIAL_MAX_OUTBOUND_PACKETS) {
        return -ENOSPC;
    }

    const size_t index =
        (state->write_queue_head + state->write_queue_len) % MESH_SERIAL_MAX_OUTBOUND_PACKETS;
    struct mesh_serial_outbound_packet *slot = &state->write_queue[index];
    size_t written = 0U;
    const int encoded =
        mesh_stream_frame_encode(packet, len, slot->data, sizeof slot->data, &written);
    if (encoded < 0) {
        return encoded;
    }

    slot->length = written;
    slot->sent = 0U;
    slot->packet_id = packet_id;
    state->write_queue_len += 1U;
    return 0;
}

/* Keeps EPOLLOUT armed exactly while the queue has a remainder, so a tty that filled up wakes
   the loop instead of waiting out the poll timeout. */
static void mesh_serial_update_write_interest(struct mesh_serial_transport_state *state) {
    if (!state->fd_registered || state->loop == NULL) {
        return;
    }
    const bool want = state->write_queue_len > 0U;
    if (want == state->want_write) {
        return;
    }
    const uint32_t events = want ? (uint32_t)(EPOLLIN | EPOLLOUT) : (uint32_t)EPOLLIN;
    if (mesh_event_loop_update_fd(state->loop, state->fd, events) == 0) {
        state->want_write = want;
    }
}

static int mesh_serial_flush_write_queue(struct mesh_serial_transport_state *state) {
    if (state->fd < 0) {
        return -ENOTCONN;
    }

    while (state->write_queue_len > 0U) {
        struct mesh_serial_outbound_packet *slot = &state->write_queue[state->write_queue_head];
        const ssize_t written =
            write(state->fd, slot->data + slot->sent, slot->length - slot->sent);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* the tty is full; EPOLLOUT brings us back */
            }
            if (errno == EINTR) {
                continue;
            }
            mesh_log_warn("serial", "write failed: %s", strerror(errno));
            mesh_serial_reset_link(state, "write failed");
            return -EIO;
        }

        slot->sent += (size_t)written;
        if (slot->sent < slot->length) {
            break;
        }
        state->write_queue_head = (state->write_queue_head + 1U) % MESH_SERIAL_MAX_OUTBOUND_PACKETS;
        state->write_queue_len -= 1U;
    }

    mesh_serial_update_write_interest(state);
    return 0;
}

static int mesh_serial_session_send(void *ctx, const uint8_t *packet, size_t len,
                                    uint32_t packet_id) {
    struct mesh_serial_transport_state *state = (struct mesh_serial_transport_state *)ctx;
    if (state == NULL || state->fd < 0) {
        return -ENOTCONN;
    }

    const int queued = mesh_serial_queue_packet(state, packet, len, packet_id);
    if (queued < 0) {
        return queued;
    }
    mesh_serial_update_write_interest(state);
    return mesh_serial_flush_write_queue(state);
}

/* ------------------------------------------------------------------ read path */

static void mesh_serial_on_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct mesh_serial_transport_state *state = (struct mesh_serial_transport_state *)ctx;
    state->frames_received += 1U;
    mesh_session_handle_from_radio(state->session, payload, len);
}

/* Whatever sits between frames is the radio's own log. Surface it at debug, one line at a time,
   with control bytes stripped so it cannot scribble on the terminal. */
static void mesh_serial_on_text(const uint8_t *text, size_t len, void *ctx) {
    (void)ctx;
    char line[160];
    size_t out = 0U;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = text[i];
        if (byte == '\n' || byte == '\r') {
            if (out > 0U) {
                line[out] = '\0';
                mesh_log_debug("serial", "radio: %s", line);
                out = 0U;
            }
            continue;
        }
        if (out + 1U >= sizeof line) {
            line[out] = '\0';
            mesh_log_debug("serial", "radio: %s", line);
            out = 0U;
        }
        line[out++] = (byte >= 0x20U && byte < 0x7FU) ? (char)byte : '.';
    }
    if (out > 0U) {
        line[out] = '\0';
        mesh_log_debug("serial", "radio: %s", line);
    }
}

int mesh_serial_transport_pump(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -EINVAL;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    if (state->fd < 0) {
        return -ENOTCONN;
    }

    const struct mesh_stream_parser_callbacks callbacks = {
        .on_frame = mesh_serial_on_frame,
        .on_text = mesh_serial_on_text,
        .ctx = state,
    };

    size_t total = 0U;
    for (unsigned turn = 0U; turn < MESH_SERIAL_READS_PER_TURN; ++turn) {
        uint8_t buffer[MESH_SERIAL_READ_CHUNK];
        const ssize_t got = read(state->fd, buffer, sizeof buffer);
        if (got > 0) {
            total += (size_t)got;
            state->bytes_received += (size_t)got;
            mesh_stream_parser_push(&state->parser, buffer, (size_t)got, &callbacks);
            continue;
        }
        if (got == 0) {
            /* A tty does not normally report EOF; the node was unplugged. */
            mesh_serial_reset_link(state, "port closed");
            return -ENOTCONN;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        mesh_log_warn("serial", "read failed: %s", strerror(errno));
        mesh_serial_reset_link(state, "read failed");
        return -EIO;
    }

    return (int)total;
}

static int mesh_serial_fd_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_transport *transport = (struct mesh_transport *)userdata;
    if (transport == NULL || transport->state == NULL) {
        return 0;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;

    if ((events & (uint32_t)(EPOLLERR | EPOLLHUP)) != 0U) {
        mesh_serial_reset_link(state, "port hung up");
        return 0;
    }
    if ((events & (uint32_t)EPOLLOUT) != 0U) {
        (void)mesh_serial_flush_write_queue(state);
    }
    if ((events & (uint32_t)EPOLLIN) != 0U) {
        (void)mesh_serial_transport_pump(transport);
    }
    return 0;
}

/* ------------------------------------------------------------------ link */

static void mesh_serial_detach_fd(struct mesh_serial_transport_state *state) {
    if (state->fd < 0) {
        return;
    }
    if (state->fd_registered && state->loop != NULL) {
        mesh_event_loop_remove_fd(state->loop, state->fd);
    }
    mesh_serial_port_close(state->fd);
    state->fd = -1;
    state->fd_registered = false;
    state->want_write = false;
}

static void mesh_serial_reset_link(struct mesh_serial_transport_state *state, const char *reason) {
    if (state == NULL || state->link_state == MESH_SERIAL_LINK_DISCONNECTED) {
        return;
    }
    char port[sizeof state->connected.path];
    snprintf(port, sizeof port, "%s",
             state->connected.path[0] != '\0' ? state->connected.path : "port");
    mesh_serial_detach_fd(state);
    state->link_state = MESH_SERIAL_LINK_DISCONNECTED;
    state->wake_done_at_ms = 0U;
    mesh_stream_parser_reset(&state->parser);
    mesh_session_detach(state->session);
    mesh_serial_clear_write_queue(state);
    memset(&state->connected, 0, sizeof state->connected);
    mesh_log_info("serial", "Disconnected from %s (%s)", port, reason);
}

/* Matches a sysfs interface id ("1-1:1.1") or a device node ("/dev/ttyUSB0"). */
static struct mesh_serial_device_info *
mesh_serial_find_device(struct mesh_serial_transport_state *state, const char *identifier) {
    for (size_t i = 0; i < state->device_count; ++i) {
        struct mesh_serial_device_info *device = &state->devices[i];
        if (strcmp(device->id, identifier) == 0) {
            return device;
        }
        if (device->path[0] != '\0' && strcmp(device->path, identifier) == 0) {
            return device;
        }
    }
    return NULL;
}

int mesh_serial_transport_connect(struct mesh_transport *transport, const char *identifier) {
    if (transport == NULL || transport->state == NULL || identifier == NULL ||
        identifier[0] == '\0') {
        return -EINVAL;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    /* A new attempt supersedes whatever the last one failed with. */
    state->last_error[0] = '\0';

    if (state->state == MESH_SERIAL_STATE_DISABLED) {
        mesh_serial_set_error(state, "USB serial is disabled");
        return -ENODEV;
    }
    if (state->link_state != MESH_SERIAL_LINK_DISCONNECTED) {
        return -EBUSY;
    }

    struct mesh_serial_device_info *device = mesh_serial_find_device(state, identifier);
    if (device == NULL) {
        mesh_serial_scan_internal(state);
        device = mesh_serial_find_device(state, identifier);
    }
    if (device == NULL) {
        mesh_log_warn("serial", "No USB serial port matches '%s'", identifier);
        mesh_serial_set_error(state, "USB port is gone; is it still plugged in?");
        return -ENODEV;
    }

    /* On the Brick the node has no driver until we ask for one, and no tty until it binds. */
    if (!device->bound || device->path[0] == '\0') {
        const int bind_result = mesh_serial_usb_bind(device);
        if (bind_result < 0) {
            mesh_serial_set_error(state, "%.20s: no USB serial driver (%d)", device->name,
                                  bind_result);
            return bind_result;
        }
    }

    const int fd = mesh_serial_port_open(device->path);
    if (fd < 0) {
        mesh_log_warn("serial", "Cannot open %s: %s", device->path, strerror(-fd));
        mesh_serial_set_error(state, "Cannot open %.24s: %.20s", device->path, strerror(-fd));
        return fd;
    }
    state->fd = fd;
    state->connected = *device;

    /*
     * The node discards its output until the host sets the CDC line state. The generic
     * usbserial driver cannot do that, so it goes out of band through usbfs; a real tty driver
     * takes the normal TIOCMBIS.
     */
    if (device->needs_line_state) {
        const int line_result = mesh_serial_usb_set_line_state(device, true, true);
        if (line_result < 0) {
            mesh_log_warn("serial", "%s: could not assert DTR (%s); the node may stay silent",
                          device->path, strerror(-line_result));
        }
    } else {
        const int dtr_result = mesh_serial_port_set_dtr(fd, true);
        if (dtr_result < 0 && dtr_result != -ENOTTY && dtr_result != -EINVAL) {
            mesh_log_debug("serial", "%s: TIOCMBIS failed (%s)", device->path,
                           strerror(-dtr_result));
        }
    }

    if (state->loop != NULL) {
        const int add_result =
            mesh_event_loop_add_fd(state->loop, fd, EPOLLIN, mesh_serial_fd_callback, transport);
        if (add_result < 0) {
            mesh_log_warn("serial", "Cannot watch %s: %d", device->path, add_result);
            mesh_serial_detach_fd(state);
            memset(&state->connected, 0, sizeof state->connected);
            return add_result;
        }
        state->fd_registered = true;
        state->want_write = false;
    }

    mesh_stream_parser_reset(&state->parser);
    state->frames_received = 0U;
    state->bytes_received = 0U;
    state->write_queue_head = 0U;
    state->write_queue_len = 0U;

    uint8_t wake[MESH_SERIAL_WAKE_BYTES];
    memset(wake, (int)MESH_STREAM_FRAME_START2, sizeof wake);
    if (write(fd, wake, sizeof wake) < 0) {
        mesh_log_debug("serial", "%s: wake write failed (%s)", device->path, strerror(errno));
    }

    state->link_state = MESH_SERIAL_LINK_WAKING;
    state->wake_done_at_ms = mesh_time_monotonic_ms() + MESH_SERIAL_WAKE_SETTLE_MS;
    mesh_log_info("serial", "Opened %s (%s); waking the radio", device->path, device->name);
    return 0;
}

/* Runs while WAKING: once the radio has had its moment, start the conversation. */
static void mesh_serial_finish_wake(struct mesh_serial_transport_state *state) {
    if (state->link_state != MESH_SERIAL_LINK_WAKING ||
        mesh_time_monotonic_ms() < state->wake_done_at_ms) {
        return;
    }

    state->link_state = MESH_SERIAL_LINK_CONNECTED;
    mesh_session_attach(state->session, mesh_serial_session_send, state);
    const int handshake = mesh_session_begin_handshake(state->session);
    if (handshake < 0) {
        mesh_log_warn("serial", "Failed to request config sync: %d", handshake);
        mesh_serial_set_error(state, "%.24s: the radio did not answer", state->connected.path);
        mesh_serial_reset_link(state, "handshake failed");
        return;
    }
    mesh_log_info("serial", "Connected to %s", state->connected.path);
}

int mesh_serial_transport_disconnect(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -EINVAL;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    if (state->link_state == MESH_SERIAL_LINK_DISCONNECTED) {
        return -ENOTCONN;
    }
    mesh_serial_reset_link(state, "requested");
    return 0;
}

/* ------------------------------------------------------------------ discovery */

static size_t mesh_serial_scan_internal(struct mesh_serial_transport_state *state) {
    state->device_count = mesh_serial_usb_scan(state->devices, MESH_SERIAL_MAX_DEVICES);
    if (state->state != MESH_SERIAL_STATE_DISABLED) {
        state->state = state->device_count > 0U ? MESH_SERIAL_STATE_READY : MESH_SERIAL_STATE_IDLE;
    }
    return state->device_count;
}

size_t mesh_serial_transport_refresh_devices(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return 0U;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    state->last_scan_ms = mesh_time_monotonic_ms();
    return mesh_serial_scan_internal(state);
}

const struct mesh_serial_device_info *
mesh_serial_transport_devices(struct mesh_transport *transport, size_t *count) {
    if (transport == NULL || transport->state == NULL) {
        if (count != NULL) {
            *count = 0U;
        }
        return NULL;
    }
    const struct mesh_serial_transport_state *state =
        (const struct mesh_serial_transport_state *)transport->state;
    if (count != NULL) {
        *count = state->device_count;
    }
    return state->devices;
}

size_t mesh_serial_transport_get_devices(struct mesh_transport *transport,
                                         struct mesh_serial_device_info *out, size_t capacity) {
    if (out == NULL || capacity == 0U) {
        return 0U;
    }
    size_t count = 0U;
    const struct mesh_serial_device_info *devices =
        mesh_serial_transport_devices(transport, &count);
    if (devices == NULL) {
        return 0U;
    }
    if (count > capacity) {
        count = capacity;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = devices[i];
    }
    return count;
}

/* ------------------------------------------------------------------ transport ops */

static void mesh_serial_tick(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    if (state->state == MESH_SERIAL_STATE_DISABLED) {
        return;
    }

    const uint64_t now = mesh_time_monotonic_ms();

    if (state->link_state == MESH_SERIAL_LINK_WAKING) {
        mesh_serial_finish_wake(state);
    } else if (state->link_state == MESH_SERIAL_LINK_CONNECTED) {
        (void)mesh_serial_flush_write_queue(state);
        mesh_session_tick(state->session, now);
    }

    /* Only rescan while idle: a live link holds the port, and sysfs will not change under it. */
    if (state->link_state == MESH_SERIAL_LINK_DISCONNECTED &&
        now - state->last_scan_ms >= MESH_SERIAL_SCAN_INTERVAL_MS) {
        state->last_scan_ms = now;
        mesh_serial_scan_internal(state);
    }
}

static int mesh_serial_start(struct mesh_transport *transport, const struct mesh_app_config *config,
                             struct mesh_event_loop *loop) {
    if (transport == NULL || config == NULL || transport->state == NULL) {
        return -EINVAL;
    }

    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    /* The injected session outlives a restart; everything else is cleared. */
    struct mesh_session *injected = state->session != &state->own_session ? state->session : NULL;
    memset(state, 0, sizeof *state);
    state->session = injected;
    state->fd = -1;
    state->loop = loop;
    state->link_state = MESH_SERIAL_LINK_DISCONNECTED;
    mesh_stream_parser_reset(&state->parser);
    /* The app hands every link the same session; standalone (tests, --list-devices) each link
       falls back to its own and initialises it here. */
    if (state->session == NULL) {
        state->session = &state->own_session;
    }
    if (state->session == &state->own_session) {
        mesh_session_init(state->session);
    }

    if (!config->enable_serial) {
        mesh_log_info("serial", "Serial transport disabled by configuration");
        state->state = MESH_SERIAL_STATE_DISABLED;
        return 0;
    }

    state->state = MESH_SERIAL_STATE_IDLE;
    const size_t found = mesh_serial_scan_internal(state);
    state->last_scan_ms = mesh_time_monotonic_ms();
    if (found == 0U) {
        mesh_log_info("serial", "No USB serial ports found; watching for a node to be plugged in");
    } else {
        for (size_t i = 0; i < found; ++i) {
            const struct mesh_serial_device_info *device = &state->devices[i];
            mesh_log_info("serial", "Found %s (%04x:%04x) at %s%s", device->name, device->vendor_id,
                          device->product_id, device->bound ? device->path : "(unbound)",
                          device->needs_line_state ? ", needs DTR over usbfs" : "");
        }
    }

    return 0;
}

static void mesh_serial_stop(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    mesh_serial_reset_link(state, "shutting down");
    mesh_serial_detach_fd(state);
    state->device_count = 0U;
    state->loop = NULL;
    state->state = MESH_SERIAL_STATE_IDLE;
}

static const char *mesh_serial_status(const struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return "unavailable";
    }
    const struct mesh_serial_transport_state *state =
        (const struct mesh_serial_transport_state *)transport->state;
    switch (state->link_state) {
    case MESH_SERIAL_LINK_WAKING:
        return "connecting";
    case MESH_SERIAL_LINK_CONNECTED:
        return "connected";
    case MESH_SERIAL_LINK_DISCONNECTED:
        break;
    }
    return mesh_serial_state_to_string(state->state);
}

static void mesh_serial_set_session(struct mesh_transport *transport,
                                    struct mesh_session *session) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    ((struct mesh_serial_transport_state *)transport->state)->session = session;
}

static bool mesh_serial_take_error(struct mesh_transport *transport, char *out, size_t out_len) {
    if (transport == NULL || transport->state == NULL || out == NULL || out_len == 0U) {
        return false;
    }
    struct mesh_serial_transport_state *state =
        (struct mesh_serial_transport_state *)transport->state;
    if (state->last_error[0] == '\0') {
        return false;
    }
    snprintf(out, out_len, "%s", state->last_error);
    state->last_error[0] = '\0';
    return true;
}

static const struct mesh_transport_ops k_serial_ops = {
    .start = mesh_serial_start,
    .stop = mesh_serial_stop,
    .status = mesh_serial_status,
    .tick = mesh_serial_tick,
    .set_session = mesh_serial_set_session,
    .take_error = mesh_serial_take_error,
};

/* ------------------------------------------------------------------ accessors */

const char *mesh_serial_transport_connected_port(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return NULL;
    }
    const struct mesh_serial_transport_state *state =
        (const struct mesh_serial_transport_state *)transport->state;
    if (state->link_state != MESH_SERIAL_LINK_CONNECTED || state->connected.path[0] == '\0') {
        return NULL;
    }
    return state->connected.path;
}

bool mesh_serial_transport_is_connecting(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return false;
    }
    const struct mesh_serial_transport_state *state =
        (const struct mesh_serial_transport_state *)transport->state;
    return state->link_state == MESH_SERIAL_LINK_WAKING;
}

struct mesh_serial_transport_stats mesh_serial_transport_stats(struct mesh_transport *transport) {
    struct mesh_serial_transport_stats stats = {0U, 0U, 0U};
    if (transport == NULL || transport->state == NULL) {
        return stats;
    }
    const struct mesh_serial_transport_state *state =
        (const struct mesh_serial_transport_state *)transport->state;
    stats.frames_received = state->frames_received;
    stats.bytes_received = state->bytes_received;
    stats.junk_bytes = state->parser.dropped_bytes;
    return stats;
}

struct mesh_session *mesh_serial_transport_session(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return NULL;
    }
    return ((struct mesh_serial_transport_state *)transport->state)->session;
}

struct mesh_handshake_status
mesh_serial_transport_handshake_status(struct mesh_transport *transport) {
    struct mesh_handshake_status status;
    const struct mesh_session *session = mesh_serial_transport_session(transport);
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

struct mesh_transport *mesh_serial_transport(void) {
    static struct mesh_serial_transport_state state = {.fd = -1};
    static struct mesh_transport transport = {
        .name = "serial",
        .state = &state,
        .ops = &k_serial_ops,
    };
    return &transport;
}
