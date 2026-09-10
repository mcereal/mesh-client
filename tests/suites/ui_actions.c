#define _POSIX_C_SOURCE 200809L

/*
 * The action bar's contents.
 *
 * What the buttons do here used to be a sentence per screen, and a sentence is only checkable
 * by looking at it. Now that it is a table of (button, verb) pairs it is checkable by a test,
 * and these are the four things worth holding it to:
 *
 *   - the overlays win over the screen underneath them, in the order they stack. A bar
 *     describing a screen the user cannot reach is worse than no bar at all, and the chain that
 *     decides it is written out twice - once here and once in fb_render_snapshot() - so a
 *     screen that grows an overlay has two places to remember.
 *   - an armed destructive action says so. That was the one thing the old sentences got
 *     unmistakably right ("X again to delete this conversation") and the one most easily lost
 *     in a refactor to single verbs.
 *   - nothing overruns MESH_UI_ACTIONS_MAX, because the bar drops from the end and an overrun
 *     is silent.
 *   - every action a table names carries a verb the catalog actually has.
 */

#include "framework/mesh_test.h"

#include "mesh/ui/actions.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/status.h"
#include "mesh/ui/store.h"

#include <stdio.h>
#include <string.h>

/* A snapshot with nothing in it but the nav, which is all the bar reads. */
static void actions_snapshot(struct mesh_ui_snapshot *snapshot) {
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->nav.screen = MESH_UI_SCREEN_MESSAGES;
    snapshot->nav.settings_section = MESH_UI_SETTINGS_NO_SECTION;
    snapshot->nav.settings_channel = MESH_UI_SETTINGS_NO_CHANNEL;
}

/* Whether the bar offers `button`, and with which verb. MESH_STR_NONE when it does not. */
static enum mesh_str_id actions_label_for(const struct mesh_ui_action_bar *bar,
                                          enum mesh_ui_button button) {
    for (size_t i = 0; i < bar->count; ++i) {
        if (bar->items[i].button == button) {
            return bar->items[i].label;
        }
    }
    return MESH_STR_NONE;
}

/* One more row on the Devices tab, with the cursor left on it. */
static struct mesh_ui_device *actions_add_device(struct mesh_ui_snapshot *snapshot,
                                                 const char *identifier,
                                                 enum mesh_ui_device_kind kind) {
    struct mesh_ui_device *device = &snapshot->devices[snapshot->device_count];
    memset(device, 0, sizeof *device);
    snprintf(device->identifier, sizeof device->identifier, "%s", identifier);
    device->kind = (uint8_t)kind;
    device->in_range = true;
    device->paired = true;
    snapshot->nav.cursor[MESH_UI_SCREEN_DEVICES] = (uint32_t)snapshot->device_count;
    snapshot->device_count += 1U;
    return device;
}

MESH_TEST_CASE(actions_screens_offer_their_own_presses, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action_bar bar;

    actions_snapshot(&snapshot);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_OPEN,
                      "A should open the conversation the cursor is on");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_SHOULDERS) != MESH_STR_ACTION_TABS,
                      "the shoulders move between tabs on every screen that is not an overlay");

    /* Both of these are properties of the row, so the tab needs one to offer either. */
    snapshot.nav.screen = MESH_UI_SCREEN_DEVICES;
    actions_add_device(&snapshot, "F4:12:FA:00:0A:22", MESH_UI_DEVICE_BLE);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_CONNECT,
                      "A should connect on the Devices tab");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_Y) != MESH_STR_ACTION_FORGET,
                      "Y should forget a radio on the Devices tab");

    /*
     * Status offers the way out - but only while a radio is attached, because the line under
     * the bar already ends in the quit hint when there is none, and the same instruction twice
     * reads as a rendering fault.
     */
    snapshot.nav.screen = MESH_UI_SCREEN_STATUS;
    snapshot.handshake_valid = false;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_QUIT) != MESH_STR_NONE,
                      "Status should not repeat the quit hint while nothing is connected");
    /* And nothing else: with no radio its cards carry no verbs, so A means nothing here. */
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_NONE,
                      "Status with no radio should not offer a press it cannot answer");

    snapshot.device_count = 1U;
    snapshot.devices[0].connected = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_QUIT) != MESH_STR_ACTION_QUIT,
                      "Status should say how to leave once a radio is attached");
    /*
     * A names the verb the cursor is on rather than one word for the screen, which is the
     * compose sheet's rule and not the settings section's: the cards offer different verbs.
     * With one verb on offer there is nothing to choose between, so no direction keycap.
     */
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_DISCONNECT,
                      "A on the Link card's verb should say disconnect");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_UP_DOWN) != MESH_STR_NONE,
                      "one verb needs no gesture for choosing between verbs");

    /* A radio that has answered the handshake adds the Radio card's refresh, and the bar
       renames A as the cursor moves onto it. */
    snapshot.handshake_valid = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_UP_DOWN) != MESH_STR_ACTION_CHOOSE,
                      "two verbs want a way to choose between them");
    snapshot.nav.status_verb = (uint8_t)MESH_UI_STATUS_VERB_REFRESH;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_REFRESH,
                      "A on the Radio card's verb should say refresh");
    record_success(test_name);
}

MESH_TEST_CASE(actions_overlays_win_over_the_screen, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action_bar bar;

    /* Every overlay raised at once: they are answered in the order fb_render_snapshot() draws
       them, so the confirmation - the innermost - is the one the bar describes. */
    actions_snapshot(&snapshot);
    snapshot.nav.compose_open = true;
    snapshot.nav.keyboard_open = true;
    snapshot.nav.picker_open = true;
    snapshot.nav.confirm_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_CONFIRM,
                      "a confirmation should outrank every other overlay");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_SHOULDERS) != MESH_STR_NONE,
                      "an overlay should not offer the tabs it cannot reach");

    snapshot.nav.confirm_open = false;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_CHOOSE,
                      "the picker should outrank the keyboard under it");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_SHOULDERS) != MESH_STR_ACTION_JUMP,
                      "the shoulders should jump ten rows inside a long picker");

    /* The keyboard's START is the one verb that changes with what is being typed: a message is
       sent, a settings field is finished with. */
    snapshot.nav.picker_open = false;
    snapshot.nav.keyboard_field = MESH_UI_FIELD_NONE;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_START) != MESH_STR_ACTION_SEND,
                      "START should send a message being composed");

    snapshot.nav.keyboard_field = (uint8_t)MESH_UI_FIELD_USER_LONG_NAME;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_START) != MESH_STR_ACTION_DONE,
                      "START should finish a settings field rather than send it");
    record_success(test_name);
}

/*
 * The compose sheet's A does two different things, and the bar has to say which.
 *
 * On the draft row it opens the keyboard; on a canned row it sends that message
 * (mesh_ui_nav_compose in nav.c). The sentence this replaced said "A send / type" and so was
 * right about both at once, which a bar naming one verb per key cannot be - so it names the
 * one the row under the cursor actually offers. Missed once already; hence the case.
 */
MESH_TEST_CASE(actions_compose_names_the_row_under_the_cursor, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action_bar bar;

    actions_snapshot(&snapshot);
    snapshot.nav.compose_open = true;
    snapshot.nav.compose_cursor = MESH_UI_COMPOSE_ROW_DRAFT;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_TYPE,
                      "A opens the keyboard on the draft row, so the bar must not say send");

    snapshot.nav.compose_cursor = MESH_UI_COMPOSE_FIRST_CANNED;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_SEND,
                      "A sends the canned message the cursor is on");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_B) != MESH_STR_ACTION_BACK,
                      "B leaves the compose sheet either way");
    record_success(test_name);
}

MESH_TEST_CASE(actions_arm_before_they_destroy, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action_bar bar;

    actions_snapshot(&snapshot);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_X) != MESH_STR_ACTION_DELETE,
                      "X should offer to delete a conversation");

    snapshot.nav.messages_delete_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_X) != MESH_STR_ACTION_CONFIRM_DELETE,
                      "an armed delete should say that the next press goes through with it");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_B) != MESH_STR_ACTION_CANCEL,
                      "an armed action should offer the way out of it");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_SHOULDERS) != MESH_STR_NONE,
                      "an armed bar should say one thing, not four");

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_NODES;
    snapshot.nav.node_remove_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_CONFIRM_REMOVE,
                      "an armed node removal should say so");

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_DEVICES;
    actions_add_device(&snapshot, "F4:12:FA:00:0A:22", MESH_UI_DEVICE_BLE);
    snapshot.nav.devices_forget_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_Y) != MESH_STR_ACTION_CONFIRM_FORGET,
                      "an armed forget should say so");

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_DEVICE;
    snapshot.nav.settings_discard_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_B) != MESH_STR_ACTION_CONFIRM_DISCARD,
                      "an armed discard should say so");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_Y) != MESH_STR_ACTION_SAVE,
                      "the way to keep the edits should stay on the bar beside the way to lose "
                      "them");
    record_success(test_name);
}

/*
 * The back affordance, which the top app bar's leading slot draws.
 *
 * It is read off the bar rather than decided by a screen renderer, so that the arrow at the top
 * of the panel and the B keycap at the bottom cannot disagree. The cases worth pinning are the
 * three where "B does something" and "B leaves" come apart, because a flag on a screen would
 * have got all three wrong.
 */
MESH_TEST_CASE(actions_back_arrow_follows_the_verb_not_the_key, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action_bar bar;

    MESH_TEST_FAIL_IF(mesh_ui_action_bar_goes_back(NULL), "no bar is not a bar offering a way out");

    /* A tab's own list has nothing behind it. */
    actions_snapshot(&snapshot);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(mesh_ui_action_bar_goes_back(&bar),
                      "the conversation list is a tab's root and has nowhere to go back to");

    /* A thread does. */
    snapshot.nav.thread_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(!mesh_ui_action_bar_goes_back(&bar), "B leaves an open thread");

    /* A settings section does - until it is holding edits, where B is discard. An arrow there
       would be the chrome promising something the key does not do. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = MESH_UI_SETTINGS_DISPLAY;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(!mesh_ui_action_bar_goes_back(&bar), "B leaves an open settings section");
    snapshot.nav.settings_edit_count = 2U;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(mesh_ui_action_bar_goes_back(&bar),
                      "B discards pending edits rather than leaving, so no arrow");

    /* The two overlays where B is offered and is not a way back. */
    actions_snapshot(&snapshot);
    snapshot.nav.picker_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(mesh_ui_action_bar_goes_back(&bar),
                      "B cancels the picker, it does not go back");

    actions_snapshot(&snapshot);
    snapshot.nav.keyboard_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(mesh_ui_action_bar_goes_back(&bar),
                      "B deletes a character on the keyboard, it does not go back");

    record_success(test_name);
}

/*
 * Every state the bar can be in, walked exhaustively rather than by hand.
 *
 * The bar drops actions it cannot fit, and it drops them silently - so a table that overran
 * MESH_UI_ACTIONS_MAX would lose its last entry on the device and nowhere else. The same walk
 * catches a table naming a string id the catalog does not have, which is the other failure
 * that would only show up as a blank on screen.
 */
MESH_TEST_CASE(actions_every_state_is_well_formed, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action_bar bar;

    for (unsigned flags = 0U; flags < 64U; ++flags) {
        for (int screen = 0; screen < MESH_UI_SCREEN_COUNT; ++screen) {
            actions_snapshot(&snapshot);
            snapshot.nav.screen = (enum mesh_ui_screen)screen;
            snapshot.nav.confirm_open = (flags & 1U) != 0U;
            snapshot.nav.picker_open = (flags & 2U) != 0U;
            snapshot.nav.keyboard_open = (flags & 4U) != 0U;
            snapshot.nav.compose_open = (flags & 8U) != 0U;
            snapshot.nav.thread_open = (flags & 16U) != 0U;
            snapshot.nav.node_detail_open = (flags & 16U) != 0U;
            snapshot.nav.settings_edit_count = (flags & 16U) != 0U ? 1U : 0U;
            snapshot.nav.inbox = (flags & 32U) != 0U;
            snapshot.nav.keyboard_passkey = (flags & 32U) != 0U;
            snapshot.nav.pairing_confirm = (flags & 16U) != 0U;

            mesh_ui_actions_for(&snapshot, &bar);
            MESH_TEST_FAIL_IF(bar.count > MESH_UI_ACTIONS_MAX,
                              "a table overran the bar and would lose its last action");
            MESH_TEST_FAIL_IF(bar.count == 0U, "every state should say what its buttons do");
            for (size_t i = 0; i < bar.count; ++i) {
                MESH_TEST_FAIL_IF(bar.items[i].label == MESH_STR_NONE,
                                  "an action should carry a verb");
                MESH_TEST_FAIL_IF(mesh_str(bar.items[i].label)[0] == '\0',
                                  "an action's verb should be in the catalog");
                const char *cap = mesh_ui_button_cap(bar.items[i].button);
                MESH_TEST_FAIL_IF(cap == NULL || cap[0] == '\0',
                                  "every button on the bar should have a keycap to draw");
            }
        }
    }
    record_success(test_name);
}

/* A cap is what is printed on the case, so it is never a catalog id and never empty - including
   for a button outside the enum, which a backend must be able to draw rather than crash on. */
MESH_TEST_CASE(actions_keycaps_are_always_drawable, unit) {
    for (int i = 0; i < MESH_UI_BUTTON_COUNT; ++i) {
        const char *cap = mesh_ui_button_cap((enum mesh_ui_button)i);
        MESH_TEST_FAIL_IF(cap == NULL || cap[0] == '\0', "every button should have a keycap");
    }
    MESH_TEST_FAIL_IF(mesh_ui_button_cap((enum mesh_ui_button)MESH_UI_BUTTON_COUNT) == NULL,
                      "a button outside the enum should still return something to draw");
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_button_cap(MESH_UI_BUTTON_A), "A") != 0,
                      "A's cap is what is printed beside it");
    record_success(test_name);
}

/* A snapshot that is not there is not a screen with default controls. */
MESH_TEST_CASE(actions_no_snapshot_is_an_empty_bar, unit) {
    struct mesh_ui_action_bar bar;
    memset(&bar, 0xAB, sizeof bar);
    mesh_ui_actions_for(NULL, &bar);
    MESH_TEST_FAIL_IF(bar.count != 0U, "a missing snapshot should describe nothing");
    mesh_ui_actions_for(NULL, NULL); /* must not crash */
    record_success(test_name);
}

/*
 * The chart's bar, which is the shortest on any screen outside a dialog.
 *
 * What is checked is mostly what is *absent*: a chart has no cursor and nothing to pan, so a
 * d-pad entry here would be the one thing this table exists to prevent - a keycap that does
 * nothing. And A is absent for the same reason, which is the half that matters: the cards
 * underneath do offer A, and a bar that went on naming their verb over the picture would be
 * telling the reader to press a button whose effect they cannot see.
 */
MESH_TEST_CASE(actions_the_chart_offers_only_the_way_out, unit) {
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_STATUS;
    snapshot.nav.trend_open = true;
    snapshot.handshake_valid = true;

    struct mesh_ui_action_bar bar;
    mesh_ui_actions_for(&snapshot, &bar);

    bool has_back = false;
    for (size_t i = 0; i < bar.count; ++i) {
        MESH_TEST_FAIL_IF(bar.items[i].button == MESH_UI_BUTTON_A, "a chart has nothing to open");
        MESH_TEST_FAIL_IF(bar.items[i].button == MESH_UI_BUTTON_UP_DOWN,
                          "a chart has no cursor to move");
        has_back = has_back || bar.items[i].label == MESH_STR_ACTION_BACK;
    }
    MESH_TEST_FAIL_IF(!has_back, "a chart must say how to leave it");
    MESH_TEST_FAIL_IF(!mesh_ui_action_bar_goes_back(&bar),
                      "the top app bar's arrow is derived from the same table");

    /* And the flag alone is not enough: it outlives a change of tab, so a bar that read it
       without the screen would draw the chart's three keycaps over the Nodes list. */
    snapshot.nav.screen = MESH_UI_SCREEN_NODES;
    mesh_ui_actions_for(&snapshot, &bar);
    bool names_a_node_press = false;
    for (size_t i = 0; i < bar.count; ++i) {
        names_a_node_press = names_a_node_press || bar.items[i].button == MESH_UI_BUTTON_A;
    }
    MESH_TEST_FAIL_IF(!names_a_node_press,
                      "a chart open on another tab must not silence this one's presses");
    record_success(test_name);
}

/*
 * The Devices bar is read off the row under the cursor, because A and Y are properties of the
 * row and always were - the nav's handlers declined on three kinds of row while the bar named
 * both keycaps on all of them. A bootloader is the case that made it worth fixing: "A connect"
 * over a node that speaks no protobuf is the bar promising exactly the thing that cannot happen.
 */
MESH_TEST_CASE(actions_devices_ask_the_row_under_the_cursor, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action_bar bar;

    /* An empty list offers neither: there is no row to act on. X stays, because dropping the
       live link is not about the cursor. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_DEVICES;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_NONE,
                      "an empty device list has nothing for A to connect to");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_Y) != MESH_STR_NONE,
                      "an empty device list has nothing for Y to forget");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_X) != MESH_STR_ACTION_DISCONNECT,
                      "X drops whichever link is up and does not depend on the row");

    /* A USB node in its bootloader: no session to open, and no bond to forget either. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_DEVICES;
    struct mesh_ui_device *boot =
        actions_add_device(&snapshot, "/dev/ttyUSB0", MESH_UI_DEVICE_SERIAL);
    boot->bootloader = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_NONE,
                      "A must not offer to connect to a bootloader");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_Y) != MESH_STR_NONE,
                      "a USB port has no bond for Y to forget");

    /* The same port running firmware is connectable again - and still has nothing to forget. */
    boot->bootloader = false;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_ACTION_CONNECT,
                      "a USB node running firmware is something A can open a link to");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_Y) != MESH_STR_NONE,
                      "a USB port still has no bond to forget");

    /* The row we are already on: Y can still drop the bond, A has nothing left to do. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_DEVICES;
    struct mesh_ui_device *live =
        actions_add_device(&snapshot, "F4:12:FA:00:0A:11", MESH_UI_DEVICE_BLE);
    live->connected = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_A) != MESH_STR_NONE,
                      "A must not offer to connect to the radio already connected");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, MESH_UI_BUTTON_Y) != MESH_STR_ACTION_FORGET,
                      "a bonded radio can still be forgotten while it is the one we are on");

    record_success(test_name);
}
