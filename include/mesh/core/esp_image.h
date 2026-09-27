#pragma once

#include "inkwell/codec/esp_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves the architecture spellings from the device and release manifest to ESP chip IDs.
 * False means this application has no ESP image path for that architecture. */
bool mesh_esp_chip_for_architecture(const char *architecture, uint16_t *out_chip);
/* The other way: the architecture an image's header names, spelled as the catalog spells it
   ("esp32-s3"), or NULL for a chip this application does not know. */
const char *mesh_esp_architecture_for_chip(uint16_t chip);

/*
 * A whole-flash image - a `-merged.bin`, a `.factory.bin` - written at 0x0: the bootloader,
 * the partition table at 0x8000 and the application at its first app partition, with 0xFF
 * between them. It is what moves a board from one firmware to another, because the partition
 * table is the other firmware's too.
 *
 * What an install needs out of it: that each of the three is for this chip, and the data
 * partitions its table declares past the image's own end - which hold whatever the last
 * firmware kept there, and are erased so this one starts blank.
 */
#define MESH_ESP_PARTITION_TABLE_OFFSET 0x8000U
#define MESH_ESP_PARTITIONS_MAX 16U
/* 0x00 app, 0x01 data, as the table spells them. */
#define MESH_ESP_PARTITION_APP 0x00U
#define MESH_ESP_PARTITION_DATA 0x01U

struct mesh_esp_partition {
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    char label[17];
};

struct mesh_esp_whole_image {
    /* Where the application the bootloader will start sits in the image. */
    uint32_t app_offset;
    struct mesh_esp_partition partitions[MESH_ESP_PARTITIONS_MAX];
    size_t count;
};

/* Where `chip`'s ROM looks for its bootloader: 0x1000 on the ESP32 and S2, 0x0 after them. */
uint32_t mesh_esp_bootloader_offset(uint16_t chip);

/*
 * Reads `bytes` as a whole-flash image for `chip`. False when it is not one - no bootloader
 * for this chip where the ROM looks, no partition table, no app partition inside the image,
 * or an app that is not an application for this chip. `why` says which, for the log.
 */
bool mesh_esp_whole_image_read(const uint8_t *bytes, size_t len, uint16_t chip,
                               struct mesh_esp_whole_image *out, const char **why);

#ifdef __cplusplus
}
#endif
