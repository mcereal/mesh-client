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

#include "mesh/core/radio_settings.h"
#include "mesh/ui/backups.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include "meshtastic/config.pb.h"
#include "meshtastic/device_ui.pb.h"
#include "meshtastic/module_config.pb.h"

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

/*
 * Seen on a Heltec V3 switched to MeshCore and back: the radio's list put every Meshtastic backup
 * above every MeshCore one, so the switch read as history out of order. A radio's list is newest
 * first across both firmwares.
 */
MESH_TEST_CASE(ui_nav_backups_a_radios_list_is_in_time_order_across_firmwares, unit) {
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(settings == NULL, "memory");
    backups_settings(settings);
    struct mesh_ui_backups *b = &settings->backups;
    b->entries[0].header.saved_at = 1790000300U; /* Ridge 2: after the switch back */
    b->entries[1].header.saved_at = 1790000100U; /* Ridge 1: before the switch */
    b->entries[2].header.saved_at = 1790000200U; /* the MeshCore one, in between */
    unsigned order[4] = {0};
    size_t listed = 0U;
    struct mesh_ui_settings_item item;
    for (uint32_t row = 0U; row < 16U && listed < 4U; ++row) {
        if (mesh_ui_settings_item(settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BACKUPS,
                                  mesh_ui_backups_view(MESH_UI_BACKUPS_RADIO, 0U), row, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY) {
            order[listed++] = (unsigned)strtoul(item.text, NULL, 10);
        }
    }
    free(settings);
    MESH_TEST_FAIL_IF(listed != 3U || order[0] != 0U || order[1] != 2U || order[2] != 1U,
                      "the other firmware's backup was not listed between the two it came between");
    record_success(test_name);
}

/* A manual backup taken straight after a write's lands in the same minute, and two rows reading
   "Tue 29 Sep, 14:20" are two rows nobody can tell apart; the number the card files them under
   does. A backup alone in its minute keeps the plain time. */
MESH_TEST_CASE(ui_nav_backups_two_in_one_minute_are_told_apart_by_number, unit) {
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(settings == NULL, "memory");
    backups_settings(settings);
    struct mesh_ui_backups *b = &settings->backups;
    b->entries[0].header.saved_at = 1790000130U; /* Ridge 2 */
    b->entries[1].header.saved_at = 1790000100U; /* Ridge 1, the same minute */
    b->entries[2].header.saved_at = 1790090000U; /* the MeshCore one, a day on */
    char labels[4][MESH_UI_SETTINGS_LABEL_MAX];
    size_t listed = 0U;
    struct mesh_ui_settings_item item;
    for (uint32_t row = 0U; row < 16U && listed < 4U; ++row) {
        if (mesh_ui_settings_item(settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BACKUPS,
                                  mesh_ui_backups_view(MESH_UI_BACKUPS_RADIO, 0U), row, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY) {
            snprintf(labels[listed++], sizeof labels[0], "%s", item.label);
        }
    }
    char plain[MESH_UI_SETTINGS_LABEL_MAX];
    char second[MESH_UI_SETTINGS_LABEL_MAX];
    char first[MESH_UI_SETTINGS_LABEL_MAX];
    mesh_ui_backups_when(&b->entries[2].header, 4U, plain, sizeof plain);
    mesh_ui_backups_when(&b->entries[0].header, 2U, second, sizeof second);
    inkcell_str_format(first, sizeof first, MESH_STR_BACKUPS_WHEN_NUMBERED, second, 1U);
    char numbered[MESH_UI_SETTINGS_LABEL_MAX];
    inkcell_str_format(numbered, sizeof numbered, MESH_STR_BACKUPS_WHEN_NUMBERED, second, 2U);
    free(settings);
    MESH_TEST_FAIL_IF(listed != 3U, "a backup was not listed");
    MESH_TEST_FAIL_IF(strcmp(labels[0], plain) != 0, "a backup alone in its minute was numbered");
    MESH_TEST_FAIL_IF(strcmp(labels[1], numbered) != 0 || strcmp(labels[2], first) != 0,
                      "two backups in one minute read the same");
    record_success(test_name);
}

/*
 * Seen on a Heltec V3 once the serial, canned-message, audio and remote-hardware modules were
 * kept: a backup from before listed all four as "Modules - All of it - Only on the radio", four
 * rows no one could tell apart, and offered a restore that leaves a section only the radio has
 * exactly where it is - and then reported the four as what the restore had failed to put back.
 */
MESH_TEST_CASE(ui_backups_a_section_only_the_radio_has_is_named_and_not_restorable, unit) {
    struct mesh_radio_backup_diff *diff = calloc(1U, sizeof *diff);
    MESH_TEST_FAIL_IF(diff == NULL, "memory");
    mesh_radio_backup_diff_reset(diff, MESH_RADIO_BACKUP_MESHTASTIC);
    struct mesh_radio_backup_change *serial = mesh_radio_backup_diff_add(
        diff, MESH_RADIO_BACKUP_ADDED, MESH_RADIO_BACKUP_TOPIC_MODULE,
        (uint16_t)(MESH_UI_BACKUPS_MODULE_UNPLACED + meshtastic_ModuleConfig_serial_tag), 0U);
    char topic[64];
    mesh_ui_backups_topic(serial, topic, sizeof topic);
    const bool serial_named = strcmp(topic, "Serial module") == 0;
    struct mesh_radio_backup_change audio = *serial;
    audio.index = (uint16_t)(MESH_UI_BACKUPS_MODULE_UNPLACED + meshtastic_ModuleConfig_audio_tag);
    mesh_ui_backups_topic(&audio, topic, sizeof topic);
    const bool audio_named = strcmp(topic, "Audio") == 0;
    struct mesh_radio_backup_change canned = *serial;
    canned.index =
        (uint16_t)(MESH_UI_BACKUPS_MODULE_UNPLACED + meshtastic_ModuleConfig_canned_message_tag);
    mesh_ui_backups_topic(&canned, topic, sizeof topic);
    const bool canned_named = strcmp(topic, "Canned message input") == 0;
    struct mesh_radio_backup_change hardware = *serial;
    hardware.index =
        (uint16_t)(MESH_UI_BACKUPS_MODULE_UNPLACED + meshtastic_ModuleConfig_remote_hardware_tag);
    mesh_ui_backups_topic(&hardware, topic, sizeof topic);
    const bool hardware_named = strcmp(topic, "Remote hardware") == 0;

    const bool whole_radio_only =
        mesh_ui_backups_change_restorable(MESH_RADIO_BACKUP_MESHTASTIC, serial);
    struct mesh_radio_backup_change backup_only = *serial;
    backup_only.kind = MESH_RADIO_BACKUP_REMOVED;
    const bool whole_backup_only =
        mesh_ui_backups_change_restorable(MESH_RADIO_BACKUP_MESHTASTIC, &backup_only);
    struct mesh_radio_backup_change field = *serial;
    field.field = 3U;
    const bool field_added =
        mesh_ui_backups_change_restorable(MESH_RADIO_BACKUP_MESHTASTIC, &field);
    free(diff);
    MESH_TEST_FAIL_IF(!serial_named || !audio_named || !canned_named || !hardware_named,
                      "a module no Settings section edits was not named");
    MESH_TEST_FAIL_IF(whole_radio_only, "a section only the radio has was offered to restore");
    MESH_TEST_FAIL_IF(!whole_backup_only || !field_added,
                      "a section only the backup has, or a field set since, was not restorable");
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

MESH_TEST_CASE(ui_nav_backups_restore_is_offered_over_the_comparison_and_asks_first, unit) {
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
    mesh_test_settings_cursor_to(store,
                                 find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, NULL));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    mesh_test_settings_cursor_to(store,
                                 find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_COMPARE, NULL));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);

    /* Nothing to restore until the comparison has found something. */
    if (find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE, NULL) != UINT32_MAX) {
        failure = "restore was offered before the comparison answered";
        goto cleanup;
    }
    struct mesh_ui_backups *b = &settings->backups;
    b->compare_node = RIDGE;
    b->compare_sequence = 2U;
    b->compare_state = MESH_UI_BACKUP_COMPARE_DONE;
    mesh_radio_backup_diff_reset(&b->diff, MESH_RADIO_BACKUP_MESHTASTIC);
    struct mesh_radio_backup_change *change = mesh_radio_backup_diff_add(
        &b->diff, MESH_RADIO_BACKUP_CHANGED, MESH_RADIO_BACKUP_TOPIC_LORA, 0U, 8U);
    mesh_radio_backup_value_uint(&change->before, 5U);
    mesh_radio_backup_value_uint(&change->after, 3U);
    mesh_ui_store_set_settings(store, settings);

    const uint32_t restore = find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE, NULL);
    if (restore == UINT32_MAX || !mesh_test_settings_cursor_to(store, restore)) {
        failure = "a comparison with a difference did not offer the restore";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (!store->nav.confirm.open || action.type != MESH_UI_ACTION_NONE) {
        failure = "restore did not ask first";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_BACKUP_RESTORE || action.dest != RIDGE ||
        action.number != 2U || !store->nav.backups_compare) {
        failure = "confirming should restore the backup on screen, and stay to show the result";
        goto cleanup;
    }

    /* No choice about contacts over a comparison with none to write. */
    if (find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_ROUTES, NULL) != UINT32_MAX) {
        failure = "contact choices were offered over settings alone";
        goto cleanup;
    }

    /* A comparison that lists only a contact the radio has has nothing a restore would write:
       a restore puts back, it does not take away. */
    mesh_radio_backup_diff_reset(&b->diff, MESH_RADIO_BACKUP_MESHCORE);
    change = mesh_radio_backup_diff_add(&b->diff, MESH_RADIO_BACKUP_ADDED,
                                        MESH_RADIO_BACKUP_TOPIC_CONTACT, 0U, 1U);
    mesh_radio_backup_value_text(&change->after, "Bob", 3U);
    mesh_ui_store_set_settings(store, settings);
    if (find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE, NULL) != UINT32_MAX) {
        failure = "a restore was offered over contacts only the radio has";
        goto cleanup;
    }

    /* One only in the backup is put back, and how is a choice stepped in place under Restore. */
    change = mesh_radio_backup_diff_add(&b->diff, MESH_RADIO_BACKUP_REMOVED,
                                        MESH_RADIO_BACKUP_TOPIC_CONTACT, 0U, 1U);
    mesh_radio_backup_value_text(&change->before, "Carol", 5U);
    mesh_ui_store_set_settings(store, settings);
    const uint32_t routes = find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_ROUTES, NULL);
    if (find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE, NULL) == UINT32_MAX ||
        routes == UINT32_MAX ||
        find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_REPLACE, NULL) == UINT32_MAX) {
        failure = "a contact only in the backup did not offer the restore and its choices";
        goto cleanup;
    }
    mesh_test_settings_cursor_to(store, routes);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (store->nav.confirm.open || action.type != MESH_UI_ACTION_BACKUP_RESTORE_OPTION ||
        action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_BACKUPS_ROUTES) {
        failure = "the routes choice did not step at once";
        goto cleanup;
    }

    /* While its contacts go out, the screen counts them and offers to stop. */
    b->compare_state = MESH_UI_BACKUP_COMPARE_RESTORING;
    b->restore_contacts_total = 40U;
    b->restore_contacts_done = 12U;
    mesh_ui_store_set_settings(store, settings);
    const uint32_t stop = find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_STOP, NULL);
    if (stop == UINT32_MAX || !mesh_test_settings_cursor_to(store, stop)) {
        failure = "a restore writing contacts could not be stopped";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_BACKUP_RESTORE_STOP) {
        failure = "stop did not reach the app";
        goto cleanup;
    }
    b->restore_stopping = true;
    mesh_ui_store_set_settings(store, settings);
    if (find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_STOP, NULL) != UINT32_MAX) {
        failure = "stop was offered again once pressed";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A difference is named by the Settings row that edits it, never "Field 12". The label table
 * spells its tags as literals, because the UI layer does not include nanopb; these are the rows
 * pinned against the generated numbers, one per kind of walk: a Config variant's own tag, a
 * nested message's parent * 100 + tag, and a module - which is also told apart by the section
 * the app named it with, since every module shares one topic and Range test's 3 is not MQTT's.
 */
MESH_TEST_CASE(ui_backups_field_names_a_difference_by_its_settings_row, unit) {
    static const struct {
        uint8_t topic;
        uint16_t index;
        uint16_t field;
        inkcell_str_id label;
    } k_rows[] = {
        {MESH_RADIO_BACKUP_TOPIC_DISPLAY, meshtastic_Config_display_tag,
         meshtastic_Config_DisplayConfig_use_12h_clock_tag, MESH_STR_SETTINGS_FIELD_DISPLAY_12H},
        {MESH_RADIO_BACKUP_TOPIC_DISPLAY, meshtastic_Config_display_tag,
         meshtastic_Config_DisplayConfig_compass_orientation_tag,
         MESH_STR_SETTINGS_FIELD_DISPLAY_COMPASS},
        {MESH_RADIO_BACKUP_TOPIC_MODULE, MESH_UI_SETTINGS_RANGE_TEST,
         meshtastic_ModuleConfig_RangeTestConfig_save_tag, MESH_STR_SETTINGS_FIELD_RANGE_TEST_SAVE},
        {MESH_RADIO_BACKUP_TOPIC_MODULE, MESH_UI_SETTINGS_MQTT,
         meshtastic_ModuleConfig_MQTTConfig_username_tag, MESH_STR_SETTINGS_FIELD_MQTT_USERNAME},
        {MESH_RADIO_BACKUP_TOPIC_MODULE, MESH_UI_SETTINGS_MQTT,
         meshtastic_ModuleConfig_MQTTConfig_map_report_settings_tag * 100U +
             meshtastic_ModuleConfig_MapReportSettings_position_precision_tag,
         MESH_STR_SETTINGS_FIELD_MQTT_MAP_PRECISION},
        {MESH_RADIO_BACKUP_TOPIC_NETWORK, meshtastic_Config_network_tag,
         meshtastic_Config_NetworkConfig_ntp_server_tag, MESH_STR_NETWORK_NTP},
        {MESH_RADIO_BACKUP_TOPIC_NETWORK, meshtastic_Config_network_tag,
         meshtastic_Config_NetworkConfig_ipv4_config_tag * 100U +
             meshtastic_Config_NetworkConfig_IpV4Config_gateway_tag,
         MESH_STR_NETWORK_GATEWAY},
        {MESH_RADIO_BACKUP_TOPIC_SECURITY, meshtastic_Config_security_tag,
         meshtastic_Config_SecurityConfig_serial_enabled_tag,
         MESH_STR_SETTINGS_FIELD_SECURITY_SERIAL},
        {MESH_RADIO_BACKUP_TOPIC_RADIO_UI, 0U, meshtastic_DeviceUIConfig_screen_brightness_tag,
         MESH_STR_SETTINGS_FIELD_UI_BRIGHTNESS},
    };
    char label[64];
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; ++i) {
        struct mesh_radio_backup_change change;
        memset(&change, 0, sizeof change);
        change.topic = k_rows[i].topic;
        change.index = k_rows[i].index;
        change.field = k_rows[i].field;
        mesh_ui_backups_field(MESH_RADIO_BACKUP_MESHTASTIC, &change, label, sizeof label);
        if (strcmp(label, inkcell_str(k_rows[i].label)) != 0) {
            fprintf(stderr, "topic %u field %u: \"%s\", expected \"%s\"\n",
                    (unsigned)k_rows[i].topic, (unsigned)k_rows[i].field, label,
                    inkcell_str(k_rows[i].label));
        }
        MESH_TEST_FAIL_IF(strcmp(label, inkcell_str(k_rows[i].label)) != 0,
                          "a difference is not named by its Settings row");
    }

    /* A module the app could not place is still listed, by number, rather than borrowing the
       label another module has for the same tag. */
    struct mesh_radio_backup_change change;
    memset(&change, 0, sizeof change);
    change.topic = MESH_RADIO_BACKUP_TOPIC_MODULE;
    change.index = MESH_UI_SETTINGS_SECTION_COUNT;
    change.field = meshtastic_ModuleConfig_RangeTestConfig_save_tag;
    mesh_ui_backups_field(MESH_RADIO_BACKUP_MESHTASTIC, &change, label, sizeof label);
    char number[64];
    inkcell_str_format(number, sizeof number, MESH_STR_BACKUPS_FIELD_NUMBER,
                       (unsigned)change.field);
    MESH_TEST_FAIL_IF(strcmp(label, number) != 0,
                      "an unplaced module field took another module's label");
    record_success(test_name);
}

/* A choice reads as the word its Settings row shows, not as the protobuf's number. */
static bool backups_change_reads(const struct mesh_radio_backup_change *change, const char *before,
                                 const char *after, const char *what) {
    char expected[128];
    char text[128];
    inkcell_str_format(expected, sizeof expected, MESH_STR_BACKUPS_CHANGE_VALUE, before, after);
    mesh_ui_backups_change(MESH_RADIO_BACKUP_MESHTASTIC, change, text, sizeof text);
    if (strcmp(text, expected) != 0) {
        fprintf(stderr, "%s: \"%s\", expected \"%s\"\n", what, text, expected);
        return false;
    }
    return true;
}

static struct mesh_radio_backup_change backups_number_change(uint8_t topic, uint16_t index,
                                                             uint16_t field, int64_t before,
                                                             int64_t after) {
    struct mesh_radio_backup_change change;
    memset(&change, 0, sizeof change);
    change.kind = MESH_RADIO_BACKUP_CHANGED;
    change.topic = topic;
    change.index = index;
    change.field = field;
    change.before.kind = MESH_RADIO_BACKUP_VALUE_UINT;
    change.before.number = before;
    change.after.kind = MESH_RADIO_BACKUP_VALUE_UINT;
    change.after.number = after;
    return change;
}

MESH_TEST_CASE(ui_backups_a_choice_is_named_as_its_settings_row_names_it, unit) {
    struct mesh_radio_backup_change change = backups_number_change(
        MESH_RADIO_BACKUP_TOPIC_LORA, meshtastic_Config_lora_tag,
        meshtastic_Config_LoRaConfig_region_tag, meshtastic_Config_LoRaConfig_RegionCode_US,
        meshtastic_Config_LoRaConfig_RegionCode_UNSET);
    MESH_TEST_FAIL_IF(
        !backups_change_reads(&change,
                              mesh_radio_region_name(meshtastic_Config_LoRaConfig_RegionCode_US),
                              mesh_radio_region_name(meshtastic_Config_LoRaConfig_RegionCode_UNSET),
                              "a region is shown as a number"),
        "a region is shown as a number");

    change = backups_number_change(MESH_RADIO_BACKUP_TOPIC_DEVICE, meshtastic_Config_device_tag,
                                   meshtastic_Config_DeviceConfig_role_tag,
                                   meshtastic_Config_DeviceConfig_Role_CLIENT,
                                   meshtastic_Config_DeviceConfig_Role_ROUTER);
    MESH_TEST_FAIL_IF(!backups_change_reads(
                          &change,
                          mesh_ui_settings_enum_name(MESH_UI_FIELD_DEVICE_ROLE,
                                                     meshtastic_Config_DeviceConfig_Role_CLIENT),
                          mesh_ui_settings_enum_name(MESH_UI_FIELD_DEVICE_ROLE,
                                                     meshtastic_Config_DeviceConfig_Role_ROUTER),
                          "a role is shown as a number"),
                      "a role is shown as a number");

    /* The beacon's offered preset is one past itself on the Settings row; the backup holds the
       protobuf's own number. */
    change =
        backups_number_change(MESH_RADIO_BACKUP_TOPIC_MODULE, MESH_UI_SETTINGS_BEACON,
                              meshtastic_ModuleConfig_MeshBeaconConfig_broadcast_offer_preset_tag,
                              meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST,
                              meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST);
    MESH_TEST_FAIL_IF(
        !backups_change_reads(
            &change,
            mesh_radio_modem_preset_name(meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST),
            mesh_radio_modem_preset_name(meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST),
            "the beacon's offered preset is off by one"),
        "the beacon's offered preset is off by one");

    /* A value past the settings model's list - newer firmware's - stays a number. */
    change = backups_number_change(MESH_RADIO_BACKUP_TOPIC_LORA, meshtastic_Config_lora_tag,
                                   meshtastic_Config_LoRaConfig_region_tag, 1U, 250U);
    MESH_TEST_FAIL_IF(
        !backups_change_reads(&change,
                              mesh_radio_region_name(meshtastic_Config_LoRaConfig_RegionCode_US),
                              "250", "an unknown region is not shown as its number"),
        "an unknown region is not shown as its number");

    /* Radio UI's choices, the clock face among them though it is a bool on the wire. */
    change = backups_number_change(MESH_RADIO_BACKUP_TOPIC_RADIO_UI, 0U,
                                   meshtastic_DeviceUIConfig_theme_tag, 0U, 1U);
    MESH_TEST_FAIL_IF(!backups_change_reads(&change,
                                            mesh_ui_settings_enum_name(MESH_UI_FIELD_UI_THEME, 0U),
                                            mesh_ui_settings_enum_name(MESH_UI_FIELD_UI_THEME, 1U),
                                            "a theme is shown as a number"),
                      "a theme is shown as a number");
    change = backups_number_change(MESH_RADIO_BACKUP_TOPIC_RADIO_UI, 0U,
                                   meshtastic_DeviceUIConfig_gps_format_tag, 0U, 1U);
    MESH_TEST_FAIL_IF(
        !backups_change_reads(&change, mesh_ui_settings_enum_name(MESH_UI_FIELD_UI_GPS_FORMAT, 0U),
                              mesh_ui_settings_enum_name(MESH_UI_FIELD_UI_GPS_FORMAT, 1U),
                              "a GPS format is shown as a number"),
        "a GPS format is shown as a number");
    change = backups_number_change(MESH_RADIO_BACKUP_TOPIC_RADIO_UI, 0U,
                                   meshtastic_DeviceUIConfig_is_clockface_analog_tag, 0U, 1U);
    change.before.kind = MESH_RADIO_BACKUP_VALUE_BOOL;
    change.after.kind = MESH_RADIO_BACKUP_VALUE_BOOL;
    MESH_TEST_FAIL_IF(
        !backups_change_reads(&change, mesh_ui_settings_enum_name(MESH_UI_FIELD_UI_CLOCKFACE, 0U),
                              mesh_ui_settings_enum_name(MESH_UI_FIELD_UI_CLOCKFACE, 1U),
                              "a clock face is shown as on and off"),
        "a clock face is shown as on and off");

    /* The same tag on another module is not a choice. */
    change = backups_number_change(
        MESH_RADIO_BACKUP_TOPIC_MODULE, MESH_UI_SETTINGS_RANGE_TEST,
        meshtastic_ModuleConfig_MeshBeaconConfig_broadcast_offer_preset_tag, 1U, 2U);
    MESH_TEST_FAIL_IF(
        !backups_change_reads(&change, "1", "2", "another module's field was named as a choice"),
        "another module's field was named as a choice");
    record_success(test_name);
}

/*
 * A keyed backup says so, and offers its key back under the sheet that fits the radio on the
 * link: the plain one for the radio it is of, the one asking "is this the same radio?" for any
 * other - and the answer to the second is the only one that carries the confirmation. The keyed
 * backup itself is asked for behind a sheet of its own.
 */
MESH_TEST_CASE(ui_nav_backups_identity_asks_with_the_sheet_that_fits_the_radio, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    backups_settings(settings);
    struct mesh_ui_backups *b = &settings->backups;
    snprintf(b->live_model, sizeof b->live_model, "%s", "HELTEC_V3");
    b->entries[0].header.has_identity = true; /* Ridge's newest */
    snprintf(b->entries[0].header.model, sizeof b->entries[0].header.model, "%s", "HELTEC_V3");
    b->entries[3].header.has_identity = true; /* Valley's */
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;
    struct mesh_ui_settings_item item;

    open_backups(store);
    mesh_test_settings_cursor_to(
        store, find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_RADIO, "Ridge relay"));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    const uint32_t save = find_row(store, MESH_UI_SETTINGS_ACTION_SAVE_BACKUP_IDENTITY, NULL);
    mesh_test_settings_cursor_to(store, save);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (save == UINT32_MAX || !store->nav.confirm.open || action.type != MESH_UI_ACTION_NONE) {
        failure = "the keyed backup should be offered for the radio on the link, and ask first";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_RADIO_ACTION ||
        action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_SAVE_BACKUP_IDENTITY) {
        failure = "confirming should ask the app for the keyed backup";
        goto cleanup;
    }

    mesh_test_settings_cursor_to(store,
                                 find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, NULL));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    bool says = false;
    const uint32_t count = mesh_ui_settings_item_count(
        &store->settings, NULL, MESH_UI_SETTINGS_BACKUPS, mesh_ui_nav_open_channel(&store->nav));
    for (uint32_t row = 0U; row < count; ++row) {
        says = says ||
               (mesh_ui_settings_item(&store->settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BACKUPS,
                                      mesh_ui_nav_open_channel(&store->nav), row, &item) &&
                strcmp(item.label, "Identity key") == 0);
    }
    const uint32_t same = find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE_IDENTITY, NULL);
    if (store->nav.backups_sequence != 2U || !says || same == UINT32_MAX ||
        find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE_IDENTITY_OTHER, NULL) !=
            UINT32_MAX) {
        failure = "the radio's own keyed backup should say so and offer the plain restore";
        goto cleanup;
    }
    mesh_test_settings_cursor_to(store, same);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_BACKUP_RESTORE_IDENTITY || action.dest != RIDGE ||
        action.number != 2U || action.channel != 0U) {
        failure = "the plain sheet should restore this backup's key, unconfirmed";
        goto cleanup;
    }

    /* Valley's key, with Ridge on the link: the other sheet, and the confirmation with it. */
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    mesh_test_settings_cursor_to(
        store, find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_RADIO, "Valley"));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    mesh_test_settings_cursor_to(store,
                                 find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, NULL));
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    const uint32_t other =
        find_row(store, MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE_IDENTITY_OTHER, NULL);
    mesh_test_settings_cursor_to(store, other);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (other == UINT32_MAX || !store->nav.confirm.open ||
        store->nav.confirm.subject !=
            (uint16_t)MESH_UI_SETTINGS_ACTION_BACKUPS_RESTORE_IDENTITY_OTHER) {
        failure = "another radio's keyed backup should ask whether this is that radio";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_BACKUP_RESTORE_IDENTITY || action.dest != VALLEY ||
        action.number != 1U || action.channel != 1U) {
        failure = "only the answer to that sheet should carry the confirmation";
        goto cleanup;
    }

    /* Another board under Ridge's number is another radio; no radio of its firmware, none. */
    snprintf(b->live_model, sizeof b->live_model, "%s", "TBEAM");
    const enum mesh_ui_backups_identity board = mesh_ui_backups_identity(b, 0U);
    b->live_protocol = MESH_RADIO_BACKUP_MESHCORE;
    const enum mesh_ui_backups_identity firmware = mesh_ui_backups_identity(b, 0U);
    if (board != MESH_UI_BACKUPS_IDENTITY_OTHER || firmware != MESH_UI_BACKUPS_IDENTITY_NO_RADIO ||
        mesh_ui_backups_identity(b, 1U) != MESH_UI_BACKUPS_IDENTITY_NONE) {
        failure = "which sheet a keyed backup gets should follow the radio on the link";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
