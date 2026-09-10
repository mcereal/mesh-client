#pragma once

/*
 * Reading a zip from its back end, over a window somebody else fetched.
 *
 * This exists because upstream publishes one zip per platform and they grow - 46 MB for
 * nrf52840 at 2.7.26, 58 MB at 2.8.0, 170 MB for esp32s3 - and the image inside one for a
 * single board is a megabyte and a half. Downloading forty times what you need, on a handheld,
 * over a shared antenna, is not the plan; a zip is designed to be read from the back, and
 * GitHub's release CDN honours HTTP range requests. So the reader is written the way the
 * download is shaped: three passes over three windows, each one a range request the caller
 * makes, and nothing here opens a socket or holds the file.
 *
 *   1. the tail       -> mesh_zip_find_end()          where the central directory is
 *   2. the directory  -> mesh_zip_find_member()       the member's sizes, CRC and where it is
 *   3. a local header -> mesh_zip_local_data_start()  where its bytes actually begin
 *
 * **Every size and the CRC come from the central directory, never from the local header.** At
 * 2.7.26 the two agree; at 2.8.0 the local header's are all three *zero*, deferred to a data
 * descriptor written after the compressed bytes, because the release is built by a producer
 * that did not know the sizes when it wrote the header. Code tested against the older release
 * and reading the local header verifies the newer one against nothing. The local header is
 * asked exactly one question - how long are your two variable fields - because that is the only
 * thing in it the central directory does not also carry, and it is what places the data.
 *
 * No zip64, on purpose. These files are under 4 GB and hold a few hundred entries, so the
 * sentinel values that mean "look in the zip64 record" cannot legitimately appear - and a
 * reader that quietly treated 0xFFFFFFFF as an offset would range-read the wrong four
 * gigabytes. mesh_zip_find_end() refuses them instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How much of the tail to ask for.
 *
 * The end-of-central-directory record is 22 bytes plus a comment of up to 65,535, so this is
 * the largest a conforming zip's tail can need. It is also enough to carry the whole central
 * directory of every release zip measured: 16,316 bytes for the 139-entry nrf52840 zip at
 * 2.7.26 and 21,358 for the 171-entry one at 2.8.0, which is why the download is two round
 * trips rather than three. That is a happy accident rather than a guarantee, so whether the
 * directory is in the window we already hold is a question mesh_zip_central_slice() answers
 * rather than an assumption.
 *
 * The window must be asked for as an explicit `bytes=<start>-<end>`: the CDN in front of these
 * files answers `501 Unsupported client range` to a suffix range, so the caller has to learn
 * the file's length first.
 */
#define MESH_ZIP_TAIL_WINDOW 65536U

/* The fixed part of a local file header, ahead of its name and extra field. */
#define MESH_ZIP_LOCAL_HEADER_SIZE 30U

/*
 * A stored member path. The longest in any release zip measured is 72 - a prefixed
 * `nrf52840/firmware-seeed_wio_tracker_L1_eink-inkhud-2.8.0.47db0e3-ota.zip` - and an entry
 * whose name does not fit is skipped rather than truncated, because a truncated name could
 * match a basename that is not its own.
 */
#define MESH_ZIP_NAME_MAX 96U

/* General purpose bit 3: the sizes and CRC in the local header are 0 and follow the data. */
#define MESH_ZIP_FLAG_DATA_DESCRIPTOR 0x0008U

/* Compression methods. Only these two appear; anything else is a member we cannot inflate. */
#define MESH_ZIP_METHOD_STORE 0U
#define MESH_ZIP_METHOD_DEFLATE 8U

/* Where the central directory is, read out of the end-of-central-directory record. */
struct mesh_zip_end {
    /* Absolute, from the start of the file - which is what a range request needs. */
    uint64_t central_offset;
    uint32_t central_size;
    /* What the record claims. The walk is bounded by `central_size` as well, so a count that
       lies about being larger costs nothing. */
    uint32_t entries;
};

/* One member, as the central directory describes it. */
struct mesh_zip_entry {
    char name[MESH_ZIP_NAME_MAX];
    uint16_t flags;
    uint16_t method;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint64_t local_header_offset;
};

/*
 * Finds the end-of-central-directory record in a tail window.
 *
 * `window_offset` is where the window starts in the file, which is what lets the record be
 * checked against where it itself is: the directory it points at has to end at or before it.
 * The scan runs backwards, and a candidate is only accepted when its comment length accounts
 * for exactly the bytes after it - which is what tells a real record from the four signature
 * bytes turning up inside somebody's compressed firmware, and they do turn up.
 *
 * False when there is no record in the window, or when the record is a zip64 placeholder.
 */
bool mesh_zip_find_end(const uint8_t *window, size_t len, uint64_t window_offset,
                       struct mesh_zip_end *out);

/*
 * The central directory inside a window that already holds it, or NULL.
 *
 * NULL means "range-read `end->central_size` bytes at `end->central_offset` and call
 * mesh_zip_find_member() on those instead" - not an error. Both zips this was written against
 * fit their directory in the tail window with room to spare, and the one that does not one day
 * is the reason this is a question.
 */
const uint8_t *mesh_zip_central_slice(const struct mesh_zip_end *end, const uint8_t *window,
                                      size_t len, uint64_t window_offset);

/*
 * Finds the member whose **basename** is `basename`.
 *
 * Basename rather than whole path because the path in front of it is not stable across
 * releases: `firmware-<target>-<ver>.uf2` sits at the root of the 2.7.26 zip and under
 * `nrf52840/` in the 2.8.0 one, so a reader matching the full path finds the file in the
 * release it was written against and nothing in the next. An entry whose stored name ends in
 * '/' is a directory marker with an empty basename and never matches.
 *
 * False when no entry matched or the directory did not walk.
 */
bool mesh_zip_find_member(const uint8_t *central, size_t len, uint32_t entries,
                          const char *basename, struct mesh_zip_entry *out);

/*
 * Where `entry`'s compressed bytes begin, given the fixed part of its local header.
 *
 * The caller range-reads MESH_ZIP_LOCAL_HEADER_SIZE bytes at `entry->local_header_offset` and
 * passes them here; what comes back is an absolute offset to range-read
 * `entry->compressed_size` bytes from. The name and extra field lengths here are genuinely
 * different from the central directory's - the extra field carries different tags in the two
 * places - which is why this round trip exists at all rather than the data offset being
 * computed from the entry.
 *
 * False when the header is short or does not carry the local file header signature.
 */
bool mesh_zip_local_data_start(const uint8_t *header, size_t len,
                               const struct mesh_zip_entry *entry, uint64_t *out_offset);

#ifdef __cplusplus
}
#endif
