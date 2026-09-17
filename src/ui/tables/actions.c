#include "mesh/ui/actions.h"

#include "mesh/ui/devices.h"
#include "mesh/ui/help.h"
#include "mesh/ui/history.h"
#include "mesh/ui/input.h"
#include "mesh/ui/input_profile.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/status.h"
#include "mesh/ui/store.h"
#include "mesh/ui/trend.h"

#include <string.h>

/*
 * The caps.
 *
 * The table itself is not here, and that is the point. What is printed beside a button and
 * which evdev code that button reports are one fact about one piece of plastic, so they are
 * stated in one row of one table - src/ui/input/input_profile.c - rather than in two files that a
 * port has to remember to correct together. This module asks; it does not hold an opinion.
 *
 * MESH_UI_BUTTON_QUIT is the exception, for the reason it always was: MESHCLIENT_QUIT_KEYS can
 * move it to a key whose name nobody knows, so the module that parsed that variable is the one
 * that can say what to draw on it.
 */
const char *mesh_ui_button_cap(enum mesh_ui_button button) {
    if (button == MESH_UI_BUTTON_QUIT) {
        return mesh_ui_input_quit_cap();
    }
    return mesh_ui_input_profile_cap(mesh_ui_input_profile_from_env(), button);
}

/*
 * Building a bar.
 *
 * Every table below is written out in full rather than composed from a common tail, even
 * though "L/R tabs" ends most of them. A composed tail would be one line shorter and would
 * hide the thing the tables exist to make visible: what a given state offers, all of it, in
 * one place a reader can check against the nav that handles those presses.
 */
static void bar_add(struct mesh_ui_action_bar *bar, enum mesh_ui_button button,
                    enum mesh_str_id label) {
    if (bar->count >= MESH_UI_ACTIONS_MAX) {
        return;
    }
    bar->items[bar->count].button = button;
    bar->items[bar->count].label = label;
    ++bar->count;
}

/* The press that moves between tabs, which is true on every screen that is not an overlay. */
static void bar_add_tabs(struct mesh_ui_action_bar *bar) {
    bar_add(bar, MESH_UI_BUTTON_SHOULDERS, MESH_STR_ACTION_TABS);
}

/* Declared ahead of the per-screen builders and defined below them, beside the paragraph that
   explains what it refuses to do. Every builder here ends up calling it. */
static void bar_add_help(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *bar);

static void actions_messages(const struct mesh_ui_nav *nav, const struct mesh_ui_snapshot *snapshot,
                             struct mesh_ui_action_bar *bar) {
    if (!nav->thread_open) {
        if (nav->messages_delete_armed) {
            bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_CONFIRM_DELETE);
            bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
            return;
        }
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_NEW);
        bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_DELETE);
        /*
         * The mute, named for the row the cursor is on rather than for the key.
         *
         * A verb that reads the cursor is the Status screen's shape rather than the map's: the
         * keycap never appears or vanishes as the user scrolls - which is the flicker the map's
         * comment refuses - it only ever says which way this row would go. "Mute" on a muted
         * conversation would be the bar naming the state instead of the press.
         *
         * Absent on the two rows that are not conversations, because there the press does
         * nothing and a keycap that does nothing is the one thing this table exists to prevent.
         *
         * The view costs a snapshot-sized copy on the stack, which is worth naming because this
         * file is otherwise arithmetic over a few nav fields. It buys the only honest answer:
         * which row the cursor is on is derived from the message log and the channel table, and
         * the alternative - a field on the nav saying what kind of row this is - is the second
         * opinion about the nav that the map's selection and the app bar's back arrow both
         * refuse. It is built on one screen, once a frame, and the renderer for that screen
         * builds the same view a moment later.
         */
        {
            struct mesh_ui_conversation conversation;
            struct mesh_ui_store view;
            mesh_ui_store_view(snapshot, &view);
            if (mesh_ui_nav_conversation_at(&view, nav->cursor[MESH_UI_SCREEN_MESSAGES],
                                            &conversation) &&
                (conversation.kind == MESH_UI_CONVERSATION_CHANNEL ||
                 conversation.kind == MESH_UI_CONVERSATION_DIRECT)) {
                bar_add(bar, MESH_UI_BUTTON_START,
                        conversation.muted ? MESH_STR_ACTION_UNMUTE : MESH_STR_ACTION_MUTE);
            }
        }
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }

    /* The all-traffic thread is a transcript of everything, not a conversation with anybody, so
       there is nobody for a reply to go to. That is the whole of the difference. */
    if (nav->inbox) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    /* Three verbs about three different things, which is why they are three keys: A answers
       the bubble under the cursor, X puts an emoji on it, and Y writes to the conversation. */
    bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_REPLY);
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_REACT);
    bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_WRITE);
    bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
    /*
     * And one more that is only sometimes there, named for the bubble rather than for the key:
     * a message the mesh came back on can go out again, and one that arrived has nothing to
     * retry. Absent on every other row, because a keycap that does nothing is the thing this
     * table exists to prevent - the conversation list's mute above is the same shape.
     *
     * The nav is asked rather than the ack re-read here, so the press and the word naming it
     * come from one answer; on any row where this is absent, START goes on standing in for A.
     */
    if (mesh_ui_nav_resendable(nav, mesh_ui_snapshot_message_view(snapshot)) != NULL) {
        bar_add(bar, MESH_UI_BUTTON_START, MESH_STR_ACTION_RESEND);
    }
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

/*
 * The map.
 *
 * The one bar in this client that names the d-pad as something other than a cursor, and it says
 * so first: "pan" is what the four directions do here, and a reader arriving from any other
 * screen has every reason to expect them to move a selection. A is named unconditionally even
 * though it does nothing when the crosshair is on empty grid - unlike the Status screen's verbs,
 * which change with the cursor, this one is always the same verb about whatever is aimed at, and
 * a keycap that appeared and vanished as the reader panned would be the bar flickering rather
 * than informing.
 *
 * L/R still walks the tabs, which is the whole reason the d-pad could be spent: the shoulders
 * and the directions are the same press on every other screen, and this is the one place they
 * part company.
 */
static void actions_map(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *bar) {
    bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
    bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_ZOOM_IN);
    bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_ZOOM_OUT);
    bar_add(bar, MESH_UI_BUTTON_START, MESH_STR_ACTION_FIT);
    /* Late, so it is among the first to go on a narrow panel - the four presses above are the
       ones that leave the screen or change what is on it, and this one names a gesture a reader
       discovers by trying it. */
    bar_add(bar, MESH_UI_BUTTON_UP_DOWN, MESH_STR_ACTION_PAN);
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

/*
 * What A does on the row the cursor is on in the open node detail, or NONE.
 *
 * It asks node_detail.c rather than deciding, which is the same seam the Status arm below uses
 * for its verbs: the press and the word naming it come from one table, so the button the nav
 * runs and the keycap this draws cannot name two different things.
 */
static enum mesh_ui_node_press mesh_ui_actions_node_press(const struct mesh_ui_snapshot *snapshot) {
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node =
        mesh_ui_node_detail_find(hs, snapshot->nav.node_detail_node);
    if (node == NULL) {
        return MESH_UI_NODE_PRESS_NONE;
    }
    const bool is_self = hs->has_my_info && node->node_id == hs->my_info.node_num;
    /*
     * The same record the screen was drawn from, which on this screen is the same question as
     * which rows exist: a node's own measured route adds its two groups *above* the identity and
     * the readings, so a bar that read the one trace slot here would count a different list from
     * the one the cursor is standing in and name the verb belonging to some row further down.
     */
    return mesh_ui_node_detail_press_at(
        node, is_self, mesh_ui_snapshot_traceroute_view(snapshot, node->node_id), hs,
        &snapshot->history, snapshot->nav.cursor[MESH_UI_SCREEN_NODES]);
}

/* The chart's bar, defined below beside the Status arm that first needed it - a node's chart is
   the same screen, so it calls that rather than restating it. */
static void actions_trend(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *bar);

static void actions_nodes(const struct mesh_ui_nav *nav, const struct mesh_ui_snapshot *snapshot,
                          struct mesh_ui_action_bar *bar) {
    if (nav->node_remove_armed) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONFIRM_REMOVE);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
        return;
    }
    if (nav->node_detail_open) {
        /*
         * A chart of one of this node's readings, over the detail - and the Status tab's chart
         * bar, called rather than copied. The two screens are one picture with different readings
         * on it, so the presses are the same four: B leaves, Left and Right walk the span picker,
         * SELECT explains and the shoulders change tab. Naming X or Y here would be naming the
         * detail's presses over a screen that does not have them.
         */
        if (nav->node_trend != MESH_UI_HISTORY_NONE) {
            actions_trend(snapshot, bar);
            return;
        }
        /*
         * A names what it runs on the row the cursor is on - and on most rows of this screen it
         * runs nothing, so on most rows it is not named.
         *
         * This is the Status screen's rule rather than the node list's, and the difference is
         * worth stating because both are in this file. The list names X and Y over its map row
         * where they do nothing, because they are true of every *other* row and a bar that shed
         * keycaps as the cursor moved would be describing the row rather than the screen. Here
         * they are not true of every other row and never were: the screen is ten verbs and a
         * hundred facts, and mesh_ui_nav_confirm() has always returned false on the facts. The
         * bar said "A select" over all of them anyway, which is the keycap-that-does-nothing
         * this whole table exists to prevent, offered on two rows in three of the longest screen
         * in the client.
         */
        switch (mesh_ui_actions_node_press(snapshot)) {
        case MESH_UI_NODE_PRESS_TREND:
            bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_TREND);
            break;
        case MESH_UI_NODE_PRESS_SELECT:
            bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_SELECT);
            break;
        case MESH_UI_NODE_PRESS_NONE:
            break;
        }
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        /*
         * The d-pad's other axis, which walks the groups a card at a time.
         *
         * Ahead of X and Y because it is the press this screen is *for* - the rule this table's
         * ordering states - and because it is the one press here a reader has no other way of
         * discovering: X and Y duplicate the two rows at the top of the screen, while nothing
         * on the frame says Left and Right stopped being the tab switch.
         *
         * Named unconditionally, which is honest rather than lazy: Identity and Signal are
         * emitted for every node including our own, so this screen has never had fewer than two
         * groups and the press has never had nowhere to go.
         */
        bar_add(bar, MESH_UI_BUTTON_LEFT_RIGHT, MESH_STR_ACTION_GROUPS);
        bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_PIN);
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_WRITE);
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    /* The map, under any detail opened from it and over the list it was opened from - the same
       order fb_render_snapshot() draws them in, for the reason this file always follows it:
       describing a screen the reader cannot see is describing presses that will not arrive. */
    if (nav->map_open) {
        actions_map(snapshot, bar);
        return;
    }
    /*
     * The list, whose first three rows are the filter, the sort and the map rather than nodes -
     * so X and Y are named for presses that do nothing there. They are named anyway, and
     * deliberately: they are true of every other row on the screen, and a bar that shed two
     * keycaps as the cursor passed over the top rows would be describing the row rather than the
     * list. The Status screen's rule is the opposite one because its cards offer genuinely
     * different verbs; here there is one verb per key and three rows that happen not to take two
     * of them.
     *
     * The two control rows are the exception, and it is the Waypoints list's exception rather
     * than a new one: the top two rows genuinely offer a different press from the rest of the
     * list, and naming "open" over a row that filters would be the bar describing something
     * else. That is the line between the two rules. A keycap whose verb changes is named per
     * row; a keycap that simply has nothing to do on one row keeps the list's word for it.
     *
     * What changes on those rows is the *keycap* as well as the word, which is the point of it.
     * The d-pad is what edits them - the Settings tab's press, and the one the pencil in each
     * row's gutter is already pointing at - so the bar names the d-pad, exactly as the Settings
     * bar does. A still steps forward and is deliberately not named: a bar that offered both
     * would spend a slot on the second way of doing a thing it has already said how to do, on
     * the one screen here whose bar is already five keycaps long.
     *
     * The sort row takes its own word rather than borrowing "filter": the two rows look alike
     * and sit one above the other, so the bar is the only thing on the frame that says which of
     * them the press is about to move.
     */
    const uint32_t nodes_cursor = nav->cursor[MESH_UI_SCREEN_NODES];
    if (nodes_cursor == MESH_UI_NODES_FILTER_ROW || nodes_cursor == MESH_UI_NODES_SORT_ROW) {
        bar_add(bar, MESH_UI_BUTTON_LEFT_RIGHT,
                nodes_cursor == MESH_UI_NODES_FILTER_ROW ? MESH_STR_ACTION_FILTER
                                                         : MESH_STR_ACTION_SORT);
    } else {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
    }
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_PIN);
    bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_WRITE);
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

/*
 * The Waypoints tab.
 *
 * Two levels, the Nodes tab's shape - except that the list's A does two different things and
 * the bar says so: on a place it opens, and on the last row it starts a new one. Naming the
 * verb the row under the cursor offers is the Status screen's rule, and it is here for the same
 * reason: a bar that said "open" over the row that makes a place would be describing a press
 * that does something else.
 */
static void actions_waypoints(const struct mesh_ui_nav *nav,
                              const struct mesh_ui_snapshot *snapshot,
                              struct mesh_ui_action_bar *bar) {
    if (nav->waypoint_detail_open) {
        if (nav->waypoint_delete_armed) {
            bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONFIRM_DELETE);
            bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
            return;
        }
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_SELECT);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    const uint32_t places = snapshot->waypoints.count > MESH_UI_MAX_WAYPOINTS
                                ? MESH_UI_MAX_WAYPOINTS
                                : snapshot->waypoints.count;
    bar_add(bar, MESH_UI_BUTTON_A,
            nav->cursor[MESH_UI_SCREEN_WAYPOINTS] >= places ? MESH_STR_ACTION_NEW
                                                            : MESH_STR_ACTION_OPEN);
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

static void actions_devices(const struct mesh_ui_nav *nav, const struct mesh_ui_snapshot *snapshot,
                            struct mesh_ui_action_bar *bar) {
    if (nav->devices_forget_armed) {
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_CONFIRM_FORGET);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
        return;
    }
    /*
     * A and Y are properties of the row under the cursor, and the nav has always known it:
     * both handlers declined on rows the bar went on naming anyway. A bootloader is what made
     * that visible - "A connect" over a node that speaks no protobuf is the bar promising the
     * one thing this whole change exists to stop - but the row already connected and the USB
     * port with no bond were the same mistake, quieter.
     *
     * X is deliberately not asked here: its handler drops whichever link is up regardless of
     * the cursor, so it is not about the row.
     */
    struct mesh_ui_devices_row row;
    if (!mesh_ui_devices_row(snapshot->devices, snapshot->device_count, snapshot->network_host,
                             nav->cursor[MESH_UI_SCREEN_DEVICES], &row)) {
        row = (struct mesh_ui_devices_row){
            .type = (uint8_t)MESH_UI_DEVICES_ROW_DEVICE, .device = NULL, .host = ""};
    }
    if (row.type == (uint8_t)MESH_UI_DEVICES_ROW_NETWORK) {
        /*
         * The network row's two presses, and the reason they are not the row above's.
         *
         * With an address written down A connects to it and Y opens the keyboard on it; with
         * none there is only the keyboard, and it is what A does - so Y is left off rather
         * than named as a second way to the same screen, which is the duplicate keycap this
         * table exists as much to prevent as the one that does nothing.
         */
        if (row.host[0] != '\0') {
            bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONNECT);
            bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_DISCONNECT);
            bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_ADDRESS);
        } else {
            bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_ADDRESS);
            bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_DISCONNECT);
        }
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    if (mesh_ui_device_connectable(row.device)) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONNECT);
    }
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_DISCONNECT);
    if (mesh_ui_device_forgettable(row.device)) {
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_FORGET);
    }
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

/*
 * The help press, offered only where there is something to explain.
 *
 * It asks mesh_ui_help_offered() - the same call the press itself makes - rather than testing
 * the nav here, because the two answers have to be the same answer. A bar naming SELECT over a
 * screen that will not open one is the keycap-that-does-nothing this file refuses everywhere
 * else, and the *reverse* is no better: a press that works with no keycap saying so is a feature
 * nobody finds. Both were true of the two branches below before this was one call.
 *
 * Late in each bar it appears on: order is priority and the bar drops from the end, so on a
 * narrow panel the presses that edit and leave the screen survive and the one that explains it
 * is the first to go. It still sits ahead of "L/R tabs", which is true on every screen in the
 * client and therefore the least worth the room.
 */
static void bar_add_help(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *bar) {
    if (mesh_ui_help_offered(&snapshot->settings,
                             snapshot->handshake_valid ? &snapshot->handshake : NULL,
                             &snapshot->nav)) {
        bar_add(bar, MESH_UI_BUTTON_SELECT, MESH_STR_ACTION_HELP);
    }
}

static void actions_settings(const struct mesh_ui_nav *nav, const struct mesh_ui_snapshot *snapshot,
                             struct mesh_ui_action_bar *bar) {
    /*
     * The two code sheets, first because they are the deepest things this tab opens.
     *
     * One press and no more: there is nothing on it to move between, and a keycap for a press
     * that does nothing is the one thing this table exists to prevent. The tab keys are left off
     * for the same reason help leaves them off - walking sideways out of a code somebody is
     * scanning is not a move anyone means to make.
     */
    if (nav->share_open || nav->contact_open) {
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add_help(snapshot, bar);
        return;
    }
    if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
        bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_REFRESH);
        bar_add_tabs(bar);
        return;
    }
    if (nav->settings_discard_armed) {
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CONFIRM_DISCARD);
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_SAVE);
        return;
    }
    if (nav->settings_edit_count > 0U) {
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_SAVE);
        bar_add(bar, MESH_UI_BUTTON_LEFT_RIGHT, MESH_STR_ACTION_EDIT);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_DISCARD);
        /* An edit in hand does not make the setting need less explaining - if anything it is the
           likelier moment to want it - so this branch offers the same press the pristine one
           does. It was the branch that proved the bar and the handler had to share a predicate:
           SELECT worked here from the first keystroke, with nothing on the bar saying so. */
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    /* Two lists rather than sections: nothing on either is editable, so offering the edit keys
       would be advertising a press that does nothing. */
    if ((nav->settings_section == MESH_UI_SETTINGS_CHANNELS &&
         nav->settings_channel == MESH_UI_SETTINGS_NO_CHANNEL) ||
        nav->settings_section == MESH_UI_SETTINGS_MODULES) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_REFRESH);
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    /* Rows that are verbs, not values: nothing here is editable and nothing here came from the
       radio, so neither the edit keys nor the refresh mean anything. */
    if (nav->settings_section == MESH_UI_SETTINGS_ABOUT) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_RUN);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    /*
     * A section with nothing to step: About radio, Radio actions, Network.
     *
     * Left and Right are the gesture a settings section is *for*, and a section with no row in
     * the field table has no row they work on - so the keycap comes off, which is the rule this
     * file applies everywhere else. It was written as an arm per section before there were
     * three of them, and the two that had one disagreed: About radio dropped the edit keys and
     * Radio actions, whose every row says "press A" in its own value column, kept them and
     * never named A.
     *
     * Both halves are now asked rather than listed. Whether anything steps is a fact about the
     * field table; whether anything is a verb is a fact about the rows as built, which is what
     * makes it the same answer as the row itself - About radio's install press appears only
     * once a check has found something, and a device with no curl draws no verb here at all.
     * That was already the condition this arm tested by hand, spelled as `fw_supported`.
     *
     * A is named here and left off an editable section below for the same reason in both
     * places: the bar names the gesture that works on every row, and on a section of values
     * that is Left and Right, with A a sometimes-extra beside them.
     */
    const enum mesh_ui_settings_section section =
        (enum mesh_ui_settings_section)nav->settings_section;
    /*
     * A section this firmware was built without: no rows, and no press that can make any.
     *
     * The screen behind it says so in a sentence, and the bar has to agree - X is the answer to
     * a section that has *not arrived yet*, and offering it here invites the one refresh that
     * cannot work. The same goes for the edit keys on a section that has fields in the table: a
     * build with no Bluetooth has a Bluetooth field table and no Bluetooth to point it at.
     *
     * Only for EXCLUDED, deliberately. A section still waiting keeps its bar, because X is
     * exactly the press for it and the rows it names are a reply away - a bar that changed
     * shape as a fetch landed would be chrome moving under a reader for no decision they made.
     */
    if (snapshot != NULL &&
        mesh_ui_settings_section_availability(
            &snapshot->settings, snapshot->handshake_valid ? &snapshot->handshake : NULL,
            section) == MESH_UI_SETTINGS_SECTION_EXCLUDED) {
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    if (!mesh_ui_settings_section_has_fields(section)) {
        if (snapshot != NULL &&
            mesh_ui_settings_section_has_verbs(
                &snapshot->settings, snapshot->handshake_valid ? &snapshot->handshake : NULL,
                section, nav->settings_channel)) {
            bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_RUN);
        }
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_REFRESH);
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    /*
     * A section of settings. Left and Right are the press this screen is for - they step the
     * value on the row in place - so they lead, and A is left off deliberately even though it
     * opens the picker on the rows that have one: the bar names the gesture that works on every
     * row here, and a keycap that only sometimes does anything is worse than one fewer.
     */
    bar_add(bar, MESH_UI_BUTTON_LEFT_RIGHT, MESH_STR_ACTION_EDIT);
    bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_REFRESH);
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

/*
 * The chart, which has one thing on it to choose and no cursor to choose it with.
 *
 * Four keycaps, and the d-pad entry is the change: Left and Right walk the span picker over the
 * plot, which is the only control on the screen. It is named here for the rule that got it named
 * nowhere before - a keycap that does something must be in this table, exactly as a keycap that
 * does nothing must not - and it says "span" rather than "move", because what those two presses
 * move is the picture's own horizontal rather than a cursor.
 *
 * The shoulders are still the tabs and still named, which is what separates this from the map:
 * there the d-pad is taken to pan and the same pair of gestures do two things on one screen. Quit
 * is left off for the reason it is on the cards underneath: the status line already ends in it
 * when there is no radio, and this screen only exists while there is one.
 *
 * Both charts read this one function. There was never a second copy and there must not be: the
 * airtime chart and a node's are one screen drawn twice, and a bar that named the span press on
 * one of them would be describing a difference the two do not have.
 */
/*
 * Whether the open chart's readings are more than the panel is showing at once.
 *
 * Asked of the row the chart was opened from rather than of the history, which is the clamp's
 * rule and the renderer's: the row is where the chart's whole statement lives, so the readings
 * counted here are the readings being listed. `page_rows` of 0 is a backend that has not said
 * what its body holds, and answers no - a bar must not name a gesture on a guess.
 */
static bool mesh_ui_actions_trend_scrolls(const struct mesh_ui_snapshot *snapshot) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (snapshot->page_rows == 0U) {
        return false;
    }
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, nav->node_detail_node);
    const bool is_self = hs->has_my_info && node != NULL && node->node_id == hs->my_info.node_num;
    struct mesh_ui_node_item row;
    memset(&row, 0, sizeof row);
    if (!mesh_ui_node_detail_trend_row(
            node, is_self,
            mesh_ui_snapshot_traceroute_view(snapshot, node != NULL ? node->node_id : 0U), hs,
            &snapshot->history, (enum mesh_ui_history_reading)nav->node_trend, &row)) {
        return false;
    }
    return mesh_ui_trend_readings(row.trend, nav->trend_span) > snapshot->page_rows;
}

static void actions_trend(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *bar) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
    bar_add(bar, MESH_UI_BUTTON_LEFT_RIGHT, MESH_STR_ACTION_SPAN);
    /*
     * And the two presses only a node's chart has.
     *
     * The one difference the two screens do have, which is why it is a branch here rather than a
     * second copy of this function: the airtime chart's readings are six hours at one a minute
     * and are binned into columns because reading by reading is the wrong grain for them, so
     * there is no list behind it to offer. See the readings section of mesh/ui/trend.h.
     *
     * Y names the face it would turn to rather than the one that is up - a keycap hint says what
     * the press does. Up and Down are named only once there is more than a screenful, on the same
     * terms as the Status card's "choose": a gesture for moving through a set that fits on the
     * panel is a keycap that does nothing.
     */
    if (nav->node_trend == MESH_UI_HISTORY_NONE) {
        bar_add_help(snapshot, bar);
        bar_add_tabs(bar);
        return;
    }
    bar_add(bar, MESH_UI_BUTTON_Y,
            nav->trend_table ? MESH_STR_ACTION_CHART : MESH_STR_ACTION_READINGS);
    if (nav->trend_table && mesh_ui_actions_trend_scrolls(snapshot)) {
        bar_add(bar, MESH_UI_BUTTON_UP_DOWN, MESH_STR_ACTION_SCROLL);
    }
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

static void actions_status(const struct mesh_ui_snapshot *snapshot,
                           struct mesh_ui_action_bar *bar) {
    const bool connected = mesh_ui_snapshot_connected_device(snapshot) != NULL;

    /*
     * The verb the cursor is on, named rather than described.
     *
     * This is the compose sheet's rule rather than the settings section's: the cards offer
     * different verbs, so A does not mean one thing here the way "edit" does on a section of
     * fields. The bar names what *this* button runs, which is the same label the button itself
     * is drawing - one table, read twice, which is what stops the two disagreeing.
     */
    struct mesh_ui_status_actions actions;
    mesh_ui_status_actions(&actions, connected, snapshot->handshake_valid,
                           mesh_ui_history_has_airtime(&snapshot->history));
    const struct mesh_ui_status_action *chosen = mesh_ui_status_find(
        &actions, mesh_ui_status_verb_resolve(&actions, snapshot->nav.status_verb));
    if (chosen != NULL) {
        bar_add(bar, MESH_UI_BUTTON_A, chosen->label);
    }
    /* Only once there is somewhere to move to. A screen offering one verb needs no gesture for
       choosing between verbs, and a keycap that does nothing is worse than one fewer. */
    if (actions.count > 1U) {
        bar_add(bar, MESH_UI_BUTTON_UP_DOWN, MESH_STR_ACTION_CHOOSE);
    }
    /*
     * And the one thing the Brick's own chrome cannot say: how to get out. Only while a radio
     * is attached - the line under the bar already ends in the quit hint when there is none,
     * and the same instruction twice reads as a rendering fault.
     */
    if (connected) {
        bar_add(bar, MESH_UI_BUTTON_QUIT, MESH_STR_ACTION_QUIT);
    }
    bar_add_help(snapshot, bar);
    bar_add_tabs(bar);
}

void mesh_ui_actions_for(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (snapshot == NULL) {
        return;
    }

    const struct mesh_ui_nav *nav = &snapshot->nav;

    /* The overlays, in the order fb_render_snapshot() stacks them. A bar describing the screen
       underneath one is a bar for presses that will not arrive. */
    if (nav->help_open) {
        /*
         * Two presses and no third. There is nothing on this screen to choose: every row is a
         * paragraph, so the cursor scrolls and B leaves, and the tab keys are left off because
         * walking sideways out of an explanation of the screen behind it is not a move anyone
         * means to make.
         */
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add(out, MESH_UI_BUTTON_UP_DOWN, MESH_STR_ACTION_SCROLL);
        return;
    }
    /*
     * The verification sheet, ahead of the confirm for the reason nav.c takes it first.
     *
     * "answer" rather than "confirm", and it is not a synonym here. A confirm asks the user to
     * agree to something this client is about to do; this asks them what they can *see* on
     * somebody else's screen, and a keycap reading "confirm" over that question would be the
     * bar suggesting there is a right answer to press.
     */
    if (nav->verify_open) {
        bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_ANSWER);
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add(out, MESH_UI_BUTTON_UP_DOWN, MESH_STR_ACTION_CHOOSE);
        return;
    }
    if (nav->confirm_open) {
        bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONFIRM);
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
        bar_add(out, MESH_UI_BUTTON_UP_DOWN, MESH_STR_ACTION_CHOOSE);
        return;
    }
    if (nav->picker_open) {
        bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_CHOOSE);
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
        bar_add(out, MESH_UI_BUTTON_SHOULDERS, MESH_STR_ACTION_JUMP);
        bar_add(out, MESH_UI_BUTTON_UP_DOWN, MESH_STR_ACTION_MOVE);
        return;
    }
    if (nav->keyboard_open) {
        if (nav->keyboard_verify) {
            /* Four digits and nothing else: no Send, because the number does not go anywhere
               near the mesh, and "done" is the same word the grid's own key carries. */
            bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_TYPE);
            bar_add(out, MESH_UI_BUTTON_START, MESH_STR_ACTION_DONE);
            bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
            return;
        }
        if (nav->keyboard_passkey) {
            /* The numeric-comparison case answers a question the radio asked; the other one is
               a passkey being typed, and it has the digits and a cancel. */
            if (nav->pairing_confirm) {
                bar_add(out, MESH_UI_BUTTON_START, MESH_STR_ACTION_CONFIRM);
                bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
                return;
            }
            bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_TYPE);
            bar_add(out, MESH_UI_BUTTON_START, MESH_STR_ACTION_PAIR);
            bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
            return;
        }
        bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_TYPE);
        /* "send" only when something goes to a person, which is mesh_ui_kb_action_label()'s
           rule for the keycap on the grid - and it has to be the same rule, or the bar and the
           key one row above it name the same press two ways. It was the field alone, so the
           waypoint keyboard's bar said "send" over a grid whose own key said "done", and a
           network address would have joined it - and the two link keyboards did join it, saying
           "send" over a link that goes to a radio setting rather than to anybody. */
        bar_add(out, MESH_UI_BUTTON_START,
                (nav->keyboard_field != MESH_UI_FIELD_NONE || nav->keyboard_waypoint ||
                 nav->keyboard_network || nav->keyboard_verify || nav->keyboard_channel_url ||
                 nav->keyboard_contact_url)
                    ? MESH_STR_ACTION_DONE
                    : MESH_STR_ACTION_SEND);
        /*
         * X deletes and B leaves, which is the arrangement every pad-driven keyboard uses and
         * the reverse of what this one did. The bar naming them is the whole point of the swap:
         * a keyboard whose backspace is the button that goes back everywhere else is one people
         * stumble over on every draft, not once.
         */
        bar_add(out, MESH_UI_BUTTON_X, MESH_STR_ACTION_DELETE);
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add(out, MESH_UI_BUTTON_Y, MESH_STR_ACTION_SPACE);
        /* Last, and in this order, because the bar drops from the end: the shoulders reach the
           panel the character is on, which is no use without the shift that is one of them. */
        bar_add(out, MESH_UI_BUTTON_TRIGGERS, MESH_STR_ACTION_SHIFT);
        bar_add(out, MESH_UI_BUTTON_SHOULDERS, MESH_STR_ACTION_KEYS);
        return;
    }
    if (nav->compose_open) {
        /*
         * A sends the canned message the cursor is on - except on the draft row, where it opens
         * the keyboard instead (mesh_ui_nav_compose in nav.c). The sentence this replaced said
         * "A send / type" for exactly that reason; a bar names one verb per key, so it has to
         * name the one *this row* offers rather than the commoner of the two.
         */
        bar_add(out, MESH_UI_BUTTON_A,
                nav->compose_cursor == MESH_UI_COMPOSE_ROW_DRAFT ? MESH_STR_ACTION_TYPE
                                                                 : MESH_STR_ACTION_SEND);
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        return;
    }
    if (nav->reaction_open) {
        /*
         * The compose sheet's two presses, plus the one that says what an emoji on somebody's
         * message actually does - the question this overlay raises and cannot answer with a row
         * of glyphs.
         *
         * A is named for the row the cursor is on rather than for the key, the way the
         * conversation list's mute is: every emoji row sends and the last row deletes, and a
         * bar that said "send" over the delete would be naming the commoner press instead of
         * the one in front of the reader. The armed state takes over the whole bar, exactly as
         * an armed conversation delete does, so the only two presses offered are the one that
         * finishes it and the one that calls it off.
         */
        if (nav->message_delete_armed) {
            bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONFIRM_DELETE);
            bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
            return;
        }
        bar_add(out, MESH_UI_BUTTON_A,
                mesh_ui_nav_reaction_row_is_delete(nav->reaction_cursor) ? MESH_STR_ACTION_DELETE
                                                                         : MESH_STR_ACTION_SEND);
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add_help(snapshot, out);
        return;
    }

    switch (nav->screen) {
    case MESH_UI_SCREEN_MESSAGES:
        actions_messages(nav, snapshot, out);
        break;
    case MESH_UI_SCREEN_NODES:
        actions_nodes(nav, snapshot, out);
        break;
    case MESH_UI_SCREEN_WAYPOINTS:
        actions_waypoints(nav, snapshot, out);
        break;
    case MESH_UI_SCREEN_DEVICES:
        actions_devices(nav, snapshot, out);
        break;
    case MESH_UI_SCREEN_SETTINGS:
        actions_settings(nav, snapshot, out);
        break;
    case MESH_UI_SCREEN_STATUS:
    default:
        /* The chart over the cards, on the same terms the map is drawn over the node list: the
           screen is checked as well as the flag, because the flag outlives a change of tab. */
        if (nav->trend_open) {
            actions_trend(snapshot, out);
        } else {
            actions_status(snapshot, out);
        }
        break;
    }
}

bool mesh_ui_action_bar_goes_back(const struct mesh_ui_action_bar *bar) {
    if (bar == NULL) {
        return false;
    }
    /* The verb rather than the key, because B is not always the way out - it discards a
       section's pending edits, deletes a character on the keyboard and cancels the picker, and
       none of those three is a screen leaving. Matching MESH_STR_ACTION_BACK is matching what
       the bar is already telling the user. */
    for (size_t i = 0; i < bar->count; ++i) {
        if (bar->items[i].label == MESH_STR_ACTION_BACK) {
            return true;
        }
    }
    return false;
}
