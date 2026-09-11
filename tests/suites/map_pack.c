#define _POSIX_C_SOURCE 200809L

/*
 * The tile pack: a header, an index, and the refusals that keep a tile read from having to
 * check anything.
 *
 * The reader's whole argument is that everything is validated once at open, so every case here
 * is really one question - does a pack that is wrong in this particular way get refused at the
 * door, rather than at the blit on a device with nobody watching. The fixtures are built in
 * memory and written to a temporary file, because the thing under test is a file format and a
 * pread; the "tiles" are arbitrary bytes on purpose, since the reader hands encoded bytes up
 * without looking at them and validating a PNG signature at open would mean reading every tile
 * in the pack to open it.
 */

#include "framework/mesh_test.h"

#include "mesh/map/source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PACK_HEADER_LEN 128U
#define PACK_ENTRY_LEN 24U
#define PACK_TILE_LEN 32U

/* Somewhere in the Salish Sea, at the zooms a regional pack is cut to. Three levels, because
   the reader derives its zoom range from the two ends of a sorted index and one level cannot
   tell a range from a constant. */
#define PACK_FIXTURE_TILES 6U

struct pack_tile {
    uint8_t zoom;
    uint32_t x;
    uint32_t y;
};

static const struct pack_tile k_fixture[PACK_FIXTURE_TILES] = {
    {12, 655, 1416},  {13, 1310, 2832}, {13, 1311, 2833},
    {14, 2620, 5665}, {14, 2621, 5666}, {14, 2621, 5667},
};

/* What a fixture is allowed to be wrong about. Each one is a real corruption - a truncated
   copy, a builder with a bug, a card that lost a sector - rather than a hypothetical. */
struct pack_fixture {
    const char *magic;
    uint16_t tile_size;
    uint8_t format;
    bool empty;              /* a header that declares no tiles at all */
    uint32_t count_override; /* 0 keeps the real count */
    uint32_t index_at_override;
    bool unsorted;
    bool tile_off_the_world;
    bool extent_past_the_end;
    bool truncate;
    int64_t generated_s;
    const char *attribution;
    const char *name;
};

static void pack_put_u16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value & 0xFFU);
    at[1] = (uint8_t)(value >> 8);
}

static void pack_put_u32(uint8_t *at, uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) {
        at[i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
    }
}

static void pack_put_u64(uint8_t *at, uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i) {
        at[i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
    }
}

/* The nth tile's bytes: a payload that depends on which tile it is, so a read that returns the
   wrong entry's extent is caught by the content rather than only by the length. */
static void pack_fill_tile(uint8_t *out, size_t index) {
    for (size_t i = 0U; i < PACK_TILE_LEN; ++i) {
        out[i] = (uint8_t)(index * 17U + i);
    }
}

/*
 * Writes a fixture pack and hands back its path in `path`, which the caller unlinks.
 *
 * Returns the number of bytes written, or 0 when the file could not be made - which is a
 * fixture failure rather than a finding, so the cases below report it as one.
 */
static size_t pack_write(struct pack_fixture spec, char *path, size_t path_len) {
    const uint32_t count = spec.empty                  ? 0U
                           : spec.count_override != 0U ? spec.count_override
                                                       : (uint32_t)PACK_FIXTURE_TILES;
    const uint32_t index_at =
        spec.index_at_override != 0U ? spec.index_at_override : (uint32_t)PACK_HEADER_LEN;
    /* The *file* is always laid out for the tiles that exist, while the header declares
       `count` - so an override is a header claiming more index than the file holds, which is
       what a truncated copy looks like and the thing a reader must not walk off the end of. */
    const size_t held = spec.empty ? 0U : (size_t)PACK_FIXTURE_TILES;
    const size_t tiles_at = (size_t)index_at + held * PACK_ENTRY_LEN;
    const size_t total = tiles_at + held * PACK_TILE_LEN;

    uint8_t *const bytes = calloc(1U, total);
    if (bytes == NULL) {
        return 0U;
    }
    memcpy(bytes, spec.magic != NULL ? spec.magic : "MCTPACK2", 8U);
    pack_put_u16(bytes + 8, spec.tile_size != 0U ? spec.tile_size : 256U);
    bytes[10] = spec.format != 0U ? spec.format : (uint8_t)MESH_MAP_TILE_FORMAT_PNG;
    pack_put_u32(bytes + 12, index_at);
    pack_put_u32(bytes + 16, count);
    pack_put_u64(bytes + 24, (uint64_t)spec.generated_s);
    if (spec.attribution != NULL) {
        memcpy(bytes + 32, spec.attribution, strlen(spec.attribution));
    }
    if (spec.name != NULL) {
        memcpy(bytes + 96, spec.name, strlen(spec.name));
    }

    for (size_t i = 0U; i < held; ++i) {
        /* The two entries the `unsorted` fixture swaps are adjacent at the same zoom, which is
           the order a binary search actually depends on - a pack sorted by zoom alone looks
           fine until the search walks past the tile it wanted. */
        size_t which = i;
        if (spec.unsorted && i == 1U) {
            which = 2U;
        } else if (spec.unsorted && i == 2U) {
            which = 1U;
        }
        uint8_t *const entry = bytes + index_at + i * PACK_ENTRY_LEN;
        entry[0] = k_fixture[which].zoom;
        pack_put_u32(entry + 4, k_fixture[which].x);
        pack_put_u32(entry + 8, spec.tile_off_the_world && i == 0U
                                    ? (uint32_t)1 << k_fixture[which].zoom
                                    : k_fixture[which].y);
        pack_put_u32(entry + 12, PACK_TILE_LEN);
        pack_put_u64(entry + 16, spec.extent_past_the_end && i == 0U
                                     ? (uint64_t)total - 1U
                                     : (uint64_t)(tiles_at + which * PACK_TILE_LEN));
        pack_fill_tile(bytes + tiles_at + which * PACK_TILE_LEN, which);
    }

    snprintf(path, path_len, "/tmp/meshclient_pack_XXXXXX");
    const int fd = mkstemp(path);
    if (fd < 0) {
        free(bytes);
        return 0U;
    }
    const size_t written = spec.truncate ? tiles_at - 1U : total;
    const ssize_t n = write(fd, bytes, written);
    close(fd);
    free(bytes);
    return n == (ssize_t)written ? written : 0U;
}

/* A pack every case starts from: nothing wrong with it. */
static struct pack_fixture pack_good(void) {
    return (struct pack_fixture){
        .generated_s = 1757500000,
        .attribution = "(c) OpenStreetMap contributors",
        .name = "Salish Sea",
    };
}

MESH_TEST_CASE(map_pack_opens_and_says_what_it_holds, unit) {
    char path[64];
    MESH_TEST_FAIL_IF(pack_write(pack_good(), path, sizeof path) == 0U, "fixture pack written");

    struct mesh_map_source source;
    const int opened = mesh_map_source_open_pack(path, &source);
    unlink(path);
    MESH_TEST_FAIL_IF(opened != 0, "a good pack opens");

    MESH_TEST_FAIL_IF_CLEANUP(source.info.tiles != PACK_FIXTURE_TILES,
                              mesh_map_source_close(&source), "it counts its tiles");
    MESH_TEST_FAIL_IF_CLEANUP(source.info.tile_size != 256 ||
                                  source.info.format != MESH_MAP_TILE_FORMAT_PNG,
                              mesh_map_source_close(&source), "and states its shape");
    /* Derived from the two ends of the index, which is the whole reason the index is sorted by
       zoom before anything else. */
    MESH_TEST_FAIL_IF_CLEANUP(source.info.min_zoom != 12 || source.info.max_zoom != 14,
                              mesh_map_source_close(&source), "the zoom range is the index's");
    MESH_TEST_FAIL_IF_CLEANUP(strcmp(source.info.attribution, "(c) OpenStreetMap contributors") !=
                                  0,
                              mesh_map_source_close(&source), "the attribution survives");
    MESH_TEST_FAIL_IF_CLEANUP(strcmp(source.info.name, "Salish Sea") != 0,
                              mesh_map_source_close(&source), "and so does the name");
    MESH_TEST_FAIL_IF_CLEANUP(source.info.generated_s != 1757500000, mesh_map_source_close(&source),
                              "and the date it was cut");
    mesh_map_source_close(&source);
    record_success(test_name);
}

/*
 * The coverage is the union of the tiles' own corners, and it is derived rather than declared.
 *
 * The union is taken across zooms, which is the part worth a case: the fixture's z12 tile is
 * the widest and sets three of the four edges, and its *southern* edge is a whole z12 tile below
 * where the z14 tiles stop - so a reader that measured coverage at one level, or at the deepest
 * one, would get a different box from this. A header field claiming any of it is exactly the bug
 * this refuses to be able to have.
 */
MESH_TEST_CASE(map_pack_coverage_comes_from_the_tiles, unit) {
    char path[64];
    MESH_TEST_FAIL_IF(pack_write(pack_good(), path, sizeof path) == 0U, "fixture pack written");

    struct mesh_map_source source;
    const int opened = mesh_map_source_open_pack(path, &source);
    unlink(path);
    MESH_TEST_FAIL_IF(opened != 0, "the pack opens");

    const int32_t slack = 2000; /* two ten-thousandths of a degree, about 20 metres */
    const bool west =
        source.info.west_i > -1224316406 - slack && source.info.west_i < -1224316406 + slack;
    const bool east =
        source.info.east_i > -1223437500 - slack && source.info.east_i < -1223437500 + slack;
    const bool north =
        source.info.north_i > 484583519 - slack && source.info.north_i < 484583519 + slack;
    const bool south =
        source.info.south_i > 484000325 - slack && source.info.south_i < 484000325 + slack;
    MESH_TEST_FAIL_IF_CLEANUP(!west || !east, mesh_map_source_close(&source),
                              "the coverage spans the tiles east to west");
    MESH_TEST_FAIL_IF_CLEANUP(!north || !south, mesh_map_source_close(&source),
                              "and north to south");
    MESH_TEST_FAIL_IF_CLEANUP(source.info.west_i >= source.info.east_i ||
                                  source.info.south_i >= source.info.north_i,
                              mesh_map_source_close(&source), "and is the right way round");
    mesh_map_source_close(&source);
    record_success(test_name);
}

MESH_TEST_CASE(map_pack_reads_the_tile_it_was_asked_for, unit) {
    char path[64];
    MESH_TEST_FAIL_IF(pack_write(pack_good(), path, sizeof path) == 0U, "fixture pack written");

    struct mesh_map_source source;
    const int opened = mesh_map_source_open_pack(path, &source);
    unlink(path);
    MESH_TEST_FAIL_IF(opened != 0, "the pack opens");

    uint8_t want[PACK_TILE_LEN];
    uint8_t got[PACK_TILE_LEN];
    for (size_t i = 0U; i < PACK_FIXTURE_TILES; ++i) {
        const struct mesh_map_tile_key key = {
            .zoom = k_fixture[i].zoom, .x = k_fixture[i].x, .y = k_fixture[i].y};
        const int length = mesh_map_source_read(&source, key, got, sizeof got);
        MESH_TEST_FAIL_IF_CLEANUP(length != (int)PACK_TILE_LEN, mesh_map_source_close(&source),
                                  "every tile in the index reads back whole");
        pack_fill_tile(want, i);
        MESH_TEST_FAIL_IF_CLEANUP(memcmp(want, got, sizeof want) != 0,
                                  mesh_map_source_close(&source),
                                  "and reads back the bytes that belong to it");
        MESH_TEST_FAIL_IF_CLEANUP(!mesh_map_source_has(&source, key),
                                  mesh_map_source_close(&source),
                                  "and is one the pack admits to holding");
    }

    /* A neighbour the pack does not hold. Not a failure: every pack is a rectangle of the world
       with holes in it, and a hole is a state the map draws rather than an error it reports. */
    const struct mesh_map_tile_key absent = {.zoom = 14, .x = 2622, .y = 5666};
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_source_read(&source, absent, got, sizeof got) != 0,
                              mesh_map_source_close(&source), "a missing tile is zero, not error");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_source_has(&source, absent), mesh_map_source_close(&source),
                              "and the pack says so without touching the card");

    /* A key that could not name a tile at any zoom gets the same answer as a hole rather than a
       read: x 4096 does not exist at zoom 12, so there is nothing to look for. */
    const struct mesh_map_tile_key impossible = {.zoom = 12, .x = 4096, .y = 1416};
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_source_read(&source, impossible, got, sizeof got) != 0,
                              mesh_map_source_close(&source), "a key off the world is a miss");

    /* Refused rather than truncated: half a PNG decodes into a picture of something. */
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_map_source_read(&source, (struct mesh_map_tile_key){.zoom = 12, .x = 655, .y = 1416},
                             got, PACK_TILE_LEN - 1U) != -EMSGSIZE,
        mesh_map_source_close(&source), "a short buffer is refused");
    mesh_map_source_close(&source);
    record_success(test_name);
}

/*
 * Every way a pack can be wrong that the reader would otherwise have to re-check on each read.
 *
 * One case rather than eight, because they are one claim: open() is where a pack stops being
 * something anyone has to be careful with. A refusal that moved to the read path would be a
 * check the event loop pays for, every frame, about a file that has not changed since it was
 * opened.
 */
MESH_TEST_CASE(map_pack_refuses_what_it_cannot_trust, unit) {
    struct {
        const char *what;
        struct pack_fixture spec;
    } cases[] = {
        {"a file that is not a pack", {.magic = "MCTPACK1"}},
        {"tiles that are not 256 pixels", {.tile_size = 512}},
        {"tiles that are not PNG", {.format = 9}},
        {"a pack holding nothing", {.empty = true}},
        {"an index longer than the file", {.count_override = 4096U}},
        {"an index inside its own header", {.index_at_override = 64U}},
        {"an index that was not sorted", {.unsorted = true}},
        {"a tile outside the world at its zoom", {.tile_off_the_world = true}},
        {"a tile whose bytes run past the end", {.extent_past_the_end = true}},
        {"a file that was copied half way", {.truncate = true}},
    };

    for (size_t i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        char path[64];
        const size_t written = pack_write(cases[i].spec, path, sizeof path);
        MESH_TEST_FAIL_IF(written == 0U, "fixture pack written");
        struct mesh_map_source source;
        const int opened = mesh_map_source_open_pack(path, &source);
        unlink(path);
        if (opened == 0) {
            mesh_map_source_close(&source);
        }
        MESH_TEST_FAIL_IF(opened == 0, cases[i].what);
    }
    record_success(test_name);
}

MESH_TEST_CASE(map_pack_missing_file_is_enoent, unit) {
    struct mesh_map_source source;
    const int opened = mesh_map_source_open_pack("/tmp/meshclient-no-such-pack.mctp", &source);
    MESH_TEST_FAIL_IF(opened != -ENOENT, "a path that is not there is -ENOENT");
    MESH_TEST_FAIL_IF(source.read != NULL || source.context != NULL, "and leaves nothing to close");

    /* The same promise for the arguments that never reach a file: a caller that closes
       unconditionally on its way out - which is what "nothing to close" is for - would
       otherwise be calling through whatever `close` its uninitialised local happened to hold. */
    struct mesh_map_source unopened;
    memset(&unopened, 0xAB, sizeof unopened);
    MESH_TEST_FAIL_IF(mesh_map_source_open_pack(NULL, &unopened) != -EINVAL,
                      "a NULL path is -EINVAL");
    MESH_TEST_FAIL_IF(unopened.close != NULL || unopened.read != NULL || unopened.context != NULL,
                      "and it still leaves nothing to close");
    mesh_map_source_close(&unopened);
    MESH_TEST_FAIL_IF(mesh_map_source_open_pack("/tmp", NULL) != -EINVAL,
                      "and no output is -EINVAL rather than a write through NULL");

    /* Closing a source that was never opened is deliberately safe, so a caller on its way out
       does not have to remember whether it got one. */
    mesh_map_source_close(&source);
    mesh_map_source_close(NULL);
    MESH_TEST_FAIL_IF(mesh_map_source_has(&source, (struct mesh_map_tile_key){.zoom = 1}),
                      "and answers nothing about tiles");
    record_success(test_name);
}
