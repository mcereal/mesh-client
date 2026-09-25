#define _POSIX_C_SOURCE 200809L

/* Navigating conversations: tabs, unread counts, channels and the keyboard. */

#include "inkcell/ui/emoji.h"
#include "inkwell/base/array.h"
#include "inkwell/base/text.h"

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/ui/commands.h"
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action) ||
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (!store.nav.keyboard_open || store.nav.compose_open ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "Y in a conversation should open the keyboard, not the compose overlay";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.keyboard_open || store.nav.compose_open || !store.nav.thread_open) {
        failure = "B on an empty draft should leave the thread showing";
        goto cleanup;
    }

    /* A replies: the canned list over the thread, which needs no destination of its own. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.compose_open || store.nav.keyboard_open ||
        store.nav.compose_cursor != MESH_UI_COMPOSE_FIRST_CANNED ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "A in a conversation should open the compose overlay";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.dest != 0x3000U || action.channel != 0U ||
        strcmp(action.text, mesh_ui_canned_text(0)) != 0 || store.nav.compose_open ||
        !store.nav.thread_open) {
        failure = "a canned row should send to the open thread and close the overlay";
        goto cleanup;
    }

    /* B leaves the thread for the conversation list, on the row it was opened from. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 2U) {
        failure = "B should return to BRVO's row in the conversation list";
        goto cleanup;
    }

    /* The all-traffic row is a view over everything: A there drills into the conversation the
       selected line belongs to rather than guessing a destination. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action); /* the broadcast from ALFA */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.inbox ||
        store.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR || store.nav.target_channel != 0U) {
        failure = "A in all traffic should open the conversation the line belongs to";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* Y on the list (and A on the New message row) opens the picker, which both retargets and
       opens the conversation; what lands over it is whichever key asked. LEFT/RIGHT page the
       picker instead of switching tabs. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (!store.nav.picker_open ||
        store.nav.picker_follow != (uint8_t)MESH_UI_PICKER_FOLLOW_KEYBOARD ||
        mesh_ui_nav_picker_count(&store) != 3U) {
        failure = "Y on the conversation list should open the send-to picker";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.picker_open) {
        failure = "LEFT/RIGHT in the picker must page, not switch tabs";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.picker_open || store.nav.picker_follow != (uint8_t)MESH_UI_PICKER_FOLLOW_NONE ||
        store.nav.thread_open) {
        failure = "B should cancel the picker and leave the list showing";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action); /* the New message row */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.picker_open || store.nav.picker_cursor != 0U) {
        failure = "the New message row should open the picker";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.picker_open || store.nav.target_node != 0x2000U ||
        strcmp(store.nav.target_name, "ALFA") != 0 || !store.nav.thread_open ||
        !store.nav.compose_open || store.nav.keyboard_open) {
        failure = "picking from the New message row should land on the quick replies";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action); /* close compose */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action); /* close the thread */
    if (store.nav.compose_open || store.nav.thread_open) {
        failure = "B should back out of the overlay and then the thread";
        goto cleanup;
    }

    /* Tabs wrap in both directions; L1/R1 mirror Left/Right. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        failure = "LEFT from the first tab should wrap to the last";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "R1 from the last tab should wrap to the first";
        goto cleanup;
    }

    /* Nodes tab: A opens the node's detail; the detail's first row opens its conversation. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_NODES) {
        failure = "RIGHT should reach Nodes";
        goto cleanup;
    }
    /* The list's first two rows are the filter chips and the map, not nodes - so it takes
       MESH_UI_NODES_LEAD_ROWS steps down to reach one. */
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    /* Our own node has a detail too - it is the one battery the user can do something about -
       but no "Message this node" row, so A inside it does nothing. */
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action) || !store.nav.node_detail_open) {
        failure = "A on our own node should open its detail";
        goto cleanup;
    }
    if (mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action) || store.nav.thread_open) {
        failure = "our own node's detail should offer nothing to message";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.node_detail_open ||
        store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_LEAD_ROWS) {
        failure = "B should back out of the detail onto the node it came from";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action); /* clamps at the last row */
    /* Three nodes under the two lead rows, so the last row is one before their sum. */
    const uint32_t last_node_row = MESH_UI_NODES_LEAD_ROWS + 3U - 1U;
    if (store.nav.cursor[MESH_UI_SCREEN_NODES] != last_node_row) {
        failure = "DOWN must clamp at the last node";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    /* Row 0 of the detail is the row that opens the node's verbs, and it is a row the cursor
       may stand on - so opening a node lands on it rather than one under it. */
    if (!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U ||
        store.nav.node_list_cursor != last_node_row ||
        store.nav.cursor[MESH_UI_SCREEN_NODES] != 0U) {
        failure = "A on a node should open that node's detail";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.node_actions_open || store.nav.node_actions_cursor != 0U) {
        failure = "the detail's first row should open the node's verbs, at the top of them";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.thread_open ||
        store.nav.compose_open || store.nav.target_node != 0x3000U ||
        strcmp(store.nav.target_name, "BRVO") != 0) {
        failure = "the sheet's first verb should open its conversation, not compose";
        goto cleanup;
    }
    /* Y goes one step further and opens the keyboard over it, from either level: the hint on
       both screens says "write", and writing is typing. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);     /* back to the list */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* Nodes, as we left it */
    /* Which is the sheet of verbs, still open behind the conversation the last press opened.
       Y is not named there - "write" is a row on that screen - so this leaves it first, which is
       what the reader pressing Y from a node's detail has done too. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.node_actions_open || !store.nav.node_detail_open) {
        failure = "B on the sheet should land back on the detail, not on the list";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.thread_open ||
        !store.nav.keyboard_open || store.nav.compose_open || store.nav.target_node != 0x3000U) {
        failure = "Y in a node's detail should open its conversation ready to type";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* Nodes */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);     /* close the detail */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || !store.nav.keyboard_open ||
        store.nav.compose_open || store.nav.target_node != 0x3000U) {
        failure = "Y on the node list should open its conversation ready to type";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* Nodes */

    /* The places are a row of the Nodes list, and their list is never empty - the row that
       makes a place is always there, so A lands on a screen with something under the cursor
       even on a mesh that has shared nothing. B goes back to the roster, on the row it left. */
    if (!mesh_test_open_waypoints(&store) ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES) != 1U) {
        failure = "the Waypoints row should open the places, which always offer their new row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (mesh_ui_nav_waypoints_showing(&store.nav) ||
        store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_WAYPOINTS_ROW) {
        failure = "B on the places should land on the roster's Waypoints row";
        goto cleanup;
    }

    /*
     * The Radio tab opens on the Status cards, which have no list: their rows are the verbs the
     * cards offer, walked flat. The fixture has a radio attached and a completed handshake, so
     * all five are on offer - Devices and Disconnect on the Link card, the node lists on the
     * Mesh card, and the details and Refresh on the Radio card - and a fresh cursor stands on
     * the first.
     */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_RADIO ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_RADIO) != 5U ||
        store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_DEVICES) {
        failure = "RIGHT from Nodes should reach the Radio tab's cards, on Devices";
        goto cleanup;
    }

    /* The device list, one level in: A connects to an unconnected device and does nothing on
       the connected one, and B goes back to the cards on the verb that opened it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!mesh_ui_nav_devices_showing(&store.nav) || action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the Devices verb should open the device list and ask nothing";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the connected device should not reconnect";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_CONNECT ||
        strcmp(action.identifier, "AA:BB:CC:DD:EE:02") != 0) {
        failure = "A on another device should request a connect";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (!mesh_ui_nav_status_showing(&store.nav) ||
        store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_DEVICES) {
        failure = "B on the device list should land back on the cards, on Devices";
        goto cleanup;
    }

    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_DISCONNECT ||
        strcmp(action.identifier, "AA:BB:CC:DD:EE:01") != 0) {
        failure = "A on the Link card's Disconnect should drop the link it names";
        goto cleanup;
    }
    /* The Mesh card's node lists and the Radio card's details: each a page one level in, opened
       without asking the radio for anything, and B lands back on the verb that opened it. */
    const uint8_t pages[] = {(uint8_t)MESH_UI_STATUS_VERB_NODE_LISTS,
                             (uint8_t)MESH_UI_STATUS_VERB_DETAILS};
    const uint8_t sections[] = {(uint8_t)MESH_UI_SETTINGS_NODE_LISTS,
                                (uint8_t)MESH_UI_SETTINGS_RADIO_DETAILS};
    for (size_t i = 0; i < sizeof pages; ++i) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
        memset(&action, 0, sizeof action);
        mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
        if (store.nav.status_verb != pages[i] || action.type != MESH_UI_ACTION_NONE ||
            mesh_ui_nav_open_section(&store.nav) != sections[i]) {
            failure = "A on a card's page verb should open that page and ask nothing";
            goto cleanup;
        }
        mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
        if (!mesh_ui_nav_status_showing(&store.nav) || store.nav.status_verb != pages[i]) {
            failure = "B on a page should land back on the cards, on the verb that opened it";
            goto cleanup;
        }
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH ||
        action.type != MESH_UI_ACTION_REFRESH_SETTINGS) {
        failure = "A on the Radio card should re-read the configuration";
        goto cleanup;
    }
    /* And the cursor stops there: two verbs, no third card to step onto. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH) {
        failure = "DOWN must clamp at the last verb on the Status cards";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);

    /* Back in the all-traffic thread, a cursor on the newest line follows new traffic; one
       that was moved up stays where it was. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* Settings */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* wraps to Messages */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action); /* back to row 0 */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);  /* the all-traffic row */
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
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
    if (strcmp(snapshot.nav.toast.text, "Sent to BRVO") != 0) {
        failure = "toast not carried in the snapshot";
        goto cleanup;
    }
    mesh_ui_store_tick(&store, 2000U);
    if (store.nav.toast.text[0] == '\0') {
        failure = "toast expired too early";
        goto cleanup;
    }
    mesh_ui_store_tick(&store, 6000U);
    if (store.nav.toast.text[0] != '\0' || !mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "toast should expire after a few seconds and repaint";
        goto cleanup;
    }
    mesh_ui_store_set_toast(&store, 7000U, "Connecting");
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_SELECT, &action) ||
        store.nav.toast.text[0] != '\0') {
        failure = "any key should dismiss a toast";
        goto cleanup;
    }

    /*
     * A press dismisses what is showing, and what was waiting behind it is next. Stranded in the
     * queue instead, it would sit there unseen - the tick only walks the queue when something is
     * showing - until a later notice went straight up ahead of it and it came back out of order.
     * The next one is dated by the press that uncovered it, as a notice a press raises is.
     */
    mesh_ui_store_tick(&store, 20000U);
    mesh_ui_store_post_toast(&store, 20000U, "first");
    mesh_ui_store_post_toast(&store, 20000U, "second");
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_SELECT, &action) ||
        strcmp(store.nav.toast.text, "second") != 0 || store.nav.toast.queued != 0U ||
        store.nav.toast.until_ms != 20000U + 4000U) {
        failure = "dismissing a notice should hand the snackbar to the one waiting behind it";
        goto cleanup;
    }
    mesh_ui_store_post_toast(&store, 20000U, "third");
    if (strcmp(store.nav.toast.text, "second") != 0 || store.nav.toast.queued != 1U) {
        failure = "a notice arriving after a dismiss should wait behind the one it uncovered";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;

    /* Park the conversation list on #Primary (row 1) without opening it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "expected the conversation list on row 1";
        goto cleanup;
    }

    /* Walking to Nodes and back changes nothing about what Messages shows. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    /* Past the lead rows and our own node, onto BRVO. */
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS + 2U; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "visiting Nodes must not change what Messages shows";
        goto cleanup;
    }

    /* Opening a node's conversation from Nodes (through its detail) is one B away from the list
       again, and the list comes back where it was rather than on the node just visited. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* open the detail */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* its "Actions" row */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* "Message this node" */
    if (!store.nav.thread_open || store.nav.target_node != 0x3000U ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "A on a node should open its conversation on the Messages tab";
        goto cleanup;
    }
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action)) {
        failure = "B should leave the conversation";
        goto cleanup;
    }
    if (store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "B should restore the conversation list where it was";
        goto cleanup;
    }
    /* Nothing is left to back out of, so a second B is inert rather than surprising. */
    if (mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action)) {
        failure = "B on the conversation list should be a no-op";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A draft belongs to the conversation it was started in. One shared buffer put words written to
 * one peer into the next thread opened, and compose opens on a non-empty draft - one press from
 * sending them to somebody else.
 */
MESH_TEST_CASE(ui_nav_draft_follows_its_conversation, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_action action;

    /* BRVO is row 2 and #Primary row 1 (see ui_nav_navigation). Start a message to BRVO and
       leave it unsent: B keeps the draft, a second B leaves the thread. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "meet at the creek");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.thread_open || strcmp(store.nav.draft, "meet at the creek") != 0) {
        failure = "B twice should leave the thread with the draft kept";
        goto cleanup;
    }

    /* Into the channel: nothing of BRVO's is waiting there, so A lands on the canned replies. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR) {
        failure = "expected #Primary open";
        goto cleanup;
    }
    if (store.nav.draft[0] != '\0') {
        failure = "a draft written to one peer must not follow the user into a channel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.compose_open || store.nav.compose_cursor == MESH_UI_COMPOSE_ROW_DRAFT) {
        failure = "compose in a thread with no draft of its own should open on the canned rows";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* The channel gets a draft of its own, and each comes back to its own thread. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "to everyone");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.target_node != 0x3000U || strcmp(store.nav.draft, "meet at the creek") != 0) {
        failure = "going back to BRVO should bring BRVO's draft back";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR ||
        strcmp(store.nav.draft, "to everyone") != 0) {
        failure = "the channel's own draft should come back";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The triggers in a thread: R2 to the newest bubble, L2 to the first unread one and then the
 * oldest. A thread opens on its newest bubble, and the "New" line is otherwise a long walk up.
 */
MESH_TEST_CASE(ui_nav_thread_triggers_jump_to_unread_and_newest, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof messages);
    messages.count = 6U;
    for (uint32_t i = 0; i < messages.count; ++i) {
        messages.entries[i].packet_id = 100U + i;
        messages.entries[i].peer = 0x3000U;
        messages.entries[i].direction = MESH_MESSAGE_INBOUND;
        snprintf(messages.entries[i].peer_name, sizeof messages.entries[i].peer_name, "%s", "BRVO");
        snprintf(messages.entries[i].text, sizeof messages.entries[i].text, "line %u", (unsigned)i);
    }
    mesh_ui_store_set_messages(&store, &messages);

    struct mesh_ui_action action;
    struct mesh_ui_snapshot snapshot;
    /* Onto BRVO's row (row 2) and in. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    const uint32_t *cursor = &store.nav.cursor[MESH_UI_SCREEN_MESSAGES];
    if (!store.nav.thread_open || store.nav.target_node != 0x3000U || *cursor != 5U) {
        failure = "expected BRVO's thread open on its newest bubble";
        goto cleanup;
    }

    /* Read up to line 2 when the thread was opened: the first unread bubble is row 3. */
    store.nav.thread_unread_from = 102U;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_L2, &action);
    if (*cursor != 3U) {
        failure = "L2 should stop on the first unread bubble";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_L2, &action);
    if (*cursor != 0U) {
        failure = "a second L2 should go on to the oldest bubble";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R2, &action);
    if (*cursor != 5U) {
        failure = "R2 should go to the newest bubble";
        goto cleanup;
    }
    /* With nothing unread, L2 is simply the top. */
    store.nav.thread_unread_from = 0U;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_L2, &action);
    if (*cursor != 0U || !store.nav.thread_open) {
        failure = "with no unread mark L2 should go to the oldest bubble, in the thread";
        goto cleanup;
    }

    /* The bar names the pair, on the triggers. */
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    struct mesh_ui_command_set commands;
    mesh_ui_commands_for(&snapshot, &commands);
    const struct mesh_ui_command *jump =
        mesh_ui_commands_find(&commands, MESH_UI_COMMAND_THREAD_JUMP);
    if (jump == NULL || jump->button != INKCELL_BUTTON_TRIGGERS) {
        failure = "a thread's bar should offer the jump on the triggers";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The open thread's row in the conversation list is found by what the thread is, not by where the
 * list was parked - the list beside a thread on a wide window reads it, and a peer's row moves
 * with every message that re-ranks the peers.
 */
MESH_TEST_CASE(ui_nav_open_conversation_row_follows_the_thread, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* Where each kind of conversation actually is. */
    uint32_t brvo = UINT32_MAX;
    uint32_t primary = UINT32_MAX;
    const uint32_t count = mesh_ui_nav_conversation_count(&store);
    for (uint32_t i = 0U; i < count; ++i) {
        struct mesh_ui_conversation conversation;
        if (!mesh_ui_nav_conversation_at(&store, i, &conversation)) {
            break;
        }
        if (conversation.kind == MESH_UI_CONVERSATION_DIRECT && conversation.node == 0x3000U) {
            brvo = i;
        }
        if (conversation.kind == MESH_UI_CONVERSATION_CHANNEL && conversation.channel == 0U) {
            primary = i;
        }
    }
    if (brvo == UINT32_MAX || primary == UINT32_MAX) {
        failure = "the fixture should list BRVO and the primary channel";
        goto cleanup;
    }

    /* A thread opened from somewhere else - the Nodes tab's "Message this node" - with the list
       parked on the primary channel: the row is BRVO's, not the parked one. */
    struct mesh_ui_action action;
    while (store.nav.cursor[MESH_UI_SCREEN_MESSAGES] < primary) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS + 2U; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* the detail */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* its "Actions" row */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* "Message this node" */
    if (!store.nav.thread_open || store.nav.target_node != 0x3000U ||
        store.nav.conversation_list_cursor != primary) {
        failure = "the test needs BRVO's thread open over a list parked on the channel";
        goto cleanup;
    }
    if (mesh_ui_nav_open_conversation_row(&store.nav, &store) != brvo) {
        failure = "a direct thread's row should be its peer's, wherever the list was parked";
        goto cleanup;
    }

    /* B back to the list, and A on the channel it was parked on. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open ||
        mesh_ui_nav_open_conversation_row(&store.nav, &store) != primary) {
        failure = "a channel thread's row should be that channel's";
        goto cleanup;
    }

    /* And all traffic, which is the first row. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    while (store.nav.cursor[MESH_UI_SCREEN_MESSAGES] > 0U) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open || !store.nav.inbox ||
        mesh_ui_nav_open_conversation_row(&store.nav, &store) != 0U) {
        failure = "all traffic is the first row";
        goto cleanup;
    }

    /* With nothing open it is the parked row, whatever that is. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (mesh_ui_nav_open_conversation_row(&store.nav, &store) !=
        store.nav.conversation_list_cursor) {
        failure = "with no thread open the row is the parked one";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Every cell of this client's emoji pages is a glyph this build can draw.
 *
 * The table is written as code points rather than pasted in, which keeps a patch tool from
 * mangling it and puts the other failure in its place: a code point nobody checked against the
 * sprite table draws as a replacement box, and goes on the air as a character the reader's
 * client may well render fine - so the keyboard is the only place it looks broken, and only on
 * the device.
 *
 * The check itself is inkcell's, which is where it has to live: the glyph tables are inkcell's,
 * so this client walking its own cells against them was a test reaching down a layer to read
 * data it does not own. What is left here is the assertion - these pages, this build - because
 * the pages are the half inkcell deliberately does not carry.
 */
MESH_TEST_CASE(kb_emoji_cells_are_drawable, unit) {
    struct mesh_ui_nav nav;
    memset(&nav, 0, sizeof nav);
    const struct inkcell_keyboard_layout layout = mesh_ui_nav_kb_layout(&nav);

    uint32_t page = 0U;
    uint32_t cell = 0U;
    char message[112];
    snprintf(message, sizeof message, "emoji page %u cell %u is not one drawable glyph", page,
             cell);
    const bool drawable = inkcell_keyboard_layout_drawable(&layout, &page, &cell);
    /* Written after the call as well as before it, so the message names the offender rather
       than the first cell of the first page. */
    snprintf(message, sizeof message, "emoji page %u cell %u is not one drawable glyph", page,
             cell);
    MESH_TEST_FAIL_IF(!drawable, message);
    record_success(test_name);
}

/*
 * The remapped face buttons: X deletes, B leaves and keeps what was typed, and the triggers
 * move the caret.
 *
 * All three are the same complaint - the keyboard used the pad the way nothing else does. B was
 * a backspace on the one screen where B is not "back", X was a shift nobody guessed at, and the
 * two triggers the case carries were read by nothing at all.
 */
MESH_TEST_CASE(kb_face_buttons_follow_the_pad, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    mesh_test_open_tab(&store, MESH_UI_SCREEN_MESSAGES);
    /* Past the all-traffic row, which has no one destination to write to, and into a
       conversation; Y there is the way straight to the keyboard. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (!store.nav.keyboard_open) {
        failure = "Y should open the keyboard on an open thread";
        goto cleanup;
    }

    /* R1 is the capitals, and A takes one and drops back - the one-capital rule the triggers'
       shift had, on the panel step that already reached the same layer. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    if (store.nav.kb.layer != INKCELL_KB_UPPER) {
        failure = "R1 from the letters should reach the capitals";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (strcmp(store.nav.draft, "Q") != 0 || store.nav.kb.layer != INKCELL_KB_LOWER) {
        failure = "a capital should apply to one character and then fall back";
        goto cleanup;
    }

    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* q */
    if (strcmp(store.nav.draft, "Qq") != 0) {
        failure = "A should type the cell under the cursor";
        goto cleanup;
    }
    /* The triggers are the caret: L2 steps back over the q, the next letter goes in before
       it, and R2 steps on again. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_L2, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* w */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (strcmp(store.nav.draft, "Qwq") != 0) {
        failure = "L2 should move the caret back, and A should type there";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R2, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (strcmp(store.nav.draft, "Qw") != 0) {
        failure = "R2 should move the caret on, and X should delete before it";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action); /* q */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (strcmp(store.nav.draft, "Qwq") != 0) {
        failure = "the caret back on the end should type on the end";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* q, back to Qq */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (strcmp(store.nav.draft, "Q") != 0) {
        failure = "X should delete a character";
        goto cleanup;
    }

    /* B leaves, and the draft survives it: the compose sheet is where it lands, with what was
       typed still on the draft row. The grid's own cancel key is what throws text away. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.keyboard_open || strcmp(store.nav.draft, "Q") != 0 ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "B should leave the keyboard and keep the draft";
        goto cleanup;
    }

    /* X with nothing to delete is not a way out. It was, when B did both jobs, and carrying
       that over would mean a backspace that sometimes closes the screen. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action); /* back to the keyboard */
    store.nav.draft[0] = '\0';
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (!store.nav.keyboard_open) {
        failure = "X on an empty draft should not close the keyboard";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Channel table drives the To: cycle and the conversation filter; the keyboard builds a draft. */
MESH_TEST_CASE(ui_nav_channels_and_keyboard, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.target_channel != 1U ||
        strcmp(store.nav.target_name, "#Team") != 0 ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 1U) {
        failure = "A on the #Team row should open that channel";
        goto cleanup;
    }
    uint32_t indices[MESH_UI_MAX_MESSAGES];
    if (mesh_ui_nav_filter_messages(&store.nav, mesh_ui_message_list_view(&store.messages), indices,
                                    MESH_UI_MAX_MESSAGES) != 1U ||
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.dest != MESH_MESSAGE_BROADCAST_ADDR ||
        action.channel != 1U) {
        failure = "canned send should target the open thread's channel";
        goto cleanup;
    }

    /* Keyboard: type "Hi", a space, delete it, a space again, START sends "Hi ". The draft row
       is the overlay's own way in, and it keeps the overlay behind it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action); /* draft row */
    if (store.nav.compose_cursor != MESH_UI_COMPOSE_ROW_DRAFT) {
        failure = "expected the draft row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.keyboard_open || !store.nav.compose_open || store.nav.kb.row != 0U ||
        store.nav.kb.col != 0U) {
        failure = "A on the draft row should open the keyboard at the top-left";
        goto cleanup;
    }
    /* LEFT/RIGHT move within the grid while the keyboard is open, never switch tabs. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.kb.col != INKCELL_KB_COLS - 1U || store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "LEFT should wrap to the last column";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* back to col 0 */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action); /* row 2: asdfghjkl' */
    for (int i = 0; i < 5; ++i) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action); /* the capitals */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);  /* H */
    if (strcmp(store.nav.draft, "H") != 0 || store.nav.kb.layer != INKCELL_KB_LOWER) {
        failure = "shift should apply to one character";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action); /* row 1: qwertyuiop */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* col 7: i */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action); /* space */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action); /* delete it */
    if (strcmp(store.nav.draft, "Hi") != 0) {
        failure = "typing/deleting produced the wrong draft";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || strcmp(action.text, "Hi ") != 0 ||
        action.channel != 1U || store.nav.keyboard_open || store.nav.draft[0] != '\0' ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "START should send the draft and return to the conversation";
        goto cleanup;
    }

    /* The action row: moving down from column 9 lands on the last (cancel) key; the mapping
       comes back to a sensible column. Cancel drops the draft and closes the keyboard. B on
       an empty draft also closes it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);    /* keyboard open, row 0 col 0 */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);    /* '1' */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action); /* col 9 */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);   /* wraps to the action row */
    if (store.nav.kb.row != INKCELL_KB_CHAR_ROWS || store.nav.kb.col != INKCELL_KB_ACTIONS - 1U) {
        failure = "column should map onto the action row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* cancel */
    if (store.nav.keyboard_open || store.nav.draft[0] != '\0' || !store.nav.compose_open) {
        failure = "cancel should discard the draft and leave the compose overlay showing";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* reopen (cursor still on draft) */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.keyboard_open) {
        failure = "B with an empty draft should close the keyboard";
        goto cleanup;
    }
    if (mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action) == false &&
        action.type != MESH_UI_ACTION_NONE) {
        failure = "unexpected action";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    /* Into BRVO's conversation, whose one message is packet 12. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.target_node != 0x3000U) {
        failure = "the test needs BRVO's thread open";
        goto cleanup;
    }

    /* A aims at the bubble under the cursor, and the canned row that follows carries it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.compose_open || store.nav.reply_to != 12U) {
        failure = "A should open the compose sheet aimed at the message under the cursor";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (!store.nav.keyboard_open || store.nav.reply_to != 0U) {
        failure = "Y should open the keyboard with nothing to answer";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* X is the tapback: the emoji list, aimed at the same bubble. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (!store.nav.reaction_open || store.nav.reply_to != 12U || store.nav.reaction_cursor != 0U) {
        failure = "X should open the tapback picker on the message under the cursor";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (store.nav.reaction_open || action.type != MESH_UI_ACTION_NONE) {
        failure = "a message with no packet id should not open the tapback picker";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * START on a bubble the mesh came back on: the same words, going out again.
 *
 * A failed message was drawable and nothing else - the error bubble and its mark have been
 * there since the transcript was written, and the only thing a reader could do about one was
 * type it out a second time. What is checked here is that the retry is aimed off the *record*
 * rather than off the nav: where it was going, on which channel, what it said, and what it was
 * answering all come from the bubble, which is what makes a retry of a threaded reply still a
 * reply to the same message rather than to its own failed attempt.
 */
MESH_TEST_CASE(ui_nav_resend_repeats_a_failed_message, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* One more line in BRVO's conversation: ours, answering theirs, and undelivered. */
    struct mesh_ui_message_list messages = store.messages;
    messages.entries[messages.count].packet_id = 13U;
    messages.entries[messages.count].peer = 0x3000U;
    messages.entries[messages.count].direction = MESH_MESSAGE_OUTBOUND;
    messages.entries[messages.count].ack = MESH_MESSAGE_ACK_FAILED;
    messages.entries[messages.count].ack_error = 1U;
    messages.entries[messages.count].reply_id = 12U;
    messages.entries[messages.count].channel = 2U;
    snprintf(messages.entries[messages.count].peer_name,
             sizeof messages.entries[messages.count].peer_name, "%s", "BRVO");
    snprintf(messages.entries[messages.count].text, sizeof messages.entries[messages.count].text,
             "%s", "yes, on my way");
    messages.count++;
    mesh_ui_store_set_messages(&store, &messages);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    /* Into BRVO's conversation: their message, then ours under it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.target_node != 0x3000U) {
        failure = "the test needs BRVO's thread open";
        goto cleanup;
    }

    /* A thread reads from its newest line, so the first press inside one settles the cursor
       there - here that is our own message, and it did not arrive. */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "the test needs the cursor on our own failed message";
        goto cleanup;
    }
    const struct mesh_ui_message *failed =
        mesh_ui_nav_resendable(&store.nav, mesh_ui_message_list_view(&store.messages));
    if (failed == NULL || failed->packet_id != 13U) {
        failure = "the failed bubble under the cursor should be the one offered";
        goto cleanup;
    }

    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_RESEND) {
        failure = "START on a failed bubble should ask the app to send it again";
        goto cleanup;
    }
    if (action.dest != 0x3000U || action.channel != 2U ||
        strcmp(action.text, "yes, on my way") != 0) {
        failure = "the resend should carry the failed message's destination and words";
        goto cleanup;
    }
    if (action.reply_id != 12U) {
        failure = "a retry of a reply answers what the reply answered, not the failed attempt";
        goto cleanup;
    }
    if (action.number != 13U) {
        failure = "the resend should name the attempt that failed";
        goto cleanup;
    }
    if (store.nav.compose_open || store.nav.keyboard_open) {
        failure = "the retry is one press; it should not open anything over the thread";
        goto cleanup;
    }

    /* And the nav has sent nothing and changed nothing about the log: the failed bubble stays
       where it is, because the attempt is part of what happened here. */
    if (store.messages.count != 3U || store.messages.entries[2].ack != MESH_MESSAGE_ACK_FAILED) {
        failure = "the nav should not have touched the message it asked to have sent again";
        goto cleanup;
    }

    /* One line up is their message, which arrived: there is nothing to retry on it, so START
       goes on standing in for A and opens the compose sheet. */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 0U ||
        mesh_ui_nav_resendable(&store.nav, mesh_ui_message_list_view(&store.messages)) != NULL) {
        failure = "a message we received is not one this client can send again";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_NONE || !store.nav.compose_open) {
        failure = "START on a bubble with nothing to retry should still stand in for A";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* All traffic offers it on no row at all: it is a transcript of several conversations, and
       the verbs about one bubble live in the conversation itself. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* the all-traffic row */
    if (!store.nav.inbox) {
        failure = "the test needs the all-traffic view open";
        goto cleanup;
    }
    store.nav.cursor[MESH_UI_SCREEN_MESSAGES] = 2U;
    if (mesh_ui_nav_resendable(&store.nav, mesh_ui_message_list_view(&store.messages)) != NULL) {
        failure = "all traffic should offer no retry";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * One press is one send, however long the button is held.
 *
 * A button going down and the kernel's autorepeat reach the nav as the same event: only the
 * four directions are filtered, because those are repeated by our own timer instead
 * (inkcell_input_handle_event). Every other press in this client either changes the screen or
 * arms something, so a repeat lands somewhere different - a retry is the first one that leaves
 * the cursor exactly where it was, on a bubble that is still failed until the store catches
 * up. Held, it would put the same words on the air thirty times a second, each a DM asking for
 * an ack. The latch is what makes that one message, and any press that is not START re-arms it
 * so a deliberate second retry still costs one press.
 */
MESH_TEST_CASE(ui_nav_resend_is_one_press_one_send, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_message_list messages = store.messages;
    messages.entries[messages.count].packet_id = 13U;
    messages.entries[messages.count].peer = 0x3000U;
    messages.entries[messages.count].direction = MESH_MESSAGE_OUTBOUND;
    messages.entries[messages.count].ack = MESH_MESSAGE_ACK_FAILED;
    snprintf(messages.entries[messages.count].text, sizeof messages.entries[messages.count].text,
             "%s", "yes, on my way");
    messages.count++;
    mesh_ui_store_set_messages(&store, &messages);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (!store.nav.thread_open || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "the test needs the cursor on our own failed message";
        goto cleanup;
    }

    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_RESEND || !store.nav.resend_spent) {
        failure = "the first START should send and spend the press";
        goto cleanup;
    }

    /*
     * The held button, which is the same event arriving again. The store has not published the
     * new message yet, so the bubble under the cursor is still the failed one - and this is
     * exactly the moment the guard exists for.
     */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "a held START should not send the same message twice";
        goto cleanup;
    }
    if (mesh_ui_nav_resendable(&store.nav, mesh_ui_message_list_view(&store.messages)) != NULL) {
        failure = "the bar should stop naming a press that is spent";
        goto cleanup;
    }
    /* And it stops there rather than falling through to A, which would answer a held button
       with a compose sheet over the conversation. */
    if (store.nav.compose_open) {
        failure = "a held START should not open the compose sheet";
        goto cleanup;
    }

    /*
     * Any other press re-arms it. Down is the one to check, because the cursor is already on
     * the last bubble and cannot move: what stands the latch down is the press arriving, not
     * the screen changing under it.
     */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (store.nav.resend_spent || store.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "a press that is not START should re-arm the retry where it stands";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_RESEND) {
        failure = "a deliberate second retry should still send";
        goto cleanup;
    }

    /* And leaving the conversation takes the latch with it, so the next thread opens armed. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.thread_open || store.nav.resend_spent) {
        failure = "closing the thread should stand the retry down";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
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
    const uint32_t shown = mesh_ui_nav_filter_messages(
        &nav, mesh_ui_message_list_view(&store.messages), indices, MESH_UI_MAX_MESSAGES);
    if (shown != 1U || store.messages.entries[indices[0]].packet_id != 12U) {
        failure = "the thread should show the message and neither of its reactions";
        goto cleanup;
    }

    /* All-traffic takes everything, and still not these. */
    memset(&nav, 0, sizeof nav);
    nav.thread_open = true;
    nav.inbox = true;
    const uint32_t all = mesh_ui_nav_filter_messages(
        &nav, mesh_ui_message_list_view(&store.messages), indices, MESH_UI_MAX_MESSAGES);
    if (all != 2U) {
        failure = "all-traffic should carry the two messages and neither reaction";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
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
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (store.nav.messages_delete_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "X on the all-traffic row should do nothing";
        goto cleanup;
    }

    /* Down twice to BRVO. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (!mesh_ui_nav_conversation_at(&store, store.nav.cursor[MESH_UI_SCREEN_MESSAGES],
                                     &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_DIRECT) {
        failure = "expected the cursor to be on BRVO";
        goto cleanup;
    }

    memset(&action, 0, sizeof action);
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action) ||
        !store.nav.messages_delete_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "the first X should arm the delete and ask for nothing";
        goto cleanup;
    }
    if (!mesh_ui_nav_conversation_is_armed(&store.nav, &conversation)) {
        failure = "the armed conversation should be the one under the cursor";
        goto cleanup;
    }

    /* Any other press stands it down, and the arming does not survive to the next X. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (store.nav.messages_delete_armed) {
        failure = "moving the cursor should stand the delete down";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);

    /* Arm again, then carry it out. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    memset(&action, 0, sizeof action);
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action) ||
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
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
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
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (action.type == MESH_UI_ACTION_MUTE_CONVERSATION) {
        failure = "the all-traffic row has no mute to offer";
        goto cleanup;
    }

    /* START on the channel asks the app to flip it, naming the row it was pressed on. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
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
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.thread_unread_from != 12U) {
        failure = "reopening a read conversation should rule the line under what was read";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
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

    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    if (strcmp(nav.toast.text, "first") != 0 || nav.toast.queued != 1U) {
        failure = "a notice arriving while one is up should wait behind it";
        goto done;
    }
    /* A repeat of what is showing is one notice standing twice as long, not two events. */
    mesh_ui_nav_post_toast(&nav, 1000U, "second");
    if (nav.toast.queued != 1U) {
        failure = "a repeat of the newest waiting notice should be dropped";
        goto done;
    }
    /*
     * A press does not wait. "Connecting to X" giving way to "X needs pairing" is one sentence
     * finishing rather than two events, and four seconds of the optimistic half before the true
     * one is worse than losing it - so the setter still replaces, and what was waiting still is.
     */
    mesh_ui_nav_set_toast(&nav, 1200U, "a press answered");
    if (strcmp(nav.toast.text, "a press answered") != 0 || nav.toast.queued != 1U) {
        failure = "a press should take the snackbar without discarding what was waiting";
        goto done;
    }
    mesh_ui_nav_set_toast(&nav, 1200U, "first");

    /* Nothing moves until the showing notice has stood its four seconds. */
    if (mesh_ui_nav_tick(&nav, 2000U) || strcmp(nav.toast.text, "first") != 0) {
        failure = "a notice should not be cut short by the one waiting behind it";
        goto done;
    }
    if (!mesh_ui_nav_tick(&nav, 5201U) || strcmp(nav.toast.text, "second") != 0) {
        failure = "the tick that retires a notice should promote the next";
        goto done;
    }
    /* Dated from the promotion rather than from when it was raised: it is standing now, and a
       deadline the backend has not seen is how the snackbar tells one notice from the next. */
    if (nav.toast.until_ms <= 5201U || nav.toast.queued != 0U) {
        failure = "a promoted notice should start its own four seconds";
        goto done;
    }
    if (!mesh_ui_nav_tick(&nav, 20000U) || nav.toast.text[0] != '\0') {
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
    if (nav.toast.queued != MESH_UI_NAV_TOAST_QUEUE || strcmp(nav.toast.queue[0], "b") != 0 ||
        strcmp(nav.toast.queue[MESH_UI_NAV_TOAST_QUEUE - 1U], "d") != 0) {
        failure = "a full queue should drop its oldest waiting notice, not its newest";
        goto done;
    }
    /* Clearing clears the backlog with it, or the client starts talking again a moment later. */
    mesh_ui_nav_set_toast(&nav, 30000U, NULL);
    if (nav.toast.text[0] != '\0' || nav.toast.queued != 0U) {
        failure = "clearing the notice should clear what was waiting behind it";
    }

done:
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Unmuting a conversation that was muted before it was ever read takes the mark with it.
 *
 * The record then holds no read position and no mute, which is an empty slot in a table of 32 -
 * and one whose stamp has just been refreshed, so the eviction would throw a genuine read
 * position away ahead of it. On the device that reads as a conversation you had read coming back
 * unread.
 */
MESH_TEST_CASE(ui_nav_unmuting_drops_a_mark_with_nothing_in_it, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;

    /* One genuine read mark first, so there is something for a careless eviction to lose. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.read_state.count != 1U) {
        failure = "opening a conversation should leave one mark behind";
        goto cleanup;
    }

    /* Mute a conversation that has never been opened: a mark with a mute and no read position. */
    (void)mesh_ui_store_set_conversation_mute(&store, (uint8_t)MESH_UI_CONVERSATION_CHANNEL,
                                              MESH_MESSAGE_BROADCAST_ADDR, 0U, true);
    if (store.read_state.count != 2U) {
        failure = "muting an unread conversation should take a slot";
        goto cleanup;
    }

    (void)mesh_ui_store_set_conversation_mute(&store, (uint8_t)MESH_UI_CONVERSATION_CHANNEL,
                                              MESH_MESSAGE_BROADCAST_ADDR, 0U, false);
    if (store.read_state.count != 1U) {
        failure = "unmuting it should give the slot back rather than leaving an empty mark";
        goto cleanup;
    }
    /* And the mark that meant something is the one still there. */
    if (mesh_ui_store_conversation_read_mark(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U,
                                             0U) != 12U) {
        failure = "the genuine read mark should have survived";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The delete on the bubble sheet: the last row, two presses, and an action naming a packet id.
 *
 * A packet id rather than the row the cursor was on, because the four places a message lives
 * store the log in four different orders - the transport's ring, the restored history, the
 * store and the card - and an index into one of them names nothing in the others.
 */
MESH_TEST_CASE(ui_nav_delete_message, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;

    /* Down twice to BRVO's conversation and open it. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open || store.nav.inbox) {
        failure = "expected BRVO's thread to be open";
        goto cleanup;
    }

    /* X on the bubble raises the sheet. */
    memset(&action, 0, sizeof action);
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action) || !store.nav.reaction_open) {
        failure = "X on a bubble should open the sheet of verbs about it";
        goto cleanup;
    }
    const uint32_t packet_id = store.nav.reply_to;
    if (packet_id == 0U) {
        failure = "the sheet should be aimed at a message with an id";
        goto cleanup;
    }

    /* The delete is the last row, so the cursor does not open on it. */
    const uint32_t rows = mesh_ui_nav_reaction_row_count();
    if (!mesh_ui_nav_reaction_row_is_delete(rows - 1U) ||
        mesh_ui_nav_reaction_row_is_delete(store.nav.reaction_cursor)) {
        failure = "the delete should be the last row and not the one under the cursor";
        goto cleanup;
    }
    for (uint32_t i = store.nav.reaction_cursor; i + 1U < rows; ++i) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    if (!mesh_ui_nav_reaction_row_is_delete(store.nav.reaction_cursor)) {
        failure = "the cursor should have reached the delete row";
        goto cleanup;
    }

    /* One press arms and sends nothing - in particular, no reaction goes on the air. */
    memset(&action, 0, sizeof action);
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action) ||
        !store.nav.message_delete_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "the first A should arm the delete and ask for nothing";
        goto cleanup;
    }

    /* B stands it down without closing the sheet: B means "not that" at every level. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.message_delete_armed || !store.nav.reaction_open) {
        failure = "B should cancel the arming before it closes the sheet";
        goto cleanup;
    }

    /* Arming and then moving off the row stands it down too, so a press somewhere else cannot
       finish a delete that was started here. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (store.nav.message_delete_armed) {
        failure = "moving off the delete row should stand it down";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);

    /* Arm, then carry it out. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    memset(&action, 0, sizeof action);
    if (!mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action) ||
        action.type != MESH_UI_ACTION_DELETE_MESSAGE) {
        failure = "the second A should ask the app to delete the message";
        goto cleanup;
    }
    if (action.number != packet_id || action.dest != 0x3000U) {
        failure = "the action should name the packet and the conversation it sits in";
        goto cleanup;
    }
    if (store.nav.reaction_open || store.nav.message_delete_armed) {
        failure = "the sheet should close and the arming go with it";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * X on the compose sheet's draft row keeps the draft as a quick reply - and is offered only
 * for a draft the list would take, so the bar never names a press that comes back refused.
 */
MESH_TEST_CASE(ui_nav_compose_x_keeps_the_draft_as_a_reply, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_command_set commands;
    mesh_test_open_tab(&store, MESH_UI_SCREEN_MESSAGES);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.thread_open) {
        failure = "expected a thread open";
        goto cleanup;
    }

    /* A draft typed and backed out of: the sheet opens on its row. */
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "At the trailhead");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.compose_open || store.nav.compose_cursor != MESH_UI_COMPOSE_ROW_DRAFT) {
        failure = "compose should open on the draft row";
        goto cleanup;
    }
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_commands_for(&snapshot, &commands);
    const struct mesh_ui_command *keep =
        mesh_ui_commands_find(&commands, MESH_UI_COMMAND_SAVE_REPLY);
    if (keep == NULL || keep->button != INKCELL_BUTTON_X) {
        failure = "the draft row's bar should offer X to keep the draft";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_SAVE_QUICK_REPLY ||
        strcmp(action.text, "At the trailhead") != 0 || !store.nav.compose_open ||
        strcmp(store.nav.draft, "At the trailhead") != 0) {
        failure = "X should ask to keep the draft, and leave both the draft and the sheet";
        goto cleanup;
    }

    /* A line already on the list, and one too long for a slot, are not offered. */
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", mesh_ui_canned_text(0));
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_commands_for(&snapshot, &commands);
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (mesh_ui_commands_find(&commands, MESH_UI_COMMAND_SAVE_REPLY) != NULL ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "a draft already on the list should not be offered";
        goto cleanup;
    }
    memset(store.nav.draft, 'a', MESH_UI_CANNED_TEXT_MAX);
    store.nav.draft[MESH_UI_CANNED_TEXT_MAX] = '\0';
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "a draft too long for a slot should not be offered";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
