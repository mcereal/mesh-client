#ifndef MESH_MAP_TILE_H
#define MESH_MAP_TILE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The tile pyramid's own vocabulary: how big a tile is, which levels the client offers, how one
 * is addressed, and which of them a box of pixels covers.
 *
 * It is its own header because two modules that must not include each other both need it. The
 * viewport turns a place into a set of tile keys and includes the projection to do it; a source
 * turns one key into bytes and includes a file. Neither has any business dragging the other in,
 * and a key is four numbers - so the four numbers live here and the two sides agree about them
 * without meeting.
 *
 * Nothing here includes a protobuf, the UI store, the framebuffer or a filesystem header, and
 * nothing here does any arithmetic that needs <math.h>.
 */

/*
 * A world is 256 pixels square at zoom 0 and doubles each level.
 *
 * A raster tile set is cut this way, so measuring the world in these units is what lets a
 * basemap drop in without every coordinate in the client being rescaled. Declared before there
 * was a basemap, because it costs nothing to agree with the pyramid before there is one.
 */
#define MESH_MAP_TILE_SIZE 256

/*
 * The zoom range the client offers.
 *
 * 0 is the whole world in one 256 px tile, which on this panel is a picture of Earth the size of
 * a postage stamp - useless to look at and exactly right as the far end of a zoom-out, because
 * a mesh that spans two continents has to be able to show both. 18 is roughly building-scale,
 * which is finer than any position on a mesh is reported to: `precision_bits` rounds a fix to
 * hundreds of metres, and offering a zoom where two markers a metre apart are separable would
 * be the client claiming a precision it was never sent.
 */
#define MESH_MAP_ZOOM_MIN 0
#define MESH_MAP_ZOOM_MAX 18

/*
 * The zoom a map opens at when there is nothing to fit - one marker, or none.
 *
 * About a kilometre across the panel, which is the scale a handheld's "where am I" wants: close
 * enough that a street would be recognisable if a basemap were under it, wide enough that a
 * neighbour a few hundred metres away is on the same screen.
 */
#define MESH_MAP_ZOOM_DEFAULT 14

/*
 * One tile's address, in the XYZ convention every raster pyramid on the web is cut to: x counts
 * east from 180 degrees west and y counts *south* from the top of the picture.
 *
 * Not TMS, whose y counts the other way. The difference is one subtraction and it is the single
 * most common bug in tile code, so the client has exactly one convention and any storage that
 * disagrees converts inside its own reader rather than anywhere a screen can see.
 */
struct mesh_map_tile_key {
    uint8_t zoom;
    uint32_t x;
    uint32_t y;
};

/*
 * The block of tiles a box of pixels covers, as a rectangle in tile space plus where its corner
 * lands.
 *
 * A rectangle rather than a list of keys, because that is what it always is: a box intersects a
 * grid in a contiguous block, and handing back twenty keys would be twenty copies of arithmetic
 * a caller can do with two additions. It also makes the answer a fixed size, which is what lets
 * a renderer ask for it every frame without an allocation.
 *
 * `origin_x` and `origin_y` are where the top-left corner of tile (`x0`, `y0`) lands in the box,
 * and are the reason nothing downstream has to know where the view is: a blit is that corner
 * plus a multiple of MESH_MAP_TILE_SIZE. Both are usually in (-MESH_MAP_TILE_SIZE, 0], because
 * the first tile normally starts at or before the box's own corner. `origin_y` is the one that
 * can come back positive: a view panned against the top of the world has no tiles above it, so
 * the first row it can report starts partway down the box and the pixels above are the world's
 * own edge.
 *
 * `columns` may exceed the world's width at this zoom, and that is not an error: at zoom 2 the
 * world is 1024 pixels and this panel is wider, so the same tile is drawn twice with a world
 * between them. mesh_map_tile_span_key() is what folds a column back onto the world.
 *
 * `rows` is *clamped* to the world instead of wrapped, because the pyramid has no tiles above
 * its top row or below its bottom one - a map panned against the top of the world has empty
 * pixels there, which is what every map does and is the honest picture.
 */
struct mesh_map_tile_span {
    uint8_t zoom;
    int32_t x0;
    int32_t y0;
    int32_t columns;
    int32_t rows;
    int32_t origin_x;
    int32_t origin_y;
};

/*
 * The key at a column and row of a span, with the column folded onto the world.
 *
 * False - and `out` zeroed - when the column or row is outside the span. The fold is the only
 * arithmetic here that is not obvious: a span may run off either side of the world, so column
 * -1 at zoom 3 is tile 7, which is the same tile the reader would reach by panning the other
 * way for seven screens.
 */
bool mesh_map_tile_span_key(const struct mesh_map_tile_span *span, int32_t column, int32_t row,
                            struct mesh_map_tile_key *out);

/* Whether a key names a tile that can exist: a zoom the client offers, and a position inside
   the world at that zoom. Asked by every source before it looks anything up, so a corrupt pack
   and a caller's arithmetic mistake get the same refusal in the same place. */
bool mesh_map_tile_key_valid(struct mesh_map_tile_key key);

#ifdef __cplusplus
}
#endif

#endif /* MESH_MAP_TILE_H */
