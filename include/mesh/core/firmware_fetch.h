#pragma once

/*
 * From "there is a newer release for this board" to "the image is on disk and it is the right
 * one", in one operation.
 *
 * This is what closes phase 2 of docs/radio-firmware-roadmap.md. firmware.c answers *whether*
 * there is firmware; firmware_download.c gets *bytes*; this is the piece between them that
 * knows which zip and which member, and it exists because neither of those questions can be
 * answered without reading two more documents:
 *
 *   1. the release's own manifest (`firmware-<version>.json`) says which **platform** built
 *      this target - and the platform is what names the zip. It is not the architecture:
 *      `firmware-esp32-s3-<ver>.zip` is a 404 while `firmware-esp32s3-<ver>.zip` is 170 MB of
 *      zip, and the two spellings differ for exactly the ESP32-S3 family.
 *   2. the board's own `.mt.json`, *inside that zip*, says which of the files it publishes is
 *      the image. There is no guessing the name from a pattern here, and that is deliberate:
 *      an nRF52 board publishes a `.uf2`, an `.elf`, a `.hex` and an `-ota.zip`, and an ESP32
 *      board publishes four `.bin`s of which exactly one is the firmware and one is a whole
 *      flash including a fresh filesystem.
 *
 * So it is two downloads out of the same zip rather than one. The second costs a repeated HEAD
 * and a repeated 64 KB tail window - about 65 KB against the 58 MB not being downloaded - and
 * buys never having invented a file name.
 *
 * **The radio link must be down for the duration**, for the reason firmware_download.h gives:
 * one antenna. This module does not take that hold either; the caller does.
 */

#include "mesh/core/firmware_catalog.h"
#include "mesh/core/firmware_download.h"
#include "mesh/core/uf2.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One line for a row or a log: what happened, or why it did not. */
#define MESH_FIRMWARE_FETCH_MESSAGE_MAX 128U

enum mesh_firmware_fetch_state {
    MESH_FIRMWARE_FETCH_IDLE = 0,
    /* Reading the release's manifest: which platform's zip holds this target. */
    MESH_FIRMWARE_FETCH_RESOLVING,
    /* Range-reading the board's own `.mt.json` out of that zip. */
    MESH_FIRMWARE_FETCH_ASKING,
    /* Range-reading the image the manifest named. The only step with a bar worth drawing. */
    MESH_FIRMWARE_FETCH_DOWNLOADING,
    MESH_FIRMWARE_FETCH_READY,
    MESH_FIRMWARE_FETCH_FAILED,
    MESH_FIRMWARE_FETCH_STATE_COUNT,
};

/* Why it failed. The download's own errors come through unchanged; these are the ones this
   layer adds, and each is a different sentence. */
enum mesh_firmware_fetch_error {
    MESH_FIRMWARE_FETCH_ERROR_NONE = 0,
    /* No fetcher, or a bad argument. Nothing was started. */
    MESH_FIRMWARE_FETCH_ERROR_UNAVAILABLE,
    /* A document could not be fetched, or was not the shape it should be. */
    MESH_FIRMWARE_FETCH_ERROR_DOCUMENT,
    /* This release built nothing for this target. A real answer about the release, and the
       row that says so is not the row that says the network failed. */
    MESH_FIRMWARE_FETCH_ERROR_NO_TARGET,
    /* The `.mt.json` came back and names no image for this bus. */
    MESH_FIRMWARE_FETCH_ERROR_NO_IMAGE,
    /* The manifest describes a board on a different architecture from the one the caller
       resolved. Two documents disagreeing about what this board is, which is the moment to
       stop rather than to pick a side. */
    MESH_FIRMWARE_FETCH_ERROR_MISMATCH,
    /* A range read or the inflate failed; `download_error` says which. */
    MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD,
    /* The image arrived and is not for this chip, or is not a UF2 at all. The guard that
       protects the board rather than the download - see uf2.h. */
    MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE,
    MESH_FIRMWARE_FETCH_ERROR_COUNT,
};

struct mesh_firmware_fetch;
typedef void (*mesh_firmware_fetch_done_fn)(void *userdata,
                                            const struct mesh_firmware_fetch *fetch);

struct mesh_firmware_fetch {
    struct mesh_fetch *fetcher; /* borrowed */
    struct mesh_firmware_download download;

    enum mesh_firmware_fetch_state state;
    enum mesh_firmware_fetch_error error;
    enum mesh_firmware_download_error download_error;
    char message[MESH_FIRMWARE_FETCH_MESSAGE_MAX];

    /* What the caller asked for. */
    char target[MESH_FIRMWARE_TARGET_MAX];
    char version[MESH_FIRMWARE_VERSION_MAX];
    char manifest_url[MESH_FIRMWARE_URL_MAX];
    char expect_architecture[MESH_FIRMWARE_ARCH_MAX];
    char staging[MESH_FETCH_PATH_MAX];

    /* What the documents answered. */
    char platform[MESH_FIRMWARE_ARCH_MAX];
    char zip_url[MESH_FIRMWARE_DOWNLOAD_URL_MAX];
    char member[MESH_ZIP_NAME_MAX];
    struct mesh_firmware_manifest manifest;
    struct mesh_firmware_image image;
    enum mesh_firmware_path path;
    /* Only filled in on the USB path, where the image is a UF2 and can be read. */
    struct mesh_uf2_info uf2;

    mesh_firmware_fetch_done_fn on_done;
    void *userdata;
};

/*
 * Fetches the image `target` should be running at `version`.
 *
 * `manifest_url` is the release's own manifest, which is what the index calls `zip_url` and
 * what firmware.c already holds after a check. The zip's URL is derived from it rather than
 * assembled from a hostname, so nothing here knows it is talking to GitHub.
 *
 * `expect_architecture` is the architecture the caller resolved out of `deviceHardware`, and
 * it is a **cross-check**: the board's own manifest carries the same field, and a disagreement
 * fails rather than picking one. Passing an empty string means "no expectation", which is what
 * an inspection wants and what the install path must never do - the architecture is what
 * chooses the family the image is checked against, so with no expectation the guard checks the
 * image against itself.
 *
 * Returns 0, or -errno. On 0 `on_done` is called exactly once, later, from the loop.
 */
int mesh_firmware_fetch_start(struct mesh_firmware_fetch *fetch, struct mesh_fetch *fetcher,
                              const char *target, const char *version, const char *manifest_url,
                              const char *expect_architecture, const char *staging_dir,
                              mesh_firmware_fetch_done_fn on_done, void *userdata);

/* Drives the download's own tick. Call every loop turn. */
void mesh_firmware_fetch_tick(struct mesh_firmware_fetch *fetch, uint64_t now_ms);

/* Kills anything in flight and reports nothing. */
void mesh_firmware_fetch_cancel(struct mesh_firmware_fetch *fetch);

bool mesh_firmware_fetch_busy(const struct mesh_firmware_fetch *fetch);

/* 0-100 over the image itself, which is the only step big enough to watch. */
unsigned mesh_firmware_fetch_progress(const struct mesh_firmware_fetch *fetch);

/* Where the finished image is, or NULL until it is. */
const char *mesh_firmware_fetch_image_path(const struct mesh_firmware_fetch *fetch, char *out,
                                           size_t out_len);

const char *mesh_firmware_fetch_state_name(enum mesh_firmware_fetch_state state);

#ifdef __cplusplus
}
#endif
