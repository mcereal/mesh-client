#define _POSIX_C_SOURCE 200809L

/*
 * The whole of phase 2, end to end, against the bytes GitHub actually serves.
 *
 * An HTTPS server on loopback (support/https_fixture.h) serves ranges out of two fixtures the
 * way the CDN serves them out of one 46 MB zip: a 302 from the release host to the CDN, a HEAD
 * whose length is the file's rather than the redirect's, the tail window at
 * 46,194,237, the local header at 2,540,989 and the member's deflated bytes at 2,541,100. Every
 * one of those offsets is where that release really keeps them.
 *
 * The member fetched is the T114's `.mt.json` rather than its `.uf2`, because at 486 bytes it
 * is a fixture and at 517,956 it is not - and it exercises exactly the same four steps. What
 * comes out the far end is parsed by the manifest reader, so the case proves the chain rather
 * than the plumbing: range read, place the data, inflate it, check the length and CRC the
 * central directory carried, and read the document that falls out.
 *
 * The orchestration above it - firmware_fetch.c, which turns a target and a release into that
 * URL and that member name - is tested here too rather than in a suite of its own, because it
 * needs the same fake CDN and a second copy of one is a second thing to keep true. Its cases
 * are the two that stop before the image, since the image download is what everything above
 * already covers: a release that built nothing for this board, and two documents disagreeing
 * about what this board is.
 */

#include "framework/mesh_test.h"

#include "inkwell/net/fetch.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/firmware_catalog.h"
#include "mesh/core/firmware_download.h"
#include "mesh/core/firmware_fetch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#define T114_MANIFEST "firmware-heltec-mesh-node-t114-2.7.26.54e0d8d.mt.json"

struct download_probe {
    unsigned calls;
    enum mesh_firmware_download_state state;
    enum mesh_firmware_download_error error;
    uint64_t size;
    char image[512];
};

static void download_record(void *userdata, const struct mesh_firmware_download *download) {
    struct download_probe *const probe = (struct download_probe *)userdata;
    probe->calls++;
    probe->state = download->state;
    probe->error = download->error;
    probe->size = mesh_firmware_download_image_size(download);
    probe->image[0] = '\0';
    (void)mesh_firmware_download_image_path(download, probe->image, sizeof probe->image);
}

/* What the fake CDN does to the bytes it serves. */
enum download_cdn {
    CDN_HONEST,
    /* One byte of the T114 manifest's payload zeroed: the member arrives and is wrong. */
    CDN_CORRUPT_MEMBER,
    /* That member's uncompressed size in the central directory rewritten to 2 GiB. */
    CDN_OVERSIZED_MEMBER,
};

/* Where that size field sits in the tail window: the member's central record starts at 50,000,
   and the uncompressed size is 24 bytes into a record. */
#define TAIL_T114_MANIFEST_SIZE_FIELD 50024

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
 * The fake CDN, in the fixture's child. `userdata` is the enum download_cdn it behaves as.
 *
 * Every zip URL answers 302 to a second host, the way a GitHub release asset does - and the 302
 * carries `content-length: 0`, so a HEAD that reported the first hop's length would measure
 * every zip as empty.
 */
static void download_serve(void *userdata, const struct https_fixture_request *request,
                           struct https_fixture_conn *conn) {
    const enum download_cdn cdn = *(const enum download_cdn *)userdata;
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
        if (cdn == CDN_OVERSIZED_MEMBER && first == TAIL_BASE &&
            got >= TAIL_T114_MANIFEST_SIZE_FIELD + 4U) {
            memcpy(body + TAIL_T114_MANIFEST_SIZE_FIELD, "\x00\x00\x00\x80", 4U);
        }
    } else if (count <= sizeof body && first >= MEMBER_BASE) {
        got =
            download_slice("zip_member_t114_mt_json_2.7.26.bin", first - MEMBER_BASE, count, body);
        if (cdn == CDN_CORRUPT_MEMBER && first - MEMBER_BASE == 111U && got > 200U) {
            body[200] = '\0';
        }
    } else {
        https_fixture_reply(conn, 416, NULL, NULL, 0U);
        return;
    }
    char range[96];
    snprintf(range, sizeof range, "Content-Range: bytes %llu-%llu/" ZIP_SIZE "\r\n",
             (unsigned long long)first, (unsigned long long)(first + got - 1U));
    https_fixture_reply(conn, 206, range, body, got);
}

/* Stands the fake CDN up as `cdn`, and points `fetch` at it. */
static bool download_serve_as(struct https_fixture *server, enum download_cdn cdn,
                              struct inkwell_fetch *fetch) {
    static enum download_cdn mode;
    mode = cdn;
    https_fixture_stop(server);
    if (!https_fixture_start(server, download_serve, &mode)) {
        return false;
    }
    https_fixture_attach(server, fetch);
    return true;
}

static bool download_wait(struct inkwell_loop *loop, struct inkwell_fetch *fetch,
                          struct mesh_firmware_download *download,
                          const struct download_probe *probe) {
    for (int turn = 0; turn < 600 && probe->calls == 0U; ++turn) {
        (void)inkwell_loop_run(loop, 10);
        inkwell_fetch_tick(fetch, 0U);
        mesh_firmware_download_tick(download, 0U);
    }
    return probe->calls > 0U;
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
 * One member out of a 46 MB zip in four range requests, verified by the inflate.
 *
 * The assertions at the end are the point of the whole phase: the file on disk is the length
 * the central directory promised, and its contents are the document the manifest reader knows
 * how to read. The CRC and the length are checked on the way past, by the inflate, which is why
 * there is no separate verify step to test.
 */
MESH_TEST_CASE(firmware_download_fetches_a_member_end_to_end, unit) {
    char dir[] = "/tmp/meshclient_fwdl_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct inkwell_fetch fetch;
    struct mesh_firmware_download download;
    bool loop_up = false;
    bool fetch_up = false;

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (inkwell_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (!download_serve_as(&server, CDN_HONEST, &fetch)) {
        failure = "could not stand up the fake CDN";
        goto cleanup;
    }

    struct download_probe probe;
    memset(&probe, 0, sizeof probe);
    memset(&download, 0, sizeof download);
    if (mesh_firmware_download_start(&download, &fetch,
                                     "https://example.invalid/firmware-nrf52840-2.7.26.zip",
                                     T114_MANIFEST, dir, download_record, &probe) != 0) {
        failure = "the download should start";
        goto cleanup;
    }
    if (!mesh_firmware_download_busy(&download)) {
        failure = "and report itself busy while it runs";
        goto cleanup;
    }
    if (!download_wait(&loop, &fetch, &download, &probe)) {
        failure = "the download should have finished";
        goto cleanup;
    }
    if (probe.state != MESH_FIRMWARE_DOWNLOAD_READY) {
        failure = "it should have finished ready";
        goto cleanup;
    }
    if (probe.error != MESH_FIRMWARE_DOWNLOAD_ERROR_NONE) {
        failure = "with no error";
        goto cleanup;
    }
    if (probe.size != 1157ULL) {
        failure = "the T114's manifest is 1,157 bytes, as the central directory said";
        goto cleanup;
    }
    if (probe.image[0] == '\0') {
        failure = "and the image should have a path";
        goto cleanup;
    }
    if (mesh_firmware_download_progress(&download) != 100U) {
        failure = "a finished download reports 100";
        goto cleanup;
    }

    /* The far end of the chain: what landed is the document, not merely 1,157 bytes. */
    {
        FILE *const image = fopen(probe.image, "rb");
        if (image == NULL) {
            failure = "the staged image should be readable";
            goto cleanup;
        }
        char text[2048];
        const size_t got = fread(text, 1U, sizeof text - 1U, image);
        fclose(image);
        text[got] = '\0';
        struct mesh_firmware_manifest manifest;
        if (!mesh_firmware_manifest_parse(text, got, &manifest)) {
            failure = "and it should parse as the board's manifest";
            goto cleanup;
        }
        if (manifest.hw_model != 69U || strcmp(manifest.target, "heltec-mesh-node-t114") != 0) {
            failure = "naming the board it was fetched for";
            goto cleanup;
        }
        const struct mesh_firmware_image *const uf2 =
            mesh_firmware_manifest_image(&manifest, MESH_FIRMWARE_PATH_USB);
        if (uf2 == NULL || uf2->bytes != 1467392ULL) {
            failure = "and pointing at the 1,467,392-byte UF2 phase 3 will write";
            goto cleanup;
        }
    }

    /* The intermediates are gone and the image is not: a retry should skip nothing it needs
       and keep nothing it does not. */
    {
        char stale[512];
        struct stat info;
        snprintf(stale, sizeof stale, "%s/firmware.member", dir);
        if (stat(stale, &info) == 0) {
            failure = "the member should have been cleaned up";
            goto cleanup;
        }
        snprintf(stale, sizeof stale, "%s/firmware.window", dir);
        if (stat(stale, &info) == 0) {
            failure = "and so should the tail window";
            goto cleanup;
        }
    }

cleanup:
    if (fetch_up) {
        inkwell_fetch_shutdown(&fetch);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    download_clean_dir(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A release that does not build for this board, a member that did not survive the trip, and
 * one whose directory claims more than any radio could hold.
 *
 * Refusals that must not be one row. "This release has no file for your radio" is an
 * answer about the release; a CRC that does not match is an answer about the network, and the
 * second is worth retrying where the first is not.
 */
MESH_TEST_CASE(firmware_download_tells_a_missing_member_from_a_broken_one, unit) {
    char dir[] = "/tmp/meshclient_fwdl_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct inkwell_fetch fetch;
    struct mesh_firmware_download download;
    bool loop_up = false;
    bool fetch_up = false;

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (inkwell_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (!download_serve_as(&server, CDN_HONEST, &fetch)) {
        failure = "could not stand up the fake CDN";
        goto cleanup;
    }

    /* A real board that this release's nrf52840 zip does not carry: the walk succeeds and
       finds nothing, which is not the same as the walk failing. */
    struct download_probe probe;
    memset(&probe, 0, sizeof probe);
    memset(&download, 0, sizeof download);
    if (mesh_firmware_download_start(&download, &fetch, "https://example.invalid/zip",
                                     "firmware-heltec-v3-2.7.26.54e0d8d.bin", dir, download_record,
                                     &probe) != 0) {
        failure = "the download should start";
        goto cleanup;
    }
    if (!download_wait(&loop, &fetch, &download, &probe)) {
        failure = "it should have finished";
        goto cleanup;
    }
    if (probe.state != MESH_FIRMWARE_DOWNLOAD_FAILED ||
        probe.error != MESH_FIRMWARE_DOWNLOAD_ERROR_NO_MEMBER) {
        failure = "an ESP32 image is not in the nrf52840 zip, and that is its own answer";
        goto cleanup;
    }

    /* Now the same fetch with one byte of the member's payload flipped. Everything up to the
       inflate, and the inflate is what catches it. */
    if (!download_serve_as(&server, CDN_CORRUPT_MEMBER, &fetch)) {
        failure = "could not stand up the corrupting CDN";
        goto cleanup;
    }
    memset(&probe, 0, sizeof probe);
    memset(&download, 0, sizeof download);
    if (mesh_firmware_download_start(&download, &fetch, "https://example.invalid/zip",
                                     T114_MANIFEST, dir, download_record, &probe) != 0) {
        failure = "the second download should start";
        goto cleanup;
    }
    if (!download_wait(&loop, &fetch, &download, &probe)) {
        failure = "it should have finished too";
        goto cleanup;
    }
    if (probe.state != MESH_FIRMWARE_DOWNLOAD_FAILED ||
        probe.error != MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE) {
        failure = "a member that did not survive the trip is refused by the inflate";
        goto cleanup;
    }
    {
        /* And nothing half-written is left behind for a retry to pick up and use. */
        char image[512];
        struct stat info;
        snprintf(image, sizeof image, "%s/firmware.image", dir);
        if (stat(image, &info) == 0) {
            failure = "a failed download leaves no image behind";
            goto cleanup;
        }
    }

    /* A directory claiming the manifest inflates to 2 GiB. The size is what gets allocated and
       inflated into on the loop, so it is refused before the member is even fetched. */
    if (!download_serve_as(&server, CDN_OVERSIZED_MEMBER, &fetch)) {
        failure = "could not stand up the oversizing CDN";
        goto cleanup;
    }
    memset(&probe, 0, sizeof probe);
    memset(&download, 0, sizeof download);
    if (mesh_firmware_download_start(&download, &fetch, "https://example.invalid/zip",
                                     T114_MANIFEST, dir, download_record, &probe) != 0) {
        failure = "the third download should start";
        goto cleanup;
    }
    if (!download_wait(&loop, &fetch, &download, &probe)) {
        failure = "it should have finished as well";
        goto cleanup;
    }
    if (probe.state != MESH_FIRMWARE_DOWNLOAD_FAILED ||
        probe.error != MESH_FIRMWARE_DOWNLOAD_ERROR_UNSUPPORTED) {
        failure = "a member bigger than any radio's flash is refused, not allocated";
        goto cleanup;
    }
    if (download.located) {
        failure = "and refused off the directory, before its header or bytes were fetched";
        goto cleanup;
    }

cleanup:
    if (fetch_up) {
        inkwell_fetch_shutdown(&fetch);
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

/* The arguments that are refused before anything is started. */
MESH_TEST_CASE(firmware_download_refuses_what_it_cannot_do, unit) {
    struct inkwell_fetch fetch;
    MESH_TEST_FAIL_IF(inkwell_fetch_init(&fetch, NULL) != 0, "a loopless fetcher should init");
    MESH_TEST_FAIL_IF(inkwell_fetch_available(&fetch), "and report itself unavailable");

    struct mesh_firmware_download download;
    memset(&download, 0, sizeof download);
    MESH_TEST_FAIL_IF(
        mesh_firmware_download_start(&download, &fetch, NULL, "a", "/tmp", NULL, NULL) != -EINVAL,
        "no URL is -EINVAL");
    MESH_TEST_FAIL_IF(
        mesh_firmware_download_start(&download, &fetch, "u", "", "/tmp", NULL, NULL) != -EINVAL,
        "and so is an empty member name");
    MESH_TEST_FAIL_IF(
        mesh_firmware_download_start(&download, &fetch, "u", "a", "/tmp", NULL, NULL) != -ENOTSUP,
        "with no fetcher there is nothing to try");
    MESH_TEST_FAIL_IF(mesh_firmware_download_busy(&download),
                      "and nothing was started, so nothing is running");
    MESH_TEST_FAIL_IF(mesh_firmware_download_progress(&download) != 0U,
                      "nor is there anything to report progress on");
    MESH_TEST_FAIL_IF(mesh_firmware_download_image_path(&download, NULL, 0U) != NULL,
                      "and no image to point at");
    inkwell_fetch_shutdown(&fetch);
    record_success(test_name);
}

#ifdef INKWELL_HAVE_TLS

/*
 * The orchestration, over the same fake CDN: a target and a release become a zip URL and a
 * member name, and the two documents that decide them are read in order.
 *
 * Both cases here stop before the image, which is deliberate - the image download is what the
 * cases above cover, and what these are about is the *resolution*. Each is a refusal that has
 * to be its own row: "this release built nothing for your board" is an answer about upstream,
 * and "these two documents describe different chips" is a reason to stop rather than to pick.
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
    if (!download_serve_as(&server, CDN_HONEST, &fetcher)) {
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
            "esp32-s3", dir, fetch_probe_done, &probe) != 0) {
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
            dir, fetch_probe_done, &probe) != 0) {
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

#endif /* INKWELL_HAVE_TLS */
