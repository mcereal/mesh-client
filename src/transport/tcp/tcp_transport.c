#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include "inkwell/net/reason.h"
#include "inkwell/net/tcp.h"
#include "mesh/i18n/net_reason.h"
#include "mesh/i18n/strings.h"
#include "mesh/transport/tcp.h"

#include "mesh/core/config.h"
#include "mesh/transport/stream_link.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    MESH_TCP_LINK_CONNECTED,
};

struct mesh_tcp_transport_state {
    enum mesh_tcp_state state;
    enum mesh_tcp_link_state link_state;
    struct inkwell_loop *loop;

    /* Everything about an established stream that a serial link does identically. */
    struct mesh_stream_link link;

    /* The platform half: asynchronous resolve, socket setup, and the connect deadline. */
    struct inkwell_tcp_connector connector;

    char target[MESH_TCP_TARGET_MAX]; /* what is being connected to, or what is up */
    /*
     * The last host this transport was pointed at, whether or not it is up: the configuration
     * at startup, and then whatever a connect asked for. Updated by connect() rather than by a
     * setter of its own, so the two cannot drift - a press that reached the link but not this
     * would be reconnected, after the first drop, to the host it replaced, or to none at all
     * when the startup configuration named none.
     */
    char configured[MESH_TCP_TARGET_MAX];
    uint64_t next_heartbeat_ms;

    /* Owned only when nothing was injected; `session` is what the code uses. */
    struct mesh_session own_session;
    struct mesh_session *session;
    /*
     * Why the last connect attempt failed, waiting to be shown once - as a reason and a number
     * rather than as a sentence, because the sentence is this client's to write and not the
     * link's. take_error() below is where the two become words.
     *
     * Two fields and not one because there are two kinds of failure here, and the split is the
     * extraction seam made visible. `failure` is inkwell's vocabulary for getting to a host,
     * which every link that reaches a network fails in the same ways; `own_failure` is the
     * three this link says itself - a policy that refused to try, a target only this link knows
     * the shape of, and a radio that did not finish the handshake once the socket was up. The
     * socket half is what eventually moves down to inkwell, and it only ever sets the first.
     */
    struct inkwell_net_failure failure;
    inkcell_str_id own_failure;
    bool own_failure_set;
    /* Whichever of the target or the host the failure was about, copied because the fields it
       came from are cleared as the attempt is torn down - and it is the user's own typed text
       echoed back, never a word this code chose. */
    char failure_subject[MESH_TCP_TARGET_MAX];
};

static void mesh_tcp_reset_link(struct mesh_tcp_transport_state *state, const char *reason);

static void mesh_tcp_clear_error(struct mesh_tcp_transport_state *state) {
    if (state == NULL) {
        return;
    }
    state->failure.reason = INKWELL_NET_OK;
    state->failure.detail = 0;
    state->own_failure_set = false;
    state->failure_subject[0] = '\0';
}

/* True while a failure is recorded and has not been read. */
static bool mesh_tcp_has_error(const struct mesh_tcp_transport_state *state) {
    return state != NULL && (inkwell_net_failed(&state->failure) || state->own_failure_set);
}

static void mesh_tcp_set_subject(struct mesh_tcp_transport_state *state, const char *subject) {
    inkwell_str_copy(state->failure_subject, sizeof state->failure_subject,
                     subject != NULL ? subject : "");
}

/*
 * Records a failure for the UI to pick up. First one wins until it is read, which is what keeps
 * the sentence the one that explains the attempt rather than the one from whatever tore it down
 * afterwards.
 *
 * `detail` is the number behind the reason - a negative errno for UNREACHABLE - and 0 where the
 * reason says everything. `subject` is the target or host the failure is about, as the user
 * wrote it.
 */
static void mesh_tcp_fail(struct mesh_tcp_transport_state *state, enum inkwell_net_reason reason,
                          int detail, const char *subject) {
    if (state == NULL || mesh_tcp_has_error(state)) {
        return;
    }
    state->failure.reason = reason;
    state->failure.detail = detail;
    mesh_tcp_set_subject(state, subject);
}

/* The same, for the three failures that are this link's own and have no reason in inkwell's
   vocabulary. See the note on `own_failure` in the state above. */
static void mesh_tcp_fail_own(struct mesh_tcp_transport_state *state, inkcell_str_id text,
                              const char *subject) {
    if (state == NULL || mesh_tcp_has_error(state)) {
        return;
    }
    state->own_failure = text;
    state->own_failure_set = true;
    mesh_tcp_set_subject(state, subject);
}

static const char *mesh_tcp_state_to_string(enum mesh_tcp_state state) {
    switch (state) {
    case MESH_TCP_STATE_DISABLED:
        return inkcell_str(MESH_STR_TRANSPORT_DISABLED);
    case MESH_TCP_STATE_IDLE:
        return inkcell_str(MESH_STR_TRANSPORT_NO_HOST);
    case MESH_TCP_STATE_READY:
        return inkcell_str(MESH_STR_TRANSPORT_RUNNING);
    }
    return inkcell_str(MESH_STR_TRANSPORT_UNKNOWN);
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

static int mesh_tcp_fd_callback(int fd, uint32_t events, void *userdata);

/*
 * The generic connector finished. Hand its descriptor to the Meshtastic stream and start the
 * application handshake, which is deliberately above that connector.
 *
 * There is no wake burst and no settle here, unlike the serial link: a socket that has just
 * completed a three-way handshake has a far end that is listening by definition, and nothing was
 * on this stream before us to leave a parser mid-frame.
 */
static void mesh_tcp_on_connected(void *userdata, const struct inkwell_tcp_connect_result *result) {
    struct mesh_transport *transport = (struct mesh_transport *)userdata;
    if (transport == NULL || transport->state == NULL || result == NULL) {
        return;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;

    if (inkwell_net_failed(&result->failure)) {
        char host[MESH_TCP_TARGET_MAX];
        uint16_t port = 0U;
        (void)mesh_tcp_target_split(state->target, host, sizeof host, &port);
        const bool lookup = result->failure.reason == INKWELL_NET_UNKNOWN_HOST ||
                            result->failure.reason == INKWELL_NET_LOOKUP_FAILED ||
                            result->failure.reason == INKWELL_NET_LOOKUP_TIMED_OUT;
        mesh_tcp_fail(state, result->failure.reason, result->failure.detail,
                      lookup ? host : state->target);
        inkwell_log_warn("tcp", "Cannot connect to %s: %s", state->target,
                         inkwell_net_reason_name(result->failure.reason));
        mesh_tcp_reset_link(state, "connect failed");
        return;
    }

    const int opened = mesh_stream_link_open(&state->link, result->fd, MESH_STREAM_LINK_SOCKET,
                                             state->loop, mesh_tcp_fd_callback, transport);
    if (opened < 0) {
        inkwell_log_warn("tcp", "Cannot watch %s: %d", state->target, opened);
        close(result->fd);
        mesh_tcp_reset_link(state, "could not watch the socket");
        return;
    }

    state->link_state = MESH_TCP_LINK_CONNECTED;
    mesh_session_attach(state->session, mesh_tcp_session_send, state);
    const int handshake = mesh_session_begin_handshake(state->session);
    if (handshake < 0) {
        inkwell_log_warn("tcp", "Failed to request config sync: %d", handshake);
        mesh_tcp_fail_own(state, MESH_STR_LINK_TCP_NO_ANSWER, state->target);
        mesh_tcp_reset_link(state, "handshake failed");
        return;
    }
    state->next_heartbeat_ms = inkwell_time_monotonic_ms() + MESH_TCP_HEARTBEAT_INTERVAL_MS;
    inkwell_log_info("tcp", "Connected to %s", state->target);
}

static int mesh_tcp_fd_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_transport *transport = (struct mesh_transport *)userdata;
    if (transport == NULL || transport->state == NULL) {
        return 0;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;

    if ((events & (uint32_t)(INKWELL_LOOP_ERR | INKWELL_LOOP_HUP)) != 0U) {
        mesh_tcp_reset_link(state, "the connection dropped");
        return 0;
    }
    if ((events & (uint32_t)INKWELL_LOOP_OUT) != 0U) {
        if (mesh_stream_link_flush(&state->link) == -EIO) {
            mesh_tcp_reset_link(state, "write failed");
            return 0;
        }
    }
    if ((events & (uint32_t)INKWELL_LOOP_IN) != 0U) {
        (void)mesh_tcp_transport_pump(transport);
    }
    return 0;
}

/* ------------------------------------------------------------------ link */

static void mesh_tcp_reset_link(struct mesh_tcp_transport_state *state, const char *reason) {
    if (state == NULL ||
        (state->link_state == MESH_TCP_LINK_DISCONNECTED &&
         !inkwell_tcp_connector_busy(&state->connector) && state->target[0] == '\0')) {
        return;
    }
    char target[MESH_TCP_TARGET_MAX];
    inkwell_str_copy(target, sizeof target, state->target[0] != '\0' ? state->target : "the radio");

    state->link_state = MESH_TCP_LINK_DISCONNECTED;
    state->next_heartbeat_ms = 0U;
    mesh_session_detach(state->session);
    inkwell_tcp_connector_cancel(&state->connector);
    mesh_stream_link_close(&state->link);
    state->target[0] = '\0';
    inkwell_log_info("tcp", "Disconnected from %s (%s)", target, reason);
}

/*
 * This link is now pointed at `state->target`, whether or not the attempt succeeds.
 *
 * Adopted rather than inferred from a return code: auto-connect reads `configured` to know where
 * to go back to, and enumerating the ways a connect can be refused missed three of them once
 * already (`tcp_refused_target_is_remembered_by_nobody`). The bar is that the attempt got off the
 * ground - a socket for an address, a forked child for a name - and not that it worked. A radio
 * that is switched off is still the address the user wrote down.
 */
static void mesh_tcp_adopt_target(struct mesh_tcp_transport_state *state, const char *target) {
    inkwell_str_copy(state->configured, sizeof state->configured, target);
    state->state = MESH_TCP_STATE_READY;
}

int mesh_tcp_transport_connect(struct mesh_transport *transport, const char *target) {
    if (transport == NULL || transport->state == NULL || target == NULL || target[0] == '\0') {
        return -EINVAL;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    /* A new attempt supersedes whatever the last one failed with. */
    mesh_tcp_clear_error(state);

    if (state->state == MESH_TCP_STATE_DISABLED) {
        mesh_tcp_fail_own(state, MESH_STR_LINK_TCP_DISABLED, "");
        return -ENODEV;
    }
    if (state->link_state != MESH_TCP_LINK_DISCONNECTED ||
        inkwell_tcp_connector_busy(&state->connector)) {
        return -EBUSY;
    }

    char host[MESH_TCP_TARGET_MAX];
    uint16_t port = 0U;
    if (mesh_tcp_target_split(target, host, sizeof host, &port) < 0) {
        inkwell_log_warn("tcp", "'%s' is not an address and port", target);
        mesh_tcp_fail_own(state, MESH_STR_LINK_TCP_BAD_TARGET, target);
        return -EINVAL;
    }

    /* Named now because everything below reports through it; adopted only once something is
       actually under way - see mesh_tcp_adopt_target(). */
    inkwell_str_copy(state->target, sizeof state->target, target);

    const struct inkwell_tcp_connect_options options = {
        .timeout_ms = MESH_TCP_CONNECT_TIMEOUT_MS,
        .no_delay = true,
        .keepalive = true,
        .keepalive_idle_s = MESH_TCP_KEEPALIVE_IDLE_S,
        .keepalive_interval_s = MESH_TCP_KEEPALIVE_INTERVAL_S,
        .keepalive_count = MESH_TCP_KEEPALIVE_COUNT,
    };
    struct inkwell_net_failure failure = {0};
    const int started =
        inkwell_tcp_connector_start(&state->connector, host, port, &options, mesh_tcp_on_connected,
                                    transport, inkwell_time_monotonic_ms(), &failure);
    if (started < 0) {
        inkwell_log_warn("tcp", "Cannot connect to %s: %d", host, started);
        mesh_tcp_fail(state,
                      inkwell_net_failed(&failure) ? failure.reason : INKWELL_NET_UNREACHABLE,
                      failure.detail, host);
        state->target[0] = '\0';
        return started;
    }

    /* Accepted, not necessarily connected: a dead radio is still the address the user chose. */
    mesh_tcp_adopt_target(state, target);
    inkwell_log_info("tcp", "Connecting to %s", target);
    return 0;
}

int mesh_tcp_transport_disconnect(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -EINVAL;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    if (state->link_state == MESH_TCP_LINK_DISCONNECTED &&
        !inkwell_tcp_connector_busy(&state->connector)) {
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

    const uint64_t now = inkwell_time_monotonic_ms();

    inkwell_tcp_connector_tick(&state->connector, now);
    if (inkwell_tcp_connector_busy(&state->connector)) {
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
    if (mesh_session_link_silent(state->session)) {
        mesh_tcp_reset_link(state, "radio stopped answering");
    }
}

static int mesh_tcp_start(struct mesh_transport *transport, const struct mesh_app_config *config,
                          struct inkwell_loop *loop) {
    if (transport == NULL || config == NULL || transport->state == NULL) {
        return -EINVAL;
    }

    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    /* The injected session outlives a restart; everything else is cleared. */
    struct mesh_session *injected = state->session != &state->own_session ? state->session : NULL;
    memset(state, 0, sizeof *state);
    state->session = injected;
    state->loop = loop;
    state->link_state = MESH_TCP_LINK_DISCONNECTED;
    (void)inkwell_tcp_connector_init(&state->connector, loop);
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
        inkwell_log_info("tcp", "Network transport disabled by configuration");
        state->state = MESH_TCP_STATE_DISABLED;
        return 0;
    }

    inkwell_str_copy(state->configured, sizeof state->configured, config->preferred_tcp_host);
    if (state->configured[0] == '\0') {
        state->state = MESH_TCP_STATE_IDLE;
        inkwell_log_debug("tcp", "No host configured; nothing to connect to");
    } else {
        state->state = MESH_TCP_STATE_READY;
        inkwell_log_info("tcp", "Configured for %s", state->configured);
    }
    return 0;
}

static void mesh_tcp_stop(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    mesh_tcp_reset_link(state, "shutting down");
    inkwell_tcp_connector_shutdown(&state->connector);
    mesh_stream_link_close(&state->link);
    state->loop = NULL;
    state->state = MESH_TCP_STATE_IDLE;
}

static const char *mesh_tcp_status(const struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return inkcell_str(MESH_STR_TRANSPORT_UNAVAILABLE);
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    if (inkwell_tcp_connector_busy(&state->connector)) {
        return inkcell_str(MESH_STR_TRANSPORT_CONNECTING);
    }
    switch (state->link_state) {
    case MESH_TCP_LINK_CONNECTED:
        return inkcell_str(MESH_STR_TRANSPORT_CONNECTED);
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
    if (!mesh_tcp_has_error(state)) {
        return false;
    }

    if (state->own_failure_set) {
        (void)inkcell_str_format(out, out_len, state->own_failure, state->failure_subject);
        mesh_tcp_clear_error(state);
        return true;
    }
    /* A plain TCP link has no TLS session, so no TLS text to pass. A reason with nothing to say
       about it is unreachable today - every reason this file records has a sentence - but a new
       one in inkwell would land here rather than print a stale buffer. */
    const bool said =
        mesh_net_reason_format(&state->failure, state->failure_subject, NULL, out, out_len);
    mesh_tcp_clear_error(state);
    return said;
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

int mesh_tcp_transport_forget(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -ENODEV;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    /* The link first: a socket left up to a host nothing is configured for is a link the user
       has just said they do not want and no screen can now name. */
    mesh_tcp_reset_link(state, "address cleared");
    state->configured[0] = '\0';
    if (state->state == MESH_TCP_STATE_READY) {
        state->state = MESH_TCP_STATE_IDLE;
    }
    inkwell_log_info("tcp", "Network address cleared");
    return 0;
}

bool mesh_tcp_transport_is_connecting(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return false;
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    return inkwell_tcp_connector_busy(&state->connector);
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
    /* Both descriptors read as closed before start() runs; a zeroed struct would have fd 0,
       which is stdin and is very much open. */
    static struct mesh_tcp_transport_state state = {
        .connector = {.fd = -1, .resolve = {.child = -1, .child_fd = -1}},
        .link = {.stream = {.fd = -1}},
    };
    static struct mesh_transport transport = {
        .name = "tcp",
        .state = &state,
        .ops = &k_tcp_ops,
    };
    return &transport;
}
