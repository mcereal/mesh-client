#pragma once

/*
 * What this build of Mbed TLS is, and is not.
 *
 * This is a MBEDTLS_USER_CONFIG_FILE: it is included *after* the library's own
 * `mbedtls_config.h` and subtracts from it. That direction is deliberate. A hand-written
 * replacement config starts from nothing and has to enumerate every primitive a handshake might
 * need - and the failure mode of getting that list wrong is not a build error, it is a client
 * that works against the broker the author tested and fails against somebody else's with a
 * cipher suite mismatch nobody can read. Starting from the library's own tested default and
 * removing only what this client provably cannot meet keeps that class of mistake out.
 *
 * What is removed is therefore narrow: protocols this never speaks, primitives no public broker
 * has offered this decade, and the library's own socket and self-test code. Everything a TLS 1.2
 * or 1.3 client meets in the wild stays, including the key exchanges and curves that are merely
 * unfashionable rather than gone.
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
 * No pre-shared keys, in any of their forms. Authentication here is a server certificate against
 * a CA bundle; a PSK suite would be a second trust model with no way to configure it - the
 * radio's MQTT settings carry a username and a password, not a key.
 */
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
/* Static RSA and static ECDH: no forward secrecy, gone from TLS 1.3, and not offered by any
   broker that has been configured this decade. */
#undef MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED

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

/* Built-in test vectors for every primitive, which nothing in this binary calls. */
#undef MBEDTLS_SELF_TEST

/* ---- primitives no broker offers --------------------------------------------------------- */

/* Block ciphers that are not AES: 3DES is withdrawn, and Camellia and ARIA are regional
   alternatives that no public broker has ever negotiated. */
#undef MBEDTLS_DES_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_ARIA_C

/*
 * Elliptic curves, trimmed to the ones that exist in practice: P-256, P-384, P-521 and
 * Curve25519, plus the Curve448 the default already carries. What goes is the sub-256-bit NIST
 * curves - below the strength any current CA will issue - the Koblitz curves, which TLS never
 * adopted, and Brainpool, which is specified for TLS and used by essentially nobody.
 *
 * Each curve is a table of constants, so this is where most of the size saved actually comes
 * from. It is also the trim most likely to be wrong later: if a handshake ever fails on a named
 * group, this list is the first place to look.
 */
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED

/* ---- what is deliberately kept ----------------------------------------------------------- */

/*
 * `MBEDTLS_ERROR_C` stays, against the instinct to drop it. It is what turns a handshake failure
 * into `mbedtls_strerror()`'s sentence, and the whole reason this module exists in the shape it
 * does is that "why is this radio not reaching the broker" has to be answerable on a screen with
 * no terminal attached. A numeric -0x2700 on a handheld is not an answer.
 *
 * `MBEDTLS_FS_IO` stays because the CA bundle is a file - the one the pak ships, since the Brick
 * has no system certificate store at all.
 *
 * `MBEDTLS_SSL_SRV_C` stays, although this binary is only ever a client. It is what lets
 * tests/suites/mqtt_proxy.c stand a real TLS broker on a loopback socket and complete a real
 * handshake against it, with no network and no openssl in the container. A TLS path that has
 * never completed a handshake under test is one that ships broken, and the alternative - two
 * builds of the library with two configurations - is a worse trade than the code this leaves in.
 */

/* ---- where randomness comes from ---------------------------------------------------------- */

/*
 * **The entropy file, which upstream defaults to the blocking one.**
 *
 * mbedtls reaches for `getrandom()` first and only falls back to reading a file when
 * `HAVE_GETRANDOM` was not detected at compile time - and that detection is a glibc version test
 * in entropy_poll.c that the cross toolchain this pak is built with does not satisfy. So the
 * device build takes the file path, and the file upstream picks by default is `/dev/random`.
 *
 * On a desktop that is merely old-fashioned. On the Brick it is a hang. `/dev/random` blocks
 * until the kernel's entropy *estimate* reaches read_wakeup_threshold (64 bits here), and a
 * handheld with no disks, no network interrupts worth the name and no hardware RNG refills that
 * estimate at a crawl: measured at 15 bits, climbing to 54 over three minutes, with a TLS
 * handshake waiting on the other side of it.
 *
 * And the wait is not in a corner. `mesh_tls_client_start()` seeds the DRBG inline, on the one
 * epoll thread, so the whole client stops - no frames, no button handling, not even the MENU
 * press that quits it. It presents as a frozen device, and the only thing on screen that could
 * explain it is a status card that is no longer being drawn.
 *
 * `/dev/urandom` is the same CSPRNG on any kernel this runs on, it is seeded from the same pool,
 * and it does not block once that pool has been initialised - which happened days before any of
 * this. The "entropy depletion" argument for preferring /dev/random does not describe how
 * modern Linux works; what it does describe is this hang.
 *
 * It bit on the second connection rather than the first, which is worth knowing when reading a
 * bug report: five days of uptime had banked enough for one handshake, and that handshake spent
 * it.
 */
#undef MBEDTLS_PLATFORM_DEV_RANDOM
#define MBEDTLS_PLATFORM_DEV_RANDOM "/dev/urandom"
