#include "mesh/utils/inflate.h"

#include "mesh_wuffs.h"

#include <stdlib.h>

/*
 * Wuffs' deflate decoder and CRC32 hasher, behind the two calls in mesh/utils/inflate.h.
 *
 * Both structs are allocated rather than static, unlike the tile decoder next to the map: that
 * one runs on every pan, and this runs once per firmware download, on a path that already reads
 * the member into memory and has a failure row for doing so.
 */

enum mesh_inflate_result mesh_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
                                      size_t out_cap, size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (in == NULL || in_len == 0U || out == NULL || out_len == NULL) {
        return MESH_INFLATE_CORRUPT;
    }
    wuffs_deflate__decoder *const decoder = wuffs_deflate__decoder__alloc();
    if (decoder == NULL) {
        return MESH_INFLATE_NO_MEMORY;
    }

    /*
     * One call, because both ends are whole: the source is marked closed, so running out of it
     * is an error rather than a suspension, and the destination is the entire output, so a
     * back-reference reads straight out of it and no history has to be carried between calls.
     * Deflate's work buffer is declared as one byte at worst, and this version's decoder never
     * touches it, so an empty slice is enough.
     */
    wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader((uint8_t *)in, in_len, true);
    wuffs_base__io_buffer destination = wuffs_base__ptr_u8__writer(out, out_cap);
    const wuffs_base__status status = wuffs_deflate__decoder__transform_io(
        decoder, &destination, &source, wuffs_base__empty_slice_u8());
    free(decoder);

    *out_len = destination.meta.wi;
    if (wuffs_base__status__is_ok(&status)) {
        return MESH_INFLATE_OK;
    }
    if (status.repr == wuffs_base__suspension__short_write) {
        return MESH_INFLATE_TOO_LONG;
    }
    return MESH_INFLATE_CORRUPT;
}

bool mesh_crc32(const uint8_t *bytes, size_t len, uint32_t *out) {
    if (out == NULL || (bytes == NULL && len != 0U)) {
        return false;
    }
    wuffs_crc32__ieee_hasher *const hasher = wuffs_crc32__ieee_hasher__alloc();
    if (hasher == NULL) {
        return false;
    }
    *out = wuffs_crc32__ieee_hasher__update_u32(hasher,
                                                wuffs_base__make_slice_u8((uint8_t *)bytes, len));
    free(hasher);
    return true;
}
