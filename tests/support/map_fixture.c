#define _POSIX_C_SOURCE 200809L

/*
 * A pack of solid-colour tiles: a PNG encoder small enough to be obviously correct, and the
 * pack layout around it.
 *
 * The encoder writes a palette image - which is what a pack builder quantises to, and what the
 * decoder was measured against - with the pixel data in *stored* deflate blocks. Nothing here
 * compresses: a fixture that took a real deflate implementation to write would be a second
 * thing to get wrong in a file whose whole job is to be correct, and 66 KiB a tile in a
 * temporary file costs nothing. The checksums are the two that a decoder actually verifies, so
 * a tile that is wrong here is refused by the client exactly as a damaged one would be.
 */

#include "support/map_fixture.h"

#include "mesh/map/viewport.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAP_FIXTURE_HEADER_LEN 128U
#define MAP_FIXTURE_ENTRY_LEN 24U

static uint32_t map_fixture_crc32(const uint8_t *bytes, size_t len, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0U; i < len; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static uint32_t map_fixture_adler32(const uint8_t *bytes, size_t len) {
    uint32_t a = 1U;
    uint32_t b = 0U;
    for (size_t i = 0U; i < len; ++i) {
        a = (a + bytes[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

static void map_fixture_put_be32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

static void map_fixture_put_le16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value & 0xFFU);
    at[1] = (uint8_t)(value >> 8);
}

static void map_fixture_put_le32(uint8_t *at, uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) {
        at[i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
    }
}

static void map_fixture_put_le64(uint8_t *at, uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i) {
        at[i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
    }
}

/* A PNG chunk: length, type, payload, and the CRC over the type and the payload. */
static size_t map_fixture_chunk(uint8_t *out, const char *type, const uint8_t *payload,
                                size_t len) {
    map_fixture_put_be32(out, (uint32_t)len);
    memcpy(out + 4, type, 4U);
    if (len > 0U) {
        memcpy(out + 8, payload, len);
    }
    map_fixture_put_be32(out + 8 + len, map_fixture_crc32(out + 4, len + 4U, 0U));
    return len + 12U;
}

/* The order a pack's index is in: zoom, then x, then y. */
static bool map_fixture_before(struct mesh_map_tile_key a, struct mesh_map_tile_key b) {
    if (a.zoom != b.zoom) {
        return a.zoom < b.zoom;
    }
    if (a.x != b.x) {
        return a.x < b.x;
    }
    return a.y < b.y;
}

void mesh_test_map_tile_colour(size_t index, unsigned shade, uint8_t rgb[3]) {
    /* Spread through the cube rather than along one axis, so no two tiles in a fixture come out
       near enough to be confused by a test counting pixels - and so a second pack written with
       another shade is a different picture of the same tiles. */
    rgb[0] = (uint8_t)(40U + ((index * 53U + shade * 17U) % 200U));
    rgb[1] = (uint8_t)(40U + ((index * 97U + shade * 61U) % 200U));
    rgb[2] = (uint8_t)(40U + ((index * 29U + shade * 113U) % 200U));
}

/*
 * One 256x256 tile of a single colour, written into `out`.
 *
 * Returns the length, or 0 when the buffer is too small. The image is one palette entry deep,
 * so its pixel data is 256 rows of a filter byte and 256 zero indices.
 */
static size_t map_fixture_png(uint8_t *out, size_t cap, const uint8_t rgb[3]) {
    const size_t raw_len = (size_t)MESH_MAP_TILE_SIZE * ((size_t)MESH_MAP_TILE_SIZE + 1U);
    /* Two bytes of zlib header, four of adler, and a five-byte header per stored block. */
    const size_t blocks = (raw_len + 65534U) / 65535U;
    const size_t needed = 8U + 25U + 12U + 3U + 12U + (12U + 2U + raw_len + blocks * 5U + 4U);
    if (cap < needed) {
        return 0U;
    }
    uint8_t *const raw = calloc(1U, raw_len);
    uint8_t *const stream = calloc(1U, 2U + raw_len + blocks * 5U + 4U);
    if (raw == NULL || stream == NULL) {
        free(raw);
        free(stream);
        return 0U;
    }
    /* Every row is filter 0 - "none" - and then 256 indices into a palette of one. */
    for (size_t row = 0U; row < (size_t)MESH_MAP_TILE_SIZE; ++row) {
        raw[row * ((size_t)MESH_MAP_TILE_SIZE + 1U)] = 0U;
    }

    size_t at = 0U;
    stream[at++] = 0x78U; /* deflate, 32 KiB window */
    stream[at++] = 0x01U; /* no preset dictionary, fastest - the check byte for 0x78 */
    size_t remaining = raw_len;
    const uint8_t *cursor = raw;
    while (remaining > 0U) {
        const uint16_t take = remaining > 65535U ? (uint16_t)65535U : (uint16_t)remaining;
        stream[at++] = (uint8_t)(remaining == (size_t)take ? 1U : 0U); /* final block? */
        map_fixture_put_le16(stream + at, take);
        at += 2U;
        map_fixture_put_le16(stream + at, (uint16_t)~take);
        at += 2U;
        memcpy(stream + at, cursor, take);
        at += take;
        cursor += take;
        remaining -= take;
    }
    map_fixture_put_be32(stream + at, map_fixture_adler32(raw, raw_len));
    at += 4U;

    static const uint8_t signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'};
    size_t length = 0U;
    memcpy(out, signature, sizeof signature);
    length += sizeof signature;

    uint8_t ihdr[13];
    map_fixture_put_be32(ihdr, (uint32_t)MESH_MAP_TILE_SIZE);
    map_fixture_put_be32(ihdr + 4, (uint32_t)MESH_MAP_TILE_SIZE);
    ihdr[8] = 8U;  /* bits per sample */
    ihdr[9] = 3U;  /* colour type: palette */
    ihdr[10] = 0U; /* deflate */
    ihdr[11] = 0U; /* the only filter method there is */
    ihdr[12] = 0U; /* not interlaced */
    length += map_fixture_chunk(out + length, "IHDR", ihdr, sizeof ihdr);
    length += map_fixture_chunk(out + length, "PLTE", rgb, 3U);
    length += map_fixture_chunk(out + length, "IDAT", stream, at);
    length += map_fixture_chunk(out + length, "IEND", NULL, 0U);

    free(raw);
    free(stream);
    return length;
}

int mesh_test_map_pack_write(struct mesh_test_map_pack *pack, const struct mesh_map_tile_key *keys,
                             size_t count, unsigned shade, const char *name,
                             const char *attribution) {
    if (pack == NULL || keys == NULL || count == 0U) {
        return -EINVAL;
    }
    memset(pack, 0, sizeof *pack);

    /* One tile is about 66 KiB of stored deflate, and the whole pack is built in memory before
       anything is written - a fixture that wrote as it went would leave a half-pack behind on
       the first failure. */
    const size_t tile_cap = 96U * 1024U;
    const size_t index_at = MAP_FIXTURE_HEADER_LEN;
    const size_t tiles_at = index_at + count * MAP_FIXTURE_ENTRY_LEN;
    uint8_t *const bytes = calloc(1U, tiles_at + count * tile_cap);
    if (bytes == NULL) {
        return -ENOMEM;
    }

    /*
     * Sorted, because an index that is not is the one corruption a reader cannot catch at the
     * blit: a binary search over an unsorted index does not fail, it misses. The pack refuses
     * one at open (tests/suites/map_pack.c), so a fixture that handed its keys over in the
     * order a caller found convenient would simply not open.
     */
    struct mesh_map_tile_key *const sorted = calloc(count, sizeof *sorted);
    if (sorted == NULL) {
        free(bytes);
        return -ENOMEM;
    }
    memcpy(sorted, keys, count * sizeof *sorted);
    for (size_t i = 1U; i < count; ++i) {
        const struct mesh_map_tile_key held = sorted[i];
        size_t j = i;
        while (j > 0U && map_fixture_before(held, sorted[j - 1U])) {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = held;
    }

    memcpy(bytes, "MCTPACK2", 8U);
    map_fixture_put_le16(bytes + 8, (uint16_t)MESH_MAP_TILE_SIZE);
    bytes[10] = 1U; /* PNG */
    map_fixture_put_le32(bytes + 12, (uint32_t)index_at);
    map_fixture_put_le32(bytes + 16, (uint32_t)count);
    map_fixture_put_le64(bytes + 24, 0U);
    if (attribution != NULL) {
        memcpy(bytes + 32, attribution, strlen(attribution));
    }
    if (name != NULL) {
        memcpy(bytes + 96, name, strlen(name));
    }

    size_t offset = tiles_at;
    for (size_t i = 0U; i < count; ++i) {
        uint8_t rgb[3];
        mesh_test_map_tile_colour(i, shade, rgb);
        const size_t written = map_fixture_png(bytes + offset, tile_cap, rgb);
        if (written == 0U) {
            free(sorted);
            free(bytes);
            return -ENOBUFS;
        }
        uint8_t *const entry = bytes + index_at + i * MAP_FIXTURE_ENTRY_LEN;
        entry[0] = sorted[i].zoom;
        map_fixture_put_le32(entry + 4, sorted[i].x);
        map_fixture_put_le32(entry + 8, sorted[i].y);
        map_fixture_put_le32(entry + 12, (uint32_t)written);
        map_fixture_put_le64(entry + 16, (uint64_t)offset);
        offset += written;
    }

    snprintf(pack->path, sizeof pack->path, "/tmp/meshclient_tiles_XXXXXX");
    const int fd = mkstemp(pack->path);
    if (fd < 0) {
        free(sorted);
        free(bytes);
        pack->path[0] = '\0';
        return -errno;
    }
    const ssize_t n = write(fd, bytes, offset);
    close(fd);
    free(sorted);
    free(bytes);
    if (n != (ssize_t)offset) {
        mesh_test_map_pack_remove(pack);
        return -EIO;
    }
    return 0;
}

void mesh_test_map_pack_remove(struct mesh_test_map_pack *pack) {
    if (pack == NULL || pack->path[0] == '\0') {
        return;
    }
    (void)unlink(pack->path);
    pack->path[0] = '\0';
}

size_t mesh_test_map_keys_around(int32_t latitude_i, int32_t longitude_i, uint8_t zoom, int columns,
                                 int rows, struct mesh_map_tile_key *out, size_t cap) {
    if (out == NULL || columns <= 0 || rows <= 0) {
        return 0U;
    }
    /* A one-pixel box is the tile the centre stands on, whatever the projection does with it. */
    struct mesh_map_viewport viewport;
    mesh_map_viewport_init(&viewport, latitude_i, longitude_i, zoom);
    mesh_map_viewport_resize(&viewport, 1, 1);
    struct mesh_map_tile_span span;
    struct mesh_map_tile_key centre;
    if (!mesh_map_viewport_tiles(&viewport, &span) ||
        !mesh_map_tile_span_key(&span, 0, 0, &centre)) {
        return 0U;
    }

    const uint32_t world = (uint32_t)1 << zoom;
    size_t written = 0U;
    for (int row = -(rows / 2); row <= rows / 2 && written < cap; ++row) {
        for (int column = -(columns / 2); column <= columns / 2 && written < cap; ++column) {
            const int64_t y = (int64_t)centre.y + row;
            if (y < 0 || y >= (int64_t)world) {
                continue; /* there is nothing above the top row of the world */
            }
            struct mesh_map_tile_key key = {
                .zoom = zoom,
                /* East of the antimeridian is west of it, which is the one wrap a fixture has to
                   get right for the same reason the span does. */
                .x = (uint32_t)(((int64_t)centre.x + column + world) % world),
                .y = (uint32_t)y,
            };
            out[written++] = key;
        }
    }
    return written;
}
