#pragma once

/*
 * SHA-256, for verifying a downloaded update against the digest the GitHub API reports.
 *
 * Self-contained on purpose: the release build is a static musl binary with libdbus as its only
 * dependency, and shelling out to the device's sha256sum would put the one check that makes
 * downloading an executable safe at the mercy of whatever busybox happens to ship.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_SHA256_DIGEST_LEN 32U
/* 64 hex digits and a NUL. */
#define MESH_SHA256_HEX_LEN 65U

struct mesh_sha256 {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t block_len;
};

void mesh_sha256_init(struct mesh_sha256 *ctx);
void mesh_sha256_update(struct mesh_sha256 *ctx, const void *data, size_t len);
void mesh_sha256_final(struct mesh_sha256 *ctx, uint8_t out[MESH_SHA256_DIGEST_LEN]);

/* Lower-case hex, NUL-terminated. `out_len` must be at least MESH_SHA256_HEX_LEN. */
void mesh_sha256_hex(const uint8_t digest[MESH_SHA256_DIGEST_LEN], char *out, size_t out_len);

/* Hashes a whole file. Returns 0, or -errno. */
int mesh_sha256_file(const char *path, uint8_t out[MESH_SHA256_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif
