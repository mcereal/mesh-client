#pragma once

#include "mesh/core/protocol.h"
#include "mesh/ui/store_settings.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which of this client's features each mesh protocol has - the table the publish reads to fill
 * mesh_ui_settings.protocol and .protocol_lacks.
 *
 * A row per protocol, named by its ops table's name, so a second protocol is a row here and
 * every screen that asks mesh_ui_settings_supports() follows. A protocol with no row is one
 * this UI has never been told about: it is published as MESH_UI_PROTOCOL_OTHER lacking every
 * feature, because offering a Meshtastic verb to it would be offering a press that fails.
 *
 * Nothing on the link yet reads as Meshtastic, which is what this client has always assumed
 * before a radio answered - the cached roster and settings it starts from are Meshtastic's.
 */
void mesh_ui_protocol_features(const struct mesh_protocol *protocol, uint8_t *out_protocol,
                               uint32_t *out_lacks);

#ifdef __cplusplus
}
#endif
