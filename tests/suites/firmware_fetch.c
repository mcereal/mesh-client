#define _POSIX_C_SOURCE 200809L

/*
 * The orchestration of a firmware download: a target and a release become a zip URL and a
 * member name, and the two documents that decide them are read in order.
 *
 * An HTTPS server on loopback (support/https_fixture.h) plays the release host and its CDN: the
 * release's own manifest whole, and ranges out of two fixtures the way the CDN serves them out of
 * one 46 MB zip - a 302 from the release host to the CDN, a HEAD whose length is the file's rather
 * than the redirect's, the tail window at 46,194,237, the local header at 2,540,989 and the
 * member's deflated bytes at 2,541,100. Every one of those offsets is where that release really
 * keeps them.
 *
 * The range reads, the inflate and the CRC check are inkwell's `net/zip_fetch.h`, and its own
 * suite runs them against the same fixtures; what is here is the *resolution* above them.
 */

#include "framework/mesh_test.h"

#include "inkwell/net/fetch.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/firmware_catalog.h"
#include "mesh/core/firmware_fetch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MESH_TEST_DATA_DIR
#define MESH_TEST_DATA_DIR "tests/data"
#endif

#ifdef INKWELL_HAVE_TLS

#include "support/https_fixture.h"

/* The 2.7.26 nrf52840 zip, and where the two fixtures sit inside it. */
#define ZIP_SIZE "46259773"
#define TAIL_BASE 46194237
#define MEMBER_BASE 2540989

/* Reads `count` bytes at `offset` of a fixture into `out`. Returns how many it could. */
static size_t download_slice(const char *name, uint64_t offset, uint64_t count, char *out) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", MESH_TEST_DATA_DIR, name);
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return 0U;
    }
    size_t got = 0U;
    if (fseek(file, (long)offset, SEEK_SET) == 0) {
        got = fread(out, 1U, (size_t)count, file);
    }
    fclose(file);
    return got;
}

/*
 * The fake release host and CDN, in the fixture's child.
 *
 * Every zip URL answers 302 to a second host, the way a GitHub release asset does - and the 302
 * carries `content-length: 0`, so a HEAD that reported the first hop's length would measure
 * every zip as empty.
 */
static void download_serve(void *userdata, const struct https_fixture_request *request,
                           struct https_fixture_conn *conn) {
    (void)userdata;
    const size_t target_len = strlen(request->target);
    if (target_len > 5U && strcmp(request->target + target_len - 5U, ".json") == 0) {
        /* The release's own manifest, which is fetched whole. */
        char path[512];
        snprintf(path, sizeof path, "%s/firmware_release_2.7.26.json", MESH_TEST_DATA_DIR);
        https_fixture_reply_file(conn, request, path);
        return;
    }
    if (strcmp(request->host, "objects.githubusercontent.com") != 0) {
        https_fixture_reply(conn, 302, "Location: https://objects.githubusercontent.com/zip\r\n",
                            NULL, 0U);
        return;
    }
    if (strcmp(request->method, "HEAD") == 0) {
        https_fixture_printf(conn, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n"
                                   "Content-Length: " ZIP_SIZE "\r\n\r\n");
        return;
    }
    if (!request->ranged) {
        https_fixture_reply(conn, 403, NULL, NULL, 0U);
        return;
    }
    const uint64_t first = request->first;
    const uint64_t count = request->last - first + 1U;
    static char body[1024U * 1024U];
    size_t got = 0U;
    if (count <= sizeof body && first >= TAIL_BASE) {
        got = download_slice("zip_tail_nrf52840_2.7.26.bin", first - TAIL_BASE, count, body);
    } else if (count <= sizeof body && first >= MEMBER_BASE) {
        got =
            download_slice("zip_member_t114_mt_json_2.7.26.bin", first - MEMBER_BASE, count, body);
    } else {
        https_fixture_reply(conn, 416, NULL, NULL, 0U);
        return;
    }
    char range[96];
    snprintf(range, sizeof range, "Content-Range: bytes %llu-%llu/" ZIP_SIZE "\r\n",
             (unsigned long long)first, (unsigned long long)(first + got - 1U));
    https_fixture_reply(conn, 206, range, body, got);
}

/* Stands the fake CDN up, and points `fetch` at it. */
static bool download_serve_as(struct https_fixture *server, struct inkwell_fetch *fetch) {
    https_fixture_stop(server);
    if (!https_fixture_start(server, download_serve, NULL)) {
        return false;
    }
    https_fixture_attach(server, fetch);
    return true;
}

/* The orchestrator's completion, which carries no result of its own: everything worth
   asserting is on the struct, which outlives the call. */
struct fetch_probe {
    unsigned calls;
};

static void fetch_probe_done(void *userdata, const struct mesh_firmware_fetch *fetch) {
    (void)fetch;
    ((struct fetch_probe *)userdata)->calls++;
}

static bool fetch_wait_done(struct inkwell_loop *loop, struct inkwell_fetch *fetcher,
                            struct mesh_firmware_fetch *fetch, const struct fetch_probe *probe) {
    for (int turn = 0; turn < 600 && probe->calls == 0U; ++turn) {
        (void)inkwell_loop_run(loop, 10);
        inkwell_fetch_tick(fetcher, 0U);
        mesh_firmware_fetch_tick(fetch, 0U);
    }
    return probe->calls > 0U;
}

/* Removes the staged files and the temporary directory, whatever the case did. */
static void download_clean_dir(const char *dir) {
    static const char *const k_files[] = {"firmware.window", "firmware.central", "firmware.header",
                                          "firmware.member", "firmware.image"};
    char path[512];
    for (size_t i = 0; i < sizeof k_files / sizeof k_files[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", dir, k_files[i]);
        (void)unlink(path);
    }
    (void)rmdir(dir);
}

/*
 * A target and a release become a zip URL and a
 * member name, and the two documents that decide them are read in order.
 *
 * Both cases here stop before the image, which is deliberate - the image download is what
 * inkwell's net_zip_fetch suite covers, and what these are about is the *resolution*. Each is a
 * refusal that has to be its own row: "this release built nothing for your board" is an answer
 * about upstream, and "these two documents describe different chips" is a reason to stop rather
 * than to pick.
 */
MESH_TEST_CASE(firmware_fetch_resolves_a_target_to_a_zip_and_a_member, unit) {
    char dir[] = "/tmp/meshclient_fwdl_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct inkwell_fetch fetcher;
    struct mesh_firmware_fetch fetch;
    struct fetch_probe probe;
    bool loop_up = false;
    bool fetch_up = false;

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (inkwell_fetch_init(&fetcher, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (!download_serve_as(&server, &fetcher)) {
        failure = "could not stand up the fake CDN";
        goto cleanup;
    }

    /*
     * The T114 with an ESP32-S3 expectation. Everything resolves - the release manifest names
     * nrf52840, the zip URL is built from it, the `.mt.json` is range-read out of the zip and
     * parsed - and then the cross-check fires, which is exactly where it should.
     */
    memset(&probe, 0, sizeof probe);
    memset(&fetch, 0, sizeof fetch);
    if (mesh_firmware_fetch_start(
            &fetch, &fetcher, "heltec-mesh-node-t114", "2.7.26.54e0d8d",
            "https://example.invalid/download/v2.7.26.54e0d8d/firmware-2.7.26.54e0d8d.json",
            "esp32-s3", MESH_FIRMWARE_PATH_NONE, false, dir, fetch_probe_done, &probe) != 0) {
        failure = "the fetch should start";
        goto cleanup;
    }
    if (!fetch_wait_done(&loop, &fetcher, &fetch, &probe)) {
        failure = "it should have finished";
        goto cleanup;
    }
    if (fetch.state != MESH_FIRMWARE_FETCH_FAILED ||
        fetch.error != MESH_FIRMWARE_FETCH_ERROR_MISMATCH) {
        failure = "two documents describing different chips is a mismatch, not a guess";
        goto cleanup;
    }
    /* It got far enough to have done the resolution, which is what this case is really for. */
    if (strcmp(fetch.platform, "nrf52840") != 0) {
        failure = "the release manifest says the T114 is built on nrf52840";
        goto cleanup;
    }
    if (strcmp(fetch.zip_url, "https://example.invalid/download/v2.7.26.54e0d8d/"
                              "firmware-nrf52840-2.7.26.54e0d8d.zip") != 0) {
        failure = "and the zip URL is derived from the manifest's own, not from a hostname";
        goto cleanup;
    }
    if (fetch.manifest.hw_model != 69U) {
        failure = "the board manifest was read out of the zip before the check fired";
        goto cleanup;
    }

    /* A board this release did not build for: the first document answers and nothing is
       range-read at all. */
    memset(&probe, 0, sizeof probe);
    memset(&fetch, 0, sizeof fetch);
    if (mesh_firmware_fetch_start(
            &fetch, &fetcher, "no-such-board", "2.7.26.54e0d8d",
            "https://example.invalid/download/v2.7.26.54e0d8d/firmware-2.7.26.54e0d8d.json", "",
            MESH_FIRMWARE_PATH_NONE, false, dir, fetch_probe_done, &probe) != 0) {
        failure = "the second fetch should start";
        goto cleanup;
    }
    if (!fetch_wait_done(&loop, &fetcher, &fetch, &probe)) {
        failure = "it should have finished too";
        goto cleanup;
    }
    if (fetch.state != MESH_FIRMWARE_FETCH_FAILED ||
        fetch.error != MESH_FIRMWARE_FETCH_ERROR_NO_TARGET) {
        failure = "a release that built nothing for this board is its own answer";
        goto cleanup;
    }
    if (fetch.zip_url[0] != '\0') {
        failure = "and no zip was named, because there was nothing to name one for";
        goto cleanup;
    }

cleanup:
    if (fetch_up) {
        inkwell_fetch_shutdown(&fetcher);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    download_clean_dir(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A release host whose CDN refuses every ranged read, which zip_fetch counts as the network. */
static void download_serve_no_ranges(void *userdata, const struct https_fixture_request *request,
                                     struct https_fixture_conn *conn) {
    if (request->ranged) {
        https_fixture_reply(conn, 503, NULL, NULL, 0U);
        return;
    }
    download_serve(userdata, request, conn);
}

/*
 * A failed download already reads "download failed" on the toast, so the message under it says
 * what the category cannot. It used to be "the download failed", which put the same words on
 * the screen twice and nothing about a server that had stopped answering.
 */
MESH_TEST_CASE(firmware_fetch_says_why_a_download_failed, unit) {
    char dir[] = "/tmp/meshclient_fwdl_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct inkwell_fetch fetcher;
    struct mesh_firmware_fetch fetch;
    struct fetch_probe probe;
    bool loop_up = false;
    bool fetch_up = false;

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (inkwell_fetch_init(&fetcher, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (!https_fixture_start(&server, download_serve_no_ranges, NULL)) {
        failure = "could not stand up the fake CDN";
        goto cleanup;
    }
    https_fixture_attach(&server, &fetcher);

    memset(&probe, 0, sizeof probe);
    memset(&fetch, 0, sizeof fetch);
    if (mesh_firmware_fetch_start(
            &fetch, &fetcher, "heltec-mesh-node-t114", "2.7.26.54e0d8d",
            "https://example.invalid/download/v2.7.26.54e0d8d/firmware-2.7.26.54e0d8d.json", "",
            MESH_FIRMWARE_PATH_NONE, false, dir, fetch_probe_done, &probe) != 0) {
        failure = "the fetch should start";
        goto cleanup;
    }
    if (!fetch_wait_done(&loop, &fetcher, &fetch, &probe)) {
        failure = "it should have finished";
        goto cleanup;
    }
    if (fetch.state != MESH_FIRMWARE_FETCH_FAILED ||
        fetch.error != MESH_FIRMWARE_FETCH_ERROR_DOWNLOAD) {
        failure = "a CDN refusing every range is a failed download";
        goto cleanup;
    }
    if (fetch.message[0] == '\0' || strstr(fetch.message, "download failed") != NULL) {
        failure = "the message should say why, not repeat the category";
        goto cleanup;
    }

cleanup:
    if (fetch_up) {
        inkwell_fetch_shutdown(&fetcher);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    download_clean_dir(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* ---- a release that names its file ------------------------------------------------------- */

/* The first 48 bytes of firmware-heltec-v3-2.7.26.54e0d8d.bin: an ESP32-S3 app header. */
static const uint8_t k_s3_header[48] = {
    0xE9, 0x07, 0x02, 0x3F, 0xD8, 0x72, 0x37, 0x40, 0xEE, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00, 0x18, 0x3C, 0x30, 0x37, 0x07, 0x00,
    0x32, 0x54, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
#define DIRECT_IMAGE_LEN 4096U
/* The chip the served header names: its twelfth byte. The fixture's child is forked after this
   is set, so it serves what the case asked for. */
static uint8_t g_direct_chip = 0x09U;
/* Serve a whole flash - bootloader, table, app - rather than an app. */
static bool g_direct_whole = false;
#define DIRECT_WHOLE_LEN (0x10000U + DIRECT_IMAGE_LEN)

/* A release asset the way GitHub serves one: a 302 to its storage host, then the file whole. */
static void direct_serve(void *userdata, const struct https_fixture_request *request,
                         struct https_fixture_conn *conn) {
    (void)userdata;
    if (strcmp(request->host, "objects.githubusercontent.com") != 0) {
        char location[160];
        snprintf(location, sizeof location, "Location: https://objects.githubusercontent.com%s\r\n",
                 request->target);
        https_fixture_reply(conn, 302, location, NULL, 0U);
        return;
    }
    static uint8_t image[DIRECT_WHOLE_LEN];
    if (g_direct_whole) {
        /* merge_bin's layout: the bootloader at 0x0 with no app descriptor, the table at
           0x8000 naming app0 at 0x10000, the app there, 0xFF between. */
        memset(image, 0xFF, sizeof image);
        memcpy(image, k_s3_header, sizeof k_s3_header);
        memset(image + 32, 0x00, 4U);
        static const uint8_t k_app0[32] = {0xAA, 0x50, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00,
                                           0x00, 0x00, 0x10, 0x00, 'a',  'p',  'p',  '0'};
        memcpy(image + 0x8000U, k_app0, sizeof k_app0);
        memcpy(image + 0x10000U, k_s3_header, sizeof k_s3_header);
        https_fixture_reply(conn, 200, NULL, (const char *)image, DIRECT_WHOLE_LEN);
        return;
    }
    memset(image, 0x5A, DIRECT_IMAGE_LEN);
    memcpy(image, k_s3_header, sizeof k_s3_header);
    image[12] = g_direct_chip;
    https_fixture_reply(conn, 200, NULL, (const char *)image, DIRECT_IMAGE_LEN);
}

static const char *direct_run(struct inkwell_loop *loop, struct inkwell_fetch *fetcher,
                              struct mesh_firmware_fetch *fetch, uint64_t bytes, bool whole,
                              const char *dir) {
    struct fetch_probe probe;
    memset(&probe, 0, sizeof probe);
    memset(fetch, 0, sizeof *fetch);
    if (mesh_firmware_fetch_start_direct(
            fetch, fetcher,
            "https://example.invalid/download/companion-v1.17.1/Heltec_v3_companion_radio_usb-"
            "v1.17.1-d929643.bin",
            "Heltec_v3_companion_radio_usb-v1.17.1-d929643.bin", bytes, "esp32",
            MESH_FIRMWARE_PATH_USB, whole, dir, fetch_probe_done, &probe) != 0) {
        return "the fetch should start";
    }
    return fetch_wait_done(loop, fetcher, fetch, &probe) ? NULL : "it should have finished";
}

MESH_TEST_CASE(firmware_fetch_takes_a_named_file_and_reads_its_chip, unit) {
    char dir[] = "/tmp/meshclient_fwdl_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct inkwell_fetch fetcher;
    static struct mesh_firmware_fetch fetch;
    bool loop_up = false;
    bool fetch_up = false;
    char path[512];

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (inkwell_fetch_init(&fetcher, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (!https_fixture_start(&server, direct_serve, NULL)) {
        failure = "could not stand up the fake release host";
        goto cleanup;
    }
    https_fixture_attach(&server, &fetcher);

    failure = direct_run(&loop, &fetcher, &fetch, DIRECT_IMAGE_LEN, false, dir);
    if (failure != NULL) {
        goto cleanup;
    }
    if (fetch.state != MESH_FIRMWARE_FETCH_READY) {
        failure = fetch.message[0] != '\0' ? fetch.message : "the image should have landed";
        goto cleanup;
    }
    if (strcmp(fetch.manifest.architecture, "esp32-s3") != 0) {
        failure = "the family the catalog knew becomes the chip the header names";
        goto cleanup;
    }
    if (mesh_firmware_fetch_image_path(&fetch, path, sizeof path) == NULL ||
        strstr(path, "firmware.image") == NULL || mesh_firmware_fetch_progress(&fetch) != 100U) {
        failure = "and it is where the handover will look for it";
        goto cleanup;
    }

    /* The same file, described as shorter than it is: not the file that was described. */
    failure = direct_run(&loop, &fetcher, &fetch, DIRECT_IMAGE_LEN - 1U, false, dir);
    if (failure != NULL) {
        goto cleanup;
    }
    if (fetch.state != MESH_FIRMWARE_FETCH_FAILED ||
        fetch.error != MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE) {
        failure = "a file longer than its release says is refused as it arrives";
        goto cleanup;
    }
    failure = direct_run(&loop, &fetcher, &fetch, DIRECT_IMAGE_LEN + 1U, false, dir);
    if (failure == NULL && (fetch.state != MESH_FIRMWARE_FETCH_FAILED ||
                            fetch.error != MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE)) {
        failure = "and one shorter is not the file described";
    }
    if (failure != NULL) {
        goto cleanup;
    }

    /* The flasher says "esp32" for a C3 board too, and its ROM is reached the same way - so a
       chip Meshtastic's path table has no cable for is still one this can write. */
    g_direct_chip = 0x05U;
    https_fixture_stop(&server);
    if (!https_fixture_start(&server, direct_serve, NULL)) {
        failure = "could not stand the release host up again";
        goto cleanup;
    }
    https_fixture_attach(&server, &fetcher);
    failure = direct_run(&loop, &fetcher, &fetch, DIRECT_IMAGE_LEN, false, dir);
    if (failure == NULL && (fetch.state != MESH_FIRMWARE_FETCH_READY ||
                            strcmp(fetch.manifest.architecture, "esp32-c3") != 0)) {
        failure = "an ESP32-C3 app is read as one";
    }
    if (failure != NULL) {
        goto cleanup;
    }

    /* Asked for a whole flash, an app is not one - and a merged image is, named by its
       bootloader's chip. */
    g_direct_chip = 0x09U;
    failure = direct_run(&loop, &fetcher, &fetch, DIRECT_IMAGE_LEN, true, dir);
    if (failure == NULL && (fetch.state != MESH_FIRMWARE_FETCH_FAILED ||
                            fetch.error != MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE)) {
        failure = "an app image is not a whole flash";
    }
    if (failure != NULL) {
        goto cleanup;
    }
    g_direct_whole = true;
    https_fixture_stop(&server);
    if (!https_fixture_start(&server, direct_serve, NULL)) {
        failure = "could not stand the release host up again";
        goto cleanup;
    }
    https_fixture_attach(&server, &fetcher);
    failure = direct_run(&loop, &fetcher, &fetch, DIRECT_WHOLE_LEN, true, dir);
    if (failure == NULL && (fetch.state != MESH_FIRMWARE_FETCH_READY ||
                            strcmp(fetch.manifest.architecture, "esp32-s3") != 0)) {
        failure = fetch.message[0] != '\0' ? fetch.message : "a whole S3 flash is read as one";
    }

cleanup:
    g_direct_chip = 0x09U;
    g_direct_whole = false;
    if (fetch_up) {
        inkwell_fetch_shutdown(&fetcher);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    download_clean_dir(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

#endif /* INKWELL_HAVE_TLS */
