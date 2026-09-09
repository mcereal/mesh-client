#ifndef MESH_UI_ROUTE_H
#define MESH_UI_ROUTE_H

/*
 * Where the user is, as one comparable value - and therefore which way they just moved.
 *
 * Everything else in this layer answers a question about the *present* frame: which buttons
 * mean something (actions.h), what the client has to say for itself (chrome.h), which card
 * carries which verb (status.h). A transition is the one thing that is not a fact about a
 * frame at all. Opening a node and backing out of one are the same two frames in the opposite
 * order, so nothing you can read off either frame on its own says which happened.
 *
 * The audit that asked for this (docs/components-roadmap.md, 2.16) expected the answer to be a
 * field on `struct mesh_ui_nav` - "this move was a push" - written by every call site that
 * opens or closes a level, plus a rule about who clears it. It is not, and for the reason the
 * top app bar's back arrow is not a flag either: *a second opinion about the nav is a second
 * opinion that can be wrong*. There are eleven places that open a level and nine that close
 * one, and a new one that forgot to say so would animate the wrong way round - which is not a
 * crash, is not caught by a test that never thought to look, and is exactly the kind of drift
 * a derived answer cannot have.
 *
 * So a route is *derived*: it reads the nav the way a renderer does and says where that nav
 * is. Then the direction is arithmetic on two of them, and the "was" lives in the one place
 * that legitimately has one - the backend, beside the animation table, for the reasons in
 * anim.h. Nothing in the store, the nav or a snapshot remembers how it got here.
 *
 * A route deliberately does *not* carry where a cursor is, what a draft says, or which press
 * has been armed. Those move constantly and change nothing about which place is on screen; a
 * route that included them would restart the animation under the user's thumb every time they
 * scrolled.
 */

#include "mesh/ui/nav.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What kind of place this is.
 *
 * It exists so that two routes can be compared exactly rather than through a hash: `slot` and
 * `subject` are read in the terms of the level that filled them, so a settings section 3 and a
 * channel slot 3 are two different places without either of them needing a distinct numbering.
 * The order is not a hierarchy - `depth` is - it is just a list.
 */
enum mesh_ui_route_level {
    MESH_UI_ROUTE_LIST = 0, /* the tab's own list: conversations, nodes, devices, sections */
    MESH_UI_ROUTE_THREAD,   /* one conversation, or the all-traffic transcript */
    MESH_UI_ROUTE_NODE,     /* one node's detail */
    MESH_UI_ROUTE_WAYPOINT, /* one shared place's detail */
    MESH_UI_ROUTE_SECTION,  /* one settings section, including the Modules and Channels lists */
    MESH_UI_ROUTE_CHANNEL,  /* one channel slot inside the Channels section */
    MESH_UI_ROUTE_COMPOSE,  /* the compose sheet over a thread */
    MESH_UI_ROUTE_PICKER,   /* the send-to picker */
    MESH_UI_ROUTE_KEYBOARD, /* free text: a message, a setting, a pairing PIN */
    MESH_UI_ROUTE_CONFIRM,  /* the confirmation dialog */
    MESH_UI_ROUTE_REACTION, /* the tapback picker over one message */
    MESH_UI_ROUTE_HELP,     /* what this screen is for, over the screen it explains */
    MESH_UI_ROUTE_COUNT
};

/*
 * A place in the navigation model.
 *
 * `depth` is how many levels in it is - 0 is a tab's own list, and every level opened over one
 * adds one - which is what says whether a move went further in or back out *within one tab*.
 * `screen` decides ahead of it whenever the tab changed, because the strip is on the panel
 * already saying which way that went and the body must not contradict it.
 */
struct mesh_ui_route {
    uint8_t depth;
    uint8_t screen; /* enum mesh_ui_screen */
    uint8_t level;  /* enum mesh_ui_route_level */
    /* The channel slot, settings section or keyboard field this level names; 0 when it names
       none. Read in `level`'s terms and meaningless outside them. */
    uint8_t slot;
    /* The node this level is about, 0 when it is about none. */
    uint32_t subject;
};

/* Which way a move between two places went. */
enum mesh_ui_transition {
    MESH_UI_TRANSITION_NONE = 0, /* the same place: nothing moved */
    MESH_UI_TRANSITION_FORWARD,  /* further in, or rightwards along the tabs */
    MESH_UI_TRANSITION_BACK,     /* back out, or leftwards along the tabs */
};

/* Reads the place this nav is showing. Never fails; a NULL nav yields the Messages list, which
   is where mesh_ui_nav_init() leaves one. */
void mesh_ui_route_of(const struct mesh_ui_nav *nav, struct mesh_ui_route *out);

/* Whether two routes are the same place. Exact, field by field - see enum
   mesh_ui_route_level for why there is no hash here. */
bool mesh_ui_route_same(const struct mesh_ui_route *a, const struct mesh_ui_route *b);

/*
 * Which way the move from `from` to `to` went.
 *
 * A change of tab decides first, and by the *shorter way round the strip*, because the strip is
 * a ring that wraps and because L/R work from a nested screen: Right off an open node detail
 * lands on the Devices list, one tab rightwards and a level shallower at the same time. The tab
 * strip is on the panel above the body saying which way that went, so it is what the body has
 * to agree with. It is also the honest answer for the move that changes tab without a press -
 * the node detail's "Message this node", which lands on a thread one tab to the left.
 *
 * Within one tab the hierarchy decides, because in and out is what the four transitions worth
 * animating are (2.16: opening a node, entering a section, raising the keyboard, pressing B).
 *
 * A change with neither to go on - an overlay replaced in place, which is what a pairing prompt
 * standing down onto the keyboard it displaced is - reads as forward, because something new
 * arrived.
 */
enum mesh_ui_transition mesh_ui_route_move(const struct mesh_ui_route *from,
                                           const struct mesh_ui_route *to);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_ROUTE_H */
