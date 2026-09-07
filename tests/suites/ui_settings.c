#define _POSIX_C_SOURCE 200809L

/* The settings model itself: rows, edits, key text, coordinates, About. */

#include "framework/mesh_test.h"

#include "mesh/core/radio_settings.h"
/* For enum mesh_traceroute_state, which the UI's traceroute carries as a byte. */
#include "mesh/core/session.h"
#include "mesh/core/updater.h"
#include "mesh/ui/layout.h"
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

    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_section_loaded(&settings, &handshake, MESH_UI_SETTINGS_LORA) ||
            mesh_ui_settings_section_loaded(&settings, &handshake, MESH_UI_SETTINGS_DISPLAY) ||
            mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_DISPLAY,
                                        MESH_UI_SETTINGS_NO_CHANNEL) != 0U,
        "section loaded flags are wrong");

    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
                          strcmp(item.label, "Region") != 0 || strcmp(item.value, "US") != 0 ||
                          item.kind != MESH_UI_SETTING_ENUM,
                      "LoRa region row is wrong");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
                          strcmp(item.label, "Preset") != 0 ||
                          strcmp(item.value, "Long Range - Moderate") != 0,
                      "LoRa preset row is wrong");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U,
                                             MESH_UI_SETTINGS_DEVICE, MESH_UI_SETTINGS_NO_CHANNEL,
                                             0U, &item) ||
                          strcmp(item.value, "Router Late") != 0,
                      "device role row is wrong");
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
            item.kind != MESH_UI_SETTING_KEY || strncmp(item.value, "deadbeef...", 11U) != 0 ||
            strstr(item.value, "32 bytes") == NULL,
        "public key fingerprint is wrong");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 2U ||
            !mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
            strcmp(item.label, "1 Team") != 0 || strstr(item.value, "AES-128") == NULL ||
            strstr(item.value, "up on") == NULL || strstr(item.value, "down off") == NULL,
        "channel row is wrong");
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
            strcmp(item.label, "0 Primary") != 0 || strstr(item.value, "default key") == NULL,
        "primary channel row is wrong");
    MESH_TEST_FAIL_IF(mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                            MESH_UI_SETTINGS_NO_CHANNEL, 99U, &item),
                      "out-of-range row should fail");
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
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_USER,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
                          item.field != MESH_UI_FIELD_USER_LONG_NAME ||
                          item.kind != MESH_UI_SETTING_TEXT || item.dirty ||
                          strcmp(item.text, "Meshtastic 0ad8") != 0 ||
                          strcmp(item.value, "Meshtastic 0ad8") != 0,
                      "long name row is wrong");
    MESH_TEST_FAIL_IF(mesh_ui_settings_text_max(MESH_UI_FIELD_USER_LONG_NAME) != 24U ||
                          mesh_ui_settings_text_max(MESH_UI_FIELD_USER_SHORT_NAME) != 4U ||
                          mesh_ui_settings_text_max(MESH_UI_FIELD_DISPLAY_12H) != 0U,
                      "text caps are wrong");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_DISPLAY,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
                          item.field != MESH_UI_FIELD_DISPLAY_SCREEN_ON ||
                          item.kind != MESH_UI_SETTING_NUMBER || item.number != 60U ||
                          strcmp(item.value, "1m") != 0,
                      "screen-on row is wrong");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 60U, +1) != 120U ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 60U, -1) != 30U ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 3600U, +1) != 3600U ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 0U, -1) != 0U ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_TELEMETRY_INTERVAL, 1234U, +1) != 1800U ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_TELEMETRY_INTERVAL, 1234U, -1) != 900U,
        "number presets step wrong");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_COMPASS) != 8U ||
            mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_UNITS) != 2U ||
            mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_FLIP) != 0U ||
            strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_DISPLAY_UNITS, 1U), "Imperial") != 0 ||
            strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_DISPLAY_COMPASS, 7U), "270 flip") != 0,
        "enum tables are wrong");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_field_section(MESH_UI_FIELD_SF_SERVER) != MESH_UI_SETTINGS_STORE_FORWARD ||
            mesh_ui_settings_field_kind(MESH_UI_FIELD_TELEMETRY_INTERVAL) !=
                MESH_UI_SETTING_NUMBER ||
            strcmp(mesh_ui_settings_field_label(MESH_UI_FIELD_USER_SHORT_NAME), "Short name") != 0,
        "field descriptions are wrong");
    /* All fifteen TelemetryConfig wire fields have a row, under five headings. Pinned as a
       count because the section is meant to be complete: a field added upstream, or one left
       out of a builder, moves this number. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_TELEMETRY,
                                                  MESH_UI_SETTINGS_NO_CHANNEL) != 20U,
                      "telemetry should be fifteen fields under five headings");
    /* Telemetry's groups sit under headings, so the device interval is row 2: heading,
       Enabled, Interval. The heading itself carries no field and no value. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_TELEMETRY,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
                          item.kind != MESH_UI_SETTING_HEADING ||
                          item.field != MESH_UI_FIELD_NONE || item.value[0] != '\0' ||
                          strcmp(item.label, "Device") != 0,
                      "telemetry should open on a heading");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_TELEMETRY,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
                          item.field != MESH_UI_FIELD_TELEMETRY_INTERVAL ||
                          strcmp(item.value, "1234s") != 0,
                      "telemetry interval row is wrong");

    /* Pending edits show in place, marked. */
    struct mesh_ui_setting_edit edits[2];
    memset(edits, 0, sizeof edits);
    edits[0].field = MESH_UI_FIELD_DISPLAY_UNITS;
    edits[0].number = 0U;
    edits[1].field = MESH_UI_FIELD_USER_SHORT_NAME;
    snprintf(edits[1].text, sizeof edits[1].text, "%s", "BRCK");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_DISPLAY,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
                          item.field != MESH_UI_FIELD_DISPLAY_UNITS || !item.dirty ||
                          item.number != 0U || strcmp(item.value, "Metric") != 0,
                      "an enum edit should render in place");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_USER,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
                          !item.dirty || strcmp(item.text, "BRCK") != 0 ||
                          strcmp(item.value, "BRCK") != 0,
                      "a text edit should render in place");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_DISPLAY,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
                          item.dirty || item.number != 60U,
                      "rows without an edit stay clean");
    MESH_TEST_FAIL_IF(mesh_ui_settings_find_edit(edits, 2U, MESH_UI_FIELD_USER_SHORT_NAME) !=
                              &edits[1] ||
                          mesh_ui_settings_find_edit(edits, 2U, MESH_UI_FIELD_DISPLAY_FLIP) != NULL,
                      "find_edit is wrong");
    record_success(test_name);
}

/*
 * The two lists phase 9 split the Settings tab into: the top level, which no longer contains a
 * module, and the Modules list, which contains nothing else.
 */
MESH_TEST_CASE(ui_settings_modules, unit) {
    /* Nothing on the top level is a module, and Modules itself is on it exactly once. */
    uint32_t modules_rows = 0U;
    for (uint32_t i = 0; i < mesh_ui_settings_root_count(); ++i) {
        const enum mesh_ui_settings_section section = mesh_ui_settings_root_at(i);
        MESH_TEST_FAIL_IF(mesh_ui_settings_section_is_module(section),
                          "a module should not be on the top-level list");
        modules_rows += (section == MESH_UI_SETTINGS_MODULES) ? 1U : 0U;
    }
    MESH_TEST_FAIL_IF(modules_rows != 1U, "the top level should carry one Modules row");
    MESH_TEST_FAIL_IF(mesh_ui_settings_module_count() == 0U,
                      "the Modules list should not be empty");
    for (uint32_t i = 0; i < mesh_ui_settings_module_count(); ++i) {
        MESH_TEST_FAIL_IF(!mesh_ui_settings_section_is_module(mesh_ui_settings_module_at(i)),
                          "every Modules row should be a module");
    }

    /* The list itself: a folder, so it renders with no radio, and each row says whether the
       radio has sent that module rather than hiding the ones it has not. */
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_section_loaded(&settings, NULL, MESH_UI_SETTINGS_MODULES),
                      "the Modules list should render without a radio");
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_MODULES,
                                                  MESH_UI_SETTINGS_NO_CHANNEL) !=
                          mesh_ui_settings_module_count(),
                      "the Modules list should have a row per module");

    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_MODULES,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
                          item.kind != MESH_UI_SETTING_ACTION ||
                          item.number != (uint32_t)mesh_ui_settings_module_at(0U) ||
                          strcmp(item.value, "not loaded") != 0,
                      "a module the radio has not sent should say so");

    /* Once the radio has sent one, the value is its enabled state. Telemetry has no single
       flag, so it counts as on when any of its five groups is measuring. */
    settings.has_store_forward = true;
    settings.store_forward_enabled = true;
    settings.has_telemetry = true;
    for (uint32_t i = 0; i < mesh_ui_settings_module_count(); ++i) {
        MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U,
                                                 MESH_UI_SETTINGS_MODULES,
                                                 MESH_UI_SETTINGS_NO_CHANNEL, i, &item),
                          "every Modules row should build");
        const enum mesh_ui_settings_section section = mesh_ui_settings_module_at(i);
        const char *expect = section == MESH_UI_SETTINGS_STORE_FORWARD ? "on"
                             : section == MESH_UI_SETTINGS_TELEMETRY   ? "off"
                                                                       : "not loaded";
        MESH_TEST_FAIL_IF(strcmp(item.value, expect) != 0, "a module row shows the wrong state");
    }
    settings.health_measurement_enabled = true;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_MODULES,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
                          strcmp(item.value, "on") != 0,
                      "telemetry should read as on once any group measures");
    record_success(test_name);
}

/*
 * Phase 10's six modules: the rows they build, and the one row model none of the others use -
 * a NUMBER over a signed value.
 */
MESH_TEST_CASE(ui_settings_small_modules, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;

    /* Every phase 10 section is a module, and none of them reached the top level. */
    const enum mesh_ui_settings_section k_added[] = {
        MESH_UI_SETTINGS_NEIGHBOR_INFO, MESH_UI_SETTINGS_RANGE_TEST,
        MESH_UI_SETTINGS_PAXCOUNTER,    MESH_UI_SETTINGS_TAK,
        MESH_UI_SETTINGS_AMBIENT,       MESH_UI_SETTINGS_STATUS_MESSAGE,
    };
    for (size_t i = 0; i < sizeof k_added / sizeof k_added[0]; ++i) {
        MESH_TEST_FAIL_IF(!mesh_ui_settings_section_is_module(k_added[i]),
                          "a phase 10 section should be a module");
        MESH_TEST_FAIL_IF(mesh_ui_settings_section_loaded(&settings, NULL, k_added[i]),
                          "a module the radio has not sent should not report loaded");
    }

    /*
     * The RSSI rows. Their presets are negative dBm stored through a cast to uint32_t, which
     * only steps correctly because every preset shares a sign - so this is the assertion that
     * would catch a positive value being added to the list later.
     */
    settings.has_paxcounter = true;
    settings.paxcounter_enabled = true;
    settings.paxcounter_wifi_threshold = -80;
    settings.paxcounter_ble_threshold = -90;
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_number_step(MESH_UI_FIELD_PAX_WIFI_THRESHOLD, (uint32_t)(int32_t)-80,
                                     +1) != (uint32_t)(int32_t)-75 ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_PAX_WIFI_THRESHOLD, (uint32_t)(int32_t)-80,
                                         -1) != (uint32_t)(int32_t)-85,
        "an RSSI row should step towards zero on Right and away on Left");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_number_step(MESH_UI_FIELD_PAX_WIFI_THRESHOLD, (uint32_t)(int32_t)-100,
                                     -1) != (uint32_t)(int32_t)-100 ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_PAX_WIFI_THRESHOLD, (uint32_t)(int32_t)-60,
                                         +1) != (uint32_t)(int32_t)-60,
        "an RSSI row should stop at both ends rather than wrapping");

    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_PAXCOUNTER,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 3U, &item) ||
                          item.field != MESH_UI_FIELD_PAX_WIFI_THRESHOLD ||
                          strcmp(item.value, "-80 dBm") != 0,
                      "the WiFi floor should render as signed dBm");
    /* A radio that never had these set reports 0, which is not a floor the module uses; the
       row shows the firmware's own default instead of "0 dBm". */
    settings.paxcounter_wifi_threshold = 0;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_PAXCOUNTER,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 3U, &item) ||
                          strcmp(item.value, "-80 dBm") != 0,
                      "an unset RSSI floor should show the firmware default");

    /* TAK is two contiguous enums, which is what the nav's (value + 1) % count stepping needs. */
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_enum_count(MESH_UI_FIELD_TAK_TEAM) != 15U ||
            mesh_ui_settings_enum_count(MESH_UI_FIELD_TAK_ROLE) != 9U ||
            strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_TAK_TEAM, 5U), "Red") != 0 ||
            strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_TAK_ROLE, 5U), "Medic") != 0,
        "the TAK enum tables are wrong");

    /* node_status is 79 bytes on the wire and the edit buffer has to hold all of them. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_text_max(MESH_UI_FIELD_STATUS_TEXT) != 79U ||
                          MESH_UI_SETTING_TEXT_MAX < 80U,
                      "a status message should not be truncated by the edit buffer");

    /*
     * A module fragment is a section like any other: holding only one has to read as "we have
     * something from the radio". mesh_radio_settings_loaded() enumerates the flags by hand, so
     * a module added without being listed there reads as no radio at all - checked here for
     * every phase 10 module, one at a time, because the bug only shows when its flag is the
     * only one set.
     */
    struct mesh_radio_settings radio;
    for (size_t i = 0; i < sizeof k_added / sizeof k_added[0]; ++i) {
        mesh_radio_settings_reset(&radio);
        switch (k_added[i]) {
        case MESH_UI_SETTINGS_NEIGHBOR_INFO:
            radio.has_neighbor_info = true;
            break;
        case MESH_UI_SETTINGS_RANGE_TEST:
            radio.has_range_test = true;
            break;
        case MESH_UI_SETTINGS_PAXCOUNTER:
            radio.has_paxcounter = true;
            break;
        case MESH_UI_SETTINGS_TAK:
            radio.has_tak = true;
            break;
        case MESH_UI_SETTINGS_AMBIENT:
            radio.has_ambient_lighting = true;
            break;
        case MESH_UI_SETTINGS_STATUS_MESSAGE:
            radio.has_status_message = true;
            break;
        default:
            break;
        }
        MESH_TEST_FAIL_IF(!mesh_radio_settings_loaded(&radio),
                          "one module fragment on its own should count as loaded");
    }

    /* Range test says what its transmitter does, and only listens at 0. */
    settings.has_range_test = true;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RANGE_TEST,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 3U, &item) ||
                          item.field != MESH_UI_FIELD_RANGE_TEST_SENDER ||
                          strcmp(item.value, "never") != 0,
                      "an unset range-test sender should read as never");
    record_success(test_name);
}

/*
 * Phase 11's three: the modules the heading row was added for, and the one with no enabled
 * flag at all.
 */
MESH_TEST_CASE(ui_settings_large_modules, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_detection_sensor = true;
    settings.has_external_notification = true;
    settings.has_traffic_management = true;

    /*
     * External notification is three near-identical output groups. Every group must have the
     * same three-row shape under its own heading, and no row may be shared between them - a
     * copy-paste that pointed two groups at one field would show as the same value twice and
     * write one pin where the user set two.
     */
    static const struct {
        const char *heading;
        enum mesh_ui_setting_field pin;
        enum mesh_ui_setting_field message;
        enum mesh_ui_setting_field bell;
    } k_groups[] = {
        {"Output", MESH_UI_FIELD_EXTNOTIF_PIN, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG,
         MESH_UI_FIELD_EXTNOTIF_ALERT_BELL},
        {"Vibra", MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA,
         MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA},
        {"Buzzer", MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER,
         MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER},
    };
    const uint32_t ext_rows = mesh_ui_settings_item_count(
        &settings, NULL, MESH_UI_SETTINGS_EXT_NOTIFICATION, MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(ext_rows != 18U, "external notify should be 15 fields under 3 headings");
    for (size_t g = 0; g < sizeof k_groups / sizeof k_groups[0]; ++g) {
        /* The heading, then pin / message / bell, in that order. */
        bool found = false;
        for (uint32_t row = 0; row + 3U < ext_rows && !found; ++row) {
            struct mesh_ui_settings_item head;
            if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &head) ||
                head.kind != MESH_UI_SETTING_HEADING ||
                strcmp(head.label, k_groups[g].heading) != 0) {
                continue;
            }
            found = true;
            const enum mesh_ui_setting_field expect[3] = {k_groups[g].pin, k_groups[g].message,
                                                          k_groups[g].bell};
            for (uint32_t i = 0; i < 3U; ++i) {
                struct mesh_ui_settings_item item;
                MESH_TEST_FAIL_IF(!mesh_ui_settings_item(
                                      &settings, NULL, NULL, 0U, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                      MESH_UI_SETTINGS_NO_CHANNEL, row + 1U + i, &item) ||
                                      item.field != expect[i],
                                  "an output group has the wrong rows under its heading");
            }
        }
        MESH_TEST_FAIL_IF(!found, "an output group is missing its heading");
    }

    /*
     * Fifteen editable fields in one section, against MESH_UI_SETTINGS_EDITS_MAX. Over the cap
     * mesh_ui_nav_edit_set() returns false and the press silently does nothing, which is how
     * the old cap of 8 hid - so this is asserted rather than assumed.
     */
    uint32_t editable = 0U;
    for (uint32_t row = 0; row < ext_rows; ++row) {
        struct mesh_ui_settings_item item;
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                  MESH_UI_SETTINGS_NO_CHANNEL, row, &item) &&
            item.field != MESH_UI_FIELD_NONE) {
            editable++;
        }
    }
    MESH_TEST_FAIL_IF(editable != 15U, "external notify should offer all fifteen wire fields");
    MESH_TEST_FAIL_IF(editable > MESH_UI_SETTINGS_EDITS_MAX,
                      "a section must not have more editable rows than an edit list can hold");

    /* Traffic management has no enabled toggle: every row is off at 0 by the module's own
       convention, and the section says so on its first row. */
    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_TRAFFIC,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
                          item.field != MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL ||
                          strcmp(item.value, "off") != 0,
                      "an unset traffic limit should read as off");

    /*
     * The three count rows carry both a zero_label and a formatter. The formatter used to win,
     * so they read "default" under a section whose stated convention is that 0 is off - checked
     * on all three, since one of them passing says nothing about the others.
     */
    static const enum mesh_ui_setting_field k_traffic_counts[] = {
        MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS,
        MESH_UI_FIELD_TRAFFIC_RATE_PACKETS,
        MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD,
    };
    const uint32_t traffic_rows = mesh_ui_settings_item_count(
        &settings, NULL, MESH_UI_SETTINGS_TRAFFIC, MESH_UI_SETTINGS_NO_CHANNEL);
    for (size_t f = 0; f < sizeof k_traffic_counts / sizeof k_traffic_counts[0]; ++f) {
        bool seen = false;
        for (uint32_t row = 0; row < traffic_rows; ++row) {
            struct mesh_ui_settings_item r;
            if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_TRAFFIC,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &r) ||
                r.field != k_traffic_counts[f]) {
                continue;
            }
            seen = true;
            MESH_TEST_FAIL_IF(strcmp(r.value, "off") != 0,
                              "a traffic count row at 0 should read off, not default");
        }
        MESH_TEST_FAIL_IF(!seen, "a traffic count row is missing");
    }

    /*
     * GPIO rows step through every number in the range rather than a curated subset. The first
     * version of the list was an ESP32 pin map, which made 9, 10 and 28 - all usable output on
     * a RAK4631 - unreachable, so those three are checked by name along with contiguity.
     */
    static const uint32_t k_omitted_before[] = {9U, 10U, 28U};
    for (size_t i = 0; i < sizeof k_omitted_before / sizeof k_omitted_before[0]; ++i) {
        const uint32_t pin = k_omitted_before[i];
        MESH_TEST_FAIL_IF(mesh_ui_settings_number_step(MESH_UI_FIELD_DETECT_PIN, pin - 1U, +1) !=
                              pin,
                          "a GPIO row should be able to reach every pin in the range");
    }
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_number_step(MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA, 0U, -1) != 0U ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA, 48U, +1) != 48U,
        "a GPIO row should stop at both ends of the range");

    /* Detection sensor: a contiguous trigger enum and a pin that reads as unset at 0. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_enum_count(MESH_UI_FIELD_DETECT_TRIGGER) != 6U ||
                          strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_DETECT_TRIGGER, 2U),
                                 "Falling edge") != 0,
                      "the detection trigger enum is wrong");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_DETECTION,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 6U, &item) ||
                          item.field != MESH_UI_FIELD_DETECT_PIN ||
                          strcmp(item.value, "unset") != 0,
                      "an unset monitor pin should say so rather than reading as GPIO 0");
    settings.detection_monitor_pin = 17U;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_DETECTION,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 6U, &item) ||
                          strcmp(item.value, "GPIO 17") != 0,
                      "a set monitor pin should name the GPIO");
    record_success(test_name);
}

/* Keys as text: base64 out, base64 or hex in, per-field lengths and choices. */
/*
 * Every warning the confirm sheet puts in front of a destructive press has to fit inside it.
 * The sheet draws four wrapped lines and no more, so a longer one is not scrolled or shortened
 * - it is cut, mid-sentence, and the clause that goes is the last one, which is where these
 * put what is lost. Two of them had drifted past the limit before this test existed.
 *
 * 38 cells is narrower than the Brick's confirm sheet actually is, which is the point: it
 * leaves the copy room to survive a slightly larger glyph or a slightly narrower panel.
 */
MESH_TEST_CASE(ui_settings_confirm_fits, unit) {
    static const enum mesh_ui_settings_action actions[] = {
        MESH_UI_SETTINGS_ACTION_REBOOT,
        MESH_UI_SETTINGS_ACTION_SHUTDOWN,
        MESH_UI_SETTINGS_ACTION_RESET_NODEDB,
        MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES,
        MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES,
        MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG,
        MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE,
    };
    for (size_t i = 0; i < sizeof actions / sizeof actions[0]; ++i) {
        char text[256];
        char message[128];
        MESH_TEST_FAIL_IF(!mesh_ui_settings_action_needs_confirm(actions[i]),
                          "an action in the confirm list does not ask first");
        mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_ACTIONS, actions[i], text, sizeof text);
        const uint32_t lines = mesh_ui_wrap_lines(text, 38U);
        snprintf(message, sizeof message, "action %u needs %u lines and the sheet draws 4",
                 (unsigned)actions[i], lines);
        MESH_TEST_FAIL_IF(lines > 4U, message);
        mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_NO_CHANNEL,
                                       actions[i], text, sizeof text);
        MESH_TEST_FAIL_IF(text[0] == '\0', "an action reached the sheet with no question on it");
    }
    record_success(test_name);
}

MESH_TEST_CASE(ui_settings_key_text, unit) {
    static const uint8_t k_default[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                          0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    char text[64];
    mesh_ui_settings_key_text(k_default, sizeof k_default, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "1PG7OiApB1nwvP+rz05pAQ==") != 0,
                      "base64 of the default key is wrong");
    uint8_t parsed[32];
    size_t len = 0U;
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_key_parse("1PG7OiApB1nwvP+rz05pAQ==", parsed, sizeof parsed, &len) ||
            len != 16U || memcmp(parsed, k_default, 16U) != 0,
        "base64 should parse back");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_key_parse("d4f1bb3a20290759f0bcffabcf4e6901", parsed,
                                                  sizeof parsed, &len) ||
                          len != 16U || memcmp(parsed, k_default, 16U) != 0,
                      "hex should parse too");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_key_parse("AQ==", parsed, sizeof parsed, &len) ||
                          len != 1U || parsed[0] != 1U ||
                          !mesh_ui_settings_key_parse("", parsed, sizeof parsed, &len) || len != 0U,
                      "one-byte and empty keys should parse");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_key_parse("1PG7OiApB1nwvP+rz05pAQ=", parsed, sizeof parsed, &len) ||
            mesh_ui_settings_key_parse("1PG7Oi=pB1nwvP+rz05pAQ==", parsed, sizeof parsed, &len) ||
            mesh_ui_settings_key_parse("not a key!", parsed, sizeof parsed, &len) ||
            mesh_ui_settings_key_parse("abc", parsed, sizeof parsed, &len),
        "bad text must be refused");
    uint8_t all[32];
    for (unsigned i = 0; i < 32U; ++i) {
        all[i] = (uint8_t)(i * 7U);
    }
    mesh_ui_settings_key_text(all, 32U, text, sizeof text);
    MESH_TEST_FAIL_IF(strlen(text) != 44U ||
                          !mesh_ui_settings_key_parse(text, parsed, sizeof parsed, &len) ||
                          len != 32U || memcmp(parsed, all, 32U) != 0,
                      "a 32-byte key should round-trip");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_KEY, 1U) ||
                          mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_KEY, 8U) ||
                          !mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_PRIVATE_KEY, 32U) ||
                          mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_PRIVATE_KEY, 16U) ||
                          !mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_ADMIN_KEY_1, 0U) ||
                          mesh_ui_settings_key_len_ok(MESH_UI_FIELD_DISPLAY_FLIP, 0U),
                      "key length rules are wrong");
    MESH_TEST_FAIL_IF(mesh_ui_settings_key_choices(MESH_UI_FIELD_SECURITY_PRIVATE_KEY) !=
                              (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) |
                               MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256)) ||
                          (mesh_ui_settings_key_choices(MESH_UI_FIELD_CHANNEL_KEY) &
                           MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_DEFAULT)) == 0U ||
                          mesh_ui_settings_key_choices(MESH_UI_FIELD_LORA_HOPS) != 0U,
                      "key choices are wrong");
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
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, false, NULL, NULL,
                                               items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count != mesh_ui_node_detail_count(&node, false, NULL, NULL),
                      "the count the nav walks disagrees with the built list");
    MESH_TEST_FAIL_IF(count == 0U || items[0].kind != MESH_UI_NODE_ROW_ACTION ||
                          items[0].action != MESH_UI_NODE_ACTION_MESSAGE,
                      "the first row should be the message action");

    /* A bare node has no metrics, position or environment to show. */
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(strcmp(items[i].label, "Position") == 0 ||
                              strcmp(items[i].label, "Environment") == 0 ||
                              strcmp(items[i].label, "Device metrics") == 0,
                          "a bare node should not show an empty section");
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
    const uint32_t self_count = mesh_ui_node_detail_count(&node, true, NULL, NULL);
    struct mesh_ui_node_item self_items[MESH_UI_NODE_ITEMS_MAX];
    mesh_ui_node_detail_build(&node, true, 1750000600U, NULL, false, NULL, NULL, self_items,
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

    count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, false, NULL, NULL, items,
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
    MESH_TEST_FAIL_IF(!battery_ok || !latitude_ok || !temperature_ok,
                      "a reported value was missing or misformatted");
    MESH_TEST_FAIL_IF(count != mesh_ui_node_detail_count(&node, false, NULL, NULL),
                      "the count disagrees once the sections appear");

    record_success(test_name);
}

/*
 * The About section is the one part of the Settings tab that works with no radio: it opens on
 * a store that has never seen a handshake, and its rows come from the client info the app
 * publishes rather than from the air.
 */
MESH_TEST_CASE(ui_settings_about, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
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
    snprintf(settings.client.theme, sizeof settings.client.theme, "%s", "dark");
    snprintf(settings.client.theme_name, sizeof settings.client.theme_name, "%s", "Dark");
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

    /*
     * The theme row. It steps the look on A and shows the name it is on, and it sits above the
     * update rows because those return early - with no updater, mid-check, or with an install
     * ready - and a row after them would vanish exactly when somebody wanted it.
     */
    uint32_t theme_row = rows;
    for (uint32_t i = 0; i < rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            strcmp(item.label, "Theme") == 0) {
            theme_row = i;
        }
    }
    if (theme_row >= rows) {
        failure = "About should offer a theme row";
        goto cleanup;
    }
    if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                              MESH_UI_SETTINGS_NO_CHANNEL, theme_row, &item)) {
        if (item.kind != MESH_UI_SETTING_ACTION ||
            item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_THEME) {
            failure = "the theme row should be the cycle-theme action";
            goto cleanup;
        }
        if (strcmp(item.value, "Dark") != 0) {
            failure = "the theme row should show the name of the theme in use";
            goto cleanup;
        }
    }
    /* Back to the top - an earlier case above left the cursor on the check row - and then
       down to the theme row. Up clamps at the first row, so this lands where it says. */
    for (uint32_t i = 0; i < rows; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    }
    for (uint32_t i = 0; i < theme_row; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_CYCLE_THEME) {
        failure = "A on the theme row should ask the app to step the theme";
        goto cleanup;
    }

    /*
     * With MESHCLIENT_THEME holding the choice the row becomes a fact. A switch that sprang
     * back on the next frame would look broken, which is the same reason the dev-updates row
     * goes read-only when its environment variable is set.
     */
    settings.client.theme_from_env = true;
    mesh_ui_store_set_settings(&store, &settings);
    bool pinned_row_found = false;
    const uint32_t pinned_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    for (uint32_t i = 0; i < pinned_rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            strncmp(item.label, "Theme", 5U) == 0) {
            pinned_row_found = true;
            if (item.kind != MESH_UI_SETTING_INFO ||
                item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_THEME) {
                failure = "an environment-held theme should not offer a press";
                goto cleanup;
            }
            /* The note is in the label so it fits whatever the theme is called, and the value
               stays the plain name - a clipped "(env" would read as a bug. */
            if (strstr(item.label, "(env)") == NULL || strcmp(item.value, "Dark") != 0) {
                failure = "an environment-held theme should say so without clipping the name";
                goto cleanup;
            }
        }
    }
    if (!pinned_row_found) {
        failure = "the theme row should still be shown when the environment holds it";
        goto cleanup;
    }
    settings.client.theme_from_env = false;
    mesh_ui_store_set_settings(&store, &settings);

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

    /*
     * While a child is running none of the *update* actions are offered, so a second press
     * cannot stack one. Scoped to those four rather than to every action in the section: the
     * theme row is also an action and is unaffected by a download - it touches nothing the
     * updater owns - and taking a working control away for an unrelated reason would be its
     * own bug.
     */
    settings.client.update_state = (uint8_t)MESH_UPDATE_DOWNLOADING;
    settings.client.update_busy = true;
    mesh_ui_store_set_settings(&store, &settings);
    const uint32_t busy_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    for (uint32_t i = 0; i < busy_rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.kind == MESH_UI_SETTING_ACTION &&
            item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_THEME) {
            failure = "a busy updater should offer no update actions";
            goto cleanup;
        }
    }

    /*
     * The working row while a child is running is a *meter*, and which of the two bars it asks
     * for is the whole of what the progress plumbing is for.
     *
     * A download with a size to divide by carries a fraction, and the row's words are the figure
     * alone - the row above already says which step is running, and repeating it here would clip
     * the one thing this row adds. A step with no length carries MESH_UI_METER_UNKNOWN, which is
     * a bar that moves without claiming a position rather than one parked at zero.
     */
    settings.client.update_progress_known = true;
    settings.client.update_progress = 714U;
    mesh_ui_store_set_settings(&store, &settings);
    bool found_meter = false;
    for (uint32_t i = 0; i < busy_rows; ++i) {
        if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item) ||
            item.kind != MESH_UI_SETTING_METER) {
            continue;
        }
        found_meter = true;
        if (item.number != 714U) {
            failure = "a download's meter should carry the permille the updater reported";
            goto cleanup;
        }
        if (strcmp(item.value, "71%") != 0) {
            failure = "a download's meter row should say the figure and nothing else";
            goto cleanup;
        }
        /* A fact, not a control: nothing on it may be edited, or Left and Right on a progress
           bar would try to set a download's position. */
        if (item.field != MESH_UI_FIELD_NONE) {
            failure = "a meter row should carry no editable field";
            goto cleanup;
        }
    }
    if (!found_meter) {
        failure = "a running download should be shown as a meter";
        goto cleanup;
    }

    settings.client.update_state = (uint8_t)MESH_UPDATE_CHECKING;
    settings.client.update_progress_known = false;
    settings.client.update_progress = 0U;
    mesh_ui_store_set_settings(&store, &settings);
    found_meter = false;
    for (uint32_t i = 0; i < mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
         ++i) {
        if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item) ||
            item.kind != MESH_UI_SETTING_METER) {
            continue;
        }
        found_meter = true;
        if (item.number != MESH_UI_METER_UNKNOWN) {
            failure = "a step with no length should be an unknown meter, not a zero one";
            goto cleanup;
        }
        if (strcmp(item.value, "checking...") != 0) {
            failure = "a step with no length should still say what it is doing";
            goto cleanup;
        }
    }
    if (!found_meter) {
        failure = "a running check should be shown as a meter";
        goto cleanup;
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
    MESH_TEST_FAIL_IF(strcmp(text, "44.64880") != 0, "a positive coordinate formats wrong");
    mesh_ui_settings_coord_text(-635752000, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "-63.57520") != 0, "a negative coordinate formats wrong");
    /* Between -1 and 0 the whole degrees are zero, so the sign has nowhere else to live. */
    mesh_ui_settings_coord_text(-5000000, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "-0.50000") != 0,
                      "a coordinate inside the first degree loses its sign");

    int32_t value = 0;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_coord_parse("44.6488", 90, &value) || value != 446488000,
                      "a plain coordinate should parse");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_coord_parse("-63.57520", 180, &value) ||
                          value != -635752000,
                      "a negative coordinate should parse");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_coord_parse("7", 90, &value) || value != 70000000,
                      "a whole number of degrees should parse");
    /* Round trip, which is the property that actually matters: what the row showed is what
       the radio gets back when nobody edits it. */
    mesh_ui_settings_coord_text(-5000000, text, sizeof text);
    MESH_TEST_FAIL_IF(!mesh_ui_settings_coord_parse(text, 90, &value) || value != -5000000,
                      "a formatted coordinate should parse back to itself");

    /* A bare dot, with or without a sign, would otherwise read as zero - and a zero the
       (0, 0) guard downstream cannot catch, because the other coordinate is real. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_coord_parse(".", 90, &value) ||
                          mesh_ui_settings_coord_parse("-.", 90, &value) ||
                          mesh_ui_settings_coord_parse("+.", 90, &value) ||
                          mesh_ui_settings_coord_parse("", 90, &value) ||
                          mesh_ui_settings_coord_parse("44.6N", 90, &value) ||
                          mesh_ui_settings_coord_parse("north", 90, &value) ||
                          mesh_ui_settings_coord_parse("91", 90, &value) ||
                          mesh_ui_settings_coord_parse("-90.5", 90, &value) ||
                          mesh_ui_settings_coord_parse("181", 180, &value),
                      "rubbish and out-of-range coordinates should be refused");
    /* A longitude is not a latitude: the same number passes one and fails the other. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_coord_parse("120.0", 180, &value) ||
                          mesh_ui_settings_coord_parse("120.0", 90, &value),
                      "the range limit is not being applied");

    record_success(test_name);
}

/*
 * When an SNR may be drawn rather than merely printed.
 *
 * The rule the signal staircase and the node detail's SNR bar are both held to, and the reason
 * it is a function rather than a condition written twice: a reading that describes the last
 * relay, or a reading that was never taken, is a true number about something else. Printing it
 * is unhelpful; drawing it is a claim, and the two failures below are the ones that look like
 * a good link rather than like a bug.
 */
MESH_TEST_CASE(node_detail_signal_heard, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x1234U;
    node.in_nodedb = true;
    node.has_hops_away = true;
    node.hops_away = 0U;
    node.snr = 4.5f;
    MESH_TEST_FAIL_IF(!mesh_ui_node_signal_heard(&node),
                      "a node heard directly with a real reading should be drawable");

    /* Unknown is not zero. Older firmware and replayed NodeDB entries leave hop metadata unset,
       and reading that as "zero hops" hands a possibly-relayed node a staircase. */
    node.has_hops_away = false;
    MESH_TEST_FAIL_IF(mesh_ui_node_signal_heard(&node),
                      "a node whose hop count the firmware never reported was drawn as direct");

    node.has_hops_away = true;
    node.hops_away = 2U;
    MESH_TEST_FAIL_IF(mesh_ui_node_signal_heard(&node),
                      "a relayed node's SNR describes the relay and must not be drawn");

    node.hops_away = 0U;
    node.via_mqtt = true;
    MESH_TEST_FAIL_IF(mesh_ui_node_signal_heard(&node),
                      "a node arriving over MQTT was never on the air and must not be drawn");

    /* An SNR of exactly 0.0 is the session layer's own "no measurement" - it declines to store
       a zero - so a zeroed record must not read as the middling link 0 dB bands to. */
    node.via_mqtt = false;
    node.snr = 0.0f;
    MESH_TEST_FAIL_IF(mesh_ui_node_signal_heard(&node),
                      "a node with no reading behind its figure was drawn as a good link");

    /* And the pair that makes the point: the same record with a real reading is drawable, so
       the test is about the measurement rather than about the node. */
    node.snr = -12.0f;
    MESH_TEST_FAIL_IF(!mesh_ui_node_signal_heard(&node),
                      "a weak but real reading should still be drawable");
    MESH_TEST_FAIL_IF(mesh_ui_node_signal_heard(NULL), "a missing node answered yes");
    record_success(test_name);
}

/*
 * The four sensor groups a node can report beyond device metrics and environment, and the row
 * budget they all have to fit inside.
 *
 * The budget is the point of the test. rows_next() drops silently past MESH_UI_NODE_ITEMS_MAX,
 * so a group added without raising the cap does not fail - it takes the last rows of whatever
 * group happens to be built last off the screen, which nothing would notice. Building the
 * worst case on purpose is what turns that into a failing assertion.
 */
MESH_TEST_CASE(node_detail_row_budget, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x6001U;
    snprintf(node.long_name, sizeof node.long_name, "Everything Sensor");
    snprintf(node.short_name, sizeof node.short_name, "ALL");
    node.last_heard = 1750000000U;
    node.snr = 3.25f;
    node.has_user = true;
    node.has_hops_away = true;
    node.hops_away = 3U;
    node.hw_model = 9U;
    node.role = 1U;
    node.is_licensed = true;
    node.public_key_len = 32U;

    node.metrics.valid = true;
    node.metrics.has_battery = true;
    node.metrics.battery_level = 64U;
    node.metrics.has_voltage = true;
    node.metrics.has_channel_utilization = true;
    node.metrics.has_air_util_tx = true;
    node.metrics.has_uptime = true;

    node.position.valid = true;
    node.position.has_altitude = true;
    node.position.sats_in_view = 9U;
    node.position.precision_bits = 32U;

    node.environment.valid = true;
    node.environment.has_temperature = true;
    node.environment.has_humidity = true;
    node.environment.has_pressure = true;
    node.environment.has_iaq = true;
    node.environment.has_lux = true;
    node.environment.has_voltage = true;
    node.environment.has_current = true;

    node.power.valid = true;
    for (size_t ch = 0; ch < sizeof node.power.channel / sizeof node.power.channel[0]; ++ch) {
        node.power.channel[ch].has_voltage = true;
        node.power.channel[ch].voltage = 3.7f + (float)ch;
        node.power.channel[ch].has_current = true;
        node.power.channel[ch].current = 120.0f;
    }

    node.air_quality.valid = true;
    node.air_quality.has_pm10 = true;
    node.air_quality.pm10_standard = 4U;
    node.air_quality.has_pm25 = true;
    node.air_quality.pm25_standard = 12U;
    node.air_quality.has_pm100 = true;
    node.air_quality.pm100_standard = 18U;
    node.air_quality.has_co2 = true;
    node.air_quality.co2 = 812U;
    node.air_quality.has_voc_index = true;
    node.air_quality.voc_index = 103.0f;
    node.air_quality.has_nox_index = true;
    node.air_quality.nox_index = 1.0f;

    node.health.valid = true;
    node.health.has_heart_bpm = true;
    node.health.heart_bpm = 62U;
    node.health.has_spo2 = true;
    node.health.spo2 = 98U;
    node.health.has_temperature = true;
    node.health.temperature = 36.6f;

    node.host.valid = true;
    node.host.has_uptime = true;
    node.host.uptime_seconds = 90061U;
    node.host.has_freemem = true;
    node.host.freemem_kib = 512U * 1024U;
    node.host.has_diskfree = true;
    node.host.diskfree_mib = 4096U;
    node.host.has_load = true;
    node.host.load1 = 42U;
    node.host.load5 = 137U;
    node.host.load15 = 8U;

    /* A completed trace to this node, at the full length RouteDiscovery allows in both
       directions: the action rows are the largest block on the screen and they are the one
       part that does not depend on the node's hardware. */
    struct mesh_ui_traceroute trace;
    memset(&trace, 0, sizeof trace);
    trace.state = MESH_TRACEROUTE_DONE;
    trace.target = node.node_id;
    trace.completed = 1750000500U;
    trace.forward_count = MESH_UI_TRACEROUTE_MAX_HOPS;
    trace.back_count = MESH_UI_TRACEROUTE_MAX_HOPS;
    for (uint8_t i = 0; i < MESH_UI_TRACEROUTE_MAX_HOPS; ++i) {
        trace.forward[i].node_id = 0x7000U + i;
        trace.forward[i].has_snr = true;
        trace.forward[i].snr_quarter_db = 20;
        snprintf(trace.forward[i].name, sizeof trace.forward[i].name, "hop%u", (unsigned)i);
        trace.back[i] = trace.forward[i];
    }

    /*
     * And the worst case for the two neighbour groups: this node reports the ten out-edges
     * upstream allows, and ten *other* nodes report hearing it. The roster is what both are
     * read from, so it has to be built as well as the node.
     */
    static struct mesh_ui_handshake_state roster;
    memset(&roster, 0, sizeof roster);
    roster.node_count = 1U + MESH_UI_MAX_NEIGHBORS;
    roster.nodes[0] = node;
    roster.nodes[0].neighbors.valid = true;
    roster.nodes[0].neighbors.time = 1750000000U;
    roster.nodes[0].neighbors.count = (uint8_t)MESH_UI_MAX_NEIGHBORS;
    for (uint8_t i = 0; i < MESH_UI_MAX_NEIGHBORS; ++i) {
        roster.nodes[0].neighbors.entries[i].node_id = 0x6100U + i;
        roster.nodes[0].neighbors.entries[i].snr = 5.0f;

        struct mesh_ui_node_summary *peer = &roster.nodes[1U + i];
        peer->node_id = 0x6100U + i;
        snprintf(peer->short_name, sizeof peer->short_name, "N%u", (unsigned)i);
        peer->neighbors.valid = true;
        peer->neighbors.count = 1U;
        peer->neighbors.entries[0].node_id = node.node_id;
        peer->neighbors.entries[0].snr = -2.5f;
    }
    node = roster.nodes[0];

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, &trace, true,
                                                     &roster, NULL, items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count >= MESH_UI_NODE_ITEMS_MAX,
                      "a node reporting everything fills the row budget; raise it");
    MESH_TEST_FAIL_IF(count != mesh_ui_node_detail_count(&node, false, &trace, &roster),
                      "the count the nav walks disagrees with the built list");

    /* Each group is there, and each reading is formatted the way its units are read. */
    struct {
        const char *label;
        const char *value;
        bool seen;
    } expect[] = {
        {"Power", NULL, false},
        {"Channel 1", "3.70 V, 120 mA", false},
        {"Air quality", NULL, false},
        {"PM2.5", "12 ug/m3", false},
        {"PM1 / PM10", "4 / 18 ug/m3", false},
        {"CO2", "812 ppm", false},
        {"Health", NULL, false},
        {"SpO2", "98%", false},
        {"Host", NULL, false},
        {"Free disk", "4.0 GB", false},
        {"Free memory", "512 MB", false},
        /* The load average arrives as the real value times 100 and must not be shown raw. */
        {"Load", "0.42 1.37 0.08", false},
        /* Both neighbour groups, and a neighbour resolved to its name rather than shown as a
           node number - the wire carries only the number. */
        {"Neighbours", NULL, false},
        {"Heard by", NULL, false},
        {"N0", "5.00 dB", false},
    };
    for (uint32_t i = 0; i < count; ++i) {
        for (size_t e = 0; e < sizeof expect / sizeof expect[0]; ++e) {
            if (strcmp(items[i].label, expect[e].label) != 0) {
                continue;
            }
            if (expect[e].value == NULL || strcmp(items[i].value, expect[e].value) == 0) {
                expect[e].seen = true;
            }
        }
    }
    for (size_t e = 0; e < sizeof expect / sizeof expect[0]; ++e) {
        if (!expect[e].seen) {
            char message[128];
            snprintf(message, sizeof message, "row '%s' is missing or misformatted",
                     expect[e].label);
            record_failure(test_name, message);
            return;
        }
    }

    /* And a node that reports none of them shows none of the headings, rather than four empty
       groups - which is the whole reason they are separate groups. */
    struct mesh_ui_node_summary bare;
    memset(&bare, 0, sizeof bare);
    bare.node_id = 0x6002U;
    const uint32_t bare_count = mesh_ui_node_detail_build(
        &bare, false, 1750000600U, NULL, false, NULL, NULL, items, MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < bare_count; ++i) {
        MESH_TEST_FAIL_IF(
            strcmp(items[i].label, "Power") == 0 || strcmp(items[i].label, "Air quality") == 0 ||
                strcmp(items[i].label, "Health") == 0 || strcmp(items[i].label, "Host") == 0 ||
                strcmp(items[i].label, "Neighbours") == 0 ||
                strcmp(items[i].label, "Heard by") == 0,
            "a node with no sensors should show no sensor groups");
    }

    record_success(test_name);
}

/*
 * "Heard by" on a mesh denser than one report can describe.
 *
 * Upstream's ten-entry cap is on what a single node reports about its own neighbours; it says
 * nothing about how many nodes may report hearing this one, which on a dense mesh is everyone
 * in range. Capping the *count* at ten as well would make the one screen whose question is
 * "how many can hear me" answer it wrongly and without saying so.
 */
MESH_TEST_CASE(node_detail_listener_count, unit) {
    static struct mesh_ui_handshake_state roster;
    memset(&roster, 0, sizeof roster);

    const uint32_t subject_id = 0x7001U;
    const uint32_t listeners = MESH_UI_NODE_MAX_LISTENERS + 4U;

    roster.node_count = 1U + listeners;
    roster.nodes[0].node_id = subject_id;
    snprintf(roster.nodes[0].short_name, sizeof roster.nodes[0].short_name, "SUBJ");
    for (uint32_t i = 0; i < listeners; ++i) {
        struct mesh_ui_node_summary *peer = &roster.nodes[1U + i];
        peer->node_id = 0x7100U + i;
        snprintf(peer->short_name, sizeof peer->short_name, "L%u", (unsigned)i);
        peer->neighbors.valid = true;
        peer->neighbors.count = 1U;
        peer->neighbors.entries[0].node_id = subject_id;
        peer->neighbors.entries[0].snr = 2.0f;
    }

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_node_detail_build(&roster.nodes[0], false, 1750000600U, NULL, false, &roster, NULL,
                                  items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count >= MESH_UI_NODE_ITEMS_MAX, "the row budget was filled");

    /* Counted from the heading onwards rather than by label shape: "Last heard" and "Load" are
       both rows on this screen that begin with an L. */
    uint32_t listener_rows = 0U;
    bool saw_heading = false;
    bool saw_remainder = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Heard by") == 0) {
            saw_heading = true;
            continue;
        }
        if (saw_heading && !saw_remainder && items[i].kind == MESH_UI_NODE_ROW_INFO &&
            strcmp(items[i].label, "and more") != 0) {
            listener_rows++;
        }
        if (strcmp(items[i].label, "and more") == 0) {
            saw_remainder = true;
            char expected[32];
            snprintf(expected, sizeof expected, "%u not shown",
                     (unsigned)(listeners - MESH_UI_NODE_MAX_LISTENERS));
            MESH_TEST_FAIL_IF(strcmp(items[i].value, expected) != 0,
                              "the remainder row does not say how many were left out");
        }
    }
    MESH_TEST_FAIL_IF(!saw_heading, "the Heard by group is missing");
    MESH_TEST_FAIL_IF(listener_rows != MESH_UI_NODE_MAX_LISTENERS,
                      "the drawn listeners should stop at the row budget");
    MESH_TEST_FAIL_IF(!saw_remainder,
                      "listeners beyond the row budget were dropped without saying so");

    /* And with the rows exactly filled there is nothing left over to announce. */
    roster.node_count = 1U + MESH_UI_NODE_MAX_LISTENERS;
    const uint32_t exact =
        mesh_ui_node_detail_build(&roster.nodes[0], false, 1750000600U, NULL, false, &roster, NULL,
                                  items, MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < exact; ++i) {
        MESH_TEST_FAIL_IF(strcmp(items[i].label, "and more") == 0,
                          "a full but untruncated list should not claim a remainder");
    }

    record_success(test_name);
}

/*
 * An RSSI is a measurement this radio made. A node heard over RF and then relayed to us over
 * MQTT keeps that reading - it is still true, and clearing it would make the row flicker on a
 * mesh whose bridge relays traffic we also hear ourselves - but the row has to stop claiming to
 * describe the packet that just arrived.
 */
MESH_TEST_CASE(node_detail_rssi_is_stamped, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x7201U;
    snprintf(node.short_name, sizeof node.short_name, "RS");
    node.has_rssi = true;
    node.rx_rssi = -97;
    node.last_heard = 1750000000U;
    node.rssi_time = 1750000000U;

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, false, NULL, NULL,
                                               items, MESH_UI_NODE_ITEMS_MAX);
    bool plain = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "RSSI") == 0 && strcmp(items[i].value, "-97 dBm") == 0) {
            plain = true;
        }
    }
    MESH_TEST_FAIL_IF(!plain, "a reading from the newest packet should carry no age");

    /* Now the node turns up over MQTT: last_heard moves on, the reading does not. */
    node.last_heard = 1750000500U;
    node.via_mqtt = true;
    count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, false, NULL, NULL, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    bool stamped = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "RSSI") == 0) {
            stamped = (strcmp(items[i].value, "-97 dBm, 10m ago") == 0);
        }
    }
    MESH_TEST_FAIL_IF(!stamped,
                      "an RSSI older than the newest packet should say when it was measured");

    record_success(test_name);
}

/*
 * Every section answers with an icon, and the two lists of sections can therefore fill their
 * leading slot on every row.
 *
 * A leading slot is declared for a whole list, so one section with no icon is not one row
 * missing a picture: it is a row whose words start where every other row's icon is, in a list
 * the eye is meant to run straight down. A section added without a line in k_section_icons
 * compiles perfectly happily, which is what this is here to catch.
 */
MESH_TEST_CASE(ui_settings_every_section_has_an_icon, unit) {
    char message[128];
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        if (!mesh_ui_icon_is_valid(mesh_ui_settings_section_icon(section))) {
            snprintf(message, sizeof message, "section %s has no icon",
                     mesh_ui_settings_section_name(section));
            record_failure(test_name, message);
            return;
        }
    }
    /* Out of range is the absence of one rather than whatever sits past the table. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_section_icon(
                          (enum mesh_ui_settings_section)MESH_UI_SETTINGS_SECTION_COUNT) !=
                          MESH_UI_ICON_NONE,
                      "an unknown section should answer with no icon");
    record_success(test_name);
}

/*
 * The invariant struct mesh_ui_settings_item's `icon` is documented with: a section gives every
 * one of its rows an icon or gives none of them one, and mesh_ui_settings_section_icons_rows()
 * is the answer a renderer declares the slot from.
 *
 * The list is built with everything loaded, because a module row that has not been answered for
 * still names its module - and that is the row most likely to be the one that forgets.
 */
MESH_TEST_CASE(ui_settings_row_icons_are_all_or_nothing, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.has_device = true;
    settings.has_display = true;
    settings.has_position = true;
    settings.has_power = true;
    settings.has_bluetooth = true;
    settings.has_security = true;
    settings.has_mqtt = true;
    settings.has_store_forward = true;
    settings.has_telemetry = true;

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.channel_count = 1U;

    char message[160];
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        const bool declared = mesh_ui_settings_section_icons_rows(section);
        const uint32_t count = mesh_ui_settings_item_count(&settings, &handshake, section,
                                                           MESH_UI_SETTINGS_NO_CHANNEL);
        for (uint32_t row = 0; row < count; ++row) {
            struct mesh_ui_settings_item item;
            if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, section,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
                break;
            }
            if (mesh_ui_icon_is_valid(item.icon) != declared) {
                snprintf(message, sizeof message,
                         "%s row %u %s an icon, and the section says it %s",
                         mesh_ui_settings_section_name(section), row,
                         mesh_ui_icon_is_valid(item.icon) ? "has" : "has no",
                         declared ? "should" : "should not");
                record_failure(test_name, message);
                return;
            }
        }
    }
    record_success(test_name);
}

/*
 * The slider's model: where a NUMBER field's value sits on the field's own scale.
 *
 * Four claims, and each one is a way the picture could have been wrong. The ends are the ends.
 * A geometric preset list is walked in *stop* space, so the middle preset is halfway along -
 * value space would put screen-on's 10m at a sixth of a track that runs to an hour. A value the
 * list does not contain lands between the two stops it falls between rather than being refused,
 * which is the whole of what an axis can do that a set of alternatives cannot. And a field whose
 * numbers name something has no scale at all.
 */
MESH_TEST_CASE(ui_settings_number_track_places_a_value, unit) {
    struct mesh_ui_settings_track track;

    /* {0, 10, 15, 30, 60, 120, 300, 600}, and the 0 is "off" - a true bottom, on the scale. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_DISPLAY_CAROUSEL, 0U, &track),
                      "a carousel interval is a scale and should place a value");
    MESH_TEST_FAIL_IF(track.unplaced, "\"off\" is the bottom of this scale, not off it");
    MESH_TEST_FAIL_IF(track.position != 0, "the first preset should sit at the start of the track");
    MESH_TEST_FAIL_IF(track.stops != 8U, "the carousel offers eight choices to mark");

    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_DISPLAY_CAROUSEL, 600U, &track),
                      "the last preset should place");
    MESH_TEST_FAIL_IF(track.position != 1000, "the last preset should sit at the end of the track");

    /* The fourth of eight stops: three sevenths along, not 30/600 of the way. A value-space
       placement would answer 50, which is what makes this the assertion worth writing. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_DISPLAY_CAROUSEL, 30U, &track),
                      "a listed preset should place");
    MESH_TEST_FAIL_IF(track.position != 3 * 1000 / 7,
                      "the stops are evenly spaced, so the fourth of eight is three sevenths in");

    /* 45 is not on the list: half of the way from 30 to 60, so half a stop past the fourth. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_DISPLAY_CAROUSEL, 45U, &track),
                      "a value between two presets should still place");
    MESH_TEST_FAIL_IF(track.unplaced, "an axis has room between its stops; nothing is refused");
    MESH_TEST_FAIL_IF(track.position != (3 * 1000 + 500) / 7,
                      "a value between two stops belongs between them");

    /* A GPIO pin is a name written as a number. Nothing about pin 24 is two thirds of anything. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_number_track(MESH_UI_FIELD_DETECT_PIN, 24U, &track),
                      "a GPIO pin is not a magnitude and must not be drawn as a length");
    MESH_TEST_FAIL_IF(mesh_ui_settings_number_track(MESH_UI_FIELD_LORA_SPREAD, 10U, &track),
                      "a spreading factor is an alternative, not a quantity");
    MESH_TEST_FAIL_IF(mesh_ui_settings_number_track(MESH_UI_FIELD_DEVICE_ROLE, 1U, &track),
                      "only a NUMBER field has a preset scale");
    record_success(test_name);
}

/*
 * A value below the bottom stop is off the track, not at the bottom of it.
 *
 * LoRa's transmit power is the case that found this: 0 means "as much as this radio has", and
 * the first version of the slider drew it with the handle hard left - "max", reported at the
 * empty end of its own bar. Every "default" is the same mistake more quietly, because a value
 * the firmware picks is not the shortest interval, it is an interval nobody here knows. And two
 * lists reach it without any word at all, simply by starting above zero.
 */
MESH_TEST_CASE(ui_settings_number_track_refuses_what_it_cannot_place, unit) {
    struct mesh_ui_settings_track track;

    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_LORA_TX_POWER, 0U, &track),
                      "transmit power is a scale; the row still draws one");
    MESH_TEST_FAIL_IF(!track.unplaced, "\"max\" is not a point on a power scale");
    MESH_TEST_FAIL_IF(track.stops == 0U,
                      "an unplaced value still has a track to draw its stops on");

    /* And the rest of the same list is a scale: 2 dBm is its bottom, 30 its top. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_LORA_TX_POWER, 2U, &track),
                      "a stated power should place");
    MESH_TEST_FAIL_IF(track.unplaced || track.position != 0,
                      "the lowest stated power is the bottom of the track");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_LORA_TX_POWER, 30U, &track),
                      "the highest stated power should place");
    MESH_TEST_FAIL_IF(track.position != 1000, "the highest stated power is the top of the track");

    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_number_track(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 0U, &track) ||
            !track.unplaced,
        "a screen timeout the firmware picks is not the shortest one this client offers");

    /*
     * And the same at the other source of an off-track value: a list that simply starts above
     * zero. The public map drops a report under an hour and the firmware floors neighbour info at
     * four, so those presets begin there - while a radio nobody has configured reports 0 for
     * both, and MQTT's map settings are an optional submessage that is absent far more often than
     * it is set. Nothing stands a zero aside on either field; the value is under the bottom stop,
     * which is the whole of the test.
     */
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_number_track(MESH_UI_FIELD_MQTT_MAP_INTERVAL, 0U, &track) ||
            !track.unplaced,
        "a map report interval of nothing at all is not the shortest one the map will accept");
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_number_track(MESH_UI_FIELD_MQTT_MAP_INTERVAL, 3600U, &track) ||
            track.unplaced || track.position != 0,
        "the first preset is the bottom of the track, not off it");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_number_track(MESH_UI_FIELD_NEIGHBOR_INTERVAL, 0U, &track) ||
                          !track.unplaced,
                      "a neighbour interval below the firmware's own floor is not four hours");
    record_success(test_name);
}

/*
 * Every scale runs the length of its own track, in one direction, with nothing unplaced in the
 * middle of it.
 *
 * A table invariant rather than a behaviour, walked over every field the client has. Three ways
 * a preset list can be malformed and none of them is visible from the entry that declares it: a
 * list that does not climb draws a handle that goes backwards when the reader presses Right; a
 * list whose zero was stood aside but which never had one loses its first real choice off the
 * end of the track; and a list too short to have two stops is a point rather than an axis. Each
 * would show up on exactly one section of one screen, which is the kind of thing a device gets
 * shipped with.
 */
MESH_TEST_CASE(ui_settings_number_scales_are_well_formed, unit) {
    char message[192];
    for (int f = 0; f < (int)MESH_UI_FIELD_COUNT; ++f) {
        const enum mesh_ui_setting_field field = (enum mesh_ui_setting_field)f;
        struct mesh_ui_settings_track track;
        if (!mesh_ui_settings_number_track(field, 0U, &track)) {
            continue;
        }
        if (track.stops < 2U) {
            snprintf(message, sizeof message, "%s draws a track with %u stops on it",
                     mesh_ui_settings_field_label(field), (unsigned)track.stops);
            record_failure(test_name, message);
            return;
        }
        /* Up the preset list with the same walk Right does, from the bottom to wherever it
           stops. Only the field's own zero may be unplaced, and only if it is the first thing
           the walk sees. */
        uint32_t value = 0U;
        int32_t last = -1;
        uint32_t seen = 0U;
        uint32_t unplaced = 0U;
        for (;;) {
            if (!mesh_ui_settings_number_track(field, value, &track)) {
                record_failure(test_name, "a field stopped being a scale mid-walk");
                return;
            }
            if (track.unplaced) {
                /* At most one, and it must be the first thing the walk sees. Two would mean a
                   list marked SCALE_PRESETS_AFTER_ZERO() that does not in fact start at 0, which
                   silently drops its first real choice off the bottom of the track. */
                if (seen > 0U || ++unplaced > 1U) {
                    snprintf(message, sizeof message, "%s has an unplaced value inside its scale",
                             mesh_ui_settings_field_label(field));
                    record_failure(test_name, message);
                    return;
                }
            } else {
                if (track.position < last) {
                    snprintf(message, sizeof message, "%s steps up and its handle goes backwards",
                             mesh_ui_settings_field_label(field));
                    record_failure(test_name, message);
                    return;
                }
                last = track.position;
                ++seen;
            }
            const uint32_t next = mesh_ui_settings_number_step(field, value, 1);
            if (next == value) {
                break;
            }
            value = next;
        }
        /* Stepping up until it stops has to arrive at the end of the track. A list whose top
           preset placed anywhere short of it would draw a control the reader cannot fill. */
        if (seen < 2U || last != 1000) {
            snprintf(message, sizeof message,
                     "%s walks %u placed values and ends at %d rather than the end of its track",
                     mesh_ui_settings_field_label(field), (unsigned)seen, (int)last);
            record_failure(test_name, message);
            return;
        }
    }
    record_success(test_name);
}
