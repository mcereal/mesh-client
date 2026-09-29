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
#include "mesh/proto/meshcore_url.h"
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
 * The table the publish reads. The Meshtastic session lacks only the login a MeshCore repeater
 * asks for; a protocol the table has
 * never heard of lacks everything, because offering it a Meshtastic verb is offering a press
 * that fails; and nothing on the link reads as the Meshtastic this client has always assumed.
 */
MESH_TEST_CASE(ui_protocol_features_by_protocol, unit) {
    const uint32_t k_meshcore_only =
        (uint32_t)(MESH_UI_FEATURE_NODE_LOGIN | MESH_UI_FEATURE_NODE_STATUS |
                   MESH_UI_FEATURE_NODE_NEIGHBORS | MESH_UI_FEATURE_NODE_COMMANDS);
    static struct mesh_session session;
    mesh_session_init(&session);
    const struct mesh_protocol meshtastic = mesh_session_protocol(&session);
    uint8_t id = 0xFFU;
    uint32_t lacks = 0xFFFFFFFFU;
    mesh_ui_protocol_features(&meshtastic, &id, &lacks);
    MESH_TEST_FAIL_IF(id != (uint8_t)MESH_UI_PROTOCOL_MESHTASTIC || lacks != k_meshcore_only,
                      "the Meshtastic session has every feature but what MeshCore asks of a "
                      "repeater");

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
                          (lacks & MESH_UI_FEATURE_REMOTE_ADMIN) == 0U,
                      "MeshCore lacks waypoints and Meshtastic's admin");
    MESH_TEST_FAIL_IF(
        (lacks & MESH_UI_FEATURE_CONTACT_LINKS) != 0U ||
            (lacks & MESH_UI_FEATURE_CHANNEL_LINKS) != 0U ||
            (lacks & MESH_UI_FEATURE_TRACEROUTE) != 0U ||
            (lacks & MESH_UI_FEATURE_NODE_LOGIN) != 0U || (lacks & k_meshcore_only) != 0U,
        "and shares contacts and channels in its own app's links, traces a route, and "
        "logs in to a repeater, asks its status and neighbours, and sends it commands");

    const struct mesh_protocol none = {NULL, NULL};
    mesh_ui_protocol_features(&none, &id, &lacks);
    MESH_TEST_FAIL_IF(id != (uint8_t)MESH_UI_PROTOCOL_MESHTASTIC || lacks != k_meshcore_only,
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

    /* MeshCore removes and pins a contact, and keeps no mute or ignore to offer beside them. */
    static const struct mesh_protocol_ops k_meshcore = {.name = "meshcore"};
    static int meshcore_self;
    const struct mesh_protocol meshcore_protocol = {&k_meshcore, &meshcore_self};
    uint32_t meshcore_lacks = 0U;
    mesh_ui_protocol_features(&meshcore_protocol, NULL, &meshcore_lacks);
    count = mesh_ui_node_actions_build(&node, false, NULL, false, meshcore_lacks, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(has_verb(items, count, MESH_UI_NODE_ACTION_REMOVE) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_FAVORITE) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_REQUEST_TELEMETRY),
                      "a heard node the radio never added is no contact to remove, pin or ask");
    MESH_TEST_FAIL_IF(!has_verb(items, count, MESH_UI_NODE_ACTION_ADD_CONTACT),
                      "but one it can add");
    node.in_nodedb = true;
    count = mesh_ui_node_actions_build(&node, false, NULL, false, meshcore_lacks, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(!has_verb(items, count, MESH_UI_NODE_ACTION_REQUEST_TELEMETRY) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_REQUEST_POSITION) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_REQUEST_INFO),
                      "a MeshCore contact is asked for its readings, not Meshtastic's name or fix");
    MESH_TEST_FAIL_IF(!has_verb(items, count, MESH_UI_NODE_ACTION_REMOVE) ||
                          !has_verb(items, count, MESH_UI_NODE_ACTION_FAVORITE) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_MUTE) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_IGNORE),
                      "MeshCore offers remove and pin on a contact, without mute or ignore");
    MESH_TEST_FAIL_IF(has_verb(items, count, MESH_UI_NODE_ACTION_ADD_CONTACT),
                      "and a contact is not offered as one to add");
    node.public_key_len = 6U;
    count = mesh_ui_node_actions_build(&node, false, NULL, false, meshcore_lacks, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(has_verb(items, count, MESH_UI_NODE_ACTION_REMOVE) ||
                          has_verb(items, count, MESH_UI_NODE_ACTION_FAVORITE),
                      "nor is a sender known only by its key's prefix");
    node.public_key_len = 32U;
    node.in_nodedb = false;

    /* One bit, one verb: the gates are not one switch wearing several names. */
    count = mesh_ui_node_actions_build(&node, false, NULL, false, MESH_UI_FEATURE_TRACEROUTE, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(has_verb(items, count, MESH_UI_NODE_ACTION_TRACEROUTE) ||
                          !has_verb(items, count, MESH_UI_NODE_ACTION_ADMIN),
                      "lacking traceroute hides traceroute and nothing else");
    record_success(test_name);
}

/* Every row of a repeater's status fits the row it is written into, in every language the
   build ships, with every counter at its widest: a cut label or value says nothing on the frame
   and splits a UTF-8 character where it lands. */
MESH_TEST_CASE(ui_protocol_repeater_status_fits_its_rows, unit) {
    static const inkcell_str_id kLabels[] = {
        MESH_STR_NODE_HEAD_RELAY, MESH_STR_NODE_ACT_REQUEST_STATUS, MESH_STR_NODE_ACT_LOGIN,
        MESH_STR_NODE_UPTIME,     MESH_STR_NODE_PACKETS_RECV,       MESH_STR_NODE_PACKETS_SENT,
        MESH_STR_NODE_DUPLICATES, MESH_STR_NODE_NOISE_FLOOR,        MESH_STR_NODE_LAST_RX,
        MESH_STR_NODE_AIRTIME_TX, MESH_STR_NODE_AIRTIME_RX,         MESH_STR_NODE_TX_QUEUE,
        MESH_STR_NODE_ERRORS,     MESH_STR_NODE_RECV_ERRORS,        MESH_STR_NODE_POSTS,
        MESH_STR_NODE_REPORTED,
    };
    for (size_t locale = 0; locale < inkcell_i18n_locale_count(); ++locale) {
        const struct inkcell_i18n_locale *const which = inkcell_i18n_locale_at(locale);
        char reason[160];
        for (size_t i = 0; i < sizeof kLabels / sizeof kLabels[0]; ++i) {
            const char *const text = inkcell_str_in(which, kLabels[i]);
            if (strlen(text) >= MESH_UI_NODE_LABEL_MAX) {
                snprintf(reason, sizeof reason, "%s: %s is %u bytes, over the %u-byte row label",
                         which->id, inkcell_str_id_name(kLabels[i]), (unsigned)strlen(text),
                         (unsigned)MESH_UI_NODE_LABEL_MAX - 1U);
                MESH_TEST_FAIL_IF(true, reason);
            }
        }
        /* Formatted in that language, the way the row is: through the catalog's own call. */
        MESH_TEST_FAIL_IF(!inkcell_i18n_set_locale(which->id), "the locale can be chosen");
        char value[256];
        const int widths[] = {
            inkcell_str_format(value, sizeof value, MESH_STR_NODE_VAL_FLOOD_DIRECT, UINT32_MAX,
                               UINT32_MAX),
            inkcell_str_format(value, sizeof value, MESH_STR_NODE_VAL_RSSI_SNR, INT16_MIN,
                               (double)INT16_MIN / 4.0),
            inkcell_str_format(value, sizeof value, MESH_STR_NODE_VAL_RSSI, INT16_MIN),
            inkcell_str_format(value, sizeof value, MESH_STR_NODE_VAL_POSTS, (unsigned)UINT16_MAX,
                               (unsigned)UINT16_MAX),
            inkcell_str_format(value, sizeof value, MESH_STR_NODE_VAL_NUMBER, UINT32_MAX),
        };
        (void)inkcell_i18n_set_locale(inkcell_i18n_locale_english()->id);
        for (size_t i = 0; i < sizeof widths / sizeof widths[0]; ++i) {
            if (widths[i] < 0 || (size_t)widths[i] >= MESH_UI_NODE_VALUE_MAX) {
                snprintf(reason, sizeof reason, "%s: value %u is %d bytes at its widest, over %u",
                         which->id, (unsigned)i, widths[i], (unsigned)MESH_UI_NODE_VALUE_MAX - 1U);
                MESH_TEST_FAIL_IF(true, reason);
            }
        }
    }
    record_success(test_name);
}

/* A repeater's or room server's status is asked from the sheet where its login is, and what
   it answers is a group of its own on the detail - found by its label, not its position. */
MESH_TEST_CASE(ui_protocol_meshcore_asks_a_repeater_for_status, unit) {
    static const struct mesh_protocol_ops k_meshcore = {.name = "meshcore"};
    static int meshcore_self;
    const struct mesh_protocol meshcore_protocol = {&k_meshcore, &meshcore_self};
    uint32_t lacks = 0U;
    mesh_ui_protocol_features(&meshcore_protocol, NULL, &lacks);

    static struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x1234U;
    node.public_key_len = 32U;
    memset(node.public_key, 0x42, 32U);
    node.in_nodedb = true;
    node.role = 4U;
    MESH_TEST_FAIL_IF(!mesh_ui_node_statusable(&node, lacks), "a repeater contact");
    node.role = 12U;
    MESH_TEST_FAIL_IF(!mesh_ui_node_statusable(&node, lacks), "a room server contact");
    MESH_TEST_FAIL_IF(mesh_ui_node_statusable(&node, MESH_UI_FEATURE_NODE_STATUS),
                      "not on Meshtastic");
    node.role = 0U;
    MESH_TEST_FAIL_IF(mesh_ui_node_statusable(&node, lacks), "not a chat node");
    node.role = 4U;
    node.in_nodedb = false;
    MESH_TEST_FAIL_IF(mesh_ui_node_statusable(&node, lacks), "not a heard repeater");
    node.in_nodedb = true;

    /* Its neighbours are a repeater's alone: a room server keeps no list of what it hears. */
    MESH_TEST_FAIL_IF(!mesh_ui_node_neighbourable(&node, lacks), "a repeater contact's neighbours");
    MESH_TEST_FAIL_IF(mesh_ui_node_neighbourable(&node, MESH_UI_FEATURE_NODE_NEIGHBORS),
                      "not on Meshtastic");
    node.role = 12U;
    MESH_TEST_FAIL_IF(mesh_ui_node_neighbourable(&node, lacks), "not a room server's");
    node.role = 4U;
    node.in_nodedb = false;
    MESH_TEST_FAIL_IF(mesh_ui_node_neighbourable(&node, lacks), "not a heard repeater's");
    node.in_nodedb = true;

    static struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    uint32_t count = mesh_ui_node_actions_build(&node, false, NULL, false, lacks, items,
                                                MESH_UI_NODE_ACTIONS_MAX);
    bool offered = false;
    for (uint32_t i = 0; i < count; ++i) {
        offered = offered || items[i].action == MESH_UI_NODE_ACTION_REQUEST_STATUS;
    }
    MESH_TEST_FAIL_IF(!offered, "the repeater's sheet offers its status");
    /* The trace is MeshCore's path discovery, which the radio runs only to a contact. */
    bool traced = false;
    for (uint32_t i = 0; i < count; ++i) {
        traced = traced || items[i].action == MESH_UI_NODE_ACTION_TRACEROUTE;
    }
    MESH_TEST_FAIL_IF(!traced, "a contact's sheet offers a trace");
    node.in_nodedb = false;
    count = mesh_ui_node_actions_build(&node, false, NULL, false, lacks, items,
                                       MESH_UI_NODE_ACTIONS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(items[i].action == MESH_UI_NODE_ACTION_TRACEROUTE,
                          "a node the radio does not carry is not traced");
    }
    node.in_nodedb = true;

    const char *heading = inkcell_str(MESH_STR_NODE_HEAD_RELAY);
    count = mesh_ui_node_detail_build(&node, false, 1750000000U, NULL, NULL, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(strcmp(items[i].label, heading) == 0, "no status, no group");
    }
    node.relay.valid = true;
    node.relay.time = 1749999940U;
    node.relay.uptime_seconds = 3600U;
    node.relay.packets_recv = 700U;
    node.relay.recv_flood = 600U;
    node.relay.recv_direct = 100U;
    node.relay.noise_floor = -118;
    count = mesh_ui_node_detail_build(&node, false, 1750000000U, NULL, NULL, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    bool grouped = false;
    bool received = false;
    bool floor = false;
    for (uint32_t i = 0; i < count; ++i) {
        grouped = grouped || strcmp(items[i].label, heading) == 0;
        received =
            received || (strcmp(items[i].label, inkcell_str(MESH_STR_NODE_PACKETS_RECV)) == 0 &&
                         strcmp(items[i].value, "600 flood, 100 direct") == 0);
        floor = floor || (strcmp(items[i].label, inkcell_str(MESH_STR_NODE_NOISE_FLOOR)) == 0 &&
                          strcmp(items[i].value, "-118 dBm") == 0);
    }
    MESH_TEST_FAIL_IF(!grouped || !received || !floor,
                      "its answer is a group of its own, its counts split by routing");
    record_success(test_name);
}

/*
 * A login is offered on a MeshCore repeater or room server the radio carries by its whole key,
 * and nowhere else: not on a chat node, not on one only heard, and not on Meshtastic, whose
 * repeaters answer no such thing. Its row raises a keyboard whose Done sends what was typed -
 * blank included, which is a guest's login - and whose B sends nothing.
 */
MESH_TEST_CASE(ui_protocol_meshcore_logs_in_to_a_repeater, unit) {
    static const struct mesh_protocol_ops k_meshcore = {.name = "meshcore"};
    static int meshcore_self;
    const struct mesh_protocol meshcore_protocol = {&k_meshcore, &meshcore_self};
    uint32_t meshcore_lacks = 0U;
    mesh_ui_protocol_features(&meshcore_protocol, NULL, &meshcore_lacks);

    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x1234U;
    node.public_key_len = 32U;
    memset(node.public_key, 0x42, 32U);
    node.in_nodedb = true;
    node.role = 4U; /* a repeater's advert */
    MESH_TEST_FAIL_IF(!mesh_ui_node_loginable(&node, meshcore_lacks), "a repeater contact");
    node.role = 12U; /* a room server's */
    MESH_TEST_FAIL_IF(!mesh_ui_node_loginable(&node, meshcore_lacks), "a room server contact");
    MESH_TEST_FAIL_IF(mesh_ui_node_loginable(&node, MESH_UI_FEATURE_NODE_LOGIN),
                      "not on Meshtastic");
    node.role = 0U;
    MESH_TEST_FAIL_IF(mesh_ui_node_loginable(&node, meshcore_lacks), "not a chat node");
    node.role = 4U;
    node.in_nodedb = false;
    MESH_TEST_FAIL_IF(mesh_ui_node_loginable(&node, meshcore_lacks), "not a heard repeater");

    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    store.settings.protocol_lacks = meshcore_lacks;

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* Nodes */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    struct mesh_ui_node_summary *repeater = (struct mesh_ui_node_summary *)mesh_ui_node_detail_find(
        &store.handshake, store.nav.node_detail_node);
    if (!store.nav.node_detail_open || repeater == NULL) {
        failure = "A opens a node's detail";
        goto cleanup;
    }
    repeater->role = 4U;
    repeater->in_nodedb = true;
    repeater->public_key_len = 32U;
    memset(repeater->public_key, 0x42, 32U);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    struct mesh_ui_node_item items[MESH_UI_NODE_ACTIONS_MAX];
    const uint32_t count = mesh_ui_node_actions_build(repeater, false, NULL, false, meshcore_lacks,
                                                      items, MESH_UI_NODE_ACTIONS_MAX);
    uint32_t login_row = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].action == MESH_UI_NODE_ACTION_LOGIN) {
            login_row = i;
        }
    }
    if (!store.nav.node_actions_open || login_row >= count) {
        failure = "the repeater's sheet carries a login";
        goto cleanup;
    }
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "hi");
    for (int round = 0; round < 4; ++round) {
        while (store.nav.node_actions_cursor < login_row &&
               mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action)) {
        }
        memset(&action, 0, sizeof action);
        mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
        if (!store.nav.keyboard_open || !store.nav.keyboard_login ||
            store.nav.login_node != repeater->node_id || store.nav.draft[0] != '\0') {
            failure = "the row raises a blank keyboard for that node's password";
            goto cleanup;
        }
        if (round == 0) {
            mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* one character */
            char shown[MESH_UI_DRAFT_MAX];
            size_t caret_back = 0U;
            static struct mesh_ui_snapshot published;
            (void)mesh_ui_store_consume_updates(&store, &published);
            if (strcmp(published.nav.draft, "*") != 0) {
                failure = "a snapshot carries the password masked, never as typed";
                goto cleanup;
            }
            if (strcmp(mesh_ui_nav_kb_shown(&store.nav, shown, sizeof shown, &caret_back), "*") !=
                    0 ||
                caret_back != 0U) {
                failure = "a password is shown as one mark per character";
                goto cleanup;
            }
        }
        if (round == 3) {
            /* A pairing comparison arriving over it: the login gives way, password and all,
               and the six digits are shown as they are, since they are the thing to compare. */
            mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
            mesh_ui_store_open_passkey_prompt(&store, "Radio", 123456U, true);
            static struct mesh_ui_snapshot prompted;
            (void)mesh_ui_store_consume_updates(&store, &prompted);
            if (!store.nav.keyboard_passkey || store.nav.keyboard_login ||
                strcmp(prompted.nav.draft, "123456") != 0 ||
                strcmp(prompted.nav.draft_saved, "hi") != 0) {
                failure = "a pairing prompt closes the login and shows its digits unmasked";
                goto cleanup;
            }
            mesh_ui_store_close_passkey_prompt(&store);
            if (store.nav.keyboard_open || strcmp(store.nav.draft, "hi") != 0) {
                failure = "and nothing of the login comes back when it closes";
                goto cleanup;
            }
            memset(&action, 0, sizeof action);
            continue;
        }
        if (round == 2) {
            /* Longer than the draft it parked, so what B leaves is the test. */
            for (int typed = 0; typed < 15; ++typed) {
                mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
            }
            if (strlen(store.nav.draft) <= strlen("hi")) {
                failure = "fifteen characters were typed";
                goto cleanup;
            }
            mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
            if (action.type != MESH_UI_ACTION_NONE || store.nav.keyboard_open) {
                failure = "B leaves without logging in";
                goto cleanup;
            }
            const size_t parked = strlen("hi");
            for (size_t at = parked + 1U; at < sizeof store.nav.draft; ++at) {
                if (store.nav.draft[at] != '\0') {
                    failure = "nothing of the password is left past the restored draft";
                    goto cleanup;
                }
            }
        } else {
            mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
            if (action.type != MESH_UI_ACTION_LOGIN || action.dest != repeater->node_id ||
                (round == 0) != (action.text[0] != '\0') || store.nav.keyboard_open) {
                failure = round == 0 ? "Done logs in with what was typed"
                                     : "and with nothing typed, as a guest";
                goto cleanup;
            }
        }
        if (strcmp(store.nav.draft, "hi") != 0 || store.nav.login_node != 0U) {
            failure = "the password goes, and the draft it parked comes back";
            goto cleanup;
        }
    }
    /* The status row, just past the login, asks it of the same node. */
    while (store.nav.node_actions_cursor <= login_row &&
           mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action)) {
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (login_row + 1U >= count ||
        items[login_row + 1U].action != MESH_UI_NODE_ACTION_REQUEST_STATUS ||
        action.type != MESH_UI_ACTION_REQUEST_STATUS || action.dest != repeater->node_id) {
        failure = "the row after the login asks the repeater for its status";
        goto cleanup;
    }
    /* And the one after that, which nodes it hears. */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (login_row + 2U >= count ||
        items[login_row + 2U].action != MESH_UI_NODE_ACTION_REQUEST_NEIGHBORS ||
        action.type != MESH_UI_ACTION_REQUEST_NEIGHBORS || action.dest != repeater->node_id) {
        failure = "the row after the status asks the repeater for its neighbours";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A password shows one mark per character, and the caret stays where the next one goes -
   the start of the field included, and past a character wider than a byte. */
MESH_TEST_CASE(ui_protocol_password_mask_keeps_the_caret, unit) {
    static struct mesh_ui_nav nav;
    memset(&nav, 0, sizeof nav);
    nav.keyboard_open = true;
    nav.keyboard_login = true;
    snprintf(nav.draft, sizeof nav.draft, "%s",
             "\xC3\xA9"
             "a"); /* e-acute, then a */
    char shown[MESH_UI_DRAFT_MAX];
    static const struct {
        size_t draft_back;
        size_t shown_back;
    } k_cases[] = {{0U, 0U}, {1U, 1U}, {3U, 2U}};
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_used(&nav) != 3U, "the cap counts bytes, as typed");
    nav.login_draft_bytes = 3U;
    snprintf(nav.draft, sizeof nav.draft, "%s", "**"); /* as a snapshot publishes it */
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_used(&nav) != 3U,
                      "and a published mask still counts the bytes the password is");
    nav.login_draft_bytes = 0U;
    snprintf(nav.draft, sizeof nav.draft, "%s",
             "\xC3\xA9"
             "a");
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        size_t back = k_cases[i].draft_back;
        MESH_TEST_FAIL_IF(strcmp(mesh_ui_nav_kb_shown(&nav, shown, sizeof shown, &back), "**") != 0,
                          "two characters show as two marks");
        MESH_TEST_FAIL_IF(back != k_cases[i].shown_back,
                          "the caret is counted in marks: at the end, between, and at the start");
    }
    nav.keyboard_login = false;
    MESH_TEST_FAIL_IF(mesh_ui_nav_kb_shown(&nav, shown, sizeof shown, NULL) != nav.draft,
                      "any other keyboard shows its draft as typed");
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
 * drill-down - and it is the radio's favourite flag, so it follows mesh_ui_node_pinnable() exactly
 * as the row does. A shortcut left behind would be the same verb through a side door.
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

    store.settings.protocol_lacks = MESH_UI_FEATURE_NODE_PIN;
    if (bar_offers_pin(&store, MESH_UI_FEATURE_NODE_PIN)) {
        failure = "no Pin on the bar for a protocol without pinning";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type == MESH_UI_ACTION_TOGGLE_FAVORITE) {
        failure = "and X sends no favourite toggle for one";
        goto cleanup;
    }

    /* Without the per-node flags a pin is a contact's favourite: a node the radio does not
       carry by its whole key has none to set. */
    /* Nor on this radio's own detail, whose press refuses to pin ourselves. */
    {
        static struct mesh_ui_snapshot detail;
        (void)mesh_ui_store_consume_updates(&store, &detail);
        detail.handshake = store.handshake; /* consumed already, by the bar above */
        detail.handshake_valid = true;
        detail.settings.protocol_lacks = 0U;
        detail.nav.screen = MESH_UI_SCREEN_NODES;
        detail.nav.node_detail_open = true;
        detail.nav.node_detail_node = detail.handshake.my_info.node_num;
        struct mesh_ui_command_set commands;
        mesh_ui_commands_for(&detail, &commands);
        if (!detail.handshake.has_my_info ||
            mesh_ui_commands_find(&commands, MESH_UI_COMMAND_PIN) != NULL) {
            failure = "no Pin on this radio's own detail";
            goto cleanup;
        }
    }

    store.settings.protocol_lacks = MESH_UI_FEATURE_NODE_FLAGS;
    if (bar_offers_pin(&store, MESH_UI_FEATURE_NODE_FLAGS)) {
        failure = "no Pin on the bar over a node that is no whole-key contact";
        goto cleanup;
    }
    for (uint32_t i = 0; i < store.handshake.node_count; ++i) {
        if (mesh_ui_node_pinnable(&store.handshake.nodes[i], MESH_UI_FEATURE_NODE_FLAGS)) {
            failure = "no populated node is a whole-key contact";
            goto cleanup;
        }
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type == MESH_UI_ACTION_TOGGLE_FAVORITE) {
        failure = "X sends no favourite toggle for a node that is no contact";
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
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_cap(&store.nav) != MESH_UI_MESSAGE_TEXT_MAX - 1U,
                      "Meshtastic's draft holds the whole payload, and no more");

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
    store.nav.keyboard_channel_url = false;
    store.nav.keyboard_contact_url = true;
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_cap(&store.nav) <
                          strlen("meshcore://") + 2U * MESH_MESHCORE_CARD_MAX,
                      "and a contact link holds the longest MeshCore card");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* On a protocol with no waypoints, R2 on the Map tab says so rather than opening a list whose only
   row makes a Meshtastic one. */
MESH_TEST_CASE(ui_protocol_waypoints_row_follows_the_protocol, unit) {
    const char *failure = NULL;
    static struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    store.nav.screen = MESH_UI_SCREEN_MAP;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.settings.protocol_lacks = MESH_UI_FEATURE_WAYPOINTS;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R2, &action);
    if (store.nav.waypoints_open) {
        failure = "a protocol without waypoints does not open the list";
        goto cleanup;
    }
    if (strcmp(store.nav.toast.text, inkcell_str(MESH_STR_TOAST_NO_WAYPOINTS)) != 0) {
        failure = "and the press says why";
        goto cleanup;
    }

    store.settings.protocol_lacks = 0U;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R2, &action);
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

/* MeshCore shares a contact in its own app's link: Share once there is a link, Add once the
   radio has answered - no Meshtastic admin channel is waited on. */
MESH_TEST_CASE(ui_protocol_meshcore_offers_contact_links, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.has_owner = true;
    settings.protocol = (uint8_t)MESH_UI_PROTOCOL_MESHCORE;
    static const struct mesh_protocol_ops k_meshcore = {.name = "meshcore"};
    static int meshcore_self;
    const struct mesh_protocol protocol = {&k_meshcore, &meshcore_self};
    mesh_ui_protocol_features(&protocol, NULL, &settings.protocol_lacks);
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                     MESH_UI_SETTINGS_ACTION_SHARE_CONTACT) ||
                          section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                         MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT),
                      "nothing to share or add before the radio has answered");
    settings.has_meshcore_other = true;
    MESH_TEST_FAIL_IF(section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                     MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT),
                      "no Add while the handshake still walks the contacts");
    settings.meshcore_ready = true;
    snprintf(settings.contact_url, sizeof settings.contact_url, "%s",
             "meshcore://contact/add?name=MPBC&public_key="
             "ef490a40000000000000000000000000000000000000000000000000000000000&type=1");
    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                      MESH_UI_SETTINGS_ACTION_SHARE_CONTACT) ||
                          !section_offers(&settings, &handshake, MESH_UI_SETTINGS_USER,
                                          MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT),
                      "then this radio's link to show, and a stranger's to add");
    record_success(test_name);
}

/* A MeshCore link is one channel, so the share row is under each channel in use rather than at
   the head of the list; the import row stays at the head, and the set's share row does not
   appear. Pressing a channel's share row opens the share screen on that slot. */
MESH_TEST_CASE(ui_protocol_meshcore_shares_one_channel_at_a_time, unit) {
    static struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.protocol = (uint8_t)MESH_UI_PROTOCOL_MESHCORE;
    static const struct mesh_protocol_ops k_meshcore = {.name = "meshcore"};
    static int meshcore_self;
    const struct mesh_protocol meshcore_protocol = {&k_meshcore, &meshcore_self};
    uint32_t lacks = 0U;
    mesh_ui_protocol_features(&meshcore_protocol, NULL, &lacks);
    settings.protocol_lacks = lacks;
    settings.has_channels = true;
    settings.channels_settled = true;
    settings.channels[0].present = true;
    settings.channels[0].role = 1U;
    snprintf(settings.channels[0].name, sizeof settings.channels[0].name, "%s", "Public");
    settings.channels[0].psk_len = 16U;
    settings.channels[1].present = true; /* read, and unused */
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.link_up = true;

    MESH_TEST_FAIL_IF(!section_offers(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                      MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS) ||
                          section_offers(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                         MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS),
                      "the list offers an import and no set to share");
    static struct mesh_ui_settings_item items[64];
    bool shared[2] = {false, false};
    for (uint8_t slot = 0; slot < 2U; ++slot) {
        const uint32_t count = mesh_ui_settings_items(&settings, &handshake, NULL, 0U,
                                                      MESH_UI_SETTINGS_CHANNELS, slot, items, 64U);
        for (uint32_t i = 0; i < count; ++i) {
            shared[slot] = shared[slot] ||
                           (items[i].kind == INKSTAND_FORM_ACTION &&
                            items[i].number == (uint32_t)MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS);
        }
    }
    MESH_TEST_FAIL_IF(!shared[0] || shared[1], "a channel in use shares itself; an unused one not");
    record_success(test_name);
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
    /* The heading, Maps, Backups, Profiles, About and the four. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_root_count(&settings) != 9U, "nothing else is either");

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
    /* And, once SELF_INFO has said what they are, MeshCore's other parameters under it. */
    settings.has_meshcore_other = true;
    settings.meshcore_telemetry_modes = 0x26U; /* base 2, location 1, sensors 2 */
    settings.meshcore_manual_add = 1U;
    settings.meshcore_advert_loc_policy = 1U;
    n = section_fields(&settings, &handshake, MESH_UI_SETTINGS_USER, fields, 16U);
    /* The two headings are rows with no field. */
    const uint16_t user[] = {
        MESH_UI_FIELD_USER_LONG_NAME, MESH_UI_FIELD_NONE,         MESH_UI_FIELD_ADVERT_LOCATION,
        MESH_UI_FIELD_ASK_TELEMETRY,  MESH_UI_FIELD_ASK_LOCATION, MESH_UI_FIELD_ASK_SENSORS,
        MESH_UI_FIELD_NONE,           MESH_UI_FIELD_AUTO_ADD,     MESH_UI_FIELD_EXTRA_ACKS};
    MESH_TEST_FAIL_IF(n != sizeof user / sizeof user[0] || memcmp(fields, user, sizeof user) != 0,
                      "User adds what this radio shares and what it takes in");
    struct mesh_ui_settings_item user_rows[16];
    const uint32_t user_count =
        mesh_ui_settings_items(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_USER,
                               MESH_UI_SETTINGS_NO_CHANNEL, user_rows, 16U);
    uint32_t values[MESH_UI_FIELD_COUNT];
    memset(values, 0xff, sizeof values);
    for (uint32_t i = 0; i < user_count; ++i) {
        if (user_rows[i].field != MESH_UI_FIELD_NONE) {
            values[user_rows[i].field] = user_rows[i].number;
        }
    }
    MESH_TEST_FAIL_IF(
        values[MESH_UI_FIELD_ASK_TELEMETRY] != 2U || values[MESH_UI_FIELD_ASK_LOCATION] != 1U ||
            values[MESH_UI_FIELD_ASK_SENSORS] != 2U ||
            values[MESH_UI_FIELD_ADVERT_LOCATION] != 1U || values[MESH_UI_FIELD_AUTO_ADD] != 0U ||
            values[MESH_UI_FIELD_EXTRA_ACKS] != 0U,
        "each row reads its own bits, and auto-add is manual-add turned over");
    /* And once DEVICE_INFO has said what it is, the Bluetooth PIN under a heading of its own:
       fixed shows its six digits, random shows none. */
    settings.has_meshcore_pin = true;
    settings.meshcore_ble_pin = 482913U;
    n = section_fields(&settings, &handshake, MESH_UI_SETTINGS_USER, fields, 16U);
    MESH_TEST_FAIL_IF(n != sizeof user / sizeof user[0] + 3U ||
                          fields[n - 3U] != MESH_UI_FIELD_NONE ||
                          fields[n - 2U] != MESH_UI_FIELD_MESHCORE_PAIRING ||
                          fields[n - 1U] != MESH_UI_FIELD_MESHCORE_PIN,
                      "User ends with the Bluetooth PIN");
    uint32_t pin_count =
        mesh_ui_settings_items(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_USER,
                               MESH_UI_SETTINGS_NO_CHANNEL, user_rows, 16U);
    MESH_TEST_FAIL_IF(pin_count < 2U || user_rows[pin_count - 2U].number != 1U ||
                          strcmp(user_rows[pin_count - 1U].text, "482913") != 0,
                      "a fixed PIN is Fixed and its digits");
    settings.meshcore_ble_pin = 0U;
    pin_count = mesh_ui_settings_items(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_USER,
                                       MESH_UI_SETTINGS_NO_CHANNEL, user_rows, 16U);
    MESH_TEST_FAIL_IF(pin_count < 2U || user_rows[pin_count - 2U].number != 0U ||
                          user_rows[pin_count - 1U].text[0] != '\0',
                      "and a random one is Random with no digits to show");
    settings.has_meshcore_pin = false;
    settings.has_meshcore_other = false;
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

    /* A channel slot opens its editor, and the editor is a name and a 16-byte key: no role,
       no MQTT, no precision, no mute. Public is not cleared; a secondary slot can be. */
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
    MESH_TEST_FAIL_IF(slot_count < 2U || slots[0].kind != INKSTAND_FORM_ACTION ||
                          slots[1].kind != INKSTAND_FORM_ACTION || slots[1].number != 1U,
                      "a slot opens the editor, and an empty one offers set-up");
    struct mesh_ui_settings_item rows[16];
    uint32_t row_count = mesh_ui_settings_items(&settings, &handshake, NULL, 0U,
                                                MESH_UI_SETTINGS_CHANNELS, 0U, rows, 16U);
    MESH_TEST_FAIL_IF(row_count != 2U || rows[0].field != MESH_UI_FIELD_CHANNEL_ANY_NAME ||
                          rows[1].field != MESH_UI_FIELD_CHANNEL_ANY_KEY,
                      "the editor is a name and a key, and Public is not cleared");
    MESH_TEST_FAIL_IF(mesh_ui_settings_text_max(MESH_UI_FIELD_CHANNEL_ANY_NAME) != 31U ||
                          (mesh_ui_settings_key_choices(MESH_UI_FIELD_CHANNEL_ANY_KEY) &
                           (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256) |
                            MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_NONE))) != 0U ||
                          !mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_ANY_KEY, 16U) ||
                          mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_ANY_KEY, 32U),
                      "a name is 31 bytes and a key is 16, with no 256-bit or open choice");
    settings.channels[1].role = 2U;
    snprintf(settings.channels[1].name, sizeof settings.channels[1].name, "%s", "#test");
    row_count = mesh_ui_settings_items(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                                       1U, rows, 16U);
    MESH_TEST_FAIL_IF(row_count != 3U || rows[2].kind != INKSTAND_FORM_ACTION ||
                          rows[2].number != MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL,
                      "and a channel in a secondary slot can be cleared");

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
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_CHANNELS,
                                          MESH_UI_SETTINGS_ACTION_NONE, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, inkcell_str(MESH_STR_CONFIRM_TEXT_CHANNELS_PLAIN)) != 0,
                      "nor does the channel sheet");
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_CHANNELS,
                                          MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, inkcell_str(MESH_STR_CONFIRM_TEXT_CLEAR_PLAIN)) != 0,
                      "and a clear names no MQTT settings or role");
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_USER,
                                          MESH_UI_SETTINGS_ACTION_NONE, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, inkcell_str(MESH_STR_CONFIRM_TEXT_PLAIN)) != 0,
                      "and no other save promises a reboot");
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_RADIO_DETAILS, MESH_UI_SETTINGS_ACTION_REBOOT,
                                  text, sizeof text);
    char reboot[512];
    snprintf(reboot, sizeof reboot, "%s", text);
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_RADIO_DETAILS,
                                          MESH_UI_SETTINGS_ACTION_REBOOT, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, reboot) != 0, "while a verb's own sheet is left alone");
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_BACKUPS, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE,
                                  text, sizeof text);
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_BACKUPS,
                                          MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE, text,
                                          sizeof text);
    char expected[512];
    snprintf(expected, sizeof expected, "%s%s%s",
             inkcell_str(MESH_STR_CONFIRM_TEXT_BACKUPS_RESTORE_PLAIN),
             inkcell_str(MESH_STR_CONFIRM_TEXT_RESTORE_ROUTES_CLEAR),
             inkcell_str(MESH_STR_CONFIRM_TEXT_RESTORE_NEWER_KEEP));
    MESH_TEST_FAIL_IF(strcmp(text, expected) != 0,
                      "a restore promises no restart and says how its contacts go back");
    settings.backups.restore_keep_routes = true;
    settings.backups.restore_replace_newer = true;
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_BACKUPS,
                                          MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE, text,
                                          sizeof text);
    MESH_TEST_FAIL_IF(
        strstr(text, inkcell_str(MESH_STR_CONFIRM_TEXT_RESTORE_ROUTES_KEEP)) == NULL ||
            strstr(text, inkcell_str(MESH_STR_CONFIRM_TEXT_RESTORE_NEWER_REPLACE)) == NULL,
        "the sheet did not say the choices as they now stand");
    /* The overlay holds 256 bytes, and a backup from other firmware adds a sentence after:
       the whole sheet, with the longest firmware names the note takes, fits in every language. */
    static const char *const locales[] = {"en", "es"};
    const char *const version = "12345678901234567890";
    size_t longest = 0U;
    for (size_t l = 0; l < sizeof locales / sizeof locales[0]; ++l) {
        (void)inkcell_i18n_set_locale(locales[l]);
        char sheet[512];
        mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_BACKUPS,
                                              MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE, sheet,
                                              sizeof sheet);
        const size_t at = strlen(sheet);
        inkcell_str_format(sheet + at, sizeof sheet - at, MESH_STR_CONFIRM_TEXT_RESTORE_FIRMWARE,
                           version, version);
        longest = strlen(sheet) > longest ? strlen(sheet) : longest;
    }
    (void)inkcell_i18n_set_locale("en");
    MESH_TEST_FAIL_IF(longest >= 256U, "the restore sheet and its note do not fit the overlay");
    settings.protocol_lacks = 0U;
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_ACTION_NONE, text,
                                  sizeof text);
    mesh_ui_settings_confirm_for_protocol(&settings, MESH_UI_SETTINGS_LORA,
                                          MESH_UI_SETTINGS_ACTION_NONE, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, inkcell_str(MESH_STR_CONFIRM_TEXT_LORA)) != 0,
                      "while Meshtastic's still warns of both");
    record_success(test_name);
}
