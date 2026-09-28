#include "mesh/map/tile_image.h"

#include "inkwell/codec/png.h"

#include <errno.h>
#include <string.h>

/*
 * The tile-shaped half of a PNG decode. The decoder is inkwell's; what is here is the two
 * buffers a 256-square tile needs and the size it is fixed at.
 *
 * They are static rather than allocated, for two reasons that are both about not having a
 * failure path. There is exactly one decode in flight ever - the client is one epoll loop with
 * no threads in it - so a second copy is not something anything could use. And static storage is
 * zero-page-backed until it is touched, so a client that never opens a map never faults these in
 * and pays the address space rather than the memory; a lazy malloc would buy the same thing and
 * add an allocation that can fail, on the one path where there is nothing sensible to do about
 * it.
 *
 * The work buffer is sized for the 4-bytes-a-pixel bracket, which is the choice
 * inkwell_png_workbuf_bytes() exists to let a caller make. A pack builder quantises to palette
 * and the roadmap keeps 24-bit as the expensive bracket, but a style with transparency in it is
 * an ordinary thing to publish and nothing stops one reaching this, so paying 256 KiB to decode
 * it is better than refusing it. Sixteen bits a channel is where the line goes: it is twice the
 * memory for precision no panel on this device can show, no tile server emits it, and the
 * refusal is stated (MESH_MAP_TILE_UNSUPPORTED) rather than a crash or a wrong picture.
 */
#define TILE_WORK_BYTES 262400U /* inkwell_png_workbuf_bytes(256, 256, 4) */

static struct inkwell_png_decoder g_decoder;
static uint8_t g_work[TILE_WORK_BYTES];

size_t mesh_map_tile_decoder_bytes(void) { return sizeof g_decoder + sizeof g_work; }

int mesh_map_tile_decode(const uint8_t *encoded, size_t len, uint8_t *pixels, size_t pixels_len) {
    return inkwell_png_decode_bgra(&g_decoder, encoded, len, (uint32_t)MESH_MAP_TILE_SIZE,
                                   (uint32_t)MESH_MAP_TILE_SIZE, pixels, pixels_len, g_work,
                                   sizeof g_work);
}

int mesh_map_tile_overzoom(const uint8_t *ancestor_pixels, struct mesh_map_tile_key ancestor,
                           struct mesh_map_tile_key child, uint8_t *pixels, size_t pixels_len) {
    if (ancestor_pixels == NULL || pixels == NULL) {
        return -EINVAL;
    }
    if (pixels_len < MESH_MAP_TILE_IMAGE_BYTES) {
        return -ENOBUFS;
    }
    struct mesh_map_tile_key above;
    if (child.zoom <= ancestor.zoom ||
        (unsigned)(child.zoom - ancestor.zoom) > MESH_MAP_TILE_OVERZOOM_LEVELS ||
        !mesh_map_tile_ancestor(child, ancestor.zoom, &above) || above.x != ancestor.x ||
        above.y != ancestor.y) {
        return -EINVAL;
    }

    const unsigned shift = (unsigned)(child.zoom - ancestor.zoom);
    const size_t repeat = (size_t)1 << shift;
    const size_t part = (size_t)MESH_MAP_TILE_SIZE >> shift;
    /* Where the child's square starts inside the ancestor's, in the ancestor's pixels. */
    const size_t left = (size_t)(child.x - (ancestor.x << shift)) * part;
    const size_t top = (size_t)(child.y - (ancestor.y << shift)) * part;
    const size_t stride = (size_t)MESH_MAP_TILE_SIZE * MESH_MAP_TILE_PIXEL_BYTES;

    /* One source row widened into the first of its `repeat` rows, and copied down into the
       rest: the widening is per pixel and the copy is a memcpy, which is most of the tile. */
    for (size_t row = 0; row < part; ++row) {
        const uint8_t *in =
            ancestor_pixels + (top + row) * stride + left * MESH_MAP_TILE_PIXEL_BYTES;
        uint8_t *const first = pixels + row * repeat * stride;
        uint8_t *out = first;
        for (size_t column = 0; column < part; ++column, in += MESH_MAP_TILE_PIXEL_BYTES) {
            for (size_t copy = 0; copy < repeat; ++copy, out += MESH_MAP_TILE_PIXEL_BYTES) {
                memcpy(out, in, MESH_MAP_TILE_PIXEL_BYTES);
            }
        }
        for (size_t copy = 1; copy < repeat; ++copy) {
            memcpy(first + copy * stride, first, stride);
        }
    }
    return 0;
}
