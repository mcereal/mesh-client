#include "mesh/ui/protocols.h"

#include "inkwell/base/array.h"

#include <string.h>

/* Everything but what is asked of a MeshCore repeater or room server: a login, a status, its
   neighbours and its commands; and a receiver row of its own, which Meshtastic's GPS row in
   Position is instead. */
#define MESHTASTIC_LACKS                                                                           \
    ((uint32_t)(MESH_UI_FEATURE_NODE_LOGIN | MESH_UI_FEATURE_NODE_STATUS |                         \
                MESH_UI_FEATURE_NODE_NEIGHBORS | MESH_UI_FEATURE_NODE_COMMANDS |                   \
                MESH_UI_FEATURE_NODE_PATH | MESH_UI_FEATURE_NODE_SHARE |                           \
                MESH_UI_FEATURE_RADIO_RECEIVER))

static const struct {
    const char *name; /* mesh_protocol_ops.name */
    enum mesh_ui_protocol protocol;
    uint32_t lacks;
} k_protocols[] = {
    {"meshtastic", MESH_UI_PROTOCOL_MESHTASTIC, MESHTASTIC_LACKS},
    /* Text, direct and on channels, over a roster of contacts. Everything below is a verb
       MeshCore's companion protocol either has no counterpart for or that this client does
       not speak yet: its remote admin is not Meshtastic's. Its traceroute is a path discovery,
       which names each repeater and reads no SNR. A login to a repeater, its status, its
       neighbours and a conversation that is its console are MeshCore's alone. Its contact and
       channel links are the MeshCore app's `meshcore://contact/add` and `meshcore://channel/add`,
       read and written by mesh/proto/meshcore_url.h - one channel to a link, so each channel shares
       its own. Its firmware is checked against MeshCore's own releases, not Meshtastic's.
     */
    {"meshcore", MESH_UI_PROTOCOL_MESHCORE,
     MESH_UI_FEATURE_WAYPOINTS | MESH_UI_FEATURE_NODE_REQUESTS | MESH_UI_FEATURE_NODE_FLAGS |
         MESH_UI_FEATURE_REMOTE_ADMIN | MESH_UI_FEATURE_KEY_VERIFICATION | MESH_UI_FEATURE_MODULES |
         MESH_UI_FEATURE_REACTIONS | MESH_UI_FEATURE_FULL_CONFIG |
         MESH_UI_FEATURE_RADIO_MAINTENANCE | MESH_UI_FEATURE_RADIO_OFF |
         MESH_UI_FEATURE_RADIO_RECEIVER},
    /* Direct messages to a contact or a neighbour, with their delivery state; a contact's
       position on the map, the node's own, and a node's presence card on its detail, which need
       no verb. Its companion protocol has groups, join codes and firmware updates, which this
       client speaks but no screen offers yet (docs/protocols.md), no channels, no contact link
       and none of the per-node verbs. What it has by version is mesh_ui_protocol_tern_lacks(). */
    {"tern", MESH_UI_PROTOCOL_TERN, MESH_UI_FEATURES_ALL},
};

/* The companion version each of a Tern node's features is had from. */
#define TERN_RECEIVER_SINCE 8U
#define TERN_OFF_SINCE 9U

uint32_t mesh_ui_protocol_tern_lacks(uint8_t version, bool has_receiver) {
    uint32_t lacks = MESH_UI_FEATURES_ALL;
    if (version >= TERN_RECEIVER_SINCE && has_receiver) {
        lacks &= ~(uint32_t)MESH_UI_FEATURE_RADIO_RECEIVER;
    }
    if (version >= TERN_OFF_SINCE) {
        lacks &= ~(uint32_t)MESH_UI_FEATURE_RADIO_OFF;
    }
    return lacks;
}

void mesh_ui_protocol_features(const struct mesh_protocol *protocol, uint8_t *out_protocol,
                               uint32_t *out_lacks) {
    uint8_t id = (uint8_t)MESH_UI_PROTOCOL_MESHTASTIC;
    uint32_t lacks = MESHTASTIC_LACKS;
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
