#include "mesh/ui/firmware_progress.h"

#include "mesh/core/firmware.h"
#include "mesh/core/firmware_update.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/store_settings.h"

#include <string.h>

static const inkcell_str_id k_stage_names[MESH_UI_FIRMWARE_STAGE_COUNT] = {
    [MESH_UI_FIRMWARE_STAGE_DOWNLOAD] = MESH_STR_FW_STAGE_DOWNLOAD,
    [MESH_UI_FIRMWARE_STAGE_PREPARE] = MESH_STR_FW_STAGE_PREPARE,
    [MESH_UI_FIRMWARE_STAGE_INSTALL] = MESH_STR_FW_STAGE_INSTALL,
    [MESH_UI_FIRMWARE_STAGE_RESTART] = MESH_STR_FW_STAGE_RESTART,
};

inkcell_str_id mesh_ui_firmware_stage_name(enum mesh_ui_firmware_stage stage) {
    return (unsigned)stage < (unsigned)MESH_UI_FIRMWARE_STAGE_COUNT ? k_stage_names[stage]
                                                                     : MESH_STR_FW_STAGE_DOWNLOAD;
}

/*
 * The stage a state is on, and the sentence for it. One row per rung of the ladder, so a state
 * added to the enum without a stage here is a hole the test for this table finds.
 */
struct firmware_rung {
    uint8_t stage;
    inkcell_str_id status;
};

static const struct firmware_rung k_rungs[MESH_FIRMWARE_UPDATE_STATE_COUNT] = {
    [MESH_FIRMWARE_UPDATE_IDLE] = {MESH_UI_FIRMWARE_STAGE_DOWNLOAD, MESH_STR_FW_SCREEN_RESOLVING},
    [MESH_FIRMWARE_UPDATE_RESOLVING] = {MESH_UI_FIRMWARE_STAGE_DOWNLOAD,
                                        MESH_STR_FW_SCREEN_RESOLVING},
    [MESH_FIRMWARE_UPDATE_DOWNLOADING] = {MESH_UI_FIRMWARE_STAGE_DOWNLOAD,
                                          MESH_STR_FW_SCREEN_DOWNLOADING},
    [MESH_FIRMWARE_UPDATE_READY] = {MESH_UI_FIRMWARE_STAGE_PREPARE, MESH_STR_FW_SCREEN_READY},
    [MESH_FIRMWARE_UPDATE_ARMING] = {MESH_UI_FIRMWARE_STAGE_PREPARE, MESH_STR_FW_SCREEN_ARMING},
    [MESH_FIRMWARE_UPDATE_WAITING] = {MESH_UI_FIRMWARE_STAGE_PREPARE, MESH_STR_FW_SCREEN_WAITING},
    [MESH_FIRMWARE_UPDATE_WRITING] = {MESH_UI_FIRMWARE_STAGE_INSTALL, MESH_STR_FW_SCREEN_WRITING},
    [MESH_FIRMWARE_UPDATE_RESTARTING] = {MESH_UI_FIRMWARE_STAGE_RESTART,
                                         MESH_STR_FW_SCREEN_RESTARTING},
    [MESH_FIRMWARE_UPDATE_DONE] = {MESH_UI_FIRMWARE_STAGE_COUNT, MESH_STR_FW_SCREEN_DONE},
    [MESH_FIRMWARE_UPDATE_FAILED] = {MESH_UI_FIRMWARE_STAGE_DOWNLOAD, MESH_STR_FW_SCREEN_FAILED},
};

/*
 * Where a failure happened, which the record no longer says: FAILED replaces the state it
 * failed in. The error does say it, coarsely, and coarsely is all the steps row needs - the
 * bytes never arrived, the radio would not take the request, or the handover itself broke.
 */
static uint8_t firmware_failed_stage(enum mesh_firmware_update_error error) {
    switch (error) {
    case MESH_FIRMWARE_UPDATE_ERROR_NO_RADIO:
    case MESH_FIRMWARE_UPDATE_ERROR_REFUSED:
        return MESH_UI_FIRMWARE_STAGE_PREPARE;
    case MESH_FIRMWARE_UPDATE_ERROR_HANDOVER:
        return MESH_UI_FIRMWARE_STAGE_INSTALL;
    default:
        return MESH_UI_FIRMWARE_STAGE_DOWNLOAD;
    }
}

void mesh_ui_firmware_progress_of(const struct mesh_ui_settings *settings,
                                  struct mesh_ui_firmware_progress *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->title = MESH_STR_FW_SCREEN_TITLE;
    out->status = MESH_STR_FW_SCREEN_RESOLVING;
    out->hint = MESH_STR_FW_SCREEN_HINT_USB;
    if (settings == NULL) {
        return;
    }

    enum mesh_firmware_update_state state =
        (enum mesh_firmware_update_state)settings->fw_update_state;
    if ((unsigned)state >= (unsigned)MESH_FIRMWARE_UPDATE_STATE_COUNT) {
        state = MESH_FIRMWARE_UPDATE_IDLE;
    }
    out->stage = k_rungs[state].stage;
    out->status = k_rungs[state].status;
    out->title = settings->fw_switching ? MESH_STR_FW_SCREEN_TITLE_SWITCH : MESH_STR_FW_SCREEN_TITLE;
    const bool ble = settings->fw_bus == (uint8_t)MESH_FIRMWARE_PATH_BLE;
    out->hint = ble ? MESH_STR_FW_SCREEN_HINT_BLE : MESH_STR_FW_SCREEN_HINT_USB;

    /* The two rungs with a fraction fill the dial; `firmware_update.c` answers 0 on the rest,
       and a ring drawn empty through a forty-second wait says the job stalled. */
    if ((state == MESH_FIRMWARE_UPDATE_DOWNLOADING || state == MESH_FIRMWARE_UPDATE_WRITING) &&
        settings->fw_update_progress > 0U) {
        out->determinate = true;
        out->permille = (uint16_t)((settings->fw_update_progress > 100U
                                        ? 100U
                                        : settings->fw_update_progress) *
                                   10U);
    }

    if (state == MESH_FIRMWARE_UPDATE_DONE) {
        out->done = true;
        out->determinate = true;
        out->permille = 1000U;
        out->hint = MESH_STR_FW_SCREEN_HINT_DONE;
    } else if (state == MESH_FIRMWARE_UPDATE_FAILED) {
        out->failed = true;
        out->stage = firmware_failed_stage(
            (enum mesh_firmware_update_error)settings->fw_update_error);
        /* The one failure that leaves the radio somewhere it cannot leave on its own gets the
           banner's own way out; every other one left the radio as it was. */
        out->hint = settings->fw_radio_in_loader ? MESH_STR_FW_SCREEN_HINT_LOADER
                    : settings->fw_update_error == (uint8_t)MESH_FIRMWARE_UPDATE_ERROR_HANDOVER
                        ? MESH_STR_FW_SCREEN_HINT_HANDOVER
                        : MESH_STR_FW_SCREEN_HINT_UNTOUCHED;
    }
}
