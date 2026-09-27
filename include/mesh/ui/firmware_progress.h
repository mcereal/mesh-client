#ifndef MESH_UI_FIRMWARE_PROGRESS_H
#define MESH_UI_FIRMWARE_PROGRESS_H

/*
 * What the firmware install's screen says: which stage, how far through it, and in what words.
 *
 * The install reports a ladder of eight states (mesh/core/firmware_update.h), and that ladder is
 * right for a settings row that answers "where has this got to" in one word. A screen that is
 * *about* the install has a different question to answer - how much is left - and eight rungs
 * of which six have no fraction do not answer it. So they are folded here into four stages a
 * person would name - fetching the image, getting the radio ready for it, writing it, and the
 * radio coming back - and the stage is what the steps row draws while the dial says how far
 * through it the job is.
 *
 * Here rather than in the renderer because it is a decision about meaning, not about pixels,
 * and because it is the kind a test should hold: which stage a failure is drawn at, and whether
 * a state has a fraction worth drawing, are both easy to get subtly wrong.
 */

#include "inkcell/i18n/strings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_settings;

enum mesh_ui_firmware_stage {
    MESH_UI_FIRMWARE_STAGE_DOWNLOAD = 0, /* resolving, downloading */
    MESH_UI_FIRMWARE_STAGE_PREPARE,      /* waiting for the radio, arming, waiting for its loader */
    MESH_UI_FIRMWARE_STAGE_INSTALL,      /* writing */
    MESH_UI_FIRMWARE_STAGE_RESTART,      /* restarting */
    MESH_UI_FIRMWARE_STAGE_COUNT,
};

struct mesh_ui_firmware_progress {
    /* The stage under way; MESH_UI_FIRMWARE_STAGE_COUNT once the job is done. */
    uint8_t stage;
    bool done;
    /* The job stopped at `stage` and will not go on. */
    bool failed;
    /* The state carries a fraction, so the dial fills rather than spins. */
    bool determinate;
    uint16_t permille;
    inkcell_str_id title;  /* what the screen is about: an update or a switch */
    inkcell_str_id status; /* where it has got to, as a sentence */
    inkcell_str_id hint;   /* what the reader should do, or what it means now */
};

/* Reads the install's record out of the settings. Leaves `out` a done-nothing download when
   there is no install at all, which the nav does not open the screen on. */
void mesh_ui_firmware_progress_of(const struct mesh_ui_settings *settings,
                                  struct mesh_ui_firmware_progress *out);

/* The stage's name for the steps row. */
inkcell_str_id mesh_ui_firmware_stage_name(enum mesh_ui_firmware_stage stage);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_FIRMWARE_PROGRESS_H */
