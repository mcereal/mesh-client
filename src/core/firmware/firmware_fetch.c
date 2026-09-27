#include "mesh/core/firmware_fetch.h"

#include "mesh/core/esp_image.h"

#include "inkwell/base/file.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The release manifest is 9.8 KB at 2.7.26 - 129 targets of two keys each - and is read into
   memory rather than staged, because nothing downstream wants it as a file. */
#define FETCH_DOCUMENT_TIMEOUT_MS 20000U

/* Per-step deadlines for the range reads. The image is 0.5-1.5 MB compressed (an esp32s3 one
   is the large end) and lands in a few seconds over a Brick's Wi-Fi; the rest are a few
   kilobytes each and a round trip. */
#define FETCH_STEP_TIMEOUT_MS 20000U
#define FETCH_IMAGE_TIMEOUT_MS 120000U
/* And how long any of them may sit silent once connected before it is asked for again. The
   image's two minutes are for a slow link, not for a CDN that took the request and went quiet:
   one did, on a Brick, and the retry landed the image in five seconds. */
#define FETCH_IDLE_TIMEOUT_MS 15000U

/*
 * The most an image may claim to be, in or out of the zip. Both sizes come from a directory
 * somebody else served and the uncompressed one is allocated and inflated into on the loop, so
 * inkwell makes its caller say. No supported radio has more than 16 MB of flash, so no image can
 * be bigger; twice that is headroom for a board that does, not for a real image.
 */
#define FETCH_IMAGE_MAX (32U * 1024U * 1024U)

/* What the staged files are called: firmware.window, firmware.member, firmware.image ... */
#define FETCH_STAGING_STEM "firmware"

static void fetch_on_manifest(void *userdata, const struct inkwell_fetch_result *result);
static void fetch_on_direct(void *userdata, const struct inkwell_fetch_result *result);

/* Where the image landed, whichever way it came. */
static const char *fetch_output_path(const struct mesh_firmware_fetch *fetch, char *out,
                                     size_t out_len) {
    if (fetch->direct) {
        if (fetch->direct_path[0] == '\0' || strlen(fetch->direct_path) >= out_len) {
            return NULL;
        }
        inkwell_str_copy(out, out_len, fetch->direct_path);
        return out;
    }
    return inkwell_zip_fetch_output_path(&fetch->download, out, out_len);
}
static void fetch_on_download(void *userdata, const struct inkwell_zip_fetch *download);

static void fetch_finish(struct mesh_firmware_fetch *fetch, enum mesh_firmware_fetch_state state,
                         enum mesh_firmware_fetch_error error, const char *message) {
    fetch->state = state;
    fetch->error = error;
    inkwell_str_copy(fetch->message, sizeof fetch->message, message != NULL ? message : "");
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
    inkwell_log_error("firmware", "%s", message != NULL ? message : "the fetch failed");
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
    inkwell_str_copy(fetch->member, sizeof fetch->member, member);
    struct inkwell_zip_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = fetch->zip_url;
    request.member = fetch->member;
    request.staging_dir = fetch->staging;
    request.stem = FETCH_STAGING_STEM;
    request.max_member_bytes = FETCH_IMAGE_MAX;
    request.step_timeout_ms = FETCH_STEP_TIMEOUT_MS;
    request.member_timeout_ms = FETCH_IMAGE_TIMEOUT_MS;
    request.idle_timeout_ms = FETCH_IDLE_TIMEOUT_MS;
    request.on_done = fetch_on_download;
    request.userdata = fetch;
    return inkwell_zip_fetch_start(&fetch->download, fetch->fetcher, &request) == 0;
}

static void fetch_on_manifest(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_firmware_fetch *const fetch = (struct mesh_firmware_fetch *)userdata;
    if (result->outcome != INKWELL_FETCH_OK || result->body == NULL) {
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
    inkwell_log_info("firmware", "%s is built on %s; reading %s", fetch->target, fetch->platform,
                     fetch->zip_url);

    char member[INKWELL_ZIP_NAME_MAX];
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
    char path[INKWELL_FETCH_PATH_MAX];
    if (inkwell_zip_fetch_output_path(&fetch->download, path, sizeof path) == NULL) {
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
       wants; the install path always states one - and the bus, which it has to be one this
       architecture takes. */
    const char *const architecture = fetch->expect_architecture[0] != '\0'
                                         ? fetch->expect_architecture
                                         : fetch->manifest.architecture;
    fetch->path = fetch->bus != MESH_FIRMWARE_PATH_NONE
                      ? fetch->bus
                      : mesh_firmware_path_for_architecture(architecture);
    if (!mesh_firmware_architecture_takes(architecture, fetch->path)) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_NO_IMAGE,
                   "this board cannot be installed to over this bus");
        return;
    }

    const struct mesh_firmware_image *const image =
        mesh_firmware_manifest_image(&fetch->manifest, fetch->path);
    if (image == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_NO_IMAGE,
                   "this board publishes no image for this bus");
        return;
    }
    fetch->image = *image;
    inkwell_log_info("firmware", "The image is %s, %llu bytes", fetch->image.name,
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
    /* A direct download was measured as it landed, against the size its release gave. */
    if (!fetch->direct && inkwell_zip_fetch_size(&fetch->download) != fetch->image.bytes) {
        /* The zip's central directory and the board's manifest disagree about how long the
           image is. Neither is more authoritative than the other, so this stops. */
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE,
                   "the image is not the length its manifest describes");
        return;
    }
    const char *const expected = fetch->expect_architecture[0] != '\0'
                                     ? fetch->expect_architecture
                                     : fetch->manifest.architecture;
    if (fetch->path != MESH_FIRMWARE_PATH_USB ||
        mesh_firmware_architecture_uses_esp_rom(expected)) {
        /* An ESP32 app image is a blob with a one-byte magic and nothing that names the board
           it is for. Phase 4 checks it by handing the loader a hash and being refused; there
           is nothing to read here. An nRF52's DFU package is checked by the install that
           opens it, against the CRC16 its own init packet carries. Over USB an ESP32's image
           goes to its ROM, and the serial handover reads its header for the chip before
           anything is erased. */
        fetch_finish(fetch, MESH_FIRMWARE_FETCH_READY, MESH_FIRMWARE_FETCH_ERROR_NONE, "");
        return;
    }

    char path[INKWELL_FETCH_PATH_MAX];
    if (fetch_output_path(fetch, path, sizeof path) == NULL) {
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
    const enum inkwell_uf2_verdict verdict = inkwell_uf2_validate(bytes, got, family, &fetch->uf2);
    free(bytes);
    if (verdict != INKWELL_UF2_OK) {
        char message[MESH_FIRMWARE_FETCH_MESSAGE_MAX];
        snprintf(message, sizeof message, "the image is not a UF2 for this board (verdict %d)",
                 (int)verdict);
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE, message);
        return;
    }
    inkwell_log_info("firmware", "%u blocks, family %08x, %#x-%#x", (unsigned)fetch->uf2.blocks,
                     (unsigned)fetch->uf2.family_id, (unsigned)fetch->uf2.first_address,
                     (unsigned)fetch->uf2.last_address);
    fetch_finish(fetch, MESH_FIRMWARE_FETCH_READY, MESH_FIRMWARE_FETCH_ERROR_NONE, "");
}

/* What went wrong under "download failed", which is what the toast already says - so this is
   the part a reader could not have guessed, never the category again. */
static const char *fetch_download_detail(enum inkwell_zip_fetch_error error) {
    switch (error) {
    case INKWELL_ZIP_FETCH_ERROR_NETWORK:
        /* A lost connection, a silent server and an HTTP error all land here - so nothing that
           is only true of one of them. */
        return "the release host did not deliver it; try again";
    case INKWELL_ZIP_FETCH_ERROR_STAGING:
        return "the download could not be saved";
    case INKWELL_ZIP_FETCH_ERROR_NOT_A_ZIP:
        return "the release is not a zip";
    case INKWELL_ZIP_FETCH_ERROR_NO_MEMBER:
        return "the release zip has no such file";
    case INKWELL_ZIP_FETCH_ERROR_UNSUPPORTED:
        return "the release zip is in a form this client cannot read";
    case INKWELL_ZIP_FETCH_ERROR_INFLATE:
        return "the image arrived damaged; try again";
    case INKWELL_ZIP_FETCH_ERROR_NONE:
    case INKWELL_ZIP_FETCH_ERROR_COUNT:
    default:
        return "";
    }
}

static void fetch_on_download(void *userdata, const struct inkwell_zip_fetch *download) {
    struct mesh_firmware_fetch *const fetch = (struct mesh_firmware_fetch *)userdata;
    if (download->state != INKWELL_ZIP_FETCH_READY) {
        fetch->download_error = download->error;
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD,
                   fetch_download_detail(download->error));
        return;
    }
    if (fetch->state == MESH_FIRMWARE_FETCH_ASKING) {
        fetch_read_manifest(fetch);
    } else {
        fetch_check_image(fetch);
    }
}

/* ---- or the image, directly ---------------------------------------------------------------*/

/*
 * An image a release named directly has landed. It is the length its release said, or it is
 * not the file that was described; and an ESP32 family image names its chip in its header,
 * which is what the handover will hold the chip on the cable to.
 */
static void fetch_check_direct(struct mesh_firmware_fetch *fetch) {
    size_t len = 0U;
    uint8_t *const bytes = inkwell_file_read(fetch->direct_path, FETCH_IMAGE_MAX, &len);
    if (bytes == NULL) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD, "the image is unreadable");
        return;
    }
    if ((uint64_t)len != fetch->image.bytes) {
        free(bytes);
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE,
                   "the image is not the length its release describes");
        return;
    }
    if (!mesh_firmware_architecture_uses_esp_rom(fetch->expect_architecture)) {
        free(bytes);
        fetch_check_image(fetch);
        return;
    }
    /* Read against the chip the header claims, so what is being asked is whether this is an
       application at all; which chip is the handover's question, put to the chip itself. */
    struct inkwell_esp_image_info info;
    memset(&info, 0, sizeof info);
    (void)inkwell_esp_image_validate(bytes, len, 0U, &info);
    const enum inkwell_esp_image_verdict verdict =
        inkwell_esp_image_validate(bytes, len, info.chip_id, &info);
    free(bytes);
    const char *const architecture = mesh_esp_architecture_for_chip(info.chip_id);
    if (verdict != INKWELL_ESP_IMAGE_OK || architecture == NULL ||
        !mesh_firmware_architecture_takes(architecture, fetch->path)) {
        char message[MESH_FIRMWARE_FETCH_MESSAGE_MAX];
        snprintf(message, sizeof message,
                 "the image is not an app this client can write (%s, "
                 "chip 0x%04x)",
                 inkwell_esp_image_verdict_name(verdict), (unsigned)info.chip_id);
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE, message);
        return;
    }
    inkwell_str_copy(fetch->manifest.architecture, sizeof fetch->manifest.architecture,
                     architecture);
    inkwell_log_info("firmware", "The image is an %s app, %zu bytes", architecture, len);
    fetch_finish(fetch, MESH_FIRMWARE_FETCH_READY, MESH_FIRMWARE_FETCH_ERROR_NONE, "");
}

static void fetch_on_direct(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_firmware_fetch *const fetch = (struct mesh_firmware_fetch *)userdata;
    if (fetch == NULL || fetch->state != MESH_FIRMWARE_FETCH_DOWNLOADING) {
        return;
    }
    if (result->outcome == INKWELL_FETCH_TOO_LARGE) {
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE,
                   "the image is longer than its release describes");
        return;
    }
    if (result->outcome != INKWELL_FETCH_OK) {
        inkwell_log_warn("firmware", "The image download failed: %s (%s)",
                         inkwell_fetch_outcome_name(result->outcome), result->detail);
        fetch_fail(fetch, MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD,
                   fetch_download_detail(INKWELL_ZIP_FETCH_ERROR_NETWORK));
        return;
    }
    fetch_check_direct(fetch);
}

int mesh_firmware_fetch_start_direct(struct mesh_firmware_fetch *fetch,
                                     struct inkwell_fetch *fetcher, const char *image_url,
                                     const char *image_name, uint64_t bytes,
                                     const char *expect_architecture, enum mesh_firmware_path bus,
                                     const char *staging_dir, mesh_firmware_fetch_done_fn on_done,
                                     void *userdata) {
    if (fetch == NULL || fetcher == NULL || image_url == NULL || image_name == NULL ||
        expect_architecture == NULL || staging_dir == NULL || image_url[0] == '\0' ||
        staging_dir[0] == '\0' || bytes == 0U || bytes > FETCH_IMAGE_MAX ||
        bus == MESH_FIRMWARE_PATH_NONE) {
        return -EINVAL;
    }
    if (mesh_firmware_fetch_busy(fetch)) {
        return -EBUSY;
    }
    if (!inkwell_fetch_available(fetcher)) {
        return -ENOTSUP;
    }
    if (!mesh_firmware_architecture_takes(expect_architecture, bus)) {
        return -ENOTSUP;
    }

    memset(fetch, 0, sizeof *fetch);
    fetch->fetcher = fetcher;
    fetch->direct = true;
    inkwell_str_copy(fetch->direct_url, sizeof fetch->direct_url, image_url);
    const int written = snprintf(fetch->direct_path, sizeof fetch->direct_path, "%s/%s.image",
                                 staging_dir, FETCH_STAGING_STEM);
    if (written < 0 || (size_t)written >= sizeof fetch->direct_path) {
        return -ENAMETOOLONG;
    }
    inkwell_str_copy(fetch->expect_architecture, sizeof fetch->expect_architecture,
                     expect_architecture);
    inkwell_str_copy(fetch->manifest.architecture, sizeof fetch->manifest.architecture,
                     expect_architecture);
    inkwell_str_copy(fetch->staging, sizeof fetch->staging, staging_dir);
    inkwell_str_copy(fetch->image.name, sizeof fetch->image.name, image_name);
    fetch->image.bytes = bytes;
    fetch->bus = bus;
    fetch->path = bus;
    fetch->on_done = on_done;
    fetch->userdata = userdata;
    fetch->state = MESH_FIRMWARE_FETCH_DOWNLOADING;
    (void)remove(fetch->direct_path);

    struct inkwell_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = fetch->direct_url;
    request.output_path = fetch->direct_path;
    request.output_max = bytes;
    request.timeout_ms = FETCH_IMAGE_TIMEOUT_MS;
    request.idle_timeout_ms = FETCH_IDLE_TIMEOUT_MS;
    request.on_done = fetch_on_direct;
    request.userdata = fetch;
    const int started = inkwell_fetch_start(fetcher, &request, inkwell_time_monotonic_ms());
    if (started != 0) {
        fetch->state = MESH_FIRMWARE_FETCH_IDLE;
        fetch->on_done = NULL;
        return started;
    }
    inkwell_log_info("firmware", "Fetching %s, %llu bytes", image_name, (unsigned long long)bytes);
    return 0;
}

/* ---- the public half ----------------------------------------------------------------------*/

int mesh_firmware_fetch_start(struct mesh_firmware_fetch *fetch, struct inkwell_fetch *fetcher,
                              const char *target, const char *version, const char *manifest_url,
                              const char *expect_architecture, enum mesh_firmware_path bus,
                              const char *staging_dir, mesh_firmware_fetch_done_fn on_done,
                              void *userdata) {
    if (fetch == NULL || fetcher == NULL || target == NULL || version == NULL ||
        manifest_url == NULL || staging_dir == NULL || target[0] == '\0' || version[0] == '\0' ||
        manifest_url[0] == '\0' || staging_dir[0] == '\0') {
        return -EINVAL;
    }
    if (mesh_firmware_fetch_busy(fetch)) {
        return -EBUSY;
    }
    if (!inkwell_fetch_available(fetcher)) {
        return -ENOTSUP;
    }

    memset(fetch, 0, sizeof *fetch);
    fetch->fetcher = fetcher;
    inkwell_str_copy(fetch->target, sizeof fetch->target, target);
    inkwell_str_copy(fetch->version, sizeof fetch->version, version);
    inkwell_str_copy(fetch->manifest_url, sizeof fetch->manifest_url, manifest_url);
    inkwell_str_copy(fetch->expect_architecture, sizeof fetch->expect_architecture,
                     expect_architecture != NULL ? expect_architecture : "");
    inkwell_str_copy(fetch->staging, sizeof fetch->staging, staging_dir);
    fetch->bus = bus;
    fetch->on_done = on_done;
    fetch->userdata = userdata;
    fetch->state = MESH_FIRMWARE_FETCH_RESOLVING;

    struct inkwell_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = fetch->manifest_url;
    request.timeout_ms = FETCH_DOCUMENT_TIMEOUT_MS;
    request.on_done = fetch_on_manifest;
    request.userdata = fetch;
    const int started = inkwell_fetch_start(fetcher, &request, inkwell_time_monotonic_ms());
    if (started != 0) {
        fetch->state = MESH_FIRMWARE_FETCH_IDLE;
        fetch->on_done = NULL;
        return started;
    }
    return 0;
}

void mesh_firmware_fetch_tick(struct mesh_firmware_fetch *fetch, uint64_t now_ms) {
    if (fetch != NULL) {
        inkwell_zip_fetch_tick(&fetch->download, now_ms);
    }
}

void mesh_firmware_fetch_cancel(struct mesh_firmware_fetch *fetch) {
    if (fetch == NULL) {
        return;
    }
    inkwell_zip_fetch_cancel(&fetch->download);
    if (fetch->fetcher != NULL) {
        inkwell_fetch_cancel(fetch->fetcher);
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
    if (fetch->state != MESH_FIRMWARE_FETCH_DOWNLOADING) {
        return 0U;
    }
    if (fetch->direct) {
        struct stat landed;
        if (fetch->image.bytes == 0U || stat(fetch->direct_path, &landed) != 0) {
            return 0U;
        }
        const uint64_t have = (uint64_t)landed.st_size;
        return have >= fetch->image.bytes ? 100U : (unsigned)((have * 100U) / fetch->image.bytes);
    }
    return inkwell_zip_fetch_progress(&fetch->download);
}

const char *mesh_firmware_fetch_image_path(const struct mesh_firmware_fetch *fetch, char *out,
                                           size_t out_len) {
    if (fetch == NULL || fetch->state != MESH_FIRMWARE_FETCH_READY) {
        return NULL;
    }
    return fetch_output_path(fetch, out, out_len);
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
