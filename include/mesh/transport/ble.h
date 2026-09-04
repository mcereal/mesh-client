#pragma once

#include "mesh/mesh_message.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"
#include "meshtastic/mesh.pb.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_transport *mesh_ble_transport(void);
const struct mesh_bluez_device_info *mesh_ble_transport_devices(struct mesh_transport *transport,
                                                                size_t *count);
size_t mesh_ble_transport_get_devices(struct mesh_transport *transport,
                                      struct mesh_bluez_device_info *out, size_t capacity);
size_t mesh_ble_transport_refresh_devices(struct mesh_transport *transport);
int mesh_ble_transport_connect(struct mesh_transport *transport, const char *address);
int mesh_ble_transport_disconnect(struct mesh_transport *transport);

struct mesh_ble_transport_stats {
    size_t frames_received;
    size_t bytes_received;
};

struct mesh_ble_transport_stats mesh_ble_transport_stats(struct mesh_transport *transport);
/* Queue one ToRadio protobuf (raw, unframed) for the connected node. */
int mesh_ble_transport_send_packet(struct mesh_transport *transport, const uint8_t *packet,
                                   size_t len);
const char *mesh_ble_transport_connected_address(struct mesh_transport *transport);
/* True between a successful Device1.Connect and the end of GATT service discovery; the link
   is neither usable nor reported by mesh_ble_transport_connected_address() yet. */
bool mesh_ble_transport_is_connecting(struct mesh_transport *transport);

/* Asks BlueZ whether the device is still connected. BlueZ does not push a disconnect to us
   (we only watch characteristic properties), so tick() calls this every couple of seconds
   while linked. Returns 1 when the link is up, 0 when it was found down and has been reset
   (queued messages are marked failed), a negative errno when the question could not be asked.
   Exposed for tests. */
int mesh_ble_transport_check_link(struct mesh_transport *transport);

/* Encode and queue a TEXT_MESSAGE_APP packet for the connected node, and record it in the
   message log as outbound. Pass MESH_MESSAGE_BROADCAST_ADDR to broadcast on `channel`.
   want_ack is ignored for broadcasts, which the mesh never acks directly. On success the
   assigned packet id is stored in *out_packet_id (may be NULL) so the caller can watch for
   the delivery result. Returns -ENOTCONN when no node is connected. */
int mesh_ble_transport_send_text(struct mesh_transport *transport, uint32_t dest, uint8_t channel,
                                 const char *text, bool want_ack, uint32_t *out_packet_id);

/* Borrowed view of the inbox/outbox ring. Valid until the next transport tick. */
const struct mesh_message_log *mesh_ble_transport_messages(struct mesh_transport *transport);

/* Real meshes run past 100 nodes; keep the summary cache large enough for a full NodeDB sync. */
#define MESH_BLE_MAX_NODE_SUMMARY 128U

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

/* The radio's channel table: 8 slots, PRIMARY plus SECONDARYs; DISABLED slots are kept so the
   index stays meaningful (MeshPacket.channel is this index for broadcasts). */
#define MESH_BLE_MAX_CHANNELS 8U

struct mesh_ble_channel_summary {
    uint8_t index;
    uint8_t role; /* meshtastic_Channel_Role */
    char name[12];
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
    /* Indexed by channel slot; channel_count is the highest slot seen plus one. */
    size_t channel_count;
    struct mesh_ble_channel_summary channels[MESH_BLE_MAX_CHANNELS];
};

struct mesh_ble_handshake_status
mesh_ble_transport_handshake_status(struct mesh_transport *transport);

#ifdef __cplusplus
}
#endif
