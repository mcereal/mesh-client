#include "mesh/core/esp_image.h"

#include <stddef.h>
#include <string.h>
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

uint32_t mesh_esp_bootloader_offset(uint16_t chip) {
    return chip == INKWELL_ESP_CHIP_ESP32 || chip == INKWELL_ESP_CHIP_ESP32_S2 ? 0x1000U : 0x0U;
}

static uint32_t esp_le32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

/* The table's entries up to its end marker or its MD5 row, each 32 bytes. */
#define ESP_PARTITION_ENTRY 32U
#define ESP_PARTITION_TABLE_MAX 0xC00U

static bool esp_read_table(const uint8_t *bytes, size_t len, struct mesh_esp_whole_image *out) {
    const size_t start = MESH_ESP_PARTITION_TABLE_OFFSET;
    for (size_t at = start; at + ESP_PARTITION_ENTRY <= len && at < start + ESP_PARTITION_TABLE_MAX;
         at += ESP_PARTITION_ENTRY) {
        const uint8_t *const entry = bytes + at;
        if (entry[0] != 0xAAU || entry[1] != 0x50U) {
            /* 0xEBEB is the MD5 row and 0xFFFF blank: either ends the list. */
            break;
        }
        if (out->count >= MESH_ESP_PARTITIONS_MAX) {
            return false;
        }
        struct mesh_esp_partition *const partition = &out->partitions[out->count++];
        partition->type = entry[2];
        partition->subtype = entry[3];
        partition->offset = esp_le32(entry + 4);
        partition->size = esp_le32(entry + 8);
        memcpy(partition->label, entry + 12, 16U);
        partition->label[16] = '\0';
        if (partition->size == 0U || (uint64_t)partition->offset + partition->size > (1ULL << 31)) {
            return false;
        }
    }
    return out->count > 0U;
}

bool mesh_esp_whole_image_read(const uint8_t *bytes, size_t len, uint16_t chip,
                               struct mesh_esp_whole_image *out, const char **why) {
    const char *unused = NULL;
    why = why != NULL ? why : &unused;
    *why = "";
    if (bytes == NULL || out == NULL) {
        *why = "no image";
        return false;
    }
    memset(out, 0, sizeof *out);

    /* The bootloader is an image too, with no app descriptor: its header names the chip. */
    const uint32_t loader = mesh_esp_bootloader_offset(chip);
    struct inkwell_esp_image_info info;
    const enum inkwell_esp_image_verdict boot =
        loader < len ? inkwell_esp_image_validate(bytes + loader, len - loader, chip, &info)
                     : INKWELL_ESP_IMAGE_TOO_SHORT;
    if (boot != INKWELL_ESP_IMAGE_OK && boot != INKWELL_ESP_IMAGE_NOT_AN_APP) {
        *why = boot == INKWELL_ESP_IMAGE_WRONG_CHIP ? "a bootloader for another chip"
                                                    : "no bootloader where the ROM looks";
        return false;
    }
    if (!esp_read_table(bytes, len, out)) {
        *why = "no partition table at 0x8000";
        return false;
    }
    /* The lowest app partition is the one blank otadata boots: ota_0, or factory. */
    bool found = false;
    for (size_t i = 0; i < out->count; ++i) {
        const struct mesh_esp_partition *const partition = &out->partitions[i];
        if (partition->type == MESH_ESP_PARTITION_APP &&
            (!found || partition->offset < out->app_offset)) {
            out->app_offset = partition->offset;
            found = true;
        }
    }
    if (!found || out->app_offset >= len) {
        *why = "no application inside the image";
        return false;
    }
    if (inkwell_esp_image_validate(bytes + out->app_offset, len - out->app_offset, chip, &info) !=
        INKWELL_ESP_IMAGE_OK) {
        *why = "the application is not one for this chip";
        return false;
    }
    return true;
}
