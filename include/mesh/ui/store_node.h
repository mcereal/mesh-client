#pragma once

/*
 * A node as the Nodes tab knows it: its identity, the seven telemetry tables it has reported,
 * who it says it can hear, and the two things this client can go and ask it - a route, and a
 * key verified out of band.
 *
 * Every record here mirrors one in mesh/core/session.h without nanopb, which is the seam the
 * whole store is built on: a backend or a screen test compiles against a snapshot rather than
 * against the client. Nothing here knows about a store, so `mesh/ui/trust.h` and the node
 * renderers can read it without the rest. The roster that carries these is
 * mesh/ui/store_handshake.h; see mesh/ui/store.h for the whole.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    /* When that ratio was measured, which is not always `last_heard`; see the session's twin for
       the two ways they come apart. 0 for a node this run has heard no ratio from - including
       every node restored from the card, whose reading came back without the moment it was
       taken. It is what the signal trend is keyed on. */
    uint32_t snr_time;
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
    /* The routing half of the last packet's header: the last byte of the node that relayed it
       to us, and the last byte of the next hop that packet named. See the session's twin for
       what a byte can and cannot be resolved to, and why neither is cached to the card. */
    bool has_route;
    uint8_t relay_node;
    uint8_t next_hop;
    /*
     * Whether more than one node ends in that byte, answered at publish over the *whole*
     * session roster rather than here.
     *
     * Two bools rather than the resolved names, because the screen resolves a byte live - a
     * relay that was two hex digits at connect time becomes a name the moment its NodeInfo
     * lands - and only the ambiguity needs an authority this side of the seam does not have.
     * This roster is capped at MESH_UI_MAX_HANDSHAKE_NODES and the session's is twice that, so
     * a byte can look unique here purely because its other claimant was ranked away, and a
     * screen scanning only what it was given would name that one node and sound certain.
     */
    bool relay_ambiguous;
    bool next_hop_ambiguous;
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
    /* Whether that key has been proven to be this node's, out of band - the radio's
       `is_key_manually_verified`. The two fields together are what mesh_ui_key_trust_of()
       reads, and between them they are the whole of what the padlock on a direct message is
       entitled to say. */
    bool key_verified;
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
 * The key-verification ceremony's three limits and its stages, restated on this side of the
 * seam exactly as the waypoint limits above are - and for the weaker of the two reasons that
 * file gives: mesh/core/key_verification.h pulls in nothing at all, so including it would cost
 * nothing today. What it would cost is the rule that this header names no core module, which is
 * what lets every backend and every screen test compile against a snapshot rather than against
 * the client. Pinned against the core's in the nodes suite.
 */
#define MESH_UI_VERIFY_NAME_MAX 41U
#define MESH_UI_VERIFY_CHARS_MAX 11U
#define MESH_UI_VERIFY_DIGITS 6U

/* enum mesh_key_verification_stage, value for value. */
enum mesh_ui_verify_stage {
    MESH_UI_VERIFY_IDLE = 0,
    MESH_UI_VERIFY_WAITING,      /* the two radios are talking; nothing to answer yet */
    MESH_UI_VERIFY_SHOW_NUMBER,  /* read these four digits out to the other person */
    MESH_UI_VERIFY_ENTER_NUMBER, /* type the four digits they are reading out */
    MESH_UI_VERIFY_COMPARE,      /* compare the characters and answer */
};

/*
 * The ceremony in progress, as the sheet that draws it needs it.
 *
 * Flattened rather than shared with the core's struct for the reason every record here is:
 * what the UI needs is which question to ask and what to put in it. The clocks the core keeps
 * for its own expiry are not here, because a sheet that counted down would be a second opinion
 * about a deadline the core already owns.
 *
 * `remote_name` is the name the *radio* used, not one this client resolved. That is the point
 * of it: both ends of a verification are looking at their own screen, and a name we substituted
 * from the roster would be this client agreeing with itself while the two users compare.
 *
 * Not persisted. An exchange belongs to one link and one nonce; see mesh/core/session.h.
 */
struct mesh_ui_verification {
    uint8_t stage; /* enum mesh_ui_verify_stage */
    bool we_initiated;
    uint32_t remote_node;
    uint32_t security_number; /* SHOW_NUMBER only; 0 otherwise */
    char remote_name[MESH_UI_VERIFY_NAME_MAX];
    char characters[MESH_UI_VERIFY_CHARS_MAX]; /* COMPARE only */
};

#ifdef __cplusplus
}
#endif
