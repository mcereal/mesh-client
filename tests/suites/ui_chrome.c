#define _POSIX_C_SOURCE 200809L

/*
 * What the frame says about the client rather than about a screen.
 *
 * Both answers in src/ui/chrome.c are read by the renderer and by nothing else, which is
 * exactly why they are worth a suite of their own: a rule that only a screenshot can check is
 * a rule that gets quietly broken. The cases below are the three the table is written to keep -
 * a banner says only what nothing else on the frame says, a banner must resolve, and a modal
 * owns the body - plus the split that keeps either indicator from being noise: what is moving
 * is the bar's, what has settled is the banner's, and nothing is both.
 */

#include "framework/mesh_test.h"

#include "mesh/core/firmware_update.h"
#include "mesh/core/updater.h"
#include "mesh/ui/chrome.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdio.h>
#include <string.h>

/* A client with a radio attached and its configuration read: nothing in flight, nothing to
   announce. Every case below is this plus one thing. */
static void chrome_fixture(struct mesh_ui_snapshot *snapshot) {
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->device_count = 1U;
    snapshot->devices[0].connected = true;
    snprintf(snapshot->devices[0].identifier, sizeof snapshot->devices[0].identifier, "%s",
             "F4:12:FA:00:0A:11");
    snapshot->handshake_valid = true;
    snapshot->handshake.config_complete = true;
    snapshot->nav.screen = MESH_UI_SCREEN_NODES;
    snapshot->nav.settings_section = (uint8_t)MESH_UI_SETTINGS_NO_SECTION;
    snapshot->settings.client.update_state = (uint8_t)MESH_UPDATE_IDLE;
}

MESH_TEST_CASE(ui_chrome_bar_follows_what_is_in_flight, unit) {
    struct mesh_ui_snapshot snapshot;

    chrome_fixture(&snapshot);
    MESH_TEST_FAIL_IF(mesh_ui_chrome_busy(&snapshot),
                      "a settled client is not waiting on anything");

    /* The four requests already sent. Each on its own, because a bar that only appeared when
       two of them coincided would be a bar nobody ever saw. */
    chrome_fixture(&snapshot);
    snapshot.handshake.request_in_flight = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot), "a handshake in flight is work outstanding");

    chrome_fixture(&snapshot);
    snapshot.handshake.config_complete = false;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot),
                      "an unfinished handshake is work outstanding");

    chrome_fixture(&snapshot);
    snapshot.settings.admin_busy = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot), "an admin read is work outstanding");

    chrome_fixture(&snapshot);
    snapshot.settings.write_pending = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot), "a queued write is work outstanding");

    chrome_fixture(&snapshot);
    snapshot.settings.client.update_busy = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot), "an update check is work outstanding");

    record_success(test_name);
}

/*
 * The gate that stops the bar running for the whole of a session.
 *
 * The node roster deliberately outlives the connection, so a client sitting on a cached roster
 * with no radio has an incomplete handshake for as long as it runs. Reading `config_complete`
 * on its own would put a bar on every frame of that session, which is a bar that has stopped
 * saying anything.
 */
MESH_TEST_CASE(ui_chrome_bar_needs_a_radio_to_be_waiting_on, unit) {
    struct mesh_ui_snapshot snapshot;
    chrome_fixture(&snapshot);
    snapshot.devices[0].connected = false;
    snapshot.handshake.config_complete = false;
    MESH_TEST_FAIL_IF(mesh_ui_chrome_busy(&snapshot),
                      "a cached roster and no radio is not a handshake in progress");

    /* The same client with the radio back: now it genuinely is syncing. */
    snapshot.devices[0].connected = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot), "a reconnect is a handshake in progress");
    record_success(test_name);
}

MESH_TEST_CASE(ui_chrome_banner_reports_the_settled_update_states, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_banner banner;

    chrome_fixture(&snapshot);
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(&snapshot, &banner),
                      "an idle client announces nothing");
    MESH_TEST_FAIL_IF(banner.kind != (uint8_t)MESH_UI_BANNER_NONE || banner.detail == NULL,
                      "a refused banner has to come back cleared, with a detail that is safe to "
                      "draw");

    chrome_fixture(&snapshot);
    snapshot.settings.client.update_state = (uint8_t)MESH_UPDATE_AVAILABLE;
    snapshot.settings.client.update_can_install = true;
    snprintf(snapshot.settings.client.update_latest, sizeof snapshot.settings.client.update_latest,
             "%s", "9.9.9");
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "a newer release is worth saying");
    MESH_TEST_FAIL_IF(banner.kind != (uint8_t)MESH_UI_BANNER_UPDATE_AVAILABLE,
                      "the wrong banner for an available release");
    MESH_TEST_FAIL_IF(strcmp(banner.detail, "9.9.9") != 0,
                      "the version is the banner's detail, and is not translated");

    chrome_fixture(&snapshot);
    snapshot.settings.client.update_state = (uint8_t)MESH_UPDATE_READY;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "an installed release waiting for a restart is worth saying");
    MESH_TEST_FAIL_IF(banner.kind != (uint8_t)MESH_UI_BANNER_UPDATE_READY,
                      "the wrong banner for an installed release");
    /* READY does not need update_can_install: it is already installed, so the permission
       question has been answered by the fact that it happened. */

    /* Every state the updater passes through, and only the two settled ones raise anything.
       The three in flight are the bar's - a state that is one is never the other. */
    for (int state = 0; state < (int)MESH_UPDATE_STATE_COUNT; ++state) {
        chrome_fixture(&snapshot);
        snapshot.settings.client.update_state = (uint8_t)state;
        snapshot.settings.client.update_can_install = true;
        const bool raised = mesh_ui_chrome_banner(&snapshot, &banner);
        const bool expected =
            state == (int)MESH_UPDATE_AVAILABLE || state == (int)MESH_UPDATE_READY;
        MESH_TEST_FAIL_IF(raised != expected, "an updater state raises the wrong kind of notice");
    }

    record_success(test_name);
}

/*
 * A banner must resolve, and this is the case that rule exists for.
 *
 * A build that is not an official release compares itself against GitHub and reports what it
 * finds, but is not allowed to install it. A banner raised there is a container nothing the
 * user can do would ever clear, on every screen, for the rest of the run - so the same fact
 * that stops the About screen offering an install row stops this being raised at all.
 */
MESH_TEST_CASE(ui_chrome_banner_only_names_what_can_be_resolved, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_banner banner;

    chrome_fixture(&snapshot);
    snapshot.settings.client.update_state = (uint8_t)MESH_UPDATE_AVAILABLE;
    snapshot.settings.client.update_can_install = false;
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(&snapshot, &banner),
                      "a release this build cannot install is a banner nothing clears");

    snapshot.settings.client.update_can_install = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "a release this build can install is a banner a press clears");
    record_success(test_name);
}

/*
 * A banner says only what nothing else on the frame says.
 *
 * Settings > About states the updater in full - the state, the channel, and the row that
 * installs it - so a banner over that section would be the client telling you something while
 * you are already reading it. Everywhere else, including the Settings tab's own root, it
 * stands.
 */
MESH_TEST_CASE(ui_chrome_banner_stands_down_where_the_screen_already_says_it, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_banner banner;

    chrome_fixture(&snapshot);
    snapshot.settings.client.update_state = (uint8_t)MESH_UPDATE_READY;

    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_ABOUT;
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(&snapshot, &banner),
                      "the About section already says this in more detail");

    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_NO_SECTION;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "the Settings root says nothing about the updater");

    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_LORA;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "another section says nothing about the updater either");

    /* And the About *section* is what stands it down, not the About row's screen number: the
       tab alone must not, or every Settings screen would lose it. */
    snapshot.nav.screen = MESH_UI_SCREEN_STATUS;
    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_ABOUT;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "a stale section on another tab is not the About screen");
    record_success(test_name);
}

/*
 * A modal owns the body.
 *
 * Each of the six takes the body for a question, and the banner shortens the body - so one
 * raised over a dialog would move the question while it was being answered. Walked one at a
 * time rather than in the combination the nav happens to produce, because the rule is about the
 * component and a seventh overlay has to be checked against it.
 *
 * The tapback picker was the fifth, and it is why the walk is written this way: it was added as
 * a body-owning overlay without being added to mesh_ui_chrome_modal_open(), so with an update
 * ready it drew under a banner that every one of its siblings suppresses. The help screen is the
 * sixth and arrived with exactly the same omission, which is the case for keeping this loop
 * rather than trusting the predicate to be remembered.
 */
MESH_TEST_CASE(ui_chrome_banner_yields_to_a_modal, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_banner banner;

    for (int overlay = 0; overlay < 6; ++overlay) {
        chrome_fixture(&snapshot);
        snapshot.settings.client.update_state = (uint8_t)MESH_UPDATE_READY;
        switch (overlay) {
        case 0:
            snapshot.nav.confirm_open = true;
            break;
        case 1:
            snapshot.nav.picker_open = true;
            break;
        case 2:
            snapshot.nav.keyboard_open = true;
            break;
        case 3:
            snapshot.nav.compose_open = true;
            break;
        case 4:
            snapshot.nav.reaction_open = true;
            break;
        default:
            snapshot.nav.help_open = true;
            break;
        }
        MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(&snapshot, &banner),
                          "a banner shortened the body an overlay had taken");
        /* The bar is the other half of the rule and deliberately keeps running: it costs no
           row, so there is nothing for it to move. */
        snapshot.settings.admin_busy = true;
        MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot),
                          "work in flight is still in flight under an overlay");
    }
    record_success(test_name);
}

/* Neither answer may be asked with nothing to answer about. Both are called once a frame from
   fb_render_snapshot(), which is the one place a NULL would be a black screen rather than a
   missing row. */
MESH_TEST_CASE(ui_chrome_answers_without_a_snapshot, unit) {
    struct mesh_ui_banner banner;
    MESH_TEST_FAIL_IF(mesh_ui_chrome_busy(NULL), "nothing is in flight when there is nothing");
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(NULL, &banner), "nothing is announced about nothing");
    MESH_TEST_FAIL_IF(banner.detail == NULL, "a cleared banner still has to be safe to draw");
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(NULL, NULL), "and no output is not a crash");
    record_success(test_name);
}

/*
 * The one banner that is about a different computer.
 *
 * A radio in the ESP32 update loader is off the mesh and cannot get itself back: the loader has
 * no timer, no reboot counter and no fallback to the old firmware, so it sits there advertising
 * until an image finishes arriving. That is why it outranks the updater's two - both of those
 * are about a release that will still be there in an hour - and why it is a warning rather than
 * an error: nothing is broken, and what is true is that the radio is not on the mesh.
 *
 * It stands down inside Settings > About radio for the rule the updater's pair follow one
 * section over: that screen is where the press that resolves it lives, so a banner over it would
 * be the client telling you something while you are already reading it.
 */
MESH_TEST_CASE(ui_chrome_banner_says_the_radio_is_in_its_loader, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_banner banner;

    chrome_fixture(&snapshot);
    snapshot.settings.fw_radio_in_loader = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "a radio stuck in its loader is worth saying on every screen");
    MESH_TEST_FAIL_IF(banner.kind != (uint8_t)MESH_UI_BANNER_RADIO_IN_LOADER,
                      "the wrong banner for a radio in its loader");
    MESH_TEST_FAIL_IF(banner.family != MESH_UI_FAMILY_WARNING,
                      "nothing is broken, so it is a warning rather than an error");
    MESH_TEST_FAIL_IF(banner.supporting == MESH_STR_NONE,
                      "and it has to say what resolves it, or it is a banner that cannot");

    /* Ahead of the updater's, which is the whole point of there being an order. */
    snapshot.settings.client.update_state = (uint8_t)MESH_UPDATE_READY;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner) ||
                          banner.kind != (uint8_t)MESH_UI_BANNER_RADIO_IN_LOADER,
                      "a radio off the mesh outranks a release waiting for a restart");

    /* And down inside the section that offers the press. */
    chrome_fixture(&snapshot);
    snapshot.settings.fw_radio_in_loader = true;
    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_RADIO;
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(&snapshot, &banner),
                      "About radio already says this, and offers the row that fixes it");
    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_LORA;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "no other section says anything about it");
    record_success(test_name);
}

MESH_TEST_CASE(ui_chrome_banner_reports_a_crash_from_the_previous_run, unit) {
    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_banner banner;

    chrome_fixture(&snapshot);
    snapshot.settings.client.crash_report_waiting = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner),
                      "a client that stopped on its own is worth saying on every screen");
    MESH_TEST_FAIL_IF(banner.kind != (uint8_t)MESH_UI_BANNER_CRASH_REPORT,
                      "the wrong banner for a waiting crash report");
    MESH_TEST_FAIL_IF(banner.family != MESH_UI_FAMILY_ERROR,
                      "something was plainly broken, which is what separates this from the "
                      "loader's warning");
    MESH_TEST_FAIL_IF(banner.supporting == MESH_STR_NONE,
                      "it has to name where the report is, or it is a banner that cannot resolve");

    /*
     * The order, from both sides. A fault outranks the updater's news; a radio sitting off the
     * mesh right now outranks a fault that has already finished happening.
     */
    snapshot.settings.client.update_state = (uint8_t)MESH_UPDATE_READY;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner) ||
                          banner.kind != (uint8_t)MESH_UI_BANNER_CRASH_REPORT,
                      "a crash outranks a release waiting for a restart");
    snapshot.settings.fw_radio_in_loader = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_banner(&snapshot, &banner) ||
                          banner.kind != (uint8_t)MESH_UI_BANNER_RADIO_IN_LOADER,
                      "a radio off the mesh now outranks a fault that is over");

    /* Down inside About, which is the section holding the path and the press that discards it. */
    chrome_fixture(&snapshot);
    snapshot.settings.client.crash_report_waiting = true;
    snapshot.nav.screen = MESH_UI_SCREEN_SETTINGS;
    snapshot.nav.settings_section = (uint8_t)MESH_UI_SETTINGS_ABOUT;
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(&snapshot, &banner),
                      "About already says this, and offers the row that clears it");

    /*
     * And it goes away when the report does. This is the resolvability rule rather than a
     * restatement of the flag: the banner table refuses anything a user cannot make untrue, and
     * what makes this one resolvable is that one press in About clears exactly this field.
     */
    chrome_fixture(&snapshot);
    snapshot.settings.client.crash_report_waiting = false;
    MESH_TEST_FAIL_IF(mesh_ui_chrome_banner(&snapshot, &banner),
                      "a discarded report left the banner standing");
    record_success(test_name);
}

/*
 * An install in flight is the longest thing this client ever does, and until the bar learned
 * about it there was no sign of it anywhere but the one section it runs from.
 *
 * The settled states are deliberately not the bar's: DONE and FAILED are facts, and a bar that
 * went on turning after a job ended would be saying work is happening that is not.
 */
MESH_TEST_CASE(ui_chrome_bar_follows_a_firmware_install, unit) {
    struct mesh_ui_snapshot snapshot;

    for (int state = 0; state < (int)MESH_FIRMWARE_UPDATE_STATE_COUNT; ++state) {
        chrome_fixture(&snapshot);
        snapshot.settings.fw_update_state = (uint8_t)state;
        const bool expected =
            mesh_firmware_update_state_busy((enum mesh_firmware_update_state)state);
        MESH_TEST_FAIL_IF(mesh_ui_chrome_busy(&snapshot) != expected,
                          "the bar should turn for exactly the states that are work in flight");
    }
    /* The check next door is the same question about the other press. */
    chrome_fixture(&snapshot);
    snapshot.settings.fw_busy = true;
    MESH_TEST_FAIL_IF(!mesh_ui_chrome_busy(&snapshot),
                      "a firmware check is a request already sent");
    record_success(test_name);
}
