#pragma once

/*
 * The UI store: the one piece of state the whole client draws from, and the umbrella over the
 * records that make it up.
 *
 * What lives *here* is the state machine - the store, the snapshot it publishes, the flags
 * that say what moved, and the API that drives all three. The records themselves are split by
 * subject, and a reader who needs one should include that header rather than this one:
 *
 *   mesh/ui/store_device.h     a radio discovery found, and how to reach it
 *   mesh/ui/store_node.h       a node's identity, telemetry, neighbours, route, key ceremony
 *   mesh/ui/store_channel.h    a channel slot, cached and in full
 *   mesh/ui/store_handshake.h  the roster and channel table the radio handed us
 *   mesh/ui/store_message.h    the transcript, the waypoint book, the read marks
 *   mesh/ui/store_settings.h   everything the Settings tab reads
 *
 * This header includes all six, because the snapshot embeds every record by value. That is
 * also the limit of what the split buys, and worth stating plainly so nobody measures it and
 * concludes the split failed: a file that handles a `struct mesh_ui_snapshot` needs the
 * definition of everything in it, so it still rebuilds when any record changes. Only a reader
 * that wants a record and not the whole state - `mesh/ui/trust.h` wanting a node,
 * `mesh/ui/devices.h` wanting a row - is decoupled by naming the narrow header, and those are
 * the ones that should. Including the umbrella to reach one record is how a header becomes a
 * god header again.
 *
 * Every record is nanopb-free by construction and names no core module, which is what lets
 * every backend and every screen test compile against a snapshot rather than against the
 * client. Several limits are therefore restated on this side of the seam and pinned against
 * the core's by a test; each says so where it is declared.
 */

#include "inkcell/ui/focus.h"
#include "mesh/ui/history.h"

#include "mesh/ui/nav.h"
#include "mesh/ui/store_channel.h"
#include "mesh/ui/store_device.h"
#include "mesh/ui/store_handshake.h"
#include "mesh/ui/store_message.h"
#include "mesh/ui/store_mqtt.h"
#include "mesh/ui/store_node.h"
#include "mesh/ui/store_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    /* The key-verification ceremony moved. Its own flag rather than NAV's, because the sheet
       that draws it is opened by the *radio* asking a question rather than by a press, and a
       frame that repainted only when the nav moved would leave the question unasked until the
       user happened to touch something. */
    MESH_UI_UPDATE_VERIFY = 1U << 8,
    /* The broker connection moved. Its own flag rather than a share of TRANSPORT's, because
       TRANSPORT means the radio link and the CLI backend prints a line when it changes - an
       MQTT retry every five seconds would scroll the radio's own state off the console. */
    MESH_UI_UPDATE_MQTT = 1U << 9,
};
typedef uint32_t mesh_ui_update_flags;

struct mesh_ui_snapshot {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    struct mesh_ui_message_list messages;
    /*
     * The open conversation, read as deep as the card could fill it. Drawn instead of
     * `messages` while it is a window over the conversation the nav has open - see
     * mesh_ui_store_message_view(), which is the only thing any screen should ask.
     */
    struct mesh_ui_thread thread;
    /* The places the mesh has shared. Not persisted: see mesh_ui_store_set_waypoints(). */
    struct mesh_ui_waypoint_list waypoints;
    /* Which conversations have been read, so the list can badge the ones that have not. */
    struct mesh_ui_read_state read_state;
    /* Transport state ("waiting-for-bluez", "scanning", "running", ...). Rendered by the
       backends so an empty device list is diagnosable on a device with no console. */
    char transport_status[MESH_UI_TRANSPORT_STATUS_MAX];
    /*
     * The network address somebody wrote down, whether or not the link is up, and empty when
     * there is none.
     *
     * A network cannot be scanned, so this is the Devices tab's whole answer to "what could I
     * reach over the network" - and the row that carries it is also the only way to type one.
     * It is deliberately not one of the rows above: `devices` is what discovery found, and a
     * host that has never connected was found by nobody.
     */
    char network_host[MESH_UI_NETWORK_HOST_MAX];
    /* Cursor, current tab, compose target: what the user is doing, as opposed to what the
       radio is doing. Clamped to the lists above before every snapshot. */
    struct mesh_ui_nav nav;
    /* Radio configuration for the Settings tab. Not persisted: it describes the radio that
       is connected right now. */
    struct mesh_ui_settings settings;
    /* The trace in flight, or the last one's outcome. Not persisted: what is worth keeping
       about a finished trace is in the log beside it. */
    struct mesh_ui_traceroute traceroute;
    /* Every route measured and not yet forgotten, one entry per node. Persisted with the
       roster; ask mesh_ui_snapshot_traceroute_view() rather than reading either field. */
    struct mesh_ui_traceroute_log traceroutes;
    /* The key-verification ceremony in progress, or a record with `stage` IDLE. Not
       persisted: it belongs to one link and one nonce. */
    struct mesh_ui_verification verification;
    /*
     * What the client has watched happen, as opposed to everything above, which is what is true
     * now. The one part of a snapshot that is not a copy of what the radio last said - see
     * include/mesh/ui/history.h.
     *
     * Persisted in two halves and by neither of the store's own calls: the radio's airtime pair
     * rides the handshake cache (store_file.c), and a node's readings are a per-node log the app
     * writes on every publish and reads back when that node's detail screen is opened
     * (mesh/ui/store_trends.h).
     */
    struct mesh_ui_history history;
    /* The broker connection held for the attached radio, or a zeroed record when no radio has
       asked for one. Not persisted: see mesh/ui/store_mqtt.h. */
    struct mesh_ui_mqtt_state mqtt;
    mesh_ui_update_flags update_flags;
    /*
     * How many body rows the backend's last paged list had room for - see `page_rows` on struct
     * mesh_ui_store, which is where it is told and what this is a copy of.
     *
     * Published because the action bar now needs it: a node chart listing its readings names Up
     * and Down only once there are more readings than fit, and the bar is built from a snapshot.
     * The setter still publishes nothing, so this is what the *last* publish saw - which makes
     * the hint at most one frame late on a panel that has just changed size, and 0 before the
     * first paged list has been drawn. Both answer "do not name the gesture", which is the safe
     * direction: a keycap named over nothing is the thing this table exists to prevent.
     *
     * It cannot oscillate, which is the question a fact about the panel feeding the bar has to
     * answer. Naming a gesture can only cost the body a line, and fewer rows can only make
     * "more readings than fit" more true - so the condition moves in one direction and settles.
     */
    uint32_t page_rows;
};

struct mesh_ui_store {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    struct mesh_ui_message_list messages;
    struct mesh_ui_thread thread;
    struct mesh_ui_waypoint_list waypoints;
    struct mesh_ui_read_state read_state;
    char transport_status[MESH_UI_TRANSPORT_STATUS_MAX];
    char network_host[MESH_UI_NETWORK_HOST_MAX];
    struct mesh_ui_nav nav;
    struct mesh_ui_settings settings;
    struct mesh_ui_traceroute traceroute;
    struct mesh_ui_traceroute_log traceroutes;
    struct mesh_ui_verification verification;
    struct mesh_ui_history history;
    struct mesh_ui_mqtt_state mqtt;
    /*
     * The clock the last mesh_ui_store_tick() carried, which is what stamps a history sample.
     *
     * The setters do not take a time - they are called from wherever a publish happens to reach
     * the store - and a series needs one, so the store keeps the last it was told. The event
     * loop ticks every turn, so it is never more than a turn stale; before the first tick it is
     * 0, which is a real point on a monotonic clock rather than a missing one.
     */
    uint64_t now_ms;
    /*
     * How many body rows the backend's last paged list had room for, or 0 when it has not said.
     *
     * The one fact about the panel the nav needs: a card of facts on the node detail is one stop
     * when it fits and a page at a time when it does not, and only the backend knows which. Told
     * before every press by the controller, from the frame the reader is looking at, so the nav
     * and the renderer page a card by the same number. 0 pages nothing.
     */
    uint32_t page_rows;
    /*
     * The boxes the last frame drew, or NULL.
     *
     * The other fact about the panel a press can need, and told the same way and at the same
     * moment: a d-pad asks "what is over there", and an index cannot answer it. Two answers side
     * by side on one line, two stacked because the words were too long for one - the difference
     * is a fact about the frame that was drawn, and a nav resolving it from a cursor is a nav
     * guessing which layout the renderer chose.
     *
     * Borrowed, not owned. The map is rebuilt every frame by whoever draws, so this is good
     * only until the next one - which is exactly as long as a press between two frames needs
     * it. NULL is the whole of the opt-out: a backend that registers nothing, and every screen
     * that has not adopted this, walk their own indices as they always did.
     */
    const struct inkcell_focus_map *focus;
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
/*
 * The broker connection, whole.
 *
 * One setter for the whole record rather than one per field, because every field of it is read
 * off the same proxy in the same turn and a half-updated record is a card that says "Connected"
 * beside last week's error. NULL clears it, which is what a client that has stopped proxying
 * publishes. Marks MESH_UI_UPDATE_MQTT only when something actually moved - this is called on
 * every publish, and the counters move on most of them.
 */
void mesh_ui_store_set_mqtt(struct mesh_ui_store *store, const struct mesh_ui_mqtt_state *mqtt);
/*
 * The network address the client is configured to reach, or NULL/"" for none.
 *
 * A network has no scan behind it, so this is published alongside discovery rather than as one
 * of its rows: nothing found this host, somebody typed it. It carries MESH_UI_UPDATE_DISCOVERY
 * because the Devices tab is the one screen it changes.
 */
void mesh_ui_store_set_network_host(struct mesh_ui_store *store, const char *host);
void mesh_ui_store_set_messages(struct mesh_ui_store *store,
                                const struct mesh_ui_message_list *messages);
/*
 * Replaces the open conversation's window; quiet when nothing changed. NULL empties it, which
 * is how the app says the reader has left the thread.
 *
 * Filled by the app from the archive on the card (mesh/ui/store_archive.h) rather than from
 * anything the radio said, which is why it is a setter of its own and not part of
 * mesh_ui_store_set_messages(): the flat list turns over whenever traffic arrives, and this
 * turns over when the reader opens a different conversation.
 */
void mesh_ui_store_set_thread(struct mesh_ui_store *store, const struct mesh_ui_thread *thread);

/*
 * The messages the screen in front of the reader should draw.
 *
 * Every screen asks this rather than reaching for either list, and that is the whole of what
 * keeps the deep window honest: it answers with the window only when the window is over the
 * conversation `nav` has open, and with the flat list otherwise. See the definition for the
 * three cases the flat list is the right answer to.
 *
 * The nav is passed rather than taken off the store because they come apart: mesh_ui_store_view()
 * builds a store with none, and its callers hold the real one. NULL falls back to the store's.
 */
struct mesh_ui_message_view mesh_ui_store_message_view(const struct mesh_ui_store *store,
                                                       const struct mesh_ui_nav *nav);

/* The same answer, asked of the record a backend holds. See mesh_ui_store_view() for why a
   snapshot cannot simply be turned into a store and asked the question above. */
struct mesh_ui_message_view mesh_ui_snapshot_message_view(const struct mesh_ui_snapshot *snapshot);
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
/*
 * Replaces the traceroute view; quiet when nothing changed.
 *
 * A record that carries a finished route is also recorded in the log, which is the only way an
 * entry gets in there: the app hands this whatever the session's one trace slot says, every
 * publish, and the store decides what was worth keeping.
 */
void mesh_ui_store_set_traceroute(struct mesh_ui_store *store,
                                  const struct mesh_ui_traceroute *traceroute);

/*
 * What this client knows about the route to one node, or NULL when it knows nothing.
 *
 * The trace in flight when it is this node's - so a running or timed-out trace still says so -
 * and otherwise that node's last measured route, which may have been measured in an earlier
 * run. Every screen asks this rather than reaching for `traceroute`: reading the one slot
 * directly is what used to make a second node's detail describe itself with the first node's
 * trace.
 */
const struct mesh_ui_traceroute *mesh_ui_store_traceroute_view(const struct mesh_ui_store *store,
                                                               uint32_t node_id);

/* The same answer, asked of the record a backend holds. */
const struct mesh_ui_traceroute *
mesh_ui_snapshot_traceroute_view(const struct mesh_ui_snapshot *snapshot, uint32_t node_id);
/* Replaces the key-verification view; quiet when nothing changed. NULL is the idle record,
   which is how the app says an exchange ended. */
void mesh_ui_store_set_verification(struct mesh_ui_store *store,
                                    const struct mesh_ui_verification *verification);

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

/*
 * One message, out of both lists and out of the open window, with its reactions.
 *
 * Named by its conversation as well as its packet id, because an id is only unique per sender -
 * see mesh_ui_message_list_forget_message(), which this is the store's half of.
 *
 * The same warning the conversation delete carries applies here and for the same reason: this
 * is only the UI's copy, and a delete that does not also reach the transport's ring and the
 * history read back at startup is undone by the next publish. MESH_UI_ACTION_DELETE_MESSAGE is
 * what reaches all three.
 */
uint32_t mesh_ui_store_forget_message(struct mesh_ui_store *store, uint8_t kind, uint32_t node,
                                      uint8_t channel, uint32_t packet_id);

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

/*
 * The key-verification sheet and the keyboard that collects the security number, opened and
 * closed by the app from the ceremony's stage - not by a press, for the pairing prompt's
 * reason: the thing that raises them is the radio asking.
 *
 * The two `open` calls return whether the overlay is *now up*, which is not the same as whether
 * this call put it there: already-open counts. Both defer to a BlueZ pairing prompt, which is
 * blocking a bond on a much shorter clock - so a false here means "not yet", and the caller
 * must not record the stage as shown or it will never try again.
 */
bool mesh_ui_store_open_verify_sheet(struct mesh_ui_store *store);
void mesh_ui_store_close_verify_sheet(struct mesh_ui_store *store);
bool mesh_ui_store_open_verify_number(struct mesh_ui_store *store);
void mesh_ui_store_close_verify_number(struct mesh_ui_store *store);
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

/* See `page_rows` on struct mesh_ui_store. Publishes nothing: it is read by the next snapshot
   rather than by this frame, and every reader of it treats "not said yet" as "do not act". */
void mesh_ui_store_set_page_rows(struct mesh_ui_store *store, uint32_t rows);

/* See `focus` on struct mesh_ui_store. Publishes nothing, and borrows rather than copies: the
   map is the last frame's and is handed over for the press that follows it. */
void mesh_ui_store_set_focus_map(struct mesh_ui_store *store, const struct inkcell_focus_map *map);

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
 * src/ui/tables/actions.c, which needs the row under the cursor to name a press.
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

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot);

int mesh_ui_store_save(const struct mesh_ui_store *store, const char *path);
int mesh_ui_store_load(struct mesh_ui_store *store, const char *path);

#ifdef __cplusplus
}
#endif
