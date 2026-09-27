#include "mesh/core/esp_image.h"

#include <stddef.h>
#include <strings.h>

/* Both spellings on purpose, because both are in circulation. Taking only one is the bug that
   passes every test run against an nRF52 and fails the first time somebody points it at an S3.
   The hyphenated one comes first, and is the one a chip is named back by. */
static const struct {
    const char *name;
    uint16_t chip;
} k_chips[] = {
    {"esp32", INKWELL_ESP_CHIP_ESP32},      {"esp32-s2", INKWELL_ESP_CHIP_ESP32_S2},
    {"esp32s2", INKWELL_ESP_CHIP_ESP32_S2}, {"esp32-s3", INKWELL_ESP_CHIP_ESP32_S3},
    {"esp32s3", INKWELL_ESP_CHIP_ESP32_S3}, {"esp32-c3", INKWELL_ESP_CHIP_ESP32_C3},
    {"esp32c3", INKWELL_ESP_CHIP_ESP32_C3}, {"esp32-c6", INKWELL_ESP_CHIP_ESP32_C6},
    {"esp32c6", INKWELL_ESP_CHIP_ESP32_C6},
};

bool mesh_esp_chip_for_architecture(const char *architecture, uint16_t *out_chip) {
    if (architecture == NULL || out_chip == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof k_chips / sizeof k_chips[0]; ++i) {
        if (strcasecmp(architecture, k_chips[i].name) == 0) {
            *out_chip = k_chips[i].chip;
            return true;
        }
    }
    return false;
}

const char *mesh_esp_architecture_for_chip(uint16_t chip) {
    for (size_t i = 0; i < sizeof k_chips / sizeof k_chips[0]; ++i) {
        if (k_chips[i].chip == chip) {
            return k_chips[i].name;
        }
    }
    return NULL;
}
