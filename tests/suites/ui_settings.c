#define _POSIX_C_SOURCE 200809L

/* The settings model itself: rows, edits, key text, coordinates, About. */

#include "framework/mesh_test.h"

#include "mesh/core/updater.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include "meshtastic/config.pb.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

MESH_TEST_CASE(ui_settings_items, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.use_preset = true;
    settings.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    settings.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE;
    settings.hop_limit = 3U;
    settings.tx_enabled = true;
    settings.has_device = true;
    settings.role = meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
    settings.has_security = true;
    settings.public_key_len = 32U;
    settings.public_key[0] = 0xDEU;
    settings.public_key[1] = 0xADU;
    settings.public_key[2] = 0xBEU;
    settings.public_key[3] = 0xEFU;

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.channel_count = 2U;
    handshake.channels[0].index = 0U;
    handshake.channels[0].role = 1U;
    handshake.channels[0].psk_len = 1U;
    handshake.channels[1].index = 1U;
    handshake.channels[1].role = 2U;
    handshake.channels[1].psk_len = 16U;
    handshake.channels[1].uplink_enabled = true;
    snprintf(handshake.channels[1].name, sizeof handshake.channels[1].name, "%s", "Team");

    if (!mesh_ui_settings_section_loaded(&settings, &handshake, MESH_UI_SETTINGS_LORA) ||
        mesh_ui_settings_section_loaded(&settings, &handshake, MESH_UI_SETTINGS_DISPLAY) ||
        mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_DISPLAY,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 0U) {
        record_failure(test_name, "section loaded flags are wrong");
        return;
    }

    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        strcmp(item.label, "Region") != 0 || strcmp(item.value, "US") != 0 ||
        item.kind != MESH_UI_SETTING_ENUM) {
        record_failure(test_name, "LoRa region row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        strcmp(item.label, "Preset") != 0 || strcmp(item.value, "Long Range - Moderate") != 0) {
        record_failure(test_name, "LoRa preset row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_DEVICE,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        strcmp(item.value, "Router Late") != 0) {
        record_failure(test_name, "device role row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.kind != MESH_UI_SETTING_KEY || strncmp(item.value, "deadbeef...", 11U) != 0 ||
        strstr(item.value, "32 bytes") == NULL) {
        record_failure(test_name, "public key fingerprint is wrong");
        return;
    }
    if (mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 2U ||
        !mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        strcmp(item.label, "1 Team") != 0 || strstr(item.value, "AES-128") == NULL ||
        strstr(item.value, "up on") == NULL || strstr(item.value, "down off") == NULL) {
        record_failure(test_name, "channel row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        strcmp(item.label, "0 Primary") != 0 || strstr(item.value, "default key") == NULL) {
        record_failure(test_name, "primary channel row is wrong");
        return;
    }
    if (mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                              MESH_UI_SETTINGS_NO_CHANNEL, 99U, &item)) {
        record_failure(test_name, "out-of-range row should fail");
        return;
    }
    record_success(test_name);
}

/* Editable rows: the field table, pending edits rendered in place, and the steppers. */
MESH_TEST_CASE(ui_settings_edits, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_owner = true;
    snprintf(settings.long_name, sizeof settings.long_name, "%s", "Meshtastic 0ad8");
    snprintf(settings.short_name, sizeof settings.short_name, "%s", "0ad8");
    settings.has_display = true;
    settings.screen_on_secs = 60U;
    settings.compass_orientation = 7U;
    settings.units = 1U;
    settings.has_telemetry = true;
    settings.device_update_interval = 1234U; /* not a preset */

    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_USER,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.field != MESH_UI_FIELD_USER_LONG_NAME || item.kind != MESH_UI_SETTING_TEXT ||
        item.dirty || strcmp(item.text, "Meshtastic 0ad8") != 0 ||
        strcmp(item.value, "Meshtastic 0ad8") != 0) {
        record_failure(test_name, "long name row is wrong");
        return;
    }
    if (mesh_ui_settings_text_max(MESH_UI_FIELD_USER_LONG_NAME) != 24U ||
        mesh_ui_settings_text_max(MESH_UI_FIELD_USER_SHORT_NAME) != 4U ||
        mesh_ui_settings_text_max(MESH_UI_FIELD_DISPLAY_12H) != 0U) {
        record_failure(test_name, "text caps are wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.field != MESH_UI_FIELD_DISPLAY_SCREEN_ON || item.kind != MESH_UI_SETTING_NUMBER ||
        item.number != 60U || strcmp(item.value, "1m") != 0) {
        record_failure(test_name, "screen-on row is wrong");
        return;
    }
    if (mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 60U, +1) != 120U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 60U, -1) != 30U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 3600U, +1) != 3600U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 0U, -1) != 0U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_TELEMETRY_INTERVAL, 1234U, +1) != 1800U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_TELEMETRY_INTERVAL, 1234U, -1) != 900U) {
        record_failure(test_name, "number presets step wrong");
        return;
    }
    if (mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_COMPASS) != 8U ||
        mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_UNITS) != 2U ||
        mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_FLIP) != 0U ||
        strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_DISPLAY_UNITS, 1U), "Imperial") != 0 ||
        strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_DISPLAY_COMPASS, 7U), "270 flip") != 0) {
        record_failure(test_name, "enum tables are wrong");
        return;
    }
    if (mesh_ui_settings_field_section(MESH_UI_FIELD_SF_SERVER) != MESH_UI_SETTINGS_STORE_FORWARD ||
        mesh_ui_settings_field_kind(MESH_UI_FIELD_TELEMETRY_INTERVAL) != MESH_UI_SETTING_NUMBER ||
        strcmp(mesh_ui_settings_field_label(MESH_UI_FIELD_USER_SHORT_NAME), "Short name") != 0) {
        record_failure(test_name, "field descriptions are wrong");
        return;
    }
    /* A read-only row has no field. */
    if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_TELEMETRY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        item.field != MESH_UI_FIELD_TELEMETRY_INTERVAL || strcmp(item.value, "1234s") != 0) {
        record_failure(test_name, "telemetry interval row is wrong");
        return;
    }

    /* Pending edits show in place, marked. */
    struct mesh_ui_setting_edit edits[2];
    memset(edits, 0, sizeof edits);
    edits[0].field = MESH_UI_FIELD_DISPLAY_UNITS;
    edits[0].number = 0U;
    edits[1].field = MESH_UI_FIELD_USER_SHORT_NAME;
    snprintf(edits[1].text, sizeof edits[1].text, "%s", "BRCK");
    if (!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
        item.field != MESH_UI_FIELD_DISPLAY_UNITS || !item.dirty || item.number != 0U ||
        strcmp(item.value, "Metric") != 0) {
        record_failure(test_name, "an enum edit should render in place");
        return;
    }
    if (!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_USER,
                               MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        !item.dirty || strcmp(item.text, "BRCK") != 0 || strcmp(item.value, "BRCK") != 0) {
        record_failure(test_name, "a text edit should render in place");
        return;
    }
    if (!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.dirty || item.number != 60U) {
        record_failure(test_name, "rows without an edit stay clean");
        return;
    }
    if (mesh_ui_settings_find_edit(edits, 2U, MESH_UI_FIELD_USER_SHORT_NAME) != &edits[1] ||
        mesh_ui_settings_find_edit(edits, 2U, MESH_UI_FIELD_DISPLAY_FLIP) != NULL) {
        record_failure(test_name, "find_edit is wrong");
        return;
    }
    record_success(test_name);
}

/* Keys as text: base64 out, base64 or hex in, per-field lengths and choices. */
MESH_TEST_CASE(ui_settings_key_text, unit) {
    static const uint8_t k_default[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                          0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    char text[64];
    mesh_ui_settings_key_text(k_default, sizeof k_default, text, sizeof text);
    if (strcmp(text, "1PG7OiApB1nwvP+rz05pAQ==") != 0) {
        record_failure(test_name, "base64 of the default key is wrong");
        return;
    }
    uint8_t parsed[32];
    size_t len = 0U;
    if (!mesh_ui_settings_key_parse("1PG7OiApB1nwvP+rz05pAQ==", parsed, sizeof parsed, &len) ||
        len != 16U || memcmp(parsed, k_default, 16U) != 0) {
        record_failure(test_name, "base64 should parse back");
        return;
    }
    if (!mesh_ui_settings_key_parse("d4f1bb3a20290759f0bcffabcf4e6901", parsed, sizeof parsed,
                                    &len) ||
        len != 16U || memcmp(parsed, k_default, 16U) != 0) {
        record_failure(test_name, "hex should parse too");
        return;
    }
    if (!mesh_ui_settings_key_parse("AQ==", parsed, sizeof parsed, &len) || len != 1U ||
        parsed[0] != 1U || !mesh_ui_settings_key_parse("", parsed, sizeof parsed, &len) ||
        len != 0U) {
        record_failure(test_name, "one-byte and empty keys should parse");
        return;
    }
    if (mesh_ui_settings_key_parse("1PG7OiApB1nwvP+rz05pAQ=", parsed, sizeof parsed, &len) ||
        mesh_ui_settings_key_parse("1PG7Oi=pB1nwvP+rz05pAQ==", parsed, sizeof parsed, &len) ||
        mesh_ui_settings_key_parse("not a key!", parsed, sizeof parsed, &len) ||
        mesh_ui_settings_key_parse("abc", parsed, sizeof parsed, &len)) {
        record_failure(test_name, "bad text must be refused");
        return;
    }
    uint8_t all[32];
    for (unsigned i = 0; i < 32U; ++i) {
        all[i] = (uint8_t)(i * 7U);
    }
    mesh_ui_settings_key_text(all, 32U, text, sizeof text);
    if (strlen(text) != 44U || !mesh_ui_settings_key_parse(text, parsed, sizeof parsed, &len) ||
        len != 32U || memcmp(parsed, all, 32U) != 0) {
        record_failure(test_name, "a 32-byte key should round-trip");
        return;
    }
    if (!mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_KEY, 1U) ||
        mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_KEY, 8U) ||
        !mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_PRIVATE_KEY, 32U) ||
        mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_PRIVATE_KEY, 16U) ||
        !mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_ADMIN_KEY_1, 0U) ||
        mesh_ui_settings_key_len_ok(MESH_UI_FIELD_DISPLAY_FLIP, 0U)) {
        record_failure(test_name, "key length rules are wrong");
        return;
    }
    if (mesh_ui_settings_key_choices(MESH_UI_FIELD_SECURITY_PRIVATE_KEY) !=
            (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) |
             MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256)) ||
        (mesh_ui_settings_key_choices(MESH_UI_FIELD_CHANNEL_KEY) &
         MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_DEFAULT)) == 0U ||
        mesh_ui_settings_key_choices(MESH_UI_FIELD_LORA_HOPS) != 0U) {
        record_failure(test_name, "key choices are wrong");
        return;
    }
    record_success(test_name);
}

/* The Nodes tab's detail rows: which ones a node produces, and that the count the nav walks
   agrees with the list the backend draws. */
MESH_TEST_CASE(ui_node_detail_items, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x5001U;
    snprintf(node.long_name, sizeof node.long_name, "Weather Hut");
    snprintf(node.short_name, sizeof node.short_name, "WX");
    node.last_heard = 1750000000U;
    node.snr = -4.5f;

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, false, items,
                                               MESH_UI_NODE_ITEMS_MAX);
    if (count != mesh_ui_node_detail_count(&node, false, NULL)) {
        record_failure(test_name, "the count the nav walks disagrees with the built list");
        return;
    }
    if (count == 0U || items[0].kind != MESH_UI_NODE_ROW_ACTION ||
        items[0].action != MESH_UI_NODE_ACTION_MESSAGE) {
        record_failure(test_name, "the first row should be the message action");
        return;
    }

    /* A bare node has no metrics, position or environment to show. */
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Position") == 0 || strcmp(items[i].label, "Environment") == 0 ||
            strcmp(items[i].label, "Device metrics") == 0) {
            record_failure(test_name, "a bare node should not show an empty section");
            return;
        }
    }
    /* But it does say when we last heard it, in the same shorthand the list uses. */
    bool saw_age = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Last heard") == 0 && strcmp(items[i].value, "10m ago") == 0) {
            saw_age = true;
        }
    }
    if (!saw_age) {
        record_failure(test_name, "the age of the last packet should be a row");
        return;
    }

    /* Our own node cannot be messaged and its SNR against itself means nothing. */
    const uint32_t self_count = mesh_ui_node_detail_count(&node, true, NULL);
    struct mesh_ui_node_item self_items[MESH_UI_NODE_ITEMS_MAX];
    mesh_ui_node_detail_build(&node, true, 1750000600U, NULL, false, self_items,
                              MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < self_count; ++i) {
        if (self_items[i].kind == MESH_UI_NODE_ROW_ACTION ||
            strcmp(self_items[i].label, "SNR") == 0) {
            record_failure(test_name, "our own node should offer no message row and no SNR");
            return;
        }
    }

    /* With readings, each section appears and each value is formatted for the screen. */
    node.metrics.valid = true;
    node.metrics.time = 1750000000U;
    node.metrics.has_battery = true;
    node.metrics.battery_level = 101U; /* upstream's "plugged in" */
    node.position.valid = true;
    node.position.latitude_i = 447654321;
    node.position.longitude_i = -680012345;
    node.environment.valid = true;
    node.environment.has_temperature = true;
    node.environment.temperature = 20.0f;

    count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    bool battery_ok = false;
    bool latitude_ok = false;
    bool temperature_ok = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Battery") == 0 && strcmp(items[i].value, "plugged in") == 0) {
            battery_ok = true;
        }
        if (strcmp(items[i].label, "Latitude") == 0 &&
            strncmp(items[i].value, "44.76543", 8) == 0) {
            latitude_ok = true;
        }
        if (strcmp(items[i].label, "Temperature") == 0 &&
            strcmp(items[i].value, "20.0 C (68.0 F)") == 0) {
            temperature_ok = true;
        }
    }
    if (!battery_ok || !latitude_ok || !temperature_ok) {
        record_failure(test_name, "a reported value was missing or misformatted");
        return;
    }
    if (count != mesh_ui_node_detail_count(&node, false, NULL)) {
        record_failure(test_name, "the count disagrees once the sections appear");
        return;
    }

    record_success(test_name);
}

/*
 * The About section is the one part of the Settings tab that works with no radio: it opens on
 * a store that has never seen a handshake, and its rows come from the client info the app
 * publishes rather than from the air.
 */
MESH_TEST_CASE(ui_settings_about, unit) {
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    const char *failure = NULL;

    /* Deliberately nothing from a radio: no handshake, no loaded sections. */
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    snprintf(settings.client.version, sizeof settings.client.version, "%s", "1.12.0");
    snprintf(settings.client.backend, sizeof settings.client.backend, "%s", "fb");
    snprintf(settings.client.data_dir, sizeof settings.client.data_dir, "%s", "/tmp/meshclient");
    settings.client.update_supported = true;
    settings.client.update_can_install = true;
    settings.client.update_state = (uint8_t)MESH_UPDATE_IDLE;
    snprintf(settings.client.update_channel, sizeof settings.client.update_channel, "%s", "Stable");
    mesh_ui_store_set_settings(&store, &settings);

    if (!mesh_ui_settings_section_loaded(&store.settings, NULL, MESH_UI_SETTINGS_ABOUT)) {
        failure = "About should be loaded with no radio connected";
        goto cleanup;
    }
    if (mesh_ui_settings_section_loaded(&store.settings, NULL, MESH_UI_SETTINGS_LORA)) {
        failure = "the radio's own sections should still read as not loaded";
        goto cleanup;
    }

    struct mesh_ui_action action;
    for (int i = 0; i < 4; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    /* About is the first row, so the cursor is already on it. */
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != MESH_UI_SETTINGS_ABOUT) {
        failure = "Settings should open with the cursor on About";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_ABOUT) {
        failure = "A should open About";
        goto cleanup;
    }

    /* Version, backend, data dir, update status, then the check action. */
    const uint32_t rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    if (rows < 2U) {
        failure = "About should have rows";
        goto cleanup;
    }
    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        strcmp(item.label, "Version") != 0 || strcmp(item.value, "1.12.0") != 0) {
        failure = "the first row should be the client version";
        goto cleanup;
    }

    /* Find the check row and press A on it; it must raise CHECK_UPDATE and nothing else. */
    uint32_t check_row = rows;
    for (uint32_t i = 0; i < rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.kind == MESH_UI_SETTING_ACTION &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CHECK_UPDATE) {
            check_row = i;
        }
    }
    if (check_row >= rows) {
        failure = "About should offer a check action when updates are supported";
        goto cleanup;
    }
    for (uint32_t i = 0; i < check_row; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_CHECK_UPDATE) {
        failure = "A on the check row should ask the app to check";
        goto cleanup;
    }

    /* No install row until a check has actually found something: the action that replaces the
       running binary must never be reachable on a guess. */
    for (uint32_t i = 0; i < rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE) {
            failure = "there should be no install row before a check finds an update";
            goto cleanup;
        }
    }

    /* With an update found, the install row appears and A on it asks for the install. */
    settings.client.update_state = (uint8_t)MESH_UPDATE_AVAILABLE;
    snprintf(settings.client.update_latest, sizeof settings.client.update_latest, "%s", "1.13.0");
    snprintf(settings.client.update_message, sizeof settings.client.update_message, "%s",
             "1.13.0 available (running 1.12.0)");
    mesh_ui_store_set_settings(&store, &settings);
    const uint32_t available_rows =
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    uint32_t install_row = available_rows;
    for (uint32_t i = 0; i < available_rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE) {
            install_row = i;
        }
    }
    if (install_row >= available_rows) {
        failure = "an available update should offer an install row";
        goto cleanup;
    }
    store.nav.cursor[MESH_UI_SCREEN_SETTINGS] = install_row;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_INSTALL_UPDATE) {
        failure = "A on the install row should ask the app to install";
        goto cleanup;
    }

    /* The channel row is a setting the user can step with A, so its value column carries the
       channel rather than a button hint, and A on it asks the app to cycle it. */
    uint32_t channel_row = available_rows;
    for (uint32_t i = 0; i < available_rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL) {
            channel_row = i;
        }
    }
    if (channel_row >= available_rows) {
        failure = "About should offer the update channel";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                               MESH_UI_SETTINGS_NO_CHANNEL, channel_row, &item) ||
        strcmp(item.value, "Stable") != 0) {
        failure = "the channel row should show the channel, not a button hint";
        goto cleanup;
    }
    store.nav.cursor[MESH_UI_SCREEN_SETTINGS] = channel_row;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_CYCLE_UPDATE_CHANNEL) {
        failure = "A on the channel row should ask the app to cycle the channel";
        goto cleanup;
    }

    /*
     * A build that cannot install must never show an install row, even when a check has found
     * a newer release - and it has to say so, because "here is a newer version" with no way to
     * take it is exactly the dead end this section used to present on a dev build.
     */
    settings.client.update_can_install = false;
    settings.client.update_state = (uint8_t)MESH_UPDATE_UP_TO_DATE;
    snprintf(settings.client.update_message, sizeof settings.client.update_message, "%s",
             "Latest is 1.13.0; dev build, not installing");
    mesh_ui_store_set_settings(&store, &settings);
    const uint32_t dev_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    bool pointed_at_the_switch = false;
    uint32_t dev_toggle_row = dev_rows;
    for (uint32_t i = 0; i < dev_rows; ++i) {
        if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item)) {
            continue;
        }
        if (item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE) {
            failure = "a build that cannot install should offer no install row";
            goto cleanup;
        }
        if (item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES) {
            dev_toggle_row = i;
        }
        if (strcmp(item.value, "turn on Dev updates") == 0) {
            pointed_at_the_switch = true;
        }
    }
    if (!pointed_at_the_switch) {
        failure = "a build that cannot install should name the row that changes it";
        goto cleanup;
    }
    /*
     * That row has to be reachable from the device itself. The opt-in was an environment
     * variable first, which meant a handheld could only be let through from an ssh session on
     * another machine - the switch is here so the About screen alone is enough.
     */
    if (dev_toggle_row >= dev_rows) {
        failure = "a dev build should offer the dev-updates switch";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                               MESH_UI_SETTINGS_NO_CHANNEL, dev_toggle_row, &item) ||
        strcmp(item.value, "off") != 0) {
        failure = "the dev-updates switch should show its own position";
        goto cleanup;
    }
    store.nav.cursor[MESH_UI_SCREEN_SETTINGS] = dev_toggle_row;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_DEV_UPDATES) {
        failure = "A on the dev-updates row should ask the app to toggle it";
        goto cleanup;
    }

    /* Held on by MESHCLIENT_UPDATE_ALLOW_DEV it is a fact, not a switch: a toggle that sprang
       back to where it was would read as broken. */
    settings.client.update_allow_dev = true;
    settings.client.update_allow_dev_from_env = true;
    mesh_ui_store_set_settings(&store, &settings);
    for (uint32_t i = 0; i < mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
         ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES) {
            failure = "an env-held dev-updates row should not be a switch";
            goto cleanup;
        }
    }
    settings.client.update_allow_dev = false;
    settings.client.update_allow_dev_from_env = false;

    /* A release build has no guard to lift, so it is never shown the switch. */
    settings.client.update_is_release = true;
    settings.client.update_can_install = true;
    settings.client.update_state = (uint8_t)MESH_UPDATE_AVAILABLE;
    mesh_ui_store_set_settings(&store, &settings);
    for (uint32_t i = 0; i < mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
         ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES) {
            failure = "a release build should not offer the dev-updates switch";
            goto cleanup;
        }
    }

    /* While a child is running neither action is offered, so a second press cannot stack one. */
    settings.client.update_state = (uint8_t)MESH_UPDATE_DOWNLOADING;
    settings.client.update_busy = true;
    mesh_ui_store_set_settings(&store, &settings);
    const uint32_t busy_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    for (uint32_t i = 0; i < busy_rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.kind == MESH_UI_SETTING_ACTION) {
            failure = "a busy updater should offer no actions";
            goto cleanup;
        }
    }

    /* A device with no curl or wget says so instead of offering rows that cannot work. */
    memset(&settings.client, 0, sizeof settings.client);
    snprintf(settings.client.version, sizeof settings.client.version, "%s", "1.12.0");
    snprintf(settings.client.update_message, sizeof settings.client.update_message, "%s",
             "No curl or wget on this device");
    mesh_ui_store_set_settings(&store, &settings);
    const uint32_t bare_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    bool said_why = false;
    for (uint32_t i = 0; i < bare_rows; ++i) {
        if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item)) {
            continue;
        }
        if (item.kind == MESH_UI_SETTING_ACTION) {
            failure = "an unsupported updater should offer no actions";
            goto cleanup;
        }
        if (strcmp(item.value, "No curl or wget on this device") == 0) {
            said_why = true;
        }
    }
    if (!said_why) {
        failure = "an unsupported updater should say why";
        goto cleanup;
    }

    /* B backs out to the section list, as in every other section. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        failure = "B should return to the section list";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

/* Decimal degrees in and out. Parsed as integers rather than through a double, because the
   wire wants exactly seven decimal places and the last one has to survive the trip. */
MESH_TEST_CASE(ui_settings_coords, unit) {
    char text[32];

    mesh_ui_settings_coord_text(446488000, text, sizeof text);
    if (strcmp(text, "44.64880") != 0) {
        record_failure(test_name, "a positive coordinate formats wrong");
        return;
    }
    mesh_ui_settings_coord_text(-635752000, text, sizeof text);
    if (strcmp(text, "-63.57520") != 0) {
        record_failure(test_name, "a negative coordinate formats wrong");
        return;
    }
    /* Between -1 and 0 the whole degrees are zero, so the sign has nowhere else to live. */
    mesh_ui_settings_coord_text(-5000000, text, sizeof text);
    if (strcmp(text, "-0.50000") != 0) {
        record_failure(test_name, "a coordinate inside the first degree loses its sign");
        return;
    }

    int32_t value = 0;
    if (!mesh_ui_settings_coord_parse("44.6488", 90, &value) || value != 446488000) {
        record_failure(test_name, "a plain coordinate should parse");
        return;
    }
    if (!mesh_ui_settings_coord_parse("-63.57520", 180, &value) || value != -635752000) {
        record_failure(test_name, "a negative coordinate should parse");
        return;
    }
    if (!mesh_ui_settings_coord_parse("7", 90, &value) || value != 70000000) {
        record_failure(test_name, "a whole number of degrees should parse");
        return;
    }
    /* Round trip, which is the property that actually matters: what the row showed is what
       the radio gets back when nobody edits it. */
    mesh_ui_settings_coord_text(-5000000, text, sizeof text);
    if (!mesh_ui_settings_coord_parse(text, 90, &value) || value != -5000000) {
        record_failure(test_name, "a formatted coordinate should parse back to itself");
        return;
    }

    /* A bare dot, with or without a sign, would otherwise read as zero - and a zero the
       (0, 0) guard downstream cannot catch, because the other coordinate is real. */
    if (mesh_ui_settings_coord_parse(".", 90, &value) ||
        mesh_ui_settings_coord_parse("-.", 90, &value) ||
        mesh_ui_settings_coord_parse("+.", 90, &value) ||
        mesh_ui_settings_coord_parse("", 90, &value) ||
        mesh_ui_settings_coord_parse("44.6N", 90, &value) ||
        mesh_ui_settings_coord_parse("north", 90, &value) ||
        mesh_ui_settings_coord_parse("91", 90, &value) ||
        mesh_ui_settings_coord_parse("-90.5", 90, &value) ||
        mesh_ui_settings_coord_parse("181", 180, &value)) {
        record_failure(test_name, "rubbish and out-of-range coordinates should be refused");
        return;
    }
    /* A longitude is not a latitude: the same number passes one and fails the other. */
    if (!mesh_ui_settings_coord_parse("120.0", 180, &value) ||
        mesh_ui_settings_coord_parse("120.0", 90, &value)) {
        record_failure(test_name, "the range limit is not being applied");
        return;
    }

    record_success(test_name);
}
