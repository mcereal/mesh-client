#pragma once

/*
 * The connected radio's configuration, as the radio reports it, plus the AdminMessage
 * plumbing needed to ask for it again and (later) to change it. Transport-agnostic: the BLE
 * transport feeds FromRadio fragments and ADMIN_APP packets in and pulls encoded ToRadio
 * requests out; serial/TCP would do the same. docs/settings-roadmap.md explains the plan.
 *
 * Every admin reply carries a session passkey that a set_* must echo back (firmware 2.5+
 * rejects a write without one, and keys live for five minutes). Writes therefore always go out
 * behind a fresh get_owner_request, and are followed by the matching get_* so the view returns
 * to what the radio actually holds. A set_* is answered with a Routing packet quoting our
 * packet id: error NONE is the ack, anything else (ADMIN_BAD_SESSION_KEY in particular) is a
 * rejection.
 */

#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a request asks for. The CONFIG and MODULE_CONFIG kinds carry the section type. */
enum mesh_admin_request_kind {
    MESH_ADMIN_GET_OWNER = 0,
    MESH_ADMIN_GET_METADATA,
    MESH_ADMIN_GET_CONFIG,        /* type = meshtastic_AdminMessage_ConfigType */
    MESH_ADMIN_GET_MODULE_CONFIG, /* type = meshtastic_AdminMessage_ModuleConfigType */
    MESH_ADMIN_SET_OWNER,         /* payload.owner */
    MESH_ADMIN_SET_CONFIG,        /* type as GET_CONFIG; payload.config with its oneof set */
    MESH_ADMIN_SET_MODULE_CONFIG, /* type as GET_MODULE_CONFIG; payload.module_config */
    MESH_ADMIN_GET_CHANNEL,       /* type = channel index (sent as index + 1 on the wire) */
    MESH_ADMIN_SET_CHANNEL,       /* type = channel index; payload.channel */
    MESH_ADMIN_SET_TIME,          /* type = UTC epoch seconds; no payload, nothing to read back */
    MESH_ADMIN_SET_FAVORITE,      /* type = node number to pin in the radio's NodeDB */
    MESH_ADMIN_REMOVE_FAVORITE,   /* type = node number to unpin */
    MESH_ADMIN_SET_IGNORED,       /* type = node number the radio should drop packets from */
    MESH_ADMIN_REMOVE_IGNORED,    /* type = node number to stop ignoring */
    /* Radio actions: things the radio *does* once rather than settings it keeps. `type` is the
       delay in seconds for REBOOT and SHUTDOWN and is ignored by the three resets. Nothing is
       read back - there is no state to re-read, and by the time an answer could be built the
       radio is on its way down. */
    MESH_ADMIN_REBOOT,
    MESH_ADMIN_SHUTDOWN,
    MESH_ADMIN_RESET_NODEDB,
    MESH_ADMIN_FACTORY_RESET_CONFIG, /* config to defaults, BLE bonds kept */
    MESH_ADMIN_FACTORY_RESET_DEVICE, /* everything to defaults, BLE bonds cleared */
    /* The radio's own location, set by hand. Unlike the rest of the Position section these do
       not go through set_config: the firmware stores the coordinates *and* sets
       `position.fixed_position` itself, so a client that only flipped the config flag would
       turn fixed position on with no position behind it. Both are read back with a
       get_config POSITION, which is why `type` carries the ConfigType. */
    MESH_ADMIN_SET_FIXED_POSITION,    /* payload.position */
    MESH_ADMIN_REMOVE_FIXED_POSITION, /* no payload */
    /* NodeDB entries, addressed by node number in `type`, alongside the favorite and ignore
       pair above. */
    MESH_ADMIN_REMOVE_NODE,  /* drop this node from the radio's NodeDB */
    MESH_ADMIN_TOGGLE_MUTED, /* flip this node's muted flag; the verb is a toggle, not a set */
};

/* How long the radio is told to wait before a reboot or a shutdown. Not zero: the firmware
   answers before it acts, and the Routing ack has to get out of the door while the radio is
   still listening. It is also just long enough to notice a mistake. */
#define MESH_RADIO_ACTION_DELAY_SECONDS 5U

struct mesh_admin_request {
    enum mesh_admin_request_kind kind;
    uint32_t type;
    uint32_t my_node;   /* MeshPacket.to: admin goes to ourselves */
    uint32_t packet_id; /* MeshPacket.id; replies quote it in Data.request_id */
    /* What a set_* carries. The firmware replaces the whole section, so this is the full
       struct as the radio last reported it with the edits applied, never a partial. */
    union {
        meshtastic_User owner;
        meshtastic_Config config;
        meshtastic_ModuleConfig module_config;
        meshtastic_Channel channel;
        meshtastic_Position position;
    } payload;
};

/* True for the SET_* kinds the Settings tab asks for, which are counted and announced.
   MESH_ADMIN_SET_TIME is deliberately not one of them: the clock is pushed by itself on every
   connect, and it must not toast "saved" or occupy the ", saving" marker the user's own save
   owns. It is still acked by a Routing reply like any other set_*; that reply just releases
   the queue without touching the counters. The favorite kinds are out for the same reason -
   they are a press on the Nodes tab, not a settings section, and must not make the Settings
   tab claim an unsaved write is in flight. The radio actions are out for a sharper version of
   the same reason: a rebooting or powered-off radio stops answering mid-request, and a missing
   ack there is the action working, not a save being rejected. */
bool mesh_admin_request_is_write(enum mesh_admin_request_kind kind);

/* True for the kinds that make the radio do something rather than keep something. */
bool mesh_admin_request_is_action(enum mesh_admin_request_kind kind);

/* last_write_error when the radio never answered a set_*. Routing errors are positive. */
#define MESH_RADIO_SETTINGS_WRITE_TIMEOUT (-1)

/* Queue depth: a full refresh is 13 sections plus every channel slot, and a save adds three. */
/* A full refresh queues the probe pair, every Config section, every ModuleConfig section and
   every channel slot - 27 with the modules phase 10 keeps, and one per module after that. A
   full queue drops silently, so this is sized well clear of the phases still to come rather
   than to what fits today. */
#define MESH_RADIO_SETTINGS_FETCH_MAX 48U
#define MESH_RADIO_SETTINGS_MAX_CHANNELS 8U
/* A reply that has not arrived after this long is given up on and the queue moves on. */
#define MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS 5000U
/* Floor on a clock we are willing to push at a radio: 2025-01-01T00:00:00Z. A Brick whose RTC
   has been lost reads back somewhere near the epoch, and a node with no time at all is better
   off than a node confidently set to 1970. */
#define MESH_RADIO_CLOCK_MIN_EPOCH 1735689600U

struct mesh_radio_settings {
    bool has_device;
    meshtastic_Config_DeviceConfig device;
    bool has_position;
    meshtastic_Config_PositionConfig position;
    bool has_power;
    meshtastic_Config_PowerConfig power;
    bool has_network;
    meshtastic_Config_NetworkConfig network;
    bool has_display;
    meshtastic_Config_DisplayConfig display;
    bool has_lora;
    meshtastic_Config_LoRaConfig lora;
    bool has_bluetooth;
    meshtastic_Config_BluetoothConfig bluetooth;
    bool has_security;
    meshtastic_Config_SecurityConfig security;
    bool has_mqtt;
    meshtastic_ModuleConfig_MQTTConfig mqtt;
    bool has_store_forward;
    meshtastic_ModuleConfig_StoreForwardConfig store_forward;
    bool has_telemetry;
    meshtastic_ModuleConfig_TelemetryConfig telemetry;
    bool has_neighbor_info;
    meshtastic_ModuleConfig_NeighborInfoConfig neighbor_info;
    bool has_range_test;
    meshtastic_ModuleConfig_RangeTestConfig range_test;
    bool has_paxcounter;
    meshtastic_ModuleConfig_PaxcounterConfig paxcounter;
    bool has_tak;
    meshtastic_ModuleConfig_TAKConfig tak;
    bool has_ambient_lighting;
    meshtastic_ModuleConfig_AmbientLightingConfig ambient_lighting;
    bool has_status_message;
    meshtastic_ModuleConfig_StatusMessageConfig status_message;
    bool has_owner;
    meshtastic_User owner;
    bool has_metadata;
    meshtastic_DeviceMetadata metadata;
    /* The channel table as the radio sent it, by slot, keys included: set_channel must carry
       the whole Channel back. Never persisted. */
    bool has_channel[MESH_RADIO_SETTINGS_MAX_CHANNELS];
    meshtastic_Channel channels[MESH_RADIO_SETTINGS_MAX_CHANNELS];

    /* Admin session. */
    bool has_session_passkey;
    uint8_t session_passkey[8];
    size_t session_passkey_len;
    uint32_t admin_replies; /* ADMIN_APP replies decoded since reset */

    /* One-at-a-time request queue. */
    struct mesh_admin_request queue[MESH_RADIO_SETTINGS_FETCH_MAX];
    size_t queue_head;
    size_t queue_len;
    uint32_t pending_request_id; /* 0 = nothing in flight */
    uint64_t pending_sent_at_ms;
    bool pending_is_write;
    unsigned timeouts;

    /* Write outcomes, counted so the app can announce each one once. */
    uint32_t writes_sent;
    uint32_t writes_acked;    /* Routing reply with error NONE */
    uint32_t writes_failed;   /* Routing error, timeout, or the GATT write itself failed */
    int32_t last_write_error; /* meshtastic_Routing_Error, MESH_RADIO_SETTINGS_WRITE_TIMEOUT,
                                 or a negative errno from the transport */
};

void mesh_radio_settings_reset(struct mesh_radio_settings *settings);

/* True once any section, the owner or the metadata has arrived. */
bool mesh_radio_settings_loaded(const struct mesh_radio_settings *settings);

/* Fold in fragments the radio streams during the want_config handshake. */
void mesh_radio_settings_apply_config(struct mesh_radio_settings *settings,
                                      const meshtastic_Config *config);
void mesh_radio_settings_apply_module_config(struct mesh_radio_settings *settings,
                                             const meshtastic_ModuleConfig *config);
void mesh_radio_settings_apply_metadata(struct mesh_radio_settings *settings,
                                        const meshtastic_DeviceMetadata *metadata);
void mesh_radio_settings_apply_owner(struct mesh_radio_settings *settings,
                                     const meshtastic_User *owner);
void mesh_radio_settings_apply_channel(struct mesh_radio_settings *settings,
                                       const meshtastic_Channel *channel);

/* Folds an ADMIN_APP packet in: captures the session passkey, stores whatever get_*_response
   it carries, and releases the fetch queue when it answers the pending request. A ROUTING_APP
   packet answering the pending request (the ack or rejection of a set_*) is consumed the same
   way. Returns 1 when the packet was ours (the caller should not treat it as a message), 0
   when it was something else, a negative errno on bad input. */
int mesh_radio_settings_ingest(struct mesh_radio_settings *settings,
                               const meshtastic_MeshPacket *packet);

/* Encodes one request as a ToRadio protobuf ready for a single BLE write: a MeshPacket to
   ourselves on ADMIN_APP with want_response set, carrying the passkey we hold (harmless on a
   get; required on a set). Returns 0 and sets *written on success. */
int mesh_radio_settings_encode_request(const struct mesh_radio_settings *settings,
                                       const struct mesh_admin_request *request, uint8_t *out,
                                       size_t out_len, size_t *written);

/* Fetch queue. queue_probe() asks for the owner and the metadata (enough to prove the admin
   round trip); queue_all() asks for everything the Settings tab shows, every channel slot
   included. Duplicates of a kind already queued are skipped. Returns the number added. */
size_t mesh_radio_settings_queue_probe(struct mesh_radio_settings *settings);
size_t mesh_radio_settings_queue_all(struct mesh_radio_settings *settings);

/* Queues one write: a get_owner_request first (a fresh passkey, since the one we hold may be
   minutes old), the set_* itself, then the get_* for the same section. `write->kind` must be
   a SET_* kind with its payload filled in; my_node and packet_id are assigned at send time.
   Returns the number of requests queued, -EINVAL for a non-write, -ENOSPC when the queue
   cannot take all three. */
int mesh_radio_settings_queue_write(struct mesh_radio_settings *settings,
                                    const struct mesh_admin_request *write);
/* Queues a clock push: a get_owner_request for a fresh passkey, then set_time_only carrying
   `epoch` (UTC seconds). There is no get_time, so nothing is read back. Returns the number of
   requests queued, -EINVAL for an epoch below MESH_RADIO_CLOCK_MIN_EPOCH, -ENOSPC when the
   queue cannot take both. */
int mesh_radio_settings_queue_time(struct mesh_radio_settings *settings, uint32_t epoch);

/* Pins or unpins a node in the radio's NodeDB, the same shape as the clock push: a get_owner
   for a fresh passkey, then the set. There is no get_favorite to read back with - the flag
   comes home on the next NodeInfo for that node, so the caller updates its own copy. */
int mesh_radio_settings_queue_favorite(struct mesh_radio_settings *settings, uint32_t node_id,
                                       bool favorite);

/* Adds or removes a node from the radio's ignore list. Identical in shape to the favorite
   pair above, and identical in consequence: no get_ignored exists either, so the caller keeps
   its own copy of the flag in step. */
int mesh_radio_settings_queue_ignored(struct mesh_radio_settings *settings, uint32_t node_id,
                                      bool ignored);

/* Drops a node from the radio's NodeDB, and flips a node's muted flag. The same shape again -
   a passkey refresh then the verb, nothing to read back. `toggle_muted_node` really is a
   toggle on the wire: unlike the favorite and ignore pair there is no way to state the wanted
   flag, so a press that races an incoming NodeInfo can land on the value it started from. */
int mesh_radio_settings_queue_remove_node(struct mesh_radio_settings *settings, uint32_t node_id);
int mesh_radio_settings_queue_toggle_muted(struct mesh_radio_settings *settings, uint32_t node_id);

/* Queues one radio action, the same shape again: a get_owner for a fresh passkey (the firmware
   rejects these without one exactly as it rejects a set_*), then the action itself. `seconds`
   is the delay for MESH_ADMIN_REBOOT and MESH_ADMIN_SHUTDOWN and is ignored by the resets.
   Returns the number of requests queued, -EINVAL for a kind that is not an action, -ENOSPC
   when the queue cannot take both. A second press before the first has gone out is the same
   request and is not queued twice. */
int mesh_radio_settings_queue_action(struct mesh_radio_settings *settings,
                                     enum mesh_admin_request_kind kind, uint32_t seconds);

/* True while a set_* is queued or awaiting its reply. */
bool mesh_radio_settings_write_pending(const struct mesh_radio_settings *settings);
/* Records that a dequeued request could not be sent at all (the caller's write failed). */
void mesh_radio_settings_mark_unsent(struct mesh_radio_settings *settings,
                                     const struct mesh_admin_request *request, int error);

/* Hands out the next request to send when nothing is in flight (or the in-flight one has
   timed out). The caller fills in packet_id/my_node, encodes and sends it, then calls
   mark_sent(). Returns false when there is nothing to send right now. */
bool mesh_radio_settings_next_request(struct mesh_radio_settings *settings, uint64_t now_ms,
                                      struct mesh_admin_request *out);
void mesh_radio_settings_mark_sent(struct mesh_radio_settings *settings, uint32_t packet_id,
                                   uint64_t now_ms);
/* True while a reply is awaited. */
bool mesh_radio_settings_busy(const struct mesh_radio_settings *settings);

/* Human names for the enums the Settings tab shows; "?" / a numeric fallback when unknown. */
const char *mesh_radio_role_name(uint32_t role);
const char *mesh_radio_region_name(uint32_t region);
const char *mesh_radio_modem_preset_name(uint32_t preset);
const char *mesh_radio_hw_model_name(uint32_t model, char *fallback, size_t fallback_len);

#ifdef __cplusplus
}
#endif
