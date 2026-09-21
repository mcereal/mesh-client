#pragma once

/*
 * An HTTPS server on loopback, for the suites that fetch.
 *
 * A real one: a listening socket, a TLS handshake with a certificate the client verifies, and
 * HTTP/1.1 written back by hand. It runs in a forked child that blocks, so a suite's wait loop
 * only has to turn its own event loop - the server needs nothing from it - and a handler can
 * sleep on a gate file half way through a body, which is how a download is stopped at a known
 * fraction.
 *
 * The certificate names the hosts the client really fetches from (github.com, api.github.com,
 * *.githubusercontent.com, api.meshtastic.org) as well as `example.invalid` and 127.0.0.1, and
 * https_fixture_attach() points a fetcher's connections here - so a suite asks for the real URL,
 * and the name the certificate is checked against is the real name.
 *
 * The handler runs in the child. What it changes stays there; what a suite needs back comes
 * through the request log (https_fixture_requests()) or a file the handler writes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef INKWELL_HAVE_TLS

struct inkwell_fetch;

struct https_fixture_request {
    char method[8];
    char host[256]; /* the Host header, port removed */
    char target[4096];
    /* A `Range: bytes=first-last`, when the request carried one. */
    bool ranged;
    uint64_t first;
    uint64_t last;
};

struct https_fixture_conn; /* the connection a handler answers on; lives in the child */

typedef void (*https_fixture_handler)(void *userdata, const struct https_fixture_request *request,
                                      struct https_fixture_conn *conn);

struct https_fixture {
    pid_t child;
    int listen_fd;
    uint16_t port;
    char ca_path[64];  /* the certificate, as a bundle for SSL_CERT_FILE */
    char log_path[64]; /* one line per request: "METHOD host target[ first-last]" */
};

/*
 * Starts the server, and sets SSL_CERT_FILE to its certificate so a fetch trusts it - which is
 * the override path the client really has, not a test hook. A case that wants the certificate
 * *not* trusted unsets it. False when anything failed; stop() is safe either way.
 */
bool https_fixture_start(struct https_fixture *fixture, https_fixture_handler handler,
                         void *userdata);
/* Kills the server, removes its files, and unsets SSL_CERT_FILE. */
void https_fixture_stop(struct https_fixture *fixture);

/* Every connection `fetch` makes comes here, whatever host its URL names. */
void https_fixture_attach(const struct https_fixture *fixture, struct inkwell_fetch *fetch);

/*
 * The server's certificate, PEM, for a case that wants to make it a trust anchor rather than
 * name it in a bundle file - the two are different paths through the TLS client and only one of
 * them is what a shipped build uses. Static, and the same bytes the server presents.
 */
const char *https_fixture_cert_pem(void);

/* The request log so far, NUL-terminated. Returns its length. */
size_t https_fixture_requests(const struct https_fixture *fixture, char *out, size_t cap);

/* ---- for a handler, in the child */

/* Writes raw bytes on the connection: a status line, a header, part of a body. */
void https_fixture_send(struct https_fixture_conn *conn, const void *data, size_t len);
void https_fixture_printf(struct https_fixture_conn *conn, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
/*
 * A whole reply: status line, `Content-Length`, `extra` (header lines, each ending in CRLF, or
 * NULL), the blank line and - unless the request was a HEAD - the body.
 */
void https_fixture_reply(struct https_fixture_conn *conn, int status, const char *extra,
                         const void *body, size_t len);
/*
 * A file, the way a CDN serves one: 200 and the whole of it, or 206 and the requested range when
 * the request carried one. 404 when the file will not read.
 */
void https_fixture_reply_file(struct https_fixture_conn *conn,
                              const struct https_fixture_request *request, const char *path);
/* Ends the connection without a close_notify, the way a dropped network does. */
void https_fixture_cut(struct https_fixture_conn *conn);

#endif /* INKWELL_HAVE_TLS */
