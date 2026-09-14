#define _POSIX_C_SOURCE 200809L

#include "support/ble_fixture.h"

#include "support/proto_fixture.h"

#include "mesh/transport/ble.h"

#include <stdio.h>
#include <string.h>

#define RIG_ADAPTER "/org/bluez/hci0"

/* BlueZ names a device object by its address with the colons turned into underscores. */
static void device_path(char *out, size_t cap, const char *address) {
    char node[64];
    size_t n = 0U;
    for (size_t i = 0; address[i] != '\0' && n + 1U < sizeof node; ++i) {
        node[n++] = address[i] == ':' ? '_' : address[i];
    }
    node[n] = '\0';
    snprintf(out, cap, "%s/dev_%s", RIG_ADAPTER, node);
}

void mesh_test_ble_rig_init(struct mesh_test_ble_rig *rig, const char *address, const char *name,
                            int16_t rssi) {
    memset(rig, 0, sizeof *rig);
    rig->ble = mesh_ble_transport();

    char base[96];
    device_path(base, sizeof base, address);
    snprintf(rig->toradio_path, sizeof rig->toradio_path, "%s/service000a/char000b", base);
    snprintf(rig->fromradio_path, sizeof rig->fromradio_path, "%s/service000a/char000d", base);
    snprintf(rig->fromnum_path, sizeof rig->fromnum_path, "%s/service000a/char000f", base);

    snprintf(rig->devices[0].address, sizeof rig->devices[0].address, "%s", address);
    snprintf(rig->devices[0].name, sizeof rig->devices[0].name, "%s", name);
    rig->devices[0].rssi = rssi;
    rig->devices[0].paired = true;
    rig->device_count = 1U;

    for (size_t i = 0; i < MESH_TEST_BLE_MAX_READS; ++i) {
        rig->read_payloads[i] = rig->read_buffers[i];
    }

    rig->mock.adapter_path = RIG_ADAPTER;
    rig->mock.devices = rig->devices;
    rig->mock.device_count = rig->device_count;
    rig->mock.toradio_char_path = rig->toradio_path;
    rig->mock.fromradio_char_path = rig->fromradio_path;
    rig->mock.fromnum_char_path = rig->fromnum_path;
    rig->mock.read_payloads = rig->read_payloads;
    rig->mock.read_payload_lengths = rig->read_payload_lengths;
    rig->mock.read_payload_count = MESH_TEST_BLE_MAX_READS;
    rig->mock.read_index = &rig->read_index;
    rig->mock.write_capture_buffer = rig->write_capture;
    rig->mock.write_capture_capacity = sizeof rig->write_capture;
    rig->mock.write_capture_length = &rig->write_len;
    rig->mock.write_capture_path = rig->write_path;
    rig->mock.write_capture_path_capacity = sizeof rig->write_path;
    rig->mock.write_call_count = &rig->write_call_count;
    rig->mock.write_lengths = rig->write_lengths;
    rig->mock.write_lengths_capacity = sizeof rig->write_lengths / sizeof rig->write_lengths[0];
}

bool mesh_test_ble_rig_add_device(struct mesh_test_ble_rig *rig, const char *address,
                                  const char *name, int16_t rssi) {
    if (rig->device_count >= MESH_TEST_BLE_MAX_DEVICES) {
        return false;
    }
    struct mesh_bluez_device_info *const device = &rig->devices[rig->device_count];
    snprintf(device->address, sizeof device->address, "%s", address);
    snprintf(device->name, sizeof device->name, "%s", name);
    device->rssi = rssi;
    device->paired = true;
    rig->device_count += 1U;
    rig->mock.device_count = rig->device_count;
    return true;
}

void mesh_test_ble_rig_reload(struct mesh_test_ble_rig *rig) {
    mesh_bluez_client_mock_enable(&rig->mock);
}

int mesh_test_ble_rig_start(struct mesh_test_ble_rig *rig) {
    mesh_test_ble_rig_reload(rig);
    rig->config = mesh_app_config_default();
    mesh_event_loop_init(&rig->loop);
    rig->started = true;
    return rig->ble->ops->start(rig->ble, &rig->config, &rig->loop);
}

int mesh_test_ble_rig_connect(struct mesh_test_ble_rig *rig) {
    mesh_ble_transport_refresh_devices(rig->ble);
    return mesh_ble_transport_connect(rig->ble, rig->devices[0].address);
}

bool mesh_test_ble_rig_script(struct mesh_test_ble_rig *rig, size_t slot,
                              const meshtastic_FromRadio *message) {
    if (slot >= MESH_TEST_BLE_MAX_READS) {
        return false;
    }
    size_t len = 0U;
    if (!mesh_test_encode_from_radio(message, rig->read_buffers[slot], MESH_TEST_BLE_READ_CAP,
                                     &len)) {
        return false;
    }
    rig->read_payload_lengths[slot] = len;
    return true;
}

void mesh_test_ble_rig_close(struct mesh_test_ble_rig *rig) {
    if (rig->started) {
        rig->ble->ops->stop(rig->ble);
        mesh_event_loop_shutdown(&rig->loop);
        rig->started = false;
    }
    mesh_bluez_client_mock_disable();
}
