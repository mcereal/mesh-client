#include "mesh/map/tile_cache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * The eviction policy, and why it is a linear scan.
 *
 * Every slot carries the cache's own counter as of the last time it was looked at or written,
 * and the victim is the smallest of them. That is least-recently-used, kept without a list:
 * there are at most MESH_MAP_TILE_CACHE_SLOTS_MAX slots and a frame asks about twenty tiles, so
 * the whole of a frame's bookkeeping is a few thousand comparisons of three integers - less
 * arithmetic than placing one marker, and far less than the 2-5 ms a single cold tile costs.
 *
 * A linked list would be the textbook answer and would buy nothing here. What it would add is
 * two pointers per slot and the one failure a cache of this size can actually have, which is a
 * list that has come apart: an LRU chain that loses a node leaks a slot silently, and the
 * symptom is a map that reads the card more often as it runs. A scan cannot be wrong about
 * anything, and its cost is bounded by a constant this file sets.
 *
 * The counter is 64-bit and deliberately never wraps. At one bump per tile per frame it would
 * need longer than the hardware will exist, so nothing here has to handle a counter that has
 * gone round - which is the other classic bug in this shape, and the one that shows up as the
 * most recently used tile being evicted first.
 */

static bool key_equal(struct mesh_map_tile_key a, struct mesh_map_tile_key b) {
    return a.zoom == b.zoom && a.x == b.x && a.y == b.y;
}

/* Counts from 1, so `used == 0` can mean "empty" without a second field saying so. */
static uint64_t cache_tick(struct mesh_map_tile_cache *cache) { return ++cache->clock; }

static uint8_t *slot_pixels(struct mesh_map_tile_cache *cache, size_t index) {
    return cache->pixels + index * MESH_MAP_TILE_CACHE_TILE_BYTES;
}

/* The slot holding `key`, ready or loading, or `slot_count` for none. */
static size_t slot_find(const struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key) {
    for (size_t i = 0U; i < cache->slot_count; ++i) {
        if (cache->slots[i].used != 0U && key_equal(cache->slots[i].key, key)) {
            return i;
        }
    }
    return cache->slot_count;
}

/*
 * The slot to take next: an empty one, or the least recently used one that is not being written
 * into.
 *
 * Returns `slot_count` when every slot is loading, which is unreachable while there is one
 * decode in flight at a time - the caller turns it into a NULL claim rather than into a slot
 * something else is halfway through writing.
 */
static size_t slot_evict(struct mesh_map_tile_cache *cache) {
    size_t victim = cache->slot_count;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0U; i < cache->slot_count; ++i) {
        if (cache->slots[i].used == 0U) {
            return i;
        }
        if (cache->slots[i].loading) {
            continue;
        }
        if (cache->slots[i].used < oldest) {
            oldest = cache->slots[i].used;
            victim = i;
        }
    }
    return victim;
}

/* Drops a slot whatever state it is in, reporting whether the frame lost a tile by it. */
static bool slot_release(struct mesh_map_tile_cache *cache, size_t index) {
    const bool was_ready = cache->slots[index].ready;
    memset(&cache->slots[index], 0, sizeof cache->slots[index]);
    return was_ready;
}

static size_t absent_find(const struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key) {
    for (size_t i = 0U; i < MESH_MAP_TILE_CACHE_ABSENT_MAX; ++i) {
        if (cache->absent[i].used != 0U && key_equal(cache->absent[i].key, key)) {
            return i;
        }
    }
    return MESH_MAP_TILE_CACHE_ABSENT_MAX;
}

static size_t absent_evict(const struct mesh_map_tile_cache *cache) {
    size_t victim = 0U;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0U; i < MESH_MAP_TILE_CACHE_ABSENT_MAX; ++i) {
        if (cache->absent[i].used == 0U) {
            return i;
        }
        if (cache->absent[i].used < oldest) {
            oldest = cache->absent[i].used;
            victim = i;
        }
    }
    return victim;
}

/* Drops any record that `key` is missing, reporting whether there was one. */
static bool absent_forget(struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key) {
    const size_t at = absent_find(cache, key);
    if (at >= MESH_MAP_TILE_CACHE_ABSENT_MAX) {
        return false;
    }
    memset(&cache->absent[at], 0, sizeof cache->absent[at]);
    return true;
}

int mesh_map_tile_cache_init(struct mesh_map_tile_cache *cache, size_t budget_bytes) {
    if (cache == NULL) {
        return -EINVAL;
    }
    /* Zeroed before anything can fail, so the contract that a failed init leaves nothing to
       release holds however the failure arrived - the same rule mesh_map_source_open_pack()
       states about a source nobody managed to open. */
    memset(cache, 0, sizeof *cache);

    size_t slots = budget_bytes / MESH_MAP_TILE_CACHE_TILE_BYTES;
    if (slots == 0U) {
        return -EINVAL;
    }
    if (slots > MESH_MAP_TILE_CACHE_SLOTS_MAX) {
        slots = MESH_MAP_TILE_CACHE_SLOTS_MAX;
    }

    /* calloc rather than malloc: the kernel hands back zero pages and nothing is touched until
       a tile lands in one, so a cache sized for a map the user never opens costs address space
       rather than eight megabytes of resident memory. */
    uint8_t *const pixels = calloc(slots, MESH_MAP_TILE_CACHE_TILE_BYTES);
    if (pixels == NULL) {
        return -ENOMEM;
    }
    cache->pixels = pixels;
    cache->slot_count = slots;
    return 0;
}

void mesh_map_tile_cache_deinit(struct mesh_map_tile_cache *cache) {
    if (cache == NULL) {
        return;
    }
    free(cache->pixels);
    memset(cache, 0, sizeof *cache);
}

void mesh_map_tile_cache_clear(struct mesh_map_tile_cache *cache) {
    if (cache == NULL) {
        return;
    }
    /* The counter is the cheap proof that this cache has answered for something: it only ever
       moves when a slot or an absence is written. A clear that found an abandoned claim and
       nothing else reports a change nobody needed, which is the direction to be wrong in. */
    const bool lost = cache->clock != 0U;
    memset(cache->slots, 0, sizeof cache->slots);
    memset(cache->absent, 0, sizeof cache->absent);
    /* Safe to restart the counter only because every `used` went to 0 with it. */
    cache->clock = 0U;
    if (lost) {
        ++cache->revision;
    }
}

enum mesh_map_tile_state mesh_map_tile_cache_get(struct mesh_map_tile_cache *cache,
                                                 struct mesh_map_tile_key key,
                                                 const uint8_t **pixels) {
    if (pixels != NULL) {
        *pixels = NULL;
    }
    if (cache == NULL || cache->pixels == NULL) {
        return MESH_MAP_TILE_MISS;
    }

    const size_t at = slot_find(cache, key);
    if (at < cache->slot_count) {
        /* A loading slot is a MISS: it holds a tile that is halfway decoded, and the caller's
           answer to "not readable" is the same either way - draw the grid and wait. */
        if (!cache->slots[at].ready) {
            return MESH_MAP_TILE_MISS;
        }
        cache->slots[at].used = cache_tick(cache);
        if (pixels != NULL) {
            *pixels = slot_pixels(cache, at);
        }
        return MESH_MAP_TILE_READY;
    }

    const size_t hole = absent_find(cache, key);
    if (hole < MESH_MAP_TILE_CACHE_ABSENT_MAX) {
        /* An absence that is being looked at is an absence worth keeping, for the same reason a
           tile being drawn is: what the view is standing on is what should survive eviction. */
        cache->absent[hole].used = cache_tick(cache);
        return MESH_MAP_TILE_ABSENT;
    }
    return MESH_MAP_TILE_MISS;
}

uint8_t *mesh_map_tile_cache_claim(struct mesh_map_tile_cache *cache,
                                   struct mesh_map_tile_key key) {
    if (cache == NULL || cache->pixels == NULL || !mesh_map_tile_key_valid(key)) {
        return NULL;
    }

    bool answer_changed = false;

    size_t at = slot_find(cache, key);
    if (at < cache->slot_count) {
        /* Already loading: the same buffer, not a second slot. This is the whole of request
           de-duplication - a caller cannot spend two slots on one tile however often it asks.
           A loading key is never in the absent table, so there is nothing to forget here. */
        if (cache->slots[at].loading) {
            return slot_pixels(cache, at);
        }
        if (cache->slots[at].ready) {
            cache->slots[at].ready = false;
            answer_changed = true;
        }
    } else {
        at = slot_evict(cache);
        if (at >= cache->slot_count) {
            /* Nothing has been touched, which is the point of doing this before the absence is
               dropped: a refused claim must leave the cache exactly as it found it. Forgetting
               first and failing after would turn a key we *know* is not in the pack back into an
               unknown one, and the fill loop would spend a read rediscovering it. */
            return NULL;
        }
        answer_changed |= slot_release(cache, at);
        cache->slots[at].key = key;
    }

    /* The slot is secured, so it is now true that this key is no longer one we think is
       missing: the caller is about to decode it. */
    answer_changed |= absent_forget(cache, key);
    cache->slots[at].loading = true;
    cache->slots[at].used = cache_tick(cache);
    if (answer_changed) {
        ++cache->revision;
    }
    return slot_pixels(cache, at);
}

void mesh_map_tile_cache_commit(struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key) {
    if (cache == NULL || cache->pixels == NULL) {
        return;
    }
    const size_t at = slot_find(cache, key);
    /* Only a loading slot can be committed, which is what makes a stray or repeated commit a
       no-op rather than a way of publishing a slot holding nothing. */
    if (at >= cache->slot_count || !cache->slots[at].loading) {
        return;
    }
    cache->slots[at].loading = false;
    cache->slots[at].ready = true;
    cache->slots[at].used = cache_tick(cache);
    ++cache->revision;
}

void mesh_map_tile_cache_abandon(struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key) {
    if (cache == NULL || cache->pixels == NULL) {
        return;
    }
    const size_t at = slot_find(cache, key);
    if (at >= cache->slot_count || !cache->slots[at].loading) {
        return;
    }
    /* No revision change: the claim that made this slot loading already reported the tile it
       displaced, and a slot that was never readable cannot stop being it. */
    (void)slot_release(cache, at);
}

void mesh_map_tile_cache_note_absent(struct mesh_map_tile_cache *cache,
                                     struct mesh_map_tile_key key) {
    if (cache == NULL || !mesh_map_tile_key_valid(key)) {
        return;
    }

    /* A key lives in at most one of the two tables, so the pixels go before the absence is
       written - otherwise get() could answer READY or ABSENT for one tile depending on which
       table it happened to consult first. */
    const size_t at = slot_find(cache, key);
    bool answer_changed = at < cache->slot_count && slot_release(cache, at);

    size_t hole = absent_find(cache, key);
    if (hole >= MESH_MAP_TILE_CACHE_ABSENT_MAX) {
        hole = absent_evict(cache);
        /* The entry this displaced stops being answered for, which is a change in its own
           right - and the key just learned about was not absent a moment ago either. */
        answer_changed = true;
    }
    cache->absent[hole].key = key;
    cache->absent[hole].used = cache_tick(cache);
    if (answer_changed) {
        ++cache->revision;
    }
}

uint32_t mesh_map_tile_cache_revision(const struct mesh_map_tile_cache *cache) {
    return cache != NULL ? cache->revision : 0U;
}

size_t mesh_map_tile_cache_slots(const struct mesh_map_tile_cache *cache) {
    return cache != NULL ? cache->slot_count : 0U;
}

size_t mesh_map_tile_cache_bytes(const struct mesh_map_tile_cache *cache) {
    return cache != NULL ? cache->slot_count * MESH_MAP_TILE_CACHE_TILE_BYTES : 0U;
}

size_t mesh_map_tile_cache_ready(const struct mesh_map_tile_cache *cache) {
    if (cache == NULL) {
        return 0U;
    }
    size_t ready = 0U;
    for (size_t i = 0U; i < cache->slot_count; ++i) {
        if (cache->slots[i].ready) {
            ++ready;
        }
    }
    return ready;
}
