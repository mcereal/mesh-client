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
 *   - nothing overruns INKCELL_ACTIONS_MAX, because the bar drops from the end and an overrun
 *     is silent.
 *   - every action a table names carries a verb the catalog actually has.
 */

#include "framework/mesh_test.h"

#include "mesh/core/message.h"
/* For enum mesh_traceroute_state, which the UI's traceroute carries as a byte. */
#include "mesh/core/session.h"
#include "mesh/ui/actions.h"
#include "mesh/ui/commands.h"
#include "mesh/ui/history.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
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

/* Whether the bar offers `button`, and with which verb. INKCELL_STR_NONE when it does not. */
static inkcell_str_id actions_label_for(const struct inkcell_action_bar *bar,
                                        enum inkcell_button button) {
    for (size_t i = 0; i < bar->count; ++i) {
        if (bar->items[i].button == button) {
            return bar->items[i].label;
        }
    }
    return INKCELL_STR_NONE;
}

/* The semantic command bound to a compatibility button, or NONE when it is absent. */
static enum mesh_ui_command_id command_for_button(const struct mesh_ui_command_set *commands,
                                                  enum inkcell_button button) {
    const struct mesh_ui_command *command = mesh_ui_commands_find_button(commands, button);
    return command != NULL ? command->id : MESH_UI_COMMAND_NONE;
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
    snapshot->nav.cursor[MESH_UI_SCREEN_RADIO] = (uint32_t)snapshot->device_count;
    snapshot->device_count += 1U;
    return device;
}

MESH_TEST_CASE(commands_are_the_source_of_the_legacy_action_bar, unit) {
    struct mesh_ui_snapshot snapshot;
    actions_snapshot(&snapshot);

    struct mesh_ui_command_set commands;
    mesh_ui_commands_for(&snapshot, &commands);
    const struct mesh_ui_command *open = mesh_ui_commands_find(&commands, MESH_UI_COMMAND_OPEN);
    const struct mesh_ui_command *delete_command =
        mesh_ui_commands_find_button(&commands, INKCELL_BUTTON_X);
    MESH_TEST_FAIL_IF(open == NULL || open->button != INKCELL_BUTTON_A,
                      "the conversation list should offer semantic OPEN on the Brick's A");
    MESH_TEST_FAIL_IF(delete_command == NULL || delete_command->id != MESH_UI_COMMAND_DELETE,
                      "the conversation list's X binding should identify DELETE");

    struct inkcell_action_bar bar;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(bar.count != commands.count,
                      "the legacy bar should be a projection of the command set");
    for (size_t i = 0U; i < bar.count; ++i) {
        MESH_TEST_FAIL_IF(commands.items[i].id == MESH_UI_COMMAND_NONE,
                          "every advertised action should have a semantic command id");
        MESH_TEST_FAIL_IF(bar.items[i].button != commands.items[i].button ||
                              bar.items[i].label != commands.items[i].label,
                          "the legacy bar should preserve command order, binding and label");
    }

    /* Route precedence is shared by dispatch and command discovery: VERIFY is above CONFIRM. */
    snapshot.nav.confirm_open = true;
    snapshot.nav.verify_open = true;
    mesh_ui_commands_for(&snapshot, &commands);
    MESH_TEST_FAIL_IF(mesh_ui_commands_find(&commands, MESH_UI_COMMAND_ANSWER) == NULL,
                      "the top verification context should offer ANSWER");
    MESH_TEST_FAIL_IF(mesh_ui_commands_find(&commands, MESH_UI_COMMAND_CONFIRM) != NULL,
                      "the confirmation underneath verification must not leak a command");

    /* Dynamic rows declare their semantic identity directly rather than recovering it from the
       translated label after the table is built. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_NODES;
    snapshot.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    mesh_ui_commands_for(&snapshot, &commands);
    MESH_TEST_FAIL_IF(command_for_button(&commands, INKCELL_BUTTON_LEFT_RIGHT) !=
                          MESH_UI_COMMAND_FILTER,
                      "the Nodes filter row should declare FILTER");
    snapshot.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_SORT_ROW;
    mesh_ui_commands_for(&snapshot, &commands);
    MESH_TEST_FAIL_IF(command_for_button(&commands, INKCELL_BUTTON_LEFT_RIGHT) !=
                          MESH_UI_COMMAND_SORT,
                      "the Nodes sort row should declare SORT");

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_NODES;
    snapshot.nav.waypoints_open = true;
    mesh_ui_commands_for(&snapshot, &commands);
    MESH_TEST_FAIL_IF(command_for_button(&commands, INKCELL_BUTTON_A) != MESH_UI_COMMAND_NEW,
                      "the empty Waypoints row should declare NEW");
    snapshot.waypoints.count = 1U;
    mesh_ui_commands_for(&snapshot, &commands);
    MESH_TEST_FAIL_IF(command_for_button(&commands, INKCELL_BUTTON_A) != MESH_UI_COMMAND_OPEN,
                      "an existing waypoint row should declare OPEN");

    record_success(test_name);
}

MESH_TEST_CASE(actions_screens_offer_their_own_presses, unit) {
    struct mesh_ui_snapshot snapshot;
    struct inkcell_action_bar bar;

    actions_snapshot(&snapshot);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_OPEN,
                      "A should open the conversation the cursor is on");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_SHOULDERS) != MESH_STR_ACTION_TABS,
                      "the shoulders move between tabs on every screen that is not an overlay");

    /* Both of these are properties of the row, so the tab needs one to offer either. */
    snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot.nav.devices_open = true;
    actions_add_device(&snapshot, "F4:12:FA:00:0A:22", MESH_UI_DEVICE_BLE);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_CONNECT,
                      "A should connect on the device list");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != MESH_STR_ACTION_FORGET,
                      "Y should forget a radio on the device list");

    /*
     * Status offers the way out - but only while a radio is attached, because the line under
     * the bar already ends in the quit hint when there is none, and the same instruction twice
     * reads as a rendering fault.
     */
    snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot.nav.devices_open = false;
    snapshot.handshake_valid = false;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_QUIT) != INKCELL_STR_NONE,
                      "Status should not repeat the quit hint while nothing is connected");
    /* And A is the one verb the cards carry with no radio: the way to the device list. */
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_DEVICES,
                      "Status with no radio should offer the device list and nothing else");

    snapshot.device_count = 1U;
    snapshot.devices[0].connected = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_QUIT) != MESH_STR_ACTION_QUIT,
                      "Status should say how to leave once a radio is attached");
    /*
     * A names the verb the cursor is on rather than one word for the screen, which is the
     * compose sheet's rule and not the settings section's: the cards offer different verbs.
     * The Link card carries two now, so the bar offers a way to choose between them.
     */
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_DEVICES,
                      "a fresh cursor stands on the Link card's device list");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_UP_DOWN) != MESH_STR_ACTION_CHOOSE,
                      "two verbs want a way to choose between them");
    snapshot.nav.status_verb = (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_DISCONNECT,
                      "A on the Link card's other verb should say disconnect");

    /* With no radio there is only the one verb, and nothing to choose between. */
    snapshot.devices[0].connected = false;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_UP_DOWN) != INKCELL_STR_NONE,
                      "one verb needs no gesture for choosing between verbs");
    snapshot.devices[0].connected = true;

    /* A radio that has answered the handshake adds the Radio card's refresh, and the bar
       renames A as the cursor moves onto it. */
    snapshot.handshake_valid = true;
    mesh_ui_actions_for(&snapshot, &bar);
    snapshot.nav.status_verb = (uint8_t)MESH_UI_STATUS_VERB_REFRESH;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_REFRESH,
                      "A on the Radio card's verb should say refresh");
    record_success(test_name);
}

/*
 * The retry keycap, which is a property of the bubble and not of the screen.
 *
 * Both halves matter. A thread whose cursor is on a message that arrived must not name START,
 * because START goes on standing in for A there and a keycap that does nothing is what this
 * table exists to prevent - and the bar must not run past INKCELL_ACTIONS_MAX when it does
 * appear, because a thread already names six presses without it and the bar drops from the end.
 */
MESH_TEST_CASE(actions_resend_names_only_a_failed_bubble, unit) {
    struct mesh_ui_snapshot snapshot;
    struct inkcell_action_bar bar;

    actions_snapshot(&snapshot);
    snapshot.nav.thread_open = true;
    snapshot.nav.target_node = 0x3000U;
    snapshot.messages.count = 1U;
    snapshot.messages.entries[0].packet_id = 12U;
    snapshot.messages.entries[0].peer = 0x3000U;
    snapshot.messages.entries[0].direction = MESH_MESSAGE_INBOUND;

    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_REPLY,
                      "A answers the bubble under the cursor in a thread");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_START) != INKCELL_STR_NONE,
                      "a message that arrived has nothing to send again");

    /* Our own, undelivered. */
    snapshot.messages.entries[0].direction = MESH_MESSAGE_OUTBOUND;
    snapshot.messages.entries[0].ack = MESH_MESSAGE_ACK_FAILED;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_START) != MESH_STR_ACTION_RESEND,
                      "START should name the retry on a failed bubble");
    MESH_TEST_FAIL_IF(bar.count > INKCELL_ACTIONS_MAX,
                      "the thread's bar should still fit with the retry on it");

    /* A retry already raised on this press: the keycap goes with it, so a held START never
       names a verb it will refuse. */
    snapshot.nav.resend_spent = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_START) != INKCELL_STR_NONE,
                      "a spent retry should not be named on the bar");
    snapshot.nav.resend_spent = false;

    /* One this client sent and the mesh confirmed: back to nothing to do. */
    snapshot.messages.entries[0].ack = MESH_MESSAGE_ACK_DELIVERED;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_START) != INKCELL_STR_NONE,
                      "a message that was acknowledged has nothing to send again");

    /* And all traffic offers it on no row, the way it offers no reply and no tapback. */
    snapshot.messages.entries[0].ack = MESH_MESSAGE_ACK_FAILED;
    snapshot.nav.inbox = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_START) != INKCELL_STR_NONE,
                      "all traffic should name no retry");
    record_success(test_name);
}

MESH_TEST_CASE(actions_overlays_win_over_the_screen, unit) {
    struct mesh_ui_snapshot snapshot;
    struct inkcell_action_bar bar;

    /* Every overlay raised at once: they are answered in the order fb_render_snapshot() draws
       them, so the confirmation - the innermost - is the one the bar describes. */
    actions_snapshot(&snapshot);
    snapshot.nav.compose_open = true;
    snapshot.nav.keyboard_open = true;
    snapshot.nav.picker_open = true;
    snapshot.nav.confirm_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_CONFIRM,
                      "a confirmation should outrank every other overlay");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_SHOULDERS) != INKCELL_STR_NONE,
                      "an overlay should not offer the tabs it cannot reach");

    snapshot.nav.confirm_open = false;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_CHOOSE,
                      "the picker should outrank the keyboard under it");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_SHOULDERS) != MESH_STR_ACTION_JUMP,
                      "the shoulders should jump ten rows inside a long picker");

    /* The keyboard's START is the one verb that changes with what is being typed: a message is
       sent, a settings field is finished with. */
    snapshot.nav.picker_open = false;
    snapshot.nav.keyboard_field = MESH_UI_FIELD_NONE;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_START) != MESH_STR_ACTION_SEND,
                      "START should send a message being composed");

    snapshot.nav.keyboard_field = (uint8_t)MESH_UI_FIELD_USER_LONG_NAME;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_START) != MESH_STR_ACTION_DONE,
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
    struct inkcell_action_bar bar;

    actions_snapshot(&snapshot);
    snapshot.nav.compose_open = true;
    snapshot.nav.compose_cursor = MESH_UI_COMPOSE_ROW_DRAFT;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_TYPE,
                      "A opens the keyboard on the draft row, so the bar must not say send");

    snapshot.nav.compose_cursor = MESH_UI_COMPOSE_FIRST_CANNED;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_SEND,
                      "A sends the canned message the cursor is on");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_B) != MESH_STR_ACTION_BACK,
                      "B leaves the compose sheet either way");
    record_success(test_name);
}

MESH_TEST_CASE(actions_arm_before_they_destroy, unit) {
    struct mesh_ui_snapshot snapshot;
    struct inkcell_action_bar bar;

    actions_snapshot(&snapshot);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_X) != MESH_STR_ACTION_DELETE,
                      "X should offer to delete a conversation");

    snapshot.nav.messages_delete_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_X) != MESH_STR_ACTION_CONFIRM_DELETE,
                      "an armed delete should say that the next press goes through with it");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_B) != MESH_STR_ACTION_CANCEL,
                      "an armed action should offer the way out of it");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_SHOULDERS) != INKCELL_STR_NONE,
                      "an armed bar should say one thing, not four");

    /* On the sheet of verbs, because that is the only screen the remove row is on - the detail
       under it carries one action and it is the row that opens this. A bar asked about an arming
       that could not be true there would be describing a press nobody can make. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_NODES;
    snapshot.nav.node_detail_open = true;
    snapshot.nav.node_actions_open = true;
    snapshot.nav.node_remove_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_CONFIRM_REMOVE,
                      "an armed node removal should say so");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_B) != MESH_STR_ACTION_CANCEL,
                      "an armed action should offer the way out of it");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_SHOULDERS) != INKCELL_STR_NONE,
                      "an armed bar should say one thing, not four");

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot.nav.devices_open = true;
    actions_add_device(&snapshot, "F4:12:FA:00:0A:22", MESH_UI_DEVICE_BLE);
    snapshot.nav.devices_forget_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != MESH_STR_ACTION_CONFIRM_FORGET,
                      "an armed forget should say so");

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_DEVICE;
    snapshot.nav.settings_discard_armed = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_B) != MESH_STR_ACTION_CONFIRM_DISCARD,
                      "an armed discard should say so");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != MESH_STR_ACTION_SAVE,
                      "the way to keep the edits should stay on the bar beside the way to lose "
                      "them");
    record_success(test_name);
}

/*
 * A settings section with no rows, and the two different reasons it can have none.
 *
 * The bar is derived from the section rather than listed per section id, which is what lets it
 * disagree with the screen in exactly one case: a firmware built without a module has a field
 * table for it and no data, so the static half of the derivation still offered Left/Right over
 * a screen reading "This radio's firmware was built without it", and X over a refresh that
 * could never land. A section merely waiting keeps both, because both are about to mean
 * something.
 */
MESH_TEST_CASE(actions_excluded_section_offers_only_the_way_out, unit) {
    struct mesh_ui_snapshot snapshot;
    struct inkcell_action_bar bar;

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = MESH_UI_SETTINGS_BLUETOOTH;
    snapshot.settings.loaded = true;

    /* Waiting: the radio has not sent it, and X is the press that asks again. */
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_X) != MESH_STR_ACTION_REFRESH ||
                          actions_label_for(&bar, INKCELL_BUTTON_LEFT_RIGHT) !=
                              MESH_STR_ACTION_EDIT,
                      "a section that has not arrived yet keeps the presses that fill it");

    /* Excluded: neither press can produce a row, so neither is named. */
    snapshot.settings.has_metadata = true;
    snapshot.settings.excluded_modules =
        mesh_ui_settings_section_excluded_bit(MESH_UI_SETTINGS_BLUETOOTH);
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_X) != INKCELL_STR_NONE ||
                          actions_label_for(&bar, INKCELL_BUTTON_LEFT_RIGHT) != INKCELL_STR_NONE ||
                          actions_label_for(&bar, INKCELL_BUTTON_A) != INKCELL_STR_NONE,
                      "an excluded section should advertise no press that needs rows");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_B) != MESH_STR_ACTION_BACK,
                      "the way out is the one press that still works");

    /* And a radio that sends the section anyway has its bar back: the mask is not the last
       word, the rows are. */
    snapshot.settings.has_bluetooth = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_LEFT_RIGHT) != MESH_STR_ACTION_EDIT,
                      "a section the radio sent is editable whatever the mask says");
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
    struct inkcell_action_bar bar;

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

    /* The overlay where B is offered and is not a way back. */
    actions_snapshot(&snapshot);
    snapshot.nav.picker_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(mesh_ui_action_bar_goes_back(&bar),
                      "B cancels the picker, it does not go back");

    /* The keyboard is the other way round now, and this is the assertion that turned over when
       it changed: B was a backspace there and is a way out, so the arrow follows it. That is
       the whole point of deriving the arrow from the verb rather than from the key - the bar
       changed and the chrome came with it, with nothing to remember. */
    actions_snapshot(&snapshot);
    snapshot.nav.keyboard_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(!mesh_ui_action_bar_goes_back(&bar), "B leaves the keyboard");

    /* Except on the two prompts a radio raised, where leaving is cancelling something that is
       waiting on an answer - and the bar says "cancel" rather than "back". */
    snapshot.nav.keyboard_passkey = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(mesh_ui_action_bar_goes_back(&bar),
                      "B abandons the bond on the passkey prompt, it does not go back");

    record_success(test_name);
}

/*
 * The compact footer keeps every verb and drops only what the chrome already says: the tabs, and
 * B where the heading's arrow was derived from it. Where B is anything else - discarding edits
 * here - the arrow is not drawn and the keycap is the only thing saying what B does, so it stays.
 */
static bool actions_bar_has(const struct inkcell_action_bar *bar, inkcell_str_id label) {
    for (size_t i = 0; i < bar->count; ++i) {
        if (bar->items[i].label == label) {
            return true;
        }
    }
    return false;
}

MESH_TEST_CASE(actions_compact_drops_only_what_the_chrome_says, unit) {
    struct mesh_ui_snapshot snapshot;
    struct inkcell_action_bar bar;

    actions_snapshot(&snapshot);
    snapshot.nav.thread_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    const size_t full = bar.count;
    mesh_ui_actions_compact(&bar, mesh_ui_action_bar_goes_back(&bar));
    MESH_TEST_FAIL_IF(actions_bar_has(&bar, MESH_STR_ACTION_TABS), "the tab strip is the tabs");
    MESH_TEST_FAIL_IF(actions_bar_has(&bar, MESH_STR_ACTION_BACK), "the arrow is the way back");
    MESH_TEST_FAIL_IF(bar.count != full - 2U, "nothing but those two should go");
    MESH_TEST_FAIL_IF(!actions_bar_has(&bar, MESH_STR_ACTION_REPLY) ||
                          !actions_bar_has(&bar, MESH_STR_ACTION_REACT) ||
                          !actions_bar_has(&bar, MESH_STR_ACTION_WRITE),
                      "every verb about the thread should stay");
    MESH_TEST_FAIL_IF(bar.items[0].label != MESH_STR_ACTION_REPLY,
                      "the screen's first verb should still lead");

    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = MESH_UI_SETTINGS_DISPLAY;
    snapshot.nav.settings_edit_count = 2U;
    mesh_ui_actions_for(&snapshot, &bar);
    const bool b_before = actions_bar_has(&bar, MESH_STR_ACTION_DISCARD);
    mesh_ui_actions_compact(&bar, mesh_ui_action_bar_goes_back(&bar));
    MESH_TEST_FAIL_IF(!b_before || !actions_bar_has(&bar, MESH_STR_ACTION_DISCARD),
                      "B as discard has no arrow to stand in for it and should stay");

    mesh_ui_actions_compact(NULL, true);
    record_success(test_name);
}

/*
 * Every state the bar can be in, walked exhaustively rather than by hand.
 *
 * The bar drops actions it cannot fit, and it drops them silently - so a table that overran
 * INKCELL_ACTIONS_MAX would lose its last entry on the device and nowhere else. The same walk
 * catches a table naming a string id the catalog does not have, which is the other failure
 * that would only show up as a blank on screen.
 */
MESH_TEST_CASE(actions_every_state_is_well_formed, unit) {
    struct mesh_ui_snapshot snapshot;
    struct inkcell_action_bar bar;

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
            MESH_TEST_FAIL_IF(bar.count > INKCELL_ACTIONS_MAX,
                              "a table overran the bar and would lose its last action");
            MESH_TEST_FAIL_IF(bar.count == 0U, "every state should say what its buttons do");
            for (size_t i = 0; i < bar.count; ++i) {
                MESH_TEST_FAIL_IF(bar.items[i].label == INKCELL_STR_NONE,
                                  "an action should carry a verb");
                MESH_TEST_FAIL_IF(inkcell_str(bar.items[i].label)[0] == '\0',
                                  "an action's verb should be in the catalog");
                const char *cap = inkcell_button_cap(bar.items[i].button);
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
    for (int i = 0; i < INKCELL_BUTTON_COUNT; ++i) {
        const char *cap = inkcell_button_cap((enum inkcell_button)i);
        MESH_TEST_FAIL_IF(cap == NULL || cap[0] == '\0', "every button should have a keycap");
    }
    MESH_TEST_FAIL_IF(inkcell_button_cap((enum inkcell_button)INKCELL_BUTTON_COUNT) == NULL,
                      "a button outside the enum should still return something to draw");
    MESH_TEST_FAIL_IF(strcmp(inkcell_button_cap(INKCELL_BUTTON_A), "A") != 0,
                      "A's cap is what is printed beside it");
    record_success(test_name);
}

/* A snapshot that is not there is not a screen with default controls. */
MESH_TEST_CASE(actions_no_snapshot_is_an_empty_bar, unit) {
    struct inkcell_action_bar bar;
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
    snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot.nav.devices_open = false;
    snapshot.nav.trend_open = true;
    snapshot.handshake_valid = true;

    struct inkcell_action_bar bar;
    mesh_ui_actions_for(&snapshot, &bar);

    bool has_back = false;
    for (size_t i = 0; i < bar.count; ++i) {
        MESH_TEST_FAIL_IF(bar.items[i].button == INKCELL_BUTTON_A, "a chart has nothing to open");
        MESH_TEST_FAIL_IF(bar.items[i].button == INKCELL_BUTTON_UP_DOWN,
                          "a chart has no cursor to move");
        has_back = has_back || bar.items[i].label == MESH_STR_ACTION_BACK;
    }
    MESH_TEST_FAIL_IF(!has_back, "a chart must say how to leave it");
    MESH_TEST_FAIL_IF(!mesh_ui_action_bar_goes_back(&bar),
                      "the top app bar's arrow is derived from the same table");

    /* And the flag alone is not enough: it outlives a change of tab, so a bar that read it
       without the screen would draw the chart's three keycaps over the Nodes list.
       On a node row rather than on row 0, because the list's first two rows are the filter and
       the sort and those name the d-pad instead of A - so a cursor left at zero would be asking
       this about the one part of the list where A is deliberately not named. */
    snapshot.nav.screen = MESH_UI_SCREEN_NODES;
    snapshot.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_LEAD_ROWS;
    mesh_ui_actions_for(&snapshot, &bar);
    bool names_a_node_press = false;
    for (size_t i = 0; i < bar.count; ++i) {
        names_a_node_press = names_a_node_press || bar.items[i].button == INKCELL_BUTTON_A;
    }
    MESH_TEST_FAIL_IF(!names_a_node_press,
                      "a chart open on another tab must not silence this one's presses");
    record_success(test_name);
}

/*
 * The bar reads the same route the screen does.
 *
 * A node's measured route is two groups drawn *above* its identity and its readings, so which
 * rows exist at which index depends on it - and the route a screen draws is the one for the node
 * being looked at rather than whatever the one trace slot happens to hold. A bar that asked the
 * slot instead counted a shorter list, so the cursor standing on a chartable reading was scored
 * against some row eleven places further down: "A trend" went missing over the row that opens a
 * chart, which is the keycap-that-does-nothing this table exists to prevent, wearing its other
 * face.
 *
 * The state here is the ordinary one after two traces, and the one a Brick is in after every
 * restart: the slot is another node's, and this node's route comes out of the log.
 */
MESH_TEST_CASE(actions_node_detail_reads_this_nodes_route, unit) {
    struct mesh_ui_snapshot snapshot;
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_NODES;
    snapshot.nav.node_detail_open = true;
    snapshot.nav.node_detail_node = 0x2000U;
    snapshot.nav.node_trend = MESH_UI_HISTORY_NONE;

    snapshot.handshake_valid = true;
    snapshot.handshake.has_my_info = true;
    snapshot.handshake.my_info.node_num = 0x1000U;
    snapshot.handshake.node_count = 1U;
    struct mesh_ui_node_summary *node = &snapshot.handshake.nodes[0];
    node->node_id = 0x2000U;
    node->metrics.valid = true;
    node->metrics.has_battery = true;
    node->metrics.battery_level = 72U;

    /* Two readings, because one is a level and a row only offers a chart once there is a line
       to draw. */
    mesh_ui_history_reset(&snapshot.history);
    mesh_ui_history_note_battery(&snapshot.history, 1000U, node->node_id, 80U);
    mesh_ui_history_note_battery(&snapshot.history, 2000U, node->node_id, 72U);

    /* This node's route, measured and kept; and the slot still on the node traced after it. */
    struct mesh_ui_traceroute *logged = &snapshot.traceroutes.entries[0];
    snapshot.traceroutes.count = 1U;
    logged->state = MESH_TRACEROUTE_DONE;
    logged->target = node->node_id;
    logged->completed = 1750000000U;
    logged->forward_count = 3U;
    logged->back_count = 3U;
    for (uint8_t i = 0U; i < 3U; ++i) {
        logged->forward[i].node_id = 0x1000U + i;
        logged->back[i].node_id = 0x3000U - i;
    }
    snapshot.traceroute.state = MESH_TRACEROUTE_DONE;
    snapshot.traceroute.target = 0x9000U;
    snapshot.traceroute.forward_count = 2U;

    /* The row the chart hangs on, found in the list the screen actually draws. */
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(
        node, false, 0U, mesh_ui_snapshot_traceroute_view(&snapshot, node->node_id),
        &snapshot.handshake, &snapshot.history, false, items, MESH_UI_NODE_ITEMS_MAX);
    uint32_t row = count;
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].trend_reading != MESH_UI_HISTORY_NONE) {
            row = i;
            break;
        }
    }
    MESH_TEST_FAIL_IF(row == count, "the battery row should carry the trend behind it");

    snapshot.nav.cursor[MESH_UI_SCREEN_NODES] = row;
    struct inkcell_action_bar bar;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_TREND,
                      "the bar should name the press the row under the cursor really has");
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
    struct inkcell_action_bar bar;

    /*
     * A list with nothing discovered still has the network row, so the cursor is standing on
     * it: A is the way to type an address and Y is left off, because with no address to edit
     * it would be a second keycap for the one thing this row does. X stays, because dropping
     * the live link is not about the cursor.
     */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot.nav.devices_open = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_ADDRESS,
                      "an empty device list still offers the network address");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != INKCELL_STR_NONE,
                      "with no address set, Y would be a second way to the same keyboard");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_X) != MESH_STR_ACTION_DISCONNECT,
                      "X drops whichever link is up and does not depend on the row");

    /* Once an address is written down the row's A is the ordinary connect, and Y edits it. */
    snprintf(snapshot.network_host, sizeof snapshot.network_host, "192.168.1.50");
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_CONNECT,
                      "a configured network row is something A can open a link to");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != MESH_STR_ACTION_ADDRESS,
                      "Y on a configured network row edits the address");

    /* A USB node in its bootloader: no session to open, and no bond to forget either. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot.nav.devices_open = true;
    struct mesh_ui_device *boot =
        actions_add_device(&snapshot, "/dev/ttyUSB0", MESH_UI_DEVICE_SERIAL);
    boot->bootloader = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != INKCELL_STR_NONE,
                      "A must not offer to connect to a bootloader");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != INKCELL_STR_NONE,
                      "a USB port has no bond for Y to forget");

    /* The same port running firmware is connectable again - and still has nothing to forget. */
    boot->bootloader = false;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != MESH_STR_ACTION_CONNECT,
                      "a USB node running firmware is something A can open a link to");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != INKCELL_STR_NONE,
                      "a USB port still has no bond to forget");

    /* The row we are already on: Y can still drop the bond, A has nothing left to do. */
    actions_snapshot(&snapshot);
    snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot.nav.devices_open = true;
    struct mesh_ui_device *live =
        actions_add_device(&snapshot, "F4:12:FA:00:0A:11", MESH_UI_DEVICE_BLE);
    live->connected = true;
    mesh_ui_actions_for(&snapshot, &bar);
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_A) != INKCELL_STR_NONE,
                      "A must not offer to connect to the radio already connected");
    MESH_TEST_FAIL_IF(actions_label_for(&bar, INKCELL_BUTTON_Y) != MESH_STR_ACTION_FORGET,
                      "a bonded radio can still be forgotten while it is the one we are on");

    record_success(test_name);
}

/*
 * The verbs a pointer finds in the heading: the same command set the keycaps are projected from,
 * less what a pointer already has somewhere better.
 *
 * The conversation list is the case with every kind of verb on it: a row's own A (the row is
 * clicked), a creation verb (the pill), a destructive one, a verb that names the row (mute), help
 * and the tabs.
 */
MESH_TEST_CASE(actions_heading_is_what_a_pointer_has_nowhere_else, unit) {
    struct mesh_ui_snapshot snapshot;
    actions_snapshot(&snapshot);
    struct mesh_ui_heading_action verbs[MESH_UI_HEADING_ACTIONS_MAX];
    const size_t count = mesh_ui_actions_heading(&snapshot, verbs, MESH_UI_HEADING_ACTIONS_MAX);

    MESH_TEST_FAIL_IF(count < 3U, "the conversation list should offer new, delete and help");
    for (size_t i = 0U; i < count; ++i) {
        MESH_TEST_FAIL_IF(verbs[i].id == MESH_UI_COMMAND_OPEN ||
                              verbs[i].id == MESH_UI_COMMAND_TABS,
                          "a row's own A and the tabs are clicked, not offered in the heading");
        MESH_TEST_FAIL_IF(verbs[i].icon == INKCELL_ICON_NONE,
                          "every heading verb should be drawn with a symbol");
    }
    MESH_TEST_FAIL_IF(verbs[0].id != MESH_UI_COMMAND_NEW || !verbs[0].primary,
                      "New should lead the heading, as the verb the list is for");
    MESH_TEST_FAIL_IF(verbs[1].id != MESH_UI_COMMAND_DELETE || !verbs[1].destructive ||
                          verbs[1].primary,
                      "Delete should follow, marked as the verb that throws something away");
    MESH_TEST_FAIL_IF(verbs[count - 1U].id != MESH_UI_COMMAND_HELP || verbs[count - 1U].primary,
                      "help should close the heading and never be its pill");

    /* Help's slot survives a heading too short for everything - it is kept, not trimmed. */
    const size_t two = mesh_ui_actions_heading(&snapshot, verbs, 2U);
    MESH_TEST_FAIL_IF(two != 2U || verbs[0].id != MESH_UI_COMMAND_NEW ||
                          verbs[1].id != MESH_UI_COMMAND_HELP,
                      "a short heading should give up its last verb before its help");

    MESH_TEST_FAIL_IF(mesh_ui_command_icon(MESH_UI_COMMAND_BACK) != INKCELL_ICON_NONE ||
                          mesh_ui_command_icon(MESH_UI_COMMAND_MOVE) != INKCELL_ICON_NONE ||
                          mesh_ui_command_icon(MESH_UI_COMMAND_QUIT) != INKCELL_ICON_NONE,
                      "back, the paired moves and quit have a better home than the heading");
    MESH_TEST_FAIL_IF(mesh_ui_actions_heading(NULL, verbs, 0U) != 0U, "no room is no verbs");
    record_success(test_name);
}
