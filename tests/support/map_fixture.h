#ifndef MESH_TEST_MAP_FIXTURE_H
#define MESH_TEST_MAP_FIXTURE_H

#include "mesh/map/tile.h"

#include <stddef.h>
#include <stdint.h>

/*
 * A tile pack holding pictures rather than bytes.
 *
 * tests/suites/map_pack.c builds packs whose "tiles" are arbitrary bytes, because what it is
 * about is the index and the pread - a reader that validated a PNG signature at open would have
 * to read every tile in the file to open it. Anything that goes on to *draw* a tile needs the
 * bytes to be a real picture, and needs to know what colour that picture is so a frame can be
 * looked for it. That is what this is: one solid colour per tile, encoded as the palette PNG a
 * pack builder produces, and told apart by a colour derived from the tile's place in the list.
 *
 * A second suite needed it, which is what moved it here - the rule at the top of tests/.
 */

/* A pack on disk. The caller removes it, which is why it carries its own path. */
struct mesh_test_map_pack {
    char path[64];
};

/*
 * Writes a pack holding one solid tile per key.
 *
 * 0 on success, a negative errno otherwise. The keys are sorted into the order a pack's index
 * has to be in, so the nth tile is the nth key in (zoom, x, y) order rather than the nth the
 * caller passed. The nth tile is filled with
 * mesh_test_map_tile_colour(n), so a test that renders the pack knows what to look for and two
 * packs written with different offsets are visibly different pictures of the same place - which
 * is what the rule about clearing a cache on a source swap needs to be checked against.
 */
int mesh_test_map_pack_write(struct mesh_test_map_pack *pack, const struct mesh_map_tile_key *keys,
                             size_t count, unsigned shade, const char *name,
                             const char *attribution);

/* Removes the file. Safe on a pack that was never written. */
void mesh_test_map_pack_remove(struct mesh_test_map_pack *pack);

/* The colour the nth tile of a pack written with `shade` is filled with, as r, g, b. */
void mesh_test_map_tile_colour(size_t index, unsigned shade, uint8_t rgb[3]);

/*
 * The tile the centre of a view falls on, and the block of keys around it.
 *
 * Asked of the viewport rather than worked out with logarithms here, so a fixture covers the
 * tiles the renderer will actually ask for - including at the antimeridian, where doing the
 * arithmetic twice is how the two copies come to disagree. Fills `out` with
 * `columns * rows` keys and returns how many were written.
 */
size_t mesh_test_map_keys_around(int32_t latitude_i, int32_t longitude_i, uint8_t zoom, int columns,
                                 int rows, struct mesh_map_tile_key *out, size_t cap);

#endif /* MESH_TEST_MAP_FIXTURE_H */
