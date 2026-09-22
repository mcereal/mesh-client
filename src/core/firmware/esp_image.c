#include "mesh/core/esp_image.h"

#include <stddef.h>
#include <strings.h>

bool mesh_esp_chip_for_architecture(const char *architecture, uint16_t *out_chip) {
    if (architecture == NULL || out_chip == NULL) {
        return false;
    }
    /* Both spellings on purpose, because both are in circulation. Taking only one is the bug that
       passes every test run against an nRF52 and fails the first time somebody points it at an
       S3. */
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
    for (size_t i = 0; i < sizeof k_chips / sizeof k_chips[0]; ++i) {
        if (strcasecmp(architecture, k_chips[i].name) == 0) {
            *out_chip = k_chips[i].chip;
            return true;
        }
    }
    return false;
}
