#define _POSIX_C_SOURCE 200809L

/*
 * Where the nav is, and which way it just moved.
 *
 * This is the half of a screen transition that is not drawing at all, and it is the half worth
 * pinning: a slide that goes the wrong way round is not a crash, does not fail a build, and is
 * invisible in a screenshot. The cases below walk the real key handler, because the point of
 * deriving a route rather than declaring one is that no call site has to remember to say what
 * it did - and the only way to check that claim is to press the buttons.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <string.h>

static struct mesh_ui_route route_now(const struct mesh_ui_store *store) {
    struct mesh_ui_route route;
    mesh_ui_route_of(&store->nav, &route);
    return route;
}

/* One press, and which way it moved the frame. */
static enum mesh_ui_transition press(struct mesh_ui_store *store, enum mesh_ui_key key) {
    const struct mesh_ui_route before = route_now(store);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(store, key, &action);
    const struct mesh_ui_route after = route_now(store);
    return mesh_ui_route_move(&before, &after);
}

MESH_TEST_CASE(ui_route_depth_counts_the_levels, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    if (route_now(&store).depth != 0U || route_now(&store).level != MESH_UI_ROUTE_LIST) {
        failure = "the conversation list is the Messages tab's own level";
        goto cleanup;
    }

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    /* Off the all-traffic row and onto a conversation there is somebody to reply in: the
       firehose has no destination, so it offers neither the compose sheet nor the keyboard. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.thread_open || route_now(&store).depth != 1U ||
        route_now(&store).level != MESH_UI_ROUTE_THREAD) {
        failure = "an open thread is one level in";
        goto cleanup;
    }

    /* A on a thread raises the compose sheet, and A on its draft row raises the keyboard over
       the sheet without closing it. Two levels, not one - which is the whole reason every
       overlay that is up is counted rather than only the topmost. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.compose_open || route_now(&store).depth != 2U ||
        route_now(&store).level != MESH_UI_ROUTE_COMPOSE) {
        failure = "the compose sheet is a level over the thread";
        goto cleanup;
    }
    while (store.nav.compose_cursor != MESH_UI_COMPOSE_ROW_DRAFT) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || !store.nav.compose_open || route_now(&store).depth != 3U ||
        route_now(&store).level != MESH_UI_ROUTE_KEYBOARD) {
        failure = "the keyboard over the sheet is a third level";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_route_settings_goes_three_deep, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    if (route_now(&store).depth != 0U) {
        failure = "the section list is the Settings tab's own level";
        goto cleanup;
    }

    /* A section reached straight off the list, and one reached through the Modules list. The
       second is a level further in even though both are a section, which is exactly what the
       parked `settings_parent` says and what B has to undo one step at a time. */
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_LORA) || route_now(&store).depth != 1U ||
        route_now(&store).level != MESH_UI_ROUTE_SECTION) {
        failure = "a top-level section is one level in";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_MQTT) || route_now(&store).depth != 2U) {
        failure = "a module section is two levels in";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_route_in_and_out_are_opposite, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    if (press(&store, MESH_UI_KEY_A) != MESH_UI_TRANSITION_FORWARD) {
        failure = "opening a thread should move forward";
        goto cleanup;
    }
    if (press(&store, MESH_UI_KEY_B) != MESH_UI_TRANSITION_BACK) {
        failure = "backing out of a thread should move back";
        goto cleanup;
    }

    /* The Nodes tab's own two levels, so that the rule is the shape of the hierarchy rather
       than anything the Messages tab does. */
    if (press(&store, MESH_UI_KEY_RIGHT) != MESH_UI_TRANSITION_FORWARD ||
        store.nav.screen != MESH_UI_SCREEN_NODES) {
        failure = "Right should move rightwards along the tabs";
        goto cleanup;
    }
    if (press(&store, MESH_UI_KEY_LEFT) != MESH_UI_TRANSITION_BACK) {
        failure = "Left should move leftwards along the tabs";
        goto cleanup;
    }
    (void)press(&store, MESH_UI_KEY_RIGHT);
    if (press(&store, MESH_UI_KEY_A) != MESH_UI_TRANSITION_FORWARD || !store.nav.node_detail_open) {
        failure = "opening a node should move forward";
        goto cleanup;
    }
    if (press(&store, MESH_UI_KEY_B) != MESH_UI_TRANSITION_BACK) {
        failure = "backing out of a node should move back";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The tab strip decides whenever the tab changed, and it decides the short way round.
 *
 * Both halves are cases the first version of this got wrong, and both are cases a screenshot
 * cannot show. L/R work from a nested screen - each tab keeps its own place - so Right off an
 * open node detail is one tab rightwards *and* a level shallower, and reading the depth there
 * slid the new tab in from the left while the strip above it travelled right. And the strip
 * wraps, so comparing two tab indices called Right off the last tab the largest leftwards move
 * there is.
 */
MESH_TEST_CASE(ui_route_the_tab_strip_decides, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.node_detail_open || store.nav.screen != MESH_UI_SCREEN_NODES) {
        failure = "could not open a node detail to leave from";
        goto cleanup;
    }
    /* One tab rightwards and one level shallower at the same time. The strip is what the eye
       is following, so the strip is what the body has to agree with. */
    if (press(&store, MESH_UI_KEY_R1) != MESH_UI_TRANSITION_FORWARD ||
        store.nav.screen != MESH_UI_SCREEN_DEVICES || route_now(&store).depth != 0U) {
        failure = "Right off a nested screen should still move rightwards";
        goto cleanup;
    }

    while (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_R1, &action);
    }
    if (press(&store, MESH_UI_KEY_R1) != MESH_UI_TRANSITION_FORWARD ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "Right off the last tab wraps to the first, and is still rightwards";
        goto cleanup;
    }
    if (press(&store, MESH_UI_KEY_L1) != MESH_UI_TRANSITION_BACK ||
        store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        failure = "Left off the first tab wraps to the last, and is still leftwards";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The half of the model that is about what a route is *not*.
 *
 * A route that moved when the cursor did would restart the animation under the user's thumb on
 * every press of Down, which is the one way this could be worse than no transition at all.
 */
MESH_TEST_CASE(ui_route_ignores_the_cursor_and_the_draft, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    if (press(&store, MESH_UI_KEY_DOWN) != MESH_UI_TRANSITION_NONE ||
        press(&store, MESH_UI_KEY_UP) != MESH_UI_TRANSITION_NONE) {
        failure = "walking the conversation list is not a move between places";
        goto cleanup;
    }
    /* X arms a delete on the row under the cursor, which changes the frame and changes no
       place. Nothing armed is part of a route, and this is the one that is easiest to reach. */
    if (press(&store, MESH_UI_KEY_X) != MESH_UI_TRANSITION_NONE) {
        failure = "arming a delete is not a move between places";
        goto cleanup;
    }
    (void)press(&store, MESH_UI_KEY_B);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* a thread */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action); /* straight to typing */
    if (!store.nav.keyboard_open) {
        failure = "Y should open the keyboard on the thread";
        goto cleanup;
    }
    const struct mesh_ui_route typing = route_now(&store);
    for (int i = 0; i < 3; ++i) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* type a character */
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    if (store.nav.draft[0] == '\0') {
        failure = "the keyboard should have typed something";
        goto cleanup;
    }
    const struct mesh_ui_route typed = route_now(&store);
    if (!mesh_ui_route_same(&typing, &typed)) {
        failure = "typing moved the route";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The three things a thread can be, told apart.
 *
 * All traffic, a channel and a direct conversation are one level and one screen, so nothing but
 * the subject distinguishes them - and two of them would collide on the obvious encoding, where
 * "channel 0" and "no channel at all" are the same zero. Walking between two threads is not a
 * move a user can make (B comes first), but a route that could not tell them apart would be a
 * route that reported the wrong place, which is worse than reporting no move.
 */
MESH_TEST_CASE(ui_route_tells_the_threads_apart, unit) {
    const char *failure = NULL;
    struct mesh_ui_nav inbox;
    struct mesh_ui_nav channel;
    struct mesh_ui_nav direct;
    mesh_ui_nav_init(&inbox);
    mesh_ui_nav_init(&channel);
    mesh_ui_nav_init(&direct);

    inbox.thread_open = true;
    inbox.inbox = true;

    channel.thread_open = true;
    channel.target_node = MESH_MESSAGE_BROADCAST_ADDR;
    channel.target_channel = 0U;

    direct.thread_open = true;
    direct.target_node = 0x0A0B0C0DU;

    struct mesh_ui_route a;
    struct mesh_ui_route b;
    struct mesh_ui_route c;
    mesh_ui_route_of(&inbox, &a);
    mesh_ui_route_of(&channel, &b);
    mesh_ui_route_of(&direct, &c);

    if (a.depth != 1U || b.depth != 1U || c.depth != 1U) {
        failure = "every thread is one level in";
        goto cleanup;
    }
    if (mesh_ui_route_same(&a, &b) || mesh_ui_route_same(&b, &c) || mesh_ui_route_same(&a, &c)) {
        failure = "two different threads read as the same place";
        goto cleanup;
    }
    /* And the same thread twice is the same place, which is what stops a republished snapshot
       animating. */
    struct mesh_ui_route again;
    mesh_ui_route_of(&channel, &again);
    if (!mesh_ui_route_same(&b, &again) ||
        mesh_ui_route_move(&b, &again) != MESH_UI_TRANSITION_NONE) {
        failure = "the same thread read as two places";
        goto cleanup;
    }

cleanup:
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
