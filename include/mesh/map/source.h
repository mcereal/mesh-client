#ifndef MESH_MAP_SOURCE_H
#define MESH_MAP_SOURCE_H

#include "mesh/map/tile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where a tile's bytes come from, and what the thing they came from has to say about itself.
 *
 * One interface with one implementation, which is the point rather than an admission: a pack on
 * the SD card is the only source there is and the only one step 4 needs, and the seam exists so
 * that the *screen* above it never learns which one it got. docs/maps-roadmap.md keeps an HTTP
 * source under step 5, gated on a provider that needs no secret from the reader; if that day
 * comes it arrives here and nothing above changes.
 *
 * A source hands back *encoded* bytes - a PNG as it sits on disk. Decoding is the next layer
 * up, because a decoder is a dependency and this is a file offset and a read: keeping them
 * apart is what lets the pack be tested with no decoder linked in, which is how the tests below
 * a decoder-shaped hole are written.
 *
 * Nothing here includes a protobuf, the UI store or the framebuffer.
 */

/* The longest attribution a pack may carry. Every raster style that permits offline use asks
   for a line of credit and they are short - "(c) OpenStreetMap contributors" is thirty bytes -
   and this is drawn in a corner of the map, so a pack cannot buy itself a paragraph. */
#define MESH_MAP_ATTRIBUTION_MAX 64

/* What the pack calls itself: a style or a region, for a log line and for the settings row that
   eventually lists what is installed. Never drawn instead of the attribution. */
#define MESH_MAP_SOURCE_NAME_MAX 32

/*
 * The largest encoded tile a source will hand back.
 *
 * A 256x256 palette PNG of a dense city runs 10-25 KiB and the 24-bit bracket measured on the
 * Brick ran 25 KiB; a megabyte is two orders of magnitude of headroom and still small enough
 * that the buffer is a caller's local rather than an allocation. What it really is, is the
 * bound that stops a corrupt index turning a tile read into a request for the whole file.
 */
#define MESH_MAP_TILE_BYTES_MAX (1024U * 1024U)

/* What a tile is encoded as. One value, because the pack builder quantises on the host and the
   decoder the Brick was measured with reads PNG: a format byte that could say anything is a
   format byte a reader has to be able to refuse, which is the whole reason it is on the wire. */
enum mesh_map_tile_format {
    MESH_MAP_TILE_FORMAT_PNG = 1,
};

/*
 * What a source says about itself, and what the map draws its attribution from.
 *
 * The zoom range and the coverage are *derived* from the tiles a pack actually holds rather
 * than declared in its header, which is the same rule the app bar's back arrow follows: a
 * second opinion about what is in a file is an opinion that can be wrong, and the way it goes
 * wrong is a pack that claims a city and holds a suburb. A builder that lied about its own
 * bounds would be believed by every screen that drew them.
 *
 * The coverage is a bounding box in the same fixed-point 1e-7 degrees as every other coordinate
 * in the client, taken from the tiles' own corners - so it is the area the pack *could* answer
 * for, and not a promise that every tile inside it is present. A pack is allowed to be sparse;
 * a hole reads as a tile that is missing, which is a state the map has to draw anyway.
 */
struct mesh_map_source_info {
    uint16_t tile_size;
    uint8_t format;
    uint8_t min_zoom;
    uint8_t max_zoom;
    uint32_t tiles;
    int32_t north_i;
    int32_t south_i;
    int32_t east_i;
    int32_t west_i;
    /* Seconds since the epoch, or 0 when the builder did not say. A pack is a photograph of a
       map at a moment and ages the way a paper one does, so the date is metadata rather than a
       nicety - and a reader with no clock can still show it, because it is absolute. */
    int64_t generated_s;
    char attribution[MESH_MAP_ATTRIBUTION_MAX];
    char name[MESH_MAP_SOURCE_NAME_MAX];
};

struct mesh_map_source;

/*
 * Reads one tile's encoded bytes into `out`.
 *
 * Returns the length written; 0 when this source does not hold that tile, which is an ordinary
 * answer and not a failure - every pack is a rectangle of the world with holes in it; and
 * -errno when the read itself failed, which is a card that has been pulled out.
 *
 * `cap` is what the caller has room for. A tile longer than that is refused rather than
 * truncated, because half a PNG decodes into a picture of something.
 */
typedef int (*mesh_map_source_read_fn)(struct mesh_map_source *source, struct mesh_map_tile_key key,
                                       uint8_t *out, size_t cap);

/* Whether the source holds that tile, without reading its bytes. A separate entry point rather
   than a zero-length read, because for a pack the answer is already in RAM and the point is not
   to touch the card for it. */
typedef bool (*mesh_map_source_has_fn)(const struct mesh_map_source *source,
                                       struct mesh_map_tile_key key);

typedef void (*mesh_map_source_close_fn)(struct mesh_map_source *source);

struct mesh_map_source {
    struct mesh_map_source_info info;
    mesh_map_source_read_fn read;
    mesh_map_source_has_fn has;
    mesh_map_source_close_fn close;
    void *context;
};

/*
 * Opens a tile pack and fills in `out`.
 *
 * 0 on success, -errno otherwise: -ENOENT for a path that is not there, -EINVAL for a file that
 * is not a pack or whose index does not describe itself honestly, -ENOMEM when the index will
 * not fit. Everything the reader will later trust is checked here, once, so that a tile read is
 * a binary search and one pread and can be wrong about nothing.
 *
 * On failure `out` is zeroed and there is nothing to close.
 */
int mesh_map_source_open_pack(const char *path, struct mesh_map_source *out);

/* Reads one tile. The same contract as the function pointer above, with the NULL checks a
   caller would otherwise write at every call site. */
int mesh_map_source_read(struct mesh_map_source *source, struct mesh_map_tile_key key, uint8_t *out,
                         size_t cap);

/* Closes a source and zeroes it. Safe on a source that was never opened, so a caller can close
   unconditionally on its way out. */
void mesh_map_source_close(struct mesh_map_source *source);

/* Whether a source holds a tile at all, without reading its bytes - what a renderer asks before
   it decides whether a blank square is a missing tile or a tile still on its way. */
bool mesh_map_source_has(const struct mesh_map_source *source, struct mesh_map_tile_key key);

#ifdef __cplusplus
}
#endif

#endif /* MESH_MAP_SOURCE_H */
