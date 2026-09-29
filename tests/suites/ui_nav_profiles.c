/*
 * Profiles on screen: made in a picker over a backup, then listed, opened, compared with the
 * radio and applied in their own section.
 *
 * The claims are the ones a reader would notice breaking: the picker asks the app for the backup
 * on screen and hands it back the parts ticked and the name typed; B lands on the row a level was
 * opened from; a profile is compared and applied by its own number, and only behind a sheet; one
 * for the other firmware says so rather than offering a comparison; and a profile deleted under
 * its screen takes the screen with it.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/radio_profile.h"
#include "mesh/ui/backups.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/profiles.h"
#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include "meshtastic/module_config.pb.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RIDGE 0x0badcafeU /* a Meshtastic radio, on the link */

static void add_profile(struct mesh_ui_profiles *p, uint32_t sequence, const char *name,
                        uint8_t protocol) {
    struct mesh_ui_profile *item = &p->items[p->count++];
    memset(item, 0, sizeof *item);
    item->sequence = sequence;
    item->header.reason = MESH_RADIO_BACKUP_PROFILE;
    item->header.protocol = protocol;
    snprintf(item->header.name, sizeof item->header.name, "%s", name);
    mesh_radio_profile_parts_set(&item->header.parts, MESH_RADIO_BACKUP_TOPIC_LORA, 0U, true);
}

/* One backup of Ridge on the link, two profiles - one for each firmware - and a .cfg. */
static void profiles_settings(struct mesh_ui_settings *settings) {
    memset(settings, 0, sizeof *settings);
    struct mesh_ui_backups *b = &settings->backups;
    b->enabled = true;
    b->live_node = RIDGE;
    b->live_protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    struct mesh_ui_backup_radio *radio = &b->radios[b->radio_count++];
    radio->node = RIDGE;
    radio->count = 1U;
    radio->listed = 1U;
    radio->protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    snprintf(radio->name, sizeof radio->name, "%s", "Ridge relay");
    struct mesh_ui_backup_entry *entry = &b->entries[b->entry_count++];
    entry->sequence = 7U;
    entry->header.node_id = RIDGE;
    entry->header.protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    entry->header.reason = MESH_RADIO_BACKUP_MANUAL;
    snprintf(entry->header.name, sizeof entry->header.name, "%s", "Ridge relay");

    struct mesh_ui_profiles *p = &settings->profiles;
    p->enabled = true;
    add_profile(p, 3U, "EU narrow", MESH_RADIO_BACKUP_MESHCORE);
    add_profile(p, 5U, "Hike mesh", MESH_RADIO_BACKUP_MESHTASTIC);
    p->cfg_count = 1U;
    snprintf(p->cfgs[0], sizeof p->cfgs[0], "%s", "phone.cfg");
}

static uint32_t find_row_in(const struct mesh_ui_store *store,
                            enum mesh_ui_settings_section section,
                            enum mesh_ui_settings_action action, const char *label) {
    const uint8_t view = mesh_ui_nav_open_channel(&store->nav);
    const uint32_t count = mesh_ui_settings_item_count(&store->settings, NULL, section, view);
    struct mesh_ui_settings_item item;
    for (uint32_t row = 0U; row < count; ++row) {
        if (mesh_ui_settings_item(&store->settings, NULL, NULL, 0U, section, view, row, &item) &&
            item.number == (uint32_t)action && item.kind == INKSTAND_FORM_ACTION &&
            (label == NULL || strcmp(item.label, label) == 0)) {
            return row;
        }
    }
    return UINT32_MAX;
}

static uint32_t find_row(const struct mesh_ui_store *store, enum mesh_ui_settings_action action,
                         const char *label) {
    return find_row_in(store, MESH_UI_SETTINGS_PROFILES, action, label);
}

/* Moves to the row carrying `action` in the open section and presses A on it. */
static bool press(struct mesh_ui_store *store, enum mesh_ui_settings_section section,
                  enum mesh_ui_settings_action action, const char *label,
                  struct mesh_ui_action *out) {
    const uint32_t row = find_row_in(store, section, action, label);
    if (row == UINT32_MAX || !mesh_test_settings_cursor_to(store, row)) {
        return false;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, out);
    return true;
}

static uint8_t depth(const struct mesh_ui_store *store) {
    struct mesh_ui_route route;
    mesh_ui_route_of(&store->nav, &route);
    return route.depth;
}

MESH_TEST_CASE(ui_nav_profiles_walks_in_compares_and_back_out_to_each_row, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    profiles_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;
    uint32_t *const cursor = &store->nav.cursor[MESH_UI_SCREEN_SETTINGS];

    if (!mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS) ||
        !mesh_test_settings_open(store, MESH_UI_SETTINGS_PROFILES) || depth(store) != 1U) {
        failure = "Profiles should open from the Settings list";
        goto cleanup;
    }
    const uint32_t hike = find_row(store, MESH_UI_SETTINGS_ACTION_PROFILES_OPEN, "Hike mesh");
    if (!press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_OPEN, "Hike mesh",
               &action) ||
        store->nav.profiles_sequence != 5U || depth(store) != 2U) {
        failure = "A on a profile should open it, held by its number";
        goto cleanup;
    }
    if (!press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_COMPARE, NULL,
               &action) ||
        action.type != MESH_UI_ACTION_PROFILE_COMPARE || action.number != 5U ||
        !store->nav.profiles_compare || depth(store) != 3U) {
        failure = "Compare should ask the app about this profile and open its screen";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    if (store->nav.profiles_compare || store->nav.profiles_sequence != 5U) {
        failure = "B should leave the comparison for the profile";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    if (store->nav.profiles_sequence != 0U || *cursor != hike ||
        store->nav.settings_section != MESH_UI_SETTINGS_PROFILES) {
        failure = "B should land on the profile's own row of the list";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A .cfg copied into the folder while the client runs is there the next time the section opens:
   opening it asks the app to read the folder again, and opening another section does not. */
MESH_TEST_CASE(ui_nav_profiles_opening_the_section_reads_the_folder_again, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    profiles_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;

    (void)mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS);
    uint32_t profiles = UINT32_MAX;
    uint32_t backups = UINT32_MAX;
    for (uint32_t i = 0; i < mesh_ui_settings_root_count(&store->settings); ++i) {
        if (mesh_ui_settings_root_at(&store->settings, i) == MESH_UI_SETTINGS_PROFILES) {
            profiles = i;
        } else if (mesh_ui_settings_root_at(&store->settings, i) == MESH_UI_SETTINGS_BACKUPS) {
            backups = i;
        }
    }
    memset(&action, 0, sizeof action);
    if (backups == UINT32_MAX || !mesh_test_settings_cursor_to(store, backups)) {
        failure = "Backups should be on the Settings list";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type == MESH_UI_ACTION_PROFILE_RESCAN) {
        failure = "Opening another section should not read the profiles folder";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_B, &action);
    memset(&action, 0, sizeof action);
    if (profiles == UINT32_MAX || !mesh_test_settings_cursor_to(store, profiles)) {
        failure = "Profiles should be on the Settings list";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (store->nav.settings_section != MESH_UI_SETTINGS_PROFILES ||
        action.type != MESH_UI_ACTION_PROFILE_RESCAN) {
        failure = "Opening Profiles should ask the app to read the folder again";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_nav_profiles_apply_is_offered_over_the_comparison_and_asks_first, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    profiles_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;

    (void)mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS);
    (void)mesh_test_settings_open(store, MESH_UI_SETTINGS_PROFILES);
    (void)press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_OPEN,
                "Hike mesh", &action);
    (void)press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_COMPARE, NULL,
                &action);
    if (find_row(store, MESH_UI_SETTINGS_ACTION_PROFILES_APPLY, NULL) != UINT32_MAX) {
        failure = "apply was offered before the comparison answered";
        goto cleanup;
    }
    struct mesh_ui_profiles *p = &settings->profiles;
    p->compare_sequence = 5U;
    p->compare_node = RIDGE;
    p->compare_state = MESH_UI_BACKUP_COMPARE_DONE;
    mesh_radio_backup_diff_reset(&p->diff, MESH_RADIO_BACKUP_MESHTASTIC);
    struct mesh_radio_backup_change *change = mesh_radio_backup_diff_add(
        &p->diff, MESH_RADIO_BACKUP_CHANGED, MESH_RADIO_BACKUP_TOPIC_LORA, 0U, 8U);
    mesh_radio_backup_value_uint(&change->before, 5U);
    mesh_radio_backup_value_uint(&change->after, 3U);
    mesh_ui_store_set_settings(store, settings);

    if (!press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_APPLY, NULL,
               &action) ||
        !store->nav.confirm.open || action.type != MESH_UI_ACTION_NONE) {
        failure = "apply was not offered, or did not ask first";
        goto cleanup;
    }
    /* The link moves to another radio with the sheet open: the apply still names the one that
       was compared, for the app to refuse. */
    p->compare_node = 0x0f00d00dU;
    mesh_ui_store_set_settings(store, settings);
    mesh_ui_store_handle_key(store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_PROFILE_APPLY || action.number != 5U ||
        !store->nav.profiles_compare) {
        failure = "confirming should apply the profile on screen, and stay to show the result";
        goto cleanup;
    }
    if (action.dest != RIDGE) {
        failure = "the apply should name the radio the profile was compared with";
        goto cleanup;
    }

    /* The same comparison of the radio as it now stands: nothing to apply. */
    mesh_radio_backup_diff_reset(&p->diff, MESH_RADIO_BACKUP_MESHTASTIC);
    mesh_ui_store_set_settings(store, settings);
    if (find_row(store, MESH_UI_SETTINGS_ACTION_PROFILES_APPLY, NULL) != UINT32_MAX) {
        failure = "apply was offered over a radio that matches";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A MeshCore profile, with a Meshtastic radio on the link: no comparison to offer, and the row
   says why - the refusal the core makes, shown before it is ever asked. */
MESH_TEST_CASE(ui_nav_profiles_for_the_other_firmware_cannot_be_compared, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    profiles_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;

    (void)mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS);
    (void)mesh_test_settings_open(store, MESH_UI_SETTINGS_PROFILES);
    (void)press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_OPEN,
                "EU narrow", &action);
    if (store->nav.profiles_sequence != 3U) {
        failure = "the MeshCore profile did not open";
        goto cleanup;
    }
    if (find_row(store, MESH_UI_SETTINGS_ACTION_PROFILES_COMPARE, NULL) != UINT32_MAX ||
        find_row(store, MESH_UI_SETTINGS_ACTION_PROFILES_EXPORT, NULL) != UINT32_MAX) {
        failure = "a MeshCore profile offered a comparison with a Meshtastic radio, or a .cfg";
        goto cleanup;
    }
    const uint8_t view = mesh_ui_nav_open_channel(&store->nav);
    const uint32_t count =
        mesh_ui_settings_item_count(&store->settings, NULL, MESH_UI_SETTINGS_PROFILES, view);
    struct mesh_ui_settings_item item;
    bool said = false;
    for (uint32_t row = 0U; row < count; ++row) {
        said = said || (mesh_ui_settings_item(&store->settings, NULL, NULL, 0U,
                                              MESH_UI_SETTINGS_PROFILES, view, row, &item) &&
                        item.kind == INKSTAND_FORM_ACTION_OFF &&
                        item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_PROFILES_COMPARE &&
                        strcmp(item.value, inkcell_str(MESH_STR_PROFILES_COMPARE_OFF_OTHER)) == 0);
    }
    if (!said) {
        failure = "the comparison row did not say the profile is for the other firmware";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_nav_profiles_delete_and_import_act_on_what_is_on_screen, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    profiles_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;

    (void)mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS);
    (void)mesh_test_settings_open(store, MESH_UI_SETTINGS_PROFILES);
    if (!press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_IMPORT,
               "phone.cfg", &action) ||
        action.type != MESH_UI_ACTION_PROFILE_IMPORT ||
        strcmp(action.identifier, "phone.cfg") != 0 || store->nav.confirm.open) {
        failure = "A on a .cfg should read it in by its name, with nothing to confirm";
        goto cleanup;
    }

    (void)press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_OPEN,
                "Hike mesh", &action);
    if (!press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_EXPORT, NULL,
               &action) ||
        action.type != MESH_UI_ACTION_PROFILE_EXPORT || action.number != 5U) {
        failure = "Save as a .cfg should write this profile out";
        goto cleanup;
    }
    if (!press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_DELETE, NULL,
               &action) ||
        !store->nav.confirm.open) {
        failure = "delete did not ask first";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_PROFILE_DELETE || action.number != 5U ||
        store->nav.profiles_sequence != 0U) {
        failure = "confirming should delete this profile and go back to the list";
        goto cleanup;
    }

    /* And one deleted from under its screen - by another press, or off the card - takes it. */
    (void)press(store, MESH_UI_SETTINGS_PROFILES, MESH_UI_SETTINGS_ACTION_PROFILES_OPEN,
                "Hike mesh", &action);
    settings->profiles.count = 1U;
    mesh_ui_store_set_settings(store, settings);
    (void)mesh_ui_nav_clamp(&store->nav, store);
    if (store->nav.profiles_sequence != 0U ||
        store->nav.profiles_view != MESH_UI_SETTINGS_NO_CHANNEL) {
        failure = "a profile gone from the card left its screen open";
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
 * The picker over a backup: it asks the app for that backup's parts, a tick is the app's to
 * answer, and Name and save opens the keyboard on the radio's own name - Done hands the name on
 * and closes the picker onto the backup.
 */
MESH_TEST_CASE(ui_nav_profiles_are_made_in_a_picker_over_a_backup, unit) {
    const char *failure = NULL;
    struct mesh_ui_store *store = calloc(1U, sizeof *store);
    struct mesh_ui_settings *settings = calloc(1U, sizeof *settings);
    MESH_TEST_FAIL_IF(store == NULL || settings == NULL, "memory");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(store) != 0, "store init failed");
    mesh_test_nav_populate(store);
    profiles_settings(settings);
    mesh_ui_store_set_settings(store, settings);
    struct mesh_ui_action action;

    (void)mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS);
    (void)mesh_test_settings_open(store, MESH_UI_SETTINGS_BACKUPS);
    (void)press(store, MESH_UI_SETTINGS_BACKUPS, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_RADIO, NULL,
                &action);
    (void)press(store, MESH_UI_SETTINGS_BACKUPS, MESH_UI_SETTINGS_ACTION_BACKUPS_OPEN_ENTRY, NULL,
                &action);
    const uint32_t make = find_row_in(store, MESH_UI_SETTINGS_BACKUPS,
                                      MESH_UI_SETTINGS_ACTION_BACKUPS_MAKE_PROFILE, NULL);
    if (!press(store, MESH_UI_SETTINGS_BACKUPS, MESH_UI_SETTINGS_ACTION_BACKUPS_MAKE_PROFILE, NULL,
               &action) ||
        action.type != MESH_UI_ACTION_PROFILE_DRAFT || action.dest != RIDGE ||
        action.number != 7U || !store->nav.backups_pick) {
        failure = "Make a profile should open the picker and ask for this backup's parts";
        goto cleanup;
    }
    if (find_row_in(store, MESH_UI_SETTINGS_BACKUPS, MESH_UI_SETTINGS_ACTION_PROFILES_SAVE, NULL) !=
        UINT32_MAX) {
        failure = "the picker offered a save before the parts arrived";
        goto cleanup;
    }

    struct mesh_ui_profiles *p = &settings->profiles;
    p->draft_node = RIDGE;
    p->draft_sequence = 7U;
    p->draft_protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    p->draft_count = 2U;
    p->draft_parts[0] = (struct mesh_radio_profile_part){.topic = MESH_RADIO_BACKUP_TOPIC_LORA};
    p->draft_parts[1] = (struct mesh_radio_profile_part){.topic = MESH_RADIO_BACKUP_TOPIC_MODULE,
                                                         .index = meshtastic_ModuleConfig_mqtt_tag};
    mesh_radio_profile_parts_set(&p->draft_picked, MESH_RADIO_BACKUP_TOPIC_LORA, 0U, true);
    mesh_radio_profile_parts_set(&p->draft_picked, MESH_RADIO_BACKUP_TOPIC_MODULE,
                                 meshtastic_ModuleConfig_mqtt_tag, true);
    mesh_ui_store_set_settings(store, settings);

    const uint32_t mqtt =
        find_row_in(store, MESH_UI_SETTINGS_BACKUPS, MESH_UI_SETTINGS_ACTION_PROFILES_PART, NULL) +
        1U;
    if (!mesh_test_settings_cursor_to(store, mqtt)) {
        failure = "the second part is not a row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_PROFILE_DRAFT_TOGGLE || action.number != 1U) {
        failure = "A on a part should ask the app to tick it, by its place in the list";
        goto cleanup;
    }

    if (!press(store, MESH_UI_SETTINGS_BACKUPS, MESH_UI_SETTINGS_ACTION_PROFILES_SAVE, NULL,
               &action) ||
        !store->nav.keyboard_open || !store->nav.keyboard_profile_name ||
        strcmp(store->nav.draft, "Ridge relay") != 0) {
        failure = "Name and save should open the keyboard on the radio's name";
        goto cleanup;
    }
    mesh_ui_store_handle_key(store, INKCELL_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_PROFILE_MAKE || strcmp(action.text, "Ridge relay") != 0) {
        failure = "Done should hand the name to the app";
        goto cleanup;
    }
    if (store->nav.keyboard_open || store->nav.backups_pick ||
        store->nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store->nav.cursor[MESH_UI_SCREEN_SETTINGS] != make) {
        failure = "the picker should close onto the backup, on the row it was opened from";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(store);
    free(store);
    free(settings);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
