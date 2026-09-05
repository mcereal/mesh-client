#define _POSIX_C_SOURCE 200809L

#include "mesh/core/app.h"
#include "mesh/core/version.h"

#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/preferences.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

/* ---- link routing --------------------------------------------------------------------- */

/*
 * Two links, one session, one radio at a time. The Devices tab lists BLE advertisers and USB
 * ports together, so a connect has to be routed to the transport that owns the row, and taking
 * one link up drops the other.
 */

static struct mesh_transport *mesh_app_transport_for_kind(uint8_t kind) {
    return kind == (uint8_t)MESH_UI_DEVICE_SERIAL ? mesh_serial_transport() : mesh_ble_transport();
}

struct mesh_transport *mesh_app_active_transport(void) {
    struct mesh_transport *serial = mesh_serial_transport();
    struct mesh_transport *ble = mesh_ble_transport();
    if (serial != NULL && mesh_serial_transport_connected_port(serial) != NULL) {
        return serial;
    }
    if (ble != NULL && mesh_ble_transport_connected_address(ble) != NULL) {
        return ble;
    }
    if (serial != NULL && mesh_serial_transport_is_connecting(serial)) {
        return serial;
    }
    if (ble != NULL &&
        (mesh_ble_transport_is_connecting(ble) || mesh_ble_transport_is_pairing(ble))) {
        return ble;
    }
    return ble;
}

const char *mesh_app_connected_identifier(void) {
    const char *port = mesh_serial_transport_connected_port(mesh_serial_transport());
    if (port != NULL && port[0] != '\0') {
        return port;
    }
    const char *address = mesh_ble_transport_connected_address(mesh_ble_transport());
    return (address != NULL && address[0] != '\0') ? address : NULL;
}

bool mesh_app_link_connecting(void) {
    /* Pairing counts: it is the first half of a connect the user asked for, and auto-connect
       taking the serial link up underneath it would leave two transports on one session. */
    return mesh_ble_transport_is_connecting(mesh_ble_transport()) ||
           mesh_ble_transport_is_pairing(mesh_ble_transport()) ||
           mesh_serial_transport_is_connecting(mesh_serial_transport());
}

/* Drops whatever link is up or coming up, except the transport we are about to use. */
static void mesh_app_release_other_link(const struct mesh_transport *keep) {
    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_transport *serial = mesh_serial_transport();
    if (ble != keep &&
        (mesh_ble_transport_connected_address(ble) != NULL ||
         mesh_ble_transport_is_connecting(ble) || mesh_ble_transport_is_pairing(ble))) {
        /* A pairing left running would finish and then connect BLE on top of this link. */
        mesh_ble_transport_disconnect(ble);
    }
    if (serial != keep && (mesh_serial_transport_connected_port(serial) != NULL ||
                           mesh_serial_transport_is_connecting(serial))) {
        mesh_serial_transport_disconnect(serial);
    }
}

/* Connects `identifier` over the transport `kind` names, dropping the other link first.
   Returns what the transport's connect returned. */
static int mesh_app_link_connect(struct mesh_app *app, const char *identifier, uint8_t kind) {
    struct mesh_transport *transport = mesh_app_transport_for_kind(kind);
    if (transport == NULL) {
        return -ENODEV;
    }

    mesh_app_release_other_link(transport);

    /* This becomes the node auto-connect goes back to. The two preferences are kept apart so
       unplugging a USB node does not erase which radio to look for over the air. */
    if (kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
        snprintf(app->config.preferred_serial_device, sizeof app->config.preferred_serial_device,
                 "%s", identifier);
        if (mesh_serial_transport_connected_port(transport) != NULL ||
            mesh_serial_transport_is_connecting(transport)) {
            mesh_serial_transport_disconnect(transport);
        }
        return mesh_serial_transport_connect(transport, identifier);
    }

    snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s",
             identifier);
    if (mesh_ble_transport_connected_address(transport) != NULL ||
        mesh_ble_transport_is_connecting(transport) || mesh_ble_transport_is_pairing(transport)) {
        mesh_ble_transport_disconnect(transport);
    }
    /* A connect the user asked for pairs the node when it needs it; auto-connect's own
       attempts go through mesh_ble_transport_connect() and never raise a PIN prompt. */
    return mesh_ble_transport_connect_and_pair(transport, identifier);
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
    meshtastic_Config_DeviceConfig *device = &write->payload.config.payload_variant.device;
    meshtastic_Config_DisplayConfig *display = &write->payload.config.payload_variant.display;
    meshtastic_Config_BluetoothConfig *bluetooth = &write->payload.config.payload_variant.bluetooth;
    meshtastic_ModuleConfig_MQTTConfig *mqtt = &write->payload.module_config.payload_variant.mqtt;
    meshtastic_ModuleConfig_StoreForwardConfig *sf =
        &write->payload.module_config.payload_variant.store_forward;
    meshtastic_ModuleConfig_TelemetryConfig *telemetry =
        &write->payload.module_config.payload_variant.telemetry;
    meshtastic_Config_PositionConfig *position = &write->payload.config.payload_variant.position;
    meshtastic_Config_PowerConfig *power = &write->payload.config.payload_variant.power;
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
    case MESH_UI_FIELD_DEVICE_ROLE:
        device->role = (meshtastic_Config_DeviceConfig_Role)edit->number;
        break;
    case MESH_UI_FIELD_DEVICE_TZDEF:
        snprintf(device->tzdef, sizeof device->tzdef, "%.*s", (int)(sizeof device->tzdef - 1U),
                 edit->text);
        break;
    case MESH_UI_FIELD_DEVICE_REBROADCAST:
        device->rebroadcast_mode = (meshtastic_Config_DeviceConfig_RebroadcastMode)edit->number;
        break;
    case MESH_UI_FIELD_DEVICE_NODEINFO_SECS:
        device->node_info_broadcast_secs = edit->number;
        break;
    case MESH_UI_FIELD_DEVICE_LED_HEARTBEAT:
        /* The row is the plain statement; the protobuf field is the negation of it. */
        device->led_heartbeat_disabled = !on;
        break;
    case MESH_UI_FIELD_DEVICE_DOUBLE_TAP:
        device->double_tap_as_button_press = on;
        break;
    case MESH_UI_FIELD_POSITION_GPS_MODE:
        position->gps_mode = (meshtastic_Config_PositionConfig_GpsMode)edit->number;
        break;
    case MESH_UI_FIELD_POSITION_BROADCAST_SECS:
        position->position_broadcast_secs = edit->number;
        break;
    case MESH_UI_FIELD_POSITION_SMART:
        position->position_broadcast_smart_enabled = on;
        break;
    case MESH_UI_FIELD_POSITION_SMART_DISTANCE:
        position->broadcast_smart_minimum_distance = edit->number;
        break;
    case MESH_UI_FIELD_POSITION_SMART_INTERVAL:
        position->broadcast_smart_minimum_interval_secs = edit->number;
        break;
    case MESH_UI_FIELD_POSITION_GPS_INTERVAL:
        position->gps_update_interval = edit->number;
        break;
    case MESH_UI_FIELD_POWER_SAVING:
        power->is_power_saving = on;
        break;
    case MESH_UI_FIELD_POWER_LS_SECS:
        power->ls_secs = edit->number;
        break;
    case MESH_UI_FIELD_POWER_MIN_WAKE:
        power->min_wake_secs = edit->number;
        break;
    case MESH_UI_FIELD_POWER_WAIT_BT:
        power->wait_bluetooth_secs = edit->number;
        break;
    case MESH_UI_FIELD_POWER_SHUTDOWN:
        power->on_battery_shutdown_after_secs = edit->number;
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
    case MESH_UI_FIELD_MQTT_ENABLED:
        mqtt->enabled = on;
        break;
    case MESH_UI_FIELD_MQTT_ADDRESS:
        snprintf(mqtt->address, sizeof mqtt->address, "%.*s", (int)(sizeof mqtt->address - 1U),
                 edit->text);
        break;
    case MESH_UI_FIELD_MQTT_USERNAME:
        snprintf(mqtt->username, sizeof mqtt->username, "%.*s", (int)(sizeof mqtt->username - 1U),
                 edit->text);
        break;
    case MESH_UI_FIELD_MQTT_PASSWORD:
        snprintf(mqtt->password, sizeof mqtt->password, "%.*s", (int)(sizeof mqtt->password - 1U),
                 edit->text);
        break;
    case MESH_UI_FIELD_MQTT_ROOT:
        snprintf(mqtt->root, sizeof mqtt->root, "%.*s", (int)(sizeof mqtt->root - 1U), edit->text);
        break;
    case MESH_UI_FIELD_MQTT_ENCRYPTION:
        mqtt->encryption_enabled = on;
        break;
    case MESH_UI_FIELD_MQTT_TLS:
        mqtt->tls_enabled = on;
        break;
    case MESH_UI_FIELD_MQTT_MAP_REPORTING:
        mqtt->map_reporting_enabled = on;
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
    case MESH_UI_SETTINGS_DEVICE:
        if (!radio->has_device) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CONFIG;
        out->type = meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG;
        out->payload.config.which_payload_variant = meshtastic_Config_device_tag;
        out->payload.config.payload_variant.device = radio->device;
        break;
    case MESH_UI_SETTINGS_POSITION:
        if (!radio->has_position) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CONFIG;
        out->type = meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
        out->payload.config.which_payload_variant = meshtastic_Config_position_tag;
        out->payload.config.payload_variant.position = radio->position;
        break;
    case MESH_UI_SETTINGS_POWER:
        if (!radio->has_power) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_CONFIG;
        out->type = meshtastic_AdminMessage_ConfigType_POWER_CONFIG;
        out->payload.config.which_payload_variant = meshtastic_Config_power_tag;
        out->payload.config.payload_variant.power = radio->power;
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
    case MESH_UI_SETTINGS_MQTT:
        if (!radio->has_mqtt) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_MODULE_CONFIG;
        out->type = meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG;
        out->payload.module_config.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
        out->payload.module_config.payload_variant.mqtt = radio->mqtt;
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
        const enum mesh_ui_setting_field field = (enum mesh_ui_setting_field)action->edits[i].field;
        if (mesh_ui_settings_field_section(field) !=
            (enum mesh_ui_settings_section)action->section) {
            continue; /* an edit from another section has no business in this write */
        }
        if (mesh_ui_settings_field_consumer(field) != MESH_UI_SETTING_CONSUMER_SECTION) {
            continue; /* in this section but written by its own row, not by Y */
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

/*
 * "Set fixed position" and "Clear fixed position". Not a section save - the firmware takes
 * these through set_fixed_position rather than set_config, and sets PositionConfig's own
 * `fixed_position` flag itself - but announced like one, because from where the user is
 * standing it is the same press with the same ack behind it.
 *
 * A coordinate row the user did not touch keeps what the radio reported, so pinning a GPS fix
 * down is opening the section and pressing one row.
 */
static void mesh_app_save_fixed_position(struct mesh_app *app, const struct mesh_ui_action *action,
                                         uint64_t now) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const bool clearing = ((enum mesh_ui_settings_action)action->number ==
                           MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION);
    const struct mesh_ui_settings *ui = &app->ui_store.settings;
    int result = 0;

    if (clearing) {
        result = mesh_session_clear_fixed_position(&app->session);
    } else {
        int32_t latitude = ui->has_own_position ? ui->own_latitude_i : 0;
        int32_t longitude = ui->has_own_position ? ui->own_longitude_i : 0;
        bool has_altitude = ui->has_own_altitude;
        int32_t altitude = ui->has_own_altitude ? ui->own_altitude : 0;
        bool bad = false;
        for (uint8_t i = 0; i < action->edit_count && i < MESH_UI_SETTINGS_EDITS_MAX; ++i) {
            const struct mesh_ui_setting_edit *edit = &action->edits[i];
            switch ((enum mesh_ui_setting_field)edit->field) {
            case MESH_UI_FIELD_POSITION_LATITUDE:
                bad = bad || !mesh_ui_settings_coord_parse(edit->text, 90, &latitude);
                break;
            case MESH_UI_FIELD_POSITION_LONGITUDE:
                bad = bad || !mesh_ui_settings_coord_parse(edit->text, 180, &longitude);
                break;
            case MESH_UI_FIELD_POSITION_ALTITUDE: {
                /* Metres, and a plain integer: the one coordinate row that is not degrees. */
                char *end = NULL;
                const long metres = strtol(edit->text, &end, 10);
                if (end == edit->text || (end != NULL && *end != '\0') || metres < -12000L ||
                    metres > 12000L) {
                    bad = true;
                } else {
                    has_altitude = true;
                    altitude = (int32_t)metres;
                }
                break;
            }
            default:
                /* Everything else in this section is saved with Y, not with this row. */
                break;
            }
        }
        if (bad) {
            mesh_ui_store_set_toast(&app->ui_store, now,
                                    "Latitude, longitude and altitude must be numbers");
            return;
        }
        if (latitude == 0 && longitude == 0) {
            /* Null Island is where an empty form lands, not where anybody is. */
            mesh_ui_store_set_toast(&app->ui_store, now, "Set a latitude and longitude first");
            return;
        }
        result = mesh_session_set_fixed_position(&app->session, latitude, longitude, has_altitude,
                                                 altitude);
    }

    if (result > 0) {
        const struct mesh_radio_settings *radio = mesh_session_settings(&app->session);
        app->settings_save_pending = true;
        app->settings_writes_acked_seen = radio != NULL ? radio->writes_acked : 0U;
        app->settings_writes_failed_seen = radio != NULL ? radio->writes_failed : 0U;
        snprintf(app->settings_save_section, sizeof app->settings_save_section, "%s",
                 clearing ? "Fixed position" : "Position");
        /* The GPS rows are saved with Y and stay pending until it is pressed. */
        mesh_ui_store_settings_edits_consumed(&app->ui_store,
                                              MESH_UI_SETTING_CONSUMER_FIXED_POSITION);
        snprintf(toast, sizeof toast, "%s...",
                 clearing ? "Clearing fixed position" : "Pinning position");
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", "Not connected to a node; edits kept");
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", "That is not a place on Earth");
    } else {
        snprintf(toast, sizeof toast, "Failed (%d); edits kept", result);
        mesh_log_warn("ui", "Fixed position write failed: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void mesh_app_save_settings(struct mesh_app *app, const struct mesh_ui_action *action,
                                   uint64_t now) {
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
    int result =
        mesh_app_build_settings_write(mesh_session_settings(&app->session), action, &write);
    if (result == 0) {
        result = mesh_session_write_settings(&app->session, &write);
    }
    if (result > 0) {
        const struct mesh_radio_settings *radio = mesh_session_settings(&app->session);
        app->settings_save_pending = true;
        app->settings_writes_acked_seen = radio != NULL ? radio->writes_acked : 0U;
        app->settings_writes_failed_seen = radio != NULL ? radio->writes_failed : 0U;
        snprintf(app->settings_save_section, sizeof app->settings_save_section, "%s", section_name);
        /* Only the edits this write carried: a coordinate typed in the Position section is
           written by its own row, and clearing it here would drop it unsaved. */
        mesh_ui_store_settings_edits_consumed(&app->ui_store, MESH_UI_SETTING_CONSUMER_SECTION);
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
/* Defined below, next to the publish path it was written for. */
static void mesh_app_format_peer_name(const struct mesh_handshake_status *status, uint32_t node_id,
                                      char *out, size_t out_len);

static void mesh_app_watch_sent(struct mesh_app *app, uint32_t packet_id, const char *peer);

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
        mesh_log_info("ui", "Connect to %s (%s) requested from the device", action->identifier,
                      action->kind == (uint8_t)MESH_UI_DEVICE_SERIAL ? "usb" : "ble");
        snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device,
                 "%s", action->identifier);
        app->ui_preferences.preferred_device_kind = action->kind;
        app->ui_preferences_dirty = true;
        /* Asking for a radio lifts a hold an earlier disconnect put on auto-connect. */
        app->autoconnect_held = false;
        app->autoconnect_failures = 0U;
        app->autoconnect_retry_at_ms = 0U;

        /* A user pick beats whatever auto-connect is doing or has done, on either link. */
        const int result = mesh_app_link_connect(app, action->identifier, action->kind);
        if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
            snprintf(toast, sizeof toast, "Connecting to %.40s", action->identifier);
            /* BLE resolves services from tick(), so a 0 here is not yet a connection. Arm the
               error report so whatever goes wrong next reaches the screen. */
            app->ui_report_link_error = true;
        } else if (mesh_transport_registry_take_error(&app->transport_registry, toast,
                                                      sizeof toast)) {
            mesh_log_warn("ui", "Connect to %s failed: %s (%d)", action->identifier, toast, result);
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
        const int result = mesh_session_send_text(&app->session, action->dest, action->channel,
                                                  action->text, !broadcast, &packet_id);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Sent to %s", app->ui_store.nav.target_name);
            mesh_log_info("ui", "Sent \"%s\" to %s (packet %u)", action->text,
                          app->ui_store.nav.target_name, packet_id);
            mesh_app_watch_sent(app, packet_id, app->ui_store.nav.target_name);
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
        const int result = mesh_session_refresh_settings(&app->session);
        if (result > 0 && action->edit_count > 0U) {
            snprintf(toast, sizeof toast, "Refreshing %d sections; %u edit%s kept, Y saves", result,
                     (unsigned)action->edit_count, action->edit_count == 1U ? "" : "s");
        } else if (result > 0) {
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
        mesh_app_save_settings(app, action, now);
        return;
    }
    case MESH_UI_ACTION_RADIO_ACTION: {
        /*
         * Reboot, shutdown and the three resets. Nothing here waits for an answer: the radio
         * acts a few seconds after acking and takes the link with it, so the toast says what
         * was asked for. A shutdown in particular has no reconnect to promise - the radio has
         * to be switched on by hand - so it says so rather than leaving auto-connect to look
         * broken while it retries a node that is off.
         */
        /* The two fixed-position rows are radio actions but not destructive ones: they are a
           save the user pressed for, they read the coordinate rows above them, and they are
           announced through the same "Saving ..." machinery a section save uses. */
        if ((enum mesh_ui_settings_action)action->number ==
                MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION ||
            (enum mesh_ui_settings_action)action->number ==
                MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION) {
            mesh_app_save_fixed_position(app, action, now);
            return;
        }
        enum mesh_admin_request_kind kind = MESH_ADMIN_REBOOT;
        const char *asked = "Rebooting; reconnecting shortly";
        switch ((enum mesh_ui_settings_action)action->number) {
        case MESH_UI_SETTINGS_ACTION_REBOOT:
            break;
        case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
            kind = MESH_ADMIN_SHUTDOWN;
            asked = "Shutting down; switch it on by hand";
            break;
        case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
            kind = MESH_ADMIN_RESET_NODEDB;
            asked = "Node database reset; favorites kept";
            break;
        case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
            kind = MESH_ADMIN_FACTORY_RESET_CONFIG;
            asked = "Factory reset sent; radio restarting";
            break;
        case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
            kind = MESH_ADMIN_FACTORY_RESET_DEVICE;
            asked = "Factory reset sent; forget it in Devices";
            break;
        default:
            return; /* a row the nav should never have confirmed */
        }
        const int result = mesh_session_radio_action(&app->session, kind);
        if (result > 0) {
            snprintf(toast, sizeof toast, "%s", asked);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Already requested");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else {
            snprintf(toast, sizeof toast, "Request failed (%d)", result);
            mesh_log_warn("ui", "Radio action %u failed: %d", (unsigned)action->number, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_FAVORITE: {
        const bool favorite = (action->number != 0U);
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_set_node_favorite(&app->session, action->dest, favorite);
        if (result > 0) {
            snprintf(toast, sizeof toast, "%s %.20s", favorite ? "Pinned" : "Unpinned", name);
            mesh_log_info("ui", "%s node 0x%08x from the Nodes tab",
                          favorite ? "Pinned" : "Unpinned", action->dest);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%.20s is already %s", name,
                     favorite ? "pinned" : "unpinned");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else {
            snprintf(toast, sizeof toast, "Pin failed (%d)", result);
            mesh_log_warn("ui", "Favorite for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REQUEST_NODE_INFO: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_request_node_info(&app->session, action->dest);
        if (result == 0) {
            /* Nothing here can promise an answer: the node may be out of range, asleep, or
               simply slow, and no ack comes back for the request itself. */
            snprintf(toast, sizeof toast, "Asked %.20s to introduce itself", name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -EAGAIN) {
            /* Our own owner record has not landed yet, and sending a placeholder would erase
               this node's name on whoever received it. */
            snprintf(toast, sizeof toast, "%s", "Still syncing; try again in a moment");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "Cannot ask this node");
        } else {
            snprintf(toast, sizeof toast, "Request failed (%d)", result);
            mesh_log_warn("ui", "NodeInfo request for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_IGNORE: {
        const bool ignored = (action->number != 0U);
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_set_node_ignored(&app->session, action->dest, ignored);
        if (result > 0) {
            /* Said as what it does to the traffic, not as a preference that was recorded. */
            snprintf(toast, sizeof toast,
                     ignored ? "Dropping packets from %.20s" : "Hearing %.20s again", name);
            mesh_log_info("ui", "%s node 0x%08x from the Nodes tab",
                          ignored ? "Ignoring" : "Unignoring", action->dest);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%.20s is already %s", name,
                     ignored ? "ignored" : "not ignored");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "Cannot ignore this node");
        } else {
            snprintf(toast, sizeof toast, "Ignore failed (%d)", result);
            mesh_log_warn("ui", "Ignore for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_MUTE: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_toggle_node_muted(&app->session, action->dest);
        if (result == 0) {
            /* Already on its way. Two local flips for one toggle on the wire would leave the
               row stating the opposite of what the radio is about to do. */
            snprintf(toast, sizeof toast, "%s", "Mute already requested");
        } else if (result > 0) {
            /* The session flipped the cached flag on the way through, so what it now holds is
               what we asked the radio for. */
            const struct mesh_ui_node_summary *node =
                mesh_ui_node_detail_find(&app->ui_store.handshake, action->dest);
            const bool muted = node != NULL ? node->is_muted : true;
            snprintf(toast, sizeof toast, "%s %.20s", muted ? "Muted" : "Unmuted", name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else {
            snprintf(toast, sizeof toast, "Mute failed (%d)", result);
            mesh_log_warn("ui", "Mute for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REMOVE_NODE: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_remove_node(&app->session, action->dest);
        if (result > 0) {
            /* Says how it comes back, because the row that would have undone it has gone with
               the node. */
            snprintf(toast, sizeof toast, "Removed %.14s; back when it speaks", name);
            mesh_log_info("ui", "Removed node 0x%08x from the Nodes tab", action->dest);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "That is the radio you are connected to");
        } else {
            snprintf(toast, sizeof toast, "Remove failed (%d)", result);
            mesh_log_warn("ui", "Remove of 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TRACEROUTE: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_send_traceroute(&app->session, action->dest);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Tracing route to %.20s", name);
            mesh_log_info("ui", "Traceroute to 0x%08x from the Nodes tab", action->dest);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -EBUSY) {
            /* One trace at a time is this client's half of the firmware's rate limit. */
            snprintf(toast, sizeof toast, "%s", "A traceroute is already running");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "Cannot trace a route to this node");
        } else {
            snprintf(toast, sizeof toast, "Traceroute failed (%d)", result);
            mesh_log_warn("ui", "Traceroute to 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_DISCONNECT: {
        struct mesh_transport *transport = mesh_app_active_transport();
        const char *identifier = mesh_app_connected_identifier();
        char name[64];
        snprintf(name, sizeof name, "%s",
                 action->identifier[0] != '\0' ? action->identifier
                                               : (identifier != NULL ? identifier : ""));

        int result = -ENOTCONN;
        if (transport == mesh_serial_transport()) {
            result = mesh_serial_transport_disconnect(transport);
        } else if (transport != NULL) {
            result = mesh_ble_transport_disconnect(transport);
        }

        if (result == 0) {
            /* Holding auto-connect is the point of the press: without it the next loop turn
               takes the same radio straight back. */
            app->autoconnect_held = true;
            app->ui_report_link_error = false;
            if (name[0] != '\0') {
                snprintf(toast, sizeof toast, "Disconnected from %.30s", name);
            } else {
                snprintf(toast, sizeof toast, "%s", "Disconnected");
            }
            mesh_log_info("ui", "Disconnect requested from the device (%s)",
                          name[0] != '\0' ? name : "active link");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Nothing is connected");
        } else {
            snprintf(toast, sizeof toast, "Disconnect failed (%d)", result);
            mesh_log_warn("ui", "Disconnect failed: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_FORGET: {
        if (ble == NULL || action->kind != (uint8_t)MESH_UI_DEVICE_BLE) {
            mesh_ui_store_set_toast(&app->ui_store, now, "Only Bluetooth nodes are paired");
            return;
        }
        const bool was_connected =
            (mesh_app_connected_identifier() != NULL &&
             strcmp(mesh_app_connected_identifier(), action->identifier) == 0);
        const int result = mesh_ble_transport_forget(ble, action->identifier);
        if (result == 0) {
            /* The bond is gone, so a reconnect would only fail on StartNotify until the user
               pairs again; do not let auto-connect spend the next minute proving it. */
            if (was_connected) {
                app->autoconnect_held = true;
            }
            snprintf(toast, sizeof toast, "Forgot %.30s; pair again to use it", action->identifier);
            mesh_log_info("ui", "Forgot BLE node %s", action->identifier);
        } else if (mesh_transport_registry_take_error(&app->transport_registry, toast,
                                                      sizeof toast)) {
            mesh_log_warn("ui", "Forget %s failed: %s (%d)", action->identifier, toast, result);
        } else {
            snprintf(toast, sizeof toast, "Could not forget it (%d)", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SUBMIT_PASSKEY: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        const unsigned long value = strtoul(action->text, NULL, 10);
        const int result = mesh_ble_transport_submit_passkey(ble, (uint32_t)value);
        if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Pairing...");
            /* Whatever goes wrong from here is reported by the transport, not by this call. */
            app->ui_report_link_error = true;
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "The pairing request expired");
        } else {
            snprintf(toast, sizeof toast, "Pairing failed (%d)", result);
            mesh_log_warn("ui", "Passkey submit failed: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_CANCEL_PAIRING: {
        if (ble != NULL) {
            (void)mesh_ble_transport_cancel_pairing(ble);
        }
        /* A cancelled pairing is a cancelled connect: do not let auto-connect start it over. */
        app->autoconnect_held = true;
        app->ui_report_link_error = false;
        mesh_ui_store_set_toast(&app->ui_store, now, "Pairing cancelled");
        return;
    }
    case MESH_UI_ACTION_CYCLE_UPDATE_CHANNEL: {
        /* Steps DEFAULT -> STABLE -> PRERELEASE -> DEFAULT. Saved immediately rather than
           collected as a pending edit: About has no Y-save, because there is no radio write
           behind it. */
        const enum mesh_update_channel next = (enum mesh_update_channel)(
            ((unsigned)app->updater.channel + 1U) % (unsigned)MESH_UPDATE_CHANNEL_COUNT);
        if (!mesh_updater_set_channel(&app->updater, next)) {
            mesh_ui_store_set_toast(&app->ui_store, now, "Busy; try again in a moment");
            return;
        }
        app->ui_preferences.update_channel = (uint8_t)app->updater.channel;
        app->ui_preferences_dirty = true;
        snprintf(toast, sizeof toast, "Update channel: %.*s", (int)(sizeof toast - 18U),
                 mesh_update_channel_name(app->updater.channel));
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_DEV_UPDATES: {
        if (!mesh_updater_set_allow_dev(&app->updater, !app->updater.allow_dev)) {
            mesh_ui_store_set_toast(&app->ui_store, now, "Busy; try again in a moment");
            return;
        }
        app->ui_preferences.update_allow_dev = app->updater.allow_dev;
        app->ui_preferences_dirty = true;
        mesh_ui_store_set_toast(&app->ui_store, now,
                                app->updater.allow_dev ? "Dev updates on; check again"
                                                       : "Dev updates off");
        return;
    }
    case MESH_UI_ACTION_CHECK_UPDATE: {
        const int result = mesh_updater_check(&app->updater, now);
        if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Checking for updates");
        } else if (result == -ENOTSUP) {
            snprintf(toast, sizeof toast, "%.*s", (int)(sizeof toast - 1U),
                     app->updater.message[0] != '\0' ? app->updater.message
                                                     : "Updates are unavailable here");
        } else if (result == -EBUSY) {
            snprintf(toast, sizeof toast, "%s", "Already checking");
        } else {
            snprintf(toast, sizeof toast, "Update check failed (%d)", result);
            mesh_log_warn("ui", "Update check could not start: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_INSTALL_UPDATE: {
        const int result = mesh_updater_install(&app->updater, now);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Downloading %.20s", app->updater.latest);
            mesh_log_info("ui", "Installing update %s from the About screen", app->updater.latest);
        } else if (result == -EBUSY) {
            snprintf(toast, sizeof toast, "%s", "Already working");
        } else if (result == -EINVAL) {
            /* Nothing to install: the check has not run, or found nothing newer. */
            snprintf(toast, sizeof toast, "%s", "Check for an update first");
        } else if (result == -ENOTSUP) {
            snprintf(toast, sizeof toast, "%s", "Updates are unavailable here");
        } else {
            snprintf(toast, sizeof toast, "Update failed (%d)", result);
            mesh_log_warn("ui", "Update install could not start: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_NONE:
    default:
        return;
    }
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

    /* "cli" and "stub" are asked for explicitly; everything else - including no request at all -
       resolves to the framebuffer, which is what the pak runs, and falls back to the CLI backend
       only where there is no /dev/fb0 to draw on (a container, or a dev host). */
    if (requested != NULL && strcasecmp(requested, "cli") == 0) {
        mesh_app_select_cli(app, &backend, &backend_userdata);
    } else if (requested != NULL && strcasecmp(requested, "stub") == 0) {
        mesh_app_select_stub(&backend, &backend_userdata);
    } else {
        if (requested != NULL && strcasecmp(requested, "fb") != 0 &&
            strcasecmp(requested, "auto") != 0) {
            mesh_log_warn("ui", "Unknown UI backend '%s'; using the default", requested);
        }
        if (!mesh_app_select_fb(app, &backend, &backend_userdata)) {
            mesh_app_select_cli(app, &backend, &backend_userdata);
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
static void mesh_app_format_peer_name(const struct mesh_handshake_status *status, uint32_t node_id,
                                      char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }

    if (node_id == MESH_MESSAGE_BROADCAST_ADDR) {
        snprintf(out, out_len, "all");
        return;
    }

    if (status != NULL) {
        for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
            if (status->nodes[i].node_id != node_id) {
                continue;
            }
            /* The names are already sanitised; the copy still has to respect character
               boundaries because peer_name is far shorter than long_name. */
            if (status->nodes[i].short_name[0] != '\0') {
                mesh_text_sanitise_str(status->nodes[i].short_name, out, out_len);
                return;
            }
            if (status->nodes[i].long_name[0] != '\0') {
                mesh_text_sanitise_str(status->nodes[i].long_name, out, out_len);
                return;
            }
            break;
        }
    }

    snprintf(out, out_len, "!%08x", node_id);
}

/* Position, device metrics and environment, from the session's structs into the UI's twins.
   Field by field rather than a memcpy: the two declarations are deliberately independent (the
   UI half must stay free of nanopb), so nothing but this function keeps them in step. */
static void mesh_app_copy_node_detail(const struct mesh_node_summary *src,
                                      struct mesh_ui_node_summary *dst) {
    dst->position.valid = src->position.valid;
    dst->position.latitude_i = src->position.latitude_i;
    dst->position.longitude_i = src->position.longitude_i;
    dst->position.has_altitude = src->position.has_altitude;
    dst->position.altitude = src->position.altitude;
    dst->position.time = src->position.time;
    dst->position.sats_in_view = src->position.sats_in_view;
    dst->position.precision_bits = src->position.precision_bits;

    dst->metrics.valid = src->metrics.valid;
    dst->metrics.time = src->metrics.time;
    dst->metrics.has_battery = src->metrics.has_battery;
    dst->metrics.battery_level = src->metrics.battery_level;
    dst->metrics.has_voltage = src->metrics.has_voltage;
    dst->metrics.voltage = src->metrics.voltage;
    dst->metrics.has_channel_utilization = src->metrics.has_channel_utilization;
    dst->metrics.channel_utilization = src->metrics.channel_utilization;
    dst->metrics.has_air_util_tx = src->metrics.has_air_util_tx;
    dst->metrics.air_util_tx = src->metrics.air_util_tx;
    dst->metrics.has_uptime = src->metrics.has_uptime;
    dst->metrics.uptime_seconds = src->metrics.uptime_seconds;

    dst->environment.valid = src->environment.valid;
    dst->environment.time = src->environment.time;
    dst->environment.has_temperature = src->environment.has_temperature;
    dst->environment.temperature = src->environment.temperature;
    dst->environment.has_humidity = src->environment.has_humidity;
    dst->environment.relative_humidity = src->environment.relative_humidity;
    dst->environment.has_pressure = src->environment.has_pressure;
    dst->environment.barometric_pressure = src->environment.barometric_pressure;
    dst->environment.has_iaq = src->environment.has_iaq;
    dst->environment.iaq = src->environment.iaq;
    dst->environment.has_lux = src->environment.has_lux;
    dst->environment.lux = src->environment.lux;
    dst->environment.has_voltage = src->environment.has_voltage;
    dst->environment.voltage = src->environment.voltage;
    dst->environment.has_current = src->environment.has_current;
    dst->environment.current = src->environment.current;
}

/* Lower is more important; see the ranking comment in mesh_app_publish_ui_state(). */
unsigned mesh_app_node_rank(const struct mesh_node_summary *node, uint32_t my_node,
                            const struct mesh_message_log *log,
                            const struct mesh_ui_preferences *prefs) {
    if (my_node != 0U && node->node_id == my_node) {
        return 0U;
    }
    /* A pinned node outranks even someone you are mid-conversation with: pinning is the user
       saying "keep this one where I can see it", and it is also what keeps a quiet node inside
       the UI's 128-node budget when the mesh is busy. */
    if (node->is_favorite) {
        return 1U;
    }
    /*
     * A radio of our own that we are not connected to right now. is_favorite lives in the
     * connected radio's NodeDB and is resolved per receiver, so a pin only ever teaches the
     * radio it was made on: move the Brick from one of your nodes to another and the node you
     * just unplugged arrives on the new radio as an ordinary stranger, ranked by last_heard,
     * free to fall out of the 128-node budget on a busy mesh. The client remembers its own
     * hardware instead (mesh_ui_preferences_note_radio), which needs no admin write and cannot
     * disagree with what "favorite" means on the radio.
     */
    if (mesh_ui_preferences_knows_radio(prefs, node->node_id)) {
        return 2U;
    }
    if (log != NULL) {
        for (size_t i = 0; i < log->count; ++i) {
            const struct mesh_message *message = mesh_message_log_at(log, i);
            if (message == NULL) {
                continue;
            }
            if (message->from == node->node_id || message->to == node->node_id) {
                return 3U;
            }
        }
    }
    return node->via_mqtt ? 5U : 4U;
}

/* Copies the newest MESH_UI_MAX_MESSAGES entries out of the transport ring into the store,
   merged with whatever history was restored from the cache at startup. */
static void mesh_app_publish_messages(struct mesh_app *app,
                                      const struct mesh_handshake_status *status) {
    const struct mesh_message_log *log = mesh_session_messages(&app->session);
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
        target->ack_error = source->ack_error;
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

/*
 * A traceroute, from the protobuf's shape into the one the UI draws. RouteDiscovery gives the
 * *intermediate* nodes and a parallel array of link SNRs; what a reader wants is the whole
 * path with a reading against each stop it reached. So this stitches the ends on - us at the
 * front going out, the target at the front coming back - resolves every hop to a name, and
 * pairs hop i with snr[i - 1], the link that got the packet there.
 *
 * The SNR array is normally one longer than the route (one reading per link, not per node),
 * but a firmware that disagrees must not make us read off the end, so each pairing is bounds
 * checked rather than assumed.
 */
static uint8_t mesh_app_flatten_route(const struct mesh_handshake_status *status,
                                      uint32_t first_node, const uint32_t *route,
                                      uint8_t route_count, const int8_t *snr, uint8_t snr_count,
                                      uint32_t last_node, struct mesh_ui_traceroute_hop *out) {
    uint8_t count = 0U;
    /* The path is first_node, then every node that forwarded it, then the far end. */
    uint32_t path[MESH_UI_TRACEROUTE_MAX_HOPS];
    path[count++] = first_node;
    for (uint8_t i = 0; i < route_count && count < MESH_UI_TRACEROUTE_MAX_HOPS - 1U; ++i) {
        path[count++] = route[i];
    }
    path[count++] = last_node;

    for (uint8_t i = 0; i < count; ++i) {
        struct mesh_ui_traceroute_hop *hop = &out[i];
        memset(hop, 0, sizeof *hop);
        hop->node_id = path[i];
        mesh_app_format_peer_name(status, path[i], hop->name, sizeof hop->name);
        /* The first stop is the sender: nothing carried the packet *to* it. */
        if (i > 0U && (uint8_t)(i - 1U) < snr_count) {
            hop->has_snr = true;
            hop->snr_quarter_db = snr[i - 1U];
        }
    }
    return count;
}

void mesh_app_flatten_traceroute(const struct mesh_handshake_status *status,
                                 const struct mesh_traceroute *src, uint32_t my_node,
                                 struct mesh_ui_traceroute *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL || src->state == MESH_TRACEROUTE_IDLE) {
        return;
    }
    dst->state = src->state;
    dst->target = src->target;
    dst->completed = src->completed;
    if (src->state != MESH_TRACEROUTE_DONE) {
        return;
    }
    dst->forward_count =
        mesh_app_flatten_route(status, my_node, src->route, src->route_count, src->snr,
                               src->snr_count, src->target, dst->forward);
    /* The way back is only drawn when the firmware measured it; an empty route_back with no
       readings would otherwise render as a bare two-stop path that says nothing. */
    if (src->snr_back_count > 0U || src->back_count > 0U) {
        dst->back_count =
            mesh_app_flatten_route(status, src->target, src->route_back, src->back_count,
                                   src->snr_back, src->snr_back_count, my_node, dst->back);
    }
}

/* Mesh health, copied field by field for the same reason everything else here is: nothing
   keeps the session's declaration and the UI's in step but this function. */
static void mesh_app_flatten_radio_stats(const struct mesh_radio_stats *src,
                                         struct mesh_ui_radio_stats *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL || !src->valid) {
        return;
    }
    dst->valid = true;
    dst->time = src->time;
    dst->uptime_seconds = src->uptime_seconds;
    dst->channel_utilization = src->channel_utilization;
    dst->air_util_tx = src->air_util_tx;
    dst->num_packets_tx = src->num_packets_tx;
    dst->num_packets_rx = src->num_packets_rx;
    dst->num_packets_rx_bad = src->num_packets_rx_bad;
    dst->num_rx_dupe = src->num_rx_dupe;
    dst->num_tx_relay = src->num_tx_relay;
    dst->num_tx_relay_canceled = src->num_tx_relay_canceled;
    dst->num_tx_dropped = src->num_tx_dropped;
    dst->num_online_nodes = src->num_online_nodes;
    dst->num_total_nodes = src->num_total_nodes;
    dst->has_heap = src->has_heap;
    dst->heap_total_bytes = src->heap_total_bytes;
    dst->heap_free_bytes = src->heap_free_bytes;
    dst->has_noise_floor = src->has_noise_floor;
    dst->noise_floor = src->noise_floor;
}

/* Flattens the transport's protobuf-typed view into the UI's plain struct. */
/* The About section's data: this client rather than the radio. The updater's state is copied
   across as a byte and a line of text so store.h stays free of the updater, the same way the
   radio's settings are copied free of nanopb. */
static void mesh_app_flatten_client_info(const struct mesh_app *app,
                                         struct mesh_ui_client_info *dst) {
    memset(dst, 0, sizeof *dst);
    snprintf(dst->version, sizeof dst->version, "%s", mesh_version_string());
    if (app == NULL) {
        return;
    }
    if (app->ui_controller.backend != NULL && app->ui_controller.backend->name != NULL) {
        snprintf(dst->backend, sizeof dst->backend, "%s", app->ui_controller.backend->name);
    }
    /* The preferences file's directory: where a user looking for canned.txt or the caches
       should go, which on the Brick is inside the pak's userdata and not obvious. */
    if (app->ui_preferences_path[0] != '\0') {
        /* A path longer than the display field is clipped rather than refused: it is shown
           for orientation, not used to open anything. */
        snprintf(dst->data_dir, sizeof dst->data_dir, "%.*s", (int)(sizeof dst->data_dir - 1U),
                 app->ui_preferences_path);
        char *slash = strrchr(dst->data_dir, '/');
        if (slash != NULL && slash != dst->data_dir) {
            *slash = '\0';
        }
    }

    const struct mesh_updater *updater = &app->updater;
    dst->update_state = (uint8_t)updater->state;
    dst->update_supported = mesh_updater_available(updater);
    dst->update_busy =
        updater->state == MESH_UPDATE_CHECKING || updater->state == MESH_UPDATE_DOWNLOADING;
    dst->update_can_install = mesh_updater_can_install(updater);
    dst->update_is_release = mesh_version_is_release();
    dst->update_allow_dev = updater->allow_dev;
    dst->update_allow_dev_from_env = updater->allow_dev_from_env;
    snprintf(dst->update_message, sizeof dst->update_message, "%s", updater->message);
    snprintf(dst->update_latest, sizeof dst->update_latest, "%s", updater->latest);
    snprintf(dst->update_channel, sizeof dst->update_channel, "%s",
             mesh_update_channel_name(updater->channel));
}

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
        dst->node_info_broadcast_secs = src->device.node_info_broadcast_secs;
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
        dst->gps_update_interval = src->position.gps_update_interval;
        dst->smart_minimum_distance = src->position.broadcast_smart_minimum_distance;
        dst->smart_minimum_interval_secs = src->position.broadcast_smart_minimum_interval_secs;
    }
    if (src->has_power) {
        dst->has_power = true;
        dst->is_power_saving = src->power.is_power_saving;
        dst->ls_secs = src->power.ls_secs;
        dst->min_wake_secs = src->power.min_wake_secs;
        dst->on_battery_shutdown_after_secs = src->power.on_battery_shutdown_after_secs;
        dst->wait_bluetooth_secs = src->power.wait_bluetooth_secs;
    }
    if (src->has_mqtt) {
        dst->has_mqtt = true;
        dst->mqtt_enabled = src->mqtt.enabled;
        snprintf(dst->mqtt_address, sizeof dst->mqtt_address, "%s", src->mqtt.address);
        snprintf(dst->mqtt_username, sizeof dst->mqtt_username, "%s", src->mqtt.username);
        snprintf(dst->mqtt_password, sizeof dst->mqtt_password, "%s", src->mqtt.password);
        snprintf(dst->mqtt_root, sizeof dst->mqtt_root, "%s", src->mqtt.root);
        dst->mqtt_encryption_enabled = src->mqtt.encryption_enabled;
        dst->mqtt_tls_enabled = src->mqtt.tls_enabled;
        dst->mqtt_map_reporting_enabled = src->mqtt.map_reporting_enabled;
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

/*
 * Watching a message the user just sent, so its delivery result reaches them. Only a DM with
 * want_ack has one to wait for; a broadcast is fire-and-forget and is never watched.
 */
static void mesh_app_watch_sent(struct mesh_app *app, uint32_t packet_id, const char *peer) {
    if (app == NULL || packet_id == 0U) {
        return;
    }
    const size_t capacity = sizeof app->ui_sent_watch / sizeof app->ui_sent_watch[0];
    if (app->ui_sent_watch_count >= capacity) {
        /* Drop the oldest: a result nobody has seen in eight messages is not news any more. */
        memmove(&app->ui_sent_watch[0], &app->ui_sent_watch[1],
                (capacity - 1U) * sizeof app->ui_sent_watch[0]);
        app->ui_sent_watch_count = capacity - 1U;
    }
    struct mesh_app_sent_watch *slot = &app->ui_sent_watch[app->ui_sent_watch_count++];
    slot->packet_id = packet_id;
    snprintf(slot->peer, sizeof slot->peer, "%s", peer != NULL ? peer : "");
}

/* Announces the delivery result of anything being watched, once. Failures only: a delivered
   message already shows "ok" on its row, and a toast per message would be noise. */
static void mesh_app_report_delivery(struct mesh_app *app) {
    if (app == NULL || app->ui_sent_watch_count == 0U) {
        return;
    }
    const struct mesh_message_log *log = mesh_session_messages(&app->session);
    if (log == NULL) {
        return;
    }

    size_t kept = 0U;
    for (size_t i = 0; i < app->ui_sent_watch_count; ++i) {
        struct mesh_app_sent_watch *watch = &app->ui_sent_watch[i];
        const struct mesh_message *message = NULL;
        for (size_t j = 0; j < log->count; ++j) {
            const struct mesh_message *entry = mesh_message_log_at(log, j);
            if (entry != NULL && entry->packet_id == watch->packet_id) {
                message = entry;
                break;
            }
        }
        if (message == NULL) {
            continue; /* evicted from the ring; nothing left to report */
        }
        if (message->ack == MESH_MESSAGE_ACK_PENDING) {
            app->ui_sent_watch[kept++] = *watch;
            continue;
        }
        if (message->ack == MESH_MESSAGE_ACK_FAILED) {
            char toast[MESH_UI_NAV_TOAST_MAX];
            snprintf(toast, sizeof toast, "Not delivered to %.16s: %s", watch->peer,
                     mesh_message_ack_error_to_string(message->ack_error));
            mesh_ui_store_set_toast(&app->ui_store, mesh_app_now_ms(), toast);
        }
    }
    app->ui_sent_watch_count = kept;
}

void mesh_app_publish_ui_state(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_ui_store_tick(&app->ui_store, mesh_app_now_ms());
    mesh_app_report_delivery(app);

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        mesh_ui_store_set_transport_status(&app->ui_store, "unavailable");
        return;
    }

    const struct mesh_transport *active = mesh_app_active_transport();
    const char *transport_status =
        (active != NULL && active->ops != NULL && active->ops->status != NULL)
            ? active->ops->status(active)
            : NULL;
    mesh_ui_store_set_transport_status(&app->ui_store,
                                       transport_status != NULL ? transport_status : "unknown");

    struct mesh_ui_device ui_devices[MESH_UI_MAX_DEVICES];
    memset(ui_devices, 0, sizeof(ui_devices));
    size_t device_count = 0U;

    const char *connected_address = mesh_app_connected_identifier();
    bool connected_address_seen = false;

    /* Announce a dropped link once; auto-connect brings it back and the footer tracks it. */
    const bool link_connected = (connected_address != NULL && connected_address[0] != '\0');
    if (app->ui_link_was_connected && !link_connected &&
        app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
        mesh_ui_store_set_toast(&app->ui_store, mesh_app_now_ms(), "Radio link lost; reconnecting");
    }
    app->ui_link_was_connected = link_connected;

    if (link_connected) {
        app->ui_report_link_error = false;
    }

    /* USB ports first: a plugged-in node needs no pairing and no range, so it is the one you
       almost always want, and putting it at the top makes it the default cursor row. */
    struct mesh_serial_device_info serial_devices[MESH_SERIAL_MAX_DEVICES];
    const size_t serial_count = mesh_serial_transport_get_devices(
        mesh_serial_transport(), serial_devices, MESH_SERIAL_MAX_DEVICES);
    for (size_t i = 0; i < serial_count && device_count < MESH_UI_MAX_DEVICES; ++i) {
        struct mesh_ui_device *slot = &ui_devices[device_count];
        /* Before the bind there is no tty, so the sysfs id is all we can address it by. */
        const char *identifier =
            serial_devices[i].path[0] != '\0' ? serial_devices[i].path : serial_devices[i].id;
        snprintf(slot->identifier, sizeof slot->identifier, "%s", identifier);
        snprintf(slot->name, sizeof slot->name, "%s", serial_devices[i].name);
        slot->kind = (uint8_t)MESH_UI_DEVICE_SERIAL;
        slot->rssi = 0;
        slot->paired = true; /* a cable has nothing to bond */
        slot->connected = (connected_address != NULL && connected_address[0] != '\0' &&
                           strcmp(connected_address, identifier) == 0);
        if (slot->connected) {
            connected_address_seen = true;
        }
        ++device_count;
    }

    struct mesh_bluez_device_info ble_devices[MESH_UI_MAX_DEVICES];
    const size_t ble_count = mesh_ble_transport_get_devices(ble, ble_devices, MESH_UI_MAX_DEVICES);
    /* Which row the link is working on. Not the same as connected: a BLE connect is several
       seconds of pairing and service discovery before it is a connection. */
    const char *pending_address = mesh_ble_transport_pending_address(ble);
    for (size_t i = 0; i < ble_count && device_count < MESH_UI_MAX_DEVICES; ++i) {
        struct mesh_ui_device *slot = &ui_devices[device_count];
        snprintf(slot->identifier, sizeof slot->identifier, "%s", ble_devices[i].address);
        snprintf(slot->name, sizeof slot->name, "%s", ble_devices[i].name);
        slot->kind = (uint8_t)MESH_UI_DEVICE_BLE;
        int16_t rssi = ble_devices[i].rssi;
        if (rssi < INT8_MIN) {
            rssi = INT8_MIN;
        } else if (rssi > INT8_MAX) {
            rssi = INT8_MAX;
        }
        slot->rssi = (int8_t)rssi;
        slot->paired = ble_devices[i].paired;
        slot->busy =
            (pending_address != NULL && strcmp(pending_address, ble_devices[i].address) == 0);
        slot->connected = (connected_address != NULL && connected_address[0] != '\0' &&
                           strcmp(connected_address, ble_devices[i].address) == 0);
        if (slot->connected) {
            connected_address_seen = true;
        }
        ++device_count;
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

    /*
     * The BlueZ pairing agent's question, if it has one. It arrives in the middle of a connect
     * and BlueZ holds the bond open until it is answered, so the prompt is raised from here
     * rather than by a key press - and taken down again the moment the request is gone,
     * however it ended.
     */
    {
        struct mesh_ble_pairing_request request;
        if (mesh_ble_transport_pairing_request(ble, &request)) {
            /* The advertised name beats a MAC on a 40-column prompt. */
            char label[MESH_UI_NAV_TARGET_NAME_MAX];
            snprintf(label, sizeof label, "%s", request.label);
            for (size_t i = 0; i < device_count; ++i) {
                if (ui_devices[i].kind == (uint8_t)MESH_UI_DEVICE_BLE &&
                    ui_devices[i].name[0] != '\0' &&
                    strcmp(ui_devices[i].identifier, request.address) == 0) {
                    snprintf(label, sizeof label, "%.*s", (int)(sizeof label - 1U),
                             ui_devices[i].name);
                    break;
                }
            }
            mesh_ui_store_open_passkey_prompt(&app->ui_store, label, request.passkey,
                                              request.kind ==
                                                  (uint8_t)MESH_BLUEZ_AGENT_REQUEST_CONFIRM);
        } else {
            mesh_ui_store_close_passkey_prompt(&app->ui_store);
        }
    }

    bool preferences_modified = false;
    if (connected_address != NULL && connected_address[0] != '\0') {
        const uint8_t connected_kind = (active == mesh_serial_transport())
                                           ? (uint8_t)MESH_UI_DEVICE_SERIAL
                                           : (uint8_t)MESH_UI_DEVICE_BLE;
        if (strcmp(app->ui_preferences.preferred_device, connected_address) != 0 ||
            app->ui_preferences.preferred_device_kind != connected_kind) {
            snprintf(app->ui_preferences.preferred_device,
                     sizeof app->ui_preferences.preferred_device, "%s", connected_address);
            app->ui_preferences.preferred_device_kind = connected_kind;
            preferences_modified = true;
        }
    }

    struct mesh_handshake_status status = *mesh_session_handshake(&app->session);
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
            /* Remember this radio as one of ours. Pins live in the radio's own NodeDB, so
               without this the node you connect to today is a stranger on the node you
               connect to tomorrow; see mesh_app_node_rank(). */
            if (mesh_ui_preferences_note_radio(&app->ui_preferences, my_node)) {
                preferences_modified = true;
            }
            ui_handshake.my_info.node_num = status.my_info.my_node_num;
            ui_handshake.my_info.nodedb_entries = status.my_info.nodedb_count;
            ui_handshake.my_info.reboot_count = status.my_info.reboot_count;
            for (size_t i = 0; i < status.node_count && i < MESH_SESSION_MAX_NODES; ++i) {
                if (status.nodes[i].node_id == my_node && status.nodes[i].short_name[0] != '\0') {
                    snprintf(ui_handshake.my_short_name, sizeof(ui_handshake.my_short_name), "%s",
                             status.nodes[i].short_name);
                    break;
                }
            }
        }

        /* The UI carries fewer nodes than a real mesh has. Rank them so the ones that matter
           survive the cut: ourselves, then pinned nodes, then our other radios, then anyone we
           have exchanged messages with, then nodes heard directly over RF by last_heard, then
           MQTT-fed nodes by last_heard. On a mesh with an MQTT uplink dozens of far-away nodes
           are "heard" every minute and would otherwise push the radio you are actually talking
           to off the list. Insertion sort: MESH_SESSION_MAX_NODES is small and this runs once
           per publish. */
        const struct mesh_message_log *message_log = mesh_session_messages(&app->session);
        size_t order[MESH_SESSION_MAX_NODES];
        unsigned rank[MESH_SESSION_MAX_NODES];
        size_t total =
            status.node_count > MESH_SESSION_MAX_NODES ? MESH_SESSION_MAX_NODES : status.node_count;
        const uint32_t my_node = status.has_my_info ? status.my_info.my_node_num : 0U;
        for (size_t i = 0; i < total; ++i) {
            const struct mesh_node_summary *node = &status.nodes[i];
            rank[i] = mesh_app_node_rank(node, my_node, message_log, &app->ui_preferences);
            size_t j = i;
            while (j > 0U) {
                const size_t prev_index = order[j - 1U];
                const struct mesh_node_summary *prev = &status.nodes[prev_index];
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
            const struct mesh_node_summary *src = &status.nodes[order[i]];
            struct mesh_ui_node_summary *dst = &ui_handshake.nodes[i];
            dst->node_id = src->node_id;
            snprintf(dst->long_name, sizeof(dst->long_name), "%s", src->long_name);
            snprintf(dst->short_name, sizeof(dst->short_name), "%s", src->short_name);
            dst->last_heard = src->last_heard;
            dst->snr = src->snr;
            dst->via_mqtt = src->via_mqtt;
            dst->has_hops_away = src->has_hops_away;
            dst->hops_away = src->hops_away;
            snprintf(dst->user_id, sizeof(dst->user_id), "%s", src->user_id);
            dst->hw_model = src->hw_model;
            dst->role = src->role;
            dst->is_licensed = src->is_licensed;
            dst->is_unmessagable = src->is_unmessagable;
            dst->public_key_len = src->public_key_len > sizeof(dst->public_key)
                                      ? (uint8_t)sizeof(dst->public_key)
                                      : src->public_key_len;
            memcpy(dst->public_key, src->public_key, dst->public_key_len);
            dst->is_favorite = src->is_favorite;
            dst->is_ignored = src->is_ignored;
            dst->is_muted = src->is_muted;
            dst->channel = src->channel;
            mesh_app_copy_node_detail(src, dst);
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

    mesh_app_publish_messages(app, &status);

    const struct mesh_radio_settings *radio_settings = mesh_session_settings(&app->session);
    struct mesh_ui_settings ui_settings;
    mesh_app_flatten_settings(radio_settings, &ui_settings);
    /* flatten_settings() zeroes the struct, so the client's own facts go in after it. */
    mesh_app_flatten_client_info(app, &ui_settings.client);
    /* Where the radio says it is, which is not part of PositionConfig: it comes from our own
       node's record, and it is what the Position section's coordinate rows start from. */
    if (status.has_my_info) {
        const struct mesh_node_summary *self = NULL;
        for (size_t i = 0; i < status.node_count && i < MESH_SESSION_MAX_NODES; ++i) {
            if (status.nodes[i].node_id == status.my_info.my_node_num) {
                self = &status.nodes[i];
                break;
            }
        }
        if (self != NULL && self->position.valid) {
            ui_settings.has_own_position = true;
            ui_settings.own_latitude_i = self->position.latitude_i;
            ui_settings.own_longitude_i = self->position.longitude_i;
            ui_settings.has_own_altitude = self->position.has_altitude;
            ui_settings.own_altitude = self->position.altitude;
        }
    }
    mesh_app_flatten_radio_stats(mesh_session_radio_stats(&app->session), &ui_settings.stats);
    mesh_ui_store_set_settings(&app->ui_store, &ui_settings);
    mesh_app_track_settings_save(app, radio_settings, link_connected);

    struct mesh_ui_traceroute ui_traceroute;
    mesh_app_flatten_traceroute(&status, mesh_session_traceroute(&app->session),
                                status.has_my_info ? status.my_info.my_node_num : 0U,
                                &ui_traceroute);
    mesh_ui_store_set_traceroute(&app->ui_store, &ui_traceroute);

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

    /* Marking a conversation read touches nothing else, so watch the stamp for it. */
    if (app->ui_store.read_state.stamp != app->ui_read_state_stamp) {
        app->ui_read_state_stamp = app->ui_store.read_state.stamp;
        if (app->ui_handshake_cache_path[0] != '\0') {
            app->ui_handshake_cache_dirty = true;
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

/* Exponential backoff, shared by the two ways a connect can fail: the errno connect() handed
   back, and the failure that only surfaces later from tick(). Returns the delay it scheduled. */
static uint64_t mesh_app_backoff_autoconnect(struct mesh_app *app) {
    if (app->autoconnect_failures < 8U) {
        app->autoconnect_failures++;
    }
    uint64_t delay = (uint64_t)MESH_APP_AUTOCONNECT_RETRY_MS << (app->autoconnect_failures - 1U);
    if (delay > MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS) {
        delay = MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS;
    }
    app->autoconnect_retry_at_ms = mesh_app_now_ms() + delay;
    return delay;
}

void mesh_app_autoconnect(struct mesh_app *app) {
    if (app == NULL || app->autoconnect_disabled || app->autoconnect_held ||
        app->config.run_mode != MESH_APP_RUN_FOREGROUND) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    const bool link_up = (mesh_app_connected_identifier() != NULL);
    if (link_up) {
        /* An established link is the only proof an attempt worked, so it is the only thing that
           clears the backoff. */
        app->autoconnect_failures = 0U;
    }
    if (ble == NULL || link_up || mesh_app_link_connecting()) {
        return;
    }

    uint64_t now = mesh_app_now_ms();
    if (app->autoconnect_started_ms == 0U) {
        app->autoconnect_started_ms = now;
    }
    if (now < app->autoconnect_retry_at_ms) {
        return;
    }

    /*
     * A plugged-in node wins over anything on the air: it needs no pairing, has no range to
     * lose, and is almost certainly why the cable is there. BLE keeps its own policy below for
     * when nothing is plugged in.
     */
    struct mesh_serial_device_info ports[MESH_SERIAL_MAX_DEVICES];
    struct mesh_transport *serial = mesh_serial_transport();
    const size_t port_count =
        mesh_serial_transport_get_devices(serial, ports, MESH_SERIAL_MAX_DEVICES);
    if (port_count > 0U) {
        const struct mesh_serial_device_info *port = &ports[0];
        const char *preferred_port = app->config.preferred_serial_device;
        if (preferred_port[0] != '\0') {
            for (size_t i = 0; i < port_count; ++i) {
                if (strcmp(ports[i].id, preferred_port) == 0 ||
                    (ports[i].path[0] != '\0' && strcmp(ports[i].path, preferred_port) == 0)) {
                    port = &ports[i];
                    break;
                }
            }
        }
        const char *identifier = port->path[0] != '\0' ? port->path : port->id;
        const int serial_result =
            mesh_app_link_connect(app, identifier, (uint8_t)MESH_UI_DEVICE_SERIAL);
        if (serial_result == 0 || serial_result == -EALREADY || serial_result == -EINPROGRESS) {
            if (serial_result == 0) {
                mesh_log_info("app", "Auto-connecting to %s over USB (%s)", port->name, identifier);
            }
            /* Not a success yet: the handshake still has to go out. The counter stays where it
               is until a link is actually up. */
            app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
            return;
        }
        mesh_log_warn("app", "Auto-connect to %s over USB failed (%d); trying Bluetooth",
                      identifier, serial_result);
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
        /* Not a success yet: BLE only resolves its services a few seconds from now. */
        app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
        return;
    }
    if (result == -EAGAIN) {
        app->autoconnect_retry_at_ms = now + 1000U; /* transport not READY yet */
        return;
    }

    const uint64_t delay = mesh_app_backoff_autoconnect(app);
    mesh_log_warn("app", "Auto-connect to %s failed (%d); retrying in %llu ms", target->address,
                  result, (unsigned long long)delay);
}

bool mesh_app_report_link_errors(struct mesh_app *app) {
    if (app == NULL) {
        return false;
    }

    char link_error[MESH_TRANSPORT_ERROR_MAX];
    if (!mesh_transport_registry_take_error(&app->transport_registry, link_error,
                                            sizeof link_error)) {
        return false;
    }

    if (app->ui_report_link_error && app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
        mesh_log_info("ui", "Link failure shown to the user: %s", link_error);
        mesh_ui_store_set_toast(&app->ui_store, mesh_app_now_ms(), link_error);
    } else {
        mesh_log_debug("ui", "Link failure not shown (auto-connect): %s", link_error);
    }
    app->ui_report_link_error = false;

    /*
     * This is also the only honest failure signal auto-connect has. Its backoff keys off what
     * connect() returned, and a BLE connect returns 0 several seconds before it is a
     * connection - so a node that refuses every attempt looked like an unbroken run of
     * successes and got hammered every couple of seconds forever.
     */
    if (mesh_app_connected_identifier() == NULL) {
        (void)mesh_app_backoff_autoconnect(app);
    }
    return true;
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
    app->ui_report_link_error = false;
    app->autoconnect_disabled = mesh_app_env_disabled("MESHCLIENT_AUTOCONNECT");
    if (app->autoconnect_disabled) {
        mesh_log_info("app", "Auto-connect disabled by MESHCLIENT_AUTOCONNECT");
    }
    app->ui_handshake_cache_dirty = false;
    app->ui_read_state_stamp = 0U;

    if (mesh_ui_preferences_default_path(app->ui_preferences_path,
                                         sizeof(app->ui_preferences_path)) == 0) {
        int load_result = mesh_ui_preferences_load(&app->ui_preferences, app->ui_preferences_path);
        if (load_result == 0) {
            if (app->ui_preferences.preferred_device[0] != '\0') {
                if (app->ui_preferences.preferred_device_kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
                    if (app->config.preferred_serial_device[0] == '\0') {
                        snprintf(app->config.preferred_serial_device,
                                 sizeof app->config.preferred_serial_device, "%s",
                                 app->ui_preferences.preferred_device);
                    }
                } else if (app->config.preferred_ble_device[0] == '\0') {
                    snprintf(app->config.preferred_ble_device,
                             sizeof app->config.preferred_ble_device, "%s",
                             app->ui_preferences.preferred_device);
                }
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
    /* Restored read marks are already on disk; only later ones need a save. */
    app->ui_read_state_stamp = app->ui_store.read_state.stamp;

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

    /* Never fatal: a client that cannot update itself is still a working client, and the
       About section says why rather than offering a row that would do nothing. */
    (void)mesh_updater_init(&app->updater, &app->loop);
    /* After init, which zeroes the struct. A prefs file written before the setting existed
       reads as DEFAULT, so this is a no-op for anyone who has never picked a channel. */
    (void)mesh_updater_set_channel(&app->updater,
                                   (enum mesh_update_channel)app->ui_preferences.update_channel);
    /* Skipped when the environment already asked: an explicit override on the command line
       should not be quietly undone by a file written on some earlier run. */
    if (!app->updater.allow_dev_from_env) {
        (void)mesh_updater_set_allow_dev(&app->updater, app->ui_preferences.update_allow_dev);
    }

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

    result = mesh_transport_registry_register(&app->transport_registry, mesh_serial_transport());
    if (result < 0) {
        mesh_log_error("app", "Failed to register serial transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    /* One conversation for both links, so switching between them keeps the message log. */
    mesh_session_init(&app->session);
    mesh_transport_registry_set_session(&app->transport_registry, &app->session);

    return 0;
}

void mesh_app_shutdown(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    /* The transports are process-wide singletons but the session lives in `app`; leaving them
       pointed at it would dangle for anything that uses a transport after this. */
    mesh_transport_registry_set_session(&app->transport_registry, NULL);
    mesh_ui_input_shutdown(&app->ui_input);
    mesh_signals_shutdown(&app->signals);
    /* Before the loop goes: the updater has an fd registered with it, and a half-finished
       download to clean up. */
    mesh_updater_shutdown(&app->updater);
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
            /* The updater's child is watched by the event loop; this only enforces its
               timeout and reaps a child whose exit the loop did not see. */
            mesh_updater_tick(&app->updater, mesh_app_now_ms());
            /* Before auto-connect, not after: a retry starts the link over and clears the
               reason the last attempt failed. */
            (void)mesh_app_report_link_errors(app);
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
