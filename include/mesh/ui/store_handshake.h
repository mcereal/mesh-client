#pragma once

/*
 * What the radio told us about itself when we connected: who it is, the roster it carries, and
 * the channel table it runs.
 *
 * The one record here that is *not* a copy of the current radio - it is persisted, and the
 * roster deliberately outlives the connection (docs/non-bugs.md). It is also the only place
 * the map's roster and the list's roster sit side by side, which is the reason the two arrays
 * are documented against each other rather than apart.
 *
 * See mesh/ui/store.h for the whole.
 */

#include "mesh/ui/store_channel.h"
#include "mesh/ui/store_node.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/*
 * How many of the nodes in `handshake` the radio's NodeDB no longer carries - the rows the
 * Nodes tab dims and marks "off radio". Counted from the published rows themselves rather than
 * carried alongside them, so it is always a number in the same scope as `node_count`: the UI
 * holds 128 nodes and the session roster holds 256, and "128 nodes, 200 off radio" is not a
 * thing any screen should be able to draw.
 */
uint32_t mesh_ui_handshake_off_radio(const struct mesh_ui_handshake_state *handshake);

#ifdef __cplusplus
}
#endif
