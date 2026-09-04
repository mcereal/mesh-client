#include "mesh/ui/settings.h"

#include "mesh/radio_settings.h"

#include <stdio.h>
#include <string.h>

const char *mesh_ui_settings_section_name(enum mesh_ui_settings_section section) {
    switch (section) {
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
        return handshake != NULL && handshake->channel_count > 0U;
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

/* 0 means "firmware default" for these, and the presets are what the phone apps offer. */
static const uint32_t k_screen_on_presets[] = {0U,   15U,  30U,  60U,   120U,
                                               300U, 600U, 900U, 1800U, 3600U};
static const uint32_t k_carousel_presets[] = {0U, 10U, 15U, 30U, 60U, 120U, 300U, 600U};
static const uint32_t k_interval_presets[] = {0U,    60U,   300U,   900U,   1800U,
                                              3600U, 7200U, 14400U, 43200U, 86400U};

struct field_spec {
    const char *label;
    enum mesh_ui_setting_kind kind;
    enum mesh_ui_settings_section section;
    uint32_t limit; /* TEXT: max bytes; ENUM: value count */
    const char *(*enum_name)(uint32_t value);
    const uint32_t *presets; /* NUMBER */
    size_t preset_count;
    const char *zero_label; /* NUMBER: what 0 means */
};

#define PRESETS(array) (array), (sizeof(array) / sizeof((array)[0]))

/* User.long_name is 39 bytes on the wire but the firmware truncates to 24 (mesh.proto). */
static const struct field_spec k_fields[MESH_UI_FIELD_COUNT] = {
    [MESH_UI_FIELD_NONE] = {"?", MESH_UI_SETTING_INFO, MESH_UI_SETTINGS_SECTION_COUNT, 0U, NULL,
                            NULL, 0U, NULL},
    [MESH_UI_FIELD_USER_LONG_NAME] = {"Long name", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_USER, 24U,
                                      NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_USER_SHORT_NAME] = {"Short name", MESH_UI_SETTING_TEXT, MESH_UI_SETTINGS_USER,
                                       4U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_USER_LICENSED] = {"Licensed operator", MESH_UI_SETTING_TOGGLE,
                                     MESH_UI_SETTINGS_USER, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_USER_UNMESSAGEABLE] = {"Unmessageable", MESH_UI_SETTING_TOGGLE,
                                          MESH_UI_SETTINGS_USER, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_DISPLAY_SCREEN_ON] = {"Screen on", MESH_UI_SETTING_NUMBER,
                                         MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                         PRESETS(k_screen_on_presets), "default"},
    [MESH_UI_FIELD_DISPLAY_CAROUSEL] = {"Carousel", MESH_UI_SETTING_NUMBER,
                                        MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                        PRESETS(k_carousel_presets), "off"},
    [MESH_UI_FIELD_DISPLAY_COMPASS] = {"Compass", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DISPLAY,
                                       8U, compass_name, NULL, 0U, NULL},
    [MESH_UI_FIELD_DISPLAY_12H] = {"12-hour clock", MESH_UI_SETTING_TOGGLE,
                                   MESH_UI_SETTINGS_DISPLAY, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_DISPLAY_UNITS] = {"Units", MESH_UI_SETTING_ENUM, MESH_UI_SETTINGS_DISPLAY, 2U,
                                     units_name, NULL, 0U, NULL},
    [MESH_UI_FIELD_DISPLAY_FLIP] = {"Flip screen", MESH_UI_SETTING_TOGGLE, MESH_UI_SETTINGS_DISPLAY,
                                    0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_SF_ENABLED] = {"Store & Forward", MESH_UI_SETTING_TOGGLE,
                                  MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_SF_HEARTBEAT] = {"Heartbeat", MESH_UI_SETTING_TOGGLE,
                                    MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_SF_SERVER] = {"Act as server", MESH_UI_SETTING_TOGGLE,
                                 MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_TELEMETRY_DEVICE] = {"Device metrics", MESH_UI_SETTING_TOGGLE,
                                        MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_TELEMETRY_INTERVAL] = {"Device interval", MESH_UI_SETTING_NUMBER,
                                          MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                          PRESETS(k_interval_presets), "default"},
    [MESH_UI_FIELD_TELEMETRY_ENVIRONMENT] = {"Environment", MESH_UI_SETTING_TOGGLE,
                                             MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_TELEMETRY_ENV_SCREEN] = {"Env on screen", MESH_UI_SETTING_TOGGLE,
                                            MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT] = {"Env in Fahrenheit", MESH_UI_SETTING_TOGGLE,
                                                MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NULL, 0U,
                                                NULL},
    [MESH_UI_FIELD_TELEMETRY_AIR_QUALITY] = {"Air quality", MESH_UI_SETTING_TOGGLE,
                                             MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NULL, 0U, NULL},
    [MESH_UI_FIELD_TELEMETRY_POWER] = {"Power metrics", MESH_UI_SETTING_TOGGLE,
                                       MESH_UI_SETTINGS_TELEMETRY, 0U, NULL, NULL, 0U, NULL},
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
    if (spec->kind != MESH_UI_SETTING_TEXT) {
        return 0U;
    }
    return spec->limit < MESH_UI_SETTING_TEXT_MAX ? spec->limit : MESH_UI_SETTING_TEXT_MAX - 1U;
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

static void item_number(struct item_list *list, const char *label, uint32_t value,
                        const char *unit) {
    struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_NUMBER);
    if (item != NULL) {
        snprintf(item->value, sizeof item->value, "%u%s", (unsigned)value, unit);
    }
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

static void item_seconds(struct item_list *list, const char *label, uint32_t seconds,
                         const char *zero) {
    struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_NUMBER);
    if (item != NULL) {
        format_seconds(item->value, sizeof item->value, seconds, zero);
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
        format_seconds(item->value, sizeof item->value, number,
                       spec->zero_label != NULL ? spec->zero_label : "0");
        break;
    case MESH_UI_SETTING_TEXT:
        snprintf(item->text, sizeof item->text, "%s", text != NULL ? text : "");
        snprintf(item->value, sizeof item->value, "%s", item->text[0] != '\0' ? item->text : "-");
        break;
    default:
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

static const char *pairing_name(uint8_t mode) {
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

static const char *rebroadcast_name(uint8_t mode) {
    static const char *const k_names[] = {
        "All", "All skip decoding", "Local only", "Known only", "None", "Core portnums only",
    };
    return mode < 6U ? k_names[mode] : "?";
}

static const char *gps_mode_name(uint8_t mode) {
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
    item_text(list, "Role", MESH_UI_SETTING_ENUM, mesh_radio_role_name(s->role));
    item_text(list, "Time zone", MESH_UI_SETTING_TEXT, s->tzdef[0] != '\0' ? s->tzdef : "not set");
    item_text(list, "Rebroadcast", MESH_UI_SETTING_ENUM, rebroadcast_name(s->rebroadcast_mode));
    item_toggle(list, "LED heartbeat", !s->led_heartbeat_disabled);
    item_toggle(list, "Double tap = button", s->double_tap_as_button_press);
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
    char buffer[48];
    item_text(list, "Region", MESH_UI_SETTING_ENUM, mesh_radio_region_name(s->region));
    item_toggle(list, "Use preset", s->use_preset);
    if (s->use_preset) {
        item_text(list, "Preset", MESH_UI_SETTING_ENUM,
                  mesh_radio_modem_preset_name(s->modem_preset));
    } else {
        snprintf(buffer, sizeof buffer, "%u kHz", (unsigned)s->bandwidth);
        item_text(list, "Bandwidth", MESH_UI_SETTING_NUMBER, buffer);
        item_number(list, "Spread factor", s->spread_factor, "");
        snprintf(buffer, sizeof buffer, "4/%u", (unsigned)s->coding_rate);
        item_text(list, "Coding rate", MESH_UI_SETTING_NUMBER, buffer);
    }
    item_number(list, "Hop limit", s->hop_limit, "");
    item_toggle(list, "Transmit", s->tx_enabled);
    snprintf(buffer, sizeof buffer, "%d dBm%s", (int)s->tx_power, s->tx_power == 0 ? " (max)" : "");
    item_text(list, "TX power", MESH_UI_SETTING_NUMBER, buffer);
    item_toggle(list, "Ignore MQTT", s->ignore_mqtt);
    item_toggle(list, "OK to MQTT", s->config_ok_to_mqtt);
}

static void build_bluetooth(const struct mesh_ui_settings *s, struct item_list *list) {
    char buffer[48];
    item_toggle(list, "Bluetooth", s->bluetooth_enabled);
    item_text(list, "Pairing", MESH_UI_SETTING_ENUM, pairing_name(s->pairing_mode));
    if (s->pairing_mode == 1U) {
        snprintf(buffer, sizeof buffer, "%06u", (unsigned)s->fixed_pin);
        item_text(list, "Fixed PIN", MESH_UI_SETTING_NUMBER, buffer);
    }
}

static void build_channels(const struct mesh_ui_handshake_state *hs, struct item_list *list) {
    for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
        const struct mesh_ui_channel *channel = &hs->channels[i];
        if (channel->role == 0U) {
            continue;
        }
        char label[MESH_UI_SETTINGS_LABEL_MAX];
        snprintf(label, sizeof label, "%u %s", (unsigned)channel->index,
                 channel->name[0] != '\0' ? channel->name
                                          : (channel->index == 0U ? "Primary" : "?"));
        const char *key = channel->psk_len == 0U    ? "no key"
                          : channel->psk_len == 1U  ? "default key"
                          : channel->psk_len == 16U ? "AES-128"
                          : channel->psk_len == 32U ? "AES-256"
                                                    : "odd key";
        struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_ACTION);
        if (item != NULL) {
            snprintf(item->value, sizeof item->value, "%s, %s, up %s, down %s",
                     channel->role == 1U ? "primary" : "secondary", key,
                     channel->uplink_enabled ? "on" : "off",
                     channel->downlink_enabled ? "on" : "off");
        }
    }
    if (list->count == 0U) {
        item_text(list, "Channels", MESH_UI_SETTING_INFO, "none enabled");
    }
}

static void build_security(const struct mesh_ui_settings *s, struct item_list *list) {
    char buffer[48];
    item_key(list, "Public key", s->public_key, s->public_key_len);
    item_text(list, "Private key", MESH_UI_SETTING_KEY, s->has_private_key ? "present" : "none");
    snprintf(buffer, sizeof buffer, "%u", (unsigned)s->admin_key_count);
    item_text(list, "Admin keys", MESH_UI_SETTING_INFO, buffer);
    item_text(list, "Packet signing", MESH_UI_SETTING_ENUM,
              s->packet_signature_policy == 1U ? "Balanced" : "Compatible");
    item_toggle(list, "Managed mode", s->is_managed);
    item_toggle(list, "Admin channel", s->admin_channel_enabled);
    item_toggle(list, "Serial console", s->serial_enabled);
    item_toggle(list, "Debug log API", s->debug_log_api_enabled);
}

static void build_position(const struct mesh_ui_settings *s, struct item_list *list) {
    item_text(list, "GPS", MESH_UI_SETTING_ENUM, gps_mode_name(s->gps_mode));
    item_seconds(list, "Broadcast every", s->position_broadcast_secs, "default");
    item_toggle(list, "Smart broadcast", s->position_broadcast_smart_enabled);
    item_toggle(list, "Fixed position", s->fixed_position);
}

static void build_power(const struct mesh_ui_settings *s, struct item_list *list) {
    item_toggle(list, "Power saving", s->is_power_saving);
    item_seconds(list, "Light sleep", s->ls_secs, "default");
    item_seconds(list, "Min wake", s->min_wake_secs, "default");
    item_seconds(list, "Shutdown on battery", s->on_battery_shutdown_after_secs, "off");
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
                          enum mesh_ui_settings_section section, struct item_list *list) {
    memset(list, 0, sizeof *list);
    list->edits = edits;
    list->edit_count = edits != NULL ? edit_count : 0U;
    if (settings == NULL || !mesh_ui_settings_section_loaded(settings, handshake, section)) {
        return;
    }
    switch (section) {
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
        build_channels(handshake, list);
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
                                     enum mesh_ui_settings_section section) {
    struct item_list list;
    build_section(settings, handshake, NULL, 0U, section, &list);
    return list.count;
}

bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_settings_section section, uint32_t row,
                           struct mesh_ui_settings_item *out) {
    if (out == NULL) {
        return false;
    }
    struct item_list list;
    build_section(settings, handshake, edits, edit_count, section, &list);
    if (row >= list.count) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = list.items[row];
    return true;
}
