#define _POSIX_C_SOURCE 200809L

/* Navigating settings: editing rows, channels, radio actions, fixed position. */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

MESH_TEST_CASE(ui_nav_settings, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.use_preset = true;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) !=
            mesh_ui_settings_root_count(&store.settings)) {
        failure = "Settings tab should open on the section list";
        goto cleanup;
    }
    /* The list is a curated order rather than the enum's, so the row is looked up rather than
       counted to - and no module is on it at all. */
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_LORA)) {
        failure = "cursor should reach LoRa";
        goto cleanup;
    }
    const uint32_t lora_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    if (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 0U || lora_rows == 0U) {
        failure = "A should open the LoRa section";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 1U) {
        failure = "Down should move within the section";
        goto cleanup;
    }
    /* A on the "Use preset" toggle edits it in place; nothing is sent until Y. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE || store.nav.settings_section != MESH_UI_SETTINGS_LORA ||
        store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_LORA_USE_PRESET) {
        failure = "A on a toggle should record an edit";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    /* The refresh carries the pending edit count so the toast can say they were kept: X and
       Y sit together, and a refresh that reports nothing reads like a save that did nothing. */
    if (action.type != MESH_UI_ACTION_REFRESH_SETTINGS || action.edit_count != 1U) {
        failure = "X should ask for a refresh and report the edits it kept";
        goto cleanup;
    }
    /* B with an edit asks first; B again discards and leaves. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (!store.nav.settings_discard_armed || store.nav.settings_section != MESH_UI_SETTINGS_LORA) {
        failure = "B with an edit should ask before leaving";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        mesh_ui_settings_root_at(&store.settings, store.nav.cursor[MESH_UI_SCREEN_SETTINGS]) !=
            MESH_UI_SETTINGS_LORA ||
        store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        failure = "B should return to the section list at the same row";
        goto cleanup;
    }
    /* An unloaded section opens empty rather than refusing; the backend explains. Only LoRa
       was loaded above, so Display is one of the several that have nothing to show. */
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY) ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 0U) {
        failure = "unloaded section should open with no rows";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The third level phase 9 added: Modules opens a list, a row on it opens that module, and B
 * unwinds one level at a time with each cursor where it was left.
 */
MESH_TEST_CASE(ui_nav_modules, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_telemetry = true;
    settings.device_telemetry_enabled = true;
    settings.device_update_interval = 900U;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_MODULES) ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) !=
            mesh_ui_settings_module_count()) {
        failure = "Modules should open on the module list";
        goto cleanup;
    }
    const uint32_t modules_row = store.nav.settings_list_cursor;

    /* Walk to Telemetry and open it. A on a module row is intercepted before the ordinary
       ACTION handling, so it must not have reached the app as a radio action. */
    uint32_t telemetry_row = 0U;
    while (telemetry_row < mesh_ui_settings_module_count() &&
           mesh_ui_settings_module_at(telemetry_row) != MESH_UI_SETTINGS_TELEMETRY) {
        telemetry_row++;
    }
    while (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] < telemetry_row &&
           mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action)) {
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    /* Telemetry groups its rows under a "Device" heading, so the module opens on row 1 - the
       first row its cursor may stand on - rather than on the title above it. */
    if (store.nav.settings_section != MESH_UI_SETTINGS_TELEMETRY ||
        store.nav.settings_parent != MESH_UI_SETTINGS_MODULES ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 1U || action.type != MESH_UI_ACTION_NONE) {
        failure = "A on a module row should open that module";
        goto cleanup;
    }

    /* An edit inside the module still belongs to the module's own section, not to Modules.
       The cursor is already on Enabled - the section opened there - so one step reaches
       Interval. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action); /* Interval */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_TELEMETRY_INTERVAL) {
        failure = "Right should edit the interval row inside the module";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_SAVE_SETTINGS ||
        action.section != MESH_UI_SETTINGS_TELEMETRY) {
        failure = "Y should save the module's own section";
        goto cleanup;
    }

    /* Nothing cleared the edits - that is the app's job once the write is queued, and no app
       is running here - so the first B arms the discard question and the second acts on it.
       The level it then unwinds to is the module row it came from. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (!store.nav.settings_discard_armed ||
        store.nav.settings_section != MESH_UI_SETTINGS_TELEMETRY) {
        failure = "B with edits pending should ask before discarding";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_MODULES ||
        store.nav.settings_parent != MESH_UI_SETTINGS_NO_SECTION ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != telemetry_row) {
        failure = "B should return to the Modules list at the module's row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != modules_row ||
        mesh_ui_settings_root_at(&store.settings, store.nav.cursor[MESH_UI_SCREEN_SETTINGS]) !=
            MESH_UI_SETTINGS_MODULES) {
        failure = "a second B should return to the top level at the Modules row";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Editing through the nav: Left/Right and A change rows, the keyboard edits text and gives
   the Compose draft back, Y emits the save, B asks before discarding. */
MESH_TEST_CASE(ui_nav_settings_edit, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_owner = true;
    snprintf(settings.long_name, sizeof settings.long_name, "%s", "Old Name");
    snprintf(settings.short_name, sizeof settings.short_name, "%s", "OLDN");
    settings.has_display = true;
    settings.screen_on_secs = 60U;
    settings.use_12h_clock = false;
    settings.units = 0U;
    settings.has_lora = true;
    mesh_ui_store_set_settings(&store, &settings);
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "half typed");

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY)) {
        failure = "Display should open";
        goto cleanup;
    }
    /* Taken before the first edit rather than written out as a number: what this asserts is
       that an edit does not move the row count, and a literal here would instead assert how
       many rows Display happens to have - which fails every time the section gains one. */
    const size_t rows_before_editing =
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);

    /* Right on Screen on steps to the next preset; Left twice goes back past it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    struct mesh_ui_settings_item item;
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS || store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_DISPLAY_SCREEN_ON ||
        store.nav.settings_edits[0].number != 120U ||
        !mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits,
                               store.nav.settings_edit_count, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        !item.dirty || strcmp(item.value, "2m") != 0) {
        failure = "Right should step the number and stay on the tab";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 0U) {
        failure = "stepping back to the radio's value should drop the edit";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 1U || store.nav.settings_edits[0].number != 30U) {
        failure = "Left should step down";
        goto cleanup;
    }
    /* Down to 12-hour clock: A flips a toggle. Down to Units: Left wraps the enum. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 3U ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_12H) ==
            NULL ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_12H)
                ->number != 1U ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_UNITS) ==
            NULL ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_UNITS)
                ->number != 1U) {
        failure = "toggle and enum edits are wrong";
        goto cleanup;
    }
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != rows_before_editing) {
        failure = "edits must not change the row count";
        goto cleanup;
    }

    /* B asks first; a different key stands the question down; B twice discards. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (!store.nav.settings_discard_armed ||
        store.nav.settings_section != MESH_UI_SETTINGS_DISPLAY) {
        failure = "B with edits should ask, not leave";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (store.nav.settings_discard_armed) {
        failure = "another key should cancel the discard question";
        goto cleanup;
    }
    /* Y saves: the action carries the section and every edit; the nav keeps them until the
       app says so. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_SAVE_SETTINGS || action.section != MESH_UI_SETTINGS_DISPLAY ||
        action.edit_count != 3U || action.edits[0].field != MESH_UI_FIELD_DISPLAY_SCREEN_ON ||
        action.edits[0].number != 30U || store.nav.settings_edit_count != 3U) {
        failure = "Y should emit a save with the edits";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_clear(&store);
    if (store.nav.settings_edit_count != 0U || (store.pending_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "clearing the edits should repaint";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "Y with nothing to save does nothing";
        goto cleanup;
    }

    /* Text: A on Short name opens the keyboard on that field with the value preloaded, the
       Compose draft parked; typing is capped at four bytes; done records the edit. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_test_settings_open(&store, MESH_UI_SETTINGS_USER);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    /* The word on the submit key is this client's to name and the grid's to draw, so it is
       asked for the way the renderer asks: off the layout the nav builds for the job. */
    const struct inkcell_keyboard_layout kb_layout = mesh_ui_nav_kb_layout(&store.nav);
    if (!store.nav.keyboard_open || store.nav.keyboard_field != MESH_UI_FIELD_USER_SHORT_NAME ||
        strcmp(store.nav.draft, "OLDN") != 0 || strcmp(store.nav.draft_saved, "half typed") != 0 ||
        strcmp(inkcell_str((inkcell_str_id)kb_layout.submit_label), "done") != 0) {
        failure = "A on a text row should open the keyboard for it";
        goto cleanup;
    }
    /* Row 0 col 0 of the lower layer is '1': appending at the cap is refused. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (strcmp(store.nav.draft, "OLDN") != 0) {
        failure = "the draft must respect the field's byte cap";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action); /* delete -> OLD */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* '1' -> OLD1 */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (store.nav.keyboard_open || store.nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store.nav.keyboard_field != MESH_UI_FIELD_NONE ||
        strcmp(store.nav.draft, "half typed") != 0 || action.type != MESH_UI_ACTION_NONE ||
        store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_USER_SHORT_NAME ||
        strcmp(store.nav.settings_edits[0].text, "OLD1") != 0) {
        failure = "done should record the text edit and restore the Compose draft";
        goto cleanup;
    }
    /* Reopen and cancel: nothing changes. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.keyboard_open || strcmp(store.nav.draft, "OLD1") != 0) {
        failure = "the keyboard should preload the pending edit";
        goto cleanup;
    }
    store.nav.kb.row = INKCELL_KB_CHAR_ROWS;
    store.nav.kb.col = INKCELL_KB_ACTION_CANCEL;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.keyboard_open || store.nav.settings_edit_count != 1U ||
        strcmp(store.nav.draft, "half typed") != 0) {
        failure = "cancel should keep the edit as it was";
        goto cleanup;
    }
    /* B twice leaves the section with the edits gone. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        store.nav.settings_edit_count != 0U) {
        failure = "B twice should discard and go back";
        goto cleanup;
    }
    /* Left on the section list still switches tabs. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_RADIO) {
        failure = "Left on the section list should switch tabs";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Channel editing through the nav: opening a slot, walking the key choices, typing a key,
   and the confirm overlay that stands between Y and the write. */
MESH_TEST_CASE(ui_nav_channel_edit, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_channels = true;
    /* Every slot answered for, which is what the import row waits on. */
    settings.channels_settled = true;
    settings.channels[0].present = true;
    settings.channels[0].role = 1U;
    settings.channels[0].psk_len = 1U;
    settings.channels[0].psk[0] = 1U;
    settings.channels[1].present = true;
    settings.channels[1].index = 1U;
    settings.channels[1].role = 2U;
    snprintf(settings.channels[1].name, sizeof settings.channels[1].name, "%s", "Team");
    settings.channels[1].psk_len = 16U;
    for (unsigned i = 0; i < 16U; ++i) {
        settings.channels[1].psk[i] = (uint8_t)(0xA0U + i);
    }
    settings.channels[1].position_precision = 13U;
    settings.channels[2].present = true; /* disabled: not listed */
    settings.channels[2].index = 2U;
    settings.has_bluetooth = true;
    settings.pairing_mode = 0U;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    struct mesh_ui_settings_item item;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    mesh_test_settings_open(&store, MESH_UI_SETTINGS_CHANNELS);
    /* Three slots and the import row under them. The share row is not there: this fixture has
       no link, which is what a radio that has not sent a primary looks like. */
    if (store.nav.settings_section != MESH_UI_SETTINGS_CHANNELS ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 4U ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 1U) != 1 ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 2U) != 2 ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        strcmp(item.label, "2 (empty)") != 0 || strstr(item.value, "disabled") == NULL) {
        failure = "the channel list should show every slot, the empty one openable";
        goto cleanup;
    }
    /* The import row is an ACTION row like a slot is, and carries a verb rather than a slot
       number - which is the whole of what keeps one from being read as the other. */
    if (mesh_ui_settings_channel_at_row(&store.settings, NULL, 3U) != -1 ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 3U, &item) ||
        item.kind != INKSTAND_FORM_ACTION ||
        item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS) {
        failure = "the import row should be an action row that is not a channel slot";
        goto cleanup;
    }
    /* An empty slot opens with the same settings rows, role Disabled: that is how a channel is
       added. The count is taken from a slot that is in use rather than written down, because
       what this asserts is that the two are the same list of fields - a field added to a
       channel is added to both.

       They differ by exactly one row, and only at the foot: "Clear this slot" is offered on the
       slot that has something to clear and withheld from the one that does not, which is the
       action table's rule about a press that would do nothing. Asserted as a difference of one
       rather than as two written-down numbers, so a seventh field added to a channel still has
       to appear on both. */
    const uint32_t channel_rows =
        mesh_ui_settings_item_count(&store.settings, NULL, MESH_UI_SETTINGS_CHANNELS, 1U);
    if (channel_rows < 7U ||
        mesh_ui_settings_item_count(&store.settings, NULL, MESH_UI_SETTINGS_CHANNELS, 2U) !=
            channel_rows - 1U ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 2U, 1U,
                               &item) ||
        item.field != MESH_UI_FIELD_CHANNEL_ROLE || item.number != 0U) {
        failure = "an empty slot should open with an editable Disabled role";
        goto cleanup;
    }
    /* And that one row is the clearing verb, last, on the slot that is in use. An empty slot's
       last row is the muted toggle: nothing on it offers the press. */
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U,
                               channel_rows - 1U, &item) ||
        item.kind != INKSTAND_FORM_ACTION || !mesh_ui_settings_item_is_verb(&item) ||
        item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 2U,
                               channel_rows - 2U, &item) ||
        item.field != MESH_UI_FIELD_CHANNEL_MUTED) {
        failure = "a slot in use should end in the clearing verb and an empty one should not";
        goto cleanup;
    }
    /* Nor is it offered on the primary, whose role is not offered either: a radio with no
       primary is off its own mesh, and this is the press that would get it there. */
    for (uint32_t row = 0U;
         row < mesh_ui_settings_item_count(&store.settings, NULL, MESH_UI_SETTINGS_CHANNELS, 0U);
         ++row) {
        if (mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 0U,
                                  row, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL &&
            item.kind == INKSTAND_FORM_ACTION) {
            failure = "the primary slot should not offer the clearing verb";
            goto cleanup;
        }
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.settings_channel != 1U || store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 0U ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != channel_rows ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, 2U,
                               &item) ||
        item.field != MESH_UI_FIELD_CHANNEL_KEY || item.kind != INKSTAND_FORM_KEY ||
        strcmp(item.text, "oKGio6SlpqeoqaqrrK2urw==") != 0 ||
        strstr(item.value, "oKGio6Sl...") == NULL || strstr(item.value, "AES-128") == NULL ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, 5U,
                               &item) ||
        strcmp(item.value, "~3 km") != 0) {
        failure = "A should open channel 1 with all of its rows";
        goto cleanup;
    }
    /* The primary slot's role is not offered. */
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 0U, 1U,
                               &item) ||
        item.field != MESH_UI_FIELD_NONE || strcmp(item.value, "Primary") != 0) {
        failure = "the primary channel's role should be read-only";
        goto cleanup;
    }

    /* Key row: Right walks default / random 128 / random 256 / none / back to keep. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_CHANNEL_KEY ||
        store.nav.settings_edits[0].number != MESH_UI_PSK_DEFAULT ||
        !mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits, 1U,
                               MESH_UI_SETTINGS_CHANNELS, 1U, 2U, &item) ||
        !item.dirty || strcmp(item.value, "default key") != 0 || strlen(item.text) != 24U) {
        failure = "Right on the key should pick the default key and keep the text for the keyboard";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.settings_edits[0].number != MESH_UI_PSK_RANDOM_256) {
        failure = "Right twice more should reach random AES-256";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 0U) {
        failure = "Left back to keep should drop the edit";
        goto cleanup;
    }
    /* A opens the keyboard on the key as base64; a bad key keeps it open; a good one is
       recorded. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.keyboard_field != MESH_UI_FIELD_CHANNEL_KEY ||
        strcmp(store.nav.draft, "oKGio6SlpqeoqaqrrK2urw==") != 0) {
        failure = "A on the key should open the keyboard on the current key as base64";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action); /* 23 chars: not base64 */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (!store.nav.keyboard_open || store.nav.settings_edit_count != 0U) {
        failure = "a truncated key should be refused and the keyboard stay open";
        goto cleanup;
    }
    /* Delete "w=" too, then type "a==": still 16 bytes, last byte different. 'a' is row 2
       col 0 of the lower layer; '=' is row 1 col 2 of the symbol layer, two panels along. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (strcmp(store.nav.draft, "oKGio6SlpqeoqaqrrK2ura==") != 0) {
        failure = "typing on the key keyboard went wrong";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (store.nav.keyboard_open || store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].number != MESH_UI_PSK_TYPED ||
        strcmp(store.nav.settings_edits[0].text, "oKGio6SlpqeoqaqrrK2ura==") != 0) {
        failure = "a valid typed key should be recorded";
        goto cleanup;
    }
    /* Y asks first; B cancels; Y, Up, A saves with the channel slot in the action. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (!store.nav.confirm_open || store.nav.confirm_cursor != 1U ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "Y on a channel should open the confirm overlay on Cancel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE ||
        store.nav.settings_edit_count != 1U) {
        failure = "A on Cancel should close the overlay and keep the edits";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_SAVE_SETTINGS ||
        action.section != MESH_UI_SETTINGS_CHANNELS || action.channel != 1U ||
        action.edit_count != 1U || action.edits[0].field != MESH_UI_FIELD_CHANNEL_KEY) {
        failure = "confirming should emit the save for channel 1";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_clear(&store);
    /* B leaves the channel for the list, then the list for the sections. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.settings_channel != MESH_UI_SETTINGS_NO_CHANNEL ||
        store.nav.settings_section != MESH_UI_SETTINGS_CHANNELS ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 1U) {
        failure = "B should return to the channel list at the same row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        failure = "B again should return to the section list";
        goto cleanup;
    }
    /* Bluetooth asks too; Display does not. */
    if (!mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_BLUETOOTH) ||
        mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_DISPLAY)) {
        failure = "confirm applies to Bluetooth and Channels only";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BLUETOOTH,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        item.field != MESH_UI_FIELD_BT_PIN || strcmp(item.text, "000000") != 0 ||
        mesh_ui_settings_text_max(MESH_UI_FIELD_BT_PIN) != 6U) {
        failure = "the Bluetooth PIN row is wrong";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * "Clear this slot", end to end: the press raises the question, the answer is a write, and what
 * the write says is that the slot is to be emptied rather than saved.
 *
 * The three claims the shape rests on, each a way it could have been wrong. A verb this
 * destructive reaching the radio on the press that selected it would be the trap the confirm
 * table exists to prevent. Leaving as a RADIO_ACTION would send it to the admin verb table,
 * which has no entry for it, and the press would vanish. And carrying the pending edits would
 * put a name onto the air on the way to erasing that name - so the answer drops them, along
 * with the cursor, which was standing on a row that is about to stop existing.
 */
MESH_TEST_CASE(ui_nav_clear_channel, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_channels = true;
    settings.channels_settled = true;
    settings.channels[0].present = true;
    settings.channels[0].role = 1U;
    settings.channels[0].psk_len = 1U;
    settings.channels[0].psk[0] = 1U;
    settings.channels[1].present = true;
    settings.channels[1].index = 1U;
    settings.channels[1].role = 2U;
    snprintf(settings.channels[1].name, sizeof settings.channels[1].name, "%s", "Team");
    settings.channels[1].psk_len = 16U;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    mesh_test_settings_open(&store, MESH_UI_SETTINGS_CHANNELS);
    /* Into slot 1, then down onto the last row - the verb. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    const uint32_t rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    if (store.nav.settings_channel != 1U || rows < 7U) {
        failure = "A should open the slot with the clearing verb under its rows";
        goto cleanup;
    }
    for (uint32_t i = 0; i + 1U < rows; ++i) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    /* The row under the cursor, asked of the public item list rather than of the nav's own
       `settings_current` - that one is declared in nav_internal.h, which is the group's private
       header and not something a suite may reach into. */
    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U,
                               store.nav.cursor[MESH_UI_SCREEN_SETTINGS], &item) ||
        item.kind != INKSTAND_FORM_ACTION ||
        item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
        failure = "the cursor should reach the clearing verb";
        goto cleanup;
    }

    /* An edit in hand, so the answer can be seen to drop it. */
    struct mesh_ui_action edit_action;
    memset(&edit_action, 0, sizeof edit_action);
    store.nav.settings_edit_count = 1U;
    store.nav.settings_edits[0].field = MESH_UI_FIELD_CHANNEL_NAME;
    snprintf(store.nav.settings_edits[0].text, sizeof store.nav.settings_edits[0].text, "%s",
             "Renamed");

    /* A asks rather than acts, and the question opens on Cancel. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.confirm_open || store.nav.confirm_cursor != 1U ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the clearing verb should open the question on Cancel";
        goto cleanup;
    }
    /* Onto the verb and answer. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_SAVE_SETTINGS ||
        action.section != MESH_UI_SETTINGS_CHANNELS || action.channel != 1U ||
        action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
        failure = "confirming should emit a save of that slot carrying the clearing verb";
        goto cleanup;
    }
    /* The write carries no edits - it is on its way to erasing the rows they were typed into -
       but the nav still *holds* them, because whether anything was queued is the app's answer.
       mesh_app_save_settings() consumes them on a positive result and otherwise keeps them and
       says so; dropping them here would lose the user's typing on a clear confirmed with no
       link, which erased nothing. The cursor moves either way: it was standing on a row that
       does not survive a successful clear. */
    if (action.edit_count != 0U || store.nav.settings_edit_count != 1U ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 0U) {
        failure = "clearing should carry no edits, keep the nav's, and move the cursor";
        goto cleanup;
    }

    /* And the sheet names the slot rather than asking about "the channel": eight of them are
       one press away from each other. */
    char text[96];
    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_CHANNELS, 1U,
                                   MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL, text, sizeof text);
    if (strcmp(text, "Clear channel 1?") != 0 ||
        strcmp(mesh_ui_settings_confirm_accept(MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL),
               "Clear the slot") != 0) {
        failure = "the sheet should name the slot and what answering it does";
        goto cleanup;
    }
    /* Its body is its own rather than the Channels section's save text, which promises a
       reconnect and says nothing about a key. */
    char body[256];
    char save_body[256];
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_CHANNELS, MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL,
                                  body, sizeof body);
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_CHANNELS, MESH_UI_SETTINGS_ACTION_NONE,
                                  save_body, sizeof save_body);
    if (body[0] == '\0' || strcmp(body, save_body) == 0) {
        failure = "clearing a slot should not borrow the save sheet's words";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Which disabled slots still offer the clearing verb.
 *
 * The interesting case is the one the obvious predicate gets wrong: a slot whose role, name and
 * key all read empty but which is still carrying something the clear would erase. Each of the
 * five below is reachable - saving the MQTT and module rows sets four of them on a slot that is
 * already disabled, and `id` is the firmware's own hash, left behind by anything that emptied
 * the name and key without clearing the slot. A row withheld there is the one press that would
 * tidy it up being the press that is missing.
 *
 * Walked field by field rather than asserted once, so a field added to a channel and forgotten
 * here fails on that field by name.
 */
MESH_TEST_CASE(ui_settings_clear_row_follows_every_cleared_field, unit) {
    struct mesh_ui_settings settings;
    char message[160];

    /* Truly empty: role disabled and every field at its zero. Nothing to clear, no row. */
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_channels = true;
    settings.channels_settled = true;
    settings.channels[0].present = true;
    settings.channels[0].role = 1U; /* a primary, so slot 1 is an ordinary secondary slot */
    settings.channels[1].present = true;
    settings.channels[1].index = 1U;
    const struct mesh_ui_settings empty = settings;

    bool found = false;
    uint32_t count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_CHANNELS, 1U);
    for (uint32_t row = 0; row < count; ++row) {
        struct mesh_ui_settings_item item;
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, row,
                                  &item) &&
            item.kind == INKSTAND_FORM_ACTION &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
            found = true;
        }
    }
    MESH_TEST_FAIL_IF(found, "an empty slot should not offer a press that would change nothing");

    /* And now one field at a time, each on an otherwise-empty disabled slot. */
    for (unsigned field = 0; field < 7U; ++field) {
        settings = empty;
        struct mesh_ui_channel_detail *slot = &settings.channels[1];
        const char *name = NULL;
        switch (field) {
        case 0:
            slot->role = 2U;
            name = "role";
            break;
        case 1:
            snprintf(slot->name, sizeof slot->name, "%s", "Team");
            name = "name";
            break;
        case 2:
            slot->psk_len = 16U;
            name = "key";
            break;
        case 3:
            slot->id = 77U;
            name = "id";
            break;
        case 4:
            slot->uplink_enabled = true;
            name = "uplink";
            break;
        case 5:
            slot->downlink_enabled = true;
            name = "downlink";
            break;
        default:
            slot->position_precision = 13U;
            name = "position precision";
            break;
        }
        found = false;
        count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_CHANNELS, 1U);
        for (uint32_t row = 0; row < count; ++row) {
            struct mesh_ui_settings_item item;
            if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, row,
                                      &item) &&
                item.kind == INKSTAND_FORM_ACTION &&
                item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
                found = true;
            }
        }
        if (!found) {
            snprintf(message, sizeof message,
                     "a disabled slot still holding a %s should offer the clearing verb", name);
            record_failure(test_name, message);
            return;
        }
    }

    /* Muted is the seventh and is its own case: a bool on the same submessage as the sixth. */
    settings = empty;
    settings.channels[1].is_muted = true;
    found = false;
    count = mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_CHANNELS, 1U);
    for (uint32_t row = 0; row < count; ++row) {
        struct mesh_ui_settings_item item;
        if (mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, row,
                                  &item) &&
            item.kind == INKSTAND_FORM_ACTION &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
            found = true;
        }
    }
    MESH_TEST_FAIL_IF(!found, "a disabled slot left muted should offer the clearing verb");
    record_success(test_name);
}

/*
 * The row a verb is on in a section as built, or MESH_UI_SETTINGS_ITEMS_MAX when it is not there.
 * The details page leads with whatever About radio has to say, so how far down Reboot sits is a
 * fact about the fixture's metadata rather than something a test should count to.
 */
static uint32_t settings_row_of(const struct mesh_ui_store *store,
                                enum mesh_ui_settings_section section,
                                enum mesh_ui_settings_action verb) {
    struct mesh_ui_settings_item item;
    for (uint32_t row = 0U; row < MESH_UI_SETTINGS_ITEMS_MAX; ++row) {
        if (!mesh_ui_settings_item(&store->settings, &store->handshake, NULL, 0U, section,
                                   MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
            break;
        }
        if ((item.kind == INKSTAND_FORM_ACTION || item.kind == INKSTAND_FORM_ACTION_OFF) &&
            item.field == MESH_UI_FIELD_NONE && item.number == (uint32_t)verb) {
            return row;
        }
    }
    return MESH_UI_SETTINGS_ITEMS_MAX;
}

/*
 * The verbs done to a radio, as the user walks them on the Radio tab: A opens the question
 * rather than doing the thing, Cancel is where the cursor starts, and only the answer emits an
 * action. The node lists need no config fragment, so they are reachable as soon as the
 * handshake has told us our own node number.
 */
MESH_TEST_CASE(ui_nav_radio_actions, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.can_shutdown = true;
    mesh_ui_store_set_settings(&store, &settings);

    if (!mesh_ui_settings_section_loaded(&store.settings, &store.handshake,
                                         MESH_UI_SETTINGS_NODE_LISTS) ||
        mesh_ui_settings_section_loaded(&store.settings, NULL, MESH_UI_SETTINGS_NODE_LISTS)) {
        failure = "the node lists need our node number or a roster to drop";
        goto cleanup;
    }
    /* A cached roster with no link opens it too - for the forget rows, which send nothing. */
    struct mesh_ui_handshake_state offline = store.handshake;
    offline.has_my_info = false;
    if (!mesh_ui_settings_section_loaded(&store.settings, &offline, MESH_UI_SETTINGS_NODE_LISTS)) {
        failure = "a cached roster should open the node lists with no link";
        goto cleanup;
    }
    offline.node_count = 0U;
    if (mesh_ui_settings_section_loaded(&store.settings, &offline, MESH_UI_SETTINGS_NODE_LISTS)) {
        failure = "with no link and no roster there is nothing in the node lists to press";
        goto cleanup;
    }

    /* The node lists open on the reset, not on the heading above it: a heading is not a row the
       cursor may stand on, so the walk steps over it, and Up from there stays put rather than
       parking on a row where A would do nothing and the action bar would still promise it. */
    struct mesh_ui_action action;
    struct mesh_ui_settings_item item;
    if (!mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_NODE_LISTS) ||
        !mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL, 0U,
                               &item) ||
        item.kind != INKSTAND_FORM_HEADING) {
        failure = "the node lists should open under their first heading";
        goto cleanup;
    }
    if (store.nav.cursor[MESH_UI_SCREEN_RADIO] != 1U ||
        settings_row_of(&store, MESH_UI_SETTINGS_NODE_LISTS,
                        MESH_UI_SETTINGS_ACTION_RESET_NODEDB) != 1U) {
        failure = "the node lists should open on the reset rather than on the heading above it";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_RADIO] != 1U) {
        failure = "UP off the first real row should not land on a heading";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* Reboot, on the Radio card's details page. */
    const uint32_t reboot =
        settings_row_of(&store, MESH_UI_SETTINGS_RADIO_DETAILS, MESH_UI_SETTINGS_ACTION_REBOOT);
    if (reboot >= MESH_UI_SETTINGS_ITEMS_MAX ||
        !mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_RADIO_DETAILS) ||
        !mesh_test_settings_cursor_to(&store, reboot)) {
        failure = "Reboot should be a row of the Radio card's details";
        goto cleanup;
    }

    /* A opens the question on Cancel, and asking is not doing. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.confirm_open || store.nav.confirm_cursor != 1U ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_REBOOT ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "A on Reboot should open the confirm overlay on Cancel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_NONE) {
        failure = "A on Cancel should close the overlay without acting";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE ||
        mesh_ui_nav_open_section(&store.nav) != (uint8_t)MESH_UI_SETTINGS_RADIO_DETAILS) {
        failure = "B should back out of the overlay and no further";
        goto cleanup;
    }

    /*
     * Open it again, move onto the verb, and answer: that is the only press that acts.
     *
     * With an edit waiting on the Settings tab, which is the case a shared confirm sheet has to
     * get right: a pending edit is carried by a radio action because "Set fixed position" reads
     * the rows above it, and a Reboot pressed on another tab carrying half-typed coordinates
     * would be a write nobody made.
     */
    store.nav.settings_edits[0].field = (uint16_t)MESH_UI_FIELD_POSITION_SMART;
    store.nav.settings_edits[0].number = 1U;
    store.nav.settings_edit_count = 1U;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_RADIO_ACTION ||
        action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_REBOOT ||
        action.section != MESH_UI_SETTINGS_RADIO_DETAILS || action.edit_count != 0U) {
        failure = "confirming should emit the reboot and carry none of the Settings tab's edits";
        goto cleanup;
    }

    char text[96];
    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_RADIO_DETAILS, MESH_UI_SETTINGS_NO_CHANNEL,
                                   MESH_UI_SETTINGS_ACTION_REBOOT, text, sizeof text);
    if (strcmp(text, "Reboot the radio?") != 0 ||
        strcmp(mesh_ui_settings_confirm_accept(MESH_UI_SETTINGS_ACTION_REBOOT), "Reboot now") !=
            0 ||
        strcmp(mesh_ui_settings_confirm_accept(MESH_UI_SETTINGS_ACTION_NONE), "Save to radio") !=
            0) {
        failure = "the overlay should name what it is standing in front of";
        goto cleanup;
    }
    /* A save keeps its own title, channel slot included. */
    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_CHANNELS, 1U, MESH_UI_SETTINGS_ACTION_NONE,
                                   text, sizeof text);
    if (strcmp(text, "Save channel 1?") != 0) {
        failure = "a channel save should still say which slot";
        goto cleanup;
    }

    /* A board that cannot cut its own power says so rather than offering a press that the
       firmware would drop on the floor. */
    settings.can_shutdown = false;
    mesh_ui_store_set_settings(&store, &settings);
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_RADIO_DETAILS, MESH_UI_SETTINGS_NO_CHANNEL,
                               reboot + 1U, &item) ||
        item.kind != INKSTAND_FORM_ACTION_OFF || strcmp(item.value, "not supported") != 0) {
        failure = "Shutdown should be a withdrawn verb on a board that cannot shut down";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The two rows of the node lists that are not radio actions: they drop this client's own cached
 * roster and send nothing at all. They sit under the radio's own reset because a NodeDB reset
 * leaves that roster standing on purpose, and this is the page the user is on when the Nodes
 * tab keeps saying eighty-one after the radio's database says two.
 */
MESH_TEST_CASE(ui_nav_forget_nodes, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.can_shutdown = true;
    mesh_ui_store_set_settings(&store, &settings);

    if (mesh_ui_settings_action_is_radio(MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES) ||
        !mesh_ui_settings_action_is_forget(MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES) ||
        !mesh_ui_settings_action_needs_confirm(MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES) ||
        mesh_ui_settings_action_is_forget(MESH_UI_SETTINGS_ACTION_RESET_NODEDB)) {
        failure = "forgetting nodes is a confirmed action the radio never hears about";
        goto cleanup;
    }

    /* Every row here reads the count the app published for it, and that count is what the
       press would remove - so with nothing to remove the offer is withdrawn: the same verb,
       still in the same place in the column, saying why instead of opening a question. The
       fixture publishes no forget counts, which is the state before the first sync fills them. */
    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL, 3U,
                               &item) ||
        strcmp(item.label, "Forget off-radio") != 0 || item.kind != INKSTAND_FORM_ACTION_OFF ||
        strcmp(item.value, "nothing to drop") != 0) {
        failure = "with nothing off-radio the first forget row should be a withdrawn verb";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL, 4U,
                               &item) ||
        strcmp(item.label, "Forget all cached") != 0 || item.kind != INKSTAND_FORM_ACTION_OFF ||
        strcmp(item.value, "nothing to drop") != 0) {
        failure = "an empty forget count should not draw as a press";
        goto cleanup;
    }

    /* Now the state a NodeDB reset leaves: two of the three are only ours, and a forget would
       take both. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.nodes[1].in_nodedb = false;
    handshake.nodes[2].in_nodedb = false;
    handshake.nodes_forgettable_off_radio = 2U;
    handshake.nodes_forgettable_all = 2U;
    handshake.my_info.nodedb_entries = 1U;
    mesh_ui_store_set_handshake(&store, &handshake);

    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL, 3U,
                               &item) ||
        item.kind != INKSTAND_FORM_ACTION || strcmp(item.value, "2 nodes") != 0 ||
        item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES) {
        failure = "the row should say how many nodes it would forget";
        goto cleanup;
    }

    /* The case the row must not get wrong: two nodes are off the radio and a forget keeps
       both, so the offer is withdrawn even though the Nodes tab still marks two rows "off
       radio". A press that would drop nothing must never be offered. */
    struct mesh_ui_handshake_state pinned = handshake;
    pinned.nodes[1].is_favorite = true;
    pinned.nodes[2].is_favorite = true;
    pinned.nodes_forgettable_off_radio = 0U;
    pinned.nodes_forgettable_all = 0U;
    mesh_ui_store_set_handshake(&store, &pinned);
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL, 3U,
                               &item) ||
        item.kind != INKSTAND_FORM_ACTION_OFF || strcmp(item.value, "nothing to drop") != 0) {
        failure = "a roster of pinned orphans should offer no press at all";
        goto cleanup;
    }
    /* The tab still says so: being pinned does not put a node back on the radio. */
    if (mesh_ui_handshake_off_radio(&store.handshake) != 2U) {
        failure = "the Nodes tab should still count a pinned node the radio has forgotten";
        goto cleanup;
    }
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_action action;
    if (!mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_NODE_LISTS)) {
        failure = "the node lists did not open";
        goto cleanup;
    }
    /* Down to the first forget row, which is row 3: "Nodes on the radio" and its one row, then
       "Nodes cached here". The page opens on the reset rather than on the heading above it and
       the walk steps over the heading between, so this walks to the row rather than counting
       presses - what the test is about is which row the forget verb is on. */
    if (!mesh_test_settings_cursor_to(&store, 3U)) {
        failure = "the walk should land on the first forget row";
        goto cleanup;
    }

    /* A asks first, like every other row in this section: a forgotten node comes back only
       when it speaks again. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES) {
        failure = "A on the forget row should open the confirm overlay";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_FORGET_NODES ||
        action.number != 0U) {
        failure = "confirming should ask the client to drop the off-radio nodes";
        goto cleanup;
    }

    /* One row further down is the same press for the whole roster. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_FORGET_NODES || action.number != 1U) {
        failure = "the second row should empty the roster rather than trim it";
        goto cleanup;
    }

    char text[160];
    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL,
                                   MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES, text,
                                   sizeof text);
    if (strcmp(text, "Forget off-radio nodes?") != 0) {
        failure = "the overlay should name what it is about to forget";
        goto cleanup;
    }
    mesh_ui_settings_confirm_text(MESH_UI_SETTINGS_NODE_LISTS,
                                  MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES, text,
                                  sizeof text);
    if (strstr(text, "radio") == NULL) {
        failure = "the overlay should say the radio's own database is untouched";
        goto cleanup;
    }

    /* With the link gone the roster is still ours to drop, so the rows stay pressable while
       the reset, which needs an AdminMessage, says why it cannot be.
       The link is what goes, not our knowledge of the radio: has_my_info now survives a drop,
       so clearing that here would be describing a state the client is never in. */
    handshake.link_up = false;
    mesh_ui_store_set_handshake(&store, &handshake);
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL, 1U,
                               &item) ||
        item.kind != INKSTAND_FORM_ACTION_OFF || strcmp(item.value, "not connected") != 0) {
        failure = "the reset should say why it cannot be pressed with no link";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_NO_CHANNEL, 3U,
                               &item) ||
        item.kind != INKSTAND_FORM_ACTION) {
        failure = "forgetting cached nodes needs no radio";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The Position section's fixed-position rows. They are radio actions, so they do not wait for
 * Y - but unlike the reboot and the resets they are not destructive, so they do not go through
 * the confirm overlay either, and they carry the section's pending edits because "Set fixed
 * position" is a row that reads the three rows above it.
 */
/*
 * Two flags flipped in one section: two edits, two rows dirty, and the rest of the word left
 * alone.
 *
 * The thing being held is that the ten rows are *independent* presses over one value. The
 * shape that would fail this is the obvious alternative - one pending edit for the whole word -
 * which flips the right bit and marks all ten rows changed, so the panel says the user edited
 * nine settings they never touched.
 */
MESH_TEST_CASE(ui_nav_position_flags_edit_one_bit_each, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_position = true;
    /* Asked of the field table rather than written as a literal: which bit this row is, is
       pinned against the protobuf over in ui_settings.c, and once is enough. */
    settings.position_flags = mesh_ui_settings_field_bit(MESH_UI_FIELD_POSITION_FLAG_ALTITUDE);
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_POSITION)) {
        failure = "the Position section should open";
        goto cleanup;
    }

    /* Find the rows rather than counting to them: a heading is stepped over, and the order of
       the section above them is not this test's business. */
    const uint32_t rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    uint32_t altitude_row = rows;
    uint32_t speed_row = rows;
    for (uint32_t row = 0; row < rows; ++row) {
        struct mesh_ui_settings_item item;
        if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                                   MESH_UI_SETTINGS_POSITION, MESH_UI_SETTINGS_NO_CHANNEL, row,
                                   &item)) {
            continue;
        }
        if (item.field == MESH_UI_FIELD_POSITION_FLAG_ALTITUDE) {
            altitude_row = row;
        }
        if (item.field == MESH_UI_FIELD_POSITION_FLAG_SPEED) {
            speed_row = row;
        }
    }
    if (altitude_row >= rows || speed_row >= rows) {
        failure = "the Position section is missing its flag rows";
        goto cleanup;
    }

    /* Right on the altitude row turns the one bit that was on off; Right on speed turns
       another on. Two presses, two edits. */
    if (!mesh_test_settings_cursor_to(&store, altitude_row)) {
        failure = "the cursor should reach the altitude flag";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (!mesh_test_settings_cursor_to(&store, speed_row)) {
        failure = "the cursor should reach the speed flag";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);

    if (store.nav.settings_edit_count != 2U) {
        failure = "two flags flipped should be two pending edits";
        goto cleanup;
    }
    bool altitude_off = false;
    bool speed_on = false;
    for (uint8_t i = 0; i < store.nav.settings_edit_count; ++i) {
        const struct mesh_ui_setting_edit *edit = &store.nav.settings_edits[i];
        altitude_off = altitude_off ||
                       (edit->field == MESH_UI_FIELD_POSITION_FLAG_ALTITUDE && edit->number == 0U);
        speed_on =
            speed_on || (edit->field == MESH_UI_FIELD_POSITION_FLAG_SPEED && edit->number == 1U);
    }
    if (!altitude_off || !speed_on) {
        failure = "each flag should be edited as its own field";
        goto cleanup;
    }

    /* Exactly the two pressed rows are dirty, and every other flag still reads the radio's. */
    const uint32_t count = mesh_ui_settings_group_count(MESH_UI_FIELD_GROUP_POSITION_FLAGS);
    for (uint32_t i = 0; i < count; ++i) {
        const enum mesh_ui_setting_field field =
            mesh_ui_settings_group_field(MESH_UI_FIELD_GROUP_POSITION_FLAGS, i);
        struct mesh_ui_settings_item item;
        if (!mesh_ui_settings_item(&store.settings, &store.handshake, store.nav.settings_edits,
                                   store.nav.settings_edit_count, MESH_UI_SETTINGS_POSITION,
                                   MESH_UI_SETTINGS_NO_CHANNEL,
                                   altitude_row + (field - MESH_UI_FIELD_POSITION_FLAG_ALTITUDE),
                                   &item)) {
            failure = "a flag row went missing under the edits";
            goto cleanup;
        }
        const bool pressed = field == MESH_UI_FIELD_POSITION_FLAG_ALTITUDE ||
                             field == MESH_UI_FIELD_POSITION_FLAG_SPEED;
        if (item.dirty != pressed) {
            failure = "only the flag rows that were pressed should be marked changed";
            goto cleanup;
        }
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_nav_fixed_position, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_position = true;
    settings.fixed_position = false;
    settings.has_own_position = true;
    settings.own_latitude_i = 446488000;
    settings.own_longitude_i = -635752000;
    mesh_ui_store_set_settings(&store, &settings);

    if (mesh_ui_settings_action_needs_confirm(MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION) ||
        !mesh_ui_settings_action_is_radio(MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION) ||
        !mesh_ui_settings_action_needs_confirm(MESH_UI_SETTINGS_ACTION_REBOOT)) {
        failure = "setting a position is a radio action but not a confirmed one";
        goto cleanup;
    }

    /* The coordinate rows start from where the radio says it is, and "Fixed position" is a
       fact rather than a toggle: the firmware moves that flag itself. */
    struct mesh_ui_settings_item item;
    uint32_t count = mesh_ui_settings_item_count(
        &store.settings, &store.handshake, MESH_UI_SETTINGS_POSITION, MESH_UI_SETTINGS_NO_CHANNEL);
    uint32_t latitude_row = count;
    uint32_t set_row = count;
    uint32_t fixed_row = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                                   MESH_UI_SETTINGS_POSITION, MESH_UI_SETTINGS_NO_CHANNEL, i,
                                   &item)) {
            break;
        }
        if (item.field == MESH_UI_FIELD_POSITION_LATITUDE) {
            latitude_row = i;
            if (strcmp(item.text, "44.64880") != 0) {
                failure = "the latitude row should start from the radio's own fix";
                goto cleanup;
            }
        }
        if (strcmp(item.label, "Fixed position") == 0) {
            fixed_row = i;
            if (item.kind != INKSTAND_FORM_TOGGLE || item.field != MESH_UI_FIELD_NONE) {
                failure = "Fixed position should be shown, not offered";
                goto cleanup;
            }
        }
        if (item.kind == INKSTAND_FORM_ACTION &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION) {
            set_row = i;
        }
    }
    if (latitude_row >= count || set_row >= count || fixed_row >= count) {
        failure = "the Position section is missing its fixed-position rows";
        goto cleanup;
    }
    /* Nothing to clear while it is off, so that row is not offered. */
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                                  MESH_UI_SETTINGS_POSITION, MESH_UI_SETTINGS_NO_CHANNEL, i,
                                  &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION &&
            item.kind == INKSTAND_FORM_ACTION) {
            failure = "Clear fixed position should not be offered when it is off";
            goto cleanup;
        }
    }

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    mesh_test_settings_open(&store, MESH_UI_SETTINGS_POSITION);
    if (store.nav.settings_section != MESH_UI_SETTINGS_POSITION) {
        failure = "the Position section should open";
        goto cleanup;
    }
    /* Type a latitude, then press the action row: the edit rides along with it. Walked to by
       row rather than by a count of presses - the section has a heading in it now, and the
       cursor steps over one. */
    if (!mesh_test_settings_cursor_to(&store, latitude_row)) {
        failure = "the cursor should reach the latitude row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.keyboard_open ||
        store.nav.keyboard_field != (uint16_t)MESH_UI_FIELD_POSITION_LATITUDE) {
        failure = "A on the latitude row should open the keyboard on it";
        goto cleanup;
    }
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "45.0");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_POSITION_LATITUDE) {
        failure = "the typed latitude should be recorded as a pending edit";
        goto cleanup;
    }
    if (!mesh_test_settings_cursor_to(&store, set_row)) {
        failure = "the cursor should reach the set-fixed-position row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open) {
        failure = "setting a position should not ask first";
        goto cleanup;
    }
    if (action.type != MESH_UI_ACTION_RADIO_ACTION ||
        action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION ||
        action.section != MESH_UI_SETTINGS_POSITION || action.edit_count != 1U ||
        action.edits[0].field != MESH_UI_FIELD_POSITION_LATITUDE ||
        strcmp(action.edits[0].text, "45.0") != 0) {
        failure = "the action should carry the coordinate rows it reads";
        goto cleanup;
    }

    /*
     * The section has two presses that write, and each must leave the other's pending work
     * alone. Y saves PositionConfig and the typed latitude stays waiting for its own row;
     * the row fires and the GPS edit stays waiting for Y.
     */
    if (mesh_ui_settings_field_consumer(MESH_UI_FIELD_POSITION_LATITUDE) !=
            MESH_UI_SETTING_CONSUMER_FIXED_POSITION ||
        mesh_ui_settings_field_consumer(MESH_UI_FIELD_POSITION_GPS_MODE) !=
            MESH_UI_SETTING_CONSUMER_SECTION) {
        failure = "the coordinate rows and the GPS rows are written by different presses";
        goto cleanup;
    }
    /* Add a GPS-mode edit beside the latitude one, then consume each side in turn. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    while (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] > 0U) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.settings_edit_count != 2U) {
        failure = "the GPS row should record an edit beside the latitude one";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_consumed(&store, MESH_UI_SETTING_CONSUMER_SECTION);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_POSITION_LATITUDE ||
        strcmp(store.nav.settings_edits[0].text, "45.0") != 0) {
        failure = "a section save should leave the typed latitude pending";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_consumed(&store, MESH_UI_SETTING_CONSUMER_FIXED_POSITION);
    if (store.nav.settings_edit_count != 0U) {
        failure = "the fixed-position row should consume the coordinate edits";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A canned slot cannot carry the character that separates the slots.
 *
 * The radio's list is one '|'-separated string, so a slot holding a '|' comes back as two
 * messages, shifts every slot after it, and pushes the entries this screen never showed off
 * the end of the wire's 200 bytes. The keyboard's symbols layer has a '|' on it - row 3,
 * column 6 - so this is reachable by typing rather than only in theory.
 *
 * Filtered as the edit is committed rather than refused at the save, so the row shows exactly
 * what the radio will be sent.
 */
MESH_TEST_CASE(ui_nav_canned_separator, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_canned_messages = true;
    snprintf(settings.canned_messages, sizeof settings.canned_messages, "%s", "one|two");
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_CANNED)) {
        failure = "the canned section should open from the Modules list";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.keyboard_field != MESH_UI_FIELD_CANNED_0 ||
        strcmp(store.nav.draft, "one") != 0) {
        failure = "A on a slot should open the keyboard preloaded with it";
        goto cleanup;
    }
    /* Typed rather than assembled a keypress at a time: what matters is what the commit does
       with a separator in the draft, not how it got there. */
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "meet|later");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (store.nav.keyboard_open || store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_CANNED_0 ||
        strcmp(store.nav.settings_edits[0].text, "meetlater") != 0) {
        failure = "the separator should be dropped as the edit is committed";
        goto cleanup;
    }
    /* Only the canned slots reserve one: an ordinary text field takes whatever is typed. */
    if (mesh_ui_settings_field_reserved_char(MESH_UI_FIELD_CANNED_5) != '|' ||
        mesh_ui_settings_field_reserved_char(MESH_UI_FIELD_MQTT_ROOT) != '\0') {
        failure = "only the canned slots should reserve the separator";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The verbs done to a radio are gated on the session having a send path, not on knowing our own
 * node number.
 *
 * Those were the same question only while a drop cleared has_my_info. They stopped being the
 * same when what the radio *is* began surviving a reconnect, and the rows here are the ones
 * that matter: reboot, shutdown, NodeDB reset, backup/restore and the two factory resets all
 * send an AdminMessage, so offering them over a dead link means a confirm dialog followed by
 * -ENOTCONN. The Radio tab's pages still open - the two forget rows are local and always work -
 * and the rest render as "not connected" rather than disappearing, because a page whose length
 * changes when the radio drops moves the cursor out from under the user.
 *
 * The persisted handshake is why this was worth a test of its own: has_my_info is written to
 * disk and restored, so a cold start with a cached roster and nothing connected reached this
 * code with has_my_info already true.
 */
MESH_TEST_CASE(ui_settings_actions_need_a_live_link, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.can_shutdown = true;
    mesh_ui_store_set_settings(&store, &settings);

    /* A radio we know everything about, whose link has gone: exactly what a reconnect leaves
       behind now, and what a restored roster looks like before the first connect. */
    struct mesh_ui_handshake_state dropped = store.handshake;
    dropped.link_up = false;
    /* Something for the local rows to actually offer, so "the forget rows stay pressable" is a
       claim about the gate rather than about an empty roster. */
    dropped.nodes_forgettable_off_radio = 1U;
    dropped.nodes_forgettable_all = 2U;
    if (!dropped.has_my_info || dropped.node_count == 0U) {
        failure = "the fixture should still know the radio after the link goes";
        goto cleanup;
    }

    /*
     * Both Radio tab pages, which are where these rows live now: the details page's power,
     * backup and factory rows, and the node lists' reset and forget rows. Counted together
     * because the claim is about the rows, not the page - and counted by what a row *sends*,
     * because the details page also carries the firmware rows, which talk to upstream rather
     * than to the radio and are rightly pressable with nothing attached.
     */
    static const enum mesh_ui_settings_section k_pages[] = {
        MESH_UI_SETTINGS_RADIO_DETAILS,
        MESH_UI_SETTINGS_NODE_LISTS,
    };
    unsigned radio_pressable = 0U;
    unsigned forget_pressable = 0U;
    unsigned not_connected = 0U;
    unsigned live_radio_pressable = 0U;
    for (size_t p = 0; p < sizeof k_pages / sizeof k_pages[0]; ++p) {
        const enum mesh_ui_settings_section page = k_pages[p];
        /* The page still opens, because the forget rows send nothing. */
        if (!mesh_ui_settings_section_loaded(&store.settings, &dropped, page)) {
            failure = "a cached roster should still open both pages";
            goto cleanup;
        }
        const uint32_t count = mesh_ui_settings_item_count(&store.settings, &dropped, page,
                                                           MESH_UI_SETTINGS_NO_CHANNEL);
        for (uint32_t row = 0; row < count; ++row) {
            struct mesh_ui_settings_item item;
            if (!mesh_ui_settings_item(&store.settings, &dropped, NULL, 0U, page,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
                failure = "row should exist";
                goto cleanup;
            }
            const enum mesh_ui_settings_action which = (enum mesh_ui_settings_action)item.number;
            if (item.kind == INKSTAND_FORM_ACTION && item.field == MESH_UI_FIELD_NONE) {
                radio_pressable += mesh_ui_settings_action_is_radio(which) ? 1U : 0U;
                forget_pressable += mesh_ui_settings_action_is_forget(which) ? 1U : 0U;
            }
            if (item.kind == INKSTAND_FORM_ACTION_OFF &&
                strcmp(item.value, inkcell_str(MESH_STR_SETTINGS_NOT_CONNECTED)) == 0) {
                ++not_connected;
            }
        }

        /* And with the link back, the same rows are pressable again - the gate is the link,
           not something that latched - on a page that has not changed length under the
           reader. */
        struct mesh_ui_handshake_state live = dropped;
        live.link_up = true;
        if (mesh_ui_settings_item_count(&store.settings, &live, page,
                                        MESH_UI_SETTINGS_NO_CHANNEL) != count) {
            failure = "a page must not change length when the radio drops";
            goto cleanup;
        }
        for (uint32_t row = 0; row < count; ++row) {
            struct mesh_ui_settings_item item;
            if (mesh_ui_settings_item(&store.settings, &live, NULL, 0U, page,
                                      MESH_UI_SETTINGS_NO_CHANNEL, row, &item) &&
                item.kind == INKSTAND_FORM_ACTION && item.field == MESH_UI_FIELD_NONE &&
                mesh_ui_settings_action_is_radio((enum mesh_ui_settings_action)item.number)) {
                ++live_radio_pressable;
            }
        }
    }
    /* The two forget rows are local - they drop our own cache and send nothing - so they stay
       pressable with no radio in sight. No row that sends an AdminMessage may be. */
    if (radio_pressable != 0U || forget_pressable != 2U) {
        failure = "only the two local forget rows may be pressable with no link";
        goto cleanup;
    }
    if (not_connected == 0U) {
        failure = "the AdminMessage rows should say why they cannot be pressed";
        goto cleanup;
    }
    if (live_radio_pressable == 0U) {
        failure = "a live link should make the AdminMessage rows pressable again";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Ham mode from the nav's side: the call sign is typed, A on the row asks first, and the
 * answer carries the three rows the verb reads.
 *
 * The sheet is the difference from the fixed-position pair above and the reason this is its own
 * case: setting a location is undone by setting another one, and turning the primary channel's
 * encryption off is not undone by pressing the row again.
 *
 * The other half is the consumer split, which is what lets a ham row and a LoRa row sit in one
 * section: Y must leave the call sign pending and the ham press must leave the hop limit.
 */
MESH_TEST_CASE(ui_nav_ham_mode, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.region = 1U;
    settings.hop_limit = 3U;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_settings_item item;
    const uint32_t count = mesh_ui_settings_item_count(
        &store.settings, &store.handshake, MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_NO_CHANNEL);
    uint32_t call_sign_row = count;
    uint32_t ham_row = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                                   MESH_UI_SETTINGS_LORA, MESH_UI_SETTINGS_NO_CHANNEL, i, &item)) {
            break;
        }
        if (item.field == MESH_UI_FIELD_LORA_HAM_CALL_SIGN) {
            call_sign_row = i;
        }
        if (item.kind == INKSTAND_FORM_ACTION &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_SET_HAM_MODE) {
            ham_row = i;
        }
    }
    if (call_sign_row >= count || ham_row >= count) {
        failure = "the LoRa section is missing its ham rows";
        goto cleanup;
    }

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    mesh_test_settings_open(&store, MESH_UI_SETTINGS_LORA);
    if (!mesh_test_settings_cursor_to(&store, call_sign_row)) {
        failure = "the cursor should reach the call sign row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.keyboard_open ||
        store.nav.keyboard_field != (uint16_t)MESH_UI_FIELD_LORA_HAM_CALL_SIGN) {
        failure = "A on the call sign row should open the keyboard on it";
        goto cleanup;
    }
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "KD2ABC");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_LORA_HAM_CALL_SIGN) {
        failure = "the typed call sign should be recorded as a pending edit";
        goto cleanup;
    }

    if (!mesh_test_settings_cursor_to(&store, ham_row)) {
        failure = "the cursor should reach the ham mode row";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.confirm_open || action.type == MESH_UI_ACTION_RADIO_ACTION) {
        failure = "A on the ham row should open the sheet rather than act";
        goto cleanup;
    }
    /* Cancel is where the cursor starts, so the answer has to be moved to on purpose. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_RADIO_ACTION ||
        action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_SET_HAM_MODE ||
        action.section != MESH_UI_SETTINGS_LORA || action.edit_count != 1U ||
        strcmp(action.edits[0].text, "KD2ABC") != 0) {
        failure = "the answer should carry the ham rows the verb reads";
        goto cleanup;
    }

    /* And the two presses leave each other's work alone. */
    if (!mesh_test_settings_cursor_to(&store, 6U)) {
        failure = "the cursor should reach the hop limit row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.settings_edit_count != 2U) {
        failure = "the hop limit should record an edit beside the call sign";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_consumed(&store, MESH_UI_SETTING_CONSUMER_SECTION);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_LORA_HAM_CALL_SIGN) {
        failure = "a LoRa save should leave the call sign pending";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_consumed(&store, MESH_UI_SETTING_CONSUMER_HAM_MODE);
    if (store.nav.settings_edit_count != 0U) {
        failure = "the ham press should consume the call sign";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Right on the preset row lands only where the region allows.
 *
 * The press is where the roadmap's complaint actually lives: the section used to offer all
 * thirty-eight regions against all seventeen presets with no constraint, which is a row that
 * can be *set* to something the radio will not honour. The row showing such a value is a
 * different matter and stays possible - a radio configured elsewhere arrives holding one, and
 * this asserts the cursor can still get off it.
 */
MESH_TEST_CASE(ui_nav_lora_preset_steps_inside_the_region, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.use_preset = true;
    settings.region = 1U; /* US */
    /* MEDIUM_SLOW, which the region below does not allow: the radio came from somewhere else. */
    settings.modem_preset = 3U;
    settings.region_presets.loaded = true;
    settings.region_presets.region[1U] = (struct mesh_ui_region_preset){
        /* LONG_FAST and SHORT_TURBO: 0 and 8, with nothing legal in between. */
        .presets = (1U << 0) | (1U << 8),
    };
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_LORA) ||
        !mesh_test_settings_cursor_to(&store, 2U)) {
        failure = "the LoRa preset row should be reachable";
        goto cleanup;
    }
    /* Read through mesh_ui_settings_item() rather than the nav's own accessor for that row:
       the latter lives in src/ui/nav/nav_internal.h, which is the group's private header and not
       something a test may reach into. */
    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits,
                               store.nav.settings_edit_count, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        item.field != MESH_UI_FIELD_LORA_PRESET) {
        failure = "the cursor should be on the preset row";
        goto cleanup;
    }
    /* Sitting on a preset the region does not allow: the row says so and Right gets off it. */
    if (!item.conflict) {
        failure = "a preset the region does not allow should be marked";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_LORA_PRESET ||
        store.nav.settings_edits[0].number != 8U) {
        failure = "Right should skip the presets the region does not allow";
        goto cleanup;
    }
    /* And Right again wraps inside the set rather than walking on through it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.settings_edits[0].number != 0U) {
        failure = "Right off the end of the set should wrap to its first value";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits,
                               store.nav.settings_edit_count, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        item.conflict) {
        failure = "a preset the region allows should not be marked";
        goto cleanup;
    }
    /* Left goes back the way it came, through the same set. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.settings_edits[0].number != 8U) {
        failure = "Left should walk the set backwards";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The shoulders below the tab pair walk a section a card at a time.
 *
 * The cards gave the reader a grouping they could see and had no way to cross: the d-pad walks
 * rows, Left and Right are the editor's inside a section, and L1/R1 are the tabs. Radio actions
 * is five groups and eleven rows, so the last group cost ten presses of a button that skips
 * nothing - which is a card drawn as decoration rather than as structure.
 *
 * Four claims, and each is a way the pair could be useless:
 *
 *   - forward lands on the first row of the next group, not on its title. A cursor may not stand
 *     on a heading, and a jump that left it on one would be refused a press later.
 *   - forward from the last group is refused rather than wrapping, so the two ends of a section
 *     are where they are on every other key.
 *   - back from inside a group goes to the top of *that* group before leaving it. This is the
 *     asymmetry that makes the pair usable with one thumb, and it is the part a "previous
 *     heading" implementation gets wrong.
 *   - back again, from the top of a group, crosses into the one before it.
 *
 * The Radio card's details rather than a section built here, because the rule is about what the
 * renderer cards on and that page is the verbs this was written for, now on the Radio tab - so
 * it also holds that the shoulders cross cards there as they do on the Settings tab. The cursor
 * is walked with presses so the case cannot drift from what the row order actually is.
 */
MESH_TEST_CASE(ui_nav_settings_shoulders_walk_the_cards, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    const char *failure = NULL;
    char message[160];

    if (!mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_RADIO_DETAILS)) {
        failure = "the Radio card's details could not be opened";
    }

    struct mesh_ui_action action;
    uint32_t seen[MESH_UI_SETTINGS_ITEMS_MAX];
    uint32_t groups = 0U;
    if (failure == NULL) {
        /* Forward to the end, recording where each press lands. */
        seen[groups++] = store.nav.cursor[MESH_UI_SCREEN_RADIO];
        for (;;) {
            const uint32_t before = store.nav.cursor[MESH_UI_SCREEN_RADIO];
            memset(&action, 0, sizeof action);
            (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R2, &action);
            const uint32_t after = store.nav.cursor[MESH_UI_SCREEN_RADIO];
            if (after == before) {
                break; /* refused: no group that way */
            }
            if (after < before) {
                failure = "a forward card jump moved the cursor backwards";
                break;
            }
            /* Read through mesh_ui_settings_item() rather than the nav's own accessor, which
               lives in the group's private header - the rule this file follows above. */
            struct mesh_ui_settings_item item;
            if (mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                                      MESH_UI_SETTINGS_RADIO_DETAILS, MESH_UI_SETTINGS_NO_CHANNEL,
                                      after, &item) &&
                item.kind == INKSTAND_FORM_HEADING) {
                failure = "a card jump parked the cursor on a group title";
                break;
            }
            if (groups >= MESH_UI_SETTINGS_ITEMS_MAX) {
                failure = "a card jump never stopped going forward";
                break;
            }
            seen[groups++] = after;
        }
    }
    if (failure == NULL && groups < 3U) {
        snprintf(message, sizeof message,
                 "the details page crossed %u groups, which is too few to be testing anything",
                 (unsigned)groups);
        failure = message;
    }

    /* Back from *inside* the last group goes to that group's own first row, not to the previous
       group - the asymmetry the pair is built on. Stepping down first is what puts the cursor
       inside rather than at the top, and a group of one row has nowhere to step. */
    if (failure == NULL) {
        const uint32_t top = store.nav.cursor[MESH_UI_SCREEN_RADIO];
        memset(&action, 0, sizeof action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
        if (store.nav.cursor[MESH_UI_SCREEN_RADIO] == top) {
            failure = "the last group is one row, so there is no 'inside' to come back from";
        } else {
            memset(&action, 0, sizeof action);
            (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_L2, &action);
            if (store.nav.cursor[MESH_UI_SCREEN_RADIO] != top) {
                snprintf(message, sizeof message,
                         "back from inside a group landed on row %u rather than on its first row "
                         "%u",
                         (unsigned)store.nav.cursor[MESH_UI_SCREEN_RADIO], (unsigned)top);
                failure = message;
            }
        }
    }

    /* And again, from the top, crosses into the group before it - which is where the forward
       walk said it would be. */
    if (failure == NULL) {
        memset(&action, 0, sizeof action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_L2, &action);
        const uint32_t want = seen[groups - 2U];
        if (store.nav.cursor[MESH_UI_SCREEN_RADIO] != want) {
            snprintf(message, sizeof message,
                     "back from the top of a group landed on row %u rather than on the previous "
                     "group's first row %u",
                     (unsigned)store.nav.cursor[MESH_UI_SCREEN_RADIO], (unsigned)want);
            failure = message;
        }
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A chevron on a settings verb is a promise, and the nav is what keeps it.
 *
 * The mark means "this row opens something", and the renderer used to spend it on any verb whose
 * value column was empty - which is true of every verb that opens something, and also of several
 * that do not. "Check for firmware" sends a request and redraws when the answer lands; Language
 * and Theme cycle to the next one; the fixed-position pair goes straight to the radio. Each drew
 * a chevron and raised nothing, which is the action bar's rule - a keycap that does nothing is a
 * bug - one column further right.
 *
 * So the promise is asked of `mesh_ui_settings_action_opens()`, and this is that predicate held
 * against the behaviour rather than against its own list: every section is opened, A is pressed
 * on every verb in it, and the nav is asked whether anything was raised. Written against the
 * behaviour for the reason `help_does_not_offer_a_key_that_does_nothing` is - an equivalence
 * checked against the predicate on both sides passes while both are wrong together.
 */
MESH_TEST_CASE(ui_nav_a_chevron_is_a_promise_the_nav_keeps, unit) {
    const char *failure = NULL;
    static char detail[224];
    unsigned opened = 0U;
    unsigned acted = 0U;

    /* Every section that has a verb in it: the root list, the modules under it, and the Radio
       tab's two pages - which are the rows Radio actions and About radio used to be. */
    static const enum mesh_ui_settings_section k_pages[] = {
        MESH_UI_SETTINGS_RADIO_DETAILS,
        MESH_UI_SETTINGS_NODE_LISTS,
    };
    const uint32_t roots = mesh_ui_settings_root_count(NULL);
    const uint32_t modules = mesh_ui_settings_module_count();
    const uint32_t pages = (uint32_t)(sizeof k_pages / sizeof k_pages[0]);
    for (uint32_t s = 0U; s < roots + modules + pages && failure == NULL; ++s) {
        const enum mesh_ui_settings_section section = s < roots ? mesh_ui_settings_root_at(NULL, s)
                                                      : s < roots + modules
                                                          ? mesh_ui_settings_module_at(s - roots)
                                                          : k_pages[s - roots - modules];
        const bool page = s >= roots + modules;
        /* Modules is a list of subjects rather than of settings - its rows are ACTION and are
           not verbs - and opening one from here would walk into the section it names. */
        if (section == MESH_UI_SETTINGS_MODULES) {
            continue;
        }

        for (uint32_t row = 0U; row < MESH_UI_SETTINGS_ITEMS_MAX && failure == NULL; ++row) {
            /*
             * A store per press. The point of the case is what one press left behind, and a
             * press that opened a sheet has to be undone before the next one - undoing it with
             * B would be testing the way back rather than the way in.
             */
            struct mesh_ui_store store;
            if (mesh_ui_store_init(&store) != 0) {
                failure = "store init failed";
                break;
            }
            mesh_test_nav_populate(&store);
            /*
             * A radio that has answered for everything, so the verbs are offered rather than
             * withdrawn: the link is up (the fixture's own), the metadata is in (the radio
             * actions), the channel table has settled and both links exist (the four rows that
             * open a screen or the keyboard).
             */
            struct mesh_ui_settings settings = store.settings;
            settings.loaded = true;
            settings.has_metadata = true;
            settings.can_shutdown = true;
            settings.has_channels = true;
            settings.channels_settled = true;
            settings.has_position = true;
            settings.has_lora = true;
            snprintf(settings.share_url, sizeof settings.share_url, "%s",
                     "https://meshtastic.org/e/#test");
            snprintf(settings.contact_url, sizeof settings.contact_url, "%s",
                     "https://meshtastic.org/v/#test");
            mesh_ui_store_set_settings(&store, &settings);

            struct mesh_ui_settings_item item;
            const bool reached = page ? mesh_test_open_radio_page(&store, section)
                                      : mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS) &&
                                            mesh_test_settings_open(&store, section);
            if (!reached ||
                !mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U, section,
                                       MESH_UI_SETTINGS_NO_CHANNEL, row, &item)) {
                mesh_ui_store_shutdown(&store);
                break; /* past the end of this section */
            }
            /* Only a verb the renderer would draw a chevron for. A channel slot and a module
               row are ACTION too and open a list, which is what their kind means there. */
            if (item.kind != INKSTAND_FORM_ACTION || !mesh_ui_settings_item_is_verb(&item) ||
                !mesh_test_settings_cursor_to(&store, row)) {
                mesh_ui_store_shutdown(&store);
                continue;
            }

            const enum mesh_ui_settings_action which = (enum mesh_ui_settings_action)item.number;
            struct mesh_ui_action out;
            memset(&out, 0, sizeof out);
            (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &out);
            /* Everything the nav can raise from a settings row: the question, the two code
               screens, and the keyboard the two importing rows open. */
            const bool raised = store.nav.confirm_open || store.nav.share_open ||
                                store.nav.contact_open || store.nav.keyboard_open;
            const bool promised = mesh_ui_settings_action_opens(which);
            if (raised != promised) {
                snprintf(detail, sizeof detail,
                         "\"%s\" in section %u %s a chevron and %s: a row promises what the press "
                         "does",
                         item.label, (unsigned)section, promised ? "draws" : "draws no",
                         raised ? "raised something" : "raised nothing");
                failure = detail;
            }
            opened += raised ? 1U : 0U;
            acted += raised ? 0U : 1U;
            mesh_ui_store_shutdown(&store);
        }
    }

    /* Both halves have to be reached, or the equivalence above is one of them asserted twice. */
    if (failure == NULL && (opened == 0U || acted == 0U)) {
        snprintf(detail, sizeof detail,
                 "the walk found %u verbs that raise something and %u that act where they stand "
                 "- it needs both to be saying anything",
                 opened, acted);
        failure = detail;
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The Radio tab's pages are about the radio in your hand, whatever the Settings tab is pointed at.
 *
 * The choice this holds: while another node is administered over the mesh, a Reboot on the Radio
 * tab - whose cards describe the radio on the link - would reach across the mesh and take
 * somebody else's repeater down. So the verbs done to a *remote* radio stay on the Settings tab,
 * under the banner naming it, as About radio and Radio actions; those two are listed there only
 * while there is a remote target, and the Radio tab's pages say whose radio the tab is pointed at
 * and offer the way back instead of acting on it.
 */
MESH_TEST_CASE(ui_nav_radio_pages_stay_on_the_radio_in_hand, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings = store.settings;
    settings.loaded = true;
    settings.has_metadata = true;
    settings.can_shutdown = true;
    mesh_ui_store_set_settings(&store, &settings);

    /* The radio on the link: neither Settings section is listed - both are on the Radio tab. */
    const uint32_t local_rows = mesh_ui_settings_root_count(&store.settings);
    for (uint32_t i = 0; i < local_rows; ++i) {
        const enum mesh_ui_settings_section section = mesh_ui_settings_root_at(&store.settings, i);
        if (section == MESH_UI_SETTINGS_RADIO || section == MESH_UI_SETTINGS_ACTIONS) {
            failure = "About radio and Radio actions belong to the Radio tab for our own radio";
            goto cleanup;
        }
    }

    /* Pointed at another node, both are listed, and Radio actions opens on whose radio it is. */
    settings.admin_dest = 0x7001U;
    snprintf(settings.admin_dest_name, sizeof settings.admin_dest_name, "%s", "Hill repeater");
    mesh_ui_store_set_settings(&store, &settings);
    if (mesh_ui_settings_root_count(&store.settings) != local_rows + 2U ||
        mesh_ui_settings_root_at(&store.settings, 1U) != MESH_UI_SETTINGS_RADIO) {
        failure = "a remote target should list About radio and Radio actions on the Settings tab";
        goto cleanup;
    }
    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.kind != INKSTAND_FORM_HEADING ||
        settings_row_of(&store, MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_ACTION_REBOOT) >=
            MESH_UI_SETTINGS_ITEMS_MAX ||
        settings_row_of(&store, MESH_UI_SETTINGS_ACTIONS,
                        MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES) < MESH_UI_SETTINGS_ITEMS_MAX) {
        failure = "Radio actions should name the remote radio and hold its verbs, not our roster's";
        goto cleanup;
    }

    /* The Radio tab's details: the target and the way back, and no verb that would reach it. */
    if (mesh_ui_settings_item_count(&store.settings, &store.handshake,
                                    MESH_UI_SETTINGS_RADIO_DETAILS,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 3U ||
        settings_row_of(&store, MESH_UI_SETTINGS_RADIO_DETAILS,
                        MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL) != 2U) {
        failure = "the details page should say whose radio the tab is on and offer the way back";
        goto cleanup;
    }
    /* The node lists keep the forget rows - they touch only this client - and drop the reset,
       which would empty the remote node's database. */
    if (settings_row_of(&store, MESH_UI_SETTINGS_NODE_LISTS, MESH_UI_SETTINGS_ACTION_RESET_NODEDB) <
            MESH_UI_SETTINGS_ITEMS_MAX ||
        settings_row_of(&store, MESH_UI_SETTINGS_NODE_LISTS,
                        MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES) >= MESH_UI_SETTINGS_ITEMS_MAX) {
        failure = "the node lists should keep the forget rows and drop the remote reset";
        goto cleanup;
    }

    struct mesh_ui_action action;
    /* The way back from the Radio tab's details page, with a Settings section open and an edit
       typed against the remote node: the edit goes with the target rather than staying to be
       drawn as - and saved as - a change to the radio in hand. */
    if (!mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS) ||
        !mesh_test_settings_open(&store, MESH_UI_SETTINGS_USER)) {
        failure = "Settings > User should open on a remote target";
        goto cleanup;
    }
    store.nav.settings_edits[0].field = (uint16_t)MESH_UI_FIELD_USER_LICENSED;
    store.nav.settings_edits[0].number = 1U;
    store.nav.settings_edit_count = 1U;
    if (!mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_RADIO_DETAILS) ||
        !mesh_test_settings_cursor_to(&store, 2U)) {
        failure = "the details page's way back should be reachable";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SET_ADMIN_TARGET || action.dest != 0U ||
        store.nav.settings_edit_count != 0U ||
        store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        failure = "leaving the remote node should drop the edits typed against it";
        goto cleanup;
    }
    mesh_ui_store_set_settings(&store, &settings); /* the target is still set for the next check */

    /* The way back, pressed on the Settings tab, lands on the list rather than inside a section
       the list is about to stop having. */
    if (!mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS) ||
        !mesh_test_settings_open(&store, MESH_UI_SETTINGS_RADIO) ||
        !mesh_test_settings_cursor_to(&store, 2U)) {
        failure = "Settings > About radio should open on a remote target";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SET_ADMIN_TARGET || action.dest != 0U ||
        store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        failure = "coming back to our own radio should land on the section list";
        goto cleanup;
    }

    /* And B on a page is the page's: with an edit waiting on the Settings tab it goes back to
       the cards rather than arming a discard of somebody else's typing. */
    store.nav.settings_edits[0].field = (uint16_t)MESH_UI_FIELD_POSITION_SMART;
    store.nav.settings_edits[0].number = 1U;
    store.nav.settings_edit_count = 1U;
    if (!mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_NODE_LISTS)) {
        failure = "the node lists should open";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (!mesh_ui_nav_status_showing(&store.nav) || store.nav.settings_discard_armed ||
        store.nav.settings_edit_count != 1U) {
        failure = "B on a page should leave it and leave the Settings tab's edits alone";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
