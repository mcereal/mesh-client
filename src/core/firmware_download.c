#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_download.h"

#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Per-step deadlines. The member is half a megabyte over a handheld's Wi-Fi and was measured
   at 1.8-3.0 s on a Brick; the rest are a few kilobytes each and a round trip. */
#define DOWNLOAD_STEP_TIMEOUT_MS 20000U
#define DOWNLOAD_MEMBER_TIMEOUT_MS 120000U
/* busybox gzip inflated 1.4 MB in 0.03 s on the device. This is the backstop, not the budget. */
#define DOWNLOAD_INFLATE_TIMEOUT_MS 30000U

/* The intermediates. Named rather than made unique because a download is one at a time and a
   leftover from a run that died is a file the next run overwrites rather than trips over. */
#define DOWNLOAD_FILE_WINDOW "firmware.window"
#define DOWNLOAD_FILE_CENTRAL "firmware.central"
#define DOWNLOAD_FILE_HEADER "firmware.header"
#define DOWNLOAD_FILE_ENVELOPE "firmware.gz"
#define DOWNLOAD_FILE_IMAGE "firmware.image"

/* A deflate stored block may carry this much, and the length field is what limits it. */
#define DOWNLOAD_STORED_BLOCK_MAX 65535U

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
    download_remove(download, DOWNLOAD_FILE_ENVELOPE);
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
    const int written = snprintf(download->range, sizeof download->range,
                                 "Range: bytes=%llu-%llu", (unsigned long long)first,
                                 (unsigned long long)last);
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
    if (!download_range(download, DOWNLOAD_FILE_ENVELOPE, download->data_offset,
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
        if (central == NULL) {
            /*
             * A directory bigger than the window. Neither release zip measured needs this -
             * 16 KB and 21 KB against a 64 KB window - and it is written anyway because the
             * alternative is a feature that stops working on the release that crosses the
             * line, in a way nobody would connect to the zip having grown.
             */
            download->window_offset = end.central_offset;
            download->window_len = (size_t)end.central_size;
            free(window);
            mesh_log_info("firmware", "Central directory is %u bytes; fetching it on its own",
                          (unsigned)end.central_size);
            download_step_central(download);
            return;
        }
    }

    const bool found = mesh_zip_find_member(central, central_size, entries, download->member,
                                            &download->entry);
    free(window);
    if (!found) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_NO_MEMBER);
        return;
    }
    if (download->entry.method != MESH_ZIP_METHOD_DEFLATE &&
        download->entry.method != MESH_ZIP_METHOD_STORE) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_UNSUPPORTED);
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
            mesh_log_info("firmware", "The release zip is %llu bytes",
                          (unsigned long long)size);
            download_step_window(download);
            break;
        }
        case MESH_FIRMWARE_DOWNLOAD_READING:
            /* Which of the two reads this was is the file it landed in, and the second one
               only ever happens after the first has moved `window_offset` onto the record's
               own answer. */
            download_read_directory(download,
                                    download->directory_only ? DOWNLOAD_FILE_CENTRAL
                                                             : DOWNLOAD_FILE_WINDOW,
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

/* ---- the gzip envelope --------------------------------------------------------------------*/

static void download_put_u32(FILE *file, uint32_t value) {
    const uint8_t bytes[4] = {(uint8_t)(value & 0xFFU), (uint8_t)((value >> 8) & 0xFFU),
                              (uint8_t)((value >> 16) & 0xFFU), (uint8_t)((value >> 24) & 0xFFU)};
    (void)fwrite(bytes, 1U, sizeof bytes, file);
}

/*
 * Wraps the member in a gzip envelope, in place.
 *
 * Ten bytes of header in front and eight behind - the CRC32 and the uncompressed size, both
 * from the **central** directory - around a raw deflate stream that is already on disk. The
 * envelope is written by reading the member back and writing it out again rather than by
 * seeking, because the member is what curl wrote and there is no room in front of it.
 *
 * A **stored** member is wrapped a second time, in deflate's own stored-block framing, so that
 * it reaches `gzip` as a deflate stream like any other. That is worth the twenty lines: it is
 * what keeps the CRC check on one path instead of two, and the alternative - refusing a stored
 * member - would be a feature that stops working the day upstream stops compressing an image
 * that does not compress.
 */
static bool download_envelope(struct mesh_firmware_download *download, bool *out_short) {
    *out_short = false;
    size_t len = 0U;
    uint8_t *const member = download_read(download, DOWNLOAD_FILE_ENVELOPE, &len);
    if (member == NULL) {
        return false;
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
        *out_short = true;
        return false;
    }
    char path[MESH_FETCH_PATH_MAX];
    if (!download_path(download, DOWNLOAD_FILE_ENVELOPE, path, sizeof path)) {
        free(member);
        return false;
    }
    FILE *const file = fopen(path, "wb");
    if (file == NULL) {
        free(member);
        return false;
    }

    /* Magic, deflate, no flags, no mtime, no extra flags, and an unknown OS. */
    static const uint8_t k_header[10] = {0x1FU, 0x8BU, 0x08U, 0x00U, 0x00U,
                                         0x00U, 0x00U, 0x00U, 0x00U, 0xFFU};
    (void)fwrite(k_header, 1U, sizeof k_header, file);

    if (download->entry.method == MESH_ZIP_METHOD_DEFLATE) {
        (void)fwrite(member, 1U, len, file);
    } else {
        size_t at = 0U;
        do {
            const size_t chunk =
                len - at > DOWNLOAD_STORED_BLOCK_MAX ? DOWNLOAD_STORED_BLOCK_MAX : len - at;
            const bool final = at + chunk >= len;
            const uint8_t block[5] = {
                (uint8_t)(final ? 0x01U : 0x00U), (uint8_t)(chunk & 0xFFU),
                (uint8_t)((chunk >> 8) & 0xFFU), (uint8_t)(~chunk & 0xFFU),
                (uint8_t)((~chunk >> 8) & 0xFFU)};
            (void)fwrite(block, 1U, sizeof block, file);
            (void)fwrite(member + at, 1U, chunk, file);
            at += chunk;
        } while (at < len);
    }

    download_put_u32(file, download->entry.crc32);
    download_put_u32(file, download->entry.uncompressed_size);
    const bool ok = ferror(file) == 0;
    free(member);
    return fclose(file) == 0 && ok;
}

static void download_step_inflate(struct mesh_firmware_download *download) {
    download->state = MESH_FIRMWARE_DOWNLOAD_INFLATING;
    bool arrived_short = false;
    if (!download_envelope(download, &arrived_short)) {
        download_fail(download, arrived_short ? MESH_FIRMWARE_DOWNLOAD_ERROR_NETWORK
                                              : MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }
    char envelope[MESH_FETCH_PATH_MAX];
    char image[MESH_FETCH_PATH_MAX];
    if (!download_path(download, DOWNLOAD_FILE_ENVELOPE, envelope, sizeof envelope) ||
        !download_path(download, DOWNLOAD_FILE_IMAGE, image, sizeof image)) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING);
        return;
    }
    if (pid == 0) {
        /*
         * Both ends are files, so there is no pipe to feed and nothing here can deadlock -
         * which is the whole reason the envelope is staged rather than streamed. The client's
         * one loop keeps drawing while this runs.
         */
        const int in = open(envelope, O_RDONLY | O_CLOEXEC);
        const int out = open(image, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (in < 0 || out < 0 || dup2(in, STDIN_FILENO) < 0 || dup2(out, STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(in);
        close(out);
        execlp("gzip", "gzip", "-dc", (char *)NULL);
        /* busybox names the same applet three ways and a system without gzip may still have
           one of the others. */
        execlp("gunzip", "gunzip", "-c", (char *)NULL);
        _exit(127);
    }
    download->inflater = pid;
    download->inflate_deadline_ms = mesh_time_monotonic_ms() + DOWNLOAD_INFLATE_TIMEOUT_MS;
}

static void download_reaped(struct mesh_firmware_download *download, int status) {
    download->inflater = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /*
         * `gzip: crc error` and `gzip: incorrect length` both land here, and both mean the
         * bytes arrived and are not the bytes the central directory described. Which is the
         * download's integrity check: there is no separate verify step because the inflate
         * *is* one.
         */
        mesh_log_error("firmware", "The inflate refused the member (status %d)", status);
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE);
        return;
    }
    const uint64_t size = download_file_size(download, DOWNLOAD_FILE_IMAGE);
    if (size != (uint64_t)download->entry.uncompressed_size) {
        /* Belt and braces: gzip already checks ISIZE, and a gzip that did not would leave a
           file the caller would go on to write to a radio. */
        mesh_log_error("firmware", "The image inflated to %llu bytes, not %u",
                       (unsigned long long)size, (unsigned)download->entry.uncompressed_size);
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE);
        return;
    }
    mesh_log_info("firmware", "Staged %llu bytes of firmware", (unsigned long long)size);
    download_finish(download, MESH_FIRMWARE_DOWNLOAD_READY,
                    MESH_FIRMWARE_DOWNLOAD_ERROR_NONE);
}

/* ---- the public half ----------------------------------------------------------------------*/

int mesh_firmware_download_start(struct mesh_firmware_download *download, struct mesh_fetch *fetch,
                                 const char *zip_url, const char *member, const char *staging_dir,
                                 mesh_firmware_download_done_fn on_done, void *userdata) {
    if (download == NULL || fetch == NULL || zip_url == NULL || member == NULL ||
        staging_dir == NULL || zip_url[0] == '\0' || member[0] == '\0' ||
        staging_dir[0] == '\0') {
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
    download->inflater = -1;
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
    if (download == NULL || download->inflater < 0) {
        return;
    }
    if (now_ms >= download->inflate_deadline_ms) {
        (void)kill(download->inflater, SIGKILL);
        (void)waitpid(download->inflater, NULL, 0);
        download->inflater = -1;
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE);
        return;
    }
    int status = 0;
    const pid_t reaped = waitpid(download->inflater, &status, WNOHANG);
    if (reaped == download->inflater) {
        download_reaped(download, status);
    } else if (reaped < 0) {
        download->inflater = -1;
        download_fail(download, MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE);
    }
}

void mesh_firmware_download_cancel(struct mesh_firmware_download *download) {
    if (download == NULL) {
        return;
    }
    if (download->fetch != NULL) {
        mesh_fetch_cancel(download->fetch);
    }
    if (download->inflater >= 0) {
        (void)kill(download->inflater, SIGKILL);
        (void)waitpid(download->inflater, NULL, 0);
        download->inflater = -1;
    }
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
    if (download->state == MESH_FIRMWARE_DOWNLOAD_READY) {
        return 100U;
    }
    if (download->state != MESH_FIRMWARE_DOWNLOAD_FETCHING ||
        download->entry.compressed_size == 0U) {
        return 0U;
    }
    const uint64_t landed = download_file_size(download, DOWNLOAD_FILE_ENVELOPE);
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
