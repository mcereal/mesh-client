#define _POSIX_C_SOURCE 200809L

/*
 * The BLE handover, end to end against the bluez mock: an ota_request that is answered or not,
 * a loader that appears at the radio's address plus one, a transfer that breaks and is picked
 * up again, and a radio that comes back where it was.
 *
 * The loader is tests/support/ble_ota_fixture.c. The device list is the mock's, and a test
 * brings a device into earshot by giving it an RSSI - which is exactly the evidence BlueZ has.
 */

#include "framework/mesh_test.h"
#include "support/ble_ota_fixture.h"

#include "mesh/core/esp_image.h"
#include "mesh/core/firmware_ota.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/ble_ota.h"
#include "mesh/utils/sha256.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RIG_RADIO "9C:13:9E:9D:0A:D9"
#define RIG_LOADER "9C:13:9E:9D:0A:DA"
#define RIG_ADAPTER "/org/bluez/hci0"
#define RIG_LOADER_PATH RIG_ADAPTER "/dev_9C_13_9E_9D_0A_DA"
#define RIG_IMAGE_LEN 2000U

static const char *const k_services[] = {MESH_BLE_MESHTASTIC_SERVICE_UUID,
                                         MESH_BLE_OTA_SERVICE_UUID};

/* The interval hook has no userdata - it is a function of the link, not of the install. */
static unsigned g_interval_calls;
static char g_interval_address[32];

static int rig_request_interval(int hci_dev, const char *address) {
    (void)hci_dev;
    g_interval_calls += 1U;
    snprintf(g_interval_address, sizeof g_interval_address, "%s", address);
    return 0;
}

struct fw_rig {
    struct mesh_bluez_client client;
    struct mesh_firmware_ota ota;
    struct mesh_test_ota_loader loader;
    struct mesh_bluez_mock_config mock;
    struct mesh_bluez_device_info devices[2]; /* the radio, then its loader */
    unsigned start_discovery_calls;
    unsigned stop_discovery_calls;
    char image_path[PATH_MAX];
    uint8_t *image;
    uint8_t sha256[MESH_SHA256_DIGEST_LEN];
    unsigned armed;
    uint8_t armed_hash[MESH_SHA256_DIGEST_LEN];
    int arm_result;
    unsigned done_calls;
    unsigned last_attempts;
    uint64_t now;
};

static int rig_arm(void *userdata, const uint8_t sha256[32]) {
    struct fw_rig *const rig = (struct fw_rig *)userdata;
    rig->armed += 1U;
    memcpy(rig->armed_hash, sha256, MESH_SHA256_DIGEST_LEN);
    return rig->arm_result;
}

static void rig_done(void *userdata, const struct mesh_firmware_ota *ota) {
    (void)ota;
    ((struct fw_rig *)userdata)->done_calls += 1U;
}

static bool rig_open(struct fw_rig *rig, uint16_t chip) {
    memset(rig, 0, sizeof *rig);
    g_interval_calls = 0U;
    g_interval_address[0] = '\0';
    mesh_test_ota_loader_init(&rig->loader, RIG_LOADER_PATH);

    snprintf(rig->devices[0].address, sizeof rig->devices[0].address, "%s", RIG_RADIO);
    snprintf(rig->devices[0].name, sizeof rig->devices[0].name, "Meshtastic_0ad8");
    rig->devices[0].paired = true;
    snprintf(rig->devices[1].address, sizeof rig->devices[1].address, "%s", RIG_LOADER);
    snprintf(rig->devices[1].name, sizeof rig->devices[1].name, "Meshtastic_0AD8");

    rig->mock.devices = rig->devices;
    rig->mock.device_count = 2U;
    rig->mock.device_service_uuids = k_services;
    rig->mock.adapter_path = RIG_ADAPTER;
    rig->mock.mtu = 100U;
    rig->mock.write_hook = mesh_test_ota_loader_write;
    rig->mock.write_hook_userdata = &rig->loader;
    rig->mock.start_discovery_calls = &rig->start_discovery_calls;
    rig->mock.stop_discovery_calls = &rig->stop_discovery_calls;
    mesh_bluez_client_mock_enable(&rig->mock);
    if (mesh_bluez_client_init(&rig->client) != 0) {
        return false;
    }

    rig->image = mesh_test_esp_image(RIG_IMAGE_LEN, chip);
    if (rig->image == NULL) {
        return false;
    }
    struct mesh_sha256 hasher;
    mesh_sha256_init(&hasher);
    mesh_sha256_update(&hasher, rig->image, RIG_IMAGE_LEN);
    mesh_sha256_final(&hasher, rig->sha256);

    snprintf(rig->image_path, sizeof rig->image_path, "/tmp/meshclient_ota_XXXXXX");
    const int fd = mkstemp(rig->image_path);
    if (fd < 0) {
        rig->image_path[0] = '\0';
        return false;
    }
    const bool written = write(fd, rig->image, RIG_IMAGE_LEN) == (ssize_t)RIG_IMAGE_LEN;
    close(fd);
    rig->now = 5000U;
    return written;
}

static void rig_close(struct fw_rig *rig) {
    mesh_firmware_ota_cancel(&rig->ota);
    mesh_bluez_client_shutdown(&rig->client);
    mesh_bluez_client_mock_disable();
    if (rig->image_path[0] != '\0') {
        unlink(rig->image_path);
    }
    free(rig->image);
    rig->image = NULL;
}

static struct mesh_firmware_ota_params rig_params(struct fw_rig *rig, bool arm) {
    struct mesh_firmware_ota_params params;
    memset(&params, 0, sizeof params);
    params.client = &rig->client;
    params.adapter_path = RIG_ADAPTER;
    params.image_path = rig->image_path;
    params.architecture = "esp32-s3";
    params.radio_address = arm ? RIG_RADIO : NULL;
    params.arm = arm ? rig_arm : NULL;
    params.arm_userdata = rig;
    params.request_interval = rig_request_interval;
    params.on_done = rig_done;
    params.userdata = rig;
    return params;
}

/* Fifty simulated milliseconds a turn, until the install reaches `until`, ends, or runs out of
   budget. A loader forgets its transfer when the link drops, so the fake one is restarted
   whenever the install goes back for another attempt. */
static void rig_run(struct fw_rig *rig, enum mesh_firmware_ota_state until, uint64_t budget_ms) {
    const uint64_t deadline = rig->now + budget_ms;
    while (mesh_firmware_ota_busy(&rig->ota) && rig->ota.state != until && rig->now < deadline) {
        mesh_firmware_ota_tick(&rig->ota, rig->now);
        mesh_test_ota_loader_flush(&rig->loader);
        if (rig->ota.attempts != rig->last_attempts) {
            rig->last_attempts = rig->ota.attempts;
            mesh_test_ota_loader_restart(&rig->loader);
        }
        rig->now += 50U;
    }
}

MESH_TEST_CASE(esp_image_reads_a_real_header, unit) {
    /* The header is the release's own bytes; see the fixture. */
    uint8_t image[64];
    memset(image, 0, sizeof image);
    memcpy(image, mesh_test_esp_header, sizeof mesh_test_esp_header);
    struct mesh_esp_image_info info;
    MESH_TEST_FAIL_IF(mesh_esp_image_validate(image, sizeof image, MESH_ESP_CHIP_ESP32_S3, &info) !=
                              MESH_ESP_IMAGE_OK ||
                          info.chip_id != MESH_ESP_CHIP_ESP32_S3 || info.segments != 7U,
                      "the Heltec V3's 2.7.26 image is an ESP32-S3 application of seven segments");
    MESH_TEST_FAIL_IF(mesh_esp_image_validate(image, sizeof image, MESH_ESP_CHIP_ESP32, &info) !=
                              MESH_ESP_IMAGE_WRONG_CHIP ||
                          info.chip_id != MESH_ESP_CHIP_ESP32_S3,
                      "and not an ESP32 one - chip 0 is a real chip, not a wildcard - and a "
                      "refusal still says which chip it was for");
    MESH_TEST_FAIL_IF(mesh_esp_image_validate(image, MESH_ESP_IMAGE_MIN_LEN - 1U,
                                              MESH_ESP_CHIP_ESP32_S3, NULL) !=
                          MESH_ESP_IMAGE_TOO_SHORT,
                      "a header cut short is too short");
    image[32] ^= 0xFFU;
    MESH_TEST_FAIL_IF(mesh_esp_image_validate(image, sizeof image, MESH_ESP_CHIP_ESP32_S3, NULL) !=
                          MESH_ESP_IMAGE_NOT_AN_APP,
                      "no app descriptor is not an application: a bootloader, a factory image");
    image[32] ^= 0xFFU;
    image[0] = 0xE8U;
    MESH_TEST_FAIL_IF(mesh_esp_image_validate(image, sizeof image, MESH_ESP_CHIP_ESP32_S3, NULL) !=
                          MESH_ESP_IMAGE_BAD_MAGIC,
                      "no 0xE9 is not an ESP32 image at all");

    uint16_t chip = 0xFFFFU;
    MESH_TEST_FAIL_IF(!mesh_esp_chip_for_architecture("esp32-s3", &chip) ||
                          chip != MESH_ESP_CHIP_ESP32_S3,
                      "deviceHardware's spelling");
    chip = 0xFFFFU;
    MESH_TEST_FAIL_IF(!mesh_esp_chip_for_architecture("esp32s3", &chip) ||
                          chip != MESH_ESP_CHIP_ESP32_S3,
                      "and the release manifest's, which is the one that bites");
    MESH_TEST_FAIL_IF(!mesh_esp_chip_for_architecture("esp32", &chip) || chip != MESH_ESP_CHIP_ESP32,
                      "the original ESP32 is chip 0");
    MESH_TEST_FAIL_IF(mesh_esp_chip_for_architecture("nrf52840", &chip) ||
                          mesh_esp_chip_for_architecture(NULL, &chip),
                      "an nRF52 has no app image");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_classifies_the_radio, unit) {
    /* AdminModule's sentences, verbatim at 81b3ce8. */
    MESH_TEST_FAIL_IF(mesh_firmware_ota_classify("Rebooting to BLE OTA") !=
                          MESH_FIRMWARE_OTA_ANSWER_GO_AHEAD,
                      "the go-ahead");
    MESH_TEST_FAIL_IF(
        mesh_firmware_ota_classify("Cannot start OTA: Invalid `ota_hash` provided.") !=
                MESH_FIRMWARE_OTA_ANSWER_REFUSED ||
            mesh_firmware_ota_classify("Cannot start OTA: Cannot find OTA Loader partition.") !=
                MESH_FIRMWARE_OTA_ANSWER_REFUSED ||
            mesh_firmware_ota_classify("Cannot start OTA: Device does have a valid OTA Loader.") !=
                MESH_FIRMWARE_OTA_ANSWER_REFUSED ||
            mesh_firmware_ota_classify("OTA Loader does not support BLE") !=
                MESH_FIRMWARE_OTA_ANSWER_REFUSED ||
            mesh_firmware_ota_classify("Unable to switch to the OTA partition.") !=
                MESH_FIRMWARE_OTA_ANSWER_REFUSED,
        "each of the firmware's refusals");
    MESH_TEST_FAIL_IF(mesh_firmware_ota_classify("Duty cycle limit exceeded") !=
                              MESH_FIRMWARE_OTA_ANSWER_UNRELATED ||
                          mesh_firmware_ota_classify(NULL) != MESH_FIRMWARE_OTA_ANSWER_UNRELATED,
                      "and anything else the radio has to say is not about this");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_offsets_addresses, unit) {
    char out[32];
    MESH_TEST_FAIL_IF(!mesh_firmware_ota_offset_address(RIG_RADIO, 1, out, sizeof out) ||
                          strcmp(out, RIG_LOADER) != 0,
                      "the loader is the radio plus one");
    MESH_TEST_FAIL_IF(!mesh_firmware_ota_offset_address("9C:13:9E:9D:0A:FF", 1, out, sizeof out) ||
                          strcmp(out, "9C:13:9E:9D:0B:00") != 0,
                      "with a carry into the octet before");
    MESH_TEST_FAIL_IF(!mesh_firmware_ota_offset_address("9C:13:9E:9D:0B:00", -1, out, sizeof out) ||
                          strcmp(out, "9C:13:9E:9D:0A:FF") != 0,
                      "and a borrow the other way");
    MESH_TEST_FAIL_IF(!mesh_firmware_ota_offset_address("ff:ff:ff:ff:ff:ff", 1, out, sizeof out) ||
                          strcmp(out, "00:00:00:00:00:00") != 0,
                      "forty-eight bits wrap");
    MESH_TEST_FAIL_IF(mesh_firmware_ota_offset_address("radio", 1, out, sizeof out) ||
                          mesh_firmware_ota_offset_address(RIG_RADIO, 1, out, 8U),
                      "text that is not an address, or no room for one, is refused");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_installs_over_ble, unit) {
    struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, MESH_ESP_CHIP_ESP32_S3), rig_close(&rig),
                              "the rig should open");
    const struct mesh_firmware_ota_params params = rig_params(&rig, true);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_start(&rig.ota, &params) != 0 ||
                                  rig.ota.state != MESH_FIRMWARE_OTA_ARMING,
                              rig_close(&rig), "the install should start by arming");
    MESH_TEST_FAIL_IF_CLEANUP(rig.armed != 1U || memcmp(rig.armed_hash, rig.sha256, 32U) != 0,
                              rig_close(&rig),
                              "the radio is held to the hash of the bytes that will be sent");

    rig_run(&rig, MESH_FIRMWARE_OTA_WAITING, 2000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_ARMING ||
                                  rig.start_discovery_calls != 0U,
                              rig_close(&rig),
                              "nothing is scanned for while the radio has not answered");
    mesh_firmware_ota_radio_said(&rig.ota, "Rebooting to BLE OTA");
    rig_run(&rig, MESH_FIRMWARE_OTA_CONNECTING, 3000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_WAITING || !rig.ota.go_ahead ||
                                  rig.start_discovery_calls != 1U,
                              rig_close(&rig),
                              "the go-ahead starts the scan, and no loader has been heard yet");

    rig.devices[1].rssi = -61;
    rig_run(&rig, MESH_FIRMWARE_OTA_RESTARTING, 600000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_RESTARTING ||
                                  strcmp(rig.ota.loader_address, RIG_LOADER) != 0,
                              rig_close(&rig), "the loader should be found and take the image");
    MESH_TEST_FAIL_IF_CLEANUP(g_interval_calls != 1U ||
                                  strcmp(g_interval_address, RIG_LOADER) != 0,
                              rig_close(&rig),
                              "the fast interval is asked for once, on the loader's link");
    MESH_TEST_FAIL_IF_CLEANUP(!rig.loader.finished_ok || rig.loader.received != RIG_IMAGE_LEN,
                              rig_close(&rig), "every byte, hashing to what the radio was given");
    MESH_TEST_FAIL_IF_CLEANUP(rig.stop_discovery_calls < 1U || rig.start_discovery_calls != 2U,
                              rig_close(&rig),
                              "the scan is down for the transfer and up again for the restart");

    rig_run(&rig, MESH_FIRMWARE_OTA_DONE, 5000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_RESTARTING, rig_close(&rig),
                              "a radio nobody has heard is not back yet");
    rig.devices[0].rssi = -55;
    rig_run(&rig, MESH_FIRMWARE_OTA_DONE, 5000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_DONE || !rig.ota.radio_seen ||
                                  rig.done_calls != 1U ||
                                  mesh_firmware_ota_progress(&rig.ota) != 100U ||
                                  mesh_firmware_ota_radio_in_loader(&rig.ota),
                              rig_close(&rig),
                              "the radio advertising again is done, reported exactly once");
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.image != NULL, rig_close(&rig),
                              "and the image is let go of");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_radio_refusal_leaves_it_running, unit) {
    struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, MESH_ESP_CHIP_ESP32_S3), rig_close(&rig),
                              "the rig should open");
    const struct mesh_firmware_ota_params params = rig_params(&rig, true);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_start(&rig.ota, &params) != 0, rig_close(&rig),
                              "start");
    mesh_firmware_ota_tick(&rig.ota, rig.now);
    mesh_firmware_ota_radio_said(&rig.ota, "OTA Loader does not support BLE");
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_FAILED ||
                                  rig.ota.error != MESH_FIRMWARE_OTA_ERROR_REFUSED ||
                                  strcmp(rig.ota.reason, "OTA Loader does not support BLE") != 0 ||
                                  rig.done_calls != 1U,
                              rig_close(&rig), "the radio's no ends it, in the radio's words");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_radio_in_loader(&rig.ota) ||
                                  rig.start_discovery_calls != 0U,
                              rig_close(&rig),
                              "and a radio that refused is still running: nothing to finish, "
                              "nothing scanned for");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_refuses_an_image_for_another_chip, unit) {
    struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, MESH_ESP_CHIP_ESP32_C3), rig_close(&rig),
                              "the rig should open");
    const struct mesh_firmware_ota_params params = rig_params(&rig, true);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_start(&rig.ota, &params) != -EINVAL ||
                                  rig.ota.state != MESH_FIRMWARE_OTA_FAILED ||
                                  rig.ota.error != MESH_FIRMWARE_OTA_ERROR_WRONG_IMAGE ||
                                  rig.ota.esp.chip_id != MESH_ESP_CHIP_ESP32_C3,
                              rig_close(&rig),
                              "a C3 image for an S3 radio is refused, saying which chip it was");
    MESH_TEST_FAIL_IF_CLEANUP(rig.armed != 0U || rig.done_calls != 0U, rig_close(&rig),
                              "before the radio is asked anything, and with no callback");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_resumes_a_radio_already_in_its_loader, unit) {
    struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, MESH_ESP_CHIP_ESP32_S3), rig_close(&rig),
                              "the rig should open");
    /* The client was quit mid-transfer: no radio to ask, a loader advertising. */
    rig.devices[1].rssi = -58;
    const struct mesh_firmware_ota_params params = rig_params(&rig, false);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_start(&rig.ota, &params) != 0 ||
                                  rig.ota.state != MESH_FIRMWARE_OTA_WAITING || rig.armed != 0U,
                              rig_close(&rig), "with no arm callback it starts at the loader");
    rig_run(&rig, MESH_FIRMWARE_OTA_RESTARTING, 600000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_RESTARTING ||
                                  strcmp(rig.ota.radio_address, RIG_RADIO) != 0,
                              rig_close(&rig),
                              "the transfer finishes, and the radio it belongs to is the "
                              "loader's address minus one");
    rig.devices[0].rssi = -52;
    rig_run(&rig, MESH_FIRMWARE_OTA_DONE, 5000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_DONE || !rig.ota.radio_seen,
                              rig_close(&rig), "and it is watched for there");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_retries_a_broken_transfer, unit) {
    struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, MESH_ESP_CHIP_ESP32_S3), rig_close(&rig),
                              "the rig should open");
    rig.devices[1].rssi = -60;
    rig.loader.ack_limit = 3U;
    const struct mesh_firmware_ota_params params = rig_params(&rig, false);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_start(&rig.ota, &params) != 0, rig_close(&rig),
                              "start");
    /* Let the first attempt stall, then give the loader its voice back. */
    rig_run(&rig, MESH_FIRMWARE_OTA_RESTARTING, 5000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_SENDING, rig_close(&rig),
                              "the first attempt should be stuck mid-stream");
    rig.loader.ack_limit = 0U;
    rig_run(&rig, MESH_FIRMWARE_OTA_RESTARTING, 600000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_RESTARTING ||
                                  rig.ota.attempts != 1U || !rig.loader.finished_ok ||
                                  rig.loader.received != RIG_IMAGE_LEN,
                              rig_close(&rig),
                              "a stalled transfer goes back to the loader and sends it all "
                              "again, which is what the loader expects of a link that returned");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_gives_up_and_says_where_the_radio_is, unit) {
    struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, MESH_ESP_CHIP_ESP32_S3), rig_close(&rig),
                              "the rig should open");
    rig.devices[1].rssi = -60;
    rig.loader.ack_limit = 2U;
    const struct mesh_firmware_ota_params params = rig_params(&rig, false);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_start(&rig.ota, &params) != 0, rig_close(&rig),
                              "start");
    rig_run(&rig, MESH_FIRMWARE_OTA_DONE, 600000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_FAILED ||
                                  rig.ota.error != MESH_FIRMWARE_OTA_ERROR_TRANSFER ||
                                  rig.ota.attempts != MESH_FIRMWARE_OTA_ATTEMPTS ||
                                  rig.done_calls != 1U,
                              rig_close(&rig), "three stalled transfers are a failed one");
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_firmware_ota_radio_in_loader(&rig.ota), rig_close(&rig),
                              "and it says the radio is in its loader, because it is");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_ota_loader_refusal_is_final, unit) {
    struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, MESH_ESP_CHIP_ESP32_S3), rig_close(&rig),
                              "the rig should open");
    rig.devices[1].rssi = -60;
    rig.loader.refuse_start = true;
    const struct mesh_firmware_ota_params params = rig_params(&rig, false);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_ota_start(&rig.ota, &params) != 0, rig_close(&rig),
                              "start");
    rig_run(&rig, MESH_FIRMWARE_OTA_DONE, 600000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.ota.state != MESH_FIRMWARE_OTA_FAILED ||
                                  rig.ota.error != MESH_FIRMWARE_OTA_ERROR_LOADER_REFUSED ||
                                  rig.ota.attempts != 0U ||
                                  strcmp(rig.ota.reason, "Hash Rejected (NVS Mismatch)") != 0,
                              rig_close(&rig),
                              "a loader holding another image's hash is not asked again - "
                              "the same bytes would get the same answer");
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_firmware_ota_radio_in_loader(&rig.ota), rig_close(&rig),
                              "and the radio is still in it");
    rig_close(&rig);
    record_success(test_name);
}
