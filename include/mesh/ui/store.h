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
#define MESH_UI_MAX_HANDSHAKE_NODES 128U
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
    MESH_UI_UPDATE_SETTINGS = 1U << 5,
    MESH_UI_UPDATE_TRACEROUTE = 1U << 6,
};
typedef uint32_t mesh_ui_update_flags;

/* How a device is reached. The Devices tab lists both kinds in one list, and the app routes
   a connect to the matching transport. */
enum mesh_ui_device_kind {
    MESH_UI_DEVICE_BLE = 0,
    MESH_UI_DEVICE_SERIAL,
};

struct mesh_ui_device {
    char identifier[64];
    char name[64];
    int8_t rssi; /* BLE only; 0 for a USB port */
    bool connected;
    /* BLE only: BlueZ holds a bond for this node. A node in PIN mode that is not paired
       connects and then fails, so the row says so before the user presses A. Always true for
       a USB port, which has nothing to pair. */
    bool paired;
    /* The link this row is being brought up on right now (connecting, or bonding). */
    bool busy;
    uint8_t kind; /* enum mesh_ui_device_kind */
};

/* The node detail the Nodes tab drills into, mirroring the session's structs without nanopb.
   Latitude and longitude stay in Meshtastic's fixed-point 1e-7 degrees. */
struct mesh_ui_node_position {
    bool valid;
    int32_t latitude_i;
    int32_t longitude_i;
    bool has_altitude;
    int32_t altitude;
    uint32_t time;
    uint8_t sats_in_view;
    uint8_t precision_bits;
};

struct mesh_ui_node_metrics {
    bool valid;
    uint32_t time;
    bool has_battery;
    uint8_t battery_level; /* 101 means "plugged in" */
    bool has_voltage;
    float voltage;
    bool has_channel_utilization;
    float channel_utilization;
    bool has_air_util_tx;
    float air_util_tx;
    bool has_uptime;
    uint32_t uptime_seconds;
};

struct mesh_ui_node_environment {
    bool valid;
    uint32_t time;
    bool has_temperature;
    float temperature;
    bool has_humidity;
    float relative_humidity;
    bool has_pressure;
    float barometric_pressure;
    bool has_iaq;
    uint16_t iaq;
    bool has_lux;
    float lux;
    bool has_voltage;
    float voltage;
    bool has_current;
    float current;
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
    char user_id[16];
    uint32_t hw_model;
    uint32_t role;
    bool is_licensed;
    bool is_unmessagable;
    uint8_t public_key[32];
    uint8_t public_key_len;
    bool is_favorite;
    bool is_ignored;
    bool is_muted;
    uint8_t channel;
    struct mesh_ui_node_position position;
    struct mesh_ui_node_metrics metrics;
    struct mesh_ui_node_environment environment;
};

struct mesh_ui_channel {
    uint8_t index;
    uint8_t role; /* 0 disabled, 1 primary, 2 secondary (meshtastic_Channel_Role) */
    char name[MESH_UI_CHANNEL_NAME_MAX];
    uint8_t psk_len; /* 0 none, 1 default-key index, 16 AES-128, 32 AES-256 */
    bool uplink_enabled;
    bool downlink_enabled;
    uint32_t position_precision;
};

/* One channel slot with everything set_channel needs, keys included. Lives in the settings
   (never persisted) rather than the cached handshake. */
#define MESH_UI_PSK_MAX 32U
struct mesh_ui_channel_detail {
    bool present;
    uint8_t index;
    uint8_t role; /* meshtastic_Channel_Role */
    char name[MESH_UI_CHANNEL_NAME_MAX];
    uint8_t psk[MESH_UI_PSK_MAX];
    uint8_t psk_len;
    bool uplink_enabled;
    bool downlink_enabled;
    uint32_t position_precision;
};

/*
 * The client's own facts, for the Settings tab's About section - as opposed to every other
 * field below, which describes the radio. Filled by mesh_app_publish_ui_state(); it is a
 * flattened, updater-free copy for the same reason the radio's settings are a nanopb-free one,
 * so the nav and the backends depend on neither.
 */
#define MESH_UI_CLIENT_TEXT_MAX 64U
#define MESH_UI_CLIENT_PATH_MAX 128U
#define MESH_UI_CLIENT_MESSAGE_MAX 96U

struct mesh_ui_client_info {
    char version[MESH_UI_CLIENT_TEXT_MAX];  /* "1.12.0", or "dev" */
    char backend[MESH_UI_CLIENT_TEXT_MAX];  /* the UI backend actually in use */
    char data_dir[MESH_UI_CLIENT_PATH_MAX]; /* where preferences and caches are kept */
    /* enum mesh_update_state (mesh/updater.h), carried as a byte so this header does not
       have to pull the updater in. */
    uint8_t update_state;
    char update_message[MESH_UI_CLIENT_MESSAGE_MAX];
    char update_latest[MESH_UI_CLIENT_TEXT_MAX];
    /* The update channel as the About row shows it ("Stable", "Prerelease", "Automatic
       (stable)"). A name rather than the enum for the same reason the state is a byte: the
       backends and the nav never include the updater. */
    char update_channel[MESH_UI_CLIENT_TEXT_MAX];
    /* False when the device has no curl or wget, or the running binary could not be located:
       the About section then shows why instead of an update row that cannot work. */
    bool update_supported;
    /* A check or a download is in flight, so the action row reads as busy and a second press
       does not stack another child. */
    bool update_busy;
    /* False for a build that is not an official release (and has not opted in through
       MESHCLIENT_UPDATE_ALLOW_DEV): the check still runs and reports what is out there, but
       no install row is offered, because pressing it could not do anything. */
    bool update_can_install;
    /* An official release. False means the dev-updates toggle is shown, because on a release
       build it would be a switch with nothing behind it. */
    bool update_is_release;
    /* The dev-updates toggle's own position, and whether the environment is holding it on -
       an env override the user cannot see would otherwise look like a row that ignores them. */
    bool update_allow_dev;
    bool update_allow_dev_from_env;
};

/*
 * The connected radio's configuration, flattened from the protobufs the transport decoded so
 * the backends and the settings table never include nanopb. Every `has_*` says whether that
 * section has arrived this connection; `loaded` is any of them. Read-only in phase 1 of
 * docs/settings-roadmap.md; the same fields become the edit targets later.
 */
/* Us, plus RouteDiscovery's eight intermediate slots, plus the far end. */
#define MESH_UI_TRACEROUTE_MAX_HOPS 10U
#define MESH_UI_TRACEROUTE_NAME_MAX 16U

/*
 * One stop on a traced route, already resolved for drawing: the node's name if the client
 * knows it and its id if not, and the SNR of the link *into* it. The first hop of a path is
 * the sender and so has no incoming link, which is what `has_snr` false means there.
 */
struct mesh_ui_traceroute_hop {
    uint32_t node_id;
    char name[MESH_UI_TRACEROUTE_NAME_MAX];
    bool has_snr;
    int8_t snr_quarter_db; /* the wire scale: dB * 4 */
};

/*
 * The last traceroute as the UI needs it: two ready-made paths rather than the protobuf's
 * intermediate-nodes-and-parallel-SNR-arrays. `app.c` resolves the names and stitches us and
 * the target onto the ends, so the renderer only walks a list - the same division the node
 * summary follows, and the only place that knows a route's shape.
 *
 * Not persisted: a route is true for about as long as the mesh holds still.
 */
struct mesh_ui_traceroute {
    uint8_t state; /* enum mesh_traceroute_state, carried as a byte */
    uint32_t target;
    uint32_t completed; /* our clock when the reply landed; 0 while pending */
    uint8_t forward_count;
    struct mesh_ui_traceroute_hop forward[MESH_UI_TRACEROUTE_MAX_HOPS];
    uint8_t back_count;
    struct mesh_ui_traceroute_hop back[MESH_UI_TRACEROUTE_MAX_HOPS];
};

/*
 * What the connected radio reports about itself and the air around it (LocalStats telemetry).
 * Lives beside the radio's configuration because it has the same lifetime - it describes the
 * radio that is connected right now - and is likewise never persisted.
 */
struct mesh_ui_radio_stats {
    bool valid;
    uint32_t time; /* our clock when it arrived */
    uint32_t uptime_seconds;
    float channel_utilization;
    float air_util_tx;
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
    int32_t noise_floor;
};

struct mesh_ui_settings {
    /* The client's own facts. Always populated, radio or no radio - the About section is the
       one part of this tab that does not need a connection. */
    struct mesh_ui_client_info client;
    /* Mesh health, from LocalStats telemetry rather than from the config handshake, so it
       fills in on its own schedule and is absent until the radio's first report. */
    struct mesh_ui_radio_stats stats;
    bool loaded;
    bool admin_ok;      /* at least one AdminMessage reply came back this connection */
    bool admin_busy;    /* a refresh is in flight */
    bool write_pending; /* a set_* is queued or awaiting its ack */
    uint32_t admin_replies;

    bool has_owner;
    char long_name[40];
    char short_name[5];
    bool is_licensed;
    bool is_unmessagable;

    bool has_device;
    uint8_t role;
    uint8_t rebroadcast_mode;
    char tzdef[65];
    bool led_heartbeat_disabled;
    bool double_tap_as_button_press;
    uint32_t node_info_broadcast_secs;

    bool has_display;
    uint32_t screen_on_secs;
    uint32_t carousel_secs;
    uint8_t compass_orientation;
    bool use_12h_clock;
    uint8_t units; /* 0 metric, 1 imperial */
    bool flip_screen;

    bool has_lora;
    bool use_preset;
    uint8_t modem_preset;
    uint8_t region;
    uint32_t bandwidth;
    uint32_t spread_factor;
    uint32_t coding_rate;
    uint8_t hop_limit;
    bool tx_enabled;
    int8_t tx_power;
    bool ignore_mqtt;
    bool config_ok_to_mqtt;

    bool has_bluetooth;
    bool bluetooth_enabled;
    uint8_t pairing_mode; /* 0 random pin, 1 fixed pin, 2 no pin */
    uint32_t fixed_pin;

    bool has_security;
    uint8_t public_key[32];
    uint8_t public_key_len;
    bool has_private_key;
    uint8_t private_key[32]; /* revealed only through the keyboard, for backup */
    uint8_t private_key_len;
    uint8_t admin_key_count;
    uint8_t admin_keys[3][32];
    uint8_t admin_key_lens[3];
    bool is_managed;
    bool serial_enabled;
    bool debug_log_api_enabled;
    bool admin_channel_enabled;
    uint8_t packet_signature_policy;

    bool has_position;
    /* Where the radio says it is. Not part of PositionConfig - it comes from our own node's
       NodeInfo - but it lives here because the Position section is where it is shown and set,
       and because `set_fixed_position` is the one write in that section that carries it. */
    bool has_own_position;
    int32_t own_latitude_i; /* fixed-point 1e-7 degrees, as the wire carries them */
    int32_t own_longitude_i;
    bool has_own_altitude;
    int32_t own_altitude; /* metres above sea level */
    uint8_t gps_mode;     /* 0 disabled, 1 enabled, 2 not present */
    uint32_t position_broadcast_secs;
    bool position_broadcast_smart_enabled;
    bool fixed_position;
    uint32_t gps_update_interval;
    uint32_t smart_minimum_distance; /* metres */
    uint32_t smart_minimum_interval_secs;

    bool has_power;
    bool is_power_saving;
    uint32_t ls_secs;
    uint32_t min_wake_secs;
    uint32_t on_battery_shutdown_after_secs;
    uint32_t wait_bluetooth_secs;

    bool has_mqtt;
    bool mqtt_enabled;
    char mqtt_address[64];
    char mqtt_username[64];
    char mqtt_password[32];
    char mqtt_root[32];
    bool mqtt_encryption_enabled;
    bool mqtt_tls_enabled;
    bool mqtt_map_reporting_enabled;
    /* Read-only on purpose: with this on the radio hands its MQTT traffic to the attached
       client to relay, and this client does not implement MqttClientProxyMessage. */
    bool mqtt_proxy_to_client_enabled;

    bool has_store_forward;
    bool store_forward_enabled;
    bool store_forward_heartbeat;
    bool store_forward_is_server;

    bool has_telemetry;
    uint32_t device_update_interval;
    bool device_telemetry_enabled;
    bool environment_measurement_enabled;
    bool environment_screen_enabled;
    bool environment_display_fahrenheit;
    bool air_quality_enabled;
    bool power_measurement_enabled;

    bool has_channels; /* any slot present */
    struct mesh_ui_channel_detail channels[MESH_UI_MAX_CHANNELS];

    bool has_metadata;
    char firmware_version[18];
    uint32_t hw_model;
    bool has_wifi;
    bool has_bluetooth_radio;
    bool has_ethernet;
    bool has_pkc;
    bool can_shutdown;
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
    /* meshtastic_Routing_Error behind an ack of FAILED, so the row can say why rather than
       just marking it failed. Meaningless for anything else. */
    uint8_t ack_error;
    bool broadcast;
};

struct mesh_ui_message_list {
    struct mesh_ui_message entries[MESH_UI_MAX_MESSAGES];
    uint32_t count;
    uint32_t dropped; /* older messages the transport ring has already discarded */
};

/* Enough for every channel slot plus the peers anyone realistically keeps in view; the oldest
   mark is evicted once they are all taken. */
#define MESH_UI_READ_MARKS_MAX 32U

/*
 * "Everything up to and including this packet in this conversation has been seen." A packet id
 * rather than a timestamp or an index: ids survive the ring evicting older messages and the
 * cache merging history back in, and a mark whose message has since been evicted correctly
 * reads as "everything still in view arrived after it".
 */
struct mesh_ui_read_mark {
    uint8_t kind; /* enum mesh_ui_conversation_kind: CHANNEL or DIRECT */
    uint8_t channel;
    uint32_t node;
    uint32_t packet_id;
    uint32_t stamp; /* bumped on every write, so the least recently read can be evicted */
};

struct mesh_ui_read_state {
    struct mesh_ui_read_mark marks[MESH_UI_READ_MARKS_MAX];
    uint32_t count;
    uint32_t stamp;
};

struct mesh_ui_snapshot {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    struct mesh_ui_message_list messages;
    /* Which conversations have been read, so the list can badge the ones that have not. */
    struct mesh_ui_read_state read_state;
    /* Transport state ("waiting-for-bluez", "scanning", "running", ...). Rendered by the
       backends so an empty device list is diagnosable on a device with no console. */
    char transport_status[MESH_UI_TRANSPORT_STATUS_MAX];
    /* Cursor, current tab, compose target: what the user is doing, as opposed to what the
       radio is doing. Clamped to the lists above before every snapshot. */
    struct mesh_ui_nav nav;
    /* Radio configuration for the Settings tab. Not persisted: it describes the radio that
       is connected right now. */
    struct mesh_ui_settings settings;
    /* The last traceroute, running or finished. Not persisted. */
    struct mesh_ui_traceroute traceroute;
    mesh_ui_update_flags update_flags;
};

struct mesh_ui_store {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    struct mesh_ui_message_list messages;
    struct mesh_ui_read_state read_state;
    char transport_status[MESH_UI_TRANSPORT_STATUS_MAX];
    struct mesh_ui_nav nav;
    struct mesh_ui_settings settings;
    struct mesh_ui_traceroute traceroute;
    int event_fd;
    mesh_ui_update_flags pending_flags;
};

int mesh_ui_store_init(struct mesh_ui_store *store);
void mesh_ui_store_shutdown(struct mesh_ui_store *store);

int mesh_ui_store_event_fd(const struct mesh_ui_store *store);

void mesh_ui_store_set_discovery(struct mesh_ui_store *store, const struct mesh_ui_device *devices,
                                 size_t count);
void mesh_ui_store_set_handshake(struct mesh_ui_store *store,
                                 const struct mesh_ui_handshake_state *handshake);
void mesh_ui_store_set_transport_status(struct mesh_ui_store *store, const char *status);
void mesh_ui_store_set_messages(struct mesh_ui_store *store,
                                const struct mesh_ui_message_list *messages);
/* Replaces the radio settings view; quiet when nothing changed. */
void mesh_ui_store_set_settings(struct mesh_ui_store *store,
                                const struct mesh_ui_settings *settings);
/* Replaces the traceroute view; quiet when nothing changed. */
void mesh_ui_store_set_traceroute(struct mesh_ui_store *store,
                                  const struct mesh_ui_traceroute *traceroute);

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
/* Raises (or takes down) the BLE pairing prompt. Called from the app when the BlueZ agent has
   a question outstanding, not from the key handler; see mesh_ui_nav_open_passkey(). */
void mesh_ui_store_open_passkey_prompt(struct mesh_ui_store *store, const char *label,
                                       uint32_t passkey, bool confirm);
void mesh_ui_store_close_passkey_prompt(struct mesh_ui_store *store);
/* Drops the pending Settings edits: the app calls this once a save has been queued. */
void mesh_ui_store_settings_edits_clear(struct mesh_ui_store *store);

/* Clears only the edits `consumer` has just written and keeps the rest, because the Position
   section has two submission paths: a latitude typed but not yet pinned has to survive a Y
   that saves the GPS rows, and the GPS rows have to survive a "Set fixed position". Every
   other section has one path, so clearing SECTION there clears the lot. */
void mesh_ui_store_settings_edits_consumed(struct mesh_ui_store *store,
                                           enum mesh_ui_setting_consumer consumer);
/* Time-based housekeeping (toast expiry). Call once per loop turn. */
void mesh_ui_store_tick(struct mesh_ui_store *store, uint64_t now_ms);

/* Force the next consume_updates() to yield a snapshot even when nothing changed.
   The setters above deliberately stay quiet when state is unchanged, so without this a
   client that starts with no devices and no handshake would never paint a first frame. */
void mesh_ui_store_request_refresh(struct mesh_ui_store *store);

/*
 * Marks the conversation the nav has open as read up to its newest message. Called from
 * consume_updates(), so opening a thread clears its badge and a message arriving while you are
 * sitting in that thread never raises one. The all-traffic view marks nothing: it is a view
 * over conversations, not one of them.
 *
 * Returns true when a mark changed.
 */
bool mesh_ui_store_mark_open_conversation_read(struct mesh_ui_store *store);

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot);

int mesh_ui_store_save(const struct mesh_ui_store *store, const char *path);
int mesh_ui_store_load(struct mesh_ui_store *store, const char *path);

#ifdef __cplusplus
}
#endif
