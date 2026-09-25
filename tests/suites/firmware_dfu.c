#define _POSIX_C_SOURCE 200809L

/*
 * The nRF52's BLE install: the `-ota.zip` read and checked, the Legacy DFU conversation, and the
 * handover around it, against the bluez mock.
 *
 * The bootloader is the fake below, behind the mock's write hook. It keeps the state an Adafruit
 * bootloader keeps, counts what it is sent, sends a receipt every PRN packets the way SDK 11
 * does, and on VALIDATE really computes the CRC16 of what it received and compares it with the
 * init packet's - so a transfer that reaches DONE here sent exactly the image, in whole words.
 */

#include "framework/mesh_test.h"

#include "inkwell/base/time.h"
#include "inkwell/ble/central.h"
#include "inkwell/codec/inflate.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/dfu_package.h"
#include "mesh/core/firmware_catalog.h"
#include "mesh/core/firmware_dfu.h"
#include "mesh/transport/ble_dfu.h"
#include "mesh/transport/ble_gatt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RIG_RADIO "F4:12:FA:3C:88:10"
#define RIG_ADAPTER "/org/bluez/hci0"
/* Twenty full 96-byte packets and one of 80: the short last write, and two receipt windows. */
#define RIG_IMAGE_LEN 2000U
#define RIG_MTU 100U
#define RIG_PACKET 96U

/* ---- a package, built the way adafruit-nrfutil builds one ---------------------------------- */

static void put_le16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value & 0xFFU);
    at[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *at, uint32_t value) {
    for (size_t i = 0; i < 4U; ++i) {
        at[i] = (uint8_t)(value >> (8U * i) & 0xFFU);
    }
}

struct zip_builder {
    uint8_t bytes[16384];
    size_t len;
    uint8_t central[1024];
    size_t central_len;
    uint16_t entries;
};

/* One stored member, which is how every Meshtastic -ota.zip carries all three. */
static void zip_add(struct zip_builder *zip, const char *name, const uint8_t *data, size_t len) {
    uint32_t crc = 0U;
    (void)inkwell_crc32(data, len, &crc);
    const size_t name_len = strlen(name);
    const size_t offset = zip->len;

    uint8_t *const local = zip->bytes + zip->len;
    memset(local, 0, 30U);
    put_le32(local, 0x04034B50U);
    put_le16(local + 4, 20U);
    put_le32(local + 14, crc);
    put_le32(local + 18, (uint32_t)len);
    put_le32(local + 22, (uint32_t)len);
    put_le16(local + 26, (uint16_t)name_len);
    memcpy(local + 30, name, name_len);
    memcpy(local + 30 + name_len, data, len);
    zip->len += 30U + name_len + len;

    uint8_t *const entry = zip->central + zip->central_len;
    memset(entry, 0, 46U);
    put_le32(entry, 0x02014B50U);
    put_le16(entry + 4, 20U);
    put_le16(entry + 6, 20U);
    put_le32(entry + 16, crc);
    put_le32(entry + 20, (uint32_t)len);
    put_le32(entry + 24, (uint32_t)len);
    put_le16(entry + 28, (uint16_t)name_len);
    put_le32(entry + 42, (uint32_t)offset);
    memcpy(entry + 46, name, name_len);
    zip->central_len += 46U + name_len;
    zip->entries += 1U;
}

static void zip_close(struct zip_builder *zip) {
    const size_t central_offset = zip->len;
    memcpy(zip->bytes + zip->len, zip->central, zip->central_len);
    zip->len += zip->central_len;
    uint8_t *const end = zip->bytes + zip->len;
    memset(end, 0, 22U);
    put_le32(end, 0x06054B50U);
    put_le16(end + 8, zip->entries);
    put_le16(end + 10, zip->entries);
    put_le32(end + 12, (uint32_t)zip->central_len);
    put_le32(end + 16, (uint32_t)central_offset);
    zip->len += 22U;
}

static void make_image(uint8_t *image, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        image[i] = (uint8_t)(i * 13U + 7U);
    }
}

/* The fourteen bytes the T1000-E's 2.7.26 package carries, with this image's CRC16. */
static size_t make_init(uint8_t *init, const uint8_t *image, size_t len) {
    put_le16(init, 0x0052U);
    put_le16(init + 2, 0xFFFFU);
    put_le32(init + 4, 0xFFFFFFFFU);
    put_le16(init + 8, 1U);
    put_le16(init + 10, 0x0123U);
    put_le16(init + 12, mesh_dfu_crc16(image, len));
    return 14U;
}

static const char k_manifest[] =
    "{\n    \"manifest\": {\n        \"application\": {\n"
    "            \"bin_file\": \"firmware-tracker-t1000-e-2.7.26.54e0d8d.bin\",\n"
    "            \"dat_file\": \"firmware-tracker-t1000-e-2.7.26.54e0d8d.dat\",\n"
    "            \"init_packet_data\": {\"device_type\": 82, \"softdevice_req\": [291]}\n"
    "        },\n        \"dfu_version\": 0.5\n    }\n}\n";

/* `damage` flips a bit of the image after the init packet's CRC16 was taken from it. */
static void make_package(struct zip_builder *zip, const char *manifest, bool damage) {
    static uint8_t image[RIG_IMAGE_LEN];
    uint8_t init[16];
    make_image(image, sizeof image);
    const size_t init_len = make_init(init, image, sizeof image);
    if (damage) {
        image[100] ^= 0x01U;
    }
    memset(zip, 0, sizeof *zip);
    zip_add(zip, "firmware-tracker-t1000-e-2.7.26.54e0d8d.bin", image, sizeof image);
    zip_add(zip, "firmware-tracker-t1000-e-2.7.26.54e0d8d.dat", init, init_len);
    zip_add(zip, "manifest.json", (const uint8_t *)manifest, strlen(manifest));
    zip_close(zip);
}

MESH_TEST_CASE(dfu_crc16_is_nordics, unit) {
    /* CRC-16/CCITT-FALSE's check value, which is what Nordic's crc16_compute() is. */
    MESH_TEST_FAIL_IF(mesh_dfu_crc16((const uint8_t *)"123456789", 9U) != 0x29B1U,
                      "crc16 of \"123456789\" is 0x29B1");
    MESH_TEST_FAIL_IF(mesh_dfu_crc16(NULL, 0U) != 0xFFFFU, "and of nothing is its seed");
    record_success(test_name);
}

MESH_TEST_CASE(dfu_package_reads_an_nrf52_ota_zip, unit) {
    static struct zip_builder zip;
    make_package(&zip, k_manifest, false);
    struct mesh_dfu_package package;
    const enum mesh_dfu_package_verdict verdict =
        mesh_dfu_package_read(zip.bytes, zip.len, &package);
    uint8_t expected[RIG_IMAGE_LEN];
    make_image(expected, sizeof expected);
    const bool image_ok = package.image != NULL && package.image_len == RIG_IMAGE_LEN &&
                          memcmp(package.image, expected, RIG_IMAGE_LEN) == 0;
    const bool init_ok = package.init_len == 14U && package.device_type == 0x52U &&
                         package.softdevice == 0x0123U && package.crc_checked;
    mesh_dfu_package_free(&package);

    MESH_TEST_FAIL_IF(verdict != MESH_DFU_PACKAGE_OK, "a well-formed package reads");
    MESH_TEST_FAIL_IF(!image_ok, "the image is the .bin the manifest names, byte for byte");
    MESH_TEST_FAIL_IF(!init_ok, "the init packet is read and its CRC16 matched the image");
    record_success(test_name);
}

MESH_TEST_CASE(dfu_package_refuses_what_the_bootloader_would, unit) {
    static struct zip_builder zip;
    struct mesh_dfu_package package;

    make_package(&zip, k_manifest, true);
    MESH_TEST_FAIL_IF(mesh_dfu_package_read(zip.bytes, zip.len, &package) !=
                              MESH_DFU_PACKAGE_CRC_MISMATCH ||
                          package.image != NULL,
                      "an image that is not the one its init packet describes never reaches a "
                      "radio");

    /* A bootloader package: flashing it as an application is how a board is bricked. */
    make_package(&zip,
                 "{\"manifest\": {\"bootloader\": {\"bin_file\": \"a.bin\", \"dat_file\": "
                 "\"a.dat\"}}}",
                 false);
    MESH_TEST_FAIL_IF(mesh_dfu_package_read(zip.bytes, zip.len, &package) !=
                          MESH_DFU_PACKAGE_NO_APPLICATION,
                      "a package with no application is refused");

    make_package(&zip,
                 "{\"manifest\": {\"application\": {\"bin_file\": \"missing.bin\", "
                 "\"dat_file\": \"firmware-tracker-t1000-e-2.7.26.54e0d8d.dat\"}}}",
                 false);
    MESH_TEST_FAIL_IF(mesh_dfu_package_read(zip.bytes, zip.len, &package) !=
                          MESH_DFU_PACKAGE_CORRUPT,
                      "a file the manifest names and the zip does not hold");

    make_package(&zip, k_manifest, false);
    /* Inside the .bin's stored bytes, past its local header and name: its CRC32 no longer
       holds, though the init packet was never touched. */
    zip.bytes[30U + strlen("firmware-tracker-t1000-e-2.7.26.54e0d8d.bin") + 10U] ^= 0xFFU;
    MESH_TEST_FAIL_IF(mesh_dfu_package_read(zip.bytes, zip.len, &package) !=
                          MESH_DFU_PACKAGE_CORRUPT,
                      "a member that fails its CRC32");

    const uint8_t text[] = "not a zip at all";
    MESH_TEST_FAIL_IF(mesh_dfu_package_read(text, sizeof text, &package) !=
                          MESH_DFU_PACKAGE_NOT_A_ZIP,
                      "bytes that are not a zip");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_catalog_nrf52_takes_either_bus, unit) {
    MESH_TEST_FAIL_IF(mesh_firmware_path_for_architecture("nrf52840") != MESH_FIRMWARE_PATH_USB,
                      "an nRF52's first path is still its UF2 drive");
    MESH_TEST_FAIL_IF(!mesh_firmware_architecture_takes("nrf52840", MESH_FIRMWARE_PATH_USB),
                      "an nRF52 always takes USB");
    /* BLE only where the stack can send Write Commands: on CoreBluetooth and WinRT the first
       packet would fail after START had erased the application. */
    MESH_TEST_FAIL_IF(mesh_firmware_architecture_takes("nrf52840", MESH_FIRMWARE_PATH_BLE) !=
                          mesh_firmware_nordic_dfu_available(),
                      "and BLE exactly where this build's stack can carry Nordic DFU");
#if defined(INKWELL_BLE_BACKEND_bluez)
    MESH_TEST_FAIL_IF(!mesh_firmware_nordic_dfu_available(), "BlueZ carries it");
#else
    MESH_TEST_FAIL_IF(mesh_firmware_nordic_dfu_available(), "nothing else does yet");
#endif
    MESH_TEST_FAIL_IF(!mesh_firmware_architecture_uses_nordic_dfu("nrf52840") ||
                          mesh_firmware_architecture_uses_nordic_dfu("esp32-s3"),
                      "only the nRF52 speaks Nordic DFU");
    MESH_TEST_FAIL_IF(mesh_firmware_architecture_takes("rp2040", MESH_FIRMWARE_PATH_BLE) ||
                          mesh_firmware_architecture_takes("esp32-s3", MESH_FIRMWARE_PATH_USB) ||
                          mesh_firmware_architecture_takes("esp32-c3", MESH_FIRMWARE_PATH_BLE),
                      "nobody else gains a bus");
    MESH_TEST_FAIL_IF(mesh_firmware_architecture_takes("nrf52840", MESH_FIRMWARE_PATH_NONE),
                      "and nothing travels over no bus");
    record_success(test_name);
}

/* ---- a bootloader ---------------------------------------------------------------------------- */

#define FAKE_QUEUE 8U

enum fake_state {
    FAKE_IDLE = 0,
    FAKE_SIZES,     /* START taken, the sizes due on the packet characteristic */
    FAKE_READY,     /* erased */
    FAKE_INIT,      /* taking the init packet */
    FAKE_INITED,    /* init packet accepted */
    FAKE_RECEIVING, /* taking the image */
    FAKE_RECEIVED,
};

struct fake_bootloader {
    /* What it does. */
    bool stale;         /* START answers INVALID_STATE, as after a broken transfer */
    bool miscount;      /* receipts count one byte short */
    size_t stale_times; /* when `stale`, how many STARTs are refused before one is taken */

    /* What it saw. */
    enum fake_state state;
    unsigned triggers; /* START on the radio's application, before any connect */
    unsigned starts;
    unsigned resets;
    bool activated;
    bool validated;
    uint32_t expect_len;
    uint8_t init[32];
    size_t init_len;
    uint8_t image[RIG_IMAGE_LEN + 64U];
    size_t received;
    unsigned prn;
    unsigned packets;
    unsigned receipts;
    size_t largest;
    uint64_t last_packet_ms;
    uint64_t closest_packets_ms; /* the smallest gap between two image packets */
    unsigned unaligned;          /* packets other than the last whose length is not whole words */
    bool connected_once;

    uint8_t queue[FAKE_QUEUE][8];
    size_t queue_len[FAKE_QUEUE];
    size_t queued;
    char control[160];
};

static void fake_say(struct fake_bootloader *fake, const uint8_t *bytes, size_t len) {
    if (fake->queued < FAKE_QUEUE) {
        memcpy(fake->queue[fake->queued], bytes, len);
        fake->queue_len[fake->queued] = len;
        fake->queued += 1U;
    }
}

static void fake_respond(struct fake_bootloader *fake, uint8_t opcode, uint8_t status) {
    const uint8_t bytes[3] = {0x10U, opcode, status};
    fake_say(fake, bytes, sizeof bytes);
}

static bool ends_with(const char *text, const char *suffix) {
    const size_t a = strlen(text);
    const size_t b = strlen(suffix);
    return a >= b && strcmp(text + a - b, suffix) == 0;
}

static void fake_control(struct fake_bootloader *fake, const uint8_t *data, size_t len) {
    switch (data[0]) {
    case 0x01:
        if (!fake->connected_once) {
            fake->triggers += 1U;
            return;
        }
        fake->starts += 1U;
        if (fake->stale && fake->starts <= fake->stale_times) {
            fake_respond(fake, 0x01U, 0x02U);
            return;
        }
        fake->state = FAKE_SIZES;
        return;
    case 0x02:
        if (len >= 2U && data[1] == 0x00U) {
            fake->state = FAKE_INIT;
            fake->init_len = 0U;
        } else {
            fake->state = FAKE_INITED;
            fake_respond(fake, 0x02U, 0x01U);
        }
        return;
    case 0x08:
        fake->prn = len >= 3U ? (unsigned)data[1] | (unsigned)data[2] << 8 : 0U;
        return;
    case 0x03:
        fake->state = FAKE_RECEIVING;
        fake->received = 0U;
        fake->packets = 0U;
        return;
    case 0x04: {
        const uint16_t want =
            (uint16_t)(fake->init[fake->init_len - 2U] | fake->init[fake->init_len - 1U] << 8);
        fake->validated = mesh_dfu_crc16(fake->image, fake->received) == want;
        fake_respond(fake, 0x04U, fake->validated ? 0x01U : 0x05U);
        return;
    }
    case 0x05:
        fake->activated = true;
        return;
    case 0x06:
        fake->resets += 1U;
        fake->state = FAKE_IDLE;
        return;
    default:
        return;
    }
}

static void fake_packet(struct fake_bootloader *fake, const uint8_t *data, size_t len) {
    switch (fake->state) {
    case FAKE_SIZES:
        if (len == 12U) {
            fake->expect_len = (uint32_t)data[8] | (uint32_t)data[9] << 8 |
                               (uint32_t)data[10] << 16 | (uint32_t)data[11] << 24;
            fake->state = FAKE_READY;
            fake_respond(fake, 0x01U, 0x01U);
        }
        return;
    case FAKE_INIT:
        if (fake->init_len + len <= sizeof fake->init) {
            memcpy(fake->init + fake->init_len, data, len);
            fake->init_len += len;
        }
        return;
    case FAKE_RECEIVING:
        if (fake->received + len > sizeof fake->image) {
            return;
        }
        if (len > fake->largest) {
            fake->largest = len;
        }
        memcpy(fake->image + fake->received, data, len);
        fake->received += len;
        fake->packets += 1U;
        {
            const uint64_t now = inkwell_time_monotonic_ms();
            if (fake->packets > 1U && (fake->closest_packets_ms == 0U ||
                                       now - fake->last_packet_ms < fake->closest_packets_ms)) {
                fake->closest_packets_ms = now - fake->last_packet_ms;
            }
            fake->last_packet_ms = now;
        }
        if (len % 4U != 0U && fake->received < fake->expect_len) {
            fake->unaligned += 1U;
        }
        if (fake->prn != 0U && fake->packets % fake->prn == 0U) {
            uint8_t receipt[5] = {0x11U};
            put_le32(receipt + 1,
                     (uint32_t)(fake->miscount ? fake->received - 1U : fake->received));
            fake->receipts += 1U;
            fake_say(fake, receipt, sizeof receipt);
        }
        if (fake->received >= fake->expect_len) {
            fake->state = FAKE_RECEIVED;
            fake_respond(fake, 0x03U, 0x01U);
        }
        return;
    default:
        return;
    }
}

static void fake_write(void *userdata, const char *handle, const uint8_t *data, size_t len) {
    struct fake_bootloader *const fake = (struct fake_bootloader *)userdata;
    if (len == 0U) {
        return;
    }
    if (ends_with(handle, MESH_BLE_DFU_CONTROL_UUID)) {
        snprintf(fake->control, sizeof fake->control, "%s", handle);
        fake_control(fake, data, len);
    } else if (ends_with(handle, MESH_BLE_DFU_PACKET_UUID)) {
        fake_packet(fake, data, len);
    }
}

static void fake_flush(struct fake_bootloader *fake) {
    const size_t queued = fake->queued;
    fake->queued = 0U;
    for (size_t i = 0; i < queued; ++i) {
        inkwell_ble_mock_emit_notification(fake->control, fake->queue[i], fake->queue_len[i]);
    }
}

/* ---- the conversation ------------------------------------------------------------------------ */

struct talk_rig {
    struct inkwell_ble_central client;
    struct mesh_ble_dfu dfu;
    struct fake_bootloader fake;
    struct inkwell_ble_mock_config mock;
    uint8_t image[RIG_IMAGE_LEN];
    uint8_t init[16];
    size_t init_len;
    uint64_t now;
};

static bool talk_open(struct talk_rig *rig) {
    memset(rig, 0, sizeof *rig);
    rig->fake.connected_once = true;
    rig->mock.mtu = RIG_MTU;
    rig->mock.write_hook = fake_write;
    rig->mock.write_hook_userdata = &rig->fake;
    inkwell_ble_mock_enable(&rig->mock);
    if (inkwell_ble_open(&rig->client) != 0) {
        return false;
    }
    make_image(rig->image, sizeof rig->image);
    rig->init_len = make_init(rig->init, rig->image, sizeof rig->image);
    snprintf(rig->fake.control, sizeof rig->fake.control, "%s/%s", RIG_RADIO,
             MESH_BLE_DFU_CONTROL_UUID);
    rig->now = 1000U;
    return mesh_ble_dfu_attach(&rig->dfu, &rig->client, RIG_RADIO) == 0;
}

static void talk_close(struct talk_rig *rig) {
    mesh_ble_dfu_detach(&rig->dfu);
    inkwell_ble_close(&rig->client);
    inkwell_ble_mock_disable();
}

static void talk_run(struct talk_rig *rig, uint64_t budget_ms) {
    const uint64_t until = rig->now + budget_ms;
    while (mesh_ble_dfu_busy(&rig->dfu) && rig->now < until) {
        mesh_ble_dfu_tick(&rig->dfu, rig->now);
        fake_flush(&rig->fake);
        rig->now += 10U;
    }
}

MESH_TEST_CASE(ble_dfu_packet_is_whole_words, unit) {
    MESH_TEST_FAIL_IF(mesh_ble_dfu_packet_for_mtu(247U) != 244U, "the largest ATT payload");
    MESH_TEST_FAIL_IF(mesh_ble_dfu_packet_for_mtu(517U) != 244U, "capped at 244");
    MESH_TEST_FAIL_IF(mesh_ble_dfu_packet_for_mtu(100U) != 96U,
                      "97 bytes would fit a Write Command and is refused by the bootloader");
    MESH_TEST_FAIL_IF(mesh_ble_dfu_packet_for_mtu(23U) != 20U ||
                          mesh_ble_dfu_packet_for_mtu(0U) != 20U,
                      "and 20 with no MTU");
    record_success(test_name);
}

MESH_TEST_CASE(ble_dfu_sends_an_image, unit) {
    static struct talk_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!talk_open(&rig), talk_close(&rig), "the rig should open");
    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.packet != RIG_PACKET, talk_close(&rig),
                              "the packet comes from the MTU, floored to whole words");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ble_dfu_begin(&rig.dfu, rig.init, rig.init_len, rig.image,
                                                 sizeof rig.image, rig.now) != 0,
                              talk_close(&rig), "begin");
    talk_run(&rig, 60000U);

    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_BLE_DFU_DONE, talk_close(&rig),
                              "the conversation should finish");
    MESH_TEST_FAIL_IF_CLEANUP(
        rig.fake.expect_len != RIG_IMAGE_LEN || rig.fake.init_len != rig.init_len ||
            memcmp(rig.fake.init, rig.init, rig.init_len) != 0,
        talk_close(&rig), "the sizes name the application, and the init packet arrives whole");
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.prn != MESH_BLE_DFU_PRN, talk_close(&rig),
                              "a receipt is asked for every ten packets");
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.received != RIG_IMAGE_LEN || !rig.fake.validated ||
                                  memcmp(rig.fake.image, rig.image, RIG_IMAGE_LEN) != 0,
                              talk_close(&rig), "every byte arrives and its CRC16 validates");
    MESH_TEST_FAIL_IF_CLEANUP(
        rig.fake.largest != RIG_PACKET || rig.fake.unaligned != 0U || rig.fake.packets != 21U,
        talk_close(&rig), "twenty whole-word packets and a short last one, nothing larger");
    MESH_TEST_FAIL_IF_CLEANUP(!rig.fake.activated, talk_close(&rig),
                              "and the bootloader is told to activate it");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ble_dfu_progress(&rig.dfu) != 100U, talk_close(&rig),
                              "a finished transfer is 100%");
    talk_close(&rig);
    record_success(test_name);
}

/* The bootloader's answers move the conversation on - the init packet goes out on START's
   answer - but never send image packets: BlueZ answers a Write Command when it has queued it,
   and a burst on those answers is what a stock Adafruit bootloader refused with
   OPERATION_FAILED. Packets go one per tick of the caller. */
MESH_TEST_CASE(ble_dfu_answers_never_burst_the_image, unit) {
    static struct talk_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!talk_open(&rig), talk_close(&rig), "the rig should open");
    (void)mesh_ble_dfu_begin(&rig.dfu, rig.init, rig.init_len, rig.image, sizeof rig.image,
                             rig.now);
    for (unsigned i = 0U; i < 64U; ++i) {
        fake_flush(&rig.fake);
    }
    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_BLE_DFU_SENDING ||
                                  rig.fake.init_len != rig.init_len || rig.fake.packets != 0U,
                              talk_close(&rig),
                              "the answers carry it to the image and not one packet further");
    for (unsigned i = 0U; i < 3U; ++i) {
        mesh_ble_dfu_tick(&rig.dfu, rig.now);
        fake_flush(&rig.fake);
    }
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.packets != 3U, talk_close(&rig), "then one a tick");
    talk_close(&rig);
    record_success(test_name);
}

/* On a loop, the image goes on a timer of its own: whole, and never two packets closer than the
   pace - which is the difference between a stock bootloader taking it and OPERATION_FAILED. */
MESH_TEST_CASE(ble_dfu_paces_the_image_on_a_loop, unit) {
    static struct talk_rig rig;
    struct inkwell_loop loop;
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "the loop should open");
    memset(&rig, 0, sizeof rig);
    rig.fake.connected_once = true;
    rig.mock.mtu = RIG_MTU;
    rig.mock.write_hook = fake_write;
    rig.mock.write_hook_userdata = &rig.fake;
    inkwell_ble_mock_enable(&rig.mock);
    (void)inkwell_ble_open(&rig.client);
    (void)inkwell_ble_attach_loop(&rig.client, &loop);
    make_image(rig.image, sizeof rig.image);
    rig.init_len = make_init(rig.init, rig.image, sizeof rig.image);
    snprintf(rig.fake.control, sizeof rig.fake.control, "%s/%s", RIG_RADIO,
             MESH_BLE_DFU_CONTROL_UUID);
    const int attached = mesh_ble_dfu_attach(&rig.dfu, &rig.client, RIG_RADIO);
    const bool timed = rig.dfu.pace_fd >= 0;
    (void)mesh_ble_dfu_begin(&rig.dfu, rig.init, rig.init_len, rig.image, sizeof rig.image,
                             inkwell_time_monotonic_ms());
    for (unsigned i = 0U; i < 400U && mesh_ble_dfu_busy(&rig.dfu); ++i) {
        (void)inkwell_loop_run(&loop, 5);
        fake_flush(&rig.fake);
    }
    const bool done = rig.dfu.state == MESH_BLE_DFU_DONE && rig.fake.received == RIG_IMAGE_LEN;
    const uint64_t closest = rig.fake.closest_packets_ms;
    talk_close(&rig);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(attached != 0 || !timed, "on a loop the conversation keeps its own timer");
    MESH_TEST_FAIL_IF(!done, "the paced image arrives whole");
    MESH_TEST_FAIL_IF(closest + 2U < MESH_BLE_DFU_PACE_MS,
                      "and no two packets closer than the pace, give or take the timer");
    record_success(test_name);
}

MESH_TEST_CASE(ble_dfu_stale_session_is_its_own_error, unit) {
    static struct talk_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!talk_open(&rig), talk_close(&rig), "the rig should open");
    rig.fake.stale = true;
    rig.fake.stale_times = 1U;
    (void)mesh_ble_dfu_begin(&rig.dfu, rig.init, rig.init_len, rig.image, sizeof rig.image,
                             rig.now);
    talk_run(&rig, 10000U);
    MESH_TEST_FAIL_IF_CLEANUP(
        rig.dfu.state != MESH_BLE_DFU_FAILED || rig.dfu.error != MESH_BLE_DFU_ERROR_STALE,
        talk_close(&rig), "START answered INVALID_STATE is a stale session, not a refusal");
    talk_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_dfu_receipt_that_miscounts, unit) {
    static struct talk_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!talk_open(&rig), talk_close(&rig), "the rig should open");
    rig.fake.miscount = true;
    (void)mesh_ble_dfu_begin(&rig.dfu, rig.init, rig.init_len, rig.image, sizeof rig.image,
                             rig.now);
    talk_run(&rig, 10000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_BLE_DFU_FAILED ||
                                  rig.dfu.error != MESH_BLE_DFU_ERROR_RECEIPT,
                              talk_close(&rig), "a Write Command lost is lost silently");
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.packets != MESH_BLE_DFU_PRN, talk_close(&rig),
                              "and nothing is sent past the receipt that said so");
    talk_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(ble_dfu_crc_refused_at_validate, unit) {
    static struct talk_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!talk_open(&rig), talk_close(&rig), "the rig should open");
    rig.init[12] ^= 0xFFU;
    (void)mesh_ble_dfu_begin(&rig.dfu, rig.init, rig.init_len, rig.image, sizeof rig.image,
                             rig.now);
    talk_run(&rig, 60000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_BLE_DFU_FAILED ||
                                  rig.dfu.error != MESH_BLE_DFU_ERROR_REFUSED ||
                                  rig.dfu.status != MESH_BLE_DFU_STATUS_CRC_ERROR,
                              talk_close(&rig), "the bootloader's CRC error, with its status");
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.activated, talk_close(&rig),
                              "and an image that failed validation is never activated");
    talk_close(&rig);
    record_success(test_name);
}

/* ---- the handover ---------------------------------------------------------------------------- */

static const char *const k_services[] = {MESH_BLE_MESHTASTIC_SERVICE_UUID};
static const uint8_t k_bootloader_version[] = {0x08U, 0x00U};
static const uint8_t *const k_reads[] = {k_bootloader_version};
static const size_t k_read_lengths[] = {sizeof k_bootloader_version};

static unsigned g_interval_calls;

static int rig_request_interval(struct inkwell_ble_central *central, const char *address) {
    (void)central;
    (void)address;
    g_interval_calls += 1U;
    return 0;
}

struct fw_rig {
    struct inkwell_ble_central client;
    struct mesh_firmware_dfu dfu;
    struct fake_bootloader fake;
    struct inkwell_ble_mock_config mock;
    struct inkwell_ble_device devices[1];
    char package_path[PATH_MAX];
    unsigned done_calls;
    uint64_t now;
};

static void rig_done(void *userdata, const struct mesh_firmware_dfu *dfu) {
    (void)dfu;
    ((struct fw_rig *)userdata)->done_calls += 1U;
}

static bool rig_open(struct fw_rig *rig, unsigned drops_after_polls) {
    memset(rig, 0, sizeof *rig);
    g_interval_calls = 0U;
    snprintf(rig->devices[0].address, sizeof rig->devices[0].address, "%s", RIG_RADIO);
    snprintf(rig->devices[0].name, sizeof rig->devices[0].name, "T1000-E_8810");
    rig->devices[0].paired = true;
    rig->devices[0].in_range = true;
    rig->devices[0].rssi = -60;
    rig->mock.devices = rig->devices;
    rig->mock.device_count = 1U;
    rig->mock.device_service_uuids = k_services;
    rig->mock.adapter_name = RIG_ADAPTER;
    rig->mock.mtu = RIG_MTU;
    rig->mock.write_hook = fake_write;
    rig->mock.write_hook_userdata = &rig->fake;
    rig->mock.read_payloads = k_reads;
    rig->mock.read_payload_lengths = k_read_lengths;
    rig->mock.read_payload_count = 1U;
    rig->mock.connected_drops_after_polls = drops_after_polls;
    inkwell_ble_mock_enable(&rig->mock);
    if (inkwell_ble_open(&rig->client) != 0) {
        return false;
    }
    snprintf(rig->fake.control, sizeof rig->fake.control, "%s/%s", RIG_RADIO,
             MESH_BLE_DFU_CONTROL_UUID);

    static struct zip_builder zip;
    make_package(&zip, k_manifest, false);
    snprintf(rig->package_path, sizeof rig->package_path, "/tmp/meshclient_dfu_XXXXXX");
    const int fd = mkstemp(rig->package_path);
    if (fd < 0) {
        rig->package_path[0] = '\0';
        return false;
    }
    const bool written = write(fd, zip.bytes, zip.len) == (ssize_t)zip.len;
    close(fd);
    rig->now = 5000U;
    return written;
}

static void rig_close(struct fw_rig *rig) {
    mesh_firmware_dfu_cancel(&rig->dfu);
    inkwell_ble_close(&rig->client);
    inkwell_ble_mock_disable();
    if (rig->package_path[0] != '\0') {
        unlink(rig->package_path);
    }
}

static struct mesh_firmware_dfu_params rig_params(struct fw_rig *rig, bool arm) {
    struct mesh_firmware_dfu_params params;
    memset(&params, 0, sizeof params);
    params.client = &rig->client;
    params.package_path = rig->package_path;
    params.radio_address = RIG_RADIO;
    params.arm = arm;
    params.request_interval = rig_request_interval;
    params.on_done = rig_done;
    params.userdata = rig;
    return params;
}

static void rig_run(struct fw_rig *rig, enum mesh_firmware_dfu_state until, uint64_t budget_ms) {
    const uint64_t deadline = rig->now + budget_ms;
    while (mesh_firmware_dfu_busy(&rig->dfu) && rig->dfu.state != until && rig->now < deadline) {
        mesh_firmware_dfu_tick(&rig->dfu, rig->now);
        fake_flush(&rig->fake);
        rig->now += 50U;
    }
}

/* After a DFU install BlueZ's record of a bonded nRF52 lists the bootloader's services and not
   Meshtastic's, and auto-connect never reached for it. A bonded one is still a radio of ours; a
   stranger that happens to be built on Adafruit's stack is not. */
MESH_TEST_CASE(ble_list_keeps_a_bonded_radio_after_dfu, unit) {
    static const char *const k_uuids[] = {MESH_BLE_MESHTASTIC_SERVICE_UUID,
                                          MESH_BLE_DFU_SERVICE_UUID, MESH_BLE_DFU_SERVICE_UUID};
    struct inkwell_ble_device known[3];
    memset(known, 0, sizeof known);
    snprintf(known[0].address, sizeof known[0].address, "%s", "FB:17:7C:37:6D:DA");
    snprintf(known[1].address, sizeof known[1].address, "%s", RIG_RADIO);
    known[1].paired = true;
    snprintf(known[2].address, sizeof known[2].address, "%s", "C0:FF:EE:00:00:01");
    struct inkwell_ble_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.devices = known;
    mock.device_count = 3U;
    mock.device_service_uuids = k_uuids;
    inkwell_ble_mock_enable(&mock);
    struct inkwell_ble_central client;
    (void)inkwell_ble_open(&client);
    struct inkwell_ble_device listed[8];
    size_t count = 0U;
    const int result = mesh_ble_list_meshtastic(&client, listed, 8U, &count);
    inkwell_ble_close(&client);
    inkwell_ble_mock_disable();
    MESH_TEST_FAIL_IF(result != 0 || count != 2U, "the Meshtastic node and the bonded nRF52");
    MESH_TEST_FAIL_IF(strcmp(listed[1].address, RIG_RADIO) != 0,
                      "the bonded radio under its bootloader's services is listed");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_dfu_triggers_the_bootloader, unit) {
    static struct fw_rig rig;
    /* The link goes down on the third look, which is the radio rebooting under the trigger. */
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 2U), rig_close(&rig), "the rig should open");
    const struct mesh_firmware_dfu_params params = rig_params(&rig, true);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_dfu_start(&rig.dfu, &params) != 0 ||
                                  rig.dfu.state != MESH_FIRMWARE_DFU_ARMING,
                              rig_close(&rig), "a checked package starts at arming");
    rig_run(&rig, MESH_FIRMWARE_DFU_WAITING, 5000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.triggers != 1U || rig.fake.starts != 0U, rig_close(&rig),
                              "START goes once to the radio's own control point");
    MESH_TEST_FAIL_IF_CLEANUP(strcmp(rig.fake.control, RIG_RADIO "/" MESH_BLE_DFU_CONTROL_UUID) !=
                                  0,
                              rig_close(&rig), "on the radio's address, over the open link");
    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_FIRMWARE_DFU_WAITING || !rig.dfu.dropped,
                              rig_close(&rig), "and the link dropping is what ends arming");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_dfu_installs_from_the_bootloader, unit) {
    static struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.fake.connected_once = true;
    const struct mesh_firmware_dfu_params params = rig_params(&rig, false);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_dfu_start(&rig.dfu, &params) != 0, rig_close(&rig),
                              "a resume starts");
    rig_run(&rig, MESH_FIRMWARE_DFU_DONE, 120000U);

    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_FIRMWARE_DFU_DONE || rig.done_calls != 1U,
                              rig_close(&rig), "the install should finish, and say so once");
    MESH_TEST_FAIL_IF_CLEANUP(strcmp(rig.dfu.loader_address, RIG_RADIO) != 0, rig_close(&rig),
                              "a bonded bootloader is at the radio's own address");
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.triggers != 0U || rig.fake.starts != 1U, rig_close(&rig),
                              "a resume sends no trigger, and one START");
    MESH_TEST_FAIL_IF_CLEANUP(!rig.fake.validated || !rig.fake.activated ||
                                  rig.fake.received != RIG_IMAGE_LEN,
                              rig_close(&rig), "the image arrives whole and is activated");
    MESH_TEST_FAIL_IF_CLEANUP(g_interval_calls != 1U, rig_close(&rig),
                              "the fast interval is asked for on the bootloader's link");
    MESH_TEST_FAIL_IF_CLEANUP(!rig.dfu.radio_seen, rig_close(&rig),
                              "and the radio is heard advertising again");
    rig_close(&rig);
    record_success(test_name);
}

/* What BlueZ does to a bonded radio's record: the bootloader's services replace the radio's, and
   the radio comes back advertising under a record that only lists `1530`. */
MESH_TEST_CASE(firmware_dfu_radio_back_under_the_bootloaders_services, unit) {
    static struct fw_rig rig;
    static const char *const k_bootloader_services[] = {MESH_BLE_DFU_SERVICE_UUID};
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.mock.device_service_uuids = k_bootloader_services;
    inkwell_ble_mock_enable(&rig.mock);
    rig.fake.connected_once = true;
    const struct mesh_firmware_dfu_params params = rig_params(&rig, false);
    (void)mesh_firmware_dfu_start(&rig.dfu, &params);
    rig_run(&rig, MESH_FIRMWARE_DFU_DONE, 120000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_FIRMWARE_DFU_DONE || !rig.dfu.radio_seen,
                              rig_close(&rig),
                              "the radio at its address is back, whichever services BlueZ lists");
    rig_close(&rig);
    record_success(test_name);
}

/* A broken transfer the bootloader still holds is not cleared with RESET: on a stock Adafruit
   bootloader with no application that comes back in USB DFU only, which is how a T1000-E was
   stranded. The install stops and says to use the cable. */
MESH_TEST_CASE(firmware_dfu_stale_session_asks_for_usb, unit) {
    static struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    rig.fake.connected_once = true;
    rig.fake.stale = true;
    rig.fake.stale_times = 99U;
    const struct mesh_firmware_dfu_params params = rig_params(&rig, false);
    (void)mesh_firmware_dfu_start(&rig.dfu, &params);
    rig_run(&rig, MESH_FIRMWARE_DFU_DONE, 180000U);
    MESH_TEST_FAIL_IF_CLEANUP(rig.dfu.state != MESH_FIRMWARE_DFU_FAILED ||
                                  strstr(rig.dfu.reason, "USB") == NULL,
                              rig_close(&rig), "a stale session ends the install, naming USB");
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.resets != 0U || rig.fake.starts != 1U, rig_close(&rig),
                              "with no RESET and no second START");
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_firmware_dfu_radio_in_loader(&rig.dfu), rig_close(&rig),
                              "and the radio is reported as left in its bootloader");
    rig_close(&rig);
    record_success(test_name);
}

MESH_TEST_CASE(firmware_dfu_refuses_a_damaged_package_before_the_radio, unit) {
    static struct fw_rig rig;
    MESH_TEST_FAIL_IF_CLEANUP(!rig_open(&rig, 0U), rig_close(&rig), "the rig should open");
    static struct zip_builder zip;
    make_package(&zip, k_manifest, true);
    FILE *const file = fopen(rig.package_path, "wb");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, rig_close(&rig), "rewrite the package");
    (void)fwrite(zip.bytes, 1U, zip.len, file);
    fclose(file);

    const struct mesh_firmware_dfu_params params = rig_params(&rig, true);
    const int started = mesh_firmware_dfu_start(&rig.dfu, &params);
    MESH_TEST_FAIL_IF_CLEANUP(started >= 0 || rig.dfu.error != MESH_FIRMWARE_DFU_ERROR_WRONG_IMAGE,
                              rig_close(&rig), "a package whose image fails its CRC16 is refused");
    MESH_TEST_FAIL_IF_CLEANUP(rig.fake.triggers != 0U || rig.done_calls != 0U, rig_close(&rig),
                              "before the radio is asked anything, and with no callback");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_dfu_radio_in_loader(&rig.dfu), rig_close(&rig),
                              "and the radio is not in its bootloader");
    rig_close(&rig);
    record_success(test_name);
}
