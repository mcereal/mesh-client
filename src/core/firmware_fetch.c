#include "mesh/core/firmware_fetch.h"

#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The release manifest is 9.8 KB at 2.7.26 - 129 targets of two keys each - and is read into
   memory rather than staged, because nothing downstream wants it as a file. */
#define FETCH_DOCUMENT_TIMEOUT_MS 20000U

static void fetch_on_manifest(void *userdata, const struct mesh_fetch_result *result);
static void fetch_on_download(void *userdata, const struct mesh_firmware_download *download);

static void fetch_finish(struct mesh_firmware_fetch *fetch, enum mesh_firmware_fetch_state state,
                         enum mesh_firmware_fetch_error error, const char *message) {
    fetch->state = state;
    fetch->error = error;
    mesh_str_copy(fetch->message, sizeof fetch->message, message != NULL ? message : "");
    if (fetch->on_done != NULL) {
        const mesh_firmware_fetch_done_fn done = fetch->on_done;
        void *const userdata = fetch->userdata;
        fetch->on_done = NULL;
        fetch->userdata = NULL;
        done(userdata, fetch);
    }
}

static void fetch_fail(struct mesh_firmware_fetch *fetch, enum mesh_firmware_fetch_error error,
                       const char *message) {
    mesh_log_error("firmware", "%s", message != NULL ? message : "the fetch failed");
    fetch_finish(fetch, MESH_FIRMWARE_FETCH_FAILED, error, message);
}

/* ---- step two: the zip's URL ---------------------------------------------------------------*/

/*
 * `.../download/v2.7.26.54e0d8d/firmware-2.7.26.54e0d8d.json`
 *   becomes
 * `.../download/v2.7.26.54e0d8d/firmware-nrf52840-2.7.26.54e0d8d.zip`
 *
 * Derived from the manifest's own URL rather than assembled from a hostname, so nothing in
 * this client knows it is talking to GitHub - which is also what lets the whole chain be
 * pointed at a local file server in a test.
 */
static bool fetch_build_zip_url(struct mesh_firmware_fetch *fetch) {
    const char *const slash = strrchr(fetch->manifest_url, '/');
    if (slash == NULL) {
        return false;
    }
    const size_t dir_len = (size_t)(slash - fetch->manifest_url) + 1U;
    if (dir_len >= sizeof fetch->zip_url) {
        return false;
    }
    memcpy(fetch->zip_url, fetch->manifest_url, dir_len);
    const int written = snprintf(fetch->zip_url + dir_len, sizeof fetch->zip_url - dir_len,
                                 "firmware-%s-%s.zip", fetch->platform, fetch->version);
    return written > 0 && (size_t)written < sizeof fetch->zip_url - dir_len;
}

static bool fetch_start_download(struct mesh_firmware_fetch *fetch, const char *member) {
    mesh_str_copy(fetch->member, sizeof fetch->member, member);
    const int started =
        mesh_firmware_download_start(&fetch->download, fetch->fetcher, fetch->zip_url,
                                     fetch->member, fetch->staging, fetch_on_download, fetch);
    return started == 0;
}

static void fetch_on_manifest(void *userdata, const struct mesh_fetch_result *result) {
    struct mesh_firmware_fetch *const fetch = (struct mesh_firmware_fetch *)userdata;
    if (result->outcome != MESH_FETCH_OK || result->body == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOCUMENT,
                   "the release manifest could not be read");
        return;
    }
    if (!mesh_firmware_platform_parse(result->body, result->len, fetch->target, fetch->platform,
                                      sizeof fetch->platform)) {
        /* The document parsed and this target is not in it, or it did not parse at all. Both
           come back here, and both mean there is no zip to open - but the message says which,
           because "upstream dropped your board" and "the reply was not a manifest" send a
           reader to two different places. */
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_NO_TARGET,
                   "this release built nothing for this board");
        return;
    }
    if (!fetch_build_zip_url(fetch)) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOCUMENT,
                   "the release URL is not one we "
                   "can build a zip name from");
        return;
    }
    mesh_log_info("firmware", "%s is built on %s; reading %s", fetch->target, fetch->platform,
                  fetch->zip_url);

    char member[MESH_ZIP_NAME_MAX];
    const int written =
        snprintf(member, sizeof member, "firmware-%s-%s.mt.json", fetch->target, fetch->version);
    if (written <= 0 || (size_t)written >= sizeof member) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOCUMENT,
                   "the manifest member name is too "
                   "long to ask for");
        return;
    }
    fetch->state = MESH_FIRMWARE_FETCH_ASKING;
    if (!fetch_start_download(fetch, member)) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_UNAVAILABLE, "the download would not start");
    }
}

/* ---- step three: what the board says about itself ------------------------------------------*/

static void fetch_read_manifest(struct mesh_firmware_fetch *fetch) {
    char path[MESH_FETCH_PATH_MAX];
    if (mesh_firmware_download_image_path(&fetch->download, path, sizeof path) == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOCUMENT, "the board manifest went missing");
        return;
    }
    /*
     * Read *now*, before the next download starts: both land on the same staged path, so the
     * image about to arrive overwrites the document that named it.
     */
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOCUMENT, "the board manifest is unreadable");
        return;
    }
    char text[4096];
    const size_t got = fread(text, 1U, sizeof text - 1U, file);
    fclose(file);
    text[got] = '\0';

    if (!mesh_firmware_manifest_parse(text, got, &fetch->manifest)) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOCUMENT,
                   "the board manifest is not a manifest");
        return;
    }
    /*
     * The manifest has to be about the board and the release we asked for.
     *
     * This is checked *first* and unconditionally, and both of those matter. The member was
     * found by the basename `firmware-<target>-<version>.mt.json`, so a mismatch here means an
     * archive whose file says one thing on the outside and another on the inside - and what it
     * would cost is the failure this whole feature is built around not having: two nRF52840
     * boards share an architecture, share a UF2 family, and would sail through every check
     * after this one. The size and the CRC would agree too, because they would be that other
     * board's image and it really is intact.
     *
     * Unconditional because it needs nothing from the caller. The architecture check below can
     * only fire when somebody supplied an expectation, which the inspection path deliberately
     * does not - so without this, `--fetch-firmware` had no cross-check at all.
     */
    if (!mesh_firmware_manifest_describes(&fetch->manifest, fetch->target, fetch->version)) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_MISMATCH,
                   "the manifest in this archive is for a different board or release");
        return;
    }
    /*
     * And the architecture, where the caller stated one. Two documents describing one board,
     * and a disagreement means the target resolved to something whose manifest is about a
     * different chip - at which point picking a side is picking which way to be wrong.
     */
    if (fetch->expect_architecture[0] != '\0' &&
        strcmp(fetch->expect_architecture, fetch->manifest.architecture) != 0) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_MISMATCH,
                   "the board's manifest names a different architecture");
        return;
    }
    /* With no expectation the manifest is taken at its word, which is what an inspection
       wants; the install path always states one. */
    fetch->path = mesh_firmware_path_for_architecture(fetch->expect_architecture[0] != '\0'
                                                          ? fetch->expect_architecture
                                                          : fetch->manifest.architecture);

    const struct mesh_firmware_image *const image =
        mesh_firmware_manifest_image(&fetch->manifest, fetch->path);
    if (image == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_NO_IMAGE,
                   "this board publishes no image for this bus");
        return;
    }
    fetch->image = *image;
    mesh_log_info("firmware", "The image is %s, %llu bytes", fetch->image.name,
                  (unsigned long long)fetch->image.bytes);

    fetch->state = MESH_FIRMWARE_FETCH_DOWNLOADING;
    if (!fetch_start_download(fetch, fetch->image.name)) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_UNAVAILABLE,
                   "the image download would not "
                   "start");
    }
}

/* ---- step four: is this the image for this board ------------------------------------------*/

static void fetch_check_image(struct mesh_firmware_fetch *fetch) {
    if (mesh_firmware_download_image_size(&fetch->download) != fetch->image.bytes) {
        /* The zip's central directory and the board's manifest disagree about how long the
           image is. Neither is more authoritative than the other, so this stops. */
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE,
                   "the image is not the length its manifest describes");
        return;
    }
    if (fetch->path != MESH_FIRMWARE_PATH_USB) {
        /* An ESP32 app image is a blob with a one-byte magic and nothing that names the board
           it is for. Phase 4 checks it by handing the loader a hash and being refused; there
           is nothing to read here. */
        fetch_finish(fetch, MESH_FIRMWARE_FETCH_READY, MESH_FIRMWARE_FETCH_ERROR_NONE, "");
        return;
    }

    char path[MESH_FETCH_PATH_MAX];
    if (mesh_firmware_download_image_path(&fetch->download, path, sizeof path) == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD, "the image went missing");
        return;
    }
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD, "the image is unreadable");
        return;
    }
    uint8_t *const bytes = malloc((size_t)fetch->image.bytes);
    if (bytes == NULL) {
        fclose(file);
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD,
                   "there is no room to read the "
                   "image");
        return;
    }
    const size_t got = fread(bytes, 1U, (size_t)fetch->image.bytes, file);
    fclose(file);

    /*
     * The family is taken from the architecture the *caller* resolved wherever there is one,
     * so the guard is checking the file against what the radio is rather than against what the
     * file says it is. With no expectation this degrades to reading the file's own answer,
     * which is why the install path always states one.
     */
    const char *const architecture = fetch->expect_architecture[0] != '\0'
                                         ? fetch->expect_architecture
                                         : fetch->manifest.architecture;
    const uint32_t family = mesh_uf2_family_for_architecture(architecture);
    const enum mesh_uf2_verdict verdict = mesh_uf2_validate(bytes, got, family, &fetch->uf2);
    free(bytes);
    if (verdict != MESH_UF2_OK) {
        char message[MESH_FIRMWARE_FETCH_MESSAGE_MAX];
        snprintf(message, sizeof message, "the image is not a UF2 for this board (verdict %d)",
                 (int)verdict);
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE, message);
        return;
    }
    mesh_log_info("firmware", "%u blocks, family %08x, %#x-%#x", (unsigned)fetch->uf2.blocks,
                  (unsigned)fetch->uf2.family_id, (unsigned)fetch->uf2.first_address,
                  (unsigned)fetch->uf2.last_address);
    fetch_finish(fetch, MESH_FIRMWARE_FETCH_READY, MESH_FIRMWARE_FETCH_ERROR_NONE, "");
}

static void fetch_on_download(void *userdata, const struct mesh_firmware_download *download) {
    struct mesh_firmware_fetch *const fetch = (struct mesh_firmware_fetch *)userdata;
    if (download->state != MESH_FIRMWARE_DOWNLOAD_READY) {
        fetch->download_error = download->error;
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD,
                   download->error == MESH_FIRMWARE_DOWNLOAD_ERROR_NO_MEMBER
                       ? "the release zip has no such file"
                       : "the download failed");
        return;
    }
    if (fetch->state == MESH_FIRMWARE_FETCH_ASKING) {
        fetch_read_manifest(fetch);
    } else {
        fetch_check_image(fetch);
    }
}

/* ---- the public half ----------------------------------------------------------------------*/

int mesh_firmware_fetch_start(struct mesh_firmware_fetch *fetch, struct mesh_fetch *fetcher,
                              const char *target, const char *version, const char *manifest_url,
                              const char *expect_architecture, const char *staging_dir,
                              mesh_firmware_fetch_done_fn on_done, void *userdata) {
    if (fetch == NULL || fetcher == NULL || target == NULL || version == NULL ||
        manifest_url == NULL || staging_dir == NULL || target[0] == '\0' || version[0] == '\0' ||
        manifest_url[0] == '\0' || staging_dir[0] == '\0') {
        return -EINVAL;
    }
    if (mesh_firmware_fetch_busy(fetch)) {
        return -EBUSY;
    }
    if (!mesh_fetch_available(fetcher)) {
        return -ENOTSUP;
    }

    memset(fetch, 0, sizeof *fetch);
    fetch->fetcher = fetcher;
    mesh_str_copy(fetch->target, sizeof fetch->target, target);
    mesh_str_copy(fetch->version, sizeof fetch->version, version);
    mesh_str_copy(fetch->manifest_url, sizeof fetch->manifest_url, manifest_url);
    mesh_str_copy(fetch->expect_architecture, sizeof fetch->expect_architecture,
                  expect_architecture != NULL ? expect_architecture : "");
    mesh_str_copy(fetch->staging, sizeof fetch->staging, staging_dir);
    fetch->on_done = on_done;
    fetch->userdata = userdata;
    fetch->state = MESH_FIRMWARE_FETCH_RESOLVING;

    struct mesh_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = fetch->manifest_url;
    request.timeout_ms = FETCH_DOCUMENT_TIMEOUT_MS;
    request.on_done = fetch_on_manifest;
    request.userdata = fetch;
    const int started = mesh_fetch_start(fetcher, &request, mesh_time_monotonic_ms());
    if (started != 0) {
        fetch->state = MESH_FIRMWARE_FETCH_IDLE;
        fetch->on_done = NULL;
        return started;
    }
    return 0;
}

void mesh_firmware_fetch_tick(struct mesh_firmware_fetch *fetch, uint64_t now_ms) {
    if (fetch != NULL) {
        mesh_firmware_download_tick(&fetch->download, now_ms);
    }
}

void mesh_firmware_fetch_cancel(struct mesh_firmware_fetch *fetch) {
    if (fetch == NULL) {
        return;
    }
    mesh_firmware_download_cancel(&fetch->download);
    if (fetch->fetcher != NULL) {
        mesh_fetch_cancel(fetch->fetcher);
    }
    fetch->on_done = NULL;
    fetch->userdata = NULL;
    fetch->state = MESH_FIRMWARE_FETCH_IDLE;
}

bool mesh_firmware_fetch_busy(const struct mesh_firmware_fetch *fetch) {
    if (fetch == NULL) {
        return false;
    }
    return fetch->state != MESH_FIRMWARE_FETCH_IDLE && fetch->state != MESH_FIRMWARE_FETCH_READY &&
           fetch->state != MESH_FIRMWARE_FETCH_FAILED;
}

unsigned mesh_firmware_fetch_progress(const struct mesh_firmware_fetch *fetch) {
    if (fetch == NULL) {
        return 0U;
    }
    if (fetch->state == MESH_FIRMWARE_FETCH_READY) {
        return 100U;
    }
    /* Only the image itself: the two documents are a few kilobytes each and a bar that jumped
       to 100 and back to 0 twice before the download started would be describing our work
       rather than theirs. */
    return fetch->state == MESH_FIRMWARE_FETCH_DOWNLOADING
               ? mesh_firmware_download_progress(&fetch->download)
               : 0U;
}

const char *mesh_firmware_fetch_image_path(const struct mesh_firmware_fetch *fetch, char *out,
                                           size_t out_len) {
    if (fetch == NULL || fetch->state != MESH_FIRMWARE_FETCH_READY) {
        return NULL;
    }
    return mesh_firmware_download_image_path(&fetch->download, out, out_len);
}

const char *mesh_firmware_fetch_state_name(enum mesh_firmware_fetch_state state) {
    switch (state) {
    case MESH_FIRMWARE_FETCH_IDLE:
        return "idle";
    case MESH_FIRMWARE_FETCH_RESOLVING:
        return "resolving";
    case MESH_FIRMWARE_FETCH_ASKING:
        return "reading the board manifest";
    case MESH_FIRMWARE_FETCH_DOWNLOADING:
        return "downloading";
    case MESH_FIRMWARE_FETCH_READY:
        return "ready";
    case MESH_FIRMWARE_FETCH_FAILED:
        return "failed";
    case MESH_FIRMWARE_FETCH_STATE_COUNT:
    default:
        return "unknown";
    }
}
