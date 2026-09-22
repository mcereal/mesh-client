#pragma once

#include "inkcell/ui/fb.h"
#include "inkcell/ui/headless.h"
#include "inkcell/ui/input.h"
#include "inkcell/ui/sdl.h"
#include "inkcell/ui/theme.h"

#include "inkwell/net/mqtt.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/signals.h"
#include "mesh/app/control.h"
#include "mesh/core/config.h"
#include "mesh/core/firmware.h"
#include "mesh/core/firmware_update.h"
#include "mesh/core/session.h"
#include "mesh/core/updater.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/store_archive.h"
#include "mesh/ui/store_trends.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_app_publish_cache;

/*
 * The MQTT arrangement this client last acted on, which is what a new one is compared against.
 *
 * Kept here rather than read back out of the proxy because the two are not the same question.
 * The proxy holds what it is *doing*; this holds what it was *told*, and a start the proxy
 * refused - an address the radio holds that is not a host - leaves the first empty while the
 * second is exactly what must not be tried again on the next turn. Comparing against the proxy
 * would retry a refusal at the frequency of the event loop.
 */
struct mesh_app_mqtt_plan {
    struct inkwell_mqtt_client_config config;
    char filters[INKWELL_MQTT_CLIENT_FILTERS_MAX][INKWELL_MQTT_CLIENT_FILTER_MAX];
    size_t filter_count;
    /* Something has been handed to the proxy. False before the first one and after a stop, which
       is what makes "no plan" and "a plan for no subscriptions at all" different states. */
    bool active;
};

struct mesh_app {
    struct mesh_app_publish_cache *publish_cache;
    struct mesh_app_config config;
    struct inkwell_loop loop;
    struct mesh_transport_registry transport_registry;
    /* One conversation, whichever link carries it. Every transport is pointed at this before
       start, so switching between BLE and USB keeps the message log and reuses one node cache
       rather than each link keeping its own. */
    struct mesh_session session;
    struct mesh_ui_store ui_store;
    struct mesh_ui_controller ui_controller;
    struct mesh_ui_backend_cli_context ui_cli_context;
    struct inkcell_backend_fb_context ui_fb_context;
    struct inkcell_backend_sdl_context ui_sdl_context;
    struct inkcell_backend_headless_context ui_headless_context;
    /* The control socket, when --ui-control or MESHCLIENT_UI_CONTROL named one. */
    struct mesh_app_control control;
    struct mesh_ui_preferences ui_preferences;
    /* Conversation loaded from the cache at startup. The transport's log starts empty every
       run, so this is merged back in on publish; without it the first publish would erase the
       persisted history. */
    struct mesh_ui_message_list ui_messages_cached;
    struct inkcell_input ui_input;
    /*
     * Whether the backend reads its own buttons, which the SDL one does: a window's presses
     * arrive in the same queue as its resize and its close box, so the thing that owns the
     * window owns them. The evdev reader is then not started at all - on a development host
     * both would see the same keyboard and every press would arrive twice.
     */
    bool ui_backend_reads_input;
    struct inkwell_signals signals;
    /* Self-update: HTTPS through the event loop above. Its state is flattened into the UI's
       client info on every publish, so the About section renders it without the UI ever seeing
       a connection. */
    struct mesh_updater updater;
    /*
     * The *radio's* firmware, which is a different binary on a different computer: what board this
     * is, what it is running, and what upstream has published since. Reads two documents through
     * its own fetcher and installs nothing; the install paths in firmware_install.h and
     * firmware_ota.h are what turn that into a press.
     */
    struct mesh_firmware firmware;
    /*
     * Installing it, which is the other half and a separate module because it is a separate
     * press: the check reads two documents and touches nothing, and this downloads half a
     * megabyte, sends one admin verb and then talks to a bootloader or an OTA loader over a
     * bus. Its own fetcher, for the reason the check has one - two presses must not take each
     * other's child.
     */
    struct mesh_firmware_update firmware_update;
    /*
     * The ClientNotification sequence the install has already been shown.
     *
     * The BLE path's go-ahead and its refusal both arrive as one, on the session's read path,
     * and the install is driven from the tick - so somebody has to notice a new one and hand it
     * over. A sequence rather than a flag because the radio says other things too, and a
     * notification arriving while nothing is arming must not be replayed into the next install.
     */
    uint32_t firmware_notification_seq;
    /* Whether an install stopped the transports and owes them back. A flag rather than a
       question, because "stopped" and "stopped by us" are not the same state and only the
       second one should be restarted. */
    bool firmware_transports_stopped;
    /*
     * A radio this client has just written firmware to, and how long to give it to come back
     * before its bond is treated as dead. Empty when nothing is being watched.
     *
     * Installing firmware is the one thing that changes a radio's own half of a BLE bond, and
     * an ESP32 upgrade really does rotate its keys: after 2.8.0.47db0e3 a Heltec V3's stored
     * LTK no longer matched and every connect ended le-connection-abort-by-local. BlueZ still
     * reports the device Paired, because the bond file is ours and ours is intact, so
     * mesh_ble_do_connect() takes the bonded branch and the client can only say "connect
     * failed" - a dead end whose only exit is knowing to forget the node by hand.
     *
     * Watched rather than dropped outright at the end of the install, because the bond does not
     * always die: a radio that comes back on its own has a bond worth keeping, and throwing it
     * away would cost a PIN nobody needed to type. The reconnect is the test.
     */
    char firmware_bond_watch[64];
    uint64_t firmware_bond_watch_until_ms;
    /*
     * The look the UI is drawn with. Resolved once at start-up from MESHCLIENT_THEME, then the
     * saved preference, then the default, and published in the client info on every frame -
     * which is how a switch reaches the framebuffer without anything pushing at the backend.
     * Never NULL after mesh_app_init(); every theme lookup falls back to the default anyway.
     */
    const struct inkcell_theme *ui_theme;
    /* MESHCLIENT_THEME named it, so the Settings row is a fact rather than a switch. */
    bool ui_theme_from_env;
    char ui_preferences_path[256];
    char ui_handshake_cache_path[256];
    /*
     * The per-conversation transcript on the card, beside the handshake cache rather than
     * inside it. What it is for is in mesh/ui/store_archive.h; what it is *here* for is that
     * the app is the only thing that sees both halves of a publish - the transport ring that
     * says what is new, and the nav that says which conversation the reader has open.
     */
    struct mesh_ui_archive ui_archive;
    /*
     * And the trend log beside it, on the same terms: what it is for is in
     * mesh/ui/store_trends.h, and what it is *here* for is that the app is the only thing that
     * sees both halves - the history the store has just been filled with, and the nav that says
     * which node's detail the reader has open.
     */
    struct mesh_ui_trends ui_trends;
    /* The node whose trend was last read off the card, so the read happens when the reader moves
       rather than on every publish. 0 when no detail screen is open. */
    uint32_t ui_trend_node;
    bool ui_preferences_dirty;
    bool ui_handshake_cache_dirty;
    bool ui_cache_timer_armed;
    int ui_cache_timer_fd;
    /* Read marks change without any message or handshake changing, so the cache save needs its
       own trigger: the store's read-state revision, which moves when a mark's saved fields do.
       Deliberately not that state's `stamp`, which is its eviction ordering and moves every
       time a conversation is looked at - see struct mesh_ui_read_state. */
    uint32_t ui_read_state_revision;
    /* Foreground auto-connect bookkeeping; see mesh_app_autoconnect(). */
    uint64_t autoconnect_started_ms;
    uint64_t autoconnect_retry_at_ms;
    unsigned autoconnect_failures;
    /*
     * When the network arm of auto-connect may try again.
     *
     * Its own stamp rather than a share of the one above, because a configured host is the one
     * candidate that can be absent without being *gone*: a cable is plugged in or it is not and
     * a node is advertising or it is not, but an address somebody wrote down stays written down
     * with the WiFi off. Without this the network arm runs first on every turn, fails five
     * seconds later on its connect deadline, and Bluetooth is never reached at all.
     */
    uint64_t autoconnect_tcp_retry_at_ms;
    bool autoconnect_disabled;
    bool autoconnect_waiting_logged;
    /* Set while the link that is up is the preferred node's, over the air, so the wait after it
       drops is the long one: the radio we were just on is rebooting from a settings write far
       more often than it has left the room. A cable, a network host or another radio of ours
       leaves it clear. See mesh_app_preferred_ble_link_up(). */
    bool autoconnect_after_link;
    /* Set by an explicit disconnect from the Devices tab and cleared by the next explicit
       connect. Without it auto-connect would take the radio straight back and there would be
       no way to stand the link down at all. Session-only: a restart connects as usual. */
    bool autoconnect_held;
    /* Last published link state, so a drop can be announced once on the HUD. */
    bool ui_link_was_connected;
    /* Set while a connect the user asked for is in flight. A link can fail seconds after its
       connect() returned 0, so the reason is collected later from the transport; this keeps
       auto-connect's own retries from toasting the same failure on every backoff. */
    bool ui_report_link_error;
    /*
     * Messages sent this session whose delivery result has not been announced yet. A failed
     * DM is otherwise only a "!!" on a row the user may not be looking at, and the reason for
     * it (no ack, no route, a key problem) never reaches them at all.
     */
    struct mesh_app_sent_watch {
        uint32_t packet_id;
        char peer[MESH_UI_NAV_TARGET_NAME_MAX];
    } ui_sent_watch[8];
    size_t ui_sent_watch_count;
    /*
     * The radio's own announcements, seen once each. Both counters are session-scoped and both
     * reset to 0 when the link is reset, so a counter that has gone *backwards* is a new
     * connection rather than a repeat - which is what stops the first notification of every
     * connection from being swallowed as one already announced.
     */
    uint32_t ui_notice_seq_seen;
    uint32_t ui_reboot_notices_seen;
    /*
     * The key-verification ceremony as the UI last saw it: the `seq` that was published, and
     * the stage the sheet was opened for.
     *
     * Two fields rather than one because they answer two different questions. `seq` says
     * whether the exchange moved at all, and is what stops a sheet the user pressed "Later" on
     * from being reopened on the next publish - which happens several times a second. `stage`
     * says whether it moved to a *different question*, which is when the sheet has to come back
     * whatever the user did with the last one.
     */
    uint32_t ui_verify_seq_seen;
    uint8_t ui_verify_stage_shown;
    /*
     * How many nodes the roster held that the radio's NodeDB did not, at the last sync that
     * completed - and the id of that sync, so the comparison happens once per sync rather
     * than once per publish.
     *
     * The pair exists to catch one event: the roster and the radio's database parting company
     * in bulk, which is what a NodeDB reset does and what leaves the Nodes tab at 81 while the
     * Status screen says 2. A *rise* is the signal, not the count itself - a roster that has
     * held the same 79 orphans since the last run is not news, and toasting it on every launch
     * would be. Seeded from the cache at startup for exactly that reason.
     *
     * Counted as what the Settings row the toast names would drop, not as every off-radio
     * node, so a toast never sends the user to a row with nothing to do.
     */
    uint32_t ui_nodes_off_radio_seen;
    uint32_t ui_nodes_off_radio_sync_id;
    /*
     * The newest inbound ALERT_APP message already announced. A critical alert is the one
     * message the firmware expects a client to interrupt for, and the Messages tab may not be
     * the one on screen - so it toasts wherever the user is. Held as a packet id rather than a
     * count because the log is a ring that merges a cached history in at startup: a counter
     * would fire on every message restored from disk at the next launch.
     */
    uint32_t ui_alert_announced_id;
    /*
     * The same, for a direct message - the other thing worth interrupting for, and the reason
     * the Messages tab's badge is not enough on its own: a badge answers "is there anything
     * there", and a message addressed to this node and to nobody else is worth answering "from
     * whom, and what did they say" without the user having to walk to the tab to find out.
     *
     * Only direct messages. A channel is a room full of people talking and a notice per line
     * would make the client unusable on any real mesh, which is the same line ALERT_APP and
     * DETECTION_SENSOR_APP are already split along.
     *
     * `primed` is what stops a launch announcing history. The log is seeded from the cache
     * before the first publish, so the newest direct message in it is one this client has very
     * possibly already shown the user - days ago. The first pass adopts it silently and only
     * what arrives afterwards is news.
     */
    uint32_t ui_message_announced_id;
    bool ui_message_announce_primed;
    /*
     * One MQTT broker connection, held on behalf of whichever radio is attached and asking for
     * it. Everything about it - whether to be connected, to what, with which subscriptions - is
     * derived from the radio's own MQTTConfig every loop turn; see src/app/app_mqtt.c.
     */
    struct inkwell_mqtt_client mqtt;
    /* What it was last told to do; see struct mesh_app_mqtt_plan. */
    struct mesh_app_mqtt_plan mqtt_planned;
    /*
     * Broker messages that reached the radio's doorstep and no further, which neither side
     * counts: the proxy has already booked them as received, and the session has no counter for
     * a send it refused. The link dropping mid-turn and a message too large for the radio's own
     * field are both ordinary rather than exotic.
     */
    uint32_t mqtt_undelivered;
    /* MESHCLIENT_MQTT_PROXY said no. The connection is otherwise the radio's decision entirely,
       so this is the only say the client has in it. */
    bool mqtt_disabled;
    /* A Settings save in flight: the write counters seen when it was queued, so its ack or
       rejection can be announced once; see mesh_app_track_settings_save(). */
    bool settings_save_pending;
    uint32_t settings_writes_acked_seen;
    uint32_t settings_writes_failed_seen;
    uint32_t settings_reboot_notices_seen;
    uint64_t settings_save_started_ms;
    char settings_save_section[MESH_UI_SETTINGS_LABEL_MAX];
};

int mesh_app_init(struct mesh_app *app, const struct mesh_app_config *config);

/* The transport carrying the link right now: the connected one, else one that is mid-connect,
   else the BLE transport as the default target for a connect. Never NULL. */
struct mesh_transport *mesh_app_active_transport(void);
/* Identifier of the connected radio (BLE address or tty path), or NULL when nothing is up. */
const char *mesh_app_connected_identifier(void);
/* True while either link is partway through connecting. */
bool mesh_app_link_connecting(void);
void mesh_app_shutdown(struct mesh_app *app);
int mesh_app_run(struct mesh_app *app);

/* Copies the transports' discovery, handshake, message and settings state into the UI store.
   mesh_app_run() calls it every loop turn; exposed for tests. */
void mesh_app_publish_ui_state(struct mesh_app *app);

/* One step of the foreground connect policy. With no pointer and, outside the MinUI backend,
   no way to pick a row, the device has to connect on its own: the preferred node when it is in
   range, otherwise the strongest Meshtastic advertiser. Only acts in foreground mode; a no-op
   while connected or connecting. mesh_app_run() calls it every loop turn; exposed for tests
   and for MESHCLIENT_AUTOCONNECT=0 to be honoured in one place. */
void mesh_app_autoconnect(struct mesh_app *app);
/* Pops a link failure from the transports, shows it when the user asked for the connect, and
   backs auto-connect off. Call once per loop turn, before mesh_app_autoconnect(): a retry
   restarts the link and clears the reason the last attempt failed. Returns true if one was
   drained. */
bool mesh_app_report_link_errors(struct mesh_app *app);

/* Where a node sits in the Nodes tab, lower being nearer the top: 0 the radio we are connected
   to, 1 a pinned node, 2 another radio of ours (see mesh_ui_preferences_note_radio), 3 someone
   we have messages with, 4 a node heard over RF, 5 one fed in over MQTT. Ties break on
   last_heard. The UI carries fewer nodes than a busy mesh has, so this decides who survives the
   cut. Exposed for tests. */
unsigned mesh_app_node_rank(const struct mesh_node_summary *node, uint32_t my_node,
                            const struct mesh_message_log *log,
                            const struct mesh_ui_preferences *prefs);

/* Resolves FromRadio.region_presets from the grouped form the packet carries - each distinct
   preset list once, every region pointing at one by index - into the table the LoRa rows index
   by region. Anything the map does not describe is left at zero, which every reader takes as
   "constrain nothing". Exposed for tests. */
void mesh_app_flatten_region_presets(const meshtastic_LoRaRegionPresetMap *src,
                                     struct mesh_ui_region_presets *dst);

/* Turns the session's traceroute into the two ready-to-draw paths the UI carries: the ends
   stitched on (us going out, the target coming back), every hop resolved to a name, and each
   stop paired with the SNR of the link that reached it. Exposed for tests. */
struct mesh_handshake_status;
struct mesh_traceroute;
struct mesh_ui_traceroute;
void mesh_app_flatten_traceroute(const struct mesh_handshake_status *status,
                                 const struct mesh_traceroute *src, uint32_t my_node,
                                 struct mesh_ui_traceroute *dst);

/* Builds the admin write for a MESH_UI_ACTION_SAVE_SETTINGS: the radio's own copy of the
   section with the action's edits applied, since the firmware replaces sections whole.
   -ENOENT when the radio has not sent that section yet, -ENOTSUP for a section that is still
   read-only.

   `action->number` picks which write the section means, on the terms the confirm sheet reads it
   on: MESH_UI_SETTINGS_ACTION_NONE is the ordinary save, and MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL
   in the Channels section empties `action->channel` instead of editing it. Exposed for tests. */
struct mesh_radio_settings;
struct mesh_admin_request;
int mesh_app_build_settings_write(const struct mesh_radio_settings *radio,
                                  const struct mesh_ui_action *action,
                                  struct mesh_admin_request *out);

#ifdef __cplusplus
}
#endif
