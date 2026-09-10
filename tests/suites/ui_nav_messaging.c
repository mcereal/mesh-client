#define _POSIX_C_SOURCE 200809L

/* Navigating conversations: tabs, unread counts, channels and the keyboard. */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/status.h"
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

    /* Y writes: straight to the keyboard, with no overlay behind it, so B lands back on the
       thread rather than on the canned list. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.keyboard_open || store.nav.compose_open ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "Y in a conversation should open the keyboard, not the compose overlay";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.keyboard_open || store.nav.compose_open || !store.nav.thread_open) {
        failure = "B on an empty draft should leave the thread showing";
        goto cleanup;
    }

    /* A replies: the canned list over the thread, which needs no destination of its own. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.compose_open || store.nav.keyboard_open ||
        store.nav.compose_cursor != MESH_UI_COMPOSE_FIRST_CANNED ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "A in a conversation should open the compose overlay";
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
       opens the conversation; what lands over it is whichever key asked. LEFT/RIGHT page the
       picker instead of switching tabs. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.picker_open ||
        store.nav.picker_follow != (uint8_t)MESH_UI_PICKER_FOLLOW_KEYBOARD ||
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
    if (store.nav.picker_open || store.nav.picker_follow != (uint8_t)MESH_UI_PICKER_FOLLOW_NONE ||
        store.nav.thread_open) {
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
        !store.nav.compose_open || store.nav.keyboard_open) {
        failure = "picking from the New message row should land on the quick replies";
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
    /* The list's first row opens the map, not a node - so a step down to reach one. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
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
    if (store.nav.node_detail_open || store.nav.cursor[MESH_UI_SCREEN_NODES] != 1U) {
        failure = "B should back out of the detail onto the node it came from";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* clamps at the last row */
    /* Three nodes and the map row above them, so the last row is 3. */
    if (store.nav.cursor[MESH_UI_SCREEN_NODES] != 3U) {
        failure = "DOWN must clamp at the last node";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U ||
        store.nav.node_list_cursor != 3U || store.nav.cursor[MESH_UI_SCREEN_NODES] != 0U) {
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
    /* Y goes one step further and opens the keyboard over it, from either level: the hint on
       both screens says "write", and writing is typing. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);     /* back to the list */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Nodes, detail still open */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.thread_open ||
        !store.nav.keyboard_open || store.nav.compose_open || store.nav.target_node != 0x3000U) {
        failure = "Y in a node's detail should open its conversation ready to type";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Nodes */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);     /* close the detail */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.keyboard_open ||
        store.nav.compose_open || store.nav.target_node != 0x3000U) {
        failure = "Y on the node list should open its conversation ready to type";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Nodes */

    /* Waypoints sits between Nodes and Devices, and its list is never empty - the row that
       makes a place is always there, so Right lands on a screen with something under the
       cursor even on a mesh that has shared nothing. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_WAYPOINTS ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_WAYPOINTS) != 1U) {
        failure = "RIGHT from Nodes should reach Waypoints, which always offers its new row";
        goto cleanup;
    }

    /* Devices tab: A connects to an unconnected device and does nothing on the connected one. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_DEVICES) {
        failure = "RIGHT from Waypoints should reach Devices";
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

    /*
     * Status has no list: its rows are the verbs its cards offer, walked flat. The fixture has
     * a radio attached and a completed handshake, so both are on offer - Disconnect on the Link
     * card and Refresh on the Radio card - and Down steps from one card's button to the other's.
     */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_STATUS ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_STATUS) != 2U ||
        store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT) {
        failure = "Status should offer the two verbs its cards carry";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_DISCONNECT ||
        strcmp(action.identifier, "AA:BB:CC:DD:EE:01") != 0) {
        failure = "A on the Link card should drop the link it names";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH ||
        action.type != MESH_UI_ACTION_REFRESH_SETTINGS) {
        failure = "A on the Radio card should re-read the configuration";
        goto cleanup;
    }
    /* And the cursor stops there: two verbs, no third card to step onto. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH) {
        failure = "DOWN must clamp at the last verb on Status";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);

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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* past the map row, to BRVO */
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.dest != MESH_MESSAGE_BROADCAST_ADDR ||
        action.channel != 1U) {
        failure = "canned send should target the open thread's channel";
        goto cleanup;
    }

    /* Keyboard: type "Hi", a space, delete it, a space again, START sends "Hi ". The draft row
       is the overlay's own way in, and it keeps the overlay behind it. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* draft row */
    if (store.nav.compose_cursor != MESH_UI_COMPOSE_ROW_DRAFT) {
        failure = "expected the draft row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || !store.nav.compose_open || store.nav.kb_row != 0U ||
        store.nav.kb_col != 0U) {
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
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

/*
 * The other end of the same two fields: sending them.
 *
 * Both halves were decode-only - the transcript could show a tapback nobody here could send,
 * and the bar had said "A reply" since the thread screen was written while the press it named
 * built a message that answered nothing. What is checked here is the *aim*: A answers the
 * bubble under the cursor, X reacts to it, and Y - which the bar calls "write" - answers
 * nothing at all, because a new message to a conversation is not a reply to the last thing
 * said in it.
 */
MESH_TEST_CASE(ui_nav_reply_and_react_name_their_target, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    /* Into BRVO's conversation, whose one message is packet 12. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.target_node != 0x3000U) {
        failure = "the test needs BRVO's thread open";
        goto cleanup;
    }

    /* A aims at the bubble under the cursor, and the canned row that follows carries it. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.compose_open || store.nav.reply_to != 12U) {
        failure = "A should open the compose sheet aimed at the message under the cursor";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.reply_id != 12U || action.is_reaction) {
        failure = "a canned reply should name the message it answers and not be a reaction";
        goto cleanup;
    }
    if (store.nav.reply_to != 0U) {
        failure = "the reply target should not outlive the send that used it";
        goto cleanup;
    }

    /* Y writes to the conversation, which is a different thing and says so on the wire. */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.keyboard_open || store.nav.reply_to != 0U) {
        failure = "Y should open the keyboard with nothing to answer";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);

    /* X is the tapback: the emoji list, aimed at the same bubble. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (!store.nav.reaction_open || store.nav.reply_to != 12U || store.nav.reaction_cursor != 0U) {
        failure = "X should open the tapback picker on the message under the cursor";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || !action.is_reaction || action.reply_id != 12U ||
        action.dest != 0x3000U || strcmp(action.text, mesh_ui_reaction_emoji(0)) != 0) {
        failure = "A on a tapback row should send it as a reaction about that message";
        goto cleanup;
    }
    if (store.nav.reaction_open || store.nav.reply_to != 0U) {
        failure = "the picker should close on the press that sent";
        goto cleanup;
    }

    /* And X on a bubble that never got an id has nothing to react to, so it does nothing
       rather than sending a reaction that names packet 0. */
    struct mesh_ui_message_list messages = store.messages;
    messages.entries[1].packet_id = 0U;
    mesh_ui_store_set_messages(&store, &messages);
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (store.nav.reaction_open || action.type != MESH_UI_ACTION_NONE) {
        failure = "a message with no packet id should not open the tapback picker";
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
 * A reaction is an annotation, not a message, and every place that counts or shows messages
 * has to agree about that. Before the emoji flag was read, a tapback arrived as a bubble
 * containing one emoji, became the conversation's preview text, and bumped its unread badge -
 * three wrong answers from one dropped field.
 */
MESH_TEST_CASE(ui_nav_reactions_are_not_messages, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);

    struct mesh_ui_conversation conversation;
    /* The fixture leaves BRVO's one direct message unread, with its text as the preview. */
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) || conversation.unread != 1U ||
        strcmp(conversation.preview, "just you") != 0) {
        failure = "the fixture's direct conversation is not where the test expects it";
        goto cleanup;
    }

    /* Two nodes react to that message. */
    struct mesh_ui_message_list messages = store.messages;
    for (uint32_t i = 0; i < 2U; ++i) {
        struct mesh_ui_message *reaction = &messages.entries[messages.count++];
        memset(reaction, 0, sizeof *reaction);
        reaction->packet_id = 20U + i;
        reaction->peer = 0x3000U;
        reaction->direction = MESH_MESSAGE_INBOUND;
        reaction->is_reaction = true;
        reaction->reply_id = 12U;
        snprintf(reaction->peer_name, sizeof reaction->peer_name, "%s", "BRVO");
        snprintf(reaction->text, sizeof reaction->text, "%s", "\xF0\x9F\x91\x8D");
    }
    mesh_ui_store_set_messages(&store, &messages);

    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation)) {
        failure = "the direct conversation disappeared";
        goto cleanup;
    }
    if (conversation.unread != 1U) {
        failure = "reactions should not raise the unread count";
        goto cleanup;
    }
    if (strcmp(conversation.preview, "just you") != 0) {
        failure = "a reaction should not become the conversation's preview";
        goto cleanup;
    }
    if (conversation.message_count != 1U) {
        failure = "a reaction should not be counted as a message in the conversation";
        goto cleanup;
    }

    /* And the thread itself shows the message, not the reactions. */
    struct mesh_ui_nav nav;
    memset(&nav, 0, sizeof nav);
    nav.thread_open = true;
    nav.target_node = 0x3000U;
    uint32_t indices[MESH_UI_MAX_MESSAGES];
    const uint32_t shown =
        mesh_ui_nav_filter_messages(&nav, &store.messages, indices, MESH_UI_MAX_MESSAGES);
    if (shown != 1U || store.messages.entries[indices[0]].packet_id != 12U) {
        failure = "the thread should show the message and neither of its reactions";
        goto cleanup;
    }

    /* All-traffic takes everything, and still not these. */
    memset(&nav, 0, sizeof nav);
    nav.thread_open = true;
    nav.inbox = true;
    const uint32_t all =
        mesh_ui_nav_filter_messages(&nav, &store.messages, indices, MESH_UI_MAX_MESSAGES);
    if (all != 2U) {
        failure = "all-traffic should carry the two messages and neither reaction";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

/* The two cells an avatar shows, and the seed that colours it. */
MESH_TEST_CASE(ui_nav_conversation_avatars, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_conversation conversation;

    /* All traffic and New message are not people: a mark rather than initials, and no seed. */
    if (!mesh_ui_nav_conversation_at(&store, 0U, &conversation) ||
        strcmp(conversation.initials, "*") != 0) {
        failure = "all traffic should be marked rather than lettered";
        goto cleanup;
    }
    /* A channel shows the '#' its name already leads with, seeded by the slot rather than the
       name so that renaming it keeps the colour. */
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) ||
        strcmp(conversation.initials, "#") != 0 || conversation.tint == 0U) {
        failure = "a channel should show '#' with a seed of its own";
        goto cleanup;
    }
    /* A Meshtastic short name is one word of four, so the initials are its first half. */
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) ||
        strcmp(conversation.name, "BRVO") != 0 || strcmp(conversation.initials, "BR") != 0 ||
        conversation.tint != 0x3000U) {
        failure = "a direct conversation should take two letters and the node number as its seed";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 3U, &conversation) ||
        strcmp(conversation.initials, "+") != 0) {
        failure = "the New message row should show a plus";
        goto cleanup;
    }

    /* A node with no User falls back to its "!hex" id, and the '!' is punctuation rather than
       a letter - so the disc reads "A1", not "!A". A two-word long name gives one letter from
       each word, and both are upper-cased. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.node_count = 2U;
    handshake.nodes[1].node_id = 0x2000U;
    handshake.nodes[1].short_name[0] = '\0';
    snprintf(handshake.nodes[1].long_name, sizeof handshake.nodes[1].long_name, "%s", "alfa ridge");
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof messages);
    messages.count = 2U;
    messages.entries[0].packet_id = 21U;
    messages.entries[0].peer = 0x2000U;
    snprintf(messages.entries[0].text, sizeof messages.entries[0].text, "%s", "from alfa");
    messages.entries[1].packet_id = 22U;
    messages.entries[1].peer = 0x9ABCDEF0U; /* nothing in the roster knows this one */
    snprintf(messages.entries[1].text, sizeof messages.entries[1].text, "%s", "from a stranger");
    mesh_ui_store_set_messages(&store, &messages);

    /* Newest traffic first, so the stranger is above Alfa. */
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) ||
        strcmp(conversation.initials, "9A") != 0) {
        failure = "a bare node id should letter from its hex, skipping the '!'";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 3U, &conversation) ||
        strcmp(conversation.name, "alfa ridge") != 0 || strcmp(conversation.initials, "AR") != 0) {
        failure = "a two-word name should give one upper-cased letter from each word";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * X on the conversation list: one press arms, the second asks the app to delete.
 *
 * The arming names the conversation rather than the row, because the direct peers are ordered
 * by recency - one message from somebody else re-ranks them, and a row index armed a moment
 * ago can be a different conversation by the time the second press lands.
 */
MESH_TEST_CASE(ui_nav_delete_conversation, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    struct mesh_ui_conversation conversation;

    /* Neither of the two rows that are not conversations answers to X at all. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (store.nav.messages_delete_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "X on the all-traffic row should do nothing";
        goto cleanup;
    }

    /* Down twice to BRVO. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (!mesh_ui_nav_conversation_at(&store, store.nav.cursor[MESH_UI_SCREEN_MESSAGES],
                                     &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_DIRECT) {
        failure = "expected the cursor to be on BRVO";
        goto cleanup;
    }

    memset(&action, 0, sizeof action);
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action) ||
        !store.nav.messages_delete_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "the first X should arm the delete and ask for nothing";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_is_armed(&store.nav, &conversation)) {
        failure = "the armed conversation should be the one under the cursor";
        goto cleanup;
    }

    /* Any other press stands it down, and the arming does not survive to the next X. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    if (store.nav.messages_delete_armed) {
        failure = "moving the cursor should stand the delete down";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);

    /* Arm again, then carry it out. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    memset(&action, 0, sizeof action);
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action) ||
        action.type != MESH_UI_ACTION_DELETE_CONVERSATION) {
        failure = "the second X should ask the app to delete the conversation";
        goto cleanup;
    }
    if (action.number != (uint32_t)MESH_UI_CONVERSATION_DIRECT || action.dest != 0x3000U ||
        strcmp(action.text, "BRVO") != 0) {
        failure = "the action should name the conversation and carry the name off its row";
        goto cleanup;
    }
    if (store.nav.messages_delete_armed) {
        failure = "carrying the delete out should stand the arming down";
        goto cleanup;
    }

    /* And nothing has been deleted here: the store still holds both messages until the app
       comes back with a shorter log. */
    if (store.messages.count != 2U) {
        failure = "the nav should not have touched the message log itself";
        goto cleanup;
    }

    /* Inside a thread X means nothing, so the list's delete cannot be reached from there. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (store.nav.messages_delete_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "X inside an open thread should do nothing";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A node wears the same disc wherever it is listed.
 *
 * The Messages tab, the Nodes tab and the send-to picker all draw a tinted disc with two cells
 * in it, and the whole job of that disc is to be recognised across the three - so the two cells
 * and the tint have to be derived from the *node*, never from the string a screen happens to be
 * showing. The picker's row reads "ALFA  Alfa Node", whose first two words both begin with A;
 * initials taken from that read "AA" while Messages shows "AL" for the same radio.
 *
 * That is what mesh_ui_nav_target_avatar() exists to prevent, and this is the case it was
 * written for.
 */
MESH_TEST_CASE(ui_nav_avatar_is_stable_across_lists, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* The initials rule itself: one word gives its first half, two words give their firsts,
       and punctuation is skipped rather than spent. */
    char cells[MESH_UI_CONVERSATION_INITIALS_MAX];
    mesh_ui_nav_initials("BRVO", cells, sizeof cells);
    if (strcmp(cells, "BR") != 0) {
        failure = "a one-word name should give its first two cells";
        goto cleanup;
    }
    mesh_ui_nav_initials("Home Base", cells, sizeof cells);
    if (strcmp(cells, "HB") != 0) {
        failure = "a two-word name should give the first letter of each";
        goto cleanup;
    }
    mesh_ui_nav_initials("!a1b2c3d4", cells, sizeof cells);
    if (strcmp(cells, "A1") != 0) {
        failure = "a '!hex' fallback name should skip the '!' and upper-case";
        goto cleanup;
    }
    mesh_ui_nav_initials("", cells, sizeof cells);
    if (cells[0] != '\0') {
        failure = "a nameless node should draw an empty disc rather than a wrong one";
        goto cleanup;
    }

    /* Now the agreement that matters. Find ALFA in both lists and compare what each would
       draw - the fixture gives it the long name that makes the naive derivation wrong. */
    char picker_cells[MESH_UI_CONVERSATION_INITIALS_MAX];
    char picker_name[96];
    uint32_t picker_tint = 0U;
    bool found = false;
    const uint32_t rows = mesh_ui_nav_picker_count(&store);
    for (uint32_t i = 0; i < rows; ++i) {
        uint32_t node = 0U;
        uint8_t channel = 0U;
        if (!mesh_ui_nav_picker_row(&store, i, &node, &channel, picker_name, sizeof picker_name)) {
            break;
        }
        if (node != 0x2000U) {
            continue;
        }
        mesh_ui_nav_target_avatar(&store, node, channel, picker_cells, sizeof picker_cells,
                                  &picker_tint);
        found = true;
        break;
    }
    if (!found) {
        failure = "the picker should list ALFA";
        goto cleanup;
    }
    /* The premise of the test: the picker's display name is the one that derives wrongly. */
    mesh_ui_nav_initials(picker_name, cells, sizeof cells);
    if (strcmp(cells, "AL") == 0) {
        failure = "the picker's row name no longer reproduces the bug this test pins";
        goto cleanup;
    }
    if (strcmp(picker_cells, "AL") != 0) {
        failure = "the picker's disc should come from the node's own name, not the row's";
        goto cleanup;
    }
    if (picker_tint != 0x2000U) {
        failure = "a node's tint is its number, so it survives a rename";
        goto cleanup;
    }

    /* A channel is a place: the mark rather than initials, seeded by its slot so renaming it
       keeps the colour. */
    mesh_ui_nav_target_avatar(&store, MESH_MESSAGE_BROADCAST_ADDR, 3U, picker_cells,
                              sizeof picker_cells, &picker_tint);
    if (strcmp(picker_cells, "#") != 0 || picker_tint != 0x0C000003U) {
        failure = "a channel should wear '#' seeded by its slot";
        goto cleanup;
    }

    /*
     * The fallback order is the node's, not any one field's. A roster entry with no short name
     * is *shown* as the "----" placeholder, which has no letters in it at all - so an avatar
     * derived from what the row displays would be empty while the same node wears its long
     * name's initials everywhere else.
     */
    struct mesh_ui_handshake_state *hs = &store.handshake;
    memset(&hs->nodes[1].short_name, 0, sizeof hs->nodes[1].short_name);
    mesh_ui_nav_target_avatar(&store, 0x2000U, 0U, picker_cells, sizeof picker_cells, NULL);
    if (strcmp(picker_cells, "AN") != 0) {
        failure = "a node with no short name should fall through to its long name";
        goto cleanup;
    }
    memset(&hs->nodes[1].long_name, 0, sizeof hs->nodes[1].long_name);
    mesh_ui_nav_target_avatar(&store, 0x2000U, 0U, picker_cells, sizeof picker_cells, NULL);
    if (picker_cells[0] == '\0') {
        failure = "a node with no names at all should still wear its '!hex' id, not an empty disc";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Muting a conversation: what START on the list asks for, what the store does with it, and the
 * two things it takes away - the tab's total and the row's emphasis - against the one it does
 * not, which is the row's own count.
 */
MESH_TEST_CASE(ui_nav_mute_conversation, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    struct mesh_ui_conversation conversation;

    if (mesh_ui_nav_unread_total(&store) != 2U) {
        failure = "the fixture should start with two unread conversations";
        goto cleanup;
    }

    /* START on "All traffic" is not a press: it is a view over the conversations, not one. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type == MESH_UI_ACTION_MUTE_CONVERSATION) {
        failure = "the all-traffic row has no mute to offer";
        goto cleanup;
    }

    /* START on the channel asks the app to flip it, naming the row it was pressed on. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_MUTE_CONVERSATION ||
        action.number != (uint32_t)MESH_UI_CONVERSATION_CHANNEL) {
        failure = "START on a channel should ask for that channel's mute";
        goto cleanup;
    }

    /* The app's half of it. The row still counts its one message; the total no longer does. */
    if (!mesh_ui_store_set_conversation_mute(&store, (uint8_t)action.number, action.dest,
                                             action.channel, true)) {
        failure = "muting a conversation for the first time should change something";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 1U, &conversation) || !conversation.muted ||
        conversation.unread != 1U) {
        failure = "a muted row should say so and still count what is waiting in it";
        goto cleanup;
    }
    if (mesh_ui_nav_unread_total(&store) != 1U) {
        failure = "a muted conversation should not reach the tab's badge";
        goto cleanup;
    }
    /* Setting it again is not a change, so the app can skip a repaint and a cache write. */
    if (mesh_ui_store_set_conversation_mute(&store, (uint8_t)action.number, action.dest,
                                            action.channel, true)) {
        failure = "muting an already-muted conversation should report no change";
        goto cleanup;
    }

    /*
     * The radio's own per-node mute is the other half of the predicate, and only for a direct
     * conversation: upstream's is_muted means the node "will not trigger a notification", so a
     * client that announced it anyway would be contradicting the radio in front of the user.
     */
    store.handshake.nodes[2].is_muted = true;
    mesh_ui_store_set_handshake(&store, &store.handshake);
    if (!mesh_ui_store_conversation_muted(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U,
                                          0U)) {
        failure = "the radio's node mute should mute that node's conversation";
        goto cleanup;
    }
    if (mesh_ui_store_conversation_muted_locally(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT,
                                                 0x3000U, 0U)) {
        failure = "the radio's mute is not this client's own";
        goto cleanup;
    }
    if (mesh_ui_nav_unread_total(&store) != 0U) {
        failure = "a node the radio mutes should not reach the badge either";
        goto cleanup;
    }
    /* A channel has no node, so the roster can say nothing about one. */
    store.handshake.nodes[1].is_muted = true;
    mesh_ui_store_set_handshake(&store, &store.handshake);
    if (mesh_ui_store_conversation_muted(&store, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, 0x2000U,
                                         5U)) {
        failure = "a node's mute must not mute a channel it happens to talk on";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A mute outlives a restart, and outlives the read mark it shares a slot with: a conversation
 * muted before it was ever read has no packet id to be saved under, and a loader that took the
 * id as the price of a line would unmute it on the next launch.
 */
MESH_TEST_CASE(ui_nav_mute_survives_the_cache, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    char cache_path[] = "/tmp/mesh_ui_muteXXXXXX";
    int fd = mkstemp(cache_path);
    if (fd < 0) {
        record_failure(test_name, "failed to create a temp cache file");
        mesh_ui_store_shutdown(&store);
        return;
    }
    close(fd);

    /* Muted, never read: the mark carries a mute and a packet id of 0. */
    (void)mesh_ui_store_set_conversation_mute(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U,
                                              0U, true);
    if (mesh_ui_store_save(&store, cache_path) != 0) {
        failure = "saving the cache failed";
        goto cleanup;
    }

    struct mesh_ui_store reloaded;
    if (mesh_ui_store_init(&reloaded) != 0) {
        failure = "second store init failed";
        goto cleanup;
    }
    if (mesh_ui_store_load(&reloaded, cache_path) != 0) {
        failure = "loading the cache failed";
        mesh_ui_store_shutdown(&reloaded);
        goto cleanup;
    }
    if (!mesh_ui_store_conversation_muted_locally(&reloaded, (uint8_t)MESH_UI_CONVERSATION_DIRECT,
                                                  0x3000U, 0U)) {
        failure = "a mute with no read mark behind it should survive the cache";
    }
    mesh_ui_store_shutdown(&reloaded);

cleanup:
    (void)unlink(cache_path);
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The transcript's "new from here" line: opening a thread records where the reader *was*, and
 * keeps it while they are in there - the store marks the conversation read on the very next
 * publish, so a divider derived from the live mark would sit under the newest bubble instead.
 */
MESH_TEST_CASE(ui_nav_unread_divider_marks_where_the_reader_was, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;

    /* Nothing read yet, so there is no line to rule: a divider above the first bubble would
       separate the transcript from nothing. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.thread_unread_from != 0U) {
        failure = "a conversation opened for the first time has no read mark to rule under";
        goto cleanup;
    }
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    /* The publish has marked it read, up to packet 12 - BRVO's one message. */
    if (mesh_ui_store_conversation_read_mark(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U,
                                             0U) != 12U) {
        failure = "the open conversation should have been marked read";
        goto cleanup;
    }
    /* And the divider has *not* moved with it: the reader is still standing in there. */
    if (store.nav.thread_unread_from != 0U) {
        failure = "the mark moving must not move the line the reader came in at";
        goto cleanup;
    }

    /* Leave, let something arrive, come back: now the line has somewhere to go. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.thread_unread_from != 12U) {
        failure = "reopening a read conversation should rule the line under what was read";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.thread_unread_from != 0U) {
        failure = "leaving a thread should forget where its line was";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A read mark never lands on a reaction. The unread count walks the log ignoring them and looks
 * for the marked packet to know where "read" stops, so a mark on a tapback is one that walk can
 * never meet - and the conversation stays badged however often it is opened.
 */
MESH_TEST_CASE(ui_nav_read_mark_skips_a_reaction, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;
    struct mesh_ui_conversation conversation;

    /* A tapback on BRVO's message, arriving after it - the newest entry in that conversation. */
    struct mesh_ui_message_list messages = store.messages;
    struct mesh_ui_message *reaction = &messages.entries[messages.count++];
    memset(reaction, 0, sizeof *reaction);
    reaction->packet_id = 99U;
    reaction->peer = 0x3000U;
    reaction->direction = MESH_MESSAGE_INBOUND;
    reaction->is_reaction = true;
    reaction->reply_id = 12U;
    snprintf(reaction->peer_name, sizeof reaction->peer_name, "%s", "BRVO");
    snprintf(reaction->text, sizeof reaction->text, "%s", "\xF0\x9F\x91\x8D");
    mesh_ui_store_set_messages(&store, &messages);

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);

    if (mesh_ui_store_conversation_read_mark(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U,
                                             0U) != 12U) {
        failure = "the mark should land on the newest message, not on the tapback after it";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_at(&store, 2U, &conversation) || conversation.unread != 0U) {
        failure = "a conversation whose newest entry is a reaction should still clear its badge";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The snackbar's queue: a second notice waits rather than overwriting the first, the tick that
 * retires one promotes the next, a repeat of what is already up is dropped, and a full queue
 * loses its oldest waiting entry rather than the newest thing that happened.
 */
MESH_TEST_CASE(ui_nav_toasts_queue_rather_than_overwrite, unit) {
    const char *failure = NULL;
    struct mesh_ui_nav nav;
    mesh_ui_nav_init(&nav);

    mesh_ui_nav_post_toast(&nav, 1000U, "first");
    mesh_ui_nav_post_toast(&nav, 1000U, "second");
    if (strcmp(nav.toast, "first") != 0 || nav.toast_queued != 1U) {
        failure = "a notice arriving while one is up should wait behind it";
        goto done;
    }
    /* A repeat of what is showing is one notice standing twice as long, not two events. */
    mesh_ui_nav_post_toast(&nav, 1000U, "second");
    if (nav.toast_queued != 1U) {
        failure = "a repeat of the newest waiting notice should be dropped";
        goto done;
    }
    /*
     * A press does not wait. "Connecting to X" giving way to "X needs pairing" is one sentence
     * finishing rather than two events, and four seconds of the optimistic half before the true
     * one is worse than losing it - so the setter still replaces, and what was waiting still is.
     */
    mesh_ui_nav_set_toast(&nav, 1200U, "a press answered");
    if (strcmp(nav.toast, "a press answered") != 0 || nav.toast_queued != 1U) {
        failure = "a press should take the snackbar without discarding what was waiting";
        goto done;
    }
    mesh_ui_nav_set_toast(&nav, 1200U, "first");

    /* Nothing moves until the showing notice has stood its four seconds. */
    if (mesh_ui_nav_tick(&nav, 2000U) || strcmp(nav.toast, "first") != 0) {
        failure = "a notice should not be cut short by the one waiting behind it";
        goto done;
    }
    if (!mesh_ui_nav_tick(&nav, 5201U) || strcmp(nav.toast, "second") != 0) {
        failure = "the tick that retires a notice should promote the next";
        goto done;
    }
    /* Dated from the promotion rather than from when it was raised: it is standing now, and a
       deadline the backend has not seen is how the snackbar tells one notice from the next. */
    if (nav.toast_until_ms <= 5201U || nav.toast_queued != 0U) {
        failure = "a promoted notice should start its own four seconds";
        goto done;
    }
    if (!mesh_ui_nav_tick(&nav, 20000U) || nav.toast[0] != '\0') {
        failure = "an empty queue should let the snackbar go";
        goto done;
    }

    /* A burst longer than the queue keeps the newest, because a notice is only worth showing
       while it is still news. */
    mesh_ui_nav_post_toast(&nav, 30000U, "showing");
    mesh_ui_nav_post_toast(&nav, 30000U, "a");
    mesh_ui_nav_post_toast(&nav, 30000U, "b");
    mesh_ui_nav_post_toast(&nav, 30000U, "c");
    mesh_ui_nav_post_toast(&nav, 30000U, "d");
    if (nav.toast_queued != MESH_UI_NAV_TOAST_QUEUE || strcmp(nav.toast_queue[0], "b") != 0 ||
        strcmp(nav.toast_queue[MESH_UI_NAV_TOAST_QUEUE - 1U], "d") != 0) {
        failure = "a full queue should drop its oldest waiting notice, not its newest";
        goto done;
    }
    /* Clearing clears the backlog with it, or the client starts talking again a moment later. */
    mesh_ui_nav_set_toast(&nav, 30000U, NULL);
    if (nav.toast[0] != '\0' || nav.toast_queued != 0U) {
        failure = "clearing the notice should clear what was waiting behind it";
    }

done:
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
