#ifndef MESH_MAP_TILE_IMAGE_H
#define MESH_MAP_TILE_IMAGE_H

#include "mesh/map/tile.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A tile's bytes turned into pixels: the one function that knows what a PNG is.
 *
 * It is a seam rather than a wrapper. Nothing above it names Wuffs, includes its header or
 * knows a PNG from a JPEG - what this promises is a fixed-size block of pixels in a stated
 * order, which is the only thing a cache or a blit has any business relying on. The decoder
 * behind it was chosen by measurement (docs/maps-roadmap.md): twice stb_image's speed on the
 * palette tiles a map pack is built from, 1.24 ms against 2.41 for one tile on the Brick, and
 * memory-safe by construction, which matters more than the speed does for a decoder whose input
 * is a file this client did not write.
 *
 * There is no decode *state* in this API and no handle to open. The client is one epoll loop
 * with no threads in it, so there is exactly one decode in flight ever, and a handle would be a
 * thing nobody could want two of.
 */

/*
 * Four bytes a pixel, in the order **blue, green, red, alpha**.
 *
 * Stated in bytes rather than as a word, because a word is a claim about the host's endianness
 * and this is a claim about memory. On this hardware - aarch64, little-endian - reading four of
 * them as a uint32_t gives 0xAARRGGBB, which is the Brick's own panel word: the display layer
 * composites with per-pixel alpha, so the alpha byte is load-bearing rather than padding, and a
 * pixel that arrives 0x00RRGGBB is invisible rather than black. An opaque PNG decodes with
 * alpha 255 throughout, which is what a basemap tile always is.
 *
 * The framebuffer's real channel layout is read from the kernel at runtime rather than assumed,
 * so the blit converts when it has to. What this fixes is that there is one format to convert
 * *from*, decided once, instead of a decoder handing back whatever the file happened to contain.
 */
#define MESH_MAP_TILE_PIXEL_BYTES 4U

/* One decoded tile: 256 KiB. The number docs/maps-roadmap.md budgets a tile cache in. */
#define MESH_MAP_TILE_IMAGE_BYTES                                                                  \
    ((size_t)MESH_MAP_TILE_SIZE * (size_t)MESH_MAP_TILE_SIZE * (size_t)MESH_MAP_TILE_PIXEL_BYTES)

/*
 * Decodes one map tile.
 *
 * `encoded` is a tile exactly as a source handed it over. `pixels` is where it lands and must be
 * at least MESH_MAP_TILE_IMAGE_BYTES; the whole of it is written.
 *
 * 0 on success. Otherwise:
 *
 *   -EINVAL   the arguments are not usable, or the image is not MESH_MAP_TILE_SIZE square
 *   -ENOBUFS  `pixels` is smaller than a tile
 *   -EILSEQ   the bytes are not a PNG, or are a broken one
 *   -ENOTSUP  a PNG this client does not decode: sixteen bits a channel, in practice
 *
 * The last three are worth keeping apart, because they are somebody else's problem in three
 * different directions. A buffer too small is this client asking wrong, and is a bug. A broken
 * image is a file being broken, and is a Tuesday - a pack lives on a card somebody pulled out
 * of a laptop. And a PNG we decline is a *pack builder* having produced something no map style
 * needs, which is the one of the three a user can fix.
 *
 * Only a tile exactly MESH_MAP_TILE_SIZE square is accepted, and that is a refusal rather than a
 * scale. The pack builder rejects any other size on the host and a pack states its tile size in
 * its header, so a wrong one here means those two disagree with the file's own contents - and
 * the honest answer to three disagreeing authorities is none of them.
 */
int mesh_map_tile_decode(const uint8_t *encoded, size_t len, uint8_t *pixels, size_t pixels_len);

/*
 * How much memory decoding holds, for the budget docs/maps-roadmap.md keeps.
 *
 * It is a constant rather than a total that grows with use: the decoder's own state and its
 * scratch buffer are allocated once, statically, and every tile after the first reuses them.
 * Nothing is allocated per tile, which is the property that matters on an event loop - a decode
 * cannot fail for want of memory, and cannot pause to find some.
 */
size_t mesh_map_tile_decoder_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_MAP_TILE_IMAGE_H */
