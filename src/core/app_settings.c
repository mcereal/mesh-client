#define _POSIX_C_SOURCE 200809L

/*
 * Pending UI edits -> admin writes.
 *
 * The firmware replaces a config section whole rather than merging fields, so a save is always
 * "the radio's own copy of the section, with our edits applied on top". That makes this the one
 * place where the UI's flat field ids meet nanopb's tagged unions, and the exact reverse of
 * mesh_app_flatten_settings() over in app_publish.c.
 */

#include "app_internal.h"

#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

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
    meshtastic_ModuleConfig_NeighborInfoConfig *neighbor =
        &write->payload.module_config.payload_variant.neighbor_info;
    meshtastic_ModuleConfig_RangeTestConfig *range_test =
        &write->payload.module_config.payload_variant.range_test;
    meshtastic_ModuleConfig_PaxcounterConfig *pax =
        &write->payload.module_config.payload_variant.paxcounter;
    meshtastic_ModuleConfig_TAKConfig *tak = &write->payload.module_config.payload_variant.tak;
    meshtastic_ModuleConfig_AmbientLightingConfig *ambient =
        &write->payload.module_config.payload_variant.ambient_lighting;
    meshtastic_ModuleConfig_StatusMessageConfig *status =
        &write->payload.module_config.payload_variant.statusmessage;
    meshtastic_Config_PositionConfig *position = &write->payload.config.payload_variant.position;
    meshtastic_Config_PowerConfig *power = &write->payload.config.payload_variant.power;
    meshtastic_ChannelSettings *channel = &write->payload.channel.settings;
    meshtastic_Config_LoRaConfig *lora = &write->payload.config.payload_variant.lora;
    meshtastic_Config_SecurityConfig *security = &write->payload.config.payload_variant.security;
    const bool on = edit->number != 0U;

    switch ((enum mesh_ui_setting_field)edit->field) {
    case MESH_UI_FIELD_USER_LONG_NAME:
        mesh_str_copy(owner->long_name, sizeof owner->long_name, edit->text);
        break;
    case MESH_UI_FIELD_USER_SHORT_NAME:
        mesh_str_copy(owner->short_name, sizeof owner->short_name, edit->text);
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
        mesh_str_copy(device->tzdef, sizeof device->tzdef, edit->text);
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
        mesh_str_copy(mqtt->address, sizeof mqtt->address, edit->text);
        break;
    case MESH_UI_FIELD_MQTT_USERNAME:
        mesh_str_copy(mqtt->username, sizeof mqtt->username, edit->text);
        break;
    case MESH_UI_FIELD_MQTT_PASSWORD:
        mesh_str_copy(mqtt->password, sizeof mqtt->password, edit->text);
        break;
    case MESH_UI_FIELD_MQTT_ROOT:
        mesh_str_copy(mqtt->root, sizeof mqtt->root, edit->text);
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
    /* map_report_settings is a submessage, so its presence flag has to be set: nanopb would
       otherwise drop the whole thing from the wire and the firmware would keep its old copy. */
    case MESH_UI_FIELD_MQTT_MAP_INTERVAL:
        mqtt->has_map_report_settings = true;
        mqtt->map_report_settings.publish_interval_secs = edit->number;
        break;
    case MESH_UI_FIELD_MQTT_MAP_PRECISION:
        mqtt->has_map_report_settings = true;
        mqtt->map_report_settings.position_precision = edit->number;
        break;
    case MESH_UI_FIELD_MQTT_MAP_LOCATION:
        mqtt->has_map_report_settings = true;
        mqtt->map_report_settings.should_report_location = on;
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
    case MESH_UI_FIELD_SF_RECORDS:
        sf->records = edit->number;
        break;
    case MESH_UI_FIELD_SF_HISTORY_MAX:
        sf->history_return_max = edit->number;
        break;
    case MESH_UI_FIELD_SF_HISTORY_WINDOW:
        sf->history_return_window = edit->number;
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
    case MESH_UI_FIELD_TELEMETRY_ENV_INTERVAL:
        telemetry->environment_update_interval = edit->number;
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
    case MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL:
        telemetry->air_quality_interval = edit->number;
        break;
    case MESH_UI_FIELD_TELEMETRY_AIR_SCREEN:
        telemetry->air_quality_screen_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_POWER:
        telemetry->power_measurement_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_POWER_INTERVAL:
        telemetry->power_update_interval = edit->number;
        break;
    case MESH_UI_FIELD_TELEMETRY_POWER_SCREEN:
        telemetry->power_screen_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_HEALTH:
        telemetry->health_measurement_enabled = on;
        break;
    case MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL:
        telemetry->health_update_interval = edit->number;
        break;
    case MESH_UI_FIELD_TELEMETRY_HEALTH_SCREEN:
        telemetry->health_screen_enabled = on;
        break;
    case MESH_UI_FIELD_NEIGHBOR_ENABLED:
        neighbor->enabled = on;
        break;
    case MESH_UI_FIELD_NEIGHBOR_INTERVAL:
        neighbor->update_interval = edit->number;
        break;
    case MESH_UI_FIELD_NEIGHBOR_OVER_LORA:
        neighbor->transmit_over_lora = on;
        break;
    case MESH_UI_FIELD_RANGE_TEST_ENABLED:
        range_test->enabled = on;
        break;
    case MESH_UI_FIELD_RANGE_TEST_SENDER:
        range_test->sender = edit->number;
        break;
    case MESH_UI_FIELD_RANGE_TEST_SAVE:
        range_test->save = on;
        break;
    case MESH_UI_FIELD_RANGE_TEST_CLEAR:
        range_test->clear_on_reboot = on;
        break;
    case MESH_UI_FIELD_PAX_ENABLED:
        pax->enabled = on;
        break;
    case MESH_UI_FIELD_PAX_INTERVAL:
        pax->paxcounter_update_interval = edit->number;
        break;
    /* Back through the cast the preset table stores these in; the wire field is int32. */
    case MESH_UI_FIELD_PAX_WIFI_THRESHOLD:
        pax->wifi_threshold = (int32_t)edit->number;
        break;
    case MESH_UI_FIELD_PAX_BLE_THRESHOLD:
        pax->ble_threshold = (int32_t)edit->number;
        break;
    case MESH_UI_FIELD_TAK_TEAM:
        tak->team = (meshtastic_Team)edit->number;
        break;
    case MESH_UI_FIELD_TAK_ROLE:
        tak->role = (meshtastic_MemberRole)edit->number;
        break;
    case MESH_UI_FIELD_AMBIENT_LED:
        ambient->led_state = on;
        break;
    case MESH_UI_FIELD_AMBIENT_CURRENT:
        ambient->current = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_AMBIENT_RED:
        ambient->red = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_AMBIENT_GREEN:
        ambient->green = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_AMBIENT_BLUE:
        ambient->blue = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_STATUS_TEXT:
        mesh_str_copy(status->node_status, sizeof status->node_status, edit->text);
        break;
    case MESH_UI_FIELD_CHANNEL_NAME:
        mesh_str_copy(channel->name, sizeof channel->name, edit->text);
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
/*
 * Which module a settings section is, as an admin ModuleConfigType.
 *
 * This is the one place the UI's sections meet the wire's module ids, and it is all that is
 * left of what used to be nine write arms: the union tag, the storage and its size come from
 * the module table in radio_settings.c, so the type and the tag can no longer be typed apart
 * and disagree. A section that is not a module answers 0 and falls through to the switch.
 */
static bool module_admin_type(enum mesh_ui_settings_section section, uint32_t *out_type) {
    switch (section) {
    case MESH_UI_SETTINGS_MQTT:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG;
        return true;
    case MESH_UI_SETTINGS_STORE_FORWARD:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG;
        return true;
    case MESH_UI_SETTINGS_TELEMETRY:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG;
        return true;
    case MESH_UI_SETTINGS_NEIGHBOR_INFO:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_NEIGHBORINFO_CONFIG;
        return true;
    case MESH_UI_SETTINGS_RANGE_TEST:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_RANGETEST_CONFIG;
        return true;
    case MESH_UI_SETTINGS_PAXCOUNTER:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_PAXCOUNTER_CONFIG;
        return true;
    case MESH_UI_SETTINGS_TAK:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_TAK_CONFIG;
        return true;
    case MESH_UI_SETTINGS_AMBIENT:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_AMBIENTLIGHTING_CONFIG;
        return true;
    case MESH_UI_SETTINGS_STATUS_MESSAGE:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_STATUSMESSAGE_CONFIG;
        return true;
    default:
        return false;
    }
}

int mesh_app_build_settings_write(const struct mesh_radio_settings *radio,
                                  const struct mesh_ui_action *action,
                                  struct mesh_admin_request *out) {
    if (radio == NULL || action == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);

    /* Every module goes out the same way: the section names the type, the table supplies the
       tag and the bytes. -ENOENT when the radio has not sent that section, exactly as the
       hand-written arms reported it. */
    uint32_t admin_type = 0U;
    if (module_admin_type((enum mesh_ui_settings_section)action->section, &admin_type)) {
        const struct mesh_module_binding *binding = mesh_radio_module_for_type(admin_type);
        if (binding == NULL) {
            return -ENOTSUP;
        }
        if (!mesh_radio_module_load(radio, binding, &out->payload.module_config)) {
            return -ENOENT;
        }
        out->kind = MESH_ADMIN_SET_MODULE_CONFIG;
        out->type = admin_type;
    } else {
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
void mesh_app_save_fixed_position(struct mesh_app *app, const struct mesh_ui_action *action,
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

void mesh_app_save_settings(struct mesh_app *app, const struct mesh_ui_action *action,
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
void mesh_app_track_settings_save(struct mesh_app *app, const struct mesh_radio_settings *radio,
                                  bool link_connected) {
    if (!app->settings_save_pending) {
        return;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = mesh_time_monotonic_ms();
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
