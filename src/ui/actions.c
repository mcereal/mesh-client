#include "mesh/ui/actions.h"

#include "mesh/ui/help.h"
#include "mesh/ui/history.h"
#include "mesh/ui/input.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/status.h"
#include "mesh/ui/store.h"

#include <string.h>

/*
 * The caps.
 *
 * "A" through "START" are what is printed on the Brick; the two pairs are drawn from the
 * font's arrows (U+2190..U+2193), which is why they are a keycap the eye reads as a direction
 * rather than the words "Up/Down" spending four cells saying it.
 *
 * MESH_UI_BUTTON_QUIT is the one that is not a constant, and it is answered by the input layer
 * rather than here: MESHCLIENT_QUIT_KEYS can move it to a key whose name nobody knows, and the
 * module that parsed that variable is the one that can say so.
 */
static const char *const k_caps[MESH_UI_BUTTON_COUNT] = {
    [MESH_UI_BUTTON_A] = "A",
    [MESH_UI_BUTTON_B] = "B",
    [MESH_UI_BUTTON_X] = "X",
    [MESH_UI_BUTTON_Y] = "Y",
    [MESH_UI_BUTTON_START] = "START",
    [MESH_UI_BUTTON_SELECT] = "SELECT",
    [MESH_UI_BUTTON_SHOULDERS] = "L/R",
    [MESH_UI_BUTTON_UP_DOWN] = "\xE2\x86\x91\xE2\x86\x93",    /* up arrow, down arrow */
    [MESH_UI_BUTTON_LEFT_RIGHT] = "\xE2\x86\x90\xE2\x86\x92", /* left arrow, right arrow */
    [MESH_UI_BUTTON_QUIT] = NULL,
};

const char *mesh_ui_button_cap(enum mesh_ui_button button) {
    if (button == MESH_UI_BUTTON_QUIT) {
        return mesh_ui_input_quit_cap();
    }
    if ((unsigned)button >= (unsigned)MESH_UI_BUTTON_COUNT || k_caps[button] == NULL) {
        return "";
    }
    return k_caps[button];
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

static void actions_nodes(const struct mesh_ui_nav *nav, const struct mesh_ui_snapshot *snapshot,
                          struct mesh_ui_action_bar *bar) {
    if (nav->node_remove_armed) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONFIRM_REMOVE);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
        return;
    }
    if (nav->node_detail_open) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_SELECT);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
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
     * The list, whose first row is the map rather than a node - so X and Y are named for presses
     * that do nothing there. They are named anyway, and deliberately: they are true of every
     * other row on the screen, and a bar that shed two keycaps as the cursor passed over the top
     * row would be describing the row rather than the list. The Status screen's rule is the
     * opposite one because its cards offer genuinely different verbs; here there is one verb per
     * key and one row that happens not to take them.
     */
    bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
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
    const struct mesh_ui_device *row = NULL;
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_DEVICES];
    if (cursor < snapshot->device_count) {
        row = &snapshot->devices[cursor];
    }
    if (mesh_ui_device_connectable(row)) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONNECT);
    }
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_DISCONNECT);
    if (mesh_ui_device_forgettable(row)) {
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
     * About radio: a page of readings, with one verb on it.
     *
     * Nothing in this section is editable - which is the whole of what its name promises - so
     * the edit keys come off, where every other read-only screen already leaves them off and
     * this one did not: the bar was advertising a press that worked on none of its rows.
     *
     * A goes on for the opposite reason to the one that keeps it off a settings section below.
     * There the bar names Left and Right because they are the gesture that works on *every*
     * row, and A would be a sometimes-extra beside them; here there is no universal gesture at
     * all, so the only press the screen has is the only press the bar can name. It is gated on
     * the same condition the row is, because a device with no curl and no wget draws no verb
     * here and a keycap for it would name a button that is not on the frame.
     */
    if (nav->settings_section == MESH_UI_SETTINGS_RADIO) {
        if (snapshot != NULL && snapshot->settings.fw_supported) {
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
 * The chart, which is the one screen in the client with nothing on it to choose.
 *
 * Three keycaps, and the shortest bar there is outside a dialog: B leaves, SELECT explains, the
 * shoulders change tab. There is deliberately no d-pad entry - a chart has no cursor and nothing
 * to pan, and naming "move" here would be the one thing this table exists to prevent, a keycap
 * that does nothing. Quit is left off for the same reason it is on the cards underneath: the
 * status line already ends in it when there is no radio, and this screen only exists while
 * there is one.
 */
static void actions_trend(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *bar) {
    bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
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
        bar_add(out, MESH_UI_BUTTON_START,
                nav->keyboard_field != MESH_UI_FIELD_NONE ? MESH_STR_ACTION_DONE
                                                          : MESH_STR_ACTION_SEND);
        bar_add(out, MESH_UI_BUTTON_B, MESH_STR_ACTION_DELETE);
        bar_add(out, MESH_UI_BUTTON_X, MESH_STR_ACTION_SHIFT);
        bar_add(out, MESH_UI_BUTTON_Y, MESH_STR_ACTION_SPACE);
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
        /* The compose sheet's two presses exactly: every row here sends, and B is the way out -
           plus the one that says what an emoji on somebody's message actually does, which is the
           question this overlay raises and cannot answer with a row of glyphs. */
        bar_add(out, MESH_UI_BUTTON_A, MESH_STR_ACTION_SEND);
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
