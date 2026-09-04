#pragma once

#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/signals.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/fb.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/input.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/store.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_app {
    struct mesh_app_config config;
    struct mesh_event_loop loop;
    struct mesh_transport_registry transport_registry;
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
    char ui_preferences_path[256];
    char ui_handshake_cache_path[256];
    bool ui_preferences_dirty;
    bool ui_handshake_cache_dirty;
    /* Foreground auto-connect bookkeeping; see mesh_app_autoconnect(). */
    uint64_t autoconnect_started_ms;
    uint64_t autoconnect_retry_at_ms;
    unsigned autoconnect_failures;
    bool autoconnect_disabled;
    bool autoconnect_waiting_logged;
};

int mesh_app_init(struct mesh_app *app, const struct mesh_app_config *config);
void mesh_app_shutdown(struct mesh_app *app);
int mesh_app_run(struct mesh_app *app);

/* One step of the foreground connect policy. With no pointer and, outside the MinUI backend,
   no way to pick a row, the device has to connect on its own: the preferred node when it is in
   range, otherwise the strongest Meshtastic advertiser. Only acts in foreground mode; a no-op
   while connected or connecting. mesh_app_run() calls it every loop turn; exposed for tests
   and for MESHCLIENT_AUTOCONNECT=0 to be honoured in one place. */
void mesh_app_autoconnect(struct mesh_app *app);

#ifdef __cplusplus
}
#endif
