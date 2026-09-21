#define _POSIX_C_SOURCE 200809L

/*
 * Pending UI edits -> admin writes.
 *
 * The firmware replaces a config section whole rather than merging fields, so a save is always
 * "the radio's own copy of the section, with our edits applied on top". That makes this the one
 * place where the UI's flat field ids meet nanopb's tagged unions, and the exact reverse of
 * mesh_app_flatten_settings() over in app_publish.c.
 */

#include "inkwell/base/array.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include "app_internal.h"

#include "mesh/i18n/strings.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

/*
 * What the two LoRa frequency rows will take.
 *
 * A ceiling rather than a band plan: the widest region this client offers is LORA_24, which is
 * the 2.4 GHz band, so anything past 3 GHz is a typo rather than a radio. The trim is a
 * crystal correction, and a megahertz either way is already far past any crystal that works.
 * Neither is a legality check - no number here can be one, which is what the rows' notes say.
 */
#define MESH_LORA_FREQUENCY_MAX_MHZ 3000
#define MESH_LORA_TRIM_MAX_HZ 1000000

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

/*
 * A KEY edit onto a ChannelSettings' psk: keep, the default-key shorthand, a new random key of
 * either size, none, or the bytes that were typed.
 *
 * Takes the submessage rather than the channel, because a ChannelSettings is not only ever a
 * channel: the mesh beacon embeds one as the channel it offers, and a key is filled in there
 * by exactly the same six answers. Extracted when the second caller arrived rather than copied,
 * which is what keeps the random sizes and the parse bound from being stated twice.
 */
static int apply_channel_key(meshtastic_ChannelSettings *settings,
                             const struct mesh_ui_setting_edit *edit) {
    switch ((enum mesh_ui_psk_choice)edit->number) {
    case MESH_UI_PSK_KEEP:
        return 0;
    case MESH_UI_PSK_DEFAULT:
        settings->psk.size = 1U;
        settings->psk.bytes[0] = 1U;
        return 0;
    case MESH_UI_PSK_RANDOM_128:
    case MESH_UI_PSK_RANDOM_256: {
        const size_t len = edit->number == MESH_UI_PSK_RANDOM_128 ? 16U : 32U;
        const int result = mesh_app_random_key(settings->psk.bytes, len);
        if (result < 0) {
            inkwell_log_error("ui", "No random bytes for a channel key: %d", result);
            return -EIO;
        }
        settings->psk.size = (pb_size_t)len;
        return 0;
    }
    case MESH_UI_PSK_NONE:
        settings->psk.size = 0U;
        return 0;
    case MESH_UI_PSK_TYPED: {
        size_t len = 0U;
        if (!mesh_ui_settings_key_parse(edit->text, settings->psk.bytes, sizeof settings->psk.bytes,
                                        &len)) {
            return -EINVAL;
        }
        settings->psk.size = (pb_size_t)len;
        return 0;
    }
    default:
        return -EINVAL;
    }
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
    meshtastic_ModuleConfig_DetectionSensorConfig *detect =
        &write->payload.module_config.payload_variant.detection_sensor;
    meshtastic_ModuleConfig_ExternalNotificationConfig *ext =
        &write->payload.module_config.payload_variant.external_notification;
    meshtastic_ModuleConfig_TrafficManagementConfig *traffic =
        &write->payload.module_config.payload_variant.traffic_management;
    meshtastic_ModuleConfig_MeshBeaconConfig *beacon =
        &write->payload.module_config.payload_variant.mesh_beacon;
    meshtastic_Config_PositionConfig *position = &write->payload.config.payload_variant.position;
    meshtastic_Config_PowerConfig *power = &write->payload.config.payload_variant.power;
    meshtastic_ChannelSettings *channel = &write->payload.channel.settings;
    meshtastic_Config_LoRaConfig *lora = &write->payload.config.payload_variant.lora;
    meshtastic_Config_SecurityConfig *security = &write->payload.config.payload_variant.security;
    meshtastic_DeviceUIConfig *ui = &write->payload.ui_config;
    const bool on = edit->number != 0U;

    /*
     * A flag row is one bit of a word, so it is set or cleared rather than assigned - and that
     * is the whole of what the kind costs here. Every FLAG field is handled by these four
     * lines; what is per-field is only *which* word, which is the switch below it.
     *
     * Ahead of the field switch rather than as ten case labels inside it, because the ten are
     * not ten writes: they are one write with a different mask, and a group upstream adds
     * later is an arm in flag_group_word() rather than another run of labels.
     */
    const uint32_t bit = mesh_ui_settings_field_bit((enum mesh_ui_setting_field)edit->field);
    if (bit != 0U) {
        uint32_t *word = NULL;
        switch (mesh_ui_settings_field_section((enum mesh_ui_setting_field)edit->field)) {
        case MESH_UI_SETTINGS_POSITION:
            word = &position->position_flags;
            break;
        case MESH_UI_SETTINGS_BEACON:
            word = &beacon->flags;
            break;
        default:
            return -ENOTSUP; /* a flag field whose group nothing here knows how to write */
        }
        *word = on ? (*word | bit) : (*word & ~bit);
        return 0;
    }

    /*
     * A broadcast target's rows are twelve fields over four copies of one shape, so they are
     * answered by arithmetic rather than by twelve case labels - which is the same argument the
     * flag block above makes: the twelve are not twelve writes, they are one write with a
     * different index. The group says where the run starts and how long it is, and
     * MESH_UI_BEACON_TARGET_FIELDS cuts it into records; a fifth target upstream adds is then a
     * count in the group table rather than three more labels here.
     */
    {
        const enum mesh_ui_setting_field first =
            mesh_ui_settings_group_field(MESH_UI_FIELD_GROUP_BEACON_TARGETS, 0U);
        const uint32_t run = mesh_ui_settings_group_count(MESH_UI_FIELD_GROUP_BEACON_TARGETS);
        const uint32_t field = (uint32_t)edit->field;
        if (first != MESH_UI_FIELD_NONE && field >= (uint32_t)first &&
            field < (uint32_t)first + run) {
            const uint32_t offset = field - (uint32_t)first;
            const uint32_t record = offset / MESH_UI_BEACON_TARGET_FIELDS;
            if (record >= INKWELL_ARRAY_LEN(beacon->broadcast_targets)) {
                return -ENOTSUP;
            }
            /* Editing the third row of a radio that only sent one target grows the list, with
               the records in between left as the zeroes they already are - and dropped again by
               the compaction, since a record that says nothing is not a destination. */
            for (pb_size_t i = beacon->broadcast_targets_count; i <= (pb_size_t)record; ++i) {
                memset(&beacon->broadcast_targets[i], 0, sizeof beacon->broadcast_targets[i]);
            }
            if ((pb_size_t)(record + 1U) > beacon->broadcast_targets_count) {
                beacon->broadcast_targets_count = (pb_size_t)(record + 1U);
            }
            meshtastic_ModuleConfig_MeshBeaconConfig_BroadcastTarget *target =
                &beacon->broadcast_targets[record];
            switch (offset % MESH_UI_BEACON_TARGET_FIELDS) {
            case 0U:
                /* Stored one past itself so the row can show "absent": see the store's note. */
                target->has_preset = edit->number != 0U;
                target->preset = (meshtastic_Config_LoRaConfig_ModemPreset)(edit->number != 0U
                                                                                ? edit->number - 1U
                                                                                : 0U);
                break;
            case 1U:
                target->region = (meshtastic_Config_LoRaConfig_RegionCode)edit->number;
                break;
            default:
                target->has_channel_index = edit->number != 0U;
                target->channel_index = edit->number != 0U ? edit->number - 1U : 0U;
                break;
            }
            return 0;
        }
    }

    switch ((enum mesh_ui_setting_field)edit->field) {
    case MESH_UI_FIELD_USER_LONG_NAME:
        inkwell_str_copy(owner->long_name, sizeof owner->long_name, edit->text);
        break;
    case MESH_UI_FIELD_USER_SHORT_NAME:
        inkwell_str_copy(owner->short_name, sizeof owner->short_name, edit->text);
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
        inkwell_str_copy(device->tzdef, sizeof device->tzdef, edit->text);
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
    case MESH_UI_FIELD_DISPLAY_OLED:
        display->oled = (meshtastic_Config_DisplayConfig_OledType)edit->number;
        break;
    case MESH_UI_FIELD_DISPLAY_MODE:
        display->displaymode = (meshtastic_Config_DisplayConfig_DisplayMode)edit->number;
        break;
    case MESH_UI_FIELD_DISPLAY_HEADING_BOLD:
        display->heading_bold = on;
        break;
    case MESH_UI_FIELD_DISPLAY_WAKE_ON_MOTION:
        display->wake_on_tap_or_motion = on;
        break;
    case MESH_UI_FIELD_DISPLAY_LONG_NAMES:
        display->use_long_node_name = on;
        break;
    case MESH_UI_FIELD_DISPLAY_MESSAGE_BUBBLES:
        display->enable_message_bubbles = on;
        break;
    case MESH_UI_FIELD_MQTT_ENABLED:
        mqtt->enabled = on;
        break;
    /*
     * The write that turns this client into the radio's route to the internet, or hands the job
     * back to the radio's own WiFi. Nothing else is needed on this side: the proxy is re-derived
     * from MQTTConfig on every loop turn, so the radio's reply to this write - which arrives as
     * the config sync after the reboot a settings write causes - is what starts or stops it.
     */
    case MESH_UI_FIELD_MQTT_PROXY:
        mqtt->proxy_to_client_enabled = on;
        break;
    case MESH_UI_FIELD_MQTT_ADDRESS:
        inkwell_str_copy(mqtt->address, sizeof mqtt->address, edit->text);
        break;
    case MESH_UI_FIELD_MQTT_USERNAME:
        inkwell_str_copy(mqtt->username, sizeof mqtt->username, edit->text);
        break;
    case MESH_UI_FIELD_MQTT_PASSWORD:
        inkwell_str_copy(mqtt->password, sizeof mqtt->password, edit->text);
        break;
    case MESH_UI_FIELD_MQTT_ROOT:
        inkwell_str_copy(mqtt->root, sizeof mqtt->root, edit->text);
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
        inkwell_str_copy(status->node_status, sizeof status->node_status, edit->text);
        break;
    case MESH_UI_FIELD_DETECT_ENABLED:
        detect->enabled = on;
        break;
    case MESH_UI_FIELD_DETECT_NAME:
        inkwell_str_copy(detect->name, sizeof detect->name, edit->text);
        break;
    case MESH_UI_FIELD_DETECT_MIN_BROADCAST:
        detect->minimum_broadcast_secs = edit->number;
        break;
    case MESH_UI_FIELD_DETECT_STATE_BROADCAST:
        detect->state_broadcast_secs = edit->number;
        break;
    case MESH_UI_FIELD_DETECT_SEND_BELL:
        detect->send_bell = on;
        break;
    case MESH_UI_FIELD_DETECT_PIN:
        detect->monitor_pin = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_DETECT_TRIGGER:
        detect->detection_trigger_type =
            (meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType)edit->number;
        break;
    case MESH_UI_FIELD_DETECT_PULLUP:
        detect->use_pullup = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ENABLED:
        ext->enabled = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ACTIVE:
        ext->active = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_OUTPUT_MS:
        ext->output_ms = edit->number;
        break;
    case MESH_UI_FIELD_EXTNOTIF_NAG:
        ext->nag_timeout = (uint16_t)edit->number;
        break;
    case MESH_UI_FIELD_EXTNOTIF_PWM:
        ext->use_pwm = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_I2S:
        ext->use_i2s_as_buzzer = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_PIN:
        ext->output = edit->number;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ALERT_MSG:
        ext->alert_message = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ALERT_BELL:
        ext->alert_bell = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA:
        ext->output_vibra = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA:
        ext->alert_message_vibra = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA:
        ext->alert_bell_vibra = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER:
        ext->output_buzzer = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER:
        ext->alert_message_buzzer = on;
        break;
    case MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER:
        ext->alert_bell_buzzer = on;
        break;
    case MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL:
        traffic->position_min_interval_secs = edit->number;
        break;
    case MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS:
        traffic->nodeinfo_direct_response_max_hops = edit->number;
        break;
    case MESH_UI_FIELD_TRAFFIC_RATE_WINDOW:
        traffic->rate_limit_window_secs = edit->number;
        break;
    case MESH_UI_FIELD_TRAFFIC_RATE_PACKETS:
        traffic->rate_limit_max_packets = edit->number;
        break;
    case MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD:
        traffic->unknown_packet_threshold = edit->number;
        break;
    case MESH_UI_FIELD_BEACON_INTERVAL:
        beacon->broadcast_interval_secs = edit->number;
        break;
    case MESH_UI_FIELD_BEACON_MESSAGE:
        inkwell_str_copy(beacon->broadcast_message, sizeof beacon->broadcast_message, edit->text);
        break;
    /* Either row of the offered channel brings the submessage with it - a ChannelSettings that
       is absent carries neither the name nor the key, the same pairing the two module_settings
       rows of a channel have. Whether it *stays* is decided after every edit has landed, by the
       name: see the block at the end of mesh_app_build_settings_write(). */
    case MESH_UI_FIELD_BEACON_OFFER_NAME:
        beacon->has_broadcast_offer_channel = true;
        inkwell_str_copy(beacon->broadcast_offer_channel.name,
                         sizeof beacon->broadcast_offer_channel.name, edit->text);
        break;
    case MESH_UI_FIELD_BEACON_OFFER_KEY:
        beacon->has_broadcast_offer_channel = true;
        return apply_channel_key(&beacon->broadcast_offer_channel, edit);
    case MESH_UI_FIELD_BEACON_OFFER_REGION:
        beacon->broadcast_offer_region = (meshtastic_Config_LoRaConfig_RegionCode)edit->number;
        break;
    case MESH_UI_FIELD_BEACON_OFFER_PRESET:
        beacon->has_broadcast_offer_preset = edit->number != 0U;
        beacon->broadcast_offer_preset =
            (meshtastic_Config_LoRaConfig_ModemPreset)(edit->number != 0U ? edit->number - 1U : 0U);
        break;
    case MESH_UI_FIELD_CHANNEL_NAME:
        inkwell_str_copy(channel->name, sizeof channel->name, edit->text);
        break;
    case MESH_UI_FIELD_CHANNEL_ROLE:
        write->payload.channel.role =
            on ? meshtastic_Channel_Role_SECONDARY : meshtastic_Channel_Role_DISABLED;
        break;
    case MESH_UI_FIELD_CHANNEL_KEY:
        return apply_channel_key(channel, edit);
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
    case MESH_UI_FIELD_CHANNEL_MUTED:
        /* The same submessage the row above writes, and it has to be marked present for either:
           a Channel whose module_settings is absent carries neither field. */
        channel->has_module_settings = true;
        channel->module_settings.is_muted = on;
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
    case MESH_UI_FIELD_LORA_BOOST_GAIN:
        lora->sx126x_rx_boosted_gain = on;
        break;
    case MESH_UI_FIELD_LORA_OVERRIDE_DUTY:
        lora->override_duty_cycle = on;
        break;
    case MESH_UI_FIELD_LORA_CHANNEL_NUM: {
        /* A slot is a whole number and the wire holds it in sixteen bits; a decimal parse at
           no places is what refuses "12.5" rather than reading it as 12. */
        int64_t slot = 0;
        if (!mesh_ui_settings_decimal_parse(edit->text, 0U, UINT16_MAX, &slot) || slot < 0) {
            return -EINVAL;
        }
        lora->channel_num = (uint16_t)slot;
        break;
    }
    case MESH_UI_FIELD_LORA_OVERRIDE_FREQ: {
        int64_t megahertz = 0;
        if (!mesh_ui_settings_decimal_parse(edit->text, MESH_UI_FREQUENCY_DIGITS,
                                            MESH_LORA_FREQUENCY_MAX_MHZ, &megahertz) ||
            megahertz < 0) {
            return -EINVAL;
        }
        lora->override_frequency = mesh_app_unscale_float(megahertz, MESH_UI_FREQUENCY_DIGITS);
        break;
    }
    case MESH_UI_FIELD_LORA_FREQUENCY_TRIM: {
        /* Signed, unlike every other number in this switch: a crystal is as likely to be fast
           as slow, and the row is the correction rather than the frequency. */
        int64_t hertz = 0;
        if (!mesh_ui_settings_decimal_parse(edit->text, MESH_UI_HERTZ_DIGITS, MESH_LORA_TRIM_MAX_HZ,
                                            &hertz)) {
            return -EINVAL;
        }
        lora->frequency_offset = mesh_app_unscale_float(hertz, MESH_UI_HERTZ_DIGITS);
        break;
    }
    /*
     * The three ignore slots. Each writes its own index and the whole field is compacted after
     * the loop, the way a cleared admin key is: a repeated field with a hole in the middle is
     * a list the firmware reads as shorter than it is.
     */
    case MESH_UI_FIELD_LORA_IGNORE_NODE_0:
    case MESH_UI_FIELD_LORA_IGNORE_NODE_1:
    case MESH_UI_FIELD_LORA_IGNORE_NODE_2: {
        const pb_size_t slot =
            (pb_size_t)((enum mesh_ui_setting_field)edit->field - MESH_UI_FIELD_LORA_IGNORE_NODE_0);
        uint32_t node = 0U;
        if (!mesh_ui_settings_node_id_parse(edit->text, &node)) {
            return -EINVAL;
        }
        if (lora->ignore_incoming_count < slot + 1U) {
            /* A slot written past the end of what the radio sent: the ones before it stay 0
               and are closed up by the compaction below. */
            lora->ignore_incoming_count = slot + 1U;
        }
        lora->ignore_incoming[slot] = node;
        break;
    }
    /* Ham mode's rows never reach here: mesh_ui_settings_field_consumer() files them under
       MESH_UI_SETTING_CONSUMER_HAM_MODE, and mesh_app_save_ham_mode() is what reads them. */
    case MESH_UI_FIELD_LORA_HAM_CALL_SIGN:
    case MESH_UI_FIELD_LORA_HAM_FREQUENCY:
    case MESH_UI_FIELD_LORA_HAM_TX_POWER:
        return -ENOTSUP;
    case MESH_UI_FIELD_SECURITY_PRIVATE_KEY:
        switch ((enum mesh_ui_psk_choice)edit->number) {
        case MESH_UI_PSK_KEEP:
            break;
        case MESH_UI_PSK_RANDOM_256: {
            const int result = mesh_app_random_key(security->private_key.bytes, 32U);
            if (result < 0) {
                inkwell_log_error("ui", "No random bytes for a private key: %d", result);
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
    case MESH_UI_FIELD_UI_THEME:
        ui->theme = (meshtastic_Theme)edit->number;
        break;
    case MESH_UI_FIELD_UI_BRIGHTNESS:
        ui->screen_brightness = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_UI_SCREEN_TIMEOUT:
        ui->screen_timeout = (uint16_t)edit->number;
        break;
    case MESH_UI_FIELD_UI_ALERT:
        ui->alert_enabled = on;
        break;
    case MESH_UI_FIELD_UI_BANNER:
        ui->banner_enabled = on;
        break;
    case MESH_UI_FIELD_UI_RING_TONE:
        ui->ring_tone_id = (uint8_t)edit->number;
        break;
    case MESH_UI_FIELD_UI_COMPASS_MODE:
        ui->compass_mode = (meshtastic_CompassMode)edit->number;
        break;
    case MESH_UI_FIELD_UI_GPS_FORMAT:
        ui->gps_format = (meshtastic_DeviceUIConfig_GpsCoordinateFormat)edit->number;
        break;
    case MESH_UI_FIELD_UI_CLOCKFACE:
        ui->is_clockface_analog = on;
        break;
    /* The canned slots are not applied one at a time: the wire carries the whole list as one
       string, so the six of them plus whatever the radio holds beyond them are assembled
       together in mesh_app_build_settings_write. Named here so the default arm's warning
       stays about fields nobody has wired up. */
    case MESH_UI_FIELD_CANNED_0:
    case MESH_UI_FIELD_CANNED_1:
    case MESH_UI_FIELD_CANNED_2:
    case MESH_UI_FIELD_CANNED_3:
    case MESH_UI_FIELD_CANNED_4:
    case MESH_UI_FIELD_CANNED_5:
        break;
    default:
        inkwell_log_warn("ui", "Ignoring edit to unknown settings field %u", (unsigned)edit->field);
        break;
    }
    return 0;
}

/*
 * The canned message list, assembled from the radio's own copy and the section's edits.
 *
 * Three things this has to get right, and all three are about not losing somebody's messages.
 * A radio holding more entries than the section has rows keeps them: the walk runs to whichever
 * is longer, and an entry past the last slot is copied across as it arrived. Empty slots are
 * skipped rather than written as empty entries, the same gap-closing a cleared admin key gets,
 * so emptying the third of six does not leave a blank quick reply behind it.
 *
 * And it can fail. The slot count and the per-slot cap are chosen together so the six rows
 * always fit the wire's 200 bytes, but a radio holding *more* than six can have a tail long
 * enough that lengthening a visible message pushes it over - and a join that stopped at the cap
 * would send a list with that tail missing, which the radio would take as a deletion. So the
 * overflow is reported and the save refused: false here is -E2BIG at the caller and a toast,
 * not a shorter list. Refusing is the whole point - the alternative is a save that silently
 * deletes messages this screen never showed.
 */
static bool mesh_app_build_canned_list(const char *held, const struct mesh_ui_action *action,
                                       char *out, size_t out_len) {
    out[0] = '\0';
    size_t used = 0U;
    uint32_t entries = mesh_ui_settings_canned_count(held);
    if (entries < MESH_UI_CANNED_SLOTS) {
        entries = MESH_UI_CANNED_SLOTS;
    }
    for (uint32_t i = 0; i < entries; ++i) {
        /* Sized from the wire rather than from the edit buffer: an entry this screen cannot
           show is still carried across, and a radio whose one message is longer than a slot
           must not have it cut down by a save that was not about it. */
        char text[MESH_UI_CANNED_MESSAGES_MAX];
        mesh_ui_settings_canned_entry(held, i, text, sizeof text);
        if (i < MESH_UI_CANNED_SLOTS) {
            const enum mesh_ui_setting_field field =
                (enum mesh_ui_setting_field)(MESH_UI_FIELD_CANNED_0 + i);
            for (uint8_t e = 0; e < action->edit_count && e < MESH_UI_SETTINGS_EDITS_MAX; ++e) {
                if ((enum mesh_ui_setting_field)action->edits[e].field == field) {
                    inkwell_str_copy(text, sizeof text, action->edits[e].text);
                    break;
                }
            }
        }
        if (text[0] == '\0') {
            continue;
        }
        const size_t sep = (used > 0U) ? 1U : 0U;
        const size_t len = strlen(text);
        if (used + sep + len >= out_len) {
            return false;
        }
        if (sep != 0U) {
            out[used++] = '|';
        }
        memcpy(out + used, text, len);
        used += len;
        out[used] = '\0';
    }
    return true;
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
    case MESH_UI_SETTINGS_DETECTION:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_DETECTIONSENSOR_CONFIG;
        return true;
    case MESH_UI_SETTINGS_EXT_NOTIFICATION:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_EXTNOTIF_CONFIG;
        return true;
    case MESH_UI_SETTINGS_TRAFFIC:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_TRAFFICMANAGEMENT_CONFIG;
        return true;
    case MESH_UI_SETTINGS_BEACON:
        *out_type = meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG;
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
        case MESH_UI_SETTINGS_RADIO_UI:
            if (!radio->has_ui_config) {
                return -ENOENT;
            }
            /* The radio's whole DeviceUIConfig, not a fresh one with our rows in it: it also
               carries a touchscreen calibration and a map home point that this client has no
               rows for and could not reconstruct. */
            out->kind = MESH_ADMIN_SET_UI_CONFIG;
            out->payload.ui_config = radio->ui_config;
            break;
        case MESH_UI_SETTINGS_CANNED:
            if (!radio->has_canned_messages) {
                return -ENOENT;
            }
            out->kind = MESH_ADMIN_SET_CANNED_MESSAGES;
            if (!mesh_app_build_canned_list(radio->canned_messages, action, out->payload.text,
                                            sizeof out->payload.text)) {
                /* The edits plus the entries the screen could not show do not fit the wire.
                   Refused rather than truncated: a shorter list is a deletion. */
                return -E2BIG;
            }
            /* Assembled whole above rather than field by field below, so there is nothing for
               the edit loop to apply. */
            return 0;
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
            /*
             * "Clear this slot", which is a write of the same shape with nothing carried over.
             *
             * Assembled whole and returned here rather than expressed as edits, for the reason
             * the canned list is: what goes out is not the radio's channel with three fields
             * changed but an empty one, and every field of ChannelSettings is meant to go -
             * `id` and the two MQTT flags included. Starting from the radio's copy the way a
             * save does would carry whichever of them this client has no row for.
             *
             * DISABLED plus a zeroed ChannelSettings is what the phone apps write for an unused
             * slot, so a table this leaves behind is one they read as the same eight slots we
             * do. Returning early is what keeps the edit loop below off it: a name typed a
             * moment ago is not something to send on the way to clearing the name.
             */
            if ((enum mesh_ui_settings_action)action->number ==
                MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
                out->payload.channel.role = meshtastic_Channel_Role_DISABLED;
                out->payload.channel.settings =
                    (meshtastic_ChannelSettings)meshtastic_ChannelSettings_init_default;
                return 0;
            }
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
        out->payload.config.which_payload_variant == meshtastic_Config_lora_tag) {
        /* ignore_incoming is repeated, so an emptied slot is closed up rather than left as a
           0 the firmware would read as a node number. The same compaction the admin keys get
           below, and for the same reason. */
        meshtastic_Config_LoRaConfig *lora = &out->payload.config.payload_variant.lora;
        pb_size_t kept = 0U;
        for (pb_size_t i = 0; i < lora->ignore_incoming_count && i < 3U; ++i) {
            if (lora->ignore_incoming[i] == 0U) {
                continue;
            }
            lora->ignore_incoming[kept++] = lora->ignore_incoming[i];
        }
        for (pb_size_t i = kept; i < 3U; ++i) {
            lora->ignore_incoming[i] = 0U;
        }
        lora->ignore_incoming_count = kept;
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
    if (out->kind == MESH_ADMIN_SET_MODULE_CONFIG &&
        out->payload.module_config.which_payload_variant ==
            meshtastic_ModuleConfig_mesh_beacon_tag) {
        /*
         * broadcast_targets is repeated, and a record whose three rows all read "whatever the
         * radio is running" names no destination at all - it is the empty slot the section lists
         * so a target can be added to it. So it is dropped rather than sent, the same compaction
         * a cleared admin key or ignore slot gets, and for the same reason: the rows are fixed
         * and the entries are not.
         */
        meshtastic_ModuleConfig_MeshBeaconConfig *beacon =
            &out->payload.module_config.payload_variant.mesh_beacon;
        pb_size_t kept = 0U;
        for (pb_size_t i = 0; i < beacon->broadcast_targets_count &&
                              i < INKWELL_ARRAY_LEN(beacon->broadcast_targets);
             ++i) {
            const meshtastic_ModuleConfig_MeshBeaconConfig_BroadcastTarget *target =
                &beacon->broadcast_targets[i];
            if (!target->has_preset && !target->has_channel_index &&
                target->region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
                continue;
            }
            if (kept != i) {
                beacon->broadcast_targets[kept] = beacon->broadcast_targets[i];
            }
            kept++;
        }
        for (pb_size_t i = kept; i < INKWELL_ARRAY_LEN(beacon->broadcast_targets); ++i) {
            memset(&beacon->broadcast_targets[i], 0, sizeof beacon->broadcast_targets[i]);
        }
        beacon->broadcast_targets_count = kept;
        /*
         * And the offer, by the same rule one level up: an invitation with no name on it is not
         * an invitation, so the submessage goes rather than travelling as a bare key.
         *
         * Decided here rather than in the row's own arm because a save carries several edits in
         * whatever order the user made them, and a name emptied before the key was touched has
         * to mean the same as one emptied after it. The row's note promises this - "leave it
         * empty to offer no channel" - and the promise is the screen's, so it is kept against
         * the assembled record rather than against one edit.
         */
        if (beacon->has_broadcast_offer_channel &&
            beacon->broadcast_offer_channel.name[0] == '\0') {
            beacon->has_broadcast_offer_channel = false;
            memset(&beacon->broadcast_offer_channel, 0, sizeof beacon->broadcast_offer_channel);
        }
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
                                    inkcell_str(MESH_STR_TOAST_COORDS_NOT_NUMBERS));
            return;
        }
        if (latitude == 0 && longitude == 0) {
            /* Null Island is where an empty form lands, not where anybody is. */
            mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_NEED_COORDS));
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
                 inkcell_str(clearing ? MESH_STR_SAVE_SECTION_FIXED_POS
                                      : MESH_STR_SAVE_SECTION_POSITION));
        /* The GPS rows are saved with Y and stay pending until it is pressed. */
        mesh_ui_store_settings_edits_consumed(&app->ui_store,
                                              MESH_UI_SETTING_CONSUMER_FIXED_POSITION);
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_WORKING_ELLIPSIS,
                           inkcell_str(clearing ? MESH_STR_TOAST_CLEARING_FIXED_POS
                                                : MESH_STR_TOAST_PINNING_POSITION));
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED_KEPT));
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_ON_EARTH));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FAILED_KEPT, result);
        inkwell_log_warn("ui", "Fixed position write failed: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/*
 * "Switch to ham mode". The fixed-position shape one section over: a row that reads the three
 * rows above it, because `set_ham_mode` is one verb over three things this tab keeps apart.
 *
 * An action rather than a write, so nothing is read back and this asks for a refresh instead -
 * the same answer a restore gets, and for the same reason: what moved is the owner record, the
 * primary channel and LoRaConfig at once, and a screen left drawing what it had would show a
 * node that is no longer the one in front of it.
 *
 * An untouched row keeps what the radio reported, so a licensed node already on its band is
 * re-sent the values it has rather than zeros.
 */
void mesh_app_save_ham_mode(struct mesh_app *app, const struct mesh_ui_action *action,
                            uint64_t now) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const struct mesh_ui_settings *ui = &app->ui_store.settings;
    char call_sign[MESH_UI_SETTING_TEXT_MAX];
    inkwell_str_copy(call_sign, sizeof call_sign, ui->is_licensed ? ui->long_name : "");
    int64_t frequency = ui->override_frequency_scaled;
    int32_t tx_power = ui->tx_power;
    bool bad = false;

    for (uint8_t i = 0; i < action->edit_count && i < MESH_UI_SETTINGS_EDITS_MAX; ++i) {
        const struct mesh_ui_setting_edit *edit = &action->edits[i];
        switch ((enum mesh_ui_setting_field)edit->field) {
        case MESH_UI_FIELD_LORA_HAM_CALL_SIGN:
            inkwell_str_copy(call_sign, sizeof call_sign, edit->text);
            break;
        case MESH_UI_FIELD_LORA_HAM_FREQUENCY:
            bad = bad || !mesh_ui_settings_decimal_parse(edit->text, MESH_UI_FREQUENCY_DIGITS,
                                                         MESH_LORA_FREQUENCY_MAX_MHZ, &frequency);
            break;
        case MESH_UI_FIELD_LORA_HAM_TX_POWER:
            tx_power = (int32_t)(int8_t)(uint8_t)edit->number;
            break;
        default:
            /* Everything else in this section is saved with Y, not with this row. */
            break;
        }
    }
    if (bad || frequency < 0) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_HAM_BAD_FREQUENCY));
        return;
    }
    /* Refused here rather than sent: without a call sign the firmware would rename the node to
       nothing and still take the primary channel's key off, which is the failure this mode's
       whole warning is about. */
    if (call_sign[0] == '\0') {
        mesh_ui_store_set_toast(&app->ui_store, now,
                                inkcell_str(MESH_STR_TOAST_HAM_NEED_CALL_SIGN));
        return;
    }

    const int result = mesh_session_set_ham_mode(
        &app->session, call_sign, mesh_app_unscale_float(frequency, MESH_UI_FREQUENCY_DIGITS),
        tx_power);
    if (result > 0) {
        mesh_ui_store_settings_edits_consumed(&app->ui_store, MESH_UI_SETTING_CONSUMER_HAM_MODE);
        (void)mesh_session_refresh_settings(&app->session);
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_HAM_SWITCHING, call_sign);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED_KEPT));
    } else if (result == -EBUSY) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_ALREADY_REQUESTED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FAILED_KEPT, result);
        inkwell_log_warn("ui", "Ham mode write failed: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

void mesh_app_save_settings(struct mesh_app *app, const struct mesh_ui_action *action,
                            uint64_t now) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    char section_label[MESH_UI_SETTINGS_LABEL_MAX];
    if ((enum mesh_ui_settings_section)action->section == MESH_UI_SETTINGS_CHANNELS &&
        action->channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        inkcell_str_format(section_label, sizeof section_label, MESH_STR_SAVE_SECTION_CHANNEL,
                           (unsigned)action->channel);
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
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SAVING_SECTION, section_name);
        inkwell_log_info("ui", "Saving %s: %u edits, %d admin requests", section_name,
                         (unsigned)action->edit_count, result);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED_KEPT));
    } else if (result == -ENOENT) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SECTION_NOT_LOADED, section_name);
    } else if (result == -ENOTSUP) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SECTION_READ_ONLY, section_name);
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_INVALID_VALUE));
    } else if (result == -E2BIG) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_TOO_LONG_KEPT));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SAVE_FAILED_KEPT, result);
        inkwell_log_warn("ui", "Saving %s failed: %d", section_name, result);
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
    const uint64_t now = inkwell_time_monotonic_ms();
    if (radio != NULL && radio->writes_failed > app->settings_writes_failed_seen) {
        switch (radio->last_write_error) {
        case meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY:
            inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SAVE_SESSION_EXPIRED,
                               app->settings_save_section);
            break;
        case meshtastic_Routing_Error_BAD_REQUEST:
            inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SAVE_BAD_VALUE,
                               app->settings_save_section);
            break;
        case MESH_RADIO_SETTINGS_WRITE_TIMEOUT:
            inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SAVE_NO_REPLY,
                               app->settings_save_section);
            break;
        default:
            inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SAVE_REJECTED,
                               app->settings_save_section, (int)radio->last_write_error);
            break;
        }
        inkwell_log_warn("ui", "Save of %s failed: error %d", app->settings_save_section,
                         (int)radio->last_write_error);
    } else if (radio != NULL && radio->writes_acked > app->settings_writes_acked_seen) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SAVED_MAY_RESTART,
                           app->settings_save_section);
        inkwell_log_info("ui", "Save of %s acknowledged", app->settings_save_section);
    } else if (!link_connected) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_RESTARTING_APPLY));
        inkwell_log_info("ui", "Link dropped while saving %s; assuming reboot",
                         app->settings_save_section);
    } else {
        return; /* still waiting */
    }
    app->settings_save_pending = false;
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}
