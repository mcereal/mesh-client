#include "mesh/core/esp_image.h"

#include <string.h>
#include <strings.h>

/* esp_image_header_t: magic, segment count, two flash bytes, entry point, then the pin fields
   and the chip id at offset 12 - little-endian, like everything the chip writes. */
#define ESP_IMAGE_MAGIC 0xE9U
#define ESP_IMAGE_SEGMENTS_OFFSET 1U
#define ESP_IMAGE_CHIP_OFFSET 12U
/* The fixed header is 24 bytes and the first segment's header 8 more, so an application's
   esp_app_desc_t - which ESP-IDF places at the very start of the first segment - begins at 32. */
#define ESP_APP_DESC_OFFSET 32U
#define ESP_APP_DESC_MAGIC 0xABCD5432UL

static const char *const k_verdict_names[] = {
    "ok", "too short", "bad magic", "wrong chip", "not an application",
};

const char *mesh_esp_image_verdict_name(enum mesh_esp_image_verdict verdict) {
    return (size_t)verdict < sizeof k_verdict_names / sizeof k_verdict_names[0]
               ? k_verdict_names[verdict]
               : "?";
}

enum mesh_esp_image_verdict mesh_esp_image_validate(const uint8_t *bytes, size_t len,
                                                    uint16_t expect_chip,
                                                    struct mesh_esp_image_info *info) {
    struct mesh_esp_image_info scratch;
    struct mesh_esp_image_info *const out = info != NULL ? info : &scratch;
    memset(out, 0, sizeof *out);

    if (bytes == NULL || len < MESH_ESP_IMAGE_MIN_LEN) {
        return MESH_ESP_IMAGE_TOO_SHORT;
    }
    if (bytes[0] != ESP_IMAGE_MAGIC) {
        return MESH_ESP_IMAGE_BAD_MAGIC;
    }
    out->segments = bytes[ESP_IMAGE_SEGMENTS_OFFSET];
    out->chip_id =
        (uint16_t)(bytes[ESP_IMAGE_CHIP_OFFSET] | (uint16_t)bytes[ESP_IMAGE_CHIP_OFFSET + 1U] << 8);
    if (out->chip_id != expect_chip) {
        return MESH_ESP_IMAGE_WRONG_CHIP;
    }
    const uint32_t desc_magic = (uint32_t)bytes[ESP_APP_DESC_OFFSET] |
                                (uint32_t)bytes[ESP_APP_DESC_OFFSET + 1U] << 8 |
                                (uint32_t)bytes[ESP_APP_DESC_OFFSET + 2U] << 16 |
                                (uint32_t)bytes[ESP_APP_DESC_OFFSET + 3U] << 24;
    if (desc_magic != ESP_APP_DESC_MAGIC) {
        return MESH_ESP_IMAGE_NOT_AN_APP;
    }
    return MESH_ESP_IMAGE_OK;
}

bool mesh_esp_chip_for_architecture(const char *architecture, uint16_t *out_chip) {
    if (architecture == NULL || out_chip == NULL) {
        return false;
    }
    /* Both spellings on purpose, because both are in circulation: see the three-spellings table
       in docs/radio-firmware-roadmap.md. Taking only one is the bug that passes every test run
       against an nRF52 and fails the first time somebody points it at an S3. */
    static const struct {
        const char *name;
        uint16_t chip;
    } k_chips[] = {
        {"esp32", MESH_ESP_CHIP_ESP32},      {"esp32-s2", MESH_ESP_CHIP_ESP32_S2},
        {"esp32s2", MESH_ESP_CHIP_ESP32_S2}, {"esp32-s3", MESH_ESP_CHIP_ESP32_S3},
        {"esp32s3", MESH_ESP_CHIP_ESP32_S3}, {"esp32-c3", MESH_ESP_CHIP_ESP32_C3},
        {"esp32c3", MESH_ESP_CHIP_ESP32_C3}, {"esp32-c6", MESH_ESP_CHIP_ESP32_C6},
        {"esp32c6", MESH_ESP_CHIP_ESP32_C6},
    };
    for (size_t i = 0; i < sizeof k_chips / sizeof k_chips[0]; ++i) {
        if (strcasecmp(architecture, k_chips[i].name) == 0) {
            *out_chip = k_chips[i].chip;
            return true;
        }
    }
    return false;
}
