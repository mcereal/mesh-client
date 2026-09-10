#pragma once

#include "mesh/ui/history.h"
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
/*
 * The map's own roster: every node the *session* holds that has a position, and the width of
 * the name drawn beside one.
 *
 * Two rosters rather than one, because the two screens want different sets. The session keeps
 * MESH_SESSION_MAX_NODES and the ranking publishes the best 128 of them, which is right for a
 * list a reader scrolls and wrong for a map - a node's rank says how likely you are to talk to
 * it, and a marker is on the panel or it is not.
 *
 * Widening `nodes` to the session's own size was the obvious answer and is the one
 * docs/maps-roadmap.md's fourth pre-work item warned against: a summary carries seven
 * telemetry tables and is 532 bytes, so doubling it would have added some 68 KB to a snapshot
 * that is copied whole, to reach two coordinates. This carries the two coordinates.
 *
 * Only positioned nodes are here. An unpositioned one contributes nothing a marker needs, and
 * how many the client knows is a different question that `nodes_known` already answers - so
 * this cap can never truncate: a roster in which every node has a fix still fits. It is pinned
 * against MESH_SESSION_MAX_NODES in the map suite, the way the waypoint limits are pinned
 * against the book's, and for the same reason - this header is nanopb-free by construction and
 * mesh/core/session.h is not.
 */
#define MESH_UI_MAX_MAP_NODES 256U
/*
 * The longest thing drawn beside a marker: a node's short name is four characters and a
 * waypoint's name is its own, cut to something a label can carry without becoming the map.
 *
 * It lives here rather than in mesh/ui/map.h, which is where it was written and which still
 * reads it through this header, because it is now the width of a *published* field. A limit
 * that a producer and a consumer both have to agree about belongs beside the record, not
 * beside the renderer.
 */
#define MESH_UI_MAP_LABEL_MAX 16U
#define MESH_UI_TRANSPORT_STATUS_MAX 32U
/* Newest messages carried to the backends. Matches the transport ring so a per-conversation
   view has the same history the radio gave us; the Brick shows a screenful at a time. */
#define MESH_UI_MAX_MESSAGES 64U
/*
 * The waypoint book's own capacity and upstream's two string limits, restated on this side of
 * the seam - as MESH_UI_MESSAGE_TEXT_MAX already restates the message payload's.
 *
 * The store is nanopb-free by construction, and mesh/core/waypoint.h is not: it takes a
 * meshtastic_MeshPacket. Including it here to reach three numbers would drag the generated
 * protobuf headers into every backend and every test that draws a screen. The numbers are
 * pinned against the core's in the waypoints suite, which is the check that keeps two
 * declarations of one limit honest.
 */
#define MESH_UI_MAX_WAYPOINTS 32U
#define MESH_UI_WAYPOINT_NAME_MAX 31U

/*
 * The radio firmware catalog's two string limits, restated for the same reason - though the
 * reason here is the weaker one, and worth being honest about: mesh/core/firmware_catalog.h
 * pulls in nothing at all, so including it would cost nothing today. What it would cost is the
 * rule. This header names no core module anywhere else, which is what lets every backend and
 * every screen test compile against a snapshot rather than against the client; a first
 * exception is how a seam stops being one. Pinned against the core's in the firmware suite.
 */
#define MESH_UI_FW_VERSION_MAX 24U
#define MESH_UI_FW_BOARD_MAX 48U
#define MESH_UI_WAYPOINT_DESCRIPTION_MAX 101U
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
    MESH_UI_UPDATE_WAYPOINTS = 1U << 7,
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
    int8_t rssi; /* BLE only; 0 for a USB port, and meaningless unless in_range */
    /* Whether the radio answered the last scan. BlueZ lists every node it holds a bond for,
       so a row can name a radio sitting at home all day; it has no RSSI to show and saying
       "0dBm" about it reads as the strongest signal on the screen. Always true for a USB
       port, which is present or is not a row. */
    bool in_range;
    bool connected;
    /* BLE only: BlueZ holds a bond for this node. A node in PIN mode that is not paired
       connects and then fails, so the row says so before the user presses A. Always true for
       a USB port, which has nothing to pair. */
    bool paired;
    /* The link this row is being brought up on right now (connecting, or bonding). */
    bool busy;
    /* USB only: this device is a node sitting in its UF2 bootloader rather than running
       firmware. It cannot carry a session, and the row says so instead of disappearing - the
       Devices tab is where "why will this not connect" is answered, and a board in its
       bootloader is the most answerable version of that question there is. */
    bool bootloader;
    uint8_t kind; /* enum mesh_ui_device_kind */
};

/* The node detail the Nodes tab drills into, mirroring the session's structs without nanopb.
   Latitude and longitude stay in Meshtastic's fixed-point 1e-7 degrees. */
/* Mirrors struct mesh_node_position; see session.h for why the two clocks are both kept.
   `time` is the node's account of when the fix was taken and is often 0; `received` is ours
   and is what the detail falls back to, labelled as the different question it answers. Either
   may be 0 - a fix replayed out of the radio's NodeDB has no arrival we witnessed - and a row
   with neither says it does not know rather than picking one. */
struct mesh_ui_node_position {
    bool valid;
    int32_t latitude_i;
    int32_t longitude_i;
    bool has_altitude;
    int32_t altitude;
    uint32_t time;
    uint32_t received;
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

/*
 * The four Telemetry groups beyond device metrics and environment, mirroring the session's
 * declarations (mesh/core/session.h) without nanopb. Curated rather than complete, for the
 * reason given there; each field carries the sender's own has_*, because on a sensor node a
 * missing reading and a reading of zero are different statements.
 */
struct mesh_ui_node_power_channel {
    bool has_voltage;
    float voltage; /* volts */
    bool has_current;
    float current; /* mA */
};

struct mesh_ui_node_power {
    bool valid;
    uint32_t time;
    struct mesh_ui_node_power_channel channel[3];
};

struct mesh_ui_node_air_quality {
    bool valid;
    uint32_t time;
    bool has_pm10;
    uint16_t pm10_standard; /* ug/m3 */
    bool has_pm25;
    uint16_t pm25_standard;
    bool has_pm100;
    uint16_t pm100_standard;
    bool has_co2;
    uint16_t co2; /* ppm */
    bool has_voc_index;
    float voc_index; /* Sensirion's unitless 1..500, 100 is normal */
    bool has_nox_index;
    float nox_index;
};

struct mesh_ui_node_health {
    bool valid;
    uint32_t time;
    bool has_heart_bpm;
    uint8_t heart_bpm;
    bool has_spo2;
    uint8_t spo2; /* percent */
    bool has_temperature;
    float temperature; /* Celsius, body rather than air */
};

struct mesh_ui_node_host {
    bool valid;
    uint32_t time;
    bool has_uptime;
    uint32_t uptime_seconds;
    bool has_freemem;
    uint32_t freemem_kib;
    bool has_diskfree;
    uint32_t diskfree_mib;
    bool has_load;
    uint32_t load1; /* the real load average times 100, as the firmware sends it */
    uint32_t load5;
    uint32_t load15;
};

/*
 * Who a node reported it can hear (NEIGHBORINFO_APP), mirroring the session's declaration. Ten
 * is upstream's own cap on the list, not a screen budget. The names are not resolved here: the
 * node detail resolves them against the roster it is already handed, and the same lists read
 * across that roster are what answer "who hears *this* node" - the reverse edge, which nothing
 * on the wire reports directly.
 */
#define MESH_UI_MAX_NEIGHBORS 10U

struct mesh_ui_node_neighbor {
    uint32_t node_id;
    float snr;
};

struct mesh_ui_node_neighbors {
    bool valid;
    uint32_t time;
    uint32_t broadcast_interval_secs; /* 0 when the node did not say */
    uint8_t count;
    struct mesh_ui_node_neighbor entries[MESH_UI_MAX_NEIGHBORS];
};

struct mesh_ui_node_summary {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
    /* How loud the last directly-heard packet was, as opposed to how far above the noise; see
       the session's declaration. `rssi_time` is when it was measured, which is not always
       `last_heard` - a node relayed over MQTT keeps its last RF reading, and the stamp is what
       stops the row claiming to describe a packet it does not. */
    bool has_rssi;
    int16_t rx_rssi;    /* dBm */
    uint32_t rssi_time; /* epoch of the reading; equals last_heard when it is the newest */
    bool via_mqtt;
    bool has_hops_away;
    uint8_t hops_away;
    char user_id[16];
    /* False while the name is the one derived from the node number rather than one the node
       gave; see mesh_session_default_identity(). */
    bool has_user;
    /* False for a node we remember that the radio's NodeDB no longer carries. */
    bool in_nodedb;
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
    struct mesh_ui_node_power power;
    struct mesh_ui_node_air_quality air_quality;
    struct mesh_ui_node_health health;
    struct mesh_ui_node_host host;
    struct mesh_ui_node_neighbors neighbors;
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
    /*
     * The look the UI is drawn with: the theme's id for whoever draws, its name for whoever
     * reads. It rides in the snapshot rather than being pushed at a backend, because backends
     * are stateless by design - they draw what the snapshot says, and a theme is no different
     * from the cursor in that respect. That is the whole of what makes the switch live.
     */
    char theme[MESH_UI_CLIENT_TEXT_MAX];
    char theme_name[MESH_UI_CLIENT_TEXT_MAX];
    /* The language the catalog resolved to, in that language ("English", "Deutsch"). It rides
       here for the reason the theme name does: About reads it, and a backend is not the place
       to ask src/i18n anything. Chosen at startup from MESHCLIENT_LANG and the POSIX locale
       variables; see docs/i18n.md. */
    char language_name[MESH_UI_CLIENT_TEXT_MAX];
    bool language_from_env;
    /* MESHCLIENT_THEME is holding it. The row then says so instead of offering a press that
       the environment would override on the next frame. */
    bool theme_from_env;
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
    /*
     * How far the step in flight has got, in permille, and whether it is a real fraction.
     *
     * Two fields rather than a sentinel because they answer two different questions and a
     * renderer needs both: "is anything happening" is `update_busy`, "how much of it is left"
     * is this pair. Unknown is the ordinary case, not a failure - a check has no length and a
     * hash is taken in one go - and it is what a bar draws as motion without a position.
     *
     * Only ever the download sets `known`, and only once GitHub told us the asset's size. See
     * mesh_updater_progress().
     */
    uint16_t update_progress;
    bool update_progress_known;
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

/* A router's name is drawn in a settings row's value column, which is short. */
#define MESH_UI_STORE_FORWARD_NAME_MAX 24U

/*
 * Store & Forward as the Settings section reads it: who the router is and what the last request
 * for the missed traffic did.
 *
 * `received` and `stored` are both here and they are not the same number. A router replays its
 * whole window, most of which a client that was only briefly off already heard live - so a row
 * saying "30 messages" about a replay that added none of them would be describing the router's
 * work rather than the user's inbox. `expected` is what the router said was coming, which is a
 * third number again while the replay is still arriving.
 *
 * Not persisted, for the traceroute's reason: it describes one exchange with one router over
 * one connection, and the messages it fetched are in the transcript, which is.
 */
struct mesh_ui_store_forward {
    uint8_t state; /* enum mesh_store_forward_state, carried as a byte */
    uint32_t router;
    char router_name[MESH_UI_STORE_FORWARD_NAME_MAX];
    bool router_secondary; /* the router said it is not this mesh's primary one */
    uint32_t expected;
    uint32_t received;
    uint32_t stored;
    /* Counts requests rather than events, so a backend can tell a new one from the same one
       further along. */
    uint32_t seq;
    /* The router's own storage, when it has volunteered it (ROUTER_STATS). */
    bool has_stats;
    uint32_t messages_saved;
    uint32_t messages_max;
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

/*
 * The newest thing the radio said to the user in its own words (FromRadio.clientNotification),
 * flattened for the backends. `seq` is the session's running count, so a backend can tell one
 * notification from a repeat of the same text and the app can announce each exactly once; 0
 * means none has arrived on this connection.
 */
#define MESH_UI_RADIO_NOTICE_TEXT_MAX 128U

struct mesh_ui_radio_notice {
    uint32_t seq;
    uint32_t time;     /* the radio's clock, epoch seconds; 0 when it has none */
    uint32_t received; /* our clock when it landed, epoch seconds; 0 when we have none */
    uint8_t level;     /* meshtastic_LogRecord_Level, carried as a byte */
    char text[MESH_UI_RADIO_NOTICE_TEXT_MAX];
};

/*
 * The radio's outgoing packet queue. `res` non-zero is the radio having refused a packet
 * outright - it never went on the air, so no Routing reply will ever explain it - and `free`
 * against `maxlen` is how close the link is to that happening again.
 */
struct mesh_ui_queue_status {
    bool valid;
    int8_t res;
    uint8_t free;
    uint8_t maxlen;
};

/*
 * What the radio's own network interfaces are doing (AdminMessage.get_device_connection_status).
 *
 * Flat rather than four nested records because that is how it is read: a screen asks "is the
 * WiFi up and what is it on", not "give me the WiFi object". Each `has_*` is the radio having
 * reported that interface at all, which is a different fact from it being connected - a board
 * with no WiFi reports no WiFi, and a board whose WiFi is off reports it disconnected.
 */
struct mesh_ui_connection_status {
    bool valid; /* the radio answered the request at all */
    bool has_wifi;
    bool wifi_connected;
    char wifi_ssid[33];
    int32_t wifi_rssi; /* dBm */
    uint32_t wifi_ip;  /* as the wire carries it: a fixed32 in network byte order */
    bool wifi_mqtt;
    bool wifi_syslog;
    bool has_ethernet;
    bool ethernet_connected;
    uint32_t ethernet_ip;
    bool ethernet_mqtt;
    bool ethernet_syslog;
    bool has_bluetooth;
    bool bluetooth_connected;
    uint32_t bluetooth_pin;
    int32_t bluetooth_rssi;
    bool has_serial;
    bool serial_connected;
    uint32_t serial_baud;
};

/* The wire caps for the two strings the radio keeps outside any Config section
   (meshtastic/admin.options). Literals because this header is the nanopb-free side of the
   fence; a test pins them against the protobuf so a bump upstream cannot pass unnoticed. */
#define MESH_UI_CANNED_MESSAGES_MAX 201U
#define MESH_UI_RINGTONE_MAX 231U

struct mesh_ui_settings {
    /* The client's own facts. Always populated, radio or no radio - the About section is the
       one part of this tab that does not need a connection. */
    struct mesh_ui_client_info client;
    /* Mesh health, from LocalStats telemetry rather than from the config handshake, so it
       fills in on its own schedule and is absent until the radio's first report. */
    struct mesh_ui_radio_stats stats;
    /* What the radio has said and how full its send queue is. Like `stats`, these arrive on
       the radio's own schedule rather than through the handshake, and neither is persisted. */
    struct mesh_ui_radio_notice notice;
    struct mesh_ui_queue_status queue;
    /* Times the radio has told us it restarted on this connection. */
    uint32_t reboot_notices;
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
    /* MQTTConfig.map_report_settings. Only read by the radio with map reporting on. */
    uint32_t mqtt_map_publish_interval_secs;
    uint32_t mqtt_map_position_precision;
    bool mqtt_map_should_report_location;

    /* Talking to a router, as opposed to the rows below that configure the module. Lives here
       because the section's rows are built from this struct, the way `connection` and the
       radio's own position do - both of which are likewise live state rather than config. */
    struct mesh_ui_store_forward store_forward;

    bool has_store_forward;
    bool store_forward_enabled;
    bool store_forward_heartbeat;
    bool store_forward_is_server;
    uint32_t store_forward_records;
    uint32_t store_forward_history_return_max;
    uint32_t store_forward_history_return_window;

    bool has_telemetry;
    uint32_t device_update_interval;
    bool device_telemetry_enabled;
    bool environment_measurement_enabled;
    uint32_t environment_update_interval;
    bool environment_screen_enabled;
    bool environment_display_fahrenheit;
    bool air_quality_enabled;
    uint32_t air_quality_interval;
    bool air_quality_screen_enabled;
    bool power_measurement_enabled;
    uint32_t power_update_interval;
    bool power_screen_enabled;
    bool health_measurement_enabled;
    uint32_t health_update_interval;
    bool health_screen_enabled;

    bool has_neighbor_info;
    bool neighbor_info_enabled;
    uint32_t neighbor_info_interval;
    bool neighbor_info_over_lora;

    bool has_range_test;
    bool range_test_enabled;
    uint32_t range_test_sender; /* seconds between test packets; 0 = receive only */
    bool range_test_save;
    bool range_test_clear_on_reboot;

    bool has_paxcounter;
    bool paxcounter_enabled;
    uint32_t paxcounter_interval;
    /* RSSI floors, so negative. Kept signed here and cast at the row, the way LoRa tx power is. */
    int32_t paxcounter_wifi_threshold;
    int32_t paxcounter_ble_threshold;

    bool has_tak;
    uint8_t tak_team; /* meshtastic_Team, 0..14 */
    uint8_t tak_role; /* meshtastic_MemberRole, 0..8 */

    bool has_ambient_lighting;
    bool ambient_led_state;
    uint8_t ambient_current;
    uint8_t ambient_red;
    uint8_t ambient_green;
    uint8_t ambient_blue;

    bool has_status_message;
    char status_message[80];

    bool has_detection_sensor;
    bool detection_enabled;
    uint32_t detection_minimum_broadcast_secs;
    uint32_t detection_state_broadcast_secs;
    bool detection_send_bell;
    char detection_name[20];
    uint8_t detection_monitor_pin;
    uint8_t detection_trigger_type; /* 0..5, TriggerType */
    bool detection_use_pullup;

    bool has_external_notification;
    bool extnotif_enabled;
    uint32_t extnotif_output_ms;
    uint32_t extnotif_nag_timeout;
    bool extnotif_active;
    bool extnotif_use_pwm;
    bool extnotif_use_i2s_as_buzzer;
    /* Three outputs, each with its own pin and its own pair of alert flags. */
    uint8_t extnotif_output;
    uint8_t extnotif_output_vibra;
    uint8_t extnotif_output_buzzer;
    bool extnotif_alert_message;
    bool extnotif_alert_message_vibra;
    bool extnotif_alert_message_buzzer;
    bool extnotif_alert_bell;
    bool extnotif_alert_bell_vibra;
    bool extnotif_alert_bell_buzzer;

    /* No enabled flag anywhere in this one: upstream removed the bool toggles in favour of
       "a non-zero value implicitly enables it", so 0 is off for every row. */
    bool has_traffic_management;
    uint32_t traffic_position_min_interval_secs;
    uint32_t traffic_nodeinfo_max_hops;
    uint32_t traffic_rate_limit_window_secs;
    uint32_t traffic_rate_limit_max_packets;
    uint32_t traffic_unknown_packet_threshold;

    bool has_channels; /* any slot present */
    struct mesh_ui_channel_detail channels[MESH_UI_MAX_CHANNELS];

    /* The radio's own screen (DeviceUIConfig), which is a different thing from Display: that
       is the OLED's geometry and units, this is the graphical UI's own preferences. The two
       locks are shown and not offered - a client that can lock a radio's screen and settings
       has no verb to unlock them again, and the PIN behind them is not on the wire. */
    bool has_ui_config;
    uint8_t ui_theme;     /* meshtastic_Theme: 0 dark, 1 light, 2 red */
    uint32_t ui_language; /* meshtastic_Language; 0..19 then 30, 31 - see the read-only row */
    uint8_t ui_brightness;
    uint32_t ui_screen_timeout; /* seconds */
    bool ui_alert_enabled;
    bool ui_banner_enabled;
    uint8_t ui_ring_tone_id;
    uint8_t ui_compass_mode;
    uint8_t ui_gps_format;
    bool ui_clockface_analog;
    bool ui_screen_lock;
    bool ui_settings_lock;

    /* The radio's quick-reply list as the wire carries it: one string, entries separated by
       '|'. Split for display by the Canned messages section, rejoined on save. */
    bool has_canned_messages;
    char canned_messages[MESH_UI_CANNED_MESSAGES_MAX];
    /* The RTTTL the buzzer plays. Read-only: see the row for why. */
    bool has_ringtone;
    char ringtone[MESH_UI_RINGTONE_MAX];

    struct mesh_ui_connection_status connection;

    bool has_metadata;
    char firmware_version[18];
    uint32_t hw_model;
    bool has_wifi;
    bool has_bluetooth_radio;
    bool has_ethernet;
    bool has_pkc;
    bool can_shutdown;

    /*
     * What is known about *newer* firmware for this radio, flattened out of
     * src/core/firmware.c the same way the client's own update state is flattened into
     * mesh_ui_client_info - as a byte and a line of text, so store.h stays free of a module
     * that forks child processes.
     *
     * Reported rather than offered: this client installs nothing yet
     * (docs/radio-firmware-roadmap.md), and `fw_blocker_reason` is the line that says why. A
     * refusal is a row, not a silence, because a radio behaving oddly is often a radio on old
     * firmware and "why can I not fix that from here" deserves an answer.
     */
    bool fw_supported; /* a fetcher exists, so the check can do anything at all */
    bool fw_busy;      /* a document is in flight */
    /* Which of upstream's two release lists is being read - its own word for it, so it is not
       a string id: "stable" and "alpha" are names, like a region code. */
    char fw_channel[12];
    uint8_t fw_state; /* enum mesh_firmware_state (mesh/core/firmware.h) */
    char fw_message[96];
    /* The newest release on the followed channel, and the board this radio was identified as.
       Either may be empty: a check that has not run, or a board nothing claimed. */
    char fw_latest[MESH_UI_FW_VERSION_MAX];
    char fw_board[MESH_UI_FW_BOARD_MAX];
    /* Why nothing can be installed from here, already turned into a line - empty when the only
       reason is that the phase that would do it has not been written. */
    char fw_blocker_reason[64];
};

struct mesh_ui_my_info {
    uint32_t node_num;
    uint32_t nodedb_entries;
    uint32_t reboot_count;
};

/*
 * A node as the map needs it: where it is, what to write beside it, and how much to believe it.
 *
 * The label is resolved by whoever fills this rather than by the marker that draws it, because
 * a node carries two names totalling 45 bytes and a map wants four characters of one of them.
 * The rule is the short name, falling back to the long one: a map is mostly empty space with
 * initials in it, and it is the same abbreviation the node list already shows in its disc, so a
 * reader who learned a node by its initials on one screen recognises it on the other. A node
 * that never introduced itself has both derived from its number, so there is always something.
 */
struct mesh_ui_map_node {
    uint32_t node_id;
    int32_t latitude_i;
    int32_t longitude_i;
    /* When the fix was heard, by our clock; 0 when we witnessed no arrival - a fix replayed out
       of the radio's NodeDB has none. Never the node's own dating: see mesh_ui_node_position. */
    uint32_t received;
    /* How much the sender says it rounded the position off. 0 means it never set the field,
       which is "did not say" rather than "exact", and is the ordinary case. */
    uint8_t precision_bits;
    /* False for a node we remember that the radio's NodeDB no longer carries. */
    bool in_nodedb;
    /*
     * Whether the ranked rows above published this node, which is what decides whether it can
     * be opened.
     *
     * The map draws every positioned node the session holds and the list carries the best 128,
     * so this is the first thing in the client that can be *shown* and not opened - a node
     * ranked 200th by the list's rules has a marker here and no row anywhere, and a node detail
     * resolves by id through `nodes`. Publish knows the answer for free: both arrays are cut
     * from the same ranking, so a map entry has a row exactly when its place in that ranking is
     * inside the cut.
     *
     * docs/maps-roadmap.md's fifth pre-work item named this case - "a map-only node may be
     * outside the detail roster: resolve its detail by ID through the app/store seam" - and
     * closed as "open by construction: there are no map-only nodes until there is a map". There
     * are now, and until that seam exists this is what keeps A on such a marker a clean no-op
     * rather than a detail that opens and is clamped shut on the same frame.
     */
    bool has_row;
    char label[MESH_UI_MAP_LABEL_MAX];
};

struct mesh_ui_handshake_state {
    bool request_in_flight;
    uint32_t request_id;
    bool config_complete;
    uint32_t config_complete_id;
    bool has_my_info;
    struct mesh_ui_my_info my_info;
    bool has_config;
    /*
     * Whether the session has a send path right now - not whether we know anything about the
     * radio. The two used to be the same question, because has_my_info was cleared on every
     * drop, and `connected` in the Settings > Actions rows was spelled has_my_info for that
     * reason. Once what the radio *is* began surviving a reconnect, that spelling would have
     * left reboot, shutdown, NodeDB reset, backup/restore and factory reset pressable over a
     * dead link, failing with -ENOTCONN after the confirm dialog. It was already wrong before
     * that: the handshake is persisted, so a cold start with a restored roster had has_my_info
     * true with nothing connected.
     *
     * This is mesh_session_attached() - exactly the condition an AdminMessage can go out under,
     * and exactly the one that returns -ENOTCONN when it cannot.
     */
    bool link_up;
    /*
     * How many nodes the replay now running has delivered, against my_info.nodedb_entries. The
     * Status screen said "in progress" and nothing else for as long as a sync took, which on a
     * 135-node radio is seventeen seconds and on a flapping link was forever; this is what lets
     * the row show that the seventeen seconds are going somewhere.
     */
    uint32_t sync_nodes;
    /* How many of the roster's nodes are published below - at most MESH_UI_MAX_HANDSHAKE_NODES. */
    uint32_t node_count;
    /*
     * How many the session roster actually holds, which is up to MESH_SESSION_MAX_NODES and so
     * up to twice `node_count`. The two differ silently otherwise: ranking decides which 128
     * survive the cut and the rest simply are not there, with no row saying so. The radio's own
     * `my_info.nodedb_entries` cannot stand in for this - it is the radio's count, and the
     * roster deliberately outlives the radio's database, so after a NodeDB reset it is the
     * smaller of the two.
     */
    uint32_t nodes_known;
    /*
     * What each of the two Settings forget rows would actually drop, counted over the *whole*
     * session roster rather than the 128 published below - the roster holds twice that, and a
     * row that offers to empty it has to say how many it empties.
     *
     * They are what the action removes, not what is off the radio: our own record and every
     * pinned node survive a forget, so a roster whose off-radio nodes are all pinned reports
     * zero here and the row draws as a fact. The Nodes tab's own "off radio" total is a
     * different question - what is on screen and stale - and is counted from the rows below by
     * mesh_ui_handshake_off_radio(), so it can never exceed the count beside it.
     */
    uint32_t nodes_forgettable_off_radio;
    uint32_t nodes_forgettable_all;
    char primary_channel[33];
    char my_short_name[6];
    struct mesh_ui_node_summary nodes[MESH_UI_MAX_HANDSHAKE_NODES];
    /*
     * The map's roster: every node in the *session* roster with a position, ranked as `nodes`
     * is and not cut to 128. See MESH_UI_MAX_MAP_NODES for why this is a second array rather
     * than a wider first one.
     *
     * It is not a subset of `nodes` and must not be read as one: a node ranked 200th by the
     * list's rules is nowhere above and is a marker here. 0 on a handshake nobody published -
     * a roster loaded from the cache, a hand-built fixture - which mesh_ui_map_build() reads as
     * "ask the published rows instead", because that is the best any such handshake has.
     */
    uint32_t map_node_count;
    struct mesh_ui_map_node map_nodes[MESH_UI_MAX_MAP_NODES];
    /* Channel table by slot; disabled slots are present with role 0. */
    uint32_t channel_count;
    struct mesh_ui_channel channels[MESH_UI_MAX_CHANNELS];
    /* The radio the roster belongs to, carried so a restart can hand it back to the session.
       Not my_info.node_num: that one goes with the connection and is 0 while disconnected,
       which is exactly when the cache tends to be written. */
    uint32_t roster_owner;
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
    /* enum mesh_message_kind: an ordinary text message, the firmware's critical alert, or a
       detection sensor announcing itself. All three arrive as text on a channel; only the
       transcript's labelling tells them apart. */
    uint8_t kind;
    uint8_t ack; /* enum mesh_message_ack */
    /* meshtastic_Routing_Error behind an ack of FAILED, so the row can say why rather than
       just marking it failed. Meaningless for anything else. */
    uint8_t ack_error;
    bool broadcast;
    /* Decrypted with our public key rather than a channel PSK: addressed to us and readable by
       nobody else. On a default-key channel a "direct" message is not that, and the transcript
       has no other way to say so. */
    bool pki_encrypted;
    /* The message this one answers, and whether it is a reaction rather than a reply. A
       reaction is an annotation on its target, not a line of its own, so it is filtered out of
       the thread (mesh_ui_nav_filter_messages) and drawn on the bubble it belongs to. */
    uint32_t reply_id;
    bool is_reaction;
};

/*
 * One shared place, resolved for display: `from_name` is the sender's short name when we know
 * it, so a backend never joins against the node list, exactly as a message's peer_name is.
 *
 * A copy of struct mesh_waypoint rather than the thing itself, because the store is nanopb-free
 * by construction and this side of the seam is what backends and the cache read. The fields the
 * core does not carry are the two derived ones below.
 */
struct mesh_ui_waypoint {
    uint32_t id;
    int32_t latitude_i;
    int32_t longitude_i;
    bool has_coords;
    uint32_t expire;
    uint32_t locked_to;
    uint32_t icon;
    uint32_t from;
    uint32_t heard;
    uint8_t channel;
    bool ours;
    /*
     * Whether this client may change it - `locked_to` is 0, or it is us.
     *
     * Derived at publish rather than at the press, because the answer needs our own node number
     * and the nav has no business knowing one. It decides whether deleting broadcasts a
     * withdrawal to the mesh or merely drops our copy, and the detail says which it would do
     * before the press rather than after it.
     */
    bool editable;
    char name[MESH_UI_WAYPOINT_NAME_MAX];
    char description[MESH_UI_WAYPOINT_DESCRIPTION_MAX];
    char from_name[16];
};

struct mesh_ui_waypoint_list {
    struct mesh_ui_waypoint entries[MESH_UI_MAX_WAYPOINTS];
    uint32_t count;
    uint32_t dropped; /* places the book evicted to make room for newer ones */
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
 * What the client remembers about one conversation, keyed the way the UI names a destination.
 *
 * Two things, and they share a slot because they share a key and a lifetime: how far the user
 * has read, and whether they want to hear about it at all. A mute in a table of its own would
 * be a second array keyed on (kind, channel, node) and a second eviction rule to keep in step
 * with this one - the read mark's key *is* the conversation's identity, so there is nothing a
 * separate table would express that a field here does not.
 *
 * `packet_id` is "everything up to and including this packet has been seen". A packet id
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
    /*
     * The user has asked not to be interrupted by this conversation: no badge on the tab, no
     * snackbar when something arrives. The messages still arrive and the thread still fills.
     *
     * Deliberately not the only input to that question - see mesh_ui_store_conversation_muted(),
     * which also honours the radio's own per-node mute. This is the half the client owns.
     */
    bool muted;
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
    /* The places the mesh has shared. Not persisted: see mesh_ui_store_set_waypoints(). */
    struct mesh_ui_waypoint_list waypoints;
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
    /*
     * What the client has watched happen, as opposed to everything above, which is what is true
     * now. The one part of a snapshot that is not a copy of what the radio last said - see
     * include/mesh/ui/history.h. Not persisted, for the reason stated there.
     */
    struct mesh_ui_history history;
    mesh_ui_update_flags update_flags;
};

struct mesh_ui_store {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    struct mesh_ui_message_list messages;
    struct mesh_ui_waypoint_list waypoints;
    struct mesh_ui_read_state read_state;
    char transport_status[MESH_UI_TRANSPORT_STATUS_MAX];
    struct mesh_ui_nav nav;
    struct mesh_ui_settings settings;
    struct mesh_ui_traceroute traceroute;
    struct mesh_ui_history history;
    /*
     * The clock the last mesh_ui_store_tick() carried, which is what stamps a history sample.
     *
     * The setters do not take a time - they are called from wherever a publish happens to reach
     * the store - and a series needs one, so the store keeps the last it was told. The event
     * loop ticks every turn, so it is never more than a turn stale; before the first tick it is
     * 0, which is a real point on a monotonic clock rather than a missing one.
     */
    uint64_t now_ms;
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
/*
 * Replaces the shared-places view; quiet when nothing changed.
 *
 * Deliberately not persisted with the roster, and the reason is the one the history file
 * already gives for trends: the roster is what we *know*, and it is worth keeping because a
 * node the radio evicted is gone for good otherwise. A waypoint is not like that - it lives on
 * the mesh, every client that hears it holds one, and the sharer can withdraw it. A cache would
 * put back places the mesh had already agreed were gone, since a withdrawal that arrived while
 * this client was off is a packet nobody replays.
 */
void mesh_ui_store_set_waypoints(struct mesh_ui_store *store,
                                 const struct mesh_ui_waypoint_list *waypoints);
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
/*
 * How many of the nodes in `handshake` the radio's NodeDB no longer carries - the rows the
 * Nodes tab dims and marks "off radio". Counted from the published rows themselves rather than
 * carried alongside them, so it is always a number in the same scope as `node_count`: the UI
 * holds 128 nodes and the session roster holds 256, and "128 nodes, 200 off radio" is not a
 * thing any screen should be able to draw.
 */
uint32_t mesh_ui_handshake_off_radio(const struct mesh_ui_handshake_state *handshake);

void mesh_ui_message_list_merge(const struct mesh_ui_message_list *cached,
                                const struct mesh_ui_message_list *live,
                                struct mesh_ui_message_list *out);

/*
 * Drops every message in one conversation from a list. Returns how many went.
 *
 * `kind` is an enum mesh_ui_conversation_kind: CHANNEL reads `channel`, DIRECT reads `node`,
 * and neither ALL nor NEW names a conversation, so both remove nothing - "delete everything"
 * is not a thing one press on a list row should be able to mean.
 */
uint32_t mesh_ui_message_list_forget(struct mesh_ui_message_list *list, uint8_t kind, uint32_t node,
                                     uint8_t channel);

/*
 * The same on the store, plus the conversation's read mark, signalling a repaint when
 * anything went.
 *
 * This is only the UI's copy. The messages also sit in the transport's ring and in the history
 * the app read back from its cache, and a delete that misses either of those puts the
 * conversation straight back on the next publish - which is why the app owns
 * MESH_UI_ACTION_DELETE_CONVERSATION rather than the store doing it on a key press.
 */
uint32_t mesh_ui_store_forget_conversation(struct mesh_ui_store *store, uint8_t kind, uint32_t node,
                                           uint8_t channel);

/* Navigation. A key press moves the cursor or switches tabs and, for A on an actionable row,
   fills *out_action for the caller to carry out (connect, send). Returns true when the frame
   needs repainting; the store has already signalled its eventfd in that case. */
bool mesh_ui_store_handle_key(struct mesh_ui_store *store, enum mesh_ui_key key,
                              struct mesh_ui_action *out_action);
/* Show a transient one-line notice on the backends ("Sent to ABCD"). */
void mesh_ui_store_set_toast(struct mesh_ui_store *store, uint64_t now_ms, const char *text);
/* A notice about something that arrived, which queues rather than replacing - see
   mesh_ui_nav_post_toast(). */
void mesh_ui_store_post_toast(struct mesh_ui_store *store, uint64_t now_ms, const char *text);
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

/*
 * Whether this conversation may interrupt the user: no tab badge, no snackbar when something
 * arrives. `kind` is CHANNEL or DIRECT, named the way mesh_ui_nav_conversation_at() names one.
 *
 * Two inputs, deliberately, because there are two places a mute can already have been asked
 * for and a client that read only its own would contradict the radio in front of the user.
 * The local flag is the one this client owns and the only one START toggles. The other is
 * upstream's `NodeInfo.is_muted`, whose whole definition is that the node "will not trigger a
 * notification" - so a radio told to stop announcing a node, and a Brick that then announced
 * it anyway, would be two answers to one question. It applies to a direct conversation only:
 * the flag is per node and the NodeDB has nothing to say about a channel.
 *
 * One predicate rather than a field on the conversation, so the badge, the snackbar and the
 * row's own icon cannot disagree about who is muted.
 */
bool mesh_ui_store_conversation_muted(const struct mesh_ui_store *store, uint8_t kind,
                                      uint32_t node, uint8_t channel);

/* How far this conversation has been read: the packet id of the newest message the user has
   seen in it, or 0 when the client holds no mark. What the transcript rules its "new from here"
   line under - read once, when the thread opens, because the mark itself moves a moment later. */
uint32_t mesh_ui_store_conversation_read_mark(const struct mesh_ui_store *store, uint8_t kind,
                                              uint32_t node, uint8_t channel);

/* Whether *this client* is muting it, ignoring what the radio thinks. What START toggles, and
   what the press has to read to know which way it is about to go. */
bool mesh_ui_store_conversation_muted_locally(const struct mesh_ui_store *store, uint8_t kind,
                                              uint32_t node, uint8_t channel);

/* Sets the local mute. Returns true when it changed, so the caller can skip a repaint and a
   save it does not need. */
bool mesh_ui_store_set_conversation_mute(struct mesh_ui_store *store, uint8_t kind, uint32_t node,
                                         uint8_t channel, bool muted);

/*
 * A read-only store standing in for a snapshot, for the answers that are written against one.
 *
 * The conversation list, the node rows, the waypoint list and the map roster are all derived by
 * functions that take a `struct mesh_ui_store`, because that is where the data lives - and a
 * backend and the action bar are both handed a `const struct mesh_ui_snapshot` instead. This is
 * the adaptor, and it lives here rather than in either caller because it now has callers on
 * both sides of that seam: the fb renderer, which had a private copy of it, and
 * src/ui/actions.c, which needs the row under the cursor to name a press.
 *
 * `nav` is deliberately left zeroed. Nothing that takes a store reads it, and the screens that
 * need one are handed it separately - a view that carried it would be a second copy of the
 * cursor, free to disagree with the one the frame is drawn from.
 */
void mesh_ui_store_view(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_store *view);

/*
 * The radio this snapshot is attached to, or NULL.
 *
 * Four places ask it - the action bar, the bottom bar's status line, the Status tab's Link card
 * and the Devices screen - and each used to write the loop out again. They have to agree: one
 * of them saying "connected" while another says "not connected" is the worst possible answer to
 * the question, and two copies of a loop is how that happens.
 */
const struct mesh_ui_device *
mesh_ui_snapshot_connected_device(const struct mesh_ui_snapshot *snapshot);

/*
 * What A and Y would do on a Devices row, asked by the press and by the action bar.
 *
 * Same reasoning as the function above, one level down: a bar that names a keycap the press
 * then declines is the keycap-that-does-nothing `src/ui/actions.c` refuses everywhere else, and
 * the only way two files stay agreed about it is to give them one function to ask. Both of these
 * were conditions written into the nav's handlers with an unconditional entry in the bar beside
 * them, which is exactly how that disagreement arises.
 *
 * NULL is false for both, so a cursor past the end needs no separate test at either call site.
 */
/* A opens a link: a row with an address, not already the one we are on, and not a node sitting
   in its bootloader - which has no session to offer however good the cable is. */
bool mesh_ui_device_connectable(const struct mesh_ui_device *device);
/* Y forgets a bond, so only a BLE row has one to forget; a USB port has nothing to bond. */
bool mesh_ui_device_forgettable(const struct mesh_ui_device *device);

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot);

int mesh_ui_store_save(const struct mesh_ui_store *store, const char *path);
int mesh_ui_store_load(struct mesh_ui_store *store, const char *path);

#ifdef __cplusplus
}
#endif
