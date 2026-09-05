#include "mesh/ui/settings.h"

#include "mesh/radio_settings.h"
#include "mesh/updater.h"

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
    default:
        return "?";
    }
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

struct field_spec {
    const char *label;
    enum mesh_ui_setting_kind kind;
    enum mesh_ui_settings_section section;
    uint32_t limit; /* TEXT: max bytes; ENUM: value count */
    const char *(*enum_name)(uint32_t value);
    const uint32_t *presets; /* NUMBER */
    size_t preset_count;
    const char *zero_label; /* NUMBER: what 0 means (seconds formatting) */
    void (*format)(uint32_t value, char *out, size_t out_len); /* NUMBER: overrides seconds */
    uint32_t choices; /* KEY: MESH_UI_PSK_CHOICE_BIT mask Left/Right walk */
};

#define CHANNEL_KEY_CHOICES                                                                        \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_DEFAULT) |      \
     MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_128) |                                              \
     MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_NONE))
#define PRIVATE_KEY_CHOICES                                                                        \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256))
#define ADMIN_KEY_CHOICES                                                                          \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_NONE))

#define PRESETS(array) (array), (sizeof(array) / sizeof((array)[0]))
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
    [MESH_UI_FIELD_POSITION_FIXED] = {"Fixed position", MESH_UI_SETTING_TOGGLE,
                                      MESH_UI_SETTINGS_POSITION, 0U, NULL, NO_PRESETS, NULL, NULL,
                                      0U},
    [MESH_UI_FIELD_POSITION_GPS_INTERVAL] = {"GPS interval", MESH_UI_SETTING_NUMBER,
                                             MESH_UI_SETTINGS_POSITION, 0U, NULL,
                                             PRESETS(k_gps_interval_presets), "default", NULL, 0U},
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
    [MESH_UI_FIELD_SF_ENABLED] = {"Store & Forward", MESH_UI_SETTING_TOGGLE,
                                  MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, NULL, NULL,
                                  0U},
    [MESH_UI_FIELD_SF_HEARTBEAT] = {"Heartbeat", MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, NULL,
                                    NULL, 0U},
    [MESH_UI_FIELD_SF_SERVER] = {"Act as server", MESH_UI_SETTING_TOGGLE,
                                 MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, NULL, NULL,
                                 0U},
    [MESH_UI_FIELD_TELEMETRY_DEVICE] = {"Device metrics", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                        NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_INTERVAL] = {"Device interval", MESH_UI_SETTING_NUMBER,
                                          MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                          PRESETS(k_interval_presets), "default", NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENVIRONMENT] = {"Environment", MESH_UI_SETTING_TOGGLE,
                                             MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                             NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_SCREEN] = {"Env on screen", MESH_UI_SETTING_TOGGLE,
                                            MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                            NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT] = {"Env in Fahrenheit", MESH_UI_SETTING_TOGGLE,
                                                MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS,
                                                NULL, NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_AIR_QUALITY] = {"Air quality", MESH_UI_SETTING_TOGGLE,
                                             MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL,
                                             NULL, 0U},
    [MESH_UI_FIELD_TELEMETRY_POWER] = {"Power metrics", MESH_UI_SETTING_TOGGLE,
                                       MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NO_PRESETS, NULL, NULL,
                                       0U},
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
    [MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY] = {"Packet signing", MESH_UI_SETTING_ENUM,
                                                 MESH_UI_SETTINGS_SECURITY, 3U,
                                                 signature_policy_name, NO_PRESETS, NULL, NULL, 0U},
};

static const struct field_spec *field_spec(enum mesh_ui_setting_field field) {
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

void mesh_ui_settings_confirm_text(enum mesh_ui_settings_section section, char *out,
                                   size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
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

void mesh_ui_settings_key_hex(const uint8_t *key, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (key == NULL) {
        return;
    }
    size_t pos = 0U;
    for (size_t i = 0; i < len && pos + 3U <= out_len; ++i) {
        snprintf(out + pos, out_len - pos, "%02x", key[i]);
        pos += 2U;
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static const char k_base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void mesh_ui_settings_key_text(const uint8_t *key, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (key == NULL || (len + 2U) / 3U * 4U + 1U > out_len) {
        return;
    }
    size_t pos = 0U;
    for (size_t i = 0; i < len; i += 3U) {
        const uint32_t b0 = key[i];
        const uint32_t b1 = i + 1U < len ? key[i + 1U] : 0U;
        const uint32_t b2 = i + 2U < len ? key[i + 2U] : 0U;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[pos++] = k_base64[(triple >> 18) & 0x3FU];
        out[pos++] = k_base64[(triple >> 12) & 0x3FU];
        out[pos++] = i + 1U < len ? k_base64[(triple >> 6) & 0x3FU] : '=';
        out[pos++] = i + 2U < len ? k_base64[triple & 0x3FU] : '=';
    }
    out[pos] = '\0';
}

static int base64_value(char c) {
    const char *at = c != '\0' ? strchr(k_base64, c) : NULL;
    return at != NULL ? (int)(at - k_base64) : -1;
}

static bool parse_hex(const char *text, size_t digits, uint8_t *out, size_t out_cap,
                      size_t *out_len) {
    if (digits % 2U != 0U || digits / 2U > out_cap) {
        return false;
    }
    for (size_t i = 0; i < digits; i += 2U) {
        const int hi = hex_nibble(text[i]);
        const int lo = hex_nibble(text[i + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2U] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = digits / 2U;
    return true;
}

static bool parse_base64(const char *text, size_t chars, uint8_t *out, size_t out_cap,
                         size_t *out_len) {
    if (chars % 4U != 0U) {
        return false;
    }
    size_t len = 0U;
    for (size_t i = 0; i < chars; i += 4U) {
        int values[4];
        unsigned pad = 0U;
        for (unsigned j = 0; j < 4U; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                /* Padding only in the last group's last two places. */
                if (i + 4U != chars || j < 2U) {
                    return false;
                }
                pad++;
                values[j] = 0;
                continue;
            }
            if (pad > 0U) {
                return false;
            }
            values[j] = base64_value(c);
            if (values[j] < 0) {
                return false;
            }
        }
        const uint32_t triple = ((uint32_t)values[0] << 18) | ((uint32_t)values[1] << 12) |
                                ((uint32_t)values[2] << 6) | (uint32_t)values[3];
        const unsigned bytes = 3U - pad;
        if (len + bytes > out_cap) {
            return false;
        }
        out[len++] = (uint8_t)(triple >> 16);
        if (bytes > 1U) {
            out[len++] = (uint8_t)(triple >> 8);
        }
        if (bytes > 2U) {
            out[len++] = (uint8_t)triple;
        }
    }
    *out_len = len;
    return true;
}

bool mesh_ui_settings_key_parse(const char *text, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (text == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    const size_t chars = strlen(text);
    if (chars == 0U) {
        *out_len = 0U;
        return true;
    }
    /* Hex first: a base64 string made only of hex digits is ambiguous, and hex is what the
       firmware logs show. Only the three key sizes are hex; anything else is base64. */
    bool all_hex = true;
    for (size_t i = 0; i < chars; ++i) {
        if (hex_nibble(text[i]) < 0) {
            all_hex = false;
            break;
        }
    }
    if (all_hex && (chars == 2U || chars == 32U || chars == 64U)) {
        return parse_hex(text, chars, out, out_cap, out_len);
    }
    return parse_base64(text, chars, out, out_cap, out_len);
}

/* "oKGio6Sl... (AES-128)", "default key", "no encryption"; `aes` names the size the way the
   channel list does, else it is plain bits. */
static void key_summary(const uint8_t *key, size_t len, bool aes, char *out, size_t out_len) {
    if (len == 0U) {
        snprintf(out, out_len, "%s", aes ? "no encryption" : "none");
        return;
    }
    if (len == 1U) {
        if (key[0] == 1U) {
            snprintf(out, out_len, "%s", "default key");
        } else {
            snprintf(out, out_len, "simple key %u", (unsigned)key[0]);
        }
        return;
    }
    char text[48];
    mesh_ui_settings_key_text(key, len, text, sizeof text);
    if (aes) {
        snprintf(out, out_len, "%.8s... (AES-%u)", text, (unsigned)(len * 8U));
    } else {
        snprintf(out, out_len, "%.8s... (%u-bit)", text, (unsigned)(len * 8U));
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

/* ---- item builders ------------------------------------------------------------------------ */

struct item_list {
    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    uint32_t count;
    const struct mesh_ui_setting_edit *edits;
    size_t edit_count;
};

static struct mesh_ui_settings_item *item_add(struct item_list *list, const char *label,
                                              enum mesh_ui_setting_kind kind) {
    if (list->count >= MESH_UI_SETTINGS_ITEMS_MAX) {
        return NULL;
    }
    struct mesh_ui_settings_item *item = &list->items[list->count++];
    memset(item, 0, sizeof *item);
    snprintf(item->label, sizeof item->label, "%s", label);
    item->kind = kind;
    return item;
}

static void item_text(struct item_list *list, const char *label, enum mesh_ui_setting_kind kind,
                      const char *value) {
    struct mesh_ui_settings_item *item = item_add(list, label, kind);
    if (item != NULL) {
        snprintf(item->value, sizeof item->value, "%.*s", (int)(sizeof item->value - 1U), value);
    }
}

static void item_toggle(struct item_list *list, const char *label, bool value) {
    item_text(list, label, MESH_UI_SETTING_TOGGLE, value ? "on" : "off");
}

/* "30s", "5m", "2h"; `zero` says what 0 means for this field ("off", "default"). */
static void format_seconds(char *out, size_t out_len, uint32_t seconds, const char *zero) {
    if (seconds == 0U) {
        snprintf(out, out_len, "%s", zero);
    } else if (seconds % 3600U == 0U) {
        snprintf(out, out_len, "%uh", (unsigned)(seconds / 3600U));
    } else if (seconds % 60U == 0U) {
        snprintf(out, out_len, "%um", (unsigned)(seconds / 60U));
    } else {
        snprintf(out, out_len, "%us", (unsigned)seconds);
    }
}

/* An editable row: the field's spec supplies label and kind; a pending edit replaces the
   radio's value and marks the row dirty. `text` is only read for TEXT fields. */
static void item_field(struct item_list *list, enum mesh_ui_setting_field field, uint32_t number,
                       const char *text) {
    const struct field_spec *spec = field_spec(field);
    struct mesh_ui_settings_item *item = item_add(list, spec->label, spec->kind);
    if (item == NULL) {
        return;
    }
    item->field = field;
    const struct mesh_ui_setting_edit *edit =
        mesh_ui_settings_find_edit(list->edits, list->edit_count, field);
    if (edit != NULL) {
        item->dirty = true;
        number = edit->number;
        text = edit->text;
    }
    item->number = number;
    switch (spec->kind) {
    case MESH_UI_SETTING_TOGGLE:
        snprintf(item->value, sizeof item->value, "%s", number != 0U ? "on" : "off");
        break;
    case MESH_UI_SETTING_ENUM:
        snprintf(item->value, sizeof item->value, "%s", mesh_ui_settings_enum_name(field, number));
        break;
    case MESH_UI_SETTING_NUMBER:
        if (spec->format != NULL) {
            spec->format(number, item->value, sizeof item->value);
        } else {
            format_seconds(item->value, sizeof item->value, number,
                           spec->zero_label != NULL ? spec->zero_label : "0");
        }
        break;
    case MESH_UI_SETTING_TEXT:
        snprintf(item->text, sizeof item->text, "%s", text != NULL ? text : "");
        snprintf(item->value, sizeof item->value, "%.*s", (int)(sizeof item->value - 1U),
                 item->text[0] != '\0' ? item->text : "-");
        break;
    default:
        break;
    }
}

/* A KEY row. `key`/`len` is the radio's current key; an edit is a choice, or typed text. The
   text carried is what the keyboard should open on: the typed text if there is one, else the
   current key as base64 (an explicit reveal, never shown in the row). */
static void item_key_field(struct item_list *list, enum mesh_ui_setting_field field,
                           const uint8_t *key, size_t len) {
    const struct field_spec *spec = field_spec(field);
    const bool aes = (field == MESH_UI_FIELD_CHANNEL_KEY);
    struct mesh_ui_settings_item *item = item_add(list, spec->label, spec->kind);
    if (item == NULL) {
        return;
    }
    item->field = field;
    mesh_ui_settings_key_text(key, len, item->text, sizeof item->text);
    const struct mesh_ui_setting_edit *edit =
        mesh_ui_settings_find_edit(list->edits, list->edit_count, field);
    item->number = edit != NULL ? edit->number : (uint32_t)MESH_UI_PSK_KEEP;
    item->dirty = edit != NULL;
    switch ((enum mesh_ui_psk_choice)item->number) {
    case MESH_UI_PSK_DEFAULT:
        snprintf(item->value, sizeof item->value, "%s", "default key");
        break;
    case MESH_UI_PSK_RANDOM_128:
        snprintf(item->value, sizeof item->value, "%s", "new random AES-128");
        break;
    case MESH_UI_PSK_RANDOM_256:
        snprintf(item->value, sizeof item->value, "%s",
                 aes ? "new random AES-256" : "new random key");
        break;
    case MESH_UI_PSK_NONE:
        snprintf(item->value, sizeof item->value, "%s", aes ? "no encryption" : "none (clear)");
        break;
    case MESH_UI_PSK_TYPED: {
        uint8_t typed[MESH_UI_PSK_MAX];
        size_t typed_len = 0U;
        snprintf(item->text, sizeof item->text, "%s", edit->text);
        if (mesh_ui_settings_key_parse(edit->text, typed, sizeof typed, &typed_len)) {
            key_summary(typed, typed_len, aes, item->value, sizeof item->value);
        } else {
            snprintf(item->value, sizeof item->value, "%s", "invalid key");
        }
        break;
    }
    case MESH_UI_PSK_KEEP:
    default:
        key_summary(key, len, aes, item->value, sizeof item->value);
        break;
    }
}

/* Keys are shown as a short fingerprint: enough to compare against the phone app's view,
   not enough to leak the key to someone reading over your shoulder. */
static void item_key(struct item_list *list, const char *label, const uint8_t *key, size_t len) {
    struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_KEY);
    if (item == NULL) {
        return;
    }
    if (len == 0U) {
        snprintf(item->value, sizeof item->value, "%s", "none");
        return;
    }
    size_t shown = len < 4U ? len : 4U;
    char hex[9] = {0};
    for (size_t i = 0; i < shown; ++i) {
        snprintf(hex + 2U * i, sizeof hex - 2U * i, "%02x", key[i]);
    }
    snprintf(item->value, sizeof item->value, "%s... (%u bytes)", hex, (unsigned)len);
}

/* An ACTION row: drawn like an editable one and activated with A, carrying what it does in
   `number` so the nav can raise the action without knowing about updates. */
static void item_action(struct item_list *list, const char *label, const char *value,
                        enum mesh_ui_settings_action action) {
    struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_ACTION);
    if (item != NULL) {
        snprintf(item->value, sizeof item->value, "%s", value);
        item->number = (uint32_t)action;
    }
}

/*
 * About: what this client is, and the self-update rows. The only section that renders with no
 * radio connected, and the only one whose values come from the app rather than the air.
 *
 * Every row that responds to A carries a verb in its value column, not a bare "A". The button
 * hint on its own read as data - "Check for updates > A" looks like a setting whose value is
 * the letter A - and left no clue that anything would happen.
 *
 * The update rows are deliberately a check and a separate install rather than one button. The
 * install downloads and replaces the running binary, so it is worth a second, deliberate press
 * once the user can see which version they are about to move to.
 */
static void build_about(const struct mesh_ui_settings *s, struct item_list *list) {
    const struct mesh_ui_client_info *client = &s->client;
    item_text(list, "Version", MESH_UI_SETTING_INFO,
              client->version[0] != '\0' ? client->version : "?");
    if (client->backend[0] != '\0') {
        item_text(list, "UI backend", MESH_UI_SETTING_INFO, client->backend);
    }
    if (client->data_dir[0] != '\0') {
        item_text(list, "Data", MESH_UI_SETTING_INFO, client->data_dir);
    }

    if (!client->update_supported) {
        item_text(list, "Updates", MESH_UI_SETTING_INFO,
                  client->update_message[0] != '\0' ? client->update_message : "unavailable");
        return;
    }

    /*
     * The channel is a setting, so its value column is the setting rather than a verb; that it
     * responds to A is what the marker says. It comes before the status because it decides
     * which question a check will ask.
     *
     * While a child is running it drops to a plain fact: switching channel mid-download would
     * pull the asset out from under it, so the updater refuses, and a row that refuses is
     * worse than one that never invited the press.
     */
    const char *const channel = client->update_channel[0] != '\0' ? client->update_channel : "?";
    if (client->update_busy) {
        item_text(list, "Update channel", MESH_UI_SETTING_INFO, channel);
    } else {
        item_action(list, "Update channel", channel, MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL);
    }

    /*
     * The dev-updates switch, on a build that is not a release. It exists because the guard it
     * lifts is the only thing standing between a hand-deployed build and the install path, and
     * the alternative way in - an environment variable - needs a computer and an ssh session,
     * which is exactly what a handheld does not have. A release build never sees this row:
     * there is no guard on it to lift.
     */
    if (!client->update_is_release) {
        if (client->update_allow_dev_from_env) {
            /* Held on by MESHCLIENT_UPDATE_ALLOW_DEV. Shown as a fact rather than a switch,
               because a toggle that sprang back would look broken. */
            item_text(list, "Dev updates", MESH_UI_SETTING_INFO, "on (environment)");
        } else if (client->update_busy) {
            item_text(list, "Dev updates", MESH_UI_SETTING_INFO,
                      client->update_allow_dev ? "on" : "off");
        } else {
            item_action(list, "Dev updates", client->update_allow_dev ? "on" : "off",
                        MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES);
        }
    }

    const enum mesh_update_state state = (enum mesh_update_state)client->update_state;
    item_text(list, "Update status", MESH_UI_SETTING_INFO,
              client->update_message[0] != '\0' ? client->update_message
                                                : mesh_update_state_name(state));

    /* While a child is running neither update row does anything, so both say so rather than
       inviting a press that would be swallowed. */
    if (client->update_busy) {
        item_text(list, "Working", MESH_UI_SETTING_INFO,
                  state == MESH_UPDATE_DOWNLOADING ? "downloading..." : "checking...");
        return;
    }
    if (state == MESH_UPDATE_READY) {
        item_text(list, "Installed", MESH_UI_SETTING_INFO, "quit and relaunch");
        return;
    }

    item_action(list, "Check for updates", "press A", MESH_UI_SETTINGS_ACTION_CHECK_UPDATE);
    if (state == MESH_UPDATE_AVAILABLE) {
        /* The version goes in the label so the value column can say how to act on it: the row
           the user has to find is the one that names what it will install. The label is
           bounded by its own column, not by what the release named itself. */
        char label[MESH_UI_SETTINGS_LABEL_MAX];
        snprintf(label, sizeof label, "Install %.*s", (int)(sizeof label - 9U),
                 client->update_latest);
        item_action(list, label, "press A", MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE);
    } else if (!client->update_can_install) {
        /* Nothing here will offer an install, so say so once - and name the row that changes
           it, rather than leaving the user hunting for one that is never coming. */
        item_text(list, "Installing", MESH_UI_SETTING_INFO, "turn on Dev updates");
    }
}

static void build_radio(const struct mesh_ui_settings *s, const struct mesh_ui_handshake_state *hs,
                        struct item_list *list) {
    char buffer[48];
    if (s->has_metadata) {
        item_text(list, "Firmware", MESH_UI_SETTING_INFO,
                  s->firmware_version[0] != '\0' ? s->firmware_version : "?");
        item_text(list, "Hardware", MESH_UI_SETTING_INFO,
                  mesh_radio_hw_model_name(s->hw_model, buffer, sizeof buffer));
    }
    if (hs != NULL && hs->has_my_info) {
        snprintf(buffer, sizeof buffer, "!%08x", hs->my_info.node_num);
        item_text(list, "Node number", MESH_UI_SETTING_INFO, buffer);
        snprintf(buffer, sizeof buffer, "%u", hs->my_info.reboot_count);
        item_text(list, "Reboots", MESH_UI_SETTING_INFO, buffer);
    }
    if (s->has_lora) {
        item_text(list, "LoRa region", MESH_UI_SETTING_INFO, mesh_radio_region_name(s->region));
    }
    if (s->has_metadata) {
        snprintf(buffer, sizeof buffer, "%s%s%s%s", s->has_bluetooth_radio ? "BLE " : "",
                 s->has_wifi ? "WiFi " : "", s->has_ethernet ? "Ethernet " : "",
                 s->has_pkc ? "PKC" : "");
        item_text(list, "Capabilities", MESH_UI_SETTING_INFO, buffer[0] != '\0' ? buffer : "none");
        item_toggle(list, "Can shut down", s->can_shutdown);
    }
    if (s->admin_ok) {
        snprintf(buffer, sizeof buffer, "ok (%u replies)%s", (unsigned)s->admin_replies,
                 s->write_pending ? ", saving"
                 : s->admin_busy  ? ", refreshing"
                                  : "");
    } else {
        snprintf(buffer, sizeof buffer, "%s", s->admin_busy ? "waiting for reply" : "no reply yet");
    }
    item_text(list, "Admin session", MESH_UI_SETTING_INFO, buffer);
}

static void build_user(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_USER_LONG_NAME, 0U, s->long_name);
    item_field(list, MESH_UI_FIELD_USER_SHORT_NAME, 0U, s->short_name);
    item_field(list, MESH_UI_FIELD_USER_LICENSED, s->is_licensed ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_USER_UNMESSAGEABLE, s->is_unmessagable ? 1U : 0U, NULL);
}

static void build_device(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DEVICE_ROLE, s->role, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_TZDEF, 0U, s->tzdef);
    item_field(list, MESH_UI_FIELD_DEVICE_REBROADCAST, s->rebroadcast_mode, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_NODEINFO_SECS, s->node_info_broadcast_secs, NULL);
    /* The protobuf field is led_heartbeat_disabled; the row is the plain statement. */
    item_field(list, MESH_UI_FIELD_DEVICE_LED_HEARTBEAT, s->led_heartbeat_disabled ? 0U : 1U, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_DOUBLE_TAP, s->double_tap_as_button_press ? 1U : 0U,
               NULL);
}

static void build_display(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DISPLAY_SCREEN_ON, s->screen_on_secs, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_CAROUSEL, s->carousel_secs, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_COMPASS, s->compass_orientation, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_12H, s->use_12h_clock ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_UNITS, s->units, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_FLIP, s->flip_screen ? 1U : 0U, NULL);
}

static void build_lora(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_LORA_REGION, s->region, NULL);
    item_field(list, MESH_UI_FIELD_LORA_USE_PRESET, s->use_preset ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_PRESET, s->modem_preset, NULL);
    /* The manual trio only applies with the preset off; they stay listed so the row count
       does not move under the cursor as the toggle is edited. */
    item_field(list, MESH_UI_FIELD_LORA_BANDWIDTH, s->bandwidth, NULL);
    item_field(list, MESH_UI_FIELD_LORA_SPREAD, s->spread_factor, NULL);
    item_field(list, MESH_UI_FIELD_LORA_CODING, s->coding_rate, NULL);
    item_field(list, MESH_UI_FIELD_LORA_HOPS, s->hop_limit, NULL);
    item_field(list, MESH_UI_FIELD_LORA_TX_ENABLED, s->tx_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_TX_POWER, (uint32_t)(uint8_t)s->tx_power, NULL);
    item_field(list, MESH_UI_FIELD_LORA_IGNORE_MQTT, s->ignore_mqtt ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_OK_TO_MQTT, s->config_ok_to_mqtt ? 1U : 0U, NULL);
}

static void build_bluetooth(const struct mesh_ui_settings *s, struct item_list *list) {
    char pin[8];
    snprintf(pin, sizeof pin, "%06u", (unsigned)(s->fixed_pin % 1000000U));
    item_field(list, MESH_UI_FIELD_BT_ENABLED, s->bluetooth_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_BT_MODE, s->pairing_mode, NULL);
    item_field(list, MESH_UI_FIELD_BT_PIN, 0U, pin);
}

static void channel_label(uint8_t index, const char *name, char *out, size_t out_len) {
    snprintf(out, out_len, "%u %s", (unsigned)index,
             name[0] != '\0' ? name : (index == 0U ? "Primary" : "?"));
}

static void channel_summary(uint8_t role, uint8_t psk_len, bool uplink, bool downlink, char *out,
                            size_t out_len) {
    const char *key = psk_len == 0U    ? "no key"
                      : psk_len == 1U  ? "default key"
                      : psk_len == 16U ? "AES-128"
                      : psk_len == 32U ? "AES-256"
                                       : "odd key";
    snprintf(out, out_len, "%s, %s, up %s, down %s", role == 1U ? "primary" : "secondary", key,
             uplink ? "on" : "off", downlink ? "on" : "off");
}

/* The channel list. With the radio's full table held every slot is listed, disabled ones
   included, and A opens it: that is how a channel is added (set up an empty slot) or removed
   (set its role to Disabled). Without the table only the handshake summary of the enabled
   slots is shown, read-only. */
static void build_channels(const struct mesh_ui_settings *s,
                           const struct mesh_ui_handshake_state *hs, struct item_list *list) {
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    if (s->has_channels) {
        for (uint32_t i = 0; i < MESH_UI_MAX_CHANNELS; ++i) {
            const struct mesh_ui_channel_detail *channel = &s->channels[i];
            if (!channel->present) {
                continue;
            }
            if (channel->role == 0U) {
                snprintf(label, sizeof label, "%u (empty)", (unsigned)channel->index);
            } else {
                channel_label(channel->index, channel->name, label, sizeof label);
            }
            struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_ACTION);
            if (item == NULL) {
                continue;
            }
            item->number = channel->index;
            if (channel->role == 0U) {
                snprintf(item->value, sizeof item->value, "%s", "disabled, A to set up");
            } else {
                channel_summary(channel->role, channel->psk_len, channel->uplink_enabled,
                                channel->downlink_enabled, item->value, sizeof item->value);
            }
        }
    } else if (hs != NULL) {
        for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
            const struct mesh_ui_channel *channel = &hs->channels[i];
            if (channel->role == 0U) {
                continue;
            }
            channel_label(channel->index, channel->name, label, sizeof label);
            struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_INFO);
            if (item != NULL) {
                channel_summary(channel->role, channel->psk_len, channel->uplink_enabled,
                                channel->downlink_enabled, item->value, sizeof item->value);
                item->number = channel->index;
            }
        }
    }
    if (list->count == 0U) {
        item_text(list, "Channels", MESH_UI_SETTING_INFO, "none known yet");
    }
}

/* One channel's rows. The primary slot's role is shown but not offered: a mesh with two
   primaries or none is not something to reach by accident. */
static void build_channel(const struct mesh_ui_settings *s, uint8_t slot, struct item_list *list) {
    if (slot >= MESH_UI_MAX_CHANNELS || !s->channels[slot].present) {
        return;
    }
    const struct mesh_ui_channel_detail *channel = &s->channels[slot];
    item_field(list, MESH_UI_FIELD_CHANNEL_NAME, 0U, channel->name);
    if (channel->role == 1U) {
        item_text(list, "Role", MESH_UI_SETTING_INFO, "Primary");
    } else {
        item_field(list, MESH_UI_FIELD_CHANNEL_ROLE, channel->role == 2U ? 1U : 0U, NULL);
    }
    item_key_field(list, MESH_UI_FIELD_CHANNEL_KEY, channel->psk, channel->psk_len);
    item_field(list, MESH_UI_FIELD_CHANNEL_UPLINK, channel->uplink_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_CHANNEL_DOWNLINK, channel->downlink_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_CHANNEL_POSITION, channel->position_precision, NULL);
}

int mesh_ui_settings_channel_at_row(const struct mesh_ui_settings *settings,
                                    const struct mesh_ui_handshake_state *handshake, uint32_t row) {
    struct mesh_ui_settings_item item;
    if (settings == NULL || !settings->has_channels ||
        !mesh_ui_settings_item(settings, handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, row, &item) ||
        item.kind != MESH_UI_SETTING_ACTION) {
        return -1;
    }
    return (int)item.number;
}

static void build_security(const struct mesh_ui_settings *s, struct item_list *list) {
    item_key(list, "Public key", s->public_key, s->public_key_len);
    item_key_field(list, MESH_UI_FIELD_SECURITY_PRIVATE_KEY, s->private_key, s->private_key_len);
    for (unsigned i = 0; i < 3U; ++i) {
        item_key_field(list, (enum mesh_ui_setting_field)(MESH_UI_FIELD_SECURITY_ADMIN_KEY_0 + i),
                       s->admin_keys[i], s->admin_key_lens[i]);
    }
    item_field(list, MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY, s->packet_signature_policy, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_MANAGED, s->is_managed ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL, s->admin_channel_enabled ? 1U : 0U,
               NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_SERIAL, s->serial_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_DEBUG_LOG, s->debug_log_api_enabled ? 1U : 0U, NULL);
}

static void build_position(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_POSITION_GPS_MODE, s->gps_mode, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_BROADCAST_SECS, s->position_broadcast_secs, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_SMART, s->position_broadcast_smart_enabled ? 1U : 0U,
               NULL);
    /* Listed whatever the toggle says, so the row count does not move under the cursor while
       smart broadcast is being turned on and off; the same rule the LoRa trio follows. */
    item_field(list, MESH_UI_FIELD_POSITION_SMART_DISTANCE, s->smart_minimum_distance, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_SMART_INTERVAL, s->smart_minimum_interval_secs, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_FIXED, s->fixed_position ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_GPS_INTERVAL, s->gps_update_interval, NULL);
}

static void build_power(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_POWER_SAVING, s->is_power_saving ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_POWER_LS_SECS, s->ls_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_MIN_WAKE, s->min_wake_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_WAIT_BT, s->wait_bluetooth_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_SHUTDOWN, s->on_battery_shutdown_after_secs, NULL);
}

static void build_mqtt(const struct mesh_ui_settings *s, struct item_list *list) {
    item_toggle(list, "MQTT", s->mqtt_enabled);
    item_text(list, "Server", MESH_UI_SETTING_TEXT,
              s->mqtt_address[0] != '\0' ? s->mqtt_address : "default");
    item_text(list, "Root topic", MESH_UI_SETTING_TEXT,
              s->mqtt_root[0] != '\0' ? s->mqtt_root : "default");
    item_toggle(list, "Encryption", s->mqtt_encryption_enabled);
    item_toggle(list, "TLS", s->mqtt_tls_enabled);
    item_toggle(list, "Proxy via client", s->mqtt_proxy_to_client_enabled);
}

static void build_store_forward(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_SF_ENABLED, s->store_forward_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SF_HEARTBEAT, s->store_forward_heartbeat ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SF_SERVER, s->store_forward_is_server ? 1U : 0U, NULL);
}

static void build_telemetry(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_TELEMETRY_DEVICE, s->device_telemetry_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_INTERVAL, s->device_update_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENVIRONMENT,
               s->environment_measurement_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_SCREEN, s->environment_screen_enabled ? 1U : 0U,
               NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT,
               s->environment_display_fahrenheit ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_AIR_QUALITY, s->air_quality_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_POWER, s->power_measurement_enabled ? 1U : 0U, NULL);
}

static void build_section(const struct mesh_ui_settings *settings,
                          const struct mesh_ui_handshake_state *handshake,
                          const struct mesh_ui_setting_edit *edits, size_t edit_count,
                          enum mesh_ui_settings_section section, uint8_t channel,
                          struct item_list *list) {
    memset(list, 0, sizeof *list);
    list->edits = edits;
    list->edit_count = edits != NULL ? edit_count : 0U;
    if (settings == NULL || !mesh_ui_settings_section_loaded(settings, handshake, section)) {
        return;
    }
    switch (section) {
    case MESH_UI_SETTINGS_ABOUT:
        build_about(settings, list);
        break;
    case MESH_UI_SETTINGS_RADIO:
        build_radio(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_USER:
        build_user(settings, list);
        break;
    case MESH_UI_SETTINGS_DEVICE:
        build_device(settings, list);
        break;
    case MESH_UI_SETTINGS_DISPLAY:
        build_display(settings, list);
        break;
    case MESH_UI_SETTINGS_LORA:
        build_lora(settings, list);
        break;
    case MESH_UI_SETTINGS_BLUETOOTH:
        build_bluetooth(settings, list);
        break;
    case MESH_UI_SETTINGS_CHANNELS:
        if (channel != MESH_UI_SETTINGS_NO_CHANNEL) {
            build_channel(settings, channel, list);
        } else {
            build_channels(settings, handshake, list);
        }
        break;
    case MESH_UI_SETTINGS_SECURITY:
        build_security(settings, list);
        break;
    case MESH_UI_SETTINGS_POSITION:
        build_position(settings, list);
        break;
    case MESH_UI_SETTINGS_POWER:
        build_power(settings, list);
        break;
    case MESH_UI_SETTINGS_MQTT:
        build_mqtt(settings, list);
        break;
    case MESH_UI_SETTINGS_STORE_FORWARD:
        build_store_forward(settings, list);
        break;
    case MESH_UI_SETTINGS_TELEMETRY:
        build_telemetry(settings, list);
        break;
    default:
        break;
    }
}

uint32_t mesh_ui_settings_item_count(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section, uint8_t channel) {
    struct item_list list;
    build_section(settings, handshake, NULL, 0U, section, channel, &list);
    return list.count;
}

bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_settings_section section, uint8_t channel, uint32_t row,
                           struct mesh_ui_settings_item *out) {
    if (out == NULL) {
        return false;
    }
    struct item_list list;
    build_section(settings, handshake, edits, edit_count, section, channel, &list);
    if (row >= list.count) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = list.items[row];
    return true;
}
