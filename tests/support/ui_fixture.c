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

/* Presses Down until the cursor is on `row`, then A. The cursor is left wherever the press
   put it, which for a list row is the top of what it opened. */
static bool settings_step_to(struct mesh_ui_store *store, uint32_t row) {
    struct mesh_ui_action action;
    while (store->nav.cursor[MESH_UI_SCREEN_SETTINGS] > row) {
        mesh_ui_store_handle_key(store, MESH_UI_KEY_UP, &action);
    }
    while (store->nav.cursor[MESH_UI_SCREEN_SETTINGS] < row) {
        const uint32_t before = store->nav.cursor[MESH_UI_SCREEN_SETTINGS];
        mesh_ui_store_handle_key(store, MESH_UI_KEY_DOWN, &action);
        if (store->nav.cursor[MESH_UI_SCREEN_SETTINGS] == before) {
            return false; /* the list is shorter than the row asked for */
        }
    }
    mesh_ui_store_handle_key(store, MESH_UI_KEY_A, &action);
    return true;
}

bool mesh_test_settings_open(struct mesh_ui_store *store, enum mesh_ui_settings_section section) {
    if (store->nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store->nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    if (mesh_ui_settings_section_is_module(section)) {
        if (!mesh_test_settings_open(store, MESH_UI_SETTINGS_MODULES)) {
            return false;
        }
        for (uint32_t i = 0; i < mesh_ui_settings_module_count(); ++i) {
            if (mesh_ui_settings_module_at(i) != section) {
                continue;
            }
            return settings_step_to(store, i) && store->nav.settings_section == (uint8_t)section;
        }
        return false;
    }
    for (uint32_t i = 0; i < mesh_ui_settings_root_count(); ++i) {
        if (mesh_ui_settings_root_at(i) != section) {
            continue;
        }
        return settings_step_to(store, i) && store->nav.settings_section == (uint8_t)section;
    }
    return false;
}
