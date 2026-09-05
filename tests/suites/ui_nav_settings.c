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
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.use_preset = true;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    /* Messages → Nodes → Devices → Status → Settings. */
    for (int i = 0; i < 4; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) !=
            MESH_UI_SETTINGS_SECTION_COUNT) {
        failure = "Settings tab should open on the section list";
        goto cleanup;
    }
    for (int i = 0; i < MESH_UI_SETTINGS_LORA; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    if (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != MESH_UI_SETTINGS_LORA) {
        failure = "cursor should sit on LoRa";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    const uint32_t lora_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    if (store.nav.settings_section != MESH_UI_SETTINGS_LORA ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 0U || lora_rows == 0U ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "A should open the LoRa section";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 1U) {
        failure = "Down should move within the section";
        goto cleanup;
    }
    /* A on the "Use preset" toggle edits it in place; nothing is sent until Y. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE || store.nav.settings_section != MESH_UI_SETTINGS_LORA ||
        store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_LORA_USE_PRESET) {
        failure = "A on a toggle should record an edit";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    /* The refresh carries the pending edit count so the toast can say they were kept: X and
       Y sit together, and a refresh that reports nothing reads like a save that did nothing. */
    if (action.type != MESH_UI_ACTION_REFRESH_SETTINGS || action.edit_count != 1U) {
        failure = "X should ask for a refresh and report the edits it kept";
        goto cleanup;
    }
    /* B with an edit asks first; B again discards and leaves. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (!store.nav.settings_discard_armed || store.nav.settings_section != MESH_UI_SETTINGS_LORA) {
        failure = "B with an edit should ask before leaving";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != MESH_UI_SETTINGS_LORA ||
        store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        failure = "B should return to the section list at the same row";
        goto cleanup;
    }
    /* An unloaded section opens empty rather than refusing; the backend explains. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_DISPLAY ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 0U) {
        failure = "unloaded section should open with no rows";
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

/* Editing through the nav: Left/Right and A change rows, the keyboard edits text and gives
   the Compose draft back, Y emits the save, B asks before discarding. */
MESH_TEST_CASE(ui_nav_settings_edit, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
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
    for (int i = 0; i < 4; ++i) { /* right to the Settings tab */
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    for (int i = 0; i < MESH_UI_SETTINGS_DISPLAY; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_DISPLAY) {
        failure = "Display should open";
        goto cleanup;
    }

    /* Right on Screen on steps to the next preset; Left twice goes back past it. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 0U) {
        failure = "stepping back to the radio's value should drop the edit";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 1U || store.nav.settings_edits[0].number != 30U) {
        failure = "Left should step down";
        goto cleanup;
    }
    /* Down to 12-hour clock: A flips a toggle. Down to Units: Left wraps the enum. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
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
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 6U) {
        failure = "edits must not change the row count";
        goto cleanup;
    }

    /* B asks first; a different key stands the question down; B twice discards. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (!store.nav.settings_discard_armed ||
        store.nav.settings_section != MESH_UI_SETTINGS_DISPLAY) {
        failure = "B with edits should ask, not leave";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    if (store.nav.settings_discard_armed) {
        failure = "another key should cancel the discard question";
        goto cleanup;
    }
    /* Y saves: the action carries the section and every edit; the nav keeps them until the
       app says so. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "Y with nothing to save does nothing";
        goto cleanup;
    }

    /* Text: A on Short name opens the keyboard on that field with the value preloaded, the
       Compose draft parked; typing is capped at four bytes; done records the edit. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    for (int i = 0; i < MESH_UI_SETTINGS_DISPLAY - MESH_UI_SETTINGS_USER; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.keyboard_field != MESH_UI_FIELD_USER_SHORT_NAME ||
        strcmp(store.nav.draft, "OLDN") != 0 || strcmp(store.nav.draft_saved, "half typed") != 0 ||
        strcmp(mesh_ui_kb_action_label(&store.nav, MESH_UI_KB_ACTION_SEND), "done") != 0) {
        failure = "A on a text row should open the keyboard for it";
        goto cleanup;
    }
    /* Row 0 col 0 of the lower layer is '1': appending at the cap is refused. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (strcmp(store.nav.draft, "OLDN") != 0) {
        failure = "the draft must respect the field's byte cap";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* delete -> OLD */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* '1' -> OLD1 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || strcmp(store.nav.draft, "OLD1") != 0) {
        failure = "the keyboard should preload the pending edit";
        goto cleanup;
    }
    store.nav.kb_row = MESH_UI_KB_CHAR_ROWS;
    store.nav.kb_col = MESH_UI_KB_ACTION_CANCEL;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.keyboard_open || store.nav.settings_edit_count != 1U ||
        strcmp(store.nav.draft, "half typed") != 0) {
        failure = "cancel should keep the edit as it was";
        goto cleanup;
    }
    /* B twice leaves the section with the edits gone. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        store.nav.settings_edit_count != 0U) {
        failure = "B twice should discard and go back";
        goto cleanup;
    }
    /* Left on the section list still switches tabs. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_STATUS) {
        failure = "Left on the section list should switch tabs";
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

/* Channel editing through the nav: opening a slot, walking the key choices, typing a key,
   and the confirm overlay that stands between Y and the write. */
MESH_TEST_CASE(ui_nav_channel_edit, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_channels = true;
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
    for (int i = 0; i < 4; ++i) { /* right to the Settings tab */
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    for (int i = 0; i < MESH_UI_SETTINGS_CHANNELS; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_CHANNELS ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 3U ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 1U) != 1 ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 2U) != 2 ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 3U) != -1 ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        strcmp(item.label, "2 (empty)") != 0 || strstr(item.value, "disabled") == NULL) {
        failure = "the channel list should show every slot, the empty one openable";
        goto cleanup;
    }
    /* An empty slot opens with the same rows, role Disabled: that is how a channel is added. */
    if (mesh_ui_settings_item_count(&store.settings, NULL, MESH_UI_SETTINGS_CHANNELS, 2U) != 6U ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 2U, 1U,
                               &item) ||
        item.field != MESH_UI_FIELD_CHANNEL_ROLE || item.number != 0U) {
        failure = "an empty slot should open with an editable Disabled role";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_channel != 1U || store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 0U ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 6U ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, 2U,
                               &item) ||
        item.field != MESH_UI_FIELD_CHANNEL_KEY || item.kind != MESH_UI_SETTING_KEY ||
        strcmp(item.text, "oKGio6SlpqeoqaqrrK2urw==") != 0 ||
        strstr(item.value, "oKGio6Sl...") == NULL || strstr(item.value, "AES-128") == NULL ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, 5U,
                               &item) ||
        strcmp(item.value, "~3 km") != 0) {
        failure = "A should open channel 1 with its six rows";
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_CHANNEL_KEY ||
        store.nav.settings_edits[0].number != MESH_UI_PSK_DEFAULT ||
        !mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits, 1U,
                               MESH_UI_SETTINGS_CHANNELS, 1U, 2U, &item) ||
        !item.dirty || strcmp(item.value, "default key") != 0 || strlen(item.text) != 24U) {
        failure = "Right on the key should pick the default key and keep the text for the keyboard";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.settings_edits[0].number != MESH_UI_PSK_RANDOM_256) {
        failure = "Right twice more should reach random AES-256";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 0U) {
        failure = "Left back to keep should drop the edit";
        goto cleanup;
    }
    /* A opens the keyboard on the key as base64; a bad key keeps it open; a good one is
       recorded. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.keyboard_field != MESH_UI_FIELD_CHANNEL_KEY ||
        strcmp(store.nav.draft, "oKGio6SlpqeoqaqrrK2urw==") != 0) {
        failure = "A on the key should open the keyboard on the current key as base64";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* 23 chars: not base64 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (!store.nav.keyboard_open || store.nav.settings_edit_count != 0U) {
        failure = "a truncated key should be refused and the keyboard stay open";
        goto cleanup;
    }
    /* Delete "w=" too, then type "a==": still 16 bytes, last byte different. 'a' is row 2
       col 0 of the lower layer; '=' is row 1 col 2 of the symbol layer. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (strcmp(store.nav.draft, "oKGio6SlpqeoqaqrrK2ura==") != 0) {
        failure = "typing on the key keyboard went wrong";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (store.nav.keyboard_open || store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].number != MESH_UI_PSK_TYPED ||
        strcmp(store.nav.settings_edits[0].text, "oKGio6SlpqeoqaqrrK2ura==") != 0) {
        failure = "a valid typed key should be recorded";
        goto cleanup;
    }
    /* Y asks first; B cancels; Y, Up, A saves with the channel slot in the action. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.confirm_open || store.nav.confirm_cursor != 1U ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "Y on a channel should open the confirm overlay on Cancel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE ||
        store.nav.settings_edit_count != 1U) {
        failure = "A on Cancel should close the overlay and keep the edits";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_SAVE_SETTINGS ||
        action.section != MESH_UI_SETTINGS_CHANNELS || action.channel != 1U ||
        action.edit_count != 1U || action.edits[0].field != MESH_UI_FIELD_CHANNEL_KEY) {
        failure = "confirming should emit the save for channel 1";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_clear(&store);
    /* B leaves the channel for the list, then the list for the sections. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_channel != MESH_UI_SETTINGS_NO_CHANNEL ||
        store.nav.settings_section != MESH_UI_SETTINGS_CHANNELS ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 1U) {
        failure = "B should return to the channel list at the same row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
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
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * The Radio actions section as the user walks it: A opens the question rather than doing the
 * thing, Cancel is where the cursor starts, and only the answer emits an action. The section
 * needs no config fragment, so it is reachable as soon as the handshake has told us our own
 * node number.
 */
MESH_TEST_CASE(ui_nav_radio_actions, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.can_shutdown = true;
    mesh_ui_store_set_settings(&store, &settings);

    if (!mesh_ui_settings_section_loaded(&store.settings, &store.handshake,
                                         MESH_UI_SETTINGS_ACTIONS) ||
        mesh_ui_settings_section_loaded(&store.settings, NULL, MESH_UI_SETTINGS_ACTIONS)) {
        failure = "Radio actions needs our node number and nothing else";
        goto cleanup;
    }

    struct mesh_ui_action action;
    for (int i = 0; i < 4; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    for (int i = 0; i < MESH_UI_SETTINGS_ACTIONS; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_ACTIONS ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 5U) {
        failure = "the Radio actions section should open with five rows";
        goto cleanup;
    }

    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&store.settings, &store.handshake, NULL, 0U,
                               MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.kind != MESH_UI_SETTING_ACTION ||
        item.number != (uint32_t)MESH_UI_SETTINGS_ACTION_REBOOT ||
        strcmp(item.label, "Reboot") != 0) {
        failure = "Reboot should be the first row";
        goto cleanup;
    }

    /* A opens the question on Cancel, and asking is not doing. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.confirm_open || store.nav.confirm_cursor != 1U ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_REBOOT ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "A on Reboot should open the confirm overlay on Cancel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_NONE) {
        failure = "A on Cancel should close the overlay without acting";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE) {
        failure = "B should back out of the overlay";
        goto cleanup;
    }

    /* Open it again, move onto the verb, and answer: that is the only press that acts. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_RADIO_ACTION ||
        action.number != (uint32_t)MESH_UI_SETTINGS_ACTION_REBOOT ||
        action.section != MESH_UI_SETTINGS_ACTIONS || action.edit_count != 0U) {
        failure = "confirming should emit the reboot and carry no edits";
        goto cleanup;
    }

    char text[96];
    mesh_ui_settings_confirm_title(MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_NO_CHANNEL,
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
                               MESH_UI_SETTINGS_ACTIONS, MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        item.kind != MESH_UI_SETTING_INFO || strcmp(item.value, "not supported") != 0) {
        failure = "Shutdown should be a fact on a board that cannot shut down";
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

/*
 * The Position section's fixed-position rows. They are radio actions, so they do not wait for
 * Y - but unlike the reboot and the resets they are not destructive, so they do not go through
 * the confirm overlay either, and they carry the section's pending edits because "Set fixed
 * position" is a row that reads the three rows above it.
 */
MESH_TEST_CASE(ui_nav_fixed_position, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
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
            if (item.kind != MESH_UI_SETTING_TOGGLE || item.field != MESH_UI_FIELD_NONE) {
                failure = "Fixed position should be shown, not offered";
                goto cleanup;
            }
        }
        if (item.kind == MESH_UI_SETTING_ACTION &&
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
            item.kind == MESH_UI_SETTING_ACTION) {
            failure = "Clear fixed position should not be offered when it is off";
            goto cleanup;
        }
    }

    struct mesh_ui_action action;
    for (int i = 0; i < 4; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    for (int i = 0; i < MESH_UI_SETTINGS_POSITION; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_POSITION) {
        failure = "the Position section should open";
        goto cleanup;
    }
    /* Type a latitude, then press the action row: the edit rides along with it. */
    for (uint32_t i = 0; i < latitude_row; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open ||
        store.nav.keyboard_field != (uint8_t)MESH_UI_FIELD_POSITION_LATITUDE) {
        failure = "A on the latitude row should open the keyboard on it";
        goto cleanup;
    }
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "45.0");
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_POSITION_LATITUDE) {
        failure = "the typed latitude should be recorded as a pending edit";
        goto cleanup;
    }
    for (uint32_t i = latitude_row; i < set_row; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    while (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] > 0U) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
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
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}
