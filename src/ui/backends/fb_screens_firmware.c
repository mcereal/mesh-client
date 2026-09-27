/*
 * The radio firmware install, as the whole screen.
 *
 * The one job in this client long enough to be watched: a download, a radio that has to be
 * talked into its updater, a write, and a restart, and minutes of it. The settings row that
 * reports it answers "where has this got to" in one word; this answers "how much is left", which
 * is a different drawing - the dial for how far through this stage, the steps for which stage of
 * four, and a sentence for each under it. What it says is mesh/ui/firmware_progress.h's; this
 * only lays it out.
 *
 * Stacked down the middle of the panel with no tabs above it: the frame drops the navigation
 * for this route, because walking sideways out of a write is not a move anyone means to make and
 * the keys for it are swallowed anyway. The dial takes whatever height the words leave.
 */

#include "inkcell/ui/widgets.h"

#include "fb_screens_internal.h"
#include "mesh/core/firmware_update.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/firmware_progress.h"

#include <stdio.h>

/* Animation slot for the dial - a key into the state's table, not a pixel. */
#define FB_FIRMWARE_DIAL_ID 0x46570001U

/* One line of `text` in `role`, centred on the region; returns its height. */
static int fb_firmware_line(struct inkcell_draw_state *state, int y, const char *text,
                            enum inkcell_type role, enum inkcell_color color) {
    const struct inkcell_type_style style = inkcell_fb_type_style(state, role);
    const struct inkcell_box region = inkcell_fb_region(state);
    const int width = inkcell_fb_text_width_styled(state, text, &style);
    inkcell_fb_draw_text_styled(state, region.x + (region.w - width) / 2, y, text, &style,
                                inkcell_fb_color(state, color),
                                inkcell_fb_color(state, INKCELL_COLOR_BG));
    return inkcell_scale_px((int)inkcell_fb_font(state)->height, style.scale);
}

static int fb_firmware_line_height(const struct inkcell_draw_state *state,
                                   enum inkcell_type role) {
    return inkcell_scale_px((int)inkcell_fb_font(state)->height,
                            inkcell_fb_type_style(state, role).scale);
}

void fb_render_firmware(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                        struct inkcell_fb_layout *layout) {
    const struct mesh_ui_settings *s = &snapshot->settings;
    struct mesh_ui_firmware_progress progress;
    mesh_ui_firmware_progress_of(s, &progress);

    const struct inkcell_box region = inkcell_fb_region(state);
    const int gap = inkcell_fb_space(state, INKCELL_SPACE_MD);
    const int wide_gap = inkcell_fb_space(state, INKCELL_SPACE_LG);
    const int line = layout->line;

    /* The versions under the title: the radio's and the one going on, or just the one going on
       when the radio has not said - or when this is a switch, where the two numbers belong to
       two different projects and an arrow between them would compare them. */
    char versions[64] = "";
    if (s->fw_latest[0] != '\0') {
        if (s->has_metadata && s->firmware_version[0] != '\0' && !s->fw_switching) {
            inkcell_str_format(versions, sizeof versions, MESH_STR_FW_SCREEN_VERSIONS,
                               s->firmware_version, s->fw_latest);
        } else {
            (void)snprintf(versions, sizeof versions, "%s", s->fw_latest);
        }
    }

    /* The failure's own words, which the error's name stands in for when the radio said none. */
    const char *detail = NULL;
    if (progress.failed) {
        detail = s->fw_update_detail[0] != '\0'
                     ? s->fw_update_detail
                     : mesh_firmware_update_error_name(
                           (enum mesh_firmware_update_error)s->fw_update_error);
        if (detail[0] == '\0') {
            detail = NULL;
        }
    }
    const char *hint = inkcell_str(progress.hint);
    const int hint_lines = (int)inkcell_fb_wrapped_lines(state, hint, (size_t)layout->body_w,
                                                         state->scale);
    const int hint_rows = hint_lines > 2 ? 2 : hint_lines;

    /* Everything but the dial, measured, so the dial can have the rest. */
    const int title_h = fb_firmware_line_height(state, INKCELL_TYPE_HEADLINE);
    const int status_h = fb_firmware_line_height(state, INKCELL_TYPE_TITLE);
    const int steps_h = inkcell_fb_steps_height(state, state->scale);
    int words = title_h + gap + status_h + gap + hint_rows * line + wide_gap + steps_h;
    if (versions[0] != '\0') {
        words += line;
    }
    if (detail != NULL) {
        words += line;
    }
    const int top = layout->body_y + gap;
    const int bottom = layout->footer_y - gap;
    int side = bottom - top - words - 2 * gap;
    /* No more than half the body, so the screen keeps some air round its subject rather than
       being a ring with words squeezed into the corners. */
    if (side > (bottom - top) / 2) {
        side = (bottom - top) / 2;
    }
    if (side > region.w) {
        side = region.w;
    }
    const bool dial = side >= inkcell_fb_dial_min_side(state, state->scale);
    /* The block centred in the body, so a short panel and a tall one both read as one thing. */
    int y = top + ((bottom - top) - (words + (dial ? side + 2 * gap : 0))) / 2;
    if (y < top) {
        y = top;
    }

    y += fb_firmware_line(state, y, inkcell_str(progress.title), INKCELL_TYPE_HEADLINE,
                          INKCELL_COLOR_TEXT);
    if (versions[0] != '\0') {
        y += fb_firmware_line(state, y, versions, INKCELL_TYPE_BODY, INKCELL_COLOR_TEXT_DIM) +
             (line - fb_firmware_line_height(state, INKCELL_TYPE_BODY));
    }
    y += gap;

    if (dial) {
        y += gap;
        char figure[16] = "";
        if (progress.determinate && !progress.done) {
            inkcell_str_format(figure, sizeof figure, MESH_STR_NODE_VAL_PERCENT,
                               (unsigned)(progress.permille / 10U));
        }
        const struct inkcell_fb_dial ring = {
            .rect = {.x = region.x + (region.w - side) / 2, .y = y, .w = side, .h = side},
            .id = FB_FIRMWARE_DIAL_ID,
            .kind = progress.determinate || progress.failed ? INKCELL_FB_DIAL_DETERMINATE
                                                            : INKCELL_FB_DIAL_INDETERMINATE,
            /* A failure keeps the ring where it stopped: how far it got is part of the story. */
            .value = (int32_t)progress.permille,
            .scale = {.min = 0, .max = 1000},
            .tone = progress.failed ? INKCELL_TONE_ERROR
                    : progress.done ? INKCELL_TONE_SUCCESS
                                    : INKCELL_TONE_PRIMARY,
            .ground = INKCELL_COLOR_BG,
            .label = figure[0] != '\0' ? figure : NULL,
            .icon = progress.failed ? INKCELL_ICON_CLOSE
                    : progress.done ? INKCELL_ICON_CHECK
                                    : INKCELL_ICON_NONE,
        };
        inkcell_fb_draw_dial(state, &ring);
        y += side + gap;
    }

    y += fb_firmware_line(state, y, inkcell_str(progress.status), INKCELL_TYPE_TITLE,
                          INKCELL_COLOR_TEXT_STRONG);
    if (detail != NULL) {
        y += fb_firmware_line(state, y, detail, INKCELL_TYPE_BODY, INKCELL_COLOR_TEXT) +
             (line - fb_firmware_line_height(state, INKCELL_TYPE_BODY));
    }
    y += gap;
    y += inkcell_fb_draw_wrapped_centered(state, y, hint, (size_t)layout->body_w, 2,
                                          inkcell_fb_tone_color(state, INKCELL_TONE_DIM),
                                          inkcell_fb_color(state, INKCELL_COLOR_BG)) *
         line;
    y += wide_gap;

    const char *labels[MESH_UI_FIRMWARE_STAGE_COUNT];
    for (size_t i = 0U; i < (size_t)MESH_UI_FIRMWARE_STAGE_COUNT; ++i) {
        labels[i] = inkcell_str(mesh_ui_firmware_stage_name((enum mesh_ui_firmware_stage)i));
    }
    /* Three quarters of the column at most: four markers spread edge to edge across a wide
       panel stop reading as one row. */
    const int steps_w = layout->body_w * 3 / 4;
    const struct inkcell_fb_steps steps = {
        .rect = {.x = region.x + (region.w - steps_w) / 2, .y = y, .w = steps_w, .h = steps_h},
        .labels = labels,
        .count = (size_t)MESH_UI_FIRMWARE_STAGE_COUNT,
        .current = progress.stage,
        .tone = progress.done ? INKCELL_TONE_SUCCESS : INKCELL_TONE_PRIMARY,
        .halted = progress.failed,
        .ground = INKCELL_COLOR_BG,
    };
    inkcell_fb_draw_steps(state, &steps);
}
