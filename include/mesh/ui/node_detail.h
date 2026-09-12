#pragma once

/*
 * One node as data: the rows the Nodes tab shows when you open a node, in the same shape the
 * Settings tab uses (label, value already formatted, kind). Backends draw the list; the nav
 * walks it and only ever asks how many rows there are.
 *
 * Everything here comes from the node record the session already holds - the NodeDB sync fills
 * it and NODEINFO/POSITION/TELEMETRY packets keep it current - so opening a node costs no
 * radio traffic and works just as well on a cached, disconnected node list.
 *
 * Rows are built rather than indexed: which of them exist depends on what the node has
 * actually reported, so a builder that emits the list in one pass is the only place the layout
 * lives. The backend builds once per frame and the nav asks for the count.
 */

#include "mesh/ui/history.h"
#include "mesh/ui/icon.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UI_NODE_LABEL_MAX 20U
#define MESH_UI_NODE_VALUE_MAX 48U
/*
 * Every row every node can produce, all at once. rows_next() drops silently past this, so it
 * has to be an upper bound rather than a guess: the arithmetic is 33 action rows (the group's
 * own heading and ten actions, plus a traced route of up to ten stops in each direction with
 * its two headings and its stamp), 11 identity, 7 signal, and then one group per kind of
 * reading - 7 device metrics, 7 position, 9 environment, 5 power, 7 air quality, 5 health, 6
 * host - which comes to 97 for a node that reports everything at the end of a ten-hop trace -
 * plus the two neighbour groups: 12 for the list the node reported (heading, ten out-edges -
 * upstream's own cap - and the stamp) and 12 for the nodes that report hearing it (heading, ten
 * rows and the line saying how many were left out), making 121.
 *
 * Rounded up for headroom, and pinned by node_detail_row_budget in the ui_settings suite so a
 * new group cannot quietly push the last one off the screen.
 */
#define MESH_UI_NODE_ITEMS_MAX 128U

/*
 * How many "Heard by" rows the node detail draws. Upstream's ten-entry cap is on what one node
 * reports, not on how many nodes may report hearing this one - on a dense mesh that is everyone
 * in range - so this is a row budget and the screen says how many it left out rather than
 * quietly answering "how many can hear me" with the wrong number.
 */
#define MESH_UI_NODE_MAX_LISTENERS 10U

enum mesh_ui_node_row_kind {
    MESH_UI_NODE_ROW_INFO = 0, /* label and value */
    MESH_UI_NODE_ROW_HEADING,  /* a group title; no value, not selectable */
    MESH_UI_NODE_ROW_ACTION,   /* A does something; `action` says what */
    /*
     * A reading with an absolute scale, on the same terms as a setting's MESH_UI_SETTING_METER:
     * still a fact with a label and a formatted value, and additionally a number a backend that
     * can draw one may draw.
     *
     * It is a description of the content rather than an instruction to a renderer, which is why
     * the row keeps its `value` text: the CLI backend has no bar and shows a complete fact, and
     * the fb backend shows the same fact with a length beside it.
     *
     * Which readings get one is the whole of the judgement here, and the test is not "is this a
     * number" - most of this screen is numbers. It is whether the figure has *ends the reader
     * does not know*. A battery percentage is meaningless without knowing that 15 is nearly
     * flat; an SNR in decibels is meaningless without knowing the demodulator gives up near
     * -17. A node number, an altitude and a satellite count have no such ends, and a bar under
     * one of them would be inventing a scale to draw against.
     */
    MESH_UI_NODE_ROW_METER,
};

enum mesh_ui_node_action {
    MESH_UI_NODE_ACTION_NONE = 0,
    MESH_UI_NODE_ACTION_MESSAGE,      /* open this node's conversation */
    MESH_UI_NODE_ACTION_FAVORITE,     /* pin or unpin the node in the radio's NodeDB */
    MESH_UI_NODE_ACTION_TRACEROUTE,   /* ask the mesh which way it reaches this node */
    MESH_UI_NODE_ACTION_REQUEST_INFO, /* ask the node to introduce itself */
    /* Ask for a fix or a reading now, rather than at the node's next broadcast - fifteen
       minutes and half an hour at the firmware's defaults. */
    MESH_UI_NODE_ACTION_REQUEST_POSITION,
    MESH_UI_NODE_ACTION_REQUEST_TELEMETRY,
    MESH_UI_NODE_ACTION_IGNORE, /* have the radio drop this node's packets */
    MESH_UI_NODE_ACTION_MUTE,   /* stop this node raising notifications on the radio */
    MESH_UI_NODE_ACTION_REMOVE, /* drop this node from the radio's NodeDB */
    /*
     * Share where this node says it is as a waypoint.
     *
     * Here rather than only on the Waypoints tab because the Brick has no GPS of its own: our
     * radio's fix is often the one thing missing, and a node that has just reported one is the
     * other place a real coordinate can come from. Offered only when the node has a fix - a
     * place with no coordinates is not a place.
     */
    MESH_UI_NODE_ACTION_WAYPOINT,
    /* Open the map looking at this node. Its own action rather than a second way into the
       Nodes list's map row, because the press names a node and the map opens aimed at it. */
    MESH_UI_NODE_ACTION_SHOW_ON_MAP,
};

struct mesh_ui_node_item {
    char label[MESH_UI_NODE_LABEL_MAX];
    char value[MESH_UI_NODE_VALUE_MAX];
    uint8_t kind;   /* enum mesh_ui_node_row_kind */
    uint8_t action; /* enum mesh_ui_node_action */
    /*
     * The symbol in the row's leading slot, or MESH_UI_ICON_NONE.
     *
     * Here rather than in the renderer for the reason settings.c's k_section_icons[] is there
     * and not in fb_screens.c: what a row is *about* is a property of the row, and a backend
     * that decided by switching on `action` would be a second table to keep in step with this
     * one. A row with nothing to say leaves it unset and the slot still holds its width, which
     * is what stops a list whose icons are optional from starting its text in two columns -
     * see FB_LEADING_ICON.
     *
     * A HEADING carries one too, and it is the group's subject rather than a row's: a backend
     * that draws these groups as cards draws it as the card's own icon, which is the cell the
     * eye finds when it is looking for Signal rather than Identity on a screen a hundred and
     * twenty rows long. A flat list leaves that slot empty, and the group states its subject
     * either way - which of the two is happening is the renderer's business, not the builder's.
     */
    uint8_t icon; /* enum mesh_ui_icon */
    /*
     * The ink the row's words take: how much of a statement pressing it makes.
     *
     * A plain fact is MESH_UI_TONE_NORMAL and an ordinary verb is the primary, which is what
     * every row here used to be - so "Message this node" and "Remove from radio" were the same
     * colour and the only difference between them was the reading. The two that cost something
     * name the warning and the error family instead, on the same terms as the dialog that
     * confirms a reboot: a destructive control says so before it is pressed, not after.
     */
    uint8_t tone; /* enum mesh_ui_tone */
    /*
     * ACTION: the row is a boolean the press flips, and `on` is where it stands.
     *
     * The three rows that are one - pinned, muted, ignored - said "Yes" and "No" in the value
     * column, which is a control written down as a word. `value` still holds those words,
     * exactly as a settings TOGGLE row does: a backend with no sprites draws the fact and the
     * fb backend draws the control instead, which is the one choice a backend is for. What is
     * new is that the state is a *field* rather than a string the renderer would have had to
     * parse back, so the knob and the verb the action bar names read the same flag.
     */
    bool toggle;
    bool on;
    /*
     * METER: the reading, the ends it is measured between, and where it changes meaning.
     *
     * Held as whole units of whatever the row is about - percent, decibels - rather than
     * normalised here, because the two ends and the two thresholds are one statement about the
     * reading and normalising would split it: a builder that handed over a fraction would have
     * had to convert the thresholds too, by arithmetic nothing could check against the ends it
     * used. mesh_ui_scale_permille() does it once, where the bar is drawn.
     *
     * `banded` is the NULL a pointer would have carried; a row without one is a plain bar.
     */
    int32_t number;
    struct mesh_ui_scale scale;
    struct mesh_ui_band band;
    bool banded;
    /*
     * METER: what this reading has been doing, or NULL for one nothing has watched.
     *
     * Borrowed from the history the build was handed, which outlives the frame drawn from it.
     * A pointer rather than a copy because a series is two dozen samples and this screen builds
     * a hundred and nineteen rows: the one row that carries a trend should not cost every row
     * that does not.
     *
     * It hangs on a METER row and only on one, and that is the rule rather than an accident of
     * which rows have one so far. A trend is measured on the same `scale` the bar beside it is,
     * so a row with a trend and no ends to draw it between would be a line against nothing.
     */
    const struct mesh_ui_series *trend;
    /*
     * METER: which reading that trend is of, and so which chart A on this row opens.
     *
     * MESH_UI_HISTORY_NONE on every row without one, which is what makes "is this row pressable"
     * the same question as "does this row have a trend" - see mesh_ui_node_detail_trend_at().
     *
     * It is on the row rather than worked out from the row's label or its position because the
     * row is where the chart's *whole* statement already lives: the scale it is drawn against,
     * the band ruled across it, the words naming it and the series itself are four things that
     * have to agree, and they agree by being one row. A renderer that looked up the series by
     * reading and the scale by a switch of its own would be the node detail's opinion about
     * temperature and the chart's, which is the split status.c exists to prevent one tab over.
     */
    uint8_t trend_reading; /* enum mesh_ui_history_reading */
};

/*
 * Fills `out` with the node's rows and returns how many were written (at most `capacity`).
 * `is_self` drops the rows that make no sense for our own node. `now` is the wall clock used
 * to age timestamps; pass 0 to leave ages out, which is what a Brick with no clock wants.
 *
 * `trace` is the client's one traceroute slot and may be NULL. Its rows are emitted only when
 * it is a trace of *this* node, so opening a different node after a trace shows that node's
 * own state rather than somebody else's route.
 *
 * `remove_armed` is the nav's "the next press really does it" state for the remove row, which
 * is the one row here whose consequence the user cannot walk back from - the node leaves the
 * list and takes its own row with it. It only changes what that row's value column says.
 *
 * `roster` is the whole node list and may be NULL, in which case the neighbour rows are left
 * out. Two of this screen's groups need it rather than just this node: a neighbour is a bare
 * node number on the wire and has to be resolved to a name, and "who hears this node" is not
 * reported by anybody - it only exists as the reverse of every *other* node's list.
 *
 * `history` is what the client has watched this node's readings do (include/mesh/ui/history.h)
 * and may be NULL. It adds no rows and removes none - a trend is a third thing said about a
 * reading a row is already making, so it hangs on the row that made it - which is why
 * mesh_ui_node_detail_count() does not take one and why the nav and the renderer cannot come to
 * different totals over it.
 */
uint32_t mesh_ui_node_detail_build(const struct mesh_ui_node_summary *node, bool is_self,
                                   uint32_t now, const struct mesh_ui_traceroute *trace,
                                   bool remove_armed, const struct mesh_ui_handshake_state *roster,
                                   const struct mesh_ui_history *history,
                                   struct mesh_ui_node_item *out, uint32_t capacity);

/*
 * Which reading row `row` of this node's detail charts, or MESH_UI_HISTORY_NONE when that row
 * charts nothing - which is most of them, and every row of a node nothing has been watched on.
 *
 * Three callers ask it and they must not answer it separately: the nav decides whether A opens
 * a chart, the action bar decides whether to name the press, and the renderer decides what to
 * draw. That is status.c's rule about verbs applied to rows - the button under the cursor and
 * the verb in the bar disagreeing is exactly what a second opinion here would produce.
 *
 * It builds the rows to answer, which is what every other question about this screen already
 * costs: the row list is built per frame by the renderer and per press by the nav, because which
 * rows exist depends on what the node has reported. It takes no clock for the same reason
 * mesh_ui_node_detail_count() passes none - the wall time formats ages into values and adds no
 * row, and the nav has been relying on that to agree with the renderer since before this.
 */
enum mesh_ui_history_reading
mesh_ui_node_detail_trend_at(const struct mesh_ui_node_summary *node, bool is_self,
                             const struct mesh_ui_traceroute *trace,
                             const struct mesh_ui_handshake_state *roster,
                             const struct mesh_ui_history *history, uint32_t row);

/*
 * The row charting `reading`, or false when this node has none.
 *
 * The other half of the question above, asked the other way round: that one starts from a row and
 * says which reading it charts, this one starts from a reading and finds its row. Both build, and
 * both exist so that the press, the bar, the clamp and the renderer are reading one answer.
 *
 * Two callers, and the reason they must be the same call is a state the client could otherwise
 * sit in. A reading is an *optional field* of an optional Telemetry variant, so a later report
 * that omits it takes the row away while the history the client already kept stays exactly as
 * full as it was. Asked of the history, the chart stays open over a row that is gone: the
 * renderer falls back to drawing the detail, while the nav, the action bar, the help screen and
 * the key handler all still believe a picture is up - so the reader is looking at a list whose
 * d-pad is swallowed until they press B. Asked of the row, it closes.
 *
 * `out` may be NULL for a caller that only wants the answer, which is the clamp; the renderer
 * takes the copy because the row is where the chart's whole statement lives.
 */
bool mesh_ui_node_detail_trend_row(const struct mesh_ui_node_summary *node, bool is_self,
                                   const struct mesh_ui_traceroute *trace,
                                   const struct mesh_ui_handshake_state *roster,
                                   const struct mesh_ui_history *history,
                                   enum mesh_ui_history_reading reading,
                                   struct mesh_ui_node_item *out);

/* Rows the node would produce. The nav needs nothing else from this module. */
uint32_t mesh_ui_node_detail_count(const struct mesh_ui_node_summary *node, bool is_self,
                                   const struct mesh_ui_traceroute *trace,
                                   const struct mesh_ui_handshake_state *roster);

/*
 * Whether `node`'s SNR is a measurement of *this node's own link*, and so whether it can be
 * drawn rather than merely printed.
 *
 * Three ways it is not, and the reading is a true number about something else in all of them:
 *
 *   - `via_mqtt`: the packet did not cross the air to us at all.
 *   - `hops_away > 0`: the SNR is the last relay's, not this node's.
 *   - `!has_hops_away`: the firmware did not say. Unknown is not zero - older firmware and
 *     replayed NodeDB entries both leave it unset - and treating it as zero is how a node that
 *     may never have been heard directly gets a confident-looking staircase.
 *
 * And one way the reading itself is not there: `snr` of exactly 0.0 is the session layer's own
 * "no measurement" - mesh_session_apply_packet() declines to store a zero for that reason,
 * while a NodeDB entry carrying none assigns one anyway. The two are indistinguishable by the
 * time they reach here, so a bar drawn on that value would put three of four rungs against a
 * node nothing has been heard from. A genuine 0.0 dB link loses its rungs to this and keeps its
 * figure, which is the right way round: the printed number is a fact either way.
 *
 * The distinction is the whole rule this screen and the Nodes list are held to. Printing a
 * number that describes something else is unhelpful; drawing it is a claim.
 */
bool mesh_ui_node_signal_heard(const struct mesh_ui_node_summary *node);

/*
 * The node with that id, or NULL when it is not in the list. The open detail is remembered by
 * id rather than by row because app.c re-ranks the node list on every publish (by last_heard,
 * which changes constantly) - a row index would quietly slide onto a different node while the
 * user was reading one.
 */
const struct mesh_ui_node_summary *
mesh_ui_node_detail_find(const struct mesh_ui_handshake_state *handshake, uint32_t node_id);

/* The node on row `row` of the Nodes list, or NULL past the end. */
const struct mesh_ui_node_summary *
mesh_ui_node_detail_at(const struct mesh_ui_handshake_state *handshake, uint32_t row);

#ifdef __cplusplus
}
#endif
