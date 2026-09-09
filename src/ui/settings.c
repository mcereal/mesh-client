#define _POSIX_C_SOURCE 200809L

/*
 * What each setting is: the field table, and everything derived from it.
 *
 * One designated-initialiser table (k_fields) is the single description of every editable field -
 * its label, its section, how it steps, what its values are called. Adding a setting is adding a
 * row there plus a case in src/core/app_settings.c; nothing else in the client should be
 * switching on a field id.
 */

#include "settings_internal.h"

#include "mesh/core/radio_settings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mesh/ui/anim.h"
#include "mesh/ui/settings.h"

#include "mesh/core/radio_settings.h"
#include "mesh/core/updater.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *mesh_ui_settings_section_name(enum mesh_ui_settings_section section) {
    switch (section) {
    case MESH_UI_SETTINGS_ABOUT:
        return mesh_str(MESH_STR_SETTINGS_SECTION_ABOUT);
    case MESH_UI_SETTINGS_RADIO:
        return mesh_str(MESH_STR_SETTINGS_SECTION_RADIO);
    case MESH_UI_SETTINGS_USER:
        return mesh_str(MESH_STR_SETTINGS_SECTION_USER);
    case MESH_UI_SETTINGS_DEVICE:
        return mesh_str(MESH_STR_SETTINGS_SECTION_DEVICE);
    case MESH_UI_SETTINGS_DISPLAY:
        return mesh_str(MESH_STR_SETTINGS_SECTION_DISPLAY);
    case MESH_UI_SETTINGS_LORA:
        return mesh_str(MESH_STR_SETTINGS_SECTION_LORA);
    case MESH_UI_SETTINGS_BLUETOOTH:
        return mesh_str(MESH_STR_SETTINGS_SECTION_BLUETOOTH);
    case MESH_UI_SETTINGS_CHANNELS:
        return mesh_str(MESH_STR_SETTINGS_SECTION_CHANNELS);
    case MESH_UI_SETTINGS_SECURITY:
        return mesh_str(MESH_STR_SETTINGS_SECTION_SECURITY);
    case MESH_UI_SETTINGS_POSITION:
        return mesh_str(MESH_STR_SETTINGS_SECTION_POSITION);
    case MESH_UI_SETTINGS_POWER:
        return mesh_str(MESH_STR_SETTINGS_SECTION_POWER);
    case MESH_UI_SETTINGS_MQTT:
        return mesh_str(MESH_STR_SETTINGS_SECTION_MQTT);
    case MESH_UI_SETTINGS_STORE_FORWARD:
        return mesh_str(MESH_STR_SETTINGS_SECTION_STORE_FORWARD);
    case MESH_UI_SETTINGS_TELEMETRY:
        return mesh_str(MESH_STR_SETTINGS_SECTION_TELEMETRY);
    case MESH_UI_SETTINGS_ACTIONS:
        return mesh_str(MESH_STR_SETTINGS_SECTION_ACTIONS);
    case MESH_UI_SETTINGS_MODULES:
        return mesh_str(MESH_STR_SETTINGS_SECTION_MODULES);
    case MESH_UI_SETTINGS_NEIGHBOR_INFO:
        return mesh_str(MESH_STR_SETTINGS_SECTION_NEIGHBOR_INFO);
    case MESH_UI_SETTINGS_RANGE_TEST:
        return mesh_str(MESH_STR_SETTINGS_SECTION_RANGE_TEST);
    case MESH_UI_SETTINGS_PAXCOUNTER:
        return mesh_str(MESH_STR_SETTINGS_SECTION_PAXCOUNTER);
    case MESH_UI_SETTINGS_TAK:
        return mesh_str(MESH_STR_SETTINGS_SECTION_TAK);
    case MESH_UI_SETTINGS_AMBIENT:
        return mesh_str(MESH_STR_SETTINGS_SECTION_AMBIENT);
    case MESH_UI_SETTINGS_STATUS_MESSAGE:
        return mesh_str(MESH_STR_SETTINGS_SECTION_STATUS_MESSAGE);
    case MESH_UI_SETTINGS_DETECTION:
        return mesh_str(MESH_STR_SETTINGS_SECTION_DETECTION);
    case MESH_UI_SETTINGS_EXT_NOTIFICATION:
        return mesh_str(MESH_STR_SETTINGS_SECTION_EXT_NOTIFICATION);
    case MESH_UI_SETTINGS_TRAFFIC:
        return mesh_str(MESH_STR_SETTINGS_SECTION_TRAFFIC);
    case MESH_UI_SETTINGS_RADIO_UI:
        return mesh_str(MESH_STR_SETTINGS_SECTION_RADIO_UI);
    case MESH_UI_SETTINGS_CANNED:
        return mesh_str(MESH_STR_SETTINGS_SECTION_CANNED);
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

/*
 * What each section is about, in one line each, in the enum's own order.
 *
 * A table rather than a switch because it is a lookup with no cases in it, and because a
 * section added without an icon then comes out as MESH_UI_ICON_NONE - which draws nothing and
 * leaves the row where it was, rather than failing to compile in a file that has nothing to do
 * with icons.
 */
static const enum mesh_ui_icon k_section_icons[MESH_UI_SETTINGS_SECTION_COUNT] = {
    [MESH_UI_SETTINGS_ABOUT] = MESH_UI_ICON_ABOUT,
    /* Facts about the radio, which is what the Status tab's Radio card holds - the same
       sentence, so the same icon. */
    [MESH_UI_SETTINGS_RADIO] = MESH_UI_ICON_RADIO,
    [MESH_UI_SETTINGS_USER] = MESH_UI_ICON_USER,
    [MESH_UI_SETTINGS_DEVICE] = MESH_UI_ICON_DEVICE,
    [MESH_UI_SETTINGS_DISPLAY] = MESH_UI_ICON_DISPLAY,
    [MESH_UI_SETTINGS_LORA] = MESH_UI_ICON_LORA,
    [MESH_UI_SETTINGS_BLUETOOTH] = MESH_UI_ICON_BLUETOOTH,
    [MESH_UI_SETTINGS_CHANNELS] = MESH_UI_ICON_CHANNEL,
    [MESH_UI_SETTINGS_SECURITY] = MESH_UI_ICON_SECURITY,
    [MESH_UI_SETTINGS_POSITION] = MESH_UI_ICON_POSITION,
    [MESH_UI_SETTINGS_POWER] = MESH_UI_ICON_POWER,
    [MESH_UI_SETTINGS_MQTT] = MESH_UI_ICON_MQTT,
    [MESH_UI_SETTINGS_STORE_FORWARD] = MESH_UI_ICON_STORE_FWD,
    [MESH_UI_SETTINGS_TELEMETRY] = MESH_UI_ICON_TELEMETRY,
    [MESH_UI_SETTINGS_ACTIONS] = MESH_UI_ICON_ACTIONS,
    [MESH_UI_SETTINGS_MODULES] = MESH_UI_ICON_MODULES,
    [MESH_UI_SETTINGS_NEIGHBOR_INFO] = MESH_UI_ICON_NEIGHBORS,
    [MESH_UI_SETTINGS_RANGE_TEST] = MESH_UI_ICON_RANGE_TEST,
    [MESH_UI_SETTINGS_PAXCOUNTER] = MESH_UI_ICON_PAXCOUNTER,
    [MESH_UI_SETTINGS_TAK] = MESH_UI_ICON_TAK,
    [MESH_UI_SETTINGS_AMBIENT] = MESH_UI_ICON_AMBIENT,
    [MESH_UI_SETTINGS_STATUS_MESSAGE] = MESH_UI_ICON_STATUS_MSG,
    [MESH_UI_SETTINGS_DETECTION] = MESH_UI_ICON_DETECTION,
    [MESH_UI_SETTINGS_EXT_NOTIFICATION] = MESH_UI_ICON_EXT_NOTIFY,
    [MESH_UI_SETTINGS_TRAFFIC] = MESH_UI_ICON_TRAFFIC,
    /* Two more sections answering with an icon another part of the UI owns, for the reason the
       three above do: Radio UI *is* the radio's screen, which is what DISPLAY says, and a
       canned message is a quick reply, which is what REPLY says. */
    [MESH_UI_SETTINGS_RADIO_UI] = MESH_UI_ICON_DISPLAY,
    [MESH_UI_SETTINGS_CANNED] = MESH_UI_ICON_REPLY,
};

enum mesh_ui_icon mesh_ui_settings_section_icon(enum mesh_ui_settings_section section) {
    return section < MESH_UI_SETTINGS_SECTION_COUNT ? k_section_icons[section] : MESH_UI_ICON_NONE;
}

/*
 * What each section is *for*, in a sentence or two, in the enum's own order.
 *
 * A table beside the icons and for the same reason the comment above them gives: it is a lookup
 * with no cases in it, and a section added without a note then reads as "no note" rather than
 * failing to compile in a file that has nothing to do with help.
 *
 * Every section has one, and that is a rule rather than an observation - it is what makes the
 * help key worth offering on every section screen. A field's note may be MESH_STR_NONE, because
 * most settings explain themselves; a section's may not, because "what is this whole screen
 * about" is the question somebody who opened it has by definition.
 */
static const enum mesh_str_id k_section_notes[MESH_UI_SETTINGS_SECTION_COUNT] = {
    [MESH_UI_SETTINGS_ABOUT] = MESH_STR_SETTINGS_NOTE_ABOUT,
    [MESH_UI_SETTINGS_RADIO] = MESH_STR_SETTINGS_NOTE_RADIO,
    [MESH_UI_SETTINGS_USER] = MESH_STR_SETTINGS_NOTE_USER,
    [MESH_UI_SETTINGS_DEVICE] = MESH_STR_SETTINGS_NOTE_DEVICE,
    [MESH_UI_SETTINGS_DISPLAY] = MESH_STR_SETTINGS_NOTE_DISPLAY,
    [MESH_UI_SETTINGS_LORA] = MESH_STR_SETTINGS_NOTE_LORA,
    [MESH_UI_SETTINGS_BLUETOOTH] = MESH_STR_SETTINGS_NOTE_BLUETOOTH,
    [MESH_UI_SETTINGS_CHANNELS] = MESH_STR_SETTINGS_NOTE_CHANNELS,
    [MESH_UI_SETTINGS_SECURITY] = MESH_STR_SETTINGS_NOTE_SECURITY,
    [MESH_UI_SETTINGS_POSITION] = MESH_STR_SETTINGS_NOTE_POSITION,
    [MESH_UI_SETTINGS_POWER] = MESH_STR_SETTINGS_NOTE_POWER,
    [MESH_UI_SETTINGS_MQTT] = MESH_STR_SETTINGS_NOTE_MQTT,
    [MESH_UI_SETTINGS_STORE_FORWARD] = MESH_STR_SETTINGS_NOTE_STORE_FORWARD,
    [MESH_UI_SETTINGS_TELEMETRY] = MESH_STR_SETTINGS_NOTE_TELEMETRY,
    [MESH_UI_SETTINGS_ACTIONS] = MESH_STR_SETTINGS_NOTE_ACTIONS,
    [MESH_UI_SETTINGS_MODULES] = MESH_STR_SETTINGS_NOTE_MODULES,
    [MESH_UI_SETTINGS_NEIGHBOR_INFO] = MESH_STR_SETTINGS_NOTE_NEIGHBOR_INFO,
    [MESH_UI_SETTINGS_RANGE_TEST] = MESH_STR_SETTINGS_NOTE_RANGE_TEST,
    [MESH_UI_SETTINGS_PAXCOUNTER] = MESH_STR_SETTINGS_NOTE_PAXCOUNTER,
    [MESH_UI_SETTINGS_TAK] = MESH_STR_SETTINGS_NOTE_TAK,
    [MESH_UI_SETTINGS_AMBIENT] = MESH_STR_SETTINGS_NOTE_AMBIENT,
    [MESH_UI_SETTINGS_STATUS_MESSAGE] = MESH_STR_SETTINGS_NOTE_STATUS_MESSAGE,
    [MESH_UI_SETTINGS_DETECTION] = MESH_STR_SETTINGS_NOTE_DETECTION,
    [MESH_UI_SETTINGS_EXT_NOTIFICATION] = MESH_STR_SETTINGS_NOTE_EXT_NOTIFICATION,
    [MESH_UI_SETTINGS_TRAFFIC] = MESH_STR_SETTINGS_NOTE_TRAFFIC,
    [MESH_UI_SETTINGS_RADIO_UI] = MESH_STR_SETTINGS_NOTE_RADIO_UI,
    [MESH_UI_SETTINGS_CANNED] = MESH_STR_SETTINGS_NOTE_CANNED,
};

enum mesh_str_id mesh_ui_settings_section_note(enum mesh_ui_settings_section section) {
    return section < MESH_UI_SETTINGS_SECTION_COUNT ? k_section_notes[section] : MESH_STR_NONE;
}

bool mesh_ui_settings_section_icons_rows(enum mesh_ui_settings_section section) {
    return section == MESH_UI_SETTINGS_MODULES;
}

/*
 * The two lists, as tables.
 *
 * k_root is the top level in the order it is read, which is roughly "this client, then what
 * the radio is, then how it talks, then everything optional, then the things that are not
 * settings at all". It is thirteen rows, which fits the Brick's screen without scrolling -
 * that is the point of Modules being one row rather than seventeen.
 */
static const enum mesh_ui_settings_section k_root[] = {
    MESH_UI_SETTINGS_ABOUT,     MESH_UI_SETTINGS_RADIO,    MESH_UI_SETTINGS_USER,
    MESH_UI_SETTINGS_DEVICE,    MESH_UI_SETTINGS_DISPLAY,  MESH_UI_SETTINGS_RADIO_UI,
    MESH_UI_SETTINGS_POSITION,  MESH_UI_SETTINGS_POWER,    MESH_UI_SETTINGS_LORA,
    MESH_UI_SETTINGS_BLUETOOTH, MESH_UI_SETTINGS_CHANNELS, MESH_UI_SETTINGS_SECURITY,
    MESH_UI_SETTINGS_MODULES,   MESH_UI_SETTINGS_ACTIONS,
};

/* Every ModuleConfig variant this client keeps. Grows by one row per module as the phases
   land; the order is the protobuf's field order, which is as good as any and is stable. */
static const enum mesh_ui_settings_section k_modules[] = {
    MESH_UI_SETTINGS_MQTT,
    MESH_UI_SETTINGS_STORE_FORWARD,
    MESH_UI_SETTINGS_TELEMETRY,
    MESH_UI_SETTINGS_RANGE_TEST,
    MESH_UI_SETTINGS_NEIGHBOR_INFO,
    MESH_UI_SETTINGS_AMBIENT,
    MESH_UI_SETTINGS_PAXCOUNTER,
    MESH_UI_SETTINGS_STATUS_MESSAGE,
    MESH_UI_SETTINGS_TAK,
    MESH_UI_SETTINGS_DETECTION,
    MESH_UI_SETTINGS_EXT_NOTIFICATION,
    MESH_UI_SETTINGS_TRAFFIC,
    /* Last, and the one row here that is not a ModuleConfig: the canned message list is its
       own pair of admin verbs. It is in this list because the question the list answers - what
       is this radio running - is one it answers, and nowhere else would be shorter to find. */
    MESH_UI_SETTINGS_CANNED,
};

uint32_t mesh_ui_settings_root_count(void) { return (uint32_t)MESH_ARRAY_LEN(k_root); }

enum mesh_ui_settings_section mesh_ui_settings_root_at(uint32_t row) {
    return row < MESH_ARRAY_LEN(k_root) ? k_root[row] : MESH_UI_SETTINGS_ABOUT;
}

uint32_t mesh_ui_settings_module_count(void) { return (uint32_t)MESH_ARRAY_LEN(k_modules); }

enum mesh_ui_settings_section mesh_ui_settings_module_at(uint32_t row) {
    return row < MESH_ARRAY_LEN(k_modules) ? k_modules[row] : MESH_UI_SETTINGS_MQTT;
}

bool mesh_ui_settings_section_is_module(enum mesh_ui_settings_section section) {
    for (size_t i = 0; i < MESH_ARRAY_LEN(k_modules); ++i) {
        if (k_modules[i] == section) {
            return true;
        }
    }
    return false;
}

bool mesh_ui_settings_section_loaded(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section) {
    if (settings == NULL) {
        return false;
    }
    switch (section) {
    case MESH_UI_SETTINGS_ABOUT:
        /* The client knows its own version with no radio in sight, which is the whole point of
           having this section: it is reachable before anything is connected. */
        return true;
    case MESH_UI_SETTINGS_RADIO:
        return settings->has_metadata || (handshake != NULL && handshake->has_my_info);
    case MESH_UI_SETTINGS_USER:
        return settings->has_owner;
    case MESH_UI_SETTINGS_DEVICE:
        return settings->has_device;
    case MESH_UI_SETTINGS_DISPLAY:
        return settings->has_display;
    case MESH_UI_SETTINGS_LORA:
        return settings->has_lora;
    case MESH_UI_SETTINGS_BLUETOOTH:
        return settings->has_bluetooth;
    case MESH_UI_SETTINGS_CHANNELS:
        return settings->has_channels || (handshake != NULL && handshake->channel_count > 0U);
    case MESH_UI_SETTINGS_SECURITY:
        return settings->has_security;
    case MESH_UI_SETTINGS_POSITION:
        return settings->has_position;
    case MESH_UI_SETTINGS_POWER:
        return settings->has_power;
    case MESH_UI_SETTINGS_MQTT:
        return settings->has_mqtt;
    case MESH_UI_SETTINGS_STORE_FORWARD:
        return settings->has_store_forward;
    case MESH_UI_SETTINGS_TELEMETRY:
        return settings->has_telemetry;
    case MESH_UI_SETTINGS_ACTIONS:
        /* Nothing is read for this section, so what it waits on is not a config fragment but
           the one thing an AdminMessage cannot be addressed without: our own node number. A
           cached roster with no link opens it too, for the two rows that drop that roster and
           send nothing - the rest then render as "not connected". */
        return handshake != NULL && (handshake->has_my_info || handshake->node_count > 0U);
    case MESH_UI_SETTINGS_MODULES:
        /* A folder, not a fragment. It lists every module whether or not the radio has sent
           one, because "which of these has not arrived" is exactly what the list is for. */
        return true;
    case MESH_UI_SETTINGS_NEIGHBOR_INFO:
        return settings->has_neighbor_info;
    case MESH_UI_SETTINGS_RANGE_TEST:
        return settings->has_range_test;
    case MESH_UI_SETTINGS_PAXCOUNTER:
        return settings->has_paxcounter;
    case MESH_UI_SETTINGS_TAK:
        return settings->has_tak;
    case MESH_UI_SETTINGS_AMBIENT:
        return settings->has_ambient_lighting;
    case MESH_UI_SETTINGS_STATUS_MESSAGE:
        return settings->has_status_message;
    case MESH_UI_SETTINGS_DETECTION:
        return settings->has_detection_sensor;
    case MESH_UI_SETTINGS_EXT_NOTIFICATION:
        return settings->has_external_notification;
    case MESH_UI_SETTINGS_TRAFFIC:
        return settings->has_traffic_management;
    case MESH_UI_SETTINGS_RADIO_UI:
        return settings->has_ui_config;
    case MESH_UI_SETTINGS_CANNED:
        return settings->has_canned_messages;
    default:
        return false;
    }
}

/* ---- editable fields ---------------------------------------------------------------------- */

static const char *compass_name(uint32_t orientation) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_COMPASS_0,        MESH_STR_ENUM_COMPASS_90,
        MESH_STR_ENUM_COMPASS_180,      MESH_STR_ENUM_COMPASS_270,
        MESH_STR_ENUM_COMPASS_0_FLIP,   MESH_STR_ENUM_COMPASS_90_FLIP,
        MESH_STR_ENUM_COMPASS_180_FLIP, MESH_STR_ENUM_COMPASS_270_FLIP,
    };
    return mesh_str(orientation < MESH_ARRAY_LEN(k_names) ? k_names[orientation]
                                                          : MESH_STR_COMMON_UNKNOWN_SHORT);
}

static const char *units_name(uint32_t units) {
    return mesh_str(units == 1U ? MESH_STR_ENUM_UNITS_IMPERIAL : MESH_STR_ENUM_UNITS_METRIC);
}

/* DetectionSensorConfig.TriggerType, 0..5 and contiguous. Named for what the pin does rather
   than for the constant: "Low" says more than "LOGIC_LOW" next to the word Trigger. */
static const char *trigger_name(uint32_t trigger) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_TRIGGER_LOW,        MESH_STR_ENUM_TRIGGER_HIGH,
        MESH_STR_ENUM_TRIGGER_FALLING,    MESH_STR_ENUM_TRIGGER_RISING,
        MESH_STR_ENUM_TRIGGER_EITHER_LOW, MESH_STR_ENUM_TRIGGER_EITHER_HIGH,
    };
    return mesh_str(trigger < MESH_ARRAY_LEN(k_names) ? k_names[trigger]
                                                      : MESH_STR_COMMON_UNKNOWN_SHORT);
}

/* DeviceUIConfig's three editable enums, all contiguous from 0 - which is what the nav's
   (value + 1) % count stepping needs, and why Language is not among them. */
static const char *ui_theme_name(uint32_t theme) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_UI_THEME_DARK,
        MESH_STR_ENUM_UI_THEME_LIGHT,
        MESH_STR_ENUM_UI_THEME_RED,
    };
    return mesh_str(theme < MESH_ARRAY_LEN(k_names) ? k_names[theme]
                                                    : MESH_STR_COMMON_UNKNOWN_SHORT);
}

static const char *ui_compass_name(uint32_t mode) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_UI_COMPASS_DYNAMIC,
        MESH_STR_ENUM_UI_COMPASS_FIXED,
        MESH_STR_ENUM_UI_COMPASS_FREEZE,
    };
    return mesh_str(mode < MESH_ARRAY_LEN(k_names) ? k_names[mode] : MESH_STR_COMMON_UNKNOWN_SHORT);
}

/* Named for what a reader would call the format rather than for the acronym, except where the
   acronym is what it is called - MGRS and UTM are not expanded on a map either. */
static const char *ui_gps_format_name(uint32_t format) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_UI_GPS_DEC,  MESH_STR_ENUM_UI_GPS_DMS, MESH_STR_ENUM_UI_GPS_UTM,
        MESH_STR_ENUM_UI_GPS_MGRS, MESH_STR_ENUM_UI_GPS_OLC, MESH_STR_ENUM_UI_GPS_OSGR,
        MESH_STR_ENUM_UI_GPS_MLS,
    };
    return mesh_str(format < MESH_ARRAY_LEN(k_names) ? k_names[format]
                                                     : MESH_STR_COMMON_UNKNOWN_SHORT);
}

/* `is_clockface_analog` is a bool on the wire and an enum here: a row reading "Clock face: on"
   says nothing, and the two values have names. */
static const char *ui_clockface_name(uint32_t analog) {
    return mesh_str(analog != 0U ? MESH_STR_ENUM_UI_CLOCK_ANALOG : MESH_STR_ENUM_UI_CLOCK_DIGITAL);
}

/* meshtastic_Team and meshtastic_MemberRole, from atak.proto. Both are contiguous from 0, which
   is what the nav's (value + 1) % count stepping needs; value 0 is "Unspecifed" upstream (their
   spelling), shown here as what the firmware actually does with it.

   Named as the phone apps name them - "RTO", not its expansion - for the reason keys are shown
   as base64: a setting read off the Brick should be recognisable in the app and back. */
static const char *tak_team_name(uint32_t team) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_TAK_TEAM_DEFAULT,   MESH_STR_ENUM_TAK_TEAM_WHITE,
        MESH_STR_ENUM_TAK_TEAM_YELLOW,    MESH_STR_ENUM_TAK_TEAM_ORANGE,
        MESH_STR_ENUM_TAK_TEAM_MAGENTA,   MESH_STR_ENUM_TAK_TEAM_RED,
        MESH_STR_ENUM_TAK_TEAM_MAROON,    MESH_STR_ENUM_TAK_TEAM_PURPLE,
        MESH_STR_ENUM_TAK_TEAM_DARK_BLUE, MESH_STR_ENUM_TAK_TEAM_BLUE,
        MESH_STR_ENUM_TAK_TEAM_CYAN,      MESH_STR_ENUM_TAK_TEAM_TEAL,
        MESH_STR_ENUM_TAK_TEAM_GREEN,     MESH_STR_ENUM_TAK_TEAM_DARK_GREEN,
        MESH_STR_ENUM_TAK_TEAM_BROWN,
    };
    return mesh_str(team < MESH_ARRAY_LEN(k_names) ? k_names[team] : MESH_STR_COMMON_UNKNOWN_SHORT);
}

static const char *tak_role_name(uint32_t role) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_TAK_ROLE_DEFAULT,     MESH_STR_ENUM_TAK_ROLE_MEMBER,
        MESH_STR_ENUM_TAK_ROLE_LEAD,        MESH_STR_ENUM_TAK_ROLE_HQ,
        MESH_STR_ENUM_TAK_ROLE_SNIPER,      MESH_STR_ENUM_TAK_ROLE_MEDIC,
        MESH_STR_ENUM_TAK_ROLE_FORWARD_OBS, MESH_STR_ENUM_TAK_ROLE_RTO,
        MESH_STR_ENUM_TAK_ROLE_K9,
    };
    return mesh_str(role < MESH_ARRAY_LEN(k_names) ? k_names[role] : MESH_STR_COMMON_UNKNOWN_SHORT);
}

static const char *rebroadcast_name(uint32_t mode) {
    static const enum mesh_str_id k_names[] = {
        MESH_STR_ENUM_REBROADCAST_ALL,   MESH_STR_ENUM_REBROADCAST_ALL_SKIP,
        MESH_STR_ENUM_REBROADCAST_LOCAL, MESH_STR_ENUM_REBROADCAST_KNOWN,
        MESH_STR_ENUM_REBROADCAST_NONE,  MESH_STR_ENUM_REBROADCAST_CORE,
    };
    return mesh_str(mode < MESH_ARRAY_LEN(k_names) ? k_names[mode] : MESH_STR_COMMON_UNKNOWN_SHORT);
}

static const char *gps_mode_name(uint32_t mode) {
    switch (mode) {
    case 0U:
        return mesh_str(MESH_STR_ENUM_GPS_DISABLED);
    case 1U:
        return mesh_str(MESH_STR_ENUM_GPS_ENABLED);
    case 2U:
        return mesh_str(MESH_STR_ENUM_GPS_NOT_PRESENT);
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

static const char *channel_role_name(uint32_t value) {
    return mesh_str(value == 1U ? MESH_STR_ENUM_CHANNEL_SECONDARY : MESH_STR_ENUM_CHANNEL_DISABLED);
}

static const char *pairing_enum_name(uint32_t mode) {
    switch (mode) {
    case 0U:
        return mesh_str(MESH_STR_ENUM_PAIRING_RANDOM_PIN);
    case 1U:
        return mesh_str(MESH_STR_ENUM_PAIRING_FIXED_PIN);
    case 2U:
        return mesh_str(MESH_STR_ENUM_PAIRING_NO_PIN);
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

/* Position precision is a bit count; the phone apps label the useful ones by distance. */
void mesh_ui_settings_format_precision(uint32_t bits, char *out, size_t out_len) {
    static const enum mesh_str_id k_distance[] = {
        MESH_STR_VALUE_PRECISION_23KM,  MESH_STR_VALUE_PRECISION_12KM,
        MESH_STR_VALUE_PRECISION_6KM,   MESH_STR_VALUE_PRECISION_3KM,
        MESH_STR_VALUE_PRECISION_1_5KM, MESH_STR_VALUE_PRECISION_730M,
        MESH_STR_VALUE_PRECISION_360M,  MESH_STR_VALUE_PRECISION_180M,
        MESH_STR_VALUE_PRECISION_90M,   MESH_STR_VALUE_PRECISION_45M,
    };
    if (bits == 0U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_VALUE_PRECISION_OFF));
    } else if (bits >= 32U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_VALUE_PRECISION_EXACT));
    } else if (bits >= 10U && bits <= 19U) {
        snprintf(out, out_len, "%s", mesh_str(k_distance[bits - 10U]));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_PRECISION_BITS, (unsigned)bits);
    }
}

/* Metres, which is what PositionConfig's smart-broadcast threshold is in. */
static void format_metres(uint32_t metres, char *out, size_t out_len) {
    if (metres == 0U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_DEFAULT));
    } else if (metres >= 1000U && metres % 1000U == 0U) {
        mesh_str_format(out, out_len, MESH_STR_VALUE_KILOMETRES, (unsigned)(metres / 1000U));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_METRES, (unsigned)metres);
    }
}

/*
 * The role names as a *chooser* shows them. ROUTER_CLIENT and REPEATER are still in the
 * protobuf and still what an older radio reports, so they have to be steppable - the row
 * could not otherwise display the setting a node already has - but they are marked so nobody
 * picks one on purpose. mesh_radio_role_name() stays unmarked: reading another node's role in
 * the Nodes tab is not making a choice.
 */
static const char *role_enum_name(uint32_t role) {
    switch (role) {
    case 3U:
        return mesh_str(MESH_STR_ENUM_ROLE_ROUTER_CLIENT);
    case 4U:
        return mesh_str(MESH_STR_ENUM_ROLE_REPEATER);
    default:
        return mesh_radio_role_name(role);
    }
}

static const char *region_enum_name(uint32_t region) { return mesh_radio_region_name(region); }
static const char *preset_enum_name(uint32_t preset) {
    return mesh_radio_modem_preset_name(preset);
}
static const char *signature_policy_name(uint32_t policy) {
    switch (policy) {
    case 0U:
        return mesh_str(MESH_STR_ENUM_SIGNATURE_COMPATIBLE);
    case 1U:
        return mesh_str(MESH_STR_ENUM_SIGNATURE_BALANCED);
    case 2U:
        return mesh_str(MESH_STR_ENUM_SIGNATURE_STRICT);
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

static void format_bandwidth(uint32_t khz, char *out, size_t out_len) {
    if (khz == 31U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_VALUE_BANDWIDTH_31));
    } else if (khz == 62U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_VALUE_BANDWIDTH_62));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_BANDWIDTH_KHZ, (unsigned)khz);
    }
}
static void format_plain(uint32_t value, char *out, size_t out_len) {
    mesh_str_format(out, out_len, MESH_STR_VALUE_PLAIN, (unsigned)value);
}
static void format_coding_rate(uint32_t value, char *out, size_t out_len) {
    mesh_str_format(out, out_len, MESH_STR_VALUE_CODING_RATE, (unsigned)value);
}
static void format_tx_power(uint32_t value, char *out, size_t out_len) {
    if (value == 0U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_VALUE_TX_POWER_MAX));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_DBM, (int)(int8_t)value);
    }
}

static const uint32_t k_bandwidth_presets[] = {31U, 62U, 125U, 250U, 500U};
static const uint32_t k_spread_presets[] = {7U, 8U, 9U, 10U, 11U, 12U};
static const uint32_t k_coding_presets[] = {5U, 6U, 7U, 8U};
static const uint32_t k_hop_presets[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
static const uint32_t k_tx_power_presets[] = {0U, 2U, 5U, 8U, 10U, 14U, 17U, 20U, 22U, 27U, 30U};

static const uint32_t k_precision_presets[] = {0U,  10U, 11U, 12U, 13U, 14U,
                                               15U, 16U, 17U, 18U, 19U, 32U};

/*
 * DeviceUIConfig's three number rows.
 *
 * Brightness is a level out of 255 and every stop is a real setting, so it is a plain scale
 * with no zero to stand outside it - a screen at 0 is a screen turned down, not a screen with
 * no brightness. The timeout's 0 is "never sleep", which is not a duration, so it stands
 * outside the track the way every other zero-as-a-word does. The ring tone is an index into
 * the firmware's own list of tunes: it names one rather than measuring anything, so it is a
 * NAMED_PRESETS run of every value the field can hold rather than a curated few.
 */
static const uint32_t k_ui_brightness_presets[] = {16U,  32U,  64U,  96U, 128U,
                                                   160U, 192U, 224U, 255U};
static const uint32_t k_ui_timeout_presets[] = {0U,   15U,  30U,  60U,   120U,
                                                300U, 600U, 900U, 1800U, 3600U};
static const uint32_t k_ui_ringtone_presets[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};

/* 0 means "firmware default" for these, and the presets are what the phone apps offer. */
static const uint32_t k_screen_on_presets[] = {0U,   15U,  30U,  60U,   120U,
                                               300U, 600U, 900U, 1800U, 3600U};
static const uint32_t k_carousel_presets[] = {0U, 10U, 15U, 30U, 60U, 120U, 300U, 600U};
static const uint32_t k_interval_presets[] = {0U,    60U,   300U,   900U,   1800U,
                                              3600U, 7200U, 14400U, 43200U, 86400U};

/* Device, Position and Power intervals. Each list starts at 0, which every one of these
   fields reads as "the firmware's own default" rather than as zero seconds. */
static const uint32_t k_nodeinfo_presets[] = {0U, 3600U, 7200U, 10800U, 21600U, 43200U, 86400U};
static const uint32_t k_gps_interval_presets[] = {0U, 30U, 60U, 120U, 300U, 600U, 900U, 1800U};
static const uint32_t k_smart_distance_presets[] = {0U, 10U, 25U, 50U, 100U, 250U, 500U, 1000U};
static const uint32_t k_smart_interval_presets[] = {0U, 30U, 60U, 120U, 300U, 600U};
static const uint32_t k_sleep_presets[] = {0U, 60U, 300U, 600U, 900U, 1800U, 3600U};
static const uint32_t k_wake_presets[] = {0U, 5U, 10U, 30U, 60U, 120U, 300U};
static const uint32_t k_wait_bt_presets[] = {0U, 10U, 30U, 60U, 120U, 300U};
static const uint32_t k_shutdown_presets[] = {0U,    300U,   1800U,  3600U,
                                              7200U, 21600U, 43200U, 86400U};

/* Store & Forward. The firmware sizes its own ring when `records` is 0, which on an ESP32
   with PSRAM is larger than a hand-picked number would be, so 0 stays the first preset and
   the rest are for a node deliberately kept small. `history_return_window` is seconds of
   backlog a client may ask for; the other two are counts, not seconds, which is why they
   carry format_count rather than the seconds default. */
static const uint32_t k_sf_records_presets[] = {0U, 25U, 50U, 100U, 250U, 500U, 1000U};
static const uint32_t k_sf_history_presets[] = {0U, 10U, 25U, 50U, 100U, 250U};
static const uint32_t k_sf_window_presets[] = {0U, 300U, 900U, 1800U, 3600U, 7200U, 86400U};

/* Map reporting. The public map drops anything under an hour, so the presets start there
   rather than offering a value the server will not honour. */
static const uint32_t k_map_interval_presets[] = {3600U, 7200U, 10800U, 21600U, 43200U, 86400U};

/* Neighbor info. The firmware floors the interval at 14400 (4 hours) and quietly raises
   anything under it, so the presets start there instead of showing a value that will not
   survive the read-back. */
static const uint32_t k_neighbor_presets[] = {14400U, 21600U, 43200U, 86400U};

/* Range test's sender interval. A sender transmits to everyone on the channel on this timer,
   so the list starts at 0 (receive only, which is what the module is for most of the time) and
   then at 30s - fast enough to walk a link out, slow enough not to swamp the channel. */
static const uint32_t k_range_test_presets[] = {0U, 30U, 60U, 120U, 300U, 600U, 1800U};

/*
 * Paxcounter's two RSSI floors, which are the only NUMBER rows in the client over a signed
 * value. They are stored as uint32_t like every other preset, through a cast.
 *
 * That works because they are all negative: two's-complement negatives cast to uint32_t all
 * land in the top half of the range and keep their relative order (-100 -> 0xFFFFFF9C is below
 * -80 -> 0xFFFFFFB0), so mesh_ui_settings_number_step() walks them correctly without knowing
 * they are signed. A list mixing signs would not step correctly, and there is no reason for one
 * here: an RSSI threshold above 0 dBm is not a threshold.
 */
#define RSSI(dbm) ((uint32_t)(int32_t)(dbm))
static const uint32_t k_rssi_presets[] = {RSSI(-100), RSSI(-95), RSSI(-90), RSSI(-85), RSSI(-80),
                                          RSSI(-75),  RSSI(-70), RSSI(-65), RSSI(-60)};

/*
 * GPIO pins: every number in the range, not a curated subset.
 *
 * The first version of this list was the pins an ESP32 typically exposes, which is exactly the
 * board-specific assumption there is no way to make here - nothing on the wire says what board
 * this is, and a RAK4631 puts usable output on 9, 10 and 28, all of which that list omitted and
 * so made unreachable. A contiguous range covers nRF52840 (P0.00-P1.15, flat 0-47) and ESP32-S3
 * (0-48) alike, and stepping it is not tedious because the d-pad autorepeats (mesh_ui_input
 * honours repeat for the navigation keys).
 *
 * Whether a pin is wired to anything remains the radio's business and unanswerable here; this
 * only rules out numbers no board has.
 */
static const uint32_t k_gpio_presets[] = {
    0U,  1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,  9U,  10U, 11U, 12U, 13U, 14U, 15U, 16U,
    17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U, 32U, 33U,
    34U, 35U, 36U, 37U, 38U, 39U, 40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U, 48U};

/* External notification's on-time and repeat: output_ms is milliseconds, nag_timeout seconds. */
static const uint32_t k_output_ms_presets[] = {100U, 250U, 500U, 1000U, 2000U, 5000U};
static const uint32_t k_nag_presets[] = {0U, 10U, 30U, 60U, 120U, 300U};

/* Detection sensor's two intervals: a floor between messages, and a heartbeat the proto says
   is off at 0. */
static const uint32_t k_detect_min_presets[] = {0U, 10U, 30U, 60U, 300U, 900U};
static const uint32_t k_detect_state_presets[] = {0U, 60U, 300U, 900U, 1800U, 3600U};

/* Traffic management. Every row is off at 0 by the module's own convention, so each list
   starts there; hops and packet counts are counts rather than seconds. */
static const uint32_t k_traffic_interval_presets[] = {0U, 30U, 60U, 120U, 300U, 600U, 1800U};
static const uint32_t k_traffic_hops_presets[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
static const uint32_t k_traffic_packets_presets[] = {0U, 5U, 10U, 20U, 50U, 100U, 200U};

/* Ambient lighting: an LED current in mA (the firmware's default is 10) and three 0-255
   channels. Three number rows rather than a colour picker, which a d-pad cannot do well. */
static const uint32_t k_led_current_presets[] = {0U, 5U, 10U, 15U, 20U, 25U, 30U};
static const uint32_t k_led_level_presets[] = {0U, 32U, 64U, 96U, 128U, 160U, 192U, 224U, 255U};

#define CHANNEL_KEY_CHOICES                                                                        \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_DEFAULT) |      \
     MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_128) |                                              \
     MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_NONE))
#define PRIVATE_KEY_CHOICES                                                                        \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256))
#define ADMIN_KEY_CHOICES                                                                          \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_NONE))

/* Milliseconds, for the notification's on-time: the seconds formatter would call 500 ms
   "500s". */
static void format_millis(uint32_t value, char *out, size_t out_len) {
    if (value % 1000U == 0U && value != 0U) {
        mesh_str_format(out, out_len, MESH_STR_VALUE_SECONDS, (unsigned)(value / 1000U));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_MILLISECONDS, (unsigned)value);
    }
}

/* A GPIO pin, or nothing at all. */
static void format_pin(uint32_t value, char *out, size_t out_len) {
    if (value == 0U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_VALUE_PIN_UNSET));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_PIN, (unsigned)value);
    }
}

/* Signed dBm, read back out of the uint32_t the preset table stores it in. */
static void format_rssi(uint32_t value, char *out, size_t out_len) {
    mesh_str_format(out, out_len, MESH_STR_VALUE_DBM, (int)(int32_t)value);
}

/* A plain 0-255 level, so an LED channel does not read as a duration. */
static void format_level(uint32_t value, char *out, size_t out_len) {
    mesh_str_format(out, out_len, MESH_STR_VALUE_PLAIN, (unsigned)value);
}

/* Milliamps, for the LED current row. */
static void format_milliamps(uint32_t value, char *out, size_t out_len) {
    mesh_str_format(out, out_len, MESH_STR_VALUE_MILLIAMPS, (unsigned)value);
}

/* NUMBER fields whose value is a count rather than a duration; without this the seconds
   formatter would render 100 records as "1m40s". */
static void format_count(uint32_t value, char *out, size_t out_len) {
    if (value == 0U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_DEFAULT));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_PLAIN, (unsigned)value);
    }
}

/*
 * A NUMBER field's presets, and what its numbers *are*.
 *
 * SCALE_PRESETS: they measure something - seconds, metres, packets, dBm - so where a value sits
 * between the ends of the list is a fact about it, and a row may draw that as a length.
 *
 * SCALE_PRESETS_AFTER_ZERO: all but the first. These lists open with a 0 the field reads as a
 * word - "default" on most of them, "max" on LoRa's transmit power - and a word is not a
 * quantity: the scale is what follows it, and 0 is off the track rather than at the bottom of
 * it. The distinction is not pedantic. Transmit power drawn the other way put "max" at the
 * empty end of its own bar.
 *
 * NAMED_PRESETS: they name something that happens to be written as a number - a GPIO pin, a
 * spreading factor, a bandwidth, a count of coordinate bits. Stepping them is still the right
 * way to edit them; drawing one as a length is a claim about magnitude that none of them makes.
 *
 * Every field states which, because nothing in a preset list says it: {0, 1, ... 7} is a hop
 * limit under MESH_UI_FIELD_LORA_HOPS and a pin under MESH_UI_FIELD_DETECT_PIN, and a leading 0
 * is "never" on one field and "the firmware decides" on the next.
 */
#define SCALE_PRESETS(array) (array), MESH_ARRAY_LEN(array), true, false
/* The same, for a list whose leading 0 is "the firmware's own default" or "as much as this
   radio has" rather than the bottom of the scale: the track spans what follows it. */
#define SCALE_PRESETS_AFTER_ZERO(array) (array), MESH_ARRAY_LEN(array), true, true
#define NAMED_PRESETS(array) (array), MESH_ARRAY_LEN(array), false, false
#define NO_PRESETS NULL, 0U, false, false

/* User.long_name is 39 bytes on the wire but the firmware truncates to 24 (mesh.proto). */
static const struct field_spec k_fields[MESH_UI_FIELD_COUNT] = {
    [MESH_UI_FIELD_NONE] = {MESH_STR_COMMON_UNKNOWN_SHORT, MESH_UI_SETTING_INFO,
                            MESH_UI_SETTINGS_SECTION_COUNT, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                            NULL, 0U},
    [MESH_UI_FIELD_USER_LONG_NAME] = {MESH_STR_SETTINGS_FIELD_USER_LONG_NAME, MESH_UI_SETTING_TEXT,
                                      MESH_UI_SETTINGS_USER, 24U, NULL, NO_PRESETS, MESH_STR_NONE,
                                      NULL, 0U},
    [MESH_UI_FIELD_USER_SHORT_NAME] = {MESH_STR_SETTINGS_FIELD_USER_SHORT_NAME,
                                       MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_USER, 4U, NULL,
                                       NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_USER_LICENSED] = {MESH_STR_SETTINGS_FIELD_USER_LICENSED, MESH_UI_SETTING_TOGGLE,
                                     MESH_UI_SETTINGS_USER, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                     NULL, 0U},
    [MESH_UI_FIELD_USER_UNMESSAGEABLE] = {MESH_STR_SETTINGS_FIELD_USER_UNMESSAGEABLE,
                                          MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_USER, 0U, NULL,
                                          NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    /* Thirteen values, two of them retired but still steppable: see role_enum_name(). */
    [MESH_UI_FIELD_DEVICE_ROLE] = {MESH_STR_SETTINGS_FIELD_DEVICE_ROLE, MESH_UI_SETTING_ENUM,
                                   MESH_UI_SETTINGS_DEVICE, 13U, role_enum_name, NO_PRESETS,
                                   MESH_STR_NONE, NULL, 0U},
    /* tzdef is 64 bytes on the wire. The radio applies it to its own clock only; it has no
       bearing on what this client shows, which follows the Brick's own TZ. */
    [MESH_UI_FIELD_DEVICE_TZDEF] = {MESH_STR_SETTINGS_FIELD_DEVICE_TZDEF, MESH_UI_SETTING_TEXT,
                                    MESH_UI_SETTINGS_DEVICE, 64U, NULL, NO_PRESETS, MESH_STR_NONE,
                                    NULL, 0U},
    [MESH_UI_FIELD_DEVICE_REBROADCAST] = {MESH_STR_SETTINGS_FIELD_DEVICE_REBROADCAST,
                                          MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DEVICE, 6U,
                                          rebroadcast_name, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_DEVICE_NODEINFO_SECS] = {MESH_STR_SETTINGS_FIELD_DEVICE_NODEINFO_SECS,
                                            MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_DEVICE, 0U,
                                            NULL, SCALE_PRESETS_AFTER_ZERO(k_nodeinfo_presets),
                                            MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_DEVICE_LED_HEARTBEAT] = {MESH_STR_SETTINGS_FIELD_DEVICE_LED_HEARTBEAT,
                                            MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_DEVICE, 0U,
                                            NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_DEVICE_DOUBLE_TAP] = {MESH_STR_SETTINGS_FIELD_DEVICE_DOUBLE_TAP,
                                         MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_DEVICE, 0U, NULL,
                                         NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_POSITION_GPS_MODE] = {MESH_STR_SETTINGS_FIELD_POSITION_GPS_MODE,
                                         MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_POSITION, 3U,
                                         gps_mode_name, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_POSITION_BROADCAST_SECS] = {MESH_STR_SETTINGS_FIELD_POSITION_BROADCAST_SECS,
                                               MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POSITION,
                                               0U, NULL,
                                               SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                               MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_POSITION_SMART] = {MESH_STR_SETTINGS_FIELD_POSITION_SMART,
                                      MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_POSITION, 0U, NULL,
                                      NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_POSITION_SMART_DISTANCE] = {MESH_STR_SETTINGS_FIELD_POSITION_SMART_DISTANCE,
                                               MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POSITION,
                                               0U, NULL, SCALE_PRESETS(k_smart_distance_presets),
                                               MESH_STR_NONE, format_metres, 0U},
    [MESH_UI_FIELD_POSITION_SMART_INTERVAL] = {MESH_STR_SETTINGS_FIELD_POSITION_SMART_INTERVAL,
                                               MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POSITION,
                                               0U, NULL,
                                               SCALE_PRESETS_AFTER_ZERO(k_smart_interval_presets),
                                               MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_POSITION_GPS_INTERVAL] = {MESH_STR_SETTINGS_FIELD_POSITION_GPS_INTERVAL,
                                             MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POSITION, 0U,
                                             NULL, SCALE_PRESETS_AFTER_ZERO(k_gps_interval_presets),
                                             MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_POSITION_LATITUDE] = {MESH_STR_SETTINGS_FIELD_POSITION_LATITUDE,
                                         MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_POSITION, 15U, NULL,
                                         NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_POSITION_LONGITUDE] = {MESH_STR_SETTINGS_FIELD_POSITION_LONGITUDE,
                                          MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_POSITION, 15U,
                                          NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_POSITION_ALTITUDE] = {MESH_STR_SETTINGS_FIELD_POSITION_ALTITUDE,
                                         MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_POSITION, 7U, NULL,
                                         NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_POWER_SAVING] = {MESH_STR_SETTINGS_FIELD_POWER_SAVING, MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_POWER, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                    NULL, 0U},
    [MESH_UI_FIELD_POWER_LS_SECS] = {MESH_STR_SETTINGS_FIELD_POWER_LS_SECS, MESH_UI_SETTING_NUMBER,
                                     MESH_UI_SETTINGS_POWER, 0U, NULL,
                                     SCALE_PRESETS_AFTER_ZERO(k_sleep_presets),
                                     MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_POWER_MIN_WAKE] = {MESH_STR_SETTINGS_FIELD_POWER_MIN_WAKE,
                                      MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POWER, 0U, NULL,
                                      SCALE_PRESETS_AFTER_ZERO(k_wake_presets),
                                      MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_POWER_WAIT_BT] = {MESH_STR_SETTINGS_FIELD_POWER_WAIT_BT, MESH_UI_SETTING_NUMBER,
                                     MESH_UI_SETTINGS_POWER, 0U, NULL,
                                     SCALE_PRESETS_AFTER_ZERO(k_wait_bt_presets),
                                     MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_POWER_SHUTDOWN] = {MESH_STR_SETTINGS_FIELD_POWER_SHUTDOWN,
                                      MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POWER, 0U, NULL,
                                      SCALE_PRESETS(k_shutdown_presets), MESH_STR_ZERO_OFF, NULL,
                                      0U},
    [MESH_UI_FIELD_DISPLAY_SCREEN_ON] = {MESH_STR_SETTINGS_FIELD_DISPLAY_SCREEN_ON,
                                         MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                         SCALE_PRESETS_AFTER_ZERO(k_screen_on_presets),
                                         MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_CAROUSEL] = {MESH_STR_SETTINGS_FIELD_DISPLAY_CAROUSEL,
                                        MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                        SCALE_PRESETS(k_carousel_presets), MESH_STR_ZERO_OFF, NULL,
                                        0U},
    [MESH_UI_FIELD_DISPLAY_COMPASS] = {MESH_STR_SETTINGS_FIELD_DISPLAY_COMPASS,
                                       MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DISPLAY, 8U,
                                       compass_name, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_12H] = {MESH_STR_SETTINGS_FIELD_DISPLAY_12H, MESH_UI_SETTING_TOGGLE,
                                   MESH_UI_SETTINGS_DISPLAY, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                   NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_UNITS] = {MESH_STR_SETTINGS_FIELD_DISPLAY_UNITS, MESH_UI_SETTING_ENUM,
                                     MESH_UI_SETTINGS_DISPLAY, 2U, units_name, NO_PRESETS,
                                     MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_FLIP] = {MESH_STR_SETTINGS_FIELD_DISPLAY_FLIP, MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_DISPLAY, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                    NULL, 0U},
    [MESH_UI_FIELD_MQTT_ENABLED] = {MESH_STR_SETTINGS_FIELD_MQTT_ENABLED, MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_MQTT, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                    NULL, 0U},
    [MESH_UI_FIELD_MQTT_ADDRESS] = {MESH_STR_SETTINGS_FIELD_MQTT_ADDRESS, MESH_UI_SETTING_TEXT,
                                    MESH_UI_SETTINGS_MQTT, 63U, NULL, NO_PRESETS, MESH_STR_NONE,
                                    NULL, 0U},
    [MESH_UI_FIELD_MQTT_USERNAME] = {MESH_STR_SETTINGS_FIELD_MQTT_USERNAME, MESH_UI_SETTING_TEXT,
                                     MESH_UI_SETTINGS_MQTT, 63U, NULL, NO_PRESETS, MESH_STR_NONE,
                                     NULL, 0U},
    [MESH_UI_FIELD_MQTT_PASSWORD] = {MESH_STR_SETTINGS_FIELD_MQTT_PASSWORD, MESH_UI_SETTING_TEXT,
                                     MESH_UI_SETTINGS_MQTT, 31U, NULL, NO_PRESETS, MESH_STR_NONE,
                                     NULL, 0U},
    [MESH_UI_FIELD_MQTT_ROOT] = {MESH_STR_SETTINGS_FIELD_MQTT_ROOT, MESH_UI_SETTING_TEXT,
                                 MESH_UI_SETTINGS_MQTT, 31U, NULL, NO_PRESETS, MESH_STR_NONE, NULL,
                                 0U},
    [MESH_UI_FIELD_MQTT_ENCRYPTION] = {MESH_STR_SETTINGS_FIELD_MQTT_ENCRYPTION,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                       NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_MQTT_TLS] = {MESH_STR_SETTINGS_FIELD_MQTT_TLS, MESH_UI_SETTING_TOGGLE,
                                MESH_UI_SETTINGS_MQTT, 0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL,
                                0U},
    /* Spelled out: this one publishes the node's position to a public map, which is not what
       "map reporting" reads as to somebody stepping through toggles. */
    [MESH_UI_FIELD_MQTT_MAP_REPORTING] = {MESH_STR_SETTINGS_FIELD_MQTT_MAP_REPORTING,
                                          MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                          NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_MQTT_MAP_INTERVAL] = {MESH_STR_SETTINGS_FIELD_MQTT_MAP_INTERVAL,
                                         MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                         SCALE_PRESETS(k_map_interval_presets), MESH_STR_NONE, NULL,
                                         0U},
    [MESH_UI_FIELD_MQTT_MAP_PRECISION] = {MESH_STR_SETTINGS_FIELD_MQTT_MAP_PRECISION,
                                          MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                          NAMED_PRESETS(k_precision_presets), MESH_STR_ZERO_OFF,
                                          mesh_ui_settings_format_precision, 0U},
    [MESH_UI_FIELD_MQTT_MAP_LOCATION] = {MESH_STR_SETTINGS_FIELD_MQTT_MAP_LOCATION,
                                         MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                         NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SF_ENABLED] = {MESH_STR_SETTINGS_FIELD_SF_ENABLED, MESH_UI_SETTING_TOGGLE,
                                  MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS,
                                  MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SF_HEARTBEAT] = {MESH_STR_SETTINGS_FIELD_SF_HEARTBEAT, MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS,
                                    MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SF_SERVER] = {MESH_STR_SETTINGS_FIELD_SF_SERVER, MESH_UI_SETTING_TOGGLE,
                                 MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS,
                                 MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SF_RECORDS] = {MESH_STR_SETTINGS_FIELD_SF_RECORDS, MESH_UI_SETTING_NUMBER,
                                  MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL,
                                  SCALE_PRESETS_AFTER_ZERO(k_sf_records_presets), MESH_STR_NONE,
                                  format_count, 0U},
    [MESH_UI_FIELD_SF_HISTORY_MAX] = {MESH_STR_SETTINGS_FIELD_SF_HISTORY_MAX,
                                      MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_STORE_FORWARD, 0U,
                                      NULL, SCALE_PRESETS_AFTER_ZERO(k_sf_history_presets),
                                      MESH_STR_NONE, format_count, 0U},
    [MESH_UI_FIELD_SF_HISTORY_WINDOW] = {MESH_STR_SETTINGS_FIELD_SF_HISTORY_WINDOW,
                                         MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_STORE_FORWARD, 0U,
                                         NULL, SCALE_PRESETS_AFTER_ZERO(k_sf_window_presets),
                                         MESH_STR_ZERO_DEFAULT, NULL, 0U},
    /* The five telemetry groups each sit under a heading, so their rows are named for what
       they are inside the group rather than repeating it ("Enabled", not "Env enabled"). The
       only consumer of a field label outside the row list is the keyboard title, and none of
       these is a TEXT field. */
    [MESH_UI_FIELD_TELEMETRY_DEVICE] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_DEVICE,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                        NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_INTERVAL] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_INTERVAL,
                                          MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                          NULL, SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                          MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENVIRONMENT] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_ENVIRONMENT,
                                             MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                             NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_INTERVAL] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_ENV_INTERVAL,
                                              MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TELEMETRY,
                                              0U,
                                              NULL, SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                              MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_SCREEN] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_ENV_SCREEN,
                                            MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                            NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_ENV_FAHRENHEIT,
                                                MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY,
                                                0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_AIR_QUALITY] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_AIR_QUALITY,
                                             MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                             NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_AIR_INTERVAL,
                                              MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TELEMETRY,
                                              0U,
                                              NULL, SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                              MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_AIR_SCREEN] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_AIR_SCREEN,
                                            MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                            NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_POWER] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_POWER,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                       NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_POWER_INTERVAL] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_POWER_INTERVAL,
                                                MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TELEMETRY,
                                                0U, NULL,
                                                SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                                MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_POWER_SCREEN] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_POWER_SCREEN,
                                              MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY,
                                              0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_HEALTH] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_HEALTH,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                        NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_HEALTH_INTERVAL,
                                                 MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TELEMETRY,
                                                 0U, NULL,
                                                 SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                                 MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_HEALTH_SCREEN] = {MESH_STR_SETTINGS_FIELD_TELEMETRY_HEALTH_SCREEN,
                                               MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_TELEMETRY,
                                               0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_NAME] = {MESH_STR_SETTINGS_FIELD_CHANNEL_NAME, MESH_UI_SETTING_TEXT,
                                    MESH_UI_SETTINGS_CHANNELS, 11U, NULL, NO_PRESETS, MESH_STR_NONE,
                                    NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_ROLE] = {MESH_STR_SETTINGS_FIELD_CHANNEL_ROLE, MESH_UI_SETTING_ENUM,
                                    MESH_UI_SETTINGS_CHANNELS, 2U, channel_role_name, NO_PRESETS,
                                    MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_KEY] = {MESH_STR_SETTINGS_FIELD_CHANNEL_KEY, MESH_UI_SETTING_KEY,
                                   MESH_UI_SETTINGS_CHANNELS, 64U, NULL, NO_PRESETS, MESH_STR_NONE,
                                   NULL, CHANNEL_KEY_CHOICES},
    [MESH_UI_FIELD_CHANNEL_UPLINK] = {MESH_STR_SETTINGS_FIELD_CHANNEL_UPLINK,
                                      MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_CHANNELS, 0U, NULL,
                                      NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_DOWNLINK] = {MESH_STR_SETTINGS_FIELD_CHANNEL_DOWNLINK,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_CHANNELS, 0U, NULL,
                                        NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_POSITION] = {MESH_STR_SETTINGS_FIELD_CHANNEL_POSITION,
                                        MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_CHANNELS, 0U, NULL,
                                        NAMED_PRESETS(k_precision_presets), MESH_STR_ZERO_OFF,
                                        mesh_ui_settings_format_precision, 0U},
    [MESH_UI_FIELD_BT_ENABLED] = {MESH_STR_SETTINGS_FIELD_BT_ENABLED, MESH_UI_SETTING_TOGGLE,
                                  MESH_UI_SETTINGS_BLUETOOTH, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                  NULL, 0U},
    [MESH_UI_FIELD_BT_MODE] = {MESH_STR_SETTINGS_FIELD_BT_MODE, MESH_UI_SETTING_ENUM,
                               MESH_UI_SETTINGS_BLUETOOTH, 3U, pairing_enum_name, NO_PRESETS,
                               MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_BT_PIN] = {MESH_STR_SETTINGS_FIELD_BT_PIN, MESH_UI_SETTING_TEXT,
                              MESH_UI_SETTINGS_BLUETOOTH, 6U, NULL, NO_PRESETS, MESH_STR_NONE, NULL,
                              0U},
    [MESH_UI_FIELD_LORA_REGION] = {MESH_STR_SETTINGS_FIELD_LORA_REGION, MESH_UI_SETTING_ENUM,
                                   MESH_UI_SETTINGS_LORA, 38U, region_enum_name, NO_PRESETS,
                                   MESH_STR_NONE, NULL, 0U, MESH_STR_SETTINGS_NOTE_LORA_REGION},
    [MESH_UI_FIELD_LORA_USE_PRESET] = {MESH_STR_SETTINGS_FIELD_LORA_USE_PRESET,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                       NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_LORA_PRESET] = {MESH_STR_SETTINGS_FIELD_LORA_PRESET, MESH_UI_SETTING_ENUM,
                                   MESH_UI_SETTINGS_LORA, 17U, preset_enum_name, NO_PRESETS,
                                   MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_LORA_BANDWIDTH] = {MESH_STR_SETTINGS_FIELD_LORA_BANDWIDTH,
                                      MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                      NAMED_PRESETS(k_bandwidth_presets), MESH_STR_NONE,
                                      format_bandwidth, 0U},
    [MESH_UI_FIELD_LORA_SPREAD] = {MESH_STR_SETTINGS_FIELD_LORA_SPREAD, MESH_UI_SETTING_NUMBER,
                                   MESH_UI_SETTINGS_LORA, 0U, NULL, NAMED_PRESETS(k_spread_presets),
                                   MESH_STR_NONE, format_plain, 0U,
                                   MESH_STR_SETTINGS_NOTE_LORA_SPREAD},
    [MESH_UI_FIELD_LORA_CODING] = {MESH_STR_SETTINGS_FIELD_LORA_CODING, MESH_UI_SETTING_NUMBER,
                                   MESH_UI_SETTINGS_LORA, 0U, NULL, NAMED_PRESETS(k_coding_presets),
                                   MESH_STR_NONE, format_coding_rate, 0U,
                                   MESH_STR_SETTINGS_NOTE_LORA_CODING},
    [MESH_UI_FIELD_LORA_HOPS] = {MESH_STR_SETTINGS_FIELD_LORA_HOPS, MESH_UI_SETTING_NUMBER,
                                 MESH_UI_SETTINGS_LORA, 0U, NULL, SCALE_PRESETS(k_hop_presets),
                                 MESH_STR_NONE, format_plain, 0U, MESH_STR_SETTINGS_NOTE_LORA_HOPS},
    [MESH_UI_FIELD_LORA_TX_ENABLED] = {MESH_STR_SETTINGS_FIELD_LORA_TX_ENABLED,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                       NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_LORA_TX_POWER] = {MESH_STR_SETTINGS_FIELD_LORA_TX_POWER, MESH_UI_SETTING_NUMBER,
                                     MESH_UI_SETTINGS_LORA, 0U, NULL,
                                     SCALE_PRESETS_AFTER_ZERO(k_tx_power_presets), MESH_STR_NONE,
                                     format_tx_power, 0U, MESH_STR_SETTINGS_NOTE_LORA_TX_POWER},
    [MESH_UI_FIELD_LORA_IGNORE_MQTT] = {MESH_STR_SETTINGS_FIELD_LORA_IGNORE_MQTT,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                        NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_LORA_OK_TO_MQTT] = {MESH_STR_SETTINGS_FIELD_LORA_OK_TO_MQTT,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                       NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SECURITY_PRIVATE_KEY] = {MESH_STR_SETTINGS_FIELD_SECURITY_PRIVATE_KEY,
                                            MESH_UI_SETTING_KEY, MESH_UI_SETTINGS_SECURITY, 64U,
                                            NULL, NO_PRESETS, MESH_STR_NONE, NULL,
                                            PRIVATE_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_0] = {MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_KEY_0,
                                            MESH_UI_SETTING_KEY, MESH_UI_SETTINGS_SECURITY, 64U,
                                            NULL, NO_PRESETS, MESH_STR_NONE, NULL,
                                            ADMIN_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_1] = {MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_KEY_1,
                                            MESH_UI_SETTING_KEY, MESH_UI_SETTINGS_SECURITY, 64U,
                                            NULL, NO_PRESETS, MESH_STR_NONE, NULL,
                                            ADMIN_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_2] = {MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_KEY_2,
                                            MESH_UI_SETTING_KEY, MESH_UI_SETTINGS_SECURITY, 64U,
                                            NULL, NO_PRESETS, MESH_STR_NONE, NULL,
                                            ADMIN_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_MANAGED] = {MESH_STR_SETTINGS_FIELD_SECURITY_MANAGED,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U, NULL,
                                        NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL] = {MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_CHANNEL,
                                              MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U,
                                              NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SECURITY_SERIAL] = {MESH_STR_SETTINGS_FIELD_SECURITY_SERIAL,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U, NULL,
                                       NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_SECURITY_DEBUG_LOG] = {MESH_STR_SETTINGS_FIELD_SECURITY_DEBUG_LOG,
                                          MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U,
                                          NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_NEIGHBOR_ENABLED] = {MESH_STR_SETTINGS_FIELD_NEIGHBOR_ENABLED,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U,
                                        NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_NEIGHBOR_INTERVAL] = {MESH_STR_SETTINGS_FIELD_NEIGHBOR_INTERVAL,
                                         MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U,
                                         NULL, SCALE_PRESETS(k_neighbor_presets), MESH_STR_NONE,
                                         NULL, 0U},
    [MESH_UI_FIELD_NEIGHBOR_OVER_LORA] = {MESH_STR_SETTINGS_FIELD_NEIGHBOR_OVER_LORA,
                                          MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_NEIGHBOR_INFO,
                                          0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_ENABLED] = {MESH_STR_SETTINGS_FIELD_RANGE_TEST_ENABLED,
                                          MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_RANGE_TEST, 0U,
                                          NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_SENDER] = {MESH_STR_SETTINGS_FIELD_RANGE_TEST_SENDER,
                                         MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_RANGE_TEST, 0U,
                                         NULL, SCALE_PRESETS(k_range_test_presets),
                                         MESH_STR_ZERO_NEVER, NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_SAVE] = {MESH_STR_SETTINGS_FIELD_RANGE_TEST_SAVE,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_RANGE_TEST, 0U,
                                       NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_CLEAR] = {MESH_STR_SETTINGS_FIELD_RANGE_TEST_CLEAR,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_RANGE_TEST, 0U,
                                        NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_PAX_ENABLED] = {MESH_STR_SETTINGS_FIELD_PAX_ENABLED, MESH_UI_SETTING_TOGGLE,
                                   MESH_UI_SETTINGS_PAXCOUNTER, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                   NULL, 0U},
    [MESH_UI_FIELD_PAX_INTERVAL] = {MESH_STR_SETTINGS_FIELD_PAX_INTERVAL, MESH_UI_SETTING_NUMBER,
                                    MESH_UI_SETTINGS_PAXCOUNTER, 0U, NULL,
                                    SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                    MESH_STR_ZERO_DEFAULT, NULL, 0U},
    [MESH_UI_FIELD_PAX_WIFI_THRESHOLD] = {MESH_STR_SETTINGS_FIELD_PAX_WIFI_THRESHOLD,
                                          MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_PAXCOUNTER, 0U,
                                          NULL, SCALE_PRESETS(k_rssi_presets), MESH_STR_NONE,
                                          format_rssi, 0U},
    [MESH_UI_FIELD_PAX_BLE_THRESHOLD] = {MESH_STR_SETTINGS_FIELD_PAX_BLE_THRESHOLD,
                                         MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_PAXCOUNTER, 0U,
                                         NULL, SCALE_PRESETS(k_rssi_presets), MESH_STR_NONE,
                                         format_rssi, 0U},
    [MESH_UI_FIELD_TAK_TEAM] = {MESH_STR_SETTINGS_FIELD_TAK_TEAM, MESH_UI_SETTING_ENUM,
                                MESH_UI_SETTINGS_TAK, 15U, tak_team_name, NO_PRESETS, MESH_STR_NONE,
                                NULL, 0U},
    [MESH_UI_FIELD_TAK_ROLE] = {MESH_STR_SETTINGS_FIELD_TAK_ROLE, MESH_UI_SETTING_ENUM,
                                MESH_UI_SETTINGS_TAK, 9U, tak_role_name, NO_PRESETS, MESH_STR_NONE,
                                NULL, 0U},
    [MESH_UI_FIELD_AMBIENT_LED] = {MESH_STR_SETTINGS_FIELD_AMBIENT_LED, MESH_UI_SETTING_TOGGLE,
                                   MESH_UI_SETTINGS_AMBIENT, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                   NULL, 0U},
    [MESH_UI_FIELD_AMBIENT_CURRENT] = {MESH_STR_SETTINGS_FIELD_AMBIENT_CURRENT,
                                       MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                       SCALE_PRESETS(k_led_current_presets), MESH_STR_NONE,
                                       format_milliamps, 0U},
    [MESH_UI_FIELD_AMBIENT_RED] = {MESH_STR_SETTINGS_FIELD_AMBIENT_RED, MESH_UI_SETTING_NUMBER,
                                   MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                   SCALE_PRESETS(k_led_level_presets), MESH_STR_NONE, format_level,
                                   0U},
    [MESH_UI_FIELD_AMBIENT_GREEN] = {MESH_STR_SETTINGS_FIELD_AMBIENT_GREEN, MESH_UI_SETTING_NUMBER,
                                     MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                     SCALE_PRESETS(k_led_level_presets), MESH_STR_NONE,
                                     format_level, 0U},
    [MESH_UI_FIELD_AMBIENT_BLUE] = {MESH_STR_SETTINGS_FIELD_AMBIENT_BLUE, MESH_UI_SETTING_NUMBER,
                                    MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                    SCALE_PRESETS(k_led_level_presets), MESH_STR_NONE, format_level,
                                    0U},
    /* 79 bytes on the wire; MESH_UI_SETTING_TEXT_MAX was raised to 80 to hold it. */
    [MESH_UI_FIELD_STATUS_TEXT] = {MESH_STR_SETTINGS_FIELD_STATUS_TEXT, MESH_UI_SETTING_TEXT,
                                   MESH_UI_SETTINGS_STATUS_MESSAGE, 79U, NULL, NO_PRESETS,
                                   MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_DETECT_ENABLED] = {MESH_STR_SETTINGS_FIELD_DETECT_ENABLED,
                                      MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_DETECTION, 0U, NULL,
                                      NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    /* 20 bytes on the wire including the NUL, so 19 typed. */
    [MESH_UI_FIELD_DETECT_NAME] = {MESH_STR_SETTINGS_FIELD_DETECT_NAME, MESH_UI_SETTING_TEXT,
                                   MESH_UI_SETTINGS_DETECTION, 19U, NULL, NO_PRESETS, MESH_STR_NONE,
                                   NULL, 0U},
    [MESH_UI_FIELD_DETECT_MIN_BROADCAST] = {MESH_STR_SETTINGS_FIELD_DETECT_MIN_BROADCAST,
                                            MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_DETECTION, 0U,
                                            NULL, SCALE_PRESETS(k_detect_min_presets),
                                            MESH_STR_ZERO_NONE, NULL, 0U},
    [MESH_UI_FIELD_DETECT_STATE_BROADCAST] = {MESH_STR_SETTINGS_FIELD_DETECT_STATE_BROADCAST,
                                              MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_DETECTION,
                                              0U, NULL, SCALE_PRESETS(k_detect_state_presets),
                                              MESH_STR_ZERO_OFF, NULL, 0U},
    [MESH_UI_FIELD_DETECT_SEND_BELL] = {MESH_STR_SETTINGS_FIELD_DETECT_SEND_BELL,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_DETECTION, 0U,
                                        NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_DETECT_PIN] = {MESH_STR_SETTINGS_FIELD_DETECT_PIN, MESH_UI_SETTING_NUMBER,
                                  MESH_UI_SETTINGS_DETECTION, 0U, NULL,
                                  NAMED_PRESETS(k_gpio_presets), MESH_STR_NONE, format_pin, 0U},
    [MESH_UI_FIELD_DETECT_TRIGGER] = {MESH_STR_SETTINGS_FIELD_DETECT_TRIGGER, MESH_UI_SETTING_ENUM,
                                      MESH_UI_SETTINGS_DETECTION, 6U, trigger_name, NO_PRESETS,
                                      MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_DETECT_PULLUP] = {MESH_STR_SETTINGS_FIELD_DETECT_PULLUP, MESH_UI_SETTING_TOGGLE,
                                     MESH_UI_SETTINGS_DETECTION, 0U, NULL, NO_PRESETS,
                                     MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ENABLED] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ENABLED,
                                        MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                        0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ACTIVE] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ACTIVE,
                                       MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                       0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_OUTPUT_MS] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_OUTPUT_MS,
                                          MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                          0U, NULL, SCALE_PRESETS(k_output_ms_presets),
                                          MESH_STR_NONE, format_millis, 0U},
    [MESH_UI_FIELD_EXTNOTIF_NAG] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_NAG, MESH_UI_SETTING_NUMBER,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                    SCALE_PRESETS(k_nag_presets), MESH_STR_ZERO_ONCE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_PWM] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_PWM, MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS,
                                    MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_I2S] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_I2S, MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS,
                                    MESH_STR_NONE, NULL, 0U},
    /* The three output groups. Each row is named for what it is inside its group, the way the
       telemetry groups are, because the heading above it says which output it belongs to. */
    [MESH_UI_FIELD_EXTNOTIF_PIN] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_PIN, MESH_UI_SETTING_NUMBER,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                    NAMED_PRESETS(k_gpio_presets), MESH_STR_NONE, format_pin, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_MSG,
                                          MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                          0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_BELL,
                                           MESH_UI_SETTING_TOGGLE,
                                           MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS,
                                           MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_PIN_VIBRA,
                                          MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                          0U, NULL, NAMED_PRESETS(k_gpio_presets), MESH_STR_NONE,
                                          format_pin, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_MSG_VIBRA,
                                                MESH_UI_SETTING_TOGGLE,
                                                MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_BELL_VIBRA,
                                                 MESH_UI_SETTING_TOGGLE,
                                                 MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                 NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_PIN_BUZZER,
                                           MESH_UI_SETTING_NUMBER,
                                           MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                           NAMED_PRESETS(k_gpio_presets), MESH_STR_NONE, format_pin,
                                           0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER] = {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_MSG_BUZZER,
                                                 MESH_UI_SETTING_TOGGLE,
                                                 MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                 NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER] =
        {MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_BELL_BUZZER, MESH_UI_SETTING_TOGGLE,
         MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL] = {MESH_STR_SETTINGS_FIELD_TRAFFIC_POSITION_INTERVAL,
                                                 MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TRAFFIC,
                                                 0U, NULL,
                                                 SCALE_PRESETS(k_traffic_interval_presets),
                                                 MESH_STR_ZERO_OFF, NULL, 0U},
    [MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS] = {MESH_STR_SETTINGS_FIELD_TRAFFIC_NODEINFO_HOPS,
                                             MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TRAFFIC, 0U,
                                             NULL, SCALE_PRESETS(k_traffic_hops_presets),
                                             MESH_STR_ZERO_OFF, format_count, 0U},
    [MESH_UI_FIELD_TRAFFIC_RATE_WINDOW] = {MESH_STR_SETTINGS_FIELD_TRAFFIC_RATE_WINDOW,
                                           MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TRAFFIC, 0U,
                                           NULL, SCALE_PRESETS(k_traffic_interval_presets),
                                           MESH_STR_ZERO_OFF, NULL, 0U},
    [MESH_UI_FIELD_TRAFFIC_RATE_PACKETS] = {MESH_STR_SETTINGS_FIELD_TRAFFIC_RATE_PACKETS,
                                            MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TRAFFIC, 0U,
                                            NULL, SCALE_PRESETS(k_traffic_packets_presets),
                                            MESH_STR_ZERO_OFF, format_count, 0U},
    [MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD] = {MESH_STR_SETTINGS_FIELD_TRAFFIC_UNKNOWN_THRESHOLD,
                                                 MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_TRAFFIC,
                                                 0U, NULL, SCALE_PRESETS(k_traffic_packets_presets),
                                                 MESH_STR_ZERO_OFF, format_count, 0U},
    [MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY] = {MESH_STR_SETTINGS_FIELD_SECURITY_SIGNATURE_POLICY,
                                                 MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_SECURITY,
                                                 3U, signature_policy_name, NO_PRESETS,
                                                 MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_UI_THEME] = {MESH_STR_SETTINGS_FIELD_UI_THEME, MESH_UI_SETTING_ENUM,
                                MESH_UI_SETTINGS_RADIO_UI, 3U, ui_theme_name, NO_PRESETS,
                                MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_UI_BRIGHTNESS] = {MESH_STR_SETTINGS_FIELD_UI_BRIGHTNESS, MESH_UI_SETTING_NUMBER,
                                     MESH_UI_SETTINGS_RADIO_UI, 0U, NULL,
                                     SCALE_PRESETS(k_ui_brightness_presets), MESH_STR_NONE,
                                     format_plain, 0U},
    [MESH_UI_FIELD_UI_SCREEN_TIMEOUT] = {MESH_STR_SETTINGS_FIELD_UI_SCREEN_TIMEOUT,
                                         MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_RADIO_UI, 0U,
                                         NULL, SCALE_PRESETS_AFTER_ZERO(k_ui_timeout_presets),
                                         MESH_STR_ZERO_NEVER, NULL, 0U},
    [MESH_UI_FIELD_UI_ALERT] = {MESH_STR_SETTINGS_FIELD_UI_ALERT, MESH_UI_SETTING_TOGGLE,
                                MESH_UI_SETTINGS_RADIO_UI, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                NULL, 0U},
    [MESH_UI_FIELD_UI_BANNER] = {MESH_STR_SETTINGS_FIELD_UI_BANNER, MESH_UI_SETTING_TOGGLE,
                                 MESH_UI_SETTINGS_RADIO_UI, 0U, NULL, NO_PRESETS, MESH_STR_NONE,
                                 NULL, 0U},
    [MESH_UI_FIELD_UI_RING_TONE] = {MESH_STR_SETTINGS_FIELD_UI_RING_TONE, MESH_UI_SETTING_NUMBER,
                                    MESH_UI_SETTINGS_RADIO_UI, 0U, NULL,
                                    NAMED_PRESETS(k_ui_ringtone_presets), MESH_STR_NONE,
                                    format_plain, 0U},
    [MESH_UI_FIELD_UI_COMPASS_MODE] = {MESH_STR_SETTINGS_FIELD_UI_COMPASS_MODE,
                                       MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_RADIO_UI, 3U,
                                       ui_compass_name, NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_UI_GPS_FORMAT] = {MESH_STR_SETTINGS_FIELD_UI_GPS_FORMAT, MESH_UI_SETTING_ENUM,
                                     MESH_UI_SETTINGS_RADIO_UI, 7U, ui_gps_format_name, NO_PRESETS,
                                     MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_UI_CLOCKFACE] = {MESH_STR_SETTINGS_FIELD_UI_CLOCKFACE, MESH_UI_SETTING_ENUM,
                                    MESH_UI_SETTINGS_RADIO_UI, 2U, ui_clockface_name, NO_PRESETS,
                                    MESH_STR_NONE, NULL, 0U},
    /* The per-slot cap, not the wire's 200: see MESH_UI_CANNED_SLOTS for why the two are
       chosen together. The keyboard reads `limit` as the number of bytes it may commit. */
    [MESH_UI_FIELD_CANNED_0] = {MESH_STR_SETTINGS_FIELD_CANNED_0, MESH_UI_SETTING_TEXT,
                                MESH_UI_SETTINGS_CANNED, MESH_UI_CANNED_SLOT_MAX - 1U, NULL,
                                NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CANNED_1] = {MESH_STR_SETTINGS_FIELD_CANNED_1, MESH_UI_SETTING_TEXT,
                                MESH_UI_SETTINGS_CANNED, MESH_UI_CANNED_SLOT_MAX - 1U, NULL,
                                NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CANNED_2] = {MESH_STR_SETTINGS_FIELD_CANNED_2, MESH_UI_SETTING_TEXT,
                                MESH_UI_SETTINGS_CANNED, MESH_UI_CANNED_SLOT_MAX - 1U, NULL,
                                NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CANNED_3] = {MESH_STR_SETTINGS_FIELD_CANNED_3, MESH_UI_SETTING_TEXT,
                                MESH_UI_SETTINGS_CANNED, MESH_UI_CANNED_SLOT_MAX - 1U, NULL,
                                NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CANNED_4] = {MESH_STR_SETTINGS_FIELD_CANNED_4, MESH_UI_SETTING_TEXT,
                                MESH_UI_SETTINGS_CANNED, MESH_UI_CANNED_SLOT_MAX - 1U, NULL,
                                NO_PRESETS, MESH_STR_NONE, NULL, 0U},
    [MESH_UI_FIELD_CANNED_5] = {MESH_STR_SETTINGS_FIELD_CANNED_5, MESH_UI_SETTING_TEXT,
                                MESH_UI_SETTINGS_CANNED, MESH_UI_CANNED_SLOT_MAX - 1U, NULL,
                                NO_PRESETS, MESH_STR_NONE, NULL, 0U},
};

/*
 * The canned list is one string with '|' between entries. An empty list is no entries rather
 * than one empty one, and a trailing separator does not invent a last entry: both matter
 * because the count is what the "also on radio" row reports and what decides how far the write
 * builder walks.
 */
char mesh_ui_settings_field_reserved_char(enum mesh_ui_setting_field field) {
    switch (field) {
    case MESH_UI_FIELD_CANNED_0:
    case MESH_UI_FIELD_CANNED_1:
    case MESH_UI_FIELD_CANNED_2:
    case MESH_UI_FIELD_CANNED_3:
    case MESH_UI_FIELD_CANNED_4:
    case MESH_UI_FIELD_CANNED_5:
        return '|';
    default:
        return '\0';
    }
}

uint32_t mesh_ui_settings_canned_count(const char *list) {
    if (list == NULL || list[0] == '\0') {
        return 0U;
    }
    uint32_t count = 1U;
    for (const char *p = list; *p != '\0'; ++p) {
        if (*p == '|') {
            count++;
        }
    }
    /* A list ending in '|' has no entry after it. */
    if (list[strlen(list) - 1U] == '|') {
        count--;
    }
    return count;
}

void mesh_ui_settings_canned_entry(const char *list, uint32_t index, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (list == NULL) {
        return;
    }
    const char *start = list;
    for (uint32_t i = 0; i < index; ++i) {
        const char *sep = strchr(start, '|');
        if (sep == NULL) {
            return;
        }
        start = sep + 1;
    }
    const char *end = strchr(start, '|');
    size_t len = (end != NULL) ? (size_t)(end - start) : strlen(start);
    if (len >= out_len) {
        len = out_len - 1U;
    }
    memcpy(out, start, len);
    out[len] = '\0';
}

const struct field_spec *field_spec(enum mesh_ui_setting_field field) {
    if ((unsigned)field >= MESH_UI_FIELD_COUNT) {
        return &k_fields[MESH_UI_FIELD_NONE];
    }
    return &k_fields[field];
}

const char *mesh_ui_settings_field_label(enum mesh_ui_setting_field field) {
    return mesh_str(field_spec(field)->label);
}

enum mesh_str_id mesh_ui_settings_field_label_id(enum mesh_ui_setting_field field) {
    return field_spec(field)->label;
}

enum mesh_ui_setting_kind mesh_ui_settings_field_kind(enum mesh_ui_setting_field field) {
    return field_spec(field)->kind;
}

enum mesh_ui_settings_section mesh_ui_settings_field_section(enum mesh_ui_setting_field field) {
    return field_spec(field)->section;
}

enum mesh_str_id mesh_ui_settings_field_note(enum mesh_ui_setting_field field) {
    return field_spec(field)->note;
}

uint32_t mesh_ui_settings_enum_count(enum mesh_ui_setting_field field) {
    const struct field_spec *spec = field_spec(field);
    return spec->kind == MESH_UI_SETTING_ENUM ? spec->limit : 0U;
}

const char *mesh_ui_settings_enum_name(enum mesh_ui_setting_field field, uint32_t value) {
    const struct field_spec *spec = field_spec(field);
    if (spec->kind != MESH_UI_SETTING_ENUM || spec->enum_name == NULL) {
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
    return spec->enum_name(value);
}

uint32_t mesh_ui_settings_number_step(enum mesh_ui_setting_field field, uint32_t value, int delta) {
    const struct field_spec *spec = field_spec(field);
    if (spec->kind != MESH_UI_SETTING_NUMBER || spec->presets == NULL || delta == 0) {
        return value;
    }
    if (delta > 0) {
        for (size_t i = 0; i < spec->preset_count; ++i) {
            if (spec->presets[i] > value) {
                return spec->presets[i];
            }
        }
        return value;
    }
    for (size_t i = spec->preset_count; i > 0U; --i) {
        if (spec->presets[i - 1U] < value) {
            return spec->presets[i - 1U];
        }
    }
    return value;
}

/*
 * Where `value` sits on a NUMBER field's own scale.
 *
 * In *stop* space rather than in value space, and that is the arithmetic worth explaining. The
 * presets a field offers climb geometrically - screen-on is 15s, 30s, a minute, two, five, ten,
 * fifteen, half an hour, an hour - so a handle placed at value/3600 would put eight of the ten
 * choices inside the first sixth of the track and leave the last two with the rest of it. What
 * the reader is choosing between is the *choices*, so the stops are evenly spaced and the
 * position is the index among them.
 *
 * A value the list does not contain still gets a position, interpolated between the two stops it
 * falls between, and that is one thing this can do that a segmented button cannot: a set of
 * alternatives has no room between its members, so an unknown value there had to fall back to
 * words (§10). An axis has room. A radio reporting 42 seconds - a firmware default, a value
 * written by a phone app with a different list - lands where 42 seconds actually is, which is a
 * true statement about a number this client would not itself have offered.
 *
 * What an axis still cannot place is a value below its own bottom stop, and there are two ways
 * to have one. Most of these lists open with a value that is not a quantity at all - a 0 the
 * field reads as "the firmware's own default", or as "as much as this radio has" - which is
 * `preset_zero_aside`, and the track spans what follows it. And two lists simply start above
 * zero, because the thing at the other end refuses anything below that, while a radio nobody has
 * configured still reports 0. Both come back `unplaced` rather than at the bottom, by the same
 * test: anything under the first stop is off the track.
 *
 * The unsigned comparisons are the ones mesh_ui_settings_number_step() walks with, and they are
 * correct over the two signed RSSI lists for the reason stated there - every entry is negative,
 * so two's complement keeps their order, and an unsigned difference of two of them is the true
 * distance between them.
 */
bool mesh_ui_settings_number_track(enum mesh_ui_setting_field field, uint32_t value,
                                   struct mesh_ui_settings_track *out) {
    const struct field_spec *spec = field_spec(field);
    if (spec->kind != MESH_UI_SETTING_NUMBER || !spec->preset_scale || spec->presets == NULL) {
        return false;
    }
    const size_t aside = spec->preset_zero_aside ? 1U : 0U;
    if (spec->preset_count < aside + 2U) {
        return false;
    }
    const uint32_t *stops = spec->presets + aside;
    const size_t last = spec->preset_count - aside - 1U;

    struct mesh_ui_settings_track track = {.stops = (uint32_t)(last + 1U)};
    if (value < stops[0]) {
        /*
         * Below the bottom stop is off the track, on every scale rather than only on the ones
         * that stand a zero aside.
         *
         * Two lists here start above zero because the thing on the other end refuses anything
         * below it - the public map drops a report under an hour, the firmware floors neighbour
         * info at four - and a radio that has never been configured reports 0 for both. Placing
         * that at the first stop would draw "off" exactly as "every hour", which is the same
         * false claim `max` was making at the other end of transmit power's list.
         */
        track.unplaced = true;
    } else if (value >= stops[last]) {
        track.position = MESH_UI_ANIM_ONE;
    } else if (value > stops[0]) {
        size_t i = 0U;
        while (i < last && stops[i + 1U] <= value) {
            ++i;
        }
        /* The stop itself, plus however far past it the value has got towards the next one. The
           span cannot be zero - a preset list is strictly increasing - but a list edited into
           holding a repeat would divide by it, and the stop is the honest answer for that. */
        const uint32_t span = stops[i + 1U] - stops[i];
        const uint64_t within =
            span > 0U ? ((uint64_t)(value - stops[i]) * MESH_UI_ANIM_ONE) / span : 0U;
        track.position = (int32_t)(((uint64_t)i * MESH_UI_ANIM_ONE + within) / last);
    }
    if (out != NULL) {
        *out = track;
    }
    return true;
}

uint32_t mesh_ui_settings_text_max(enum mesh_ui_setting_field field) {
    const struct field_spec *spec = field_spec(field);
    if (spec->kind != MESH_UI_SETTING_TEXT && spec->kind != MESH_UI_SETTING_KEY) {
        return 0U;
    }
    return spec->limit < MESH_UI_SETTING_TEXT_MAX ? spec->limit : MESH_UI_SETTING_TEXT_MAX - 1U;
}

uint32_t mesh_ui_settings_key_choices(enum mesh_ui_setting_field field) {
    const struct field_spec *spec = field_spec(field);
    return spec->kind == MESH_UI_SETTING_KEY ? spec->choices : 0U;
}

bool mesh_ui_settings_key_len_ok(enum mesh_ui_setting_field field, size_t len) {
    switch (field) {
    case MESH_UI_FIELD_CHANNEL_KEY:
        return len == 0U || len == 1U || len == 16U || len == 32U;
    case MESH_UI_FIELD_SECURITY_PRIVATE_KEY:
        return len == 32U;
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_0:
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_1:
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_2:
        return len == 0U || len == 32U;
    default:
        return false;
    }
}

bool mesh_ui_settings_section_needs_confirm(enum mesh_ui_settings_section section) {
    return section == MESH_UI_SETTINGS_BLUETOOTH || section == MESH_UI_SETTINGS_CHANNELS ||
           section == MESH_UI_SETTINGS_LORA || section == MESH_UI_SETTINGS_SECURITY ||
           section == MESH_UI_SETTINGS_POWER;
}

/* The fixed-position pair are the exception: setting a location is undone by setting another
   one and clearing it by setting it again, so a question in front of either would be a press
   the user has to make twice for nothing. */
enum mesh_ui_setting_consumer mesh_ui_settings_field_consumer(enum mesh_ui_setting_field field) {
    switch (field) {
    case MESH_UI_FIELD_POSITION_LATITUDE:
    case MESH_UI_FIELD_POSITION_LONGITUDE:
    case MESH_UI_FIELD_POSITION_ALTITUDE:
        return MESH_UI_SETTING_CONSUMER_FIXED_POSITION;
    default:
        return MESH_UI_SETTING_CONSUMER_SECTION;
    }
}

bool mesh_ui_settings_action_needs_confirm(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_REBOOT || action == MESH_UI_SETTINGS_ACTION_SHUTDOWN ||
           action == MESH_UI_SETTINGS_ACTION_RESET_NODEDB ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE ||
           action == MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP ||
           mesh_ui_settings_action_is_forget(action);
}

/* Spelled out rather than "everything that needs confirming, plus the position pair": the
   forget rows need the sheet too and are the one thing in that section the radio never hears
   about, so the two questions stopped having the same answer. */
bool mesh_ui_settings_action_is_radio(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_REBOOT || action == MESH_UI_SETTINGS_ACTION_SHUTDOWN ||
           action == MESH_UI_SETTINGS_ACTION_RESET_NODEDB ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE ||
           action == MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION ||
           action == MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION ||
           action == MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP ||
           action == MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY;
}

bool mesh_ui_settings_action_is_forget(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES ||
           action == MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES;
}

void mesh_ui_settings_confirm_title(enum mesh_ui_settings_section section, uint8_t channel,
                                    enum mesh_ui_settings_action action, char *out,
                                    size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_REBOOT));
        return;
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_SHUTDOWN));
        return;
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_RESET_DB));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_FORGET_OFF));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_FORGET_ALL));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_FACTORY_CFG));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_FACTORY_DEV));
        return;
    case MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_BACKUP));
        return;
    case MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_RESTORE));
        return;
    case MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TITLE_RM_BACKUP));
        return;
    default:
        break;
    }
    if (section == MESH_UI_SETTINGS_CHANNELS && channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        mesh_str_format(out, out_len, MESH_STR_CONFIRM_TITLE_SAVE_CHANNEL, (unsigned)channel);
        return;
    }
    mesh_str_format(out, out_len, MESH_STR_CONFIRM_TITLE_SAVE,
                    mesh_ui_settings_section_name(section));
}

const char *mesh_ui_settings_confirm_accept(enum mesh_ui_settings_action action) {
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_REBOOT);
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_SHUTDOWN);
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_RESET_DB);
    case MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_FORGET_OFF);
    case MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_FORGET_ALL);
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_FACTORY_CFG);
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_FACTORY_DEV);
    case MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_BACKUP);
    case MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_RESTORE);
    case MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_RM_BACKUP);
    default:
        return mesh_str(MESH_STR_CONFIRM_ACCEPT_SAVE);
    }
}

void mesh_ui_settings_confirm_text(enum mesh_ui_settings_section section,
                                   enum mesh_ui_settings_action action, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    /*
     * The actions first: they are the only rows here that cannot be undone by pressing the
     * opposite one, so each says what is lost rather than only what happens. A reset that
     * spares something says so too - "favorites excepted" is the difference between a press
     * the user regrets and one they do not.
     */
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_REBOOT));
        return;
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_SHUTDOWN));
        return;
    /* Four wrapped lines is what the sheet draws, so each of these stops inside it: a warning
       whose last clause is cut off is worse than a shorter one. */
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_RESET_DB));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_FORGET_OFF));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_FORGET_ALL));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_FACTORY_CFG));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_FACTORY_DEV));
        return;
    case MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_BACKUP));
        return;
    case MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_RESTORE));
        return;
    case MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_RM_BACKUP));
        return;
    default:
        break;
    }
    switch (section) {
    case MESH_UI_SETTINGS_BLUETOOTH:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_BLUETOOTH));
        break;
    case MESH_UI_SETTINGS_CHANNELS:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_CHANNELS));
        break;
    case MESH_UI_SETTINGS_LORA:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_LORA));
        break;
    case MESH_UI_SETTINGS_SECURITY:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_SECURITY));
        break;
    case MESH_UI_SETTINGS_POWER:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_POWER));
        break;
    default:
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CONFIRM_TEXT_DEFAULT));
        break;
    }
}

const struct mesh_ui_setting_edit *
mesh_ui_settings_find_edit(const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_setting_field field) {
    if (edits == NULL || field == MESH_UI_FIELD_NONE) {
        return NULL;
    }
    for (size_t i = 0; i < edit_count && i < MESH_UI_SETTINGS_EDITS_MAX; ++i) {
        if (edits[i].field == (uint8_t)field) {
            return &edits[i];
        }
    }
    return NULL;
}
