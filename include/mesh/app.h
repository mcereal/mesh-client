#pragma once

#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/session.h"
#include "mesh/signals.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/fb.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/input.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/updater.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_app {
    struct mesh_app_config config;
    struct mesh_event_loop loop;
    struct mesh_transport_registry transport_registry;
    /* One conversation, whichever link carries it. Every transport is pointed at this before
       start, so switching between BLE and USB keeps the message log and reuses one node cache
       rather than each link keeping its own. */
    struct mesh_session session;
    struct mesh_ui_store ui_store;
    struct mesh_ui_controller ui_controller;
    struct mesh_ui_backend_cli_context ui_cli_context;
    struct mesh_ui_backend_minui_context ui_minui_context;
    struct mesh_ui_backend_fb_context ui_fb_context;
    struct mesh_ui_preferences ui_preferences;
    /* Conversation loaded from the cache at startup. The transport's log starts empty every
       run, so this is merged back in on publish; without it the first publish would erase the
       persisted history. */
    struct mesh_ui_message_list ui_messages_cached;
    struct mesh_ui_input ui_input;
    struct mesh_signals signals;
    /* Self-update: forks the device's curl/wget through the event loop above. Its state is
       flattened into the UI's client info on every publish, so the About section renders it
       without the UI ever seeing a child process. */
    struct mesh_updater updater;
    char ui_preferences_path[256];
    char ui_handshake_cache_path[256];
    bool ui_preferences_dirty;
    bool ui_handshake_cache_dirty;
    /* Read marks change without any message or handshake changing, so the cache save needs its
       own trigger: the store bumps this stamp every time a conversation is marked read. */
    uint32_t ui_read_state_stamp;
    /* Foreground auto-connect bookkeeping; see mesh_app_autoconnect(). */
    uint64_t autoconnect_started_ms;
    uint64_t autoconnect_retry_at_ms;
    unsigned autoconnect_failures;
    bool autoconnect_disabled;
    bool autoconnect_waiting_logged;
    /* Last published link state, so a drop can be announced once on the HUD. */
    bool ui_link_was_connected;
    /* Set while a connect the user asked for is in flight. A link can fail seconds after its
       connect() returned 0, so the reason is collected later from the transport; this keeps
       auto-connect's own retries from toasting the same failure on every backoff. */
    bool ui_report_link_error;
    /* A Settings save in flight: the write counters seen when it was queued, so its ack or
       rejection can be announced once; see mesh_app_track_settings_save(). */
    bool settings_save_pending;
    uint32_t settings_writes_acked_seen;
    uint32_t settings_writes_failed_seen;
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

/* Builds the admin write for a MESH_UI_ACTION_SAVE_SETTINGS: the radio's own copy of the
   section with the action's edits applied, since the firmware replaces sections whole.
   -ENOENT when the radio has not sent that section yet, -ENOTSUP for a section that is still
   read-only. Exposed for tests. */
struct mesh_radio_settings;
struct mesh_admin_request;
int mesh_app_build_settings_write(const struct mesh_radio_settings *radio,
                                  const struct mesh_ui_action *action,
                                  struct mesh_admin_request *out);

#ifdef __cplusplus
}
#endif
