#pragma once

#include "mesh/i18n/strings.h"
#include "mesh/ui/store_handshake.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_handshake_state;
struct mesh_ui_node_summary;

/*
 * ---- the Nodes list's filter ------------------------------------------------------------------
 *
 * Which of the roster the list is showing, as a table read by everything that has an opinion
 * about it - src/ui/nav/nav.c for the row count and the row-to-node mapping,
 * src/ui/tables/actions.c for the verb the press gets named with, and the backend for the chips it
 * draws. That is src/ui/tables/status.c's shape one tab over, and it is here for status.c's reason:
 * three files walking the same list is how the chip under the cursor and the rows under the chip
 * come to disagree about what "Direct" means.
 *
 * Why a filter exists at all. The roster deliberately outlives the connection and the radio's
 * NodeDB evicts, so this list is the one screen in the client that grows without bound - a busy
 * mesh publishes the best 128 of 256 and the reader is looking for one of them. Sorting cannot
 * answer that (the list is already ranked, and a rank is what put the node they want at 94). What
 * a reader always knows is which *kind* of node they are after, and there are only ever two kinds
 * worth asking for: the ones they chose to keep, and the ones that are actually in earshot. When
 * they know a piece of the name too, the Find row narrows further - see
 * mesh_ui_node_query_matches().
 *
 * So the set is deliberately three and closed. Every candidate for a fourth failed the same
 * test - a filter has to be a question a reader arrives with, not a column the client happens to
 * hold. "Positioned" is the map's roster and the map row is two rows away; "off radio" is a
 * state the rows already say in the column where a signal would be; "has telemetry" is a fact
 * about a node nobody goes looking for a node by.
 */
enum mesh_ui_node_filter {
    /* Everything published, in the rank the app put it in. The list as it has always been. */
    MESH_UI_NODE_FILTER_ALL = 0,
    /*
     * Heard directly: in the radio's NodeDB, no relay, no MQTT, and a reading behind it.
     *
     * **The test is what the list would draw a staircase on, and nothing narrower.** That is
     * one sentence and it took two goes to get right, so it is worth spelling out both halves.
     *
     * mesh_ui_node_signal_heard() is most of it, for reasons that are not obvious from the
     * field names: a node reached over hops has an SNR describing the last relay, one arriving
     * over MQTT describes nothing on the air at all, and an unset `hops_away` is the firmware
     * declining to say rather than a zero - so a filter asking `hops_away == 0` would answer
     * "in earshot" for every node the firmware was quiet about.
     *
     * `in_nodedb` is the half that predicate does not carry, and leaving it out was a real bug
     * rather than a hypothetical one. mesh_session_resolve_nodedb_membership() clears that flag
     * from the sync epoch alone and touches neither `snr` nor `hops_away`, so a node that was
     * genuinely heard directly and has since dropped out of the radio's database keeps a
     * perfectly good reading. The renderer tests `!in_nodedb` *first* and draws "off radio"
     * where a staircase would go - so such a node sat in Direct with nothing beside its name,
     * which is the chip and the column disagreeing about one fact on one row: the exact failure
     * the paragraph above exists to prevent, arriving through the branch above the one it was
     * watching. Match the renderer's precedence, not one of its conditions.
     */
    MESH_UI_NODE_FILTER_DIRECT,
    /*
     * Pinned: the nodes the reader chose to keep at the top - and never our own.
     *
     * "Pinned" rather than "favourites" because X is called pin, the row wears
     * INKCELL_ICON_PINNED, and a third word for one fact is the thing the action bar's table
     * exists to stop.
     *
     * Our own node is excluded for the reason it wears no star: a radio can carry a stale
     * `is_favorite` on its own NodeDB entry, and both nav.c's X and the detail's own pin row
     * refuse to toggle it - so `is_favorite` alone put a row under Pinned with no star on it
     * and no press that could clear it. That is the same divergence the Direct arm above had,
     * one field over: this filter is "the rows the list draws a star on", and the list draws no
     * star on us.
     */
    MESH_UI_NODE_FILTER_PINNED,
    MESH_UI_NODE_FILTER_COUNT,
};

/*
 * Whether this node belongs in that filter. NULL is in nothing, including ALL.
 *
 * The handshake is here for one field - which node is ours - and it is a parameter rather than
 * a fact about the summary because that is where every other reader of this question gets it
 * (`hs->my_info.node_num`). A NULL handshake answers as though nothing is ours, which is what a
 * roster published before MyInfo arrived actually looks like.
 */
bool mesh_ui_node_filter_matches(const struct mesh_ui_handshake_state *handshake,
                                 const struct mesh_ui_node_summary *node,
                                 enum mesh_ui_node_filter filter);

/* How many of the published roster the filter keeps. */
uint32_t mesh_ui_node_filter_count(const struct mesh_ui_handshake_state *handshake,
                                   enum mesh_ui_node_filter filter);

/*
 * The Find row's text, and whether a node answers to it.
 *
 * A piece of the long name, the short name or the `!1234abcd` id, with ASCII case folded -
 * what somebody looking for a node remembers of it, typed a character at a time on a d-pad, so
 * a part is as good as the whole. NULL or "" is no query, and matches every node: the Find row
 * narrows what the filter kept, it never stands in for it.
 */
bool mesh_ui_node_query_matches(const struct mesh_ui_node_summary *node, const char *query);

/* The filter and the query together: how many rows the list has under both. The count every
   reader of the list's length asks while a query is set, so the cursor and the rows agree. */
uint32_t mesh_ui_node_query_count(const struct mesh_ui_handshake_state *handshake,
                                  enum mesh_ui_node_filter filter, const char *query);

/* The next filter along, wrapping. What A on the filter row does - the settings enum row's step,
   and the reason the chips need no second key. */
enum mesh_ui_node_filter mesh_ui_node_filter_step(enum mesh_ui_node_filter filter, int delta);

/* The chip's word. */
inkcell_str_id mesh_ui_node_filter_label(enum mesh_ui_node_filter filter);

/*
 * ---- the Nodes list's sort --------------------------------------------------------------------
 *
 * What order the rows the filter kept are drawn in - a second table beside the first, read by
 * the same three files and for the same reason. An order the screen draws and an order the
 * cursor indexes have to be one answer, or the row under the cursor is not the row the press
 * opens.
 *
 * Why a sort as well as a filter, when the note above says sorting cannot answer what the chips
 * answer. Both halves of that stand, because they are different questions. A filter answers
 * "which kind of node", and no ordering will ever answer it; a sort answers "which of them is
 * nearest, or newest, or called what", and no chip will ever answer that - a chip is a
 * membership test, and every one of these is a comparison between two rows.
 *
 * Distance is the one that earns the axis on its own. A busy mesh is a hundred and twenty-eight
 * names and the reader wants the four that are within walking distance; the client holds every
 * one of those fixes and draws them on a map two rows down, and a list had no way of asking. The
 * three chips cannot be taught to: "near me" is not a kind of node, it is an ordering of all of
 * them, and a fourth chip for it would have to pick a radius the client has no business picking.
 *
 * Name is the second. A to Z puts a name you would know on sight at a place you can scroll to,
 * and costs no keyboard - which is what the reader who cannot spell it needs. The Find row is
 * for the one who can type a piece of it: a fragment of the long name, the short name or the id
 * is enough, and the keyboard's caret makes a typo a fix rather than a retype.
 *
 * All of it is a lens on the published roster and not on the mesh. mesh_app_node_rank() cuts the
 * roster to MESH_UI_MAX_HANDSHAKE_NODES before any of this runs, so "nearest" means nearest of
 * what the client carries rather than nearest on the air. That is the same scope the filter has,
 * and it is the reason the cut is a *rank* rather than a sort: the rank decides who is worth
 * ordering, and these decide how to read the ones that were.
 */
enum mesh_ui_node_sort {
    /*
     * The order the app published, which is mesh_app_node_rank(): ourselves, then the nodes we
     * pinned, then our other radios, then whoever we have exchanged messages with, then the rest
     * by last heard.
     *
     * First, and the resting value, because it is the list as it has always been - a reader who
     * never presses this row sees exactly what they saw before it existed. It is also the only
     * member here that is not a question about one field: it is the client's whole opinion of
     * what matters, and it is the opinion the 128-node cut was already made with.
     */
    MESH_UI_NODE_SORT_DEFAULT = 0,
    /*
     * Last heard, newest first - the phone apps' default, and the one thing the ranking above
     * deliberately will not say. A pin floats a quiet node over one that spoke a second ago,
     * which is the whole point of a pin and the whole problem with it when the question is who
     * is awake right now.
     *
     * A node with no `last_heard` sorts to the end rather than to the top: 0 there is "the
     * roster has never heard this one speak", not a timestamp in 1970.
     */
    MESH_UI_NODE_SORT_HEARD,
    /*
     * By name, A to Z, folding ASCII case.
     *
     * The name compared is the one the row *draws first* - the long name, falling back to the
     * short one - and that is worth stating because the row draws both. A list sorted on a
     * column it is not showing is a list that looks unsorted, and the long name is the row's
     * first line, the one the eye runs down. A node that has said neither sorts to the end, for
     * the reason an unheard node does: it has not answered the question, so it cannot be placed
     * by the answer.
     *
     * ASCII case only, which is what strcasecmp folds. Accented and non-Latin names therefore
     * sort by their bytes rather than by the reader's alphabet - a real limitation, honestly a
     * small one on a roster of mostly-ASCII radio names, and the alternative is a collation
     * table this client has no room for.
     */
    MESH_UI_NODE_SORT_NAME,
    /*
     * Nearest first, great-circle, from our own fix to theirs.
     *
     * Two fixes are needed and either can be missing, so most of this sort is the answer to
     * "cannot say": a node we cannot measure keeps its published order, below everything we
     * can. With no fix of our own *nothing* can be measured, and the list is the published order
     * under a row saying Distance - which is honest and looks broken, so the row says the rest
     * of it in its own value column: see MESH_STR_NODES_SORT_NO_FIX.
     */
    MESH_UI_NODE_SORT_DISTANCE,
    /*
     * Fewest hops first: how far away a node is in mesh terms rather than in metres, and the
     * reading that still works when nobody is sharing a position.
     *
     * `has_hops_away` and not `hops_away == 0`, which is the same trap the Direct chip is
     * written against: an unset field is the firmware declining to say, and a sort that read it
     * as zero would put every node nothing is known about at the top of a list of near ones.
     * Those go to the end, in their published order.
     */
    MESH_UI_NODE_SORT_HOPS,
    MESH_UI_NODE_SORT_COUNT,
};

/*
 * The list, once: which of the published roster the filter kept, in the order the sort puts it.
 *
 * A built view rather than a walk per row, and the reason is the distance sort. Every other
 * question here is a field comparison, but a great-circle distance is a haversine - two sines, a
 * square root and an atan2 - and a comparison sort asks its key for every pair it considers. Ask
 * per comparison and a 128-row list spends thousands of them on a frame that draws eight rows;
 * ask once per node and it spends 128. That is the shape src/ui/views/waypoints.c settled on for
 * the same reason, and it is the only reason this is a struct rather than another index-th-element
 * function beside mesh_ui_node_filter_at() - which it replaces, because two ways to turn a row
 * into a node is exactly how the cursor and the list come to disagree.
 *
 * `order` holds indices into handshake->nodes, so a view outlives nothing: it is built from a
 * handshake and used with the same one, in the same frame or the same press.
 */
struct mesh_ui_node_view {
    uint8_t order[MESH_UI_MAX_HANDSHAKE_NODES];
    uint32_t count;
};

/*
 * Fills `out` with the rows this filter and this sort produce. A NULL handshake, and a filter or
 * sort this enum has never held, are all answered rather than refused - `nav.node_filter` and
 * `nav.node_sort` are bytes restored from a file, and the failure a reader can act on is a strip
 * that has come back to its first chip, not a list that is empty for a reason nothing on the
 * frame can say.
 */
void mesh_ui_node_view_build(const struct mesh_ui_handshake_state *handshake,
                             enum mesh_ui_node_filter filter, enum mesh_ui_node_sort sort,
                             struct mesh_ui_node_view *out);
/* The same, keeping only the rows `query` also matches - see mesh_ui_node_query_matches(). */
void mesh_ui_node_view_build_query(const struct mesh_ui_handshake_state *handshake,
                                   enum mesh_ui_node_filter filter, const char *query,
                                   enum mesh_ui_node_sort sort, struct mesh_ui_node_view *out);

/*
 * The `index`-th row of a built view, or NULL past the end.
 *
 * The counterpart of mesh_ui_node_detail_at(), and the only way a row becomes a node while the
 * list is on screen: mesh_ui_nav_node_at_row() and the renderer both come through here, so the
 * node a row drew and the node its press opens cannot be two different nodes.
 */
const struct mesh_ui_node_summary *
mesh_ui_node_view_at(const struct mesh_ui_handshake_state *handshake,
                     const struct mesh_ui_node_view *view, uint32_t index);

/* Where in the view one node is, by its id: the index mesh_ui_node_view_at() would take, or
   `view->count` when the filter left it out or the roster no longer holds it. */
uint32_t mesh_ui_node_view_find(const struct mesh_ui_handshake_state *handshake,
                                const struct mesh_ui_node_view *view, uint32_t node_id);

/* The next sort along, wrapping. What A on the sort row does, the way A on the filter row steps
   the chips above it. */
enum mesh_ui_node_sort mesh_ui_node_sort_step(enum mesh_ui_node_sort sort, int delta);

/* The chip's word. */
inkcell_str_id mesh_ui_node_sort_label(enum mesh_ui_node_sort sort);

/*
 * Whether this sort can say anything about this roster - false only for Distance with no fix of
 * our own to measure from.
 *
 * Its own question rather than something the renderer works out, because the renderer is not
 * allowed to know what a sort needs: the row draws a word and something else answers whether the
 * word is telling the truth. It is the same shape as the map row asking
 * mesh_ui_map_has_markers() before it claims to open anything.
 */
bool mesh_ui_node_sort_available(const struct mesh_ui_handshake_state *handshake,
                                 enum mesh_ui_node_sort sort);

/*
 * A node row's second line: "ALFA · 2 hops · 1.2 km · 87% battery".
 *
 * The first line is the node's name and the right-hand column is how long ago it was heard, so
 * this is everything else a reader scans a roster for, quietest first to loudest last: which
 * four-letter radio it is, how it reaches us, how far away it is, and whether it is about to go
 * quiet. A fact the node has not given is left out rather than drawn as a dash - a line of
 * placeholders is noise on exactly the rows that have least to say.
 *
 * How it reaches us is one fact with several answers, in the order that decides it. "Off radio"
 * wins, because a node the radio's NodeDB no longer holds is one a DM cannot reach whatever the
 * rest says; then hops, then MQTT. A node heard directly says nothing here, because the signal
 * bars in the row's right-hand column already say it - which needs mesh_ui_node_signal_heard(),
 * not `hops_away == 0`, for the reason the Direct chip gives. A figure in decibels is the last
 * resort, for a node whose hops were never reported but whose SNR was.
 *
 * The short name is left off when it is the first line - a node with no long name - and so is
 * everything about the route on our own row, which has none. The distance needs a fix at both
 * ends, carries a "~" when either end was rounded, and is left out when the rounding is as large
 * as the distance itself. Empty is a valid answer; `out` is always terminated.
 */
void mesh_ui_node_row_facts(const struct mesh_ui_handshake_state *handshake,
                            const struct mesh_ui_node_summary *node, bool imperial, char *out,
                            size_t out_len);

#ifdef __cplusplus
}
#endif
