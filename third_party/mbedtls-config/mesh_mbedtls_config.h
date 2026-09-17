#pragma once

/*
 * What this build of Mbed TLS is, and is not - the TLS and X.509 half.
 *
 * This is a MBEDTLS_USER_CONFIG_FILE: it is included *after* the library's own
 * `mbedtls_config.h` and subtracts from it. That direction is deliberate. A hand-written
 * replacement config starts from nothing and has to enumerate every primitive a handshake might
 * need - and the failure mode of getting that list wrong is not a build error, it is a client
 * that works against the broker the author tested and fails against somebody else's with a
 * cipher suite mismatch nobody can read. Starting from the library's own tested default and
 * removing only what this client provably cannot meet keeps that class of mistake out.
 *
 * What is removed is therefore narrow: protocols this never speaks, and the library's own
 * socket and timer code. Everything a TLS 1.2 or 1.3 client meets in the wild stays, including
 * the key exchanges that are merely unfashionable rather than gone.
 *
 * Mbed TLS 4.x is two projects, so this is one of a pair. Everything cryptographic - ciphers,
 * curves, where randomness comes from - is configured next door in `mesh_psa_crypto_config.h`,
 * which is the TF_PSA_CRYPTO_USER_CONFIG_FILE. A `MBEDTLS_*_C` knob that used to live here and
 * has no entry any more was not dropped on purpose; it no longer exists, because 4.0 replaced
 * the crypto side's build-time switches with `PSA_WANT_*`.
 *
 * This file is ours; `third_party/mbedtls` next to it is the vendored submodule and is never
 * edited. See docs/transport.md.
 */

/* ---- protocols this client does not speak ------------------------------------------------ */

/*
 * No DTLS. MQTT is TCP, and datagram TLS brings its own retransmission timer, replay window and
 * cookie exchange - a second, quite different state machine that nothing here would ever enter.
 */
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID

/*
 * No pre-shared keys, in either form 4.x still offers. Authentication here is a server
 * certificate against a CA bundle; a PSK suite would be a second trust model with no way to
 * configure it - the radio's MQTT settings carry a username and a password, not a key.
 *
 * The other four this file used to turn off - DHE-PSK, RSA-PSK, and static RSA and ECDH - are
 * absent rather than disabled. 4.0 removed the key exchanges without forward secrecy outright,
 * which is the same trim arriving upstream.
 */
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED

/* ---- the socket, which is ours ----------------------------------------------------------- */

/*
 * Mbed TLS ships its own blocking socket layer and its own timer. Both are exactly what this
 * project cannot use: there is one epoll loop and nothing may block on it, so src/core/tls_client.c
 * supplies non-blocking BIO callbacks over a descriptor it owns. Turning these off is not a size
 * optimisation - it is making the unusable thing unavailable, so a later change cannot reach for
 * `mbedtls_net_connect()` and quietly stall the UI.
 */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

/* ---- what is deliberately kept ----------------------------------------------------------- */

/*
 * `MBEDTLS_ERROR_C` stays, against the instinct to drop it. It is what turns a handshake failure
 * into `mbedtls_strerror()`'s sentence, and the whole reason this module exists in the shape it
 * does is that "why is this radio not reaching the broker" has to be answerable on a screen with
 * no terminal attached. A numeric -0x2700 on a handheld is not an answer.
 *
 * `MBEDTLS_SSL_SRV_C` stays, although this binary is only ever a client. It is what lets
 * tests/suites/mqtt_proxy.c stand a real TLS broker on a loopback socket and complete a real
 * handshake against it, with no network and no openssl in the container. A TLS path that has
 * never completed a handshake under test is one that ships broken, and the alternative - two
 * builds of the library with two configurations - is a worse trade than the code this leaves in.
 */
