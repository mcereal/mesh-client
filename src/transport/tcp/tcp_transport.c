#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "inkcell/utils/log.h"
#include "inkcell/utils/text.h"
#include "inkcell/utils/time.h"

#include "mesh/i18n/strings.h"
#include "mesh/transport/tcp.h"

#include "mesh/core/config.h"
#include "mesh/core/resolve.h"
#include "mesh/transport/stream_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
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
    /*
     * A name is being looked up. There is no socket yet - the address to open one to is what the
     * forked resolver is out fetching - which is the whole reason this is a state of its own and
     * not part of CONNECTING: a target in this state has nothing for epoll to watch, and the
     * connect deadline has not started because there is nothing to connect to.
     */
    MESH_TCP_LINK_RESOLVING,
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

    /*
     * The forked name lookup, when the target is a name.
     *
     * Owned by the transport rather than shared, because a lookup is part of one connect
     * attempt: dropping the link has to be able to abandon it, and a resolver shared with
     * anything else could not be cancelled here without cancelling somebody else's.
     */
    struct mesh_resolve resolve;

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
    /* Why the last connect attempt failed, in words, waiting to be shown once. */
    char last_error[MESH_TRANSPORT_ERROR_MAX];
};

static void mesh_tcp_reset_link(struct mesh_tcp_transport_state *state, const char *reason);

/* Records a failure for the UI to pick up. First one wins until it is read. */
static void mesh_tcp_set_error(struct mesh_tcp_transport_state *state, enum inkcell_str_id text,
                               ...) {
    if (state == NULL || state->last_error[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, text);
    (void)inkcell_str_vformat(state->last_error, sizeof state->last_error, text, args);
    va_end(args);
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
        inkcell_log_warn("tcp", "Cannot reach %s: %s", state->target, strerror(error));
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
        inkcell_log_warn("tcp", "Cannot watch %s: %d", state->target, opened);
        close(fd);
        mesh_tcp_reset_link(state, "could not watch the socket");
        return;
    }

    state->link_state = MESH_TCP_LINK_CONNECTED;
    state->connect_deadline_ms = 0U;
    mesh_session_attach(state->session, mesh_tcp_session_send, state);
    const int handshake = mesh_session_begin_handshake(state->session);
    if (handshake < 0) {
        inkcell_log_warn("tcp", "Failed to request config sync: %d", handshake);
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_NO_ANSWER, state->target);
        mesh_tcp_reset_link(state, "handshake failed");
        return;
    }
    state->next_heartbeat_ms = inkcell_time_monotonic_ms() + MESH_TCP_HEARTBEAT_INTERVAL_MS;
    inkcell_log_info("tcp", "Connected to %s", state->target);
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
    inkcell_str_copy(target, sizeof target, state->target[0] != '\0' ? state->target : "the radio");

    state->link_state = MESH_TCP_LINK_DISCONNECTED;
    state->connect_deadline_ms = 0U;
    state->next_heartbeat_ms = 0U;
    mesh_session_detach(state->session);
    /* A name still being looked up is part of the attempt being abandoned. Cancelling rather
       than letting it land is what stops a child that was out for five seconds opening a socket
       to a link the user has already dropped. */
    mesh_resolve_cancel(&state->resolve);
    mesh_tcp_drop_pending(state);
    mesh_stream_link_close(&state->link);
    state->target[0] = '\0';
    inkcell_log_info("tcp", "Disconnected from %s (%s)", target, reason);
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
static void mesh_tcp_adopt_target(struct mesh_tcp_transport_state *state) {
    inkcell_str_copy(state->configured, sizeof state->configured, state->target);
    state->state = MESH_TCP_STATE_READY;
}

/*
 * The half of a connect that begins once there is an address: open the socket, start the
 * handshake, watch for it to finish.
 *
 * Split out from mesh_tcp_transport_connect() because a name reaches it a second time and
 * seconds later, from the resolver's completion - and the only thing the two paths differ in
 * is how they got the `sockaddr`. `state->target` and `state->configured` are already set by
 * the time this runs; it is not this function's business to decide what the link is pointed at.
 */
static int mesh_tcp_open(struct mesh_tcp_transport_state *state, struct mesh_transport *transport,
                         const struct sockaddr_storage *address, socklen_t address_len) {
    const char *const target = state->target;

    const int fd = socket(address->ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        const int error = errno;
        inkcell_log_warn("tcp", "Cannot open a socket: %s", strerror(error));
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_UNREACHABLE, target, strerror(error));
        return -error;
    }
    mesh_tcp_adopt_target(state);

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

    const int connected = connect(fd, (const struct sockaddr *)address, address_len);
    if (connected < 0 && errno != EINPROGRESS) {
        const int error = errno;
        inkcell_log_warn("tcp", "Cannot reach %s: %s", target, strerror(error));
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
            inkcell_log_warn("tcp", "Cannot watch %s: %d", target, added);
            close(fd);
            state->pending_fd = -1;
            state->target[0] = '\0';
            return added;
        }
        state->pending_registered = true;
    }

    state->link_state = MESH_TCP_LINK_CONNECTING;
    state->connect_deadline_ms = inkcell_time_monotonic_ms() + MESH_TCP_CONNECT_TIMEOUT_MS;
    inkcell_log_info("tcp", "Connecting to %s", target);

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

/*
 * The name came back. Either open the socket to it, or say why we are not going to.
 *
 * Nothing is returned to anybody here: the press that asked for this was answered when the
 * lookup started, so a failure at this point reaches the user the way a failed connect does -
 * through `last_error`, picked up by whichever screen asks next.
 */
static void mesh_tcp_on_resolved(void *userdata, const struct mesh_resolve_result *result) {
    struct mesh_transport *transport = (struct mesh_transport *)userdata;
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    /*
     * A lookup that was overtaken - the link was dropped, or something else connected while the
     * child was out - is not this answer's to act on. mesh_resolve_cancel() already suppresses
     * the callback for any teardown that went through us; this is the belt behind those braces.
     */
    if (state->link_state != MESH_TCP_LINK_RESOLVING) {
        return;
    }

    char host[MESH_TCP_TARGET_MAX];
    uint16_t port = 0U;
    (void)mesh_tcp_target_split(state->target, host, sizeof host, &port);

    if (result->outcome != MESH_RESOLVE_OK) {
        /*
         * Two sentences rather than one, because they ask for different things from the person
         * reading them: a name that does not resolve is a typo or the wrong network, and a
         * resolver that did not answer is neither - the name may be perfectly good.
         */
        switch (result->outcome) {
        case MESH_RESOLVE_NOT_FOUND:
            inkcell_log_warn("tcp", "No address for %s", host);
            mesh_tcp_set_error(state, MESH_STR_LINK_TCP_UNKNOWN_HOST, host);
            break;
        case MESH_RESOLVE_TIMED_OUT:
            inkcell_log_warn("tcp", "Looking up %s took too long", host);
            mesh_tcp_set_error(state, MESH_STR_LINK_TCP_LOOKUP_FAILED, host);
            break;
        default:
            inkcell_log_warn("tcp", "Cannot look up %s: %s", host,
                             result->error != 0 ? gai_strerror(result->error) : "no resolver");
            mesh_tcp_set_error(state, MESH_STR_LINK_TCP_LOOKUP_FAILED, host);
            break;
        }
        state->link_state = MESH_TCP_LINK_DISCONNECTED;
        state->target[0] = '\0';
        return;
    }

    /* Back to DISCONNECTED first: mesh_tcp_open() is the same function the literal path calls,
       and it moves the link to CONNECTING itself. */
    state->link_state = MESH_TCP_LINK_DISCONNECTED;
    if (mesh_tcp_open(state, transport, &result->address, result->address_len) < 0 &&
        state->link_state == MESH_TCP_LINK_DISCONNECTED) {
        state->target[0] = '\0';
    }
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
        inkcell_log_warn("tcp", "'%s' is not an address and port", target);
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_BAD_TARGET, target);
        return -EINVAL;
    }

    /* Named now because everything below reports through it; adopted only once something is
       actually under way - see mesh_tcp_adopt_target(). */
    inkcell_str_copy(state->target, sizeof state->target, target);

    struct sockaddr_storage address;
    socklen_t address_len = 0;
    if (mesh_resolve_literal(host, port, &address, &address_len)) {
        const int opened = mesh_tcp_open(state, transport, &address, address_len);
        if (opened < 0 && state->link_state == MESH_TCP_LINK_DISCONNECTED) {
            state->target[0] = '\0';
        }
        return opened;
    }

    /*
     * A name. The lookup is a forked child and the answer arrives on a later loop turn, so what
     * this returns is "under way" rather than "connected" - and unlike the literal path there is
     * no refusal to hand back, because nothing has been tried yet. Whatever goes wrong from here
     * is reported through `last_error`; see mesh_tcp_on_resolved().
     */
    const int started = mesh_resolve_start(&state->resolve, host, port, mesh_tcp_on_resolved,
                                           transport, inkcell_time_monotonic_ms());
    if (started < 0) {
        inkcell_log_warn("tcp", "Cannot look up %s: %d", host, started);
        mesh_tcp_set_error(state, MESH_STR_LINK_TCP_LOOKUP_FAILED, host);
        state->target[0] = '\0';
        return started;
    }

    /*
     * A name is adopted here, on the fork rather than on the answer, which is the same bar the
     * address path uses one function up: the lookup is under way. `meshtastic.local` on a network
     * that is not this one is the address the user wrote down exactly as an IP to a radio that is
     * switched off is.
     */
    mesh_tcp_adopt_target(state);
    state->link_state = MESH_TCP_LINK_RESOLVING;
    inkcell_log_info("tcp", "Looking up %s", host);
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

    const uint64_t now = inkcell_time_monotonic_ms();

    /* The resolver's own deadline and its reap, every turn: the fd callback sees the answer, but
       the child is only ever collected here. */
    mesh_resolve_tick(&state->resolve, now);

    if (state->link_state == MESH_TCP_LINK_RESOLVING) {
        /* Nothing to time out here that mesh_resolve_tick() does not already own - it kills the
           child on its own deadline and reports TIMED_OUT, which mesh_tcp_on_resolved() turns
           into the error and the drop. */
        return;
    }

    if (state->link_state == MESH_TCP_LINK_CONNECTING) {
        if (state->connect_deadline_ms != 0U && now >= state->connect_deadline_ms) {
            inkcell_log_warn("tcp", "%s did not answer in time", state->target);
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
    if (mesh_session_link_silent(state->session)) {
        mesh_tcp_reset_link(state, "radio stopped answering");
    }
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
    /* After the memset, which would otherwise leave the resolver looking like it owned pid 0. */
    (void)mesh_resolve_init(&state->resolve, loop);
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
        inkcell_log_info("tcp", "Network transport disabled by configuration");
        state->state = MESH_TCP_STATE_DISABLED;
        return 0;
    }

    inkcell_str_copy(state->configured, sizeof state->configured, config->preferred_tcp_host);
    if (state->configured[0] == '\0') {
        state->state = MESH_TCP_STATE_IDLE;
        inkcell_log_debug("tcp", "No host configured; nothing to connect to");
    } else {
        state->state = MESH_TCP_STATE_READY;
        inkcell_log_info("tcp", "Configured for %s", state->configured);
    }
    return 0;
}

static void mesh_tcp_stop(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    mesh_tcp_reset_link(state, "shutting down");
    mesh_resolve_shutdown(&state->resolve);
    mesh_tcp_drop_pending(state);
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
    switch (state->link_state) {
    /* Looking a name up is the first half of connecting and reads as one thing from outside:
       the user pressed connect and it has not finished. */
    case MESH_TCP_LINK_RESOLVING:
    case MESH_TCP_LINK_CONNECTING:
        return inkcell_str(MESH_STR_TRANSPORT_CONNECTING);
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
    if (state->last_error[0] == '\0') {
        return false;
    }
    inkcell_str_copy(out, out_len, state->last_error);
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

int mesh_tcp_transport_forget(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return -ENODEV;
    }
    struct mesh_tcp_transport_state *state = (struct mesh_tcp_transport_state *)transport->state;
    /* The link first: a socket left up to a host nothing is configured for is a link the user
       has just said they do not want and no screen can now name. */
    mesh_tcp_reset_link(state, "address cleared");
    mesh_tcp_drop_pending(state);
    state->configured[0] = '\0';
    if (state->state == MESH_TCP_STATE_READY) {
        state->state = MESH_TCP_STATE_IDLE;
    }
    inkcell_log_info("tcp", "Network address cleared");
    return 0;
}

bool mesh_tcp_transport_is_connecting(struct mesh_transport *transport) {
    if (transport == NULL || transport->state == NULL) {
        return false;
    }
    const struct mesh_tcp_transport_state *state =
        (const struct mesh_tcp_transport_state *)transport->state;
    return state->link_state == MESH_TCP_LINK_CONNECTING ||
           state->link_state == MESH_TCP_LINK_RESOLVING;
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
