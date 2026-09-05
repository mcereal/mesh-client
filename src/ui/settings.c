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
        return "About MeshClient";
    case MESH_UI_SETTINGS_RADIO:
        return "Radio";
    case MESH_UI_SETTINGS_USER:
        return "User";
    case MESH_UI_SETTINGS_DEVICE:
        return "Device";
    case MESH_UI_SETTINGS_DISPLAY:
        return "Display";
    case MESH_UI_SETTINGS_LORA:
        return "LoRa";
    case MESH_UI_SETTINGS_BLUETOOTH:
        return "Bluetooth";
    case MESH_UI_SETTINGS_CHANNELS:
        return "Channels";
    case MESH_UI_SETTINGS_SECURITY:
        return "Security";
    case MESH_UI_SETTINGS_POSITION:
        return "Position";
    case MESH_UI_SETTINGS_POWER:
        return "Power";
    case MESH_UI_SETTINGS_MQTT:
        return "MQTT";
    case MESH_UI_SETTINGS_STORE_FORWARD:
        return "Store & Forward";
    case MESH_UI_SETTINGS_TELEMETRY:
        return "Telemetry";
    case MESH_UI_SETTINGS_ACTIONS:
        return "Radio actions";
    case MESH_UI_SETTINGS_MODULES:
        return "Modules";
    case MESH_UI_SETTINGS_NEIGHBOR_INFO:
        return "Neighbor info";
    case MESH_UI_SETTINGS_RANGE_TEST:
        return "Range test";
    case MESH_UI_SETTINGS_PAXCOUNTER:
        return "Paxcounter";
    case MESH_UI_SETTINGS_TAK:
        return "TAK";
    case MESH_UI_SETTINGS_AMBIENT:
        return "Ambient lighting";
    case MESH_UI_SETTINGS_STATUS_MESSAGE:
        return "Status message";
    case MESH_UI_SETTINGS_DETECTION:
        return "Detection sensor";
    case MESH_UI_SETTINGS_EXT_NOTIFICATION:
        return "External notify";
    case MESH_UI_SETTINGS_TRAFFIC:
        return "Traffic management";
    default:
        return "?";
    }
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
    MESH_UI_SETTINGS_ABOUT,    MESH_UI_SETTINGS_RADIO,    MESH_UI_SETTINGS_USER,
    MESH_UI_SETTINGS_DEVICE,   MESH_UI_SETTINGS_DISPLAY,  MESH_UI_SETTINGS_POSITION,
    MESH_UI_SETTINGS_POWER,    MESH_UI_SETTINGS_LORA,     MESH_UI_SETTINGS_BLUETOOTH,
    MESH_UI_SETTINGS_CHANNELS, MESH_UI_SETTINGS_SECURITY, MESH_UI_SETTINGS_MODULES,
    MESH_UI_SETTINGS_ACTIONS,
};

/* Every ModuleConfig variant this client keeps. Grows by one row per module as the phases
   land; the order is the protobuf's field order, which is as good as any and is stable. */
static const enum mesh_ui_settings_section k_modules[] = {
    MESH_UI_SETTINGS_MQTT,       MESH_UI_SETTINGS_STORE_FORWARD,    MESH_UI_SETTINGS_TELEMETRY,
    MESH_UI_SETTINGS_RANGE_TEST, MESH_UI_SETTINGS_NEIGHBOR_INFO,    MESH_UI_SETTINGS_AMBIENT,
    MESH_UI_SETTINGS_PAXCOUNTER, MESH_UI_SETTINGS_STATUS_MESSAGE,   MESH_UI_SETTINGS_TAK,
    MESH_UI_SETTINGS_DETECTION,  MESH_UI_SETTINGS_EXT_NOTIFICATION, MESH_UI_SETTINGS_TRAFFIC,
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
           the one thing an AdminMessage cannot be addressed without: our own node number. */
        return handshake != NULL && handshake->has_my_info;
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
    default:
        return false;
    }
}

/* ---- editable fields ---------------------------------------------------------------------- */

static const char *compass_name(uint32_t orientation) {
    static const char *const k_names[] = {
        "0 deg", "90 deg", "180 deg", "270 deg", "0 flip", "90 flip", "180 flip", "270 flip",
    };
    return orientation < 8U ? k_names[orientation] : "?";
}

static const char *units_name(uint32_t units) { return units == 1U ? "Imperial" : "Metric"; }

/* DetectionSensorConfig.TriggerType, 0..5 and contiguous. Named for what the pin does rather
   than for the constant: "Low" says more than "LOGIC_LOW" next to the word Trigger. */
static const char *trigger_name(uint32_t trigger) {
    static const char *const k_names[] = {
        "Low", "High", "Falling edge", "Rising edge", "Any edge, low", "Any edge, high",
    };
    return trigger < MESH_ARRAY_LEN(k_names) ? k_names[trigger] : "?";
}

/* meshtastic_Team and meshtastic_MemberRole, from atak.proto. Both are contiguous from 0, which
   is what the nav's (value + 1) % count stepping needs; value 0 is "Unspecifed" upstream (their
   spelling), shown here as what the firmware actually does with it.

   Named as the phone apps name them - "RTO", not its expansion - for the reason keys are shown
   as base64: a setting read off the Brick should be recognisable in the app and back. */
static const char *tak_team_name(uint32_t team) {
    static const char *const k_names[] = {
        "Default (cyan)", "White", "Yellow", "Orange", "Magenta", "Red",        "Maroon", "Purple",
        "Dark blue",      "Blue",  "Cyan",   "Teal",   "Green",   "Dark green", "Brown",
    };
    return team < MESH_ARRAY_LEN(k_names) ? k_names[team] : "?";
}

static const char *tak_role_name(uint32_t role) {
    static const char *const k_names[] = {
        "Default (member)", "Team member", "Team lead", "HQ", "Sniper", "Medic",
        "Forward observer", "RTO",         "K9",
    };
    return role < MESH_ARRAY_LEN(k_names) ? k_names[role] : "?";
}

static const char *rebroadcast_name(uint32_t mode) {
    static const char *const k_names[] = {
        "All", "All skip decoding", "Local only", "Known only", "None", "Core portnums only",
    };
    return mode < 6U ? k_names[mode] : "?";
}

static const char *gps_mode_name(uint32_t mode) {
    switch (mode) {
    case 0U:
        return "Disabled";
    case 1U:
        return "Enabled";
    case 2U:
        return "Not present";
    default:
        return "?";
    }
}

static const char *channel_role_name(uint32_t value) {
    return value == 1U ? "Secondary" : "Disabled";
}

static const char *pairing_enum_name(uint32_t mode) {
    switch (mode) {
    case 0U:
        return "Random PIN";
    case 1U:
        return "Fixed PIN";
    case 2U:
        return "No PIN";
    default:
        return "?";
    }
}

/* Position precision is a bit count; the phone apps label the useful ones by distance. */
static void format_precision(uint32_t bits, char *out, size_t out_len) {
    static const char *const k_distance[] = {
        "~23 km", "~12 km", "~6 km",  "~3 km", "~1.5 km",
        "~730 m", "~360 m", "~180 m", "~90 m", "~45 m",
    };
    if (bits == 0U) {
        snprintf(out, out_len, "%s", "off");
    } else if (bits >= 32U) {
        snprintf(out, out_len, "%s", "precise");
    } else if (bits >= 10U && bits <= 19U) {
        snprintf(out, out_len, "%s", k_distance[bits - 10U]);
    } else {
        snprintf(out, out_len, "%u bits", (unsigned)bits);
    }
}

/* Metres, which is what PositionConfig's smart-broadcast threshold is in. */
static void format_metres(uint32_t metres, char *out, size_t out_len) {
    if (metres == 0U) {
        snprintf(out, out_len, "%s", "default");
    } else if (metres >= 1000U && metres % 1000U == 0U) {
        snprintf(out, out_len, "%u km", (unsigned)(metres / 1000U));
    } else {
        snprintf(out, out_len, "%u m", (unsigned)metres);
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
        return "Router Client (retired)";
    case 4U:
        return "Repeater (retired)";
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
        return "Compatible";
    case 1U:
        return "Balanced";
    case 2U:
        return "Strict";
    default:
        return "?";
    }
}

static void format_bandwidth(uint32_t khz, char *out, size_t out_len) {
    if (khz == 31U) {
        snprintf(out, out_len, "%s", "31.25 kHz");
    } else if (khz == 62U) {
        snprintf(out, out_len, "%s", "62.5 kHz");
    } else {
        snprintf(out, out_len, "%u kHz", (unsigned)khz);
    }
}
static void format_plain(uint32_t value, char *out, size_t out_len) {
    snprintf(out, out_len, "%u", (unsigned)value);
}
static void format_coding_rate(uint32_t value, char *out, size_t out_len) {
    snprintf(out, out_len, "4/%u", (unsigned)value);
}
static void format_tx_power(uint32_t value, char *out, size_t out_len) {
    if (value == 0U) {
        snprintf(out, out_len, "%s", "max");
    } else {
        snprintf(out, out_len, "%d dBm", (int)(int8_t)value);
    }
}

static const uint32_t k_bandwidth_presets[] = {31U, 62U, 125U, 250U, 500U};
static const uint32_t k_spread_presets[] = {7U, 8U, 9U, 10U, 11U, 12U};
static const uint32_t k_coding_presets[] = {5U, 6U, 7U, 8U};
static const uint32_t k_hop_presets[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
static const uint32_t k_tx_power_presets[] = {0U, 2U, 5U, 8U, 10U, 14U, 17U, 20U, 22U, 27U, 30U};

static const uint32_t k_precision_presets[] = {0U,  10U, 11U, 12U, 13U, 14U,
                                               15U, 16U, 17U, 18U, 19U, 32U};

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

/* GPIO pins. Which pins a board actually exposes is its own business and nothing on the wire
   says, so the row offers the range an ESP32 or nRF52 could plausibly expose: a pin outside it
   is certainly wrong, one inside it merely might be. 0 is what the firmware reads as unset. */
static const uint32_t k_gpio_presets[] = {0U,  1U,  2U,  3U,  4U,  5U,  6U,  12U, 13U,
                                          14U, 15U, 16U, 17U, 18U, 19U, 21U, 22U, 23U,
                                          25U, 26U, 27U, 32U, 33U, 34U, 35U, 36U, 39U};

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
        snprintf(out, out_len, "%us", (unsigned)(value / 1000U));
    } else {
        snprintf(out, out_len, "%ums", (unsigned)value);
    }
}

/* A GPIO pin, or nothing at all. */
static void format_pin(uint32_t value, char *out, size_t out_len) {
    if (value == 0U) {
        snprintf(out, out_len, "%s", "unset");
    } else {
        snprintf(out, out_len, "GPIO %u", (unsigned)value);
    }
}

/* Signed dBm, read back out of the uint32_t the preset table stores it in. */
static void format_rssi(uint32_t value, char *out, size_t out_len) {
    snprintf(out, out_len, "%d dBm", (int)(int32_t)value);
}

/* A plain 0-255 level, so an LED channel does not read as a duration. */
static void format_level(uint32_t value, char *out, size_t out_len) {
    snprintf(out, out_len, "%u", (unsigned)value);
}

/* Milliamps, for the LED current row. */
static void format_milliamps(uint32_t value, char *out, size_t out_len) {
    snprintf(out, out_len, "%u mA", (unsigned)value);
}

/* NUMBER fields whose value is a count rather than a duration; without this the seconds
   formatter would render 100 records as "1m40s". */
static void format_count(uint32_t value, char *out, size_t out_len) {
    if (value == 0U) {
        snprintf(out, out_len, "%s", "default");
    } else {
        snprintf(out, out_len, "%u", (unsigned)value);
    }
}

#define PRESETS(array) (array), MESH_ARRAY_LEN(array)
#define NO_PRESETS NULL, 0U

/* User.long_name is 39 bytes on the wire but the firmware truncates to 24 (mesh.proto). */
static const struct field_spec k_fields[MESH_UI_FIELD_COUNT] = {
    [MESH_UI_FIELD_NONE] = {"?", MESH_UI_SETTING_INFO, MESH_UI_SETTINGS_SECTION_COUNT, 0U, NULL,
                            NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_USER_LONG_NAME] = {"Long name", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_USER, 24U,
                                      NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_USER_SHORT_NAME] = {"Short name", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_USER,
                                       4U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_USER_LICENSED] = {"Licensed operator", MESH_UI_SETTING_TOGGLE,
                                     MESH_UI_SETTINGS_USER, 0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_USER_UNMESSAGEABLE] = {"Unmessageable", MESH_UI_SETTING_TOGGLE,
                                          MESH_UI_SETTINGS_USER, 0U, NULL, NO_PRESETS, NULL, NULL,
                                          0U},
    /* Thirteen values, two of them retired but still steppable: see role_enum_name(). */
    [MESH_UI_FIELD_DEVICE_ROLE] = {"Role", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DEVICE, 13U,
                                   role_enum_name, NO_PRESETS, NULL, NULL, 0U},
    /* tzdef is 64 bytes on the wire. The radio applies it to its own clock only; it has no
       bearing on what this client shows, which follows the Brick's own TZ. */
    [MESH_UI_FIELD_DEVICE_TZDEF] = {"Time zone", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_DEVICE, 64U,
                                    NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_DEVICE_REBROADCAST] = {"Rebroadcast", MESH_UI_SETTING_ENUM,
                                          MESH_UI_SETTINGS_DEVICE, 6U, rebroadcast_name, NO_PRESETS,
                                          NULL, NULL, 0U},
    [MESH_UI_FIELD_DEVICE_NODEINFO_SECS] = {"NodeInfo every", MESH_UI_SETTING_NUMBER,
                                            MESH_UI_SETTINGS_DEVICE, 0U, NULL,
                                            PRESETS(k_nodeinfo_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_DEVICE_LED_HEARTBEAT] = {"LED heartbeat", MESH_UI_SETTING_TOGGLE,
                                            MESH_UI_SETTINGS_DEVICE, 0U, NULL, NO_PRESETS, NULL,
                                            NULL, 0U},
    [MESH_UI_FIELD_DEVICE_DOUBLE_TAP] = {"Double tap = button", MESH_UI_SETTING_TOGGLE,
                                         MESH_UI_SETTINGS_DEVICE, 0U, NULL, NO_PRESETS, NULL, NULL,
                                         0U},
    [MESH_UI_FIELD_POSITION_GPS_MODE] = {"GPS", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_POSITION, 3U,
                                         gps_mode_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_POSITION_BROADCAST_SECS] = {"Broadcast every", MESH_UI_SETTING_NUMBER,
                                               MESH_UI_SETTINGS_POSITION, 0U, NULL,
                                               PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_POSITION_SMART] = {"Smart broadcast", MESH_UI_SETTING_TOGGLE,
                                      MESH_UI_SETTINGS_POSITION, 0U, NULL, NO_PRESETS, NULL, NULL,
                                      0U},
    [MESH_UI_FIELD_POSITION_SMART_DISTANCE] = {"Smart distance", MESH_UI_SETTING_NUMBER,
                                               MESH_UI_SETTINGS_POSITION, 0U, NULL,
                                               PRESETS(k_smart_distance_presets), NULL,
                                               format_metres, 0U},
    [MESH_UI_FIELD_POSITION_SMART_INTERVAL] = {"Smart interval", MESH_UI_SETTING_NUMBER,
                                               MESH_UI_SETTINGS_POSITION, 0U, NULL,
                                               PRESETS(k_smart_interval_presets), "default", NULL,
                                               0U},
    [MESH_UI_FIELD_POSITION_GPS_INTERVAL] = {"GPS interval", MESH_UI_SETTING_NUMBER,
                                             MESH_UI_SETTINGS_POSITION, 0U, NULL,
                                             PRESETS(k_gps_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_POSITION_LATITUDE] = {"Latitude", MESH_UI_SETTING_TEXT,
                                         MESH_UI_SETTINGS_POSITION, 15U, NULL, NULL, 0U, NULL, NULL,
                                         0U},
    [MESH_UI_FIELD_POSITION_LONGITUDE] = {"Longitude", MESH_UI_SETTING_TEXT,
                                          MESH_UI_SETTINGS_POSITION, 15U, NULL, NULL, 0U, NULL,
                                          NULL, 0U},
    [MESH_UI_FIELD_POSITION_ALTITUDE] = {"Altitude (m)", MESH_UI_SETTING_TEXT,
                                         MESH_UI_SETTINGS_POSITION, 7U, NULL, NULL, 0U, NULL, NULL,
                                         0U},
    [MESH_UI_FIELD_POWER_SAVING] = {"Power saving", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_POWER,
                                    0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_POWER_LS_SECS] = {"Light sleep", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POWER,
                                     0U, NULL, PRESETS(k_sleep_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_POWER_MIN_WAKE] = {"Min wake", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_POWER,
                                      0U, NULL, PRESETS(k_wake_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_POWER_WAIT_BT] = {"Wait for Bluetooth", MESH_UI_SETTING_NUMBER,
                                     MESH_UI_SETTINGS_POWER, 0U, NULL, PRESETS(k_wait_bt_presets),
                                     "default", NULL, 0U},
    [MESH_UI_FIELD_POWER_SHUTDOWN] = {"Shutdown on battery", MESH_UI_SETTING_NUMBER,
                                      MESH_UI_SETTINGS_POWER, 0U, NULL, PRESETS(k_shutdown_presets),
                                      "off", NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_SCREEN_ON] = {"Screen on", MESH_UI_SETTING_NUMBER,
                                         MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                         PRESETS(k_screen_on_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_CAROUSEL] = {"Carousel", MESH_UI_SETTING_NUMBER,
                                        MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                        PRESETS(k_carousel_presets), "off", NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_COMPASS] = {"Compass", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DISPLAY,
                                       8U, compass_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_12H] = {"12-hour clock", MESH_UI_SETTING_TOGGLE,
                                   MESH_UI_SETTINGS_DISPLAY, 0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_UNITS] = {"Units", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DISPLAY, 2U,
                                     units_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_DISPLAY_FLIP] = {"Flip screen", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_DISPLAY,
                                    0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_ENABLED] = {"MQTT", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                    NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_ADDRESS] = {"Server", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_MQTT, 63U,
                                    NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_USERNAME] = {"Username", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_MQTT, 63U,
                                     NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_PASSWORD] = {"Password", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_MQTT, 31U,
                                     NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_ROOT] = {"Root topic", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_MQTT, 31U,
                                 NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_ENCRYPTION] = {"Encryption", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_MQTT,
                                       0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_TLS] = {"TLS", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                NO_PRESETS, NULL, NULL, 0U},
    /* Spelled out: this one publishes the node's position to a public map, which is not what
       "map reporting" reads as to somebody stepping through toggles. */
    [MESH_UI_FIELD_MQTT_MAP_REPORTING] = {"Report to public map", MESH_UI_SETTING_TOGGLE,
                                          MESH_UI_SETTINGS_MQTT, 0U, NULL, NO_PRESETS, NULL, NULL,
                                          0U},
    [MESH_UI_FIELD_MQTT_MAP_INTERVAL] = {"Map interval", MESH_UI_SETTING_NUMBER,
                                         MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                         PRESETS(k_map_interval_presets), NULL, NULL, 0U},
    [MESH_UI_FIELD_MQTT_MAP_PRECISION] = {"Map precision", MESH_UI_SETTING_NUMBER,
                                          MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                          PRESETS(k_precision_presets), "off", format_precision,
                                          0U},
    [MESH_UI_FIELD_MQTT_MAP_LOCATION] = {"Map my location", MESH_UI_SETTING_TOGGLE,
                                         MESH_UI_SETTINGS_MQTT, 0U, NULL, NO_PRESETS, NULL, NULL,
                                         0U},
    [MESH_UI_FIELD_SF_ENABLED] = {"Store & Forward", MESH_UI_SETTING_TOGGLE,
                                  MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, NULL, NULL,
                                  0U},
    [MESH_UI_FIELD_SF_HEARTBEAT] = {"Heartbeat", MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, NULL,
                                    NULL, 0U},
    [MESH_UI_FIELD_SF_SERVER] = {"Act as server", MESH_UI_SETTING_TOGGLE,
                                 MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, NULL, NULL,
                                 0U},
    [MESH_UI_FIELD_SF_RECORDS] = {"Records kept", MESH_UI_SETTING_NUMBER,
                                  MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL,
                                  PRESETS(k_sf_records_presets), NULL, format_count, 0U},
    [MESH_UI_FIELD_SF_HISTORY_MAX] = {"History max", MESH_UI_SETTING_NUMBER,
                                      MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL,
                                      PRESETS(k_sf_history_presets), NULL, format_count, 0U},
    [MESH_UI_FIELD_SF_HISTORY_WINDOW] = {"History window", MESH_UI_SETTING_NUMBER,
                                         MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL,
                                         PRESETS(k_sf_window_presets), "default", NULL, 0U},
    /* The five telemetry groups each sit under a heading, so their rows are named for what
       they are inside the group rather than repeating it ("Enabled", not "Env enabled"). The
       only consumer of a field label outside the row list is the keyboard title, and none of
       these is a TEXT field. */
    [MESH_UI_FIELD_TELEMETRY_DEVICE] = {"Enabled", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                        NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_INTERVAL] = {"Interval", MESH_UI_SETTING_NUMBER,
                                          MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                          PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENVIRONMENT] = {"Enabled", MESH_UI_SETTING_TOGGLE,
                                             MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                             NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_INTERVAL] = {"Interval", MESH_UI_SETTING_NUMBER,
                                              MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                              PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_SCREEN] = {"Show on screen", MESH_UI_SETTING_TOGGLE,
                                            MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                            NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT] = {"Fahrenheit", MESH_UI_SETTING_TOGGLE,
                                                MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS,
                                                NULL, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_AIR_QUALITY] = {"Enabled", MESH_UI_SETTING_TOGGLE,
                                             MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                             NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL] = {"Interval", MESH_UI_SETTING_NUMBER,
                                              MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                              PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_AIR_SCREEN] = {"Show on screen", MESH_UI_SETTING_TOGGLE,
                                            MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                            NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_POWER] = {"Enabled", MESH_UI_SETTING_TOGGLE,
                                       MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL, NULL,
                                       0U},
    [MESH_UI_FIELD_TELEMETRY_POWER_INTERVAL] = {"Interval", MESH_UI_SETTING_NUMBER,
                                                MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                                PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_POWER_SCREEN] = {"Show on screen", MESH_UI_SETTING_TOGGLE,
                                              MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS,
                                              NULL, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_HEALTH] = {"Enabled", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                        NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL] = {"Interval", MESH_UI_SETTING_NUMBER,
                                                 MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                                 PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_HEALTH_SCREEN] = {"Show on screen", MESH_UI_SETTING_TOGGLE,
                                               MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS,
                                               NULL, NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_NAME] = {"Name", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_CHANNELS, 11U,
                                    NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_ROLE] = {"Role", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_CHANNELS, 2U,
                                    channel_role_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_CHANNEL_KEY] = {"Key", MESH_UI_SETTING_KEY, MESH_UI_SETTINGS_CHANNELS, 64U, NULL,
                                   NO_PRESETS, NULL, NULL, CHANNEL_KEY_CHOICES},
    [MESH_UI_FIELD_CHANNEL_UPLINK] = {"MQTT uplink", MESH_UI_SETTING_TOGGLE,
                                      MESH_UI_SETTINGS_CHANNELS, 0U, NULL, NO_PRESETS, NULL, NULL,
                                      0U},
    [MESH_UI_FIELD_CHANNEL_DOWNLINK] = {"MQTT downlink", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_CHANNELS, 0U, NULL, NO_PRESETS, NULL, NULL,
                                        0U},
    [MESH_UI_FIELD_CHANNEL_POSITION] = {"Position precision", MESH_UI_SETTING_NUMBER,
                                        MESH_UI_SETTINGS_CHANNELS, 0U, NULL,
                                        PRESETS(k_precision_presets), "off", format_precision, 0U},
    [MESH_UI_FIELD_BT_ENABLED] = {"Bluetooth", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_BLUETOOTH,
                                  0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_BT_MODE] = {"Pairing", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_BLUETOOTH, 3U,
                               pairing_enum_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_BT_PIN] = {"Fixed PIN", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_BLUETOOTH, 6U,
                              NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_LORA_REGION] = {"Region", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_LORA, 38U,
                                   region_enum_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_LORA_USE_PRESET] = {"Use preset", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_LORA,
                                       0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_LORA_PRESET] = {"Preset", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_LORA, 17U,
                                   preset_enum_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_LORA_BANDWIDTH] = {"Bandwidth", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_LORA,
                                      0U, NULL, PRESETS(k_bandwidth_presets), NULL,
                                      format_bandwidth, 0U},
    [MESH_UI_FIELD_LORA_SPREAD] = {"Spread factor", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_LORA,
                                   0U, NULL, PRESETS(k_spread_presets), NULL, format_plain, 0U},
    [MESH_UI_FIELD_LORA_CODING] = {"Coding rate", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_LORA, 0U,
                                   NULL, PRESETS(k_coding_presets), NULL, format_coding_rate, 0U},
    [MESH_UI_FIELD_LORA_HOPS] = {"Hop limit", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_LORA, 0U,
                                 NULL, PRESETS(k_hop_presets), NULL, format_plain, 0U},
    [MESH_UI_FIELD_LORA_TX_ENABLED] = {"Transmit", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_LORA,
                                       0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_LORA_TX_POWER] = {"TX power", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_LORA, 0U,
                                     NULL, PRESETS(k_tx_power_presets), NULL, format_tx_power, 0U},
    [MESH_UI_FIELD_LORA_IGNORE_MQTT] = {"Ignore MQTT", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_LORA, 0U, NULL, NO_PRESETS, NULL, NULL,
                                        0U},
    [MESH_UI_FIELD_LORA_OK_TO_MQTT] = {"OK to MQTT", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_LORA,
                                       0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_SECURITY_PRIVATE_KEY] = {"Private key", MESH_UI_SETTING_KEY,
                                            MESH_UI_SETTINGS_SECURITY, 64U, NULL, NO_PRESETS, NULL,
                                            NULL, PRIVATE_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_0] = {"Admin key 1", MESH_UI_SETTING_KEY,
                                            MESH_UI_SETTINGS_SECURITY, 64U, NULL, NO_PRESETS, NULL,
                                            NULL, ADMIN_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_1] = {"Admin key 2", MESH_UI_SETTING_KEY,
                                            MESH_UI_SETTINGS_SECURITY, 64U, NULL, NO_PRESETS, NULL,
                                            NULL, ADMIN_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_2] = {"Admin key 3", MESH_UI_SETTING_KEY,
                                            MESH_UI_SETTINGS_SECURITY, 64U, NULL, NO_PRESETS, NULL,
                                            NULL, ADMIN_KEY_CHOICES},
    [MESH_UI_FIELD_SECURITY_MANAGED] = {"Managed mode", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_SECURITY, 0U, NULL, NO_PRESETS, NULL, NULL,
                                        0U},
    [MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL] = {"Admin channel", MESH_UI_SETTING_TOGGLE,
                                              MESH_UI_SETTINGS_SECURITY, 0U, NULL, NO_PRESETS, NULL,
                                              NULL, 0U},
    [MESH_UI_FIELD_SECURITY_SERIAL] = {"Serial console", MESH_UI_SETTING_TOGGLE,
                                       MESH_UI_SETTINGS_SECURITY, 0U, NULL, NO_PRESETS, NULL, NULL,
                                       0U},
    [MESH_UI_FIELD_SECURITY_DEBUG_LOG] = {"Debug log API", MESH_UI_SETTING_TOGGLE,
                                          MESH_UI_SETTINGS_SECURITY, 0U, NULL, NO_PRESETS, NULL,
                                          NULL, 0U},
    [MESH_UI_FIELD_NEIGHBOR_ENABLED] = {"Neighbor info", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U, NULL, NO_PRESETS, NULL,
                                        NULL, 0U},
    [MESH_UI_FIELD_NEIGHBOR_INTERVAL] = {"Interval", MESH_UI_SETTING_NUMBER,
                                         MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U, NULL,
                                         PRESETS(k_neighbor_presets), NULL, NULL, 0U},
    [MESH_UI_FIELD_NEIGHBOR_OVER_LORA] = {"Send over LoRa", MESH_UI_SETTING_TOGGLE,
                                          MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U, NULL, NO_PRESETS,
                                          NULL, NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_ENABLED] = {"Range test", MESH_UI_SETTING_TOGGLE,
                                          MESH_UI_SETTINGS_RANGE_TEST, 0U, NULL, NO_PRESETS, NULL,
                                          NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_SENDER] = {"Send every", MESH_UI_SETTING_NUMBER,
                                         MESH_UI_SETTINGS_RANGE_TEST, 0U, NULL,
                                         PRESETS(k_range_test_presets), "never", NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_SAVE] = {"Save CSV", MESH_UI_SETTING_TOGGLE,
                                       MESH_UI_SETTINGS_RANGE_TEST, 0U, NULL, NO_PRESETS, NULL,
                                       NULL, 0U},
    [MESH_UI_FIELD_RANGE_TEST_CLEAR] = {"Clear CSV on boot", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_RANGE_TEST, 0U, NULL, NO_PRESETS, NULL,
                                        NULL, 0U},
    [MESH_UI_FIELD_PAX_ENABLED] = {"Paxcounter", MESH_UI_SETTING_TOGGLE,
                                   MESH_UI_SETTINGS_PAXCOUNTER, 0U, NULL, NO_PRESETS, NULL, NULL,
                                   0U},
    [MESH_UI_FIELD_PAX_INTERVAL] = {"Interval", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_PAXCOUNTER,
                                    0U, NULL, PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_PAX_WIFI_THRESHOLD] = {"WiFi floor", MESH_UI_SETTING_NUMBER,
                                          MESH_UI_SETTINGS_PAXCOUNTER, 0U, NULL,
                                          PRESETS(k_rssi_presets), NULL, format_rssi, 0U},
    [MESH_UI_FIELD_PAX_BLE_THRESHOLD] = {"BLE floor", MESH_UI_SETTING_NUMBER,
                                         MESH_UI_SETTINGS_PAXCOUNTER, 0U, NULL,
                                         PRESETS(k_rssi_presets), NULL, format_rssi, 0U},
    [MESH_UI_FIELD_TAK_TEAM] = {"Team", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_TAK, 15U,
                                tak_team_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_TAK_ROLE] = {"Role", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_TAK, 9U,
                                tak_role_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_AMBIENT_LED] = {"LED", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_AMBIENT, 0U,
                                   NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_AMBIENT_CURRENT] = {"Current", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_AMBIENT,
                                       0U, NULL, PRESETS(k_led_current_presets), NULL,
                                       format_milliamps, 0U},
    [MESH_UI_FIELD_AMBIENT_RED] = {"Red", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_AMBIENT, 0U,
                                   NULL, PRESETS(k_led_level_presets), NULL, format_level, 0U},
    [MESH_UI_FIELD_AMBIENT_GREEN] = {"Green", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_AMBIENT, 0U,
                                     NULL, PRESETS(k_led_level_presets), NULL, format_level, 0U},
    [MESH_UI_FIELD_AMBIENT_BLUE] = {"Blue", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_AMBIENT, 0U,
                                    NULL, PRESETS(k_led_level_presets), NULL, format_level, 0U},
    /* 79 bytes on the wire; MESH_UI_SETTING_TEXT_MAX was raised to 80 to hold it. */
    [MESH_UI_FIELD_STATUS_TEXT] = {"Status", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_STATUS_MESSAGE,
                                   79U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_DETECT_ENABLED] = {"Detection sensor", MESH_UI_SETTING_TOGGLE,
                                      MESH_UI_SETTINGS_DETECTION, 0U, NULL, NO_PRESETS, NULL, NULL,
                                      0U},
    /* 20 bytes on the wire including the NUL, so 19 typed. */
    [MESH_UI_FIELD_DETECT_NAME] = {"Name", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_DETECTION, 19U,
                                   NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_DETECT_MIN_BROADCAST] = {"Min interval", MESH_UI_SETTING_NUMBER,
                                            MESH_UI_SETTINGS_DETECTION, 0U, NULL,
                                            PRESETS(k_detect_min_presets), "none", NULL, 0U},
    [MESH_UI_FIELD_DETECT_STATE_BROADCAST] = {"Heartbeat", MESH_UI_SETTING_NUMBER,
                                              MESH_UI_SETTINGS_DETECTION, 0U, NULL,
                                              PRESETS(k_detect_state_presets), "off", NULL, 0U},
    [MESH_UI_FIELD_DETECT_SEND_BELL] = {"Send bell", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_DETECTION, 0U, NULL, NO_PRESETS, NULL,
                                        NULL, 0U},
    [MESH_UI_FIELD_DETECT_PIN] = {"Monitor pin", MESH_UI_SETTING_NUMBER, MESH_UI_SETTINGS_DETECTION,
                                  0U, NULL, PRESETS(k_gpio_presets), NULL, format_pin, 0U},
    [MESH_UI_FIELD_DETECT_TRIGGER] = {"Trigger", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DETECTION,
                                      6U, trigger_name, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_DETECT_PULLUP] = {"Pull-up", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_DETECTION,
                                     0U, NULL, NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ENABLED] = {"External notify", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS,
                                        NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ACTIVE] = {"Active high", MESH_UI_SETTING_TOGGLE,
                                       MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS,
                                       NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_OUTPUT_MS] = {"On for", MESH_UI_SETTING_NUMBER,
                                          MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                          PRESETS(k_output_ms_presets), NULL, format_millis, 0U},
    [MESH_UI_FIELD_EXTNOTIF_NAG] = {"Repeat for", MESH_UI_SETTING_NUMBER,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                    PRESETS(k_nag_presets), "once", NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_PWM] = {"Use PWM", MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS, NULL,
                                    NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_I2S] = {"I2S as buzzer", MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS, NULL,
                                    NULL, 0U},
    /* The three output groups. Each row is named for what it is inside its group, the way the
       telemetry groups are, because the heading above it says which output it belongs to. */
    [MESH_UI_FIELD_EXTNOTIF_PIN] = {"Pin", MESH_UI_SETTING_NUMBER,
                                    MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                    PRESETS(k_gpio_presets), NULL, format_pin, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG] = {"On message", MESH_UI_SETTING_TOGGLE,
                                          MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS,
                                          NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL] = {"On bell", MESH_UI_SETTING_TOGGLE,
                                           MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS,
                                           NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA] = {"Pin", MESH_UI_SETTING_NUMBER,
                                          MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                          PRESETS(k_gpio_presets), NULL, format_pin, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA] = {"On message", MESH_UI_SETTING_TOGGLE,
                                                MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA] = {"On bell", MESH_UI_SETTING_TOGGLE,
                                                 MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                 NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER] = {"Pin", MESH_UI_SETTING_NUMBER,
                                           MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                           PRESETS(k_gpio_presets), NULL, format_pin, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER] = {"On message", MESH_UI_SETTING_TOGGLE,
                                                 MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                 NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER] = {"On bell", MESH_UI_SETTING_TOGGLE,
                                                  MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                  NO_PRESETS, NULL, NULL, 0U},
    [MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL] = {"Position min gap", MESH_UI_SETTING_NUMBER,
                                                 MESH_UI_SETTINGS_TRAFFIC, 0U, NULL,
                                                 PRESETS(k_traffic_interval_presets), "off", NULL,
                                                 0U},
    [MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS] = {"NodeInfo max hops", MESH_UI_SETTING_NUMBER,
                                             MESH_UI_SETTINGS_TRAFFIC, 0U, NULL,
                                             PRESETS(k_traffic_hops_presets), "off", format_count,
                                             0U},
    [MESH_UI_FIELD_TRAFFIC_RATE_WINDOW] = {"Rate window", MESH_UI_SETTING_NUMBER,
                                           MESH_UI_SETTINGS_TRAFFIC, 0U, NULL,
                                           PRESETS(k_traffic_interval_presets), "off", NULL, 0U},
    [MESH_UI_FIELD_TRAFFIC_RATE_PACKETS] = {"Rate max packets", MESH_UI_SETTING_NUMBER,
                                            MESH_UI_SETTINGS_TRAFFIC, 0U, NULL,
                                            PRESETS(k_traffic_packets_presets), "off", format_count,
                                            0U},
    [MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD] = {"Unknown limit", MESH_UI_SETTING_NUMBER,
                                                 MESH_UI_SETTINGS_TRAFFIC, 0U, NULL,
                                                 PRESETS(k_traffic_packets_presets), "off",
                                                 format_count, 0U},
    [MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY] = {"Packet signing", MESH_UI_SETTING_ENUM,
                                                 MESH_UI_SETTINGS_SECURITY, 3U,
                                                 signature_policy_name, NO_PRESETS, NULL, NULL, 0U},
};

const struct field_spec *field_spec(enum mesh_ui_setting_field field) {
    if ((unsigned)field >= MESH_UI_FIELD_COUNT) {
        return &k_fields[MESH_UI_FIELD_NONE];
    }
    return &k_fields[field];
}

const char *mesh_ui_settings_field_label(enum mesh_ui_setting_field field) {
    return field_spec(field)->label;
}

enum mesh_ui_setting_kind mesh_ui_settings_field_kind(enum mesh_ui_setting_field field) {
    return field_spec(field)->kind;
}

enum mesh_ui_settings_section mesh_ui_settings_field_section(enum mesh_ui_setting_field field) {
    return field_spec(field)->section;
}

uint32_t mesh_ui_settings_enum_count(enum mesh_ui_setting_field field) {
    const struct field_spec *spec = field_spec(field);
    return spec->kind == MESH_UI_SETTING_ENUM ? spec->limit : 0U;
}

const char *mesh_ui_settings_enum_name(enum mesh_ui_setting_field field, uint32_t value) {
    const struct field_spec *spec = field_spec(field);
    if (spec->kind != MESH_UI_SETTING_ENUM || spec->enum_name == NULL) {
        return "?";
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
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE;
}

bool mesh_ui_settings_action_is_radio(enum mesh_ui_settings_action action) {
    return mesh_ui_settings_action_needs_confirm(action) ||
           action == MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION ||
           action == MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION;
}

void mesh_ui_settings_confirm_title(enum mesh_ui_settings_section section, uint8_t channel,
                                    enum mesh_ui_settings_action action, char *out,
                                    size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        snprintf(out, out_len, "%s", "Reboot the radio?");
        return;
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        snprintf(out, out_len, "%s", "Shut the radio down?");
        return;
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        snprintf(out, out_len, "%s", "Reset the node database?");
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        snprintf(out, out_len, "%s", "Factory reset the config?");
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        snprintf(out, out_len, "%s", "Factory reset everything?");
        return;
    default:
        break;
    }
    if (section == MESH_UI_SETTINGS_CHANNELS && channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        snprintf(out, out_len, "Save channel %u?", (unsigned)channel);
        return;
    }
    snprintf(out, out_len, "Save %s?", mesh_ui_settings_section_name(section));
}

const char *mesh_ui_settings_confirm_accept(enum mesh_ui_settings_action action) {
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        return "Reboot now";
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        return "Shut down now";
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        return "Reset the node database";
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        return "Factory reset config";
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        return "Factory reset device";
    default:
        return "Save to radio";
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
        snprintf(out, out_len, "%s",
                 "The radio restarts in a few seconds. The link drops with it and auto-connect "
                 "brings it back; anything sent to this node meanwhile is lost.");
        return;
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        snprintf(out, out_len, "%s",
                 "The radio powers off in a few seconds and nothing here can wake it again: "
                 "that takes its own button. Everything it has stored survives.");
        return;
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        snprintf(out, out_len, "%s",
                 "The radio forgets every node it has heard, favorites excepted. Names and "
                 "positions return only as each node speaks again, which on a quiet mesh is "
                 "hours. This client's own cached list is left alone.");
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        snprintf(out, out_len, "%s",
                 "Every setting on this radio returns to its factory default, the Bluetooth "
                 "bond excepted. Channels and their keys go with them: the node leaves your "
                 "mesh until it is set up again.");
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        snprintf(out, out_len, "%s",
                 "Every setting and the node database return to factory defaults and the "
                 "Bluetooth bond is cleared, so this node has to be forgotten in Devices (Y) "
                 "and paired again. Its identity key changes.");
        return;
    default:
        break;
    }
    switch (section) {
    case MESH_UI_SETTINGS_BLUETOOTH:
        snprintf(out, out_len, "%s",
                 "The radio will reboot. Changing the pairing mode or PIN invalidates the "
                 "Brick's bond: forget the node in Devices (Y) and connect again to pair "
                 "with the new PIN. Turning Bluetooth off cuts this client off entirely.");
        break;
    case MESH_UI_SETTINGS_CHANNELS:
        snprintf(out, out_len, "%s",
                 "The radio will reboot. A new key or name moves this radio to a different "
                 "channel: every other node needs the same settings to keep talking to it.");
        break;
    case MESH_UI_SETTINGS_LORA:
        snprintf(out, out_len, "%s",
                 "The radio will reboot. A region or preset the other nodes do not share takes "
                 "this radio off the mesh, and a wrong region may be illegal to transmit on. "
                 "Transmit off makes it receive-only.");
        break;
    case MESH_UI_SETTINGS_SECURITY:
        snprintf(out, out_len, "%s",
                 "A new private key changes this node's identity: peers must learn it again "
                 "and old direct messages stay unreadable. Managed mode locks out every "
                 "client whose key is not an admin key, this one included.");
        break;
    case MESH_UI_SETTINGS_POWER:
        snprintf(out, out_len, "%s",
                 "The radio will reboot. Power saving puts the radio to sleep between "
                 "packets, and a short light-sleep or wake time can leave too little "
                 "Bluetooth on for this client to reconnect on its own.");
        break;
    default:
        snprintf(out, out_len, "%s", "The radio will reboot to apply this.");
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
