#define _POSIX_C_SOURCE 200809L

/* App glue: auto-connect policy, link routing, and settings writes built from UI state. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"
#include "support/serial_fixture.h"

#include "mesh/core/app.h"
#include "mesh/core/config.h"
#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/serial_usb.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The clock mesh_app_publish_ui_state stamps its toasts with. */
static uint64_t test_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* The foreground policy: a saved preferred node wins even when a stronger one is in range;
   with nothing saved, the strongest advertiser is used. */
MESH_TEST_CASE(app_autoconnect_policy, unit) {
    const char *failure = NULL;

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -30, .paired = true},
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = -70, .paired = true},
    };

    uint8_t write_capture[64];
    size_t write_len = 0U;
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 2U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    /* Keep the app's preference files out of the real $HOME. */
    char home_dir[] = "/tmp/mesh_app_autoconnectXXXXXX";
    if (mkdtemp(home_dir) == NULL) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "mkdtemp failed");
        return;
    }
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    /* This test is about BLE ranking; a USB port on the build host now outranks every
       advertiser, so keep the serial link out of it. */
    config.enable_serial = false;
    snprintf(config.preferred_ble_device, sizeof config.preferred_ble_device, "%s", "NodeSeven");

    struct mesh_app app;
    memset(&app, 0, sizeof app);
    bool app_ready = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    mesh_app_autoconnect(&app);
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "preferred node (by name) should win over a stronger one";
        goto cleanup;
    }

    /* Drop the link and the preference: the strongest node should be chosen next. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    app.config.preferred_ble_device[0] = '\0';
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[0].address) != 0) {
        failure = "strongest node should be chosen without a preference";
        goto cleanup;
    }

    /* Already connected: another turn must be a no-op rather than a reconnect. */
    write_len = 0U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (write_len != 0U) {
        failure = "autoconnect should not act while connected";
        goto cleanup;
    }

    /* Not in foreground mode it must never connect. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "second disconnect failed";
        goto cleanup;
    }
    app.config.run_mode = MESH_APP_RUN_SINGLE_POLL;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "single-poll mode must not auto-connect";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(app_settings_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_owner = true;
    snprintf(radio.owner.long_name, sizeof radio.owner.long_name, "%s", "Old Name");
    snprintf(radio.owner.short_name, sizeof radio.owner.short_name, "%s", "OLDN");
    radio.owner.public_key.size = 32U;
    radio.owner.public_key.bytes[0] = 0x42U;
    radio.has_telemetry = true;
    radio.telemetry.device_update_interval = 900U;
    radio.telemetry.environment_measurement_enabled = true;
    radio.telemetry.power_update_interval = 777U;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_USER;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_USER_LONG_NAME;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "Brick");
    action.edits[1].field = MESH_UI_FIELD_USER_UNMESSAGEABLE;
    action.edits[1].number = 1U;
    action.edits[2].field = MESH_UI_FIELD_DISPLAY_FLIP; /* wrong section: ignored */
    action.edits[2].number = 1U;

    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_OWNER ||
                          strcmp(write.payload.owner.long_name, "Brick") != 0 ||
                          strcmp(write.payload.owner.short_name, "OLDN") != 0 ||
                          !write.payload.owner.has_is_unmessagable ||
                          !write.payload.owner.is_unmessagable ||
                          write.payload.owner.public_key.size != 32U ||
                          write.payload.owner.public_key.bytes[0] != 0x42U,
                      "set_owner should be the radio's user plus the edits");

    action.section = MESH_UI_SETTINGS_TELEMETRY;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_TELEMETRY_INTERVAL;
    action.edits[0].number = 3600U;
    action.edits[1].field = MESH_UI_FIELD_TELEMETRY_DEVICE;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG ||
            write.payload.module_config.which_payload_variant !=
                meshtastic_ModuleConfig_telemetry_tag ||
            write.payload.module_config.payload_variant.telemetry.device_update_interval != 3600U ||
            !write.payload.module_config.payload_variant.telemetry.device_telemetry_enabled ||
            !write.payload.module_config.payload_variant.telemetry
                 .environment_measurement_enabled ||
            write.payload.module_config.payload_variant.telemetry.power_update_interval != 777U,
        "set_module_config should keep the fields we do not show");

    radio.has_device = true;
    radio.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
    radio.device.node_info_broadcast_secs = 10800U;
    action.section = MESH_UI_SETTINGS_DEVICE;
    action.edit_count = 1U;
    action.edits[0].field = MESH_UI_FIELD_DEVICE_TZDEF;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "AST4");
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_CONFIG ||
            write.type != meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG ||
            write.payload.config.which_payload_variant != meshtastic_Config_device_tag ||
            strcmp(write.payload.config.payload_variant.device.tzdef, "AST4") != 0 ||
            write.payload.config.payload_variant.device.role !=
                meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE ||
            write.payload.config.payload_variant.device.node_info_broadcast_secs != 10800U,
        "set_device_config should carry the timezone and the rest");

    /* Role is an ordinary enum edit, and the LED row is the one field the UI shows inverted:
       "LED heartbeat on" has to become led_heartbeat_disabled = false. */
    radio.device.led_heartbeat_disabled = true;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_DEVICE_ROLE;
    action.edits[0].number = (uint32_t)meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
    action.edits[1].field = MESH_UI_FIELD_DEVICE_LED_HEARTBEAT;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.config.payload_variant.device.role !=
                              meshtastic_Config_DeviceConfig_Role_ROUTER_LATE ||
                          write.payload.config.payload_variant.device.led_heartbeat_disabled,
                      "the device role or the inverted LED row was not applied");

    radio.has_position = true;
    radio.position.position_broadcast_secs = 900U;
    radio.position.gps_update_interval = 120U;
    radio.position.position_flags = 811U; /* not shown; must survive the write */
    action.section = MESH_UI_SETTINGS_POSITION;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_POSITION_GPS_MODE;
    action.edits[0].number = (uint32_t)meshtastic_Config_PositionConfig_GpsMode_DISABLED;
    action.edits[1].field = MESH_UI_FIELD_POSITION_SMART_DISTANCE;
    action.edits[1].number = 250U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_CONFIG ||
            write.type != meshtastic_AdminMessage_ConfigType_POSITION_CONFIG ||
            write.payload.config.which_payload_variant != meshtastic_Config_position_tag ||
            write.payload.config.payload_variant.position.gps_mode !=
                meshtastic_Config_PositionConfig_GpsMode_DISABLED ||
            write.payload.config.payload_variant.position.broadcast_smart_minimum_distance !=
                250U ||
            write.payload.config.payload_variant.position.position_broadcast_secs != 900U ||
            write.payload.config.payload_variant.position.position_flags != 811U,
        "set_position_config should carry the edits and keep the rest");

    radio.has_power = true;
    radio.power.ls_secs = 300U;
    radio.power.adc_multiplier_override = 2.5f; /* not shown; must survive the write */
    action.section = MESH_UI_SETTINGS_POWER;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_POWER_SAVING;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_POWER_WAIT_BT;
    action.edits[1].number = 30U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.type != meshtastic_AdminMessage_ConfigType_POWER_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_power_tag ||
                          !write.payload.config.payload_variant.power.is_power_saving ||
                          write.payload.config.payload_variant.power.wait_bluetooth_secs != 30U ||
                          write.payload.config.payload_variant.power.ls_secs != 300U ||
                          write.payload.config.payload_variant.power.adc_multiplier_override < 2.4f,
                      "set_power_config should carry the edits and keep the rest");

    /* Power can leave too little Bluetooth on to reconnect, so it asks before it writes. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_POWER),
                      "the Power section should be behind the confirm overlay");

    radio.has_mqtt = true;
    snprintf(radio.mqtt.address, sizeof radio.mqtt.address, "%s", "mqtt.example.org");
    radio.mqtt.proxy_to_client_enabled = true; /* read-only row; must survive the write */
    action.section = MESH_UI_SETTINGS_MQTT;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_MQTT_USERNAME;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "brick");
    action.edits[1].field = MESH_UI_FIELD_MQTT_MAP_REPORTING;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG ||
            write.payload.module_config.which_payload_variant != meshtastic_ModuleConfig_mqtt_tag ||
            strcmp(write.payload.module_config.payload_variant.mqtt.username, "brick") != 0 ||
            !write.payload.module_config.payload_variant.mqtt.map_reporting_enabled ||
            strcmp(write.payload.module_config.payload_variant.mqtt.address, "mqtt.example.org") !=
                0 ||
            !write.payload.module_config.payload_variant.mqtt.proxy_to_client_enabled,
        "set_mqtt_config should carry the edits and keep the rest");

    action.section = MESH_UI_SETTINGS_DISPLAY;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a section the radio has not sent cannot be written");
    action.section = MESH_UI_SETTINGS_RADIO;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOTSUP,
                      "the Radio section is read-only");
    record_success(test_name);
}

/*
 * The module writes phase 9 completed: the fields that were being dropped, the submessage that
 * needs its presence flag set, and the Modules list refusing to be a write at all.
 */
MESH_TEST_CASE(app_module_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    struct mesh_admin_request write;

    /* Store & Forward: the three server fields that were dropped before phase 9, with the
       heartbeat the UI does show left untouched by the write. */
    radio.has_store_forward = true;
    radio.store_forward.heartbeat = true;
    action.section = MESH_UI_SETTINGS_STORE_FORWARD;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_SF_RECORDS;
    action.edits[0].number = 250U;
    action.edits[1].field = MESH_UI_FIELD_SF_HISTORY_MAX;
    action.edits[1].number = 50U;
    action.edits[2].field = MESH_UI_FIELD_SF_HISTORY_WINDOW;
    action.edits[2].number = 3600U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG ||
            write.payload.module_config.payload_variant.store_forward.records != 250U ||
            write.payload.module_config.payload_variant.store_forward.history_return_max != 50U ||
            write.payload.module_config.payload_variant.store_forward.history_return_window !=
                3600U ||
            !write.payload.module_config.payload_variant.store_forward.heartbeat,
        "set_store_forward should carry the server fields and keep the rest");

    /* Telemetry: the health trio and the intervals that had no rows before. */
    radio.has_telemetry = true;
    radio.telemetry.device_update_interval = 900U;
    action.section = MESH_UI_SETTINGS_TELEMETRY;
    action.edit_count = 4U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_TELEMETRY_HEALTH;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL;
    action.edits[1].number = 1800U;
    action.edits[2].field = MESH_UI_FIELD_TELEMETRY_POWER_SCREEN;
    action.edits[2].number = 1U;
    action.edits[3].field = MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL;
    action.edits[3].number = 3600U;
    action.edits[4].field = MESH_UI_FIELD_TELEMETRY_AIR_SCREEN;
    action.edits[4].number = 1U;
    action.edit_count = 5U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG ||
            !write.payload.module_config.payload_variant.telemetry.health_measurement_enabled ||
            write.payload.module_config.payload_variant.telemetry.health_update_interval != 1800U ||
            !write.payload.module_config.payload_variant.telemetry.power_screen_enabled ||
            write.payload.module_config.payload_variant.telemetry.air_quality_interval != 3600U ||
            !write.payload.module_config.payload_variant.telemetry.air_quality_screen_enabled ||
            write.payload.module_config.payload_variant.telemetry.device_update_interval != 900U,
        "set_telemetry should carry the health rows and keep the rest");

    /* MapReportSettings is a submessage: without has_map_report_settings nanopb drops it from
       the wire entirely and the firmware keeps whatever it had. */
    radio.has_mqtt = true;
    action.section = MESH_UI_SETTINGS_MQTT;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_MQTT_MAP_INTERVAL;
    action.edits[0].number = 7200U;
    action.edits[1].field = MESH_UI_FIELD_MQTT_MAP_LOCATION;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            !write.payload.module_config.payload_variant.mqtt.has_map_report_settings ||
            write.payload.module_config.payload_variant.mqtt.map_report_settings
                    .publish_interval_secs != 7200U ||
            !write.payload.module_config.payload_variant.mqtt.map_report_settings
                 .should_report_location,
        "a map-report edit should mark the submessage present");

    /* The Modules list is a folder, not a section: there is nothing there for Y to write. */
    action.section = MESH_UI_SETTINGS_MODULES;
    action.edit_count = 0U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOTSUP,
                      "the Modules list cannot be written");
    record_success(test_name);
}

/* Key choices become bytes, roles map back, and a bad PIN never reaches the radio. */
MESH_TEST_CASE(app_channel_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    meshtastic_Channel channel = meshtastic_Channel_init_default;
    channel.index = 1;
    channel.role = meshtastic_Channel_Role_SECONDARY;
    channel.has_settings = true;
    channel.settings.psk.size = 1U;
    channel.settings.psk.bytes[0] = 1U;
    channel.settings.id = 77U;
    mesh_radio_settings_apply_channel(&radio, &channel);
    radio.has_bluetooth = true;
    radio.bluetooth.enabled = true;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_CHANNELS;
    action.channel = 1U;
    action.edit_count = 4U;
    action.edits[0].field = MESH_UI_FIELD_CHANNEL_KEY;
    action.edits[0].number = MESH_UI_PSK_RANDOM_256;
    action.edits[1].field = MESH_UI_FIELD_CHANNEL_NAME;
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "Hikers");
    action.edits[2].field = MESH_UI_FIELD_CHANNEL_ROLE;
    action.edits[2].number = 0U; /* disabled */
    action.edits[3].field = MESH_UI_FIELD_CHANNEL_POSITION;
    action.edits[3].number = 16U;

    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CHANNEL || write.type != 1U ||
                          write.payload.channel.index != 1 ||
                          write.payload.channel.role != meshtastic_Channel_Role_DISABLED ||
                          strcmp(write.payload.channel.settings.name, "Hikers") != 0 ||
                          write.payload.channel.settings.psk.size != 32U ||
                          write.payload.channel.settings.id != 77U ||
                          !write.payload.channel.settings.has_module_settings ||
                          write.payload.channel.settings.module_settings.position_precision != 16U,
                      "the channel write should carry the edits over the radio's copy");
    bool all_zero = true;
    for (unsigned i = 0; i < 32U; ++i) {
        if (write.payload.channel.settings.psk.bytes[i] != 0U) {
            all_zero = false;
        }
    }
    MESH_TEST_FAIL_IF(all_zero, "a random key should not be all zeroes");
    action.edit_count = 1U;
    action.edits[0].number = MESH_UI_PSK_TYPED;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s",
             "d4f1bb3a20290759f0bcffabcf4e6901");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.channel.settings.psk.size != 16U ||
                          write.payload.channel.settings.psk.bytes[0] != 0xD4U ||
                          write.payload.channel.settings.psk.bytes[15] != 0x01U,
                      "a typed key should be parsed as hex");
    action.edits[0].number = MESH_UI_PSK_NONE;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.channel.settings.psk.size != 0U,
                      "no encryption is an empty key");
    action.channel = 3U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a slot the radio never sent cannot be written");

    action.section = MESH_UI_SETTINGS_BLUETOOTH;
    action.channel = MESH_UI_SETTINGS_NO_CHANNEL;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_BT_MODE;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_BT_PIN;
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "123456");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CONFIG ||
                          write.type != meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_bluetooth_tag ||
                          write.payload.config.payload_variant.bluetooth.mode !=
                              meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN ||
                          write.payload.config.payload_variant.bluetooth.fixed_pin != 123456U ||
                          !write.payload.config.payload_variant.bluetooth.enabled,
                      "the Bluetooth write is wrong");
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "12ab56");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL,
                      "a PIN that is not six digits must be refused");
    record_success(test_name);
}

/* LoRa and Security rows, and the writes built from them. */
MESH_TEST_CASE(app_lora_security_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_lora = true;
    radio.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    radio.lora.use_preset = true;
    radio.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    radio.lora.hop_limit = 3U;
    radio.lora.tx_enabled = true;
    radio.lora.frequency_offset = 1.5f;
    radio.has_security = true;
    radio.security.private_key.size = 32U;
    memset(radio.security.private_key.bytes, 0x11, 32U);
    radio.security.public_key.size = 32U;
    memset(radio.security.public_key.bytes, 0x22, 32U);
    radio.security.admin_key_count = 2U;
    radio.security.admin_key[0].size = 32U;
    memset(radio.security.admin_key[0].bytes, 0x33, 32U);
    radio.security.admin_key[1].size = 32U;
    memset(radio.security.admin_key[1].bytes, 0x44, 32U);

    /* The flattened view carries the keys for the rows. */
    struct mesh_ui_settings settings;
    struct mesh_ui_action probe;
    memset(&probe, 0, sizeof probe);
    struct mesh_ui_snapshot *unused = NULL;
    (void)unused;
    (void)probe;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.region = 1U;
    settings.use_preset = true;
    settings.tx_power = 0;
    settings.has_security = true;
    settings.private_key_len = 32U;
    memset(settings.private_key, 0x11, 32U);
    settings.admin_key_count = 2U;
    settings.admin_key_lens[0] = 32U;
    memset(settings.admin_keys[0], 0x33, 32U);
    settings.admin_key_lens[1] = 32U;
    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_LORA,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 11U ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
            item.field != MESH_UI_FIELD_LORA_REGION || strcmp(item.value, "US") != 0 ||
            mesh_ui_settings_enum_count(MESH_UI_FIELD_LORA_REGION) != 38U ||
            strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_LORA_REGION, 37U), "ITU2 1.25m") != 0 ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 8U, &item) ||
            item.field != MESH_UI_FIELD_LORA_TX_POWER || strcmp(item.value, "max") != 0 ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_LORA_TX_POWER, 0U, +1) != 2U ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 5U, &item) ||
            strcmp(item.value, "4/0") != 0,
        "LoRa rows are wrong");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_SECURITY,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 10U ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
            item.field != MESH_UI_FIELD_SECURITY_PRIVATE_KEY || item.kind != MESH_UI_SETTING_KEY ||
            strlen(item.text) != 44U || strstr(item.value, "256-bit") == NULL ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
            item.field != MESH_UI_FIELD_SECURITY_ADMIN_KEY_2 || strcmp(item.value, "none") != 0 ||
            !mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_LORA) ||
            !mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_SECURITY),
        "Security rows are wrong");

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_LORA;
    action.channel = MESH_UI_SETTINGS_NO_CHANNEL;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_LORA_REGION;
    action.edits[0].number = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    action.edits[1].field = MESH_UI_FIELD_LORA_HOPS;
    action.edits[1].number = 5U;
    action.edits[2].field = MESH_UI_FIELD_LORA_TX_POWER;
    action.edits[2].number = 20U;
    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CONFIG ||
                          write.type != meshtastic_AdminMessage_ConfigType_LORA_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_lora_tag ||
                          write.payload.config.payload_variant.lora.region !=
                              meshtastic_Config_LoRaConfig_RegionCode_EU_868 ||
                          write.payload.config.payload_variant.lora.hop_limit != 5U ||
                          write.payload.config.payload_variant.lora.tx_power != 20 ||
                          !write.payload.config.payload_variant.lora.use_preset ||
                          write.payload.config.payload_variant.lora.frequency_offset != 1.5f,
                      "the LoRa write should carry the edits over the radio's copy");

    /* Security: a new private key is clamped and the public key cleared for the firmware to
       derive; clearing admin key 1 compacts the list. */
    action.section = MESH_UI_SETTINGS_SECURITY;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_SECURITY_PRIVATE_KEY;
    action.edits[0].number = MESH_UI_PSK_RANDOM_256;
    action.edits[1].field = MESH_UI_FIELD_SECURITY_ADMIN_KEY_0;
    action.edits[1].number = MESH_UI_PSK_NONE;
    action.edits[2].field = MESH_UI_FIELD_SECURITY_MANAGED;
    action.edits[2].number = 1U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.type != meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_security_tag,
                      "the Security write should build");
    const meshtastic_Config_SecurityConfig *sec = &write.payload.config.payload_variant.security;
    MESH_TEST_FAIL_IF(sec->private_key.size != 32U || (sec->private_key.bytes[0] & 7U) != 0U ||
                          (sec->private_key.bytes[31] & 0x80U) != 0U ||
                          (sec->private_key.bytes[31] & 0x40U) == 0U ||
                          memcmp(sec->private_key.bytes, radio.security.private_key.bytes, 32U) ==
                              0 ||
                          sec->public_key.size != 0U || !sec->is_managed,
                      "a new private key should be clamped and the public key cleared");
    MESH_TEST_FAIL_IF(sec->admin_key_count != 1U || sec->admin_key[0].size != 32U ||
                          sec->admin_key[0].bytes[0] != 0x44U || sec->admin_key[1].size != 0U,
                      "clearing an admin key should compact the list");
    /* Restoring a backed-up private key and adding an admin key by text. */
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_SECURITY_PRIVATE_KEY;
    action.edits[0].number = MESH_UI_PSK_TYPED;
    uint8_t restore[32];
    memset(restore, 0x5A, 32U);
    mesh_ui_settings_key_text(restore, 32U, action.edits[0].text, sizeof action.edits[0].text);
    action.edits[1].field = MESH_UI_FIELD_SECURITY_ADMIN_KEY_2;
    action.edits[1].number = MESH_UI_PSK_TYPED;
    memset(restore, 0x66, 32U);
    mesh_ui_settings_key_text(restore, 32U, action.edits[1].text, sizeof action.edits[1].text);
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          sec->private_key.bytes[0] != 0x5AU || sec->private_key.size != 32U ||
                          sec->public_key.size != 0U || sec->admin_key_count != 3U ||
                          sec->admin_key[2].bytes[0] != 0x66U,
                      "typed keys should be restored as given");
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "AQ=="); /* 1 byte */
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL,
                      "a private key must be 32 bytes");
    record_success(test_name);
}

/*
 * With both links available the app must prefer the plugged-in node, route the connect to the
 * serial transport, list USB ports above BLE advertisers, and - the point of the shared session -
 * fold what the USB radio says into the app's own session rather than one buried in the link.
 */
MESH_TEST_CASE(app_link_routing, unit) {
    const char *failure = NULL;
    int pair[2] = {-1, -1};
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    MESH_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0, "socketpair failed");
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -30, .paired = true},
    };
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 1U,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    const struct mesh_serial_device_info ports[] = {mesh_test_serial_device()};
    struct mesh_serial_usb_mock_config serial_mock;
    memset(&serial_mock, 0, sizeof serial_mock);
    serial_mock.devices = ports;
    serial_mock.device_count = 1U;
    serial_mock.bound_path = "/dev/ttyUSB0";
    serial_mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&serial_mock);

    char home_dir[] = "/tmp/mesh_app_link_routingXXXXXX";
    if (mkdtemp(home_dir) == NULL) {
        failure = "mkdtemp failed";
        goto cleanup;
    }
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_transport *serial = mesh_serial_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    mesh_serial_transport_refresh_devices(serial);

    /* USB wins even though a BLE advertiser is in range at a healthy RSSI. */
    mesh_app_autoconnect(&app);
    if (!mesh_serial_transport_is_connecting(serial)) {
        failure = "auto-connect should have opened the USB port first";
        goto cleanup;
    }
    if (mesh_ble_transport_connected_address(ble) != NULL ||
        mesh_ble_transport_is_connecting(ble)) {
        failure = "the BLE link should have been left alone";
        goto cleanup;
    }

    mesh_test_serial_sleep_ms(150);
    mesh_transport_registry_tick(&app.transport_registry);
    const char *connected = mesh_app_connected_identifier();
    if (connected == NULL || strcmp(connected, "/dev/ttyUSB0") != 0) {
        failure = "the app should report the tty as the connected radio";
        goto cleanup;
    }
    if (mesh_app_active_transport() != serial) {
        failure = "the serial transport should be the active link";
        goto cleanup;
    }

    /* The radio's reply has to land in the app's session, not one hidden in the transport. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x0BADCAFEU;
    uint8_t encoded[256];
    size_t encoded_len = 0U;
    if (!mesh_test_encode_from_radio(&from_radio, encoded, sizeof encoded, &encoded_len)) {
        failure = "failed to encode the reply";
        goto cleanup;
    }
    uint8_t frame[300];
    size_t frame_len = 0U;
    mesh_stream_frame_encode(encoded, encoded_len, frame, sizeof frame, &frame_len);
    if (write(pair[1], frame, frame_len) != (ssize_t)frame_len) {
        failure = "failed to write the reply into the port";
        goto cleanup;
    }
    if (mesh_serial_transport_pump(serial) <= 0) {
        failure = "pump should have read the reply";
        goto cleanup;
    }
    if (!app.session.handshake.has_my_info ||
        app.session.handshake.my_info.my_node_num != 0x0BADCAFEU) {
        failure = "the USB link should feed the app's own session";
        goto cleanup;
    }

    /* The Devices tab shows one list: USB ports first, then BLE advertisers. */
    mesh_app_publish_ui_state(&app);
    if (app.ui_store.device_count != 2U) {
        failure = "both the USB port and the BLE advertiser should be listed";
        goto cleanup;
    }
    if (app.ui_store.devices[0].kind != (uint8_t)MESH_UI_DEVICE_SERIAL ||
        strcmp(app.ui_store.devices[0].identifier, "/dev/ttyUSB0") != 0 ||
        !app.ui_store.devices[0].connected) {
        failure = "the connected USB port should be the first row";
        goto cleanup;
    }
    if (app.ui_store.devices[1].kind != (uint8_t)MESH_UI_DEVICE_BLE ||
        strcmp(app.ui_store.devices[1].identifier, "AA:BB:CC:DD:EE:06") != 0 ||
        app.ui_store.devices[1].connected) {
        failure = "the BLE advertiser should follow it, unconnected";
        goto cleanup;
    }

    /* Nothing should be reconnected while a link is up. */
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_is_connecting(ble) ||
        mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "auto-connect should stay put while the USB link is up";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    mesh_serial_usb_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * A BLE connect can return 0 and still fail seconds later, when BlueZ finishes service discovery
 * and StartNotify is rejected because the node was never paired. That used to leave the UI stuck
 * on "connecting" with the reason only in the log.
 */
MESH_TEST_CASE(app_connect_failure_toast, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = -40, .paired = true},
    };
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 1U,
        /* The node answers Connect and resolves services, then refuses the subscription. */
        .connect_pending_polls = 1U,
        .subscribe_result = -EACCES,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[] = "/tmp/mesh_app_connect_failXXXXXX";
    if (mkdtemp(home_dir) == NULL) {
        failure = "mkdtemp failed";
        goto cleanup;
    }
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    if (app.ui_controller.on_action == NULL) {
        failure = "the app should have installed a UI action handler";
        goto cleanup;
    }

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_CONNECT;
    action.kind = (uint8_t)MESH_UI_DEVICE_BLE;
    snprintf(action.identifier, sizeof action.identifier, "%s", "AA:BB:CC:DD:EE:07");
    app.ui_controller.on_action(app.ui_controller.action_userdata, &action);

    /* Connect has only been sent; nothing has failed yet. */
    if (!mesh_ble_transport_is_connecting(ble)) {
        failure = "the connect should be in flight";
        goto cleanup;
    }
    if (strstr(app.ui_store.nav.toast, "Connecting") == NULL) {
        failure = "the user should first be told the connect is in flight";
        goto cleanup;
    }
    if (mesh_app_report_link_errors(&app)) {
        failure = "no failure should be reported while the connect is still pending";
        goto cleanup;
    }

    /* Now the reply lands, services resolve, and StartNotify is refused. */
    mesh_transport_registry_tick(&app.transport_registry);
    if (mesh_ble_transport_is_connecting(ble) ||
        mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "the link should have been dropped";
        goto cleanup;
    }

    if (!mesh_app_report_link_errors(&app)) {
        failure = "the pairing failure should have been drained";
        goto cleanup;
    }
    if (strstr(app.ui_store.nav.toast, "pairing") == NULL ||
        strstr(app.ui_store.nav.toast, "EE:07") == NULL) {
        failure = "the pairing failure should have reached the screen";
        goto cleanup;
    }

    /* One report per attempt: the same failure must not keep re-toasting every turn. */
    mesh_ui_store_set_toast(&app.ui_store, test_now_ms(), "quiet");
    if (mesh_app_report_link_errors(&app)) {
        failure = "the failure should be reported once, not on every turn";
        goto cleanup;
    }

    /*
     * A connect that returned 0 and failed later is still a failure. Nothing else tells
     * auto-connect that, so without it the backoff never grows and a node that refuses every
     * time is retried every couple of seconds forever.
     */
    if (app.autoconnect_failures == 0U) {
        failure = "the late failure should have counted against auto-connect";
        goto cleanup;
    }
    const unsigned failures_before = app.autoconnect_failures;
    const uint64_t retry_before = app.autoconnect_retry_at_ms;

    /* Auto-connect retries the same doomed node on every backoff, so its failures stay in the
       log; only a connect the user asked for is worth interrupting them for. */
    app.autoconnect_retry_at_ms = 0U;
    snprintf(app.config.preferred_ble_device, sizeof app.config.preferred_ble_device, "%s",
             "AA:BB:CC:DD:EE:07");
    mesh_app_autoconnect(&app);
    mesh_transport_registry_tick(&app.transport_registry);
    if (!mesh_app_report_link_errors(&app)) {
        failure = "the auto-connect failure should still have been drained";
        goto cleanup;
    }
    if (strcmp(app.ui_store.nav.toast, "quiet") != 0) {
        failure = "an auto-connect failure should not raise a toast";
        goto cleanup;
    }
    if (app.autoconnect_failures <= failures_before) {
        failure = "each failed attempt should push the backoff out further";
        goto cleanup;
    }
    (void)retry_before;

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * Who survives the Nodes tab's budget, and specifically what happens on the day you move the
 * Brick from one of your radios to another: the one you unplugged has no pin on the new radio
 * (favorites are NodeDB state, per receiver) and must not sink to the bottom of a busy mesh.
 */
MESH_TEST_CASE(app_node_rank_known_radio, unit) {
    const uint32_t abc = 0xABC123U;
    const uint32_t def = 0xDEF456U;
    const uint32_t peer = 0x00777U;
    const uint32_t stranger = 0x00888U;

    struct mesh_ui_preferences prefs;
    memset(&prefs, 0, sizeof prefs);

    struct mesh_message_log log;
    mesh_message_log_reset(&log);
    log.count = 1U;
    log.entries[0].from = peer;
    log.entries[0].to = abc;

    struct mesh_node_summary nodes[4];
    memset(nodes, 0, sizeof nodes);
    nodes[0].node_id = abc;
    nodes[1].node_id = def;
    nodes[2].node_id = peer;
    nodes[3].node_id = stranger;
    nodes[3].via_mqtt = true;

    /* Connected to ABC123, with DEF456 pinned into ABC123's NodeDB. */
    prefs.known_radio_count = 1U;
    prefs.known_radios[0] = abc;
    nodes[1].is_favorite = true;
    if (mesh_app_node_rank(&nodes[0], abc, &log, &prefs) != 0U ||
        mesh_app_node_rank(&nodes[1], abc, &log, &prefs) != 1U ||
        mesh_app_node_rank(&nodes[2], abc, &log, &prefs) != 3U ||
        mesh_app_node_rank(&nodes[3], abc, &log, &prefs) != 5U) {
        record_failure(test_name, "us, then pinned, then a message peer, then MQTT");
        return;
    }

    /* Now the Brick is moved onto DEF456. Its NodeDB never heard of the pin ABC123 carried,
       so the flag is gone - and ABC123 is nobody's favorite over here. */
    mesh_ui_preferences_note_radio(&prefs, def);
    nodes[1].is_favorite = false;
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[1], def, &log, &prefs) != 0U,
                      "the radio we are now on is us, pinned or not");
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[0], def, &log, &prefs) != 2U,
                      "the radio we just unplugged should rank as one of ours");
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[2], def, &log, &prefs) != 3U ||
                          mesh_app_node_rank(&nodes[3], def, &log, &prefs) != 5U,
                      "everyone else should keep their tier");

    /* Without the memory - a fresh install, or a radio we have never connected to - ABC123 is
       an ordinary node heard over RF, which is the behaviour this tier exists to avoid. */
    struct mesh_ui_preferences empty;
    memset(&empty, 0, sizeof empty);
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[0], def, &log, &empty) != 3U ||
                          mesh_app_node_rank(&nodes[0], def, NULL, &empty) != 4U ||
                          mesh_app_node_rank(&nodes[0], def, NULL, NULL) != 4U,
                      "an unknown radio should fall back to the ordinary tiers");

    record_success(test_name);
}
