#define _POSIX_C_SOURCE 200809L

/*
 * The radio-firmware check, end to end: two documents, one press, and the refusal that comes
 * out of the pair.
 *
 * Driven by a stand-in curl on PATH that serves the captured fixtures in tests/data/, the same
 * trick the updater suite uses - which means the whole check runs through the real fetcher,
 * the real fork, the real event loop and the real parsers, with only the network replaced.
 */

#include "framework/mesh_test.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/firmware.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MESH_TEST_DATA_DIR
#define MESH_TEST_DATA_DIR "tests/data"
#endif

/*
 * The two constants store.h restates on the UI side of the seam.
 *
 * They are declared twice on purpose - store.h names no core module anywhere, which is what
 * lets a backend compile against a snapshot rather than against the client - and two
 * declarations of one limit stay honest only because something checks. This is that something,
 * exactly as waypoint_limits_agree_across_the_seam is for the waypoint book's three.
 */
MESH_TEST_CASE(firmware_limits_agree_across_the_seam, unit) {
    MESH_TEST_FAIL_IF(MESH_UI_FW_VERSION_MAX != MESH_FIRMWARE_VERSION_MAX,
                      "the UI's version buffer should be the catalog's");
    MESH_TEST_FAIL_IF(MESH_UI_FW_BOARD_MAX != MESH_FIRMWARE_BOARD_NAME_MAX,
                      "the UI's board-name buffer should be the catalog's");
    record_success(test_name);
}

/* A radio the client has never asked about has nothing to say and refuses to guess. */
MESH_TEST_CASE(firmware_starts_idle_and_needs_a_fetcher, unit) {
    struct mesh_firmware firmware;
    MESH_TEST_FAIL_IF(mesh_firmware_init(&firmware, NULL) != 0, "init should succeed");

    MESH_TEST_FAIL_IF(firmware.state != MESH_FIRMWARE_IDLE, "a fresh check has not run");
    MESH_TEST_FAIL_IF(firmware.blocker != MESH_FIRMWARE_BLOCKER_NO_RADIO,
                      "with nothing connected the answer is that there is no radio");
    MESH_TEST_FAIL_IF(mesh_firmware_board(&firmware) != NULL, "and no board was identified");
    /* No loop, so no fetcher: the press is refused rather than swallowed. */
    MESH_TEST_FAIL_IF(mesh_firmware_available(&firmware), "with no loop there is nothing to run");
    MESH_TEST_FAIL_IF(mesh_firmware_check(&firmware, 69U, "2.7.0.abc", 0U) != -ENOTSUP,
                      "a check with no fetcher should refuse");
    mesh_firmware_shutdown(&firmware);
    record_success(test_name);
}

/*
 * A stand-in curl that serves the captured documents by URL, and a scratch directory holding
 * it. Every case below shares the shape, so it is one helper rather than four copies.
 */
struct firmware_harness {
    char dir[64];
    char *saved_path;
    struct mesh_event_loop loop;
    struct mesh_firmware firmware;
    bool loop_up;
    bool firmware_up;
};

static bool firmware_harness_up(struct firmware_harness *harness) {
    memset(harness, 0, sizeof *harness);
    snprintf(harness->dir, sizeof harness->dir, "%s", "/tmp/meshclient_fw_XXXXXX");
    if (mkdtemp(harness->dir) == NULL) {
        return false;
    }
    char curl_path[128];
    snprintf(curl_path, sizeof curl_path, "%s/curl", harness->dir);
    FILE *script = fopen(curl_path, "w");
    if (script == NULL) {
        return false;
    }
    /* The URLs below are what the module asks for once the environment has redirected it; the
       script answers with the matching fixture, and with nothing at all for anything else. */
    fprintf(script,
            "#!/bin/sh\n"
            "for a in \"$@\"; do url=\"$a\"; done\n"
            "case \"$url\" in\n"
            "  *hardware) exec cat '%s/device_hardware.json' ;;\n"
            "  *list) exec cat '%s/firmware_list.json' ;;\n"
            "  *broken) printf 'not a document' ; exit 0 ;;\n"
            "esac\n"
            "exit 7\n",
            MESH_TEST_DATA_DIR, MESH_TEST_DATA_DIR);
    fclose(script);
    if (chmod(curl_path, 0755) != 0) {
        return false;
    }

    const char *const old_path = getenv("PATH");
    harness->saved_path = old_path != NULL ? strdup(old_path) : strdup("");
    char new_path[1024];
    snprintf(new_path, sizeof new_path, "%s:%s", harness->dir,
             old_path != NULL ? old_path : "/usr/bin");
    setenv("PATH", new_path, 1);
    setenv("MESHCLIENT_FIRMWARE_HARDWARE_URL", "https://example.invalid/hardware", 1);
    setenv("MESHCLIENT_FIRMWARE_LIST_URL", "https://example.invalid/list", 1);

    if (mesh_event_loop_init(&harness->loop) != 0) {
        return false;
    }
    harness->loop_up = true;
    if (mesh_firmware_init(&harness->firmware, &harness->loop) != 0) {
        return false;
    }
    harness->firmware_up = true;
    return mesh_firmware_available(&harness->firmware);
}

static void firmware_harness_down(struct firmware_harness *harness) {
    if (harness->firmware_up) {
        mesh_firmware_shutdown(&harness->firmware);
    }
    if (harness->loop_up) {
        mesh_event_loop_shutdown(&harness->loop);
    }
    unsetenv("MESHCLIENT_FIRMWARE_HARDWARE_URL");
    unsetenv("MESHCLIENT_FIRMWARE_LIST_URL");
    if (harness->saved_path != NULL) {
        setenv("PATH", harness->saved_path, 1);
        free(harness->saved_path);
    }
    if (harness->dir[0] != '\0') {
        char curl_path[128];
        snprintf(curl_path, sizeof curl_path, "%s/curl", harness->dir);
        unlink(curl_path);
        rmdir(harness->dir);
    }
}

/* Pumps the loop until the check stops running. Both documents are child processes, so the
   test has to turn the loop for either of them to land. */
static bool firmware_settle(struct firmware_harness *harness) {
    for (int turn = 0; turn < 600; ++turn) {
        if (harness->firmware.state != MESH_FIRMWARE_IDENTIFYING &&
            harness->firmware.state != MESH_FIRMWARE_CHECKING) {
            return true;
        }
        (void)mesh_event_loop_run(&harness->loop, 10);
        mesh_firmware_tick(&harness->firmware, 0U);
    }
    return false;
}

/*
 * The whole press for a board with a path, on the bus that path uses.
 *
 * hw_model 69 is the Heltec Mesh Node T114 - an nRF52840, so the USB path - and the fixture's
 * newest stable is 2.7.26.54e0d8d. A radio on 2.7.20 is therefore behind, identified, and
 * blocked by nothing but the phase that would install it not being written.
 */
MESH_TEST_CASE(firmware_check_identifies_and_compares, unit) {
    struct firmware_harness harness;
    const char *failure = NULL;
    if (!firmware_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_USB);

    if (mesh_firmware_check(&harness.firmware, 69U, "2.7.20.6658ec2", 0U) != 0) {
        failure = "the check should start";
        goto cleanup;
    }
    if (harness.firmware.state != MESH_FIRMWARE_IDENTIFYING) {
        failure = "it should read the hardware list first";
        goto cleanup;
    }
    if (mesh_firmware_check(&harness.firmware, 69U, "2.7.20.6658ec2", 0U) != -EBUSY) {
        failure = "a second press while one runs should be refused";
        goto cleanup;
    }
    if (!firmware_settle(&harness)) {
        failure = "the check should finish";
        goto cleanup;
    }

    if (harness.firmware.state != MESH_FIRMWARE_AVAILABLE) {
        failure = "2.7.20 is behind the fixture's newest stable";
        goto cleanup;
    }
    if (strcmp(harness.firmware.release.version, "2.7.26.54e0d8d") != 0) {
        failure = "and the newest stable is what it should have found";
        goto cleanup;
    }
    const struct mesh_firmware_board *const board = mesh_firmware_board(&harness.firmware);
    if (board == NULL || strcmp(board->target, "heltec-mesh-node-t114") != 0) {
        failure = "hw_model 69 is exactly one board";
        goto cleanup;
    }
    if (harness.firmware.blocker != MESH_FIRMWARE_BLOCKER_NONE) {
        failure = "an nRF52840 on the USB bus is blocked by nothing";
        goto cleanup;
    }
    if (strstr(harness.firmware.message, "2.7.26.54e0d8d") == NULL) {
        failure = "the row's line should name the release it found";
        goto cleanup;
    }

    /*
     * Unplug it and put it on Bluetooth. The check does not run again and the answer changes
     * anyway, because the refusal is derived from the board and the bus rather than recorded
     * when the documents landed.
     */
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_BLE);
    if (harness.firmware.blocker != MESH_FIRMWARE_BLOCKER_WRONG_BUS) {
        failure = "the same board over BLE is the wrong-bus refusal";
        goto cleanup;
    }
    if (harness.firmware.state != MESH_FIRMWARE_AVAILABLE) {
        failure = "and the check's own answer should not have moved";
        goto cleanup;
    }
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_NONE);
    if (harness.firmware.blocker != MESH_FIRMWARE_BLOCKER_NO_RADIO) {
        failure = "with the radio gone the refusal is that there is no radio";
        goto cleanup;
    }

cleanup:
    firmware_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The three refusals a real board can hit, each on a model the served hardware document
 * actually carries: 48 is four boards, 53 is an ESP32-C3 whose loader partition current
 * firmware will not boot into, and 2 is a LILYGO T-LoRa V1 upstream has stopped building.
 *
 * All three still report the newest release, which is the point: the version question and the
 * "could we install it" question are different questions, and a client that answered only the
 * second would leave somebody unable to find out their radio is three releases behind.
 */
MESH_TEST_CASE(firmware_check_names_its_refusals, unit) {
    static const struct {
        uint32_t hw_model;
        enum mesh_firmware_path bus;
        enum mesh_firmware_blocker blocker;
        const char *what;
    } k_cases[] = {
        {48U, MESH_FIRMWARE_PATH_BLE, MESH_FIRMWARE_BLOCKER_AMBIGUOUS,
         "four boards share hw_model 48 and none of them may be guessed"},
        {53U, MESH_FIRMWARE_PATH_BLE, MESH_FIRMWARE_BLOCKER_NO_PATH,
         "an ESP32-C3 has no path from this client"},
        {2U, MESH_FIRMWARE_PATH_BLE, MESH_FIRMWARE_BLOCKER_UNSUPPORTED_BOARD,
         "a board upstream no longer builds has no image whatever the bus"},
        {60000U, MESH_FIRMWARE_PATH_USB, MESH_FIRMWARE_BLOCKER_UNKNOWN_BOARD,
         "a model nothing claims cannot be identified"},
    };

    struct firmware_harness harness;
    const char *failure = NULL;
    if (!firmware_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        mesh_firmware_set_bus(&harness.firmware, k_cases[i].bus);
        if (mesh_firmware_check(&harness.firmware, k_cases[i].hw_model, "2.7.20.6658ec2", 0U) !=
            0) {
            failure = "the check should start";
            goto cleanup;
        }
        if (!firmware_settle(&harness)) {
            failure = "the check should finish";
            goto cleanup;
        }
        if (harness.firmware.blocker != k_cases[i].blocker) {
            failure = k_cases[i].what;
            goto cleanup;
        }
        if (strcmp(harness.firmware.release.version, "2.7.26.54e0d8d") != 0) {
            failure = "a refusal is not a reason to stop reporting the newest release";
            goto cleanup;
        }
    }

cleanup:
    firmware_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A radio that has never said what it is, and a document that will not read.
 *
 * The first is the ordinary case on a link that has just come up, and it deliberately skips the
 * hardware list: 39 KB whose every answer is keyed on a model number we do not have. The second
 * is what a proxy, a captive portal or a bad day produces, and it has to be a row rather than a
 * silent idle.
 */
MESH_TEST_CASE(firmware_check_survives_a_silent_radio_and_a_bad_reply, unit) {
    struct firmware_harness harness;
    const char *failure = NULL;
    if (!firmware_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_USB);

    if (mesh_firmware_check(&harness.firmware, 0U, "", 0U) != 0) {
        failure = "a check for a radio that has not said should still start";
        goto cleanup;
    }
    if (harness.firmware.state != MESH_FIRMWARE_CHECKING) {
        failure = "and should go straight to the release index";
        goto cleanup;
    }
    if (!firmware_settle(&harness)) {
        failure = "the check should finish";
        goto cleanup;
    }
    if (harness.firmware.state != MESH_FIRMWARE_AVAILABLE ||
        harness.firmware.blocker != MESH_FIRMWARE_BLOCKER_UNKNOWN_BOARD) {
        failure = "a radio with no version is behind everything, and is not identified";
        goto cleanup;
    }

    /* Now a reply that is not a document. The row says so; it does not stay at "checking". */
    setenv("MESHCLIENT_FIRMWARE_LIST_URL", "https://example.invalid/broken", 1);
    if (mesh_firmware_check(&harness.firmware, 0U, "", 0U) != 0) {
        failure = "the second check should start";
        goto cleanup;
    }
    if (!firmware_settle(&harness)) {
        failure = "the second check should finish";
        goto cleanup;
    }
    if (harness.firmware.state != MESH_FIRMWARE_FAILED || harness.firmware.message[0] == '\0') {
        failure = "an unreadable index is a failure with a line on it";
        goto cleanup;
    }

    /* And a radio swap drops the lot rather than leaving rows about a node that has gone. */
    mesh_firmware_forget(&harness.firmware);
    if (harness.firmware.state != MESH_FIRMWARE_IDLE ||
        harness.firmware.release.version[0] != '\0') {
        failure = "forgetting should leave nothing behind";
        goto cleanup;
    }

cleanup:
    firmware_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
