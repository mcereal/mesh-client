/*
 * tile_bench - what one raster map tile costs on the device: fetch, decode, blit.
 *
 * Step 3 of docs/maps-roadmap.md asks one question before anything else is built: can the Brick
 * read a tile off its storage and decode it fast enough not to stall the event loop? This answers
 * it for three pack layouts (a z/x/y directory tree, an MBTiles file, and a single-file indexed
 * pack shaped like PMTiles) and two PNG decoders (stb_image and Wuffs), with the page cache warm,
 * dropped once, or dropped before every tile. It is not part of the client: build.sh cross-builds
 * it on its own and gen_tiles.py writes what it reads.
 *
 *   tile_bench -d SET -l xyz|pack|mbtiles -D none|stb|wuffs -m warm|cold-once|cold-each [-n N]
 *   tile_bench -d SET -l LAYOUT -D DECODER -m view [-r R]      a full view, then one pan
 *   tile_bench -d SET -l LAYOUT -m verify [-n N]               stb and Wuffs agree, pixel for pixel
 *
 * A decode always ends in a 256x256 BGRX tile, which is what the framebuffer wants. Wuffs writes
 * that directly; stb_image can only write RGBA, so its swizzle is counted as part of its decode -
 * it is a cost a client using it would pay. The blit is a clipped row copy into a 1024x768 frame
 * in RAM, which is where the fb backend draws before its row-span copy to /dev/fb0.
 */
#define _GNU_SOURCE
#include "sqlite3.h"
#include "stb_image.h"
#include "tb_wuffs.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TILE_PX 256
#define TILE_PIXELS (TILE_PX * TILE_PX)
#define TILE_BYTES_MAX (1U << 20)
#define FRAME_W 1024
#define FRAME_H 768
#define VIEW_COLS 5
#define VIEW_ROWS 4

enum layout { LAYOUT_XYZ, LAYOUT_PACK, LAYOUT_MBTILES };
enum decoder { DECODER_NONE, DECODER_STB, DECODER_WUFFS };
enum mode { MODE_WARM, MODE_COLD_ONCE, MODE_COLD_EACH, MODE_VIEW, MODE_VERIFY };

static const char *const k_layouts[] = {"xyz", "pack", "mbtiles"};
static const char *const k_decoders[] = {"none", "stb", "wuffs"};
static const char *const k_modes[] = {"warm", "cold-once", "cold-each", "view", "verify"};

struct tile_key {
    uint32_t z, x, y;
};

struct pack_entry {
    uint32_t z, x, y, length;
    uint64_t offset;
};

struct manifest {
    unsigned zoom, x0, y0, width, height, minzoom;
};

struct source {
    enum layout layout;
    const char *dir;
    int fd;
    struct pack_entry *entries;
    uint32_t count;
    sqlite3 *db;
    sqlite3_stmt *stmt;
};

static wuffs_png__decoder *g_wuffs;
static uint8_t *g_workbuf;
static size_t g_workbuf_len;
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static int pick(const char *const *names, int count, const char *arg) {
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], arg) == 0) {
            return i;
        }
    }
    fprintf(stderr, "unknown value: %s\n", arg);
    exit(2);
}

static void sleep_ms(unsigned ms) {
    struct timespec ts = {.tv_sec = ms / 1000U, .tv_nsec = (long)(ms % 1000U) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* Root, and a sync first: drop_caches only drops clean pages. The card is mounted `sync`, so
 * there is nothing dirty to wait for in practice. */
static void drop_caches(void) {
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, "3\n", 2) != 2) {
        perror("drop_caches (cold modes need root)");
        exit(1);
    }
    close(fd);
}

static void read_sysfs(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(out, cap, "?");
        return;
    }
    if (fgets(out, (int)cap, f) == NULL) {
        snprintf(out, cap, "?");
    }
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
}

static bool read_manifest(const char *dir, struct manifest *m) {
    char path[1024];
    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return false;
    }
    int got = fscanf(f, "zoom=%u x0=%u y0=%u width=%u height=%u minzoom=%u", &m->zoom, &m->x0,
                     &m->y0, &m->width, &m->height, &m->minzoom);
    fclose(f);
    return got == 6;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int compare_entry(const void *a, const void *b) {
    const struct pack_entry *l = a;
    const struct pack_entry *r = b;
    if (l->z != r->z) {
        return l->z < r->z ? -1 : 1;
    }
    if (l->x != r->x) {
        return l->x < r->x ? -1 : 1;
    }
    if (l->y != r->y) {
        return l->y < r->y ? -1 : 1;
    }
    return 0;
}

static bool read_full(int fd, uint8_t *buf, size_t len, off_t offset) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, buf + got, len - got, offset + (off_t)got);
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

/* The pack's index is read whole at open, the way a PMTiles reader holds its root directory:
 * a lookup is then a binary search in RAM and a tile is one pread. */
static bool source_open(struct source *s, enum layout layout, const char *dir) {
    memset(s, 0, sizeof *s);
    s->layout = layout;
    s->dir = dir;
    s->fd = -1;
    char path[1024];
    if (layout == LAYOUT_PACK) {
        snprintf(path, sizeof path, "%s/pack.mctp", dir);
        s->fd = open(path, O_RDONLY | O_CLOEXEC);
        uint8_t header[16];
        if (s->fd < 0 || !read_full(s->fd, header, sizeof header, 0) ||
            memcmp(header, "MCTPACK1", 8) != 0) {
            fprintf(stderr, "%s: not a tile pack\n", path);
            return false;
        }
        s->count = le32(header + 8);
        size_t index_len = (size_t)s->count * 24U;
        uint8_t *raw = malloc(index_len);
        s->entries = calloc(s->count, sizeof *s->entries);
        if (raw == NULL || s->entries == NULL || !read_full(s->fd, raw, index_len, 16)) {
            free(raw);
            return false;
        }
        for (uint32_t i = 0; i < s->count; i++) {
            const uint8_t *e = raw + (size_t)i * 24U;
            s->entries[i] = (struct pack_entry){
                .z = e[0],
                .x = le32(e + 4),
                .y = le32(e + 8),
                .length = le32(e + 12),
                .offset = (uint64_t)le32(e + 16) | (uint64_t)le32(e + 20) << 32,
            };
        }
        free(raw);
    } else if (layout == LAYOUT_MBTILES) {
        snprintf(path, sizeof path, "%s/tiles.mbtiles", dir);
        if (sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(s->db,
                               "SELECT tile_data FROM tiles WHERE zoom_level=?1 AND "
                               "tile_column=?2 AND tile_row=?3",
                               -1, &s->stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "%s: %s\n", path, s->db ? sqlite3_errmsg(s->db) : "open failed");
            return false;
        }
    } else {
        struct stat st;
        snprintf(path, sizeof path, "%s/xyz", dir);
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s: no tile tree\n", path);
            return false;
        }
    }
    return true;
}

static void source_close(struct source *s) {
    if (s->fd >= 0) {
        close(s->fd);
    }
    free(s->entries);
    sqlite3_finalize(s->stmt);
    sqlite3_close(s->db);
}

static long source_fetch(struct source *s, struct tile_key k, uint8_t *buf, size_t cap) {
    if (s->layout == LAYOUT_PACK) {
        struct pack_entry want = {.z = k.z, .x = k.x, .y = k.y};
        const struct pack_entry *e =
            bsearch(&want, s->entries, s->count, sizeof *s->entries, compare_entry);
        if (e == NULL || e->length > cap || !read_full(s->fd, buf, e->length, (off_t)e->offset)) {
            return -1;
        }
        return (long)e->length;
    }
    if (s->layout == LAYOUT_MBTILES) {
        sqlite3_reset(s->stmt);
        sqlite3_bind_int(s->stmt, 1, (int)k.z);
        sqlite3_bind_int(s->stmt, 2, (int)k.x);
        sqlite3_bind_int(s->stmt, 3, (int)((1U << k.z) - 1U - k.y));
        if (sqlite3_step(s->stmt) != SQLITE_ROW) {
            return -1;
        }
        int n = sqlite3_column_bytes(s->stmt, 0);
        const void *blob = sqlite3_column_blob(s->stmt, 0);
        if (blob == NULL || n <= 0 || (size_t)n > cap) {
            return -1;
        }
        memcpy(buf, blob, (size_t)n);
        return n;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/xyz/%u/%u/%u.png", s->dir, k.z, k.x, k.y);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    size_t got = 0;
    for (;;) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 || (n > 0 && got + (size_t)n == cap)) {
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    return (long)got;
}

static bool decode_stb(const uint8_t *src, size_t len, uint32_t *dst) {
    int w = 0, h = 0, n = 0;
    unsigned char *rgba = stbi_load_from_memory(src, (int)len, &w, &h, &n, 4);
    if (rgba == NULL) {
        return false;
    }
    bool ok = w == TILE_PX && h == TILE_PX;
    for (size_t i = 0; ok && i < TILE_PIXELS; i++) {
        const unsigned char *p = rgba + i * 4U;
        dst[i] = 0xFF000000U | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | (uint32_t)p[2];
    }
    stbi_image_free(rgba);
    return ok;
}

/* One decoder for the life of the process, reinitialised per tile, and a work buffer that only
 * ever grows: Wuffs allocates nothing itself, so this is the whole of its memory. */
static bool decode_wuffs(const uint8_t *src, size_t len, uint32_t *dst) {
    wuffs_base__status st =
        wuffs_png__decoder__initialize(g_wuffs, sizeof__wuffs_png__decoder(), WUFFS_VERSION,
                                       WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&st)) {
        return false;
    }
    wuffs_base__io_buffer io = wuffs_base__ptr_u8__reader((uint8_t *)src, len, true);
    wuffs_base__image_config ic;
    st = wuffs_png__decoder__decode_image_config(g_wuffs, &ic, &io);
    if (!wuffs_base__status__is_ok(&st) || wuffs_base__pixel_config__width(&ic.pixcfg) != TILE_PX ||
        wuffs_base__pixel_config__height(&ic.pixcfg) != TILE_PX) {
        return false;
    }
    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, TILE_PX, TILE_PX);
    wuffs_base__pixel_buffer pb;
    st = wuffs_base__pixel_buffer__set_from_slice(
        &pb, &ic.pixcfg, wuffs_base__make_slice_u8((uint8_t *)dst, TILE_PIXELS * 4U));
    if (!wuffs_base__status__is_ok(&st)) {
        return false;
    }
    uint64_t need = wuffs_png__decoder__workbuf_len(g_wuffs).max_incl;
    if (need > g_workbuf_len) {
        uint8_t *grown = realloc(g_workbuf, (size_t)need);
        if (grown == NULL) {
            return false;
        }
        g_workbuf = grown;
        g_workbuf_len = (size_t)need;
    }
    st = wuffs_png__decoder__decode_frame(g_wuffs, &pb, &io, WUFFS_BASE__PIXEL_BLEND__SRC,
                                          wuffs_base__make_slice_u8(g_workbuf, (size_t)need), NULL);
    if (!wuffs_base__status__is_ok(&st)) {
        return false;
    }
    for (size_t i = 0; i < TILE_PIXELS; i++) {
        dst[i] |= 0xFF000000U;
    }
    return true;
}

static bool decode(enum decoder d, const uint8_t *src, size_t len, uint32_t *dst) {
    switch (d) {
    case DECODER_STB:
        return decode_stb(src, len, dst);
    case DECODER_WUFFS:
        return decode_wuffs(src, len, dst);
    case DECODER_NONE:
    default:
        return true;
    }
}

static void blit(uint32_t *frame, const uint32_t *tile, int ox, int oy) {
    int x0 = ox < 0 ? -ox : 0;
    int y0 = oy < 0 ? -oy : 0;
    int x1 = ox + TILE_PX > FRAME_W ? FRAME_W - ox : TILE_PX;
    int y1 = oy + TILE_PX > FRAME_H ? FRAME_H - oy : TILE_PX;
    for (int y = y0; y < y1 && x1 > x0; y++) {
        memcpy(frame + (size_t)(oy + y) * FRAME_W + ox + x0, tile + (size_t)y * TILE_PX + x0,
               (size_t)(x1 - x0) * 4U);
    }
}

static int compare_u64(const void *a, const void *b) {
    uint64_t l = *(const uint64_t *)a;
    uint64_t r = *(const uint64_t *)b;
    return l < r ? -1 : l > r;
}

struct stats {
    double p50, p90, p99, max, mean;
};

static struct stats summarize(uint64_t *v, size_t n) {
    struct stats s = {0};
    if (n == 0) {
        return s;
    }
    qsort(v, n, sizeof *v, compare_u64);
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)v[i];
    }
    s.p50 = (double)v[n / 2] / 1e6;
    s.p90 = (double)v[(n * 9) / 10] / 1e6;
    s.p99 = (double)v[(n * 99) / 100] / 1e6;
    s.max = (double)v[n - 1] / 1e6;
    s.mean = sum / (double)n / 1e6;
    return s;
}

static void print_stats(const char *phase, struct stats s) {
    printf("  %-8s p50 %8.3f  p90 %8.3f  p99 %8.3f  max %8.3f  mean %8.3f  ms\n", phase, s.p50,
           s.p90, s.p99, s.max, s.mean);
}

static void result_field(const char *phase, struct stats s) {
    printf(" %s_p50=%.3f %s_p90=%.3f %s_p99=%.3f %s_max=%.3f %s_mean=%.3f", phase, s.p50, phase,
           s.p90, phase, s.p99, phase, s.max, phase, s.mean);
}

/* Every tile at the manifest's top zoom, in a seeded random order: neighbours on the ground are
 * not neighbours in the read order, so readahead cannot flatter a layout. */
static struct tile_key *tile_order(const struct manifest *m, size_t *count) {
    size_t n = (size_t)m->width * m->height;
    struct tile_key *keys = malloc(n * sizeof *keys);
    if (keys == NULL) {
        exit(1);
    }
    for (size_t i = 0; i < n; i++) {
        keys[i] = (struct tile_key){m->zoom, m->x0 + (uint32_t)(i % m->width),
                                    m->y0 + (uint32_t)(i / m->width)};
    }
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rng_next() % (i + 1);
        struct tile_key t = keys[i];
        keys[i] = keys[j];
        keys[j] = t;
    }
    *count = n;
    return keys;
}

static int run_tiles(struct source *src, enum decoder d, enum mode mode, const struct manifest *m,
                     size_t n, unsigned gap_ms, uint8_t *buf, uint32_t *tile, uint32_t *frame) {
    size_t total = 0;
    struct tile_key *keys = tile_order(m, &total);
    if (n > total) {
        n = total;
    }
    uint64_t *fetch = calloc(n, sizeof *fetch);
    uint64_t *dec = calloc(n, sizeof *dec);
    uint64_t *copy = calloc(n, sizeof *copy);
    uint64_t *all = calloc(n, sizeof *all);
    uint64_t bytes = 0;
    if (mode == MODE_WARM) {
        for (size_t i = 0; i < n; i++) {
            (void)source_fetch(src, keys[i], buf, TILE_BYTES_MAX);
        }
    } else if (mode == MODE_COLD_ONCE) {
        drop_caches();
    }
    for (size_t i = 0; i < n; i++) {
        if (gap_ms > 0) {
            sleep_ms(gap_ms);
        }
        if (mode == MODE_COLD_EACH) {
            drop_caches();
        }
        uint64_t t0 = now_ns();
        long len = source_fetch(src, keys[i], buf, TILE_BYTES_MAX);
        uint64_t t1 = now_ns();
        if (len < 0 || !decode(d, buf, (size_t)len, tile)) {
            fprintf(stderr, "tile %u/%u/%u failed\n", keys[i].z, keys[i].x, keys[i].y);
            return 1;
        }
        uint64_t t2 = now_ns();
        blit(frame, tile, (int)(rng_next() % FRAME_W) - 128, (int)(rng_next() % FRAME_H) - 128);
        uint64_t t3 = now_ns();
        fetch[i] = t1 - t0;
        dec[i] = t2 - t1;
        copy[i] = t3 - t2;
        all[i] = t3 - t0;
        bytes += (uint64_t)len;
    }
    struct stats sf = summarize(fetch, n), sd = summarize(dec, n), sc = summarize(copy, n),
                 sa = summarize(all, n);
    printf("%zu tiles, mean %.1f KiB\n", n, (double)bytes / (double)n / 1024.0);
    print_stats("fetch", sf);
    print_stats("decode", sd);
    print_stats("blit", sc);
    print_stats("tile", sa);
    printf("RESULT layout=%s decoder=%s mode=%s gap_ms=%u n=%zu kib=%.1f", k_layouts[src->layout],
           k_decoders[d], k_modes[mode], gap_ms, n, (double)bytes / (double)n / 1024.0);
    result_field("fetch", sf);
    result_field("decode", sd);
    result_field("blit", sc);
    result_field("tile", sa);
    printf("\n");
    free(keys);
    free(fetch);
    free(dec);
    free(copy);
    free(all);
    return 0;
}

/* A whole body's worth from cold: VIEW_COLS x VIEW_ROWS tiles at an arbitrary pixel offset,
 * which is the most a 1024x768 map can touch. Then one pan of a tile's width: the new column is
 * read and decoded, and the whole body is redrawn from the tiles already decoded - which is the
 * decoded-tile cache the roadmap proposes, at its smallest. */
static int run_view(struct source *src, enum decoder d, const struct manifest *m, size_t rounds,
                    uint8_t *buf, uint32_t *frame) {
    uint32_t *tiles = malloc((size_t)VIEW_ROWS * (VIEW_COLS + 1) * TILE_PIXELS * 4U);
    uint64_t *view = calloc(rounds, sizeof *view);
    uint64_t *pan = calloc(rounds, sizeof *pan);
    if (tiles == NULL || view == NULL || pan == NULL || m->width < VIEW_COLS + 1 ||
        m->height < VIEW_ROWS) {
        return 1;
    }
    for (size_t r = 0; r < rounds; r++) {
        uint32_t tx = m->x0 + rng_next() % (m->width - VIEW_COLS);
        uint32_t ty = m->y0 + rng_next() % (m->height - VIEW_ROWS + 1);
        int offx = (int)(rng_next() % TILE_PX), offy = (int)(rng_next() % TILE_PX);
        drop_caches();
        uint64_t t0 = now_ns();
        for (int row = 0; row < VIEW_ROWS; row++) {
            for (int col = 0; col < VIEW_COLS; col++) {
                uint32_t *t = tiles + ((size_t)row * (VIEW_COLS + 1) + (size_t)col) * TILE_PIXELS;
                struct tile_key k = {m->zoom, tx + (uint32_t)col, ty + (uint32_t)row};
                long len = source_fetch(src, k, buf, TILE_BYTES_MAX);
                if (len < 0 || !decode(d, buf, (size_t)len, t)) {
                    return 1;
                }
                blit(frame, t, col * TILE_PX - offx, row * TILE_PX - offy);
            }
        }
        uint64_t t1 = now_ns();
        for (int row = 0; row < VIEW_ROWS; row++) {
            uint32_t *t = tiles + ((size_t)row * (VIEW_COLS + 1) + VIEW_COLS) * TILE_PIXELS;
            struct tile_key k = {m->zoom, tx + VIEW_COLS, ty + (uint32_t)row};
            long len = source_fetch(src, k, buf, TILE_BYTES_MAX);
            if (len < 0 || !decode(d, buf, (size_t)len, t)) {
                return 1;
            }
        }
        for (int row = 0; row < VIEW_ROWS; row++) {
            for (int col = 1; col <= VIEW_COLS; col++) {
                const uint32_t *t =
                    tiles + ((size_t)row * (VIEW_COLS + 1) + (size_t)col) * TILE_PIXELS;
                blit(frame, t, (col - 1) * TILE_PX - offx, row * TILE_PX - offy);
            }
        }
        uint64_t t2 = now_ns();
        view[r] = t1 - t0;
        pan[r] = t2 - t1;
    }
    struct stats sv = summarize(view, rounds), sp = summarize(pan, rounds);
    printf("%zu rounds of a %dx%d view from cold, then one pan\n", rounds, VIEW_COLS, VIEW_ROWS);
    print_stats("view", sv);
    print_stats("pan", sp);
    printf("RESULT layout=%s decoder=%s mode=view n=%zu", k_layouts[src->layout], k_decoders[d],
           rounds);
    result_field("view", sv);
    result_field("pan", sp);
    printf("\n");
    free(tiles);
    free(view);
    free(pan);
    return 0;
}

static int run_verify(struct source *src, const struct manifest *m, size_t n, uint8_t *buf) {
    size_t total = 0;
    struct tile_key *keys = tile_order(m, &total);
    uint32_t *a = malloc(TILE_PIXELS * 4U);
    uint32_t *b = malloc(TILE_PIXELS * 4U);
    size_t bad = 0;
    if (n > total) {
        n = total;
    }
    for (size_t i = 0; i < n; i++) {
        long len = source_fetch(src, keys[i], buf, TILE_BYTES_MAX);
        if (len < 0 || !decode_stb(buf, (size_t)len, a) || !decode_wuffs(buf, (size_t)len, b) ||
            memcmp(a, b, TILE_PIXELS * 4U) != 0) {
            bad++;
        }
    }
    printf("verify: %zu tiles, %zu disagree\nRESULT mode=verify layout=%s n=%zu bad=%zu\n", n, bad,
           k_layouts[src->layout], n, bad);
    free(keys);
    free(a);
    free(b);
    return bad != 0;
}

int main(int argc, char **argv) {
    const char *dir = NULL;
    enum layout layout = LAYOUT_PACK;
    enum decoder d = DECODER_WUFFS;
    enum mode mode = MODE_COLD_ONCE;
    size_t n = 200;
    size_t rounds = 20;
    unsigned gap_ms = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d:l:D:m:n:r:g:s:")) != -1) {
        switch (opt) {
        case 'd':
            dir = optarg;
            break;
        case 'l':
            layout = (enum layout)pick(k_layouts, 3, optarg);
            break;
        case 'D':
            d = (enum decoder)pick(k_decoders, 3, optarg);
            break;
        case 'm':
            mode = (enum mode)pick(k_modes, 5, optarg);
            break;
        case 'n':
            n = strtoul(optarg, NULL, 10);
            break;
        case 'r':
            rounds = strtoul(optarg, NULL, 10);
            break;
        case 'g':
            gap_ms = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 's':
            g_rng ^= strtoull(optarg, NULL, 10) * 0x2545F4914F6CDD1DULL;
            break;
        default:
            fprintf(stderr, "usage: see the comment at the top of tile_bench.c\n");
            return 2;
        }
    }
    struct manifest m;
    struct source src;
    if (dir == NULL || !read_manifest(dir, &m) || !source_open(&src, layout, dir)) {
        return 2;
    }
    g_wuffs = wuffs_png__decoder__alloc();
    uint8_t *buf = malloc(TILE_BYTES_MAX);
    uint32_t *tile = malloc(TILE_PIXELS * 4U);
    uint32_t *frame = calloc((size_t)FRAME_W * FRAME_H, 4U);
    if (g_wuffs == NULL || buf == NULL || tile == NULL || frame == NULL) {
        return 1;
    }
    char governor[64], freq[64];
    read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", governor, sizeof governor);
    read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", freq, sizeof freq);
    printf("== %s layout=%s decoder=%s mode=%s governor=%s cur_khz=%s\n", dir, k_layouts[layout],
           k_decoders[d], k_modes[mode], governor, freq);

    int rc;
    if (mode == MODE_VIEW) {
        rc = run_view(&src, d, &m, rounds, buf, frame);
    } else if (mode == MODE_VERIFY) {
        rc = run_verify(&src, &m, n, buf);
    } else {
        rc = run_tiles(&src, d, mode, &m, n, gap_ms, buf, tile, frame);
    }

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("MEMORY maxrss_kib=%ld wuffs_decoder_bytes=%zu wuffs_workbuf_bytes=%zu\n", ru.ru_maxrss,
           sizeof__wuffs_png__decoder(), g_workbuf_len);
    source_close(&src);
    free(g_wuffs);
    free(g_workbuf);
    free(buf);
    free(tile);
    free(frame);
    return rc;
}
