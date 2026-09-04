#define _POSIX_C_SOURCE 200809L

#include "mesh/app.h"

#include "mesh/log.h"
#include "mesh/transport/ble.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/preferences.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

static void mesh_app_minui_on_device_selected(void *userdata, const char *identifier) {
    if (userdata == NULL || identifier == NULL || identifier[0] == '\0') {
        return;
    }

    struct mesh_app *app = (struct mesh_app *)userdata;
    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        mesh_log_warn("ui", "BLE transport unavailable for MinUI selection");
        return;
    }

    mesh_log_info("ui", "MinUI selection requested connect to %s", identifier);
    snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s",
             identifier);
    snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device,
             "%s", identifier);
    app->ui_preferences_dirty = true;

    int connect_result = mesh_ble_transport_connect(ble, identifier);
    if (connect_result < 0 && connect_result != -EALREADY) {
        mesh_log_warn("ui", "Failed to connect to %s via MinUI (%d)", identifier, connect_result);
    }
}

/* Button presses arrive here from the evdev reader and go straight into the UI store's
   navigation model; the repaint happens on the store's eventfd in the same loop turn. */
static void mesh_app_on_ui_key(void *userdata, enum mesh_ui_key key) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    mesh_ui_controller_handle_key(&app->ui_controller, key);
}

static uint64_t mesh_app_now_ms(void);

/* ---- settings writes ---------------------------------------------------------------------- */

/* A fresh channel key. getrandom() blocks until the kernel pool is seeded, which on the Brick
   it long since is; anything else is an error we surface rather than a weak key. */
static int mesh_app_random_key(uint8_t *out, size_t len) {
    size_t have = 0U;
    while (have < len) {
        const ssize_t got = getrandom(out + have, len - have, 0U);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        have += (size_t)got;
    }
    return 0;
}

/* Applies one pending edit to the protobuf section a write carries. The reverse of
   mesh_app_flatten_settings(): this is the only place the UI's field ids meet nanopb.
   Returns -EINVAL for a value the radio would not take (a PIN that is not six digits, hex
   that is not a key), -EIO when no random key could be drawn. */
static int mesh_app_apply_setting_edit(struct mesh_admin_request *write,
                                       const struct mesh_ui_setting_edit *edit) {
    meshtastic_User *owner = &write->payload.owner;
    meshtastic_Config_DisplayConfig *display = &write->payload.config.payload_variant.display;
    meshtastic_Config_BluetoothConfig *bluetooth = &write->payload.config.payload_variant.bluetooth;
    meshtastic_ModuleConfig_StoreForwardConfig *sf =
        &write->payload.module_config.payload_variant.store_forward;
    meshtastic_ModuleConfig_TelemetryConfig *telemetry =
        &write->payload.module_config.payload_variant.telemetry;
    meshtastic_ChannelSettings *channel = &write->payload.channel.settings;
    meshtastic_Config_LoRaConfig *lora = &write->payload.config.payload_variant.lora;
    meshtastic_Config_SecurityConfig *security = &write->payload.config.payload_variant.security;
    const bool on = edit->number != 0U;

    switch ((enum mesh_ui_setting_field)edit->field) {
    case MESH_UI_FIELD_USER_LONG_NAME:
        snprintf(owner->long_name, sizeof owner->long_name, "%.*s",
                 (int)(sizeof owner->long_name - 1U), edit->text);
        break;
    case MESH_UI_FIELD_USER_SHORT_NAME:
        snprintf(owner->short_name, sizeof owner->short_name, "%.*s",
                 (int)(sizeof owner->short_name - 1U), edit->text);
        break;
    case MESH_UI_FIELD_USER_LICENSED:
        owner->is_licensed = on;
        break;
    case MESH_UI_FIELD_USER_UNMESSAGEABLE:
        owner->has_is_unmessagable = true;
        owner->is_unmessagable = on;
        break;
    case MESH_UI_FIELD_DISPLAY_SCREEN_ON:
        display->screen_on_secs = edit->number;
        break;
    case MESH_UI_FIELD_DISPLAY_CAROUSEL:
        display->auto_screen_carousel_secs = edit->number;
        break;
    case MESH_UI_FIELD_DISPLAY_COMPASS:
        display->compass_orientation =
            (meshtastic_Config_DisplayConfig_CompassOrientation)edit->number;
        break;
    case MESH_UI_FIELD_DISPLAY_12H:
        display->use_12h_clock = on;
        break;
    case MESH_UI_FIELD_DISPLAY_UNITS:
        display->units = (meshtastic_Config_DisplayConfig_DisplayUnits)edit->number;
        break;
    case MESH_UI_FIELD_DISPLAY_FLIP:
        display->flip_screen = on;
        break;
    case MESH_UI_FIELD_SF_ENABLED:
        sf->enabled = on;
        break;
    case MESH_UI_FIELD_SF_HEARTBEAT:
        sf->heartbeat = on;
        break;
    case MESH_UI_FIELD_SF_SERVER:
        sf->is_server = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_DEVICE:
        telemetry->device_telemetry_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_INTERVAL:
        telemetry->device_update_interval = edit->number;
        break;
    case MESH_UI_FIELD_TELEMETRY_ENVIRONMENT:
        telemetry->environment_measurement_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_ENV_SCREEN:
        telemetry->environment_screen_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT:
        telemetry->environment_display_fahrenheit = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_AIR_QUALITY:
        telemetry->air_quality_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_POWER:
        telemetry->power_measurement_enabled = on;
        break;
    case MESH_UI_FIELD_CHANNEL_NAME:
        snprintf(channel->name, sizeof channel->name, "%.*s", (int)(sizeof channel->name - 1U),
                 edit->text);
        break;
    case MESH_UI_FIELD_CHANNEL_ROLE:
        write->payload.channel.role =
            on ? meshtastic_Channel_Role_SECONDARY : meshtastic_Channel_Role_DISABLED;
        break;
    case MESH_UI_FIELD_CHANNEL_KEY:
        switch ((enum mesh_ui_psk_choice)edit->number) {
        case MESH_UI_PSK_KEEP:
            break;
        case MESH_UI_PSK_DEFAULT:
            channel->psk.size = 1U;
            channel->psk.bytes[0] = 1U;
            break;
        case MESH_UI_PSK_RANDOM_128:
        case MESH_UI_PSK_RANDOM_256: {
            const size_t len = edit->number == MESH_UI_PSK_RANDOM_128 ? 16U : 32U;
            const int result = mesh_app_random_key(channel->psk.bytes, len);
            if (result < 0) {
                mesh_log_error("ui", "No random bytes for a channel key: %d", result);
                return -EIO;
            }
            channel->psk.size = (pb_size_t)len;
            break;
        }
        case MESH_UI_PSK_NONE:
            channel->psk.size = 0U;
            break;
        case MESH_UI_PSK_TYPED: {
            size_t len = 0U;
            if (!mesh_ui_settings_key_parse(edit->text, channel->psk.bytes,
                                            sizeof channel->psk.bytes, &len)) {
                return -EINVAL;
            }
            channel->psk.size = (pb_size_t)len;
            break;
        }
        default:
            return -EINVAL;
        }
        break;
    case MESH_UI_FIELD_CHANNEL_UPLINK:
        channel->uplink_enabled = on;
        break;
    case MESH_UI_FIELD_CHANNEL_DOWNLINK:
        channel->downlink_enabled = on;
        break;
    case MESH_UI_FIELD_CHANNEL_POSITION:
        channel->has_module_settings = true;
        channel->module_settings.position_precision = edit->number;
        break;
    case MESH_UI_FIELD_BT_ENABLED:
        bluetooth->enabled = on;
        break;
    case MESH_UI_FIELD_BT_MODE:
        bluetooth->mode = (meshtastic_Config_BluetoothConfig_PairingMode)edit->number;
        break;
    case MESH_UI_FIELD_LORA_REGION:
        lora->region = (meshtastic_Config_LoRaConfig_RegionCode)edit->number;
        break;
    case MESH_UI_FIELD_LORA_USE_PRESET:
        lora->use_preset = on;
        break;
    case MESH_UI_FIELD_LORA_PRESET:
        lora->modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)edit->number;
        break;
    case MESH_UI_FIELD_LORA_BANDWIDTH:
        lora->bandwidth = (uint16_t)edit->number;
        break;
    case MESH_UI_FIELD_LORA_SPREAD:
        lora->spread_factor = edit->number;
        break;
    case MESH_UI_FIELD_LORA_CODING:
        lora->coding_rate = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_LORA_HOPS:
        lora->hop_limit = edit->number;
        break;
    case MESH_UI_FIELD_LORA_TX_ENABLED:
        lora->tx_enabled = on;
        break;
    case MESH_UI_FIELD_LORA_TX_POWER:
        lora->tx_power = (int8_t)(uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_LORA_IGNORE_MQTT:
        lora->ignore_mqtt = on;
        break;
    case MESH_UI_FIELD_LORA_OK_TO_MQTT:
        lora->config_ok_to_mqtt = on;
        break;
    case MESH_UI_FIELD_SECURITY_PRIVATE_KEY:
        switch ((enum mesh_ui_psk_choice)edit->number) {
        case MESH_UI_PSK_KEEP:
            break;
        case MESH_UI_PSK_RANDOM_256: {
            const int result = mesh_app_random_key(security->private_key.bytes, 32U);
            if (result < 0) {
                mesh_log_error("ui", "No random bytes for a private key: %d", result);
                return -EIO;
            }
            /* Curve25519 clamping, as the firmware does for the key it generates itself. */
            security->private_key.bytes[0] &= 248U;
            security->private_key.bytes[31] &= 127U;
            security->private_key.bytes[31] |= 64U;
            security->private_key.size = 32U;
            /* An empty public key makes the firmware derive the matching one. */
            security->public_key.size = 0U;
            memset(security->public_key.bytes, 0, sizeof security->public_key.bytes);
            break;
        }
        case MESH_UI_PSK_TYPED: {
            size_t len = 0U;
            if (!mesh_ui_settings_key_parse(edit->text, security->private_key.bytes,
                                            sizeof security->private_key.bytes, &len) ||
                len != 32U) {
                return -EINVAL;
            }
            security->private_key.size = 32U;
            security->public_key.size = 0U;
            memset(security->public_key.bytes, 0, sizeof security->public_key.bytes);
            break;
        }
        default:
            return -EINVAL;
        }
        break;
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_0:
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_1:
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_2: {
        const unsigned slot = (unsigned)(edit->field - MESH_UI_FIELD_SECURITY_ADMIN_KEY_0);
        meshtastic_Config_SecurityConfig_admin_key_t *key = &security->admin_key[slot];
        switch ((enum mesh_ui_psk_choice)edit->number) {
        case MESH_UI_PSK_KEEP:
            break;
        case MESH_UI_PSK_NONE:
            key->size = 0U;
            break;
        case MESH_UI_PSK_TYPED: {
            size_t len = 0U;
            if (!mesh_ui_settings_key_parse(edit->text, key->bytes, sizeof key->bytes, &len) ||
                len != 32U) {
                return -EINVAL;
            }
            key->size = 32U;
            break;
        }
        default:
            return -EINVAL;
        }
        if (slot + 1U > security->admin_key_count) {
            security->admin_key_count = (pb_size_t)(slot + 1U);
        }
        break;
    }
    case MESH_UI_FIELD_SECURITY_MANAGED:
        security->is_managed = on;
        break;
    case MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL:
        security->admin_channel_enabled = on;
        break;
    case MESH_UI_FIELD_SECURITY_SERIAL:
        security->serial_enabled = on;
        break;
    case MESH_UI_FIELD_SECURITY_DEBUG_LOG:
        security->debug_log_api_enabled = on;
        break;
    case MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY:
        security->packet_signature_policy =
            (meshtastic_Config_SecurityConfig_PacketSignaturePolicy)edit->number;
        break;
    case MESH_UI_FIELD_BT_PIN: {
        if (strlen(edit->text) != 6U) {
            return -EINVAL;
        }
        uint32_t pin = 0U;
        for (const char *c = edit->text; *c != '\0'; ++c) {
            if (*c < '0' || *c > '9') {
                return -EINVAL;
            }
            pin = pin * 10U + (uint32_t)(*c - '0');
        }
        bluetooth->fixed_pin = pin;
        break;
    }
    default:
        mesh_log_warn("ui", "Ignoring edit to unknown settings field %u", (unsigned)edit->field);
        break;
    }
    return 0;
}

/* Builds the set_* for a section from what the radio last reported plus the edits. The
   firmware replaces the whole section, so the base must be the radio's own copy: -ENOENT
   when that has not arrived yet, -ENOTSUP for a section this phase does not write. */
int mesh_app_build_settings_write(const struct mesh_radio_settings *radio,
                                  const struct mesh_ui_action *action,
                                  struct mesh_admin_request *out) {
    if (radio == NULL || action == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);
    switch ((enum mesh_ui_settings_section)action->section) {
    case MESH_UI_SETTINGS_USER:
        if (!radio->has_owner) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_OWNER;
        out->payload.owner = radio->owner;
        break;
    case MESH_UI_SETTINGS_DISPLAY:
        if (!radio->has_display) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CONFIG;
        out->type = meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG;
        out->payload.config.which_payload_variant = meshtastic_Config_display_tag;
        out->payload.config.payload_variant.display = radio->display;
        break;
    case MESH_UI_SETTINGS_STORE_FORWARD:
        if (!radio->has_store_forward) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_MODULE_CONFIG;
        out->type = meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG;
        out->payload.module_config.which_payload_variant =
            meshtastic_ModuleConfig_store_forward_tag;
        out->payload.module_config.payload_variant.store_forward = radio->store_forward;
        break;
    case MESH_UI_SETTINGS_TELEMETRY:
        if (!radio->has_telemetry) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_MODULE_CONFIG;
        out->type = meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG;
        out->payload.module_config.which_payload_variant = meshtastic_ModuleConfig_telemetry_tag;
        out->payload.module_config.payload_variant.telemetry = radio->telemetry;
        break;
    case MESH_UI_SETTINGS_BLUETOOTH:
        if (!radio->has_bluetooth) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CONFIG;
        out->type = meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG;
        out->payload.config.which_payload_variant = meshtastic_Config_bluetooth_tag;
        out->payload.config.payload_variant.bluetooth = radio->bluetooth;
        break;
    case MESH_UI_SETTINGS_LORA:
        if (!radio->has_lora) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CONFIG;
        out->type = meshtastic_AdminMessage_ConfigType_LORA_CONFIG;
        out->payload.config.which_payload_variant = meshtastic_Config_lora_tag;
        out->payload.config.payload_variant.lora = radio->lora;
        break;
    case MESH_UI_SETTINGS_SECURITY:
        if (!radio->has_security) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CONFIG;
        out->type = meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG;
        out->payload.config.which_payload_variant = meshtastic_Config_security_tag;
        out->payload.config.payload_variant.security = radio->security;
        break;
    case MESH_UI_SETTINGS_CHANNELS:
        if (action->channel >= MESH_RADIO_SETTINGS_MAX_CHANNELS ||
            !radio->has_channel[action->channel]) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CHANNEL;
        out->type = action->channel;
        out->payload.channel = radio->channels[action->channel];
        out->payload.channel.index = (int8_t)action->channel;
        out->payload.channel.has_settings = true;
        break;
    default:
        return -ENOTSUP;
    }
    for (uint8_t i = 0; i < action->edit_count && i < MESH_UI_SETTINGS_EDITS_MAX; ++i) {
        if (mesh_ui_settings_field_section((enum mesh_ui_setting_field)action->edits[i].field) !=
            (enum mesh_ui_settings_section)action->section) {
            continue; /* an edit from another section has no business in this write */
        }
        const int result = mesh_app_apply_setting_edit(out, &action->edits[i]);
        if (result < 0) {
            return result;
        }
    }
    if (out->kind == MESH_ADMIN_SET_CONFIG &&
        out->payload.config.which_payload_variant == meshtastic_Config_security_tag) {
        /* admin_key is a repeated field: close the gaps a cleared slot leaves. */
        meshtastic_Config_SecurityConfig *security = &out->payload.config.payload_variant.security;
        pb_size_t kept = 0U;
        for (pb_size_t i = 0; i < security->admin_key_count && i < 3U; ++i) {
            if (security->admin_key[i].size == 0U) {
                continue;
            }
            if (kept != i) {
                security->admin_key[kept] = security->admin_key[i];
            }
            kept++;
        }
        for (pb_size_t i = kept; i < 3U; ++i) {
            memset(&security->admin_key[i], 0, sizeof security->admin_key[i]);
        }
        security->admin_key_count = kept;
    }
    return 0;
}

static void mesh_app_save_settings(struct mesh_app *app, struct mesh_transport *ble,
                                   const struct mesh_ui_action *action, uint64_t now) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    char section_label[MESH_UI_SETTINGS_LABEL_MAX];
    if ((enum mesh_ui_settings_section)action->section == MESH_UI_SETTINGS_CHANNELS &&
        action->channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        snprintf(section_label, sizeof section_label, "Channel %u", (unsigned)action->channel);
    } else {
        snprintf(section_label, sizeof section_label, "%s",
                 mesh_ui_settings_section_name((enum mesh_ui_settings_section)action->section));
    }
    const char *section_name = section_label;
    struct mesh_admin_request write;
    int result = mesh_app_build_settings_write(mesh_ble_transport_settings(ble), action, &write);
    if (result == 0) {
        result = mesh_ble_transport_write_settings(ble, &write);
    }
    if (result > 0) {
        const struct mesh_radio_settings *radio = mesh_ble_transport_settings(ble);
        app->settings_save_pending = true;
        app->settings_writes_acked_seen = radio != NULL ? radio->writes_acked : 0U;
        app->settings_writes_failed_seen = radio != NULL ? radio->writes_failed : 0U;
        snprintf(app->settings_save_section, sizeof app->settings_save_section, "%s", section_name);
        mesh_ui_store_settings_edits_clear(&app->ui_store);
        snprintf(toast, sizeof toast, "Saving %s...", section_name);
        mesh_log_info("ui", "Saving %s: %u edits, %d admin requests", section_name,
                      (unsigned)action->edit_count, result);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", "Not connected to a node; edits kept");
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s not loaded yet; X to refresh", section_name);
    } else if (result == -ENOTSUP) {
        snprintf(toast, sizeof toast, "%s is read-only for now", section_name);
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", "Invalid value (PIN is 6 digits, key is hex)");
    } else {
        snprintf(toast, sizeof toast, "Save failed (%d); edits kept", result);
        mesh_log_warn("ui", "Saving %s failed: %d", section_name, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/* Announces the outcome of a save once: the ack, the rejection, or the radio dropping the
   link to reboot with the new settings (most sections do; auto-connect brings it back). */
static void mesh_app_track_settings_save(struct mesh_app *app,
                                         const struct mesh_radio_settings *radio,
                                         bool link_connected) {
    if (!app->settings_save_pending) {
        return;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = mesh_app_now_ms();
    if (radio != NULL && radio->writes_failed > app->settings_writes_failed_seen) {
        switch (radio->last_write_error) {
        case meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY:
            snprintf(toast, sizeof toast, "%s rejected: session expired, try again",
                     app->settings_save_section);
            break;
        case meshtastic_Routing_Error_BAD_REQUEST:
            snprintf(toast, sizeof toast, "%s rejected by the radio (bad value)",
                     app->settings_save_section);
            break;
        case MESH_RADIO_SETTINGS_WRITE_TIMEOUT:
            snprintf(toast, sizeof toast, "No reply saving %s; X to check",
                     app->settings_save_section);
            break;
        default:
            snprintf(toast, sizeof toast, "%s rejected (error %d)", app->settings_save_section,
                     (int)radio->last_write_error);
            break;
        }
        mesh_log_warn("ui", "Save of %s failed: error %d", app->settings_save_section,
                      (int)radio->last_write_error);
    } else if (radio != NULL && radio->writes_acked > app->settings_writes_acked_seen) {
        snprintf(toast, sizeof toast, "%s saved; radio may restart", app->settings_save_section);
        mesh_log_info("ui", "Save of %s acknowledged", app->settings_save_section);
    } else if (!link_connected) {
        snprintf(toast, sizeof toast, "%s", "Radio restarting to apply; reconnecting");
        mesh_log_info("ui", "Link dropped while saving %s; assuming reboot",
                      app->settings_save_section);
    } else {
        return; /* still waiting */
    }
    app->settings_save_pending = false;
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/* What the navigation model cannot do by itself: talk to the radio. */
static void mesh_app_on_ui_action(void *userdata, const struct mesh_ui_action *action) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL || action == NULL) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = mesh_app_now_ms();

    switch (action->type) {
    case MESH_UI_ACTION_CONNECT: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        mesh_log_info("ui", "Connect to %s requested from the device", action->identifier);
        /* Same bookkeeping as a MinUI pick: this becomes the node auto-connect goes back to. */
        snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s",
                 action->identifier);
        snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device,
                 "%s", action->identifier);
        app->ui_preferences_dirty = true;

        /* A user pick beats whatever auto-connect is doing or has done. */
        if (mesh_ble_transport_connected_address(ble) != NULL ||
            mesh_ble_transport_is_connecting(ble)) {
            mesh_ble_transport_disconnect(ble);
        }
        const int result = mesh_ble_transport_connect(ble, action->identifier);
        if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
            snprintf(toast, sizeof toast, "Connecting to %.40s", action->identifier);
        } else {
            snprintf(toast, sizeof toast, "Connect failed (%d)", result);
            mesh_log_warn("ui", "Connect to %s failed: %d", action->identifier, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SEND_TEXT: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        const bool broadcast = (action->dest == MESH_MESSAGE_BROADCAST_ADDR);
        uint32_t packet_id = 0U;
        const int result = mesh_ble_transport_send_text(ble, action->dest, action->channel,
                                                        action->text, !broadcast, &packet_id);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Sent to %s", app->ui_store.nav.target_name);
            mesh_log_info("ui", "Sent \"%s\" to %s (packet %u)", action->text,
                          app->ui_store.nav.target_name, packet_id);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else {
            snprintf(toast, sizeof toast, "Send failed (%d)", result);
            mesh_log_warn("ui", "Send to %s failed: %d", app->ui_store.nav.target_name, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REFRESH_SETTINGS: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        const int result = mesh_ble_transport_refresh_settings(ble);
        if (result > 0) {
            snprintf(toast, sizeof toast, "Refreshing %d settings sections", result);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Refresh already in progress");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else {
            snprintf(toast, sizeof toast, "Refresh failed (%d)", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SAVE_SETTINGS: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        mesh_app_save_settings(app, ble, action, now);
        return;
    }
    case MESH_UI_ACTION_NONE:
    default:
        return;
    }
}

static bool mesh_app_select_minui(struct mesh_app *app, const struct mesh_ui_backend **backend,
                                  void **userdata, bool log_on_missing) {
    if (!mesh_ui_backend_minui_is_available()) {
        if (log_on_missing) {
            mesh_log_warn("ui", "MinUI helpers not found; falling back to CLI backend");
        }
        return false;
    }

    if (backend != NULL) {
        *backend = mesh_ui_backend_minui();
    }
    if (userdata != NULL) {
        *userdata = &app->ui_minui_context;
    }
    app->ui_minui_context.loop = &app->loop;
    app->ui_minui_context.on_device_selected = mesh_app_minui_on_device_selected;
    app->ui_minui_context.callback_userdata = app;
    return true;
}

static void mesh_app_select_cli(struct mesh_app *app, const struct mesh_ui_backend **backend,
                                void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_cli();
    }
    if (userdata != NULL) {
        *userdata = &app->ui_cli_context;
    }
}

static void mesh_app_select_stub(const struct mesh_ui_backend **backend, void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_stub();
    }
    if (userdata != NULL) {
        *userdata = NULL;
    }
}

static bool mesh_app_select_fb(struct mesh_app *app, const struct mesh_ui_backend **backend,
                               void **userdata) {
    if (!mesh_ui_backend_fb_is_available()) {
        return false;
    }

    if (backend != NULL) {
        *backend = mesh_ui_backend_fb();
    }
    if (userdata != NULL) {
        app->ui_fb_context.loop = &app->loop;
        *userdata = &app->ui_fb_context;
    }
    return true;
}

static const struct mesh_ui_backend *mesh_app_select_backend(struct mesh_app *app,
                                                             void **userdata) {
    if (userdata != NULL) {
        *userdata = NULL;
    }

    const char *requested = getenv("MESHCLIENT_UI_BACKEND");
    if (requested != NULL && requested[0] == '\0') {
        requested = NULL;
    }

    const struct mesh_ui_backend *backend = NULL;
    void *backend_userdata = NULL;

    if (requested != NULL) {
        if (strcasecmp(requested, "minui") == 0) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, true) &&
                !mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else if (strcasecmp(requested, "fb") == 0) {
            if (!mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else if (strcasecmp(requested, "cli") == 0) {
            mesh_app_select_cli(app, &backend, &backend_userdata);
        } else if (strcasecmp(requested, "stub") == 0) {
            mesh_app_select_stub(&backend, &backend_userdata);
        } else if (strcasecmp(requested, "auto") == 0) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, false) &&
                !mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else {
            mesh_log_warn("ui", "Unknown UI backend '%s'; defaulting to CLI", requested);
            mesh_app_select_cli(app, &backend, &backend_userdata);
        }
    } else {
        const char *platform = getenv("PLATFORM");
        bool prefer_minui = (platform != NULL && strcasecmp(platform, "tg5040") == 0);

        if (prefer_minui) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, false) &&
                !mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else {
            if (!mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        }
    }

    if (backend == NULL) {
        mesh_app_select_cli(app, &backend, &backend_userdata);
    }

    if (userdata != NULL) {
        *userdata = backend_userdata;
    }

    return backend;
}

/* Resolves a node number to something a human can read, preferring the short name the NodeDB
   gave us and falling back to the Meshtastic-style "!hex" id. */
static void mesh_app_format_peer_name(const struct mesh_ble_handshake_status *status,
                                      uint32_t node_id, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }

    if (node_id == MESH_MESSAGE_BROADCAST_ADDR) {
        snprintf(out, out_len, "all");
        return;
    }

    if (status != NULL) {
        for (size_t i = 0; i < status->node_count && i < MESH_BLE_MAX_NODE_SUMMARY; ++i) {
            if (status->nodes[i].node_id != node_id) {
                continue;
            }
            if (status->nodes[i].short_name[0] != '\0') {
                snprintf(out, out_len, "%s", status->nodes[i].short_name);
                return;
            }
            if (status->nodes[i].long_name[0] != '\0') {
                snprintf(out, out_len, "%s", status->nodes[i].long_name);
                return;
            }
            break;
        }
    }

    snprintf(out, out_len, "!%08x", node_id);
}

/* Lower is more important; see the ranking comment in mesh_app_publish_ui_state(). */
static unsigned mesh_app_node_rank(const struct mesh_ble_node_summary *node, uint32_t my_node,
                                   const struct mesh_message_log *log) {
    if (my_node != 0U && node->node_id == my_node) {
        return 0U;
    }
    if (log != NULL) {
        for (size_t i = 0; i < log->count; ++i) {
            const struct mesh_message *message = mesh_message_log_at(log, i);
            if (message == NULL) {
                continue;
            }
            if (message->from == node->node_id || message->to == node->node_id) {
                return 1U;
            }
        }
    }
    return node->via_mqtt ? 3U : 2U;
}

/* Copies the newest MESH_UI_MAX_MESSAGES entries out of the transport ring into the store,
   merged with whatever history was restored from the cache at startup. */
static void mesh_app_publish_messages(struct mesh_app *app, struct mesh_transport *ble,
                                      const struct mesh_ble_handshake_status *status) {
    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL) {
        return;
    }

    struct mesh_ui_message_list live;
    memset(&live, 0, sizeof(live));
    live.dropped = log->dropped;

    /* The ring holds more than the UI carries; take the newest tail of it. */
    size_t first = (log->count > MESH_UI_MAX_MESSAGES) ? log->count - MESH_UI_MAX_MESSAGES : 0U;
    for (size_t i = first; i < log->count; ++i) {
        const struct mesh_message *source = mesh_message_log_at(log, i);
        if (source == NULL) {
            continue;
        }

        struct mesh_ui_message *target = &live.entries[live.count];
        const bool outbound = (source->direction == MESH_MESSAGE_OUTBOUND);
        target->packet_id = source->packet_id;
        target->peer = outbound ? source->to : source->from;
        target->rx_time = source->rx_time;
        target->channel = source->channel;
        target->direction = source->direction;
        target->ack = source->ack;
        target->broadcast = (source->to == MESH_MESSAGE_BROADCAST_ADDR);
        mesh_app_format_peer_name(status, target->peer, target->peer_name,
                                  sizeof(target->peer_name));
        snprintf(target->text, sizeof(target->text), "%s", source->text);
        live.count++;
    }

    struct mesh_ui_message_list list;
    mesh_ui_message_list_merge(&app->ui_messages_cached, &live, &list);

    mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
    mesh_ui_store_set_messages(&app->ui_store, &list);
    if (app->ui_handshake_cache_path[0] != '\0' &&
        (app->ui_store.pending_flags & MESH_UI_UPDATE_MESSAGES) != 0U &&
        (prev_flags & MESH_UI_UPDATE_MESSAGES) == 0U) {
        app->ui_handshake_cache_dirty = true;
    }
}

/* Flattens the transport's protobuf-typed view into the UI's plain struct. */
static void mesh_app_flatten_settings(const struct mesh_radio_settings *src,
                                      struct mesh_ui_settings *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL) {
        return;
    }
    dst->loaded = mesh_radio_settings_loaded(src);
    dst->admin_ok = src->admin_replies > 0U;
    dst->admin_busy = mesh_radio_settings_busy(src) || src->queue_len > 0U;
    dst->write_pending = mesh_radio_settings_write_pending(src);
    dst->admin_replies = src->admin_replies;

    if (src->has_owner) {
        dst->has_owner = true;
        snprintf(dst->long_name, sizeof dst->long_name, "%s", src->owner.long_name);
        snprintf(dst->short_name, sizeof dst->short_name, "%s", src->owner.short_name);
        dst->is_licensed = src->owner.is_licensed;
        dst->is_unmessagable = src->owner.has_is_unmessagable && src->owner.is_unmessagable;
    }
    if (src->has_device) {
        dst->has_device = true;
        dst->role = (uint8_t)src->device.role;
        dst->rebroadcast_mode = (uint8_t)src->device.rebroadcast_mode;
        snprintf(dst->tzdef, sizeof dst->tzdef, "%s", src->device.tzdef);
        dst->led_heartbeat_disabled = src->device.led_heartbeat_disabled;
        dst->double_tap_as_button_press = src->device.double_tap_as_button_press;
    }
    if (src->has_display) {
        dst->has_display = true;
        dst->screen_on_secs = src->display.screen_on_secs;
        dst->carousel_secs = src->display.auto_screen_carousel_secs;
        dst->compass_orientation = (uint8_t)src->display.compass_orientation;
        dst->use_12h_clock = src->display.use_12h_clock;
        dst->units = (uint8_t)src->display.units;
        dst->flip_screen = src->display.flip_screen;
    }
    if (src->has_lora) {
        dst->has_lora = true;
        dst->use_preset = src->lora.use_preset;
        dst->modem_preset = (uint8_t)src->lora.modem_preset;
        dst->region = (uint8_t)src->lora.region;
        dst->bandwidth = src->lora.bandwidth;
        dst->spread_factor = src->lora.spread_factor;
        dst->coding_rate = src->lora.coding_rate;
        dst->hop_limit = (uint8_t)src->lora.hop_limit;
        dst->tx_enabled = src->lora.tx_enabled;
        dst->tx_power = (int8_t)src->lora.tx_power;
        dst->ignore_mqtt = src->lora.ignore_mqtt;
        dst->config_ok_to_mqtt = src->lora.config_ok_to_mqtt;
    }
    if (src->has_bluetooth) {
        dst->has_bluetooth = true;
        dst->bluetooth_enabled = src->bluetooth.enabled;
        dst->pairing_mode = (uint8_t)src->bluetooth.mode;
        dst->fixed_pin = src->bluetooth.fixed_pin;
    }
    if (src->has_security) {
        dst->has_security = true;
        size_t key_len = src->security.public_key.size;
        if (key_len > sizeof dst->public_key) {
            key_len = sizeof dst->public_key;
        }
        memcpy(dst->public_key, src->security.public_key.bytes, key_len);
        dst->public_key_len = (uint8_t)key_len;
        dst->has_private_key = src->security.private_key.size > 0U;
        size_t private_len = src->security.private_key.size;
        if (private_len > sizeof dst->private_key) {
            private_len = sizeof dst->private_key;
        }
        memcpy(dst->private_key, src->security.private_key.bytes, private_len);
        dst->private_key_len = (uint8_t)private_len;
        dst->admin_key_count = (uint8_t)src->security.admin_key_count;
        for (unsigned i = 0; i < 3U && i < src->security.admin_key_count; ++i) {
            size_t len = src->security.admin_key[i].size;
            if (len > sizeof dst->admin_keys[i]) {
                len = sizeof dst->admin_keys[i];
            }
            memcpy(dst->admin_keys[i], src->security.admin_key[i].bytes, len);
            dst->admin_key_lens[i] = (uint8_t)len;
        }
        dst->is_managed = src->security.is_managed;
        dst->serial_enabled = src->security.serial_enabled;
        dst->debug_log_api_enabled = src->security.debug_log_api_enabled;
        dst->admin_channel_enabled = src->security.admin_channel_enabled;
        dst->packet_signature_policy = (uint8_t)src->security.packet_signature_policy;
    }
    if (src->has_position) {
        dst->has_position = true;
        dst->gps_mode = (uint8_t)src->position.gps_mode;
        dst->position_broadcast_secs = src->position.position_broadcast_secs;
        dst->position_broadcast_smart_enabled = src->position.position_broadcast_smart_enabled;
        dst->fixed_position = src->position.fixed_position;
    }
    if (src->has_power) {
        dst->has_power = true;
        dst->is_power_saving = src->power.is_power_saving;
        dst->ls_secs = src->power.ls_secs;
        dst->min_wake_secs = src->power.min_wake_secs;
        dst->on_battery_shutdown_after_secs = src->power.on_battery_shutdown_after_secs;
    }
    if (src->has_mqtt) {
        dst->has_mqtt = true;
        dst->mqtt_enabled = src->mqtt.enabled;
        snprintf(dst->mqtt_address, sizeof dst->mqtt_address, "%s", src->mqtt.address);
        snprintf(dst->mqtt_root, sizeof dst->mqtt_root, "%s", src->mqtt.root);
        dst->mqtt_encryption_enabled = src->mqtt.encryption_enabled;
        dst->mqtt_tls_enabled = src->mqtt.tls_enabled;
        dst->mqtt_proxy_to_client_enabled = src->mqtt.proxy_to_client_enabled;
    }
    if (src->has_store_forward) {
        dst->has_store_forward = true;
        dst->store_forward_enabled = src->store_forward.enabled;
        dst->store_forward_heartbeat = src->store_forward.heartbeat;
        dst->store_forward_is_server = src->store_forward.is_server;
    }
    if (src->has_telemetry) {
        dst->has_telemetry = true;
        dst->device_update_interval = src->telemetry.device_update_interval;
        dst->device_telemetry_enabled = src->telemetry.device_telemetry_enabled;
        dst->environment_measurement_enabled = src->telemetry.environment_measurement_enabled;
        dst->environment_screen_enabled = src->telemetry.environment_screen_enabled;
        dst->environment_display_fahrenheit = src->telemetry.environment_display_fahrenheit;
        dst->air_quality_enabled = src->telemetry.air_quality_enabled;
        dst->power_measurement_enabled = src->telemetry.power_measurement_enabled;
    }
    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS && i < MESH_UI_MAX_CHANNELS; ++i) {
        if (!src->has_channel[i]) {
            continue;
        }
        const meshtastic_Channel *channel = &src->channels[i];
        struct mesh_ui_channel_detail *detail = &dst->channels[i];
        dst->has_channels = true;
        detail->present = true;
        detail->index = (uint8_t)i;
        detail->role = (uint8_t)channel->role;
        if (channel->has_settings) {
            snprintf(detail->name, sizeof detail->name, "%s", channel->settings.name);
            size_t psk_len = channel->settings.psk.size;
            if (psk_len > sizeof detail->psk) {
                psk_len = sizeof detail->psk;
            }
            memcpy(detail->psk, channel->settings.psk.bytes, psk_len);
            detail->psk_len = (uint8_t)psk_len;
            detail->uplink_enabled = channel->settings.uplink_enabled;
            detail->downlink_enabled = channel->settings.downlink_enabled;
            detail->position_precision = channel->settings.has_module_settings
                                             ? channel->settings.module_settings.position_precision
                                             : 0U;
        }
    }
    if (src->has_metadata) {
        dst->has_metadata = true;
        snprintf(dst->firmware_version, sizeof dst->firmware_version, "%s",
                 src->metadata.firmware_version);
        dst->hw_model = (uint32_t)src->metadata.hw_model;
        dst->has_wifi = src->metadata.hasWifi;
        dst->has_bluetooth_radio = src->metadata.hasBluetooth;
        dst->has_ethernet = src->metadata.hasEthernet;
        dst->has_pkc = src->metadata.hasPKC;
        dst->can_shutdown = src->metadata.canShutdown;
    }
}

static void mesh_app_publish_ui_state(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_ui_store_tick(&app->ui_store, mesh_app_now_ms());

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        mesh_ui_store_set_transport_status(&app->ui_store, "unavailable");
        return;
    }

    const char *transport_status =
        (ble->ops != NULL && ble->ops->status != NULL) ? ble->ops->status(ble) : NULL;
    mesh_ui_store_set_transport_status(&app->ui_store,
                                       transport_status != NULL ? transport_status : "unknown");

    struct mesh_bluez_device_info ble_devices[MESH_UI_MAX_DEVICES];
    size_t device_count = mesh_ble_transport_get_devices(ble, ble_devices, MESH_UI_MAX_DEVICES);

    struct mesh_ui_device ui_devices[MESH_UI_MAX_DEVICES];
    memset(ui_devices, 0, sizeof(ui_devices));

    const char *connected_address = mesh_ble_transport_connected_address(ble);
    bool connected_address_seen = false;

    /* Announce a dropped link once; auto-connect brings it back and the footer tracks it. */
    const bool link_connected = (connected_address != NULL && connected_address[0] != '\0');
    if (app->ui_link_was_connected && !link_connected &&
        app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
        mesh_ui_store_set_toast(&app->ui_store, mesh_app_now_ms(), "Radio link lost; reconnecting");
    }
    app->ui_link_was_connected = link_connected;

    for (size_t i = 0; i < device_count && i < MESH_UI_MAX_DEVICES; ++i) {
        snprintf(ui_devices[i].identifier, sizeof(ui_devices[i].identifier), "%s",
                 ble_devices[i].address);
        snprintf(ui_devices[i].name, sizeof(ui_devices[i].name), "%s", ble_devices[i].name);
        int16_t rssi = ble_devices[i].rssi;
        if (rssi < INT8_MIN) {
            rssi = INT8_MIN;
        } else if (rssi > INT8_MAX) {
            rssi = INT8_MAX;
        }
        ui_devices[i].rssi = (int8_t)rssi;
        ui_devices[i].connected = (connected_address != NULL && connected_address[0] != '\0' &&
                                   strcmp(connected_address, ble_devices[i].address) == 0);
        if (ui_devices[i].connected) {
            connected_address_seen = true;
        }
    }

    if (connected_address != NULL && connected_address[0] != '\0' && !connected_address_seen &&
        device_count < MESH_UI_MAX_DEVICES) {
        snprintf(ui_devices[device_count].identifier, sizeof(ui_devices[device_count].identifier),
                 "%s", connected_address);
        snprintf(ui_devices[device_count].name, sizeof(ui_devices[device_count].name), "%s",
                 "Connected");
        ui_devices[device_count].rssi = 0;
        ui_devices[device_count].connected = true;
        ++device_count;
    }

    mesh_ui_store_set_discovery(&app->ui_store, ui_devices, device_count);

    bool preferences_modified = false;
    if (connected_address != NULL && connected_address[0] != '\0') {
        if (strcmp(app->ui_preferences.preferred_device, connected_address) != 0) {
            snprintf(app->ui_preferences.preferred_device,
                     sizeof app->ui_preferences.preferred_device, "%s", connected_address);
            preferences_modified = true;
        }
    }

    struct mesh_ble_handshake_status status = mesh_ble_transport_handshake_status(ble);
    const bool handshake_active = status.request_in_flight || status.config_complete ||
                                  status.has_my_info || status.has_config ||
                                  (status.node_count > 0U);

    if (handshake_active) {
        struct mesh_ui_handshake_state ui_handshake;
        memset(&ui_handshake, 0, sizeof(ui_handshake));
        ui_handshake.request_in_flight = status.request_in_flight;
        ui_handshake.request_id = status.request_id;
        ui_handshake.config_complete = status.config_complete;
        ui_handshake.config_complete_id = status.config_complete_id;
        ui_handshake.has_my_info = status.has_my_info;
        ui_handshake.has_config = status.has_config;
        ui_handshake.cached = false;
        if (status.has_my_info) {
            const uint32_t my_node = status.my_info.my_node_num;
            ui_handshake.my_info.node_num = status.my_info.my_node_num;
            ui_handshake.my_info.nodedb_entries = status.my_info.nodedb_count;
            ui_handshake.my_info.reboot_count = status.my_info.reboot_count;
            for (size_t i = 0; i < status.node_count && i < MESH_BLE_MAX_NODE_SUMMARY; ++i) {
                if (status.nodes[i].node_id == my_node && status.nodes[i].short_name[0] != '\0') {
                    snprintf(ui_handshake.my_short_name, sizeof(ui_handshake.my_short_name), "%s",
                             status.nodes[i].short_name);
                    break;
                }
            }
        }

        /* The UI carries fewer nodes than a real mesh has. Rank them so the ones that matter
           survive the cut: ourselves, then anyone we have exchanged messages with, then nodes
           heard directly over RF by last_heard, then MQTT-fed nodes by last_heard. On a mesh
           with an MQTT uplink dozens of far-away nodes are "heard" every minute and would
           otherwise push the radio you are actually talking to off the list. Insertion sort:
           MESH_BLE_MAX_NODE_SUMMARY is small and this runs once per publish. */
        const struct mesh_message_log *message_log = mesh_ble_transport_messages(ble);
        size_t order[MESH_BLE_MAX_NODE_SUMMARY];
        unsigned rank[MESH_BLE_MAX_NODE_SUMMARY];
        size_t total = status.node_count > MESH_BLE_MAX_NODE_SUMMARY ? MESH_BLE_MAX_NODE_SUMMARY
                                                                     : status.node_count;
        const uint32_t my_node = status.has_my_info ? status.my_info.my_node_num : 0U;
        for (size_t i = 0; i < total; ++i) {
            const struct mesh_ble_node_summary *node = &status.nodes[i];
            rank[i] = mesh_app_node_rank(node, my_node, message_log);
            size_t j = i;
            while (j > 0U) {
                const size_t prev_index = order[j - 1U];
                const struct mesh_ble_node_summary *prev = &status.nodes[prev_index];
                if (rank[prev_index] < rank[i] ||
                    (rank[prev_index] == rank[i] && prev->last_heard >= node->last_heard)) {
                    break;
                }
                order[j] = prev_index;
                --j;
            }
            order[j] = i;
        }

        size_t copy_count = total;
        if (copy_count > MESH_UI_MAX_HANDSHAKE_NODES) {
            copy_count = MESH_UI_MAX_HANDSHAKE_NODES;
        }
        for (size_t i = 0; i < copy_count; ++i) {
            const struct mesh_ble_node_summary *src = &status.nodes[order[i]];
            struct mesh_ui_node_summary *dst = &ui_handshake.nodes[i];
            dst->node_id = src->node_id;
            snprintf(dst->long_name, sizeof(dst->long_name), "%s", src->long_name);
            snprintf(dst->short_name, sizeof(dst->short_name), "%s", src->short_name);
            dst->last_heard = src->last_heard;
            dst->snr = src->snr;
            dst->via_mqtt = src->via_mqtt;
            dst->has_hops_away = src->has_hops_away;
            dst->hops_away = src->hops_away;
        }
        ui_handshake.node_count = (uint32_t)copy_count;

        size_t channel_count = status.channel_count;
        if (channel_count > MESH_UI_MAX_CHANNELS) {
            channel_count = MESH_UI_MAX_CHANNELS;
        }
        for (size_t i = 0; i < channel_count; ++i) {
            ui_handshake.channels[i].index = status.channels[i].index;
            ui_handshake.channels[i].role = status.channels[i].role;
            ui_handshake.channels[i].psk_len = status.channels[i].psk_len;
            ui_handshake.channels[i].uplink_enabled = status.channels[i].uplink_enabled;
            ui_handshake.channels[i].downlink_enabled = status.channels[i].downlink_enabled;
            ui_handshake.channels[i].position_precision = status.channels[i].position_precision;
            snprintf(ui_handshake.channels[i].name, sizeof(ui_handshake.channels[i].name), "%s",
                     status.channels[i].name);
            if (status.channels[i].role == 1U /* PRIMARY */) {
                snprintf(ui_handshake.primary_channel, sizeof(ui_handshake.primary_channel), "%s",
                         status.channels[i].name);
            }
        }
        ui_handshake.channel_count = (uint32_t)channel_count;

        mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
        mesh_ui_store_set_handshake(&app->ui_store, &ui_handshake);
        if (app->ui_handshake_cache_path[0] != '\0' &&
            (app->ui_store.pending_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U &&
            (prev_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U) {
            app->ui_handshake_cache_dirty = true;
        }

        if (ui_handshake.primary_channel[0] != '\0' &&
            strcmp(app->ui_preferences.preferred_channel, ui_handshake.primary_channel) != 0) {
            snprintf(app->ui_preferences.preferred_channel,
                     sizeof app->ui_preferences.preferred_channel, "%s",
                     ui_handshake.primary_channel);
            preferences_modified = true;
        }
    } else {
        mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
        mesh_ui_store_set_handshake(&app->ui_store, NULL);
        if (app->ui_handshake_cache_path[0] != '\0' &&
            (app->ui_store.pending_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U &&
            (prev_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U) {
            app->ui_handshake_cache_dirty = true;
        }
    }

    mesh_app_publish_messages(app, ble, &status);

    const struct mesh_radio_settings *radio_settings = mesh_ble_transport_settings(ble);
    struct mesh_ui_settings ui_settings;
    mesh_app_flatten_settings(radio_settings, &ui_settings);
    mesh_ui_store_set_settings(&app->ui_store, &ui_settings);
    mesh_app_track_settings_save(app, radio_settings, link_connected);

    if (preferences_modified && app->ui_preferences_path[0] != '\0') {
        if (mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path) == 0) {
            app->ui_preferences_dirty = false;
        } else {
            app->ui_preferences_dirty = true;
        }
    } else if (app->ui_preferences_dirty && app->ui_preferences_path[0] != '\0') {
        if (mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path) == 0) {
            app->ui_preferences_dirty = false;
        }
    }

    if (app->ui_handshake_cache_dirty && app->ui_handshake_cache_path[0] != '\0') {
        int save_handshake = mesh_ui_store_save(&app->ui_store, app->ui_handshake_cache_path);
        if (save_handshake == 0) {
            app->ui_handshake_cache_dirty = false;
        } else {
            mesh_log_debug("app", "Failed to persist handshake cache: %d", save_handshake);
        }
    }
}

#define MESH_APP_AUTOCONNECT_RETRY_MS 2000U
#define MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS 60000U
/* How long a saved preferred node gets to show up in discovery before another node is used. */
#define MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS 30000U

static uint64_t mesh_app_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* "0", "off", "false" and "no" disable; anything else (including unset) leaves the default. */
static bool mesh_app_env_disabled(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    return strcasecmp(value, "0") == 0 || strcasecmp(value, "off") == 0 ||
           strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0;
}

void mesh_app_autoconnect(struct mesh_app *app) {
    if (app == NULL || app->autoconnect_disabled ||
        app->config.run_mode != MESH_APP_RUN_FOREGROUND) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL || mesh_ble_transport_connected_address(ble) != NULL ||
        mesh_ble_transport_is_connecting(ble)) {
        return;
    }

    uint64_t now = mesh_app_now_ms();
    if (app->autoconnect_started_ms == 0U) {
        app->autoconnect_started_ms = now;
    }
    if (now < app->autoconnect_retry_at_ms) {
        return;
    }

    struct mesh_bluez_device_info devices[MESH_UI_MAX_DEVICES];
    size_t device_count = mesh_ble_transport_get_devices(ble, devices, MESH_UI_MAX_DEVICES);
    if (device_count == 0U) {
        return; /* nothing in range yet; discovery keeps running */
    }

    const struct mesh_bluez_device_info *target = NULL;
    const char *preferred = app->config.preferred_ble_device;
    if (preferred[0] != '\0') {
        for (size_t i = 0; i < device_count; ++i) {
            if (strcasecmp(devices[i].address, preferred) == 0 ||
                strcasecmp(devices[i].name, preferred) == 0) {
                target = &devices[i];
                break;
            }
        }
        if (target == NULL &&
            now - app->autoconnect_started_ms < MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS) {
            if (!app->autoconnect_waiting_logged) {
                mesh_log_info("app", "Preferred device '%s' not in range yet; waiting", preferred);
                app->autoconnect_waiting_logged = true;
            }
            app->autoconnect_retry_at_ms = now + 1000U;
            return;
        }
    }

    if (target == NULL) {
        size_t best = 0U;
        for (size_t i = 1; i < device_count; ++i) {
            if (devices[i].rssi > devices[best].rssi) {
                best = i;
            }
        }
        target = &devices[best];
        mesh_log_info("app", "%s; using strongest node %s (%s, %d dBm)",
                      preferred[0] != '\0' ? "Preferred device not in range"
                                           : "No preferred device saved",
                      target->name, target->address, (int)target->rssi);
    }

    int result = mesh_ble_transport_connect(ble, target->address);
    if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
        if (result == 0) {
            mesh_log_info("app", "Auto-connecting to %s (%s)", target->name, target->address);
        }
        app->autoconnect_failures = 0U;
        app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
        return;
    }
    if (result == -EAGAIN) {
        app->autoconnect_retry_at_ms = now + 1000U; /* transport not READY yet */
        return;
    }

    if (app->autoconnect_failures < 8U) {
        app->autoconnect_failures++;
    }
    uint64_t delay = (uint64_t)MESH_APP_AUTOCONNECT_RETRY_MS << (app->autoconnect_failures - 1U);
    if (delay > MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS) {
        delay = MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS;
    }
    app->autoconnect_retry_at_ms = now + delay;
    mesh_log_warn("app", "Auto-connect to %s failed (%d); retrying in %llu ms", target->address,
                  result, (unsigned long long)delay);
}

int mesh_app_init(struct mesh_app *app, const struct mesh_app_config *config) {
    if (app == NULL) {
        return -EINVAL;
    }

    if (config != NULL) {
        app->config = *config;
    } else {
        app->config = mesh_app_config_default();
        mesh_app_config_apply_env_overrides(&app->config);
    }

    int result = mesh_event_loop_init(&app->loop);
    if (result < 0) {
        mesh_log_error("app", "Event loop init failed: %d", result);
        return result;
    }

    memset(&app->ui_minui_context, 0, sizeof app->ui_minui_context);
    memset(&app->ui_fb_context, 0, sizeof app->ui_fb_context);
    memset(&app->ui_input, 0, sizeof app->ui_input);
    memset(&app->signals, 0, sizeof app->signals);
    app->signals.fd = -1;
    memset(&app->ui_preferences, 0, sizeof(app->ui_preferences));
    app->ui_preferences_path[0] = '\0';
    app->ui_preferences_dirty = false;
    app->ui_handshake_cache_path[0] = '\0';
    app->autoconnect_started_ms = 0U;
    app->autoconnect_retry_at_ms = 0U;
    app->autoconnect_failures = 0U;
    app->autoconnect_waiting_logged = false;
    app->ui_link_was_connected = false;
    app->autoconnect_disabled = mesh_app_env_disabled("MESHCLIENT_AUTOCONNECT");
    if (app->autoconnect_disabled) {
        mesh_log_info("app", "Auto-connect disabled by MESHCLIENT_AUTOCONNECT");
    }
    app->ui_handshake_cache_dirty = false;

    if (mesh_ui_preferences_default_path(app->ui_preferences_path,
                                         sizeof(app->ui_preferences_path)) == 0) {
        int load_result = mesh_ui_preferences_load(&app->ui_preferences, app->ui_preferences_path);
        if (load_result == 0) {
            if (app->config.preferred_ble_device[0] == '\0' &&
                app->ui_preferences.preferred_device[0] != '\0') {
                snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device,
                         "%s", app->ui_preferences.preferred_device);
            }
        }
        int handshake_written =
            snprintf(app->ui_handshake_cache_path, sizeof(app->ui_handshake_cache_path),
                     "%s.handshake", app->ui_preferences_path);
        if (handshake_written < 0 ||
            handshake_written >= (int)sizeof(app->ui_handshake_cache_path)) {
            mesh_log_warn("app", "Handshake cache path truncated; disabling cache");
            app->ui_handshake_cache_path[0] = '\0';
        }
    }

    result = mesh_ui_store_init(&app->ui_store);
    if (result < 0) {
        mesh_log_error("app", "UI store init failed: %d", result);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    if (app->ui_handshake_cache_path[0] != '\0') {
        int handshake_load = mesh_ui_store_load(&app->ui_store, app->ui_handshake_cache_path);
        if (handshake_load < 0 && handshake_load != -ENOENT) {
            mesh_log_debug("app", "Failed to load handshake cache: %d", handshake_load);
        }
    }

    /* Keep the restored conversation aside: every publish merges it back in, so an empty
       transport log at startup never overwrites it. */
    app->ui_messages_cached = app->ui_store.messages;

    void *backend_userdata = NULL;
    const struct mesh_ui_backend *ui_backend = mesh_app_select_backend(app, &backend_userdata);
    result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, ui_backend,
                                     backend_userdata, &app->loop);
    if (result < 0) {
        mesh_log_warn("app", "UI backend init failed (%d); falling back to stub", result);
        result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store,
                                         mesh_ui_backend_stub(), NULL, &app->loop);
    }
    if (result < 0) {
        mesh_log_error("app", "UI controller init failed: %d", result);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }
    mesh_ui_controller_set_action_handler(&app->ui_controller, mesh_app_on_ui_action, app);

    /* Optional canned.txt next to the preferences file replaces the built-in quick replies. */
    if (app->ui_preferences_path[0] != '\0') {
        char canned_path[sizeof app->ui_preferences_path + 16U];
        snprintf(canned_path, sizeof canned_path, "%s", app->ui_preferences_path);
        char *slash = strrchr(canned_path, '/');
        if (slash != NULL) {
            const size_t room = sizeof canned_path - (size_t)(slash + 1 - canned_path);
            snprintf(slash + 1, room, "%s", "canned.txt");
            const int loaded = mesh_ui_canned_load(canned_path);
            if (loaded > 0) {
                mesh_log_info("app", "Loaded %d canned replies from %s", loaded, canned_path);
            } else if (loaded != -ENOENT) {
                mesh_log_warn("app", "Ignoring %s: %d", canned_path, loaded);
            }
        }
    }

    mesh_transport_registry_init(&app->transport_registry);

    result = mesh_transport_registry_register(&app->transport_registry, mesh_ble_transport());
    if (result < 0) {
        mesh_log_error("app", "Failed to register BLE transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    return 0;
}

void mesh_app_shutdown(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    mesh_ui_input_shutdown(&app->ui_input);
    mesh_signals_shutdown(&app->signals);
    mesh_ui_controller_shutdown(&app->ui_controller);
    if (app->ui_handshake_cache_path[0] != '\0') {
        mesh_ui_store_save(&app->ui_store, app->ui_handshake_cache_path);
        app->ui_handshake_cache_dirty = false;
    }
    mesh_ui_store_shutdown(&app->ui_store);
    if (app->ui_preferences_dirty && app->ui_preferences_path[0] != '\0') {
        mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path);
        app->ui_preferences_dirty = false;
    }
    mesh_event_loop_shutdown(&app->loop);
}

int mesh_app_run(struct mesh_app *app) {
    if (app == NULL) {
        return -EINVAL;
    }

    int result =
        mesh_transport_registry_start_all(&app->transport_registry, &app->config, &app->loop);
    if (result < 0) {
        return result;
    }

    /* Only the interactive run takes these over: --status and --list-devices stay plain CLI
       tools that Ctrl-C kills outright. Neither is fatal if it fails - a client that cannot
       read buttons is still better than no client. */
    mesh_signals_init(&app->signals, &app->loop);
    mesh_ui_input_init(&app->ui_input, &app->loop);
    mesh_ui_input_set_handler(&app->ui_input, mesh_app_on_ui_key, app);

    mesh_app_publish_ui_state(app);

    switch (app->config.run_mode) {
    case MESH_APP_RUN_SINGLE_POLL:
        mesh_log_debug("app", "Running single poll with timeout %d ms",
                       app->config.idle_timeout_ms);
        mesh_app_publish_ui_state(app);
        result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (result >= 0) {
            mesh_app_publish_ui_state(app);
        }
        break;
    case MESH_APP_RUN_FOREGROUND:
        mesh_log_info("app", "Starting foreground event loop (timeout %d ms)",
                      app->config.idle_timeout_ms);
        /* Paint the first frame before any transport work: the store already has a refresh
           queued, and a zero timeout drains what is ready without waiting for more. */
        mesh_event_loop_run(&app->loop, 0);
        while (true) {
            mesh_transport_registry_tick(&app->transport_registry);
            mesh_app_autoconnect(app);
            mesh_app_publish_ui_state(app);
            result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
            if (result < 0) {
                break;
            }
            if (app->loop.stop_requested) {
                mesh_log_info("app", "Event loop stop requested");
                break;
            }
            mesh_app_publish_ui_state(app);
        }
        break;
    default:
        mesh_log_warn("app", "Unknown run mode %d, performing single poll", app->config.run_mode);
        mesh_transport_registry_tick(&app->transport_registry);
        mesh_app_publish_ui_state(app);
        result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (result >= 0) {
            mesh_app_publish_ui_state(app);
        }
        break;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    /* Only the transport line changes here. A full publish would see the stopped transport
       report no handshake and no node names, and that empty state is what mesh_app_shutdown()
       would then save as the cache. */
    {
        struct mesh_transport *ble = mesh_ble_transport();
        const char *status = (ble != NULL && ble->ops != NULL && ble->ops->status != NULL)
                                 ? ble->ops->status(ble)
                                 : "stopped";
        mesh_ui_store_set_transport_status(&app->ui_store, status != NULL ? status : "stopped");
    }

    mesh_ui_input_shutdown(&app->ui_input);
    mesh_signals_shutdown(&app->signals);
    return result;
}
