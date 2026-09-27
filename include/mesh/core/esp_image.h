#pragma once

#include "inkwell/codec/esp_image.h"

#include <stdbool.h>
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

#ifdef __cplusplus
}
#endif
