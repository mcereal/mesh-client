/*
 * The Backups section: the radios, one radio's backups, one backup, and that backup against the
 * radio - four levels of one section, walked by A and left by B.
 *
 * The claims are the ones a reader would notice breaking: B always lands on the row the level
 * was opened from, a backup deleted or pruned under its screen takes the screen with it rather
 * than showing somebody else's, a comparison is asked of the app for the backup on screen, and
 * a backup taken under the other firmware is listed with its radio but cannot be compared.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/backups.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RIDGE 0x0badcafeU  /* a Meshtastic radio, on the link */
#define SWITCH 0x5a5a0001U /* the same device, earlier, under MeshCore */
#define VALLEY 0x22220002U /* another radio, not here */

static void add_entry(struct mesh_ui_backups *b, uint8_t radio, uint32_t node, uint32_t sequence,
                      uint8_t protocol, uint8_t reason, const char *device) {
    struct mesh_ui_backup_entry *entry = &b->entries[b->entry_count++];
    memset(entry, 0, sizeof *entry);
    entry->radio = radio;
    entry->sequence = sequence;
    entry->header.node_id = node;
    entry->header.protocol = protocol;
    entry->header.reason = reason;
    snprintf(entry->header.device, sizeof entry->header.device, "%s", device);
    snprintf(entry->header.firmware, sizeof entry->header.firmware, "%s", "2.5.6");
    b->radios[radio].listed++;
}

static void add_radio(struct mesh_ui_backups *b, uint32_t node, const char *name, uint8_t protocol,
                      uint16_t count, const char *device) {
    struct mesh_ui_backup_radio *radio = &b->radios[b->radio_count++];
    memset(radio, 0, sizeof *radio);
    radio->node = node;
    radio->count = count;
    radio->protocol = protocol;
    snprintf(radio->name, sizeof radio->name, "%s", name);
    snprintf(radio->device, sizeof radio->device, "%s", device);
}

/* Ridge (two backups, on the link), its MeshCore past (one), and Valley (one, not here). */
static void backups_settings(struct mesh_ui_settings *settings) {
    memset(settings, 0, sizeof *settings);
    struct mesh_ui_backups *b = &settings->backups;
    b->enabled = true;
    b->live_node = RIDGE;
    b->live_protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    add_radio(b, RIDGE, "Ridge relay", MESH_RADIO_BACKUP_MESHTASTIC, 2U, "AA:BB:CC:DD:EE:FF");
    add_radio(b, SWITCH, "Ridge", MESH_RADIO_BACKUP_MESHCORE, 1U, "AA:BB:CC:DD:EE:FF");
    add_radio(b, VALLEY, "Valley", MESH_RADIO_BACKUP_MESHTASTIC, 1U, "/dev/ttyACM0");
    add_entry(b, 0U, RIDGE, 2U, MESH_RADIO_BACKUP_MESHTASTIC, MESH_RADIO_BACKUP_BEFORE_WRITE,
              "AA:BB:CC:DD:EE:FF");
    add_entry(b, 0U, RIDGE, 1U, MESH_RADIO_BACKUP_MESHTASTIC, MESH_RADIO_BACKUP_FIRST_CONNECT,
              "AA:BB:CC:DD:EE:FF");
    add_entry(b, 1U, SWITCH, 4U, MESH_RADIO_BACKUP_MESHCORE, MESH_RADIO_BACKUP_BEFORE_FIRMWARE,
              "AA:BB:CC:DD:EE:FF");
    add_entry(b, 2U, VALLEY, 1U, MESH_RADIO_BACKUP_MESHTASTIC, MESH_RADIO_BACKUP_MANUAL,
              "/dev/ttyACM0");
}

static bool row_is(const struct mesh_ui_store *store, uint32_t row,
                   enum mesh_ui_settings_action action, struct mesh_ui_settings_item *item) {
    return mesh_ui_settings_item(&store->settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BACKUPS,
                                 mesh_ui_nav_open_channel(&store->nav), row, item) &&
           item->number == (uint32_t)action;
}

/* The row of the open level that carries `action`, or UINT32_MAX. */
static uint32_t find_row(const struct mesh_ui_store *store, enum mesh_ui_settings_action action,
                         const char *label) {
    const uint32_t count = mesh_ui_settings_item_count(
        &store->settings, NULL, MESH_UI_SETTINGS_BACKUPS, mesh_ui_nav_open_channel(&store->nav));
    struct mesh_ui_settings_item item;
    for (uint32_t row = 0U; row < count; ++row) {
        if (row_is(store, row, action, &item) &&
            (label == NULL || strcmp(item.label, label) == 0)) {
            return row;
        }
    }
    return UINT32_MAX;
}

static bool open_backups(struct mesh_ui_store *store) {
    return mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS) &&
           mesh_test_settings_open(store, MESH_UI_SETTINGS_BACKUPS);
}

static uint8_t depth(const struct mesh_ui_store *store) {
    struct mesh_ui_route route;
    mesh_ui_route_of(&store->nav, &route);
    return route.depth;
}

MESH_TEST_CASE(ui_nav_backups_walks_in_and_back_out_to_each_row, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    backups_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;
    uint32_t *const cursor = &store->nav.cursor[MESH_UI_SCREEN_SETTINGS];

    if (!open_backups(store) || depth(store) != 1U) {
        failure = "Backups should open from the Settings list, with no radio needed";
        goto cleanup;
    }
    /* Valley, the third radio: its list is one backup, and B comes back to its row. */
    const uint32_t valley = find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_RADIO, "Valley");
    if (valley == UINT32_MAX || !mesh_test_settings_cursor_to(store, valley)) {
        failure = "the radio list should name Valley";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    struct mesh_ui_settings_item first;
    if (action.type != MESH_UI_ACTION_NONE || store->nav.backups_node != VALLEY ||
        depth(store) != 2U ||
        !row_is(store, *cursor, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, &first) ||
        *cursor != 1U) {
        /* Valley is not on the link, so there is nothing to save: its first row is its one
           backup, under the heading the cursor steps over. */
        failure = "A on a radio should open its backups, at the top, asking nothing of the app";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    if (store->nav.backups_node != 0U || *cursor != valley || depth(store) != 1U) {
        failure = "B should come back to the radio's row";
        goto cleanup;
    }

    /* Ridge: its own two backups and the one from before its switch, listed together. */
    const uint32_t ridge =
        find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_RADIO, "Ridge relay");
    mesh_test_settings_cursor_to(store, ridge);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    uint32_t entries = 0U;
    const uint32_t count = mesh_ui_settings_item_count(
        &store->settings, NULL, MESH_UI_SETTINGS_BACKUPS, mesh_ui_nav_open_channel(&store->nav));
    struct mesh_ui_settings_item item;
    for (uint32_t row = 0U; row < count; ++row) {
        entries += row_is(store, row, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, &item) ? 1U : 0U;
    }
    if (entries != 3U || find_row(store, MESH_UI_SETTINGS_ACTION_SAVE_BACKUP, NULL) == UINT32_MAX) {
        failure = "Ridge should list its two backups, its MeshCore one, and a save for the radio";
        goto cleanup;
    }

    /* The newest: its detail, then the comparison. */
    const uint32_t newest = find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, NULL);
    mesh_test_settings_cursor_to(store, newest);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (store->nav.backups_sequence != 2U || store->nav.backups_entry_node != RIDGE ||
        depth(store) != 3U) {
        failure = "A on a backup should open that backup";
        goto cleanup;
    }
    const uint32_t compare = find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_COMPARE, NULL);
    if (compare == UINT32_MAX ||
        !row_is(store, compare, MESH_UI_SETTINGS_ACTION_BACKUPS_COMPARE, &item) ||
        item.kind != INKSTAND_FORM_ACTION) {
        failure = "a backup of the radio on the link should offer the comparison";
        goto cleanup;
    }
    mesh_test_settings_cursor_to(store, compare);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_BACKUP_COMPARE || action.dest != RIDGE ||
        action.number != 2U || !store->nav.backups_compare || depth(store) != 4U) {
        failure = "A on compare should open the comparison and ask the app for this backup's";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store->settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BACKUPS,
                               mesh_ui_nav_open_channel(&store->nav), 0U, &item) ||
        item.kind != INKSTAND_FORM_METER) {
        failure = "until the app answers, the comparison should say it is reading the radio";
        goto cleanup;
    }

    /* And B all the way out, each level landing where it was left. */
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    if (store->nav.backups_compare || *cursor != compare || depth(store) != 3U) {
        failure = "B from the comparison should land on the compare row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    if (store->nav.backups_sequence != 0U || *cursor != newest || depth(store) != 2U) {
        failure = "B from a backup should land on its row in the radio's list";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    if (store->nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        failure = "B from the radios should leave the section";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_nav_backups_other_firmware_and_absent_radios_cannot_compare, unit) {
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(settings == NULL, "memory");
    backups_settings(settings);
    const struct mesh_ui_backups *b = &settings->backups;
    const bool ridge = mesh_ui_backups_can_compare(b, 0U);
    const bool switched = mesh_ui_backups_can_compare(b, 2U);
    const bool valley = mesh_ui_backups_can_compare(b, 3U);
    const bool listed = mesh_ui_backups_listed_under(b, 0U, 2U);
    const bool not_valley = mesh_ui_backups_listed_under(b, 0U, 3U);

    /* And each says why in its detail, in the row the verb would be. */
    struct mesh_ui_settings_item item;
    bool other_reason = false;
    bool link_reason = false;
    for (uint32_t row = 0U; row < 40U; ++row) {
        if (mesh_ui_settings_item(settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BACKUPS,
                                  mesh_ui_backups_view(MESH_UI_BACKUPS_ENTRY, 2U), row, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_BACKUPS_COMPARE) {
            other_reason = item.kind == INKSTAND_FORM_ACTION_OFF &&
                           strcmp(item.value, "Taken under the other firmware") == 0;
        }
        if (mesh_ui_settings_item(settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BACKUPS,
                                  mesh_ui_backups_view(MESH_UI_BACKUPS_ENTRY, 3U), row, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_BACKUPS_COMPARE) {
            link_reason = item.kind == INKSTAND_FORM_ACTION_OFF &&
                          strcmp(item.value, "Connect this radio first") == 0;
        }
    }
    free(settings);
    MESH_TEST_FAIL_IF(!ridge, "the radio on the link could not be compared");
    MESH_TEST_FAIL_IF(switched || valley, "a backup of another firmware or radio was comparable");
    MESH_TEST_FAIL_IF(!listed || not_valley,
                      "a radio's list is its backups and its other firmware's, nobody else's");
    MESH_TEST_FAIL_IF(!other_reason || !link_reason, "a withdrawn compare did not say why");
    record_success(test_name);
}

MESH_TEST_CASE(ui_nav_backups_delete_asks_then_leaves_the_backup, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    backups_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;

    open_backups(store);
    mesh_test_settings_cursor_to(
        store, find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_RADIO, "Ridge relay"));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    const uint32_t oldest_row =
        find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, NULL) + 1U;
    mesh_test_settings_cursor_to(store, oldest_row);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (store->nav.backups_sequence != 1U) {
        failure = "the second row should be Ridge's first backup";
        goto cleanup;
    }
    mesh_test_settings_cursor_to(store,
                                 find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_DELETE, NULL));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (!store->nav.confirm.open || action.type != MESH_UI_ACTION_NONE ||
        store->nav.confirm.subject != (uint16_t)MESH_UI_SETTINGS_ACTION_BACKUPS_DELETE) {
        failure = "delete should ask first";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (store->nav.confirm.open || action.type != MESH_UI_ACTION_BACKUP_DELETE ||
        action.dest != RIDGE || action.number != 1U) {
        failure = "confirming should delete the backup on screen, by its node and number";
        goto cleanup;
    }
    if (store->nav.backups_sequence != 0U || store->nav.backups_node != RIDGE ||
        store->nav.cursor[MESH_UI_SCREEN_SETTINGS] != oldest_row) {
        failure = "a deleted backup should go back to its radio's list";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_nav_backups_follow_the_card_by_node_and_number, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    backups_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;

    open_backups(store);
    mesh_test_settings_cursor_to(
        store, find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_RADIO, "Valley"));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    mesh_test_settings_cursor_to(store,
                                 find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, NULL));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);

    /* A new backup of Ridge arrives at the top: Valley's moves along the list, and the screen
       stays on it rather than on whatever took its index. */
    struct mesh_ui_backups *b = &settings->backups;
    memmove(&b->entries[1], &b->entries[0], sizeof b->entries[0] * b->entry_count);
    ++b->entry_count;
    b->entries[0] = b->entries[1];
    b->entries[0].sequence = 3U;
    mesh_ui_store_set_settings(store, settings);
    mesh_ui_nav_clamp(&store->nav, store); /* what every frame's snapshot does */
    uint8_t index = 0U;
    if (store->nav.backups_sequence != 1U ||
        mesh_ui_backups_level_of(mesh_ui_nav_open_channel(&store->nav), &index) !=
            MESH_UI_BACKUPS_ENTRY ||
        store->settings.backups.entries[index].header.node_id != VALLEY) {
        failure = "a backup taken elsewhere moved the screen off Valley's";
        goto cleanup;
    }

    /* And Valley's last backup deleted from another screen: the radio goes, and so does this. */
    b->entry_count--;
    b->radio_count--;
    mesh_ui_store_set_settings(store, settings);
    mesh_ui_nav_clamp(&store->nav, store);
    if (store->nav.backups_node != 0U || store->nav.backups_sequence != 0U ||
        mesh_ui_nav_open_channel(&store->nav) != MESH_UI_SETTINGS_NO_CHANNEL) {
        failure = "a radio with no backups left should take its screens with it";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
