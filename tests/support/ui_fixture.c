#define _POSIX_C_SOURCE 200809L

#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Builds a store with three devices (one connected), a synced handshake with three nodes (the
   first is us) and two messages: a broadcast from ALFA and a direct message from BRVO. */
void mesh_test_nav_populate(struct mesh_ui_store *store) {
    struct mesh_ui_device devices[3] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .connected = true},
        {.identifier = "AA:BB:CC:DD:EE:02", .name = "NodeTwo", .rssi = -60, .connected = false},
        {.identifier = "AA:BB:CC:DD:EE:03", .name = "NodeThree", .rssi = -70, .connected = false},
    };
    mesh_ui_store_set_discovery(store, devices, 3U);

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.config_complete = true;
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 3U;
    handshake.nodes[0].node_id = 0x1000U;
    snprintf(handshake.nodes[0].short_name, sizeof handshake.nodes[0].short_name, "%s", "ME");
    handshake.nodes[1].node_id = 0x2000U;
    snprintf(handshake.nodes[1].short_name, sizeof handshake.nodes[1].short_name, "%s", "ALFA");
    snprintf(handshake.nodes[1].long_name, sizeof handshake.nodes[1].long_name, "%s", "Alfa Node");
    handshake.nodes[2].node_id = 0x3000U;
    snprintf(handshake.nodes[2].short_name, sizeof handshake.nodes[2].short_name, "%s", "BRVO");
    mesh_ui_store_set_handshake(store, &handshake);

    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof messages);
    messages.count = 2U;
    messages.entries[0].packet_id = 11U;
    messages.entries[0].peer = 0x2000U;
    messages.entries[0].broadcast = true;
    messages.entries[0].direction = MESH_MESSAGE_INBOUND;
    snprintf(messages.entries[0].peer_name, sizeof messages.entries[0].peer_name, "%s", "ALFA");
    snprintf(messages.entries[0].text, sizeof messages.entries[0].text, "%s", "hello all");
    messages.entries[1].packet_id = 12U;
    messages.entries[1].peer = 0x3000U;
    messages.entries[1].broadcast = false;
    messages.entries[1].direction = MESH_MESSAGE_INBOUND;
    snprintf(messages.entries[1].peer_name, sizeof messages.entries[1].peer_name, "%s", "BRVO");
    snprintf(messages.entries[1].text, sizeof messages.entries[1].text, "%s", "just you");
    mesh_ui_store_set_messages(store, &messages);
}
