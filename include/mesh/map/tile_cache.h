#ifndef MESH_MAP_TILE_CACHE_H
#define MESH_MAP_TILE_CACHE_H

#include "mesh/map/tile.h"
#include "mesh/map/tile_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The decoded tiles the map is standing on, and what it may say about the ones it is not.
 *
 * A cold tile costs 2-5 ms on the Brick's card (docs/maps-roadmap.md), and a view covers twenty
 * of them. Reading the pack every frame is therefore not a slow map, it is a client that stops
 * servicing BLE for a tenth of a second every time the panel repaints - so the tiles that are
 * on screen are held in RAM and the ones that leave it are let go in the order they stopped
 * being looked at.
 *
 * It is a *store*, not a loader. Nothing here opens a file, reads a source, or calls the
 * decoder; what it knows is which keys it is holding pixels for, which keys it has been told
 * are not there, and which slot to give up next. That is what keeps it testable with neither a
 * pack nor Wuffs linked in - the same split that lets source_pack.c be tested with no decoder -
 * and it is what lets the fill loop above it decide the policy the roadmap actually cares
 * about: one decode per turn of the event loop.
 *
 * Nothing here includes the framebuffer, the UI store, a protobuf or a filesystem header.
 */

/*
 * What a cached tile is made of: exactly what mesh_map_tile_decode() produces, which is BGRA at
 * MESH_MAP_TILE_PIXEL_BYTES a pixel.
 *
 * This is the question docs/maps-roadmap.md left open - "what a cache holds and what a panel
 * wants are two questions" - and it is answered in favour of the decoder rather than the panel,
 * for a reason that is about the boundary rather than about bytes. `struct fb_state` reads its
 * `bytes_per_pixel` off the kernel at runtime and it is not always 4; storing panel-format
 * pixels would mean this file learning that number, which means `src/map/` learning what a
 * framebuffer is. It would also make a cached tile the property of one backend: the capture
 * harness renders at four bytes a pixel whatever the device is doing, so the same cache would
 * be holding the wrong format for one of the two.
 *
 * What it costs is real and is worth writing down: on a panel narrower than four bytes the
 * cache holds more bytes than that panel strictly needs, and the blit converts per *drawn*
 * pixel. The conversion is not new work in a new place - fb_fill_packed() already switches on
 * `bytes_per_pixel`, and the blit joins that switch rather than adding one.
 */
#define MESH_MAP_TILE_CACHE_TILE_BYTES MESH_MAP_TILE_IMAGE_BYTES

/*
 * The most tiles a cache will hold however large a budget it is handed.
 *
 * A 1024x768 body at an arbitrary alignment intersects 5x4 tiles, so sixteen holds a full view
 * with room for the row a pan is about to reveal, and sixty-four is four such views. Past that
 * a byte budget stops describing a working set and starts describing a leak somebody typed by
 * accident: 64 slots is already 16 MiB, against two 3 MiB frame buffers and a 2.88 MB binary.
 */
#define MESH_MAP_TILE_CACHE_SLOTS_MAX 64U

/*
 * What the device asks for: 8 MiB, which is the 32 tiles docs/maps-roadmap.md budgets.
 *
 * A caller states its own budget rather than taking this, because the capture harness and the
 * tests want a cache small enough that eviction happens on purpose rather than after eight
 * megabytes of fixtures.
 */
#define MESH_MAP_TILE_CACHE_BYTES_DEFAULT (32U * MESH_MAP_TILE_CACHE_TILE_BYTES)

/*
 * How many "this tile is not in the pack" answers are remembered, and why any are.
 *
 * Every pack is a rectangle of the world with holes in it, and a hole is an ordinary answer
 * rather than a failure - a coastline pack is mostly sea it does not hold. But the fill loop
 * above this one gets one read per turn: without a record of what has already been asked for
 * and refused, a single hole on screen consumes that one read every turn, forever, and a view
 * with a hole in it never finishes filling the tiles around it. That is the bug this table
 * exists for, and it is why a refusal is remembered rather than simply not cached.
 *
 * They are kept apart from the pixel slots because a hole costs twelve bytes and a tile costs
 * 256 KiB: sharing the slots would let a sparse view evict the picture to remember the sea.
 * 128 is six screens of solid absence.
 */
#define MESH_MAP_TILE_CACHE_ABSENT_MAX 128U

/*
 * What the cache can say about one key. Exactly one of these is true of a key at a time - see
 * the invariant on mesh_map_tile_cache_note_absent().
 *
 * MISS is not an error and ABSENT is not a failure. They are the two different things a blank
 * square means, and telling them apart is the whole reason the state is an enum rather than a
 * pointer that may be NULL: on a MISS the map has a tile coming and should draw the empty grid
 * it draws while loading; on an ABSENT there is nothing coming and the grid is the final
 * picture. A renderer that could not tell them apart would either show a loading state forever
 * over open sea, or stop drawing one at all.
 */
enum mesh_map_tile_state {
    /* Nothing is known about this key. Somebody should read it. */
    MESH_MAP_TILE_MISS = 0,
    /* Pixels are held and were handed back. */
    MESH_MAP_TILE_READY,
    /* The source was asked and does not hold this tile. Do not ask again. */
    MESH_MAP_TILE_ABSENT,
};

/* One pixel slot's bookkeeping. In the header because the cache is a value a caller owns, not
   because anything outside src/map/tile_cache.c has business reading these fields. */
struct mesh_map_tile_slot {
    struct mesh_map_tile_key key;
    /* The cache's own counter at the last get() or commit(). 0 means the slot is empty, which
       is why the counter starts at 1. */
    uint64_t used;
    /* Readable. False while a decode is in flight, so a half-written tile is never drawn. */
    bool ready;
    /* Claimed and not yet committed or abandoned. Never evicted, because something is writing
       into it. */
    bool loading;
};

struct mesh_map_tile_absent {
    struct mesh_map_tile_key key;
    uint64_t used;
};

/*
 * The cache itself.
 *
 * About 3 KiB of bookkeeping plus one heap block for the pixels, so it belongs in a struct
 * somebody already owns or in static storage - not on a stack frame. The pixels are one
 * allocation taken at init and never resized, which is the decoder's rule one layer up: a cache
 * that allocated per tile would be a malloc on the event loop's repaint path, and the one thing
 * there is nothing sensible to do about at that moment is running out of memory mid-frame.
 */
struct mesh_map_tile_cache {
    uint8_t *pixels;
    size_t slot_count;
    uint64_t clock;
    uint32_t revision;
    struct mesh_map_tile_slot slots[MESH_MAP_TILE_CACHE_SLOTS_MAX];
    struct mesh_map_tile_absent absent[MESH_MAP_TILE_CACHE_ABSENT_MAX];
};

/*
 * Sizes a cache from a byte budget and allocates its pixels.
 *
 * `budget_bytes` is rounded *down* to whole tiles and clamped to MESH_MAP_TILE_CACHE_SLOTS_MAX,
 * so what a caller gets is never more than it asked for. A budget that will not hold one tile
 * is -EINVAL rather than an empty cache: a cache with no slots answers MISS to everything and
 * would read the card once per tile per frame, which is the failure this module exists to
 * prevent, arrived at by configuration.
 *
 * 0 on success, -EINVAL for a NULL cache or a budget under one tile, -ENOMEM when the block
 * will not fit. On failure the cache is zeroed and there is nothing to release.
 *
 * The struct is assumed uninitialised. Re-initialising a live cache leaks its pixels; deinit
 * first.
 */
int mesh_map_tile_cache_init(struct mesh_map_tile_cache *cache, size_t budget_bytes);

/* Releases the pixels and zeroes the cache. Safe on a zeroed cache and on one that failed to
   init, so a caller can release unconditionally on its way out. */
void mesh_map_tile_cache_deinit(struct mesh_map_tile_cache *cache);

/*
 * Forgets every tile and every absence, keeping the allocation.
 *
 * **Opening a different source must call this**, and that is the one rule a caller can break
 * invisibly. A key is three numbers about the world, not about a file: two packs of the same
 * city at the same zoom both hold a tile (14, 8192, 5461), and they are different pictures. A
 * cache carried across a swap draws the old pack's streets under the new pack's attribution,
 * and every pixel of it is a tile that was decoded from a real file and is in the right place -
 * so there is nothing on the frame that looks wrong.
 */
void mesh_map_tile_cache_clear(struct mesh_map_tile_cache *cache);

/*
 * What the cache knows about one key, and its pixels when it has them.
 *
 * `pixels` may be NULL when the caller only wants the state. When it is not, it is set to
 * MESH_MAP_TILE_CACHE_TILE_BYTES of BGRA on MESH_MAP_TILE_READY and to NULL otherwise - so a
 * caller that ignores the return value and checks the pointer is still correct.
 *
 * Takes a mutable cache because a hit is what "recently used" is made of: this is the call that
 * decides what eviction takes next, so a frame that draws a tile is what keeps it.
 *
 * The pointer is valid until the next claim() on a different key, or any clear() or deinit().
 * Nothing in the fill loop holds one across a turn, and nothing should: the whole point of a
 * cache with an eviction policy is that it may stop holding a tile.
 */
enum mesh_map_tile_state mesh_map_tile_cache_get(struct mesh_map_tile_cache *cache,
                                                 struct mesh_map_tile_key key,
                                                 const uint8_t **pixels);

/*
 * Takes the slot a tile is about to be decoded into, or NULL.
 *
 * Returns MESH_MAP_TILE_CACHE_TILE_BYTES to write, evicting the least recently used slot to
 * find them. The slot is marked loading: it is not readable, and not evictable, until
 * commit() or abandon(). A claim for a key already loading hands back the same buffer rather
 * than a second slot, which is the whole of request de-duplication here - a caller cannot get
 * two slots for one tile however many times it asks.
 *
 * A claim for a key that is currently READY re-decodes it: the slot stops being readable and
 * the old pixels are gone. That is the honest reading of "decode this again", and it is not a
 * path the fill loop takes - it claims on a MISS.
 *
 * NULL when the key is not one that can exist, when the cache was never initialised, or - only
 * reachable by a caller that claims without committing - when every slot is loading. There is
 * exactly one decode in flight in this client, so the last of those is an invariant being
 * stated rather than a case anything meets.
 *
 * Claiming a key clears any record that it was absent: the source has just been found to hold
 * it after all.
 */
uint8_t *mesh_map_tile_cache_claim(struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key);

/* Makes a claimed slot readable. A no-op for a key that is not loading, so a double commit or
   a commit after an abandon cannot make a slot holding nothing look ready. */
void mesh_map_tile_cache_commit(struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key);

/*
 * Gives a claimed slot back without making it readable - a decode that failed.
 *
 * The slot becomes empty rather than staying half-written, so the next claim takes it ahead of
 * anything real. Note that forgetting to call this is safe rather than merely unlucky: a slot
 * left loading is never readable, so the worst a lost abandon costs is one slot until the cache
 * is cleared. That is deliberate - the alternative is a shape where a missed call puts a
 * half-decoded tile on the panel.
 */
void mesh_map_tile_cache_abandon(struct mesh_map_tile_cache *cache, struct mesh_map_tile_key key);

/*
 * Records that the source does not hold this key.
 *
 * Drops any pixels being held for it, which is what keeps the invariant the state enum rests
 * on: **a key is in at most one of the two tables**, so the cache cannot answer two ways about
 * one tile depending on which it looks at first.
 *
 * The absence is forgotten when the table is full and this is the least recently asked-about
 * entry, and re-learned by one read. That is the right way round: a hole nobody is looking at
 * costing one read when they look again is cheaper than a hole on screen being forgotten.
 */
void mesh_map_tile_cache_note_absent(struct mesh_map_tile_cache *cache,
                                     struct mesh_map_tile_key key);

/*
 * Changes every time the cache's answer about some key changes - a commit, an eviction, an
 * absence learned or dropped, a clear. Not on a plain hit, which changes only what eviction
 * will take next.
 *
 * This is what lets a repaint be requested when a decode finishes, which the map needs and
 * nothing else in this client does: a frame is otherwise a function of the snapshot, and no
 * radio packet arrives to say that a tile landed. Compare it with the last one seen; never
 * order two of them, because it wraps.
 *
 * It counts *answers* rather than pixels because MISS and ABSENT are drawn differently - one is
 * a tile still coming and the other is the final picture of open sea - so learning that a tile
 * is not in the pack changes the frame exactly as decoding one does. Where the two readings
 * differ this errs towards reporting a change: a repaint nobody needed costs a frame, and a
 * repaint that did not happen is a map that stays blank until the user presses something.
 */
uint32_t mesh_map_tile_cache_revision(const struct mesh_map_tile_cache *cache);

/* How many tiles this cache can hold, after the budget was rounded and clamped. */
size_t mesh_map_tile_cache_slots(const struct mesh_map_tile_cache *cache);

/* What the pixels actually cost, for a log line and for the budget the roadmap keeps. Excludes
   the decoder's own static blocks, which mesh_map_tile_decoder_bytes() answers for. */
size_t mesh_map_tile_cache_bytes(const struct mesh_map_tile_cache *cache);

/* How many slots are readable right now. For tests and diagnostics; no screen reads it. */
size_t mesh_map_tile_cache_ready(const struct mesh_map_tile_cache *cache);

#ifdef __cplusplus
}
#endif

#endif /* MESH_MAP_TILE_CACHE_H */
