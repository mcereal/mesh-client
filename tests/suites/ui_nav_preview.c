#define _POSIX_C_SOURCE 200809L

/*
 * What a split frame previews beside a list: mesh_ui_nav_preview(), which is A on the row under
 * the cursor, run on a copy. These hold the two halves of that claim - the preview is exactly
 * where A goes, and asking for it changes nothing.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <string.h>

/* The row of the conversation list holding a conversation of `kind`, or UINT32_MAX. */
static uint32_t preview_conversation_row(const struct mesh_ui_store *store,
                                         enum mesh_ui_conversation_kind kind) {
    const uint32_t count = mesh_ui_nav_conversation_count(store);
    for (uint32_t i = 0U; i < count; ++i) {
        struct mesh_ui_conversation conversation;
        if (mesh_ui_nav_conversation_at(store, i, &conversation) &&
            conversation.kind == (uint8_t)kind) {
            return i;
        }
    }
    return UINT32_MAX;
}

/*
 * Each row with a detail previews what A opens: the channel previews that channel's thread, and
 * then A itself, pressed for real, lands on the same conversation. The nav it was asked of is
 * untouched, and so is the conversation's read mark - a preview is looked at, not opened.
 */
MESH_TEST_CASE(ui_nav_preview_is_where_a_goes_and_moves_nothing, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_tab(&store, MESH_UI_SCREEN_MESSAGES),
                              mesh_ui_store_shutdown(&store), "the Messages tab should open");
    const uint32_t row = preview_conversation_row(&store, MESH_UI_CONVERSATION_CHANNEL);
    MESH_TEST_FAIL_IF_CLEANUP(row == UINT32_MAX, mesh_ui_store_shutdown(&store),
                              "the fixture should list a channel");
    store.nav.cursor[MESH_UI_SCREEN_MESSAGES] = row;

    struct mesh_ui_nav before;
    memcpy(&before, &store.nav, sizeof before);
    const uint32_t mark = mesh_ui_store_conversation_read_mark(
        &store, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, MESH_MESSAGE_BROADCAST_ADDR, 0U);

    struct mesh_ui_nav preview;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_nav_preview(&store.nav, &store, &preview),
                              mesh_ui_store_shutdown(&store), "a channel row should preview");
    MESH_TEST_FAIL_IF_CLEANUP(memcmp(&before, &store.nav, sizeof before) != 0,
                              mesh_ui_store_shutdown(&store),
                              "asking for a preview should leave the nav where it was");
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_store_conversation_read_mark(&store, (uint8_t)MESH_UI_CONVERSATION_CHANNEL,
                                             MESH_MESSAGE_BROADCAST_ADDR, 0U) != mark,
        mesh_ui_store_shutdown(&store), "and should mark nothing read");
    MESH_TEST_FAIL_IF_CLEANUP(
        !preview.thread_open || preview.inbox || preview.target_node != MESH_MESSAGE_BROADCAST_ADDR,
        mesh_ui_store_shutdown(&store), "the preview should be the channel's thread");

    struct mesh_ui_action action;
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_nav_handle_key(&store.nav, &store, INKCELL_KEY_A, &action) ||
            !store.nav.thread_open || store.nav.target_node != preview.target_node ||
            store.nav.target_channel != preview.target_channel,
        mesh_ui_store_shutdown(&store), "A should open the conversation the preview showed");

    /* And with the thread open there is nothing to preview: the detail is the thread itself. */
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_preview(&store.nav, &store, &preview),
                              mesh_ui_store_shutdown(&store),
                              "an open thread should have no preview beside it");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* A row whose A goes somewhere other than the detail beside the list has no preview: New
   message raises the picker. */
MESH_TEST_CASE(ui_nav_preview_is_nothing_for_new_message, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_tab(&store, MESH_UI_SCREEN_MESSAGES),
                              mesh_ui_store_shutdown(&store), "the Messages tab should open");
    const uint32_t row = preview_conversation_row(&store, MESH_UI_CONVERSATION_NEW);
    MESH_TEST_FAIL_IF_CLEANUP(row == UINT32_MAX, mesh_ui_store_shutdown(&store),
                              "the list should end in New message");
    store.nav.cursor[MESH_UI_SCREEN_MESSAGES] = row;
    struct mesh_ui_nav preview;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_preview(&store.nav, &store, &preview),
                              mesh_ui_store_shutdown(&store),
                              "New message should have nothing to preview");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.picker_open, mesh_ui_store_shutdown(&store),
                              "and asking should not raise the picker");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The roster: a node previews its detail, and the chip bar in front of the nodes does not. */
MESH_TEST_CASE(ui_nav_preview_is_a_node_but_not_the_chip_bar, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_tab(&store, MESH_UI_SCREEN_NODES),
                              mesh_ui_store_shutdown(&store), "the Nodes tab should open");
    struct mesh_ui_nav preview;
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_CHIP_ROW;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_preview(&store.nav, &store, &preview),
                              mesh_ui_store_shutdown(&store),
                              "the chip bar should have nothing to preview");
    const uint8_t filter = store.nav.node_filter;

    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_LEAD_ROWS + 1U;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_nav_preview(&store.nav, &store, &preview),
                              mesh_ui_store_shutdown(&store), "a node's row should preview");
    MESH_TEST_FAIL_IF_CLEANUP(!preview.node_detail_open || preview.node_detail_node == 0U,
                              mesh_ui_store_shutdown(&store),
                              "the preview should be that node's detail");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_detail_open || store.nav.node_filter != filter,
                              mesh_ui_store_shutdown(&store),
                              "and asking should neither open it nor press a chip");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The Settings list: a section previews its fields, a heading between sections does not. */
MESH_TEST_CASE(ui_nav_preview_is_a_section_but_not_a_heading, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS),
                              mesh_ui_store_shutdown(&store), "the Settings tab should open");
    const uint32_t rows = mesh_ui_settings_root_count(&store.settings);
    uint32_t section = UINT32_MAX;
    uint32_t heading = UINT32_MAX;
    for (uint32_t i = 0U; i < rows; ++i) {
        if (mesh_ui_settings_root_is_heading(&store.settings, i)) {
            heading = heading == UINT32_MAX ? i : heading;
        } else {
            section = section == UINT32_MAX ? i : section;
        }
    }
    MESH_TEST_FAIL_IF_CLEANUP(section == UINT32_MAX || heading == UINT32_MAX,
                              mesh_ui_store_shutdown(&store),
                              "the list should hold a section and a heading");
    struct mesh_ui_nav preview;
    store.nav.cursor[MESH_UI_SCREEN_SETTINGS] = section;
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_nav_preview(&store.nav, &store, &preview) ||
            preview.settings_section != (uint8_t)mesh_ui_settings_root_at(&store.settings, section),
        mesh_ui_store_shutdown(&store), "a section's row should preview that section");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION,
                              mesh_ui_store_shutdown(&store), "and asking should not open it");
    store.nav.cursor[MESH_UI_SCREEN_SETTINGS] = heading;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_preview(&store.nav, &store, &preview),
                              mesh_ui_store_shutdown(&store),
                              "a heading should have nothing to preview");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* Only the three tabs with a list and a detail: the Radio tab's cards have no pane to fill. */
MESH_TEST_CASE(ui_nav_preview_is_nothing_on_the_radio_tab, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_open_tab(&store, MESH_UI_SCREEN_RADIO),
                              mesh_ui_store_shutdown(&store), "the Radio tab should open");
    struct mesh_ui_nav preview;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_preview(&store.nav, &store, &preview),
                              mesh_ui_store_shutdown(&store),
                              "the Radio tab should have nothing to preview");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}
