#define _POSIX_C_SOURCE 200809L

/* The settings model itself: rows, edits, key text, coordinates, About. */

#include "inkcell/ui/layout.h"
#include "inkwell/base/text.h"

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/firmware.h"
#include "mesh/core/firmware_update.h"
#include "mesh/core/radio_settings.h"
/* For enum mesh_traceroute_state, which the UI's traceroute carries as a byte. */
#include "mesh/core/session.h"
#include "mesh/core/updater.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"

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
                          item.kind != INKSTAND_FORM_ENUM,
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
            item.kind != INKSTAND_FORM_KEY || strncmp(item.value, "deadbeef...", 11U) != 0 ||
            strstr(item.value, "32 bytes") == NULL,
        "public key fingerprint is wrong");
    /*
     * What a slot says in the list is what it is and how it is keyed, and *not* its two MQTT
     * bits. Four facts do not fit a settings value column - every row of the list came out cut
     * mid-word, "secondary, AES-128, up" - so the pair that was never readable came off the row
     * and the uplink and downlink toggles in the slot's own section are where they are read.
     * Asserted as an absence as well as a presence, because the failure this is about is a
     * value that fits nowhere rather than a value that is missing.
     */
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 2U ||
            !mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
            strcmp(item.label, "1 Team") != 0 || strstr(item.value, "AES-128") == NULL ||
            strstr(item.value, "secondary") == NULL || strstr(item.value, "up") != NULL ||
            strstr(item.value, "down") != NULL,
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
                          item.kind != INKSTAND_FORM_TEXT || item.dirty ||
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
                          item.kind != INKSTAND_FORM_NUMBER || item.number != 60U ||
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
            mesh_ui_settings_field_kind(MESH_UI_FIELD_TELEMETRY_INTERVAL) != INKSTAND_FORM_NUMBER ||
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
                          item.kind != INKSTAND_FORM_HEADING || item.field != MESH_UI_FIELD_NONE ||
                          item.value[0] != '\0' || strcmp(item.label, "Device") != 0,
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
    for (uint32_t i = 0; i < mesh_ui_settings_root_count(NULL); ++i) {
        const enum mesh_ui_settings_section section = mesh_ui_settings_root_at(NULL, i);
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
                          item.kind != INKSTAND_FORM_ACTION ||
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
 * Every section present, so a walk over all of them builds every row rather than the empty
 * screen behind a section the radio has not sent.
 *
 * Written out member by member rather than by memset-ing the struct to 0xFF: the rows read the
 * values too, and a section whose enum fields are all 255 builds rows about nothing. What is
 * wanted here is the shape of the list, which is what `has_*` decides.
 */
/*
 * The row carrying a field, found rather than counted.
 *
 * `mesh_test_settings_cursor_to()` is this rule for the nav; a test reading a row straight out
 * of a section needs it for the same reason - a heading added above a row moves its index and
 * says nothing, so a test that counted would fail somewhere other than where the change was.
 */
static bool settings_find_field_edited(const struct mesh_ui_settings *settings,
                                       const struct mesh_ui_setting_edit *edits, size_t edit_count,
                                       enum mesh_ui_settings_section section,
                                       enum mesh_ui_setting_field field,
                                       struct mesh_ui_settings_item *out) {
    const uint32_t rows =
        mesh_ui_settings_item_count(settings, NULL, section, MESH_UI_SETTINGS_NO_CHANNEL);
    for (uint32_t row = 0; row < rows; ++row) {
        if (mesh_ui_settings_item(settings, NULL, edits, edit_count, section,
                                  MESH_UI_SETTINGS_NO_CHANNEL, row, out) &&
            out->field == field) {
            return true;
        }
    }
    return false;
}

static bool settings_find_field(const struct mesh_ui_settings *settings,
                                enum mesh_ui_settings_section section,
                                enum mesh_ui_setting_field field,
                                struct mesh_ui_settings_item *out) {
    return settings_find_field_edited(settings, NULL, 0U, section, field, out);
}

static void settings_mark_all_present(struct mesh_ui_settings *settings) {
    settings->has_owner = true;
    settings->has_device = true;
    settings->has_display = true;
    settings->has_lora = true;
    settings->has_bluetooth = true;
    settings->has_network = true;
    settings->has_security = true;
    settings->has_position = true;
    settings->has_power = true;
    settings->has_mqtt = true;
    settings->has_store_forward = true;
    settings->has_telemetry = true;
    settings->has_neighbor_info = true;
    settings->has_range_test = true;
    settings->has_paxcounter = true;
    settings->has_tak = true;
    settings->has_ambient_lighting = true;
    settings->has_status_message = true;
    settings->has_detection_sensor = true;
    settings->has_external_notification = true;
    settings->has_traffic_management = true;
    settings->has_mesh_beacon = true;
    settings->has_ui_config = true;
    settings->has_canned_messages = true;
    settings->has_metadata = true;
    settings->has_channels = true;
    for (size_t i = 0; i < MESH_UI_MAX_CHANNELS; ++i) {
        settings->channels[i].present = true;
        settings->channels[i].index = (uint8_t)i;
    }
    /* A fixed position on, so the section offers the Clear row as well - the widest the list
       gets is the count the edit list has to carry. */
    settings->fixed_position = true;
    settings->has_own_position = true;
}

/*
 * Every label a settings row can carry, in every language, against the buffer it is copied into.
 *
 * `item_add_named()` copies with snprintf, so a label wider than MESH_UI_SETTINGS_LABEL_MAX is
 * cut and nothing anywhere says so - the same silent shortening the edit buffer's own test was
 * written for, one layer up and in a place only a translator can reach. English fits by
 * construction because the row was drawn beside it; a translation is written against a catalog
 * file with no screen in front of it, and "Enviado con la posición" is one byte over.
 *
 * Keyed on the id's *name*, the way the Spanish completeness check is: an id becomes a row's
 * label by being called SETTINGS_FIELD_* or HEAD_*, so that is what is measured. A label that
 * exceeds the buffer names itself, because "a label is too long" over eleven hundred ids is a
 * bisect rather than a failure message.
 */
MESH_TEST_CASE(ui_settings_labels_fit_the_row_in_every_language, unit) {
    for (size_t l = 0; l < inkcell_i18n_locale_count(); ++l) {
        const struct inkcell_i18n_locale *locale = inkcell_i18n_locale_at(l);
        if (locale == NULL) {
            continue;
        }
        for (int id = 0; id < (int)MESH_STR_COUNT; ++id) {
            const char *name = inkcell_str_id_name((inkcell_str_id)id);
            if (name == NULL ||
                (strncmp(name, "SETTINGS_FIELD_", 15) != 0 && strncmp(name, "HEAD_", 5) != 0)) {
                continue;
            }
            const char *text = inkcell_str_in(locale, (inkcell_str_id)id);
            if (text == NULL || strlen(text) < MESH_UI_SETTINGS_LABEL_MAX) {
                continue;
            }
            char reason[200];
            snprintf(reason, sizeof reason, "%s in %s is %u bytes against a row label of %u", name,
                     locale->id, (unsigned)strlen(text), (unsigned)MESH_UI_SETTINGS_LABEL_MAX - 1U);
            record_failure(test_name, reason);
            return;
        }
    }
    record_success(test_name);
}

/*
 * The flag row model: ten rows over one word, and the masks pinned against the protobuf.
 *
 * Written against meshtastic_Config_PositionConfig_PositionFlags rather than against the
 * numbers in k_fields, for the reason the excluded-modules table is: the UI layer is the
 * nanopb-free side of the fence, so the literals it holds have to be checked somewhere that
 * can see both. A renumbering upstream fails here rather than on a radio.
 *
 * The second half is the one that would be missed: every field of the group has to name a
 * *different* bit, and together they have to cover the whole word. A copy-pasted row carrying
 * its neighbour's mask draws two rows that move together, which reads on screen as a control
 * that does not work rather than as a mistake in a table.
 */
MESH_TEST_CASE(ui_settings_position_flags_are_the_wire_bits, unit) {
    static const struct {
        enum mesh_ui_setting_field field;
        uint32_t bit;
    } k_expected[] = {
        {MESH_UI_FIELD_POSITION_FLAG_ALTITUDE,
         meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE},
        {MESH_UI_FIELD_POSITION_FLAG_ALTITUDE_MSL,
         meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE_MSL},
        {MESH_UI_FIELD_POSITION_FLAG_GEOIDAL,
         meshtastic_Config_PositionConfig_PositionFlags_GEOIDAL_SEPARATION},
        {MESH_UI_FIELD_POSITION_FLAG_DOP, meshtastic_Config_PositionConfig_PositionFlags_DOP},
        {MESH_UI_FIELD_POSITION_FLAG_HVDOP, meshtastic_Config_PositionConfig_PositionFlags_HVDOP},
        {MESH_UI_FIELD_POSITION_FLAG_SATINVIEW,
         meshtastic_Config_PositionConfig_PositionFlags_SATINVIEW},
        {MESH_UI_FIELD_POSITION_FLAG_SEQ_NO, meshtastic_Config_PositionConfig_PositionFlags_SEQ_NO},
        {MESH_UI_FIELD_POSITION_FLAG_TIMESTAMP,
         meshtastic_Config_PositionConfig_PositionFlags_TIMESTAMP},
        {MESH_UI_FIELD_POSITION_FLAG_HEADING,
         meshtastic_Config_PositionConfig_PositionFlags_HEADING},
        {MESH_UI_FIELD_POSITION_FLAG_SPEED, meshtastic_Config_PositionConfig_PositionFlags_SPEED},
    };
    const uint32_t count = mesh_ui_settings_group_count(MESH_UI_FIELD_GROUP_POSITION_FLAGS);
    MESH_TEST_FAIL_IF(count != (uint32_t)(sizeof k_expected / sizeof k_expected[0]),
                      "the position flag group and this test disagree about how many bits");
    uint32_t seen = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        const enum mesh_ui_setting_field field =
            mesh_ui_settings_group_field(MESH_UI_FIELD_GROUP_POSITION_FLAGS, i);
        MESH_TEST_FAIL_IF(field != k_expected[i].field,
                          "the position flag group is not in the order the test expects");
        const uint32_t bit = mesh_ui_settings_field_bit(field);
        MESH_TEST_FAIL_IF(bit != k_expected[i].bit,
                          "a position flag row does not carry the protobuf's own bit");
        MESH_TEST_FAIL_IF(mesh_ui_settings_field_kind(field) != INKSTAND_FORM_FLAG,
                          "a row in a flag group is not a flag");
        MESH_TEST_FAIL_IF((seen & bit) != 0U, "two position flag rows claim the same bit");
        seen |= bit;
    }
    MESH_TEST_FAIL_IF(seen != 0x03FFU, "the position flag rows do not cover the whole word");
    /* And nothing outside a group is a flag: a field given the kind and left out of the table
       would be a row the write builder has no mask for. */
    for (int i = 0; i < (int)MESH_UI_FIELD_COUNT; ++i) {
        const enum mesh_ui_setting_field field = (enum mesh_ui_setting_field)i;
        if (mesh_ui_settings_field_kind(field) != INKSTAND_FORM_FLAG) {
            MESH_TEST_FAIL_IF(mesh_ui_settings_field_bit(field) != 0U,
                              "a field that is not a flag answers with a bit");
            continue;
        }
        bool grouped = false;
        for (int g = 0; g < (int)MESH_UI_FIELD_GROUP_COUNT && !grouped; ++g) {
            const enum mesh_ui_setting_field_group group = (enum mesh_ui_setting_field_group)g;
            for (uint32_t n = 0; n < mesh_ui_settings_group_count(group) && !grouped; ++n) {
                grouped = mesh_ui_settings_group_field(group, n) == field;
            }
        }
        MESH_TEST_FAIL_IF(!grouped, "a flag field belongs to no group");
        MESH_TEST_FAIL_IF(mesh_ui_settings_field_bit(field) == 0U, "a flag field has no bit");
    }
    record_success(test_name);
}

/*
 * The ten rows read their own bit out of the word, and say "on" and "off" like any toggle.
 *
 * The value column matters as much as the control: the fb backend draws a checkbox and the CLI
 * backend draws the words, so a flag that only said anything through the square would be a
 * setting one of the two backends could not report at all.
 */
MESH_TEST_CASE(ui_settings_position_flag_rows, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_position = true;
    /* Altitude, precision and the fix time on; everything else off. */
    settings.position_flags = meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE |
                              meshtastic_Config_PositionConfig_PositionFlags_DOP |
                              meshtastic_Config_PositionConfig_PositionFlags_TIMESTAMP;

    const uint32_t rows = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_POSITION,
                                                      MESH_UI_SETTINGS_NO_CHANNEL);
    uint32_t heading_row = rows;
    for (uint32_t row = 0; row < rows; ++row) {
        struct mesh_ui_settings_item item;
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_POSITION,
                                  MESH_UI_SETTINGS_NO_CHANNEL, row, &item) &&
            item.kind == INKSTAND_FORM_HEADING &&
            strcmp(item.label, inkcell_str(MESH_STR_HEAD_POSITION_CARRIES)) == 0) {
            heading_row = row;
            break;
        }
    }
    MESH_TEST_FAIL_IF(heading_row >= rows, "the position flags have no heading over them");
    const uint32_t count = mesh_ui_settings_group_count(MESH_UI_FIELD_GROUP_POSITION_FLAGS);
    MESH_TEST_FAIL_IF(heading_row + count >= rows, "the flag rows do not fit under the heading");
    for (uint32_t i = 0; i < count; ++i) {
        const enum mesh_ui_setting_field field =
            mesh_ui_settings_group_field(MESH_UI_FIELD_GROUP_POSITION_FLAGS, i);
        struct mesh_ui_settings_item item;
        MESH_TEST_FAIL_IF(
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_POSITION,
                                   MESH_UI_SETTINGS_NO_CHANNEL, heading_row + 1U + i, &item),
            "a flag row is missing from under the heading");
        MESH_TEST_FAIL_IF(item.field != field, "the flag rows are not in the group's order");
        const bool on = (settings.position_flags & mesh_ui_settings_field_bit(field)) != 0U;
        MESH_TEST_FAIL_IF(item.number != (on ? 1U : 0U),
                          "a flag row does not carry the state of its own bit");
        MESH_TEST_FAIL_IF(strcmp(item.value, on ? "on" : "off") != 0,
                          "a flag row should say on or off like any other boolean");
    }
    record_success(test_name);
}

/*
 * The record group: twelve fields, four records, and the arithmetic both ends depend on.
 *
 * The row builder and the write builder each cut the group's run into records by dividing by
 * MESH_UI_BEACON_TARGET_FIELDS, so what they need is not merely that twelve fields exist - it
 * is that they are contiguous, in the enum's own order, and repeat one shape. A field inserted
 * in the middle of the run moves every target after it, silently and identically at both ends,
 * which is a bug with no symptom until a radio reads back the wrong destination.
 */
MESH_TEST_CASE(ui_settings_beacon_targets_are_four_records_of_one_shape, unit) {
    static const enum inkstand_form_kind k_shape[MESH_UI_BEACON_TARGET_FIELDS] = {
        INKSTAND_FORM_ENUM,   /* preset */
        INKSTAND_FORM_ENUM,   /* region */
        INKSTAND_FORM_NUMBER, /* channel */
    };
    const uint32_t count = mesh_ui_settings_group_count(MESH_UI_FIELD_GROUP_BEACON_TARGETS);
    MESH_TEST_FAIL_IF(count != MESH_UI_BEACON_TARGETS * MESH_UI_BEACON_TARGET_FIELDS,
                      "the beacon target group is not four records of three rows");
    const enum mesh_ui_setting_field first =
        mesh_ui_settings_group_field(MESH_UI_FIELD_GROUP_BEACON_TARGETS, 0U);
    MESH_TEST_FAIL_IF(first != MESH_UI_FIELD_BEACON_TARGET_0_PRESET,
                      "the beacon target group does not start where the enum says");
    for (uint32_t i = 0; i < count; ++i) {
        const enum mesh_ui_setting_field field =
            mesh_ui_settings_group_field(MESH_UI_FIELD_GROUP_BEACON_TARGETS, i);
        MESH_TEST_FAIL_IF(field != (enum mesh_ui_setting_field)((uint32_t)first + i),
                          "the beacon target run is not contiguous");
        MESH_TEST_FAIL_IF(mesh_ui_settings_field_kind(field) !=
                              k_shape[i % MESH_UI_BEACON_TARGET_FIELDS],
                          "a beacon target record does not repeat the shape the others have");
        MESH_TEST_FAIL_IF(mesh_ui_settings_field_section(field) != MESH_UI_SETTINGS_BEACON,
                          "a beacon target row belongs to another section");
    }
    /*
     * A target's preset row is the LoRa section's list plus the one value that stands for "no
     * preset here", which is what lets an `optional` wire field be an ordinary contiguous enum.
     * Written as the relationship rather than as 18, because 17 is upstream's number and the
     * two rows have to move together when it changes.
     */
    MESH_TEST_FAIL_IF(mesh_ui_settings_enum_count(MESH_UI_FIELD_BEACON_TARGET_0_PRESET) !=
                              mesh_ui_settings_enum_count(MESH_UI_FIELD_LORA_PRESET) + 1U ||
                          mesh_ui_settings_enum_count(MESH_UI_FIELD_BEACON_OFFER_PRESET) !=
                              mesh_ui_settings_enum_count(MESH_UI_FIELD_LORA_PRESET) + 1U,
                      "a beacon preset row is not the modem presets plus the absent one");

    /* The three flags are the group model's first reading, still: one word, three bits. */
    uint32_t seen = 0U;
    for (uint32_t i = 0; i < mesh_ui_settings_group_count(MESH_UI_FIELD_GROUP_BEACON_FLAGS); ++i) {
        const uint32_t bit = mesh_ui_settings_field_bit(
            mesh_ui_settings_group_field(MESH_UI_FIELD_GROUP_BEACON_FLAGS, i));
        MESH_TEST_FAIL_IF((seen & bit) != 0U, "two beacon flag rows claim the same bit");
        seen |= bit;
    }
    MESH_TEST_FAIL_IF(
        seen != (uint32_t)(meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LISTEN_ENABLED |
                           meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_BROADCAST_ENABLED |
                           meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LEGACY_SPLIT),
        "the beacon flag rows do not cover the protobuf's own three bits");
    record_success(test_name);
}

/*
 * The beacon section as it is read: four numbered target groups, and the three ways a row says
 * a value is absent.
 *
 * The second half is the point. Absent is a value here - the wire's `optional` for two of the
 * three rows and RegionCode's own UNSET for the other - so it has to *show* as something, and
 * showing as "Unset" beside two rows reading "as configured" would be a record that looks like
 * it is holding a mistake. The offer says the other thing, because an offer that names no
 * preset is not offering one rather than falling back to anything.
 */
MESH_TEST_CASE(ui_settings_beacon_rows, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_mesh_beacon = true;
    settings.beacon_flags = meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_BROADCAST_ENABLED;
    settings.beacon_interval_secs = 7200U;
    snprintf(settings.beacon_message, sizeof settings.beacon_message, "Anyone out there?");
    /* Target 2 names a channel; every other row of every other target holds nothing. */
    settings.beacon_targets[1].channel = 4U; /* one past itself: channel 3 */

    const uint32_t rows = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_BEACON,
                                                      MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(rows > MESH_UI_SETTINGS_ITEMS_MAX,
                      "the beacon section does not fit the item list");

    uint32_t headings = 0U;
    uint32_t target_rows = 0U;
    bool saw_broadcast_on = false;
    bool saw_named_channel = false;
    const char *const absent_target = inkcell_str(MESH_STR_ENUM_BEACON_AS_RUNNING);
    const char *const absent_offer = inkcell_str(MESH_STR_ENUM_BEACON_NOT_OFFERED);
    for (uint32_t row = 0; row < rows; ++row) {
        struct mesh_ui_settings_item item;
        MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BEACON,
                                                 MESH_UI_SETTINGS_NO_CHANNEL, row, &item),
                          "a beacon row is missing");
        if (item.kind == INKSTAND_FORM_HEADING) {
            char expect[MESH_UI_SETTINGS_LABEL_MAX];
            inkcell_str_format(expect, sizeof expect, MESH_STR_HEAD_BEACON_TARGET,
                               (unsigned)(headings + 1U));
            if (strcmp(item.label, expect) == 0) {
                headings++;
            }
            continue;
        }
        if (item.field == MESH_UI_FIELD_BEACON_BROADCAST) {
            saw_broadcast_on = item.number == 1U && strcmp(item.value, "on") == 0;
        }
        /* The offer's two enum rows, which the radio left empty: absent there is "not offered",
           because an offer naming no preset is not offering one. */
        if (item.field == MESH_UI_FIELD_BEACON_OFFER_REGION ||
            item.field == MESH_UI_FIELD_BEACON_OFFER_PRESET) {
            MESH_TEST_FAIL_IF(strcmp(item.value, absent_offer) != 0,
                              "an offer row with nothing in it should say it is not offered");
        }
        if (item.field < MESH_UI_FIELD_BEACON_TARGET_0_PRESET) {
            continue;
        }
        target_rows++;
        if (item.field == MESH_UI_FIELD_BEACON_TARGET_1_CHANNEL) {
            /* Stored one past itself and shown as itself, which is the whole of the shift. */
            saw_named_channel = strcmp(item.value, "3") == 0;
            continue;
        }
        /* Every other target row is empty, and all three rows of a record say so identically -
           a "Unset" between two "as configured" reads as a record holding a mistake. */
        MESH_TEST_FAIL_IF(strcmp(item.value, absent_target) != 0,
                          "every row of an empty target should read the same way");
    }
    MESH_TEST_FAIL_IF(headings != MESH_UI_BEACON_TARGETS,
                      "the four targets are not four numbered groups");
    MESH_TEST_FAIL_IF(target_rows != MESH_UI_BEACON_TARGETS * MESH_UI_BEACON_TARGET_FIELDS,
                      "an empty target slot should still be listed");
    MESH_TEST_FAIL_IF(!saw_broadcast_on, "a beacon flag row does not read its own bit");
    MESH_TEST_FAIL_IF(!saw_named_channel,
                      "a target's channel is stored one past itself and shown as itself");
    record_success(test_name);
}

/*
 * The beacon's preset rows answer to the region beside them, the same way LoRa's does - and the
 * shift is what makes that not quite the same code.
 *
 * Two regions, two sets, and the three readings the rule has here: a target naming a region is
 * constrained by it, a target naming none falls back to the running config's (which is what the
 * firmware does with UNSET), and an offer naming none constrains nothing at all - because 0 on
 * that row means "not offered" rather than "whatever is running". `absent` is legal in every
 * one of them, which is the bit the shifted mask has to put back at the bottom.
 */
MESH_TEST_CASE(ui_settings_beacon_presets_follow_their_region, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_mesh_beacon = true;
    settings.has_lora = true;
    settings.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    settings.region_presets.loaded = true;
    const uint32_t us_set = (1U << meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST) |
                            (1U << meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO);
    const uint32_t eu_set = 1U << meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
    settings.region_presets.region[meshtastic_Config_LoRaConfig_RegionCode_US] =
        (struct mesh_ui_region_preset){.presets = us_set};
    settings.region_presets.region[meshtastic_Config_LoRaConfig_RegionCode_EU_868] =
        (struct mesh_ui_region_preset){.presets = eu_set};

    /* Target 1 names EU and a preset EU will not take. Target 2 names no region, so the US
       set the radio is running is what its preset has to be legal in. The offer names no
       region at all. */
    settings.beacon_targets[0].region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    settings.beacon_targets[0].preset =
        (uint32_t)meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO + 1U;
    settings.beacon_targets[1].preset =
        (uint32_t)meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST + 1U;
    settings.beacon_offer_preset =
        (uint32_t)meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW + 1U;

    struct mesh_ui_settings_item offer_preset;
    struct mesh_ui_settings_item first;
    struct mesh_ui_settings_item second;
    MESH_TEST_FAIL_IF(!settings_find_field(&settings, MESH_UI_SETTINGS_BEACON,
                                           MESH_UI_FIELD_BEACON_OFFER_PRESET, &offer_preset) ||
                          !settings_find_field(&settings, MESH_UI_SETTINGS_BEACON,
                                               MESH_UI_FIELD_BEACON_TARGET_0_PRESET, &first) ||
                          !settings_find_field(&settings, MESH_UI_SETTINGS_BEACON,
                                               MESH_UI_FIELD_BEACON_TARGET_1_PRESET, &second),
                      "the beacon's preset rows are missing");

    /* One past itself, so the legal set moves up with the values and absent keeps bit 0. */
    MESH_TEST_FAIL_IF(first.choices != ((eu_set << 1U) | 1U),
                      "a target's preset row should carry its own region's set, shifted");
    MESH_TEST_FAIL_IF(!first.conflict,
                      "a preset the target's region will not take should be marked, not hidden");
    MESH_TEST_FAIL_IF(second.choices != ((us_set << 1U) | 1U),
                      "a target naming no region should follow the one the radio is running");
    MESH_TEST_FAIL_IF(second.conflict, "a legal pair should mark nothing");
    MESH_TEST_FAIL_IF(offer_preset.choices != 0U || offer_preset.conflict,
                      "an offer naming no region constrains nothing: 0 there is not offered");

    /* And the offer, once it does name one. The preset it is on is not legal in US. */
    struct mesh_ui_setting_edit edits[1];
    memset(edits, 0, sizeof edits);
    edits[0].field = MESH_UI_FIELD_BEACON_OFFER_REGION;
    edits[0].number = meshtastic_Config_LoRaConfig_RegionCode_US;
    MESH_TEST_FAIL_IF(!settings_find_field_edited(&settings, edits, 1U, MESH_UI_SETTINGS_BEACON,
                                                  MESH_UI_FIELD_BEACON_OFFER_PRESET, &offer_preset),
                      "the offered preset row should still be there after a region edit");
    MESH_TEST_FAIL_IF(offer_preset.choices != ((us_set << 1U) | 1U) || !offer_preset.conflict,
                      "the offered preset should follow the pending region it is advertised for");
    record_success(test_name);
}

/*
 * Every section's editable rows against MESH_UI_SETTINGS_EDITS_MAX, not just the widest one.
 *
 * Over the cap mesh_ui_nav_edit_set() returns false and the press silently does nothing, which
 * is how the old cap of 8 hid for a whole phase. The check used to be written out for External
 * notification alone, which is the section that happened to be the widest on the day it was
 * written - Position overtook it the moment it grew ten flag rows. So it walks all of them and
 * names the one that does not fit.
 */
MESH_TEST_CASE(ui_settings_sections_fit_the_edit_list, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings_mark_all_present(&settings);
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        /* Channels are the one section whose rows repeat per slot, and each slot is saved on
           its own - so the count that matters is one channel's. */
        const uint8_t channel =
            section == MESH_UI_SETTINGS_CHANNELS ? 0U : MESH_UI_SETTINGS_NO_CHANNEL;
        const uint32_t rows = mesh_ui_settings_item_count(&settings, NULL, section, channel);
        uint32_t editable = 0U;
        for (uint32_t row = 0; row < rows; ++row) {
            struct mesh_ui_settings_item item;
            if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, section, channel, row, &item) &&
                item.field != MESH_UI_FIELD_NONE) {
                editable++;
            }
        }
        if (editable > MESH_UI_SETTINGS_EDITS_MAX) {
            char reason[160];
            snprintf(reason, sizeof reason, "%s offers %u rows against an edit list of %u",
                     mesh_ui_settings_section_name(section), (unsigned)editable,
                     (unsigned)MESH_UI_SETTINGS_EDITS_MAX);
            record_failure(test_name, reason);
            return;
        }
    }
    record_success(test_name);
}

/*
 * The two reasons a section is empty, and the one predicate that tells them apart.
 *
 * "not loaded" was drawn from three places and meant two things: a section the radio has not
 * sent yet, and a section its firmware was built without. The first is an invitation to press
 * X and the second is not, so a radio with MQTT compiled out told you to keep refreshing a
 * screen that would never fill.
 *
 * The bit table is pinned against meshtastic_ExcludedModules here rather than read from it in
 * the UI layer, which is the nanopb-free side of the fence - the same trade the canned message
 * and ringtone caps make in store.h. A renumbering upstream fails here.
 */
MESH_TEST_CASE(ui_settings_section_availability, unit) {
    static const struct {
        enum mesh_ui_settings_section section;
        uint32_t bit;
    } k_expected[] = {
        {MESH_UI_SETTINGS_BLUETOOTH, meshtastic_ExcludedModules_BLUETOOTH_CONFIG},
        {MESH_UI_SETTINGS_NETWORK, meshtastic_ExcludedModules_NETWORK_CONFIG},
        {MESH_UI_SETTINGS_MQTT, meshtastic_ExcludedModules_MQTT_CONFIG},
        {MESH_UI_SETTINGS_EXT_NOTIFICATION, meshtastic_ExcludedModules_EXTNOTIF_CONFIG},
        {MESH_UI_SETTINGS_STORE_FORWARD, meshtastic_ExcludedModules_STOREFORWARD_CONFIG},
        {MESH_UI_SETTINGS_RANGE_TEST, meshtastic_ExcludedModules_RANGETEST_CONFIG},
        {MESH_UI_SETTINGS_TELEMETRY, meshtastic_ExcludedModules_TELEMETRY_CONFIG},
        {MESH_UI_SETTINGS_CANNED, meshtastic_ExcludedModules_CANNEDMSG_CONFIG},
        {MESH_UI_SETTINGS_NEIGHBOR_INFO, meshtastic_ExcludedModules_NEIGHBORINFO_CONFIG},
        {MESH_UI_SETTINGS_AMBIENT, meshtastic_ExcludedModules_AMBIENTLIGHTING_CONFIG},
        {MESH_UI_SETTINGS_DETECTION, meshtastic_ExcludedModules_DETECTIONSENSOR_CONFIG},
        {MESH_UI_SETTINGS_PAXCOUNTER, meshtastic_ExcludedModules_PAXCOUNTER_CONFIG},
    };
    for (size_t i = 0; i < sizeof k_expected / sizeof k_expected[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_ui_settings_section_excluded_bit(k_expected[i].section) !=
                              k_expected[i].bit,
                          "a section's excluded-module bit does not match the protobuf");
    }
    /* And nothing else claims a bit: a section added to the table without a line here would
       otherwise be pinned by nobody. */
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        bool listed = false;
        for (size_t e = 0; e < sizeof k_expected / sizeof k_expected[0]; ++e) {
            listed = listed || k_expected[e].section == section;
        }
        MESH_TEST_FAIL_IF(listed != (mesh_ui_settings_section_excluded_bit(section) != 0U),
                          "the excluded-module table and this test disagree about a section");
    }

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.excluded_modules = meshtastic_ExcludedModules_MQTT_CONFIG;

    MESH_TEST_FAIL_IF(
        mesh_ui_settings_section_availability(&settings, NULL, MESH_UI_SETTINGS_MQTT) !=
                MESH_UI_SETTINGS_SECTION_EXCLUDED ||
            mesh_ui_settings_section_availability(&settings, NULL, MESH_UI_SETTINGS_TELEMETRY) !=
                MESH_UI_SETTINGS_SECTION_WAITING,
        "an excluded module should not read as one that has not arrived yet");
    MESH_TEST_FAIL_IF(
        strcmp(inkcell_str(mesh_ui_settings_availability_label(MESH_UI_SETTINGS_SECTION_EXCLUDED)),
               "not in firmware") != 0 ||
            strcmp(
                inkcell_str(mesh_ui_settings_availability_label(MESH_UI_SETTINGS_SECTION_WAITING)),
                "not loaded") != 0 ||
            mesh_ui_settings_availability_reason(MESH_UI_SETTINGS_SECTION_EXCLUDED) ==
                mesh_ui_settings_availability_reason(MESH_UI_SETTINGS_SECTION_WAITING),
        "the two empty sections should not read the same, in a row or on the screen behind it");

    /* A radio that says it excluded a module and then sends one is saying two things, and the
       one with rows in it wins. */
    settings.has_mqtt = true;
    MESH_TEST_FAIL_IF(mesh_ui_settings_section_availability(
                          &settings, NULL, MESH_UI_SETTINGS_MQTT) != MESH_UI_SETTINGS_SECTION_READY,
                      "a section the radio sent should be ready whatever the mask says");
    settings.has_mqtt = false;

    /* Without metadata there is no mask to believe, so every missing section is still waiting. */
    settings.has_metadata = false;
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_section_availability(&settings, NULL, MESH_UI_SETTINGS_MQTT) !=
            MESH_UI_SETTINGS_SECTION_WAITING,
        "a mask that was never sent should exclude nothing");
    settings.has_metadata = true;

    /* The Modules list is the row somebody actually reads it on. */
    struct mesh_ui_settings_item item;
    for (uint32_t i = 0; i < mesh_ui_settings_module_count(); ++i) {
        if (mesh_ui_settings_module_at(i) != MESH_UI_SETTINGS_MQTT) {
            continue;
        }
        MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U,
                                                 MESH_UI_SETTINGS_MODULES,
                                                 MESH_UI_SETTINGS_NO_CHANNEL, i, &item) ||
                              strcmp(item.value, "not in firmware") != 0,
                          "the Modules row for an excluded module should say so");
    }
    record_success(test_name);
}

/*
 * The Network section: read-only, and the four static rows only when they mean anything.
 *
 * It is also where the UDP broadcast bit is pinned against the protobuf, for the reason the
 * excluded-module bits are pinned above - the row reads a literal, so something has to hold
 * that literal to the wire.
 */
MESH_TEST_CASE(ui_settings_network_section, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_network = true;
    settings.wifi_enabled = true;
    snprintf(settings.wifi_ssid, sizeof settings.wifi_ssid, "%s", "Shed");
    settings.enabled_protocols = meshtastic_Config_NetworkConfig_ProtocolFlags_UDP_BROADCAST;

    const uint32_t dhcp_rows = mesh_ui_settings_item_count(
        &settings, NULL, MESH_UI_SETTINGS_NETWORK, MESH_UI_SETTINGS_NO_CHANNEL);
    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_NETWORK,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
                          strcmp(item.value, "Shed") != 0,
                      "the section should name the network the radio was told to join");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_NETWORK,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
                          strcmp(item.value, "DHCP") != 0,
                      "address mode 0 is DHCP");
    /* An empty ntp_server is the firmware's own default rather than no time source at all. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_NETWORK,
                                             MESH_UI_SETTINGS_NO_CHANNEL, dhcp_rows - 3U, &item) ||
                          strcmp(item.value, "firmware default") != 0,
                      "an unset NTP server should name the default rather than draw blank");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_NETWORK,
                                             MESH_UI_SETTINGS_NO_CHANNEL, dhcp_rows - 1U, &item) ||
                          item.number != 1U,
                      "the UDP broadcast row should read the protocol flag");

    /* Nothing here is editable, and that is a property of every row rather than of the screen. */
    for (uint32_t i = 0; i < dhcp_rows; ++i) {
        MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U,
                                                 MESH_UI_SETTINGS_NETWORK,
                                                 MESH_UI_SETTINGS_NO_CHANNEL, i, &item) ||
                              item.field != MESH_UI_FIELD_NONE,
                          "no Network row should carry an editable field");
    }

    /* Static adds a heading and the four addresses under it, formatted the way the connection
       rows format the one the radio actually got. */
    settings.address_mode = (uint8_t)meshtastic_Config_NetworkConfig_AddressMode_STATIC;
    settings.ipv4_ip = 0x0A01A8C0U; /* 192.168.1.10, as the wire carries it */
    const uint32_t static_rows = mesh_ui_settings_item_count(
        &settings, NULL, MESH_UI_SETTINGS_NETWORK, MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(static_rows != dhcp_rows + 5U,
                      "Static should add a heading and four addresses");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_NETWORK,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 5U, &item) ||
                          item.kind != INKSTAND_FORM_HEADING,
                      "the static addresses should sit under a heading");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_NETWORK,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 6U, &item) ||
                          strcmp(item.value, "192.168.1.10") != 0,
                      "a static address should be drawn from the fixed32 the radio sent");
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

    /* node_status is 79 bytes on the wire and the edit buffer has to hold all of them - which
       it does by being measured from this field, today the widest one in the table. */
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
                head.kind != INKSTAND_FORM_HEADING ||
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
        /* Not a Radio actions row, and in the list anyway: the sheet is one component and its
           four lines are the constraint every caller of it has, wherever the row lives. */
        MESH_UI_SETTINGS_ACTION_SET_HAM_MODE,
        MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL,
    };
    for (size_t i = 0; i < sizeof actions / sizeof actions[0]; ++i) {
        char text[256];
        char message[128];
        MESH_TEST_FAIL_IF(!mesh_ui_settings_action_needs_confirm(actions[i]),
                          "an action in the confirm list does not ask first");
        mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_ACTIONS, actions[i], text, sizeof text);
        const uint32_t lines = inkcell_wrap_lines(text, 38U);
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

/*
 * The edit buffer is the widest TEXT or KEY field and its NUL, walked from the field table
 * rather than taken on trust from mesh/ui/settings_text.def.
 *
 * That file sizes MESH_UI_SETTING_TEXT_MAX and the table's rows name their limits out of it, so
 * the two agree by construction - for every field that is *in* it. A TEXT row written with a
 * bare number instead is the way back to the bug the def removed: the value is cut to the
 * buffer as it is committed, silently, and the radio would have taken the whole string. So this
 * asks the table, one field at a time, and names the field that does not fit.
 *
 * The equality at the end is the other direction: a field listed in the def and since dropped
 * from the table leaves every edit slot carrying bytes no field can use, which is 16 slots and
 * 32 rows' worth of a mistake nothing else would report.
 */
MESH_TEST_CASE(settings_text_fields_fit_the_edit_buffer, unit) {
    uint32_t widest = 0U;
    for (unsigned f = 0; f < (unsigned)MESH_UI_FIELD_COUNT; ++f) {
        const enum mesh_ui_setting_field field = (enum mesh_ui_setting_field)f;
        const enum inkstand_form_kind kind = mesh_ui_settings_field_kind(field);
        if (kind != INKSTAND_FORM_TEXT && kind != INKSTAND_FORM_KEY) {
            continue;
        }
        const uint32_t limit = mesh_ui_settings_text_max(field);
        char message[128];
        snprintf(message, sizeof message, "%s takes %u bytes and the edit buffer holds %u",
                 mesh_ui_settings_field_label(field), (unsigned)limit,
                 (unsigned)MESH_UI_SETTING_TEXT_MAX - 1U);
        MESH_TEST_FAIL_IF((size_t)limit + 1U > MESH_UI_SETTING_TEXT_MAX, message);
        if (limit > widest) {
            widest = limit;
        }
    }
    MESH_TEST_FAIL_IF(widest == 0U, "no text field was found at all");
    MESH_TEST_FAIL_IF((size_t)widest + 1U != MESH_UI_SETTING_TEXT_MAX,
                      "the edit buffer is wider than the widest field plus its NUL");
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
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, NULL, NULL, false,
                                               items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count != mesh_ui_node_detail_count(&node, false, NULL, NULL),
                      "the count the nav walks disagrees with the built list");
    /* The verbs are a screen of their own, so what the detail leads with is the one row that
       opens them - and it is a row the nav's cursor may stand on, not a heading. */
    MESH_TEST_FAIL_IF(count < 2U || items[0].kind != MESH_UI_NODE_ROW_ACTION ||
                          items[0].action != MESH_UI_NODE_ACTION_OPEN_ACTIONS,
                      "the detail should open with the row that opens the node's verbs");
    MESH_TEST_FAIL_IF(strcmp(items[0].label, "Actions") != 0,
                      "and that row wears the name the group it replaced carried");
    for (uint32_t i = 1U; i < count; ++i) {
        MESH_TEST_FAIL_IF(items[i].kind == MESH_UI_NODE_ROW_ACTION,
                          "no verb but that one belongs on the detail any more");
    }

    /* And the verbs themselves, off the sheet. Every action row says what it is with a symbol
       and how much it costs with its ink - the two tables in node_detail.c, checked here so a
       verb added without an entry in either is a failure rather than a blank gutter nobody
       notices. */
    struct mesh_ui_node_item verbs[MESH_UI_NODE_ACTIONS_MAX];
    const uint32_t verb_count =
        mesh_ui_node_actions_build(&node, false, NULL, false, verbs, MESH_UI_NODE_ACTIONS_MAX);
    MESH_TEST_FAIL_IF(verb_count != mesh_ui_node_actions_count(&node, false, NULL),
                      "the count the nav walks disagrees with the built sheet");
    MESH_TEST_FAIL_IF(verb_count == 0U || verbs[0].action != MESH_UI_NODE_ACTION_MESSAGE,
                      "the message action should be the first verb on the sheet");
    for (uint32_t i = 0; i < verb_count; ++i) {
        MESH_TEST_FAIL_IF(verbs[i].kind != MESH_UI_NODE_ROW_ACTION,
                          "the sheet is verbs and nothing else - no headings, no facts");
        MESH_TEST_FAIL_IF(verbs[i].icon == INKCELL_ICON_NONE,
                          "every action row should name an icon");
        if (verbs[i].action == MESH_UI_NODE_ACTION_REMOVE) {
            MESH_TEST_FAIL_IF(verbs[i].tone != INKCELL_TONE_ERROR,
                              "removing a node should be drawn in the error family");
        } else if (verbs[i].action == MESH_UI_NODE_ACTION_IGNORE) {
            MESH_TEST_FAIL_IF(verbs[i].tone != INKCELL_TONE_WARNING,
                              "ignoring a node should be drawn in the warning family");
        } else {
            /* And everything else is the ordinary ink, which is the half of that statement the
               table used to get wrong: eleven verbs in the accent is not eleven emphases, it is
               a card with none - and the two rows above cannot be the exception if they are not
               the exception. The accent is on these rows still, in the disc at the leading edge
               (INKCELL_FB_LEADING_TONAL), which is a colour the words are not competing with. */
            MESH_TEST_FAIL_IF(verbs[i].tone != INKCELL_TONE_NORMAL,
                              "an ordinary verb should draw in the ordinary ink");
        }
        /* The three flags are controls rather than errands, and each carries its state as a
           field as well as in the words a text backend prints. */
        const bool boolean = verbs[i].action == MESH_UI_NODE_ACTION_FAVORITE ||
                             verbs[i].action == MESH_UI_NODE_ACTION_MUTE ||
                             verbs[i].action == MESH_UI_NODE_ACTION_IGNORE;
        MESH_TEST_FAIL_IF(verbs[i].toggle != boolean, "only the flag rows should be toggles");
        MESH_TEST_FAIL_IF(boolean && verbs[i].value[0] == '\0',
                          "a toggle row should still say its state in words");
    }

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
    mesh_ui_node_detail_build(&node, true, 1750000600U, NULL, NULL, NULL, false, self_items,
                              MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < self_count; ++i) {
        if (self_items[i].kind == MESH_UI_NODE_ROW_ACTION ||
            strcmp(self_items[i].label, "SNR") == 0) {
            record_failure(test_name, "our own node should offer no message row and no SNR");
            return;
        }
    }
    /* Not even the row that opens the sheet: a node with no verbs has no sheet, and a row that
       opened an empty screen is the press-that-does-nothing this client's tables exist to
       prevent. This node has reported no position yet, which is what leaves it with none. */
    MESH_TEST_FAIL_IF(mesh_ui_node_actions_count(&node, true, NULL) != 0U,
                      "our own node with no fix should offer no verbs at all");

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

    count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, NULL, NULL, false, items,
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
 * The two things the position section must not overstate: how precisely it knows where a node
 * is, and how recently. Both used to be said in a way that read as more than we had - a bit
 * count that means nothing to a reader, and a "Fix" heading over a timestamp that was often
 * simply absent.
 */
/*
 * Which of a node's facts are *states* and which are readings, asked of the rows themselves.
 *
 * The screen draws a state as a capsule and a reading as words, and the whole of what decides is
 * `chip` on the row - so this is the table that stops the two from being decided by a renderer
 * matching on value text, which is a second table that drifts the first time a string is
 * retranslated.
 *
 * It is also the bar, in both directions. A state is a closed set the reader is *checking*:
 * whether the key is verified, whether the packets crossed the air. A reading has no such set -
 * there is no "6.75 dB" to be in - and neither has anything the node chose for itself. A card
 * where every row is a bubble is a column of colour reporting nothing, which is the failure this
 * guards against from the other side.
 */
MESH_TEST_CASE(node_detail_states_are_chips, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x5002U;
    snprintf(node.long_name, sizeof node.long_name, "Ridge Relay");
    snprintf(node.short_name, sizeof node.short_name, "RDG");
    node.has_user = true;
    node.in_nodedb = true;
    node.last_heard = 1750000000U;
    node.snr = -4.5f;
    node.has_hops_away = true;
    node.role = 1U;
    node.public_key_len = 32U;

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, NULL, NULL, false,
                                               items, MESH_UI_NODE_ITEMS_MAX);

    bool trust_chip = false;
    bool via_chip = false;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_node_item *item = &items[i];
        if (item->kind != MESH_UI_NODE_ROW_INFO) {
            MESH_TEST_FAIL_IF(item->chip, "only a stated fact can be a state");
            continue;
        }
        if (strcmp(item->label, "Key") == 0) {
            trust_chip = item->chip;
        }
        if (strcmp(item->label, "Heard via") == 0) {
            via_chip = item->chip;
            /* Over the air is the ordinary answer and takes the neutral tone, which is what
               draws the quiet outlined capsule rather than a filled one. */
            MESH_TEST_FAIL_IF(item->tone != (uint8_t)INKCELL_TONE_NORMAL,
                              "a node heard over the air is in no particular state");
        }
        /* The readings this node reports carry no capsule, and neither does anything it chose
           for itself. Both directions in one loop, because the bar is one bar. */
        if (strcmp(item->label, "SNR") == 0 || strcmp(item->label, "Long name") == 0 ||
            strcmp(item->label, "Node number") == 0 || strcmp(item->label, "Public key") == 0 ||
            strcmp(item->label, "Last heard") == 0 || strcmp(item->label, "Hops away") == 0) {
            MESH_TEST_FAIL_IF(item->chip, "a reading or a name is not a state");
        }
    }
    MESH_TEST_FAIL_IF(!trust_chip, "what a key is worth is the state this screen is qualified by");
    MESH_TEST_FAIL_IF(!via_chip, "how a node reached us is a state");

    /* And the states that only exist when the answer is the bad one say so in their tone, so the
       capsule is filled from the family rather than merely outlined. */
    node.via_mqtt = true;
    node.in_nodedb = false;
    count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, NULL, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    bool mqtt_ok = false;
    bool nodedb_ok = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Heard via") == 0) {
            mqtt_ok = items[i].chip && items[i].tone == (uint8_t)INKCELL_TONE_TERTIARY;
        }
        if (strcmp(items[i].label, "NodeDB") == 0) {
            nodedb_ok = items[i].chip && items[i].tone == (uint8_t)INKCELL_TONE_WARNING;
        }
    }
    MESH_TEST_FAIL_IF(!mqtt_ok, "a node reaching us over MQTT should say so in a tone of its own");
    MESH_TEST_FAIL_IF(!nodedb_ok, "a node the radio has forgotten should warn");

    record_success(test_name);
}

MESH_TEST_CASE(ui_node_detail_position_honesty, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x4001U;
    snprintf(node.long_name, sizeof node.long_name, "%s", "Ridge");
    snprintf(node.short_name, sizeof node.short_name, "%s", "RDG");
    node.has_user = true;
    node.position.valid = true;
    node.position.latitude_i = 447654321;
    node.position.longitude_i = -680012345;

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t now = 1750000600U;

    /* A node that dated its own fix: the row asks the node's question and gets the node's
       answer, ten minutes before `now`. */
    node.position.time = 1750000000U;
    node.position.received = 1750000500U;
    node.position.precision_bits = 16U;
    uint32_t count = mesh_ui_node_detail_build(&node, false, now, NULL, NULL, NULL, false, items,
                                               MESH_UI_NODE_ITEMS_MAX);
    bool fix_ok = false;
    bool precision_ok = false;
    bool heard_row_present = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Fix") == 0 && strcmp(items[i].value, "10m ago") == 0) {
            fix_ok = true;
        }
        if (strcmp(items[i].label, "Fix heard") == 0) {
            heard_row_present = true;
        }
        /* Not "16 bits": the distance the sender rounded to, which is the same word the
           channel's own position_precision row uses for the same number. */
        if (strcmp(items[i].label, "Precision") == 0 && strcmp(items[i].value, "~360 m") == 0) {
            precision_ok = true;
        }
    }
    MESH_TEST_FAIL_IF(!fix_ok, "a dated fix should be aged by the node's own clock");
    MESH_TEST_FAIL_IF(heard_row_present, "a dated fix should not also claim an arrival row");
    MESH_TEST_FAIL_IF(!precision_ok, "precision should read as a distance, not a bit count");

    /*
     * The common case: the node dated nothing, because upstream leaves `time` off the mesh to
     * save space. The row switches to our clock and says so - the old behaviour put "unknown"
     * here, throwing away the one honest answer we had.
     */
    node.position.time = 0U;
    node.position.received = 1750000300U;
    count = mesh_ui_node_detail_build(&node, false, now, NULL, NULL, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    bool heard_ok = false;
    bool fix_row_present = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Fix heard") == 0 && strcmp(items[i].value, "5m ago") == 0) {
            heard_ok = true;
        }
        if (strcmp(items[i].label, "Fix") == 0) {
            fix_row_present = true;
        }
    }
    MESH_TEST_FAIL_IF(!heard_ok, "an undated fix should be aged by our own arrival clock");
    MESH_TEST_FAIL_IF(fix_row_present,
                      "an undated fix must not be labelled as though the node dated it");

    /* A fix restored from a cache written before arrival times existed knows neither, and
       says so rather than picking one. */
    node.position.received = 0U;
    count = mesh_ui_node_detail_build(&node, false, now, NULL, NULL, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    bool unknown_ok = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Fix heard") == 0 && strcmp(items[i].value, "?") == 0) {
            unknown_ok = true;
        }
    }
    MESH_TEST_FAIL_IF(!unknown_ok, "a fix with no clock at all should say it does not know");

    /* precision_bits 0 is "the node never said", not "off": no row rather than a claim. */
    node.position.precision_bits = 0U;
    count = mesh_ui_node_detail_build(&node, false, now, NULL, NULL, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(strcmp(items[i].label, "Precision") == 0,
                          "an unstated precision must not draw a row");
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
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    /* About is the first row, so the cursor is already on it. */
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != MESH_UI_SETTINGS_ABOUT) {
        failure = "Settings should open with the cursor on About";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
            item.kind == INKSTAND_FORM_ACTION &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CHECK_UPDATE) {
            check_row = i;
        }
    }
    if (check_row >= rows) {
        failure = "About should offer a check action when updates are supported";
        goto cleanup;
    }
    for (uint32_t i = 0; i < check_row; ++i) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
        if (item.kind != INKSTAND_FORM_ACTION ||
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
        mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    }
    for (uint32_t i = 0; i < theme_row; ++i) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
            if (item.kind != INKSTAND_FORM_INFO ||
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
            item.kind == INKSTAND_FORM_ACTION &&
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
     * the one thing this row adds. A step with no length carries INKSTAND_FORM_METER_UNKNOWN, which
     * is a bar that moves without claiming a position rather than one parked at zero.
     */
    settings.client.update_progress_known = true;
    settings.client.update_progress = 714U;
    mesh_ui_store_set_settings(&store, &settings);
    bool found_meter = false;
    for (uint32_t i = 0; i < busy_rows; ++i) {
        if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item) ||
            item.kind != INKSTAND_FORM_METER) {
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
            item.kind != INKSTAND_FORM_METER) {
            continue;
        }
        found_meter = true;
        if (item.number != INKSTAND_FORM_METER_UNKNOWN) {
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
        if (item.kind == INKSTAND_FORM_ACTION) {
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        failure = "B should return to the section list";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
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
    /*
     * Heard straight off the air, which is the *larger* of the two signal groups rather than the
     * more interesting one: a directly-heard node is the only kind that earns a bar under its
     * SNR and under its received strength, and a relayed one loses both. The routing rows do not
     * depend on the hop count - they are emitted on has_route - so this keeps them.
     */
    node.has_hops_away = true;
    node.hops_away = 0U;
    node.has_rssi = true;
    node.rx_rssi = -96;
    node.rssi_time = node.last_heard;
    /* Routed rather than flooded: both routing rows present, which is what the budget has to
       hold. The relay byte is not this node's own, so it names a relay rather than reading
       "direct" - one row either way. */
    node.has_route = true;
    node.relay_node = 0x11U;
    node.next_hop = 0x22U;
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
    const uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, &trace, &roster,
                                                     NULL, false, items, MESH_UI_NODE_ITEMS_MAX);
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
        &bare, false, 1750000600U, NULL, NULL, NULL, false, items, MESH_UI_NODE_ITEMS_MAX);
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
        mesh_ui_node_detail_build(&roster.nodes[0], false, 1750000600U, NULL, &roster, NULL, false,
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
        mesh_ui_node_detail_build(&roster.nodes[0], false, 1750000600U, NULL, &roster, NULL, false,
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
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, NULL, NULL, false,
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
    count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, NULL, NULL, false, items,
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
 * The two routing rows, and the four answers between them that are not a node name.
 *
 * They read off MeshPacket.relay_node and .next_hop, which are last bytes rather than node
 * numbers, so every one of those four is a case where saying a name would be a guess:
 *
 *   - nothing heard from the node at all - the rows are absent, because a next hop of 0 is a
 *     real reading and a row of zeroes would read as a flooded packet that never arrived;
 *   - the relay byte is the node's own - "direct", the packet came straight to us;
 *   - two nodes in the roster end in the byte - the partial id, because either name is a
 *     coin toss drawn as a measurement;
 *   - the next hop is 0 - "flood", which is upstream's NO_NEXT_HOP_PREFERENCE and is how the
 *     whole mesh worked before firmware 2.5.
 */
MESH_TEST_CASE(node_detail_routing_rows, unit) {
    struct mesh_ui_handshake_state roster;
    memset(&roster, 0, sizeof roster);
    roster.node_count = 3U;
    roster.nodes[0].node_id = 0x7301U;
    snprintf(roster.nodes[0].short_name, sizeof roster.nodes[0].short_name, "SUBJ");
    roster.nodes[1].node_id = 0x7355U;
    snprintf(roster.nodes[1].short_name, sizeof roster.nodes[1].short_name, "RLAY");
    roster.nodes[2].node_id = 0x9955U;
    snprintf(roster.nodes[2].short_name, sizeof roster.nodes[2].short_name, "TWIN");

    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x7301U;
    snprintf(node.short_name, sizeof node.short_name, "SUBJ");
    node.last_heard = 1750000000U;

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    /* The value column's own width, so copying a row's value out cannot truncate it - a test
       that compared a shortened copy would pass on a row that draws something else. */
    char relay[MESH_UI_NODE_VALUE_MAX];
    char hop[MESH_UI_NODE_VALUE_MAX];

    /* Nothing heard: neither row exists. */
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL,
                                               false, items, MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(strcmp(items[i].label, "Relayed by") == 0 ||
                              strcmp(items[i].label, "Next hop") == 0,
                          "a node nothing has been heard from should have no routing rows");
    }

    /* Heard, relayed by an ambiguous byte, flooded onward. */
    node.has_route = true;
    node.relay_node = 0x55U;
    node.next_hop = 0U;
    relay[0] = '\0';
    hop[0] = '\0';
    count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Relayed by") == 0) {
            snprintf(relay, sizeof relay, "%s", items[i].value);
        } else if (strcmp(items[i].label, "Next hop") == 0) {
            snprintf(hop, sizeof hop, "%s", items[i].value);
        }
    }
    MESH_TEST_FAIL_IF(strcmp(relay, "!..55") != 0,
                      "two nodes end in 0x55, so neither of them relayed it as far as we know");
    MESH_TEST_FAIL_IF(strcmp(hop, "flood") != 0, "a next hop of zero is flooded, not unknown");

    /* Drop the collision and the same byte becomes a name. The next hop resolves through the
       same door, so naming the subject there proves it is one rule and not two. */
    roster.node_count = 2U;
    node.relay_node = 0x55U;
    node.next_hop = 0x01U;
    relay[0] = '\0';
    hop[0] = '\0';
    count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Relayed by") == 0) {
            snprintf(relay, sizeof relay, "%s", items[i].value);
        } else if (strcmp(items[i].label, "Next hop") == 0) {
            snprintf(hop, sizeof hop, "%s", items[i].value);
        }
    }
    MESH_TEST_FAIL_IF(strcmp(relay, "RLAY") != 0, "one candidate should be named");
    MESH_TEST_FAIL_IF(strcmp(hop, "SUBJ") != 0, "a next hop resolves the same way a relay does");

    /* And the node named as its own relay, which is the firmware saying nothing carried it. */
    node.relay_node = 0x01U; /* the subject's own last byte */
    relay[0] = '\0';
    count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Relayed by") == 0) {
            snprintf(relay, sizeof relay, "%s", items[i].value);
        }
    }
    MESH_TEST_FAIL_IF(strcmp(relay, "direct") != 0,
                      "a packet that came straight from the node is not relayed by it");

    /*
     * Unless the hop count contradicts it. The packet came two hops, so something carried it,
     * and the byte matching the node's own is a collision with whatever that was - "direct"
     * there would be this row disagreeing with the hop row three lines above it.
     */
    node.has_hops_away = true;
    node.hops_away = 2U;
    relay[0] = '\0';
    count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Relayed by") == 0) {
            snprintf(relay, sizeof relay, "%s", items[i].value);
        }
    }
    MESH_TEST_FAIL_IF(strcmp(relay, "!..01") != 0,
                      "a packet that took a hop cannot have come straight from its sender");

    /* Zero hops is the firmware saying direct rather than declining to, so the row may. */
    node.hops_away = 0U;
    relay[0] = '\0';
    count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Relayed by") == 0) {
            snprintf(relay, sizeof relay, "%s", items[i].value);
        }
    }
    MESH_TEST_FAIL_IF(strcmp(relay, "direct") != 0, "zero hops agrees with the byte");

    /*
     * The collision this screen cannot see for itself. `relay_ambiguous` says the *session*
     * roster holds a second claimant for the byte - one this screen was never handed, because
     * the publish ranks a big mesh down to MESH_UI_MAX_HANDSHAKE_NODES. RLAY is the only
     * claimant among these three nodes, so without the flag the row would name it and sound
     * certain.
     */
    node.has_hops_away = false;
    node.relay_node = 0x55U;
    node.relay_ambiguous = true;
    relay[0] = '\0';
    count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Relayed by") == 0) {
            snprintf(relay, sizeof relay, "%s", items[i].value);
        }
    }
    MESH_TEST_FAIL_IF(strcmp(relay, "!..55") != 0,
                      "a claimant the publish ranked away still makes the byte ambiguous");

    /* And the next hop reads the same flag, so neither row can be certain alone. */
    node.relay_ambiguous = false;
    node.next_hop = 0x55U;
    node.next_hop_ambiguous = true;
    hop[0] = '\0';
    count = mesh_ui_node_detail_build(&node, false, 1750000060U, NULL, &roster, NULL, false, items,
                                      MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, "Next hop") == 0) {
            snprintf(hop, sizeof hop, "%s", items[i].value);
        }
    }
    MESH_TEST_FAIL_IF(strcmp(hop, "!..55") != 0, "the next hop carries its own ambiguity");

    record_success(test_name);
}

/*
 * A verb that cannot be walked back is drawn red, and a red verb always has the confirm sheet
 * in front of it.
 *
 * The pair is the point rather than either half. The sheet and the colour are two statements of
 * one fact - "this one is different" - so a red row that A fires straight off is a trap, and a
 * sheet in front of a row drawn like every other is a question nobody was expecting. The
 * direction that bites is the first, which is why it is the one asserted over the whole table
 * rather than for the handful of rows somebody remembered.
 *
 * Not the converse: plenty of rows are confirmed without being red. A reboot asks before it
 * drops the link and costs nothing but the wait, which is exactly the weight below the red.
 */
MESH_TEST_CASE(settings_verbs_that_cannot_be_undone_are_red, unit) {
    char message[160];
    for (int i = 1; i < (int)MESH_UI_SETTINGS_ACTION_COUNT; ++i) {
        const enum mesh_ui_settings_action action = (enum mesh_ui_settings_action)i;
        if (mesh_ui_settings_action_tone(action) != INKCELL_TONE_ERROR) {
            continue;
        }
        if (!mesh_ui_settings_action_needs_confirm(action)) {
            snprintf(message, sizeof message,
                     "action %d is drawn in the error tone and fires without a confirm sheet", i);
            record_failure(test_name, message);
            return;
        }
    }
    /* And the weight is a lookup rather than whatever sits past the table. */
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_action_tone((enum mesh_ui_settings_action)MESH_UI_SETTINGS_ACTION_COUNT) !=
            INKCELL_TONE_NORMAL,
        "an unknown action should answer with the ordinary weight");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_action_icon((enum mesh_ui_settings_action)MESH_UI_SETTINGS_ACTION_COUNT) !=
            INKCELL_ICON_NONE,
        "an unknown action should answer with no symbol");
    record_success(test_name);
}

/*
 * INKSTAND_FORM_ACTION is doing two jobs, and mesh_ui_settings_item_is_verb() is what tells
 * them apart: Radio actions' rows *do* something, and the Modules and Channels lists' rows open
 * a list. Both are ACTION because the nav answers both with A.
 *
 * A renderer reads the answer to decide whether a row gets a tonal disc and gives up its value
 * column, so getting it wrong does not fail anywhere - it draws a card of channel slots as a
 * card of verbs, with the summary that says what each slot is set to thrown away.
 */
MESH_TEST_CASE(settings_openers_are_not_verbs, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_channels = true;
    settings.channels[0].present = true;
    settings.channels[0].index = 0U;
    settings.channels[0].role = 1U;

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.link_up = true;
    handshake.channel_count = 1U;

    struct mesh_ui_settings_item item;
    char message[160];

    /* Modules: every row is a section wearing a row's clothes. */
    const uint32_t modules = mesh_ui_settings_item_count(
        &settings, &handshake, MESH_UI_SETTINGS_MODULES, MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(modules == 0U, "the Modules list should have rows");
    for (uint32_t row = 0; row < modules; ++row) {
        if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_MODULES,
                                   MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
            break;
        }
        if (mesh_ui_settings_item_is_verb(&item)) {
            snprintf(message, sizeof message, "module row %u reads as a verb", row);
            record_failure(test_name, message);
            return;
        }
    }

    /* A channel slot, which carries its number rather than an action. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U,
                                             MESH_UI_SETTINGS_CHANNELS, MESH_UI_SETTINGS_NO_CHANNEL,
                                             0U, &item),
                      "the Channels list should have a slot row");
    MESH_TEST_FAIL_IF(item.kind != INKSTAND_FORM_ACTION,
                      "a channel slot should still be the kind the nav opens with A");
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_is_verb(&item),
                      "a channel slot opens a list and is not a verb");

    /* And the other side of it: Radio actions is nothing but verbs, and every one of them
       carries the symbol its disc is drawn from. */
    const uint32_t actions = mesh_ui_settings_item_count(
        &settings, &handshake, MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(actions == 0U, "Radio actions should have rows");
    uint32_t verbs = 0U;
    for (uint32_t row = 0; row < actions; ++row) {
        if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_ACTIONS,
                                   MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
            break;
        }
        if (item.kind == INKSTAND_FORM_HEADING) {
            continue;
        }
        if (!mesh_ui_settings_item_is_verb(&item)) {
            snprintf(message, sizeof message, "Radio actions row %u does not read as a verb", row);
            record_failure(test_name, message);
            return;
        }
        if (!inkcell_icon_is_valid(item.icon)) {
            snprintf(message, sizeof message, "Radio actions row %u is a verb with no symbol", row);
            record_failure(test_name, message);
            return;
        }
        ++verbs;
    }
    MESH_TEST_FAIL_IF(verbs < 8U, "Radio actions should be a section of presses");
    record_success(test_name);
}

/*
 * A link dropping changes what the rows *offer* and nothing else about them.
 *
 * This is the rule item_radio_action() was written for and the reason a withdrawn verb is a kind
 * rather than a disappearance: a section whose length changes when the radio drops moves the
 * cursor out from under whoever was reading it, and "not connected" is the answer they were
 * about to press A to find out. Nothing tested it - the shape was held by every row remembering
 * to be built the same way twice.
 *
 * Asserted over the labels rather than the count alone, because the count survives a section
 * that reorders itself and the cursor does not.
 */
MESH_TEST_CASE(settings_withdrawn_verbs_keep_the_section_shape, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;

    struct mesh_ui_handshake_state up;
    memset(&up, 0, sizeof up);
    up.has_my_info = true;
    up.link_up = true;

    struct mesh_ui_handshake_state down = up;
    down.link_up = false;

    char live[MESH_UI_SETTINGS_ITEMS_MAX][MESH_UI_SETTINGS_LABEL_MAX];
    const uint32_t count = mesh_ui_settings_item_count(&settings, &up, MESH_UI_SETTINGS_ACTIONS,
                                                       MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(count == 0U || count > MESH_UI_SETTINGS_ITEMS_MAX,
                      "Radio actions should have rows over a live link");
    struct mesh_ui_settings_item item;
    for (uint32_t row = 0; row < count; ++row) {
        MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &up, NULL, 0U, MESH_UI_SETTINGS_ACTIONS,
                                                 MESH_UI_SETTINGS_NO_CHANNEL, row, &item),
                          "a row should exist over a live link");
        inkwell_str_copy(live[row], sizeof live[row], item.label);
    }

    char message[160];
    const uint32_t dropped = mesh_ui_settings_item_count(&settings, &down, MESH_UI_SETTINGS_ACTIONS,
                                                         MESH_UI_SETTINGS_NO_CHANNEL);
    if (dropped != count) {
        snprintf(message, sizeof message, "the section is %u rows with a link and %u without",
                 count, dropped);
        record_failure(test_name, message);
        return;
    }
    for (uint32_t row = 0; row < count; ++row) {
        if (!mesh_ui_settings_item(&settings, &down, NULL, 0U, MESH_UI_SETTINGS_ACTIONS,
                                   MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
            record_failure(test_name, "a row should exist with no link");
            return;
        }
        if (strcmp(item.label, live[row]) != 0) {
            /* The precision is what keeps the message inside `message`: a compiler bounds a
               row of `live` by the whole table rather than by the row, so a plain %s here
               reads as a 768-byte write into 160. A row holds one label, by construction. */
            snprintf(message, sizeof message, "row %u is \"%.*s\" with a link and \"%s\" without",
                     row, (int)sizeof live[row], live[row], item.label);
            record_failure(test_name, message);
            return;
        }
        if (item.kind == INKSTAND_FORM_HEADING) {
            continue;
        }
        /* Withdrawn, and still the same verb: same symbol, same place, and the nav cannot fire
           it because it is no longer the kind A answers. */
        if (item.kind == INKSTAND_FORM_ACTION) {
            continue; /* the two forget rows are local and stay pressable */
        }
        if (item.kind != INKSTAND_FORM_ACTION_OFF || !mesh_ui_settings_item_is_verb(&item) ||
            !inkcell_icon_is_valid(item.icon)) {
            snprintf(message, sizeof message, "row %u (%.*s) lost its shape when the link dropped",
                     row, (int)sizeof live[row], live[row]);
            record_failure(test_name, message);
            return;
        }
    }
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
        if (!inkcell_icon_is_valid(mesh_ui_settings_section_icon(section))) {
            snprintf(message, sizeof message, "section %s has no icon",
                     mesh_ui_settings_section_name(section));
            record_failure(test_name, message);
            return;
        }
    }
    /* Out of range is the absence of one rather than whatever sits past the table. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_section_icon(
                          (enum mesh_ui_settings_section)MESH_UI_SETTINGS_SECTION_COUNT) !=
                          INKCELL_ICON_NONE,
                      "an unknown section should answer with no icon");
    record_success(test_name);
}

/*
 * The leading disc marks a press that *acts*, and a row that merely holds a value never carries
 * one - whatever key steps it.
 *
 * The rule the whole leading slot answers to, asserted where it is decided rather than where it
 * is drawn. Five presses step their own row's value (the language, the theme, this client's
 * update channel and its dev-updates switch, and the radio's firmware channel) and they used to
 * be verbs for no better reason than that the nav answers them with A. That put a disc on two of
 * About's four rows and on two of About radio's fourteen, and left both screens with an icon
 * column that started and stopped down the page - which is the complaint this is the fix for.
 *
 * Three claims, and the third is the one that would otherwise go quietly:
 *
 *   - a cycle row is not a verb, carries no symbol, and states its value. Without the value the
 *     row would be inert on the screen and the press invisible.
 *   - it is still an ACTION with its action in `number`, so the nav reaches it exactly as
 *     before. `verb` says what the row *is*; it was never what the press reads.
 *   - the walk actually met some. A predicate nothing answers true is a rule holding nothing,
 *     and the earlier version of this case passed against an About with no theme row because
 *     the client info it was given had never named a theme.
 */
MESH_TEST_CASE(ui_settings_a_disc_marks_a_press_that_acts, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    /* About's own two cycles need the client to have named a language and a theme, and the
       update channel needs an updater. The firmware channel needs a fetcher on About radio. */
    snprintf(settings.client.version, sizeof settings.client.version, "%s", "1.12.0");
    snprintf(settings.client.language_name, sizeof settings.client.language_name, "%s", "English");
    snprintf(settings.client.theme_name, sizeof settings.client.theme_name, "%s", "Dark");
    snprintf(settings.client.update_channel, sizeof settings.client.update_channel, "%s", "stable");
    settings.client.update_supported = true;
    settings.client.update_state = (uint8_t)MESH_UPDATE_IDLE;
    settings.fw_supported = true;
    snprintf(settings.fw_channel, sizeof settings.fw_channel, "%s", "stable");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.channel_count = 1U;

    char message[192];
    uint32_t cycles = 0U;
    uint32_t verbs = 0U;
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        const uint32_t count = mesh_ui_settings_item_count(&settings, &handshake, section,
                                                           MESH_UI_SETTINGS_NO_CHANNEL);
        for (uint32_t row = 0; row < count; ++row) {
            struct mesh_ui_settings_item item;
            if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, section,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
                break;
            }
            if (item.kind != INKSTAND_FORM_ACTION && item.kind != INKSTAND_FORM_ACTION_OFF) {
                /* Not a row built from the action table at all - a channel slot and a module row
                   are ACTION and carry a slot or a section in `number`, which is why the two
                   kinds above are what this walk reads and `cycle` is what it trusts. */
                continue;
            }
            if (!item.cycle) {
                verbs += mesh_ui_settings_item_is_verb(&item) ? 1U : 0U;
                continue;
            }
            cycles++;
            if (mesh_ui_settings_item_is_verb(&item)) {
                snprintf(message, sizeof message, "\"%s\" steps its own value and is still a verb",
                         item.label);
                record_failure(test_name, message);
                return;
            }
            if (inkcell_icon_is_valid(item.icon)) {
                snprintf(message, sizeof message, "\"%s\" steps its own value and carries a disc",
                         item.label);
                record_failure(test_name, message);
                return;
            }
            if (item.value[0] == '\0') {
                snprintf(message, sizeof message,
                         "\"%s\" steps its own value and does not say what it is", item.label);
                record_failure(test_name, message);
                return;
            }
            if (!mesh_ui_settings_action_is_cycle((enum mesh_ui_settings_action)item.number)) {
                snprintf(message, sizeof message,
                         "\"%s\" says it cycles and its action in `number` does not", item.label);
                record_failure(test_name, message);
                return;
            }
        }
    }

    if (cycles < 3U || verbs == 0U) {
        snprintf(message, sizeof message,
                 "the walk found %u rows that cycle and %u verbs - it needs both to be saying "
                 "anything",
                 cycles, verbs);
        record_failure(test_name, message);
        return;
    }
    record_success(test_name);
}

/*
 * A row the reader cannot change is a stated fact, and one they can is not.
 *
 * mesh_ui_settings_item_is_fact() is what a renderer asks to decide which of a row's two tiers
 * recedes, and the two ways it can be wrong are both a screen that lies about itself: a reading
 * drawn as a control reads as something to press, and a control drawn as a reading reads as
 * greyed out. The second is the one that bit - the two locks on Radio UI and the "Can shut down"
 * pair on About radio are switches the radio decides, and they sat at full strength beside a
 * "Language" row that is exactly as fixed.
 *
 * The ACTION exclusion is the claim worth a case of its own: a channel slot and a module row
 * have no field and are not verbs, so `verb` alone would read both as facts and quieten the one
 * tier that is the name of the thing the press opens.
 */
MESH_TEST_CASE(ui_settings_a_fact_is_a_row_nothing_changes, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.has_channels = true;
    settings.channels_settled = true;
    settings.channels[0].present = true;
    settings.channels[0].index = 0U;
    settings.channels[0].role = 1U;
    snprintf(settings.channels[0].name, sizeof settings.channels[0].name, "%s", "LongFast");
    settings.has_ui_config = true;
    settings.has_lora = true;
    settings.has_position = true;
    snprintf(settings.client.version, sizeof settings.client.version, "%s", "1.12.0");
    snprintf(settings.client.theme_name, sizeof settings.client.theme_name, "%s", "Dark");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.channel_count = 1U;

    char message[192];
    /* One counter per shape, so a walk that never met one of them is a walk that asserted
       nothing about it - which is how the ACTION clause would go quietly. */
    uint32_t readings = 0U;
    uint32_t fields = 0U;
    uint32_t verbs = 0U;
    uint32_t opens = 0U;
    uint32_t cycles = 0U;
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        const uint32_t count = mesh_ui_settings_item_count(&settings, &handshake, section,
                                                           MESH_UI_SETTINGS_NO_CHANNEL);
        for (uint32_t row = 0; row < count; ++row) {
            struct mesh_ui_settings_item item;
            if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, section,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
                break;
            }
            const bool fact = mesh_ui_settings_item_is_fact(&item);
            /*
             * What this row's shape says it should be, decided one shape at a time. Written as
             * five separate claims rather than as one expression on purpose: an expression would
             * be this case re-stating the predicate it is checking, and would agree with it
             * however the predicate changed.
             */
            bool expected = false;
            uint32_t *counter = NULL;
            if (item.kind == INKSTAND_FORM_HEADING) {
                /* Names the card rather than standing on it. */
                expected = false;
                counter = NULL;
            } else if (mesh_ui_settings_item_is_verb(&item)) {
                expected = false; /* something happens when it is pressed */
                counter = &verbs;
            } else if (item.cycle) {
                expected = false; /* A steps its own value */
                counter = &cycles;
            } else if (item.kind == INKSTAND_FORM_ACTION) {
                expected = false; /* a channel slot or a module: the press opens a list */
                counter = &opens;
            } else if (item.field != MESH_UI_FIELD_NONE) {
                expected = false; /* Left and Right edit it */
                counter = &fields;
            } else {
                expected = true; /* read off the radio or off this client, and that is all */
                counter = &readings;
            }
            if (fact != expected) {
                snprintf(message, sizeof message,
                         "\"%s\" in section %s reads as %s and its shape says %s", item.label,
                         mesh_ui_settings_section_name(section), fact ? "a fact" : "a control",
                         expected ? "a fact" : "a control");
                record_failure(test_name, message);
                return;
            }
            if (counter != NULL) {
                (*counter)++;
            }
        }
    }

    if (readings == 0U || fields == 0U || verbs == 0U || opens == 0U || cycles == 0U) {
        snprintf(message, sizeof message,
                 "the walk met %u readings, %u fields, %u verbs, %u rows that open and %u that "
                 "cycle - it has to meet all five to be saying anything",
                 readings, fields, verbs, opens, cycles);
        record_failure(test_name, message);
        return;
    }
    record_success(test_name);
}

/*
 * The marker gutter says how a row is changed, and a row that is changed two different ways does
 * not get to wear one mark for both.
 *
 * mesh_ui_settings_item_marker() is what a renderer asks, and the failure it exists to prevent is
 * the one this tab shipped with: every row with a field behind it took the pencil, so eighty-odd
 * switches and checkboxes each carried a promise of a keyboard that does not open. A pencil is a
 * keyboard, a stepper is Left and Right, and a control in the value column says it itself.
 *
 * Written as the shapes rather than as one expression, for the reason the fact case above is: an
 * expression here would be this walk re-stating the function it is checking, and would agree with
 * it however it changed.
 *
 * The counters are the other half. Four of these shapes are rare - a key row lives in two
 * sections, a cycle row in three - and a walk that never met one asserted nothing about it.
 */
MESH_TEST_CASE(ui_settings_a_marker_says_how_the_row_is_changed, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.has_channels = true;
    settings.channels_settled = true;
    settings.channels[0].present = true;
    settings.channels[0].index = 0U;
    settings.channels[0].role = 1U;
    snprintf(settings.channels[0].name, sizeof settings.channels[0].name, "%s", "LongFast");
    settings.has_ui_config = true;
    settings.has_lora = true;
    settings.has_position = true;
    settings.has_mqtt = true;
    snprintf(settings.client.version, sizeof settings.client.version, "%s", "1.12.0");
    snprintf(settings.client.theme_name, sizeof settings.client.theme_name, "%s", "Dark");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.channel_count = 1U;

    char message[224];
    uint32_t typed = 0U;    /* the pencil: a keyboard opens */
    uint32_t stepped = 0U;  /* the stepper: Left and Right walk a set */
    uint32_t controls = 0U; /* a switch or a checkbox, which says it without a mark */
    uint32_t cycles = 0U;   /* the swap rune: A moves this row's own value */
    uint32_t quiet = 0U;    /* nothing to say: a fact, a heading, a meter, a verb */
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        const uint32_t count = mesh_ui_settings_item_count(&settings, &handshake, section,
                                                           MESH_UI_SETTINGS_NO_CHANNEL);
        for (uint32_t row = 0; row < count; ++row) {
            struct mesh_ui_settings_item item;
            if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, section,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
                break;
            }
            enum inkcell_icon expected = INKCELL_ICON_NONE;
            uint32_t *counter = &quiet;
            if (item.cycle) {
                expected = INKCELL_ICON_SWAP;
                counter = &cycles;
            } else if (item.field == MESH_UI_FIELD_NONE) {
                /* Nothing behind it to change, whatever the kind says - the radio's own
                   switches arrive as read-only toggles and must not offer a press. */
                expected = INKCELL_ICON_NONE;
                counter = &quiet;
            } else if (item.kind == INKSTAND_FORM_TEXT || item.kind == INKSTAND_FORM_KEY) {
                expected = INKCELL_ICON_EDIT;
                counter = &typed;
            } else if (item.kind == INKSTAND_FORM_ENUM || item.kind == INKSTAND_FORM_NUMBER) {
                expected = INKCELL_ICON_STEPPER;
                counter = &stepped;
            } else if (item.kind == INKSTAND_FORM_TOGGLE || item.kind == INKSTAND_FORM_FLAG) {
                expected = INKCELL_ICON_NONE;
                counter = &controls;
            }
            const enum inkcell_icon marker = mesh_ui_settings_item_marker(&item);
            if (marker != expected) {
                snprintf(message, sizeof message,
                         "\"%s\" in section %s wears %s and its shape says %s", item.label,
                         mesh_ui_settings_section_name(section),
                         marker == INKCELL_ICON_NONE ? "nothing" : inkcell_icon_name(marker),
                         expected == INKCELL_ICON_NONE ? "nothing" : inkcell_icon_name(expected));
                record_failure(test_name, message);
                return;
            }
            (*counter)++;
        }
    }

    if (typed == 0U || stepped == 0U || controls == 0U || cycles == 0U || quiet == 0U) {
        snprintf(message, sizeof message,
                 "the walk met %u typed, %u stepped, %u controls, %u cycles and %u quiet rows - "
                 "it has to meet all five to be saying anything",
                 typed, stepped, controls, cycles, quiet);
        record_failure(test_name, message);
        return;
    }
    record_success(test_name);
}

/*
 * The two state marks outrank how the row is changed, and in that order.
 *
 * A gutter holds one mark, so the three things that want it have to be ranked somewhere, and the
 * ranking is the whole of what a reader about to lose an edit relies on. A pending edit hidden
 * behind a stepper is a change the reader does not know they are about to walk away from; a
 * conflict hidden behind a dot is a value the radio will refuse, reported as merely unsaved.
 */
MESH_TEST_CASE(ui_settings_a_state_mark_outranks_the_offer, unit) {
    struct mesh_ui_settings_item item;
    memset(&item, 0, sizeof item);
    item.kind = INKSTAND_FORM_TOGGLE;
    item.field = MESH_UI_FIELD_USER_LICENSED;

    /* A switch says its own offer, so the gutter is free for a state to use. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_marker(&item) != INKCELL_ICON_NONE,
                      "a switch should leave the gutter empty");
    item.dirty = true;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_marker(&item) != INKCELL_ICON_UNSAVED,
                      "an unsaved switch should still say so");

    /* And over a row that would otherwise have had something to say. */
    item.kind = INKSTAND_FORM_ENUM;
    item.field = MESH_UI_FIELD_DEVICE_ROLE;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_marker(&item) != INKCELL_ICON_UNSAVED,
                      "an unsaved edit outranks the stepper");
    item.conflict = true;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_marker(&item) != INKCELL_ICON_WARNING,
                      "a value the radio will refuse outranks an unsaved one");
    item.dirty = false;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_marker(&item) != INKCELL_ICON_WARNING,
                      "a conflict is a conflict whether or not it is also unsaved");

    item.conflict = false;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_marker(&item) != INKCELL_ICON_STEPPER,
                      "with nothing to report the row is back to saying how it is changed");
    record_success(test_name);
}

/*
 * The invariant struct mesh_ui_settings_item's `icon` is documented with: which rows carry a
 * symbol, which is what the renderer measures its leading slot from.
 *
 * Two claims here, and between them they are what the renderer relies on:
 *
 *   - a verb always carries a symbol. A tonal disc with nothing in it is a row that is not about
 *     anything, and one blank disc in a column of them is the hole the reader's eye stops in.
 *   - every row that is *not* a verb answers mesh_ui_settings_section_icons_rows(), exactly as
 *     the whole section used to. That is still the rule for the two lists of subjects.
 *
 * What this does *not* decide is the gutter. A symbol is per row; the width every row's words
 * start at is per section, because a list that indents only the rows with something in it starts
 * its text in two columns - so a section holding any verb reserves the disc's gutter on its
 * fields too, and INKCELL_FB_LEADING_TONAL_SLOT is what it reserves it with. About is the case the
 * two rules part company on: four rows, two of them verbs, and only the verbs have a symbol to
 * draw.
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
            const bool has_icon = inkcell_icon_is_valid(item.icon);
            if (mesh_ui_settings_item_is_verb(&item)) {
                if (!has_icon) {
                    snprintf(message, sizeof message, "%s row %u is a verb with no symbol",
                             mesh_ui_settings_section_name(section), row);
                    record_failure(test_name, message);
                    return;
                }
                continue;
            }
            /*
             * A heading carries no symbol at all, in any section, and that is a rule of its own
             * rather than an exemption from this one.
             *
             * A symbol on a heading is drawn as a card *header* - the disc out at the card's own
             * edge, where the card's rows begin two cells further in, past the slot this case is
             * about. Eight of the tab's headings used to take one and thirty did not, five of the
             * eight in Radio actions, so that section announced its groups in a shape no other
             * section used. The node detail and Status keep their card headers and are card
             * screens; a settings section is a list of fields, and a list has one subheader.
             */
            if (item.kind == INKSTAND_FORM_HEADING) {
                if (has_icon) {
                    snprintf(message, sizeof message,
                             "%s row %u is a heading with a symbol - the Settings tab heads every "
                             "group the same way",
                             mesh_ui_settings_section_name(section), row);
                    record_failure(test_name, message);
                    return;
                }
                continue;
            }
            if (has_icon != declared) {
                snprintf(message, sizeof message,
                         "%s row %u %s an icon, and the section says it %s",
                         mesh_ui_settings_section_name(section), row, has_icon ? "has" : "has no",
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

/*
 * The radio's own screen, and its canned message list.
 *
 * Two sections that arrive together and are otherwise unalike: one is a plain run of fields
 * over a config the radio streams, the other is six rows over a single '|'-separated string.
 * What they share is the pair of rules this test is really about - that a row shown and not
 * offered stays shown (the two locks and the language), and that a screen with fewer rows than
 * the radio has entries does not lose the rest.
 */
MESH_TEST_CASE(ui_settings_radio_ui_and_canned, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_ui_config = true;
    settings.ui_theme = 1U; /* LIGHT */
    settings.ui_brightness = 128U;
    settings.ui_screen_timeout = 0U;
    settings.ui_compass_mode = 2U;
    settings.ui_gps_format = 3U;
    settings.ui_clockface_analog = true;
    settings.ui_language = 30U; /* the value past the gap in meshtastic_Language */
    settings.ui_settings_lock = true;

    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO_UI,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
                          item.field != MESH_UI_FIELD_UI_THEME || strcmp(item.value, "Light") != 0,
                      "the theme row should name the radio's theme");
    /* 0 is "never sleep", which is not a duration: it stands outside the track as a word. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO_UI,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
                          item.field != MESH_UI_FIELD_UI_SCREEN_TIMEOUT ||
                          strcmp(item.value, "never") != 0,
                      "a screen timeout of 0 should read as never");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO_UI,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 3U, &item) ||
                          item.field != MESH_UI_FIELD_UI_CLOCKFACE ||
                          strcmp(item.value, "Analog") != 0,
                      "the clock face should be named rather than switched on and off");
    /* Every enum offered here has to be contiguous from 0, which is what the nav's stepping
       needs; Language is the one that is not, and is why it is an INFO row below. */
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_enum_count(MESH_UI_FIELD_UI_THEME) != 3U ||
            mesh_ui_settings_enum_count(MESH_UI_FIELD_UI_COMPASS_MODE) != 3U ||
            mesh_ui_settings_enum_count(MESH_UI_FIELD_UI_GPS_FORMAT) != 7U ||
            strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_UI_GPS_FORMAT, 3U), "MGRS") != 0,
        "the Radio UI enum tables are wrong");

    /* The three rows under the last heading: shown, never offered. The language row in
       particular has to name the value past the gap rather than falling off its table. */
    uint32_t rows = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO_UI,
                                                MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(rows != 13U, "Radio UI should draw nine fields under three read-only rows");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO_UI,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 10U, &item) ||
                          item.field != MESH_UI_FIELD_NONE ||
                          strcmp(item.value, "Chinese (simplified)") != 0,
                      "the language row should name a value past the enum's gap");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO_UI,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 12U, &item) ||
                          item.field != MESH_UI_FIELD_NONE || item.number != 1U,
                      "the settings lock should be shown and not offered");

    /* The list splitter. An empty list is no entries, and a trailing separator does not
       invent a last one - both are what the write builder's walk length depends on. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_canned_count("") != 0U ||
                          mesh_ui_settings_canned_count("solo") != 1U ||
                          mesh_ui_settings_canned_count("a|b|c") != 3U ||
                          mesh_ui_settings_canned_count("a|b|") != 2U,
                      "the canned list splitter miscounts");
    char entry[MESH_UI_CANNED_SLOT_MAX];
    mesh_ui_settings_canned_entry("On my way|Roger|Standing by", 2U, entry, sizeof entry);
    MESH_TEST_FAIL_IF(strcmp(entry, "Standing by") != 0, "the last entry should not keep a tail");
    mesh_ui_settings_canned_entry("On my way|Roger", 5U, entry, sizeof entry);
    MESH_TEST_FAIL_IF(entry[0] != '\0', "an entry past the end should be empty");

    settings.has_canned_messages = true;
    snprintf(settings.canned_messages, sizeof settings.canned_messages, "%s",
             "one|two|three|four|five|six|seven|eight");
    rows = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_CANNED,
                                       MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(rows != MESH_UI_CANNED_SLOTS + 1U,
                      "a radio with more messages than slots should say so on a row of its own");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CANNED,
                                             MESH_UI_SETTINGS_NO_CHANNEL, MESH_UI_CANNED_SLOTS,
                                             &item) ||
                          strcmp(item.value, "2 more, kept") != 0,
                      "the row should count what the slots cannot show");
    /* Six entries and no more: every slot is a row and nothing says anything is missing. */
    snprintf(settings.canned_messages, sizeof settings.canned_messages, "%s", "one|two");
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_CANNED,
                                                  MESH_UI_SETTINGS_NO_CHANNEL) !=
                          MESH_UI_CANNED_SLOTS,
                      "empty slots are rows too, and there is nothing to warn about");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CANNED,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
                          item.field != MESH_UI_FIELD_CANNED_4 || item.text[0] != '\0',
                      "an unused slot should be an empty text row rather than absent");

    /* The two caps are chosen together: six slots of 32 plus five separators has to fit the
       wire's 200, or a save would drop the last message with nothing saying so. */
    MESH_TEST_FAIL_IF(MESH_UI_CANNED_SLOTS * MESH_UI_CANNED_SLOT_MAX + (MESH_UI_CANNED_SLOTS - 1U) >
                          MESH_UI_CANNED_MESSAGES_MAX,
                      "the canned slots must fit the wire's own cap");
    record_success(test_name);
}

/*
 * What the radio's own interfaces are doing, on About radio.
 *
 * Only the interfaces the radio reported are drawn - four headings of "not present" would be a
 * screen describing hardware that does not exist - and the rows are readings rather than
 * settings, which is the rule that keeps them on an About section at all. The row budget is
 * checked here because this is the one section whose length is decided by the radio: a board
 * reporting all four is the longest it can be, and a section that overran the cap would lose
 * its last rows silently.
 */
MESH_TEST_CASE(ui_settings_connection_status, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;

    const uint32_t bare = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                                      MESH_UI_SETTINGS_NO_CHANNEL);
    /* A radio that has not answered the request adds nothing at all. */
    settings.connection.valid = true;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                                  MESH_UI_SETTINGS_NO_CHANNEL) != bare,
                      "an answer naming no interfaces should add no rows");

    settings.connection.has_wifi = true;
    settings.connection.wifi_connected = true;
    settings.connection.wifi_rssi = -57;
    /* 192.168.1.40 as the wire carries it: a fixed32 in network byte order. */
    settings.connection.wifi_ip = 0x2801A8C0U;
    snprintf(settings.connection.wifi_ssid, sizeof settings.connection.wifi_ssid, "%s", "shed");

    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                             MESH_UI_SETTINGS_NO_CHANNEL, bare, &item) ||
                          item.kind != INKSTAND_FORM_HEADING,
                      "the interfaces should start under a heading");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                             MESH_UI_SETTINGS_NO_CHANNEL, bare + 2U, &item) ||
                          strcmp(item.value, "shed") != 0,
                      "the WiFi row should name the network");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                             MESH_UI_SETTINGS_NO_CHANNEL, bare + 3U, &item) ||
                          strcmp(item.value, "192.168.1.40") != 0,
                      "the address should read the same on either endianness");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                             MESH_UI_SETTINGS_NO_CHANNEL, bare + 4U, &item) ||
                          strcmp(item.value, "-57 dBm") != 0 || item.field != MESH_UI_FIELD_NONE,
                      "the signal should be a reading, not a setting");

    /* The longest this section can be: every interface reported at once. */
    settings.connection.has_ethernet = true;
    settings.connection.has_bluetooth = true;
    settings.connection.bluetooth_pin = 123456U;
    settings.connection.has_serial = true;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                                  MESH_UI_SETTINGS_NO_CHANNEL) >
                          MESH_UI_SETTINGS_ITEMS_MAX,
                      "a radio reporting every interface must still fit the item list");
    record_success(test_name);
}

/*
 * The firmware rows, from "there is something newer" through the press to the job running.
 *
 * Three things are being pinned and each of them is a decision rather than an observation:
 * **the press exists only when the app said so**, because `fw_can_install` is five questions
 * the app answers once so the row cannot answer them differently; **the row names the bus**, so
 * the confirm sheet behind it can say what is actually true; and **a job in flight takes the
 * section over**, because there is no second install to start and the check would take the
 * download's own child.
 */
MESH_TEST_CASE(ui_settings_radio_firmware_install_row, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.fw_supported = true;
    settings.fw_state = (uint8_t)MESH_FIRMWARE_AVAILABLE;
    snprintf(settings.fw_latest, sizeof settings.fw_latest, "%s", "2.7.26.54e0d8d");
    snprintf(settings.fw_channel, sizeof settings.fw_channel, "%s", "stable");

    /* A check that found something the client cannot install says why, and offers nothing. */
    snprintf(settings.fw_blocker_reason, sizeof settings.fw_blocker_reason, "%s",
             "connect it by USB");
    uint32_t count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                                 MESH_UI_SETTINGS_NO_CHANNEL);
    struct mesh_ui_settings_item item;
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                                MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
                              item.kind == INKSTAND_FORM_ACTION &&
                              mesh_ui_settings_action_is_install_firmware(
                                  (enum mesh_ui_settings_action)item.number),
                          "a blocked board should offer no install row");
    }

    /* With the app's permission and a serial link, the press appears and names the USB install.
       A on it opens the sheet rather than doing anything: this is the row that cannot be taken
       back. */
    settings.fw_blocker_reason[0] = '\0';
    settings.fw_can_install = true;
    settings.fw_bus = (uint8_t)MESH_FIRMWARE_PATH_USB;
    count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    bool found_usb = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB) {
            found_usb = true;
        }
    }
    MESH_TEST_FAIL_IF(!found_usb, "a board on the cable should offer the USB install");

    settings.fw_bus = (uint8_t)MESH_FIRMWARE_PATH_BLE;
    count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    bool found_ble = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE) {
            found_ble = true;
        }
    }
    MESH_TEST_FAIL_IF(!found_ble, "a radio on the air should offer the Bluetooth install");

    /*
     * A job in flight. The section drops to the meter and the release it is installing, and the
     * two presses that were there - the channel and the check - are gone, because both would
     * take the fetcher the download is using.
     */
    settings.fw_update_state = (uint8_t)MESH_FIRMWARE_UPDATE_WRITING;
    settings.fw_update_progress = 43U;
    settings.fw_can_install = false;
    count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    bool metered = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item)) {
            continue;
        }
        MESH_TEST_FAIL_IF(
            item.kind == INKSTAND_FORM_ACTION &&
                (item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE ||
                 item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_FIRMWARE_CHANNEL),
            "nothing else in the section is pressable while an install runs");
        if (item.kind == INKSTAND_FORM_METER && strstr(item.value, "43%") != NULL) {
            metered = true;
            /* A fraction the module answered with, on the bar rather than only in the words. */
            MESH_TEST_FAIL_IF(item.number != 430U, "the meter should carry the same fraction");
        }
    }
    MESH_TEST_FAIL_IF(!metered, "a running install should draw its own progress");

    /* A step with no fraction draws no bar rather than a bar stuck at nothing: forty seconds of
       waiting for a bootloader at 0% reads as a job that stalled. */
    settings.fw_update_state = (uint8_t)MESH_FIRMWARE_UPDATE_WAITING;
    settings.fw_update_progress = 0U;
    count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    bool indeterminate = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.kind == INKSTAND_FORM_METER && item.number == INKSTAND_FORM_METER_UNKNOWN) {
            indeterminate = true;
        }
    }
    MESH_TEST_FAIL_IF(!indeterminate, "a step with no fraction should say so rather than draw 0%");
    record_success(test_name);
}

/*
 * The Installing row when nothing is refusing the install, which is not the same as a refusal
 * with nothing to say.
 *
 * `fw_can_install` is false here for the one reason that is not a blocker: the radio is already
 * on the newest release, so `firmware_blocker()` answered NONE and `fw_blocker_reason` is
 * empty. That emptiness used to be filled with a placeholder written while the install press
 * did not exist yet, so a radio that was perfectly installable - attached, resolved to one
 * board, on the bus that board names - was told the installer had not been built.
 *
 * Nothing caught it because every other case in this section has something to install, and it
 * is the ordinary state a few seconds after any flash: the state the section is in most of the
 * time is the one nothing was asserting about.
 */
MESH_TEST_CASE(ui_settings_up_to_date_radio_says_nothing_about_installing, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.fw_supported = true;
    settings.fw_state = (uint8_t)MESH_FIRMWARE_UP_TO_DATE;
    settings.fw_can_install = false;
    settings.fw_bus = (uint8_t)MESH_FIRMWARE_PATH_BLE;
    snprintf(settings.fw_channel, sizeof settings.fw_channel, "%s", "stable");

    uint32_t count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                                 MESH_UI_SETTINGS_NO_CHANNEL);
    struct mesh_ui_settings_item item;
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                                MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
                              strcmp(item.label, "Installing") == 0,
                          "an up-to-date radio has nothing to say about installing");
    }

    /* A real refusal still gets its row. What means "nothing to say" is the empty reason, not
       the state - an up-to-date radio on the wrong bus is still worth explaining. */
    snprintf(settings.fw_blocker_reason, sizeof settings.fw_blocker_reason, "%s",
             "connect it by USB");
    count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    bool said_why = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            strcmp(item.label, "Installing") == 0 && strcmp(item.value, "connect it by USB") == 0) {
            said_why = true;
        }
    }
    MESH_TEST_FAIL_IF(!said_why, "a blocked install should still say why");
    record_success(test_name);
}

/*
 * The confirm sheet, which is the reason there are two install actions rather than one.
 *
 * Over USB the worst case is a board sitting in its bootloader that any computer can write
 * again; over Bluetooth the radio leaves the mesh for a loader it cannot come back out of on
 * its own. Those are different warnings, and a sheet that gave both the same one would be
 * understating exactly the case that needs the warning.
 */
MESH_TEST_CASE(ui_settings_firmware_confirm_says_what_is_true, unit) {
    char usb_title[96];
    char ble_title[96];
    char usb_text[256];
    char ble_text[256];

    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_action_needs_confirm(MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB) ||
            !mesh_ui_settings_action_needs_confirm(MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE),
        "neither install may happen on the press that selected it");

    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_RADIO, MESH_UI_SETTINGS_NO_CHANNEL,
                                   MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB, usb_title,
                                   sizeof usb_title);
    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_RADIO, MESH_UI_SETTINGS_NO_CHANNEL,
                                   MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE, ble_title,
                                   sizeof ble_title);
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB, usb_text,
                                  sizeof usb_text);
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE, ble_text,
                                  sizeof ble_text);

    MESH_TEST_FAIL_IF(usb_title[0] == '\0' || ble_title[0] == '\0', "both sheets need a question");
    MESH_TEST_FAIL_IF(strcmp(usb_title, ble_title) == 0, "the two buses are not the same question");
    MESH_TEST_FAIL_IF(strcmp(usb_text, ble_text) == 0, "and they are not the same warning either");
    MESH_TEST_FAIL_IF(
        strcmp(mesh_ui_settings_confirm_accept(MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB),
               mesh_ui_settings_confirm_accept(MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE)) == 0,
        "nor the same thing to agree to");
    /* Neither may fall through to the section save's wording, which says the radio will reboot
       to apply a setting - true of neither of these and reassuring about both. */
    char save_text[256];
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_RADIO, MESH_UI_SETTINGS_ACTION_NONE, save_text,
                                  sizeof save_text);
    MESH_TEST_FAIL_IF(strcmp(usb_text, save_text) == 0 || strcmp(ble_text, save_text) == 0,
                      "an install is not a settings save with a longer wait");
    record_success(test_name);
}

/*
 * The press that goes through the sheet reaches the app as its own action, carrying the bus the
 * sheet named.
 *
 * The bus rides the action rather than being re-read from the snapshot because a link that
 * moved between the question and the answer would otherwise have the user agreeing to one thing
 * and getting the other - which on this press is a radio sent into a loader it was never
 * promised it would be.
 */
MESH_TEST_CASE(ui_settings_firmware_install_goes_through_the_sheet, unit) {
    struct mesh_ui_store store;
    struct mesh_ui_settings settings;
    struct mesh_ui_action action;
    const char *failure = NULL;

    mesh_ui_store_init(&store);
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.fw_supported = true;
    settings.fw_state = (uint8_t)MESH_FIRMWARE_AVAILABLE;
    settings.fw_can_install = true;
    settings.fw_bus = (uint8_t)MESH_FIRMWARE_PATH_BLE;
    snprintf(settings.fw_latest, sizeof settings.fw_latest, "%s", "2.7.26.54e0d8d");
    snprintf(settings.fw_channel, sizeof settings.fw_channel, "%s", "stable");
    mesh_ui_store_set_settings(&store, &settings);

    store.nav.screen = MESH_UI_SCREEN_SETTINGS;
    store.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_RADIO;
    store.nav.settings_channel = MESH_UI_SETTINGS_NO_CHANNEL;

    const uint32_t rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    uint32_t install_row = rows;
    struct mesh_ui_settings_item item;
    for (uint32_t i = 0; i < rows; ++i) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE) {
            install_row = i;
        }
    }
    if (install_row >= rows) {
        failure = "the section should offer the install";
        goto cleanup;
    }

    store.nav.cursor[MESH_UI_SCREEN_SETTINGS] = install_row;
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the row opens the question rather than sending the radio anywhere";
        goto cleanup;
    }
    if (!store.nav.confirm_open ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE) {
        failure = "and the sheet should be the one for this bus";
        goto cleanup;
    }
    if (store.nav.confirm_cursor != 1U) {
        failure = "the sheet opens on Cancel, so a repeated press changes nothing";
        goto cleanup;
    }

    /* B backs out and nothing happens, which is the half of the sheet that matters most. */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE) {
        failure = "backing out of the sheet should install nothing";
        goto cleanup;
    }

    /* And through it: the action carries the bus rather than making the app look it up again. */
    store.nav.cursor[MESH_UI_SCREEN_SETTINGS] = install_row;
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    store.nav.confirm_cursor = 0U;
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_INSTALL_RADIO_FIRMWARE) {
        failure = "agreeing to the sheet should ask the app to install";
        goto cleanup;
    }
    if (action.number != 1U) {
        failure = "and should carry the bus the sheet named";
        goto cleanup;
    }
    if (store.nav.confirm_open) {
        failure = "the sheet closes behind it";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Every group on the node detail is one unbroken run of rows.
 *
 * This is the invariant the fb backend's card grouping rests on. A card there is a *contiguous*
 * run of rows sharing an ordinal, and the ordinal is derived from the headings - a heading opens
 * a card and everything under it belongs to that card until the next heading. That derivation
 * has no way back: nothing in the row list says a run has *rejoined* a group it left, so a group
 * interrupted by another one is drawn as two cards, the second of them under a heading that has
 * nothing to do with it.
 *
 * The traced route is how that happened. It was emitted beside the verb that starts it, which
 * put two heading-led groups in the middle of the action block - so Request info, Mute, Ignore
 * and Remove were drawn inside the "Route back" card. Harmless-looking in a flat list of dimmed
 * headings and a card telling a lie once the groups became cards.
 *
 * So the assertion is the property rather than the ordering: walk the rows, and require that no
 * `kind` ever comes back after something else has intervened. It is checked against a node with
 * a finished trace *and* a fix, which is the arrangement that broke - the fix adds two more
 * action rows after the trace, so a fix that merely moved the route one row earlier would still
 * fail this.
 */
MESH_TEST_CASE(node_detail_groups_are_unbroken_runs, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x5000U;
    snprintf(node.short_name, sizeof node.short_name, "TRCE");
    node.last_heard = 1750000000U;
    /* A fix, so the two position-gated action rows are emitted after the trace's own. */
    node.position.valid = true;
    node.position.latitude_i = 375000000;
    node.position.longitude_i = -1224000000;
    node.position.received = 1750000000U;

    struct mesh_ui_traceroute trace;
    memset(&trace, 0, sizeof trace);
    trace.state = MESH_TRACEROUTE_DONE;
    trace.target = node.node_id;
    trace.completed = 1750000500U;
    trace.forward_count = 3U;
    trace.back_count = 2U;
    for (uint8_t i = 0; i < MESH_UI_TRACEROUTE_MAX_HOPS; ++i) {
        trace.forward[i].node_id = 0x7000U + i;
        trace.forward[i].has_snr = true;
        trace.forward[i].snr_quarter_db = 20;
        snprintf(trace.forward[i].name, sizeof trace.forward[i].name, "hop%u", (unsigned)i);
        trace.back[i] = trace.forward[i];
    }

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, &trace, NULL, NULL,
                                                     false, items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count == 0U, "a node with a trace and a fix should produce rows");

    /*
     * The group a row is in, derived exactly as the renderer derives it: a heading opens one and
     * every row under it belongs to that one. Then the check is that a group, once left, is
     * never returned to.
     */
    bool closed[MESH_UI_NODE_ITEMS_MAX];
    memset(closed, 0, sizeof closed);
    uint32_t group = 0U;
    bool saw_route = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].kind == (uint8_t)MESH_UI_NODE_ROW_HEADING) {
            MESH_TEST_FAIL_IF(closed[group], "a group was reopened after another one intervened");
            closed[group] = true;
            group++;
            MESH_TEST_FAIL_IF(group >= MESH_UI_NODE_ITEMS_MAX, "more headings than rows");
            saw_route = saw_route || strstr(items[i].label, "Route") != NULL;
        }
    }
    /* And that the arrangement this is about was actually built - a node whose trace did not
       land would pass the walk above by having no route headings at all. */
    MESH_TEST_FAIL_IF(!saw_route, "the fixture should have produced the traced route's groups");

    /* The action rows are the run that broke, so say so directly as well: every one of them
       falls between the first and the last, with nothing of another kind among them. */
    uint32_t first = count;
    uint32_t last = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].kind == (uint8_t)MESH_UI_NODE_ROW_ACTION) {
            if (first == count) {
                first = i;
            }
            last = i;
        }
    }
    MESH_TEST_FAIL_IF(first == count, "the node should offer actions");
    for (uint32_t i = first; i <= last; ++i) {
        MESH_TEST_FAIL_IF(items[i].kind != (uint8_t)MESH_UI_NODE_ROW_ACTION,
                          "a non-action row sits inside the run of action rows");
    }
    record_success(test_name);
}

/*
 * The decimal pair the coordinate rows and the frequency rows now share.
 *
 * Worth its own case rather than leaning on ui_settings_coords: what the generalisation made
 * possible is a *different* number of places, and the bug it would hide is a scale that is
 * right at seven and wrong at four. Both directions, because a row that formats one way and
 * parses another is a value that drifts every time nobody edits it.
 */
MESH_TEST_CASE(ui_settings_decimals, unit) {
    char text[32];
    int64_t value = 0;

    /* A frequency: four places held and four shown. */
    mesh_ui_settings_decimal_text(9068750, MESH_UI_FREQUENCY_DIGITS, MESH_UI_FREQUENCY_DIGITS, text,
                                  sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "906.8750") != 0, "a frequency formats wrong");
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_decimal_parse("906.875", MESH_UI_FREQUENCY_DIGITS, 3000, &value) ||
            value != 9068750,
        "a frequency typed with fewer places should still parse");
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_decimal_parse(text, MESH_UI_FREQUENCY_DIGITS, 3000, &value) ||
            value != 9068750,
        "a formatted frequency should parse back to itself");

    /* No places at all, which is what the frequency-slot row parses with: a whole number, and
       "12.5" refused rather than quietly read as 12. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_decimal_parse("42", 0U, 65535, &value) || value != 42,
                      "a whole number should parse at no decimal places");
    mesh_ui_settings_decimal_text(42, 0U, 0U, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "42") != 0, "a whole number should format with no point");

    /* Signed, which the frequency trim is and no coordinate row below zero degrees exercises
       at one place. */
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_decimal_parse("-12.5", MESH_UI_HERTZ_DIGITS, 1000000, &value) ||
            value != -125,
        "a negative trim should parse");
    mesh_ui_settings_decimal_text(-125, MESH_UI_HERTZ_DIGITS, MESH_UI_HERTZ_DIGITS, text,
                                  sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "-12.5") != 0, "a negative trim formats wrong");

    /* Shown narrower than held, which is the coordinate rows' case: rounded rather than cut,
       so the digit that decides is the first one dropped. */
    mesh_ui_settings_decimal_text(445999999, MESH_UI_COORD_DIGITS, 5U, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "44.60000") != 0, "a narrowed decimal should round, not cut");

    /* And the refusals, which are what keep a typo off the air. */
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_decimal_parse("906.8 MHz", MESH_UI_FREQUENCY_DIGITS, 3000, &value) ||
            mesh_ui_settings_decimal_parse("", MESH_UI_FREQUENCY_DIGITS, 3000, &value) ||
            mesh_ui_settings_decimal_parse("4000", MESH_UI_FREQUENCY_DIGITS, 3000, &value),
        "rubbish, an empty row and an out-of-range frequency should all be refused");
    record_success(test_name);
}

/*
 * Node numbers, in the three spellings somebody might have one in front of them in.
 *
 * The empty case is the one that matters most: an untouched ignore slot is not a bad value, it
 * is an empty slot, and a parse that refused it would make the row unclearable.
 */
MESH_TEST_CASE(ui_settings_node_ids, unit) {
    char text[16];
    uint32_t id = 0U;

    mesh_ui_settings_node_id_text(0x433D1B2CU, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "!433d1b2c") != 0, "a node id formats wrong");
    mesh_ui_settings_node_id_text(0U, text, sizeof text);
    MESH_TEST_FAIL_IF(text[0] != '\0', "an unused slot should draw as an empty row");

    MESH_TEST_FAIL_IF(!mesh_ui_settings_node_id_parse("!433d1b2c", &id) || id != 0x433D1B2CU,
                      "the spelling the apps use should parse");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_node_id_parse("433D1B2C", &id) || id != 0x433D1B2CU,
                      "eight bare hex digits should parse, upper case included");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_node_id_parse("0x433d1b2c", &id) || id != 0x433D1B2CU,
                      "a 0x prefix should parse");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_node_id_parse("123456", &id) || id != 123456U,
                      "a plain decimal should parse");
    /*
     * Eight digits and no letters is the ambiguous case, and it reads as decimal.
     *
     * Both readings are a legal node number, so the rule has to be one a person can predict:
     * a letter means hex, all digits mean decimal, and the hex reading always has "!" or "0x"
     * available to ask for it. Guessing hex on the *width* made eight digits mean something
     * seven and nine did not, and a wrong guess here ignores a node nobody named.
     */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_node_id_parse("12345678", &id) || id != 12345678U,
                      "eight bare digits are decimal, because nothing in them says hex");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_node_id_parse("!12345678", &id) || id != 0x12345678U,
                      "and the marker is how the same digits are asked for as hex");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_node_id_parse("  ", &id) || id != 0U,
                      "an empty row is an empty slot rather than a bad value");
    MESH_TEST_FAIL_IF(mesh_ui_settings_node_id_parse("!", &id) ||
                          mesh_ui_settings_node_id_parse("nope", &id) ||
                          mesh_ui_settings_node_id_parse("99999999999", &id),
                      "a bare marker, a word and a number too wide should all be refused");
    record_success(test_name);
}

/*
 * LoRa's advanced rows, its ignore slots and its ham group, as the section actually draws them.
 *
 * The row count is asserted because the three headings are the thing that can silently go
 * missing - a group title is the only row here with nothing behind it to notice its absence -
 * and because the edit-list check one file over is only meaningful against a known shape.
 */
MESH_TEST_CASE(ui_settings_lora_advanced, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.has_owner = true;
    settings.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    settings.sx126x_rx_boosted_gain = true;
    settings.channel_num = 42U;
    settings.override_frequency_scaled = 9068750; /* 906.8750 MHz */
    settings.frequency_offset_scaled = -125;      /* -12.5 Hz */
    settings.ignore_incoming[0] = 0x433D1B2CU;
    settings.tx_power = 27;
    snprintf(settings.long_name, sizeof settings.long_name, "%s", "KD2ABC");

    struct mesh_ui_settings_item item;
    const uint32_t rows = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_LORA,
                                                      MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(rows != 26U, "LoRa should draw eleven rows, three headings and twelve more");

    struct {
        uint32_t row;
        enum inkstand_form_kind kind;
        enum mesh_ui_setting_field field;
        const char *value;
    } const expect[] = {
        {11U, INKSTAND_FORM_HEADING, MESH_UI_FIELD_NONE, NULL},
        {12U, INKSTAND_FORM_TOGGLE, MESH_UI_FIELD_LORA_BOOST_GAIN, "on"},
        {14U, INKSTAND_FORM_TEXT, MESH_UI_FIELD_LORA_CHANNEL_NUM, "42"},
        {15U, INKSTAND_FORM_TEXT, MESH_UI_FIELD_LORA_OVERRIDE_FREQ, "906.8750 MHz"},
        {16U, INKSTAND_FORM_TEXT, MESH_UI_FIELD_LORA_FREQUENCY_TRIM, "-12.5 Hz"},
        {17U, INKSTAND_FORM_HEADING, MESH_UI_FIELD_NONE, NULL},
        {18U, INKSTAND_FORM_TEXT, MESH_UI_FIELD_LORA_IGNORE_NODE_0, "!433d1b2c"},
        {21U, INKSTAND_FORM_HEADING, MESH_UI_FIELD_NONE, NULL},
        {22U, INKSTAND_FORM_TEXT, MESH_UI_FIELD_LORA_HAM_CALL_SIGN, NULL},
        {23U, INKSTAND_FORM_TEXT, MESH_UI_FIELD_LORA_HAM_FREQUENCY, "906.8750 MHz"},
        {24U, INKSTAND_FORM_NUMBER, MESH_UI_FIELD_LORA_HAM_TX_POWER, "27 dBm"},
        {25U, INKSTAND_FORM_ACTION, MESH_UI_FIELD_NONE, NULL},
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; ++i) {
        if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                   MESH_UI_SETTINGS_NO_CHANNEL, expect[i].row, &item) ||
            item.kind != expect[i].kind || item.field != expect[i].field ||
            (expect[i].value != NULL && strcmp(item.value, expect[i].value) != 0)) {
            char reason[160];
            snprintf(reason, sizeof reason, "LoRa row %u is not what it should be (%s)",
                     (unsigned)expect[i].row, item.value);
            record_failure(test_name, reason);
            return;
        }
    }

    /*
     * The unit goes in the value column and never into the text the keyboard opens on: a row
     * that offered "906.8750 MHz" back to be edited is a row you have to delete four characters
     * from before you can type a number.
     */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 15U, &item) ||
                          strcmp(item.text, "906.8750") != 0,
                      "the keyboard should open on the bare number, not on the unit");

    /* An empty ignore slot draws as an empty row rather than as node 0. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 19U, &item) ||
                          item.field != MESH_UI_FIELD_LORA_IGNORE_NODE_1 || item.text[0] != '\0',
                      "an unused ignore slot should be an empty row");

    /*
     * The call sign is not offered from the long name until the radio says it is licensed: on
     * an unlicensed node the long name is a nickname, and pre-filling it would be this client
     * putting words in an operator's mouth.
     */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 22U, &item) ||
                          item.text[0] != '\0',
                      "an unlicensed node's long name is not a call sign");
    settings.is_licensed = true;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 22U, &item) ||
                          strcmp(item.text, "KD2ABC") != 0,
                      "a licensed node's call sign should be offered back");
    record_success(test_name);
}

/*
 * Which press owns which row, and what the sheet in front of the ham row says.
 *
 * The consumer split is the whole reason the ham rows can live in LoRa at all: Y writes
 * LoRaConfig and must leave them, the row below them writes them and must leave everything
 * else. Getting it backwards is silent - the press works and the other half of the screen
 * quietly loses its pending edits.
 */
MESH_TEST_CASE(ui_settings_ham_mode_is_its_own_press, unit) {
    const enum mesh_ui_setting_field ham[] = {MESH_UI_FIELD_LORA_HAM_CALL_SIGN,
                                              MESH_UI_FIELD_LORA_HAM_FREQUENCY,
                                              MESH_UI_FIELD_LORA_HAM_TX_POWER};
    for (size_t i = 0; i < sizeof ham / sizeof ham[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_ui_settings_field_consumer(ham[i]) !=
                              MESH_UI_SETTING_CONSUMER_HAM_MODE,
                          "a ham row belongs to its own press, not to Y");
    }
    const enum mesh_ui_setting_field section[] = {
        MESH_UI_FIELD_LORA_BOOST_GAIN, MESH_UI_FIELD_LORA_OVERRIDE_DUTY,
        MESH_UI_FIELD_LORA_CHANNEL_NUM, MESH_UI_FIELD_LORA_OVERRIDE_FREQ,
        MESH_UI_FIELD_LORA_IGNORE_NODE_0};
    for (size_t i = 0; i < sizeof section / sizeof section[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_ui_settings_field_consumer(section[i]) !=
                              MESH_UI_SETTING_CONSUMER_SECTION,
                          "an advanced row is part of the section's own save");
    }

    /* The sheet. Unlike the fixed-position pair this one asks first, because what it turns off
       is the primary channel's encryption and pressing the row again does not turn it back on. */
    MESH_TEST_FAIL_IF(
        !mesh_ui_settings_action_needs_confirm(MESH_UI_SETTINGS_ACTION_SET_HAM_MODE) ||
            !mesh_ui_settings_action_is_radio(MESH_UI_SETTINGS_ACTION_SET_HAM_MODE),
        "ham mode talks to the radio, and asks before it does");
    char title[128];
    char body[256];
    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_NO_CHANNEL,
                                   MESH_UI_SETTINGS_ACTION_SET_HAM_MODE, title, sizeof title);
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_ACTION_SET_HAM_MODE, body,
                                  sizeof body);
    MESH_TEST_FAIL_IF(
        strstr(title, "ham") == NULL || strstr(body, "encryption") == NULL ||
            strcmp(mesh_ui_settings_confirm_accept(MESH_UI_SETTINGS_ACTION_SET_HAM_MODE),
                   mesh_ui_settings_confirm_accept(MESH_UI_SETTINGS_ACTION_NONE)) == 0,
        "the ham sheet should name the mode, say what it turns off, and not read "
        "as an ordinary save");
    record_success(test_name);
}

/*
 * The choice walk both kinds with a *set* of values share.
 *
 * It replaced two copies of one modulo loop, of which only the KEY copy knew what a set was -
 * which is why the enums could offer a value the rest of their section did not allow. The rules
 * that matter are the edges, and every one of them is a way the merged walk could be wrong: an
 * empty mask is every value rather than none, a value past `count` is never in the set whatever
 * the mask says, a row sitting on a value outside the set can still get off it, and a lap with
 * nothing legal on it answers where it started rather than somewhere arbitrary.
 */
MESH_TEST_CASE(ui_settings_choice_step, unit) {
    /* No mask is no constraint: the plain wrap-around the enums had before sets existed. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(0U, 4U, 3U, +1) != 0U,
                      "an unconstrained row should wrap past the last value");
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(0U, 4U, 0U, -1) != 3U,
                      "an unconstrained row should wrap past the first value");

    /* Values 0, 3 and 5 legal out of eight. Both directions skip the rest. */
    const uint32_t mask = (1U << 0) | (1U << 3) | (1U << 5);
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(mask, 8U, 0U, +1) != 3U,
                      "Right should land on the next legal value, not the next value");
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(mask, 8U, 3U, +1) != 5U,
                      "Right should keep skipping what the set leaves out");
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(mask, 8U, 5U, +1) != 0U,
                      "Right off the end of the set should wrap to its first value");
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(mask, 8U, 0U, -1) != 5U,
                      "Left off the start of the set should wrap to its last value");

    /* A value the set leaves out is still steppable: the radio may be holding one, and the row
       has to be able to get off it. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(mask, 8U, 4U, +1) != 5U,
                      "a row sitting on an illegal value should step to a legal one");

    /* One legal value, and none: the press does nothing rather than pretending to. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(1U << 2, 8U, 2U, +1) != 2U,
                      "a set of one should leave the row where it is");
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_step(1U << 9, 8U, 1U, +1) != 1U,
                      "a set with nothing inside the range should leave the row where it is");

    /* And the predicate the row's warning is drawn from. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_choice_allowed(0U, 4U, 3U),
                      "every value in range is allowed when nothing constrains the row");
    MESH_TEST_FAIL_IF(mesh_ui_settings_choice_allowed(0U, 4U, 4U),
                      "a value past the end of the enum is not allowed by an empty mask either");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_choice_allowed(mask, 8U, 3U) ||
                          mesh_ui_settings_choice_allowed(mask, 8U, 4U),
                      "the predicate should read the set it is given");
    record_success(test_name);
}

/*
 * FromRadio.region_presets, from the packet's grouped form to the row's set.
 *
 * Three silences mean the same thing and all three are here, because a caller that read any of
 * them as "no preset is legal" would leave the row unsteppable on exactly the radios that tell
 * us least: a firmware that sends no map at all, a region the map leaves out, and a region code
 * past the end of the table.
 */
MESH_TEST_CASE(ui_settings_region_presets, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);

    MESH_TEST_FAIL_IF(mesh_ui_settings_region_preset(
                          &settings, meshtastic_Config_LoRaConfig_RegionCode_US) != NULL,
                      "a radio that sent no map should constrain nothing");

    settings.region_presets.loaded = true;
    struct mesh_ui_region_preset *us =
        &settings.region_presets.region[meshtastic_Config_LoRaConfig_RegionCode_US];
    us->presets = (1U << meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST) |
                  (1U << meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO);

    const struct mesh_ui_region_preset *found =
        mesh_ui_settings_region_preset(&settings, meshtastic_Config_LoRaConfig_RegionCode_US);
    MESH_TEST_FAIL_IF(found == NULL || found->presets != us->presets,
                      "a region the map describes should answer with its own set");
    MESH_TEST_FAIL_IF(mesh_ui_settings_region_preset(
                          &settings, meshtastic_Config_LoRaConfig_RegionCode_EU_868) != NULL,
                      "a region the map left out should constrain nothing");
    MESH_TEST_FAIL_IF(mesh_ui_settings_region_preset(&settings, MESH_UI_REGION_COUNT) != NULL,
                      "a region code past the end of the table should constrain nothing");

    /* The table is as wide as the protobuf's own enum, and the set is a word the presets fit
       in. Both are literals on this side of the fence, so this is where they are checked. */
    MESH_TEST_FAIL_IF(MESH_UI_REGION_COUNT !=
                          (uint32_t)_meshtastic_Config_LoRaConfig_RegionCode_MAX + 1U,
                      "the region table is not as wide as the protobuf's region codes");
    MESH_TEST_FAIL_IF((uint32_t)_meshtastic_Config_LoRaConfig_ModemPreset_MAX >= 32U,
                      "a modem preset no longer fits in the set the row carries");
    record_success(test_name);
}

/*
 * The LoRa pair: the preset row carries the region's set, and says so when it cannot honour it.
 *
 * The region read is the *pending* one, which is the whole reason the rows are built in this
 * order. A user steps the region and the preset under it has to answer for where they have just
 * arrived, not for where the radio still is.
 */
MESH_TEST_CASE(ui_settings_lora_preset_follows_the_region, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.has_owner = true;
    settings.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    settings.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    settings.region_presets.loaded = true;
    const uint32_t us_set = (1U << meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST) |
                            (1U << meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO);
    settings.region_presets.region[meshtastic_Config_LoRaConfig_RegionCode_US] =
        (struct mesh_ui_region_preset){.presets = us_set};
    /* An amateur band, with a different set and nothing in common with the one above it. */
    const uint32_t ham_set = 1U << meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
    settings.region_presets.region[meshtastic_Config_LoRaConfig_RegionCode_ITU1_2M] =
        (struct mesh_ui_region_preset){.presets = ham_set, .licensed_only = true};

    struct mesh_ui_settings_item region;
    struct mesh_ui_settings_item preset;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &region) ||
                          region.field != MESH_UI_FIELD_LORA_REGION,
                      "the region is still the LoRa section's first row");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 2U, &preset) ||
                          preset.field != MESH_UI_FIELD_LORA_PRESET,
                      "the preset is still the LoRa section's third row");
    MESH_TEST_FAIL_IF(preset.choices != us_set,
                      "the preset row should carry the set its region allows");
    MESH_TEST_FAIL_IF(preset.conflict || region.conflict,
                      "a legal pair on an unlicensed band should mark nothing");

    /* Now the pending region moves to the amateur band. The preset the radio is on is not legal
       there, and the node claims no licence - so both rows have something to say. */
    struct mesh_ui_setting_edit edits[1];
    memset(edits, 0, sizeof edits);
    edits[0].field = MESH_UI_FIELD_LORA_REGION;
    edits[0].number = meshtastic_Config_LoRaConfig_RegionCode_ITU1_2M;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 1U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 2U, &preset),
                      "the preset row should still be there after a region edit");
    MESH_TEST_FAIL_IF(preset.choices != ham_set,
                      "the preset row should follow the pending region, not the radio's");
    MESH_TEST_FAIL_IF(!preset.conflict,
                      "a preset the new region will not take should be marked, not hidden");
    MESH_TEST_FAIL_IF(preset.number != (uint32_t)meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST,
                      "the row should still show the preset the radio is actually on");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 1U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &region) ||
                          !region.conflict,
                      "an amateur band on a node claiming no licence should be marked");

    /*
     * But not before the owner record has arrived, which is a real window rather than a
     * defensive one: the preset map reaches us before the channel table, and our own NodeInfo -
     * which carries the owner - later still. Through it `is_licensed` is a zeroed member, and a
     * mark drawn from that tells an operator we know nothing about that they are unlicensed.
     */
    settings.has_owner = false;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 1U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &region) ||
                          region.conflict,
                      "an unknown licence state is not the same as an unlicensed one");
    settings.has_owner = true;

    /* And an operator who does hold one gets no mark: for them it is simply the band. */
    settings.is_licensed = true;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 1U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 0U, &region) ||
                          region.conflict,
                      "a licensed operator should not be warned about their own band");

    /* A radio whose firmware predates the map constrains nothing, and nothing is marked. */
    settings.region_presets.loaded = false;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, NULL, edits, 1U, MESH_UI_SETTINGS_LORA,
                                             MESH_UI_SETTINGS_NO_CHANNEL, 2U, &preset) ||
                          preset.choices != 0U || preset.conflict,
                      "a firmware that sends no map should leave the preset row as it was");
    record_success(test_name);
}

/*
 * About radio, while the tab is describing another node's radio.
 *
 * This section's whole job is to say what the radio is, so it is the one place that has to name
 * *which* radio - and the one place that carries the press that comes back. It is also where the
 * banner stands down (see ui_chrome), which only works if what it stands down for is here.
 */
MESH_TEST_CASE(ui_settings_about_radio_names_the_node_being_configured, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.admin_ok = true;

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1111U;
    handshake.my_info.reboot_count = 4U;

    /* Locally: the node number is our own and the reboot count sits beside it, as it always
       has. Nothing about the section changes when there is no remote target. */
    struct mesh_ui_settings_item item;
    bool saw_local_number = false;
    bool saw_reboots = false;
    uint32_t count = mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_RADIO,
                                                 MESH_UI_SETTINGS_NO_CHANNEL);
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item)) {
            continue;
        }
        saw_local_number = saw_local_number || strcmp(item.value, "!00001111") == 0;
        saw_reboots = saw_reboots || strcmp(item.label, inkcell_str(MESH_STR_RADIO_REBOOTS)) == 0;
    }
    MESH_TEST_FAIL_IF(!saw_local_number || !saw_reboots,
                      "the ordinary section is the one it always was");

    /* Pointed somewhere else, the first rows say so and offer the way back. */
    settings.admin_dest = 0x7001U;
    snprintf(settings.admin_dest_name, sizeof settings.admin_dest_name, "Hill repeater");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U,
                                             MESH_UI_SETTINGS_RADIO, MESH_UI_SETTINGS_NO_CHANNEL,
                                             0U, &item) ||
                          item.kind != INKSTAND_FORM_HEADING,
                      "whose radio this is comes first, under a heading of its own");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U,
                                             MESH_UI_SETTINGS_RADIO, MESH_UI_SETTINGS_NO_CHANNEL,
                                             1U, &item) ||
                          strcmp(item.value, "Hill repeater") != 0,
                      "and names the node, because every row below it describes that radio");
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U,
                                             MESH_UI_SETTINGS_RADIO, MESH_UI_SETTINGS_NO_CHANNEL,
                                             2U, &item) ||
                          item.kind != INKSTAND_FORM_ACTION ||
                          item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL,
                      "with the press that comes back to our own radio under it");

    /*
     * And the node number becomes the target's. It is the one row in this section that does not
     * come from the admin path - `my_info` is the handshake's, so it is always our own radio's -
     * and left alone it would be this section quietly answering "which radio" with the wrong
     * one, three rows under the heading that just said which.
     */
    bool saw_remote_number = false;
    saw_local_number = false;
    saw_reboots = false;
    count = mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item)) {
            continue;
        }
        saw_remote_number = saw_remote_number || strcmp(item.value, "!00007001") == 0;
        saw_local_number = saw_local_number || strcmp(item.value, "!00001111") == 0;
        saw_reboots = saw_reboots || strcmp(item.label, inkcell_str(MESH_STR_RADIO_REBOOTS)) == 0;
    }
    MESH_TEST_FAIL_IF(!saw_remote_number || saw_local_number,
                      "the node number has to be the one this section is describing");
    MESH_TEST_FAIL_IF(saw_reboots,
                      "MyNodeInfo is what a radio tells the client attached to it, and no admin "
                      "verb asks a node over the mesh how often it has restarted");

    /*
     * The firmware group is gone with it.
     *
     * Those rows are about the radio on the end of the link - the check reads that radio's
     * model and the install writes down that cable - so under a heading that has just named
     * another node they would be offering an image for a board nobody here is holding. There
     * is nothing to put in their place: an image crosses a cable or a BLE link, never a mesh.
     */
    settings.fw_supported = true;
    settings.fw_can_install = true;
    snprintf(settings.fw_latest, sizeof settings.fw_latest, "2.7.7");
    snprintf(settings.fw_board, sizeof settings.fw_board, "Heltec Mesh Node T114");
    settings.has_metadata = true;
    settings.hw_model = 9U;
    bool saw_install = false;
    bool saw_catalog_board = false;
    count = mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                   MESH_UI_SETTINGS_NO_CHANNEL, i, &item)) {
            continue;
        }
        saw_install = saw_install ||
                      item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB ||
                      item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE ||
                      item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE;
        saw_catalog_board = saw_catalog_board || strcmp(item.value, "Heltec Mesh Node T114") == 0;
    }
    MESH_TEST_FAIL_IF(saw_install,
                      "a firmware image goes down a cable, so it has no business being offered "
                      "under a heading naming a node across the mesh");
    MESH_TEST_FAIL_IF(saw_catalog_board,
                      "the board name came out of a check about the radio in your hand, so the "
                      "Hardware row falls back to the target's own model instead");

    /* And the group comes back with the target. */
    settings.admin_dest = 0U;
    saw_install = false;
    count = mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_RADIO,
                                        MESH_UI_SETTINGS_NO_CHANNEL);
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_RADIO,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE) {
            saw_install = true;
        }
    }
    MESH_TEST_FAIL_IF(!saw_install, "back on our own radio the rows are the ones they were");
    settings.admin_dest = 0x7001U;

    /*
     * And the section still fits, which is the half of this that a row added to the top can
     * break silently: the list is built onto the stack every frame and anything past
     * MESH_UI_SETTINGS_ITEMS_MAX is dropped without a word - so the longest this section can be
     * is a remote target *and* a radio reporting every interface it has.
     */
    settings.has_metadata = true;
    settings.has_bluetooth_radio = true;
    settings.has_wifi = true;
    settings.has_ethernet = true;
    settings.has_pkc = true;
    settings.connection.valid = true;
    settings.connection.has_wifi = true;
    settings.connection.wifi_connected = true;
    settings.connection.has_ethernet = true;
    settings.connection.has_bluetooth = true;
    settings.connection.bluetooth_pin = 123456U;
    settings.connection.has_serial = true;
    MESH_TEST_FAIL_IF(mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_RADIO,
                                                  MESH_UI_SETTINGS_NO_CHANNEL) >
                          MESH_UI_SETTINGS_ITEMS_MAX,
                      "About radio has to fit the item list with the remote rows on top of it");
    record_success(test_name);
}

/*
 * The node detail's remote-administration row exists only for a node we hold a key for.
 *
 * Sharper than the gate on the two key rows beside it: those would merely have nothing to do,
 * and this one could not address a packet at all - an AdminMessage crossing the mesh is sealed
 * to the far node's public key, and there is no unencrypted remote admin to fall back to.
 */
MESH_TEST_CASE(ui_settings_node_detail_offers_remote_admin_with_a_key, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x7001U;
    snprintf(node.short_name, sizeof node.short_name, "RPTR");

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, NULL, NULL, false,
                                               items, MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(items[i].action == (uint8_t)MESH_UI_NODE_ACTION_ADMIN,
                          "a node with no key cannot be sent an admin request at all");
    }

    node.public_key_len = 32U;
    node.public_key[0] = 0xA0U;
    /* On the sheet of verbs rather than on the detail: the row is a press, and the presses are
       a screen of their own now. */
    struct mesh_ui_node_item verbs[MESH_UI_NODE_ACTIONS_MAX];
    uint32_t verb_count =
        mesh_ui_node_actions_build(&node, false, NULL, false, verbs, MESH_UI_NODE_ACTIONS_MAX);
    uint32_t at = verb_count;
    for (uint32_t i = 0; i < verb_count; ++i) {
        if (verbs[i].action == (uint8_t)MESH_UI_NODE_ACTION_ADMIN) {
            at = i;
        }
    }
    MESH_TEST_FAIL_IF(at == verb_count, "a node we hold a key for can be configured over the mesh");
    MESH_TEST_FAIL_IF(verbs[at].tone != (uint8_t)INKCELL_TONE_WARNING,
                      "it is the one row here that changes what every other screen means, so it "
                      "says so before it is pressed");

    /* Never against our own node: the radio on the end of the link is not administered over the
       air, and the row would be the way out of remote admin offered as the way in. */
    verb_count =
        mesh_ui_node_actions_build(&node, true, NULL, false, verbs, MESH_UI_NODE_ACTIONS_MAX);
    for (uint32_t i = 0; i < verb_count; ++i) {
        MESH_TEST_FAIL_IF(verbs[i].action == (uint8_t)MESH_UI_NODE_ACTION_ADMIN,
                          "our own radio is not configured over the mesh");
    }
    record_success(test_name);
}

/*
 * The confirm sheet names the radio when the press would leave this one.
 *
 * The one place the remote-administration banner cannot reach: a modal owns the body, so the
 * container that has been saying "this is somebody else's radio" on every other frame is gone at
 * exactly the moment "Factory reset?" is put to the user.
 */
MESH_TEST_CASE(ui_settings_confirm_names_the_remote_radio, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);

    char text[256];
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_ACTION_REBOOT, text,
                                  sizeof text);
    const size_t plain = strlen(text);
    mesh_ui_settings_confirm_add_subject(&settings, MESH_UI_SETTINGS_ACTION_REBOOT, text,
                                         sizeof text);
    MESH_TEST_FAIL_IF(strlen(text) != plain,
                      "with no remote target the sheet is the one it always was");

    settings.admin_dest = 0x7001U;
    snprintf(settings.admin_dest_name, sizeof settings.admin_dest_name, "Hill repeater");
    mesh_ui_settings_confirm_add_subject(&settings, MESH_UI_SETTINGS_ACTION_REBOOT, text,
                                         sizeof text);
    MESH_TEST_FAIL_IF(strstr(text, "Hill repeater") == NULL,
                      "a reboot that leaves this radio has to say whose radio it reaches");
    MESH_TEST_FAIL_IF(strncmp(text, inkcell_str(MESH_STR_CONFIRM_TEXT_REBOOT), 16) != 0,
                      "added to the sheet's own words, not instead of them - what is being "
                      "asked has not changed, only which radio it lands on");

    /* A section save is the same hazard with a quieter name: Save on LoRa while a target is set
       writes somebody else's region. */
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_ACTION_NONE, text,
                                  sizeof text);
    mesh_ui_settings_confirm_add_subject(&settings, MESH_UI_SETTINGS_ACTION_NONE, text,
                                         sizeof text);
    MESH_TEST_FAIL_IF(strstr(text, "Hill repeater") == NULL,
                      "a save is a write, and a write goes wherever the tab is pointed");

    /*
     * And the presses that stay home whatever the tab is pointed at. Saying "this goes over the
     * mesh" of a row that empties this client's own roster would be false, and the sheet would
     * be the thing that made it false.
     */
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_ACTIONS,
                                  MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES, text, sizeof text);
    const size_t local = strlen(text);
    mesh_ui_settings_confirm_add_subject(&settings, MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES, text,
                                         sizeof text);
    MESH_TEST_FAIL_IF(strlen(text) != local, "forgetting our own cached nodes reaches no radio");
    mesh_ui_settings_confirm_add_subject(&settings, MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB,
                                         text, sizeof text);
    MESH_TEST_FAIL_IF(strlen(text) != local, "and firmware goes over a bus, not over the mesh");
    record_success(test_name);
}

/*
 * The Radio card's details page fits the item list at its longest.
 *
 * It is About radio with the verbs done to a radio under it, and About radio was already the
 * section a row added to the top could push past MESH_UI_SETTINGS_ITEMS_MAX - the list is built
 * onto the stack every frame and anything past the cap is dropped without a word, which here
 * would be the factory resets falling off the bottom of the page. So the longest it can be: a
 * radio reporting every interface it has, a firmware check that found something to install, and
 * a link up so every verb is live.
 */
MESH_TEST_CASE(ui_settings_radio_details_fits_at_its_longest, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.link_up = true;
    handshake.my_info.node_num = 0x1111U;

    settings.loaded = true;
    settings.has_metadata = true;
    settings.can_shutdown = true;
    settings.admin_ok = true;
    settings.has_bluetooth_radio = true;
    settings.has_wifi = true;
    settings.has_ethernet = true;
    settings.has_pkc = true;
    settings.fw_supported = true;
    settings.fw_can_install = true;
    snprintf(settings.fw_latest, sizeof settings.fw_latest, "2.7.7");
    snprintf(settings.fw_board, sizeof settings.fw_board, "Heltec Mesh Node T114");
    snprintf(settings.firmware_version, sizeof settings.firmware_version, "2.7.6");
    snprintf(settings.fw_channel, sizeof settings.fw_channel, "stable");
    settings.connection.valid = true;
    settings.connection.has_wifi = true;
    settings.connection.wifi_connected = true;
    settings.connection.has_ethernet = true;
    settings.connection.has_bluetooth = true;
    settings.connection.bluetooth_pin = 123456U;
    settings.connection.has_serial = true;

    const uint32_t count = mesh_ui_settings_item_count(
        &settings, &handshake, MESH_UI_SETTINGS_RADIO_DETAILS, MESH_UI_SETTINGS_NO_CHANNEL);
    MESH_TEST_FAIL_IF(count > MESH_UI_SETTINGS_ITEMS_MAX,
                      "the details page has to fit the item list with every interface reported");
    /* And the last row is the one that must not be the one dropped. */
    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(!mesh_ui_settings_item(&settings, &handshake, NULL, 0U,
                                             MESH_UI_SETTINGS_RADIO_DETAILS,
                                             MESH_UI_SETTINGS_NO_CHANNEL, count - 1U, &item) ||
                          item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE,
                      "the page ends on the factory reset, not wherever the cap cut it off");
    record_success(test_name);
}
