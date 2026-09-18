#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_download.h"

#include "mesh/utils/inflate.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Per-step deadlines. The member is half a megabyte over a handheld's Wi-Fi and was measured
   at 1.8-3.0 s on a Brick; the rest are a few kilobytes each and a round trip. */
#define DOWNLOAD_STEP_TIMEOUT_MS 20000U
#define DOWNLOAD_MEMBER_TIMEOUT_MS 120000U

/* The intermediates. Named rather than made unique because a download is one at a time and a
   leftover from a run that died is a file the next run overwrites rather than trips over. */
#define DOWNLOAD_FILE_WINDOW "firmware.window"
#define DOWNLOAD_FILE_CENTRAL "firmware.central"
#define DOWNLOAD_FILE_HEADER "firmware.header"
#define DOWNLOAD_FILE_MEMBER "firmware.member"
#define DOWNLOAD_FILE_IMAGE "firmware.image"

static void download_step_window(struct mesh_firmware_download *download);
static void download_step_central(struct mesh_firmware_download *download);
static void download_step_header(struct mesh_firmware_download *download);
static void download_step_member(struct mesh_firmware_download *download);
static void download_step_inflate(struct mesh_firmware_download *download);

/* ---- staging ----------------------------------------------------------------------------- */

static bool download_path(const struct mesh_firmware_download *download, const char *name,
                          char *out, size_t out_len) {
    const int written = snprintf(out, out_len, "%s/%s", download->staging, name);
    return written > 0 && (size_t)written < out_len;
}

static void download_remove(const struct mesh_firmware_download *download, const char *name) {
    char path[MESH_FETCH_PATH_MAX];
    if (download_path(download, name, path, sizeof path)) {
        (void)unlink(path);
    }
}

/* Everything except the image itself. Called on the way to READY and on the way to FAILED:
   the intermediates are worth nothing to a retry, and the member is the one big one. */
static void download_clean(const struct mesh_firmware_download *download) {
    download_remove(download, DOWNLOAD_FILE_WINDOW);
    download_remove(download, DOWNLOAD_FILE_CENTRAL);
    download_remove(download, DOWNLOAD_FILE_HEADER);
    download_remove(download, DOWNLOAD_FILE_MEMBER);
}

/* Reads a staged file whole. The largest is the member at ~0.5 MB, against ~700 MB free, and
   the roadmap wants the image in memory before any handover begins anyway. */
static uint8_t *download_read(const struct mesh_firmware_download *download, const char *name,
                              size_t *out_len) {
    *out_len = 0U;
    char path[MESH_FETCH_PATH_MAX];
    if (!download_path(download, name, path, sizeof path)) {
        return NULL;
    }
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *const bytes = malloc((size_t)size);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    const size_t got = fread(bytes, 1U, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(bytes);
        return NULL;
    }
    *out_len = got;
    return bytes;
}

static uint64_t download_file_size(const struct mesh_firmware_download *download,
                                   const char *name) {
    char path[MESH_FETCH_PATH_MAX];
    struct stat info;
    if (!download_path(download, name, path, sizeof path) || stat(path, &info) != 0) {
        return 0U;
    }
    return info.st_size > 0 ? (uint64_t)info.st_size : 0U;
}

/* ---- finishing --------------------------------------------------------------------------- */

static void download_finish(struct mesh_firmware_download *download,
                            enum mesh_firmware_download_state state,
                            enum mesh_firmware_download_error error) {
    download->state = state;
    download->error = error;
    download_clean(download);
    if (state == MESH_FIRMWARE_DOWNLOAD_FAILED) {
        /* A half-written image is worse than none: it is the right length often enough to be
           tried, and it is the file a retry would otherwise skip fetching. */
        download_remove(download, DOWNLOAD_FILE_IMAGE);
    }
    if (download->on_done != NULL) {
        const mesh_firmware_download_done_fn done = download->on_done;
        void *const userdata = download->userdata;
        /* Cleared before the call: a caller starting the next download from inside this one is
           the shape mesh_fetch already supports, and it must not see a stale callback. */
        download->on_done = NULL;
        download->userdata = NULL;
        done(userdata, download);
    }
}

static void download_fail(struct mesh_firmware_download *download,
                          enum mesh_firmware_download_error error) {
    download_finish(download, MESH_FIRMWARE_DOWNLOAD_FAILED, error);
}

/* ---- the range requests -------------------------------------------------------------------*/

static void download_on_fetch(void *userdata, const struct mesh_fetch_result *result);

/*
 * Starts one range read into `name`.
 *
 * `first` and `last` are inclusive, as HTTP means them, and they are always both present: a
 * suffix range (`bytes=-65536`) is answered `501 Unsupported client range` by the cache in
 * front of these files, which is the whole reason the HEAD step exists.
 */
static bool download_range(struct mesh_firmware_download *download, const char *name,
                           uint64_t first, uint64_t last, uint32_t timeout_ms) {
    if (!download_path(download, name, download->active_path, sizeof download->active_path)) {
        return false;
    }
    const int written = snprintf(download->range, sizeof download->range, "Range: bytes=%llu-%llu",
                                 (unsigned long long)first, (unsigned long long)last);
    if (written <= 0 || (size_t)written >= sizeof download->range) {
        return false;
    }
    struct mesh_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = download->zip_url;
    request.headers[0] = download->range;
    request.output_path = download->active_path;
    request.timeout_ms = timeout_ms;
    request.on_done = download_on_fetch;
    request.userdata = download;
    return mesh_fetch_start(download->fetch, &request, mesh_time_monotonic_ms()) == 0;
}

static void download_step_window(struct mesh_firmware_download *download) {
    /*
     * The largest tail a conforming zip can need, or the whole file when it is smaller than
     * that - which no release zip is, but a 404 page dressed as one might be.
     */
    const uint64_t want = (uint64_t)MESH_ZIP_TAIL_WINDOW;
    download->window_offset = download->zip_size > want ? download->zip_size - want : 0U;
    download->window_len = (size_t)(download->zip_size - download->window_offset);
    download->state = MESH_FIRMWARE_DOWNLOAD_READING;
    if (!download_range(download, DOWNLOAD_FILE_WINDOW, download->window_offset,
                        download->zip_size - 1U, DOWNLOAD_STEP_TIMEOUT_MS)) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
    }
}

static void download_step_central(struct mesh_firmware_download *download) {
    download->state = MESH_FIRMWARE_DOWNLOAD_READING;
    download->directory_only = true;
    if (!download_range(download, DOWNLOAD_FILE_CENTRAL, download->window_offset,
                        download->window_offset + (uint64_t)download->window_len - 1U,
                        DOWNLOAD_STEP_TIMEOUT_MS)) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
    }
}

static void download_step_header(struct mesh_firmware_download *download) {
    download->state = MESH_FIRMWARE_DOWNLOAD_LOCATING;
    if (!download_range(download, DOWNLOAD_FILE_HEADER, download->entry.local_header_offset,
                        download->entry.local_header_offset + MESH_ZIP_LOCAL_HEADER_SIZE - 1U,
                        DOWNLOAD_STEP_TIMEOUT_MS)) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
    }
}

static void download_step_member(struct mesh_firmware_download *download) {
    download->state = MESH_FIRMWARE_DOWNLOAD_FETCHING;
    if (!download_range(download, DOWNLOAD_FILE_MEMBER, download->data_offset,
                        download->data_offset + download->entry.compressed_size - 1U,
                        DOWNLOAD_MEMBER_TIMEOUT_MS)) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
    }
}

/* ---- reading what came back ---------------------------------------------------------------*/

/* The tail window, or the directory fetched on its own. Both end here. */
static void download_read_directory(struct mesh_firmware_download *download, const char *name,
                                    bool second_pass) {
    size_t len = 0U;
    uint8_t *const window = download_read(download, name, &len);
    if (window == NULL) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }

    const uint8_t *central = NULL;
    uint32_t central_size = 0U;
    uint32_t entries = 0U;
    if (second_pass) {
        /* This read *is* the directory: its bounds were taken from the record last time. */
        central = window;
        central_size = (uint32_t)len;
        entries = (uint32_t)UINT16_MAX;
    } else {
        struct mesh_zip_end end;
        if (!mesh_zip_find_end(window, len, download->window_offset, &end)) {
            free(window);
            download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NOT_A_ZIP);
            return;
        }
        central = mesh_zip_central_slice(&end, window, len, download->window_offset);
        central_size = end.central_size;
        entries = end.entries;
        download->central_offset = end.central_offset;
        if (central == NULL) {
            /*
             * A directory bigger than the window. Neither release zip measured needs this -
             * 16 KB and 21 KB against a 64 KB window - and it is written anyway because the
             * alternative is a feature that stops working on the release that crosses the
             * line, in a way nobody would connect to the zip having grown.
             */
            download->window_offset = end.central_offset;
            download->window_len = (size_t)end.central_size;
            download->central_offset = end.central_offset;
            free(window);
            mesh_log_info("firmware", "Central directory is %u bytes; fetching it on its own",
                          (unsigned)end.central_size);
            download_step_central(download);
            return;
        }
    }

    const enum mesh_zip_search search =
        mesh_zip_find_member(central, central_size, entries, download->member, &download->entry);
    free(window);
    if (search == MESH_ZIP_MALFORMED) {
        /*
         * Deliberately not NO_MEMBER. The walk stopped being a walk, which says nothing about
         * whether the file we want is in there - and NO_MEMBER is the row that tells somebody
         * upstream no longer builds for their board, which is a sentence about a different
         * thing and one they cannot retry their way out of.
         */
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NOT_A_ZIP);
        return;
    }
    if (search != MESH_ZIP_FOUND) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NO_MEMBER);
        return;
    }
    if (download->entry.method != MESH_ZIP_METHOD_DEFLATE &&
        download->entry.method != MESH_ZIP_METHOD_STORE) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_UNSUPPORTED);
        return;
    }
    /*
     * The member has to sit in front of the directory that described it. Nothing in the reader
     * can check that - it is handed a directory and not a file - and the offset is the one
     * answer here that becomes a range request, so a member claiming to live inside the central
     * directory would have us fetch the directory and inflate it as firmware.
     */
    if (download->central_offset != 0U &&
        download->entry.local_header_offset + (uint64_t)download->entry.compressed_size >
            download->central_offset) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NOT_A_ZIP);
        return;
    }
    if (download->entry.compressed_size == 0U || download->entry.uncompressed_size == 0U) {
        /* Both come from the central directory, so zero here is not the 2.8.0 local-header
           case - it is a member with nothing in it, which no image ever is. */
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NO_MEMBER);
        return;
    }
    download->located = true;
    mesh_log_info("firmware", "%s is %u bytes in the zip, %u out", download->entry.name,
                  (unsigned)download->entry.compressed_size,
                  (unsigned)download->entry.uncompressed_size);
    download_step_header(download);
}

static void download_read_header(struct mesh_firmware_download *download) {
    size_t len = 0U;
    uint8_t *const header = download_read(download, DOWNLOAD_FILE_HEADER, &len);
    if (header == NULL) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }
    const bool placed =
        mesh_zip_local_data_start(header, len, &download->entry, &download->data_offset);
    free(header);
    if (!placed) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NOT_A_ZIP);
        return;
    }
    download_step_member(download);
}

static void download_on_fetch(void *userdata, const struct mesh_fetch_result *result) {
    struct mesh_firmware_download *const download = (struct mesh_firmware_download *)userdata;
    if (result->outcome != MESH_FETCH_OK) {
        mesh_log_error("firmware", "A range read failed (outcome %d, status %d)",
                       (int)result->outcome, result->status);
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NETWORK);
        return;
    }
    switch (download->state) {
    case MESH_FIRMWARE_DOWNLOAD_MEASURING: {
        uint64_t size = 0U;
        if (!mesh_fetch_content_length(result->body, result->len, &size) ||
            size < MESH_ZIP_LOCAL_HEADER_SIZE) {
            download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NOT_A_ZIP);
            return;
        }
        download->zip_size = size;
        mesh_log_info("firmware", "The release zip is %llu bytes", (unsigned long long)size);
        download_step_window(download);
        break;
    }
    case MESH_FIRMWARE_DOWNLOAD_READING:
        /* Which of the two reads this was is the file it landed in, and the second one
           only ever happens after the first has moved `window_offset` onto the record's
           own answer. */
        download_read_directory(
            download, download->directory_only ? DOWNLOAD_FILE_CENTRAL : DOWNLOAD_FILE_WINDOW,
            download->directory_only);
        break;
    case MESH_FIRMWARE_DOWNLOAD_LOCATING:
        download_read_header(download);
        break;
    case MESH_FIRMWARE_DOWNLOAD_FETCHING:
        download_step_inflate(download);
        break;
    default:
        break;
    }
}

/* ---- the inflate --------------------------------------------------------------------------*/

static void download_step_inflate(struct mesh_firmware_download *download) {
    /* The work is the next tick's, so the done callback arrives from the tick exactly as it did
       when the inflate was a child being reaped there - see mesh_firmware_update_tick(). */
    download->state = MESH_FIRMWARE_DOWNLOAD_INFLATING;
    download->inflate_pending = true;
}

static bool download_write_image(const struct mesh_firmware_download *download,
                                 const uint8_t *bytes, size_t len) {
    char path[MESH_FETCH_PATH_MAX];
    if (!download_path(download, DOWNLOAD_FILE_IMAGE, path, sizeof path)) {
        return false;
    }
    FILE *const file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const bool written = fwrite(bytes, 1U, len, file) == len;
    return fclose(file) == 0 && written;
}

/*
 * The member, inflated in memory and checked against the central directory.
 *
 * The length and the CRC32 the directory carried are the download's integrity check: there is
 * no separate verify step because this *is* one. Both come from the **central** directory,
 * never the local header, which at 2.8.0 says zero for all three - see mesh/utils/zip.h.
 *
 * A stored member takes the same check with the inflate skipped, so there is one path to the
 * CRC rather than two, and a release that stops compressing an image that does not compress
 * still downloads.
 */
static void download_inflate(struct mesh_firmware_download *download) {
    size_t len = 0U;
    uint8_t *const member = download_read(download, DOWNLOAD_FILE_MEMBER, &len);
    if (member == NULL) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }
    if (len != (size_t)download->entry.compressed_size) {
        /*
         * Not a staging failure and not a corrupt member: the bytes the directory promised did
         * not all arrive. curl exits 0 on a reply that is short but consistent with its own
         * Content-Length, so this is the only place a truncated member is caught - and it is
         * the network's fault, which means the answer is "try again" rather than "give up".
         */
        mesh_log_error("firmware", "The member arrived %zu bytes long, not %u", len,
                       (unsigned)download->entry.compressed_size);
        free(member);
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NETWORK);
        return;
    }

    const size_t want = (size_t)download->entry.uncompressed_size;
    uint8_t *image = member;
    size_t produced = len;
    if (download->entry.method == MESH_ZIP_METHOD_DEFLATE) {
        image = malloc(want);
        if (image == NULL) {
            free(member);
            download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
            return;
        }
        const enum mesh_inflate_result inflated = mesh_inflate(member, len, image, want, &produced);
        free(member);
        if (inflated == MESH_INFLATE_NO_MEMORY) {
            free(image);
            download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
            return;
        }
        if (inflated != MESH_INFLATE_OK) {
            mesh_log_error("firmware", "The member is not a deflate stream that fits %zu bytes",
                           want);
            free(image);
            download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE);
            return;
        }
    }

    uint32_t crc = 0U;
    if (!mesh_crc32(image, produced, &crc)) {
        free(image);
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }
    if (produced != want || crc != download->entry.crc32) {
        /* The bytes arrived and they are not the bytes the central directory described. */
        mesh_log_error("firmware", "The image is %zu bytes with CRC %08x, not %zu with %08x",
                       produced, (unsigned)crc, want, (unsigned)download->entry.crc32);
        free(image);
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE);
        return;
    }

    const bool written = download_write_image(download, image, produced);
    free(image);
    if (!written) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }
    mesh_log_info("firmware", "Staged %zu bytes of firmware", produced);
    download_finish(download, MESH_FIRMWARE_DOWNLOAD_READY, MESH_FIRMWARE_DOWNLOAD_ERROR_NONE);
}

/* ---- the public half ----------------------------------------------------------------------*/

int mesh_firmware_download_start(struct mesh_firmware_download *download, struct mesh_fetch *fetch,
                                 const char *zip_url, const char *member, const char *staging_dir,
                                 mesh_firmware_download_done_fn on_done, void *userdata) {
    if (download == NULL || fetch == NULL || zip_url == NULL || member == NULL ||
        staging_dir == NULL || zip_url[0] == '\0' || member[0] == '\0' || staging_dir[0] == '\0') {
        return -EINVAL;
    }
    if (mesh_firmware_download_busy(download)) {
        return -EBUSY;
    }
    if (!mesh_fetch_available(fetch)) {
        return -ENOTSUP;
    }
    if (mesh_fetch_busy(fetch)) {
        return -EBUSY;
    }

    memset(download, 0, sizeof *download);
    download->fetch = fetch;
    mesh_str_copy(download->zip_url, sizeof download->zip_url, zip_url);
    mesh_str_copy(download->member, sizeof download->member, member);
    mesh_str_copy(download->staging, sizeof download->staging, staging_dir);
    download->on_done = on_done;
    download->userdata = userdata;
    download->state = MESH_FIRMWARE_DOWNLOAD_MEASURING;

    /*
     * The HEAD first, because the range that follows it cannot be expressed without a length -
     * the CDN refuses a suffix range outright.
     */
    struct mesh_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = download->zip_url;
    request.method = MESH_FETCH_HEAD;
    request.timeout_ms = DOWNLOAD_STEP_TIMEOUT_MS;
    request.on_done = download_on_fetch;
    request.userdata = download;
    const int started = mesh_fetch_start(fetch, &request, mesh_time_monotonic_ms());
    if (started != 0) {
        download->state = MESH_FIRMWARE_DOWNLOAD_IDLE;
        download->on_done = NULL;
        return started;
    }
    return 0;
}

void mesh_firmware_download_tick(struct mesh_firmware_download *download, uint64_t now_ms) {
    (void)now_ms;
    if (download == NULL || !download->inflate_pending) {
        return;
    }
    download->inflate_pending = false;
    download_inflate(download);
}

void mesh_firmware_download_cancel(struct mesh_firmware_download *download) {
    if (download == NULL) {
        return;
    }
    if (download->fetch != NULL) {
        mesh_fetch_cancel(download->fetch);
    }
    download->inflate_pending = false;
    download_clean(download);
    download_remove(download, DOWNLOAD_FILE_IMAGE);
    download->on_done = NULL;
    download->userdata = NULL;
    download->state = MESH_FIRMWARE_DOWNLOAD_IDLE;
}

bool mesh_firmware_download_busy(const struct mesh_firmware_download *download) {
    if (download == NULL) {
        return false;
    }
    return download->state != MESH_FIRMWARE_DOWNLOAD_IDLE &&
           download->state != MESH_FIRMWARE_DOWNLOAD_READY &&
           download->state != MESH_FIRMWARE_DOWNLOAD_FAILED;
}

unsigned mesh_firmware_download_progress(const struct mesh_firmware_download *download) {
    if (download == NULL) {
        return 0U;
    }
    /*
     * The member has landed by the time the inflate starts, so INFLATING is 100 and not 0.
     * Reported the other way the bar ran to full, dropped to empty and filled again - which is
     * what it did on the device the first time this was watched, and which reads as a download
     * starting over rather than as a step finishing.
     */
    if (download->state == MESH_FIRMWARE_DOWNLOAD_READY ||
        download->state == MESH_FIRMWARE_DOWNLOAD_INFLATING) {
        return 100U;
    }
    if (download->state != MESH_FIRMWARE_DOWNLOAD_FETCHING ||
        download->entry.compressed_size == 0U) {
        return 0U;
    }
    const uint64_t landed = download_file_size(download, DOWNLOAD_FILE_MEMBER);
    if (landed >= (uint64_t)download->entry.compressed_size) {
        return 100U;
    }
    return (unsigned)((landed * 100U) / (uint64_t)download->entry.compressed_size);
}

const char *mesh_firmware_download_image_path(const struct mesh_firmware_download *download,
                                              char *out, size_t out_len) {
    if (download == NULL || out == NULL || out_len == 0U ||
        download->state != MESH_FIRMWARE_DOWNLOAD_READY) {
        return NULL;
    }
    return download_path(download, DOWNLOAD_FILE_IMAGE, out, out_len) ? out : NULL;
}

uint64_t mesh_firmware_download_image_size(const struct mesh_firmware_download *download) {
    return download != NULL ? (uint64_t)download->entry.uncompressed_size : 0U;
}
