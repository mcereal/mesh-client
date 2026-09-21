#include "mesh/map/tile_image.h"

#include "inkwell/codec/png.h"

#include <errno.h>

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
