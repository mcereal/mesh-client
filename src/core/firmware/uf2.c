#include "mesh/core/uf2.h"

#include <stddef.h>
#include <string.h>

/* The RP2350 release uses the ARM secure family, rather than the other published variants. */
static const struct {
    const char *architecture;
    uint32_t family;
} k_families[] = {
    {"nrf52840", INKWELL_UF2_FAMILY_NRF52840},
    {"rp2040", INKWELL_UF2_FAMILY_RP2040},
    {"rp2350", INKWELL_UF2_FAMILY_RP2350},
};

uint32_t mesh_uf2_family_for_architecture(const char *architecture) {
    if (architecture == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < sizeof k_families / sizeof k_families[0]; ++i) {
        if (strcmp(k_families[i].architecture, architecture) == 0) {
            return k_families[i].family;
        }
    }
    return 0U;
}
