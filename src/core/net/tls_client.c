#define _POSIX_C_SOURCE 200809L

#include "mesh/core/tls_client.h"

#include "mesh/core/ca_roots.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *mesh_tls_ca_override(void) {
    const char *const path = getenv("SSL_CERT_FILE");
    return path != NULL && path[0] != '\0' ? path : NULL;
}

#ifndef MESHCLIENT_HAVE_TLS

/*
 * No Mbed TLS in this build.
 *
 * Every entry point still exists and every one of them refuses, so nothing that uses TLS needs
 * an #ifdef of its own - the same arrangement the BLE transport has when D-Bus headers are
 * missing. A caller asks `mesh_tls_available()` when it wants to say something useful about why,
 * and otherwise just gets -ENOTSUP from start() and never reaches the rest.
 */

bool mesh_tls_available(void) { return false; }

int mesh_tls_client_start(struct mesh_tls_client *tls, int fd, const char *hostname,
                          const char *ca_bundle) {
    (void)fd;
    (void)hostname;
    (void)ca_bundle;
    if (tls == NULL) {
        return -EINVAL;
    }
    memset(tls, 0, sizeof *tls);
    tls->fd = -1;
    inkcell_str_copy(tls->error, sizeof tls->error, "this build has no TLS");
    return -ENOTSUP;
}

int mesh_tls_client_handshake(struct mesh_tls_client *tls) {
    (void)tls;
    return -ENOTSUP;
}

int mesh_tls_client_read(struct mesh_tls_client *tls, uint8_t *out, size_t cap) {
    (void)tls;
    (void)out;
    (void)cap;
    return -ENOTSUP;
}

int mesh_tls_client_write(struct mesh_tls_client *tls, const uint8_t *data, size_t len) {
    (void)tls;
    (void)data;
    (void)len;
    return -ENOTSUP;
}

void mesh_tls_client_stop(struct mesh_tls_client *tls) {
    if (tls != NULL) {
        tls->state = NULL;
        tls->fd = -1;
    }
}

const char *mesh_tls_client_error(const struct mesh_tls_client *tls) {
    return tls != NULL ? tls->error : "";
}

#else /* MESHCLIENT_HAVE_TLS */

#include <mbedtls/error.h>
/* For the MBEDTLS_ERR_NET_* codes only. `MBEDTLS_NET_C` is off - the socket layer this header
   declares is the blocking one this project cannot use - but those constants are the documented
   contract for what a BIO callback returns, and they are defined here regardless of it. */
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * How many session tickets one read() may consume before giving the loop back.
 *
 * The same reasoning as `MQTT_READS_PER_TURN`, one level down and for the same single event
 * loop: that bound counts calls into this file and cannot see a call that does not return. A
 * real broker sends one ticket, so this is never reached in practice - it is here so that a peer
 * which streams them cannot hold the UI instead.
 */
#define MESH_TLS_TICKETS_PER_READ 8U

/*
 * Everything Mbed TLS needs for one session, on the heap.
 *
 * Heap rather than inline in `struct mesh_tls_client` so that mqtt_proxy.h - and therefore
 * everything that includes it - does not have to include Mbed TLS's headers to know how big a
 * connection is. One allocation per connection attempt, not per packet, which is the same order
 * as the socket it sits on.
 */
struct mesh_tls_state {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    /* Only filled when the caller named a bundle file; the compiled-in roots are shared. */
    mbedtls_x509_crt ca;
    bool handshaked;
};

/*
 * PSA has to be initialised once before the first handshake - in 4.x every primitive and the
 * RNG behind the handshake are PSA - and it is a process-wide thing rather than a per-session
 * one. A static flag is the whole synchronisation this needs: there is one thread and there is
 * not going to be another.
 *
 * This is also where the connection can stall: seeding the PSA RNG reads the entropy source,
 * and on the Brick that file has to be /dev/urandom or the loop blocks. third_party/
 * mbedtls-config/mesh_psa_crypto_config.h is where that is set, and why.
 */
static bool tls_psa_ready = false;

/*
 * The compiled-in roots, parsed once and kept for the life of the process.
 *
 * Parsed on the first connection that needs them rather than at startup, so a client that never
 * speaks TLS never pays for them. Once, rather than per session, because the chain is read-only
 * once built - Mbed TLS only ever walks it - and 121 roots are a noticeable parse to repeat on
 * every reconnect of a broker that keeps dropping. Never freed: it lives exactly as long as
 * anything that could want it, and a static holding it is reachable, not leaked.
 *
 * `_nocopy`, so each certificate points into the table in .rodata instead of duplicating it on
 * the heap; the table is static for the same lifetime, which is the whole of that API's contract.
 */
static mbedtls_x509_crt tls_roots;
static bool tls_roots_ready = false;

static bool tls_load_roots(struct mesh_tls_client *tls) {
    if (tls_roots_ready) {
        return true;
    }
    mbedtls_x509_crt_init(&tls_roots);
    for (size_t i = 0U; i < mesh_ca_root_count; ++i) {
        const int rc = mbedtls_x509_crt_parse_der_nocopy(&tls_roots, mesh_ca_roots[i].der,
                                                         mesh_ca_roots[i].len);
        if (rc != 0) {
            /*
             * All or nothing, and nothing is not remembered. Every root parses in this build -
             * `ca_roots_all_parse` fails otherwise - so a failure here is the device, not the
             * table: an allocation that did not succeed. Keeping the roots that did load would
             * make whatever chains to the rest unverifiable for the life of the process; dropping
             * them and failing this one connection lets the next attempt load the whole set.
             */
            char detail[64];
            mbedtls_strerror(rc, detail, sizeof detail);
            (void)snprintf(tls->error, sizeof tls->error, "could not load built-in root %.48s: %s",
                           mesh_ca_roots[i].name, detail);
            mbedtls_x509_crt_free(&tls_roots);
            return false;
        }
    }
    inkcell_log_debug("tls", "%zu built-in roots loaded", mesh_ca_root_count);
    tls_roots_ready = true;
    return true;
}

/* Mbed TLS's own sentence for a negative code, which is the entire reason MBEDTLS_ERROR_C is
   left enabled: "X509 - Certificate verification failed" is an answer and -0x2700 is not. */
static void tls_record_error(struct mesh_tls_client *tls, int code, const char *what) {
    char detail[128];
    mbedtls_strerror(code, detail, sizeof detail);
    if (detail[0] == '\0') {
        (void)snprintf(detail, sizeof detail, "error %d", code);
    }
    (void)snprintf(tls->error, sizeof tls->error, "%s: %s", what, detail);
}

/* ------------------------------------------------------------------ the socket underneath */

/*
 * The two BIO callbacks. These are the only place this file touches the descriptor, and they
 * are what make the whole thing non-blocking: `EAGAIN` becomes Mbed TLS's WANT_READ/WANT_WRITE,
 * which unwinds all the way back out to the event loop instead of waiting.
 *
 * `send()` with MSG_NOSIGNAL rather than `write()`, for the reason stream_link.h spells out at
 * length: writing to a socket whose peer has gone raises SIGPIPE, whose default disposition
 * kills the process - so a broker that drops off between two turns of the loop would take the
 * client down with it, before any errno this code handles could be returned.
 */
static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    const int fd = (int)(intptr_t)ctx;
    const ssize_t written = send(fd, buf, len, MSG_NOSIGNAL);
    if (written >= 0) {
        return (int)written;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    const int fd = (int)(intptr_t)ctx;
    const ssize_t got = recv(fd, buf, len, 0);
    if (got > 0) {
        return (int)got;
    }
    if (got == 0) {
        /* A socket EOF underneath a session that expected more is a truncated connection, not a
           clean close - a clean one arrives as a close_notify record and Mbed TLS reports it
           itself. Telling them apart is what lets the caller distinguish "the broker hung up"
           from "the network went away". */
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (errno == ECONNRESET) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ------------------------------------------------------------------ errno out of mbedtls */

/*
 * One place that decides what a Mbed TLS return code means to a caller that speaks errno.
 *
 * WANT_READ and WANT_WRITE are the only two that are not failures, and they differ only in which
 * epoll event to arm next - which is why `wants_write` is set here rather than guessed by the
 * caller from what it was doing at the time.
 */
static int tls_translate(struct mesh_tls_client *tls, int code, const char *what) {
    if (code == MBEDTLS_ERR_SSL_WANT_READ) {
        tls->wants_write = false;
        return -EAGAIN;
    }
    if (code == MBEDTLS_ERR_SSL_WANT_WRITE) {
        tls->wants_write = true;
        return -EAGAIN;
    }
    tls->wants_write = false;
    if (code == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return -ENOTCONN;
    }
    tls_record_error(tls, code, what);
    if (code == MBEDTLS_ERR_NET_CONN_RESET) {
        return -ECONNRESET;
    }
    return -EPROTO;
}

/* ------------------------------------------------------------------ lifecycle */

bool mesh_tls_available(void) { return true; }

static void tls_release(struct mesh_tls_client *tls) {
    struct mesh_tls_state *state = tls->state;
    if (state == NULL) {
        return;
    }
    mbedtls_ssl_free(&state->ssl);
    mbedtls_ssl_config_free(&state->conf);
    mbedtls_x509_crt_free(&state->ca);
    free(state);
    tls->state = NULL;
}

int mesh_tls_client_start(struct mesh_tls_client *tls, int fd, const char *hostname,
                          const char *ca_bundle) {
    if (tls == NULL || fd < 0 || hostname == NULL || hostname[0] == '\0') {
        return -EINVAL;
    }

    memset(tls, 0, sizeof *tls);
    tls->fd = fd;

    struct mesh_tls_state *state = calloc(1U, sizeof *state);
    if (state == NULL) {
        inkcell_str_copy(tls->error, sizeof tls->error, "out of memory");
        return -ENOMEM;
    }
    tls->state = state;

    mbedtls_ssl_init(&state->ssl);
    mbedtls_ssl_config_init(&state->conf);
    mbedtls_x509_crt_init(&state->ca);

    if (!tls_psa_ready) {
        const psa_status_t psa = psa_crypto_init();
        if (psa != PSA_SUCCESS) {
            (void)snprintf(tls->error, sizeof tls->error, "crypto init failed (%d)", (int)psa);
            tls_release(tls);
            return -EIO;
        }
        tls_psa_ready = true;
    }

    /*
     * Which roots the peer is checked against. There is always an answer - the compiled-in set -
     * so there is no state in which a session starts without one and verification quietly stops.
     */
    mbedtls_x509_crt *roots = &tls_roots;
    int rc = 0;
    if (ca_bundle == NULL || ca_bundle[0] == '\0') {
        if (!tls_load_roots(tls)) {
            tls_release(tls);
            return -EIO;
        }
    } else {
        /*
         * A named file replaces the built-in roots rather than adding to them: whoever named it
         * wants a private CA honoured, and very likely only that one. And a file that is missing
         * or empty is a failure that names it, not a quiet fall back to the built-in set - a
         * broker behind a private CA would otherwise fail with "certificate not trusted" and
         * send whoever is debugging it after the broker rather than the path.
         *
         * A positive return is the count of certificates that failed to parse while others
         * succeeded, which a real-world bundle does routinely - expired roots, formats this build
         * was not configured for. That is fine as long as something loaded.
         */
        rc = mbedtls_x509_crt_parse_file(&state->ca, ca_bundle);
        if (rc < 0 || state->ca.version == 0) {
            (void)snprintf(tls->error, sizeof tls->error, "no usable certificates in %.80s",
                           ca_bundle);
            tls_release(tls);
            return -EIO;
        }
        if (rc > 0) {
            inkcell_log_debug("tls", "%d certificate(s) in %s did not parse", rc, ca_bundle);
        }
        roots = &state->ca;
    }

    rc = mbedtls_ssl_config_defaults(&state->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        tls_record_error(tls, rc, "could not configure TLS");
        tls_release(tls);
        return -EIO;
    }

    mbedtls_ssl_conf_authmode(&state->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&state->conf, roots, NULL);
    /* No `mbedtls_ssl_conf_rng()`: 4.x draws randomness from PSA, which psa_crypto_init() above
       has already seeded. There is no per-session DRBG to hand it any more. */

    rc = mbedtls_ssl_setup(&state->ssl, &state->conf);
    if (rc != 0) {
        tls_record_error(tls, rc, "could not start TLS");
        tls_release(tls);
        return -EIO;
    }
    /* Both the SNI extension and the name the certificate is checked against. */
    rc = mbedtls_ssl_set_hostname(&state->ssl, hostname);
    if (rc != 0) {
        tls_record_error(tls, rc, "bad hostname");
        tls_release(tls);
        return -EIO;
    }
    /* No timeout callback: the descriptor is watched by the event loop and the deadline belongs
       to whoever is driving the handshake, not to the library. */
    mbedtls_ssl_set_bio(&state->ssl, (void *)(intptr_t)fd, tls_bio_send, tls_bio_recv, NULL);
    return 0;
}

int mesh_tls_client_handshake(struct mesh_tls_client *tls) {
    if (tls == NULL || tls->state == NULL) {
        return -EINVAL;
    }
    if (tls->state->handshaked) {
        return 0;
    }
    const int rc = mbedtls_ssl_handshake(&tls->state->ssl);
    if (rc == 0) {
        tls->wants_write = false;
        tls->state->handshaked = true;
        inkcell_log_info("tls", "%s over %s", mbedtls_ssl_get_ciphersuite(&tls->state->ssl),
                         mbedtls_ssl_get_version(&tls->state->ssl));
        return 0;
    }
    /*
     * A verification failure is reported separately from everything else, because it is the one
     * a person can act on: the broker's certificate is not signed by anything in the bundle, or
     * the name on it is not the name they typed. mbedtls_ssl_get_verify_result() says which.
     */
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        char why[128];
        const uint32_t flags = mbedtls_ssl_get_verify_result(&tls->state->ssl);
        if (mbedtls_x509_crt_verify_info(why, sizeof why, "", flags) > 0) {
            char *newline = strchr(why, '\n');
            if (newline != NULL) {
                *newline = '\0';
            }
            /* No prefix: mbedtls_x509_crt_verify_info() already opens with "The certificate",
               and the string this ends up in is bounded for a status row - a prefix costs
               characters at the far end, where the reason actually is. */
            inkcell_str_copy(tls->error, sizeof tls->error, why);
        } else {
            inkcell_str_copy(tls->error, sizeof tls->error, "the certificate did not check out");
        }
        tls->wants_write = false;
        return -EPROTO;
    }
    return tls_translate(tls, rc, "TLS handshake");
}

int mesh_tls_client_read(struct mesh_tls_client *tls, uint8_t *out, size_t cap) {
    if (tls == NULL || tls->state == NULL || out == NULL || cap == 0U) {
        return -EINVAL;
    }
    /*
     * TLS 1.3 sends its session tickets *after* the handshake, so a read on a 1.3 session can
     * land on one at any point. Mbed TLS reports that by returning rather than by swallowing it,
     * and it is not a failure: the ticket record is consumed, nothing was decrypted for the
     * caller, and the stream continues on the next read.
     *
     * It cannot be answered with -EAGAIN either. Application data is often already sitting in
     * the session behind the ticket - a broker's CONNACK arrives in the same flight as often as
     * not - and the socket is empty by then, so epoll has nothing left to report and the
     * connection parks forever on bytes it has already received. Retrying here is the rule this
     * header states for callers, applied one level down.
     *
     * The retry is bounded, though, because this runs on the one event loop. Each turn consumes
     * a record and the socket is non-blocking, so a well-behaved peer ends this at WANT_READ
     * within a turn or two - a real broker sends one ticket. A peer that streams them faster
     * than they are decrypted would otherwise keep this function from returning, and nothing
     * above could stop it: `MQTT_READS_PER_TURN` bounds calls *to* here, not the work inside
     * one. So a run this long gives the loop back with `more_to_read` set instead, which is the
     * proxy's own way of saying the same thing one level up.
     *
     * The code is marked experimental in ssl.h, hence the guard.
     */
    tls->more_to_read = false;
    int rc;
    unsigned tickets = 0U;
    for (;;) {
        rc = mbedtls_ssl_read(&tls->state->ssl, out, cap);
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            if (++tickets < MESH_TLS_TICKETS_PER_READ) {
                continue;
            }
            tls->wants_write = false;
            tls->more_to_read = true;
            return -EAGAIN;
        }
#endif
        break;
    }
    if (rc > 0) {
        tls->wants_write = false;
        return rc;
    }
    if (rc == 0) {
        return -ENOTCONN;
    }
    return tls_translate(tls, rc, "TLS read");
}

int mesh_tls_client_write(struct mesh_tls_client *tls, const uint8_t *data, size_t len) {
    if (tls == NULL || tls->state == NULL || data == NULL || len == 0U) {
        return -EINVAL;
    }
    const int rc = mbedtls_ssl_write(&tls->state->ssl, data, len);
    if (rc > 0) {
        tls->wants_write = false;
        return rc;
    }
    return tls_translate(tls, rc, "TLS write");
}

void mesh_tls_client_stop(struct mesh_tls_client *tls) {
    if (tls == NULL) {
        return;
    }
    tls_release(tls);
    tls->fd = -1;
    tls->wants_write = false;
}

const char *mesh_tls_client_error(const struct mesh_tls_client *tls) {
    return tls != NULL ? tls->error : "";
}

#endif /* MESHCLIENT_HAVE_TLS */
