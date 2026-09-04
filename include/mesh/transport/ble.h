#pragma once

#include "mesh/mesh_message.h"
#include "mesh/radio_settings.h"
#include "mesh/session.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"

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

/* The session this link feeds: handshake, node cache, message log and radio settings. The
   functions below are conveniences over it for callers that only hold the transport. */
struct mesh_session *mesh_ble_transport_session(struct mesh_transport *transport);

struct mesh_handshake_status mesh_ble_transport_handshake_status(struct mesh_transport *transport);

/* Borrowed view of the connected radio's configuration (Config/ModuleConfig sections,
   owner, metadata, admin session). Reset with the handshake on every (re)connect. Valid
   until the next transport tick. */
const struct mesh_radio_settings *mesh_ble_transport_settings(struct mesh_transport *transport);

/* Re-reads every section the Settings tab shows through the admin path, one request per
   tick. Returns the number of requests queued, -ENOTCONN when no node is connected. */
int mesh_ble_transport_refresh_settings(struct mesh_transport *transport);

/* Queues one settings write (a SET_* request with its payload filled in; see
   mesh_radio_settings_queue_write) behind a passkey refresh and ahead of a read-back. The
   result arrives through the settings' writes_acked/writes_failed counters. Returns the
   number of requests queued, -ENOTCONN without a connected radio, -ENOSPC when the queue is
   full, -EINVAL for anything but a write. */
int mesh_ble_transport_write_settings(struct mesh_transport *transport,
                                      const struct mesh_admin_request *write);

#ifdef __cplusplus
}
#endif
