#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A TLS client session over a descriptor somebody else owns.
 *
 * Two things use it: a broker connection (mqtt_proxy.c) and an HTTPS request (fetch.c). Both have
 * to sit on the same epoll loop as everything else and be readable and writable between UI
 * frames - a broker connection because it is long-lived and bidirectional, a download because it
 * is megabytes arriving while the screen still has to draw. There is nowhere to block.
 *
 * So this is Mbed TLS (third_party/mbedtls, a pinned submodule) driven through BIO callbacks
 * over a non-blocking socket, reporting `-EAGAIN` all the way up rather than waiting. The
 * library is configured by the pair of files in third_party/mbedtls-config/, which say what was
 * taken out of it and why - `mesh_mbedtls_config.h` for TLS and X.509, `mesh_psa_crypto_config.h`
 * for everything cryptographic, because 4.x is two projects.
 *
 * **Certificates are always verified.** There is no insecure mode and no argument that turns one
 * on. A device with no system certificate store is what makes one tempting - but a connection
 * carries this client's traffic to somebody else's server, and an unauthenticated one is a
 * connection to whoever answered.
 *
 * *What* they are verified against is not decided here. `mesh_tls_set_roots()` is handed the
 * trust anchors once at startup and every session is checked against those; where they came from
 * - compiled in, read off a card, taken from a system store - is a question about the product,
 * not about TLS. This client compiles Mozilla's set in (include/mesh/core/ca_roots.h) for a
 * reason that is entirely its own, and that file is where it is written down.
 *
 * A build without the submodule compiles this to a stub that reports itself unavailable and
 * refuses to start, the same way the BLE transport compiles out without D-Bus headers. Nothing
 * that uses it needs an #ifdef.
 */

/* The descriptor is borrowed, never closed here: the caller opened it, the caller connected it,
   and on any error the caller is the one that has to decide whether to retry. */
struct mesh_tls_state; /* defined in src/core/net/tls_client.c; heap-held, one per session */

struct mesh_tls_client {
    int fd;
    struct mesh_tls_state *state; /* NULL until start(), and again after stop() */
    /*
     * Which direction the session is blocked on, which is *not* a property of what the caller
     * asked for. A handshake flight, and a write that triggers one, can block waiting to read;
     * a read can block waiting to write. Arming the wrong epoll event parks the connection
     * forever on a socket that will never become ready in the direction being watched.
     */
    bool wants_write;
    /*
     * Set when read() gave the loop back with work still inside the session rather than because
     * the socket was empty - today, a run of session tickets long enough to hit its budget. The
     * descriptor may well have nothing to report, so a caller that waits for epoll after this
     * waits forever; it has to come back of its own accord, the way the MQTT proxy's own
     * `more_to_read` does. Cleared at the top of every read.
     */
    bool more_to_read;
    char error[160];
};

/* True when this build has Mbed TLS at all. False makes every start() fail with -ENOTSUP. */
bool mesh_tls_available(void);

/* One trust anchor: the DER, and the label whoever supplied it gives it - which is what a log
   line has to name when a root will not parse, since the DER itself says nothing a reader can
   use. */
struct mesh_tls_ca_root {
    const char *name;
    const unsigned char *der;
    size_t len;
};

/*
 * The trust anchors every session is verified against. Called once, before the first session,
 * from wherever this process decides what it trusts.
 *
 * `roots` is borrowed and must be static for the life of the process: the certificates are parsed
 * without copying, so the chain Mbed TLS walks points into this table rather than into the heap.
 * Calling it again replaces the set and discards what was parsed from the last one.
 *
 * There is no default and no fallback. With nothing registered, a session that names no bundle
 * file fails at start() rather than connecting to something it cannot check - because a layer
 * this far down has no business holding an opinion about who a product trusts, and the failure
 * mode of guessing is a connection that looks fine.
 */
void mesh_tls_set_roots(const struct mesh_tls_ca_root *roots, size_t count);

/*
 * The bundle file somebody asked for in place of the registered roots - `SSL_CERT_FILE`, the
 * variable OpenSSL and curl already honour - or NULL for none. For a broker behind a private CA,
 * and deliberately nothing more clever: a path that is set is used, readable or not, so that a
 * typo fails naming the file rather than falling back and failing somewhere less obvious.
 */
const char *mesh_tls_ca_override(void);

/*
 * Begins a session on `fd`, which must already be open, non-blocking and connected (or
 * connecting - the first handshake flight will simply block until it is).
 *
 * `hostname` is both the SNI to send and the name the certificate is checked against, so it is
 * the name the user typed rather than the address it resolved to: a certificate is issued for a
 * name, and checking one against an IP address fails for every broker on the internet.
 *
 * `ca_bundle` is a PEM file to verify against instead of the registered roots, or NULL/"" for the
 * registered roots. A named file that is missing or holds nothing usable is a hard failure, never
 * a fallback - see mesh_tls_ca_override(). Returns 0, -ENOTSUP without Mbed TLS, -EINVAL for bad
 * arguments, -EIO when the library or the bundle would not initialise, or -EIO with no roots
 * registered and no bundle named, with error() set.
 */
int mesh_tls_client_start(struct mesh_tls_client *tls, int fd, const char *hostname,
                          const char *ca_bundle);

/*
 * Drives the handshake. Returns 0 when it is complete, -EAGAIN when it needs the socket to
 * become ready again (check wants_write for which way), or a negative errno on a failure that
 * will not improve - a bad certificate, a name that does not match, no shared cipher suite.
 *
 * Call it once per readiness event until it stops saying -EAGAIN. It is safe to call after it
 * has returned 0, where it returns 0 again.
 */
int mesh_tls_client_handshake(struct mesh_tls_client *tls);

/*
 * Reads decrypted bytes. Returns the count, 0 never, -EAGAIN when there is nothing ready,
 * -ENOTCONN when the peer closed the session cleanly, or another negative errno.
 *
 * **A reader must loop until -EAGAIN.** One TLS record can hold more than one call's worth of
 * plaintext, and the leftovers live inside the session rather than in the socket - so the
 * descriptor is empty, epoll has nothing to report, and a reader that stops after one call waits
 * forever on data it has already received. This is the classic way TLS on an event loop hangs.
 *
 * **A -EAGAIN with `more_to_read` set is not an empty socket** and must not be answered by
 * waiting for one. It means this stopped early to give the loop back, and the caller has to
 * call again on its next turn.
 */
int mesh_tls_client_read(struct mesh_tls_client *tls, uint8_t *out, size_t cap);

/*
 * Writes plaintext. Returns the count written, which may be short, -EAGAIN when the socket
 * would block, or another negative errno.
 *
 * A short write must be retried with the same bytes at the same offset: Mbed TLS has already
 * framed them into a record and will refuse a caller that comes back with something else.
 */
int mesh_tls_client_write(struct mesh_tls_client *tls, const uint8_t *data, size_t len);

/*
 * Ends the session and releases everything it held. Does **not** close the descriptor.
 *
 * No close_notify is sent. It would be one more write that can block on a socket the caller is
 * about to close anyway, and the thing it protects against - a truncation attack on a stream
 * whose end is meaningful - is the *peer's* to worry about: MQTT messages carry their own
 * length, and fetch.c sends one request and reads the server's close_notify, not its own.
 */
void mesh_tls_client_stop(struct mesh_tls_client *tls);

/* Why the last call failed, in words, or "" when none has. Never NULL. */
const char *mesh_tls_client_error(const struct mesh_tls_client *tls);

#ifdef __cplusplus
}
#endif
