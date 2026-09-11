/*
 * Size probe: the smallest program that reaches one tile path, linked the way the client is
 * (static, -Os, sections collected). Built once per PROBE_* and once with none; the difference
 * in stripped size is what that path would add to meshclient. The inputs come from argv so the
 * compiler cannot prove any of it dead.
 */
#include <stddef.h>
#include <stdint.h>

#if defined(PROBE_STB)
#include "stb_image.h"
#elif defined(PROBE_WUFFS)
#include "tb_wuffs.h"
#include <stdlib.h>
#elif defined(PROBE_SQLITE)
#include "sqlite3.h"
#endif

int main(int argc, char **argv) {
    const uint8_t *src = (const uint8_t *)argv[0];
    size_t len = (size_t)argc * 64U;
    (void)src;
    (void)len;
#if defined(PROBE_STB)
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load_from_memory(src, (int)len, &w, &h, &n, 4);
    int ok = px != NULL;
    stbi_image_free(px);
    return ok;
#elif defined(PROBE_WUFFS)
    wuffs_png__decoder *dec = wuffs_png__decoder__alloc();
    if (dec == NULL) {
        return 1;
    }
    wuffs_base__io_buffer io = wuffs_base__ptr_u8__reader((uint8_t *)src, len, true);
    wuffs_base__image_config ic;
    wuffs_base__status st = wuffs_png__decoder__decode_image_config(dec, &ic, &io);
    if (wuffs_base__status__is_ok(&st)) {
        uint32_t w = wuffs_base__pixel_config__width(&ic.pixcfg);
        uint32_t h = wuffs_base__pixel_config__height(&ic.pixcfg);
        wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
                                      WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);
        uint8_t *px = malloc((size_t)w * h * 4U);
        uint64_t wl = wuffs_png__decoder__workbuf_len(dec).max_incl;
        uint8_t *work = malloc((size_t)wl + 1U);
        wuffs_base__pixel_buffer pb;
        st = wuffs_base__pixel_buffer__set_from_slice(
            &pb, &ic.pixcfg, wuffs_base__make_slice_u8(px, (size_t)w * h * 4U));
        if (px != NULL && work != NULL && wuffs_base__status__is_ok(&st)) {
            st =
                wuffs_png__decoder__decode_frame(dec, &pb, &io, WUFFS_BASE__PIXEL_BLEND__SRC,
                                                 wuffs_base__make_slice_u8(work, (size_t)wl), NULL);
        }
        free(work);
        free(px);
    }
    free(dec);
    return wuffs_base__status__is_ok(&st);
#elif defined(PROBE_SQLITE)
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(db,
                                "SELECT tile_data FROM tiles WHERE zoom_level=?1 AND "
                                "tile_column=?2 AND tile_row=?3",
                                -1, &stmt, NULL);
    }
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, argc);
        sqlite3_bind_int(stmt, 2, argc);
        sqlite3_bind_int(stmt, 3, argc);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            rc = sqlite3_column_bytes(stmt, 0) + (sqlite3_column_blob(stmt, 0) != NULL);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
#else
    return argc;
#endif
}
