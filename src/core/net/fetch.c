#define _POSIX_C_SOURCE 200809L

#include "mesh/core/fetch.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/tls_client.h"
#include "mesh/core/version.h"
#include "mesh/proto/http.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * How much one turn of the loop reads before giving it back.
 *
 * A download is megabytes arriving as fast as the network allows, and every byte of it is
 * decrypted and written on the thread the UI draws on. Reading until the socket is empty would
 * hold the loop for as long as the sender stays ahead of us, which on a fast link is the whole
 * download. Level-triggered epoll brings us straight back to whatever is left.
 *
 * The read size is one TLS record's worth of plaintext, which is what makes stopping safe: each
 * read takes a whole record out of the session, so what is left is in the socket - where epoll
 * can see it - rather than inside Mbed TLS, where it cannot.
 */
#define FETCH_READ_CHUNK 16384U
#define FETCH_READS_PER_TURN 8U

/* The request line and Host, plus every header the caller may add. */
#define FETCH_REQUEST_MAX                                                                          \
    (MESH_HTTP_URL_MAX + MESH_HTTP_HOST_MAX + 128U +                                               \
     (MESH_FETCH_HEADERS_MAX + 1U) * (MESH_FETCH_HEADER_MAX + 2U))

#define FETCH_DEFAULT_TIMEOUT_MS 30000U
/*
 * How long one address gets to accept a connection before the next is tried. A refusal moves on
 * at once; this is for the address that never answers at all - an IPv6 address with no IPv6
 * route behind it - which would otherwise spend the whole request's deadline on one SYN. A
 * connect over the Brick's WiFi to a CDN takes well under a second.
 */
#define FETCH_CONNECT_ATTEMPT_MS 3000U

enum fetch_phase {
    FETCH_RESOLVING = 0,
    FETCH_CONNECTING,
    FETCH_HANDSHAKE,
    FETCH_SENDING,
    FETCH_RECEIVING,
};

struct mesh_fetch_conn {
    /* ---- the request, copied: a caller's strings need not outlive start() */
    enum mesh_fetch_method method;
    char headers[MESH_FETCH_HEADERS_MAX + 1U][MESH_FETCH_HEADER_MAX];
    size_t header_count;
    bool ranged;
    char output_path[MESH_FETCH_PATH_MAX];
    size_t response_max;
    uint64_t deadline_ms;
    mesh_fetch_done_fn on_done;
    void *userdata;

    /* ---- the hop in flight */
    struct mesh_http_url url;
    unsigned redirects;
    enum fetch_phase phase;
    /* Where the hop's host is, and which of them to try next. */
    struct mesh_resolve_address addresses[MESH_RESOLVE_ADDRESSES_MAX];
    size_t address_count;
    size_t address_next;
    uint64_t attempt_deadline_ms;
    int fd;
    bool fd_registered;
    struct mesh_tls_client tls;
    char request[FETCH_REQUEST_MAX];
    size_t request_len;
    size_t request_sent;
    struct mesh_http_response response;
    bool head_seen;
    /* A TLS read stopped with work still inside the session; tick() comes back for it. */
    bool more_to_read;

    /* ---- where the body goes */
    int out_fd;
    char *body;
    size_t body_len;

    /* Room for a path and an errno, or a host and a TLS error. */
    char detail[MESH_FETCH_PATH_MAX + 96U];
    uint8_t buffer[FETCH_READ_CHUNK];
};

/*
 * Closes the hop's connection. Not its lookup: the resolver belongs to the fetcher, not to the
 * request, and a request freed after its completion ran may be freed *under* the next one - which
 * the completion started, and whose lookup is the one in the resolver now.
 */
static void fetch_drop_socket(struct mesh_fetch *fetch, struct mesh_fetch_conn *conn) {
    mesh_tls_client_stop(&conn->tls);
    if (conn->fd >= 0) {
        if (conn->fd_registered && fetch->loop != NULL) {
            (void)mesh_event_loop_remove_fd(fetch->loop, conn->fd);
        }
        close(conn->fd);
    }
    conn->fd = -1;
    conn->fd_registered = false;
    conn->more_to_read = false;
}

static void fetch_free(struct mesh_fetch *fetch, struct mesh_fetch_conn *conn) {
    fetch_drop_socket(fetch, conn);
    if (conn->out_fd >= 0) {
        close(conn->out_fd);
    }
    free(conn->body);
    free(conn);
}

/*
 * Hands the outcome to whoever asked for it, exactly once.
 *
 * The order here is the re-entrancy rule: the request is lifted off the fetcher and the socket
 * closed *before* the callback runs, so a caller that starts its next request from inside the
 * completion is starting one against an idle fetcher. The body and the detail live in the lifted
 * request, which is freed after the call.
 */
static void fetch_complete(struct mesh_fetch *fetch, enum mesh_fetch_outcome outcome) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    if (conn == NULL) {
        return;
    }
    fetch->conn = NULL;
    /* Before the callback, which may start a lookup of its own. */
    mesh_resolve_cancel(&fetch->resolve);
    fetch_drop_socket(fetch, conn);
    if (conn->out_fd >= 0) {
        const int closed = close(conn->out_fd);
        conn->out_fd = -1;
        if (closed != 0 && outcome == MESH_FETCH_OK) {
            outcome = MESH_FETCH_FILE;
            snprintf(conn->detail, sizeof conn->detail, "closing %s: %s", conn->output_path,
                     strerror(errno));
        }
    }
    if (outcome != MESH_FETCH_OK) {
        free(conn->body);
        conn->body = NULL;
        conn->body_len = 0U;
    }

    const struct mesh_fetch_result result = {
        .outcome = outcome,
        .status = mesh_http_response_head_done(&conn->response) ? conn->response.status : 0,
        .body = conn->body,
        .len = conn->body_len,
        .detail = conn->detail,
    };
    if (conn->on_done != NULL) {
        conn->on_done(conn->userdata, &result);
    }
    fetch_free(fetch, conn);
}

static void fetch_fail(struct mesh_fetch *fetch, enum mesh_fetch_outcome outcome,
                       const char *format, ...) __attribute__((format(printf, 3, 4)));

static void fetch_fail(struct mesh_fetch *fetch, enum mesh_fetch_outcome outcome,
                       const char *format, ...) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    if (conn == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(conn->detail, sizeof conn->detail, format, args);
    va_end(args);
    fetch_complete(fetch, outcome);
}

/* ---- the body --------------------------------------------------------------------------- */

/* Appends to the captured reply, capped so a runaway one cannot grow without bound. */
static bool fetch_capture(struct mesh_fetch_conn *conn, const uint8_t *bytes, size_t len) {
    if (len > conn->response_max || conn->body_len + len + 1U > conn->response_max) {
        return false;
    }
    char *grown = realloc(conn->body, conn->body_len + len + 1U);
    if (grown == NULL) {
        return false;
    }
    memcpy(grown + conn->body_len, bytes, len);
    conn->body_len += len;
    grown[conn->body_len] = '\0';
    conn->body = grown;
    return true;
}

/* One slice of body, to wherever it goes. False once the request has been completed. */
static bool fetch_sink(struct mesh_fetch *fetch, const uint8_t *bytes, size_t len) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    if (conn->out_fd < 0) {
        if (!fetch_capture(conn, bytes, len)) {
            fetch_fail(fetch, MESH_FETCH_TOO_LARGE, "reply passed %zu bytes", conn->response_max);
            return false;
        }
        return true;
    }
    while (len > 0U) {
        const ssize_t wrote = write(conn->out_fd, bytes, len);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            fetch_fail(fetch, MESH_FETCH_FILE, "writing %s: %s", conn->output_path,
                       wrote < 0 ? strerror(errno) : "short write");
            return false;
        }
        bytes += wrote;
        len -= (size_t)wrote;
    }
    return true;
}

/* The reply is whole. A HEAD's answer is its head, which the codec has kept. */
static void fetch_finish(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    if (conn->method == MESH_FETCH_HEAD &&
        !fetch_capture(conn, (const uint8_t *)conn->response.head, conn->response.head_len)) {
        fetch_fail(fetch, MESH_FETCH_TOO_LARGE, "head passed %zu bytes", conn->response_max);
        return;
    }
    fetch_complete(fetch, MESH_FETCH_OK);
}

/* ---- one hop ---------------------------------------------------------------------------- */

static void fetch_hop(struct mesh_fetch *fetch);
static void fetch_try_next(struct mesh_fetch *fetch);

static void fetch_arm(struct mesh_fetch *fetch, struct mesh_fetch_conn *conn, bool write) {
    if (fetch->loop != NULL && conn->fd_registered) {
        (void)mesh_event_loop_update_fd(fetch->loop, conn->fd,
                                        write ? (uint32_t)EPOLLOUT : (uint32_t)EPOLLIN);
    }
}

/*
 * The head is in. Decides what the rest of the reply is for: a redirect is followed and its body
 * never read, a failure is reported without reading one either, and a 2xx opens the file its body
 * goes to. False once the request has been completed or has moved on to another hop.
 */
static bool fetch_on_head(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    const int status = conn->response.status;

    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        const char *value = NULL;
        size_t len = 0U;
        if (!mesh_http_response_header(&conn->response, "location", &value, &len)) {
            fetch_fail(fetch, MESH_FETCH_HTTP_STATUS, "%d with no Location", status);
            return false;
        }
        if (conn->redirects >= MESH_FETCH_REDIRECTS_MAX) {
            fetch_fail(fetch, MESH_FETCH_PROTOCOL, "more than %u redirects",
                       MESH_FETCH_REDIRECTS_MAX);
            return false;
        }
        char location[MESH_HTTP_URL_MAX];
        struct mesh_http_url next;
        if (len >= sizeof location) {
            fetch_fail(fetch, MESH_FETCH_PROTOCOL, "a %zu-byte Location", len);
            return false;
        }
        memcpy(location, value, len);
        location[len] = '\0';
        if (!mesh_http_url_resolve(&conn->url, location, &next)) {
            fetch_fail(fetch, MESH_FETCH_PROTOCOL, "unusable Location %.96s", location);
            return false;
        }
        if (!next.tls) {
            fetch_fail(fetch, MESH_FETCH_PROTOCOL, "redirect off https to %.96s", next.host);
            return false;
        }
        mesh_log_debug("fetch", "%d from %s to %s", status, conn->url.host, next.host);
        fetch_drop_socket(fetch, conn);
        conn->url = next;
        conn->redirects++;
        fetch_hop(fetch);
        return false;
    }
    if (status < 200 || status > 299) {
        fetch_fail(fetch, MESH_FETCH_HTTP_STATUS, "HTTP %d from %s", status, conn->url.host);
        return false;
    }
    if (conn->ranged && status != 206) {
        fetch_fail(fetch, MESH_FETCH_PROTOCOL, "range ignored: %d from %s", status, conn->url.host);
        return false;
    }
    if (conn->output_path[0] != '\0' && conn->method == MESH_FETCH_GET) {
        conn->out_fd = open(conn->output_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (conn->out_fd < 0) {
            fetch_fail(fetch, MESH_FETCH_FILE, "opening %s: %s", conn->output_path,
                       strerror(errno));
            return false;
        }
    }
    return true;
}

/*
 * The connection ended. Whether that finishes the reply is the codec's call, with one thing only
 * this side knows: whether the end was a TLS close_notify. A body framed by the close is only
 * whole if the server said so - a cut connection looks exactly like the end of one otherwise.
 */
static void fetch_on_close(struct mesh_fetch *fetch, int rc) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    const bool clean = rc == -ENOTCONN;
    if (!mesh_http_response_head_done(&conn->response)) {
        fetch_fail(fetch, MESH_FETCH_NETWORK, "%s closed before replying: %s", conn->url.host,
                   clean ? "close_notify" : mesh_tls_client_error(&conn->tls));
        return;
    }
    if (conn->response.framing == MESH_HTTP_FRAMING_UNTIL_CLOSE && !clean) {
        fetch_fail(fetch, MESH_FETCH_NETWORK, "%s dropped an unframed body after %llu bytes",
                   conn->url.host, (unsigned long long)conn->response.body_received);
        return;
    }
    if (!mesh_http_response_finish(&conn->response)) {
        fetch_fail(fetch, MESH_FETCH_NETWORK, "%s cut the body short after %llu bytes",
                   conn->url.host, (unsigned long long)conn->response.body_received);
        return;
    }
    fetch_finish(fetch);
}

/* Feeds one read's worth through the codec. False once the request has been completed or has
   moved on to another hop. */
static bool fetch_feed(struct mesh_fetch *fetch, const uint8_t *in, size_t len) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    size_t at = 0U;
    while (at < len) {
        const uint8_t *body = NULL;
        size_t body_len = 0U;
        const size_t used =
            mesh_http_response_feed(&conn->response, in + at, len - at, &body, &body_len);
        at += used;
        if (mesh_http_response_failed(&conn->response)) {
            fetch_fail(fetch, MESH_FETCH_PROTOCOL, "bad reply from %s: %s", conn->url.host,
                       mesh_http_error_name(conn->response.error));
            return false;
        }
        if (!conn->head_seen && mesh_http_response_head_done(&conn->response)) {
            conn->head_seen = true;
            if (!fetch_on_head(fetch)) {
                return false;
            }
        }
        if (body_len > 0U && !fetch_sink(fetch, body, body_len)) {
            return false;
        }
        if (mesh_http_response_done(&conn->response)) {
            /* Whatever follows is not part of this reply, and with `Connection: close` there
               should be nothing. */
            fetch_finish(fetch);
            return false;
        }
        if (used == 0U) {
            break;
        }
    }
    return true;
}

static void fetch_receive(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    conn->more_to_read = false;
    for (unsigned i = 0U; i < FETCH_READS_PER_TURN; ++i) {
        const int rc = mesh_tls_client_read(&conn->tls, conn->buffer, sizeof conn->buffer);
        if (rc == -EAGAIN) {
            conn->more_to_read = conn->tls.more_to_read;
            fetch_arm(fetch, conn, conn->tls.wants_write);
            return;
        }
        if (rc < 0) {
            fetch_on_close(fetch, rc);
            return;
        }
        if (!fetch_feed(fetch, conn->buffer, (size_t)rc)) {
            return;
        }
    }
    /* Budget spent. The rest is in the socket and epoll will say so on the next turn. */
    fetch_arm(fetch, conn, false);
}

static void fetch_send(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    while (conn->request_sent < conn->request_len) {
        const int rc =
            mesh_tls_client_write(&conn->tls, (const uint8_t *)conn->request + conn->request_sent,
                                  conn->request_len - conn->request_sent);
        if (rc == -EAGAIN) {
            fetch_arm(fetch, conn, conn->tls.wants_write);
            return;
        }
        if (rc < 0) {
            fetch_fail(fetch, MESH_FETCH_NETWORK, "sending to %s: %s", conn->url.host,
                       mesh_tls_client_error(&conn->tls));
            return;
        }
        conn->request_sent += (size_t)rc;
    }
    conn->phase = FETCH_RECEIVING;
    fetch_receive(fetch);
}

static void fetch_handshake(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    const int rc = mesh_tls_client_handshake(&conn->tls);
    if (rc == -EAGAIN) {
        fetch_arm(fetch, conn, conn->tls.wants_write);
        return;
    }
    if (rc < 0) {
        fetch_fail(fetch, MESH_FETCH_TLS, "%s: %s", conn->url.host,
                   mesh_tls_client_error(&conn->tls));
        return;
    }

    const char *lines[MESH_FETCH_HEADERS_MAX + 1U];
    for (size_t i = 0U; i < conn->header_count; ++i) {
        lines[i] = conn->headers[i];
    }
    const int len =
        mesh_http_request_format(conn->request, sizeof conn->request,
                                 conn->method == MESH_FETCH_HEAD ? MESH_HTTP_HEAD : MESH_HTTP_GET,
                                 &conn->url, lines, conn->header_count);
    if (len < 0) {
        fetch_fail(fetch, MESH_FETCH_PROTOCOL, "request to %s does not fit", conn->url.host);
        return;
    }
    conn->request_len = (size_t)len;
    conn->request_sent = 0U;
    mesh_http_response_init(&conn->response, conn->method == MESH_FETCH_HEAD);
    conn->head_seen = false;
    conn->phase = FETCH_SENDING;
    fetch_send(fetch);
}

/* The TCP connect finished, one way or the other. */
static void fetch_connected(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    int error = 0;
    socklen_t error_len = (socklen_t)sizeof error;
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
        error = errno;
    }
    if (error != 0) {
        snprintf(conn->detail, sizeof conn->detail, "connecting to %s: %s", conn->url.host,
                 strerror(error));
        fetch_drop_socket(fetch, conn);
        fetch_try_next(fetch);
        return;
    }
    /* This family works on this network; the next hop and the next request try it first. */
    fetch->preferred_family = conn->addresses[conn->address_next - 1U].address.ss_family;
    /* Checked against the URL's host, not the address it resolved to: a certificate is issued
       for a name. The override is read per request, so a test can set it around one. */
    const int started =
        mesh_tls_client_start(&conn->tls, conn->fd, conn->url.host, mesh_tls_ca_override());
    if (started < 0) {
        fetch_fail(fetch, MESH_FETCH_TLS, "%s: %s", conn->url.host,
                   mesh_tls_client_error(&conn->tls));
        return;
    }
    conn->phase = FETCH_HANDSHAKE;
    fetch_handshake(fetch);
}

static int fetch_on_fd(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_fetch *const fetch = (struct mesh_fetch *)userdata;
    if (fetch == NULL || fetch->conn == NULL || fetch->conn->fd < 0) {
        return 0;
    }
    switch (fetch->conn->phase) {
    case FETCH_CONNECTING:
        /* EPOLLERR here is the ordinary refusal, and getsockopt() is what names it. */
        if ((events & (uint32_t)(EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
            fetch_connected(fetch);
        }
        break;
    case FETCH_HANDSHAKE:
        fetch_handshake(fetch);
        break;
    case FETCH_SENDING:
        fetch_send(fetch);
        break;
    case FETCH_RECEIVING:
        fetch_receive(fetch);
        break;
    case FETCH_RESOLVING:
    default:
        break;
    }
    return 0;
}

/* Starts connecting to the next address. False, with conn->detail saying why, when it could
   not even begin. */
static bool fetch_open(struct mesh_fetch *fetch, const struct mesh_resolve_address *address) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    const int fd =
        socket(address->address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(conn->detail, sizeof conn->detail, "socket: %s", strerror(errno));
        return false;
    }
    if (connect(fd, (const struct sockaddr *)&address->address, address->len) < 0 &&
        errno != EINPROGRESS) {
        snprintf(conn->detail, sizeof conn->detail, "connecting to %s: %s", conn->url.host,
                 strerror(errno));
        close(fd);
        return false;
    }
    conn->fd = fd;
    if (mesh_event_loop_add_fd(fetch->loop, fd, (uint32_t)(EPOLLIN | EPOLLOUT), fetch_on_fd,
                               fetch) < 0) {
        snprintf(conn->detail, sizeof conn->detail, "no room on the loop for %s", conn->url.host);
        fetch_drop_socket(fetch, conn);
        return false;
    }
    conn->fd_registered = true;
    conn->phase = FETCH_CONNECTING;
    conn->attempt_deadline_ms = fetch->now_ms + FETCH_CONNECT_ATTEMPT_MS;
    return true;
}

/*
 * Connects to the next address the host has, or fails the request once there are none left -
 * with the last address's reason, which is the one a log reader can act on.
 */
static void fetch_try_next(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    while (conn->address_next < conn->address_count) {
        const struct mesh_resolve_address *const address = &conn->addresses[conn->address_next++];
        if (fetch_open(fetch, address)) {
            return;
        }
    }
    fetch_complete(fetch, MESH_FETCH_NETWORK);
}

/* The family that last connected goes first; the rest keep the resolver's order. */
static void fetch_prefer_family(struct mesh_fetch *fetch, struct mesh_fetch_conn *conn) {
    for (size_t i = 1U; i < conn->address_count; ++i) {
        if (conn->addresses[i].address.ss_family == fetch->preferred_family &&
            conn->addresses[0].address.ss_family != fetch->preferred_family) {
            const struct mesh_resolve_address preferred = conn->addresses[i];
            memmove(&conn->addresses[1], &conn->addresses[0], i * sizeof conn->addresses[0]);
            conn->addresses[0] = preferred;
            return;
        }
    }
}

static void fetch_on_resolved(void *userdata, const struct mesh_resolve_result *result) {
    struct mesh_fetch *const fetch = (struct mesh_fetch *)userdata;
    struct mesh_fetch_conn *const conn = fetch->conn;
    if (conn == NULL || conn->phase != FETCH_RESOLVING) {
        return;
    }
    if (result->outcome != MESH_RESOLVE_OK || result->address_count == 0U) {
        fetch_fail(fetch, MESH_FETCH_NETWORK, "could not resolve %s (%s)", conn->url.host,
                   result->outcome == MESH_RESOLVE_NOT_FOUND   ? "no such name"
                   : result->outcome == MESH_RESOLVE_TIMED_OUT ? "timed out"
                                                               : "lookup failed");
        return;
    }
    conn->address_count = result->address_count;
    memcpy(conn->addresses, result->addresses, result->address_count * sizeof conn->addresses[0]);
    fetch_prefer_family(fetch, conn);
    fetch_try_next(fetch);
}

/*
 * The addresses mesh_fetch_connect_to() named, each a numeric address, comma-separated the way
 * curl's --resolve takes them. False when one does not parse.
 */
static bool fetch_connect_to_list(const struct mesh_fetch *fetch, struct mesh_fetch_conn *conn) {
    char list[sizeof fetch->connect_host];
    memcpy(list, fetch->connect_host, sizeof list);
    char *save = NULL;
    for (char *host = strtok_r(list, ",", &save);
         host != NULL && conn->address_count < MESH_RESOLVE_ADDRESSES_MAX;
         host = strtok_r(NULL, ",", &save)) {
        struct mesh_resolve_address *const address = &conn->addresses[conn->address_count];
        if (!mesh_resolve_literal(host, fetch->connect_port, &address->address, &address->len)) {
            return false;
        }
        conn->address_count++;
    }
    return conn->address_count > 0U;
}

/* One hop: from the current URL to a connecting socket, by way of a lookup when it is a name. */
static void fetch_hop(struct mesh_fetch *fetch) {
    struct mesh_fetch_conn *const conn = fetch->conn;
    conn->phase = FETCH_RESOLVING;
    conn->head_seen = false;
    conn->address_count = 0U;
    conn->address_next = 0U;
    mesh_http_response_init(&conn->response, conn->method == MESH_FETCH_HEAD);

    if (fetch->connect_host[0] != '\0') {
        if (!fetch_connect_to_list(fetch, conn)) {
            /* Not an address list: a name, looked up like any other. */
            conn->address_count = 0U;
            const int started =
                mesh_resolve_start(&fetch->resolve, fetch->connect_host, fetch->connect_port,
                                   fetch_on_resolved, fetch, fetch->now_ms);
            if (started < 0) {
                fetch_fail(fetch, MESH_FETCH_NETWORK, "could not start a lookup for %s: %s",
                           fetch->connect_host, strerror(-started));
            }
            return;
        }
        fetch_try_next(fetch);
        return;
    }
    struct mesh_resolve_address *const address = &conn->addresses[0];
    if (mesh_resolve_literal(conn->url.host, conn->url.port, &address->address, &address->len)) {
        conn->address_count = 1U;
        fetch_try_next(fetch);
        return;
    }
    const int started = mesh_resolve_start(&fetch->resolve, conn->url.host, conn->url.port,
                                           fetch_on_resolved, fetch, fetch->now_ms);
    if (started < 0) {
        fetch_fail(fetch, MESH_FETCH_NETWORK, "could not start a lookup for %s: %s", conn->url.host,
                   strerror(-started));
    }
}

/* ---- the API ---------------------------------------------------------------------------- */

int mesh_fetch_init(struct mesh_fetch *fetch, struct mesh_event_loop *loop) {
    if (fetch == NULL) {
        return -EINVAL;
    }
    memset(fetch, 0, sizeof *fetch);
    fetch->loop = loop;
    return mesh_resolve_init(&fetch->resolve, loop);
}

void mesh_fetch_shutdown(struct mesh_fetch *fetch) {
    if (fetch == NULL) {
        return;
    }
    mesh_fetch_cancel(fetch);
    mesh_resolve_shutdown(&fetch->resolve);
}

bool mesh_fetch_available(const struct mesh_fetch *fetch) {
    return fetch != NULL && fetch->loop != NULL && mesh_tls_available();
}

bool mesh_fetch_busy(const struct mesh_fetch *fetch) {
    return fetch != NULL && fetch->conn != NULL;
}

void mesh_fetch_connect_to(struct mesh_fetch *fetch, const char *host, uint16_t port) {
    if (fetch == NULL) {
        return;
    }
    if (host == NULL || !mesh_str_copy(fetch->connect_host, sizeof fetch->connect_host, host)) {
        fetch->connect_host[0] = '\0';
    }
    fetch->connect_port = port;
}

/* Case-blind prefix test for a header line's name. */
static bool fetch_header_is(const char *line, const char *name) {
    const size_t len = strlen(name);
    for (size_t i = 0U; i < len; ++i) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

int mesh_fetch_start(struct mesh_fetch *fetch, const struct mesh_fetch_request *request,
                     uint64_t now_ms) {
    if (fetch == NULL || request == NULL || request->url == NULL || request->on_done == NULL) {
        return -EINVAL;
    }
    if (!mesh_fetch_available(fetch)) {
        return -ENOTSUP;
    }
    if (fetch->conn != NULL) {
        return -EBUSY;
    }

    struct mesh_fetch_conn *const conn = calloc(1U, sizeof *conn);
    if (conn == NULL) {
        return -ENOMEM;
    }
    conn->fd = -1;
    conn->out_fd = -1;
    if (!mesh_http_url_parse(request->url, &conn->url) || !conn->url.tls) {
        free(conn);
        return -EINVAL;
    }
    bool agent = false;
    for (size_t i = 0U; i < MESH_FETCH_HEADERS_MAX && request->headers[i] != NULL; ++i) {
        if (!mesh_str_copy(conn->headers[conn->header_count], MESH_FETCH_HEADER_MAX,
                           request->headers[i])) {
            free(conn);
            return -EINVAL;
        }
        agent = agent || fetch_header_is(request->headers[i], "user-agent:");
        conn->ranged = conn->ranged || fetch_header_is(request->headers[i], "range:");
        conn->header_count++;
    }
    if (!agent) {
        snprintf(conn->headers[conn->header_count++], MESH_FETCH_HEADER_MAX,
                 "User-Agent: meshclient/%s", mesh_version_string());
    }
    if (request->output_path != NULL &&
        !mesh_str_copy(conn->output_path, sizeof conn->output_path, request->output_path)) {
        free(conn);
        return -EINVAL;
    }
    conn->method = request->method;
    conn->response_max =
        request->response_max > 0U ? request->response_max : MESH_FETCH_RESPONSE_MAX;
    conn->deadline_ms =
        now_ms + (request->timeout_ms > 0U ? request->timeout_ms : FETCH_DEFAULT_TIMEOUT_MS);
    conn->on_done = request->on_done;
    conn->userdata = request->userdata;

    fetch->now_ms = now_ms;
    fetch->conn = conn;
    /*
     * The first hop can fail before anything is waited on - no socket, no room on the loop - and
     * that is reported through the callback like any other failure. So the callback is held back
     * until this returns: a caller is promised it never runs from inside start().
     */
    conn->on_done = NULL;
    fetch_hop(fetch);
    if (fetch->conn == NULL) {
        /* Already completed, silently. Say why the way start() says anything: with an errno. */
        return -EIO;
    }
    fetch->conn->on_done = request->on_done;
    return 0;
}

void mesh_fetch_tick(struct mesh_fetch *fetch, uint64_t now_ms) {
    if (fetch == NULL) {
        return;
    }
    fetch->now_ms = now_ms;
    mesh_resolve_tick(&fetch->resolve, now_ms);
    struct mesh_fetch_conn *const conn = fetch->conn;
    if (conn == NULL) {
        return;
    }
    if (now_ms >= conn->deadline_ms) {
        /* The caller knows which step this was and says so itself. */
        fetch_fail(fetch, MESH_FETCH_TIMED_OUT, "%s did not finish in time", conn->url.host);
        return;
    }
    if (conn->phase == FETCH_CONNECTING && now_ms >= conn->attempt_deadline_ms) {
        snprintf(conn->detail, sizeof conn->detail, "connecting to %s: no answer in %u ms",
                 conn->url.host, FETCH_CONNECT_ATTEMPT_MS);
        fetch_drop_socket(fetch, conn);
        fetch_try_next(fetch);
        return;
    }
    if (conn->more_to_read && conn->phase == FETCH_RECEIVING) {
        fetch_receive(fetch);
    }
}

bool mesh_fetch_content_length(const char *headers, size_t len, uint64_t *out) {
    if (headers == NULL || out == NULL) {
        return false;
    }
    const size_t length = len > 0U ? len : strlen(headers);
    static const char k_key[] = "content-length:";
    const size_t key_len = sizeof k_key - 1U;

    for (size_t at = 0U; at + key_len <= length; ++at) {
        if (at != 0U && headers[at - 1U] != '\n') {
            continue;
        }
        if (!fetch_header_is(headers + at, k_key)) {
            continue;
        }
        size_t digit = at + key_len;
        while (digit < length && (headers[digit] == ' ' || headers[digit] == '\t')) {
            digit++;
        }
        if (digit >= length || headers[digit] < '0' || headers[digit] > '9') {
            return false;
        }
        uint64_t parsed = 0U;
        while (digit < length && headers[digit] >= '0' && headers[digit] <= '9') {
            if (parsed > (UINT64_MAX - 9U) / 10U) {
                return false;
            }
            parsed = parsed * 10U + (uint64_t)(headers[digit] - '0');
            digit++;
        }
        *out = parsed;
        return true;
    }
    return false;
}

void mesh_fetch_cancel(struct mesh_fetch *fetch) {
    if (fetch == NULL || fetch->conn == NULL) {
        return;
    }
    struct mesh_fetch_conn *const conn = fetch->conn;
    fetch->conn = NULL;
    mesh_resolve_cancel(&fetch->resolve);
    fetch_free(fetch, conn);
}

const char *mesh_fetch_outcome_name(enum mesh_fetch_outcome outcome) {
    switch (outcome) {
    case MESH_FETCH_OK:
        return "ok";
    case MESH_FETCH_HTTP_STATUS:
        return "http status";
    case MESH_FETCH_TOO_LARGE:
        return "too large";
    case MESH_FETCH_NETWORK:
        return "network";
    case MESH_FETCH_TLS:
        return "tls";
    case MESH_FETCH_PROTOCOL:
        return "protocol";
    case MESH_FETCH_FILE:
        return "file";
    case MESH_FETCH_TIMED_OUT:
        return "timed out";
    case MESH_FETCH_OUTCOME_COUNT:
    default:
        return "unknown";
    }
}
