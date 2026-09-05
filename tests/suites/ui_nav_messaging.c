#define _POSIX_C_SOURCE 200809L

/* Navigating conversations: tabs, unread counts, channels and the keyboard. */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

MESH_TEST_CASE(ui_nav_navigation, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;

    /* First frame: Messages tab showing the conversation list, cursor on the first row. The
       fixture has no channel table, so the list is All traffic, #Primary, BRVO, New message. */
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "expected initial snapshot";
        goto cleanup;
    }
    if (snapshot.nav.screen != MESH_UI_SCREEN_MESSAGES || snapshot.nav.thread_open ||
        snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 0U ||
        snapshot.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR ||
        strcmp(snapshot.nav.target_name, "#Primary") != 0) {
        failure = "initial nav state wrong";
        goto cleanup;
    }
    if (mesh_ui_nav_conversation_count(&store) != 4U ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 4U) {
        failure = "conversation list should hold all traffic, one channel, BRVO and New";
        goto cleanup;
    }
    struct mesh_ui_conversation conversation;
    if (!mesh_ui_nav_conversation_at(&store, 0U, &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_ALL || conversation.message_count != 2U ||
        strcmp(conversation.preview, "just you") != 0) {
        failure = "row 0 should be all traffic, previewing the newest message";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_CHANNEL || conversation.channel != 0U ||
        strcmp(conversation.name, "#Primary") != 0 || conversation.message_count != 1U) {
        failure = "row 1 should be the primary channel with its one broadcast";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_DIRECT || conversation.node != 0x3000U ||
        strcmp(conversation.name, "BRVO") != 0) {
        failure = "row 2 should be the one direct peer";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 3U, &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_NEW ||
        mesh_ui_nav_conversation_at(&store, 4U, &conversation)) {
        failure = "the last row should be New message, and nothing past it";
        goto cleanup;
    }

    /* A on BRVO's row opens that conversation; only its messages are in view. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action) ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "opening a conversation should change the screen without an action";
        goto cleanup;
    }
    if (!store.nav.thread_open || store.nav.inbox || store.nav.target_node != 0x3000U ||
        strcmp(store.nav.target_name, "BRVO") != 0 ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 1U) {
        failure = "the thread should show only BRVO's messages";
        goto cleanup;
    }
    if (!mesh_ui_store_consume_updates(&store, &snapshot) ||
        (snapshot.update_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "nav change must signal the store";
        goto cleanup;
    }

    /* Y opens the compose overlay over the thread; it needs no destination of its own. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.compose_open || store.nav.compose_cursor != MESH_UI_COMPOSE_FIRST_CANNED ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "Y in a conversation should open the compose overlay";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.dest != 0x3000U || action.channel != 0U ||
        strcmp(action.text, mesh_ui_canned_text(0)) != 0 || store.nav.compose_open ||
        !store.nav.thread_open) {
        failure = "a canned row should send to the open thread and close the overlay";
        goto cleanup;
    }

    /* B leaves the thread for the conversation list, on the row it was opened from. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 2U) {
        failure = "B should return to BRVO's row in the conversation list";
        goto cleanup;
    }

    /* The all-traffic row is a view over everything: A there drills into the conversation the
       selected line belongs to rather than guessing a destination. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.thread_open || !store.nav.inbox ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 2U) {
        failure = "all traffic should show every message";
        goto cleanup;
    }
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "opening a thread should park the cursor on the newest line";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* the broadcast from ALFA */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.inbox ||
        store.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR || store.nav.target_channel != 0U) {
        failure = "A in all traffic should open the conversation the line belongs to";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);

    /* Y on the list (and A on the New message row) opens the picker, which both retargets and
       opens the conversation. LEFT/RIGHT page it instead of switching tabs. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.picker_open || !store.nav.picker_to_compose ||
        mesh_ui_nav_picker_count(&store) != 3U) {
        failure = "Y on the conversation list should open the send-to picker";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.picker_open) {
        failure = "LEFT/RIGHT in the picker must page, not switch tabs";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.picker_open || store.nav.picker_to_compose || store.nav.thread_open) {
        failure = "B should cancel the picker and leave the list showing";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* the New message row */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.picker_open || store.nav.picker_cursor != 0U) {
        failure = "the New message row should open the picker";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.picker_open || store.nav.target_node != 0x2000U ||
        strcmp(store.nav.target_name, "ALFA") != 0 || !store.nav.thread_open ||
        !store.nav.compose_open) {
        failure = "picking a node should open its conversation ready to write";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* close compose */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* close the thread */
    if (store.nav.compose_open || store.nav.thread_open) {
        failure = "B should back out of the overlay and then the thread";
        goto cleanup;
    }

    /* Tabs wrap in both directions; L1/R1 mirror Left/Right. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        failure = "LEFT from the first tab should wrap to the last";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_R1, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "R1 from the last tab should wrap to the first";
        goto cleanup;
    }

    /* Nodes tab: A opens the node's detail; the detail's first row opens its conversation. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_NODES) {
        failure = "RIGHT should reach Nodes";
        goto cleanup;
    }
    /* Our own node has a detail too - it is the one battery the user can do something about -
       but no "Message this node" row, so A inside it does nothing. */
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action) || !store.nav.node_detail_open) {
        failure = "A on our own node should open its detail";
        goto cleanup;
    }
    if (mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action) || store.nav.thread_open) {
        failure = "our own node's detail should offer nothing to message";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.node_detail_open || store.nav.cursor[MESH_UI_SCREEN_NODES] != 0U) {
        failure = "B should back out of the detail onto the node it came from";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* clamps at the last row */
    if (store.nav.cursor[MESH_UI_SCREEN_NODES] != 2U) {
        failure = "DOWN must clamp at the last node";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U ||
        store.nav.node_list_cursor != 2U || store.nav.cursor[MESH_UI_SCREEN_NODES] != 0U) {
        failure = "A on a node should open that node's detail";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.thread_open ||
        store.nav.compose_open || store.nav.target_node != 0x3000U ||
        strcmp(store.nav.target_name, "BRVO") != 0) {
        failure = "the detail's first row should open its conversation, not compose";
        goto cleanup;
    }
    /* Y goes one step further and opens the overlay over it, from either level. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);     /* back to the list */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Nodes, detail still open */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.thread_open ||
        !store.nav.compose_open || store.nav.target_node != 0x3000U) {
        failure = "Y in a node's detail should open its conversation ready to write";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Nodes */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);     /* close the detail */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.compose_open ||
        store.nav.target_node != 0x3000U) {
        failure = "Y on the node list should open its conversation ready to write";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Nodes */

    /* Devices tab: A connects to an unconnected device and does nothing on the connected one. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_DEVICES) {
        failure = "RIGHT from Nodes should reach Devices";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the connected device should not reconnect";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_CONNECT ||
        strcmp(action.identifier, "AA:BB:CC:DD:EE:02") != 0) {
        failure = "A on another device should request a connect";
        goto cleanup;
    }

    /* Status has no rows; the cursor must stay at zero and A must be inert. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.screen != MESH_UI_SCREEN_STATUS ||
        store.nav.cursor[MESH_UI_SCREEN_STATUS] != 0U ||
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action) ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "Status tab must be inert";
        goto cleanup;
    }

    /* Back in the all-traffic thread, a cursor on the newest line follows new traffic; one
       that was moved up stays where it was. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Settings */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* wraps to Messages */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* back to row 0 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);  /* the all-traffic row */
    if (!store.nav.thread_open || !store.nav.inbox) {
        failure = "expected the all-traffic thread";
        goto cleanup;
    }
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "opening a thread should park the cursor on the newest line";
        goto cleanup;
    }
    struct mesh_ui_message_list more = store.messages;
    more.entries[more.count] = more.entries[1];
    more.entries[more.count].packet_id = 13U;
    more.count++;
    mesh_ui_store_set_messages(&store, &more);
    if (!mesh_ui_store_consume_updates(&store, &snapshot) ||
        snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 2U) {
        failure = "cursor at the tail should follow a new message";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    more.entries[more.count] = more.entries[1];
    more.entries[more.count].packet_id = 14U;
    more.count++;
    mesh_ui_store_set_messages(&store, &more);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "cursor moved off the tail should hold its place";
        goto cleanup;
    }

    /* Lists shrinking pull the cursor back inside. */
    struct mesh_ui_message_list fewer;
    memset(&fewer, 0, sizeof fewer);
    fewer.count = 1U;
    fewer.entries[0] = more.entries[0];
    mesh_ui_store_set_messages(&store, &fewer);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 0U ||
        (snapshot.update_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "cursor must be clamped when the list shrinks";
        goto cleanup;
    }

    /* Toasts expire on tick and are dismissed by any key. */
    mesh_ui_store_set_toast(&store, 1000U, "Sent to BRVO");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (strcmp(snapshot.nav.toast, "Sent to BRVO") != 0) {
        failure = "toast not carried in the snapshot";
        goto cleanup;
    }
    mesh_ui_store_tick(&store, 2000U);
    if (store.nav.toast[0] == '\0') {
        failure = "toast expired too early";
        goto cleanup;
    }
    mesh_ui_store_tick(&store, 6000U);
    if (store.nav.toast[0] != '\0' || !mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "toast should expire after a few seconds and repaint";
        goto cleanup;
    }
    mesh_ui_store_set_toast(&store, 7000U, "Connecting");
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_SELECT, &action) ||
        store.nav.toast[0] != '\0') {
        failure = "any key should dismiss a toast";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * The complaint this model replaced: opening a node from the Nodes tab used to rewrite what the
 * Messages tab showed, leaving the user inside a direct conversation with no obvious way back to
 * everything else. Visiting Nodes must now leave the conversation list alone, and B must always
 * be the way out of a conversation.
 */
MESH_TEST_CASE(ui_nav_conversation_isolation, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;

    /* Park the conversation list on #Primary (row 1) without opening it. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "expected the conversation list on row 1";
        goto cleanup;
    }

    /* Walking to Nodes and back changes nothing about what Messages shows. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* BRVO */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "visiting Nodes must not change what Messages shows";
        goto cleanup;
    }

    /* Opening a node's conversation from Nodes (through its detail) is one B away from the list
       again, and the list comes back where it was rather than on the node just visited. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* open the detail */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* "Message this node" */
    if (!store.nav.thread_open || store.nav.target_node != 0x3000U ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "A on a node should open its conversation on the Messages tab";
        goto cleanup;
    }
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action)) {
        failure = "B should leave the conversation";
        goto cleanup;
    }
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "B should restore the conversation list where it was";
        goto cleanup;
    }
    /* Nothing is left to back out of, so a second B is inert rather than surprising. */
    if (mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action)) {
        failure = "B on the conversation list should be a no-op";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * Unread badges: inbound messages count until the conversation they belong to is opened, the
 * count survives a save/load of the cache, and the all-traffic row totals the others rather
 * than keeping a mark of its own.
 */
MESH_TEST_CASE(ui_nav_unread, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;
    struct mesh_ui_conversation conversation;
    char cache_path[] = "/tmp/mesh_ui_unreadXXXXXX";
    int fd = mkstemp(cache_path);
    if (fd < 0) {
        record_failure(test_name, "failed to create a temp cache file");
        mesh_ui_store_shutdown(&store);
        return;
    }
    close(fd);

    /* Nothing read yet: one broadcast on #Primary, one direct from BRVO, two in all. */
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) || conversation.unread != 1U) {
        failure = "the channel's one broadcast should be unread";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) || conversation.unread != 1U) {
        failure = "BRVO's direct message should be unread";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 0U, &conversation) || conversation.unread != 2U) {
        failure = "all traffic should total the rows below it";
        goto cleanup;
    }

    /* Opening BRVO clears only BRVO. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) || conversation.unread != 0U) {
        failure = "opening a conversation should clear its badge";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) || conversation.unread != 1U) {
        failure = "opening one conversation must not clear another";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 0U, &conversation) || conversation.unread != 1U) {
        failure = "the all-traffic total should drop with it";
        goto cleanup;
    }
    /* The snapshot carries the marks, so a backend drawing from it agrees. */
    if (snapshot.read_state.count != 1U) {
        failure = "the read marks should reach the snapshot";
        goto cleanup;
    }

    /* A new message into the open conversation is read on arrival; one into another is not. */
    struct mesh_ui_message_list more = store.messages;
    more.entries[more.count] = more.entries[1]; /* another direct from BRVO */
    more.entries[more.count].packet_id = 31U;
    more.count++;
    more.entries[more.count] = more.entries[0]; /* another broadcast */
    more.entries[more.count].packet_id = 32U;
    more.count++;
    mesh_ui_store_set_messages(&store, &more);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) || conversation.unread != 0U) {
        failure = "a message arriving in the open conversation should not raise a badge";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) || conversation.unread != 2U) {
        failure = "a message arriving elsewhere should raise one";
        goto cleanup;
    }

    /* All traffic is a view: opening it marks nothing read. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (!store.nav.inbox) {
        failure = "expected the all-traffic thread";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) || conversation.unread != 2U) {
        failure = "all traffic must not mark other conversations read";
        goto cleanup;
    }

    /* The marks survive a round trip through the cache. */
    if (mesh_ui_store_save(&store, cache_path) != 0) {
        failure = "save failed";
        goto cleanup;
    }
    struct mesh_ui_store loaded;
    if (mesh_ui_store_init(&loaded) != 0) {
        failure = "second store init failed";
        goto cleanup;
    }
    if (mesh_ui_store_load(&loaded, cache_path) != 0) {
        failure = "load failed";
        mesh_ui_store_shutdown(&loaded);
        goto cleanup;
    }
    if (loaded.read_state.count != 1U ||
        loaded.read_state.marks[0].kind != MESH_UI_CONVERSATION_DIRECT ||
        loaded.read_state.marks[0].node != 0x3000U) {
        failure = "the read mark did not survive the cache";
        mesh_ui_store_shutdown(&loaded);
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&loaded, 2U, &conversation) || conversation.unread != 0U) {
        failure = "a restored mark should still clear its badge";
        mesh_ui_store_shutdown(&loaded);
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&loaded, 1U, &conversation) || conversation.unread != 2U) {
        failure = "a restored mark must not clear a conversation it does not name";
        mesh_ui_store_shutdown(&loaded);
        goto cleanup;
    }
    mesh_ui_store_shutdown(&loaded);

cleanup:
    unlink(cache_path);
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* Channel table drives the To: cycle and the conversation filter; the keyboard builds a draft. */
MESH_TEST_CASE(ui_nav_channels_and_keyboard, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);

    /* Add a channel table (primary "LongFast", secondary "Team", slot 2 disabled) and a
       broadcast on the secondary channel. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.channel_count = 3U;
    handshake.channels[0].index = 0U;
    handshake.channels[0].role = 1U;
    snprintf(handshake.channels[0].name, sizeof handshake.channels[0].name, "%s", "LongFast");
    handshake.channels[1].index = 1U;
    handshake.channels[1].role = 2U;
    snprintf(handshake.channels[1].name, sizeof handshake.channels[1].name, "%s", "Team");
    handshake.channels[2].index = 2U;
    handshake.channels[2].role = 0U;
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_message_list messages = store.messages;
    messages.entries[messages.count] = messages.entries[0];
    messages.entries[messages.count].packet_id = 21U;
    messages.entries[messages.count].channel = 1U;
    snprintf(messages.entries[messages.count].text, sizeof messages.entries[0].text, "%s",
             "team only");
    messages.count++;
    mesh_ui_store_set_messages(&store, &messages);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (strcmp(snapshot.nav.target_name, "#LongFast") != 0) {
        failure = "target name should pick up the primary channel name";
        goto cleanup;
    }

    /* The conversation list: all traffic, both enabled channels (never the disabled slot),
       the one direct peer, then New message. */
    if (mesh_ui_nav_conversation_count(&store) != 5U) {
        failure = "expected five conversation rows";
        goto cleanup;
    }
    struct mesh_ui_conversation conversation;
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_CHANNEL || conversation.channel != 0U ||
        strcmp(conversation.name, "#LongFast") != 0 || conversation.message_count != 1U) {
        failure = "row 1 should be the primary channel";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) || conversation.channel != 1U ||
        strcmp(conversation.name, "#Team") != 0 || conversation.message_count != 1U ||
        strcmp(conversation.preview, "team only") != 0) {
        failure = "row 2 should be the secondary channel, previewing its broadcast";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 3U, &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_DIRECT || conversation.node != 0x3000U) {
        failure = "row 3 should be the one direct peer";
        goto cleanup;
    }

    /* Opening #Team filters the log down to its own broadcast. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.target_channel != 1U ||
        strcmp(store.nav.target_name, "#Team") != 0 ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 1U) {
        failure = "A on the #Team row should open that channel";
        goto cleanup;
    }
    uint32_t indices[MESH_UI_MAX_MESSAGES];
    if (mesh_ui_nav_filter_messages(&store.nav, &store.messages, indices, MESH_UI_MAX_MESSAGES) !=
            1U ||
        store.messages.entries[indices[0]].packet_id != 21U) {
        failure = "channel filter picked the wrong message";
        goto cleanup;
    }

    /* The picker lists #LongFast, #Team, ALFA, BRVO (never us, never the disabled slot). */
    if (mesh_ui_nav_picker_count(&store) != 4U) {
        failure = "picker should list two channels and two nodes";
        goto cleanup;
    }
    char row_name[96];
    uint32_t row_node = 0U;
    uint8_t row_channel = 0U;
    if (!mesh_ui_nav_picker_row(&store, 1U, &row_node, &row_channel, row_name, sizeof row_name) ||
        row_node != MESH_MESSAGE_BROADCAST_ADDR || row_channel != 1U ||
        strcmp(row_name, "#Team") != 0) {
        failure = "picker row 1 should be the secondary channel";
        goto cleanup;
    }

    /* A canned reply sent from the #Team thread carries channel 1, with no To: row involved. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.dest != MESH_MESSAGE_BROADCAST_ADDR ||
        action.channel != 1U) {
        failure = "canned send should target the open thread's channel";
        goto cleanup;
    }

    /* Keyboard: type "Hi", a space, delete it, a space again, START sends "Hi ". */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* draft row */
    if (store.nav.compose_cursor != MESH_UI_COMPOSE_ROW_DRAFT) {
        failure = "expected the draft row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.kb_row != 0U || store.nav.kb_col != 0U) {
        failure = "A on the draft row should open the keyboard at the top-left";
        goto cleanup;
    }
    /* LEFT/RIGHT move within the grid while the keyboard is open, never switch tabs. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.kb_col != MESH_UI_KB_COLS - 1U || store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "LEFT should wrap to the last column";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* back to col 0 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* row 2: asdfghjkl' */
    for (int i = 0; i < 5; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action); /* shift */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* H */
    if (strcmp(store.nav.draft, "H") != 0 || store.nav.kb_layer != MESH_UI_KB_LOWER) {
        failure = "shift should apply to one character";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* row 1: qwertyuiop */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* col 7: i */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action); /* space */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* delete it */
    if (strcmp(store.nav.draft, "Hi") != 0) {
        failure = "typing/deleting produced the wrong draft";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || strcmp(action.text, "Hi ") != 0 ||
        action.channel != 1U || store.nav.keyboard_open || store.nav.draft[0] != '\0' ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "START should send the draft and return to the conversation";
        goto cleanup;
    }

    /* The action row: moving down from column 9 lands on the last (cancel) key; the mapping
       comes back to a sensible column. Cancel drops the draft and closes the keyboard. B on
       an empty draft also closes it. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);    /* keyboard open, row 0 col 0 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);    /* '1' */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action); /* col 9 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);   /* wraps to the action row */
    if (store.nav.kb_row != MESH_UI_KB_CHAR_ROWS || store.nav.kb_col != MESH_UI_KB_ACTIONS - 1U) {
        failure = "column should map onto the action row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* cancel */
    if (store.nav.keyboard_open || store.nav.draft[0] != '\0' || !store.nav.compose_open) {
        failure = "cancel should discard the draft and leave the compose overlay showing";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* reopen (cursor still on draft) */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.keyboard_open) {
        failure = "B with an empty draft should close the keyboard";
        goto cleanup;
    }
    if (mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action) == false &&
        action.type != MESH_UI_ACTION_NONE) {
        failure = "unexpected action";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}
