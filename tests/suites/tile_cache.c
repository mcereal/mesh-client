/*
 * The decoded tile cache: what it holds, what it gives up first, and what it refuses to forget.
 *
 * Every case here runs with no pack and no decoder linked in, which is the property the module
 * was shaped for - a cache is a store, and a store can be tested by writing bytes into it. The
 * fill bytes below stand in for pixels precisely because nothing here should care what a tile
 * looks like: what is under test is which slot a key lands in and which one is taken next.
 *
 * Three of these are about a state rather than a value, and they are the ones worth reading.
 * MISS and ABSENT are different answers and a renderer draws them differently, so a case that
 * only checked "no pixels came back" would pass with the two confused. A slot being written
 * into must not be readable, or a half-decoded tile reaches the panel. And a claim that has
 * been made twice must not have spent two slots, which is the only form request de-duplication
 * takes in a client with one decode in flight.
 */

#include "framework/mesh_test.h"

#include "mesh/map/tile_cache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Zoom 4 is a 16x16 world: 256 distinct keys, which is more than the absent table holds and
   enough to fill any cache these cases ask for. */
#define CACHE_TEST_ZOOM 4

static struct mesh_map_tile_key tile_key(uint32_t x, uint32_t y) {
    struct mesh_map_tile_key key = {.zoom = CACHE_TEST_ZOOM, .x = x, .y = y};
    return key;
}

/* The nth distinct key of the test world, walked in row-major order. */
static struct mesh_map_tile_key tile_key_nth(uint32_t n) { return tile_key(n % 16U, n / 16U); }

/* Claims a slot, fills it with one byte and commits - one decode, with the decoder left out. */
static bool tile_put(struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key,
                     uint8_t fill) {
    uint8_t *const pixels = mesh_map_tile_cache_claim(cache, key);
    if (pixels == NULL) {
        return false;
    }
    memset(pixels, fill, MESH_MAP_TILE_CACHE_TILE_BYTES);
    mesh_map_tile_cache_commit(cache, key);
    return true;
}

static enum mesh_map_tile_state tile_state(struct mesh_map_tile_cache *cache,
                                           struct mesh_map_tile_key key) {
    return mesh_map_tile_cache_get(cache, key, NULL);
}

/* The cache is about 3 KiB of bookkeeping, so it is a heap record here rather than a local -
   the same way the client will hold one. */
static struct mesh_map_tile_cache *tile_cache_new(size_t tiles) {
    struct mesh_map_tile_cache *const cache = calloc(1U, sizeof *cache);
    if (cache == NULL) {
        return NULL;
    }
    if (mesh_map_tile_cache_init(cache, tiles * MESH_MAP_TILE_CACHE_TILE_BYTES) != 0) {
        free(cache);
        return NULL;
    }
    return cache;
}

static void tile_cache_free(struct mesh_map_tile_cache *cache) {
    mesh_map_tile_cache_deinit(cache);
    free(cache);
}

MESH_TEST_CASE(map_tile_cache_budget_rounds_down_and_clamps, unit) {
    struct mesh_map_tile_cache *const cache = calloc(1U, sizeof *cache);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    /* Three and a half tiles is three tiles: a caller never gets more room than it asked for. */
    const size_t budget = 3U * MESH_MAP_TILE_CACHE_TILE_BYTES + MESH_MAP_TILE_CACHE_TILE_BYTES / 2U;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_init(cache, budget) != 0, free(cache),
                              "a three-and-a-half tile budget should initialise");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_slots(cache) != 3U, tile_cache_free(cache),
                              "the budget should round down to whole tiles");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_bytes(cache) !=
                                  3U * MESH_MAP_TILE_CACHE_TILE_BYTES,
                              tile_cache_free(cache), "the reported cost should be three tiles");
    mesh_map_tile_cache_deinit(cache);

    const size_t over = (MESH_MAP_TILE_CACHE_SLOTS_MAX + 8U) * MESH_MAP_TILE_CACHE_TILE_BYTES;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_init(cache, over) != 0, free(cache),
                              "an oversized budget should still initialise");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_slots(cache) != MESH_MAP_TILE_CACHE_SLOTS_MAX,
                              tile_cache_free(cache), "a budget past the cap should be clamped");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_refuses_a_budget_under_one_tile, unit) {
    struct mesh_map_tile_cache *const cache = calloc(1U, sizeof *cache);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    /*
     * A cache with no slots is not an empty cache, it is the failure this module exists to
     * prevent arrived at by configuration: it answers MISS to everything, so the fill loop
     * reads the card once per tile per frame and never gets anywhere.
     */
    const int refused = mesh_map_tile_cache_init(cache, MESH_MAP_TILE_CACHE_TILE_BYTES - 1U);
    MESH_TEST_FAIL_IF_CLEANUP(refused != -EINVAL, free(cache),
                              "a budget under one tile should be -EINVAL");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_slots(cache) != 0U, free(cache),
                              "a refused init should leave no slots");

    /* The contract a caller relies on when it releases unconditionally on its way out. */
    mesh_map_tile_cache_deinit(cache);
    free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_round_trips_a_committed_tile, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    const struct mesh_map_tile_key key = tile_key(3U, 5U);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, key) != MESH_MAP_TILE_MISS, tile_cache_free(cache),
                              "an unknown key should miss");
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, key, 0xA5U), tile_cache_free(cache),
                              "claim and commit should succeed");

    const uint8_t *pixels = NULL;
    const enum mesh_map_tile_state state = mesh_map_tile_cache_get(cache, key, &pixels);
    MESH_TEST_FAIL_IF_CLEANUP(state != MESH_MAP_TILE_READY, tile_cache_free(cache),
                              "a committed tile should be ready");
    MESH_TEST_FAIL_IF_CLEANUP(pixels == NULL, tile_cache_free(cache),
                              "a ready tile should hand back pixels");
    MESH_TEST_FAIL_IF_CLEANUP(
        pixels[0] != 0xA5U || pixels[MESH_MAP_TILE_CACHE_TILE_BYTES - 1U] != 0xA5U,
        tile_cache_free(cache), "the whole tile should come back as it was written");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_ready(cache) != 1U, tile_cache_free(cache),
                              "one tile should be readable");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_hides_a_tile_being_decoded, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    const struct mesh_map_tile_key key = tile_key(1U, 1U);
    uint8_t *const pixels = mesh_map_tile_cache_claim(cache, key);
    MESH_TEST_FAIL_IF_CLEANUP(pixels == NULL, tile_cache_free(cache), "the claim should succeed");
    memset(pixels, 0x11U, MESH_MAP_TILE_CACHE_TILE_BYTES / 2U);

    /* Half a tile has been written. Nothing may draw it, and the answer is MISS rather than
       ABSENT: a tile is on its way. */
    const uint8_t *seen = (const uint8_t *)cache; /* deliberately non-NULL going in */
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_get(cache, key, &seen) != MESH_MAP_TILE_MISS,
                              tile_cache_free(cache), "a loading tile should not be readable");
    MESH_TEST_FAIL_IF_CLEANUP(seen != NULL, tile_cache_free(cache),
                              "a miss should clear the caller's pointer");

    mesh_map_tile_cache_commit(cache, key);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, key) != MESH_MAP_TILE_READY, tile_cache_free(cache),
                              "a committed tile should become readable");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_abandons_a_failed_decode, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(1U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    const struct mesh_map_tile_key key = tile_key(2U, 2U);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_claim(cache, key) == NULL, tile_cache_free(cache),
                              "the claim should succeed");
    mesh_map_tile_cache_abandon(cache, key);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, key) != MESH_MAP_TILE_MISS, tile_cache_free(cache),
                              "an abandoned tile should be unknown again");

    /* A commit arriving after the abandon must not publish a slot holding nothing. */
    mesh_map_tile_cache_commit(cache, key);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, key) != MESH_MAP_TILE_MISS, tile_cache_free(cache),
                              "a late commit should not revive an abandoned slot");

    /* And the slot came back: the cache holds one tile and another key can still use it. */
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key(3U, 3U), 0x22U), tile_cache_free(cache),
                              "the abandoned slot should be reusable");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_claims_one_slot_per_key, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    const struct mesh_map_tile_key key = tile_key(4U, 4U);
    uint8_t *const first = mesh_map_tile_cache_claim(cache, key);
    uint8_t *const again = mesh_map_tile_cache_claim(cache, key);
    MESH_TEST_FAIL_IF_CLEANUP(first == NULL || again != first, tile_cache_free(cache),
                              "a repeated claim should hand back the same slot");

    /* If the second claim had taken a slot of its own, this put would have had none left and
       would have evicted the tile being decoded. */
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key(5U, 5U), 0x33U), tile_cache_free(cache),
                              "the second slot should still be free");
    mesh_map_tile_cache_commit(cache, key);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_ready(cache) != 2U, tile_cache_free(cache),
                              "both tiles should be readable");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_evicts_the_least_recently_used, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(4U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    for (uint32_t i = 0U; i < 4U; ++i) {
        MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key_nth(i), (uint8_t)(i + 1U)),
                                  tile_cache_free(cache), "filling the cache should succeed");
    }

    /* Looking at the oldest tile is what saves it: a hit is what "recently used" is made of. */
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, tile_key_nth(0U)) != MESH_MAP_TILE_READY,
                              tile_cache_free(cache), "the first tile should still be held");
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key_nth(4U), 0x55U), tile_cache_free(cache),
                              "a fifth tile should still be accepted");

    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, tile_key_nth(1U)) != MESH_MAP_TILE_MISS,
                              tile_cache_free(cache),
                              "the tile nobody looked at should have been evicted");
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, tile_key_nth(0U)) != MESH_MAP_TILE_READY,
                              tile_cache_free(cache), "the tile that was looked at should survive");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_ready(cache) != 4U, tile_cache_free(cache),
                              "the cache should still be holding four tiles");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_never_evicts_a_slot_being_written, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    const struct mesh_map_tile_key loading = tile_key(6U, 6U);
    uint8_t *const pixels = mesh_map_tile_cache_claim(cache, loading);
    MESH_TEST_FAIL_IF_CLEANUP(pixels == NULL, tile_cache_free(cache), "the claim should succeed");

    /* Two more tiles through a two-slot cache: the one not being decoded into is the only
       candidate, so it is taken twice and the decode is never disturbed. */
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key(7U, 7U), 0x66U), tile_cache_free(cache),
                              "the free slot should take the first tile");
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key(8U, 8U), 0x77U), tile_cache_free(cache),
                              "the same slot should take the second");

    memset(pixels, 0x99U, MESH_MAP_TILE_CACHE_TILE_BYTES);
    mesh_map_tile_cache_commit(cache, loading);

    const uint8_t *seen = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_get(cache, loading, &seen) != MESH_MAP_TILE_READY,
                              tile_cache_free(cache), "the decoded tile should survive eviction");
    MESH_TEST_FAIL_IF_CLEANUP(seen == NULL || seen[0] != 0x99U, tile_cache_free(cache),
                              "the decoded tile should hold what was written into it");
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, tile_key(7U, 7U)) != MESH_MAP_TILE_MISS,
                              tile_cache_free(cache), "the first ordinary tile should be gone");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_remembers_a_hole, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    const struct mesh_map_tile_key sea = tile_key(9U, 9U);
    mesh_map_tile_cache_note_absent(cache, sea);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, sea) != MESH_MAP_TILE_ABSENT,
                              tile_cache_free(cache),
                              "a hole should be remembered as absent, not as a miss");

    /* A hole costs no pixels: both slots are still there for tiles that exist. */
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key(10U, 10U), 0x01U), tile_cache_free(cache),
                              "the first slot should be free");
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, tile_key(11U, 11U), 0x02U), tile_cache_free(cache),
                              "the second slot should be free");
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, sea) != MESH_MAP_TILE_ABSENT,
                              tile_cache_free(cache), "the hole should have survived two tiles");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_keys_answer_one_way_only, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    /*
     * The invariant the state enum rests on: a key is in at most one of the two tables. Without
     * it the cache answers READY or ABSENT for one tile depending on which table it consults
     * first, which is a picture that appears and disappears as other tiles come and go.
     */
    const struct mesh_map_tile_key key = tile_key(12U, 12U);
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, key, 0x44U), tile_cache_free(cache),
                              "the tile should be accepted");
    mesh_map_tile_cache_note_absent(cache, key);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, key) != MESH_MAP_TILE_ABSENT,
                              tile_cache_free(cache),
                              "an absence should displace the pixels held for that key");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_ready(cache) != 0U, tile_cache_free(cache),
                              "the slot should have been given back");

    /* And back the other way: the source turned out to hold it after all. */
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, key, 0x55U), tile_cache_free(cache),
                              "the tile should be accepted again");
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, key) != MESH_MAP_TILE_READY, tile_cache_free(cache),
                              "a claim should clear the absence for that key");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_forgets_the_hole_nobody_is_looking_at, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(1U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    for (uint32_t i = 0U; i < MESH_MAP_TILE_CACHE_ABSENT_MAX; ++i) {
        mesh_map_tile_cache_note_absent(cache, tile_key_nth(i));
    }
    /* The oldest entry, looked at once, is now the newest. */
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, tile_key_nth(0U)) != MESH_MAP_TILE_ABSENT,
                              tile_cache_free(cache), "the first hole should still be known");

    mesh_map_tile_cache_note_absent(cache, tile_key_nth(MESH_MAP_TILE_CACHE_ABSENT_MAX));
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, tile_key_nth(0U)) != MESH_MAP_TILE_ABSENT,
                              tile_cache_free(cache),
                              "the hole on screen should outlive the table filling up");
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, tile_key_nth(1U)) != MESH_MAP_TILE_MISS,
                              tile_cache_free(cache),
                              "the least recently asked-about hole should be the one forgotten");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_clear_drops_tiles_and_holes, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    /*
     * What a pack swap has to do. A key is three numbers about the world rather than about a
     * file, so two packs of the same city hold different pictures at the same key - and a cache
     * carried across the swap draws the old pack's streets under the new pack's attribution,
     * with nothing on the frame looking wrong.
     */
    const struct mesh_map_tile_key held = tile_key(13U, 13U);
    const struct mesh_map_tile_key sea = tile_key(14U, 14U);
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, held, 0x66U), tile_cache_free(cache),
                              "the tile should be accepted");
    mesh_map_tile_cache_note_absent(cache, sea);

    const size_t slots = mesh_map_tile_cache_slots(cache);
    mesh_map_tile_cache_clear(cache);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, held) != MESH_MAP_TILE_MISS, tile_cache_free(cache),
                              "a clear should drop the tiles");
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, sea) != MESH_MAP_TILE_MISS, tile_cache_free(cache),
                              "a clear should drop the holes too");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_slots(cache) != slots, tile_cache_free(cache),
                              "a clear should keep the allocation");
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, held, 0x77U), tile_cache_free(cache),
                              "the cache should still be usable after a clear");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_revision_tracks_what_would_be_drawn, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(1U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    /*
     * A frame is otherwise a function of the snapshot, and no radio packet arrives to say that a
     * tile landed - so this is the only thing that can ask for the repaint that shows it.
     */
    const struct mesh_map_tile_key key = tile_key(15U, 15U);
    const uint32_t start = mesh_map_tile_cache_revision(cache);
    MESH_TEST_FAIL_IF_CLEANUP(!tile_put(cache, key, 0x88U), tile_cache_free(cache),
                              "the tile should be accepted");
    const uint32_t after_commit = mesh_map_tile_cache_revision(cache);
    MESH_TEST_FAIL_IF_CLEANUP(after_commit == start, tile_cache_free(cache),
                              "a decoded tile should ask for a repaint");

    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, key) != MESH_MAP_TILE_READY, tile_cache_free(cache),
                              "the tile should be readable");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_revision(cache) != after_commit,
                              tile_cache_free(cache),
                              "drawing a tile that was already held changes nothing");

    mesh_map_tile_cache_note_absent(cache, tile_key(0U, 1U));
    const uint32_t after_hole = mesh_map_tile_cache_revision(cache);
    MESH_TEST_FAIL_IF_CLEANUP(after_hole == after_commit, tile_cache_free(cache),
                              "learning that a tile is not in the pack changes the frame");

    mesh_map_tile_cache_clear(cache);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_revision(cache) == after_hole,
                              tile_cache_free(cache), "a clear should ask for a repaint");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_refuses_a_key_that_cannot_exist, unit) {
    struct mesh_map_tile_cache *const cache = tile_cache_new(2U);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    /* Zoom 4 is a 16x16 world, so (16, 0) is off the end of it. A source would refuse the read;
       nothing should be able to spend a slot or a hole on it first. */
    const struct mesh_map_tile_key beyond = tile_key(16U, 0U);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_claim(cache, beyond) != NULL,
                              tile_cache_free(cache), "a key outside the world should not claim");
    mesh_map_tile_cache_note_absent(cache, beyond);
    MESH_TEST_FAIL_IF_CLEANUP(tile_state(cache, beyond) != MESH_MAP_TILE_MISS,
                              tile_cache_free(cache),
                              "a key outside the world should not fill the absent table");
    tile_cache_free(cache);
    record_success(test_name);
}

MESH_TEST_CASE(map_tile_cache_survives_being_used_uninitialised, unit) {
    /* Every entry point is reachable from a client whose init failed - a map opened on a device
       with no memory to spare - and none of them may be the thing that takes it down. */
    struct mesh_map_tile_cache *const cache = calloc(1U, sizeof *cache);
    MESH_TEST_FAIL_IF(cache == NULL, "cache allocation failed");

    const struct mesh_map_tile_key key = tile_key(1U, 2U);
    const uint8_t *pixels = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_get(cache, key, &pixels) != MESH_MAP_TILE_MISS,
                              free(cache), "an uninitialised cache should miss");
    MESH_TEST_FAIL_IF_CLEANUP(pixels != NULL, free(cache), "a miss should hand back no pixels");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_claim(cache, key) != NULL, free(cache),
                              "an uninitialised cache should refuse a claim");
    mesh_map_tile_cache_commit(cache, key);
    mesh_map_tile_cache_abandon(cache, key);
    mesh_map_tile_cache_clear(cache);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_cache_bytes(cache) != 0U, free(cache),
                              "an uninitialised cache should cost nothing");

    mesh_map_tile_cache_deinit(cache);
    free(cache);

    /* And the NULL guards every accessor promises. */
    MESH_TEST_FAIL_IF(mesh_map_tile_cache_get(NULL, key, NULL) != MESH_MAP_TILE_MISS,
                      "get(NULL) should miss");
    MESH_TEST_FAIL_IF(mesh_map_tile_cache_claim(NULL, key) != NULL, "claim(NULL) should refuse");
    MESH_TEST_FAIL_IF(mesh_map_tile_cache_slots(NULL) != 0U, "slots(NULL) should be zero");
    MESH_TEST_FAIL_IF(mesh_map_tile_cache_ready(NULL) != 0U, "ready(NULL) should be zero");
    MESH_TEST_FAIL_IF(mesh_map_tile_cache_revision(NULL) != 0U, "revision(NULL) should be zero");
    mesh_map_tile_cache_deinit(NULL);
    record_success(test_name);
}
