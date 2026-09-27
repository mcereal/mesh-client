/*
 * The firmware install's own screen: what it says, and when it is on the panel.
 *
 * Two halves, and each is a decision rather than an observation. The view folds the install's
 * eight states onto four stages, and the thing worth pinning there is the fold - which stage a
 * failure is drawn at, and that only a state with a real fraction fills the dial. The nav half
 * is the screen's life: it takes the panel when a job starts, B puts it away without touching
 * the job, a row brings it back, and it does not come back on its own just because the job
 * reported progress.
 */

#include "framework/mesh_test.h"

#include "mesh/core/firmware_update.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/firmware_progress.h"
#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "support/ui_fixture.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct mesh_ui_firmware_progress progress_at(enum mesh_firmware_update_state state,
                                                    uint8_t percent) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.fw_update_state = (uint8_t)state;
    settings.fw_update_progress = percent;
    struct mesh_ui_firmware_progress out;
    mesh_ui_firmware_progress_of(&settings, &out);
    return out;
}

MESH_TEST_CASE(ui_firmware_progress_folds_the_ladder_onto_four_stages, unit) {
    /* In ladder order, so a state added to the enum without a stage is a stage of 0 here - the
       download - on a rung that is past it, which the ordering check below catches. */
    uint8_t last = 0U;
    for (int state = (int)MESH_FIRMWARE_UPDATE_RESOLVING; state <= (int)MESH_FIRMWARE_UPDATE_DONE;
         ++state) {
        const struct mesh_ui_firmware_progress p =
            progress_at((enum mesh_firmware_update_state)state, 0U);
        MESH_TEST_FAIL_IF(p.stage < last, "the stages should only move forward up the ladder");
        MESH_TEST_FAIL_IF(p.status == 0U, "every rung should have a sentence");
        last = p.stage;
    }
    MESH_TEST_FAIL_IF(progress_at(MESH_FIRMWARE_UPDATE_DOWNLOADING, 10U).stage !=
                          MESH_UI_FIRMWARE_STAGE_DOWNLOAD,
                      "a download is the first stage");
    MESH_TEST_FAIL_IF(progress_at(MESH_FIRMWARE_UPDATE_WAITING, 0U).stage !=
                          MESH_UI_FIRMWARE_STAGE_PREPARE,
                      "waiting for a loader is getting the radio ready");
    MESH_TEST_FAIL_IF(progress_at(MESH_FIRMWARE_UPDATE_WRITING, 10U).stage !=
                          MESH_UI_FIRMWARE_STAGE_INSTALL,
                      "the write is the install");
    const struct mesh_ui_firmware_progress done = progress_at(MESH_FIRMWARE_UPDATE_DONE, 0U);
    MESH_TEST_FAIL_IF(!done.done || done.stage != MESH_UI_FIRMWARE_STAGE_COUNT,
                      "a finished job has every stage behind it");
    record_success(test_name);
}

/* Only the two rungs with a fraction fill the ring, and only once there is one: forty seconds of
   a bootloader enumerating drawn as an empty ring reads as a job that stalled. */
MESH_TEST_CASE(ui_firmware_progress_fills_the_dial_only_with_a_fraction, unit) {
    const struct mesh_ui_firmware_progress writing =
        progress_at(MESH_FIRMWARE_UPDATE_WRITING, 43U);
    MESH_TEST_FAIL_IF(!writing.determinate || writing.permille != 430U,
                      "a write with a fraction should fill the dial to it");
    MESH_TEST_FAIL_IF(progress_at(MESH_FIRMWARE_UPDATE_WRITING, 0U).determinate,
                      "a write with no fraction yet should spin, not sit at zero");
    MESH_TEST_FAIL_IF(progress_at(MESH_FIRMWARE_UPDATE_WAITING, 50U).determinate,
                      "a rung with no fraction should spin whatever the byte says");
    record_success(test_name);
}

/* FAILED replaces the state the job failed in, so the stage it is drawn at comes from why. */
MESH_TEST_CASE(ui_firmware_progress_draws_a_failure_where_it_happened, unit) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.fw_update_state = (uint8_t)MESH_FIRMWARE_UPDATE_FAILED;
    struct mesh_ui_firmware_progress p;

    settings.fw_update_error = (uint8_t)MESH_FIRMWARE_UPDATE_ERROR_DOWNLOAD;
    mesh_ui_firmware_progress_of(&settings, &p);
    MESH_TEST_FAIL_IF(!p.failed || p.stage != MESH_UI_FIRMWARE_STAGE_DOWNLOAD,
                      "bytes that never arrived fail the download");
    MESH_TEST_FAIL_IF(p.hint != MESH_STR_FW_SCREEN_HINT_UNTOUCHED,
                      "a failed download should say the radio was not touched");

    settings.fw_update_error = (uint8_t)MESH_FIRMWARE_UPDATE_ERROR_REFUSED;
    mesh_ui_firmware_progress_of(&settings, &p);
    MESH_TEST_FAIL_IF(p.stage != MESH_UI_FIRMWARE_STAGE_PREPARE,
                      "a radio that refused fails getting it ready");

    settings.fw_update_error = (uint8_t)MESH_FIRMWARE_UPDATE_ERROR_HANDOVER;
    mesh_ui_firmware_progress_of(&settings, &p);
    MESH_TEST_FAIL_IF(p.stage != MESH_UI_FIRMWARE_STAGE_INSTALL,
                      "a broken handover fails the install");
    MESH_TEST_FAIL_IF(p.hint == MESH_STR_FW_SCREEN_HINT_UNTOUCHED,
                      "a broken handover must not promise the radio is untouched");

    settings.fw_radio_in_loader = true;
    mesh_ui_firmware_progress_of(&settings, &p);
    MESH_TEST_FAIL_IF(p.hint != MESH_STR_FW_SCREEN_HINT_LOADER,
                      "a radio left in its loader should be told how to get it out");
    record_success(test_name);
}

static bool firmware_showing(const struct mesh_ui_store *store) {
    struct mesh_ui_route route;
    mesh_ui_route_of(&store->nav, &route);
    return route.level == (uint8_t)MESH_UI_ROUTE_FIRMWARE;
}

static void set_install(struct mesh_ui_store *store, struct mesh_ui_settings *settings,
                        enum mesh_firmware_update_state state, uint8_t percent) {
    settings->fw_update_state = (uint8_t)state;
    settings->fw_update_progress = percent;
    mesh_ui_store_set_settings(store, settings);
}

MESH_TEST_CASE(ui_firmware_screen_opens_hides_and_comes_back, unit) {
    const char *failure = NULL;
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_metadata = true;
    settings.fw_supported = true;
    snprintf(settings.fw_latest, sizeof settings.fw_latest, "2.7.26");
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    if (!mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_RADIO_DETAILS)) {
        failure = "the details page should open";
        goto cleanup;
    }

    /* The job starting is what raises it: the reader has just answered the sheet. */
    set_install(&store, &settings, MESH_FIRMWARE_UPDATE_RESOLVING, 0U);
    if (!firmware_showing(&store)) {
        failure = "an install starting should take the screen";
        goto cleanup;
    }
    /* The whole panel, so nothing underneath it can be pressed or walked away to. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    if (!firmware_showing(&store) || action.type != MESH_UI_ACTION_NONE) {
        failure = "the install's screen should take every key but B";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (firmware_showing(&store) || action.type != MESH_UI_ACTION_NONE) {
        failure = "B should hide the screen and ask the app for nothing";
        goto cleanup;
    }

    /* Progress is not a start: a job that reports a fraction does not put back what B put away. */
    set_install(&store, &settings, MESH_FIRMWARE_UPDATE_WRITING, 43U);
    if (firmware_showing(&store)) {
        failure = "progress on a running job should not reopen a hidden screen";
        goto cleanup;
    }

    /* The row under the section's meter brings it back. */
    {
        struct mesh_ui_settings_item items[32];
        const uint32_t count = mesh_ui_settings_items(
            &store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_RADIO_DETAILS,
            MESH_UI_SETTINGS_NO_CHANNEL, items, (uint32_t)(sizeof items / sizeof items[0]));
        uint32_t row = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            if (items[i].kind == INKSTAND_FORM_ACTION &&
                items[i].number == (uint32_t)MESH_UI_SETTINGS_ACTION_SHOW_FIRMWARE) {
                row = i;
            }
        }
        if (row == UINT32_MAX || !mesh_test_settings_cursor_to(&store, row)) {
            failure = "a running install should offer the row that shows it";
            goto cleanup;
        }
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!firmware_showing(&store) || action.type != MESH_UI_ACTION_NONE) {
        failure = "the show row should reopen the screen and nothing else";
        goto cleanup;
    }

    /* A finished job stays up: the answer is what the screen is for. */
    set_install(&store, &settings, MESH_FIRMWARE_UPDATE_DONE, 0U);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (!firmware_showing(&store)) {
        failure = "a finished install should stay on the panel until it is read";
        goto cleanup;
    }
    /* And a record gone back to nothing takes it down. */
    set_install(&store, &settings, MESH_FIRMWARE_UPDATE_IDLE, 0U);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (firmware_showing(&store) || store.nav.firmware_open) {
        failure = "an install with no record left should have no screen";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A job that starts while the reader is somewhere else leaves them there. */
MESH_TEST_CASE(ui_firmware_screen_does_not_follow_the_reader_to_another_tab, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF(!mesh_test_open_tab(&store, MESH_UI_SCREEN_MESSAGES),
                      "the Messages tab should open");

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    mesh_ui_store_set_settings(&store, &settings);
    set_install(&store, &settings, MESH_FIRMWARE_UPDATE_DOWNLOADING, 5U);
    const bool opened = store.nav.firmware_open;
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(opened, "an install should not pull the reader off another tab");
    record_success(test_name);
}
