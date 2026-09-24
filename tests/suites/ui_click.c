#define _POSIX_C_SOURCE 200809L

/*
 * A click, from the box the real frame drew to what the nav did with it.
 *
 * Each case renders the frame through fb_render_snapshot(), finds the id's box in the focus map
 * that frame filled, and clicks the middle of it - so a screen that stops registering its rows,
 * or registers them under the wrong block, fails here rather than in a window nobody is
 * looking at. See src/ui/nav/nav_click.c for the rules.
 */

#include "inkcell/ui/actions.h"
#include "inkcell/ui/fb_capture.h"
#include "inkcell/ui/fb_draw.h"
#include "inkcell/ui/focus.h"

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/commands.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/waypoints.h"

#include <stdio.h>
#include <string.h>

/* The frame as it stands, drawn until nothing on it is still moving - a box registered halfway
   through a slide is not where the reader would click. */
static const struct inkcell_focus_map *click_render(struct mesh_ui_store *store,
                                                    struct inkcell_capture *capture) {
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    /* A refresh only when there is nothing to publish: it marks every list as new, which is
       news an open right-click menu is put down by. */
    if (store->pending_flags == MESH_UI_UPDATE_NONE) {
        mesh_ui_store_request_refresh(store);
    }
    (void)mesh_ui_store_consume_updates(store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    for (int i = 0; i < 100 && inkcell_capture_animating(capture); ++i) {
        inkcell_capture_advance(capture, 16U);
        inkcell_capture_render(capture, &snapshot);
    }
    return inkcell_capture_state(capture)->focus;
}

/*
 * Clicks the middle of `id`'s box on a fresh frame. False when the frame drew no such box or
 * something else is on top of its middle, which are both a click that could not have happened.
 */
static bool click_on(struct mesh_ui_store *store, struct inkcell_capture *capture, uint32_t id,
                     struct mesh_ui_action *action) {
    const struct inkcell_focus_map *const map = click_render(store, capture);
    struct inkcell_focus_rect box;
    if (!inkcell_focus_rect_of(map, id, &box)) {
        return false;
    }
    const uint32_t hit = inkcell_focus_hit(map, box.x + box.w / 2, box.y + box.h / 2);
    if (hit != id) {
        return false;
    }
    (void)mesh_ui_store_handle_click(store, hit, action);
    return true;
}

static int click_open(struct mesh_ui_store *store, struct inkcell_capture **capture) {
    if (mesh_ui_store_init(store) != 0) {
        return -1;
    }
    mesh_test_nav_populate(store);
    if (mesh_ui_capture_open(capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT,
                             INKCELL_SCALE(4)) != 0) {
        mesh_ui_store_shutdown(store);
        return -1;
    }
    return 0;
}

static void click_close(struct mesh_ui_store *store, struct inkcell_capture *capture) {
    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(store);
}

MESH_TEST_CASE(ui_click_a_tab_is_that_tab, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(click_open(&store, &capture) != 0, "store or capture failed to open");
    struct mesh_ui_action action;

    /* Every tab, from wherever the last click left the strip - both directions and the wrap. */
    const enum mesh_ui_screen order[] = {MESH_UI_SCREEN_SETTINGS, MESH_UI_SCREEN_NODES,
                                         MESH_UI_SCREEN_RADIO, MESH_UI_SCREEN_MESSAGES,
                                         MESH_UI_SCREEN_WAYPOINTS};
    for (size_t i = 0; i < sizeof order / sizeof order[0]; ++i) {
        MESH_TEST_FAIL_IF_CLEANUP(
            !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)order[i], &action),
            click_close(&store, capture), "the strip drew no box for a tab");
        MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen != order[i], click_close(&store, capture),
                                  "a click on a tab should put that tab up");
    }
    click_close(&store, capture);
}

MESH_TEST_CASE(ui_click_a_row_opens_it, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(click_open(&store, &capture) != 0, "store or capture failed to open");
    struct mesh_ui_action action;

    /* NodeTwo, which is not connected: one click is A on it, which is connecting. */
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_devices(&store), click_close(&store, capture),
                              "the test needs the Devices tab");
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, &action),
        click_close(&store, capture), "the device list drew no box for its second row");
    MESH_TEST_FAIL_IF_CLEANUP(
        store.nav.cursor[MESH_UI_SCREEN_RADIO] != 1U || action.type != MESH_UI_ACTION_CONNECT ||
            strcmp(action.identifier, "AA:BB:CC:DD:EE:02") != 0,
        click_close(&store, capture), "a click on a radio should connect to that radio");
    click_close(&store, capture);
}

MESH_TEST_CASE(ui_click_a_bubble_selects_and_a_sheet_keeps_the_screen_under_it, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(click_open(&store, &capture) != 0, "store or capture failed to open");
    struct mesh_ui_action action;

    /* BRVO's conversation, the list's third row, whose one message is packet 12. */
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 2U, &action) ||
            !store.nav.thread_open || store.nav.target_node != 0x3000U,
        click_close(&store, capture), "a click on a conversation should open it");

    /* A there would answer the message. A click is the reader finding their place. */
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 0U, &action),
        click_close(&store, capture), "the thread drew no box for its bubble");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.compose_open || action.type != MESH_UI_ACTION_NONE,
                              click_close(&store, capture),
                              "a click on a bubble should select it, not answer it");

    /* The tapbacks over it. The thread and the tabs are still on the frame, and neither is
       what the reader is being asked about. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.reaction_open, click_close(&store, capture),
                              "X should open the tapbacks");
    (void)click_render(&store, capture);
    (void)mesh_ui_store_handle_click(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 0U, &action);
    (void)mesh_ui_store_handle_click(
        &store, (uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)MESH_UI_SCREEN_RADIO, &action);
    MESH_TEST_FAIL_IF_CLEANUP(
        !store.nav.reaction_open || store.nav.screen != MESH_UI_SCREEN_MESSAGES ||
            action.type != MESH_UI_ACTION_NONE,
        click_close(&store, capture), "a click under a sheet should go nowhere");

    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_SHEET_ROWS + 1U, &action),
        click_close(&store, capture), "the sheet drew no box for its second row");
    MESH_TEST_FAIL_IF_CLEANUP(
        action.type != MESH_UI_ACTION_SEND_TEXT || !action.is_reaction || action.reply_id != 12U ||
            strcmp(action.text, mesh_ui_reaction_emoji(1)) != 0,
        click_close(&store, capture), "a click on a tapback should send that one");
    click_close(&store, capture);
}

MESH_TEST_CASE(ui_click_a_heading_is_nothing_and_a_dialog_is_answered, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(click_open(&store, &capture) != 0, "store or capture failed to open");
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.can_shutdown = true;
    mesh_ui_store_set_settings(&store, &settings);
    struct mesh_ui_action action;

    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_settings_open(&store, MESH_UI_SETTINGS_ACTIONS) ||
                                  !mesh_test_settings_cursor_to(&store, 2U),
                              click_close(&store, capture), "the test needs Radio actions open");

    /* Row 0 is the Power heading. The frame draws it as a title and registers no box for it,
       and the nav would refuse one anyway: the cursor does not park on a heading. */
    struct inkcell_focus_rect heading;
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_focus_rect_of(click_render(&store, capture),
                                                    (uint32_t)MESH_UI_FOCUS_ROWS + 0U, &heading),
                              click_close(&store, capture),
                              "a heading should not be something a click can land on");
    (void)mesh_ui_store_handle_click(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 0U, &action);
    MESH_TEST_FAIL_IF_CLEANUP(
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 2U || store.nav.confirm_open,
        click_close(&store, capture), "a click on a heading should do nothing");

    /* Reboot asks first, on Cancel - and the question's two answers are clicked like anything
       else. Cancel first, then the verb. */
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, &action) ||
            !store.nav.confirm_open ||
            store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_REBOOT,
        click_close(&store, capture), "a click on Reboot should ask");
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_DIALOG + 1U, &action) ||
            store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE,
        click_close(&store, capture), "a click on Cancel should close the question");
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, &action) ||
            !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_DIALOG + 0U, &action),
        click_close(&store, capture), "the question should come back and be answerable");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.confirm_open ||
                                  action.type != MESH_UI_ACTION_RADIO_ACTION ||
                                  action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_REBOOT,
                              click_close(&store, capture), "a click on Reboot should reboot");
    click_close(&store, capture);
}

/* What presenting a frame does to the store: everything pending is now on the panel. */
static void click_settle(struct mesh_ui_store *store) {
    static struct mesh_ui_snapshot snapshot;
    (void)mesh_ui_store_consume_updates(store, &snapshot);
}

/*
 * A click that moves the cursor has walked off an armed row, as a d-pad step would have.
 *
 * The click's own press is A, and A on the detail that armed the delete is the key allowed to
 * confirm it - so without the stand-down, clicking another row and then the delete row again
 * carried the delete out on what the reader saw as its first press.
 */
MESH_TEST_CASE(ui_click_elsewhere_stands_an_armed_delete_down, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_waypoint_list list;
    memset(&list, 0, sizeof list);
    list.entries[0].id = 42U;
    list.entries[0].editable = true;
    snprintf(list.entries[0].name, sizeof list.entries[0].name, "%s", "Bridge");
    list.count = 1U;
    mesh_ui_store_set_waypoints(&store, &list);
    struct mesh_ui_action action;

    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_tab(&store, MESH_UI_SCREEN_WAYPOINTS),
                              mesh_ui_store_shutdown(&store), "the test needs the Waypoints tab");
    click_settle(&store);
    (void)mesh_ui_store_handle_click(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 0U, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.waypoint_detail_open, mesh_ui_store_shutdown(&store),
                              "a click on a place should open it");
    const uint32_t rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_WAYPOINTS);
    const uint32_t remove = (uint32_t)MESH_UI_FOCUS_ROWS + rows - 1U;

    click_settle(&store);
    (void)mesh_ui_store_handle_click(&store, remove, &action);
    MESH_TEST_FAIL_IF_CLEANUP(
        !store.nav.waypoint_delete_armed || action.type != MESH_UI_ACTION_NONE,
        mesh_ui_store_shutdown(&store), "the first click on delete should only arm it");
    click_settle(&store);
    (void)mesh_ui_store_handle_click(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 0U, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.waypoint_delete_armed || action.type != MESH_UI_ACTION_NONE,
                              mesh_ui_store_shutdown(&store),
                              "a click on another row should stand the delete down");
    click_settle(&store);
    (void)mesh_ui_store_handle_click(&store, remove, &action);
    MESH_TEST_FAIL_IF_CLEANUP(
        !store.nav.waypoint_delete_armed || action.type != MESH_UI_ACTION_NONE,
        mesh_ui_store_shutdown(&store), "coming back to delete should ask again, not delete");
    mesh_ui_store_shutdown(&store);
}

/*
 * A click names a row by where it was on the frame, and a store that has changed since is not
 * that frame. Dropped rather than answered: the radios re-sorting under the pointer would
 * otherwise connect to whichever one moved into the row that was clicked.
 */
MESH_TEST_CASE(ui_click_against_a_frame_the_store_has_moved_past_is_dropped, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_action action;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_devices(&store), mesh_ui_store_shutdown(&store),
                              "the test needs the Devices tab");
    click_settle(&store);

    const struct mesh_ui_device devices[2] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .connected = true},
        {.identifier = "AA:BB:CC:DD:EE:09", .name = "NodeNine", .rssi = -50, .connected = false},
    };
    mesh_ui_store_set_discovery(&store, devices, 2U);
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_store_handle_click(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, &action) ||
            action.type != MESH_UI_ACTION_NONE,
        mesh_ui_store_shutdown(&store), "a click on a frame the store has moved past should drop");

    click_settle(&store);
    (void)mesh_ui_store_handle_click(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, &action);
    MESH_TEST_FAIL_IF_CLEANUP(action.type != MESH_UI_ACTION_CONNECT ||
                                  strcmp(action.identifier, "AA:BB:CC:DD:EE:09") != 0,
                              mesh_ui_store_shutdown(&store),
                              "once the frame is current again the click should land");
    mesh_ui_store_shutdown(&store);
}

/* A right-click on the middle of `id`'s box on a fresh frame - click_on()'s terms. */
static bool click_context(struct mesh_ui_store *store, struct inkcell_capture *capture,
                          uint32_t id) {
    const struct inkcell_focus_map *const map = click_render(store, capture);
    struct inkcell_focus_rect box;
    if (!inkcell_focus_rect_of(map, id, &box)) {
        return false;
    }
    const int x = box.x + box.w / 2;
    const int y = box.y + box.h / 2;
    if (inkcell_focus_hit(map, x, y) != id) {
        return false;
    }
    (void)mesh_ui_store_handle_context(store, id, x, y);
    return true;
}

MESH_TEST_CASE(ui_click_a_right_click_menu_is_the_rows_own_commands, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(click_open(&store, &capture) != 0, "store or capture failed to open");
    struct mesh_ui_action action;

    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_devices(&store), click_close(&store, capture),
                              "the test needs the Devices tab");
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_context(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U) ||
            !store.nav.context_open || store.nav.cursor[MESH_UI_SCREEN_RADIO] != 1U,
        click_close(&store, capture), "a right-click on a row should select it and open its menu");

    /* Anything but a verb puts it down and does nothing else - a key, or a click off it. */
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action) || store.nav.context_open ||
            store.nav.cursor[MESH_UI_SCREEN_RADIO] != 1U,
        click_close(&store, capture), "a key should put the menu down and not move the cursor");
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_context(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U) ||
            !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_MENU_DISMISS, &action) ||
            store.nav.context_open || action.type != MESH_UI_ACTION_NONE,
        click_close(&store, capture), "a click off the menu should put it down and press nothing");

    /* New data under an open menu puts it down: the row it was opened on is held only as an
       index, which a republished list may have given to somebody else. */
    MESH_TEST_FAIL_IF_CLEANUP(!click_context(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U),
                              click_close(&store, capture), "the menu should open again");
    mesh_ui_store_set_network_host(&store, "10.0.0.9");
    (void)click_render(&store, capture);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.context_open, click_close(&store, capture),
                              "a device list republished under the menu should put it down");

    /* NodeTwo is not connected, so its row command is Connect. The renderer registers the
       semantic id rather than the A binding retained for the Brick action bar. */
    MESH_TEST_FAIL_IF_CLEANUP(!click_context(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U),
                              click_close(&store, capture), "the menu should open again");
    const struct inkcell_focus_map *const menu_map = click_render(&store, capture);
    struct inkcell_focus_rect legacy;
    MESH_TEST_FAIL_IF_CLEANUP(
        inkcell_focus_rect_of(menu_map, (uint32_t)MESH_UI_FOCUS_MENU + (uint32_t)INKCELL_BUTTON_A,
                              &legacy),
        click_close(&store, capture), "the menu should not expose a physical button target");
    const uint32_t connect = (uint32_t)MESH_UI_FOCUS_MENU + (uint32_t)MESH_UI_COMMAND_CONNECT;
    struct inkcell_focus_rect command;
    MESH_TEST_FAIL_IF_CLEANUP(!inkcell_focus_rect_of(menu_map, connect, &command) ||
                                  inkcell_focus_hit(menu_map, command.x + command.w / 2,
                                                    command.y + command.h / 2) != connect,
                              click_close(&store, capture),
                              "the menu should offer the row's Connect command");
    (void)mesh_ui_store_handle_click(&store, connect, &action);
    MESH_TEST_FAIL_IF_CLEANUP(
        store.nav.context_open || action.type != MESH_UI_ACTION_NONE, click_close(&store, capture),
        "the store fallback should dismiss, leaving command dispatch to the controller");

    /* Identity must not become presentation order. A conversation offers commands whose enum
       values sort differently, but the menu retains the established A, X, Y, Start sequence. */
    (void)click_render(&store, capture);
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_test_open_tab(&store, MESH_UI_SCREEN_MESSAGES) ||
            !click_context(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 2U),
        click_close(&store, capture), "a conversation should open its context menu");
    const struct inkcell_focus_map *const ordered_map = click_render(&store, capture);
    const enum mesh_ui_command_id ordered[] = {
        MESH_UI_COMMAND_OPEN,
        MESH_UI_COMMAND_DELETE,
        MESH_UI_COMMAND_NEW,
        MESH_UI_COMMAND_MUTE,
    };
    int previous_y = -1;
    for (size_t i = 0U; i < sizeof ordered / sizeof ordered[0]; ++i) {
        struct inkcell_focus_rect row;
        const uint32_t id = (uint32_t)MESH_UI_FOCUS_MENU + (uint32_t)ordered[i];
        MESH_TEST_FAIL_IF_CLEANUP(
            !inkcell_focus_rect_of(ordered_map, id, &row) || row.y <= previous_y,
            click_close(&store, capture), "context commands should remain in A, X, Y, Start order");
        previous_y = row.y;
    }
    click_close(&store, capture);
}

/*
 * Where the verbs are, by who is holding what.
 *
 * On the device they are keycaps at the foot and the heading carries none: the keycaps are the
 * only place a d-pad reader learns the buttons. With a pointer they are the heading's actions -
 * each a box a click lands on, named by its command - and there is no foot, since a legend for
 * buttons the reader is not holding is the handheld HUD this frame is leaving behind.
 */
MESH_TEST_CASE(ui_click_the_heading_carries_the_verbs_for_a_pointer, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(click_open(&store, &capture) != 0, "store or capture failed to open");
    const uint32_t new_verb = (uint32_t)MESH_UI_FOCUS_BAR + (uint32_t)MESH_UI_COMMAND_NEW;
    const uint32_t y_cap = INKCELL_FOCUS_ACTION_KEY(INKCELL_KEY_Y);
    struct inkcell_focus_rect box;

    const struct inkcell_focus_map *device = click_render(&store, capture);
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_focus_rect_of(device, new_verb, &box),
                              click_close(&store, capture),
                              "the device heading should carry no verbs");
    MESH_TEST_FAIL_IF_CLEANUP(!inkcell_focus_rect_of(device, y_cap, &box),
                              click_close(&store, capture),
                              "the device foot should still name Y's verb as a keycap");

    inkcell_capture_state(capture)->pointer = true;
    const struct inkcell_focus_map *pointer = click_render(&store, capture);
    MESH_TEST_FAIL_IF_CLEANUP(
        !inkcell_focus_rect_of(pointer, new_verb, &box) ||
            inkcell_focus_hit(pointer, box.x + box.w / 2, box.y + box.h / 2) != new_verb,
        click_close(&store, capture),
        "a pointer should find New in the heading, where it can be clicked");
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_focus_rect_of(pointer, y_cap, &box),
                              click_close(&store, capture),
                              "a pointer frame with its verbs in the heading should draw no foot");
    click_close(&store, capture);
}

/*
 * A window wide enough for two panes stands the thread beside the conversations rather than in
 * their place, and opening one is not a move: the list was already on the panel and stays put.
 *
 * The nav is the one-pane nav either way, so what this holds is the drawing: the list's rows are
 * the click targets until a thread opens, the thread's bubbles are after it, and every bubble is
 * to the right of where the list was - in the other pane, not over it.
 */
MESH_TEST_CASE(ui_click_a_wide_window_opens_a_thread_beside_its_list, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store failed to open");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 1920U, 1080U, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture failed to open");
    struct mesh_ui_action action;

    const struct inkcell_focus_map *map = click_render(&store, capture);
    struct inkcell_focus_rect row;
    MESH_TEST_FAIL_IF_CLEANUP(!inkcell_focus_rect_of(map, (uint32_t)MESH_UI_FOCUS_ROWS + 2U, &row),
                              click_close(&store, capture), "the list should register its rows");
    const int list_right = row.x + row.w;
    MESH_TEST_FAIL_IF_CLEANUP(list_right > 1920 / 2, click_close(&store, capture),
                              "the list should stand in the narrower, leading pane");

    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 2U, &action) ||
            !store.nav.thread_open,
        click_close(&store, capture), "a click on a conversation should open it");

    /* One frame, not settled: a slide would be running now if opening were a move. */
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_fb_transition_offset(inkcell_capture_state(capture)) != 0,
                              click_close(&store, capture),
                              "a thread opening beside its list should not slide the frame");

    map = click_render(&store, capture);
    struct inkcell_focus_rect bubble;
    MESH_TEST_FAIL_IF_CLEANUP(
        !inkcell_focus_rect_of(map, (uint32_t)MESH_UI_FOCUS_ROWS + 0U, &bubble),
        click_close(&store, capture), "the thread should register its bubbles");
    for (uint32_t i = 0U; i < 64U; ++i) {
        if (inkcell_focus_rect_of(map, (uint32_t)MESH_UI_FOCUS_ROWS + i, &bubble)) {
            MESH_TEST_FAIL_IF_CLEANUP(bubble.x < list_right, click_close(&store, capture),
                                      "every target in the rows block should be the thread's, "
                                      "in the detail pane beside the list");
        }
    }
    click_close(&store, capture);
}

/* The same split on the Nodes tab: the roster stays, and the node opened from it stands beside
   it without a slide. */
MESH_TEST_CASE(ui_click_a_wide_window_opens_a_node_beside_its_roster, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store failed to open");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 1920U, 1080U, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture failed to open");
    struct mesh_ui_action action;

    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)MESH_UI_SCREEN_NODES,
                  &action) ||
            store.nav.screen != MESH_UI_SCREEN_NODES,
        click_close(&store, capture), "a click on the tab should open the roster");
    click_settle(&store);

    const uint32_t alfa = (uint32_t)MESH_UI_FOCUS_ROWS + MESH_UI_NODES_LEAD_ROWS + 1U;
    const struct inkcell_focus_map *map = click_render(&store, capture);
    struct inkcell_focus_rect row;
    MESH_TEST_FAIL_IF_CLEANUP(!inkcell_focus_rect_of(map, alfa, &row), click_close(&store, capture),
                              "the roster should register its rows");
    const int list_right = row.x + row.w;
    MESH_TEST_FAIL_IF_CLEANUP(list_right > 1920 / 2, click_close(&store, capture),
                              "the roster should stand in the narrower, leading pane");

    MESH_TEST_FAIL_IF_CLEANUP(!click_on(&store, capture, alfa, &action) ||
                                  !store.nav.node_detail_open ||
                                  store.nav.node_detail_node != 0x2000U,
                              click_close(&store, capture), "a click on a node should open it");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_fb_transition_offset(inkcell_capture_state(capture)) != 0,
                              click_close(&store, capture),
                              "a node opening beside its roster should not slide the frame");

    /* With a node open the rows block is the detail's - the tab's cursor indexes its rows - so
       the roster beside it stops registering: a click there would move a cursor it is not. */
    map = click_render(&store, capture);
    struct inkcell_focus_rect item;
    for (uint32_t i = 0U; i < 64U; ++i) {
        if (inkcell_focus_rect_of(map, (uint32_t)MESH_UI_FOCUS_ROWS + i, &item)) {
            MESH_TEST_FAIL_IF_CLEANUP(item.x < list_right, click_close(&store, capture),
                                      "nothing in the rows block should be the roster's while a "
                                      "node is open beside it");
        }
    }
    click_close(&store, capture);
}

/* And on Settings: the section list stays, the section opened from it stands beside it, and a
   module one level down keeps it there - the list is the top-level level, whatever is open. */
MESH_TEST_CASE(ui_click_a_wide_window_opens_a_section_beside_the_sections, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store failed to open");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    mesh_ui_store_set_settings(&store, &settings);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 1920U, 1080U, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture failed to open");
    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    click_settle(&store);

    uint32_t modules = 0U;
    while (mesh_ui_settings_root_at(modules) != MESH_UI_SETTINGS_MODULES) {
        ++modules;
    }
    const uint32_t row_id = (uint32_t)MESH_UI_FOCUS_ROWS + modules;
    const struct inkcell_focus_map *map = click_render(&store, capture);
    struct inkcell_focus_rect row;
    MESH_TEST_FAIL_IF_CLEANUP(!inkcell_focus_rect_of(map, row_id, &row),
                              click_close(&store, capture), "the sections should be click targets");
    const int list_right = row.x + row.w;
    MESH_TEST_FAIL_IF_CLEANUP(list_right > 1920 / 2, click_close(&store, capture),
                              "the sections should stand in the narrower, leading pane");

    MESH_TEST_FAIL_IF_CLEANUP(!click_on(&store, capture, row_id, &action) ||
                                  store.nav.settings_section != MESH_UI_SETTINGS_MODULES,
                              click_close(&store, capture), "a click on Modules should open it");
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_fb_transition_offset(inkcell_capture_state(capture)) != 0,
                              click_close(&store, capture),
                              "a section opening beside the sections should not slide the frame");

    /* A module, from the Modules list now in the detail pane: its rows are that pane's. */
    map = click_render(&store, capture);
    MESH_TEST_FAIL_IF_CLEANUP(
        !inkcell_focus_rect_of(map, (uint32_t)MESH_UI_FOCUS_ROWS, &row) || row.x < list_right,
        click_close(&store, capture), "the Modules list should be the detail pane's targets");
    MESH_TEST_FAIL_IF_CLEANUP(!click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS, &action) ||
                                  store.nav.settings_parent != MESH_UI_SETTINGS_MODULES,
                              click_close(&store, capture), "a click on a module should open it");
    map = click_render(&store, capture);
    for (uint32_t i = 0U; i < 64U; ++i) {
        if (inkcell_focus_rect_of(map, (uint32_t)MESH_UI_FOCUS_ROWS + i, &row)) {
            MESH_TEST_FAIL_IF_CLEANUP(row.x < list_right, click_close(&store, capture),
                                      "nothing in the rows block should be the section list's "
                                      "while a section is open beside it");
        }
    }
    click_close(&store, capture);
}

MESH_TEST_CASE(ui_click_a_right_click_off_a_list_opens_nothing, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(click_open(&store, &capture) != 0, "store or capture failed to open");

    MESH_TEST_FAIL_IF_CLEANUP(
        !click_context(&store, capture,
                       (uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)MESH_UI_SCREEN_NODES) ||
            store.nav.context_open || store.nav.screen == MESH_UI_SCREEN_NODES,
        click_close(&store, capture), "a tab has no menu, and a right-click is not a click");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_store_handle_context(&store, INKCELL_FOCUS_NONE, 0, 0),
                              click_close(&store, capture), "nothing under the pointer is nothing");
    click_close(&store, capture);
}
