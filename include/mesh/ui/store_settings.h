#pragma once

/*
 * The Settings tab's whole subject: what the client is, what the radio is, and what the radio
 * is doing right now.
 *
 * Much the largest of these files, and it is one struct - `mesh_ui_settings` is the flattened
 * form of every Config and ModuleConfig section the handshake decoded, plus the client's own
 * About facts and the five running reports (stats, the radio's last word, its send queue, its
 * network interfaces, Store & Forward). It is split off here for the same reason it is large:
 * it changes with every settings phase, and before this file that meant rebuilding every
 * screen in the client. See mesh/ui/store.h for the whole.
 */

#include "mesh/ui/store_channel.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    /*
     * The crash report a *previous* run left behind, if there is one: where it is, and whether
     * to say so.
     *
     * Two fields rather than one, because they answer different questions and the path outlives
     * the flag. `crash_report_path` is where a report would be whether or not one exists - the
     * About row shows it so somebody can go and find the file - while `crash_report_waiting` is
     * what raises the banner and what the discard row clears. Collapsed into "a non-empty path
     * means there is one", the discard would have to blank the path, and the screen would then
     * stop being able to say where the next one will go.
     *
     * Both are read once at startup rather than from a stat() per frame; see
     * mesh_crash_report_waiting() for why a client must not learn mid-run that it has crashed.
     */
    char crash_report_path[MESH_UI_CLIENT_PATH_MAX];
    bool crash_report_waiting;
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

/* How many LoRa regions there are (RegionCode 0..37), which is the width of the table below.
   A literal on this side of the fence, pinned against the protobuf by a test the same way the
   two caps above are. */
#define MESH_UI_REGION_COUNT 38U

/*
 * What one LoRa region will accept, as the firmware's own table says.
 *
 * `presets` is a *set*, one bit per ModemPreset, and 0 is what a region the firmware said
 * nothing about looks like - not "no preset is legal" but "nothing is known, so constrain
 * nothing", which is what the proto asks a client to do with a region missing from the map.
 * Seventeen presets fit in the word with room to spare, and a test says so rather than a
 * reader having to.
 */
struct mesh_ui_region_preset {
    uint32_t presets;
    bool licensed_only; /* an amateur band: legal to select, illegal to transmit on unlicensed */
};

/*
 * FromRadio.region_presets, unpacked.
 *
 * The wire carries it grouped - one entry per distinct preset list, and every region pointing
 * at one by index - because it has to fit in a single packet. That indirection is a fact about
 * the packet and stops here: the rows read a region, so the publish boundary resolves the
 * indices once and hands the UI a table it can index.
 *
 * `loaded` is false on a firmware that predates the message, which is not an error state and
 * must not read as one: every region is then unconstrained and the LoRa rows behave exactly as
 * they did before this table existed.
 */
struct mesh_ui_region_presets {
    bool loaded;
    struct mesh_ui_region_preset region[MESH_UI_REGION_COUNT];
};

/*
 * The connected radio's configuration, flattened from the protobufs the transport decoded so the
 * backends and the settings table never include nanopb. Every `has_*` says whether that section has
 * arrived this connection; `loaded` is any of them. These were read-only first; the same fields are
 * the edit targets now.
 */
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
    /* The radio's own screen, not this one: `oled` overrides a panel its firmware failed to
       autodetect and `displaymode` is the layout it draws. Both restart the radio when they
       change, which is why their rows carry the note the other two reboot rows carry. */
    uint8_t oled;
    uint8_t displaymode;
    bool heading_bold;
    bool wake_on_tap_or_motion;
    bool use_long_node_name;
    bool enable_message_bubbles;

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
    /*
     * The advanced group. The two frequencies are floats on the wire and are kept as the
     * scaled integers the rows are typed in - MHz to four places, Hz to one - because the UI
     * layer draws and parses decimals and has no business rounding one twice.
     */
    bool sx126x_rx_boosted_gain;
    bool override_duty_cycle;
    uint16_t channel_num;
    int64_t override_frequency_scaled; /* MESH_UI_FREQUENCY_DIGITS places, 0 = use the slot */
    int64_t frequency_offset_scaled;   /* MESH_UI_HERTZ_DIGITS places */
    /* LoRaConfig.ignore_incoming, kept full width with 0 for an unused slot: three rows, and
       the write closes the gaps a cleared one leaves the way the admin keys do. */
    uint32_t ignore_incoming[3];
    /*
     * Which presets each region will take. Not part of `has_lora` and deliberately outside it:
     * this is the firmware's table rather than the radio's configuration, it arrives on its own
     * in the handshake, and it stays useful on a radio that has sent no LoRaConfig yet.
     */
    struct mesh_ui_region_presets region_presets;

    bool has_bluetooth;
    bool bluetooth_enabled;
    uint8_t pairing_mode; /* 0 random pin, 1 fixed pin, 2 no pin */
    uint32_t fixed_pin;

    /*
     * NetworkConfig, shown and not offered (MESH_UI_SETTINGS_NETWORK).
     *
     * `wifi_psk` is deliberately absent rather than masked: the row that would show it does not
     * exist, so the credential never reaches this side of the fence at all. Everything here is
     * what the radio was *told* to do - what it is actually doing is `connection`, filled from
     * DeviceConnectionStatus, and the two are shown on different screens because they answer
     * different questions.
     */
    bool has_network;
    bool wifi_enabled;
    char wifi_ssid[33];
    bool eth_enabled;
    bool ipv6_enabled;
    uint8_t address_mode; /* 0 DHCP, 1 static */
    /* The static four, as the wire carries them: fixed32 in network byte order, formatted by
       the same helper the connection rows use. Meaningless under DHCP, and the section says so
       by not drawing them. */
    uint32_t ipv4_ip;
    uint32_t ipv4_gateway;
    uint32_t ipv4_subnet;
    uint32_t ipv4_dns;
    char ntp_server[33];
    char rsyslog_server[33];
    /* Bitwise OR of meshtastic_Config_NetworkConfig_ProtocolFlags. One bit is defined upstream
       (UDP_BROADCAST), so the row is that bit and the rest are carried across a save untouched
       like every other field with no row. */
    uint32_t enabled_protocols;

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
    /* PositionConfig.position_flags: what a position packet carries, as a bitwise OR. Kept
       whole rather than as ten bools because that is what goes back on the wire - the ten rows
       that read it each name their own bit (MESH_UI_SETTING_FLAG). */
    uint32_t position_flags;

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

    /*
     * Mesh beacon: the one module whose record is not a flat list of scalars.
     *
     * Absent is a value on five of its rows, and one encoding is the whole of what that costs.
     * A RegionCode's own UNSET is 0 and already means "whatever the radio is running", so the
     * preset and channel rows beside it - `optional` on the wire rather than zero-as-unset -
     * are stored one past themselves: 0 is absent and n is value n-1. The alternative is a
     * second `has_*` per row, which the row model has no way to draw and the user no way to
     * press.
     */
    bool has_mesh_beacon;
    uint32_t beacon_flags; /* the FLAG rows are bits of this one word */
    uint32_t beacon_interval_secs;
    char beacon_message[MESH_UI_BEACON_MESSAGE_MAX];
    /* The offered channel, which is a whole ChannelSettings on the wire: a name and a key, the
       two halves a node needs to join it. The rest of that submessage is the radio's and is
       carried across a save untouched. */
    char beacon_offer_name[MESH_UI_CHANNEL_NAME_MAX];
    uint8_t beacon_offer_psk[MESH_UI_PSK_MAX];
    uint8_t beacon_offer_psk_len;
    uint32_t beacon_offer_region; /* RegionCode; 0 is the wire's own UNSET */
    uint32_t beacon_offer_preset; /* 0 = running config, else ModemPreset n-1 */
    /* Every slot is listed whether or not the radio sent one, the way a channel slot is: an
       empty target is how a target is added, and emptying one is how it is removed. */
    struct mesh_ui_beacon_target beacon_targets[MESH_UI_BEACON_TARGETS];

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
     * Which ModuleConfigTypes this firmware build left out, as the bitwise OR of
     * meshtastic_ExcludedModules the radio reports.
     *
     * The Modules list showed a module the radio never sent as "not loaded", which conflates
     * two different answers: one that a refresh may still fill in, and one that no refresh ever
     * will. This is the second of them, and it is a fact about the *build* rather than about
     * the connection - so it is read once with the rest of the metadata and asked of through
     * mesh_ui_settings_section_availability() rather than compared against bits at a row.
     */
    uint32_t excluded_modules;
    /* Whether this build can verify XEdDSA packet signatures at all. Read-only upstream and
       read-only here: it is the answer to a Security section whose signature policy appears to
       do nothing, so it goes in the capabilities row beside PKC. */
    bool has_xeddsa;

    /*
     * What is known about *newer* firmware for this radio, flattened out of
     * src/core/firmware.c the same way the client's own update state is flattened into
     * mesh_ui_client_info - as a byte and a line of text, so store.h stays free of a module
     * that forks child processes.
     *
     * Reported rather than offered: this client installs nothing yet, and `fw_blocker_reason` is
     * the line that says why. A refusal is a row, not a silence, because a radio behaving oddly is
     * often a radio on old firmware and "why can I not fix that from here" deserves an answer.
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
    /* Why nothing can be installed from here, already turned into a line - empty when there is
       no reason, which is when the press below is offered. */
    char fw_blocker_reason[64];

    /*
     * Installing it: the other half, flattened out of src/core/firmware_update.c the same way.
     *
     * `fw_can_install` is the press being offered and is deliberately not derivable from the
     * two bytes below - it is the check having found something, the board having a path, the
     * radio being on that bus and nothing else already running, which is four questions the
     * app answers once so the row, the action bar and the confirm sheet cannot answer them
     * three different ways.
     */
    bool fw_can_install;
    /* Which bus the radio is on, as enum mesh_firmware_path spells it - so the row can name the
       install it is offering without asking the device list what kind of link this is. */
    uint8_t fw_bus;
    uint8_t fw_update_state;    /* enum mesh_firmware_update_state (mesh/core/firmware_update.h) */
    uint8_t fw_update_error;    /* enum mesh_firmware_update_error */
    uint8_t fw_update_progress; /* 0-100 over the step that has a fraction; 0 for the rest */
    /* The radio's own words, the loader's, or a sub-module's name for what broke. Untranslated,
       like a log line - see docs/i18n.md. */
    char fw_update_detail[96];
    /*
     * The radio is sitting in its ESP32 update loader: off the mesh, and out of it only by an
     * image finishing. What the banner reads, and the one client state that is true about a
     * computer this client is not currently talking to.
     */
    bool fw_radio_in_loader;
};

#ifdef __cplusplus
}
#endif
