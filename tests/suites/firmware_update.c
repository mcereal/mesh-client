#define _POSIX_C_SOURCE 200809L

/*
 * The press that installs the radio's firmware, as far as a suite can follow it.
 *
 * What this module does is *compose*: the download, the USB handover and the BLE one are three
 * shipped modules with suites of their own, and this is the thing that runs them in order,
 * folds their three state enums onto one ladder and answers the two questions that decide who
 * may touch the bus. So the cases here are about the composition rather than about the parts -
 * the refusals that happen before anything starts, how a sub-module's failure is folded into the
 * one a row shows, and the antenna/radio split that the BLE path's reconnect depends on.
 *
 * The fake CDN is the firmware_fetch suite's, one file over: an HTTPS server on loopback
 * (support/https_fixture.h) that serves ranges out of the committed 2.7.26 fixtures the way the
 * real CDN serves them out of a 46 MB zip. What it deliberately cannot serve is a whole `.uf2` - at
 * half a megabyte that is not a fixture - so every case here that gets as far as the image gets a
 * refusal out of it, which is exactly what makes the folding worth pinning: "the bytes did not
 * arrive" and "the bytes are for another board" must not be the same row.
 */

#include "inkwell/base/text.h"

#include "framework/mesh_test.h"

#include "support/ble_ota_fixture.h"
#include "support/data_fixture.h"
#include "support/uf2_fixture.h"

#include "inkwell/ble/central.h"
#include "inkwell/codec/esp_image.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/firmware_update.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MESH_TEST_DATA_DIR
#define MESH_TEST_DATA_DIR "tests/data"
#endif

/* The 2.7.26 nrf52840 zip, and where the two committed fixtures sit inside it. */
#define ZIP_SIZE "46259773"
#define TAIL_BASE 46194237
#define MEMBER_BASE 2540989

#ifdef INKWELL_HAVE_TLS

#include "support/https_fixture.h"

/* ---- the boards a case installs onto --------------------------------------------------------- */

static struct mesh_firmware_board update_t114_board(void) {
    struct mesh_firmware_board board;
    memset(&board, 0, sizeof board);
    board.hw_model = 69U;
    snprintf(board.target, sizeof board.target, "%s", "heltec-mesh-node-t114");
    snprintf(board.name, sizeof board.name, "%s", "Heltec Mesh Node T114");
    snprintf(board.architecture, sizeof board.architecture, "%s", "nrf52840");
    board.actively_supported = true;
    board.requires_dfu = true;
    board.path = MESH_FIRMWARE_PATH_USB;
    return board;
}

static struct mesh_firmware_release update_release(void) {
    struct mesh_firmware_release release;
    memset(&release, 0, sizeof release);
    snprintf(release.version, sizeof release.version, "%s", "2.7.26.54e0d8d");
    snprintf(release.manifest_url, sizeof release.manifest_url, "%s",
             "https://example.invalid/firmware-2.7.26.54e0d8d.json");
    return release;
}

/* ---- the harness ----------------------------------------------------------------------------- */

struct update_probe {
    unsigned calls;
    enum mesh_firmware_update_state state;
    enum mesh_firmware_update_error error;
    unsigned armed_usb;
    unsigned armed_ble;
    unsigned released;
    bool radio_ready;
    int arm_result;
};

static void update_probe_done(void *userdata, const struct mesh_firmware_update *update) {
    struct update_probe *const probe = (struct update_probe *)userdata;
    probe->calls++;
    probe->state = update->state;
    probe->error = update->error;
}

static int update_probe_arm_usb(void *userdata) {
    struct update_probe *const probe = (struct update_probe *)userdata;
    probe->armed_usb++;
    return probe->arm_result;
}

static int update_probe_arm_ble(void *userdata, const uint8_t sha256[32]) {
    struct update_probe *const probe = (struct update_probe *)userdata;
    (void)sha256;
    probe->armed_ble++;
    return probe->arm_result;
}

static bool update_probe_radio_ready(void *userdata) {
    return ((struct update_probe *)userdata)->radio_ready;
}

static void update_probe_release(void *userdata) { ((struct update_probe *)userdata)->released++; }

static struct mesh_firmware_update_hooks update_hooks(struct update_probe *probe) {
    struct mesh_firmware_update_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.arm_usb = update_probe_arm_usb;
    hooks.arm_ble = update_probe_arm_ble;
    hooks.radio_ready = update_probe_radio_ready;
    hooks.release_link = update_probe_release;
    hooks.userdata = probe;
    return hooks;
}

/* ---- a zip of our own, for the one case that goes all the way through --------------------- */

/*
 * The fake below serves two windows of a real release zip and deliberately cannot serve an
 * image, so every case built on it ends in a refusal - and a ladder nobody has ever climbed to
 * the top of is a ladder whose top rung can be broken for a release without anything noticing.
 * It was: a fetch that finished *inside* mesh_firmware_update_tick() had its own completion
 * written back over by the same tick, and "resolving" was where the HUD then sat forever.
 *
 * So this builds a small zip instead. Both members are real documents - the release's committed
 * board manifest with the one number that says how long the image is rewritten, and the
 * committed image's own blocks made into a whole file - and both are **stored** rather than
 * deflated, which is a shape inkwell's codec/zip.h supports and the only one a suite can write
 * without a compressor. The deflated member with the bytes the CDN really serves is
 * inkwell's net_zip_fetch suite's job.
 */

struct update_member {
    const char *name;
    const uint8_t *bytes;
    uint32_t len;
};

static uint32_t update_crc32(const uint8_t *bytes, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < len; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static void update_put_u16(FILE *file, uint16_t value) {
    const uint8_t bytes[2] = {(uint8_t)(value & 0xFFU), (uint8_t)((value >> 8) & 0xFFU)};
    (void)fwrite(bytes, 1U, sizeof bytes, file);
}

static void update_put_u32(FILE *file, uint32_t value) {
    const uint8_t bytes[4] = {(uint8_t)(value & 0xFFU), (uint8_t)((value >> 8) & 0xFFU),
                              (uint8_t)((value >> 16) & 0xFFU), (uint8_t)((value >> 24) & 0xFFU)};
    (void)fwrite(bytes, 1U, sizeof bytes, file);
}

#define UPDATE_MEMBER_MAX 4U

static bool update_write_zip(const char *path, const struct update_member *members, size_t count) {
    if (count == 0U || count > UPDATE_MEMBER_MAX) {
        return false;
    }
    FILE *const file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    uint32_t offset[UPDATE_MEMBER_MAX];
    uint32_t crc[UPDATE_MEMBER_MAX];
    for (size_t i = 0U; i < count; ++i) {
        const long at = ftell(file);
        if (at < 0) {
            fclose(file);
            return false;
        }
        offset[i] = (uint32_t)at;
        crc[i] = update_crc32(members[i].bytes, members[i].len);
        const uint16_t name_len = (uint16_t)strlen(members[i].name);
        update_put_u32(file, 0x04034B50U);
        update_put_u16(file, 20U); /* the version that reads a stored member */
        update_put_u16(file, 0U);  /* no flags, so no data descriptor after the payload */
        update_put_u16(file, 0U);  /* stored */
        update_put_u16(file, 0U);  /* time */
        update_put_u16(file, 0U);  /* date */
        update_put_u32(file, crc[i]);
        update_put_u32(file, members[i].len);
        update_put_u32(file, members[i].len);
        update_put_u16(file, name_len);
        update_put_u16(file, 0U); /* no extra field */
        (void)fwrite(members[i].name, 1U, name_len, file);
        (void)fwrite(members[i].bytes, 1U, members[i].len, file);
    }

    const long central_at = ftell(file);
    if (central_at < 0) {
        fclose(file);
        return false;
    }
    for (size_t i = 0U; i < count; ++i) {
        const uint16_t name_len = (uint16_t)strlen(members[i].name);
        update_put_u32(file, 0x02014B50U);
        update_put_u16(file, 20U); /* made by */
        update_put_u16(file, 20U); /* needed */
        update_put_u16(file, 0U);
        update_put_u16(file, 0U); /* stored */
        update_put_u16(file, 0U);
        update_put_u16(file, 0U);
        update_put_u32(file, crc[i]);
        update_put_u32(file, members[i].len);
        update_put_u32(file, members[i].len);
        update_put_u16(file, name_len);
        update_put_u16(file, 0U); /* extra */
        update_put_u16(file, 0U); /* comment */
        update_put_u16(file, 0U); /* disk */
        update_put_u16(file, 0U); /* internal attributes */
        update_put_u32(file, 0U); /* external attributes */
        update_put_u32(file, offset[i]);
        (void)fwrite(members[i].name, 1U, name_len, file);
    }
    const long end_at = ftell(file);
    if (end_at < 0) {
        fclose(file);
        return false;
    }
    update_put_u32(file, 0x06054B50U);
    update_put_u16(file, 0U);
    update_put_u16(file, 0U);
    update_put_u16(file, (uint16_t)count);
    update_put_u16(file, (uint16_t)count);
    update_put_u32(file, (uint32_t)(end_at - central_at));
    update_put_u32(file, (uint32_t)central_at);
    update_put_u16(file, 0U); /* no comment, which is what tells the record from a coincidence */
    const bool ok = ferror(file) == 0;
    return fclose(file) == 0 && ok;
}

/*
 * The committed board manifest, saying how long *this* zip's image is.
 *
 * The same rewrite tests/support/uf2_fixture.c makes to `numBlocks`, for the same reason: the
 * document is the one upstream published apart from the single field a shorter image makes
 * untrue, so what the parser is read against is still real bytes.
 */
static char *update_manifest_for(const char *document, const char *k_bytes, size_t image_len,
                                 size_t *out_len) {
    /* `k_bytes` is the image's length in the real release, and distinct from the other files' -
       a second occurrence would mean this is rewriting something else and is refused below. */
    const size_t key_len = strlen(k_bytes);
    size_t len = 0U;
    char *const text = mesh_test_data_read(document, &len);
    if (text == NULL) {
        return NULL;
    }
    char *const at = strstr(text, k_bytes);
    if (at == NULL || strstr(at + 1, k_bytes) != NULL) {
        free(text);
        return NULL;
    }
    char replacement[24];
    const int written = snprintf(replacement, sizeof replacement, "%zu", image_len);
    if (written <= 0) {
        free(text);
        return NULL;
    }
    const size_t head = (size_t)(at - text);
    const size_t tail = len - head - key_len;
    char *const out = malloc(head + (size_t)written + tail + 1U);
    if (out == NULL) {
        free(text);
        return NULL;
    }
    memcpy(out, text, head);
    memcpy(out + head, replacement, (size_t)written);
    memcpy(out + head + (size_t)written, at + key_len, tail);
    out[head + (size_t)written + tail] = '\0';
    free(text);
    *out_len = head + (size_t)written + tail;
    return out;
}

/* Stages that zip in `dir` and returns whether both members went in. */
static bool update_stage_zip(const char *dir) {
    size_t image_len = 0U;
    uint8_t *const image = mesh_test_uf2_whole(2U, &image_len);
    if (image == NULL) {
        return false;
    }
    size_t manifest_len = 0U;
    char *const manifest =
        update_manifest_for("t114_2.7.26.mt.json", "1467392", image_len, &manifest_len);
    if (manifest == NULL) {
        free(image);
        return false;
    }
    /* Under the platform directory, which is where the 2.8.0 zips keep their members and the
       shape that makes the reader's basename match load-bearing. */
    const struct update_member members[2] = {
        {"nrf52840/firmware-heltec-mesh-node-t114-2.7.26.54e0d8d.mt.json",
         (const uint8_t *)manifest, (uint32_t)manifest_len},
        {"nrf52840/firmware-heltec-mesh-node-t114-2.7.26.54e0d8d.uf2", image, (uint32_t)image_len},
    };
    char path[512];
    snprintf(path, sizeof path, "%s/firmware.zip", dir);
    const bool written = update_write_zip(path, members, 2U);
    free(manifest);
    free(image);
    return written;
}

/* The same zip for the BLE path: a Heltec V3's manifest and an ESP32-S3 application as app0. */
static bool update_stage_esp_zip(const char *dir) {
    const size_t image_len = 4096U;
    uint8_t *const image = mesh_test_esp_image(image_len, INKWELL_ESP_CHIP_ESP32_S3);
    if (image == NULL) {
        return false;
    }
    size_t manifest_len = 0U;
    char *const manifest =
        update_manifest_for("heltec_v3_2.7.26.mt.json", "2109248", image_len, &manifest_len);
    if (manifest == NULL) {
        free(image);
        return false;
    }
    const struct update_member members[2] = {
        {"esp32s3/firmware-heltec-v3-2.7.26.54e0d8d.mt.json", (const uint8_t *)manifest,
         (uint32_t)manifest_len},
        {"esp32s3/firmware-heltec-v3-2.7.26.54e0d8d.bin", image, (uint32_t)image_len},
    };
    char path[512];
    snprintf(path, sizeof path, "%s/firmware.zip", dir);
    const bool written = update_write_zip(path, members, 2U);
    free(manifest);
    free(image);
    return written;
}

/* Reads `count` bytes at `offset` of `path` into `out`. Returns how many it could. */
static size_t update_slice(const char *path, uint64_t offset, uint64_t count, char *out) {
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

struct update_cdn {
    /* Serve the zip this file writes, rather than the two windows of the real one. */
    bool whole;
    char dir[64];
};

/*
 * The fake CDN, in the fixture's child. With `whole` it serves the zip update_stage_zip() wrote;
 * without, the two committed windows of the real one - and the one thing it does differently
 * from the download suite's: a range that is not in either window is refused rather than served
 * as something plausible. A download that "succeeded" with the wrong bytes is the failure mode
 * this whole feature exists to avoid, so the fake must not be able to fake it.
 */
static void update_serve(void *userdata, const struct https_fixture_request *request,
                         struct https_fixture_conn *conn) {
    const struct update_cdn *const cdn = (const struct update_cdn *)userdata;
    char zip[512];
    snprintf(zip, sizeof zip, "%s/firmware.zip", cdn->dir);
    const size_t target_len = strlen(request->target);
    if (target_len > 5U && strcmp(request->target + target_len - 5U, ".json") == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/firmware_release_2.7.26.json", MESH_TEST_DATA_DIR);
        https_fixture_reply_file(conn, request, path);
        return;
    }
    if (cdn->whole) {
        https_fixture_reply_file(conn, request, zip);
        return;
    }
    if (strcmp(request->method, "HEAD") == 0) {
        https_fixture_printf(conn, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n"
                                   "Content-Length: " ZIP_SIZE "\r\n\r\n");
        return;
    }
    static char body[1024U * 1024U];
    const uint64_t first = request->first;
    const uint64_t count = request->last - first + 1U;
    size_t got = 0U;
    char path[512];
    if (request->ranged && count <= sizeof body && first >= TAIL_BASE) {
        snprintf(path, sizeof path, "%s/zip_tail_nrf52840_2.7.26.bin", MESH_TEST_DATA_DIR);
        got = update_slice(path, first - TAIL_BASE, count, body);
    } else if (request->ranged && count <= sizeof body && first >= MEMBER_BASE) {
        snprintf(path, sizeof path, "%s/zip_member_t114_mt_json_2.7.26.bin", MESH_TEST_DATA_DIR);
        got = update_slice(path, first - MEMBER_BASE, count, body);
    }
    if (got == 0U) {
        https_fixture_reply(conn, 416, NULL, NULL, 0U);
        return;
    }
    char range[96];
    snprintf(range, sizeof range, "Content-Range: bytes %llu-%llu/" ZIP_SIZE "\r\n",
             (unsigned long long)first, (unsigned long long)(first + got - 1U));
    https_fixture_reply(conn, 206, range, body, got);
}

struct update_harness {
    char dir[64];
    struct update_cdn cdn;
    struct https_fixture server;
    struct inkwell_loop loop;
    struct mesh_firmware_update update;
    bool loop_up;
    bool update_up;
};

/*
 * Up with one fake CDN or the other: `whole` picks the zip this file writes, which is the one
 * a case can follow to the end, and the default is the window-serving fake that stops at the
 * image on purpose.
 */
static bool update_harness_start(struct update_harness *harness, bool whole) {
    memset(harness, 0, sizeof *harness);
    snprintf(harness->dir, sizeof harness->dir, "%s", "/tmp/meshclient_fwup_XXXXXX");
    if (mkdtemp(harness->dir) == NULL) {
        return false;
    }
    if (whole && !update_stage_zip(harness->dir)) {
        return false;
    }
    harness->cdn.whole = whole;
    snprintf(harness->cdn.dir, sizeof harness->cdn.dir, "%s", harness->dir);
    if (!https_fixture_start(&harness->server, update_serve, &harness->cdn)) {
        return false;
    }
    /* The image is staged in the scratch directory rather than /tmp, so a case cleans up after
       itself and two running side by side cannot collide. */
    setenv("MESHCLIENT_FIRMWARE_STAGING", harness->dir, 1);

    if (inkwell_loop_init(&harness->loop) != 0) {
        return false;
    }
    harness->loop_up = true;
    if (mesh_firmware_update_init(&harness->update, &harness->loop) != 0) {
        return false;
    }
    harness->update_up = true;
    https_fixture_attach(&harness->server, &harness->update.fetch);
    return mesh_firmware_update_available(&harness->update);
}

static bool update_harness_up(struct update_harness *harness) {
    return update_harness_start(harness, false);
}

static bool update_harness_up_whole(struct update_harness *harness) {
    return update_harness_start(harness, true);
}

static void update_harness_down(struct update_harness *harness) {
    if (harness->update_up) {
        mesh_firmware_update_shutdown(&harness->update);
    }
    if (harness->loop_up) {
        inkwell_loop_shutdown(&harness->loop);
    }
    unsetenv("MESHCLIENT_FIRMWARE_STAGING");
    https_fixture_stop(&harness->server);
    if (harness->dir[0] == '\0') {
        return;
    }
    static const char *const k_files[] = {"firmware.window", "firmware.central", "firmware.header",
                                          "firmware.member", "firmware.image",   "firmware.zip"};
    char path[512];
    for (size_t i = 0; i < sizeof k_files / sizeof k_files[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", harness->dir, k_files[i]);
        (void)unlink(path);
    }
    (void)rmdir(harness->dir);
}

static bool update_settle(struct update_harness *harness, const struct update_probe *probe) {
    for (int turn = 0; turn < 900 && probe->calls == 0U; ++turn) {
        (void)inkwell_loop_run(&harness->loop, 10);
        mesh_firmware_update_tick(&harness->update, (uint64_t)turn * 100U);
    }
    return probe->calls > 0U;
}

/* The same turns, for a case that is waiting for a rung rather than for the end: a job that
   reports first has stopped somewhere it was not supposed to and the loop gives up with it. */
static bool update_settle_at(struct update_harness *harness, const struct update_probe *probe,
                             enum mesh_firmware_update_state state) {
    for (int turn = 0; turn < 900 && probe->calls == 0U; ++turn) {
        (void)inkwell_loop_run(&harness->loop, 10);
        mesh_firmware_update_tick(&harness->update, (uint64_t)turn * 100U);
        if (harness->update.state == state) {
            return true;
        }
    }
    return false;
}

/* ---- the cases ------------------------------------------------------------------------------- */

#endif /* INKWELL_HAVE_TLS */

/*
 * Every state and every error has a word, and the two ladders are total.
 *
 * Cheap, and it is the check that stops a state added to the enum from drawing "unknown" in the
 * row that is supposed to be explaining what the client is doing to somebody's radio.
 */
MESH_TEST_CASE(firmware_update_names_every_state, unit) {
    for (int i = 0; i < (int)MESH_FIRMWARE_UPDATE_STATE_COUNT; ++i) {
        const char *const name =
            mesh_firmware_update_state_name((enum mesh_firmware_update_state)i);
        MESH_TEST_FAIL_IF(name == NULL || name[0] == '\0', "every state should have a word");
    }
    for (int i = 1; i < (int)MESH_FIRMWARE_UPDATE_ERROR_COUNT; ++i) {
        const char *const name =
            mesh_firmware_update_error_name((enum mesh_firmware_update_error)i);
        MESH_TEST_FAIL_IF(name == NULL || name[0] == '\0', "every failure should have a word");
    }
    MESH_TEST_FAIL_IF(mesh_firmware_update_error_name(MESH_FIRMWARE_UPDATE_ERROR_NONE)[0] != '\0',
                      "and no failure should have none");
    /* IDLE, DONE and FAILED are the three the section is allowed to sit in with the press still
       offered; everything between them takes the rows over. */
    MESH_TEST_FAIL_IF(mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_IDLE) ||
                          mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_DONE) ||
                          mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_FAILED),
                      "a settled job is not work in flight");
    MESH_TEST_FAIL_IF(!mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_READY) ||
                          !mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_WRITING),
                      "and everything between is");
    record_success(test_name);
}

#ifdef INKWELL_HAVE_TLS

/*
 * The refusals that happen before a byte moves, and the promise that comes with them: nothing
 * was started, so no completion will ever arrive - but `state` and `error` still say why,
 * because a refusal is a row.
 */
MESH_TEST_CASE(firmware_update_refuses_before_it_starts, unit) {
    struct update_probe probe;
    memset(&probe, 0, sizeof probe);
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);

    /* No loop, so no fetcher: the press cannot run whatever it is pointed at. */
    struct mesh_firmware_update bare;
    MESH_TEST_FAIL_IF(mesh_firmware_update_init(&bare, NULL) != 0, "init should succeed");
    const struct mesh_firmware_board board = update_t114_board();
    const struct mesh_firmware_release release = update_release();
    MESH_TEST_FAIL_IF(mesh_firmware_update_start(&bare, &board, &release, "", &hooks,
                                                 update_probe_done, &probe) != -ENOTSUP,
                      "with no fetcher the press should refuse");
    MESH_TEST_FAIL_IF(bare.state != MESH_FIRMWARE_UPDATE_FAILED ||
                          bare.error != MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE,
                      "and should still say why");
    MESH_TEST_FAIL_IF(probe.calls != 0U, "a refusal reports through its return, not a callback");
    mesh_firmware_update_shutdown(&bare);

    struct update_harness harness;
    const char *failure = NULL;
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    /*
     * A board with no path at all - an ESP32-C3, a portduino. It is a row in Settings already,
     * so reaching it here means a press was offered that should not have been; it is still
     * checked, because this is the module that would otherwise be trusting its caller about
     * which board this is.
     */
    struct mesh_firmware_board pathless = board;
    pathless.path = MESH_FIRMWARE_PATH_NONE;
    if (mesh_firmware_update_start(&harness.update, &pathless, &release, "", &hooks,
                                   update_probe_done, &probe) != -ENOTSUP) {
        failure = "a board with no path should refuse";
        goto cleanup;
    }
    /* A release that appeared in the index before its assets did, which really happens. */
    struct mesh_firmware_release assetless = release;
    assetless.manifest_url[0] = '\0';
    if (mesh_firmware_update_start(&harness.update, &board, &assetless, "", &hooks,
                                   update_probe_done, &probe) != -ENOTSUP) {
        failure = "a release with no manifest should refuse";
        goto cleanup;
    }
    if (probe.calls != 0U) {
        failure = "none of those should have reported through the callback";
        goto cleanup;
    }
    /* And none of them left the module looking busy, which is what would block the next press. */
    if (mesh_firmware_update_busy(&harness.update) ||
        mesh_firmware_update_holds_the_antenna(&harness.update) ||
        mesh_firmware_update_holds_the_radio(&harness.update)) {
        failure = "a refusal should hold nothing";
        goto cleanup;
    }

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The antenna and the radio are two questions, and the difference is the whole of what makes the
 * BLE path work.
 *
 * While the image comes down the *antenna* is held - Wi-Fi and Bluetooth are one part on this
 * hardware - and the radio is not, because nothing has been asked of it. That asymmetry is what
 * lets auto-connect bring a BLE radio back the moment the download lands, which is the only way
 * the `ota_request` ever reaches it. A single "busy" flag here would have left the link down
 * through a step whose entire purpose is waiting for it to come up.
 */
MESH_TEST_CASE(firmware_update_holds_the_antenna_not_the_radio, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    const struct mesh_firmware_board board = update_t114_board();
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (harness.update.state != MESH_FIRMWARE_UPDATE_RESOLVING) {
        failure = "and should open by working out which file";
        goto cleanup;
    }
    if (!mesh_firmware_update_holds_the_antenna(&harness.update)) {
        failure = "a download holds the antenna";
        goto cleanup;
    }
    if (mesh_firmware_update_holds_the_radio(&harness.update)) {
        failure = "and holds the radio only once something has been asked of it";
        goto cleanup;
    }
    if (mesh_firmware_update_radio_in_loader(&harness.update)) {
        failure = "and nothing on the USB path ever leaves a radio in a loader";
        goto cleanup;
    }
    /* Nothing has been asked of the radio yet either way. */
    if (probe.armed_usb != 0U || probe.armed_ble != 0U || probe.released != 0U) {
        failure = "a download arms nothing and releases nothing";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);
    if (mesh_firmware_update_holds_the_antenna(&harness.update) ||
        mesh_firmware_update_busy(&harness.update)) {
        failure = "and a cancelled job gives the antenna back";
        goto cleanup;
    }

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A download that fails is a download that failed, and the radio is untouched.
 *
 * The fake CDN above serves two windows of one release's zip and refuses everything else, so a
 * board it holds no member for gets as far as reading documents and no further. What is being
 * pinned is the fold: `firmware_fetch`'s eight errors become the four a row asks about, and
 * "the bytes did not arrive" must arrive as the one that means the radio never heard from us.
 */
MESH_TEST_CASE(firmware_update_reports_a_download_that_failed, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    /* A target the release built nothing for: a real answer about the release rather than a
       network failure, and the row that says so is not the row that says the wifi is down. */
    struct mesh_firmware_board unbuilt = update_t114_board();
    snprintf(unbuilt.target, sizeof unbuilt.target, "%s", "not-a-real-board");
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &unbuilt, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (!update_settle(&harness, &probe)) {
        failure = "the job should finish";
        goto cleanup;
    }
    if (probe.calls != 1U) {
        failure = "and should report exactly once";
        goto cleanup;
    }
    if (probe.state != MESH_FIRMWARE_UPDATE_FAILED ||
        probe.error != MESH_FIRMWARE_UPDATE_ERROR_DOWNLOAD) {
        failure = "a release with no image for this board is a download that failed";
        goto cleanup;
    }
    /* The radio was never asked anything, and nothing is holding a bus. */
    if (probe.armed_usb != 0U || probe.armed_ble != 0U) {
        failure = "and the radio should never have been asked";
        goto cleanup;
    }
    if (mesh_firmware_update_holds_the_antenna(&harness.update) ||
        mesh_firmware_update_holds_the_radio(&harness.update) ||
        mesh_firmware_update_busy(&harness.update)) {
        failure = "a failure lifts every hold by failing";
        goto cleanup;
    }
    /* The failure is readable rather than only categorised: the fetch's own line survives. */
    if (harness.update.detail[0] == '\0') {
        failure = "and should carry the download's own words";
        goto cleanup;
    }
    /* Pressing again is allowed, which is what "a failure lifts it by failing" is for. */
    const struct mesh_firmware_board board = update_t114_board();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "and a second press should be allowed after a failure";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A job already running refuses the next press rather than taking the child out from under the
   one in flight - the same rule the check next door follows for the same reason. */
MESH_TEST_CASE(firmware_update_refuses_a_second_press, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    const struct mesh_firmware_board board = update_t114_board();
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != -EBUSY) {
        failure = "a second press should be refused while the first runs";
        goto cleanup;
    }
    if (probe.calls != 0U) {
        failure = "and should not have reported anything";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);
    if (probe.calls != 0U) {
        failure = "a cancel reports nothing either";
        goto cleanup;
    }

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The recovery press: what a job knows after it has left a radio in its loader.
 *
 * A radio in the ESP32 loader answers no handshake, so by the time this question is asked the
 * *check's* answer has been dropped as being about a radio that is not there - which means the
 * job's own memory of the board, the release and the address is the only thing left to start
 * from. Without it the banner points at a row that is not on the screen, which is a banner that
 * cannot resolve.
 */
MESH_TEST_CASE(firmware_update_knows_when_it_can_go_back, unit) {
    struct mesh_firmware_update update;
    MESH_TEST_FAIL_IF(mesh_firmware_update_init(&update, NULL) != 0, "init should succeed");
    MESH_TEST_FAIL_IF(mesh_firmware_update_can_resume(&update),
                      "a job that never ran has nowhere to go back to");
    MESH_TEST_FAIL_IF(mesh_firmware_update_can_resume(NULL), "and neither has no job at all");

    /* A transfer that broke with the radio already in the loader, which is the state the banner
       reads and the one this press exists for. */
    update.board = update_t114_board();
    update.release = update_release();
    inkwell_str_copy(update.where, sizeof update.where, "9C:13:9E:9D:0A:D9");
    update.ble.state = MESH_FIRMWARE_OTA_FAILED;
    update.ble.error = MESH_FIRMWARE_OTA_ERROR_TRANSFER;
    MESH_TEST_FAIL_IF(!mesh_firmware_update_radio_in_loader(&update),
                      "a broken transfer leaves the radio in its loader");
    MESH_TEST_FAIL_IF(!mesh_firmware_update_can_resume(&update),
                      "and the job still holds the board and the release to finish with");

    /* A failure that left the radio *running* is not one to go back to - there is nothing
       stranded, and the ordinary press is the way to try again. */
    update.ble.error = MESH_FIRMWARE_OTA_ERROR_REFUSED;
    MESH_TEST_FAIL_IF(mesh_firmware_update_can_resume(&update),
                      "a radio that refused is still on the mesh");

    /* A release whose manifest went missing cannot be re-fetched, so it is not resumable
       however stranded the radio is: the recovery press downloads the image again. */
    update.ble.error = MESH_FIRMWARE_OTA_ERROR_TRANSFER;
    update.release.manifest_url[0] = '\0';
    MESH_TEST_FAIL_IF(mesh_firmware_update_can_resume(&update),
                      "and there has to be something to fetch");
    mesh_firmware_update_shutdown(&update);
    record_success(test_name);
}

/*
 * The image lands and the job goes on to the radio, which is the rung every other case here
 * stops short of.
 *
 * The whole chain runs: the release document names the platform, the zip's directory is read
 * out of its tail, the board manifest is inflated and says which file is the image, the image
 * is fetched and validated as a UF2 for this family, and only then is the radio asked for
 * anything. What is pinned at the end is the handover: `arm_usb` called exactly once, the
 * ladder on ARMING, and the antenna given back - because ARMING is where a BLE job needs the
 * link that the download was holding.
 *
 * The regression underneath it is one line of mesh_firmware_update_tick(). The fetch finishes
 * inside that tick - the inflate runs there - and its completion moves the ladder to
 * READY from under the case that is still running, which then described the fetch again and
 * wrote "resolving" back over it. Nothing ever left: READY is the only state that starts a
 * handover and it only ever gets a tick of its own. On the device that was a progress row that
 * said `resolving` for as long as you cared to watch it, with the image already staged.
 */
MESH_TEST_CASE(firmware_update_carries_the_image_into_the_handover, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    /* The radio never went away on this bus, so it is ready the moment the image is. */
    probe.radio_ready = true;
    if (!update_harness_up_whole(&harness)) {
        failure = "the harness should come up with a zip to serve";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    const struct mesh_firmware_board board = update_t114_board();
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (!update_settle_at(&harness, &probe, MESH_FIRMWARE_UPDATE_ARMING)) {
        /* Either it reported early - a real failure, and `detail` says which - or it is still
           going round on a rung it should have left, which is the bug this case exists for. */
        failure = probe.calls > 0U ? "the download should not have failed"
                                   : "a staged image should reach the radio, not sit on a rung";
        goto cleanup;
    }
    if (probe.armed_usb != 1U) {
        failure = "the handover asks the radio for DFU exactly once";
        goto cleanup;
    }
    if (probe.armed_ble != 0U) {
        failure = "and asks over the bus the board is on";
        goto cleanup;
    }
    if (probe.calls != 0U) {
        failure = "an install that is under way has not reported yet";
        goto cleanup;
    }
    /* The download is over, so the antenna is free - and the radio is held now instead, which
       is the swap the two questions exist to express. */
    if (mesh_firmware_update_holds_the_antenna(&harness.update)) {
        failure = "a staged image gives the antenna back";
        goto cleanup;
    }
    if (!mesh_firmware_update_holds_the_radio(&harness.update)) {
        failure = "and an armed radio is held until the write finishes";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The BLE handover leaves the radio alone until Bluetooth has an adapter to hand.
 *
 * CoreBluetooth and Windows find theirs a turn or more after the open returns. Asked at once,
 * there was none, and the install failed - which is harmless only because it failed *before*
 * arming. A radio armed first is in its loader, off the mesh, with nothing coming to finish it.
 */
MESH_TEST_CASE(firmware_update_arms_a_ble_radio_only_once_bluetooth_is_up, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    probe.radio_ready = true;
    struct inkwell_ble_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.check_ready_result = -EAGAIN;
    inkwell_ble_mock_enable(&mock);
    if (!update_harness_up_whole(&harness) || !update_stage_esp_zip(harness.dir)) {
        failure = "the harness should come up with an ESP32 zip to serve";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    struct mesh_firmware_board board;
    memset(&board, 0, sizeof board);
    board.hw_model = 43U;
    snprintf(board.target, sizeof board.target, "%s", "heltec-v3");
    snprintf(board.name, sizeof board.name, "%s", "Heltec V3");
    snprintf(board.architecture, sizeof board.architecture, "%s", "esp32-s3");
    board.actively_supported = true;
    board.path = MESH_FIRMWARE_PATH_BLE;
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "F8:5B:1B:A5:99:C9", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (!update_settle_at(&harness, &probe, MESH_FIRMWARE_UPDATE_READY)) {
        failure = probe.calls > 0U ? "the download should not have failed"
                                   : "a staged image should reach the handover";
        goto cleanup;
    }
    for (int turn = 0; turn < 20; ++turn) {
        mesh_firmware_update_tick(&harness.update, 1000U + (uint64_t)turn * 100U);
    }
    if (probe.calls != 0U || harness.update.state != MESH_FIRMWARE_UPDATE_READY) {
        failure = "a stack still starting is waited for, not failed";
        goto cleanup;
    }
    /* bluetoothd restarting after the download: BlueZ says -ENODEV until it has an owner on the
       bus again, and the transport's own bring-up waits that out too. */
    mock.check_ready_result = -ENODEV;
    inkwell_ble_mock_enable(&mock);
    for (int turn = 0; turn < 10; ++turn) {
        mesh_firmware_update_tick(&harness.update, 2000U + (uint64_t)turn * 100U);
    }
    if (probe.calls != 0U || harness.update.state != MESH_FIRMWARE_UPDATE_READY) {
        failure = "a stack that is briefly gone is waited for until READY's deadline";
        goto cleanup;
    }
    if (probe.armed_ble != 0U) {
        failure = "and the radio is not asked into its loader while it starts";
        goto cleanup;
    }
    mock.check_ready_result = 0;
    inkwell_ble_mock_enable(&mock);
    mesh_firmware_update_tick(&harness.update, 3000U);
    if (probe.armed_ble != 1U || probe.calls != 0U) {
        failure = "once it is up, the radio is armed exactly once";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);

cleanup:
    update_harness_down(&harness);
    inkwell_ble_mock_disable();
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

#endif /* INKWELL_HAVE_TLS */
