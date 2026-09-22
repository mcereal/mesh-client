#define _POSIX_C_SOURCE 200809L

/*
 * A click, from the box the real frame drew to what the nav did with it.
 *
 * Each case renders the frame through fb_render_snapshot(), finds the id's box in the focus map
 * that frame filled, and clicks the middle of it - so a screen that stops registering its rows,
 * or registers them under the wrong block, fails here rather than in a window nobody is
 * looking at. See src/ui/nav/nav_click.c for the rules.
 */

#include "inkcell/ui/fb_capture.h"
#include "inkcell/ui/fb_draw.h"
#include "inkcell/ui/focus.h"

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <string.h>

/* The frame as it stands, drawn until nothing on it is still moving - a box registered halfway
   through a slide is not where the reader would click. */
static const struct inkcell_focus_map *click_render(struct mesh_ui_store *store,
                                                    struct inkcell_capture *capture) {
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(store);
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
                                         MESH_UI_SCREEN_STATUS,   MESH_UI_SCREEN_MESSAGES,
                                         MESH_UI_SCREEN_DEVICES,  MESH_UI_SCREEN_WAYPOINTS};
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
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_tab(&store, MESH_UI_SCREEN_DEVICES),
                              click_close(&store, capture), "the test needs the Devices tab");
    MESH_TEST_FAIL_IF_CLEANUP(
        !click_on(&store, capture, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, &action),
        click_close(&store, capture), "the device list drew no box for its second row");
    MESH_TEST_FAIL_IF_CLEANUP(
        store.nav.cursor[MESH_UI_SCREEN_DEVICES] != 1U || action.type != MESH_UI_ACTION_CONNECT ||
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
        &store, (uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)MESH_UI_SCREEN_DEVICES, &action);
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
