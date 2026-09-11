#include "mesh/map/tile.h"

#include <string.h>

/* How many tiles across the world is at a zoom. 1 << 18 is 262144, so this stays well inside
   an int64 and the fold below never overflows. */
static int64_t tile_world_width(uint8_t zoom) { return (int64_t)1 << zoom; }

bool mesh_map_tile_key_valid(struct mesh_map_tile_key key) {
    if (key.zoom > MESH_MAP_ZOOM_MAX) {
        return false;
    }
    const int64_t width = tile_world_width(key.zoom);
    return (int64_t)key.x < width && (int64_t)key.y < width;
}

bool mesh_map_tile_span_key(const struct mesh_map_tile_span *span, int32_t column, int32_t row,
                            struct mesh_map_tile_key *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (span == NULL || out == NULL || column < 0 || row < 0 || column >= span->columns ||
        row >= span->rows) {
        return false;
    }

    const int64_t width = tile_world_width(span->zoom);
    /* The fold, written as a modulo that cannot come back negative: C's % keeps the sign of the
       left operand, and a span may start west of the world's own left edge. */
    int64_t x = ((int64_t)span->x0 + column) % width;
    if (x < 0) {
        x += width;
    }
    const int64_t y = (int64_t)span->y0 + row;
    if (y < 0 || y >= width) {
        return false;
    }

    out->zoom = span->zoom;
    out->x = (uint32_t)x;
    out->y = (uint32_t)y;
    return true;
}
