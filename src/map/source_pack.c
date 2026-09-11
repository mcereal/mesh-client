#define _POSIX_C_SOURCE 200809L

#include "mesh/map/source.h"

#include "mesh/geo/coords.h"
#include "mesh/geo/mercator.h"
#include "mesh/utils/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * The tile pack: one file, an index read whole at open, and a tile that is a binary search in
 * RAM plus one pread.
 *
 * It is shaped like PMTiles and it is not PMTiles, which is worth saying plainly. What the two
 * share is the only property that mattered on the Brick: a tile is at a known offset in a
 * single file, so reading one costs a seek and a read rather than a directory walk or a B-tree
 * descent. The device measurement in docs/maps-roadmap.md compared exactly that against a
 * z/x/y tree and against MBTiles, and this shape won every column - 0.80 ms to a tile's bytes
 * from cold against 4.6 ms for either of the others, because the card is FAT32 with 32 KiB
 * clusters mounted `sync`, where a tree of three thousand small files occupies three and a half
 * times its own size and a lookup walks FAT directories. That is a fact about this hardware,
 * and it is why the client carries a format of its own instead of reading one of the two
 * formats the rest of the world publishes. A converter is the host's job and lives in
 * devtools/map_pack.
 *
 * On disk, little-endian throughout and read byte by byte so the host's alignment rules do not
 * come into it:
 *
 *     0    8   "MCTPACK2"
 *     8    2   tile size in pixels
 *     10   1   tile format (1 = PNG)
 *     11   1   reserved, zero
 *     12   4   index offset from the start of the file
 *     16   4   tile count
 *     20   4   reserved, zero
 *     24   8   generated: seconds since the epoch, 0 when the builder did not say
 *     32   64  attribution, UTF-8, NUL padded
 *     96   32  name, UTF-8, NUL padded
 *     128      the index: `count` 24-byte entries, sorted ascending by (z, x, y)
 *                  0  1  zoom
 *                  1  3  reserved, zero
 *                  4  4  x
 *                  8  4  y
 *                  12 4  length in bytes
 *                  16 8  offset from the start of the file
 *              then the tile bytes, in index order
 *
 * "MCTPACK2" rather than "MCTPACK1" because there was a 1: the headerless layout
 * devtools/tile_bench measured with, which was never a format anything shipped and carried no
 * way to say what was in it. The 2 is that layout plus the header that makes a pack
 * self-describing, and the 24-byte index entry is deliberately unchanged so that the measured
 * numbers are the numbers this reader gets.
 *
 * Everything the reader will later trust is checked once, here. A tile read afterwards is a
 * bsearch and a pread and cannot be wrong: the entries are known sorted, so the search is
 * valid; the lengths are known bounded, so the caller's buffer is enough; and every extent is
 * known to lie inside the file, so a corrupt offset cannot become a read of somebody else's
 * data. The alternative - checking at each read - pays the check on the event loop, every
 * frame, for a file that has not changed since it was opened.
 */

#define PACK_MAGIC "MCTPACK2"
#define PACK_MAGIC_LEN 8U
#define PACK_HEADER_LEN 128U
#define PACK_ENTRY_LEN 24U

/*
 * The most tiles a pack may index.
 *
 * It is a bound on an allocation before it is anything else: `count` comes off the disk, and
 * 24 bytes times an unchecked u32 is a hundred gigabytes. A million tiles is 24 MB of index,
 * which is the number docs/maps-roadmap.md reasons about as the point where a format needs leaf
 * directories - so this is also where the client would have to grow them rather than a limit
 * anything real is near. A metropolitan pack to zoom 16 is tens of thousands.
 */
#define PACK_TILES_MAX 1000000U

struct pack_entry {
    uint32_t x;
    uint32_t y;
    uint32_t length;
    uint64_t offset;
    uint8_t zoom;
};

struct pack {
    int fd;
    uint32_t count;
    struct pack_entry *entries;
};

static uint16_t pack_u16(const uint8_t *at) {
    return (uint16_t)((uint16_t)at[0] | (uint16_t)((uint16_t)at[1] << 8));
}

static uint32_t pack_u32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

static uint64_t pack_u64(const uint8_t *at) {
    return (uint64_t)pack_u32(at) | ((uint64_t)pack_u32(at + 4) << 32);
}

/* A NUL-padded fixed field copied out as a C string. The last byte is forced to NUL rather than
   trusted, because the field is as long as the string it may hold and a pack written by
   something that did not know that would hand a renderer an unterminated one. */
static void pack_text(char *out, size_t cap, const uint8_t *at) {
    memcpy(out, at, cap - 1U);
    out[cap - 1U] = '\0';
}

/* pread until the whole extent is in the buffer. Short reads are ordinary on a card that is
   being pulled out, and they are exactly where a half-filled index would come from. */
static bool pack_read_at(int fd, uint8_t *buffer, size_t len, uint64_t offset) {
    size_t got = 0U;
    while (got < len) {
        const ssize_t n = pread(fd, buffer + got, len - got, (off_t)(offset + got));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

/* (z, x, y) order, which is the order the index is sorted in and the order a bsearch compares
   in. One function so the check at open and the lookup at read cannot disagree about what
   "sorted" means - an index the search believes is ordered and is not does not fail, it misses. */
static int pack_compare(const void *left, const void *right) {
    const struct pack_entry *const a = left;
    const struct pack_entry *const b = right;
    if (a->zoom != b->zoom) {
        return a->zoom < b->zoom ? -1 : 1;
    }
    if (a->x != b->x) {
        return a->x < b->x ? -1 : 1;
    }
    if (a->y != b->y) {
        return a->y < b->y ? -1 : 1;
    }
    return 0;
}

static const struct pack_entry *pack_find(const struct pack *pack, struct mesh_map_tile_key key) {
    const struct pack_entry want = {.zoom = key.zoom, .x = key.x, .y = key.y};
    return bsearch(&want, pack->entries, pack->count, sizeof *pack->entries, pack_compare);
}

/*
 * The area the pack could answer for, taken from the tiles it holds.
 *
 * Derived rather than declared, which is the rule the back arrow and the map's selection follow
 * for the same reason: a header field saying where a pack covers is a field that can be wrong,
 * and the way it goes wrong is a builder that names a city and packs a suburb. This cannot be
 * wrong about anything except sparseness, and sparseness is a state the map has to draw anyway.
 *
 * Measured on the unit square rather than in degrees, so tiles at different zooms are directly
 * comparable: tile (z, x, y) occupies [x, x+1] / 2^z of the world's width. A pack that straddles
 * the antimeridian comes back as the whole world wide, which is honest about the box and not
 * about the pack - nothing builds one, and the seam is where a bounding box stops being able to
 * say anything useful.
 */
static void pack_coverage(const struct pack *pack, struct mesh_map_source_info *info) {
    double west = 1.0;
    double east = 0.0;
    double north = 1.0;
    double south = 0.0;
    for (uint32_t i = 0U; i < pack->count; ++i) {
        const struct pack_entry *const entry = &pack->entries[i];
        const double span = 1.0 / (double)((uint64_t)1 << entry->zoom);
        const double x0 = (double)entry->x * span;
        const double y0 = (double)entry->y * span;
        if (x0 < west) {
            west = x0;
        }
        if (x0 + span > east) {
            east = x0 + span;
        }
        if (y0 < north) {
            north = y0;
        }
        if (y0 + span > south) {
            south = y0 + span;
        }
    }

    int32_t unused = 0;
    /* The inverse wraps x, so a pack reaching the world's right-hand edge would come back at
       180 degrees west. The edge is the antimeridian either way; naming it east is what keeps
       west <= east for every pack that does not cross it. */
    if (east >= 1.0) {
        info->east_i = MESH_GEO_LONGITUDE_I_MAX;
    } else {
        mesh_geo_mercator_inverse((struct mesh_geo_point){.x = east, .y = 0.5}, &unused,
                                  &info->east_i);
    }
    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = west, .y = 0.5}, &unused, &info->west_i);
    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = 0.5, .y = north}, &info->north_i,
                              &unused);
    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = 0.5, .y = south}, &info->south_i,
                              &unused);
}

/*
 * Walks the index once, refusing anything a later read would have had to trust.
 *
 * Four separate questions, and each of them is a real corruption rather than a hypothetical
 * one: a zoom the client cannot address, a tile outside the world at its own zoom, an extent
 * that is not inside the file, and an order the binary search would silently mis-search.
 */
static bool pack_check(const struct pack *pack, uint64_t file_len, uint64_t tiles_at) {
    for (uint32_t i = 0U; i < pack->count; ++i) {
        const struct pack_entry *const entry = &pack->entries[i];
        const struct mesh_map_tile_key key = {.zoom = entry->zoom, .x = entry->x, .y = entry->y};
        if (!mesh_map_tile_key_valid(key)) {
            return false;
        }
        if (entry->length == 0U || entry->length > MESH_MAP_TILE_BYTES_MAX) {
            return false;
        }
        /* Written as a subtraction rather than as `offset + length > file_len`, because the
           offset comes off the disk as a u64: a corrupt one near the top of the range makes
           that sum wrap round to a small number and pass a check it should have failed. */
        if (entry->offset < tiles_at || entry->offset > file_len ||
            file_len - entry->offset < entry->length) {
            return false;
        }
        if (i > 0U && pack_compare(&pack->entries[i - 1U], entry) >= 0) {
            return false;
        }
    }
    return true;
}

static int pack_read(struct mesh_map_source *source, struct mesh_map_tile_key key, uint8_t *out,
                     size_t cap) {
    struct pack *const pack = source->context;
    const struct pack_entry *const entry = pack_find(pack, key);
    if (entry == NULL) {
        return 0;
    }
    if ((size_t)entry->length > cap) {
        return -EMSGSIZE;
    }
    if (!pack_read_at(pack->fd, out, entry->length, entry->offset)) {
        return -EIO;
    }
    return (int)entry->length;
}

static bool pack_has(const struct mesh_map_source *source, struct mesh_map_tile_key key) {
    return source->context != NULL && pack_find(source->context, key) != NULL;
}

static void pack_free(struct pack *pack) {
    if (pack == NULL) {
        return;
    }
    if (pack->fd >= 0) {
        close(pack->fd);
    }
    free(pack->entries);
    free(pack);
}

static void pack_close(struct mesh_map_source *source) {
    pack_free(source->context);
    source->context = NULL;
}

int mesh_map_source_open_pack(const char *path, struct mesh_map_source *out) {
    if (path == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < (off_t)PACK_HEADER_LEN) {
        close(fd);
        return -EINVAL;
    }
    const uint64_t file_len = (uint64_t)info.st_size;

    uint8_t header[PACK_HEADER_LEN];
    if (!pack_read_at(fd, header, sizeof header, 0U) ||
        memcmp(header, PACK_MAGIC, PACK_MAGIC_LEN) != 0) {
        close(fd);
        return -EINVAL;
    }

    const uint16_t tile_size = pack_u16(header + 8);
    const uint8_t format = header[10];
    const uint32_t index_at = pack_u32(header + 12);
    const uint32_t count = pack_u32(header + 16);

    /*
     * The dimensions and the format are restricted rather than believed, which
     * docs/maps-roadmap.md asks for by name. A file that says its tiles are 512 pixels is not a
     * pack this client can draw, and finding that out at the blit means a screen full of
     * quarter-tiles; a file that says its tiles are vectors is the failure MBTiles is famous
     * for, where a raster-only reader opens one happily and draws nothing.
     */
    if (tile_size != MESH_MAP_TILE_SIZE || format != MESH_MAP_TILE_FORMAT_PNG) {
        close(fd);
        return -EINVAL;
    }
    if (count == 0U || count > PACK_TILES_MAX || index_at < PACK_HEADER_LEN) {
        close(fd);
        return -EINVAL;
    }
    const uint64_t index_len = (uint64_t)count * PACK_ENTRY_LEN;
    const uint64_t tiles_at = (uint64_t)index_at + index_len;
    if (tiles_at > file_len) {
        close(fd);
        return -EINVAL;
    }

    struct pack *const pack = calloc(1U, sizeof *pack);
    uint8_t *const raw = malloc((size_t)index_len);
    struct pack_entry *const entries = calloc(count, sizeof *entries);
    if (pack == NULL || raw == NULL || entries == NULL) {
        free(pack);
        free(raw);
        free(entries);
        close(fd);
        return -ENOMEM;
    }
    pack->fd = fd;
    pack->count = count;
    pack->entries = entries;

    if (!pack_read_at(fd, raw, (size_t)index_len, index_at)) {
        free(raw);
        pack_free(pack);
        return -EIO;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        const uint8_t *const at = raw + (size_t)i * PACK_ENTRY_LEN;
        entries[i] = (struct pack_entry){
            .zoom = at[0],
            .x = pack_u32(at + 4),
            .y = pack_u32(at + 8),
            .length = pack_u32(at + 12),
            .offset = pack_u64(at + 16),
        };
    }
    free(raw);

    if (!pack_check(pack, file_len, tiles_at)) {
        pack_free(pack);
        return -EINVAL;
    }

    out->info.tile_size = tile_size;
    out->info.format = format;
    out->info.tiles = count;
    /* Sorted by zoom first, so the range is the two ends of the index and needs no scan. */
    out->info.min_zoom = entries[0].zoom;
    out->info.max_zoom = entries[count - 1U].zoom;
    out->info.generated_s = (int64_t)pack_u64(header + 24);
    pack_text(out->info.attribution, sizeof out->info.attribution, header + 32);
    pack_text(out->info.name, sizeof out->info.name, header + 96);
    pack_coverage(pack, &out->info);

    out->read = pack_read;
    out->has = pack_has;
    out->close = pack_close;
    out->context = pack;
    mesh_log_info("map", "Opened tile pack %s: %u tiles, zoom %u-%u", path, count,
                  (unsigned)out->info.min_zoom, (unsigned)out->info.max_zoom);
    return 0;
}

int mesh_map_source_read(struct mesh_map_source *source, struct mesh_map_tile_key key, uint8_t *out,
                         size_t cap) {
    if (source == NULL || source->read == NULL || out == NULL || cap == 0U) {
        return -EINVAL;
    }
    if (!mesh_map_tile_key_valid(key)) {
        return 0;
    }
    return source->read(source, key, out, cap);
}

void mesh_map_source_close(struct mesh_map_source *source) {
    if (source == NULL) {
        return;
    }
    if (source->close != NULL) {
        source->close(source);
    }
    memset(source, 0, sizeof *source);
}

bool mesh_map_source_has(const struct mesh_map_source *source, struct mesh_map_tile_key key) {
    if (source == NULL || source->has == NULL || !mesh_map_tile_key_valid(key)) {
        return false;
    }
    return source->has(source, key);
}
