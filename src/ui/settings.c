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

/* ---- item builders ------------------------------------------------------------------------ */

struct item_list {
    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    uint32_t count;
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
        snprintf(item->value, sizeof item->value, "%s", value);
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

/* "30s", "5m", "2h", "off" for 0. */
static void item_seconds(struct item_list *list, const char *label, uint32_t seconds) {
    struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_NUMBER);
    if (item == NULL) {
        return;
    }
    if (seconds == 0U) {
        snprintf(item->value, sizeof item->value, "%s", "off");
    } else if (seconds % 3600U == 0U) {
        snprintf(item->value, sizeof item->value, "%uh", (unsigned)(seconds / 3600U));
    } else if (seconds % 60U == 0U) {
        snprintf(item->value, sizeof item->value, "%um", (unsigned)(seconds / 60U));
    } else {
        snprintf(item->value, sizeof item->value, "%us", (unsigned)seconds);
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

static const char *compass_name(uint8_t orientation) {
    static const char *const k_names[] = {
        "0 deg",  "90 deg",  "180 deg",  "270 deg",
        "0 flip", "90 flip", "180 flip", "270 flip",
    };
    return orientation < 8U ? k_names[orientation] : "?";
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

static void build_radio(const struct mesh_ui_settings *s,
                        const struct mesh_ui_handshake_state *hs, struct item_list *list) {
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
                 s->admin_busy ? ", refreshing" : "");
    } else {
        snprintf(buffer, sizeof buffer, "%s", s->admin_busy ? "waiting for reply" : "no reply yet");
    }
    item_text(list, "Admin session", MESH_UI_SETTING_INFO, buffer);
}

static void build_user(const struct mesh_ui_settings *s, struct item_list *list) {
    item_text(list, "Long name", MESH_UI_SETTING_TEXT, s->long_name);
    item_text(list, "Short name", MESH_UI_SETTING_TEXT, s->short_name);
    item_toggle(list, "Licensed operator", s->is_licensed);
}

static void build_device(const struct mesh_ui_settings *s, struct item_list *list) {
    item_text(list, "Role", MESH_UI_SETTING_ENUM, mesh_radio_role_name(s->role));
    item_text(list, "Time zone", MESH_UI_SETTING_TEXT, s->tzdef[0] != '\0' ? s->tzdef : "not set");
    item_text(list, "Rebroadcast", MESH_UI_SETTING_ENUM, rebroadcast_name(s->rebroadcast_mode));
    item_toggle(list, "LED heartbeat", !s->led_heartbeat_disabled);
    item_toggle(list, "Double tap = button", s->double_tap_as_button_press);
}

static void build_display(const struct mesh_ui_settings *s, struct item_list *list) {
    item_seconds(list, "Screen on", s->screen_on_secs);
    item_seconds(list, "Carousel", s->carousel_secs);
    item_text(list, "Compass", MESH_UI_SETTING_ENUM, compass_name(s->compass_orientation));
    item_toggle(list, "12-hour clock", s->use_12h_clock);
    item_text(list, "Units", MESH_UI_SETTING_ENUM, s->units == 1U ? "Imperial" : "Metric");
    item_toggle(list, "Flip screen", s->flip_screen);
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
    snprintf(buffer, sizeof buffer, "%d dBm%s", (int)s->tx_power,
             s->tx_power == 0 ? " (max)" : "");
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
                 channel->name[0] != '\0' ? channel->name : (channel->index == 0U ? "Primary" : "?"));
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
    item_seconds(list, "Broadcast every", s->position_broadcast_secs);
    item_toggle(list, "Smart broadcast", s->position_broadcast_smart_enabled);
    item_toggle(list, "Fixed position", s->fixed_position);
}

static void build_power(const struct mesh_ui_settings *s, struct item_list *list) {
    item_toggle(list, "Power saving", s->is_power_saving);
    item_seconds(list, "Light sleep", s->ls_secs);
    item_seconds(list, "Min wake", s->min_wake_secs);
    item_seconds(list, "Shutdown on battery", s->on_battery_shutdown_after_secs);
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
    item_toggle(list, "Store & Forward", s->store_forward_enabled);
    item_toggle(list, "Heartbeat", s->store_forward_heartbeat);
    item_toggle(list, "Act as server", s->store_forward_is_server);
}

static void build_telemetry(const struct mesh_ui_settings *s, struct item_list *list) {
    item_toggle(list, "Device metrics", s->device_telemetry_enabled);
    item_seconds(list, "Device interval", s->device_update_interval);
    item_toggle(list, "Environment", s->environment_measurement_enabled);
    item_toggle(list, "Env on screen", s->environment_screen_enabled);
    item_toggle(list, "Env in Fahrenheit", s->environment_display_fahrenheit);
    item_toggle(list, "Air quality", s->air_quality_enabled);
    item_toggle(list, "Power metrics", s->power_measurement_enabled);
}

static void build_section(const struct mesh_ui_settings *settings,
                          const struct mesh_ui_handshake_state *handshake,
                          enum mesh_ui_settings_section section, struct item_list *list) {
    memset(list, 0, sizeof *list);
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
    build_section(settings, handshake, section, &list);
    return list.count;
}

bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           enum mesh_ui_settings_section section, uint32_t row,
                           struct mesh_ui_settings_item *out) {
    if (out == NULL) {
        return false;
    }
    struct item_list list;
    build_section(settings, handshake, section, &list);
    if (row >= list.count) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = list.items[row];
    return true;
}
