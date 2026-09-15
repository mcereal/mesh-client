#define _POSIX_C_SOURCE 200809L

/*
 * One preference, read everywhere a length is shown.
 *
 * The radio's DisplayConfig.units is the only say the client has over metric or imperial, and
 * before these cases it was honoured by the map and the Waypoints tab and quietly ignored by
 * everything else: the MQTT section's map precision, a channel's position precision, the smart
 * broadcast threshold, and a node's altitude and footprint all read in metres whatever the
 * setting said - the last of those on the map screen itself, two lines under a range in miles.
 *
 * So the cases here are in two halves. The first holds the wordings themselves, which is
 * ordinary. The second is the one that matters and is deliberately written as a *sweep* rather
 * than as a row-by-row assertion: build every section with the radio set to imperial and let
 * nothing come back reading in metres. A row added later that formats a length in metric fails
 * it without anybody having to remember this file exists.
 */

#include "framework/mesh_test.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/units.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- the wordings ---------------------------------------------------------------------- */

MESH_TEST_CASE(ui_units_decode_the_radios_preference, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_units_imperial(0U), "0 is metric");
    MESH_TEST_FAIL_IF(!mesh_ui_units_imperial(1U), "1 is imperial");
    /* DisplayConfig.units is an enum with two values today. A third would arrive from a firmware
       newer than this build, and metric - the protobuf's own zero - is the answer that does not
       invent a system of units for it. */
    MESH_TEST_FAIL_IF(mesh_ui_units_imperial(7U), "an unknown units value falls back to metric");

    record_success(test_name);
}

MESH_TEST_CASE(ui_units_word_a_height_in_both_systems, unit) {
    char out[24];

    mesh_ui_format_altitude(312, false, out, sizeof out);
    MESH_TEST_FAIL_IF(strcmp(out, "312 m") != 0, "a metric height is whole metres");
    mesh_ui_format_altitude(312, true, out, sizeof out);
    MESH_TEST_FAIL_IF(strcmp(out, "1024 ft") != 0, "an imperial height is whole feet");

    /* A height does not become kilometres the way a range does: a node on a mountain reading
       "3.1 km" would be a height that looks like a distance away. */
    mesh_ui_format_altitude(3100, false, out, sizeof out);
    MESH_TEST_FAIL_IF(strstr(out, "km") != NULL, "a height stays in the small unit");

    /* Below sea level is a real place, and rounding towards zero would have swallowed it. */
    mesh_ui_format_altitude(-1, true, out, sizeof out);
    MESH_TEST_FAIL_IF(strcmp(out, "-3 ft") != 0, "a height below sea level keeps its sign");

    record_success(test_name);
}

MESH_TEST_CASE(ui_units_word_a_setting_length_in_both_systems, unit) {
    char out[24];

    /* The metric side keeps the round number somebody picked off the preset list: "1 km" is the
       preset and "1.0 km" is arithmetic done to it. */
    mesh_ui_format_length(1000U, false, out, sizeof out);
    MESH_TEST_FAIL_IF(strcmp(out, "1 km") != 0, "a round metric length stays whole kilometres");
    mesh_ui_format_length(500U, false, out, sizeof out);
    MESH_TEST_FAIL_IF(strcmp(out, "500 m") != 0, "a metric length under a kilometre is metres");

    mesh_ui_format_length(500U, true, out, sizeof out);
    MESH_TEST_FAIL_IF(strstr(out, "ft") == NULL, "an imperial length under a mile is feet");
    mesh_ui_format_length(5000U, true, out, sizeof out);
    MESH_TEST_FAIL_IF(strstr(out, "mi") == NULL, "an imperial length over a mile is miles");

    record_success(test_name);
}

/*
 * The precision ladder, which is a bit count on the wire and a footprint to a reader.
 *
 * Both columns are written out in the catalog rather than converted at the point of use, and the
 * reason is legibility on both sides: every metric step is a round number and so is every
 * imperial one. Running 730 m through a conversion would give "2395 ft", a figure precise about
 * a rounding - so the case checks the ten steps stay distinct and stay in their own system, not
 * that one is the arithmetic of the other.
 */
MESH_TEST_CASE(ui_units_precision_ladder_reads_in_both_systems, unit) {
    char metric[10][24];
    char imperial[10][24];

    for (uint32_t bits = 10U; bits <= 19U; ++bits) {
        const size_t row = (size_t)bits - 10U;
        mesh_ui_settings_format_precision(bits, false, metric[row], sizeof metric[row]);
        mesh_ui_settings_format_precision(bits, true, imperial[row], sizeof imperial[row]);

        MESH_TEST_FAIL_IF(strstr(metric[row], "m") == NULL,
                          "every metric step is worded in metres or kilometres");
        MESH_TEST_FAIL_IF(strstr(imperial[row], "ft") == NULL && strstr(imperial[row], "mi") == NULL,
                          "every imperial step is worded in feet or miles");
        MESH_TEST_FAIL_IF(strcmp(metric[row], imperial[row]) == 0,
                          "a step must not read the same in both systems");
    }

    for (size_t i = 1U; i < 10U; ++i) {
        MESH_TEST_FAIL_IF(strcmp(metric[i], metric[i - 1U]) == 0 ||
                              strcmp(imperial[i], imperial[i - 1U]) == 0,
                          "two precision steps collapsed into one wording");
    }

    /* The three answers that are not distances stay the same in both systems: a count of bits is
       not a length, and neither is "off" or "precise". */
    char off_metric[24];
    char off_imperial[24];
    mesh_ui_settings_format_precision(0U, false, off_metric, sizeof off_metric);
    mesh_ui_settings_format_precision(0U, true, off_imperial, sizeof off_imperial);
    MESH_TEST_FAIL_IF(strcmp(off_metric, off_imperial) != 0, "off is not a distance");
    mesh_ui_settings_format_precision(32U, false, off_metric, sizeof off_metric);
    mesh_ui_settings_format_precision(32U, true, off_imperial, sizeof off_imperial);
    MESH_TEST_FAIL_IF(strcmp(off_metric, off_imperial) != 0, "precise is not a distance");
    mesh_ui_settings_format_precision(5U, false, off_metric, sizeof off_metric);
    mesh_ui_settings_format_precision(5U, true, off_imperial, sizeof off_imperial);
    MESH_TEST_FAIL_IF(strcmp(off_metric, off_imperial) != 0, "a bit count is not a distance");

    record_success(test_name);
}

/* ---- the sweep ------------------------------------------------------------------------- */

/*
 * A value that has gone metric behind the setting's back.
 *
 * Worded as "the unit this row ends in", because a row may legitimately contain an m elsewhere -
 * "Disabled", "GPIO 12", a channel called "Team". Only the trailing unit says what system the
 * number was measured in.
 */
static bool value_reads_metric(const char *value) {
    const size_t len = strlen(value);
    return (len >= 2U && strcmp(value + len - 2U, " m") == 0) ||
           (len >= 3U && strcmp(value + len - 3U, " km") == 0);
}

static void units_fill_settings(struct mesh_ui_settings *settings, uint8_t units) {
    memset(settings, 0, sizeof *settings);
    settings->loaded = true;
    settings->has_display = true;
    settings->units = units;

    settings->has_position = true;
    settings->position_broadcast_smart_enabled = true;
    settings->smart_minimum_distance = 250U;

    settings->has_mqtt = true;
    settings->mqtt_enabled = true;
    settings->mqtt_map_reporting_enabled = true;
    settings->mqtt_map_position_precision = 16U; /* "~360 m", the firmware's own default */

    /* The channel table the Channels section reads is the settings' own, not the handshake's. */
    settings->channels[0].present = true;
    settings->channels[0].index = 0U;
    settings->channels[0].role = 1U;
    settings->channels[0].psk_len = 1U;
    settings->channels[0].position_precision = 14U;
}

/*
 * Every section, with the radio set to imperial: nothing may come back in metres.
 *
 * This is the case the whole change exists for. Settings > MQTT read "~360 m" while the map two
 * taps away read in miles, and it was not one row's mistake - it was that a formatter had no way
 * to ask what system the reader was in, so every row that wanted one quietly answered metric.
 */
MESH_TEST_CASE(ui_units_no_setting_reads_in_metres_under_imperial, unit) {
    struct mesh_ui_settings settings;
    units_fill_settings(&settings, 1U);

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.channel_count = 1U;
    handshake.channels[0].index = 0U;
    handshake.channels[0].role = 1U;
    handshake.channels[0].psk_len = 1U;

    /* The three rows the fixture exists to produce, named so the sweep cannot pass by rendering
       none of them - a settings struct that loaded no section would satisfy "nothing reads in
       metres" and prove nothing at all. */
    unsigned found = 0U;

    for (uint32_t section = 0U; section < (uint32_t)MESH_UI_SETTINGS_SECTION_COUNT; ++section) {
        struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
        const uint32_t count = mesh_ui_settings_items(
            &settings, &handshake, NULL, 0U, (enum mesh_ui_settings_section)section,
            section == (uint32_t)MESH_UI_SETTINGS_CHANNELS ? 0U : MESH_UI_SETTINGS_NO_CHANNEL,
            items, MESH_UI_SETTINGS_ITEMS_MAX);
        for (uint32_t i = 0U; i < count; ++i) {
            char why[160];
            snprintf(why, sizeof why, "'%s' reads '%s' with the radio set to imperial",
                     items[i].label, items[i].value);
            MESH_TEST_FAIL_IF(value_reads_metric(items[i].value), why);
            if (strcmp(items[i].label, "Map precision") == 0 ||
                strcmp(items[i].label, "Position precision") == 0 ||
                strcmp(items[i].label, "Smart distance") == 0) {
                ++found;
                MESH_TEST_FAIL_IF(strstr(items[i].value, " ft") == NULL &&
                                      strstr(items[i].value, " mi") == NULL,
                                  why);
            }
        }
    }

    char reached[96];
    snprintf(reached, sizeof reached, "the sweep reached %u of the three length rows", found);
    MESH_TEST_FAIL_IF(found != 3U, reached);

    record_success(test_name);
}

/*
 * The same sweep the other way round, so the fix cannot be "always say feet".
 *
 * A metric reader is the default and the larger half of the mesh; a change that made every
 * length imperial would pass the case above and be exactly as wrong.
 */
MESH_TEST_CASE(ui_units_no_setting_reads_in_feet_under_metric, unit) {
    struct mesh_ui_settings settings;
    units_fill_settings(&settings, 0U);

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.channel_count = 1U;
    handshake.channels[0].index = 0U;
    handshake.channels[0].role = 1U;
    handshake.channels[0].psk_len = 1U;

    for (uint32_t section = 0U; section < (uint32_t)MESH_UI_SETTINGS_SECTION_COUNT; ++section) {
        struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
        const uint32_t count = mesh_ui_settings_items(
            &settings, &handshake, NULL, 0U, (enum mesh_ui_settings_section)section,
            section == (uint32_t)MESH_UI_SETTINGS_CHANNELS ? 0U : MESH_UI_SETTINGS_NO_CHANNEL,
            items, MESH_UI_SETTINGS_ITEMS_MAX);
        for (uint32_t i = 0U; i < count; ++i) {
            const size_t len = strlen(items[i].value);
            const bool feet = len >= 3U && strcmp(items[i].value + len - 3U, " ft") == 0;
            const bool miles = len >= 3U && strcmp(items[i].value + len - 3U, " mi") == 0;
            char why[160];
            snprintf(why, sizeof why, "'%s' reads '%s' with the radio set to metric",
                     items[i].label, items[i].value);
            MESH_TEST_FAIL_IF(feet || miles, why);
        }
    }

    record_success(test_name);
}

/*
 * A pending edit of the Units row rewords the section under it before it is written.
 *
 * The Display section is where the row lives, so it is the one place a reader can watch the
 * choice take effect - and a row that kept the radio's old answer until the write came back
 * would be the setting appearing not to work.
 */
MESH_TEST_CASE(ui_units_follow_a_pending_edit, unit) {
    struct mesh_ui_settings settings;
    units_fill_settings(&settings, 0U);

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);

    struct mesh_ui_setting_edit edits[1];
    memset(edits, 0, sizeof edits);
    edits[0].field = MESH_UI_FIELD_DISPLAY_UNITS;
    edits[0].number = 1U;

    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_settings_items(&settings, &handshake, edits, 1U, MESH_UI_SETTINGS_POSITION,
                               MESH_UI_SETTINGS_NO_CHANNEL, items, MESH_UI_SETTINGS_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count == 0U, "the Position section should have rows");

    bool saw_length = false;
    for (uint32_t i = 0U; i < count; ++i) {
        if (strstr(items[i].value, " ft") != NULL || strstr(items[i].value, " mi") != NULL) {
            saw_length = true;
        }
        MESH_TEST_FAIL_IF(value_reads_metric(items[i].value),
                          "a pending switch to imperial should reword the rows under it");
    }
    MESH_TEST_FAIL_IF(!saw_length, "the smart broadcast threshold should be a length");

    record_success(test_name);
}

/*
 * A node's own rows: the two that are lengths follow the same preference the map does.
 *
 * Altitude and the footprint Precision names are the node detail's whole stake in this, and they
 * are the rows the map's selected-marker card repeats - so a disagreement here is two screens
 * describing one fix in two systems of units.
 */
MESH_TEST_CASE(ui_units_node_detail_lengths_follow_the_setting, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = 0x1234U;
    snprintf(node.long_name, sizeof node.long_name, "%s", "Summit");
    node.position.valid = true;
    node.position.latitude_i = 375000000;
    node.position.longitude_i = -1223000000;
    node.position.has_altitude = true;
    node.position.altitude = 312;
    node.position.precision_bits = 16U;

    for (int pass = 0; pass < 2; ++pass) {
        const bool imperial = pass == 1;
        struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
        const uint32_t count = mesh_ui_node_detail_build(&node, false, 1750000600U, NULL, false,
                                                         NULL, NULL, imperial, items,
                                                         MESH_UI_NODE_ITEMS_MAX);
        MESH_TEST_FAIL_IF(count == 0U, "a node with a fix should have rows");

        bool altitude_ok = false;
        bool precision_ok = false;
        for (uint32_t i = 0U; i < count; ++i) {
            if (strcmp(items[i].label, "Altitude") == 0) {
                altitude_ok = imperial ? strcmp(items[i].value, "1024 ft") == 0
                                       : strcmp(items[i].value, "312 m") == 0;
            } else if (strcmp(items[i].label, "Precision") == 0) {
                precision_ok = imperial ? strcmp(items[i].value, "~1200 ft") == 0
                                        : strcmp(items[i].value, "~360 m") == 0;
            }
        }
        MESH_TEST_FAIL_IF(!altitude_ok, "the altitude row should follow the radio's units");
        MESH_TEST_FAIL_IF(!precision_ok, "the precision row should follow the radio's units");
    }

    record_success(test_name);
}
