#pragma once

#include "mesh/core/protocol.h"
#include "mesh/ui/store_settings.h"

#include <stdbool.h>
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

/*
 * What a Tern node lacks, which unlike the other rows depends on the companion version both
 * ends speak - `version`, 0 before the node has said its - and on the node: turning it off is
 * version 9's, and its satellite receiver is a row from version 8 on a node that has one
 * (`has_receiver`). Everything else the `tern` row lacks, it lacks at every version.
 */
uint32_t mesh_ui_protocol_tern_lacks(uint8_t version, bool has_receiver);

#ifdef __cplusplus
}
#endif
