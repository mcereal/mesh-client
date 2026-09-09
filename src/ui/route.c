#include "mesh/ui/route.h"

#include "mesh/core/message.h"
#include "mesh/ui/settings.h"

#include <string.h>

/*
 * How deep the tab's own screen is, before any overlay is stacked on it.
 *
 * Two of the six tabs are one level and say so by having nothing to open; the three that are
 * a hierarchy each answer for their own shape, which is the same shape three times - a list,
 * and one of its rows opened over it. Settings is the only one that goes three deep, and it
 * does it two ways: a module section reached through the Modules list, and a channel reached
 * through the Channels section. Neither can be true at once, so the additions do not need to be
 * exclusive to be correct - but they are written as two separate questions because they are two.
 */
static uint8_t route_screen_depth(const struct mesh_ui_nav *nav) {
    switch (nav->screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return nav->thread_open ? 1U : 0U;
    case MESH_UI_SCREEN_NODES:
        return nav->node_detail_open ? 1U : 0U;
    case MESH_UI_SCREEN_WAYPOINTS:
        return nav->waypoint_detail_open ? 1U : 0U;
    case MESH_UI_SCREEN_SETTINGS: {
        if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
            return 0U;
        }
        uint8_t depth = 1U;
        if (nav->settings_parent != MESH_UI_SETTINGS_NO_SECTION) {
            depth++; /* a module section, reached through the Modules list */
        }
        if (nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
            depth++; /* one channel slot, reached through the Channels section */
        }
        return depth;
    }
    case MESH_UI_SCREEN_DEVICES:
    case MESH_UI_SCREEN_STATUS:
    default:
        return 0U;
    }
}

/*
 * The screen's own place, under whatever is stacked over it.
 *
 * The three subjects a thread can have are told apart here rather than left to collide: the
 * all-traffic transcript names nothing, a channel names its slot, and a direct conversation
 * names the node. Two of those would otherwise both be "channel 0 of nobody".
 */
static void route_screen_place(const struct mesh_ui_nav *nav, struct mesh_ui_route *out) {
    switch (nav->screen) {
    case MESH_UI_SCREEN_MESSAGES:
        if (!nav->thread_open) {
            return;
        }
        out->level = MESH_UI_ROUTE_THREAD;
        if (nav->inbox) {
            return;
        }
        out->subject = nav->target_node;
        if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR) {
            out->slot = (uint8_t)(nav->target_channel + 1U);
        }
        return;
    case MESH_UI_SCREEN_NODES:
        if (nav->node_detail_open) {
            out->level = MESH_UI_ROUTE_NODE;
            out->subject = nav->node_detail_node;
        }
        return;
    case MESH_UI_SCREEN_WAYPOINTS:
        if (nav->waypoint_detail_open) {
            out->level = MESH_UI_ROUTE_WAYPOINT;
            out->subject = nav->waypoint_detail_id;
        }
        return;
    case MESH_UI_SCREEN_SETTINGS:
        if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
            return;
        }
        if (nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
            out->level = MESH_UI_ROUTE_CHANNEL;
            out->slot = nav->settings_channel;
            return;
        }
        out->level = MESH_UI_ROUTE_SECTION;
        out->slot = nav->settings_section;
        return;
    case MESH_UI_SCREEN_DEVICES:
    case MESH_UI_SCREEN_STATUS:
    default:
        return;
    }
}

void mesh_ui_route_of(const struct mesh_ui_nav *nav, struct mesh_ui_route *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (nav == NULL) {
        return;
    }

    out->screen = (uint8_t)nav->screen;
    out->depth = route_screen_depth(nav);
    route_screen_place(nav, out);

    /*
     * The overlays, each of which is a level over what is under it.
     *
     * Every one that is up is counted, not just the topmost, because they genuinely stack: A on
     * the compose sheet's draft row raises the keyboard *over* the sheet and leaves it open, so
     * closing the keyboard lands back on the sheet rather than on the thread. Counting only the
     * top would make that exit look like a move to the same depth.
     *
     * `level`, on the other hand, is the topmost - it says what is being drawn - and the order
     * of the tests below is fb_render_snapshot()'s order for the same reason mesh_ui_actions_
     * for() uses it: describing a screen the user cannot reach is worse than describing none.
     */
    if (nav->compose_open) {
        out->depth++;
        out->level = MESH_UI_ROUTE_COMPOSE;
        out->slot = 0U;
        out->subject = 0U;
    }
    if (nav->reaction_open) {
        out->depth++;
        out->level = MESH_UI_ROUTE_REACTION;
        /* Which message it is about: a second X on a different bubble is a move sideways to
           another place, not a repaint of this one. */
        out->slot = 0U;
        out->subject = nav->reply_to;
    }
    if (nav->keyboard_open) {
        out->depth++;
        out->level = MESH_UI_ROUTE_KEYBOARD;
        /* Which text is being typed. A pairing PIN is its own prompt rather than a field, which
           is why it cannot be told from a message by the field alone - both are NONE. */
        out->slot = nav->keyboard_field;
        out->subject = nav->keyboard_passkey ? 1U : 0U;
    }
    if (nav->picker_open) {
        out->depth++;
        out->level = MESH_UI_ROUTE_PICKER;
        out->slot = nav->picker_follow;
        out->subject = 0U;
    }
    if (nav->confirm_open) {
        out->depth++;
        out->level = MESH_UI_ROUTE_CONFIRM;
        out->slot = nav->confirm_action;
        out->subject = 0U;
    }
}

bool mesh_ui_route_same(const struct mesh_ui_route *a, const struct mesh_ui_route *b) {
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return a->depth == b->depth && a->screen == b->screen && a->level == b->level &&
           a->slot == b->slot && a->subject == b->subject;
}

enum mesh_ui_transition mesh_ui_route_move(const struct mesh_ui_route *from,
                                           const struct mesh_ui_route *to) {
    if (from == NULL || to == NULL || mesh_ui_route_same(from, to)) {
        return MESH_UI_TRANSITION_NONE;
    }

    /*
     * A change of tab is the tab strip's to decide, whatever it did to the depth.
     *
     * L/R work from a nested screen - each tab keeps its own place, so Right off an open node
     * detail lands on the Devices list - and that is a move one tab rightwards that happens to
     * be a level shallower. Reading the depth there would slide the new tab in from the left
     * while the strip above it travelled right, which is the frame contradicting itself.
     *
     * And the strip is a ring: mesh_ui_nav_switch_screen() wraps, so Right off the last tab
     * lands on the first. Comparing the two indices would call that the biggest leftwards move
     * there is, when it is one step right. The distance is therefore measured both ways round
     * and the shorter one wins, which is the same answer for every ordinary step and the right
     * one at both ends. A dead heat - only reachable with an even number of tabs - goes
     * forward, because a tie is not a reason to run the animation backwards.
     */
    if (to->screen != from->screen) {
        const unsigned span = (unsigned)MESH_UI_SCREEN_COUNT;
        const unsigned rightwards = (span + to->screen - from->screen) % span;
        const unsigned leftwards = (span + from->screen - to->screen) % span;
        return rightwards <= leftwards ? MESH_UI_TRANSITION_FORWARD : MESH_UI_TRANSITION_BACK;
    }

    /* Within one tab, the hierarchy decides: in is forward and out is back. */
    if (to->depth != from->depth) {
        return to->depth > from->depth ? MESH_UI_TRANSITION_FORWARD : MESH_UI_TRANSITION_BACK;
    }
    return MESH_UI_TRANSITION_FORWARD;
}
