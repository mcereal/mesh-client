#define _POSIX_C_SOURCE 200809L

/*
 * The overlays. The snackbar and the dialog each take the whole of something - the footer, or
 * the body - which is why neither advances a `y` the way a component inside a screen does.
 */

#include "fb_widgets_overlay.h"
#include "fb_widgets_button.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/anim.h"
#include "mesh/ui/emoji.h"
#include "mesh/ui/layout.h"
#include "mesh/utils/text.h"

#include <string.h>

/* ---- the snackbar ------------------------------------------------------------------------ */

/*
 * The snackbar's key in the animation table.
 *
 * Every other id in there is a control's identity handed in by a screen - a settings field, a
 * row index - and those are small enumerator values. There is exactly one snackbar and no
 * screen owns it, so it takes a constant from the far end of the range, where nothing a screen
 * can pass will ever land.
 */
#define FB_ANIM_ID_SNACKBAR 0xFFFFFF01U

/*
 * How long it takes to arrive, and how long to leave.
 *
 * Not the same token, and the asymmetry is the point: arriving is the part that has to be
 * seen, leaving is the part that has to be out of the way. It is the shape of every platform's
 * transient-notice motion. The theme says how long each is (enum mesh_ui_motion); what belongs
 * here is only which of the two kinds of movement this is.
 */
#define FB_SNACKBAR_IN_MOTION MESH_UI_MOTION_MEDIUM
#define FB_SNACKBAR_OUT_MOTION MESH_UI_MOTION_SHORT

/* Material allows one line or two, and two is where a notice stops being one on a 3.2" panel.
   Anything longer is clipped rather than allowed to grow into the body. */
#define FB_SNACKBAR_LINES_MAX 2U

void fb_draw_snackbar(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      const struct fb_snackbar *bar) {
    if (state == NULL || layout == NULL) {
        return;
    }

    const char *const text = (bar != NULL && bar->text != NULL) ? bar->text : "";
    const bool showing = (text[0] != '\0');

    if (showing &&
        (bar->until_ms != state->snackbar_until_ms || strcmp(text, state->snackbar) != 0)) {
        /*
         * A different notice, so it *arrives* rather than swapping its words.
         *
         * The table is put back to nothing with a zero duration first, then aimed at the
         * resting place below - which is the only way to get an entrance out of it, since
         * re-aiming at a target it already holds is deliberately a no-op and a slot seen for
         * the first time adopts its target rather than animating to it. Both of those are what
         * stops a switch sliding on the frame a screen opens; here they have to be stepped
         * around, because a notice appearing is exactly the thing worth showing.
         */
        (void)mesh_str_copy(state->snackbar, sizeof state->snackbar, text);
        state->snackbar_until_ms = bar->until_ms;
        (void)mesh_ui_anim_track(&state->anim, FB_ANIM_ID_SNACKBAR, state->now_ms, 0, 0U,
                                 MESH_UI_EASE_OUT);
    }
    if (state->snackbar[0] == '\0') {
        return;
    }

    const int32_t position = mesh_ui_anim_track(
        &state->anim, FB_ANIM_ID_SNACKBAR, state->now_ms, showing ? MESH_UI_ANIM_ONE : 0,
        fb_motion(state, showing ? FB_SNACKBAR_IN_MOTION : FB_SNACKBAR_OUT_MOTION),
        MESH_UI_EASE_OUT);
    if (!showing && position == 0) {
        /* All the way out. The store forgot the words several frames ago; now so does this,
           and the next notice starts from an empty slot rather than from this one's. */
        state->snackbar[0] = '\0';
        state->snackbar_until_ms = 0U;
        return;
    }

    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const int line = fb_line_adv(state, scale);
    const int margin = fb_margin(state);
    /* A cell in from each edge and a proportional band above and below: a container's padding
       is measured in the same units as what it holds, so a theme asking for bigger text gets a
       proportionally roomier bar rather than a tighter one. */
    const int pad_x = adv;
    const int pad_y = scale * 2;

    const int room = (int)state->var.xres - 2 * margin - 2 * pad_x;
    if (room < adv || line <= 0) {
        return; /* a geometry too small to hold one cell of it; nothing to say here */
    }
    const size_t max_cols = (size_t)(room / adv);

    uint32_t lines = mesh_ui_wrap_lines(state->snackbar, max_cols);
    if (lines == 0U) {
        lines = 1U;
    }
    if (lines > FB_SNACKBAR_LINES_MAX) {
        lines = FB_SNACKBAR_LINES_MAX;
    }
    /* Sized to its own words rather than to the panel, the way a bubble is: a bar the full
       width of the screen is a region of the chrome, and a notice is one thing that arrived. */
    size_t cols = mesh_ui_wrap_widest(state->snackbar, max_cols);
    if (cols == 0U) {
        cols = 1U;
    }

    const int box_w = (int)cols * adv + 2 * pad_x;
    /* Centred on the panel rather than against the leading margin. A bar sized to its own words
       and pinned to the left edge reads as the start of a row that ran out of things to say -
       which is what the footer line it replaced was. Centred, it reads as one object placed
       over the screen, and it stays put as the wording changes length instead of growing
       rightwards out of a fixed corner. */
    const int box_x = ((int)state->var.xres - box_w) / 2;
    const int box_h = (int)lines * line + 2 * pad_y;
    /* Where it comes to rest: over the bottom of the body, a full margin clear of the footer.
       A card stops half a margin short of the footer because a card is *in* the body and the
       body's own bottom edge is where it belongs; this one is over everything, so it keeps the
       distance the panel edge keeps rather than the one the body does. */
    const int rest_y = layout->footer_y - margin - box_h;
    /* And where it comes from: entirely below the panel. A container that slid in from just
       off its resting place reads as a nudge; one that comes up from off-screen reads as
       something arriving, which is the whole of what this shape is for. */
    const int off_y = (int)state->var.yres;
    fb_animation_damage(state, box_x, rest_y, box_w, off_y - rest_y + box_h);
    const int y = off_y + (int)(((int64_t)(rest_y - off_y) * position) / MESH_UI_ANIM_ONE);

    /*
     * The inverted surface, and no edge on it.
     *
     * A card needs a hairline because its fill is one step off the ground and the step alone is
     * not findable in daylight. This one is the other end of the palette - the furthest from
     * the ground the theme has - so the fill is the whole cue, and an outline over it would be
     * drawing a border around the most obvious thing on the panel.
     */
    fb_fill_round_rect(state, box_x, y, box_w, box_h, fb_radius(state, MESH_UI_SHAPE_SM),
                       fb_color(state, MESH_UI_COLOR_SURFACE_INVERSE));

    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT_ON_INVERSE);
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, state->snackbar, max_cols);
    int text_y = y + pad_y;
    for (uint32_t drawn = 0U; drawn < lines && mesh_ui_wrap_next(&wrap); ++drawn) {
        fb_draw_text(state, box_x + pad_x, text_y, wrap.line, scale, ink,
                     fb_color(state, MESH_UI_COLOR_SURFACE_INVERSE));
        text_y += line;
    }
}

/* ---- the dialog ---------------------------------------------------------------------------- */

/* How large the dialog's icon is drawn, in glyph-scale steps. Big enough to be the first thing
   read - it is the only thing on the panel that says what *kind* of question this is before a
   word of it has been - and clamped by fb_draw_icon() to what the sprite can carry. */
#define FB_DIALOG_ICON_SCALE 3

/*
 * `src` shortened until the button holding it fits `max_w`.
 *
 * Measured with fb_button_width() rather than by dividing the width by a cell, because a
 * button's padding and the gap after its icon are the button's business - a caller that did the
 * arithmetic itself would disagree with the thing it is sizing the moment either changes.
 *
 * Cut on cell boundaries, so a label with an emoji or an accented character in it loses a whole
 * glyph rather than half of a UTF-8 sequence. A label that cannot be made to fit at all keeps
 * its first cell: something is drawn, and the button still marks where the press lands.
 */
static void fb_fit_button_label(const struct mesh_ui_backend_fb_state *state,
                                enum mesh_ui_icon icon, const char *src, int scale, int max_w,
                                char *out, size_t out_len) {
    mesh_str_copy(out, out_len, src != NULL ? src : "");
    while (fb_button_width(state, icon, out, scale) > max_w) {
        const size_t cells = mesh_ui_text_cells(out);
        if (cells <= 1U) {
            return;
        }
        mesh_ui_text_cell_truncate(out, cells - 1U);
    }
}

void fb_draw_dialog(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    const struct fb_dialog *dialog) {
    if (dialog == NULL) {
        return;
    }
    const int scale = state->scale;
    const int margin = fb_margin(state);
    const int line = layout->line;
    const int edge = fb_edge(state);
    const int pad = margin;
    const int adv = fb_char_adv(state, scale);

    /* One decision for the whole panel: the icon, the headline and the accept button's fill all
       come off this, so a destructive dialog cannot end up half red. */
    const enum mesh_ui_family accept_family =
        dialog->destructive ? MESH_UI_FAMILY_ERROR : MESH_UI_FAMILY_PRIMARY;
    const enum mesh_ui_tone accent_tone = mesh_ui_family_tone(accept_family);

    const int panel_x = fb_gutter(state);
    const int panel_w = (int)state->var.xres - margin;
    /* The panel's own text column, which is narrower than the body's - a dialog is inset from
       the screen and its words are inset again from its edge. */
    const int text_w = panel_w - 2 * pad;
    const size_t text_cols = text_w > adv ? (size_t)(text_w / adv) : 1U;

    const int icon_scale = scale * FB_DIALOG_ICON_SCALE;
    const bool has_icon = mesh_ui_icon_is_valid(dialog->icon);
    const int icon_h = has_icon ? fb_line_adv(state, icon_scale) : 0;
    const bool has_headline = dialog->headline != NULL && dialog->headline[0] != '\0';
    /* A little more than a line: the headline and the paragraph under it are the same glyph
       size, so the gap is the only thing distinguishing a question from its explanation. */
    const int head_h = has_headline ? line + scale : 0;
    const int button_h = line + fb_space(state, MESH_UI_SPACE_MD);
    const int room = layout->footer_y - layout->body_y - line;

    /*
     * The action row, measured before the panel is: how tall the panel has to be depends on
     * whether the two answers fit beside each other.
     *
     * They do not always. "Reset the node database" at the largest glyph scale wants more than
     * the whole panel on its own, and side by side with Cancel it ran the cancel button off the
     * left-hand edge of the *screen* - on a destructive confirmation, where the safe answer is
     * the one that disappeared. So the row stacks when it has to, which is what every platform
     * does with dialog actions too long to sit in a line, and each label is fitted to the panel
     * so a single button can never overhang it either.
     */
    const int gap = scale * 2;
    char accept_label[MESH_UI_LINE_MAX];
    char cancel_label[MESH_UI_LINE_MAX];
    fb_fit_button_label(state, MESH_UI_ICON_CHECK, dialog->accept, scale, text_w, accept_label,
                        sizeof accept_label);
    fb_fit_button_label(state, MESH_UI_ICON_CLOSE, dialog->cancel, scale, text_w, cancel_label,
                        sizeof cancel_label);
    const int cancel_w = fb_button_width(state, MESH_UI_ICON_CLOSE, cancel_label, scale);
    const int accept_w = fb_button_width(state, MESH_UI_ICON_CHECK, accept_label, scale);
    /* Stacked puts the answer that acts on top, the way a stacked dialog orders them - the
       dismissive one stays nearest the thumb. */
    const bool stacked = (accept_w + gap + cancel_w) > text_w;
    const int actions_h = stacked ? (2 * button_h + gap) : button_h;

    /*
     * Measure before drawing, as a card does and for the same reason: the fill has to go down
     * before the text, and how tall it is depends on how far the paragraph wraps.
     *
     * The paragraph is also the only part that gives way. Everything else on the panel is
     * either the question or the answers, and a dialog that dropped one of its buttons to fit
     * its explanation would be unanswerable - so the buttons, the headline and the icon are
     * reserved first and the supporting text takes what is left.
     */
    const int fixed = pad + icon_h + head_h + line / 2 + actions_h + pad;
    const int text_room = room - fixed;
    const uint32_t fits = text_room > 0 ? (uint32_t)(text_room / line) : 0U;
    uint32_t text_lines = (dialog->text != NULL && dialog->text[0] != '\0')
                              ? mesh_ui_wrap_lines(dialog->text, text_cols)
                              : 0U;
    if (text_lines > fits) {
        text_lines = fits;
    }

    int panel_h = fixed + (int)text_lines * line;
    if (panel_h > room) {
        panel_h = room;
    }
    /* Centred in what is left of the body, which is what makes it read as something placed over
       the screen rather than as the screen's own first rows. */
    const int panel_y = layout->body_y + (room > panel_h ? (room - panel_h) / 2 : 0);

    const int radius = fb_radius(state, MESH_UI_SHAPE_LG);
    fb_fill_round_rect(state, panel_x, panel_y, panel_w, panel_h, radius + edge,
                       fb_color(state, MESH_UI_COLOR_OUTLINE));
    fb_fill_round_rect(state, panel_x + edge, panel_y + edge, panel_w - 2 * edge,
                       panel_h - 2 * edge, radius, fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));

    const int content_x = panel_x + pad;
    int y = panel_y + pad;

    if (has_icon) {
        fb_draw_icon(state, content_x, y, dialog->icon, icon_scale,
                     fb_tone_color(state, accent_tone),
                     fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));
        y += icon_h;
    }

    if (has_headline) {
        struct mesh_ui_line headline;
        mesh_ui_line_reset(&headline);
        mesh_ui_line_printf(&headline, "%s", dialog->headline);
        mesh_ui_line_fit(&headline, text_cols);
        fb_draw_text(state, content_x, y, mesh_ui_line_text(&headline), scale,
                     fb_tone_color(state, accent_tone),
                     fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));
        y += head_h;
    }

    if (text_lines > 0U) {
        fb_draw_wrapped_at(state, content_x, y, dialog->text, text_cols, (int)text_lines,
                           fb_tone_color(state, MESH_UI_TONE_NORMAL),
                           fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));
    }

    /*
     * Side by side, the answer that does nothing goes on the left and the one that acts on the
     * right, so the press that costs something is never the one nearest a thumb resting where
     * it was. Stacked, the same reasoning puts the acting answer on top.
     */
    const int right = panel_x + panel_w - pad;
    const int accept_y = panel_y + panel_h - pad - (stacked ? (2 * button_h + gap) : button_h);
    const int cancel_y = stacked ? (accept_y + button_h + gap) : accept_y;
    const int accept_x = right - accept_w;
    const int cancel_x = stacked ? (right - cancel_w) : (accept_x - gap - cancel_w);

    const struct fb_button cancel = {
        .rect = {cancel_x, cancel_y, cancel_w, button_h},
        .icon = MESH_UI_ICON_CLOSE,
        .label = cancel_label,
        .selected = dialog->cursor != 0U,
        .variant = FB_BUTTON_TEXT,
        .shape = MESH_UI_SHAPE_FULL,
        .idle_tone = MESH_UI_TONE_NORMAL,
        .ground = MESH_UI_COLOR_SURFACE_HIGH,
        .scale = scale,
    };
    fb_draw_button(state, &cancel);

    /*
     * Exactly one of the two carries a fill, and it is always the focused one.
     *
     * The obvious design gives the accept a standing fill so it reads as the proposed answer,
     * and lets the cursor pick between them. That works on three of the four themes and fails
     * on the one where it matters most: the high-contrast palette deliberately collapses
     * PRIMARY, PRIMARY_CONTAINER and SURFACE_ACTIVE onto a single yellow, because a theme built
     * for legibility does not have a "held back" version of its one accent. Two buttons whose
     * fills both resolve to that yellow are two buttons a user cannot tell apart, on the theme
     * chosen by the people least able to guess.
     *
     * So the fill means focus and nothing else, and what marks the accept as the affirmative is
     * its check and its family-coloured label - a cue that survives every palette because it is
     * ink on the panel rather than one fill against another.
     */
    const bool accept_selected = dialog->cursor == 0U;
    const struct fb_button accept = {
        .rect = {accept_x, accept_y, accept_w, button_h},
        .icon = MESH_UI_ICON_CHECK,
        .label = accept_label,
        .selected = accept_selected,
        .variant = accept_selected ? FB_BUTTON_TONAL : FB_BUTTON_TEXT,
        /* The dialog's family, so a destructive confirm is a red pill rather than a red word on
           the ordinary one - and the ink on it is the one checked against that red. */
        .family = accept_family,
        .shape = MESH_UI_SHAPE_FULL,
        .idle_tone = accent_tone,
        .ground = MESH_UI_COLOR_SURFACE_HIGH,
        .scale = scale,
    };
    fb_draw_button(state, &accept);
}

/* ---- the QR code ---------------------------------------------------------------------------- */

/*
 * How many pixels one module gets, and therefore how big the code comes out.
 *
 * Integer division on purpose - see the header. A code whose modules are 7.4 pixels across has
 * boundaries that fall between pixels, and a reader thresholding a photograph of that finds
 * edges the code does not have; seven is worth more than the 5% of the box that rounding down
 * gives away.
 */
static int fb_qr_module_px(const struct fb_qr *qr) {
    if (qr == NULL || qr->code == NULL || qr->code->size == 0U) {
        return 0;
    }
    /* Four modules of quiet zone on each side, which the standard asks for and a reader needs
       to lock on at all. */
    const int modules = (int)qr->code->size + 8;
    const int room = qr->box.w < qr->box.h ? qr->box.w : qr->box.h;
    return room / modules;
}

int fb_qr_side(const struct fb_qr *qr) {
    const int px = fb_qr_module_px(qr);
    return px > 0 ? px * ((int)qr->code->size + 8) : 0;
}

void fb_draw_qr(const struct mesh_ui_backend_fb_state *state, const struct fb_qr *qr) {
    const int px = fb_qr_module_px(qr);
    if (px <= 0) {
        return;
    }
    const int size = (int)qr->code->size;
    const int side = px * (size + 8);
    const int x0 = qr->box.x + (qr->box.w - side) / 2;
    const int y0 = qr->box.y + (qr->box.h - side) / 2;

    /* The margin is drawn rather than left to the screen behind it: the quiet zone is part of
       the code, and a reader that cannot find it does not lock on. */
    fb_fill_rect(state, x0, y0, side, side, fb_color(state, MESH_UI_COLOR_CODE_GROUND));
    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_CODE);
    for (int y = 0; y < size; ++y) {
        /* A run of dark modules is one fill rather than one per module: a code at the version
           cap is thirteen thousand of them, and the whole screen is repainted whenever anything
           on the frame moves. */
        int run = 0;
        for (int x = 0; x <= size; ++x) {
            const bool dark = x < size && mesh_qr_dark(qr->code, x, y);
            if (dark) {
                run++;
                continue;
            }
            if (run > 0) {
                fb_fill_rect(state, x0 + (x - run + 4) * px, y0 + (y + 4) * px, run * px, px, ink);
                run = 0;
            }
        }
    }
}
