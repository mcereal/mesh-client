#pragma once

/*
 * What this build of Mbed TLS is, and is not - the cryptographic half.
 *
 * This is a TF_PSA_CRYPTO_USER_CONFIG_FILE: it is included *after* tf-psa-crypto's own
 * `psa/crypto_config.h` and subtracts from it, for the same reason its TLS-side twin
 * `mesh_mbedtls_config.h` does. Read that file's opening comment first; it explains why this
 * pair subtracts from a tested default rather than enumerating a minimum, and the reasoning
 * applies here unchanged.
 *
 * Mbed TLS 4.0 moved every primitive out of the TLS project and switched the crypto side from
 * `MBEDTLS_*_C` to `PSA_WANT_*`. So this file is the 3.x config's second half, rewritten in the
 * names that replaced it - and shorter than that half was, because upstream's own default has
 * since dropped several of the things this used to turn off by hand.
 *
 * This file is ours; `third_party/mbedtls` next to it is the vendored submodule and is never
 * edited. See docs/transport.md.
 */

/* ---- primitives no broker offers --------------------------------------------------------- */

/*
 * Block ciphers that are not AES. Camellia and ARIA are regional alternatives that no public
 * broker has ever negotiated.
 *
 * 3DES was the third name on this list and is not here: 4.x's default does not build it at all,
 * which is the withdrawal this build was anticipating.
 */
#undef PSA_WANT_KEY_TYPE_ARIA
#undef PSA_WANT_KEY_TYPE_CAMELLIA

/*
 * Elliptic curves, trimmed to the ones that exist in practice: P-256, P-384, P-521 and
 * Curve25519, plus the Curve448 the default already carries. What goes is the Koblitz curve,
 * which TLS never adopted, and Brainpool, which is specified for TLS and used by essentially
 * nobody.
 *
 * The sub-256-bit NIST curves this used to name - secp192r1, secp224r1, secp192k1, secp224k1 -
 * are gone from 4.x's default, below the strength any current CA will issue.
 *
 * Each curve is a table of constants, so this is where most of the size saved actually comes
 * from. It is also the trim most likely to be wrong later: if a handshake ever fails on a named
 * group, this list is the first place to look.
 */
#undef PSA_WANT_ECC_SECP_K1_256
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_256
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_384
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_512

/* Built-in test vectors for every primitive, which nothing in this binary calls. */
#undef MBEDTLS_SELF_TEST

/* ---- what is deliberately kept ----------------------------------------------------------- */

/*
 * `MBEDTLS_FS_IO` stays because the CA bundle is a file - the one the pak ships, since the Brick
 * has no system certificate store at all. It is configured on this side of the split in 4.x,
 * although the thing that needs it - `mbedtls_x509_crt_parse_file()` - is on the other.
 */

/* ---- where randomness comes from ---------------------------------------------------------- */

/*
 * **The entropy file, which upstream defaults to the blocking one.**
 *
 * tf-psa-crypto reaches for a dedicated system call first and only falls back to reading a file
 * when one was not available both at compile time and at run time - and the cross toolchain this
 * pak is built with does not satisfy that. So the device build takes the file path, and the file
 * upstream picks by default is `/dev/random`.
 *
 * On a desktop that is merely old-fashioned. On the Brick it is a hang. `/dev/random` blocks
 * until the kernel's entropy *estimate* reaches read_wakeup_threshold (64 bits here), and a
 * handheld with no disks, no network interrupts worth the name and no hardware RNG refills that
 * estimate at a crawl: measured at 15 bits, climbing to 54 over three minutes, with a TLS
 * handshake waiting on the other side of it.
 *
 * And the wait is not in a corner. `mesh_tls_client_start()` calls `psa_crypto_init()` inline,
 * on the one epoll thread, and that is what seeds the PSA RNG in 4.x - so the whole client stops:
 * no frames, no button handling, not even the MENU press that quits it. It presents as a frozen
 * device, and the only thing on screen that could explain it is a status card that is no longer
 * being drawn.
 *
 * (In 3.x the inline seed was this file's own `mbedtls_ctr_drbg_seed()` call. 4.0 removed the
 * DRBG plumbing from the TLS config and the wait moved into `psa_crypto_init()`. Same thread,
 * same stall, one frame earlier.)
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
