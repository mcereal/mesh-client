#pragma once

#include "inkwell/codec/uf2.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maps the architecture values reported by the device to the family in its release image.
 * Zero means this application has no UF2 install path for that architecture. */
uint32_t mesh_uf2_family_for_architecture(const char *architecture);

#ifdef __cplusplus
}
#endif
