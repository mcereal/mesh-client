#define _POSIX_C_SOURCE 200809L

#include "support/ble_fixture.h"

#include "support/proto_fixture.h"

#include "mesh/transport/ble.h"
#include "mesh/transport/ble_gatt.h"

#include <stdio.h>
#include <string.h>

#define RIG_ADAPTER "/org/bluez/hci0"

void mesh_test_ble_rig_init(struct mesh_test_ble_rig *rig, const char *address, const char *name,
                            int16_t rssi) {
    memset(rig, 0, sizeof *rig);
    rig->ble = mesh_ble_transport();

    /* The mock names a characteristic `<address>/<uuid>`. */
    snprintf(rig->toradio_path, sizeof rig->toradio_path, "%s/%s", address, MESH_BLE_TORADIO_UUID);
    snprintf(rig->fromradio_path, sizeof rig->fromradio_path, "%s/%s", address,
             MESH_BLE_FROMRADIO_UUID);
    snprintf(rig->fromnum_path, sizeof rig->fromnum_path, "%s/%s", address, MESH_BLE_FROMNUM_UUID);

    snprintf(rig->devices[0].address, sizeof rig->devices[0].address, "%s", address);
    snprintf(rig->devices[0].name, sizeof rig->devices[0].name, "%s", name);
    rig->devices[0].rssi = rssi;
    rig->devices[0].paired = true;
    rig->device_count = 1U;

    for (size_t i = 0; i < MESH_TEST_BLE_MAX_READS; ++i) {
        rig->read_payloads[i] = rig->read_buffers[i];
    }

    rig->mock.adapter_name = RIG_ADAPTER;
    rig->mock.devices = rig->devices;
    rig->mock.device_count = rig->device_count;
    rig->mock.read_payloads = rig->read_payloads;
    rig->mock.read_payload_lengths = rig->read_payload_lengths;
    rig->mock.read_payload_count = MESH_TEST_BLE_MAX_READS;
    rig->mock.read_index = &rig->read_index;
    rig->mock.write_capture_buffer = rig->write_capture;
    rig->mock.write_capture_capacity = sizeof rig->write_capture;
    rig->mock.write_capture_length = &rig->write_len;
    rig->mock.write_capture_handle = rig->write_path;
    rig->mock.write_capture_handle_capacity = sizeof rig->write_path;
    rig->mock.write_call_count = &rig->write_call_count;
    rig->mock.write_lengths = rig->write_lengths;
    rig->mock.write_lengths_capacity = sizeof rig->write_lengths / sizeof rig->write_lengths[0];
}

bool mesh_test_ble_rig_add_device(struct mesh_test_ble_rig *rig, const char *address,
                                  const char *name, int16_t rssi) {
    if (rig->device_count >= MESH_TEST_BLE_MAX_DEVICES) {
        return false;
    }
    struct inkwell_ble_device *const device = &rig->devices[rig->device_count];
    snprintf(device->address, sizeof device->address, "%s", address);
    snprintf(device->name, sizeof device->name, "%s", name);
    device->rssi = rssi;
    device->paired = true;
    rig->device_count += 1U;
    rig->mock.device_count = rig->device_count;
    return true;
}

void mesh_test_ble_rig_reload(struct mesh_test_ble_rig *rig) {
    inkwell_ble_mock_enable(&rig->mock);
}

int mesh_test_ble_rig_start(struct mesh_test_ble_rig *rig) {
    mesh_test_ble_rig_reload(rig);
    if (!rig->loop_ready) {
        rig->config = mesh_app_config_default();
        const int result = inkwell_loop_init(&rig->loop);
        if (result != 0) {
            return result;
        }
        rig->loop_ready = true;
    }
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
        rig->started = false;
    }
    if (rig->loop_ready) {
        inkwell_loop_shutdown(&rig->loop);
        rig->loop_ready = false;
    }
    inkwell_ble_mock_disable();
}
