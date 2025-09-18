#pragma once

#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"
#include "meshtastic/mesh.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_transport *mesh_ble_transport(void);
const struct mesh_bluez_device_info *mesh_ble_transport_devices(struct mesh_transport *transport, size_t *count);
size_t mesh_ble_transport_get_devices(struct mesh_transport *transport, struct mesh_bluez_device_info *out,
                                      size_t capacity);
size_t mesh_ble_transport_refresh_devices(struct mesh_transport *transport);
int mesh_ble_transport_connect(struct mesh_transport *transport, const char *address);
int mesh_ble_transport_disconnect(struct mesh_transport *transport);

struct mesh_ble_transport_stats {
    size_t frames_received;
    size_t bytes_received;
};

struct mesh_ble_transport_stats mesh_ble_transport_stats(struct mesh_transport *transport);
int mesh_ble_transport_send_frame(struct mesh_transport *transport, const uint8_t *frame, size_t len);
const char *mesh_ble_transport_connected_address(struct mesh_transport *transport);

#define MESH_BLE_MAX_NODE_SUMMARY 16U

struct mesh_ble_node_summary {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
    bool via_mqtt;
    bool has_hops_away;
    uint8_t hops_away;
};

struct mesh_ble_handshake_status {
    bool request_in_flight;
    uint32_t request_id;
    bool config_complete;
    uint32_t config_complete_id;
    bool has_my_info;
    meshtastic_MyNodeInfo my_info;
    bool has_config;
    meshtastic_Config config;
    size_t node_count;
    struct mesh_ble_node_summary nodes[MESH_BLE_MAX_NODE_SUMMARY];
};

struct mesh_ble_handshake_status mesh_ble_transport_handshake_status(struct mesh_transport *transport);

#ifdef __cplusplus
}
#endif
