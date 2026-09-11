#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/i18n/strings.h"
#include "mesh/transport/tcp.h"

#include "mesh/core/config.h"
#include "mesh/transport/stream_link.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * How long a connect is given before it is called lost.
 *
 * The kernel's own answer is a couple of minutes of SYN retries, which on a handheld reads as a
 * press that did nothing. This is a link to a box on the same WiFi as us; five seconds is
 * already generous, and failing fast is what lets auto-connect fall back to a cable or to
 * Bluetooth instead of sitting on a dead address.
 */
#define MESH_TCP_CONNECT_TIMEOUT_MS 5000U

/*
 * How often a quiet link says something.
 *
 * The radio drops a client that has gone silent, and a client with nothing to send is the
 * ordinary state of this link - a mesh can be quiet for hours. Thirty seconds is well inside
 * any firmware's patience and costs one 3-byte frame a minute each way.
 */
#define MESH_TCP_HEARTBEAT_INTERVAL_MS 30000U

/* Keepalive on the socket underneath, which is the other half of the same problem: a heartbeat
   proves the radio still wants us, and this is what notices that the network went away without
   anybody closing anything - a Brick carried out of WiFi range, most often. */
#define MESH_TCP_KEEPALIVE_IDLE_S 30
#define MESH_TCP_KEEPALIVE_INTERVAL_S 10
#define MESH_TCP_KEEPALIVE_COUNT 3

enum mesh_tcp_state {
    MESH_TCP_STATE_DISABLED = 0,
    MESH_TCP_STATE_IDLE,  /* enabled, but nothing has been configured to connect to */
    MESH_TCP_STATE_READY, /* a target is known */
};

enum mesh_tcp_link_state {
    MESH_TCP_LINK_DISCONNECTED = 0,
    MESH_TCP_LINK_CONNECTING, /* the socket exists, the handshake has not gone out */
    MESH_TCP_LINK_CONNECTED,
};

struct mesh_tcp_transport_state {
    enum mesh_tcp_state state;
    enum mesh_tcp_link_state link_state;
    struct mesh_event_loop *loop;

    /* Everything about an established stream that a serial link does identically. */
    struct mesh_stream_link link;

    /*
     * The connecting socket, which is deliberately not the link's.
     *
     * A link is an established stream: it watches for readability and reads. A connect in flight
     * is the opposite - it watches for *writability*, because that is how a non-blocking connect
     * reports, and reading it would be meaningless. Handing the descriptor over at the moment
     * the connect succeeds is what keeps `struct mesh_stream_link` free of a state where its
     * pump() must not be called.
     */
    int pending_fd;
    bool pending_registered;
    uint64_t connect_deadline_ms;

    char target[MESH_TCP_TARGET_MAX];     /* what is being connected to, or what is up */
    char configured[MESH_TCP_TARGET_MAX]; /* what the config said, whether or not it is up */
    uint64_t next_heartbeat_ms;

    /* Owned only when nothing was injected; `session` is what the code uses. */
    struct mesh_session own_session;
    struct mesh_session *session;
    /* Why the last connect attempt failed, in words, waiting to be shown once. */
    char last_error[MESH_TRANSPORT_ERROR_MAX];
};

static void mesh_tcp_reset_link(struct mesh_tcp_transport_state *state, const char *reason);

/* Records a failure for the UI to pick up. First one wins until it is read. */
static void mesh_tcp_set_error(struct mesh_tcp_transport_state *state, enum mesh_str_id text, ...) {
    if (state == NULL || state->last_error[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, text);
    (void)mesh_str_vformat(state->last_error, sizeof state->last_error, text, args);
    va_end(args);
}

static const char *mesh_tcp_state_to_string(enum mesh_tcp_state state) {
    switch (state) {
    case MESH_TCP_STATE_DISABLED:
        return mesh_str(MESH_STR_TRANSPORT_DISABLED);
    case MESH_TCP_STATE_IDLE:
        return mesh_str(MESH_STR_TRANSPORT_NO_HOST);
    case MESH_TCP_STATE_READY:
        return mesh_str(MESH_STR_TRANSPORT_RUNNING);
    }
    return mesh_str(MESH_STR_TRANSPORT_UNKNOWN);
}

/* ------------------------------------------------------------------ the target */

int mesh_tcp_target_split(const char *target, char *host, size_t host_len, uint16_t *port) {
    if (target == NULL || host == NULL || host_len == 0U || port == NULL) {
        return -EINVAL;
    }

    const size_t len = strlen(target);
    if (len == 0U || len >= MESH_TCP_TARGET_MAX) {
        return -EINVAL;
    }

    const char *host_start = target;
    size_t host_chars = len;
    const char *port_text = NULL;

    if (target[0] == '[') {
        /* "[fd00::1]:4403" - the brackets exist precisely because a bare v6 literal is all
           colons and the port separator would be unfindable without them. */
        const char *close = strchr(target, ']');
        if (close == NULL || close == target + 1) {
            return -EINVAL;
        }
        host_start = target + 1;
        host_chars = (size_t)(close - host_start);
        if (close[1] == ':') {
            port_text = close + 2;
        } else if (close[1] != '\0') {
            return -EINVAL;
        }
    } else {
        const char *colon = strchr(target, ':');
        /* One colon is "host:port". More than one is a bare v6 literal, which has no port on
           it: there is nowhere for one to go without brackets. */
        if (colon != NULL && strchr(colon + 1, ':') == NULL) {
            host_chars = (size_t)(colon - target);
            port_text = colon + 1;
        }
    }

    if (host_chars == 0U || host_chars >= host_len) {
        return -EINVAL;
    }

    uint16_t parsed_port = (uint16_t)MESH_TCP_DEFAULT_PORT;
    if (port_text != NULL) {
        if (port_text[0] == '\0') {
            return -EINVAL;
        }
        char *end = NULL;
        const unsigned long value = strtoul(port_text, &end, 10);
        if (end == NULL || *end != '\0' || value == 0UL || value > 65535UL) {
            return -EINVAL;
        }
        parsed_port = (uint16_t)value;
    }

    memcpy(host, host_start, host_chars);
    host[host_chars] = '\0';
    *port = parsed_port;
    return 0;
}

/*
 * Fills `out` from a numeric host, or reports that the host is a name.
 *
 * inet_pton rather than getaddrinfo, and that is the whole of why this client will not take a
 * hostname: getaddrinfo blocks, and the one thing this process must never do is block. Returns 0,
 * or -EINVAL when the text is not a literal address.
 */
static int mesh_tcp_address_from_text(const char *host, uint16_t port, struct sockaddr_storage *out,
                                      socklen_t *out_len) {
    memset(out, 0, sizeof *out);

    struct in_addr v4;
    if (inet_pton(AF_INET, host, &v4) == 1) {
        struct sockaddr_in *addr = (struct sockaddr_in *)out;
        addr->sin_family = AF_INET;
        addr->sin_port = htons(port);
        addr->sin_addr = v4;
        *out_len = (socklen_t)sizeof *addr;
        return 0;
    }

    struct in6_addr v6;
    if (inet_pton(AF_INET6, host, &v6) == 1) {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)out;
        addr->sin6_family = AF_INET6;
        addr->sin6_port = htons(port);
        addr->sin6_addr = v6;
        *out_len = (socklen_t)sizeof *addr;
        return 0;
    }

    return -EINVAL;
}

/* ------------------------------------------------------------------ send */

static int mesh_tcp_session_send(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id) {
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)ctx;
    if (state == NULL) {
        return -ENOTCONN;
    }
    const int result = mesh_stream_link_send(&state->link, packet, len, packet_id);
    if (result == -EIO) {
        mesh_tcp_reset_link(state, "write failed");
    }
    return result;
}

/* ------------------------------------------------------------------ read path */

int mesh_tcp_transport_pump(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -EINVAL;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;

    const int result = mesh_stream_link_pump(&state->link);
    if (result == -ENOTCONN) {
        mesh_tcp_reset_link(state, "the radio closed the connection");
    } else if (result == -EIO) {
        mesh_tcp_reset_link(state, "read failed");
    }
    return result;
}

/* ------------------------------------------------------------------ connecting */

static void mesh_tcp_drop_pending(struct mesh_tcp_transport_state *state) {
    if (state->pending_fd < 0) {
        return;
    }
    if (state->pending_registered && state->loop != NULL) {
        mesh_event_loop_remove_fd(state->loop, state->pending_fd);
    }
    close(state->pending_fd);
    state->pending_fd = -1;
    state->pending_registered = false;
}

static int mesh_tcp_fd_callback(int fd, uint32_t events, void *userdata);

/*
 * The connect finished. Hand the descriptor to the link, start the conversation.
 *
 * There is no wake burst and no settle here, unlike the serial link: a socket that has just
 * completed a three-way handshake has a far end that is listening by definition, and nothing was
 * on this stream before us to leave a parser mid-frame.
 */
static void mesh_tcp_finish_connect(struct mesh_tcp_transport_state *state,
                                    struct mesh_transport *transport) {
    int error = 0;
    socklen_t error_len = (socklen_t)sizeof error;
    if (getsockopt(state->pending_fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
        error = errno;
    }
    if (error != 0) {
        mesh_log_warn("tcp", "Cannot reach %s: %s", state->target, strerror(error));
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_UNREACHABLE, state->target, strerror(error));
        mesh_tcp_reset_link(state, "connect failed");
        return;
    }

    const int fd = state->pending_fd;
    /* Taken off the state before the link adopts it: the link owns the descriptor from here, and
       two owners is one double close. */
    if (state->pending_registered && state->loop != NULL) {
        mesh_event_loop_remove_fd(state->loop, fd);
    }
    state->pending_fd = -1;
    state->pending_registered = false;

    const int opened = mesh_stream_link_open(&state->link, fd, MESH_STREAM_LINK_SOCKET, state->loop,
                                             mesh_tcp_fd_callback, transport);
    if (opened < 0) {
        mesh_log_warn("tcp", "Cannot watch %s: %d", state->target, opened);
        close(fd);
        mesh_tcp_reset_link(state, "could not watch the socket");
        return;
    }

    state->link_state = MESH_TCP_LINK_CONNECTED;
    state->connect_deadline_ms = 0U;
    mesh_session_attach(state->session, mesh_tcp_session_send, state);
    const int handshake = mesh_session_begin_handshake(state->session);
    if (handshake < 0) {
        mesh_log_warn("tcp", "Failed to request config sync: %d", handshake);
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_NO_ANSWER, state->target);
        mesh_tcp_reset_link(state, "handshake failed");
        return;
    }
    state->next_heartbeat_ms = mesh_time_monotonic_ms() + MESH_TCP_HEARTBEAT_INTERVAL_MS;
    mesh_log_info("tcp", "Connected to %s", state->target);
}

static int mesh_tcp_fd_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_transport *transport = (struct mesh_transport *)userdata;
    if (transport == NULL || transport->state == NULL) {
        return 0;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;

    if (state->link_state == MESH_TCP_LINK_CONNECTING) {
        /* EPOLLERR here is the ordinary refusal - nothing is listening on that port - and
           getsockopt() is what turns it into the errno to say so. Both arms go the same way. */
        if ((events & (uint32_t)(EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
            mesh_tcp_finish_connect(state, transport);
        }
        return 0;
    }

    if ((events & (uint32_t)(EPOLLERR | EPOLLHUP)) != 0U) {
        mesh_tcp_reset_link(state, "the connection dropped");
        return 0;
    }
    if ((events & (uint32_t)EPOLLOUT) != 0U) {
        if (mesh_stream_link_flush(&state->link) == -EIO) {
            mesh_tcp_reset_link(state, "write failed");
            return 0;
        }
    }
    if ((events & (uint32_t)EPOLLIN) != 0U) {
        (void)mesh_tcp_transport_pump(transport);
    }
    return 0;
}

/* ------------------------------------------------------------------ link */

static void mesh_tcp_reset_link(struct mesh_tcp_transport_state *state, const char *reason) {
    if (state == NULL || state->link_state == MESH_TCP_LINK_DISCONNECTED) {
        return;
    }
    char target[MESH_TCP_TARGET_MAX];
    mesh_str_copy(target, sizeof target, state->target[0] != '\0' ? state->target : "the radio");

    state->link_state = MESH_TCP_LINK_DISCONNECTED;
    state->connect_deadline_ms = 0U;
    state->next_heartbeat_ms = 0U;
    mesh_session_detach(state->session);
    mesh_tcp_drop_pending(state);
    mesh_stream_link_close(&state->link);
    state->target[0] = '\0';
    mesh_log_info("tcp", "Disconnected from %s (%s)", target, reason);
}

int mesh_tcp_transport_connect(struct mesh_transport *transport, const char *target) {
    if (transport == NULL || transport->state == NULL || target == NULL || target[0] == '\0') {
        return -EINVAL;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    /* A new attempt supersedes whatever the last one failed with. */
    state->last_error[0] = '\0';

    if (state->state == MESH_TCP_STATE_DISABLED) {
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_DISABLED);
        return -ENODEV;
    }
    if (state->link_state != MESH_TCP_LINK_DISCONNECTED) {
        return -EBUSY;
    }

    char host[MESH_TCP_TARGET_MAX];
    uint16_t port = 0U;
    if (mesh_tcp_target_split(target, host, sizeof host, &port) < 0) {
        mesh_log_warn("tcp", "'%s' is not an address and port", target);
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_BAD_TARGET, target);
        return -EINVAL;
    }

    struct sockaddr_storage address;
    socklen_t address_len = 0;
    if (mesh_tcp_address_from_text(host, port, &address, &address_len) < 0) {
        mesh_log_warn("tcp", "'%s' is a name; this client needs a numeric address", host);
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_NEEDS_ADDRESS, host);
        return -EINVAL;
    }

    const int fd = socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        const int error = errno;
        mesh_log_warn("tcp", "Cannot open a socket: %s", strerror(error));
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_UNREACHABLE, target, strerror(error));
        return -error;
    }

    /* Meshtastic frames are small and a reply usually follows a request immediately, which is
       exactly the traffic Nagle delays. */
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    const int idle = MESH_TCP_KEEPALIVE_IDLE_S;
    const int interval = MESH_TCP_KEEPALIVE_INTERVAL_S;
    const int count = MESH_TCP_KEEPALIVE_COUNT;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
#endif

    mesh_str_copy(state->target, sizeof state->target, target);

    const int connected = connect(fd, (const struct sockaddr *)&address, address_len);
    if (connected < 0 && errno != EINPROGRESS) {
        const int error = errno;
        mesh_log_warn("tcp", "Cannot reach %s: %s", target, strerror(error));
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_UNREACHABLE, target, strerror(error));
        close(fd);
        state->target[0] = '\0';
        return -error;
    }

    state->pending_fd = fd;
    state->pending_registered = false;
    if (state->loop != NULL) {
        /*
         * EPOLLOUT, not EPOLLIN: a non-blocking connect reports by becoming writable, whether it
         * succeeded or was refused. The link's own EPOLLIN registration happens when the
         * descriptor is handed over.
         */
        const int added =
            mesh_event_loop_add_fd(state->loop, fd, EPOLLOUT, mesh_tcp_fd_callback, transport);
        if (added < 0) {
            mesh_log_warn("tcp", "Cannot watch %s: %d", target, added);
            close(fd);
            state->pending_fd = -1;
            state->target[0] = '\0';
            return added;
        }
        state->pending_registered = true;
    }

    state->link_state = MESH_TCP_LINK_CONNECTING;
    state->connect_deadline_ms = mesh_time_monotonic_ms() + MESH_TCP_CONNECT_TIMEOUT_MS;
    mesh_log_info("tcp", "Connecting to %s", target);

    /*
     * A loopback connect is usually complete before connect() returns, and with no loop to make
     * it writable again - a test driving this by hand, most often - nothing would ever finish it.
     */
    if (connected == 0) {
        mesh_tcp_finish_connect(state, transport);
        /*
         * And if finishing it immediately is also how it failed, say so rather than reporting a
         * connect that is under way. Nothing is: the only caller that could find out otherwise is
         * one that polls, and --status polling a link that died in this call prints "no
         * handshake" where it could have printed why.
         */
        if (state->link_state == MESH_TCP_LINK_DISCONNECTED) {
            return -ECONNREFUSED;
        }
    }
    return 0;
}

int mesh_tcp_transport_disconnect(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -EINVAL;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    if (state->link_state == MESH_TCP_LINK_DISCONNECTED) {
        return -ENOTCONN;
    }
    mesh_tcp_reset_link(state, "requested");
    return 0;
}

/* ------------------------------------------------------------------ transport ops */

static void mesh_tcp_tick(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    if (state->state == MESH_TCP_STATE_DISABLED) {
        return;
    }

    const uint64_t now = mesh_time_monotonic_ms();

    if (state->link_state == MESH_TCP_LINK_CONNECTING) {
        if (state->connect_deadline_ms != 0U && now >= state->connect_deadline_ms) {
            mesh_log_warn("tcp", "%s did not answer in time", state->target);
            mesh_tcp_set_error(state, MESH_STR_LINK_TCP_TIMEOUT, state->target);
            mesh_tcp_reset_link(state, "connect timed out");
        }
        return;
    }

    if (state->link_state != MESH_TCP_LINK_CONNECTED) {
        return;
    }

    if (mesh_stream_link_flush(&state->link) == -EIO) {
        mesh_tcp_reset_link(state, "write failed");
        return;
    }

    if (state->next_heartbeat_ms != 0U && now >= state->next_heartbeat_ms) {
        state->next_heartbeat_ms = now + MESH_TCP_HEARTBEAT_INTERVAL_MS;
        const int beat = mesh_session_send_heartbeat(state->session);
        if (beat == -EIO) {
            mesh_tcp_reset_link(state, "write failed");
            return;
        }
    }

    mesh_session_tick(state->session, now);
}

static int mesh_tcp_start(struct mesh_transport *transport, const struct mesh_app_config *config,
                          struct mesh_event_loop *loop) {
    if (transport == NULL || config == NULL || transport->state == NULL) {
        return -EINVAL;
    }

    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    /* The injected session outlives a restart; everything else is cleared. */
    struct mesh_session *injected = state->session != &state->own_session ? state->session : NULL;
    memset(state, 0, sizeof *state);
    state->session = injected;
    state->pending_fd = -1;
    state->loop = loop;
    state->link_state = MESH_TCP_LINK_DISCONNECTED;
    /* The app hands every link the same session; standalone (tests, --list-devices) each link
       falls back to its own and initialises it here. */
    if (state->session == NULL) {
        state->session = &state->own_session;
    }
    if (state->session == &state->own_session) {
        mesh_session_init(state->session);
    }
    mesh_stream_link_init(&state->link, "tcp", state->session);

    if (!config->enable_tcp) {
        mesh_log_info("tcp", "Network transport disabled by configuration");
        state->state = MESH_TCP_STATE_DISABLED;
        return 0;
    }

    mesh_str_copy(state->configured, sizeof state->configured, config->preferred_tcp_host);
    if (state->configured[0] == '\0') {
        state->state = MESH_TCP_STATE_IDLE;
        mesh_log_debug("tcp", "No host configured; nothing to connect to");
    } else {
        state->state = MESH_TCP_STATE_READY;
        mesh_log_info("tcp", "Configured for %s", state->configured);
    }
    return 0;
}

static void mesh_tcp_stop(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    mesh_tcp_reset_link(state, "shutting down");
    mesh_tcp_drop_pending(state);
    mesh_stream_link_close(&state->link);
    state->loop = NULL;
    state->state = MESH_TCP_STATE_IDLE;
}

static const char *mesh_tcp_status(const struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return mesh_str(MESH_STR_TRANSPORT_UNAVAILABLE);
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    switch (state->link_state) {
    case MESH_TCP_LINK_CONNECTING:
        return mesh_str(MESH_STR_TRANSPORT_CONNECTING);
    case MESH_TCP_LINK_CONNECTED:
        return mesh_str(MESH_STR_TRANSPORT_CONNECTED);
    case MESH_TCP_LINK_DISCONNECTED:
        break;
    }
    return mesh_tcp_state_to_string(state->state);
}

static void mesh_tcp_set_session(struct mesh_transport *transport, struct mesh_session *session) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    state->session = session;
    mesh_stream_link_set_session(&state->link, session);
}

static bool mesh_tcp_take_error(struct mesh_transport *transport, char *out, size_t out_len) {
    if (transport == NULL || transport->state == NULL || out == NULL || out_len == 0U) {
        return false;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    if (state->last_error[0] == '\0') {
        return false;
    }
    mesh_str_copy(out, out_len, state->last_error);
    state->last_error[0] = '\0';
    return true;
}

static const struct mesh_transport_ops k_tcp_ops = {
    .start = mesh_tcp_start,
    .stop = mesh_tcp_stop,
    .status = mesh_tcp_status,
    .tick = mesh_tcp_tick,
    .set_session = mesh_tcp_set_session,
    .take_error = mesh_tcp_take_error,
};

/* ------------------------------------------------------------------ accessors */

const char *mesh_tcp_transport_connected_target(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return NULL;
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    if (state->link_state != MESH_TCP_LINK_CONNECTED || state->target[0] == '\0') {
        return NULL;
    }
    return state->target;
}

const char *mesh_tcp_transport_configured_target(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return NULL;
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    return state->configured[0] != '\0' ? state->configured : NULL;
}

bool mesh_tcp_transport_is_connecting(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return false;
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    return state->link_state == MESH_TCP_LINK_CONNECTING;
}

struct mesh_tcp_transport_stats mesh_tcp_transport_stats(struct mesh_transport *transport) {
    struct mesh_tcp_transport_stats stats = {0U, 0U, 0U};
    if (transport == NULL || transport->state == NULL) {
        return stats;
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    const struct mesh_stream_link_stats link = mesh_stream_link_stats(&state->link);
    stats.frames_received = link.frames_received;
    stats.bytes_received = link.bytes_received;
    stats.junk_bytes = link.junk_bytes;
    return stats;
}

struct mesh_session *mesh_tcp_transport_session(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return NULL;
    }
    return ((struct mesh_tcp_transport_state *)transport->state)->session;
}

struct mesh_handshake_status mesh_tcp_transport_handshake_status(struct mesh_transport *transport) {
    struct mesh_handshake_status status;
    const struct mesh_session *session = mesh_tcp_transport_session(transport);
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

struct mesh_transport *mesh_tcp_transport(void) {
    static struct mesh_tcp_transport_state state = {.pending_fd = -1, .link = {.fd = -1}};
    static struct mesh_transport transport = {
        .name = "tcp",
        .state = &state,
        .ops = &k_tcp_ops,
    };
    return &transport;
}
