#include "mesh/ui/actions.h"

#include "mesh/ui/input.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
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

static void actions_messages(const struct mesh_ui_nav *nav, struct mesh_ui_action_bar *bar) {
    if (!nav->thread_open) {
        if (nav->messages_delete_armed) {
            bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_CONFIRM_DELETE);
            bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
            return;
        }
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_NEW);
        bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_DELETE);
        bar_add_tabs(bar);
        return;
    }

    /* The all-traffic thread is a transcript of everything, not a conversation with anybody, so
       there is nobody for a reply to go to. That is the whole of the difference. */
    if (nav->inbox) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
        bar_add_tabs(bar);
        return;
    }
    bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_REPLY);
    bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_WRITE);
    bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
    bar_add_tabs(bar);
}

static void actions_nodes(const struct mesh_ui_nav *nav, struct mesh_ui_action_bar *bar) {
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
        bar_add_tabs(bar);
        return;
    }
    bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_OPEN);
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_PIN);
    bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_WRITE);
    bar_add_tabs(bar);
}

static void actions_devices(const struct mesh_ui_nav *nav, struct mesh_ui_action_bar *bar) {
    if (nav->devices_forget_armed) {
        bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_CONFIRM_FORGET);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_CANCEL);
        return;
    }
    bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_CONNECT);
    bar_add(bar, MESH_UI_BUTTON_X, MESH_STR_ACTION_DISCONNECT);
    bar_add(bar, MESH_UI_BUTTON_Y, MESH_STR_ACTION_FORGET);
    bar_add_tabs(bar);
}

static void actions_settings(const struct mesh_ui_nav *nav, struct mesh_ui_action_bar *bar) {
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
        bar_add_tabs(bar);
        return;
    }
    /* Rows that are verbs, not values: nothing here is editable and nothing here came from the
       radio, so neither the edit keys nor the refresh mean anything. */
    if (nav->settings_section == MESH_UI_SETTINGS_ABOUT) {
        bar_add(bar, MESH_UI_BUTTON_A, MESH_STR_ACTION_RUN);
        bar_add(bar, MESH_UI_BUTTON_B, MESH_STR_ACTION_BACK);
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
    bar_add_tabs(bar);
}

static void actions_status(const struct mesh_ui_snapshot *snapshot,
                           struct mesh_ui_action_bar *bar) {
    /*
     * Status has no controls of its own, so the bar says the one thing the Brick's own chrome
     * cannot: how to get out. Only while a radio is attached, though - the line under the bar
     * already ends in the quit hint when there is none, and the same instruction twice reads as
     * a rendering fault.
     */
    if (mesh_ui_snapshot_connected_device(snapshot) != NULL) {
        bar_add(bar, MESH_UI_BUTTON_QUIT, MESH_STR_ACTION_QUIT);
    }
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

    switch (nav->screen) {
    case MESH_UI_SCREEN_MESSAGES:
        actions_messages(nav, out);
        break;
    case MESH_UI_SCREEN_NODES:
        actions_nodes(nav, out);
        break;
    case MESH_UI_SCREEN_DEVICES:
        actions_devices(nav, out);
        break;
    case MESH_UI_SCREEN_SETTINGS:
        actions_settings(nav, out);
        break;
    case MESH_UI_SCREEN_STATUS:
    default:
        actions_status(snapshot, out);
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
