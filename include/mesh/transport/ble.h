#pragma once

#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
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
/* Connects to a node BlueZ already holds a bond for (or one that needs none). An unpaired
   node in PIN mode gets as far as GATT and then fails on StartNotify: bonding is deliberate,
   so it is not started from here. This is the path auto-connect uses. */
int mesh_ble_transport_connect(struct mesh_transport *transport, const char *address);
/* The same, but bonds first when the node is not paired - one press of connect covering both,
   for a connect the user actually asked for. Returns 0 with the link in `pairing` when that is
   what it started, and the PIN prompt follows from mesh_ble_transport_pairing_request(). Never
   call it for an automatic connect: it would raise a PIN prompt nobody asked for. */
int mesh_ble_transport_connect_and_pair(struct mesh_transport *transport, const char *address);
/* Drops whatever is up, coming up, or pairing. -ENOTCONN when nothing was. */
int mesh_ble_transport_disconnect(struct mesh_transport *transport);

/*
 * Pairing, from inside the app. BlueZ does the bonding; we register an org.bluez.Agent1 for it
 * to ask questions on, and these are how the UI answers them. The one that matters on a
 * Meshtastic node is the six-digit PIN it shows on its own screen while pairing.
 */
struct mesh_ble_pairing_request {
    uint8_t kind;     /* enum mesh_bluez_agent_request_kind */
    char address[32]; /* the node BlueZ is bonding with */
    char label[16];   /* its short form, e.g. "6D:DA", for a one-line prompt */
    uint32_t passkey; /* CONFIRM only: the number to check against the node's screen */
};

/* Bonds without connecting afterwards. */
int mesh_ble_transport_pair(struct mesh_transport *transport, const char *address);
/* Adapter1.RemoveDevice: drops the bond entirely, which is the fix when the node's PIN has
   changed under a bond BlueZ still thinks is good. Disconnects first if it is the live link. */
int mesh_ble_transport_forget(struct mesh_transport *transport, const char *address);
bool mesh_ble_transport_is_pairing(struct mesh_transport *transport);
/* The node the link is being brought up on (pairing or connecting), NULL when idle. The
   connected address stays NULL until GATT is wired, so this is the only way for the Devices
   tab to mark the row it is working on. */
const char *mesh_ble_transport_pending_address(struct mesh_transport *transport);
/* True while BlueZ is blocked on a question for the user (the PIN prompt's whole reason to
   exist). Poll it each turn; it clears itself once answered, cancelled or timed out. */
bool mesh_ble_transport_pairing_request(struct mesh_transport *transport,
                                        struct mesh_ble_pairing_request *out);
/* Answers a pending request with the digits the user typed. */
int mesh_ble_transport_submit_passkey(struct mesh_transport *transport, uint32_t passkey);
/* Rejects a pending request and abandons the pairing. */
int mesh_ble_transport_cancel_pairing(struct mesh_transport *transport);

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
