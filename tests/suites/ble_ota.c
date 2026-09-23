#define _POSIX_C_SOURCE 200809L

/*
 * The ESP32 OTA loader's conversation, and the HCI command that makes it five minutes rather
 * than seventeen.
 *
 * The loader is tests/support/ble_ota_fixture.c behind the bluez mock's write hook. It hashes
 * what it is sent and checks it against the hash the OTA command named, so a transfer that
 * reaches DONE here has sent exactly the bytes it announced, one write per chunk.
 */

#include "framework/mesh_test.h"
#include "support/ble_ota_fixture.h"

#include "inkwell/ble/central.h"
#include "inkwell/codec/sha256.h"
#include "mesh/core/esp_image.h"
#include "mesh/transport/ble_hci.h"
#include "mesh/transport/ble_ota.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RIG_LOADER "9C:13:9E:9D:0A:DA"
#define RIG_IMAGE_LEN 1000U
/* 97-byte chunks: ten full ones and a last of 30, so the short final write is exercised. */
#define RIG_MTU 100U
#define RIG_CHUNK 97U

struct ota_rig {
    struct inkwell_ble_central client;
    struct mesh_ble_ota ota;
    struct mesh_test_ota_loader loader;
    struct inkwell_ble_mock_config mock;
    uint8_t *image;
    uint8_t sha256[INKWELL_SHA256_DIGEST_LEN];
    uint64_t now;
};

/* `write_fail_after`, when non-zero, has the mock refuse every write after that many. */
static bool rig_open(struct ota_rig *rig, unsigned write_fail_after) {
    memset(rig, 0, sizeof *rig);
    mesh_test_ota_loader_init(&rig->loader, RIG_LOADER);
    rig->mock.mtu = RIG_MTU;
    rig->mock.write_hook = mesh_test_ota_loader_write;
    rig->mock.write_hook_userdata = &rig->loader;
    rig->mock.write_fail_after_calls = write_fail_after;
    rig->mock.write_result_late = -ENOTCONN;
    inkwell_ble_mock_enable(&rig->mock);
    if (inkwell_ble_open(&rig->client) != 0) {
        return false;
    }
    rig->image = mesh_test_esp_image(RIG_IMAGE_LEN, INKWELL_ESP_CHIP_ESP32_S3);
    if (rig->image == NULL) {
        return false;
    }
    struct inkwell_sha256 hasher;
    inkwell_sha256_init(&hasher);
    inkwell_sha256_update(&hasher, rig->image, RIG_IMAGE_LEN);
    inkwell_sha256_final(&hasher, rig->sha256);
    rig->now = 1000U;
    return mesh_ble_ota_attach(&rig->ota, &rig->client, RIG_LOADER) == 0;
}

static void rig_close(struct ota_rig *rig) {
    mesh_ble_ota_detach(&rig->ota);
    inkwell_ble_close(&rig->client);
    inkwell_ble_mock_disable();
    free(rig->image);
    rig->image = NULL;
}

static int rig_begin(struct ota_rig *rig) {
    return mesh_ble_ota_begin(&rig->ota, rig->image, RIG_IMAGE_LEN, rig->sha256, rig->now);
}

/* Ticks and lets the loader answer, ten simulated milliseconds a turn, until the conversation
   ends or `budget_ms` has passed. */
static void rig_run(struct ota_rig *rig, uint64_t budget_ms) {
    const uint64_t until = rig->now + budget_ms;
    while (mesh_ble_ota_busy(&rig->ota) && rig->now < until) {
        mesh_ble_ota_tick(&rig->ota, rig->now);
        mesh_test_ota_loader_flush(&rig->loader);
        rig->now += 10U;
    }
}

MESH_TEST_CASE(ble_ota_chunk_follows_the_mtu, unit) {
    /* Three below the MTU, because a Write Request spends three bytes on its header and one
       more byte is a long write the loader would count twice. Measured on the Brick against a
       V3: MTU 255, so 252. */
    MESH_TEST_FAIL_IF(mesh_ble_ota_chunk_for_mtu(255U) != 252U, "255 is a 252-byte chunk");
    MESH_TEST_FAIL_IF(mesh_ble_ota_chunk_for_mtu(517U) != 512U ||
                          mesh_ble_ota_chunk_for_mtu(600U) != 512U,
                      "the loader's own 512 is the ceiling");
    MESH_TEST_FAIL_IF(mesh_ble_ota_chunk_for_mtu(23U) != 20U, "the default MTU is a 20-byte chunk");
    MESH_TEST_FAIL_IF(mesh_ble_ota_chunk_for_mtu(0U) != 20U ||
                          mesh_ble_ota_chunk_for_mtu(22U) != 20U,
                      "no MTU, or one ATT does not allow, falls back to 20 rather than guessing");
    record_success(test_name);
}

/* An attach whose subscribe the stack answers late says -EAGAIN until it has, and is then
   attached exactly as a quick one is. */
MESH_TEST_CASE(ble_ota_attach_waits_for_the_subscribe, unit) {
    const struct inkwell_ble_mock_config mock = {.mtu = RIG_MTU, .subscribe_pending_polls = 2U};
    inkwell_ble_mock_enable(&mock);
    struct inkwell_ble_central client;
    struct mesh_ble_ota ota;
    memset(&ota, 0, sizeof ota);
    (void)inkwell_ble_open(&client);
    const int first = mesh_ble_ota_attach(&ota, &client, RIG_LOADER);
    const int second = mesh_ble_ota_attach(&ota, &client, RIG_LOADER);
    const int third = mesh_ble_ota_attach(&ota, &client, RIG_LOADER);
    const size_t chunk = ota.chunk;
    mesh_ble_ota_detach(&ota);
    inkwell_ble_close(&client);
    inkwell_ble_mock_disable();

    MESH_TEST_FAIL_IF(first != -EAGAIN || second != -EAGAIN,
                      "an attach did not wait for its subscribe");
    MESH_TEST_FAIL_IF(third != 0 || chunk != RIG_CHUNK,
                      "a late subscribe did not finish the attach");
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_sends_an_image, unit) {
    struct ota_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.chunk != RIG_CHUNK, rig_close(&rig),
                              "the chunk should come from the MTU the link reports");
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0 || rig.ota.state != MESH_BLE_OTA_VERSION,
                              rig_close(&rig), "the conversation should open with VERSION");
    rig_run(&rig, 60000U);

    char expected[128];
    char hex[INKWELL_SHA256_HEX_LEN];
    inkwell_sha256_hex(rig.sha256, hex, sizeof hex);
    snprintf(expected, sizeof expected, "OTA %u %s\n", RIG_IMAGE_LEN, hex);
    MESH_TEST_FAIL_IF_CLEANUP(
        rig.loader.command_count != 2U || strcmp(rig.loader.commands[0], "VERSION\n") != 0 ||
            strcmp(rig.loader.commands[1], expected) != 0,
        rig_close(&rig), "VERSION, then OTA with the size and the hash, each one write");
    MESH_TEST_FAIL_IF_CLEANUP(strcmp(rig.ota.loader_version, "43 2.7.26.54e0d8d 7 v1.0.0") != 0,
                              rig_close(&rig), "what followed VERSION's OK should be kept");
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_DONE || !rig.loader.finished_ok ||
                                  rig.loader.received != RIG_IMAGE_LEN,
                              rig_close(&rig),
                              "every byte should arrive and hash to what the OTA command named");
    MESH_TEST_FAIL_IF_CLEANUP(
        rig.loader.largest_write > RIG_CHUNK || rig.loader.first_binary_len != RIG_CHUNK ||
            rig.loader.uneven_writes != 0U,
        rig_close(&rig), "every chunk but the last should be exactly one MTU's payload");
    /* Ten full chunks and one short one: eleven writes of image, and ten ACKs, because the
       last is answered OK. */
    MESH_TEST_FAIL_IF_CLEANUP(rig.loader.writes != 2U + 11U || rig.loader.acks != 10U,
                              rig_close(&rig), "one write per chunk and one ACK per write");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ble_ota_progress(&rig.ota) != 100U, rig_close(&rig),
                              "a finished transfer is 100%");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_hash_mismatch, unit) {
    struct ota_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.loader.corrupt = true;
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0, rig_close(&rig), "begin");
    rig_run(&rig, 60000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_FAILED ||
                                  rig.ota.error != MESH_BLE_OTA_ERROR_HASH_MISMATCH ||
                                  strcmp(rig.ota.reason, "Hash Mismatch") != 0,
                              rig_close(&rig),
                              "a hash that does not match is its own failure, in the loader's "
                              "words - it is the one the same image again can fix");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_loader_refuses, unit) {
    struct ota_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.loader.refuse_start = true;
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0, rig_close(&rig), "begin");
    rig_run(&rig, 60000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_FAILED ||
                                  rig.ota.error != MESH_BLE_OTA_ERROR_REFUSED ||
                                  strcmp(rig.ota.reason, "Hash Rejected (NVS Mismatch)") != 0 ||
                                  rig.loader.received != 0U,
                              rig_close(&rig),
                              "a loader holding another image's hash refuses before a byte of "
                              "image is sent, and says so");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_ack_that_never_comes, unit) {
    struct ota_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.loader.ack_limit = 3U;
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0, rig_close(&rig), "begin");
    rig_run(&rig, 9000U);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ble_ota_busy(&rig.ota) || rig.loader.writes != 2U + 4U,
                              rig_close(&rig),
                              "an unanswered chunk holds the stream: nothing more goes out");
    rig_run(&rig, 60000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_FAILED ||
                                  rig.ota.error != MESH_BLE_OTA_ERROR_TIMEOUT ||
                                  rig.ota.acked != 3U * RIG_CHUNK,
                              rig_close(&rig),
                              "and after ten seconds it is a timeout, at the bytes that were "
                              "answered");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_silent_loader, unit) {
    struct ota_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.loader.silent = true;
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0, rig_close(&rig), "begin");
    rig_run(&rig, 60000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_FAILED ||
                                  rig.ota.error != MESH_BLE_OTA_ERROR_SILENT ||
                                  rig.loader.writes != 1U,
                              rig_close(&rig),
                              "nothing answering VERSION is its own failure, and nothing else "
                              "is sent to it");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_write_refused, unit) {
    struct ota_rig rig;
    /* VERSION, OTA and two chunks, then the link is gone. */
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 4U), rig_close(&rig), "the rig should open");
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0, rig_close(&rig), "begin");
    rig_run(&rig, 60000U);
    MESH_TEST_FAIL_IF_CLEANUP(
        rig.ota.state != MESH_BLE_OTA_FAILED || rig.ota.error != MESH_BLE_OTA_ERROR_WRITE ||
            rig.ota.write_error != -ENOTCONN || rig.ota.acked != 2U * RIG_CHUNK,
        rig_close(&rig), "a write BlueZ refuses ends the conversation with BlueZ's reason");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_ok_out_of_turn, unit) {
    struct ota_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.loader.ok_instead_of_ack = true;
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0, rig_close(&rig), "begin");
    rig_run(&rig, 60000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_FAILED ||
                                  rig.ota.error != MESH_BLE_OTA_ERROR_PROTOCOL ||
                                  rig.loader.writes != 3U,
                              rig_close(&rig),
                              "an OK mid-stream is the cadence lost, and nothing more is sent "
                              "into it");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_ota_answer_split_across_notifications, unit) {
    struct ota_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.loader.silent = true;
    MESH_TEST_FAIL_IF_CLEANUP(rig_begin(&rig) != 0, rig_close(&rig), "begin");
    /* A line is a line whatever the notifications carrying it look like. */
    mesh_ble_ota_feed(&rig.ota, (const uint8_t *)"OK 4", 4U);
    mesh_ble_ota_tick(&rig.ota, rig.now);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_VERSION, rig_close(&rig),
                              "half a line is not an answer yet");
    mesh_ble_ota_feed(&rig.ota, (const uint8_t *)"3 fw 1 v1\r\n", 11U);
    mesh_ble_ota_tick(&rig.ota, rig.now);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_BLE_OTA_STARTING ||
                                  strcmp(rig.ota.loader_version, "43 fw 1 v1") != 0,
                              rig_close(&rig), "the rest of it completes the answer");
    rig_close(&rig);
    record_success(test_name);
}
