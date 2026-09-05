#ifndef MESH_CORE_APP_INTERNAL_H
#define MESH_CORE_APP_INTERNAL_H

/*
 * The seams between app.c and the three files split out of it.
 *
 * app.c had grown to 2700 lines around four jobs that only touch each other through the handful
 * of calls below: owning the link and the process lifecycle (app.c), turning the UI's pending
 * edits into admin writes (app_settings.c), reacting to a button press (app_actions.c), and
 * copying transport state into the UI store (app_publish.c).
 *
 * Nothing here is part of the client's public surface - that is include/mesh/core/app.h. These
 * declarations exist because C has no unit smaller than a translation unit: they would all be
 * `static` if the four files were still one.
 */

#include "mesh/core/app.h"

#include <stdint.h>

/* ---- app.c: link routing ---------------------------------------------------------------- */

/* Connects `identifier` over the transport `kind` names (a mesh_ui_device_kind), dropping the
   other link first. Returns the transport's connect() result. */
int mesh_app_link_connect(struct mesh_app *app, const char *identifier, uint8_t kind);

/* Button presses from the evdev reader; installed on the UI input as its handler. */
void mesh_app_on_ui_key(void *userdata, enum mesh_ui_key key);

/* ---- app_actions.c ---------------------------------------------------------------------- */

/* What the UI asked for; installed on the UI controller as its action handler. */
void mesh_app_on_ui_action(void *userdata, const struct mesh_ui_action *action);

/* ---- app_settings.c --------------------------------------------------------------------- */

/* Queues the admin write a MESH_UI_ACTION_SAVE_SETTINGS asks for and toasts the outcome. */
void mesh_app_save_settings(struct mesh_app *app, const struct mesh_ui_action *action,
                            uint64_t now);

/* The fixed-position save, which is a position write rather than a config section. */
void mesh_app_save_fixed_position(struct mesh_app *app, const struct mesh_ui_action *action,
                                  uint64_t now);

/* Announces the outcome of an in-flight save once - the ack, the rejection, or the radio
   dropping the link mid-write. Called from the publish path, which is where the radio's write
   counters become visible. */
void mesh_app_track_settings_save(struct mesh_app *app,
                                  const struct mesh_radio_settings *radio_settings,
                                  bool link_connected);

/* ---- app_publish.c ---------------------------------------------------------------------- */

/* Resolves a node number to something a human can read, preferring the short name the NodeDB
   gave us and falling back to the Meshtastic-style "!hex" id. */
void mesh_app_format_peer_name(const struct mesh_handshake_status *status, uint32_t node_id,
                               char *out, size_t out_len);

/* Seeds the session's node roster from the handshake cache the last run left on disk. Call
   once at startup, after the store has been loaded and before the first connect. */
void mesh_app_seed_nodes_from_cache(struct mesh_app *app);

/* Starts watching a sent packet so its delivery result can be announced once. */
void mesh_app_watch_sent(struct mesh_app *app, uint32_t packet_id, const char *peer);

#endif /* MESH_CORE_APP_INTERNAL_H */
