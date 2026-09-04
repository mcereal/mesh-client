#include "mesh/sha256.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const uint32_t k_round[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

static uint32_t rotr(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
}

static void sha256_block(struct mesh_sha256 *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16U; ++i) {
        w[i] = ((uint32_t)block[4U * i] << 24) | ((uint32_t)block[4U * i + 1U] << 16) |
               ((uint32_t)block[4U * i + 2U] << 8) | (uint32_t)block[4U * i + 3U];
    }
    for (unsigned i = 16U; i < 64U; ++i) {
        const uint32_t s0 = rotr(w[i - 15U], 7) ^ rotr(w[i - 15U], 18) ^ (w[i - 15U] >> 3);
        const uint32_t s1 = rotr(w[i - 2U], 17) ^ rotr(w[i - 2U], 19) ^ (w[i - 2U] >> 10);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (unsigned i = 0; i < 64U; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + k_round[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void mesh_sha256_init(struct mesh_sha256 *ctx) {
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
}

void mesh_sha256_update(struct mesh_sha256 *ctx, const void *data, size_t len) {
    if (ctx == NULL || (data == NULL && len > 0U)) {
        return;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    ctx->bits += (uint64_t)len * 8U;
    while (len > 0U) {
        const size_t room = sizeof ctx->block - ctx->block_len;
        const size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->block_len, bytes, take);
        ctx->block_len += take;
        bytes += take;
        len -= take;
        if (ctx->block_len == sizeof ctx->block) {
            sha256_block(ctx, ctx->block);
            ctx->block_len = 0U;
        }
    }
}

void mesh_sha256_final(struct mesh_sha256 *ctx, uint8_t out[MESH_SHA256_DIGEST_LEN]) {
    if (ctx == NULL || out == NULL) {
        return;
    }
    const uint64_t bits = ctx->bits;
    static const uint8_t k_pad = 0x80U;
    mesh_sha256_update(ctx, &k_pad, 1U);
    static const uint8_t k_zero = 0x00U;
    while (ctx->block_len != 56U) {
        mesh_sha256_update(ctx, &k_zero, 1U);
    }
    /* update() has been counting the padding into ctx->bits; the length appended is the
       message length captured before it. */
    uint8_t length[8];
    for (unsigned i = 0; i < 8U; ++i) {
        length[i] = (uint8_t)(bits >> (56U - 8U * i));
    }
    memcpy(ctx->block + 56U, length, sizeof length);
    sha256_block(ctx, ctx->block);
    ctx->block_len = 0U;

    for (unsigned i = 0; i < 8U; ++i) {
        out[4U * i] = (uint8_t)(ctx->state[i] >> 24);
        out[4U * i + 1U] = (uint8_t)(ctx->state[i] >> 16);
        out[4U * i + 2U] = (uint8_t)(ctx->state[i] >> 8);
        out[4U * i + 3U] = (uint8_t)ctx->state[i];
    }
}

void mesh_sha256_hex(const uint8_t digest[MESH_SHA256_DIGEST_LEN], char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (digest == NULL || out_len < MESH_SHA256_HEX_LEN) {
        return;
    }
    static const char k_hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < MESH_SHA256_DIGEST_LEN; ++i) {
        out[2U * i] = k_hex[(digest[i] >> 4) & 0x0FU];
        out[2U * i + 1U] = k_hex[digest[i] & 0x0FU];
    }
    out[2U * MESH_SHA256_DIGEST_LEN] = '\0';
}

int mesh_sha256_file(const char *path, uint8_t out[MESH_SHA256_DIGEST_LEN]) {
    if (path == NULL || out == NULL) {
        return -EINVAL;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -errno;
    }
    struct mesh_sha256 ctx;
    mesh_sha256_init(&ctx);
    uint8_t buffer[4096];
    for (;;) {
        const size_t read = fread(buffer, 1U, sizeof buffer, file);
        if (read > 0U) {
            mesh_sha256_update(&ctx, buffer, read);
        }
        if (read < sizeof buffer) {
            if (ferror(file) != 0) {
                fclose(file);
                return -EIO;
            }
            break;
        }
    }
    fclose(file);
    mesh_sha256_final(&ctx, out);
    return 0;
}
