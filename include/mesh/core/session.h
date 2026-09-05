#pragma once

#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
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

/* RouteDiscovery's arrays are capped at 8 upstream (proto/meshtastic/mesh.options), so a
   trace can cross at most that many intermediate nodes in each direction. */
#define MESH_TRACEROUTE_MAX_HOPS 8U

/* How long a traceroute waits before it is called lost. A reply has to cross the mesh twice,
   and the firmware answers only after the request has reached the far end, so this is much
   longer than an admin round trip on the local link. */
#define MESH_TRACEROUTE_TIMEOUT_MS 60000U

/* Largest ToRadio/FromRadio protobuf the session encodes or accepts. */
#define MESH_SESSION_MAX_PACKET 512U

/*
 * A node's last known fix. Meshtastic carries latitude and longitude as fixed-point 1e-7
 * degrees, so they are kept in that form and only divided out for display; `time` is the
 * radio's timestamp for the fix, which is not the same thing as when we heard from the node.
 */
struct mesh_node_position {
    bool valid;
    int32_t latitude_i;
    int32_t longitude_i;
    bool has_altitude;
    int32_t altitude; /* metres above sea level */
    uint32_t time;    /* epoch of the fix, 0 when the node did not say */
    uint8_t sats_in_view;
    uint8_t precision_bits; /* how much the sender rounded the position off */
};

/* The DeviceMetrics telemetry every node reports about itself. Each value is optional on the
   wire, so each carries its own has_* rather than being inferred from a zero. */
struct mesh_node_metrics {
    bool valid;
    uint32_t time; /* when we last saw these, our clock */
    bool has_battery;
    uint8_t battery_level; /* percent; 101 means "plugged in", as upstream defines it */
    bool has_voltage;
    float voltage;
    bool has_channel_utilization;
    float channel_utilization;
    bool has_air_util_tx;
    float air_util_tx;
    bool has_uptime;
    uint32_t uptime_seconds;
};

/*
 * EnvironmentMetrics, for the weather-station and sensor nodes. Only the handful of readings
 * worth a row on a 1024x768 handheld are kept; the wire message has forty fields, most of them
 * ADC channels nobody reads off a games console.
 */
struct mesh_node_environment {
    bool valid;
    uint32_t time;
    bool has_temperature;
    float temperature; /* Celsius */
    bool has_humidity;
    float relative_humidity;
    bool has_pressure;
    float barometric_pressure; /* hPa */
    bool has_iaq;
    uint16_t iaq;
    bool has_lux;
    float lux;
    bool has_voltage;
    float voltage;
    bool has_current;
    float current;
};

/*
 * LocalStats: the connected radio talking about itself and the air around it, delivered as a
 * TELEMETRY_APP packet from our own node rather than through the config handshake. Nothing
 * else tells us how busy the channel is, how many packets the radio dropped, or how many of
 * the nodes in its database it still considers online, so without this the Status screen can
 * only report our own bookkeeping.
 *
 * Every field is a plain proto3 scalar (no has_*), so "not reported" and zero are the same
 * value on the wire. That is fine for the counters, where zero is a real answer; the two
 * where it is not - noise floor and heap size - carry their own flag, set from whether the
 * radio sent anything plausible.
 */
struct mesh_radio_stats {
    bool valid;
    uint32_t time; /* our clock when this arrived, not the radio's */
    uint32_t uptime_seconds;
    float channel_utilization; /* percent of airtime busy, all senders */
    float air_util_tx;         /* percent of airtime this radio transmitted in */
    uint32_t num_packets_tx;
    uint32_t num_packets_rx;
    uint32_t num_packets_rx_bad;
    uint32_t num_rx_dupe;
    uint32_t num_tx_relay;
    uint32_t num_tx_relay_canceled;
    uint32_t num_tx_dropped;
    uint32_t num_online_nodes;
    uint32_t num_total_nodes;
    bool has_heap;
    uint32_t heap_total_bytes;
    uint32_t heap_free_bytes;
    bool has_noise_floor;
    int32_t noise_floor; /* dBm */
};

struct mesh_node_summary {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
    bool via_mqtt;
    bool has_hops_away;
    uint8_t hops_away;
    /* Identity, from NodeInfo.user. `user_id` is the "!0a1b2c3d" form the apps show. */
    char user_id[16];
    /* False while the only identity we have is the one derived from the node number; true once
       a real User has arrived, in a NodeInfo or in a NODEINFO_APP packet. */
    bool has_user;
    uint32_t hw_model; /* meshtastic_HardwareModel */
    uint32_t role;     /* meshtastic_Config_DeviceConfig_Role */
    bool is_licensed;
    bool is_unmessagable;
    uint8_t public_key[32];
    uint8_t public_key_len;
    /*
     * Whether the radio's NodeDB still carries this node. The radio's database is small (80
     * entries on the hardware this client targets) and evicts once it fills, while the roster
     * here outlives both the sync and the connection - so a node can be ours to show and a
     * stranger to the radio at the same time, which is also why a message to it may not get
     * out. Resolved every time a config sync completes.
     */
    bool in_nodedb;
    /* Which NodeDB replay last carried this node; bookkeeping for in_nodedb, not display. */
    uint32_t sync_epoch;
    /* NodeDB flags the radio keeps for us. */
    bool is_favorite;
    bool is_ignored;
    bool is_muted;
    uint8_t channel; /* the channel index the radio last heard this node on */
    struct mesh_node_position position;
    struct mesh_node_metrics metrics;
    struct mesh_node_environment environment;
};

enum mesh_traceroute_state {
    MESH_TRACEROUTE_IDLE = 0,
    MESH_TRACEROUTE_PENDING,
    MESH_TRACEROUTE_DONE,
    MESH_TRACEROUTE_TIMEOUT,
};

/*
 * One traceroute: the path a packet took to a node and the path its answer took back, with
 * the SNR of every link on the way. This is the only thing in the client that measures the
 * mesh between two nodes rather than reporting what a node said about itself - `hops_away` on
 * a node record is a count, not a route, and says nothing about which nodes are carrying you.
 *
 * `route` holds the *intermediate* nodes only: the full path towards the target is us, then
 * `route`, then the target, which is why `snr_count` is normally `route_count + 1` - one
 * reading per link rather than per node. The firmware sends SNR scaled by 4 and uses INT8_MIN
 * for a link it could not measure; both are kept raw here and resolved for display.
 *
 * One trace at a time, and the result is kept after it completes so the node detail can show
 * the last known route without re-running it - the firmware rate-limits traceroutes, and a
 * screen that re-traced on every repaint would be refused and would flood the mesh.
 */
struct mesh_traceroute {
    uint8_t state; /* enum mesh_traceroute_state */
    uint32_t target;
    uint32_t packet_id; /* the request, echoed back as Data.request_id */
    uint64_t sent_ms;
    uint32_t completed; /* our clock when the reply landed, 0 while pending */
    uint8_t route_count;
    uint32_t route[MESH_TRACEROUTE_MAX_HOPS];
    uint8_t snr_count;
    int8_t snr[MESH_TRACEROUTE_MAX_HOPS + 1U];
    uint8_t back_count;
    uint32_t route_back[MESH_TRACEROUTE_MAX_HOPS];
    uint8_t snr_back_count;
    int8_t snr_back[MESH_TRACEROUTE_MAX_HOPS + 1U];
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
    /* Describes the radio that is connected right now; cleared with the handshake. */
    struct mesh_radio_stats stats;
    /* Survives reconnects: a NodeDB resync must not wipe the conversation. */
    struct mesh_message_log messages;
    /* The radio's configuration and the admin session; reset with the handshake. */
    struct mesh_radio_settings settings;
    /* The last traceroute, running or finished; reset with the handshake. */
    struct mesh_traceroute traceroute;
    mesh_session_send_fn send;
    void *send_ctx;
    uint32_t next_config_request_id;
    uint32_t next_packet_id;
    /* Counts config syncs, so a node the current NodeDB replay carried can be told from one
       left over from the replay before it. Never 0 once a handshake has started. */
    uint32_t sync_epoch;
    /* The radio the roster describes. Nodes survive a reconnect to the same radio; a different
       radio is a different NodeDB and a different view of the mesh, so the roster is dropped. */
    uint32_t roster_node;
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

/* Folds one node record into the roster with no radio in the loop: used at startup to restore
   what the last run knew, so a node the radio has since evicted is not lost with it. Does
   nothing when the node is already known. */
void mesh_session_seed_node(struct mesh_session *session, const struct mesh_node_summary *node);

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

/*
 * Pins or unpins a node in the radio's NodeDB. The cached record's `is_favorite` is flipped
 * straight away rather than waiting for the radio: there is no get_favorite to read back with
 * and the flag only returns with that node's next NodeInfo, which on a quiet mesh is hours
 * away. Returns the number of admin requests queued, -ENOTCONN before the handshake has
 * my_info, -ENOENT when the node is not in the cache, -ENOSPC when the queue is full.
 */
int mesh_session_set_node_favorite(struct mesh_session *session, uint32_t node_id, bool favorite);

/*
 * Tells the radio to reboot, shut down, or reset itself (mesh_radio_settings_queue_action).
 * Nothing here can confirm that it happened: the radio acts a few seconds after answering and
 * the link drops with it, so the caller announces what was asked for, not what was done.
 *
 * Returns the number of admin requests queued, -ENOTCONN before the handshake has my_info,
 * -EINVAL for a kind that is not an action, -ENOSPC when the queue is full.
 */
int mesh_session_radio_action(struct mesh_session *session, enum mesh_admin_request_kind kind);

/*
 * Flips a node's muted flag in the radio's NodeDB. `toggle_muted_node` is a toggle on the
 * wire - there is no way to state the flag we want, the way the favorite and ignore pair let
 * us - so the cached flag is flipped to match and a press that races an incoming NodeInfo can
 * land on the value it started from. Same returns as mesh_session_set_node_favorite.
 */
int mesh_session_toggle_node_muted(struct mesh_session *session, uint32_t node_id);

/*
 * Drops a node from the radio's NodeDB, and from our cached list with it: there is nothing to
 * read back and the entry would otherwise sit there looking removed-but-present until the
 * next connection. The node returns the moment it transmits anything.
 *
 * Returns the number of admin requests queued, -ENOTCONN before the handshake has my_info,
 * -ENOENT when the node is not in the cache, -EINVAL for our own node, -ENOSPC when the
 * queue is full.
 */
int mesh_session_remove_node(struct mesh_session *session, uint32_t node_id);

/*
 * The radio's own location, set by hand: `set_fixed_position` stores the coordinates and turns
 * `PositionConfig.fixed_position` on, `remove_fixed_position` clears both. Neither goes
 * through set_config - a client that only flipped the config flag would turn fixed position on
 * with nothing behind it - and both are followed by a get_config POSITION, so the section
 * shows what the radio kept. Coordinates are Meshtastic's fixed-point 1e-7 degrees.
 *
 * Returns the number of admin requests queued, -ENOTCONN before the handshake has my_info,
 * -EINVAL for coordinates outside their range, -ENOSPC when the queue is full.
 */
int mesh_session_set_fixed_position(struct mesh_session *session, int32_t latitude_i,
                                    int32_t longitude_i, bool has_altitude, int32_t altitude);
int mesh_session_clear_fixed_position(struct mesh_session *session);

/*
 * Adds or removes a node from the radio's ignore list. Same shape and same caveat as
 * favorite: there is no get_ignored, so the cached flag is flipped here rather than waiting
 * for the node's next NodeInfo - which for a node you are ignoring precisely because it is
 * quiet, or precisely because it is not, may never be a useful wait.
 *
 * Unlike a pin this changes what the radio *does*: an ignored node's packets are dropped
 * before they reach us, so its messages stop arriving and its position stops updating.
 */
int mesh_session_set_node_ignored(struct mesh_session *session, uint32_t node_id, bool ignored);

/*
 * Asks a node to introduce itself: our own User goes out on NODEINFO_APP with want_response,
 * and the node answers with its NodeInfo. This is the only way to put a name to a node that
 * joined after the NodeDB replay - the firmware replays its database exactly once per
 * connection, so a node heard afterwards sits in the list as a bare id until it happens to
 * broadcast, which on a quiet mesh is hours.
 *
 * Returns 0, -ENOTCONN without a link or before the handshake, -EINVAL for a broadcast or for
 * our own node.
 */
int mesh_session_request_node_info(struct mesh_session *session, uint32_t dest);

/* Meshtastic packet ids only need to be unique per sender for a few minutes. Never zero. */
uint32_t mesh_session_next_packet_id(struct mesh_session *session);

/* Borrowed views; valid until the next call into the session. */
const struct mesh_handshake_status *mesh_session_handshake(const struct mesh_session *session);
const struct mesh_message_log *mesh_session_messages(const struct mesh_session *session);
const struct mesh_radio_settings *mesh_session_settings(const struct mesh_session *session);
/* The connected radio's own LocalStats, or a record with `valid` false before one arrives. */
const struct mesh_radio_stats *mesh_session_radio_stats(const struct mesh_session *session);

/*
 * Asks the mesh which way it reaches `dest`, replacing whatever the last trace found. Sends an
 * empty RouteDiscovery on TRACEROUTE_APP with want_response set; every node that forwards it
 * appends itself, and the target answers with both directions filled in.
 *
 * Returns 0, -ENOTCONN without a link, -EINVAL for a broadcast or for our own node, and
 * -EBUSY while a trace is already outstanding. The firmware rate-limits traceroutes, so a
 * caller should not retry on a whim; -EBUSY is the client's own half of that.
 */
int mesh_session_send_traceroute(struct mesh_session *session, uint32_t dest);

/* The last traceroute, running or finished. Never NULL for a live session. */
const struct mesh_traceroute *mesh_session_traceroute(const struct mesh_session *session);

#ifdef __cplusplus
}
#endif
