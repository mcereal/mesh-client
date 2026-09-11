#pragma once

/*
 * Reading the header of an ESP32 application image: the `.bin` the BLE path hands the loader.
 *
 * This is the ESP32 path's counterpart of `uf2.h`, and it exists for the same reason: the loader
 * flashes what it is given. It does check the bytes against the SHA-256 it was handed - but that
 * hash is one this client computed over this file, so it proves the file arrived intact and
 * nothing about whether it was ever meant for this chip. ESP-IDF's own image check runs in
 * `esp_ota_end()`, which is *after* the whole transfer: five minutes of streaming to learn the
 * file was for an ESP32-C3. The header answers that in 24 bytes.
 *
 * What it can answer, and only that: the magic, the chip the image was built for, and whether
 * the first segment carries an `esp_app_desc_t` (which every ESP-IDF application does, and a
 * bootloader or a partition table does not). It does **not** say which *board* the image is for
 * - a Heltec V3 image and a T-Beam S3 image are both ESP32-S3 applications - which is the
 * `.mt.json` cross-check's job, one layer up, exactly as on the UF2 path.
 *
 * Measured against `firmware-heltec-v3-2.7.26.54e0d8d.bin` on 2026-09-11: magic `0xE9`, seven
 * segments, chip id `0x0009`, and the app descriptor's magic word at offset 32. Its `version`
 * field says `esp-idf: v4.4.7 38eeba213a` and its project name `arduino-lib-builder`, so neither
 * names the Meshtastic release and neither is checked.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `esp_chip_id_t`, as ESP-IDF numbers them. 0 is a real chip here - the original ESP32 - so
   "no chip" is a separate answer from mesh_esp_chip_for_architecture() rather than a zero. */
#define MESH_ESP_CHIP_ESP32 0x0000U
#define MESH_ESP_CHIP_ESP32_S2 0x0002U
#define MESH_ESP_CHIP_ESP32_C3 0x0005U
#define MESH_ESP_CHIP_ESP32_S3 0x0009U
#define MESH_ESP_CHIP_ESP32_C6 0x000DU

/* The fixed header plus the first segment's header plus the app descriptor's magic word. */
#define MESH_ESP_IMAGE_MIN_LEN 36U

enum mesh_esp_image_verdict {
    MESH_ESP_IMAGE_OK = 0,
    MESH_ESP_IMAGE_TOO_SHORT,
    /* The first byte is not 0xE9: not an ESP32 image at all. */
    MESH_ESP_IMAGE_BAD_MAGIC,
    /* An image, for another chip. The one this check is for. */
    MESH_ESP_IMAGE_WRONG_CHIP,
    /* An image for this chip with no app descriptor where an application keeps one: a
       bootloader, or the `.factory.bin`, which starts with the bootloader. */
    MESH_ESP_IMAGE_NOT_AN_APP,
};

struct mesh_esp_image_info {
    uint16_t chip_id;
    uint8_t segments;
};

/*
 * Checks `bytes` is an ESP32 application image built for `expect_chip`.
 *
 * `info` may be NULL, and is filled in as far as the header could be read even on a refusal, so
 * the log can say which chip the wrong image was for.
 */
enum mesh_esp_image_verdict mesh_esp_image_validate(const uint8_t *bytes, size_t len,
                                                    uint16_t expect_chip,
                                                    struct mesh_esp_image_info *info);

/*
 * The chip for an architecture, in either of the spellings the two upstream documents use:
 * `deviceHardware` says `esp32-s3` and the release manifest `esp32s3`. False for anything that
 * is not an ESP32 - an nRF52, an RP2040 - which has no app image to check.
 */
bool mesh_esp_chip_for_architecture(const char *architecture, uint16_t *out_chip);

const char *mesh_esp_image_verdict_name(enum mesh_esp_image_verdict verdict);

#ifdef __cplusplus
}
#endif
