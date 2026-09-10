#include "mesh/utils/zip.h"

#include <string.h>

/* Signatures, little-endian on the wire and read byte by byte so this does not care what the
   host's endianness or alignment rules are. */
#define ZIP_SIG_EOCD 0x06054B50U
#define ZIP_SIG_CENTRAL 0x02014B50U
#define ZIP_SIG_LOCAL 0x04034B50U

#define ZIP_EOCD_SIZE 22U
#define ZIP_CENTRAL_HEADER_SIZE 46U

/* The value every zip64 field is set to when the real one lives in the zip64 record. */
#define ZIP_U16_SENTINEL 0xFFFFU
#define ZIP_U32_SENTINEL 0xFFFFFFFFU

static uint16_t zip_u16(const uint8_t *at) {
    return (uint16_t)((uint16_t)at[0] | (uint16_t)((uint16_t)at[1] << 8));
}

static uint32_t zip_u32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

/* The part of a stored path after the last '/', which is empty for a directory marker. */
static const char *zip_basename(const char *name) {
    const char *const slash = strrchr(name, '/');
    return slash != NULL ? slash + 1 : name;
}

bool mesh_zip_find_end(const uint8_t *window, size_t len, uint64_t window_offset,
                       struct mesh_zip_end *out) {
    if (window == NULL || out == NULL || len < ZIP_EOCD_SIZE) {
        return false;
    }
    memset(out, 0, sizeof *out);

    for (size_t back = len - ZIP_EOCD_SIZE;; --back) {
        if (zip_u32(window + back) == ZIP_SIG_EOCD) {
            /*
             * The signature is four bytes and the members of these zips are compressed
             * firmware, so it appears inside the payload as often as chance says it should.
             * What tells the record from the coincidence is that its comment length has to
             * account for exactly the bytes after it - a field a random match gets right one
             * time in 65,536.
             */
            const uint16_t comment = zip_u16(window + back + 20U);
            if ((size_t)comment == len - back - ZIP_EOCD_SIZE) {
                const uint16_t entries = zip_u16(window + back + 10U);
                const uint32_t size = zip_u32(window + back + 12U);
                const uint32_t offset = zip_u32(window + back + 16U);
                /*
                 * A zip64 file puts sentinels here and the real numbers in a record ahead of
                 * this one. Reading a sentinel as an offset would range-read four gigabytes
                 * into a file that does not have them, so it is refused rather than guessed
                 * at. No release zip is anywhere near needing one.
                 */
                if (entries == ZIP_U16_SENTINEL || size == ZIP_U32_SENTINEL ||
                    offset == ZIP_U32_SENTINEL) {
                    return false;
                }
                /*
                 * The directory has to end at or before the record that points at it. This is
                 * the one cross-check the window makes possible - `back` is where the record
                 * is in the file - and it is what catches a record whose fields survived the
                 * comment-length test but describe a directory somewhere the file does not
                 * have. Range-reading that offset would fetch a megabyte of somebody's
                 * firmware and walk it as headers.
                 */
                const uint64_t record_at = window_offset + (uint64_t)back;
                if ((uint64_t)offset + (uint64_t)size > record_at) {
                    return false;
                }
                out->entries = entries;
                out->central_size = size;
                out->central_offset = (uint64_t)offset;
                return true;
            }
        }
        if (back == 0U) {
            break;
        }
    }
    return false;
}

const uint8_t *mesh_zip_central_slice(const struct mesh_zip_end *end, const uint8_t *window,
                                      size_t len, uint64_t window_offset) {
    if (end == NULL || window == NULL) {
        return NULL;
    }
    if (end->central_offset < window_offset) {
        return NULL;
    }
    const uint64_t into = end->central_offset - window_offset;
    /* Both halves: it has to start inside the window *and* end inside it. A directory that
       starts in the window and runs off the end is the case that reads a few hundred entries
       correctly and then walks into the EOCD. */
    if (into > (uint64_t)len || (uint64_t)end->central_size > (uint64_t)len - into) {
        return NULL;
    }
    return window + (size_t)into;
}

enum mesh_zip_search mesh_zip_find_member(const uint8_t *central, size_t len, uint32_t entries,
                                          const char *basename, struct mesh_zip_entry *out) {
    if (central == NULL || out == NULL) {
        return MESH_ZIP_MALFORMED;
    }
    /* An empty basename is a caller asking for nothing, not a broken archive - and it is the
       one a directory marker would otherwise match. */
    if (basename == NULL || basename[0] == '\0') {
        memset(out, 0, sizeof *out);
        return MESH_ZIP_ABSENT;
    }
    memset(out, 0, sizeof *out);

    size_t at = 0U;
    /* Bounded by the count *and* by the bytes, because the two come from the same record and
       either one can be the shorter truth. */
    for (uint32_t n = 0U; n < entries; ++n) {
        if (at > len || len - at < ZIP_CENTRAL_HEADER_SIZE) {
            return MESH_ZIP_MALFORMED;
        }
        const uint8_t *const header = central + at;
        if (zip_u32(header) != ZIP_SIG_CENTRAL) {
            return MESH_ZIP_MALFORMED;
        }
        const uint16_t name_len = zip_u16(header + 28U);
        const uint16_t extra_len = zip_u16(header + 30U);
        const uint16_t comment_len = zip_u16(header + 32U);
        const size_t entry_size =
            ZIP_CENTRAL_HEADER_SIZE + (size_t)name_len + (size_t)extra_len + (size_t)comment_len;
        if (entry_size > len - at) {
            return MESH_ZIP_MALFORMED;
        }

        /* A name that does not fit is skipped rather than truncated: a truncated name can
           match a basename that is not its own, which is the one way this could hand back the
           wrong member's offset. */
        if ((size_t)name_len < MESH_ZIP_NAME_MAX) {
            char name[MESH_ZIP_NAME_MAX];
            memcpy(name, header + ZIP_CENTRAL_HEADER_SIZE, name_len);
            name[name_len] = '\0';
            if (strcmp(zip_basename(name), basename) == 0) {
                memcpy(out->name, name, (size_t)name_len + 1U);
                out->flags = zip_u16(header + 8U);
                out->method = zip_u16(header + 10U);
                out->crc32 = zip_u32(header + 16U);
                out->compressed_size = zip_u32(header + 20U);
                out->uncompressed_size = zip_u32(header + 24U);
                out->local_header_offset = (uint64_t)zip_u32(header + 42U);
                /* Same refusal as the record: a sentinel here is a zip64 file, and the offset
                   it is standing in for is somewhere this reader cannot see. */
                if (out->local_header_offset == (uint64_t)ZIP_U32_SENTINEL ||
                    out->compressed_size == ZIP_U32_SENTINEL ||
                    out->uncompressed_size == ZIP_U32_SENTINEL) {
                    return MESH_ZIP_MALFORMED;
                }
                return MESH_ZIP_FOUND;
            }
        }
        at += entry_size;
    }
    /* The walk finished, every entry was a real one, and none of them was this. */
    return MESH_ZIP_ABSENT;
}

bool mesh_zip_local_data_start(const uint8_t *header, size_t len,
                               const struct mesh_zip_entry *entry, uint64_t *out_offset) {
    if (header == NULL || entry == NULL || out_offset == NULL || len < MESH_ZIP_LOCAL_HEADER_SIZE) {
        return false;
    }
    if (zip_u32(header) != ZIP_SIG_LOCAL) {
        return false;
    }
    /*
     * These two and nothing else. The CRC and both sizes are also in this header and are read
     * from the central directory instead - at 2.8.0 all three of them here are 0, deferred to
     * a data descriptor after the payload, so a reader that trusted them would ask the CDN for
     * a zero-length range and then verify the result against a CRC of 0.
     */
    const uint16_t name_len = zip_u16(header + 26U);
    const uint16_t extra_len = zip_u16(header + 28U);
    *out_offset = entry->local_header_offset + MESH_ZIP_LOCAL_HEADER_SIZE + name_len + extra_len;
    return true;
}
