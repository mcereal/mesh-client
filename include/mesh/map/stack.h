#ifndef MESH_MAP_STACK_H
#define MESH_MAP_STACK_H

#include "mesh/map/source.h"
#include "mesh/map/tile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Several tile packs drawn as one map: a world base under every region the reader installed.
 *
 * A region pack is a state or a country cut to zoom 13 and the world base stops at 6, so on
 * their own neither is a map. Together they are, as long as each tile is drawn by the pack that
 * knows that ground best - and the rule for that is the whole of this module.
 *
 * **The pack that answers from its deepest tile draws.** Every pack is asked what it would read
 * to draw a key: the key itself when it holds it, or the key's ancestor at its deepest level when
 * the key is past it (MESH_MAP_TILE_OVERZOOM_LEVELS at most). The one whose answer is deepest
 * wins, so at zoom 11 a region's own tile beats the world base's zoom-6 tile enlarged.
 *
 * **A tie goes to the pack that stops sooner.** At zoom 4 both the world base and a region hold
 * the tile, but a region's shallow tiles were drawn from that region alone - the rest of the
 * continent is blank on them. The pack with the shallower deepest level is the broader one, and
 * the world base is the broadest there is. Past that, the order the packs were added in decides,
 * which is name order (ignoring case) in a directory, so two regions meeting at a border draw the
 * same way on every launch.
 *
 * The answer depends only on the key and the set of packs, which is what lets one tile cache hold
 * the lot under the key alone: nothing about a pixel in it says which file it came from, and it
 * does not need to. Adding or removing a pack changes answers, so it is a new stack and a
 * cleared cache.
 *
 * Nothing here draws, decodes or allocates past the packs' own indexes.
 */

/*
 * The most packs one map stands on.
 *
 * Each holds its whole index in RAM at 24 bytes a tile - a z13 state is a megabyte or so - so
 * the cap is a bound on memory a card full of downloads could otherwise take, not on anything a
 * reader would choose. Sixteen is a world base and every region anyone travels between.
 */
#define MESH_MAP_STACK_PACKS_MAX 16U

struct mesh_map_stack {
    struct mesh_map_source packs[MESH_MAP_STACK_PACKS_MAX];
    size_t count;
};

/* Which pack draws a key, and the tile of that pack it draws it from - the key itself, or its
   ancestor at the pack's deepest level. */
struct mesh_map_stack_pick {
    size_t pack;
    struct mesh_map_tile_key key;
};

/* An empty stack. A stack is plain memory; this is a memset, named so a caller does not have to
   know that. */
void mesh_map_stack_init(struct mesh_map_stack *stack);

/*
 * Opens the pack at `path` and puts it on the stack. 0, -ENOSPC when the stack is full, or the
 * pack reader's -errno - in which case the stack is as it was.
 */
int mesh_map_stack_add(struct mesh_map_stack *stack, const char *path);

/*
 * Adds every `*.mctp` in `dir`, in name order ignoring case, and returns how many opened.
 *
 * A name is matched without regard to case, because a card written from another computer is
 * FAT32 and the name may have arrived as MAP.MCTP. A download in progress is `<name>.part` and
 * does not match. A pack that will not open is logged and skipped: one damaged file on a card is
 * a hole in the map, not a reason to draw none of it. A directory that is not there opens
 * nothing and says nothing - no packs installed is the ordinary case.
 */
size_t mesh_map_stack_add_dir(struct mesh_map_stack *stack, const char *dir);

/*
 * Which pack draws `key`, by the rule at the top of this header. False when none of them can:
 * the key is outside every pack, or deeper past each one than it will be enlarged.
 *
 * Answered from the indexes in RAM; nothing is read from the card.
 */
bool mesh_map_stack_resolve(const struct mesh_map_stack *stack, struct mesh_map_tile_key key,
                            struct mesh_map_stack_pick *out);

/* Reads the tile a pick names. The same contract as mesh_map_source_read(), and -EINVAL for a
   pick that names no pack on this stack. */
int mesh_map_stack_read(struct mesh_map_stack *stack, const struct mesh_map_stack_pick *pick,
                        uint8_t *out, size_t cap);

/* The credit to draw under the map: the first pack's that has one. Every pack this client's
   pipeline builds says the same thing, and a map carries one credit line, not one a pack. "" on
   an empty stack. */
const char *mesh_map_stack_attribution(const struct mesh_map_stack *stack);

/* Closes every pack and empties the stack. Safe on one that is already empty. */
void mesh_map_stack_close(struct mesh_map_stack *stack);

#ifdef __cplusplus
}
#endif

#endif /* MESH_MAP_STACK_H */
