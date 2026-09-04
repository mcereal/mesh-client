#pragma once

#include "mesh/mesh_message.h"
#include "mesh/radio_settings.h"
#include "meshtastic/mesh.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A Meshtastic client session: everything about talking to a radio that does not depend on how
 * the bytes travel. The handshake (want_config_id and the NodeDB/config sync it triggers), the
 * node summary cache, the message log, the radio's configuration with its admin request queue,
 * and the packet id generator all live here.
 *
 * A link (BLE today, USB serial next) owns the connection and does two things: it installs a
 * send function while connected, and it hands every FromRadio protobuf it receives to
 * mesh_session_handle_from_radio(). The session never knows about GATT, ttys or framing; the
 * link never decodes a protobuf.
 */

/* Real meshes run past 100 nodes (134 seen on the bench); keep the summary cache large enough
   for a full NodeDB sync, since a node the sync drops cannot be messaged by name. */
#define MESH_SESSION_MAX_NODES 256U

/* The radio's channel table: 8 slots, PRIMARY plus SECONDARYs; DISABLED slots are kept so the
   index stays meaningful (MeshPacket.channel is this index for broadcasts). */
#define MESH_SESSION_MAX_CHANNELS 8U

/* Largest ToRadio/FromRadio protobuf the session encodes or accepts. */
#define MESH_SESSION_MAX_PACKET 512U

struct mesh_node_summary {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
    bool via_mqtt;
    bool has_hops_away;
    uint8_t hops_away;
};

struct mesh_channel_summary {
    uint8_t index;
    uint8_t role; /* meshtastic_Channel_Role */
    char name[12];
    uint8_t psk_len; /* 0 none, 1 default-key index, 16 AES-128, 32 AES-256 */
    bool uplink_enabled;
    bool downlink_enabled;
    uint32_t position_precision;
};

struct mesh_handshake_status {
    bool request_in_flight;
    uint32_t request_id;
    bool config_complete;
    uint32_t config_complete_id;
    bool has_my_info;
    meshtastic_MyNodeInfo my_info;
    bool has_config;
    meshtastic_Config config;
    size_t node_count;
    struct mesh_node_summary nodes[MESH_SESSION_MAX_NODES];
    /* Indexed by channel slot; channel_count is the highest slot seen plus one. */
    size_t channel_count;
    struct mesh_channel_summary channels[MESH_SESSION_MAX_CHANNELS];
};

/* Hands one ToRadio protobuf (raw, unframed) to the link. `packet_id` is the message log entry
   to mark FAILED if the packet never reaches the radio (0 when none); the link reports that
   through mesh_session_packet_failed(). Returns 0 or a negative errno. */
typedef int (*mesh_session_send_fn)(void *ctx, const uint8_t *packet, size_t len,
                                    uint32_t packet_id);

struct mesh_session {
    struct mesh_handshake_status handshake;
    /* Survives reconnects: a NodeDB resync must not wipe the conversation. */
    struct mesh_message_log messages;
    /* The radio's configuration and the admin session; reset with the handshake. */
    struct mesh_radio_settings settings;
    mesh_session_send_fn send;
    void *send_ctx;
    uint32_t next_config_request_id;
    uint32_t next_packet_id;
    bool node_cache_warned;
    bool admin_probe_queued; /* the post-handshake probe has been queued this connection */
};

/* Clears everything, message log included, and seeds the want_config nonce. */
void mesh_session_init(struct mesh_session *session);

/* Link up: install the send path. The handshake starts with mesh_session_begin_handshake(). */
void mesh_session_attach(struct mesh_session *session, mesh_session_send_fn send, void *ctx);
/* Link down: drops the send path and resets the handshake and settings. Messages survive. */
void mesh_session_detach(struct mesh_session *session);
bool mesh_session_attached(const struct mesh_session *session);

/* Sends want_config_id with a fresh nonce and resets the handshake state for the reply.
   Returns 0, -ENOTCONN without a link, or the send error. */
int mesh_session_begin_handshake(struct mesh_session *session);

/* Decodes one FromRadio protobuf and folds it into the handshake, node cache, settings or
   message log. Admin replies never reach the message log. */
void mesh_session_handle_from_radio(struct mesh_session *session, const uint8_t *payload,
                                    size_t len);

/* Drives the admin request queue: once the handshake completes, probes metadata and owner,
   then sends whatever is queued one request at a time. Call every loop turn while attached. */
void mesh_session_tick(struct mesh_session *session, uint64_t now_ms);

/* Sends one raw ToRadio protobuf. -ENOTCONN without a link, -EMSGSIZE when too large. */
int mesh_session_send_packet(struct mesh_session *session, const uint8_t *packet, size_t len);

/* Encode and send a TEXT_MESSAGE_APP packet and record it in the message log as outbound.
   Pass MESH_MESSAGE_BROADCAST_ADDR to broadcast on `channel`. want_ack is ignored for
   broadcasts, which the mesh never acks directly. On success the assigned packet id is stored
   in *out_packet_id (may be NULL) so the caller can watch for the delivery result. */
int mesh_session_send_text(struct mesh_session *session, uint32_t dest, uint8_t channel,
                           const char *text, bool want_ack, uint32_t *out_packet_id);

/* The link dropped a queued packet before it reached the radio: the message, if any, is
   marked FAILED rather than staying PENDING forever. */
void mesh_session_packet_failed(struct mesh_session *session, uint32_t packet_id);

/* Re-reads every section the Settings tab shows through the admin path, one request per
   tick. Returns the number of requests queued, -ENOTCONN before the handshake has my_info. */
int mesh_session_refresh_settings(struct mesh_session *session);

/* Queues one settings write (see mesh_radio_settings_queue_write) behind a passkey refresh and
   ahead of a read-back. Returns the number of requests queued, -ENOTCONN before the handshake
   has my_info, -ENOSPC when the queue is full, -EINVAL for anything but a write. */
int mesh_session_write_settings(struct mesh_session *session,
                                const struct mesh_admin_request *write);

/* Meshtastic packet ids only need to be unique per sender for a few minutes. Never zero. */
uint32_t mesh_session_next_packet_id(struct mesh_session *session);

/* Borrowed views; valid until the next call into the session. */
const struct mesh_handshake_status *mesh_session_handshake(const struct mesh_session *session);
const struct mesh_message_log *mesh_session_messages(const struct mesh_session *session);
const struct mesh_radio_settings *mesh_session_settings(const struct mesh_session *session);

#ifdef __cplusplus
}
#endif
