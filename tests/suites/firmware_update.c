#define _POSIX_C_SOURCE 200809L

/*
 * The press that installs the radio's firmware, as far as a suite can follow it.
 *
 * What this module does is *compose*: the download, the USB handover and the BLE one are three
 * shipped modules with suites of their own, and this is the thing that runs them in order,
 * folds their three state enums onto one ladder and answers the two questions that decide who
 * may touch the bus. So the cases here are about the composition rather than about the parts -
 * the refusals that happen before anything starts, how a sub-module's failure is folded into the
 * one a row shows, and the antenna/radio split that the BLE path's reconnect depends on.
 *
 * The fake CDN is the firmware_download suite's, one directory over: a shell script called
 * `curl` on PATH that serves ranges out of the committed 2.7.26 fixtures the way the real CDN
 * serves them out of a 46 MB zip. What it deliberately cannot serve is a whole `.uf2` - at half
 * a megabyte that is not a fixture - so every case here that gets as far as the image gets a
 * refusal out of it, which is exactly what makes the folding worth pinning: "the bytes did not
 * arrive" and "the bytes are for another board" must not be the same row.
 */

#include "framework/mesh_test.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/firmware_update.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MESH_TEST_DATA_DIR
#define MESH_TEST_DATA_DIR "tests/data"
#endif

/* The 2.7.26 nrf52840 zip, and where the two committed fixtures sit inside it. */
#define ZIP_SIZE "46259773"
#define TAIL_BASE 46194237
#define MEMBER_BASE 2540989

/* ---- the boards a case installs onto --------------------------------------------------------- */

static struct mesh_firmware_board update_t114_board(void) {
    struct mesh_firmware_board board;
    memset(&board, 0, sizeof board);
    board.hw_model = 69U;
    snprintf(board.target, sizeof board.target, "%s", "heltec-mesh-node-t114");
    snprintf(board.name, sizeof board.name, "%s", "Heltec Mesh Node T114");
    snprintf(board.architecture, sizeof board.architecture, "%s", "nrf52840");
    board.actively_supported = true;
    board.requires_dfu = true;
    board.path = MESH_FIRMWARE_PATH_USB;
    return board;
}

static struct mesh_firmware_release update_release(void) {
    struct mesh_firmware_release release;
    memset(&release, 0, sizeof release);
    snprintf(release.version, sizeof release.version, "%s", "2.7.26.54e0d8d");
    snprintf(release.manifest_url, sizeof release.manifest_url, "%s",
             "https://example.invalid/firmware-2.7.26.54e0d8d.json");
    return release;
}

/* ---- the harness ----------------------------------------------------------------------------- */

struct update_probe {
    unsigned calls;
    enum mesh_firmware_update_state state;
    enum mesh_firmware_update_error error;
    unsigned armed_usb;
    unsigned armed_ble;
    unsigned released;
    bool radio_ready;
    int arm_result;
};

static void update_probe_done(void *userdata, const struct mesh_firmware_update *update) {
    struct update_probe *const probe = (struct update_probe *)userdata;
    probe->calls++;
    probe->state = update->state;
    probe->error = update->error;
}

static int update_probe_arm_usb(void *userdata) {
    struct update_probe *const probe = (struct update_probe *)userdata;
    probe->armed_usb++;
    return probe->arm_result;
}

static int update_probe_arm_ble(void *userdata, const uint8_t sha256[32]) {
    struct update_probe *const probe = (struct update_probe *)userdata;
    (void)sha256;
    probe->armed_ble++;
    return probe->arm_result;
}

static bool update_probe_radio_ready(void *userdata) {
    return ((struct update_probe *)userdata)->radio_ready;
}

static void update_probe_release(void *userdata) {
    ((struct update_probe *)userdata)->released++;
}

static struct mesh_firmware_update_hooks update_hooks(struct update_probe *probe) {
    struct mesh_firmware_update_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.arm_usb = update_probe_arm_usb;
    hooks.arm_ble = update_probe_arm_ble;
    hooks.radio_ready = update_probe_radio_ready;
    hooks.release_link = update_probe_release;
    hooks.userdata = probe;
    return hooks;
}

/*
 * The fake CDN, and the one thing it does differently from the download suite's: a request for
 * a member that is not one of the two committed windows exits non-zero rather than serving
 * something plausible. A download that "succeeded" with the wrong bytes is the failure mode
 * this whole feature exists to avoid, so the fake must not be able to fake it.
 */
static bool update_install_curl(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/curl", dir);
    FILE *const file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file,
            "#!/bin/sh\n"
            "DATA='%s'\n"
            "head=0; out=''; range=''\n"
            "while [ $# -gt 0 ]; do\n"
            "  case \"$1\" in\n"
            "    -fsSLI) head=1 ;;\n"
            "    -o) shift; out=\"$1\" ;;\n"
            "    -H) shift; case \"$1\" in 'Range: bytes='*) range=\"${1#Range: bytes=}\" ;; "
            "esac ;;\n"
            "  esac\n"
            "  shift\n"
            "done\n"
            "if [ \"$head\" -eq 1 ]; then\n"
            "  printf 'HTTP/2 302 \\r\\ncontent-length: 0\\r\\n\\r\\n'\n"
            "  printf 'HTTP/2 200 \\r\\naccept-ranges: bytes\\r\\ncontent-length: %s\\r\\n\\r\\n'\n"
            "  exit 0\n"
            "fi\n"
            "if [ -z \"$range\" ]; then\n"
            "  cat \"$DATA/firmware_release_2.7.26.json\"\n"
            "  exit 0\n"
            "fi\n"
            "first=\"${range%%%%-*}\"; last=\"${range##*-}\"\n"
            "count=$((last - first + 1))\n"
            "if [ \"$first\" -ge %d ]; then\n"
            "  file=\"$DATA/zip_tail_nrf52840_2.7.26.bin\"; off=$((first - %d))\n"
            "elif [ \"$first\" -ge %d ]; then\n"
            "  file=\"$DATA/zip_member_t114_mt_json_2.7.26.bin\"; off=$((first - %d))\n"
            "else\n"
            "  exit 4\n"
            "fi\n"
            "tail -c \"+$((off + 1))\" \"$file\" | head -c \"$count\" > \"$out\"\n"
            "exit 0\n",
            MESH_TEST_DATA_DIR, ZIP_SIZE, TAIL_BASE, TAIL_BASE, MEMBER_BASE, MEMBER_BASE);
    const bool executable = fchmod(fileno(file), 0755) == 0;
    fclose(file);
    return executable;
}

struct update_harness {
    char dir[64];
    char *saved_path;
    struct mesh_event_loop loop;
    struct mesh_firmware_update update;
    bool loop_up;
    bool update_up;
};

static bool update_harness_up(struct update_harness *harness) {
    memset(harness, 0, sizeof *harness);
    snprintf(harness->dir, sizeof harness->dir, "%s", "/tmp/meshclient_fwup_XXXXXX");
    if (mkdtemp(harness->dir) == NULL) {
        return false;
    }
    if (!update_install_curl(harness->dir)) {
        return false;
    }
    const char *const old_path = getenv("PATH");
    harness->saved_path = old_path != NULL ? strdup(old_path) : strdup("");
    char new_path[1024];
    snprintf(new_path, sizeof new_path, "%s:%s", harness->dir,
             old_path != NULL ? old_path : "/usr/bin");
    setenv("PATH", new_path, 1);
    /* The image is staged in the scratch directory rather than /tmp, so a case cleans up after
       itself and two running side by side cannot collide. */
    setenv("MESHCLIENT_FIRMWARE_STAGING", harness->dir, 1);

    if (mesh_event_loop_init(&harness->loop) != 0) {
        return false;
    }
    harness->loop_up = true;
    if (mesh_firmware_update_init(&harness->update, &harness->loop) != 0) {
        return false;
    }
    harness->update_up = true;
    return mesh_firmware_update_available(&harness->update);
}

static void update_harness_down(struct update_harness *harness) {
    if (harness->update_up) {
        mesh_firmware_update_shutdown(&harness->update);
    }
    if (harness->loop_up) {
        mesh_event_loop_shutdown(&harness->loop);
    }
    unsetenv("MESHCLIENT_FIRMWARE_STAGING");
    if (harness->saved_path != NULL) {
        setenv("PATH", harness->saved_path, 1);
        free(harness->saved_path);
    }
    if (harness->dir[0] == '\0') {
        return;
    }
    static const char *const k_files[] = {"curl",         "firmware.window", "firmware.central",
                                          "firmware.header", "firmware.gz",  "firmware.image"};
    char path[512];
    for (size_t i = 0; i < sizeof k_files / sizeof k_files[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", harness->dir, k_files[i]);
        (void)unlink(path);
    }
    (void)rmdir(harness->dir);
}

static bool update_settle(struct update_harness *harness, const struct update_probe *probe) {
    for (int turn = 0; turn < 900 && probe->calls == 0U; ++turn) {
        (void)mesh_event_loop_run(&harness->loop, 10);
        mesh_firmware_update_tick(&harness->update, (uint64_t)turn * 100U);
    }
    return probe->calls > 0U;
}

/* ---- the cases ------------------------------------------------------------------------------- */

/*
 * Every state and every error has a word, and the two ladders are total.
 *
 * Cheap, and it is the check that stops a state added to the enum from drawing "unknown" in the
 * row that is supposed to be explaining what the client is doing to somebody's radio.
 */
MESH_TEST_CASE(firmware_update_names_every_state, unit) {
    for (int i = 0; i < (int)MESH_FIRMWARE_UPDATE_STATE_COUNT; ++i) {
        const char *const name =
            mesh_firmware_update_state_name((enum mesh_firmware_update_state)i);
        MESH_TEST_FAIL_IF(name == NULL || name[0] == '\0', "every state should have a word");
    }
    for (int i = 1; i < (int)MESH_FIRMWARE_UPDATE_ERROR_COUNT; ++i) {
        const char *const name =
            mesh_firmware_update_error_name((enum mesh_firmware_update_error)i);
        MESH_TEST_FAIL_IF(name == NULL || name[0] == '\0', "every failure should have a word");
    }
    MESH_TEST_FAIL_IF(mesh_firmware_update_error_name(MESH_FIRMWARE_UPDATE_ERROR_NONE)[0] != '\0',
                      "and no failure should have none");
    /* IDLE, DONE and FAILED are the three the section is allowed to sit in with the press still
       offered; everything between them takes the rows over. */
    MESH_TEST_FAIL_IF(mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_IDLE) ||
                          mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_DONE) ||
                          mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_FAILED),
                      "a settled job is not work in flight");
    MESH_TEST_FAIL_IF(!mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_READY) ||
                          !mesh_firmware_update_state_busy(MESH_FIRMWARE_UPDATE_WRITING),
                      "and everything between is");
    record_success(test_name);
}

/*
 * The refusals that happen before a byte moves, and the promise that comes with them: nothing
 * was started, so no completion will ever arrive - but `state` and `error` still say why,
 * because a refusal is a row.
 */
MESH_TEST_CASE(firmware_update_refuses_before_it_starts, unit) {
    struct update_probe probe;
    memset(&probe, 0, sizeof probe);
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);

    /* No loop, so no fetcher: the press cannot run whatever it is pointed at. */
    struct mesh_firmware_update bare;
    MESH_TEST_FAIL_IF(mesh_firmware_update_init(&bare, NULL) != 0, "init should succeed");
    const struct mesh_firmware_board board = update_t114_board();
    const struct mesh_firmware_release release = update_release();
    MESH_TEST_FAIL_IF(mesh_firmware_update_start(&bare, &board, &release, "", &hooks,
                                                 update_probe_done, &probe) != -ENOTSUP,
                      "with no fetcher the press should refuse");
    MESH_TEST_FAIL_IF(bare.state != MESH_FIRMWARE_UPDATE_FAILED ||
                          bare.error != MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE,
                      "and should still say why");
    MESH_TEST_FAIL_IF(probe.calls != 0U, "a refusal reports through its return, not a callback");
    mesh_firmware_update_shutdown(&bare);

    struct update_harness harness;
    const char *failure = NULL;
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    /*
     * A board with no path at all - an ESP32-C3, a portduino. It is a row in Settings already,
     * so reaching it here means a press was offered that should not have been; it is still
     * checked, because this is the module that would otherwise be trusting its caller about
     * which board this is.
     */
    struct mesh_firmware_board pathless = board;
    pathless.path = MESH_FIRMWARE_PATH_NONE;
    if (mesh_firmware_update_start(&harness.update, &pathless, &release, "", &hooks,
                                   update_probe_done, &probe) != -ENOTSUP) {
        failure = "a board with no path should refuse";
        goto cleanup;
    }
    /* A release that appeared in the index before its assets did, which really happens. */
    struct mesh_firmware_release assetless = release;
    assetless.manifest_url[0] = '\0';
    if (mesh_firmware_update_start(&harness.update, &board, &assetless, "", &hooks,
                                   update_probe_done, &probe) != -ENOTSUP) {
        failure = "a release with no manifest should refuse";
        goto cleanup;
    }
    if (probe.calls != 0U) {
        failure = "none of those should have reported through the callback";
        goto cleanup;
    }
    /* And none of them left the module looking busy, which is what would block the next press. */
    if (mesh_firmware_update_busy(&harness.update) ||
        mesh_firmware_update_holds_the_antenna(&harness.update) ||
        mesh_firmware_update_holds_the_radio(&harness.update)) {
        failure = "a refusal should hold nothing";
        goto cleanup;
    }

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The antenna and the radio are two questions, and the difference is the whole of what makes the
 * BLE path work.
 *
 * While the image comes down the *antenna* is held - Wi-Fi and Bluetooth are one part on this
 * hardware - and the radio is not, because nothing has been asked of it. That asymmetry is what
 * lets auto-connect bring a BLE radio back the moment the download lands, which is the only way
 * the `ota_request` ever reaches it. A single "busy" flag here would have left the link down
 * through a step whose entire purpose is waiting for it to come up.
 */
MESH_TEST_CASE(firmware_update_holds_the_antenna_not_the_radio, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    const struct mesh_firmware_board board = update_t114_board();
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (harness.update.state != MESH_FIRMWARE_UPDATE_RESOLVING) {
        failure = "and should open by working out which file";
        goto cleanup;
    }
    if (!mesh_firmware_update_holds_the_antenna(&harness.update)) {
        failure = "a download holds the antenna";
        goto cleanup;
    }
    if (mesh_firmware_update_holds_the_radio(&harness.update)) {
        failure = "and holds the radio only once something has been asked of it";
        goto cleanup;
    }
    if (mesh_firmware_update_radio_in_loader(&harness.update)) {
        failure = "and nothing on the USB path ever leaves a radio in a loader";
        goto cleanup;
    }
    /* Nothing has been asked of the radio yet either way. */
    if (probe.armed_usb != 0U || probe.armed_ble != 0U || probe.released != 0U) {
        failure = "a download arms nothing and releases nothing";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);
    if (mesh_firmware_update_holds_the_antenna(&harness.update) ||
        mesh_firmware_update_busy(&harness.update)) {
        failure = "and a cancelled job gives the antenna back";
        goto cleanup;
    }

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A download that fails is a download that failed, and the radio is untouched.
 *
 * The fake CDN above serves two windows of one release's zip and refuses everything else, so a
 * board it holds no member for gets as far as reading documents and no further. What is being
 * pinned is the fold: `firmware_fetch`'s eight errors become the four a row asks about, and
 * "the bytes did not arrive" must arrive as the one that means the radio never heard from us.
 */
MESH_TEST_CASE(firmware_update_reports_a_download_that_failed, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    /* A target the release built nothing for: a real answer about the release rather than a
       network failure, and the row that says so is not the row that says the wifi is down. */
    struct mesh_firmware_board unbuilt = update_t114_board();
    snprintf(unbuilt.target, sizeof unbuilt.target, "%s", "not-a-real-board");
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &unbuilt, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (!update_settle(&harness, &probe)) {
        failure = "the job should finish";
        goto cleanup;
    }
    if (probe.calls != 1U) {
        failure = "and should report exactly once";
        goto cleanup;
    }
    if (probe.state != MESH_FIRMWARE_UPDATE_FAILED ||
        probe.error != MESH_FIRMWARE_UPDATE_ERROR_DOWNLOAD) {
        failure = "a release with no image for this board is a download that failed";
        goto cleanup;
    }
    /* The radio was never asked anything, and nothing is holding a bus. */
    if (probe.armed_usb != 0U || probe.armed_ble != 0U) {
        failure = "and the radio should never have been asked";
        goto cleanup;
    }
    if (mesh_firmware_update_holds_the_antenna(&harness.update) ||
        mesh_firmware_update_holds_the_radio(&harness.update) ||
        mesh_firmware_update_busy(&harness.update)) {
        failure = "a failure lifts every hold by failing";
        goto cleanup;
    }
    /* The failure is readable rather than only categorised: the fetch's own line survives. */
    if (harness.update.detail[0] == '\0') {
        failure = "and should carry the download's own words";
        goto cleanup;
    }
    /* Pressing again is allowed, which is what "a failure lifts it by failing" is for. */
    const struct mesh_firmware_board board = update_t114_board();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "and a second press should be allowed after a failure";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A job already running refuses the next press rather than taking the child out from under the
   one in flight - the same rule the check next door follows for the same reason. */
MESH_TEST_CASE(firmware_update_refuses_a_second_press, unit) {
    struct update_harness harness;
    struct update_probe probe;
    const char *failure = NULL;
    memset(&probe, 0, sizeof probe);
    if (!update_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks = update_hooks(&probe);
    const struct mesh_firmware_board board = update_t114_board();
    const struct mesh_firmware_release release = update_release();
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (mesh_firmware_update_start(&harness.update, &board, &release, "2-1:1.1", &hooks,
                                   update_probe_done, &probe) != -EBUSY) {
        failure = "a second press should be refused while the first runs";
        goto cleanup;
    }
    if (probe.calls != 0U) {
        failure = "and should not have reported anything";
        goto cleanup;
    }
    mesh_firmware_update_cancel(&harness.update);
    if (probe.calls != 0U) {
        failure = "a cancel reports nothing either";
        goto cleanup;
    }

cleanup:
    update_harness_down(&harness);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
