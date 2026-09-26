#include "mesh/ui/protocols.h"

#include "inkwell/base/array.h"

#include <string.h>

static const struct {
    const char *name; /* mesh_protocol_ops.name */
    enum mesh_ui_protocol protocol;
    uint32_t lacks;
} k_protocols[] = {
    {"meshtastic", MESH_UI_PROTOCOL_MESHTASTIC, 0U},
};

void mesh_ui_protocol_features(const struct mesh_protocol *protocol, uint8_t *out_protocol,
                               uint32_t *out_lacks) {
    uint8_t id = (uint8_t)MESH_UI_PROTOCOL_MESHTASTIC;
    uint32_t lacks = 0U;
    if (mesh_protocol_bound(protocol)) {
        id = (uint8_t)MESH_UI_PROTOCOL_OTHER;
        lacks = MESH_UI_FEATURES_ALL;
        const char *name = mesh_protocol_name(protocol);
        for (size_t i = 0; i < INKWELL_ARRAY_LEN(k_protocols); ++i) {
            if (strcmp(k_protocols[i].name, name) == 0) {
                id = (uint8_t)k_protocols[i].protocol;
                lacks = k_protocols[i].lacks;
                break;
            }
        }
    }
    if (out_protocol != NULL) {
        *out_protocol = id;
    }
    if (out_lacks != NULL) {
        *out_lacks = lacks;
    }
}
