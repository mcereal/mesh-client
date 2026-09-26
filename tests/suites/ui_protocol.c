#define _POSIX_C_SOURCE 200809L

/*
 * What the screens offer for a protocol that is not Meshtastic.
 *
 * Every other UI case runs with a zeroed `protocol_lacks`, which is full Meshtastic - exactly
 * what the screens assumed before the bits existed, so those cases pass whether or not a gate is
 * there. These are the cases that fail when a Meshtastic-only verb is offered to a protocol that
 * has no counterpart for it, and each checks the Meshtastic half first so a fixture that shows
 * nothing at all cannot pass for a gate that works.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "inkcell/ui/input.h"
#include "mesh/core/meshcore.h"
#include "mesh/core/message.h"
#include "mesh/core/protocol.h"
#include "mesh/core/session.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/commands.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/protocols.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool has_verb(const struct mesh_ui_node_item *items, uint32_t count,
                     enum mesh_ui_node_action action) {
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].action == action) {
            return true;
        }
    }
    return false;
}

static bool section_offers(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           enum mesh_ui_settings_section section,
                           enum mesh_ui_settings_action which) {
    struct mesh_ui_settings_item items[64];
    const uint32_t count =
        mesh_ui_settings_items(settings, handshake, NULL, 0U, section, MESH_UI_SETTINGS_NO_CHANNEL,
                               items, (uint32_t)(sizeof items / sizeof items[0]));
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].kind == INKSTAND_FORM_ACTION && items[i].number == (uint32_t)which) {
            return true;
        }
    }
    return false;
}

static bool root_lists(const struct mesh_ui_settings *settings,
                       enum mesh_ui_settings_section section) {
    const uint32_t count = mesh_ui_settings_root_count(settings);
    for (uint32_t row = 0; row < count; ++row) {
        if (!mesh_ui_settings_root_is_heading(settings, row) &&
            mesh_ui_settings_root_at(settings, row) == section) {
            return true;
        }
    }
    return false;
}

/*
 * The table the publish reads. The Meshtastic session lacks nothing; a protocol the table has
 * never heard of lacks everything, because offering it a Meshtastic verb is offering a press
 * that fails; and nothing on the link reads as the Meshtastic this client has always assumed.
 */
MESH_TEST_CASE(ui_protocol_features_by_protocol, unit) {
    static struct mesh_session session;
    mesh_session_init(&session);
    const struct mesh_protocol meshtastic = mesh_session_protocol(&session);
    uint8_t id = 0xFFU;
    uint32_t lacks = 0xFFFFFFFFU;
    mesh_ui_protocol_features(&meshtastic, &id, &lacks);
    MESH_TEST_FAIL_IF(id != (uint8_t)MESH_UI_PROTOCOL_MESHTASTIC || lacks != 0U,
                      "the Meshtastic session has every feature");

    static const struct mesh_protocol_ops k_stranger = {.name = "stranger"};
    const struct mesh_protocol stranger = {&k_stranger, &session};
    mesh_ui_protocol_features(&stranger, &id, &lacks);
    MESH_TEST_FAIL_IF(id != (uint8_t)MESH_UI_PROTOCOL_OTHER || lacks != MESH_UI_FEATURES_ALL,
                      "a protocol with no row lacks every feature");

    /* MeshCore keeps text and the roster, and gives up every verb that is Meshtastic's alone. */
    static struct mesh_meshcore meshcore;
    mesh_meshcore_init(&meshcore, &session);
    const struct mesh_protocol companion = mesh_meshcore_protocol(&meshcore);
    mesh_ui_protocol_features(&companion, &id, &lacks);
    MESH_TEST_FAIL_IF(id != (uint8_t)MESH_UI_PROTOCOL_MESHCORE,
                      "MeshCore is published under its own name");
    MESH_TEST_FAIL_IF((lacks & MESH_UI_FEATURE_WAYPOINTS) == 0U ||
                          (lacks & MESH_UI_FEATURE_REMOTE_ADMIN) == 0U ||
                          (lacks & MESH_UI_FEATURE_CONTACT_LINKS) == 0U,
                      "MeshCore lacks waypoints, Meshtastic's admin and its links");

    const struct mesh_protocol none = {NULL, NULL};
    mesh_ui_protocol_features(&none, &id, &lacks);
    MESH_TEST_FAIL_IF(id != (uint8_t)MESH_UI_PROTOCOL_MESHTASTIC || lacks != 0U,
                      "nothing on the link is the Meshtastic the cache was written by");

    struct mesh_ui_settings zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    MESH_TEST_FAIL_IF(!mesh_ui_settings_supports(&zeroed, MESH_UI_FEATURE_TRACEROUTE) ||
                          !mesh_ui_settings_supports(NULL, MESH_UI_FEATURE_MODULES),
                      "a zeroed record - a cold start, an old cache - supports everything");
    zeroed.protocol_lacks = MESH_UI_FEATURE_TRACEROUTE;
    MESH_TEST_FAIL_IF(mesh_ui_settings_supports(&zeroed, MESH_UI_FEATURE_TRACEROUTE) ||
                          !mesh_ui_settings_supports(&zeroed, MESH_UI_FEATURE_WAYPOINTS),
                      "one lacked bit hides one feature");
    record_success(test_name);
}

/*
 * A node's sheet offers only what the protocol can carry. The node has everything a verb can be
 * gated on - a key, a fix, no place in the radio's list - so every Meshtastic verb is there, and
 * with every feature lacked only the two that belong to any mesh are left: write to it, and see
 * where it is.
 */
MESH_TEST_CASE(ui_protocol_node_sheet_offers_what_the_protocol_has, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x1234U;
    node.public_key_len = 32U;
    memset(node.public_key, 0x42, 32U);
    node.in_nodedb = false;
    node.position.valid = true;

    struct mesh_ui_node_item items[MESH_UI_NODE_ACTIONS_MAX];
    uint32_t count =
        mesh_ui_node_actions_build(&node, false, NULL, false, 0U, items, MESH_UI_NODE_ACTIONS_MAX);
    static const enum mesh_ui_node_action k_meshtastic_only[] = {
        MESH_UI_NODE_ACTION_FAVORITE,
        MESH_UI_NODE_ACTION_TRACEROUTE,
        MESH_UI_NODE_ACTION_REQUEST_INFO,
        MESH_UI_NODE_ACTION_REQUEST_POSITION,
        MESH_UI_NODE_ACTION_REQUEST_TELEMETRY,
        MESH_UI_NODE_ACTION_MUTE,
        MESH_UI_NODE_ACTION_IGNORE,
        MESH_UI_NODE_ACTION_REMOVE,
        MESH_UI_NODE_ACTION_ADD_CONTACT,
        MESH_UI_NODE_ACTION_VERIFY_KEY,
        MESH_UI_NODE_ACTION_ADMIN,
        MESH_UI_NODE_ACTION_WAYPOINT,
    };
    for (size_t i = 0; i < sizeof k_meshtastic_only / sizeof k_meshtastic_only[0]; ++i) {
        MESH_TEST_FAIL_IF(!has_verb(items, count, k_meshtastic_only[i]),
                          "Meshtastic offers every verb this node could take");
    }

    count = mesh_ui_node_actions_build(&node, false, NULL, false, MESH_UI_FEATURES_ALL, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(count != 2U || !has_verb(items, count, MESH_UI_NODE_ACTION_MESSAGE) ||
                          !has_verb(items, count, MESH_UI_NODE_ACTION_SHOW_ON_MAP),
                      "with every feature lacked, only message and show-on-map are left");
    MESH_TEST_FAIL_IF(count != mesh_ui_node_actions_count(&node, false, NULL, MESH_UI_FEATURES_ALL),
                      "the count the nav walks agrees with the built sheet");

    /* MeshCore removes a contact, and keeps no pin, mute or ignore to offer beside it. */
    static const struct mesh_protocol_ops k_meshcore = {.name = "meshcore"};
    static int meshcore_self;
    const struct mesh_protocol meshcore_protocol = {&k_meshcore, &meshcore_self};
    uint32_t meshcore_lacks = 0U;
    mesh_ui_protocol_features(&meshcore_protocol, NULL, &meshcore_lacks);
    count = mesh_ui_node_actions_build(&node, false, NULL, false, meshcore_lacks, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(!has_verb(items, count, MESH_UI_NODE_ACTION_REMOVE) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_FAVORITE) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_MUTE),
                      "MeshCore offers remove without the flags beside it");

    /* One bit, one verb: the gates are not one switch wearing several names. */
    count = mesh_ui_node_actions_build(&node, false, NULL, false, MESH_UI_FEATURE_TRACEROUTE, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(has_verb(items, count, MESH_UI_NODE_ACTION_TRACEROUTE) ||
                          !has_verb(items, count, MESH_UI_NODE_ACTION_ADMIN),
                      "lacking traceroute hides traceroute and nothing else");
    record_success(test_name);
}

/*
 * The settings a protocol cannot answer: the Modules list, the channel link and QR, the contact
 * link, and the radio's own firmware. Each is shown for Meshtastic from the same settings, then
 * missing with the bit set.
 */
MESH_TEST_CASE(ui_protocol_settings_hide_what_the_protocol_lacks, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.has_owner = true;
    snprintf(settings.contact_url, sizeof settings.contact_url, "%s", "https://example/v/#x");
    settings.admin_ok = true;
    settings.has_channels = true;
    settings.channels_settled = true;
    snprintf(settings.share_url, sizeof settings.share_url, "%s", "https://example/e/#x");
    settings.has_metadata = true;
    settings.fw_supported = true;

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;

    MESH_TEST_FAIL_IF(!root_lists(&settings, MESH_UI_SETTINGS_MODULES),
                      "Meshtastic lists its modules");
    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                      MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS) ||
                          !section_offers(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                          MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS),
                      "Meshtastic shares and imports channel links");
    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                      MESH_UI_SETTINGS_ACTION_SHARE_CONTACT) ||
                          !section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                          MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT),
                      "Meshtastic shares and imports contact links");
    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                      MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE),
                      "Meshtastic checks for the radio's firmware");
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                     MESH_UI_SETTINGS_ACTION_SEND_ADVERT),
                      "Meshtastic has no advert to send");
    const uint32_t meshtastic_root = mesh_ui_settings_root_count(&settings);

    /* Everything but the full configuration, whose own test is below: that bit takes every
       section but four, which would hide what this one is checking. */
    settings.protocol = (uint8_t)MESH_UI_PROTOCOL_OTHER;
    settings.protocol_lacks = MESH_UI_FEATURES_ALL & ~(uint32_t)MESH_UI_FEATURE_FULL_CONFIG;
    MESH_TEST_FAIL_IF(root_lists(&settings, MESH_UI_SETTINGS_MODULES) ||
                          mesh_ui_settings_root_count(&settings) != meshtastic_root - 1U,
                      "a protocol without modules has no Modules row, and only that row goes");
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                     MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS) ||
                          section_offers(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                         MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS),
                      "no channel links for a protocol without them");
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                     MESH_UI_SETTINGS_ACTION_SHARE_CONTACT) ||
                          section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                         MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT),
                      "no contact links for a protocol without them");
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                     MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE),
                      "no Meshtastic firmware check for a radio that does not run it");
    record_success(test_name);
}

/* Whether the bar the store would draw now puts React on X, for a protocol lacking `lacks`. The
   bar reads the snapshot's settings, which is what a publish would have carried there. */
static bool bar_offers_react(struct mesh_ui_store *store, uint32_t lacks) {
    static struct mesh_ui_snapshot snapshot;
    (void)mesh_ui_store_consume_updates(store, &snapshot);
    snapshot.settings.protocol_lacks = lacks;
    struct mesh_ui_command_set commands;
    mesh_ui_commands_for(&snapshot, &commands);
    return mesh_ui_commands_find(&commands, MESH_UI_COMMAND_REACT) != NULL;
}

/* X on a bubble opens the tapback picker for Meshtastic, and does nothing - and is not offered,
   because a keycap that does nothing is a bug - for a protocol without reactions. */
MESH_TEST_CASE(ui_protocol_reactions_follow_the_protocol, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    /* Into BRVO's conversation, whose one message is packet 12 (ui_nav_messaging.c). */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open) {
        failure = "the test needs BRVO's thread open";
        goto cleanup;
    }

    if (!bar_offers_react(&store, 0U)) {
        failure = "Meshtastic's bar offers React on a bubble";
        goto cleanup;
    }

    store.settings.protocol_lacks = MESH_UI_FEATURE_REACTIONS;
    if (bar_offers_react(&store, MESH_UI_FEATURE_REACTIONS)) {
        failure = "no React on the bar for a protocol without reactions";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (store.nav.reaction_open) {
        failure = "X opens no tapback picker for a protocol without reactions";
        goto cleanup;
    }

    store.settings.protocol_lacks = 0U;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (!store.nav.reaction_open) {
        failure = "and does for Meshtastic, from the same bubble";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Whether the bar the store would draw now puts Pin on X, for a protocol lacking `lacks`. */
static bool bar_offers_pin(struct mesh_ui_store *store, uint32_t lacks) {
    static struct mesh_ui_snapshot snapshot;
    (void)mesh_ui_store_consume_updates(store, &snapshot);
    snapshot.settings.protocol_lacks = lacks;
    struct mesh_ui_command_set commands;
    mesh_ui_commands_for(&snapshot, &commands);
    return mesh_ui_commands_find(&commands, MESH_UI_COMMAND_PIN) != NULL;
}

/*
 * X on the Nodes list is the one-press pin, the sheet's "Pinned to top" row without the
 * drill-down - and it is the radio's favourite flag, so it follows NODE_FLAGS exactly as the row
 * does. A shortcut left behind would be the same Meshtastic verb through a side door.
 */
MESH_TEST_CASE(ui_protocol_pin_shortcut_follows_the_protocol, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.nav.screen = MESH_UI_SCREEN_NODES;
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_LEAD_ROWS + 1U;
    if (!bar_offers_pin(&store, 0U)) {
        failure = "Meshtastic's Nodes list offers Pin on a node";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_FAVORITE) {
        failure = "X pins the node under the cursor for Meshtastic";
        goto cleanup;
    }

    store.settings.protocol_lacks = MESH_UI_FEATURE_NODE_FLAGS;
    if (bar_offers_pin(&store, MESH_UI_FEATURE_NODE_FLAGS)) {
        failure = "no Pin on the bar for a protocol without node flags";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type == MESH_UI_ACTION_TOGGLE_FAVORITE) {
        failure = "and X sends no favourite toggle for one";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* The counter under the draft is the cap the typing is held to, and it is the link's: a
   MeshCore channel message is shorter than a direct one, and both are shorter than Meshtastic's
   payload. Settings carry it into the nav, which is all the compose screen reads. */
MESH_TEST_CASE(ui_protocol_draft_cap_follows_the_link, unit) {
    static struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    store.nav.target_node = MESH_MESSAGE_BROADCAST_ADDR;
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_cap(&store.nav) != MESH_UI_DRAFT_MAX - 1U,
                      "Meshtastic's draft holds the whole payload");

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.protocol = (uint8_t)MESH_UI_PROTOCOL_MESHCORE;
    settings.direct_text_max = 160U;
    settings.channel_text_max = 150U;
    mesh_ui_store_set_settings(&store, &settings);
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_cap(&store.nav) != 150U,
                      "a channel message is held to the channel's limit");
    store.nav.target_node = 0x40414243U;
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_cap(&store.nav) != 160U, "a direct one to the node's");
    store.nav.keyboard_channel_url = true;
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_cap(&store.nav) != MESH_UI_DRAFT_MAX - 1U,
                      "a link being typed is not a message");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The Waypoints row stays - every row under it is counted from it - but on a protocol with no
   waypoints a press says so rather than opening a list whose only row makes a Meshtastic one. */
MESH_TEST_CASE(ui_protocol_waypoints_row_follows_the_protocol, unit) {
    const char *failure = NULL;
    static struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    store.nav.screen = MESH_UI_SCREEN_NODES;
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_WAYPOINTS_ROW;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.settings.protocol_lacks = MESH_UI_FEATURE_WAYPOINTS;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.waypoints_open) {
        failure = "a protocol without waypoints does not open the list";
        goto cleanup;
    }
    if (strcmp(store.nav.toast.text, inkcell_str(MESH_STR_TOAST_NO_WAYPOINTS)) != 0) {
        failure = "and the press says why";
        goto cleanup;
    }

    store.settings.protocol_lacks = 0U;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.waypoints_open) {
        failure = "Meshtastic opens it";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* The field on each row of a section, in order, up to `cap`. */
static size_t section_fields(const struct mesh_ui_settings *settings,
                             const struct mesh_ui_handshake_state *handshake,
                             enum mesh_ui_settings_section section, uint16_t *fields, size_t cap) {
    struct mesh_ui_settings_item items[64];
    const uint32_t count =
        mesh_ui_settings_items(settings, handshake, NULL, 0U, section, MESH_UI_SETTINGS_NO_CHANNEL,
                               items, (uint32_t)(sizeof items / sizeof items[0]));
    size_t n = 0U;
    for (uint32_t i = 0; i < count && n < cap; ++i) {
        fields[n++] = (uint16_t)items[i].field;
    }
    return n;
}

/*
 * MeshCore's settings are a name, four radio numbers, a power and a position, projected onto
 * Meshtastic's record. Without the full configuration Settings lists the four sections that
 * hold them, each cut to its rows - LoRa is the frequency itself rather than a region, a preset
 * and an override - and the Radio tab offers a reboot and nothing more destructive.
 */
MESH_TEST_CASE(ui_protocol_settings_follow_a_plain_configuration, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.has_owner = true;
    settings.has_lora = true;
    settings.has_position = true;
    settings.has_metadata = true;
    settings.protocol = (uint8_t)MESH_UI_PROTOCOL_MESHCORE;
    settings.protocol_lacks = MESH_UI_FEATURE_FULL_CONFIG | MESH_UI_FEATURE_RADIO_MAINTENANCE |
                              MESH_UI_FEATURE_MODULES | MESH_UI_FEATURE_CONTACT_LINKS |
                              MESH_UI_FEATURE_RADIO_FIRMWARE;
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.link_up = true;

    const enum mesh_ui_settings_section listed[] = {
        MESH_UI_SETTINGS_USER, MESH_UI_SETTINGS_POSITION, MESH_UI_SETTINGS_LORA,
        MESH_UI_SETTINGS_CHANNELS};
    for (size_t i = 0; i < sizeof listed / sizeof listed[0]; ++i) {
        MESH_TEST_FAIL_IF(!root_lists(&settings, listed[i]), "a core section is listed");
    }
    MESH_TEST_FAIL_IF(root_lists(&settings, MESH_UI_SETTINGS_DEVICE) ||
                          root_lists(&settings, MESH_UI_SETTINGS_BLUETOOTH) ||
                          root_lists(&settings, MESH_UI_SETTINGS_SECURITY),
                      "and Meshtastic's other sections are not");
    /* About, the heading and the four. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_root_count(&settings) != 6U, "nothing else is either");

    uint16_t fields[16];
    size_t n = section_fields(&settings, &handshake, MESH_UI_SETTINGS_LORA, fields, 16U);
    const uint16_t lora[] = {MESH_UI_FIELD_LORA_FREQUENCY, MESH_UI_FIELD_LORA_ANY_BANDWIDTH,
                             MESH_UI_FIELD_LORA_ANY_SPREAD, MESH_UI_FIELD_LORA_CODING,
                             MESH_UI_FIELD_LORA_ANY_TX_POWER};
    MESH_TEST_FAIL_IF(n != sizeof lora / sizeof lora[0] || memcmp(fields, lora, sizeof lora) != 0,
                      "LoRa is the frequency, the three numbers and the power");
    MESH_TEST_FAIL_IF(mesh_ui_settings_number_step(MESH_UI_FIELD_LORA_ANY_BANDWIDTH, 31U, -1) !=
                              20U ||
                          mesh_ui_settings_number_step(MESH_UI_FIELD_LORA_ANY_SPREAD, 7U, -1) != 6U,
                      "and its bandwidth and spread reach below Meshtastic's modem");
    settings.tx_power_max = 22U;
    struct mesh_ui_settings_item items[16];
    const uint32_t count =
        mesh_ui_settings_items(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, items, 16U);
    MESH_TEST_FAIL_IF(count != sizeof lora / sizeof lora[0] ||
                          items[count - 1U].ceiling != MESH_UI_ANY_TX_POWER_BIAS + 22U,
                      "and its power stops at what the radio says it can do");
    n = section_fields(&settings, &handshake, MESH_UI_SETTINGS_USER, fields, 16U);
    MESH_TEST_FAIL_IF(n != 1U || fields[0] != MESH_UI_FIELD_USER_LONG_NAME, "User is one name");
    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_POSITION,
                                      MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION),
                      "Position sets the coordinates it lists");
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_POSITION,
                                     MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION),
                      "with nothing advertised there is nothing to clear");
    settings.has_own_position = true;
    settings.own_latitude_i = 330000000;
    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_POSITION,
                                      MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION),
                      "and an advertised location can be cleared");

    /* Channels are listed, not opened: nothing could save an edit to one. */
    settings.has_channels = true;
    settings.channels[0].present = true;
    settings.channels[0].role = 1U;
    snprintf(settings.channels[0].name, sizeof settings.channels[0].name, "%s", "Public");
    settings.channels[1].present = true;
    settings.channels[1].index = 1U;
    struct mesh_ui_settings_item slots[16];
    const uint32_t slot_count =
        mesh_ui_settings_items(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, slots, 16U);
    MESH_TEST_FAIL_IF(slot_count < 1U || slots[0].kind != INKSTAND_FORM_INFO,
                      "a slot is a fact, not a row that opens the editor");
    for (uint32_t i = 1U; i < slot_count; ++i) {
        MESH_TEST_FAIL_IF(slots[i].kind == INKSTAND_FORM_ACTION && slots[i].number == 1U &&
                              slots[i].field == MESH_UI_FIELD_NONE,
                          "and an empty slot offers no set-up");
    }

    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                      MESH_UI_SETTINGS_ACTION_REBOOT),
                      "the radio can be rebooted");
    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                      MESH_UI_SETTINGS_ACTION_SEND_ADVERT) ||
                          !section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                          MESH_UI_SETTINGS_ACTION_SEND_FLOOD_ADVERT),
                      "and can advertise itself, nearby or across the mesh");
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                     MESH_UI_SETTINGS_ACTION_SHUTDOWN) ||
                          section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                         MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG) ||
                          section_offers(&settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS,
                                         MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE) ||
                          section_offers(&settings, &handshake, MESH_UI_SETTINGS_NODE_LISTS,
                                         MESH_UI_SETTINGS_ACTION_RESET_NODEDB),
                      "and nothing Meshtastic's admin verbs would do");

    char text[512];
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_ACTION_NONE, text,
                                  sizeof text);
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_LORA,
                                          MESH_UI_SETTINGS_ACTION_NONE, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, inkcell_str(MESH_STR_CONFIRM_TEXT_LORA_PLAIN)) != 0,
                      "the LoRa sheet promises no reboot and names no region");
    settings.protocol_lacks = 0U;
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_ACTION_NONE, text,
                                  sizeof text);
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_LORA,
                                          MESH_UI_SETTINGS_ACTION_NONE, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, inkcell_str(MESH_STR_CONFIRM_TEXT_LORA)) != 0,
                      "while Meshtastic's still warns of both");
    record_success(test_name);
}
