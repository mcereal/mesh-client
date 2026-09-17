#ifndef MESH_CORE_APP_INTERNAL_H
#define MESH_CORE_APP_INTERNAL_H

/*
 * The seams between app.c and the files split out of it.
 *
 * app.c had grown to 2700 lines around four jobs that only touch each other through the handful
 * of calls below: owning the link and the process lifecycle (app.c), turning the UI's pending
 * edits into admin writes (app_settings.c), reacting to a button press (app_actions.c), and
 * copying transport state into the UI store (app_publish.c). A fifth joined them rather than
 * going back into app.c - deciding whether to hold a broker connection for the attached radio
 * (app_mqtt.c), which is the one job here that reads the radio's configuration and answers with
 * a socket.
 *
 * Nothing here is part of the client's public surface - that is include/mesh/app/app.h. These
 * declarations exist because C has no unit smaller than a translation unit: they would all be
 * `static` if the files were still one.
 */

#include "mesh/app/app.h"

#include <stdint.h>

/* ---- app.c: link routing ---------------------------------------------------------------- */

/* Connects `identifier` over the transport `kind` names (a mesh_ui_device_kind), dropping the
   other link first. Returns the transport's connect() result. */
int mesh_app_link_connect(struct mesh_app *app, const char *identifier, uint8_t kind);

/* Button presses from the evdev reader; installed on the UI input as its handler. */
void mesh_app_on_ui_key(void *userdata, enum mesh_ui_key key);

/* Records that this device is the one we are on: the head of the preferences' most-recently-
   used list, and the node auto-connect reaches for first. Sets app->ui_preferences_dirty when
   the file needs rewriting. One function because the two are one fact - a config that kept the
   node you launched with while the preferences learned the node you switched to is how a
   reconnect goes back to the radio you just left. */
void mesh_app_note_connected_device(struct mesh_app *app, const char *identifier, uint8_t kind);

/* ---- app_actions.c ---------------------------------------------------------------------- */

/* What the UI asked for; installed on the UI controller as its action handler. */
void mesh_app_on_ui_action(void *userdata, const struct mesh_ui_action *action);

/*
 * Drives a radio firmware install and feeds it what the radio has said since the last turn.
 *
 * Here rather than in app.c because the install's hooks are here: the thing that owns the
 * callbacks is the thing that should own the pump. The notification relay is part of it for the
 * same reason - the BLE path's go-ahead and its refusal arrive as a ClientNotification on the
 * session's read path and the install runs from the tick, so this is the only place the two
 * meet. Call every loop turn.
 */
void mesh_app_firmware_update_tick(struct mesh_app *app, uint64_t now);

/* ---- app_mqtt.c ------------------------------------------------------------------------- */

/*
 * What the radio's MQTT configuration asks this client to connect to on its behalf.
 *
 * True when the radio wants proxying, with `out` describing the broker; false when it does not,
 * and `out` is zeroed either way. Mirrors the firmware's own defaulting rules - which are not
 * obvious and are explained where they are applied. Exposed for tests.
 */
bool mesh_app_mqtt_plan(const struct mesh_session *session, struct mesh_mqtt_proxy_config *out);

/* Whether a connection already open on `have` would have to be torn down to become `want`.
   Exposed for tests. */
bool mesh_app_mqtt_config_differs(const struct mesh_mqtt_proxy_config *have,
                                  const struct mesh_mqtt_proxy_config *want);

/* Whether what the proxy was last told to do differs from this, which is the whole of the
   decision to reconnect. `filters` is not const because C will not convert `char (*)[N]` to
   `const char (*)[N]`. Exposed for tests. */
bool mesh_app_mqtt_plan_changed(const struct mesh_app_mqtt_plan *planned,
                                const struct mesh_mqtt_proxy_config *want,
                                char (*filters)[MESH_MQTT_FILTER_MAX], size_t count);

/* Fills `out` with the topic filters this radio's configuration says to subscribe to and returns
   how many. Stops at `cap`, and at the first filter that cannot be built. Exposed for tests. */
size_t mesh_app_mqtt_filters(const struct mesh_session *session, char (*out)[MESH_MQTT_FILTER_MAX],
                             size_t cap);

/* Brings the proxy up with the event loop and the CA bundle the updater resolved. Call from
   mesh_app_init() once both exist. */
void mesh_app_mqtt_init(struct mesh_app *app);
/* Drops the connection and unregisters its descriptor. Call before the loop goes. */
void mesh_app_mqtt_shutdown(struct mesh_app *app);
/*
 * Brings the broker connection into line with what the radio is asking for, and drives its
 * clock. Call every loop turn: the whole of the decision is re-derived each time rather than
 * pushed at from wherever a setting changes, so there is no path by which a configuration can
 * move without the connection following it.
 */
void mesh_app_mqtt_tick(struct mesh_app *app, uint64_t now_ms);

/*
 * The broker connection as the Status screen reads it. Zeroed when the radio has not asked to be
 * proxied for, which is what the card is drawn on.
 *
 * Here rather than in app_publish.c because it reads the proxy and the recorded plan, which are
 * this file's to interpret - the publish path's job is to carry it to the store.
 */
struct mesh_ui_mqtt_state;
void mesh_app_mqtt_publish_state(const struct mesh_app *app, struct mesh_ui_mqtt_state *out);

/* ---- app_settings.c --------------------------------------------------------------------- */

/* Queues the admin write a MESH_UI_ACTION_SAVE_SETTINGS asks for and toasts the outcome. */
void mesh_app_save_settings(struct mesh_app *app, const struct mesh_ui_action *action,
                            uint64_t now);

/* The fixed-position save, which is a position write rather than a config section. */
void mesh_app_save_fixed_position(struct mesh_app *app, const struct mesh_ui_action *action,
                                  uint64_t now);

/* The ham-mode press, which is a set_ham_mode rather than a config section: it reads the call
   sign, frequency and power rows the way the fixed-position row reads the coordinates. */
void mesh_app_save_ham_mode(struct mesh_app *app, const struct mesh_ui_action *action,
                            uint64_t now);

/*
 * A protobuf float <-> the fixed-point integer the UI holds and types.
 *
 * The two halves of one decision and so declared together: a frequency crosses this fence
 * twice, out through app_publish.c and back through app_settings.c, and a rounding done
 * differently at the two ends is a row that reads back a hair off whatever was typed into it.
 * `digits` is one of the MESH_UI_*_DIGITS constants in mesh/ui/settings.h.
 */
int64_t mesh_app_scale_float(float value, uint32_t digits);
float mesh_app_unscale_float(int64_t scaled, uint32_t digits);

/* Announces the outcome of an in-flight save once - the ack, the rejection, or the radio
   dropping the link mid-write. Called from the publish path, which is where the radio's write
   counters become visible. */
void mesh_app_track_settings_save(struct mesh_app *app,
                                  const struct mesh_radio_settings *radio_settings,
                                  bool link_connected);

/* ---- app_publish.c ---------------------------------------------------------------------- */

/*
 * Which bus the radio is on, as the firmware module's own idea of a path - NONE for nothing
 * connected *and* for a link no firmware can travel on, which today is TCP.
 *
 * Shared because the press and the row have to agree about it: the row is built from what this
 * answered at publish time, and the press re-asks it before arming because the link can have
 * moved in between. Two readings of the bus would be two answers about which radio is being
 * sent into a loader.
 */
enum mesh_firmware_path mesh_app_firmware_bus(void);

void mesh_app_flush_ui_cache(struct mesh_app *app);
void mesh_app_close_ui_cache_timer(struct mesh_app *app);

/* Resolves a node number to something a human can read, preferring the short name the NodeDB
   gave us and falling back to the Meshtastic-style "!hex" id. */
void mesh_app_format_peer_name(const struct mesh_handshake_status *status, uint32_t node_id,
                               char *out, size_t out_len);

/*
 * The same, for the *last byte* of a node number - MeshPacket.relay_node and .next_hop, which
 * is all the LoRa header has room for.
 *
 * `origin` is the node the byte would be uninteresting for: the sender, whose own last byte the
 * firmware stamps in as it transmits, so a packet heard straight from its sender names that
 * sender. That case and a zero byte both write "" - there is nothing to say, and a chip saying
 * it would be on most of the transcript. `origin_hops` is how far the packet came, negative
 * when the firmware did not say, and it is what stops that shortcut lying: a packet that took
 * at least one hop *was* relayed, so a byte matching the sender there is a collision with some
 * other node rather than the firmware saying "direct". Pass `origin` 0 to skip the test.
 *
 * Otherwise the roster is scanned for the nodes the byte could name. Exactly one match gets
 * its short name; none and more than one both get MESH_STR_NODE_VAL_RELAY_HEX, the "!..a3"
 * partial id. That is the whole of the ambiguity policy and it is deliberately the strict one:
 * a byte matches 1 in 256 node numbers, so a mesh of a hundred nodes has collisions by
 * arithmetic rather than by bad luck, and the wrong name confidently drawn is worse than the
 * two hex digits the radio actually gave us.
 */
void mesh_app_format_relay_name(const struct mesh_handshake_status *status, uint8_t last_byte,
                                uint32_t origin, int origin_hops, char *out, size_t out_len);

/*
 * Whether more than one node in the *whole* session roster ends in `last_byte`.
 *
 * Published onto each node so the detail screen can render the byte honestly without the roster
 * that settles it. It cannot settle it itself: mesh_app_publish_ui_state() ranks a mesh larger
 * than MESH_UI_MAX_HANDSHAKE_NODES down to that many, and a byte that looks unique only because
 * its other claimant was ranked away is exactly how a confident wrong name gets drawn.
 */
bool mesh_app_relay_byte_is_ambiguous(const struct mesh_handshake_status *status,
                                      uint8_t last_byte);

/* Seeds the session's node roster from the handshake cache the last run left on disk. Call
   once at startup, after the store has been loaded and before the first connect. */
void mesh_app_seed_nodes_from_cache(struct mesh_app *app);

/*
 * The channel slot a broadcast originated by this client goes out on.
 *
 * The slot whose role is PRIMARY, and 0 when the radio has not told us its table yet. It is a
 * lookup rather than a constant because MeshPacket.channel is an *index* into that table and
 * nothing in the protocol pins the primary to slot 0 - it is only where every radio happens to
 * put it. A waypoint sent to the wrong index is one nobody on the mesh can decrypt.
 */
uint8_t mesh_app_primary_channel(const struct mesh_handshake_status *status);

/* Starts watching a sent packet so its delivery result can be announced once. */
void mesh_app_watch_sent(struct mesh_app *app, uint32_t packet_id, const char *peer);

#endif /* MESH_CORE_APP_INTERNAL_H */
