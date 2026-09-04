#pragma once

#include "mesh/ui/nav.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UI_MAX_DEVICES 16U
/* Nodes carried to the backends, newest-heard first as the radio sends them. Real meshes run
   past 100 nodes; the Nodes tab scrolls, so this is a screen budget, not a mesh limit. */
#define MESH_UI_MAX_HANDSHAKE_NODES 32U
#define MESH_UI_TRANSPORT_STATUS_MAX 32U
/* Newest messages carried to the backends. Matches the transport ring so a per-conversation
   view has the same history the radio gave us; the Brick shows a screenful at a time. */
#define MESH_UI_MAX_MESSAGES 64U
#define MESH_UI_MAX_CHANNELS 8U
#define MESH_UI_CHANNEL_NAME_MAX 12U
#define MESH_UI_MESSAGE_TEXT_MAX 234U

enum mesh_ui_update_flag {
    MESH_UI_UPDATE_NONE = 0U,
    MESH_UI_UPDATE_DISCOVERY = 1U << 0,
    MESH_UI_UPDATE_HANDSHAKE = 1U << 1,
    MESH_UI_UPDATE_TRANSPORT = 1U << 2,
    MESH_UI_UPDATE_MESSAGES = 1U << 3,
    MESH_UI_UPDATE_NAV = 1U << 4,
};
typedef uint32_t mesh_ui_update_flags;

struct mesh_ui_device {
    char identifier[64];
    char name[64];
    int8_t rssi;
    bool connected;
};

struct mesh_ui_node_summary {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
    bool via_mqtt;
    bool has_hops_away;
    uint8_t hops_away;
};

struct mesh_ui_channel {
    uint8_t index;
    uint8_t role; /* 0 disabled, 1 primary, 2 secondary (meshtastic_Channel_Role) */
    char name[MESH_UI_CHANNEL_NAME_MAX];
};

struct mesh_ui_my_info {
    uint32_t node_num;
    uint32_t nodedb_entries;
    uint32_t reboot_count;
};

struct mesh_ui_handshake_state {
    bool request_in_flight;
    uint32_t request_id;
    bool config_complete;
    uint32_t config_complete_id;
    bool has_my_info;
    struct mesh_ui_my_info my_info;
    bool has_config;
    uint32_t node_count;
    char primary_channel[33];
    char my_short_name[6];
    struct mesh_ui_node_summary nodes[MESH_UI_MAX_HANDSHAKE_NODES];
    /* Channel table by slot; disabled slots are present with role 0. */
    uint32_t channel_count;
    struct mesh_ui_channel channels[MESH_UI_MAX_CHANNELS];
    bool cached;
};

/* One line of conversation, already resolved for display: peer_name is the short name from
   the NodeDB when we know it, so backends never have to join against the node list. */
struct mesh_ui_message {
    uint32_t packet_id;
    uint32_t peer; /* the other end: sender for inbound, destination for outbound */
    uint32_t rx_time;
    char peer_name[16];
    char text[MESH_UI_MESSAGE_TEXT_MAX];
    uint8_t channel;
    uint8_t direction; /* enum mesh_message_direction */
    uint8_t ack;       /* enum mesh_message_ack */
    bool broadcast;
};

struct mesh_ui_message_list {
    struct mesh_ui_message entries[MESH_UI_MAX_MESSAGES];
    uint32_t count;
    uint32_t dropped; /* older messages the transport ring has already discarded */
};

struct mesh_ui_snapshot {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    struct mesh_ui_message_list messages;
    /* Transport state ("waiting-for-bluez", "scanning", "running", ...). Rendered by the
       backends so an empty device list is diagnosable on a device with no console. */
    char transport_status[MESH_UI_TRANSPORT_STATUS_MAX];
    /* Cursor, current tab, compose target: what the user is doing, as opposed to what the
       radio is doing. Clamped to the lists above before every snapshot. */
    struct mesh_ui_nav nav;
    mesh_ui_update_flags update_flags;
};

struct mesh_ui_store {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    struct mesh_ui_message_list messages;
    char transport_status[MESH_UI_TRANSPORT_STATUS_MAX];
    struct mesh_ui_nav nav;
    int event_fd;
    mesh_ui_update_flags pending_flags;
};

int mesh_ui_store_init(struct mesh_ui_store *store);
void mesh_ui_store_shutdown(struct mesh_ui_store *store);
void mesh_ui_store_reset(struct mesh_ui_store *store);

int mesh_ui_store_event_fd(const struct mesh_ui_store *store);

void mesh_ui_store_set_discovery(struct mesh_ui_store *store, const struct mesh_ui_device *devices,
                                 size_t count);
void mesh_ui_store_set_handshake(struct mesh_ui_store *store,
                                 const struct mesh_ui_handshake_state *handshake);
void mesh_ui_store_set_transport_status(struct mesh_ui_store *store, const char *status);
void mesh_ui_store_set_messages(struct mesh_ui_store *store,
                                const struct mesh_ui_message_list *messages);

/* Combines persisted history with this session's live messages into the newest
   MESH_UI_MAX_MESSAGES, cached entries first. A cached entry whose packet id also appears in
   `live` is dropped, so a message re-received after a restart is not shown twice.

   This exists because the transport's message log starts empty on every run: without merging,
   the first publish would push an empty list over the cache loaded at startup and the next
   save would erase the conversation for good. */
void mesh_ui_message_list_merge(const struct mesh_ui_message_list *cached,
                                const struct mesh_ui_message_list *live,
                                struct mesh_ui_message_list *out);

/* Navigation. A key press moves the cursor or switches tabs and, for A on an actionable row,
   fills *out_action for the caller to carry out (connect, send). Returns true when the frame
   needs repainting; the store has already signalled its eventfd in that case. */
bool mesh_ui_store_handle_key(struct mesh_ui_store *store, enum mesh_ui_key key,
                              struct mesh_ui_action *out_action);
/* Show a transient one-line notice on the backends ("Sent to ABCD"). */
void mesh_ui_store_set_toast(struct mesh_ui_store *store, uint64_t now_ms, const char *text);
/* Time-based housekeeping (toast expiry). Call once per loop turn. */
void mesh_ui_store_tick(struct mesh_ui_store *store, uint64_t now_ms);

/* Force the next consume_updates() to yield a snapshot even when nothing changed.
   The setters above deliberately stay quiet when state is unchanged, so without this a
   client that starts with no devices and no handshake would never paint a first frame. */
void mesh_ui_store_request_refresh(struct mesh_ui_store *store);

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot);

int mesh_ui_store_save(const struct mesh_ui_store *store, const char *path);
int mesh_ui_store_load(struct mesh_ui_store *store, const char *path);

#ifdef __cplusplus
}
#endif
