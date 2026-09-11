#include "mesh/map/tile_image.h"

#include "wuffs_png.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/*
 * The PNG decoder, and the only file in the client that includes Wuffs.
 *
 * Wuffs allocates nothing and has no opinion about where its state lives: the caller hands it a
 * struct and a scratch buffer and it uses those and nothing else. So the whole of what decoding
 * a tile costs is the two blocks below, and it is a constant rather than a total - which is the
 * property that matters on an event loop, because a decode that cannot ask for memory cannot
 * fail for want of it and cannot stop to look.
 *
 * They are static rather than allocated, for two reasons that are both about not having a
 * failure path. There is exactly one decode in flight ever - the client is one epoll loop with
 * no threads in it - so a second copy is not something anything could use. And static storage is
 * zero-page-backed until it is touched, so a client that never opens a map never faults these in
 * and pays the address space rather than the memory; a lazy malloc would buy the same thing and
 * add an allocation that can fail, on the one path where there is nothing sensible to do about
 * it.
 */

/*
 * Two sizes Wuffs reports rather than documents, so both are checked before they are used.
 *
 * That is not belt and braces. The decoder's struct is deliberately *opaque* in C - upstream
 * says in as many words that its fields and its size "aren't guaranteed to be stable across
 * Wuffs versions" - so its size is a number this file can only learn by asking. 48 KiB against
 * the 44,632 bytes this version asks for is enough headroom for one that grows a little and not
 * enough to hide one that grows a lot, which is what the check at the decode is for.
 *
 * The work buffer is the one that is easy to get wrong, and it was: Wuffs wants
 * `width * bytes_per_pixel * height + width` bytes, so it scales with the *colour type* of the
 * file and not only with the tile size the client fixed. Measured on this version, at 256 px
 * square:
 *
 *     8-bit grey, 8-bit palette   1 byte a pixel     65,792
 *     24-bit RGB                  3 bytes a pixel   196,864
 *     32-bit RGBA                 4 bytes a pixel   262,400
 *     16-bit-per-channel RGBA     8 bytes a pixel   524,544
 *
 * Sized for the third. A pack builder quantises to palette and the roadmap keeps 24-bit as the
 * expensive bracket, but a style with transparency in it is an ordinary thing to publish and
 * nothing stops one reaching this, so paying 256 KiB to decode it is better than refusing it.
 * Sixteen bits a channel is where the line goes: it is twice the memory for precision no panel
 * on this device can show, no tile server emits it, and the refusal is stated
 * (MESH_MAP_TILE_UNSUPPORTED) rather than a crash or a wrong picture.
 */
#define TILE_DECODER_BYTES (48U * 1024U)
#define TILE_WORK_BYTES 262400U

static _Alignas(max_align_t) uint8_t g_decoder[TILE_DECODER_BYTES];
static uint8_t g_work[TILE_WORK_BYTES];

size_t mesh_map_tile_decoder_bytes(void) { return sizeof g_decoder + sizeof g_work; }

int mesh_map_tile_decode(const uint8_t *encoded, size_t len, uint8_t *pixels, size_t pixels_len) {
    if (encoded == NULL || len == 0U || pixels == NULL) {
        return -EINVAL;
    }
    if (pixels_len < MESH_MAP_TILE_IMAGE_BYTES) {
        return -ENOBUFS;
    }

    /* The decoder lives in our own bytes rather than in a struct, because the struct is opaque
       in C by upstream's choice - see the sizes above. Its real size has to be passed as it is
       rather than as the block's, or Wuffs refuses the receiver as the wrong shape. */
    if (sizeof__wuffs_png__decoder() > sizeof g_decoder) {
        return -ENOBUFS;
    }
    wuffs_png__decoder *const decoder = (wuffs_png__decoder *)(void *)g_decoder;
    wuffs_base__status status =
        wuffs_png__decoder__initialize(decoder, sizeof__wuffs_png__decoder(), WUFFS_VERSION, 0);
    if (!wuffs_base__status__is_ok(&status)) {
        return -EINVAL;
    }

    /*
     * Wuffs reads through a cursor over a buffer it does not own, and `true` is the promise that
     * these bytes are the whole file rather than the start of a stream. It is what turns a
     * truncated tile into an error here instead of a request for more that nobody can answer -
     * the decoder would otherwise sit waiting on the rest of a file that has no rest.
     */
    wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader((uint8_t *)encoded, len, true);

    wuffs_base__image_config config;
    status = wuffs_png__decoder__decode_image_config(decoder, &config, &source);
    if (!wuffs_base__status__is_ok(&status)) {
        return -EILSEQ;
    }
    if (wuffs_base__pixel_config__width(&config.pixcfg) != (uint32_t)MESH_MAP_TILE_SIZE ||
        wuffs_base__pixel_config__height(&config.pixcfg) != (uint32_t)MESH_MAP_TILE_SIZE) {
        return -EINVAL;
    }

    /*
     * Asking for the format we want is what makes this free.
     *
     * A map pack is quantised to palette PNGs on the host, so the file's own pixels are indices;
     * the decoder has to walk them through a palette into *something* whatever we ask for, and
     * asking for the panel's order costs the same pass as asking for the file's. Converting
     * afterwards would be a second pass over 65,536 pixels that buys nothing.
     */
    wuffs_base__pixel_config__set(&config.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, (uint32_t)MESH_MAP_TILE_SIZE,
                                  (uint32_t)MESH_MAP_TILE_SIZE);

    wuffs_base__pixel_buffer destination;
    status = wuffs_base__pixel_buffer__set_from_slice(
        &destination, &config.pixcfg, wuffs_base__make_slice_u8(pixels, MESH_MAP_TILE_IMAGE_BYTES));
    if (!wuffs_base__status__is_ok(&status)) {
        return -ENOBUFS;
    }

    /*
     * What this file is: a PNG whose pixels need more scratch than a tile is budgeted. In
     * practice that is sixteen bits a channel, per the table above - and it is -ENOTSUP rather
     * than -ENOBUFS because nothing the caller did is wrong and no buffer it could pass would
     * help. It doubles as the version-bump guard: a Wuffs that changed this arithmetic would
     * otherwise produce a client that stops drawing tiles for a reason nothing on the device
     * could report.
     */
    const wuffs_base__range_ii_u64 wanted = wuffs_png__decoder__workbuf_len(decoder);
    if (wanted.min_incl > (uint64_t)sizeof g_work) {
        return -ENOTSUP;
    }

    status = wuffs_png__decoder__decode_frame(
        decoder, &destination, &source, WUFFS_BASE__PIXEL_BLEND__SRC,
        wuffs_base__make_slice_u8(g_work, sizeof g_work), NULL);
    if (!wuffs_base__status__is_ok(&status)) {
        return -EILSEQ;
    }
    return 0;
}
