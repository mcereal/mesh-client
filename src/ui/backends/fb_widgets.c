#define _POSIX_C_SOURCE 200809L

/*
 * The components. Everything here is built from fb_draw.c's primitives and knows nothing about
 * the snapshot - a widget takes what it draws, not where it came from, which is what lets the
 * same list component carry devices, nodes, messages and settings rows.
 */

#include "fb_widgets.h"

#include "mesh/ui/anim.h"
#include "mesh/ui/emoji.h"
#include "mesh/utils/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * The fill a variant wears in a state, and the ink that goes on it.
 *
 * One table rather than a chain of ifs at the draw site, because the point of a variant is
 * that its two colours travel together: every pair below is one mesh_ui_theme_validate()
 * holds to 4.5:1, and splitting them across branches is how a label ends up on a fill nothing
 * checked it against. `has_fill` false means the button draws no ground of its own and the
 * caller's idle tone is the ink.
 */
struct fb_button_paint {
    bool has_fill;
    struct mesh_ui_paint paint;
};

static struct fb_button_paint fb_button_paint(const struct mesh_ui_backend_fb_state *state,
                                              const struct fb_button *button) {
    const bool selected = button->selected;
    switch (button->variant) {
    case FB_BUTTON_FILLED:
        /* The neutral cursor surface, not a family: a keyboard key is a place to press, not a
           statement about what pressing it means. */
        return (struct fb_button_paint){
            true,
            {fb_color(state, selected ? MESH_UI_COLOR_SURFACE_ACTIVE : MESH_UI_COLOR_SURFACE_SEL),
             fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)}};
    case FB_BUTTON_TONAL:
        /* Under the cursor a tonal control commits to the family's full strength: the container
           is there so a label can be read over it, and a control being pressed has stopped
           being something to read. One call, so the fill and the ink cannot come from different
           halves of the family. */
        return (struct fb_button_paint){
            true,
            fb_paint(state, button->family, selected ? MESH_UI_SLOT_BASE : MESH_UI_SLOT_CONTAINER,
                     MESH_UI_STATE_REST)};
    case FB_BUTTON_TEXT:
    default:
        return selected ? (struct fb_button_paint){true,
                                                   {fb_color(state, MESH_UI_COLOR_SURFACE_ACTIVE),
                                                    fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)}}
                        : (struct fb_button_paint){false,
                                                   {fb_color(state, MESH_UI_COLOR_BG),
                                                    fb_color(state, MESH_UI_COLOR_TEXT)}};
    }
}

/* What a button's content occupies: its icon, the gap after it, and its label - all in cells
   except that gap, which is half of one. */
static int fb_button_content_w(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_button *button) {
    const int adv = fb_char_adv(state, button->scale);
    const bool has_label = button->label != NULL && button->label[0] != '\0';
    const bool has_icon = mesh_ui_icon_is_valid(button->icon);
    const int label_w = has_label ? (int)mesh_ui_text_cells(button->label) * adv : 0;
    return label_w + (has_icon ? fb_icon_box(state, button->scale) : 0) +
           ((has_icon && has_label) ? adv / 2 : 0);
}

void fb_draw_button(const struct mesh_ui_backend_fb_state *state, const struct fb_button *button) {
    const struct fb_button_paint paint = fb_button_paint(state, button);
    if (paint.has_fill) {
        fb_fill_round_rect(state, button->rect.x, button->rect.y, button->rect.w, button->rect.h,
                           fb_radius(state, button->shape), paint.paint.fill);
    }

    const bool has_label = button->label != NULL && button->label[0] != '\0';
    const bool has_icon = mesh_ui_icon_is_valid(button->icon);
    if (!has_label && !has_icon) {
        return;
    }

    /* Centre by cells, so a label holding an emoji sits where it looks centred rather than
       where its byte count says it does. An icon is one cell, so it centres by the same count.
       Vertically it is the glyph body that is centred, not the line advance: the advance
       carries the gap that accents hang in, and counting it would push every label low in its
       box. */
    const int adv = fb_char_adv(state, button->scale);
    /* Half a cell between a symbol and the word after it, which is the gap Material puts
       there and about what the eye needs to stop reading them as one shape. fb_button_content_w()
       is the same sum, so a caller sizing a box around this gets the box this fills. */
    const int text_w = fb_button_content_w(state, button);
    const int text_h = (int)fb_font(state)->height * button->scale;
    int x = button->rect.x + (button->rect.w - text_w) / 2;
    const int y = button->rect.y + (button->rect.h - text_h) / 2;
    /* Any fill at all means the label is drawn in the colour the theme validates against that
       fill. Only a button with no fill is free to take its idle tone, which is chosen against
       the ground - reading the tone over a fill is how the keyboard's action row came out
       white on white. */
    const struct mesh_ui_rgb ink =
        paint.has_fill ? paint.paint.ink : fb_tone_color(state, button->idle_tone);
    if (has_icon) {
        /* Blended against the fill the button has just laid down, or against whatever the
           caller says it is sitting on when it laid none - the two colours the ink was chosen
           against. */
        fb_draw_icon(state, x, y, button->icon, button->scale, ink,
                     paint.has_fill ? paint.paint.fill : fb_color(state, button->ground));
        x += fb_icon_box(state, button->scale) + (has_label ? adv / 2 : 0);
    }
    if (has_label) {
        fb_draw_text(state, x, y, button->label, button->scale, ink,
                     paint.has_fill ? paint.paint.fill : fb_color(state, button->ground));
    }
}

/*
 * Padding enough to clear the capsule's own curve, and derived from the *glyph scale* rather
 * than from the cell advance.
 *
 * A capsule's ends eat into their own corners, so a label needs room the flat-sided chip did
 * not: the pill's radius is half its height - (7 + 2) / 2 scale steps for the 5x7 font - and at
 * the top and bottom of a glyph body the edge has curved inwards by about 1.7 steps. Two either
 * side clears that with room to spare.
 *
 * Why not measure it in cells, which is how everything else here is measured? Because a cell is
 * six scale steps wide, so a cell of padding either side is three times what the curve needs,
 * and the strip has to *fit*: five tabs at MESHCLIENT_FB_SCALE=5 leave about 60 px of slack
 * across a 1024 px panel, and a padding that generous spends 240 of it. The last tab and its
 * indicator then fall off the right-hand edge, which is a navigation tab the user can no longer
 * see rather than a cosmetic overflow.
 */
#define FB_CHIP_PAD_STEPS 4

int fb_button_width(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_icon icon,
                    const char *label, int scale) {
    const struct fb_button button = {.icon = icon, .label = label, .scale = scale};
    return fb_button_content_w(state, &button) + FB_CHIP_PAD_STEPS * scale;
}

int fb_chip_width(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_icon icon,
                  const char *label, int scale) {
    /* The pill itself, then the gap before the next one. */
    return fb_button_width(state, icon, label, scale) + fb_char_adv(state, scale);
}

int fb_draw_chip(const struct mesh_ui_backend_fb_state *state, int x, int y, enum mesh_ui_icon icon,
                 const char *label, bool active, enum mesh_ui_color ground, int scale) {
    const int width = fb_chip_width(state, icon, label, scale);
    const struct fb_button button = {
        .rect = {.x = x,
                 .y = y - scale,
                 .w = width - fb_char_adv(state, scale), /* the pill, without the gap after it */
                 .h = fb_line_adv(state, scale)},
        .icon = icon,
        .label = label,
        .selected = false,
        .variant = active ? FB_BUTTON_TONAL : FB_BUTTON_TEXT,
        .shape = MESH_UI_SHAPE_FULL,
        .idle_tone = MESH_UI_TONE_DIM,
        .ground = ground,
        .scale = scale,
    };
    fb_draw_button(state, &button);
    return x + width;
}

/* ---- the chip strip, the navigation bar and the action bar -------------------------------- */

/* Whether this chip keeps its words at this setting. See enum fb_chip_labels. */
static const char *fb_chip_strip_label(const struct fb_chip *chip, size_t index, size_t active,
                                       enum fb_chip_labels labels) {
    if (labels == FB_CHIP_LABELS_ALL || (labels == FB_CHIP_LABELS_SELECTED && index == active)) {
        return chip->label;
    }
    return "";
}

/*
 * What a chip's badge takes, at this label setting - see struct fb_chip for why the setting is
 * what decides its shape. Zero when there is nothing waiting, so an unbadged strip measures and
 * draws exactly as it did before badges existed.
 */
static int fb_chip_badge_width(const struct mesh_ui_backend_fb_state *state, const char *badge,
                               enum fb_chip_labels labels, int scale) {
    if (badge == NULL || badge[0] == '\0') {
        return 0;
    }
    return labels == FB_CHIP_LABELS_NONE ? fb_char_adv(state, scale)
                                         : fb_badge_width(state, badge, scale);
}

/*
 * Draws it against the chip's trailing edge, in the space fb_chip_badge_width() reserved.
 *
 * Beside the pill rather than inside it, which is both what Material's navigation bar does and
 * the only placement that stays honest here: fb_draw_button() centres its content in the box it
 * is given, so a pill widened to swallow a badge would slide its own icon and label sideways by
 * half the badge - and on the active tab, the one wearing a fill, the words would run under the
 * capsule at the wider glyph scales.
 */
static void fb_draw_chip_badge(const struct mesh_ui_backend_fb_state *state, int x, int y,
                               const char *badge, enum fb_chip_labels labels, int scale) {
    const int width = fb_chip_badge_width(state, badge, labels, scale);
    if (width <= 0) {
        return;
    }
    /* The chip's own box, as fb_draw_chip() derives it. */
    const int top = y - scale;
    const int height = fb_line_adv(state, scale);

    if (labels == FB_CHIP_LABELS_NONE) {
        const int size = width / 2 > 0 ? width / 2 : 1;
        const struct mesh_ui_paint paint =
            fb_paint(state, MESH_UI_FAMILY_PRIMARY, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
        fb_fill_round_rect(state, x + (width - size) / 2, top + (height - size) / 2, size, size,
                           fb_radius(state, MESH_UI_SHAPE_FULL), paint.fill);
        return;
    }

    const struct fb_rect box = {.x = x, .y = top, .w = width, .h = height};
    const int text_h = (int)fb_font(state)->height * scale;
    fb_draw_badge(state, &box, top + (height - text_h) / 2, badge, MESH_UI_FAMILY_PRIMARY, scale);
}

int fb_chip_strip_width(const struct mesh_ui_backend_fb_state *state, const struct fb_chip *chips,
                        size_t count, size_t active, enum fb_chip_labels labels, int scale) {
    int width = 0;
    for (size_t i = 0; i < count; ++i) {
        width += fb_chip_width(state, chips[i].icon,
                               fb_chip_strip_label(&chips[i], i, active, labels), scale);
        width += fb_chip_badge_width(state, chips[i].badge, labels, scale);
    }
    return width;
}

enum fb_chip_labels fb_chip_strip_fit(const struct mesh_ui_backend_fb_state *state,
                                      const struct fb_chip *chips, size_t count, size_t active,
                                      int room, int scale) {
    enum fb_chip_labels labels = FB_CHIP_LABELS_ALL;
    while (labels < FB_CHIP_LABELS_NONE &&
           fb_chip_strip_width(state, chips, count, active, labels, scale) > room) {
        labels = (enum fb_chip_labels)(labels + 1);
    }
    return labels;
}

int fb_draw_chip_strip(const struct mesh_ui_backend_fb_state *state, int x, int y,
                       const struct fb_chip *chips, size_t count, size_t active, int room,
                       enum mesh_ui_color ground, int scale) {
    const enum fb_chip_labels labels = fb_chip_strip_fit(state, chips, count, active, room, scale);
    for (size_t i = 0; i < count; ++i) {
        /*
         * The badge goes in the gap fb_draw_chip() already leaves after the pill, and the chip
         * that carries one is that much wider. Measured and drawn by the same two calls in the
         * same order as the width above, because a strip that measures itself differently from
         * the way it draws is a strip whose last tab falls off the panel.
         */
        const int after = fb_draw_chip(state, x, y, chips[i].icon,
                                       fb_chip_strip_label(&chips[i], i, active, labels),
                                       i == active, ground, scale);
        const int badge = fb_chip_badge_width(state, chips[i].badge, labels, scale);
        if (badge > 0) {
            fb_draw_chip_badge(state, after - fb_char_adv(state, scale), y, chips[i].badge, labels,
                               scale);
        }
        x = after + badge;
    }
    return x;
}

void fb_draw_nav_bar(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                     const struct fb_chip *tabs, size_t count, size_t active) {
    const int small = layout->small;
    const int y = fb_gutter(state) + small;
    const int bar_h = y + fb_line_adv(state, small);
    const int width = (int)state->var.xres;

    fb_fill_rect(state, 0, 0, width, bar_h, fb_color(state, MESH_UI_COLOR_SURFACE_LOW));
    /* The bar under them, not the body ground: an unselected tab draws no fill of its own, and
       its icon has to blend into what the bar filled behind it. */
    (void)fb_draw_chip_strip(state, fb_gutter(state), y, tabs, count, active,
                             width - fb_margin(state), MESH_UI_COLOR_SURFACE_LOW, small);
    fb_draw_rule(state, 0, bar_h, width, small, MESH_UI_COLOR_RULE_STRONG);

    layout->nav_y = bar_h + fb_rule_height(state, small);
    layout->body_y = bar_h + fb_space_at(state, MESH_UI_SPACE_MD, small) + fb_gutter(state);
}

/* ---- the screen progress bar ---------------------------------------------------------------- */

/*
 * Its key in the animation table, from the far end of the range where no screen's own id lands
 * - the same reasoning as FB_ANIM_ID_SNACKBAR, and the same neighbourhood.
 */
#define FB_ANIM_ID_PROGRESS 0xFFFFFF03U

/*
 * How tall the bar is.
 *
 * The meter's own thickness at the *body* scale rather than at the chrome scale it sits in.
 * Chrome is drawn smaller because it is text and text has to stay legible in less room; a
 * hairline has no such reason, and one scaled down with the tab labels is a bar nobody notices
 * on the screens it exists for.
 */
static int fb_progress_thickness(const struct mesh_ui_backend_fb_state *state) {
    return fb_meter_thickness(state, state->scale);
}

void fb_draw_progress(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      bool busy) {
    if (state == NULL || layout == NULL || !busy) {
        return;
    }
    const int height = fb_progress_thickness(state);
    if (height <= 0 || layout->nav_y <= 0) {
        return;
    }
    /*
     * Full bleed, which is the one place in this UI a container reaches the panel edge.
     *
     * Everything else is inset by the margin because everything else is content. This is not:
     * it is the navigation bar's rule saying something, so it runs the width of the rule it
     * hangs off. Inset, it would read as the first row of the body - which is exactly the
     * mistake the tab strip made before it was given a surface of its own.
     */
    struct fb_meter meter = {
        .rect = {.x = 0, .y = layout->nav_y, .w = (int)state->var.xres, .h = height},
        .kind = FB_METER_INDETERMINATE,
        .tone = MESH_UI_TONE_PRIMARY,
        .id = FB_ANIM_ID_PROGRESS,
    };
    fb_draw_meter(state, &meter);
}

/* ---- the banner ------------------------------------------------------------------------------ */

void fb_draw_banner(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                    const struct fb_banner *banner) {
    if (state == NULL || layout == NULL || banner == NULL || banner->text == NULL ||
        banner->text[0] == '\0') {
        return;
    }

    const int scale = state->scale;
    const int small = layout->small;
    const int adv = fb_char_adv(state, scale);
    const int small_adv = fb_char_adv(state, small);
    const int margin = fb_margin(state);
    const int pad_x = fb_space(state, MESH_UI_SPACE_MD);
    const int pad_y = fb_space(state, MESH_UI_SPACE_SM);
    const int top = layout->body_y;
    const int width = (int)state->var.xres - 2 * margin;
    if (width <= 2 * pad_x || adv <= 0 || layout->line <= 0) {
        return;
    }

    /*
     * Measured in glyph *bodies* rather than in line advances, which is the same correction the
     * app bar's overline made: an advance carries the gap between two lines of running text,
     * and neither of these lines is running text. Spending it here would have cost a body row
     * for space nobody sees.
     */
    const int font_h = (int)fb_font(state)->height;
    const int head_top = top + pad_y;
    const int head_h = font_h * scale;
    const int sup_gap = fb_space_at(state, MESH_UI_SPACE_XS, small);
    const int sup_h = font_h * small;
    const int gap = fb_space(state, MESH_UI_SPACE_SM);

    /*
     * Whether the second line is affordable.
     *
     * A banner is chrome that eats content, so the rule is that it may not eat all of it: when
     * taking the supporting line would leave the screen under it with no row at all, the
     * supporting line is what goes. The headline is the half that says what is true; the hint
     * is the half that says where to go about it, and a hint over an empty screen is worse than
     * no hint.
     */
    bool supporting = banner->supporting != NULL && banner->supporting[0] != '\0';
    int height = 2 * pad_y + head_h + (supporting ? sup_gap + sup_h : 0);
    if (supporting && layout->footer_y - fb_gutter(state) - (top + height + gap) < layout->line) {
        supporting = false;
        height = 2 * pad_y + head_h;
    }

    const struct mesh_ui_paint paint =
        fb_paint(state, banner->family, MESH_UI_SLOT_CONTAINER, MESH_UI_STATE_REST);
    fb_fill_round_rect(state, margin, top, width, height, fb_radius(state, MESH_UI_SHAPE_MD),
                       paint.fill);

    int x = margin + pad_x;
    int right = margin + width - pad_x;

    if (banner->icon != MESH_UI_ICON_NONE) {
        fb_draw_icon(state, x, head_top, banner->icon, scale, paint.ink, paint.fill);
        x += fb_icon_box(state, scale) + adv / 2;
    }

    /*
     * The detail, against the trailing edge and at the label scale.
     *
     * It recedes by *size* rather than by colour, and that is the type scale doing the job a
     * second ink would otherwise have been invented for: the container has one validated pair,
     * and a dimmed variant of its ink is a contract no theme has been held to.
     */
    if (banner->detail != NULL && banner->detail[0] != '\0') {
        const int detail_w = (int)mesh_ui_text_cells(banner->detail) * small_adv;
        if (detail_w > 0 && right - detail_w > x) {
            right -= detail_w;
            /* Centred on the headline's glyph body rather than sharing its top edge: a smaller
               face hung from the same line reads as having slipped up off it. */
            fb_draw_text(state, right, head_top + (head_h - sup_h) / 2, banner->detail, small,
                         paint.ink, paint.fill);
            right -= small_adv;
        }
    }

    struct mesh_ui_line headline;
    mesh_ui_line_reset(&headline);
    mesh_ui_line_printf(&headline, "%s", banner->text);
    mesh_ui_line_fit(&headline, (size_t)(right > x ? (right - x) / adv : 0));
    fb_draw_text(state, x, head_top, mesh_ui_line_text(&headline), scale, paint.ink, paint.fill);

    if (supporting) {
        const int room = margin + width - pad_x - x;
        struct mesh_ui_line hint;
        mesh_ui_line_reset(&hint);
        mesh_ui_line_printf(&hint, "%s", banner->supporting);
        mesh_ui_line_fit(&hint, (size_t)(room > 0 ? room / small_adv : 0));
        /* Indented to the headline's own left edge, past the icon: the symbol leads the whole
           banner rather than only its first line, so a hint starting under it would read as a
           second, unmarked notice. */
        fb_draw_text(state, x, head_top + head_h + sup_gap, mesh_ui_line_text(&hint), small,
                     paint.ink, paint.fill);
    }

    /* What is left of the body, recomputed from its real bottom rather than deducted - see the
       tail of fb_draw_app_bar() for why a deduction is wrong here. */
    layout->body_y = top + height + gap;
    if (layout->line > 0) {
        const int remaining = layout->footer_y - fb_gutter(state) - layout->body_y;
        layout->rows = remaining > 0 ? (uint32_t)(remaining / layout->line) : 0U;
    }
}

/* The cap's pill and the verb after it, with the half cell between them that every icon-plus-
   label pair in this file uses. Measured rather than assumed, because the pill's padding is
   the button's business (see FB_CHIP_PAD_STEPS). */
static int fb_action_width(const struct mesh_ui_backend_fb_state *state,
                           const struct mesh_ui_button_action *action, int scale) {
    const int adv = fb_char_adv(state, scale);
    const char *label = mesh_str(action->label);
    return fb_button_width(state, MESH_UI_ICON_NONE, mesh_ui_button_cap(action->button), scale) +
           adv / 2 + (int)mesh_ui_text_cells(label) * adv;
}

int fb_action_bar_height(const struct mesh_ui_backend_fb_state *state,
                         const struct fb_layout *layout) {
    const int small = layout->small;
    /* The keycap row, the status line under it, and a margin below - the same margin the two
       plain lines this replaced left, so the bar sits off the panel edge by the amount the rest
       of the frame does rather than by an amount of its own. */
    return fb_space_at(state, MESH_UI_SPACE_SM, small) + fb_line_adv(state, small) +
           fb_space_at(state, MESH_UI_SPACE_XS, small) + fb_line_adv(state, small) +
           fb_margin(state);
}

void fb_draw_action_bar(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, const struct fb_action_bar *bar) {
    const int small = layout->small;
    const int width = (int)state->var.xres;
    const int top = layout->footer_y;

    /* The mirror of the navigation bar: the same recessed tier, the same rule, on the other
       edge. Chrome that is a surface at the top and bare ground at the bottom reads as a frame
       with one side missing. */
    fb_fill_rect(state, 0, top, width, (int)state->var.yres - top,
                 fb_color(state, MESH_UI_COLOR_SURFACE_LOW));
    fb_draw_rule(state, 0, top, width, small, MESH_UI_COLOR_RULE_STRONG);

    const int keys_y = top + fb_space_at(state, MESH_UI_SPACE_SM, small) + small;
    const int gap = fb_space_at(state, MESH_UI_SPACE_MD, small);
    const int right = width - fb_gutter(state);
    int x = fb_gutter(state);

    for (size_t i = 0; i < bar->count; ++i) {
        const struct mesh_ui_button_action *action = &bar->items[i];
        const char *cap = mesh_ui_button_cap(action->button);
        const int cap_w = fb_button_width(state, MESH_UI_ICON_NONE, cap, small);
        /* Dropped from the end rather than clipped: half a verb is a button whose meaning has
           to be guessed, and the tables are written with the least important action last. */
        if (x + fb_action_width(state, action, small) > right) {
            break;
        }

        const struct fb_button key = {
            .rect = {.x = x, .y = keys_y - small, .w = cap_w, .h = fb_line_adv(state, small)},
            .label = cap,
            .variant = FB_BUTTON_FILLED,
            .shape = MESH_UI_SHAPE_SM,
            .ground = MESH_UI_COLOR_SURFACE_LOW,
            .scale = small,
        };
        fb_draw_button(state, &key);

        x += cap_w + fb_char_adv(state, small) / 2;
        fb_draw_text(state, x, keys_y, mesh_str(action->label), small,
                     fb_tone_color(state, MESH_UI_TONE_DIM),
                     fb_color(state, MESH_UI_COLOR_SURFACE_LOW));
        x += (int)mesh_ui_text_cells(mesh_str(action->label)) * fb_char_adv(state, small) + gap;
    }

    if (bar->status == NULL || bar->status[0] == '\0') {
        return;
    }
    /* Sized to the line builder that produced it, not to the toast that used to share this row:
       a status line is a transport state and a radio's advertised name, and a name is only
       bounded by what the radio says it is called. */
    char status[MESH_UI_LINE_MAX];
    mesh_str_copy(status, sizeof status, bar->status);
    fb_fit(status, fb_cols(state, small));
    fb_draw_text(state, fb_margin(state),
                 keys_y - small + fb_line_adv(state, small) +
                     fb_space_at(state, MESH_UI_SPACE_XS, small),
                 status, small, fb_tone_color(state, bar->status_tone),
                 fb_color(state, MESH_UI_COLOR_SURFACE_LOW));
}

/* ---- the badge ------------------------------------------------------------------------------
 *
 * A capsule of text, filled from one family and inked with the ink that family was validated
 * with. The shape does the work: a filled rectangle on the end of a row reads as part of it,
 * and the same fill with its ends taken off reads as something sitting on top.
 *
 * It is drawn here rather than inside the trailing slot because it now has two callers - a
 * list row's unread count, and the top app bar's "3 unsaved" - and a capsule that two places
 * drew separately is a capsule that would end up two different shapes.
 */

int fb_badge_width(const struct mesh_ui_backend_fb_state *state, const char *text, int scale) {
    const size_t cells = text != NULL ? mesh_ui_text_cells(text) : 0U;
    /* Half a cell either side of the words: enough to clear the capsule's own curve at every
       glyph scale a theme may pick, and it is what the row's badge has always taken. */
    return cells > 0U ? (int)(cells + 1U) * fb_char_adv(state, scale) : 0;
}

void fb_draw_badge(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                   int text_y, const char *text, enum mesh_ui_family family, int scale) {
    if (text == NULL || text[0] == '\0' || box->w <= 0) {
        return;
    }
    /* One call for both halves: whatever the theme says reads on that family's own fill - on
       the dark palette that is the ground colour, because white on its yellow is unreadable at
       this glyph size. */
    const struct mesh_ui_paint paint =
        fb_paint(state, family, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
    fb_fill_round_rect(state, box->x, box->y, box->w, box->h, fb_radius(state, MESH_UI_SHAPE_FULL),
                       paint.fill);
    fb_draw_text(state, box->x + fb_char_adv(state, scale) / 2, text_y, text, scale, paint.ink,
                 paint.fill);
}

/* ---- the top app bar ------------------------------------------------------------------------ */

/* The trail, drawn left to right with a chevron between the levels, from `x`. Returns nothing:
   a trail that runs out of room stops, because the level nearest the title is the one worth
   keeping and it is drawn last. */
static void fb_draw_app_bar_trail(const struct mesh_ui_backend_fb_state *state,
                                  const struct fb_app_bar *bar, int x, int y, int right,
                                  int scale) {
    const int adv = fb_char_adv(state, scale);
    const struct mesh_ui_rgb ink = fb_tone_color(state, MESH_UI_TONE_DIM);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);
    const int step = fb_icon_box(state, scale);

    struct mesh_ui_line word;
    for (size_t i = 0; i < bar->trail_count; ++i) {
        const char *text = bar->trail[i];
        if (text == NULL || text[0] == '\0') {
            continue;
        }
        if (i > 0U) {
            /* The separator is an icon, not a character. The breadcrumb this replaced spelled
               it "> " inside the translated title, which handed a translator the trail's
               grammar along with its words; a chevron drawn from the icon set is the same mark
               the rows that open something already use, and it is untranslated for the same
               reason an arrow on a keycap is. */
            if (x + step + adv / 2 > right) {
                return;
            }
            /* A quarter of a cell either side. The chevron sprite fills its cell, so a
               separator advanced by the bare icon box has the two level names touching it and
               the trail reads as one word. */
            fb_draw_icon(state, x + adv / 4, y, MESH_UI_ICON_CHEVRON, scale, ink, ground);
            x += step + adv / 2;
        }
        const int room = (right - x) / adv;
        if (room <= 0) {
            return;
        }
        mesh_ui_line_reset(&word);
        mesh_ui_line_printf(&word, "%s", text);
        mesh_ui_line_fit(&word, (size_t)room);
        fb_draw_text(state, x, y, mesh_ui_line_text(&word), scale, ink, ground);
        x += (int)mesh_ui_text_cells(mesh_ui_line_text(&word)) * adv;
    }
}

int fb_app_bar_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      size_t trail_count) {
    /*
     * The same three terms fb_draw_app_bar() advances `body_y` by, in the same order.
     *
     * Written out rather than shared with the drawing path because sharing it would mean the
     * draw calling this and then re-deriving `y` from it, which is the arithmetic in a different
     * arrangement rather than in one place. Two expressions for one height is a real risk and
     * the test below the fold is what holds them together: fb_map's body is measured from this
     * and drawn under a bar laid out by that, so any disagreement puts the map's ground a few
     * pixels off its own heading, where it is visible in a capture.
     */
    int height = 0;
    if (trail_count > 0U) {
        height += (int)fb_font(state)->height * layout->small +
                  fb_space_at(state, MESH_UI_SPACE_XS, layout->small);
    }
    height += fb_line_adv(state, fb_type_scale(state, MESH_UI_TYPE_TITLE));
    height += fb_space(state, MESH_UI_SPACE_SM);
    return height;
}

void fb_draw_app_bar(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                     const struct fb_app_bar *bar) {
    /*
     * A title is drawn at MESH_UI_TYPE_TITLE, which is a step above the body.
     *
     * It used to be drawn at the body scale and told apart from the rows beneath it by
     * MESH_UI_TONE_PRIMARY alone - a heading exactly the size of its own content, with colour
     * carrying the whole of the hierarchy. The tone stays; it is now saying the same thing the
     * size already said, which is what a heading is supposed to look like.
     */
    const int scale = fb_type_scale(state, MESH_UI_TYPE_TITLE);
    const int small = layout->small;
    const int adv = fb_char_adv(state, scale);
    const int margin = fb_margin(state);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);

    int y = layout->body_y;
    /* The content column: the trail and the title share a left edge, and the back arrow hangs
       in the gutter to the left of both - which is where every platform puts it, and what keeps
       the two lines reading as one block rather than as two things that happen to be stacked. */
    const int text_x = layout->back ? margin + fb_icon_box(state, scale) + adv / 2 : margin;

    /*
     * The overline, when this screen is somewhere rather than at a tab's root.
     *
     * It costs a label-scale line and it buys the whole of the breadcrumb's width back: at the
     * title scale "Settings > Modules > Telemetry" is thirty cells of a thirty-four cell line,
     * so the leaf - the one word saying which screen this is - was the half that got elided.
     * Above the title, at the label scale, the same trail is half as wide and the title has the
     * panel to itself.
     */
    if (bar->trail_count > 0U) {
        fb_draw_app_bar_trail(state, bar, text_x, y, (int)state->var.xres - margin, small);
        /* The glyph body and a hair, not the label scale's whole line advance. The advance
           carries the gap between two lines of running text, and the trail is not running text
           - it is a caption sitting on the title. Spending the advance here cost a body row on
           every screen with a trail, which is a row of content for a gap nobody sees. */
        y += (int)fb_font(state)->height * small + fb_space_at(state, MESH_UI_SPACE_XS, small);
    }

    /*
     * The leading affordance: what B does, said by the chrome rather than only by the keycap
     * at the bottom of the panel. layout->back is the action bar's own answer (see
     * mesh_ui_action_bar_goes_back), so the arrow and the keycap cannot disagree.
     */
    if (layout->back) {
        fb_draw_icon(state, margin, y, MESH_UI_ICON_BACK, scale,
                     fb_tone_color(state, MESH_UI_TONE_DIM), ground);
    }

    /*
     * The trailing slot: a fact about the *screen*, which is the one thing a title could not
     * carry. "3 unsaved" used to be a " (unsaved)" glued onto the end of the title with a %s,
     * where it was neither countable nor a badge - a capsule cannot be spelled inside a
     * sentence.
     */
    int right = (int)state->var.xres - margin;
    const int badge_w = fb_badge_width(state, bar->badge, small);
    if (badge_w > 0) {
        /* Centred on the title's glyph body rather than on its line advance: the advance
           carries the gap accents hang in, and counting it would sit the capsule low. */
        const int title_h = (int)fb_font(state)->height * scale;
        const int badge_h = (int)fb_font(state)->height * small;
        const int text_y = y + (title_h - badge_h) / 2;
        const struct fb_rect box = {.x = right - badge_w,
                                    .y = text_y - small,
                                    .w = badge_w,
                                    .h = fb_line_adv(state, small) - small};
        fb_draw_badge(state, &box, text_y, bar->badge, bar->badge_family, small);
        right -= badge_w + fb_char_adv(state, small);
    }

    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", bar->title != NULL ? bar->title : "");
    /* Fitted to the room *this* scale leaves between the two slots, not to the body's column
       count. Bigger glyphs mean fewer of them, and a title measured against a column count it
       is not drawn at is a title that runs off the panel. */
    const int room = right > text_x ? (right - text_x) / adv : 0;
    mesh_ui_line_fit(&line, (size_t)(room > 0 ? room : 0));
    fb_draw_text(state, text_x, y, mesh_ui_line_text(&line), scale,
                 fb_tone_color(state, MESH_UI_TONE_PRIMARY), ground);

    /*
     * What is left of the body, recomputed rather than deducted.
     *
     * This used to advance by the body's line advance and deduct exactly one row, which was
     * already slightly out - the advance included a gap the deduction did not - and would be
     * properly wrong now that the line being advanced past is taller than a body row.
     *
     * Deducting a rounded-up row count is not the fix either, and that is the subtle part:
     * `rows` is a *floored* division of the body height, so the body carries a remainder of up
     * to one row that the count never included. Subtracting ceil(advance / line) from it
     * charges the bar for that remainder a second time and hides a row that does in fact
     * fit - on the Brick's panel the remainder is most of a row, so every titled screen lost
     * one for nothing.
     *
     * So the count is taken again from the same two numbers fb_render_snapshot() used, against
     * the body's real bottom. Measuring it the same way twice is what keeps the two answers
     * from disagreeing.
     */
    layout->body_y = y + fb_line_adv(state, scale) + fb_space(state, MESH_UI_SPACE_SM);
    if (layout->line > 0) {
        const int remaining = layout->footer_y - fb_gutter(state) - layout->body_y;
        layout->rows = remaining > 0 ? (uint32_t)(remaining / layout->line) : 0U;
    }
}

void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   enum mesh_ui_icon icon, const char *text) {
    int y = layout->body_y;
    uint32_t rows = layout->rows;

    /*
     * The icon is drawn at three glyph scales - a cell is one line tall, so three of them is
     * three body rows and about a fifth of the panel - and it is only drawn when the screen has
     * the rows to spare. An empty state is the one place with room for it, and the one place
     * where a symbol says "nothing here yet" faster than the sentence under it does.
     */
    const int big = state->scale * 3;
    const uint32_t cost = 4U; /* three rows for the symbol, one of air under it */
    if (mesh_ui_icon_is_valid(icon) && big <= FB_ICON_SCALE_MAX && rows > cost + 1U) {
        const int box = fb_icon_box(state, big);
        fb_draw_icon(state, ((int)state->var.xres - box) / 2, y, icon, big,
                     fb_tone_color(state, MESH_UI_TONE_DIM), fb_color(state, MESH_UI_COLOR_BG));
        y += (int)cost * layout->line;
        rows -= cost;
    }

    /* Wrapped rather than drawn flat: these strings say which button to press next, and at a
       large glyph scale a flat one ran off the right edge with the verb on it. */
    (void)fb_draw_wrapped(state, y, text, layout->cols, (int)rows,
                          fb_tone_color(state, MESH_UI_TONE_DIM),
                          fb_color(state, MESH_UI_COLOR_BG));
}

int fb_rule_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    return fb_space_at(state, MESH_UI_SPACE_XS, scale);
}

void fb_draw_rule(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int scale,
                  enum mesh_ui_color role) {
    fb_fill_rect(state, x, y, w, fb_rule_height(state, scale), fb_color(state, role));
}

/* Everything but the window, which is the one thing the three entry points differ in. */
static struct fb_list fb_list_open(const struct fb_layout *layout, struct mesh_ui_list model) {
    struct fb_list list;
    memset(&list, 0, sizeof list);
    list.model = model;
    list.y = layout->body_y;
    list.line = layout->line;
    list.cols = layout->cols;
    /* The window the rail measures, which is the body rather than the rows that happened to be
       filled: a list of three items in a body of fifteen has no rail at all, and a list of
       forty-two wants one the height of what a full window would have been. */
    list.track_y = layout->body_y;
    list.track_h = (int)layout->rows * layout->line;
    return list;
}

struct fb_list fb_list_begin_visible(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, uint32_t visible) {
    return fb_list_open(layout, mesh_ui_list_begin(count, cursor, visible));
}

struct fb_list fb_list_begin(const struct fb_layout *layout, uint32_t count, uint32_t cursor) {
    return fb_list_begin_visible(layout, count, cursor, layout->rows);
}

struct fb_list fb_list_begin_rows(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                  uint32_t per_item) {
    /*
     * The division is the model's now rather than this line's, and that is not tidying: a body
     * of fifteen rows holding two-row items used to arrive here as a window of seven, and the
     * fifteenth row was rounded away before anything could know it had been there. The model is
     * told fifteen and two, so the slack stays a fact about the window - which is what lets a
     * list of mixed heights spend it on a one-row item.
     */
    const uint32_t step = per_item > 0U && per_item < 0xFFU ? per_item : 1U;
    return fb_list_open(layout,
                        mesh_ui_list_begin_step(count, cursor, layout->rows, (uint8_t)step));
}

struct fb_list fb_list_begin_heights(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, const uint8_t *heights) {
    return fb_list_open(layout, mesh_ui_list_begin_heights(count, cursor, layout->rows, heights));
}

uint32_t fb_list_row_height(const struct fb_list *list, uint32_t index) {
    return mesh_ui_list_item_height(&list->model, index);
}

/*
 * The scroll rail. Drawn once per list, by the first row that draws - see fb_widgets.h.
 *
 * It sits in the half-margin outside the row fill, so it costs no row a single cell: rows clip
 * their text at `xres - margin` and the cursor fill stops at `xres - margin / 2`, which leaves
 * this gutter free. Its width comes from the glyph scale like every other control here, so it
 * stays in proportion when a theme asks for bigger text.
 */
static void fb_list_rail(const struct mesh_ui_backend_fb_state *state, struct fb_list *list) {
    if (list == NULL || list->rail_drawn) {
        return;
    }
    list->rail_drawn = true;
    if (list->track_h <= 0) {
        return;
    }

    /*
     * Sized from the gutter it lives in rather than from the glyph scale, unlike every other
     * control here. The gutter is half a margin wide and does not grow when a theme asks for
     * bigger text, so a rail measured in glyph steps ran out of clearance and ended up flush
     * against the panel edge at the larger scales. A quarter-margin leaves the same gap either
     * side at every scale, which is what makes it read as inset rather than as a screen edge.
     */
    int width = fb_gutter(state) / 2;
    if (width < 2) {
        width = 2;
    }

    /* The proportion is mesh_ui_list_scroll()'s - no pixels in it, and unit tested there. A
       length of 0 is a list that fits, which draws nothing at all rather than a full track. */
    const struct mesh_ui_scroll scroll =
        mesh_ui_list_scroll(&list->model, list->track_h, 4 * width);
    if (scroll.length <= 0) {
        return;
    }

    /* Centred in the gutter between the row fill's right edge and the panel edge. */
    const int x = (int)state->var.xres - fb_gutter(state) / 2 - width / 2;
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);

    /* The track is the role that already means one - the same groove a meter's fill sits in -
       and the thumb is quiet ink, named as a tone the way an icon in a row slot is. That pairing
       is contract-checked: MESH_UI_TONE_DIM owes the ground 3:1 on every theme, so the thumb is
       findable on all four, which a second neutral role chosen by eye against the track was
       not. */
    fb_fill_round_rect(state, x, list->track_y, width, list->track_h, radius,
                       fb_color(state, MESH_UI_COLOR_METER_TRACK));
    fb_fill_round_rect(state, x, list->track_y + scroll.offset, width, scroll.length, radius,
                       fb_tone_color(state, MESH_UI_TONE_DIM));
}

bool fb_list_next(struct fb_list *list, uint32_t *index) {
    return mesh_ui_list_next(&list->model, index);
}

void fb_list_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                 const char *text, enum mesh_ui_tone tone) {
    fb_list_rail(state, list);
    fb_draw_row(state, list->y, text, fb_tone_color(state, tone),
                mesh_ui_list_is_cursor(&list->model, index));
    /* By what the model says this row is, not by one row: a plain row in a list of mixed
       heights is still whatever height that list gave it, and advancing by a row would put
       every row under it in the wrong place. */
    list->y += (int)fb_list_row_height(list, index) * list->line;
}

void fb_list_subheader(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                       uint32_t index, const char *text) {
    fb_list_subheader_icon(state, list, index, text, (struct fb_leading){.kind = FB_LEADING_NONE});
}

void fb_list_subheader_icon(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                            uint32_t index, const char *text, struct fb_leading leading) {
    fb_list_rail(state, list);
    const int scale = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
    const uint32_t rows = fb_list_row_height(list, index);
    const bool selected = mesh_ui_list_is_cursor(&list->model, index);

    /* The fill is the whole step whatever size the words are, and it is the same rectangle a
       plain row lays down - a highlight that shrank to the label would be a cursor that changes
       shape as it walks down a list. */
    const struct mesh_ui_rgb ground = fb_draw_row_fill(state, list->y, rows, selected);

    /*
     * Sat on the bottom of the step, so the space the smaller glyphs free is air above the
     * heading rather than under it. That is the whole of what makes it read as a section break:
     * the gap belongs to the group beginning, not to the row that ended.
     */
    const int baseline = list->y + fb_line_adv(state, state->scale) - fb_line_adv(state, scale);
    /*
     * Indented to where its own rows start, when the list declares a leading slot.
     *
     * A heading that stayed at the margin over rows whose words begin an icon-box further in is
     * a heading naming a column nothing is in, which is the two-column problem the leading slot
     * already refuses one row at a time. The slot is measured at the *body* scale, not the
     * label scale this draws at, because it is the rows' gutter being matched rather than one
     * of this row's own.
     *
     * Nothing is drawn in it. A heading is a break between groups, and a symbol on it would be
     * a second thing saying what the words underneath already say - the icons on this screen
     * are what each row is about, and a group has no single answer to that.
     */
    int x = fb_margin(state);
    if (leading.kind != FB_LEADING_NONE) {
        x += fb_icon_box(state, state->scale) + fb_char_adv(state, state->scale) / 2;
    }
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", text != NULL ? text : "");
    mesh_ui_line_fit(&line, fb_cols(state, scale));
    /* Quiet on the ground and quiet on the fill alike: a heading names the group under it, and
       it is not one of the rows the cursor came here to read. */
    fb_draw_text(state, x, baseline, mesh_ui_line_text(&line), scale,
                 selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                          : fb_tone_color(state, MESH_UI_TONE_DIM),
                 ground);
    list->y += (int)rows * list->line;
}

/* ---- the note row -------------------------------------------------------------------------- */

/* The width a note's body wraps to. The list's own columns: a paragraph indented past the rows
   around it would be a second left margin on a panel that has room for one. */
static size_t fb_note_cols(const struct mesh_ui_backend_fb_state *state) {
    return fb_cols(state, state->scale);
}

uint32_t fb_list_note_steps(const struct mesh_ui_backend_fb_state *state, const char *heading,
                            const char *body) {
    if (state == NULL) {
        return 1U;
    }
    /* The heading costs a step even though it is drawn small, because a step is a body row and
       the list model counts in those. The air the smaller glyphs free goes above it, exactly as
       it does on a subheader. */
    uint32_t steps = (heading != NULL && heading[0] != '\0') ? 1U : 0U;
    steps += mesh_ui_wrap_lines(body != NULL ? body : "", fb_note_cols(state));
    /* A note with nothing in it is still a row: a zero-height row would put every row under it
       at the wrong offset, which is the failure the whole heights mechanism exists to prevent. */
    return steps > 0U ? steps : 1U;
}

void fb_list_note(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                  uint32_t index, const char *heading, const char *body) {
    fb_list_rail(state, list);
    const uint32_t rows = fb_list_row_height(list, index);
    const bool selected = mesh_ui_list_is_cursor(&list->model, index);
    /* One fill for the whole note, the height the *model* gave it - not the height its words
       want. The two agree when the screen measured with fb_list_note_steps(), and when they do
       not it is the model that is right, because it is what every row below was placed against. */
    const struct mesh_ui_rgb ground = fb_draw_row_fill(state, list->y, rows, selected);

    const int margin = fb_margin(state);
    const int body_line = fb_line_adv(state, state->scale);
    const bool titled = heading != NULL && heading[0] != '\0';
    int y = list->y;

    if (titled) {
        const int scale = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
        /* Sat on the bottom of its step for the reason a subheader is: the gap the small glyphs
           leave belongs above the heading, separating it from the paragraph that ended. */
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", heading);
        mesh_ui_line_fit(&line, fb_cols(state, scale));
        /*
         * The ink is chosen with the fill rather than beside it. A heading painted in the
         * primary whether or not the row was selected is a pair no theme was measured against -
         * the accent over the selection fill is the one combination the contrast contract does
         * not cover, because on a light palette they are two shades of the same hue.
         */
        fb_draw_text(state, margin, y + body_line - fb_line_adv(state, scale),
                     mesh_ui_line_text(&line), scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                              : fb_tone_color(state, MESH_UI_TONE_PRIMARY),
                     ground);
        y += body_line;
    }

    /*
     * The paragraph, one wrapped line per step.
     *
     * Clipped to the row's own height rather than to the panel: a note the model was told is
     * three steps tall draws three lines and stops, so a measure that disagreed with the words
     * loses the tail of a sentence instead of painting it over the next note. Losing text is
     * visible; overlapping it is not, which is the trade this file makes everywhere.
     */
    uint32_t drawn = titled ? 1U : 0U;
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, body != NULL ? body : "", fb_note_cols(state));
    while (drawn < rows && mesh_ui_wrap_next(&wrap)) {
        fb_draw_text(state, margin, y, wrap.line, state->scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                              : fb_tone_color(state, MESH_UI_TONE_NORMAL),
                     ground);
        y += body_line;
        drawn++;
    }

    list->y += (int)rows * list->line;
}

void fb_list_row_line(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                      uint32_t index, struct mesh_ui_line *line, enum mesh_ui_tone tone) {
    mesh_ui_line_fit(line, list->cols);
    fb_list_row(state, list, index, mesh_ui_line_text(line), tone);
}

/* ---- the conversation cell ----------------------------------------------------------------- */

/*
 * The disc and what is in it: a node's initials, or an icon for the rows that are not a person.
 * `size` is both its width and its height, so the radius is half of it and the shape is a
 * circle.
 *
 * What it holds is drawn at the largest multiplier that *fits inside the disc*, which is not
 * always the body's. A two-row item gives the disc two lines to be round in and the body scale
 * fits with room; a one-row item - a node, a picker row - gives it one, and two cells at the
 * body scale then overhang a circle barely taller than a single glyph. That drew initials
 * sliced off at both ends, which is worse than no disc at all: the whole job of the colour and
 * the two letters is to be recognised without being read.
 *
 * So the fit is measured rather than assumed. It is done here, once, because the caller cannot
 * answer it - which scale fits is a fact about this component's geometry, and a screen that had
 * to work it out would be computing a glyph size, which is the thing screens do not do. An icon
 * is measured by the same loop: it is one cell wide and drawn at the glyph body's height, which
 * is the taller of the two the loop tests.
 *
 * An icon takes the same ink the initials do - the ground colour, which every avatar tint is
 * validated against - and is blended over the tint it is standing on.
 */
static void fb_draw_avatar(const struct mesh_ui_backend_fb_state *state, int x, int y, int size,
                           const char *label, enum mesh_ui_icon icon, struct mesh_ui_rgb tint) {
    fb_fill_round_rect(state, x, y, size, size, size / 2, tint);
    const bool has_icon = mesh_ui_icon_is_valid(icon);
    if (!has_icon && (label == NULL || label[0] == '\0')) {
        return;
    }
    /*
     * The inset a circle owes its contents: at the corners of the text box the disc has already
     * curved away, so text measured against the full diameter still touches the edge. An eighth
     * on each side is what keeps two cells clear of it at every scale the theme allows.
     */
    const int room = size - size / 4;
    const size_t cells = has_icon ? 1U : mesh_ui_text_cells(label);
    int scale = state->scale;
    while (scale > 1 && ((int)cells * fb_char_adv(state, scale) > room ||
                         (int)fb_font(state)->height * scale > room)) {
        --scale;
    }

    /* Centred in cells, and vertically on the glyph body rather than the line advance - the
       advance carries the gap accents hang in, and counting it sits the initials low in the
       disc. The same reasoning as fb_draw_button's label. */
    const int text_h = (int)fb_font(state)->height * scale;
    const int content_y = y + (size - text_h) / 2;
    if (has_icon) {
        fb_draw_icon(state, x + (size - fb_icon_box(state, scale)) / 2, content_y, icon, scale,
                     fb_color(state, MESH_UI_COLOR_BG), tint);
        return;
    }
    const int text_w = (int)cells * fb_char_adv(state, scale);
    fb_draw_text(state, x + (size - text_w) / 2, content_y, label, scale,
                 fb_color(state, MESH_UI_COLOR_BG), tint);
}

/* ---- the list item ------------------------------------------------------------------------ */

/*
 * An item's geometry, all of it derived once so the fill, the text and the slots cannot
 * disagree about where the row is. Every screen used to re-derive some part of this, and the
 * parts that drifted were exactly the ones nothing else could see: how far a trailing control
 * ate into the text, and which box a control centred itself on.
 */
struct fb_item_geom {
    uint32_t rows;
    int fill_top, fill_h; /* the box the cursor fill paints, and a control centres on */
    int head_y;           /* headline baseline */
    int supp_y;           /* supporting baseline; only meaningful on a two-row item */
    int slot_h;           /* height of a box-shaped trailing - a badge */
    int head_slot_top, supp_slot_top;
    int text_x, text_right;
    int marker_x;     /* a plain row's marker cell; only meaningful when the row reserved one */
    size_t cols;      /* text columns between the leading slot and the trailing edge */
    int bar_y, bar_h; /* a stacked meter's track; bar_h of 0 is a row that has none */
};

static struct fb_item_geom fb_item_measure(const struct mesh_ui_backend_fb_state *state,
                                           const struct fb_list *list,
                                           const struct fb_list_item *item, uint32_t rows) {
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const int margin = fb_margin(state);
    struct fb_item_geom g;
    memset(&g, 0, sizeof g);

    /*
     * The height is the *list's* answer, not this item's.
     *
     * It used to be read off the item - two rows if it had a supporting line, one otherwise -
     * which was a second opinion about something the window had already decided, and the two
     * were only ever equal because every list happened to be uniform. Once a list can mix
     * heights the model is the one that has to be right: it is what placed the window, what
     * the cursor's highlight is measured against and what the scroll thumb reports. So a row
     * that draws taller than the list was told simply cannot happen from here.
     */
    g.rows = rows > 0U ? rows : 1U;
    g.head_y = list->y;
    g.text_right = (int)state->var.xres - margin;
    g.slot_h = list->line - scale;

    if (g.rows >= 2U) {
        /* Two rows set closer together than two items are: the supporting line sits a scale
           above where a second row would put it, and the space that frees becomes the gap to
           the next item. */
        g.supp_y = list->y + list->line - scale;
        g.fill_top = g.head_y - fb_space(state, MESH_UI_SPACE_XS);
        g.fill_h = (int)g.rows * list->line - fb_space(state, MESH_UI_SPACE_MD);
        g.head_slot_top = g.fill_top;
        g.supp_slot_top = g.supp_y - fb_space(state, MESH_UI_SPACE_XS);
    } else {
        g.fill_top = g.head_y - scale;
        g.fill_h = list->line;
        g.head_slot_top = g.fill_top;
        g.supp_slot_top = g.fill_top;
    }

    g.text_x = margin;
    if (item->leading.kind == FB_LEADING_AVATAR) {
        g.text_x = margin + (g.fill_h - scale) + adv / 2;
    } else if (item->leading.kind == FB_LEADING_ICON) {
        /* Reserved whether or not this row filled it, so every row's words start in the same
           column - a list that indents only the rows with something to say is a list the eye
           cannot run down. */
        g.text_x = margin + fb_icon_box(state, scale) + adv / 2;
    }
    /* The plain row's marker gutter, between whatever the leading slot put down and the words.
       Reserved for the whole list rather than for the rows that filled it, on the same terms as
       the leading slot above - and only where there is no label column, which measures its own. */
    g.marker_x = g.text_x;
    if (item->label_cols == 0U && item->marker_slot) {
        g.text_x += fb_icon_box(state, scale) + adv / 2;
    }
    g.cols = g.text_right > g.text_x ? (size_t)((g.text_right - g.text_x) / adv) : 1U;

    /*
     * A stacked meter's track, and the one thing on a two-step item that the fill has to be
     * grown for.
     *
     * The supporting line's box overhangs the fill by design - a glyph's ink sits high in its
     * cell, so text stays comfortably inside a fill that stops short of the box, and the space
     * that leaves is the gap between one item and the next. A bar has no such slack: its ink is
     * the whole of its box, so a fill measured for text left the cursor's highlight ending a few
     * pixels above the bar it was meant to be under. The fill therefore takes the bar in, plus
     * the same breathing room it has at the top.
     */
    if ((item->meter != NULL || item->slider != NULL) && g.rows >= 2U) {
        /* One answer for both of the things that can be in that step, because the step is one
           step: a screen that swapped a reading for a control on the same row must not find the
           row a few pixels shorter. The slider is the taller of the two - it reserves the room
           its handle stands up into under the cursor - so it is what the step is measured by
           wherever it is the one present. */
        g.bar_h = item->slider != NULL ? fb_slider_height(state, scale)
                                       : fb_meter_thickness(state, scale);
        /* On the supporting line's geometry: a second line set closer to its headline than two
           rows would be, which is what keeps the bar reading as part of the row above it rather
           than as something floating between two rows. */
        g.bar_y = g.supp_y + ((int)fb_font(state)->height * scale - g.bar_h) / 2;
        const int bottom = g.bar_y + g.bar_h + fb_space(state, MESH_UI_SPACE_XS);
        if (bottom - g.fill_top > g.fill_h) {
            g.fill_h = bottom - g.fill_top;
        }
    }
    return g;
}

/*
 * How wide an inline meter is, in cells.
 *
 * A length only means something against the container it is in, so the container has to be long
 * enough for the eye to divide: at four cells a half-full bar and a two-thirds-full one are the
 * same picture. Eight is about where they part company at this glyph scale, and it is still
 * short enough to leave a settings row its label and its value - which is the whole reason the
 * inline meter is a trailing slot and not a band across the row.
 */
#define FB_METER_INLINE_CELLS 8U

/*
 * Cells a trailing slot takes out of a line `cols` wide, its breathing room included.
 *
 * Zero has one meaning and it covers both ways a slot can come to nothing: there is nothing in
 * it, or the line is too narrow to give it its room. Either way the slot is not drawn and the
 * line keeps every column it has - because a trailing figure is worth less than the row it
 * would be laid across, and a row clipped to one cell has lost the thing it was about.
 *
 * The point of one function answering that is that *measuring and drawing ask it once*. A slot
 * squeezed out of the line by one calculation and then painted over that line by another is
 * exactly the bug this component exists to make unwritable - it is what a narrow panel or a
 * large glyph scale used to turn an age into, drawn back across the avatar.
 *
 * `reserved` is what the row has already promised to something else - the label column and its
 * marker gutter, or a plain row's marker cell. A slot is fitted against what is *free*, not
 * against the whole line: the headline is clipped from its tail, so a slot measured against the
 * line ate the value first and then the label, and a label column is the one thing on a settings
 * row that may not move. The wide slots are why this arrived - a switch is four cells and never
 * reached it, a segmented button is most of a value column and reaches it at every scale.
 */
/*
 * What the segmented slot takes out of a line `cols` wide, and which of its two forms it takes.
 *
 * The one slot with a second form, so it is the one slot that needs a function of its own: every
 * other kind either fits or is dropped, and a set of choices always has the chosen one in words
 * to fall back on. `out_as_text` comes back true when the control could not have its room -
 * which is a value column too narrow for the segments, not a caller that forgot to fill one in.
 *
 * Measuring and drawing both ask *this*, for the reason they both ask fb_trailing_cols(): a slot
 * sized by one rule and painted by another is what turned a trailing age into a smear across an
 * avatar, and a control that decided its own form twice would do it again.
 */
/*
 * The glyph multiplier a row's segments are set at: the label role, not the row's own.
 *
 * Which is what Material asks for and, more to the point here, what makes the component reach
 * the settings it exists for. A segmented button spends its width `count` times over, so a
 * three-valued setting at body scale wants most of a panel - "Random PIN / Fixed PIN / No PIN"
 * does not fit a value column at any scale this ships with, and would have fallen back to the
 * word it was meant to replace on every theme. It is also the right answer on its own terms: a
 * control's label is chrome, which is the type role the action bar's verbs and the navigation
 * bar's tabs already take.
 */
static int fb_segmented_scale(const struct mesh_ui_backend_fb_state *state) {
    return fb_type_scale(state, MESH_UI_TYPE_LABEL);
}

/* The chosen word instead of the control, on the terms an ordinary trailing text takes. Both
   ways of ending up there - no room, and no drawable choice - go through this, so a slot that
   fell back for one reason is measured exactly like a slot that fell back for the other. */
static size_t fb_segmented_as_text(const struct mesh_ui_backend_fb_state *state, size_t cols,
                                   const struct fb_segmented *segmented, bool *out_as_text) {
    (void)state;
    if (out_as_text != NULL) {
        *out_as_text = true;
    }
    if (segmented == NULL) {
        return 0U;
    }
    const size_t cells = mesh_ui_text_cells(segmented->value);
    return (cells > 0U && cols > cells + 1U) ? cells + 1U : 0U;
}

static size_t fb_segmented_cols(const struct mesh_ui_backend_fb_state *state, size_t cols,
                                const struct fb_segmented *segmented, bool *out_as_text) {
    if (out_as_text != NULL) {
        *out_as_text = true;
    }
    if (segmented == NULL || segmented->count == 0U || segmented->count > FB_SEGMENTED_MAX ||
        segmented->active >= segmented->count) {
        /*
         * A choice outside the set is a choice this control cannot draw, and it is a state the
         * radio can genuinely be in: an enum value from a newer firmware, or a corrupt one. The
         * item still carries it and still formats it - "Unknown" - so the words are the honest
         * answer and they are already here. Highlighting the first segment instead would have
         * the panel state that pairing is set to Random PIN when nobody knows what it is set to,
         * which is the one thing a picture is not allowed to do quietly.
         *
         * Not "every segment unlit" either: a set with nothing chosen says *none of these*,
         * which is a different false claim.
         */
        return fb_segmented_as_text(state, cols, segmented, out_as_text);
    }
    const int adv = fb_char_adv(state, state->scale);
    const int width = fb_segmented_width(state, segmented, fb_segmented_scale(state));
    /*
     * Counted in the *row's* cells whatever the segments are set at, because what it is being
     * fitted into is a line of the row's own text.
     *
     * And with no cell of air added, unlike every other slot here. The others sit next to the
     * row's value and the extra cell is the gap to it; this one *is* the value - the words it
     * replaces are inside it - so the only thing left of the line is the label column, which the
     * caller has already reserved. The gap is then exactly the cell the strictly-greater test
     * below keeps back, and adding a second one cost the three-valued settings the control at
     * the shipping scale by a single column.
     */
    const size_t want = (size_t)((width + adv - 1) / adv);
    if (width > 0 && cols > want) {
        if (out_as_text != NULL) {
            *out_as_text = false;
        }
        return want;
    }
    return fb_segmented_as_text(state, cols, segmented, out_as_text);
}

static size_t fb_trailing_cols(const struct mesh_ui_backend_fb_state *state, size_t cols,
                               size_t reserved, const struct fb_trailing *trailing) {
    const int adv = fb_char_adv(state, state->scale);
    size_t want = 0U;
    switch (trailing->kind) {
    case FB_TRAILING_TEXT: {
        const size_t cells = mesh_ui_text_cells(trailing->text);
        want = cells > 0U ? cells + 1U : 0U;
        break;
    }
    case FB_TRAILING_BADGE: {
        const size_t cells = mesh_ui_text_cells(trailing->text);
        want = cells > 0U ? cells + 2U : 0U;
        break;
    }
    case FB_TRAILING_SWITCH: {
        int width = 0;
        fb_switch_size(state, state->scale, &width, NULL);
        want = (size_t)((width + adv - 1) / adv) + 2U;
        break;
    }
    case FB_TRAILING_ICON:
        want = mesh_ui_icon_is_valid(trailing->icon) ? 2U : 0U;
        break;
    case FB_TRAILING_METER:
        /* Stated in cells rather than measured from anything, because unlike a switch a bar has
           no natural width - it is as long as it is given. FB_METER_INLINE_CELLS is that
           choice, and the extra cell is the gap to the words. */
        want = trailing->meter != NULL ? FB_METER_INLINE_CELLS + 1U : 0U;
        break;
    case FB_TRAILING_SIGNAL: {
        /* The staircase, its gap to whatever is left of it, and the figure it carries - which
           may be nothing, and then costs nothing. Stated the same way the meter's width is, and
           for the same reason: rungs have no natural width either. */
        const size_t cells = mesh_ui_text_cells(trailing->text);
        want = FB_SIGNAL_CELLS + 1U + (cells > 0U ? cells + 1U : 0U);
        break;
    }
    case FB_TRAILING_SPARK:
        /* The line and the cell of air between it and the words, stated the way the two above
           are. A trend with nothing in it costs nothing: the slot is not reserved against a
           second reading arriving, because a row whose value column was short by six cells
           until the radio repeated itself would reflow while being read. */
        want = (trailing->spark != NULL && trailing->spark->points != NULL &&
                trailing->spark->points->count >= 2U)
                   ? FB_SPARK_CELLS + 1U
                   : 0U;
        break;
    case FB_TRAILING_CHECKBOX:
    case FB_TRAILING_RADIO: {
        int width = 0;
        fb_selection_size(state, state->scale, &width, NULL);
        want = (size_t)((width + adv - 1) / adv) + 2U;
        break;
    }
    case FB_TRAILING_SEGMENTED:
        return fb_segmented_cols(state, cols > reserved ? cols - reserved : 0U, trailing->segmented,
                                 NULL);
    case FB_TRAILING_NONE:
    default:
        return 0U;
    }
    /* Strictly greater: the line keeps at least one cell of its own, which is the test the
       conversation cell made for its age before this was a shared slot. */
    return (want > 0U && cols > want + reserved) ? want : 0U;
}

/* `reserved` is the same figure fb_trailing_cols() was given - see there. Only the slot with
   two forms reads it, and it has to: a segmented button that measured itself against the free
   room and then drew itself against the whole line would be the one kind able to disagree with
   the measure that placed it. */
static void fb_draw_trailing(struct mesh_ui_backend_fb_state *state, const struct fb_item_geom *g,
                             size_t reserved, const struct fb_trailing *trailing, int baseline,
                             int slot_top, bool selected, struct mesh_ui_rgb ground) {
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const size_t cells =
        trailing->kind == FB_TRAILING_TEXT ? mesh_ui_text_cells(trailing->text) : 0U;

    switch (trailing->kind) {
    case FB_TRAILING_TEXT:
        if (cells == 0U) {
            return;
        }
        /* Always the quiet ink, on the ground and on the fill alike: a trailing figure is
           something the eye glances at on its way past, never the row's own words. */
        fb_draw_text(state, g->text_right - (int)cells * adv, baseline, trailing->text, scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                              : fb_tone_color(state, MESH_UI_TONE_DIM),
                     ground);
        return;
    case FB_TRAILING_BADGE: {
        /* The capsule is fb_draw_badge()'s, not this slot's: the top app bar's trailing slot
           draws the same thing, and two places filling their own round rect is two capsules
           that drift. What the row supplies is the box - a row knows where its own slot is. */
        const int width = fb_badge_width(state, trailing->text, scale);
        const struct fb_rect box = {
            .x = g->text_right - width, .y = slot_top, .w = width, .h = g->slot_h};
        fb_draw_badge(state, &box, baseline, trailing->text, trailing->family, scale);
        return;
    }
    case FB_TRAILING_ICON:
        /* The quiet ink a trailing figure takes, for the same reason: a chevron is something the
           eye passes on its way down the list, never one of the row's own words. */
        fb_draw_icon(state, g->text_right - fb_icon_box(state, scale), baseline, trailing->icon,
                     scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                              : fb_tone_color(state, MESH_UI_TONE_DIM),
                     fb_color(state, selected ? MESH_UI_COLOR_SURFACE_SEL : MESH_UI_COLOR_BG));
        return;
    case FB_TRAILING_SWITCH: {
        if (trailing->sw == NULL) {
            return;
        }
        int width = 0;
        int height = 0;
        fb_switch_size(state, scale, &width, &height);
        /*
         * Centred on the row's *fill* rather than on the glyph body. The two coincide for the
         * font that ships, but only the fill is what the control has to stay inside, and a
         * switch that overhangs it notches the highlight on the one row the cursor is on.
         */
        trailing->sw->rect.w = width;
        trailing->sw->rect.h = height;
        trailing->sw->rect.x = g->text_right - width;
        trailing->sw->rect.y = g->fill_top + (g->fill_h - height) / 2;
        trailing->sw->selected = selected;
        fb_draw_switch(state, trailing->sw);
        return;
    }
    case FB_TRAILING_CHECKBOX:
    case FB_TRAILING_RADIO: {
        if (trailing->sel == NULL) {
            return;
        }
        int width = 0;
        int height = 0;
        fb_selection_size(state, scale, &width, &height);
        /* Centred on the row's *fill* rather than on the glyph body, for the reason the switch
           is: only the fill is what the control has to stay inside, and one that overhangs it
           notches the highlight on the one row the cursor is on. */
        trailing->sel->rect.w = width;
        trailing->sel->rect.h = height;
        trailing->sel->rect.x = g->text_right - width;
        trailing->sel->rect.y = g->fill_top + (g->fill_h - height) / 2;
        trailing->sel->selected = selected;
        /* Set from the kind, so a caller cannot name a radio and be handed a checkbox. There is
           one statement about which of the two this is and it is the slot's. */
        trailing->sel->shape =
            trailing->kind == FB_TRAILING_RADIO ? FB_SELECTION_RADIO : FB_SELECTION_CHECKBOX;
        fb_draw_selection(state, trailing->sel);
        return;
    }
    case FB_TRAILING_SEGMENTED: {
        bool as_text = true;
        const size_t want = fb_segmented_cols(state, g->cols > reserved ? g->cols - reserved : 0U,
                                              trailing->segmented, &as_text);
        if (want == 0U) {
            return;
        }
        if (as_text) {
            /* The chosen word, drawn exactly as a trailing text is - because that is what it
               now is. Quiet ink on the ground and on the fill alike. */
            fb_draw_text(state,
                         g->text_right - (int)mesh_ui_text_cells(trailing->segmented->value) * adv,
                         baseline, trailing->segmented->value, scale,
                         selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                                  : fb_tone_color(state, MESH_UI_TONE_DIM),
                         ground);
            return;
        }
        const int seg_scale = fb_segmented_scale(state);
        const int width = fb_segmented_width(state, trailing->segmented, seg_scale);
        /* The switch's height at the *row's* scale, not the segments': two controls in one
           column have to stand the same distance off their rows, and it is the labels that are
           chrome-sized, not the control. */
        const int height = fb_segmented_height(state, scale);
        const struct fb_rect box = {.x = g->text_right - width,
                                    .y = g->fill_top + (g->fill_h - height) / 2,
                                    .w = width,
                                    .h = height};
        fb_draw_segmented(state, &box, trailing->segmented, selected, MESH_UI_COLOR_BG, seg_scale);
        return;
    }
    case FB_TRAILING_METER: {
        if (trailing->meter == NULL) {
            return;
        }
        /* Centred on the row's fill, exactly as the switch is and for the same reason: the fill
           is what the control has to stay inside. */
        const int height = fb_meter_thickness(state, scale);
        trailing->meter->rect.w = (int)FB_METER_INLINE_CELLS * adv;
        trailing->meter->rect.h = height;
        trailing->meter->rect.x = g->text_right - trailing->meter->rect.w;
        trailing->meter->rect.y = g->fill_top + (g->fill_h - height) / 2;
        trailing->meter->selected = selected;
        fb_draw_meter(state, trailing->meter);
        return;
    }
    case FB_TRAILING_SIGNAL: {
        /*
         * The rungs against the trailing edge and the figure to their left, which is the order
         * a status bar puts the two in - the signal is the thing being scanned down the column,
         * so it is the thing that keeps the fixed edge.
         *
         * Both take the row's quiet pairing, exactly as a trailing age does, and the lit rungs
         * take the row's own ink. That is deliberately the same two colours the slot already
         * draws everything else in: a staircase is furniture the eye passes on its way down a
         * list, not one of the row's words, and giving quality a colour of its own would put a
         * third statement about the link on a row that has made two.
         */
        const struct mesh_ui_rgb quiet = selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                                                  : fb_tone_color(state, MESH_UI_TONE_DIM);
        const struct mesh_ui_rgb ink =
            fb_color(state, selected ? MESH_UI_COLOR_TEXT_ON_SEL : MESH_UI_COLOR_TEXT);
        /*
         * An unlit rung is the meter's track and not the dim text colour, which is what it was
         * first drawn as. The two are different jobs: dim text is held *above* the ground so it
         * stays readable, and a rung that is not lit has nothing to read - it is there to be
         * counted against the lit ones, so it wants the role the theme already validates as the
         * empty part of an indicator. As dim text the gap between two rungs and four was there
         * but had to be looked for.
         *
         * Except under the cursor, where the track is not a colour that can be relied on: two of
         * the four themes make it exactly the cursor fill, which is the reason fb_draw_meter()
         * lays a ground of its own. Rungs have gaps between them and no ground to lay, so they
         * take the pairing the cursor does validate.
         */
        const struct mesh_ui_rgb unlit =
            selected ? quiet : fb_color(state, MESH_UI_COLOR_METER_TRACK);
        const int width = (int)FB_SIGNAL_CELLS * adv;
        const int height = fb_icon_box(state, scale);
        const struct fb_rect box = {
            .x = g->text_right - width,
            /* Centred on the row's fill, as the switch and the meter are, so a list of them
               sits on one line however the glyph body and the fill differ. */
            .y = g->fill_top + (g->fill_h - height) / 2,
            .w = width,
            .h = height,
        };
        fb_draw_signal(state, &box, trailing->signal, ink, unlit);
        const size_t figure = mesh_ui_text_cells(trailing->text);
        if (figure > 0U) {
            fb_draw_text(state, box.x - adv - (int)figure * adv, baseline, trailing->text, scale,
                         quiet, ground);
        }
        return;
    }
    case FB_TRAILING_SPARK: {
        if (trailing->spark == NULL) {
            return;
        }
        /*
         * On the *line's* slot rather than centred on the row's fill, which is where the switch,
         * the meter and the staircase sit.
         *
         * The difference only shows on the row this was built for and it shows badly: the node
         * detail's battery row is two steps, a fact on the first and a banded bar across the
         * second, so a trend centred on the fill lands in the gap between them and draws its
         * floor through the bar. Every other slot in this column is on a row whose fill is one
         * step, which is why the fill and the line were the same box until now. The slot is the
         * box that means "beside these words", and beside the words is where this belongs.
         */
        const int height = g->slot_h > 0 ? g->slot_h : fb_sparkline_height(state, scale);
        trailing->spark->rect.w = (int)FB_SPARK_CELLS * adv;
        trailing->spark->rect.h = height;
        trailing->spark->rect.x = g->text_right - trailing->spark->rect.w;
        trailing->spark->rect.y = slot_top;
        trailing->spark->selected = selected;
        fb_draw_sparkline(state, trailing->spark);
        return;
    }
    case FB_TRAILING_NONE:
    default:
        return;
    }
}

/* Cells between the end of the label column and the start of the value: a space, the marker's
   own cell, a space. Reserved on every row of a list whether or not that row has a marker in
   it, which is what keeps the values in one column. */
#define FB_ITEM_MARKER_CELLS 3U

/* The headline, as one line: either the whole row, or a label column with the marker gutter and
   a value after it. The gutter is left blank here and the icon is drawn into it afterwards. */
static void fb_item_headline(struct mesh_ui_line *line, const struct fb_list_item *item) {
    mesh_ui_line_reset(line);
    if (item->label_cols > 0U) {
        /* The label occupies its column exactly - clipped when long, padded when short -
           measured in cells so a value column still lines up under a label that is not all
           ASCII. */
        mesh_ui_line_column(line, item->label != NULL ? item->label : "", item->label_cols);
        mesh_ui_line_pad_to(line, item->label_cols + FB_ITEM_MARKER_CELLS);
        mesh_ui_line_printf(line, "%s", item->value != NULL ? item->value : "");
        return;
    }
    mesh_ui_line_printf(line, "%s", item->text != NULL ? item->text : "");
}

void fb_list_item(struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                  const struct fb_list_item *item) {
    fb_list_rail(state, list);
    const int scale = state->scale;
    const bool selected = mesh_ui_list_is_cursor(&list->model, index);
    const uint32_t rows = fb_list_row_height(list, index);
    const struct fb_item_geom g = fb_item_measure(state, list, item, rows);

    if (selected) {
        const int radius = fb_radius(state, MESH_UI_SHAPE_SM);
        const int row_x = fb_margin(state) / 2;
        const int row_w = (int)state->var.xres - fb_margin(state);
        if (item->accent_edge) {
            /*
             * The bar is laid the way a card's edge is: the marker shape first, the fill over
             * it a scale narrower on the left only. Both share their right edge, so the marker
             * survives just where the bar is meant to be - and it follows the corner instead
             * of poking a square end out of it, which is what a straight bar does once the row
             * has ends.
             */
            const enum mesh_ui_family edge_family = mesh_ui_tone_family(item->tone);
            fb_fill_round_rect(state, row_x, g.fill_top, row_w, g.fill_h, radius,
                               fb_tone_color(state, edge_family != MESH_UI_FAMILY_COUNT
                                                        ? item->tone
                                                        : MESH_UI_TONE_PRIMARY));
            fb_fill_round_rect(state, row_x + scale, g.fill_top, row_w - scale, g.fill_h, radius,
                               fb_color(state, MESH_UI_COLOR_SURFACE_SEL));
        } else {
            fb_fill_round_rect(state, row_x, g.fill_top, row_w, g.fill_h, radius,
                               fb_color(state, MESH_UI_COLOR_SURFACE_SEL));
        }
    }

    /*
     * Under the cursor everything is drawn against that fill instead of against the ground,
     * which is a different pair of colours and not a dimmer version of the same one.
     */
    const struct mesh_ui_rgb head_ink =
        selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL) : fb_tone_color(state, item->tone);
    /* What every icon on this row is blended against: the fill if the cursor laid one down, the
       ground otherwise. The row is the only thing that knows. */
    const struct mesh_ui_rgb ground =
        fb_color(state, selected ? MESH_UI_COLOR_SURFACE_SEL : MESH_UI_COLOR_BG);

    if (item->leading.kind == FB_LEADING_AVATAR) {
        const int size = g.fill_h - scale;
        const struct mesh_ui_rgb tint =
            item->leading.role < MESH_UI_COLOR_COUNT
                ? fb_color(state, item->leading.role)
                : mesh_ui_theme_avatar(state->theme, item->leading.tint);
        fb_draw_avatar(state, fb_margin(state), g.fill_top + fb_space(state, MESH_UI_SPACE_XS),
                       size, item->leading.label, item->leading.icon, tint);
    } else if (item->leading.kind == FB_LEADING_ICON) {
        fb_draw_icon(state, fb_margin(state), g.head_y, item->leading.icon, scale, head_ink,
                     ground);
    }

    struct mesh_ui_line line;
    fb_item_headline(&line, item);
    /*
     * What the headline has already spent *inside `g.cols`* before its trailing slot gets a say.
     *
     * Only the label column, and that is the whole of the subtlety. A plain row's marker gutter
     * is spent too, but it is spent by fb_item_measure() moving `g.text_x` past it before the
     * columns are counted - so it is already outside this number, and reserving it again took a
     * cell off every row with a marker slot. The label column is the other way round: it lives
     * inside the line fb_item_headline() builds, so nothing has counted it yet.
     */
    const size_t reserved = item->label_cols > 0U ? item->label_cols + FB_ITEM_MARKER_CELLS : 0U;
    const size_t head_take = fb_trailing_cols(state, g.cols, reserved, &item->trailing);
    mesh_ui_line_fit(&line, g.cols - head_take);
    fb_draw_text(state, g.text_x, g.head_y, mesh_ui_line_text(&line), scale, head_ink, ground);
    /* Into the blank cell fb_item_headline() left between the label column and the value, and
       only when the value column actually got that far - a label column wider than the row is
       clipped, and a marker drawn at a column the line no longer reaches would sit on top of
       the label. */
    if (item->label_cols > 0U && mesh_ui_icon_is_valid(item->marker_icon) &&
        g.cols > item->label_cols + FB_ITEM_MARKER_CELLS) {
        fb_draw_icon(state, g.text_x + (int)(item->label_cols + 1U) * fb_char_adv(state, scale),
                     g.head_y, item->marker_icon, scale, head_ink, ground);
    } else if (item->label_cols == 0U && item->marker_slot &&
               mesh_ui_icon_is_valid(item->marker_icon)) {
        /* Into the cell the measure held back before the words. In the row's own ink, like every
           other icon in a row's slots: the star is as loud as the name it sits beside. */
        fb_draw_icon(state, g.marker_x, g.head_y, item->marker_icon, scale, head_ink, ground);
    }
    if (head_take > 0U) {
        fb_draw_trailing(state, &g, reserved, &item->trailing, g.head_y, g.head_slot_top, selected,
                         ground);
    }

    if (g.rows >= 2U && item->supporting != NULL) {
        const struct mesh_ui_rgb supp_ink =
            selected ? fb_color(state, item->supporting_quiet ? MESH_UI_COLOR_TEXT_ON_SEL_DIM
                                                              : MESH_UI_COLOR_TEXT_ON_SEL)
                     : fb_tone_color(state, item->supporting_tone);
        /* Nothing reserved: a supporting line has no label column, and the icon that may take
           its first cell is tested against what the slot leaves rather than before it. */
        const size_t supp_take = fb_trailing_cols(state, g.cols, 0U, &item->supporting_trailing);
        /* An icon on the supporting line takes the first cell and the words move over, which is
           what "> " did when it was two characters of the preview. */
        const bool supp_icon =
            mesh_ui_icon_is_valid(item->supporting_icon) && g.cols > supp_take + 1U;
        const int supp_x = g.text_x + (supp_icon ? fb_char_adv(state, scale) : 0);
        if (supp_icon) {
            fb_draw_icon(state, g.text_x, g.supp_y, item->supporting_icon, scale, supp_ink, ground);
        }
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", item->supporting);
        mesh_ui_line_fit(&line, g.cols - supp_take - (supp_icon ? 1U : 0U));
        fb_draw_text(state, supp_x, g.supp_y, mesh_ui_line_text(&line), scale, supp_ink, ground);
        if (supp_take > 0U) {
            fb_draw_trailing(state, &g, 0U, &item->supporting_trailing, g.supp_y, g.supp_slot_top,
                             selected, ground);
        }
    }

    /*
     * The bar the row gave a step to, across the width the words had.
     *
     * Only when the list actually gave it that step. A screen that declared a meter row one
     * step tall has made a mistake, and the two ways of answering it are to draw the bar over
     * whatever is under this row or to leave it out; leaving it out is the one a screen notices
     * and the one that cannot corrupt the frame. It is the same rule the trailing slots follow
     * when the line is too narrow for them.
     */
    if (item->meter != NULL && g.bar_h > 0) {
        item->meter->rect.x = g.text_x;
        item->meter->rect.w = g.text_right - g.text_x;
        item->meter->rect.h = g.bar_h;
        item->meter->rect.y = g.bar_y;
        item->meter->selected = selected;
        fb_draw_meter(state, item->meter);
    } else if (item->slider != NULL && g.bar_h > 0) {
        /* The same box the meter would have had, and the control centres its own track in it -
           so a reading and the control that sets it start and end in the same two columns, which
           is what lets a section mix the two without the eye finding two different lists. */
        item->slider->rect.x = g.text_x;
        item->slider->rect.w = g.text_right - g.text_x;
        item->slider->rect.h = g.bar_h;
        item->slider->rect.y = g.bar_y;
        item->slider->selected = selected;
        fb_draw_slider(state, item->slider);
    }

    /*
     * The divider goes in the gap the tightened leading opened up, inset to where the text
     * starts rather than run edge to edge: a leading disc already separates the items, and a
     * full-width rule under one reads as a box drawn around it.
     */
    const bool last = (index + 1U >= list->model.count) ||
                      (index + 1U >= list->model.first + list->model.visible);
    if (item->divider && !selected && !last) {
        fb_draw_rule(state, g.text_x, g.fill_top + g.fill_h + fb_space(state, MESH_UI_SPACE_XS),
                     g.text_right - g.text_x, scale, MESH_UI_COLOR_RULE);
    }

    list->y += (int)g.rows * list->line;
}

/*
 * A conversation is a two-line item with a disc on the front, a time on the first line and an
 * unread count on the second - which is the whole of what it is, now that the item can say
 * that. What is left here is the *translation*: which of a conversation's facts goes in which
 * slot, and which of them changes what the row says rather than how it looks.
 */
void fb_draw_conversation(struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                          uint32_t index, const struct fb_conversation *conversation) {
    fb_list_rail(state, list);
    /*
     * Unread, and allowed to say so.
     *
     * The two emphases a new message earns - the preview at full weight, and the row refusing
     * to stay quiet under the cursor - are exactly what a mute is asking this row to stop
     * doing, so they are decided together here rather than by the caller passing `unread`
     * false. The count itself is untouched: it goes on drawing, one family quieter.
     */
    const bool emphasis = conversation->unread && !conversation->muted;
    struct mesh_ui_line preview;
    mesh_ui_line_reset(&preview);
    if (conversation->armed) {
        /* The armed row says what the next press does, in place of the preview it would take
           away. Nothing else on screen changes, so the warning is on the row it is about - and
           it stays loud under the cursor, which a preview does not. */
        mesh_ui_line_printf(&preview, "%s", mesh_str(MESH_STR_MESSAGES_DELETE_ARMED));
    } else if (conversation->preview[0] != '\0') {
        mesh_ui_line_printf(&preview, "%s", conversation->preview);
    } else {
        mesh_ui_line_printf(&preview, "%s", mesh_str(MESH_STR_MESSAGES_NO_MESSAGES_YET));
    }

    const struct fb_list_item item = {
        .leading =
            {
                .kind = FB_LEADING_AVATAR,
                .label = conversation->avatar,
                /* A row armed to be deleted says so inside its own disc, which is already
                   wearing the bad tone: the question and the answer in one place. */
                .icon = conversation->armed ? MESH_UI_ICON_DELETE : conversation->avatar_icon,
                .tint = conversation->tint,
                .role = conversation->armed    ? MESH_UI_COLOR_ERROR
                        : conversation->accent ? MESH_UI_COLOR_PRIMARY
                                               : MESH_UI_COLOR_COUNT,
            },
        .text = conversation->name,
        .tone = conversation->name_tone,
        /* The bell with a stroke through it, in the gutter the pinned star uses. The slot is
           reserved on every row and not only on the muted ones, because a list that indents
           only some of its rows is a list whose names start in two columns. */
        .marker_icon = conversation->muted ? MESH_UI_ICON_MUTED : MESH_UI_ICON_NONE,
        .marker_slot = true,
        /* The age sits against the right edge, quietly, the way a messenger dates a
           conversation - a fact you glance at, not one you read. */
        .trailing = {.kind = FB_TRAILING_TEXT, .text = conversation->age},
        .supporting = mesh_ui_line_text(&preview),
        /* The reply arrow says the last word in the thread was ours, which is what tells you
           whether a quiet thread is waiting on you or on them. It was "> " until it was an
           icon slot, and it is dropped on an armed row - what that row says is the warning. */
        .supporting_icon = (conversation->preview_outbound && !conversation->armed)
                               ? MESH_UI_ICON_REPLY
                               : MESH_UI_ICON_NONE,
        /* An unread preview is the row's own words at full weight - never the name's tone,
           which says what kind of conversation this is rather than how much of it is new. The
           half that still reads once the badge has been marked away. */
        .supporting_tone = conversation->armed ? MESH_UI_TONE_ERROR
                           : emphasis          ? MESH_UI_TONE_STRONG
                                               : MESH_UI_TONE_DIM,
        .supporting_quiet = !conversation->armed && !emphasis,
        /* Secondary rather than the accent on a muted row - see struct fb_conversation. The
           capsule is still there and still counts; it just stops being the loudest thing on
           the screen, which is the whole of what the user asked for. */
        .supporting_trailing = {.kind = FB_TRAILING_BADGE,
                                .family = conversation->muted ? MESH_UI_FAMILY_SECONDARY
                                                              : MESH_UI_FAMILY_PRIMARY,
                                .text = conversation->badge},
        .accent_edge = true,
        .divider = true,
    };
    fb_list_item(state, list, index, &item);
}

/* ---- chat bubbles ------------------------------------------------------------------------- */

/* An icon stands in one cell, like a glyph, and one cell of air separates two parts of the
   trailing run. Both are counted by the measure and spent by the draw. */
#define FB_BUBBLE_ICON_CELLS 1U
#define FB_BUBBLE_META_GAP 1U

/* Reactions, padlock, clock, delivery mark: the run is never longer than its four slots. */
#define FB_BUBBLE_META_PARTS 4U

/* One drawable part of a trailing run: a text or an icon, never both. */
struct fb_bubble_part {
    const char *text; /* NULL when this part is an icon */
    enum mesh_ui_icon icon;
    size_t cells;
};

/*
 * What a bubble works out to at this width, computed once and handed to both the measure and
 * the draw. Everything is in cells except `rows`, which is body rows.
 */
struct fb_bubble_metrics {
    size_t cols;        /* inner content width */
    uint32_t lines;     /* wrapped text lines */
    uint32_t notes;     /* wrapped lines of the failure reason under it */
    size_t last;        /* the width of the last line drawn, which the run tucks onto */
    bool meta_own_line; /* the run did not fit on the last one */
    struct fb_bubble_part parts[FB_BUBBLE_META_PARTS];
    size_t part_count;
    size_t meta_cols;  /* what the parts and their gaps come to; 0 when there is no run */
    size_t quote_cols; /* the quote line's text, elided to fit; 0 when there is no quote */
    uint32_t rows;
};

/* The bar down a quote's left edge and the gap after it, in cells. Both live here so the
   measure's width and the draw's text origin cannot disagree about the indent. */
#define FB_BUBBLE_QUOTE_INDENT 2U

/* A bubble never spans the whole panel: the gutter down the other side is what says which end
   of the conversation it came from, so three quarters is a look rather than a limit. */
static size_t fb_bubble_max_cols(const struct mesh_ui_backend_fb_state *state,
                                 const struct fb_layout *layout) {
    const size_t pct = fb_metrics(state)->bubble_width_pct;
    const size_t max = layout->cols > 4U ? layout->cols * pct / 100U : layout->cols;
    return max > 0U ? max : 1U;
}

static bool fb_bubble_has(const char *text) { return text != NULL && text[0] != '\0'; }

/*
 * The fill and the ink a bubble is drawn in.
 *
 * Three cases, and each is a *container* rather than a colour of its own: ours is the secondary
 * family, one that failed is the error family, and one of theirs is the neutral raised tier -
 * because "somebody else said this" is not a verdict, and a hue there would compete with the
 * sender line, which is a verdict. The cursor is a state layer over whichever of the three it
 * is, not a second fill.
 *
 * This replaced five roles - three fills, two more for the same fills under the cursor - that
 * every theme had to state and match by eye against each other. The pairs it answers with are
 * the ones mesh_ui_theme_validate() already holds, selected included.
 */
static struct mesh_ui_paint fb_bubble_paint(const struct mesh_ui_backend_fb_state *state,
                                            const struct fb_bubble *bubble) {
    const enum mesh_ui_state ui_state =
        bubble->selected ? MESH_UI_STATE_SELECTED : MESH_UI_STATE_REST;
    if (bubble->failed) {
        return fb_paint(state, MESH_UI_FAMILY_ERROR, MESH_UI_SLOT_CONTAINER, ui_state);
    }
    if (bubble->outbound) {
        return fb_paint(state, MESH_UI_FAMILY_SECONDARY, MESH_UI_SLOT_CONTAINER, ui_state);
    }
    return (struct mesh_ui_paint){
        .fill = fb_state_layer(state, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, ui_state),
        .ink = fb_color(state, MESH_UI_COLOR_TEXT),
    };
}

/*
 * The colour for a bubble's quieter lines - the sender on one of ours, the clock and the marks
 * beside it on any of them.
 *
 * Dim while the bubble sits at rest, and the bubble's own ink once the cursor is on it or the
 * fill has gone red. The selected fills are a step towards their ink by design, and dim over
 * one of those is the pairing a theme has least room for: on the dark palette it measured
 * 1.9:1, well under the 3:1 a secondary line is held to, and the bubble's own ink is a pair the
 * theme is validated on by construction.
 *
 * The delivery mark takes this ink too rather than a tone of its own. A failed message is
 * already drawn in the error family, so a red tick would be the fill said twice; and "gone out"
 * against "acknowledged" is a difference of one tick, which is the difference every messenger
 * has taught everybody to read. It also keeps the mark inside a pairing the theme is already
 * validated on, instead of asking every palette for a sixth one.
 */
static struct mesh_ui_rgb fb_bubble_quiet(const struct mesh_ui_backend_fb_state *state,
                                          const struct fb_bubble *bubble,
                                          struct mesh_ui_paint paint) {
    if (bubble->selected || bubble->failed) {
        return paint.ink;
    }
    return fb_tone_color(state, MESH_UI_TONE_DIM);
}

static void fb_bubble_part_text(struct fb_bubble_part *parts, size_t *count, const char *text) {
    if (!fb_bubble_has(text) || *count >= FB_BUBBLE_META_PARTS) {
        return;
    }
    parts[*count].text = text;
    parts[*count].icon = MESH_UI_ICON_NONE;
    parts[*count].cells = mesh_ui_text_cells(text);
    *count += 1U;
}

static void fb_bubble_part_icon(struct fb_bubble_part *parts, size_t *count,
                                enum mesh_ui_icon icon) {
    if (!mesh_ui_icon_is_valid(icon) || *count >= FB_BUBBLE_META_PARTS) {
        return;
    }
    parts[*count].text = NULL;
    parts[*count].icon = icon;
    parts[*count].cells = FB_BUBBLE_ICON_CELLS;
    *count += 1U;
}

/* The run's width: every part, plus a cell of air between each neighbouring pair. */
static size_t fb_bubble_run_cells(const struct fb_bubble_part *parts, size_t count) {
    if (count == 0U) {
        return 0U;
    }
    size_t cells = (count - 1U) * FB_BUBBLE_META_GAP;
    for (size_t i = 0; i < count; ++i) {
        cells += parts[i].cells;
    }
    return cells;
}

/*
 * Assemble the trailing run and cut it down to what the bubble can hold.
 *
 * One function because the measure and the draw both need it, and a run assembled one way and
 * painted another is how a transcript comes to overlap itself - or, as it did while the run was
 * a string the screen concatenated, to paint outside the bubble entirely.
 *
 * Parts go in the order they are drawn and come off the *front*, so what is lost first is the
 * reaction chip and what survives longest is the mark saying the message failed. Dropping
 * rather than truncating, because half a clock is not a shorter clock. It bottoms out at
 * nothing, which is the honest answer for a bubble too narrow to say anything in the corner.
 */
static size_t fb_bubble_run(const struct fb_bubble_meta *meta, size_t budget,
                            struct fb_bubble_part *parts, size_t *count) {
    *count = 0U;
    fb_bubble_part_text(parts, count, meta->reactions);
    fb_bubble_part_icon(parts, count, meta->lock);
    fb_bubble_part_text(parts, count, meta->clock);
    fb_bubble_part_icon(parts, count, meta->state);

    size_t cells = fb_bubble_run_cells(parts, *count);
    while (*count > 0U && cells > budget) {
        for (size_t i = 1U; i < *count; ++i) {
            parts[i - 1U] = parts[i];
        }
        *count -= 1U;
        cells = fb_bubble_run_cells(parts, *count);
    }
    return cells;
}

/* The widest and the last of the lines `text` wraps to at `max`, and how many there are. */
static uint32_t fb_bubble_wrap(const char *text, size_t max, size_t *widest, size_t *last) {
    uint32_t lines = 0U;
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, text, max);
    while (mesh_ui_wrap_next(&wrap)) {
        *last = mesh_ui_text_cells(wrap.line);
        if (*last > *widest) {
            *widest = *last;
        }
        lines += 1U;
    }
    return lines;
}

static struct fb_bubble_metrics fb_bubble_measure(const struct mesh_ui_backend_fb_state *state,
                                                  const struct fb_layout *layout,
                                                  const struct fb_bubble *bubble) {
    const size_t max = fb_bubble_max_cols(state, layout);
    struct fb_bubble_metrics metrics;
    memset(&metrics, 0, sizeof metrics);

    /* One wrap walk per block, and the draw makes the same ones. Anything that measured the
       text a second way - a strlen, a second wrapper - is how a bubble comes to paint over the
       one below it. */
    size_t widest = 0U;
    metrics.lines = fb_bubble_wrap(bubble->text, max, &widest, &metrics.last);
    if (metrics.lines == 0U) {
        metrics.lines = 1U; /* an empty message is still a bubble, just an empty one */
    }
    /* The reason a failed message failed, under it - so the run tucks onto *its* last line,
       which is the last line the bubble draws. */
    metrics.notes = fb_bubble_wrap(bubble->note, max, &widest, &metrics.last);

    metrics.cols = widest;
    if (fb_bubble_has(bubble->name)) {
        const size_t name_cols = mesh_ui_text_cells(bubble->name);
        if (name_cols > metrics.cols) {
            metrics.cols = name_cols;
        }
    }
    /* The quote is one line whatever it says, so it is elided here rather than wrapped - and
       the width it asks for is what is left of it, never what it started as. */
    if (fb_bubble_has(bubble->quote) && max > FB_BUBBLE_QUOTE_INDENT) {
        const size_t room = max - FB_BUBBLE_QUOTE_INDENT;
        metrics.quote_cols = mesh_ui_text_cells(bubble->quote);
        if (metrics.quote_cols > room) {
            metrics.quote_cols = room;
        }
        if (metrics.quote_cols + FB_BUBBLE_QUOTE_INDENT > metrics.cols) {
            metrics.cols = metrics.quote_cols + FB_BUBBLE_QUOTE_INDENT;
        }
    }

    /*
     * The run rides the last line when there is room for it there, which is what keeps a
     * three-word message three words tall instead of doubling it.
     *
     * `max` is the budget either way, so the run can never be wider than the bubble - and
     * because the bubble is then widened to hold it, the draw's right-aligned run cannot reach
     * past the left padding. That is the invariant the whole component turns on.
     */
    metrics.meta_cols = fb_bubble_run(&bubble->meta, max, metrics.parts, &metrics.part_count);
    if (metrics.meta_cols > 0U) {
        const size_t tucked = metrics.last + FB_BUBBLE_META_GAP + metrics.meta_cols;
        if (tucked <= max) {
            if (tucked > metrics.cols) {
                metrics.cols = tucked;
            }
        } else {
            metrics.meta_own_line = true;
            if (metrics.meta_cols > metrics.cols) {
                metrics.cols = metrics.meta_cols;
            }
        }
    }

    if (metrics.cols > max) {
        metrics.cols = max;
    }
    if (metrics.cols == 0U) {
        metrics.cols = 1U;
    }

    metrics.rows = metrics.lines + metrics.notes + (fb_bubble_has(bubble->name) ? 1U : 0U) +
                   (metrics.quote_cols > 0U ? 1U : 0U) + (metrics.meta_own_line ? 1U : 0U) +
                   (fb_bubble_has(bubble->separator) ? 1U : 0U);
    return metrics;
}

uint32_t fb_bubble_rows(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, const struct fb_bubble *bubble) {
    return fb_bubble_measure(state, layout, bubble).rows;
}

void fb_draw_separator(const struct mesh_ui_backend_fb_state *state, int y, const char *label,
                       enum mesh_ui_tone tone) {
    const int adv = fb_char_adv(state, state->scale);
    const int rule_y = y + ((int)fb_font(state)->height * state->scale) / 2;
    const int left = fb_margin(state);
    const int right = (int)state->var.xres - left;

    if (!fb_bubble_has(label)) {
        fb_draw_rule(state, left, rule_y, right - left, state->scale, MESH_UI_COLOR_RULE);
        return;
    }

    /* Centred in cells, so a label with an emoji in it sits where it looks centred. */
    const int width = (int)mesh_ui_text_cells(label) * adv;
    const int x = left + (right - left - width) / 2;
    fb_draw_rule(state, left, rule_y, x - left - adv, state->scale, MESH_UI_COLOR_RULE);
    fb_draw_rule(state, x + width + adv, rule_y, right - (x + width + adv), state->scale,
                 MESH_UI_COLOR_RULE);
    fb_draw_text(state, x, y, label, state->scale, fb_tone_color(state, tone),
                 fb_color(state, MESH_UI_COLOR_BG));
}

/* Paints one block of wrapped text from `y` down, and reports where the next row starts. The
   walk the measure made, so the rows painted are the rows reserved. */
static int fb_bubble_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int x, int y,
                                  const char *text, size_t max, int line_h, struct mesh_ui_rgb ink,
                                  struct mesh_ui_rgb fill) {
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, text, max);
    while (mesh_ui_wrap_next(&wrap)) {
        fb_draw_text(state, x, y, wrap.line, state->scale, ink, fill);
        y += line_h;
    }
    return y;
}

void fb_draw_bubble(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    int y, const struct fb_bubble *bubble) {
    const struct fb_bubble_metrics metrics = fb_bubble_measure(state, layout, bubble);
    const int adv = fb_char_adv(state, state->scale);
    const int scale = state->scale;
    const size_t max = fb_bubble_max_cols(state, layout);

    if (fb_bubble_has(bubble->separator)) {
        fb_draw_separator(state, y, bubble->separator, bubble->separator_tone);
        y += layout->line;
    }

    /* The box: content plus half a cell of padding each side, against the edge the direction
       names. The fill stops a scale short of the row it ends on, so stacked bubbles read as
       separate messages rather than as one block. */
    const int pad = adv / 2 > 0 ? adv / 2 : 1;
    const int box_w = (int)metrics.cols * adv + 2 * pad;
    const int box_x =
        bubble->outbound ? (int)state->var.xres - fb_margin(state) - box_w : fb_margin(state);
    const uint32_t box_rows = metrics.rows - (fb_bubble_has(bubble->separator) ? 1U : 0U);
    const int box_h = (int)box_rows * layout->line - scale;

    const struct mesh_ui_paint paint = fb_bubble_paint(state, bubble);
    const struct mesh_ui_rgb fill = paint.fill;
    /*
     * Rounded, because a bubble is the one shape in this UI that everybody already has a
     * picture of: a transcript of square boxes reads as a log, and the same boxes with their
     * corners off read as a conversation. It is the panel shape, so how round belongs to the
     * theme along with everything else about it.
     *
     * The cursor also gets a bar down its outer edge - on a small panel a fill one step lighter
     * is not by itself enough to find, and a colour-blind eye gets nothing from it at all - and
     * it is laid as the card's edge is: the accent shape first, the fill over it a scale
     * narrower on the outer side only, so the bar follows the corner rather than squaring it
     * off. The three edges the two shapes share leave no accent showing on them.
     */
    const int radius = fb_radius(state, MESH_UI_SHAPE_MD);
    if (bubble->selected) {
        fb_fill_round_rect(state, box_x, y - scale, box_w, box_h, radius,
                           fb_color(state, MESH_UI_COLOR_PRIMARY));
    }
    const int fill_x = (bubble->selected && !bubble->outbound) ? box_x + scale : box_x;
    const int fill_w = bubble->selected ? box_w - scale : box_w;
    fb_fill_round_rect(state, fill_x, y - scale, fill_w, box_h, radius, fill);

    const int text_x = box_x + pad;
    const struct mesh_ui_rgb body = paint.ink;
    const struct mesh_ui_rgb quiet = fb_bubble_quiet(state, bubble, paint);

    if (fb_bubble_has(bubble->name)) {
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", bubble->name);
        mesh_ui_line_fit(&line, metrics.cols);
        /* Ours is dimmed and theirs takes the primary: on our own bubble the name is a
           reminder, on theirs it is the thing being looked for. An alert overrides both - it is
           the one bubble whose heading is the point rather than the label on the point. A
           bubble that failed takes its own ink for all three, because the fill has already said
           the only thing a hue on top of it could add. */
        struct mesh_ui_rgb name_color = quiet;
        if (!bubble->failed) {
            if (bubble->alert) {
                name_color = fb_tone_color(state, MESH_UI_TONE_ERROR);
            } else if (!bubble->outbound) {
                name_color = fb_tone_color(state, MESH_UI_TONE_PRIMARY);
            }
        }
        fb_draw_text(state, text_x, y, mesh_ui_line_text(&line), scale, name_color, fill);
        y += layout->line;
    }

    /*
     * The quote, marked the way every messenger marks one: a bar down its left edge and the
     * words beside it in the quiet ink. The bar rather than a glyph because there is no arrow
     * in either face here worth the cell, and because a rule is what the eye already reads as
     * "this is being cited" - the same job the accent edge does on a list row.
     */
    if (metrics.quote_cols > 0U) {
        const int bar_w = scale;
        fb_fill_rect(state, text_x, y, bar_w, layout->line - scale,
                     fb_tone_color(state, MESH_UI_TONE_DIM));
        struct mesh_ui_line quote;
        mesh_ui_line_reset(&quote);
        mesh_ui_line_printf(&quote, "%s", bubble->quote);
        mesh_ui_line_fit(&quote, metrics.quote_cols);
        fb_draw_text(state, text_x + (int)FB_BUBBLE_QUOTE_INDENT * adv, y,
                     mesh_ui_line_text(&quote), scale, quiet, fill);
        y += layout->line;
    }

    int last_y = y;
    y = fb_bubble_draw_wrapped(state, text_x, y, bubble->text, max, layout->line, body, fill);
    if (y == last_y) {
        y += layout->line; /* the measure reserves a row for an empty message; spend it */
    }
    /* The reason under it, in the same ink: the bubble is already the error container, so a
       second red here would be the fill saying the same thing twice. */
    if (metrics.notes > 0U) {
        y = fb_bubble_draw_wrapped(state, text_x, y, bubble->note, max, layout->line, body, fill);
    }
    last_y = y - layout->line;

    if (metrics.part_count == 0U) {
        return;
    }
    /* Right-aligned against the padding, which the measure widened the bubble to leave room
       for - so this can never reach back past `text_x`. */
    int meta_x = box_x + box_w - pad - (int)metrics.meta_cols * adv;
    int meta_y = metrics.meta_own_line ? y : last_y;
    if (!metrics.meta_own_line) {
        /* Tucked against the right edge of the line it shares, which is where every messenger
           puts it - and which is why the measure widened the bubble to make room. */
        const int floor_x = text_x + (int)(metrics.last + FB_BUBBLE_META_GAP) * adv;
        if (meta_x < floor_x) {
            meta_x = floor_x;
        }
    }
    for (size_t i = 0; i < metrics.part_count; ++i) {
        const struct fb_bubble_part *part = &metrics.parts[i];
        if (part->text != NULL) {
            fb_draw_text(state, meta_x, meta_y, part->text, scale, quiet, fill);
        } else {
            fb_draw_icon(state, meta_x, meta_y, part->icon, scale, quiet, fill);
        }
        meta_x += (int)(part->cells + FB_BUBBLE_META_GAP) * adv;
    }
}

size_t fb_field_label_cols(const struct mesh_ui_backend_fb_state *state,
                           const struct fb_layout *layout, size_t preferred) {
    const struct mesh_ui_metrics *metrics = fb_metrics(state);
    if (preferred == 0U) {
        preferred = metrics->field_label_cols;
    }
    return layout->cols < metrics->narrow_cols ? layout->cols / 2U : preferred;
}

/* ---- cards -------------------------------------------------------------------------------- */

/*
 * A card's geometry, all of it derived from the theme's scale and its two card metrics.
 *
 * Computed once and shared by the measure and the draw, for the reason struct fb_bubble_metrics
 * exists: a card that measured one height and painted another would either leave a gap under
 * itself or paint over the rows below, and the screen places the next card from the height this
 * one reported.
 */
struct fb_card_metrics {
    int x, width; /* the panel, edge included */
    int pad;      /* the inset from the side edges to the content */
    int pad_y;    /* the inset from the top and bottom edges */
    int edge;     /* the hairline's thickness, and the only edge the layout knows about */
    /* What is actually painted around the panel: the hairline, or the thicker focus ring. It is
       deliberately not `edge`, because `edge` is in the content inset and in the box height -
       so a ring that widened it would move the card's text and shift every card below it by a
       few pixels for no reason but the cursor arriving. The ring grows *inward*, into the
       padding, which is what keeps the outer geometry a fact about the card rather than about
       what is selected. */
    int ring;
    int radius; /* corner radius, clamped by fb_fill_round_rect() anyway */
    /* The header line: the heading, and the verbs against its far edge. 0 when there is
       neither. */
    int heading_h;
    int button_h;  /* one action button, 0 when the card has no verbs */
    int content_x; /* where a row's text starts */
    size_t cols;   /* content width in cells */
    size_t label_cols;
    int gap;                     /* to the next card */
    enum mesh_ui_color fill;     /* the variant's surface */
    struct mesh_ui_rgb edge_ink; /* the hairline, or the focus ring when a verb is selected */
};

static struct fb_card_metrics fb_card_measure(const struct mesh_ui_backend_fb_state *state,
                                              const struct fb_layout *layout,
                                              const struct fb_card *card) {
    const struct mesh_ui_metrics *metrics = fb_metrics(state);
    const int scale = state->scale;
    struct fb_card_metrics m;
    memset(&m, 0, sizeof m);

    m.x = fb_margin(state);
    m.width = (int)state->var.xres - 2 * m.x;
    m.pad = (int)metrics->card_pad * scale;
    /* Half as much above and below as at the sides, which is not a fudge: a row is a line
       *advance* tall, and the advance already carries the leading that accents hang in, so the
       full inset is counted twice at the top and bottom of a stack of rows and only once at
       either end of one. A card padded equally all round reads bottom-heavy for that reason,
       and on a panel with fifteen body rows it costs the better part of one per card. */
    m.pad_y = m.pad / 2 > 0 ? m.pad / 2 : m.pad;
    m.edge = fb_edge(state);
    /*
     * The variant, as a fill and nothing else. Three tiers rather than three shadows: there is
     * no alpha on this panel to cast one into, so how far a card is off the ground is carried
     * by the surface it is drawn on - which is Material's tonal elevation, and is why the
     * three survive a light palette as well as a dark one.
     */
    switch (card->variant) {
    case FB_CARD_ELEVATED:
        m.fill = MESH_UI_COLOR_SURFACE_HIGH;
        break;
    case FB_CARD_OUTLINED:
        m.fill = MESH_UI_COLOR_BG;
        break;
    case FB_CARD_FILLED:
    default:
        m.fill = MESH_UI_COLOR_SURFACE;
        break;
    }
    /*
     * The focus ring. A card holding the selected verb is the one the next press acts on, and
     * that has to be findable before any of it is read - so it is the edge that changes rather
     * than the fill, drawn in the accent and at twice the thickness. See fb_draw_card().
     *
     * Only what is painted changes. `m.edge` stays the hairline, so the content inset, the
     * label column and the box height are the same whether the card is focused or not; the
     * extra thickness is taken out of the padding instead, capped so it can never reach the
     * text. Widening the layout edge here made a card grow when the cursor arrived and pushed
     * every card under it down the panel, which is a repaint of the whole screen to say one
     * thing about one card.
     */
    bool focused = false;
    for (uint32_t i = 0U; i < card->action_count && i < FB_CARD_ACTIONS_MAX; ++i) {
        focused = focused || card->actions[i].selected;
    }
    m.ring = m.edge;
    if (focused) {
        m.ring = m.edge * 2;
        m.edge_ink = fb_tone_color(state, MESH_UI_TONE_PRIMARY);
    } else {
        m.edge_ink = fb_color(state, MESH_UI_COLOR_OUTLINE);
    }
    m.radius = fb_radius(state, MESH_UI_SHAPE_MD);
    /* A heading is drawn at the chrome scale, the size the tab strip and the footer are: a
       section label is not something to read, it is something to find, and at the body scale it
       costs a whole row of content on a panel that has fifteen of them. */
    m.heading_h = card->heading[0] != '\0' ? fb_line_adv(state, layout->small) : 0;
    /*
     * The verbs sit on the heading's line, against the far edge, and they are drawn at the
     * chrome scale the heading is.
     *
     * A row of buttons under the content is where a card puts its actions on a phone, and it is
     * what this was first written as. It cost two rows off the bottom of the Status screen -
     * one per card carrying a verb - and that screen is the one that can outgrow the panel, so
     * the two rows it lost were a refused packet and a reboot count: the exact rows the card
     * exists to show. A heading is three or four cells of a line that is otherwise empty, and
     * putting the verbs in the rest of it costs nothing at all.
     *
     * The scale follows for the same reason it does on the action bar, which draws every other
     * verb on the frame: a keycap and a verb are chrome, read beside the content rather than as
     * part of it. It also keeps this line the height it already was - a body-scale button here
     * would have grown the header by the difference and given a third of a row back.
     */
    m.button_h = card->action_count > 0U ? fb_line_adv(state, layout->small) : 0;
    if (m.button_h > m.heading_h) {
        m.heading_h = m.button_h;
    }

    /* Never into the content: the ring lives in the padding, and a theme with a thick hairline
       and a tight inset must lose the ring rather than the row it would eat. Bounded by the
       *vertical* inset, which is the smaller of the two - the first row's baseline is what a
       ring grown too far would land on. */
    if (m.ring > m.edge + m.pad_y) {
        m.ring = m.edge + m.pad_y;
    }

    const int inset = m.pad + m.edge;
    m.content_x = m.x + inset;
    const int adv = fb_char_adv(state, state->scale);
    const int content_w = m.width - 2 * inset;
    m.cols = content_w >= adv ? (size_t)(content_w / adv) : 1U;

    /*
     * The label column is measured from the labels the card is actually holding, rather than
     * taken from the theme's preferred width the way a settings list takes it.
     *
     * That is the one thing declaring a card before drawing it buys that a row-at-a-time API
     * cannot: every label is already in hand, so the column is exactly as wide as the widest of
     * them and every value starts as far left as it can. A fixed column has to be wide enough
     * for the longest label any screen might use, which leaves a card of short labels - and the
     * Status cards are mostly one word - with a gutter of nothing in the middle of it.
     *
     * Still bounded: half the card is the most a label column may take, because past that a
     * value is being squeezed to line up labels that already line up.
     */
    size_t widest = 0U;
    for (uint32_t i = 0U; i < card->count; ++i) {
        /*
         * Every row that *uses* the label column, which is the question - not every row of one
         * kind. A note has no label and a meter may or may not have one, so the label itself is
         * what says whether the row is in this measurement, and a kind check here was a card of
         * labelled meters measuring its column against no labels at all and clipping each one
         * to a single cell. Measuring and drawing have to ask the same question.
         */
        const size_t cells = mesh_ui_text_cells(card->rows[i].label);
        if (cells > widest) {
            widest = cells;
        }
    }
    m.label_cols = widest + 1U;
    if (m.label_cols > m.cols / 2U) {
        m.label_cols = m.cols / 2U;
    }
    if (m.label_cols == 0U) {
        m.label_cols = 1U;
    }
    /* Enough space that two stacked cards read as two, and no more: the gap is what a fill
       needs to stop being a continuous block, which is the same job the conversation cell's
       tighter leading does. */
    m.gap = m.pad;
    return m;
}

/*
 * Body rows one row of content occupies. A note is the only one that is more than one, because
 * it is the only one with more words than fit on a line: every picture a card can carry is a
 * *length*, and a length is as thin as the theme's bar.
 *
 * There was a second, and losing it is worth a sentence because it is the rule arriving from the
 * other side. A trend row cost two, since a sparkline's reading is a shape rather than a length
 * and a shape in the height of a hairline is a hairline. It had one caller - the Status screen's
 * airtime block - and that screen now opens the same readings as a chart, which says everything
 * the two rows said and four things they could not. So the shape went to a screen and the card
 * kept the bar, and what is left here is the general form: a card draws levels, and a shape
 * wants a body.
 */
static uint32_t fb_card_row_lines(const struct fb_card_row *row, size_t cols) {
    if (row->kind != FB_CARD_ROW_NOTE) {
        return 1U;
    }
    uint32_t lines = mesh_ui_wrap_lines(row->value, cols);
    if (lines > FB_CARD_NOTE_LINES) {
        lines = FB_CARD_NOTE_LINES;
    }
    return lines > 0U ? lines : 1U;
}

/*
 * How much of a card is being drawn: whole rows, and then however many lines of the row after
 * them a note may keep.
 *
 * The second half is not a refinement. A note is a sentence the radio wrote about itself, and it
 * is the row on the Status screen that explains something no counter can - so dropping it whole
 * because its third line did not fit leaves the age it arrived at and none of the words. The
 * renderer this replaced capped the paragraph to the lines it had room for, and so does this.
 */
struct fb_card_fit {
    uint32_t rows;       /* rows drawn whole */
    uint32_t tail_lines; /* lines kept of rows[rows]; 0 when that row is not drawn at all */
};

/* The painted box for that much of the card - the gap to the next card is not part of it,
   because the last card on a screen does not spend one and a fit test that charged it anyway
   lost a row to a card that had room for it. */
static int fb_card_box_height(const struct fb_card_metrics *m, const struct fb_layout *layout,
                              const struct fb_card *card, struct fb_card_fit fit) {
    int height = 2 * (m->pad_y + m->edge) + m->heading_h;
    for (uint32_t i = 0U; i < fit.rows && i < card->count; ++i) {
        height += (int)fb_card_row_lines(&card->rows[i], m->cols) * layout->line;
    }
    return height + (int)fit.tail_lines * layout->line;
}

/*
 * As much of the card as fits between `top` and `bottom`.
 *
 * Whole rows go first, dropped from the end. Then, when the row that did not fit is a note, as
 * many of its lines as the leftover room takes - and only a note, because a field row is a label
 * and a value and half of one is not half a fact. Nothing is drawn after a clipped note either:
 * a truncated paragraph with rows under it reads as a complete one.
 */
static struct fb_card_fit fb_card_clip(const struct fb_card_metrics *m,
                                       const struct fb_layout *layout, const struct fb_card *card,
                                       int top, int bottom) {
    struct fb_card_fit fit = {.rows = card->count, .tail_lines = 0U};
    while (fit.rows > 0U && top + fb_card_box_height(m, layout, card, fit) > bottom) {
        fit.rows -= 1U;
    }
    if (fit.rows < card->count && card->rows[fit.rows].kind == FB_CARD_ROW_NOTE) {
        const uint32_t whole = fb_card_row_lines(&card->rows[fit.rows], m->cols);
        for (uint32_t lines = whole > 0U ? whole - 1U : 0U; lines > 0U; --lines) {
            const struct fb_card_fit probe = {.rows = fit.rows, .tail_lines = lines};
            if (top + fb_card_box_height(m, layout, card, probe) <= bottom) {
                fit.tail_lines = lines;
                break;
            }
        }
    }
    return fit;
}

void fb_card_begin(struct fb_card *card, enum fb_card_variant variant, enum mesh_ui_icon icon,
                   enum mesh_str_id heading, enum mesh_ui_tone tone) {
    if (card == NULL) {
        return;
    }
    memset(card, 0, sizeof *card);
    card->variant = variant;
    card->tone = tone;
    if (heading != MESH_STR_NONE) {
        card->icon = icon;
        mesh_text_sanitise_str(mesh_str(heading), card->heading, sizeof card->heading);
    }
}

/* The next free row, or NULL once the card is full. */
static struct fb_card_row *fb_card_next_row(struct fb_card *card, enum fb_card_row_kind kind,
                                            enum mesh_ui_tone tone) {
    if (card == NULL || card->count >= FB_CARD_ROWS_MAX) {
        return NULL;
    }
    struct fb_card_row *row = &card->rows[card->count++];
    memset(row, 0, sizeof *row);
    row->kind = kind;
    row->tone = tone;
    return row;
}

void fb_card_row_text(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                      const char *value) {
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_FIELD, tone);
    if (row == NULL) {
        return;
    }
    /*
     * Sanitised rather than copied: both of these are drawn, so a fixed buffer has to be cut on
     * a character and not on a byte - mesh_str_copy() would leave the tail of a multi-byte
     * sequence behind and the font would draw the pieces. It also folds control bytes away,
     * which matters for one row in particular: what the radio last said about itself arrives
     * off the air, and this is the last place before it becomes glyphs.
     */
    mesh_text_sanitise_str(mesh_str(label), row->label, sizeof row->label);
    mesh_text_sanitise_str(value, row->value, sizeof row->value);
}

void fb_card_row(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                 enum mesh_str_id value, ...) {
    char text[MESH_UI_LINE_MAX];
    va_list args;
    va_start(args, value);
    (void)mesh_str_vformat(text, sizeof text, value, args);
    va_end(args);
    fb_card_row_text(card, tone, label, text);
}

void fb_card_meter(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                   int32_t value, struct mesh_ui_scale scale, const struct mesh_ui_band *band,
                   uint32_t id) {
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_METER, tone);
    if (row == NULL) {
        return;
    }
    if (label != MESH_STR_NONE) {
        mesh_text_sanitise_str(mesh_str(label), row->label, sizeof row->label);
    }
    row->meter_value = value;
    row->meter_scale = scale;
    row->meter_banded = band != NULL;
    if (band != NULL) {
        row->meter_band = *band;
    }
    row->meter_id = id;
}

void fb_card_proportion(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                        const uint32_t *values, uint32_t count) {
    if (values == NULL || count < 2U || count > MESH_UI_PROPORTION_PARTS) {
        return;
    }
    /* The whole, tested here rather than in the drawing, so that a card asking for a composition
       and a card asking for a trend answer an absent reading the same way: with no row. */
    uint64_t total = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        total += values[i];
    }
    if (total == 0U) {
        return;
    }
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_PROPORTION, tone);
    if (row == NULL) {
        return;
    }
    if (label != MESH_STR_NONE) {
        mesh_text_sanitise_str(mesh_str(label), row->label, sizeof row->label);
    }
    for (uint32_t i = 0; i < count; ++i) {
        row->parts[i] = values[i];
    }
    row->part_count = count;
}

void fb_card_note(struct fb_card *card, enum mesh_ui_tone tone, const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_NOTE, tone);
    if (row == NULL) {
        return;
    }
    mesh_text_sanitise_str(text, row->value, sizeof row->value);
}

void fb_card_action(struct fb_card *card, enum mesh_str_id label, bool selected) {
    if (card == NULL || card->action_count >= FB_CARD_ACTIONS_MAX || label == MESH_STR_NONE) {
        return;
    }
    struct fb_card_action *action = &card->actions[card->action_count++];
    memset(action, 0, sizeof *action);
    mesh_text_sanitise_str(mesh_str(label), action->label, sizeof action->label);
    action->selected = selected;
}

bool fb_card_is_empty(const struct fb_card *card) { return card == NULL || card->count == 0U; }

int fb_card_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_card *card) {
    if (fb_card_is_empty(card)) {
        return 0;
    }
    const struct fb_card_metrics m = fb_card_measure(state, layout, card);
    const struct fb_card_fit whole = {.rows = card->count, .tail_lines = 0U};
    return fb_card_box_height(&m, layout, card, whole);
}

/* One row of content, drawn at `y` and returning the rows it used. `max_lines` of 0 means the
   row's own count; anything else is the budget a clipped note has been given. */
static uint32_t fb_draw_card_row(struct mesh_ui_backend_fb_state *state,
                                 const struct fb_card_metrics *m, const struct fb_layout *layout,
                                 int y, const struct fb_card_row *row, uint32_t max_lines) {
    const struct mesh_ui_rgb color = fb_tone_color(state, row->tone);
    /* Every row here is inside the panel fb_draw_card() filled, not on the ground it sits on -
       and which fill that is depends on the card's variant, so it comes from the metrics rather
       than from the surface role a card used to be. */
    const struct mesh_ui_rgb ground = fb_color(state, m->fill);
    if (row->kind == FB_CARD_ROW_NOTE) {
        /* fb_draw_wrapped() lays out from the left margin, and a card's content starts inside
           it, so the note is wrapped here against the card's own column. */
        const uint32_t budget = max_lines > 0U ? max_lines : FB_CARD_NOTE_LINES;
        struct mesh_ui_wrap wrap;
        mesh_ui_wrap_begin(&wrap, row->value, m->cols);
        uint32_t drawn = 0U;
        while (drawn < budget && mesh_ui_wrap_next(&wrap)) {
            fb_draw_text(state, m->content_x, y + (int)drawn * layout->line, wrap.line,
                         state->scale, color, ground);
            drawn += 1U;
        }
        return drawn > 0U ? drawn : 1U;
    }

    if (row->kind == FB_CARD_ROW_METER) {
        /*
         * A label in its own column and the bar across the rest, or - with no label - the bar
         * across the card's whole content width. Which of the two is the caller's sentence
         * about what the bar is for; see fb_card_meter().
         *
         * Vertically it sits on the middle of the line the text would have used, which is what
         * keeps a card of rows evenly spaced whether or not one of them is a bar.
         */
        const int adv = fb_char_adv(state, state->scale);
        int bar_x = m->content_x;
        if (row->label[0] != '\0') {
            struct mesh_ui_line line;
            mesh_ui_line_reset(&line);
            mesh_ui_line_column(&line, row->label, m->label_cols);
            mesh_ui_line_fit(&line, m->cols);
            fb_draw_text(state, m->content_x, y, mesh_ui_line_text(&line), state->scale, color,
                         ground);
            bar_x = m->content_x + (int)(m->label_cols + 1U) * adv;
        }
        const int bar_right = m->content_x + (int)m->cols * adv;
        const int height = fb_meter_thickness(state, state->scale);
        if (bar_right - bar_x > 0) {
            const struct fb_meter meter = {
                .rect = {.x = bar_x,
                         .y = y + (layout->line - state->scale - height) / 2,
                         .w = bar_right - bar_x,
                         .h = height},
                .id = row->meter_id,
                .kind = FB_METER_DETERMINATE,
                .value = row->meter_value,
                .scale = row->meter_scale,
                .band = row->meter_banded ? &row->meter_band : NULL,
                .tone = row->tone,
            };
            fb_draw_meter(state, &meter);
        }
        return 1U;
    }

    if (row->kind == FB_CARD_ROW_PROPORTION) {
        /* The meter row's own layout, because a composition is a bar and a card that placed its
           two kinds of bar differently would be reporting a difference that is not there. */
        const int adv = fb_char_adv(state, state->scale);
        int bar_x = m->content_x;
        if (row->label[0] != '\0') {
            struct mesh_ui_line line;
            mesh_ui_line_reset(&line);
            mesh_ui_line_column(&line, row->label, m->label_cols);
            mesh_ui_line_fit(&line, m->cols);
            fb_draw_text(state, m->content_x, y, mesh_ui_line_text(&line), state->scale, color,
                         ground);
            bar_x = m->content_x + (int)(m->label_cols + 1U) * adv;
        }
        const int bar_right = m->content_x + (int)m->cols * adv;
        const int height = fb_proportion_thickness(state, state->scale);
        if (bar_right - bar_x > 0) {
            struct fb_proportion bar = {
                .rect = {.x = bar_x,
                         .y = y + (layout->line - state->scale - height) / 2,
                         .w = bar_right - bar_x,
                         .h = height},
                .count = row->part_count,
                /* The card's own fill, which depends on its variant - the same colour every
                   other row on it draws its text over. */
                .ground = ground,
            };
            for (uint32_t i = 0; i < row->part_count && i < MESH_UI_PROPORTION_PARTS; ++i) {
                bar.values[i] = row->parts[i];
            }
            fb_draw_proportion(state, &bar);
        }
        return 1U;
    }

    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_column(&line, row->label, m->label_cols);
    mesh_ui_line_printf(&line, " %s", row->value);
    mesh_ui_line_fit(&line, m->cols);
    fb_draw_text(state, m->content_x, y, mesh_ui_line_text(&line), state->scale, color, ground);
    return 1U;
}

int fb_card_min_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                       const struct fb_card *card) {
    if (fb_card_is_empty(card)) {
        return 0;
    }
    const struct fb_card_metrics m = fb_card_measure(state, layout, card);
    /* One row, which is the same point fb_draw_card() refuses a card at: a heading with nothing
       under it is not a card. What the row costs is the row's own - a note that wraps to three
       lines is three - because the minimum has to be a card that can actually be drawn. */
    const struct fb_card_fit least = {.rows = 1U, .tail_lines = 0U};
    /* The box and no gap, exactly as fb_card_height() answers now. The gap *between* the two
       cards is real and still has to be paid for - it is just not this card's to state, because
       it belongs to whichever card is reserving the room and is added by
       fb_draw_card_reserving(). Counting it here as well would spend a row of content on air,
       and counting it in one of the pair and not the other is a reservation that is a row too
       generous depending on which of them a screen asked. */
    return fb_card_box_height(&m, layout, card, least);
}

bool fb_draw_card_reserving(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                            int *y, const struct fb_card *card, int reserve) {
    if (y == NULL || fb_card_is_empty(card)) {
        return false;
    }
    const struct fb_card_metrics m = fb_card_measure(state, layout, card);

    /*
     * How much of the card there is room for - the one place the "does another row fit" test
     * lives. Every dense screen used to write it out per row, and the notice paragraph on the
     * Status screen wrote a second, differently. A card with nothing left but its heading is not
     * a card, so that is the point at which it is refused outright rather than drawn as an empty
     * box; a single clipped line of a note is content, so it is not that point.
     *
     * The bound is the body's bottom, not the footer's first baseline: fb_render_snapshot() keeps
     * half a margin between the two when it counts the body rows, and a card that ran to the
     * baseline itself would put its edge against the footer text on the one screen dense enough
     * to reach it.
     */
    const int bottom = layout->footer_y - fb_margin(state) / 2;
    /*
     * And `reserve` off that, which is this card being told to leave room for what comes after
     * it.
     *
     * A column of cards is drawn in order and each one takes what it wants, so the last card is
     * the one that pays for everything above it - and paying, here, means not being drawn at
     * all. That is worse than losing a row: a card carries *verbs*, and which verbs a screen
     * offers is a table (src/ui/status.c) that knows nothing about how tall anything came out.
     * So the cursor keeps walking onto a button that is not on the frame, which is the failure
     * "a card that can end up with no rows must not be given a verb" already names, arrived at
     * from the layout side instead of the row-count side.
     *
     * A reservation turns that around: the card that can afford to lose a row loses one, and the
     * card that would otherwise vanish survives. Which is also the right order editorially - the
     * rows that go are the last ones a screen declared, and a screen declares its least
     * important rows last.
     */
    /*
     * Plus this card's own gap, because that is what separates the two: `*y` advances past the
     * box *and* the gap, so the card being reserved for starts a gap lower than this one ends.
     * Reserving the bare content height leaves it exactly one gap short, which on this panel is
     * the difference between the card being drawn and not.
     */
    int limit = reserve > 0 ? bottom - reserve - m.gap : bottom;
    struct fb_card_fit fit = fb_card_clip(&m, layout, card, *y, limit);
    if (fit.rows == 0U && fit.tail_lines == 0U) {
        /*
         * The reservation yields rather than erasing this card.
         *
         * It is a promise about the card *below*, and a promise that cannot be kept without
         * deleting the card above it is not worth keeping - two cards missing is not an
         * improvement on one. So a reservation that cannot be afforded is dropped, and the
         * column degrades to what it did before: whoever is drawn first gets the room.
         */
        fit = fb_card_clip(&m, layout, card, *y, bottom);
    }
    if (fit.rows == 0U && fit.tail_lines == 0U) {
        return false;
    }

    const int height = fb_card_box_height(&m, layout, card, fit);
    /*
     * The edge first, then the fill inside it: fb_fill_round_rect() fills rather than strokes,
     * so an outline is the larger shape with the smaller one laid over it. Two fills rather than
     * four rectangles because the corners have to follow the radius, and a stroked round rect is
     * a primitive nothing else here would use.
     *
     * The edge is not decoration. On a theme whose surface is a step off the ground - which is
     * every one that ships, because a surface far from the ground is a surface body text is no
     * longer validated against - the fill alone is nearly invisible in daylight, and the
     * hairline is the whole of what says a card is there. It is drawn in OUTLINE rather than
     * RULE for that reason: a separator may fade politely into what it divides, an edge may
     * not. mesh_ui_theme_validate() holds OUTLINE against the ground and both surfaces, which
     * is every fill a variant can put behind it - and the outlined variant, whose fill *is* the
     * ground, is why it has to hold against all three rather than against the panel's own.
     *
     * On the card holding the selected verb both of those change: the ink is the accent and the
     * painted thickness is doubled, which is the focus ring. Only the *painted* thickness - the
     * panel is the same size and its content starts in the same place either way, so the ring
     * grows inward into the padding. See fb_card_measure().
     */
    const int top = *y;
    const int inner_radius = m.radius + m.edge - m.ring > 0 ? m.radius + m.edge - m.ring : 0;
    fb_fill_round_rect(state, m.x, top, m.width, height, m.radius + m.edge, m.edge_ink);
    fb_fill_round_rect(state, m.x + m.ring, top + m.ring, m.width - 2 * m.ring, height - 2 * m.ring,
                       inner_radius, fb_color(state, m.fill));

    int row_y = top + m.pad_y + m.edge;
    /*
     * The verbs, against the far edge of the heading's line.
     *
     * Laid out from the right so the first one declared ends up leftmost, which is the order
     * the screen cursor walks them in - a strip that packed from the left would have reversed
     * that on any card with two. They are text buttons: a word in the accent, and a fill only
     * under the cursor, which is what keeps three cards' worth of verbs from competing with the
     * numbers they are about and makes the selected one unmistakable with no second cue.
     */
    const int content_right = m.content_x + (int)m.cols * fb_char_adv(state, state->scale);
    const int gap = fb_space(state, MESH_UI_SPACE_XS);
    int actions_x = content_right;
    if (m.button_h > 0) {
        /*
         * How many of them there is room for, dropped from the *end* rather than from wherever
         * the layout ran out. Laying out from the right and stopping when the next one no
         * longer fits would drop the leftmost, which is the first the cursor reaches - so a
         * card too narrow for its verbs would lose the one A runs first.
         */
        uint32_t drawn = card->action_count;
        while (drawn > 0U) {
            int total = 0;
            for (uint32_t i = 0U; i < drawn; ++i) {
                total += fb_button_width(state, MESH_UI_ICON_NONE, card->actions[i].label,
                                         layout->small);
                if (i > 0U) {
                    total += gap;
                }
            }
            if (m.content_x + total <= content_right) {
                break;
            }
            drawn -= 1U;
        }
        for (uint32_t i = drawn; i-- > 0U;) {
            const int width =
                fb_button_width(state, MESH_UI_ICON_NONE, card->actions[i].label, layout->small);
            actions_x -= width;
            const struct fb_button button = {
                .rect = {.x = actions_x, .y = row_y, .w = width, .h = m.button_h},
                .icon = MESH_UI_ICON_NONE,
                .label = card->actions[i].label,
                .selected = card->actions[i].selected,
                .variant = FB_BUTTON_TEXT,
                .shape = MESH_UI_SHAPE_FULL,
                .idle_tone = MESH_UI_TONE_PRIMARY,
                .ground = m.fill,
                .scale = layout->small,
            };
            fb_draw_button(state, &button);
            actions_x -= gap;
        }
    }
    if (m.heading_h > 0 && card->heading[0] != '\0') {
        /* The heading takes the card's tone, which is how a card reports on what it holds
           without a second cue: the Link card goes bad when the radio has gone. Every tone a
           card can take is validated against the surface, so none of them can go quiet here. */
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", card->heading);
        const struct mesh_ui_rgb ink = fb_tone_color(state, card->tone);
        int heading_x = m.content_x;
        if (mesh_ui_icon_is_valid(card->icon)) {
            /* On the card's own surface, which is what it was just filled with - the icon is
               inside the panel, not on the ground the panel sits on. */
            fb_draw_icon(state, heading_x, row_y, card->icon, layout->small, ink,
                         fb_color(state, m.fill));
            /* The same half-cell a button leaves between its symbol and its word. */
            heading_x += fb_icon_box(state, layout->small) + fb_char_adv(state, layout->small) / 2;
        }
        /* Fitted to what the verbs left rather than to the card, so a long heading is cut on a
           cell boundary instead of running under the first button. */
        const int heading_adv = fb_char_adv(state, layout->small);
        const int heading_w = actions_x - heading_x;
        mesh_ui_line_fit(&line, heading_w >= heading_adv ? (size_t)(heading_w / heading_adv) : 1U);
        fb_draw_text(state, heading_x, row_y, mesh_ui_line_text(&line), layout->small, ink,
                     fb_color(state, m.fill));
    }
    if (m.heading_h > 0) {
        row_y += m.heading_h;
    }

    for (uint32_t i = 0U; i < fit.rows; ++i) {
        row_y += (int)fb_draw_card_row(state, &m, layout, row_y, &card->rows[i], 0U) * layout->line;
    }
    if (fit.tail_lines > 0U) {
        (void)fb_draw_card_row(state, &m, layout, row_y, &card->rows[fit.rows], fit.tail_lines);
    }

    *y = top + height + m.gap;
    return true;
}

bool fb_draw_card(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout, int *y,
                  const struct fb_card *card) {
    return fb_draw_card_reserving(state, layout, y, card, 0);
}

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

void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped) {
    if (dropped > 0U) {
        mesh_str_format(out, out_len, MESH_STR_LIST_TITLE_COUNT_OLDER, name, count, dropped);
    } else {
        mesh_str_format(out, out_len, MESH_STR_LIST_TITLE_COUNT, name, count);
    }
}

/* ---- the switch -------------------------------------------------------------------------- */

/*
 * How long a knob takes to cross, and on what curve.
 *
 * The short token: what a control answering a button press wants - long enough that the eye
 * follows the knob across rather than seeing it teleport, short enough that nobody waits for
 * it. Ease-out because the press has already happened; the movement is the screen catching up,
 * so it should leave briskly and settle rather than wind up first.
 */
#define FB_SWITCH_MOTION MESH_UI_MOTION_SHORT

/* Shorter than the line advance, which carries the gap between rows: a control as tall as the
   advance touches the row above it. */
static int fb_switch_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int height = fb_line_adv(state, scale) - fb_space_at(state, MESH_UI_SPACE_MD, scale);
    return height < 6 ? 6 : height;
}

/*
 * The whole of the control's geometry, derived from the glyph metrics.
 *
 * A switch is a wide pill: 9:5 is close to what the phone platforms use and is what stops it
 * reading as a circle at a small scale. Everything else - the corner radius, the ring the knob
 * sits in, how far it travels - falls out of the height below, so a theme that asks for bigger
 * text gets a proportionally bigger control and no renderer is touched.
 */
void fb_switch_size(const struct mesh_ui_backend_fb_state *state, int scale, int *w, int *h) {
    const int height = fb_switch_height(state, scale);
    if (h != NULL) {
        *h = height;
    }
    if (w != NULL) {
        *w = height * 9 / 5;
    }
}

void fb_draw_switch(struct mesh_ui_backend_fb_state *state, const struct fb_switch *sw) {
    if (sw == NULL || sw->rect.w <= 0 || sw->rect.h <= 0) {
        return;
    }

    /*
     * Where the knob is, which is the one thing the snapshot cannot say. The table answers
     * with the target on first sight of this id and with a position in between afterwards -
     * so a screen opening does not animate and a press does.
     */
    const int32_t position =
        mesh_ui_anim_track(&state->anim, sw->id, state->now_ms, sw->on ? MESH_UI_ANIM_ONE : 0,
                           fb_motion(state, FB_SWITCH_MOTION), MESH_UI_EASE_OUT);

    const int damage_pad = fb_space(state, MESH_UI_SPACE_XS);
    fb_animation_damage(state, sw->rect.x - damage_pad, sw->rect.y - damage_pad,
                        sw->rect.w + 2 * damage_pad, sw->rect.h + 2 * damage_pad);
    const int radius = sw->rect.h / 2;

    /*
     * Its own ground under a cursor fill. Both colour pairs below are contracted against the
     * ground rather than against the cursor fill, and on the dark and colorblind themes the
     * cursor fill and the resting track are the same colour - so without this the control
     * would be invisible on precisely the row being pointed at.
     */
    if (sw->selected) {
        /* The ring is a fraction of the glyph scale rather than of the control, because the row
           fill it has to stay inside is measured from the scale too - a ring sized off the
           control overhung the highlight bar top and bottom and notched it. */
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, sw->rect.x - pad, sw->rect.y - pad, sw->rect.w + 2 * pad,
                           sw->rect.h + 2 * pad, radius + pad, fb_color(state, MESH_UI_COLOR_BG));
    }

    /*
     * The track takes its colour from the end the knob has passed, rather than fading between
     * the two as it travels.
     *
     * A fade was the first thing tried and it is wrong for a reason worth writing down: the
     * pairs below are validated against each other, and a track halfway between two of them is
     * not validated against either knob colour - so the one moment the eye is actually
     * following the control is the moment its contrast is unaccounted for. It also looks bad,
     * because on the dark theme the two ends are yellow and blue and everything between them
     * is mud. Flipping at the midpoint keeps every frame a pair a theme was held to.
     *
     * A dim switch reports a state rather than offering one, so it stays muted at both ends and
     * never takes the accent.
     */
    const bool past_middle = position > MESH_UI_ANIM_ONE / 2;
    const struct mesh_ui_paint on_paint =
        fb_paint(state, sw->family, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
    struct mesh_ui_rgb track;
    struct mesh_ui_rgb knob;
    if (sw->dim) {
        track = fb_color(state, past_middle ? MESH_UI_COLOR_RULE_STRONG : MESH_UI_COLOR_RULE);
        knob = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
    } else if (past_middle) {
        /* The on end is the family, fill and knob taken from the one pair. */
        track = on_paint.fill;
        knob = on_paint.ink;
    } else {
        track = fb_color(state, MESH_UI_COLOR_SURFACE_SEL);
        knob = fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL);
    }
    fb_fill_round_rect(state, sw->rect.x, sw->rect.y, sw->rect.w, sw->rect.h, radius, track);

    /* The knob: a disc inside a ring of track, travelling the width less that ring. */
    const int inset = sw->rect.h / 8 + 1;
    const int knob_size = sw->rect.h - 2 * inset;
    const int travel = sw->rect.w - 2 * inset - knob_size;
    const int x = sw->rect.x + inset + (travel > 0 ? (travel * position) / MESH_UI_ANIM_ONE : 0);
    fb_fill_round_rect(state, x, sw->rect.y + inset, knob_size, knob_size, knob_size / 2, knob);
}

/* ---- the selection control ----------------------------------------------------------------- */

/*
 * The same token as the switch's, and for the same reason: both answer a press, so both have to
 * be somewhere by the time the eye gets back to them.
 */
#define FB_SELECTION_MOTION MESH_UI_MOTION_SHORT

void fb_selection_size(const struct mesh_ui_backend_fb_state *state, int scale, int *w, int *h) {
    /* A square of the switch's height, so a list mixing the two controls has them the same
       distance off its rows' top and bottom edges. */
    const int side = fb_switch_height(state, scale);
    if (w != NULL) {
        *w = side;
    }
    if (h != NULL) {
        *h = side;
    }
}

/*
 * The largest glyph multiplier whose icon fits inside `box` pixels.
 *
 * A checkbox's tick is an icon, on the same terms as every other symbol in this UI, and an icon
 * is sized in glyph scales rather than in pixels - so fitting one inside a control means asking
 * the drawing code how big each scale comes out rather than picking a number. Zero when even the
 * smallest overruns, which is a checkbox that draws its fill and no tick: at that size the fill
 * is the whole of what can be read anyway.
 */
static int fb_icon_scale_within(const struct mesh_ui_backend_fb_state *state, int box) {
    for (int scale = state->scale; scale > 0; --scale) {
        if (fb_icon_drawn(state, scale) <= box) {
            return scale;
        }
    }
    return 0;
}

void fb_draw_selection(struct mesh_ui_backend_fb_state *state, const struct fb_selection *sel) {
    if (sel == NULL || sel->rect.w <= 0 || sel->rect.h <= 0) {
        return;
    }

    /*
     * How far the mark has grown, which - exactly as with the switch's knob - is the one thing
     * the snapshot cannot say: it knows on or off, not where between them this control is. The
     * table answers with the target the first time it sees an id, so a screen opening does not
     * animate and a press does.
     */
    const int32_t position =
        mesh_ui_anim_track(&state->anim, sel->id, state->now_ms, sel->on ? MESH_UI_ANIM_ONE : 0,
                           fb_motion(state, FB_SELECTION_MOTION), MESH_UI_EASE_OUT);

    const int damage_pad = fb_space(state, MESH_UI_SPACE_XS);
    fb_animation_damage(state, sel->rect.x - damage_pad, sel->rect.y - damage_pad,
                        sel->rect.w + 2 * damage_pad, sel->rect.h + 2 * damage_pad);
    const bool radio = (sel->shape == FB_SELECTION_RADIO);
    const int side = sel->rect.w < sel->rect.h ? sel->rect.w : sel->rect.h;
    /* A circle for one-of-these, a rounded square for any-of-these. The shape scale answers the
       square, because that is what a small container is; the circle is not a radius a theme gets
       to have an opinion about - a radio that is not round is not a radio. */
    const int radius = radio ? side / 2 : fb_radius(state, MESH_UI_SHAPE_SM);
    const int edge = fb_edge(state);
    /* Two hairlines, not one: this ring has to be countable down a column at arm's length, and
       a card's edge is drawn against a fill that is doing half the work of saying it is there. */
    const int ring = edge * 2 > side / 2 ? edge : edge * 2;

    /*
     * Its own ground under a cursor fill, for the reason the switch lays one: the pairs below
     * are contracted against the ground, and on two of the four themes the cursor fill is the
     * colour the resting control is drawn in - so without this the control would vanish on
     * precisely the row being pointed at.
     */
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);
    if (sel->selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, sel->rect.x - pad, sel->rect.y - pad, sel->rect.w + 2 * pad,
                           sel->rect.h + 2 * pad, radius + pad, ground);
    }

    /*
     * The colours flip at the midpoint rather than fading, which is the rule fb_draw_switch()
     * arrived at and wrote down: the theme validates pairs, and a colour halfway between two of
     * them is validated against neither - so the one moment the eye is following the control is
     * the moment its contrast is unaccounted for.
     *
     * A dim control reports a state rather than offering one, so it never takes the accent.
     */
    const bool past_middle = position > MESH_UI_ANIM_ONE / 2;
    const struct mesh_ui_paint on_paint =
        fb_paint(state, sel->family, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
    struct mesh_ui_rgb mark = on_paint.fill;
    struct mesh_ui_rgb outline = fb_color(state, MESH_UI_COLOR_OUTLINE);
    if (sel->dim) {
        mark = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
        outline = fb_color(state, MESH_UI_COLOR_RULE);
    } else if (past_middle) {
        /* The ring joins the mark once the mark is the thing being read: an accent dot inside a
           grey ring reads as a dot that has landed in the wrong control. */
        outline = mark;
    }

    /* The ring, hollowed out. Two fills, the way every outline in this file is drawn. */
    fb_fill_round_rect(state, sel->rect.x, sel->rect.y, side, side, radius, outline);
    fb_fill_round_rect(state, sel->rect.x + ring, sel->rect.y + ring, side - 2 * ring,
                       side - 2 * ring, radius - ring > 0 ? radius - ring : 0, ground);

    /*
     * The mark, grown from the centre.
     *
     * A radio's dot stays inside its ring - the ring is what says there are others, so it never
     * goes away. A checkbox's fill takes the whole box, ring included, because a checked box is
     * a filled box on every platform there is and the tick has to have a validated fill under
     * it: `on_paint` is a pair, and drawing its ink over the ground would be using half of one.
     */
    const int room = radio ? side - 2 * (ring + ring) : side;
    const int size = room > 0 ? (room * position) / MESH_UI_ANIM_ONE : 0;
    if (size <= 0) {
        return;
    }
    const int mark_x = sel->rect.x + (side - size) / 2;
    const int mark_y = sel->rect.y + (side - size) / 2;
    fb_fill_round_rect(state, mark_x, mark_y, size, size,
                       radio ? size / 2 : (radius < size / 2 ? radius : size / 2), mark);
    if (radio || !past_middle) {
        return;
    }

    /* The tick, once there is a fill validated to draw it on. Sized to the box rather than to
       the row's text, and centred on the control: fb_draw_icon() places an icon against a text
       baseline, so what it is handed here is the baseline that would put one there. */
    const int icon_scale = fb_icon_scale_within(state, size - 2 * ring);
    if (icon_scale <= 0) {
        return;
    }
    fb_draw_icon(state, sel->rect.x + (side - fb_icon_box(state, icon_scale)) / 2,
                 sel->rect.y + (side - (int)fb_font(state)->height * icon_scale) / 2,
                 MESH_UI_ICON_CHECK, icon_scale, on_paint.ink, mark);
}

/* ---- the segmented button ------------------------------------------------------------------ */

int fb_segmented_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    return fb_switch_height(state, scale);
}

int fb_segmented_width(const struct mesh_ui_backend_fb_state *state,
                       const struct fb_segmented *segmented, int scale) {
    if (segmented == NULL || segmented->count == 0U || segmented->count > FB_SEGMENTED_MAX) {
        return 0;
    }
    /*
     * Every segment as wide as the widest label, and a segment is exactly a button's worth of
     * room around it - fb_button_width() rather than a padding of this component's own, because
     * that is what actually draws here and a strip that measured itself differently from the
     * way it draws is a strip whose last segment falls off the row.
     *
     * Equal shares are not a simplification. A strip whose segments were each sized to their own
     * words is a chip strip, and what separates the two components is precisely that these are
     * *alternatives*: the eye has to compare them, and three boxes of three different widths are
     * read as three different kinds of thing.
     */
    int widest = 0;
    for (size_t i = 0; i < segmented->count; ++i) {
        const int width = fb_button_width(state, MESH_UI_ICON_NONE, segmented->labels[i], scale);
        widest = width > widest ? width : widest;
    }
    return (int)segmented->count * widest;
}

void fb_draw_segmented(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *rect,
                       const struct fb_segmented *segmented, bool selected,
                       enum mesh_ui_color ground, int scale) {
    /* `active` is checked rather than clamped, and that is the point: fb_segmented_cols() sends
       a choice outside the set to the words, so reaching here with one means the measure and
       the draw have disagreed - and drawing the first segment lit would answer the disagreement
       with a claim about the radio's configuration. Nothing is the safe answer. */
    if (rect == NULL || segmented == NULL || segmented->count == 0U ||
        segmented->count > FB_SEGMENTED_MAX || segmented->active >= segmented->count ||
        rect->w <= 0 || rect->h <= 0) {
        return;
    }
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);
    const int edge = fb_edge(state);

    /* Its own ground under a cursor fill, on the same terms as the switch's and the selection
       control's: the container is an outline, so what shows through it is whatever is behind -
       and behind it on the row the cursor is on is a fill the outline was never contracted
       against. */
    struct mesh_ui_rgb behind = fb_color(state, ground);
    if (selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        behind = fb_color(state, MESH_UI_COLOR_BG);
        fb_fill_round_rect(state, rect->x - pad, rect->y - pad, rect->w + 2 * pad,
                           rect->h + 2 * pad, radius, behind);
    }

    /* The container: one outline around the set, which is the whole of what says these are
       alternatives rather than a row of separate offers. Two fills, as every outline here is. */
    fb_fill_round_rect(state, rect->x, rect->y, rect->w, rect->h, radius,
                       fb_color(state, MESH_UI_COLOR_OUTLINE));
    fb_fill_round_rect(state, rect->x + edge, rect->y + edge, rect->w - 2 * edge,
                       rect->h - 2 * edge, radius, behind);

    const size_t active = segmented->active;
    for (size_t i = 0; i < segmented->count; ++i) {
        /* Divided by multiplying rather than by accumulating a width, so the rounding error is
           spread over the strip instead of piling up on the last segment. */
        const int left = rect->x + (int)((size_t)rect->w * i / segmented->count);
        const int right = rect->x + (int)((size_t)rect->w * (i + 1U) / segmented->count);

        /*
         * A hairline between two segments, and not against the selected one - which is
         * Material's rule and is right for the reason the divider exists at all: a separator
         * says "these two are different things", and a filled segment has already said it.
         */
        if (i > 0U && i != active && i - 1U != active) {
            fb_fill_rect(state, left, rect->y + edge, edge, rect->h - 2 * edge,
                         fb_color(state, MESH_UI_COLOR_OUTLINE));
        }

        /*
         * The segment itself is a button, because that is what it is: the tonal fill on the
         * chosen one and a dim label on the rest is the pairing fb_draw_chip() already uses for
         * a tab, and a second opinion about it here would be a segmented control that drifted
         * away from the tab strip it is a sibling of.
         *
         * Inset by the hairline so the container's outline survives underneath the fill, and by
         * it again at the two ends so a full-radius fill follows the container's corner rather
         * than squaring it off.
         */
        const struct fb_button segment = {
            .rect = {.x = left + (i == 0U ? edge : 0),
                     .y = rect->y + edge,
                     .w = right - left - (i == 0U ? edge : 0) -
                          (i + 1U == segmented->count ? edge : 0),
                     .h = rect->h - 2 * edge},
            .label = segmented->labels[i],
            .variant = i == active ? FB_BUTTON_TONAL : FB_BUTTON_TEXT,
            .shape = MESH_UI_SHAPE_FULL,
            .idle_tone = MESH_UI_TONE_DIM,
            .ground = selected ? MESH_UI_COLOR_BG : ground,
            .scale = scale,
        };
        fb_draw_button(state, &segment);
    }
}

/* ---- the meter ----------------------------------------------------------------------------- */

/*
 * How long a determinate fill takes to reach a new reading.
 *
 * Longer than the switch's, and for the opposite reason. A switch answers a press, so it has to
 * feel immediate; a meter answers a *sample*, and the samples are seconds apart - the airtime
 * figures arrive on the radio's own schedule, the download's byte count is whatever the file on
 * disk had grown to when the loop last looked. Easing across that gap is the whole trick: the
 * bar spends its time moving between two readings instead of sitting still and then jumping,
 * and what the eye gets is the rate rather than the samples.
 */
#define FB_METER_MOTION MESH_UI_MOTION_LONG

/* One pass of an indeterminate pill. Slow enough to read as travel rather than as flicker, fast
   enough that a screen with one on it does not feel stalled. */
#define FB_METER_LOOP_MOTION MESH_UI_MOTION_LOOP

/* How much of the track the pill covers. A third is the proportion Material's indeterminate bar
   settles at, and it is about the shortest that still reads as a bar rather than as a dot. */
#define FB_METER_PILL_PCT 34

/*
 * Where in its cycle the pill starts, in permille.
 *
 * A loop begins at 0, which for the travel below is the pill entirely off the leading edge -
 * so a bar drawn on the frame it first appears would be an empty track, and the one frame that
 * has to say "this is working" would say the opposite. Roughly two fifths in is where the eased
 * travel first brings the whole pill onto the track, so the widget appears with something in
 * it and loops normally from there.
 *
 * A rotation of a periodic function, not a special case for the first frame: every cycle starts
 * here, so nothing has to remember whether this is the first one.
 */
#define FB_METER_PILL_PHASE 400

int fb_meter_thickness(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int thickness = (int)fb_metrics(state)->meter_thickness * (scale > 0 ? scale : 1);
    return thickness > 1 ? thickness : 1;
}

/*
 * The fill's colour.
 *
 * A family tone and nothing else. The neutral three - normal, dim, strong - fold back to the
 * primary rather than being drawn as asked, and that is not defensiveness: a fill nobody has
 * validated against MESH_UI_COLOR_METER_TRACK is a bar that vanishes on some theme somebody has
 * not opened yet, and the primary is the one answer that is always right for "something is
 * here". Every family *is* validated against the track, so this is now a question about the
 * kind of tone rather than a list to keep in step with the validator - which is what the list
 * that used to be here was, and it went stale the moment a fourth fill existed.
 */
static enum mesh_ui_tone fb_meter_tone(enum mesh_ui_tone tone) {
    return mesh_ui_tone_family(tone) != MESH_UI_FAMILY_COUNT ? tone : MESH_UI_TONE_PRIMARY;
}

/*
 * Where a boundary is marked on the track, or -1 for one there is no point marking.
 *
 * The ends are refused deliberately. A notch at 0 or at the full width is not a threshold the
 * eye can locate against anything - it is the edge of the track, which is already drawn - and a
 * band whose boundary sits off the scale is a caller's domain and threshold disagreeing, which
 * is better shown as an unmarked bar than as a mark in the wrong place.
 */
static int fb_band_mark(const struct fb_meter *meter, int32_t boundary) {
    const int32_t permille = mesh_ui_scale_permille(meter->scale, boundary);
    if (permille <= 0 || permille >= MESH_UI_ANIM_ONE) {
        return -1;
    }
    return (int)(((int64_t)meter->rect.w * permille) / MESH_UI_ANIM_ONE);
}

void fb_draw_meter(struct mesh_ui_backend_fb_state *state, const struct fb_meter *meter) {
    if (meter == NULL || meter->rect.w <= 0 || meter->rect.h <= 0) {
        return;
    }
    const struct fb_rect r = meter->rect;
    const int damage_pad = fb_space(state, MESH_UI_SPACE_XS);
    fb_animation_damage(state, r.x - damage_pad, r.y - damage_pad, r.w + 2 * damage_pad,
                        r.h + 2 * damage_pad);
    /* A pill, always: a bar with square ends reads as a region of the screen that has been
       filled in, and one with round ends reads as a quantity in a container. */
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);

    /*
     * Its own ground under a cursor fill, for the reason the switch lays one.
     *
     * The track is validated against the body and against a card, which are the two grounds a
     * meter is drawn on - not against the cursor fill, and on two of the four themes it *is*
     * the cursor fill. Without this the track would disappear on precisely the row being
     * pointed at, leaving a fill floating in space with no length to be read against.
     */
    if (meter->selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad, radius + pad,
                           fb_color(state, MESH_UI_COLOR_BG));
    }

    fb_fill_round_rect(state, r.x, r.y, r.w, r.h, radius,
                       fb_color(state, MESH_UI_COLOR_METER_TRACK));

    if (meter->kind == FB_METER_INDETERMINATE) {
        /* A band says where a reading changes meaning and an indeterminate bar has no reading,
           so nothing here consults one: the pill is drawn in the tone it was given. */
        const struct mesh_ui_rgb ink = fb_tone_color(state, fb_meter_tone(meter->tone));
        /*
         * A pill crossing the track, from entirely off the leading edge to entirely off the
         * trailing one. Both ends of the travel are off the track on purpose: the loop's wrap
         * from ONE back to 0 then happens while nothing is drawn, so a bar that never finishes
         * also never visibly restarts.
         *
         * Eased rather than linear, so it accelerates in and settles out instead of sliding at
         * one speed - which is the difference between a thing that is working and a thing on a
         * conveyor belt.
         */
        const int pill = r.w * FB_METER_PILL_PCT / 100 > 1 ? r.w * FB_METER_PILL_PCT / 100 : 1;
        /* The phase is added before the curve, not after: shifting the sawtooth rotates where
           the cycle begins and leaves the wrap exactly where it was - at the point the pill is
           off the track entirely, which is what keeps a loop that never ends from visibly
           restarting. Shifting the eased value instead would put a jump in the middle of the
           travel. */
        const int32_t phase = (mesh_ui_anim_loop(&state->anim, meter->id, state->now_ms,
                                                 fb_motion(state, FB_METER_LOOP_MOTION)) +
                               FB_METER_PILL_PHASE) %
                              MESH_UI_ANIM_ONE;
        const int32_t t = mesh_ui_ease(MESH_UI_EASE_IN_OUT, phase);
        const int travel = r.w + pill;
        int x = r.x - pill + (int)(((int64_t)travel * t) / MESH_UI_ANIM_ONE);
        int w = pill;
        /* Clipped to the track rather than drawn past it: fb_fill_round_rect() is happy to fill
           outside a container it knows nothing about, and the container here is the thing that
           gives the pill its meaning. */
        if (x < r.x) {
            w -= r.x - x;
            x = r.x;
        }
        if (x + w > r.x + r.w) {
            w = r.x + r.w - x;
        }
        if (w > 0) {
            fb_fill_round_rect(state, x, r.y, w, r.h, radius, ink);
        }
        return;
    }

    /*
     * The reading onto the track. Once, here, rather than at the call site - which is the whole
     * reason the domain travels with the reading: the fill's length and the boundary marks
     * below are then measured by one piece of arithmetic and cannot land in different places.
     */
    const int32_t value = mesh_ui_scale_permille(meter->scale, meter->value);
    /* The band is asked in the meter's own units rather than in permille, so a boundary is
       compared against the figure a caller stated rather than against a rounded position. */
    const struct mesh_ui_rgb ink = fb_tone_color(
        state, fb_meter_tone(mesh_ui_band_tone(meter->band, meter->value, meter->tone)));

    /* Where the fill has got to, which is not where the reading is: see FB_METER_MOTION. */
    const int32_t position =
        mesh_ui_anim_track(&state->anim, meter->id, state->now_ms, value,
                           fb_motion(state, FB_METER_MOTION), MESH_UI_EASE_OUT);

    int fill = (int)(((int64_t)r.w * position) / MESH_UI_ANIM_ONE);
    if (fill <= 0) {
        /* A reading that is not zero draws something, however small. Rounding a real 0.4% down
           to no pixels at all says "nothing is happening", which is the one thing the bar is
           there to distinguish from. Exactly zero draws an empty track, as it should. */
        fill = position > 0 ? 1 : 0;
    }
    if (fill > 0) {
        fb_fill_round_rect(state, r.x, r.y, fill, r.h, radius, ink);
    }

    if (meter->band == NULL) {
        return;
    }
    /*
     * The boundaries, cut *out* of the bar rather than laid on top of it.
     *
     * A notch in the ground colour is the one mark that reads the same whether or not the fill
     * has reached it: over the track it is a gap in the track, over the fill it is a gap in the
     * fill, and either way the eye sees the bar divided where the meaning divides. A mark drawn
     * in an ink of its own would need a colour validated against both, which is two more
     * contracts every theme would have to satisfy to say something the absence of ink already
     * says.
     *
     * Same ground the widget lays under a selected track above, for the same reason: that is
     * what is behind the bar on the row the cursor is on.
     */
    const int notch = fb_space(state, MESH_UI_SPACE_XS) > 0 ? fb_space(state, MESH_UI_SPACE_XS) : 1;
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);
    const int32_t bounds[] = {meter->band->warn, meter->band->bad};
    for (size_t i = 0U; i < sizeof bounds / sizeof bounds[0]; i++) {
        const int mark = fb_band_mark(meter, bounds[i]);
        if (mark < 0) {
            continue;
        }
        int x = r.x + mark - notch / 2;
        int w = notch;
        if (x < r.x) {
            w -= r.x - x;
            x = r.x;
        }
        if (x + w > r.x + r.w) {
            w = r.x + r.w - x;
        }
        if (w > 0) {
            fb_fill_rect(state, x, r.y, w, r.h, ground);
        }
    }
}

/* ---- the slider ----------------------------------------------------------------------------- */

/*
 * The handle travels to each new value rather than appearing at it, on the meter's terms and
 * for the same reason - a control the reader just pressed should be seen to move, because that
 * is what says the press landed. EASE_OUT rather than IN_OUT: this follows a button press, and
 * a press wants a control that leaves immediately and settles.
 */
#define FB_SLIDER_MOTION MESH_UI_MOTION_SHORT

/*
 * The track, as a multiple of a meter's, and the handle as a multiple of the track.
 *
 * A control is drawn heavier than a reading of the same width, and that is not a preference: a
 * meter is looked at when the eye is already on the row, while a slider has to be *found* before
 * it can be aimed at, from wherever the cursor was. At a meter's thickness across a whole row it
 * reads as a rule with a mark on it.
 *
 * Two handle heights rather than one, because the row has to reserve the taller of them whether
 * or not the cursor is here: a handle that grew the row it is on would push every row below it
 * down as the cursor arrived, which is the correction §11 made to the progress bar and §8 made
 * to a card's focus ring. So the box is always the focused size and the resting handle is drawn
 * short inside it.
 */
#define FB_SLIDER_TRACK 2
#define FB_SLIDER_HANDLE_REST 3 /* in halves of a track */
#define FB_SLIDER_HANDLE_FOCUS 4

int fb_slider_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    return fb_meter_thickness(state, scale) * FB_SLIDER_TRACK * FB_SLIDER_HANDLE_FOCUS / 2;
}

/*
 * The choices, marked on the track.
 *
 * Cut out of it in the ground colour rather than laid on it in an ink of their own - the band
 * notches' arrangement, and the same argument: a gap reads the same over the filled half as over
 * the empty one, and a mark with a colour would be two more contracts every theme has to satisfy
 * to say what an absence already says. It follows that this must run *after* whatever it is
 * cutting into, which is the one thing about it that is easy to get wrong.
 *
 * Drawn only where they can be told apart. A settings field may offer four choices or twelve,
 * and twelve notches on a narrow panel is a dashed line rather than a set of stops - so the
 * component decides, from the width the row actually gave it, and an unmarked track is the
 * honest answer for a list too long to mark. The ends are skipped for the reason the band's are:
 * a notch at the very edge of a track is the edge of the track.
 */
static void fb_slider_stops(const struct mesh_ui_backend_fb_state *state,
                            const struct fb_rect *track, int handle_w, uint32_t stops,
                            struct mesh_ui_rgb ground) {
    const int notch = fb_space(state, MESH_UI_SPACE_XS) > 0 ? fb_space(state, MESH_UI_SPACE_XS) : 1;
    const int travel = track->w - handle_w;
    if (stops < 2U || travel <= 0 || (uint32_t)travel < (stops - 1U) * (uint32_t)(notch * 4)) {
        return;
    }
    for (uint32_t stop = 1U; stop + 1U < stops; ++stop) {
        const int x = track->x + handle_w / 2 +
                      (int)(((int64_t)travel * stop) / (int64_t)(stops - 1U)) - notch / 2;
        if (x >= track->x && x + notch <= track->x + track->w) {
            fb_fill_rect(state, x, track->y, notch, track->h, ground);
        }
    }
}

void fb_draw_slider(struct mesh_ui_backend_fb_state *state, const struct fb_slider *slider) {
    if (slider == NULL || slider->rect.w <= 0 || slider->rect.h <= 0) {
        return;
    }
    const struct fb_rect box = slider->rect;
    const int damage_pad = fb_space(state, MESH_UI_SPACE_XS);
    fb_animation_damage(state, box.x - damage_pad, box.y - damage_pad, box.w + 2 * damage_pad,
                        box.h + 2 * damage_pad);

    /* The track is a slice of the box the handle has the rest of, centred in it - so the ends of
       the track and the middle of the handle are on one line however tall either is. Recovered
       from the box rather than measured again, because a row may have been given less than it
       asked for and the control has to stay inside what it got. */
    const int full = box.h * 2 / FB_SLIDER_HANDLE_FOCUS;
    const int thickness = full > 1 ? full : 1;
    const struct fb_rect track = {
        .x = box.x, .y = box.y + (box.h - thickness) / 2, .w = box.w, .h = thickness};
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);

    /*
     * Its own ground under a cursor fill, the meter's arrangement and the switch's: the track
     * and the handle are both contracted against the body, and on two of the four themes the
     * cursor fill *is* the resting track - so without this the control disappears on precisely
     * the row it is being edited from.
     *
     * The whole box rather than the track, because the handle stands outside the track and the
     * gaps that separate it from the fill are drawn in this colour.
     */
    if (slider->selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad,
                           radius + pad, ground);
    }

    fb_fill_round_rect(state, track.x, track.y, track.w, track.h, radius,
                       fb_color(state, MESH_UI_COLOR_METER_TRACK));

    /* Narrower than the track is thick, which is the shape Material settled on and the right one
       here for a reason of its own: the handle marks a *position*, and a wide one is a range. */
    const int handle_w = thickness * 3 / 4 > 2 ? thickness * 3 / 4 : 2;
    /* The handle's centre travels the track less its own width, so the control reads as full at
       the last stop and empty at the first instead of hanging off either end. */
    const int travel = track.w - handle_w;

    /* A value the track has no room for: the marks it offers, and nothing claiming to be among
       them. Drawn here because there is no fill to draw them over. */
    if (slider->unplaced) {
        fb_slider_stops(state, &track, handle_w, slider->stops, ground);
        return;
    }

    int32_t target = slider->position;
    if (target < 0) {
        target = 0;
    } else if (target > MESH_UI_ANIM_ONE) {
        target = MESH_UI_ANIM_ONE;
    }
    const int32_t position =
        slider->id != 0U ? mesh_ui_anim_track(&state->anim, slider->id, state->now_ms, target,
                                              fb_motion(state, FB_SLIDER_MOTION), MESH_UI_EASE_OUT)
                         : target;

    const struct mesh_ui_rgb ink = fb_tone_color(state, fb_meter_tone(slider->tone));
    const int handle_x =
        track.x + (travel > 0 ? (int)(((int64_t)travel * position) / MESH_UI_ANIM_ONE) : 0);
    const int active = handle_x - track.x;
    if (active > 0) {
        fb_fill_round_rect(state, track.x, track.y, active, track.h, radius, ink);
    }

    /* After both halves of the track are down, never before either.
     *
     * A notch is a gap cut out of whatever is there, which is the whole reason it needs no ink
     * of its own - and a gap painted before the fill is a gap the fill closes. Drawn early, the
     * stops behind the handle vanished one by one as the value climbed, and at the top of the
     * scale a track that offers a dozen choices showed none of them. Same order fb_draw_meter()
     * cuts a band's boundaries in, for the same reason. */
    fb_slider_stops(state, &track, handle_w, slider->stops, ground);

    /*
     * The handle, and the gap that separates it from the fill it ends.
     *
     * Material leaves that gap and it is not decoration here either: the active track and the
     * handle are one ink, so without it the two are a single shape and the control has no
     * position to read - it is a bar with a bulge. A gap in the ground is what makes the handle a
     * thing sitting *on* the track.
     */
    const int handle_h =
        thickness * (slider->selected ? FB_SLIDER_HANDLE_FOCUS : FB_SLIDER_HANDLE_REST) / 2;
    const int handle_y = box.y + (box.h - handle_h) / 2;
    /* Wide enough to be a gap rather than a seam: the handle and the fill it ends are one ink,
       so this is the whole of what separates them. */
    const int gap = fb_space(state, MESH_UI_SPACE_SM) > 1 ? fb_space(state, MESH_UI_SPACE_SM) : 1;
    fb_fill_rect(state, handle_x - gap, track.y, handle_w + 2 * gap, track.h, ground);
    fb_fill_round_rect(state, handle_x, handle_y, handle_w, handle_h, handle_w / 2, ink);
}

/* ---- the signal staircase ------------------------------------------------------------------ */

void fb_draw_signal(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                    uint8_t level, struct mesh_ui_rgb ink, struct mesh_ui_rgb unlit) {
    if (box == NULL || box->w <= 0 || box->h <= 0) {
        return;
    }
    /*
     * Rungs and the gaps between them out of the width the slot was given, rather than a stated
     * pixel size: this is drawn beside text at whatever glyph scale the theme picked, and a
     * staircase that did not grow with it would be a set of ticks next to large type on the one
     * theme somebody chose for legibility.
     *
     * The gap is taken first and floored at a pixel. A staircase whose rungs touch is a filled
     * block, and a block has no rungs to count - which is the entire content of the widget.
     */
    const int steps = (int)MESH_UI_SIGNAL_STEPS;
    int gap = box->w / (steps * 4);
    if (gap < 1) {
        gap = 1;
    }
    int rung = (box->w - (steps - 1) * gap) / steps;
    if (rung < 1) {
        rung = 1;
    }
    const int bottom = box->y + box->h;
    const int radius = fb_radius(state, MESH_UI_SHAPE_SM);

    for (int i = 0; i < steps; i++) {
        /*
         * Rising left to right, bottom aligned, the shortest rung a quarter of the tallest.
         * Every rung is drawn whether or not it is lit - see FB_TRAILING_SIGNAL: what is being
         * read is lit rungs against a constant total, and an indicator that shortened as the
         * signal fell would be claiming a proportion four buckets cannot support.
         */
        int h = box->h * (i + 1) / steps;
        if (h < 1) {
            h = 1;
        }
        const int x = box->x + i * (rung + gap);
        fb_fill_round_rect(state, x, bottom - h, rung, h, radius, i < (int)level ? ink : unlit);
    }
}

/* ---- the sparkline ------------------------------------------------------------------------- */

int fb_sparkline_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int height = fb_line_adv(state, scale) - scale;
    return height > 1 ? height : 1;
}

/*
 * How thick the line is drawn, and how thick its floor is.
 *
 * A stroke a single pixel wide is what a line on a desktop is and it is the wrong answer on a
 * panel with 1024 pixels across 3.2 inches and no anti-aliasing: a diagonal run of single pixels
 * is a dotted line held at arm's length. The glyph scale is what everything else here grows
 * with, so the stroke grows with it too - a theme picked for legibility gets a legible line
 * rather than the same hairline beside larger type.
 */
static int fb_spark_stroke(int scale) {
    const int stroke = (scale > 0 ? scale : 1) / 2;
    return stroke > 1 ? stroke : 1;
}

/*
 * One segment, drawn a pixel column at a time.
 *
 * Bresenham's is the usual answer and this is not it, deliberately: what a column-wise walk
 * gives that a line rasteriser does not is that consecutive columns are *joined* by
 * construction - each fills from where the last one ended to where this one lands - so a steep
 * segment is a connected stroke rather than a ladder of separated pixels. On a series whose x
 * axis is time, steep is the ordinary case: two readings a minute apart on a line spanning an
 * hour land within a few columns of each other.
 */
static void fb_spark_segment(const struct mesh_ui_backend_fb_state *state, int x0, int y0, int x1,
                             int y1, int stroke, struct mesh_ui_rgb color) {
    if (x1 < x0) {
        const int swap_x = x0, swap_y = y0;
        x0 = x1;
        y0 = y1;
        x1 = swap_x;
        y1 = swap_y;
    }
    const int columns = x1 - x0;
    if (columns == 0) {
        /* Two readings the clock could not separate, or a series projected onto a box narrower
           than it has samples: a vertical connector rather than nothing, so the line still
           passes through both values. */
        const int top = y0 < y1 ? y0 : y1;
        const int bottom = y0 > y1 ? y0 : y1;
        fb_fill_rect(state, x0, top, stroke, bottom - top + stroke, color);
        return;
    }
    int previous = y0;
    for (int i = 0; i <= columns; ++i) {
        const int y = y0 + (int)(((int64_t)(y1 - y0) * i) / columns);
        const int top = y < previous ? y : previous;
        const int bottom = y > previous ? y : previous;
        fb_fill_rect(state, x0 + i, top, stroke, bottom - top + stroke, color);
        previous = y;
    }
}

void fb_draw_sparkline(const struct mesh_ui_backend_fb_state *state,
                       const struct fb_sparkline *spark) {
    if (spark == NULL || spark->points == NULL || spark->rect.w <= 0 || spark->rect.h <= 0) {
        return;
    }
    const struct mesh_ui_polyline *points = spark->points;
    /* One reading is a level, not a trend - and an empty box drawn against a radio that has
       reported once says "nothing is happening", which is the claim the whole component exists
       to avoid making by accident. Nothing is drawn, floor included. */
    if (points->count < 2U) {
        return;
    }

    const struct fb_rect r = spark->rect;
    const int stroke = fb_spark_stroke(state->scale);
    /* The line is placed so that both ends of the domain are inside the box: a reading at the
       top of its scale draws its stroke against the top edge rather than half outside it. */
    const int travel = r.h > stroke ? r.h - stroke : 0;
    const int span = r.w > 1 ? r.w - 1 : 0;

    /*
     * The floor: the bottom of the domain, drawn in the same role the meter's empty track takes
     * - which is what gives a line something to be read against without adding a colour any
     * theme has to be validated for.
     *
     * Under the cursor it takes the row's quiet ink instead, exactly as an unlit rung does and
     * for the same reason: two of the four themes make the track the cursor fill, so a floor in
     * that role would vanish on precisely the row being pointed at.
     */
    const struct mesh_ui_rgb floor_ink = spark->selected
                                             ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                                             : fb_color(state, MESH_UI_COLOR_METER_TRACK);
    fb_fill_rect(state, r.x, r.y + r.h - stroke, r.w, stroke, floor_ink);

    /*
     * The line.
     *
     * Its tone on the ground, and the row's selected ink under the cursor - which is the
     * staircase's rule, not a second one. A family tone is validated against the body and
     * against a card; it is not validated against the cursor fill, and on the contrast theme
     * that fill is white while the primary is yellow, so a stroke drawn in the tone there is a
     * line nobody can see on precisely the row being pointed at. The meter answers this by
     * laying a ground of its own under its track; a line has no track to lay one under - it is
     * a stroke among the row's words - so it takes the pairing those words take.
     */
    const struct mesh_ui_rgb ink = spark->selected
                                       ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                                       : fb_tone_color(state, fb_meter_tone(spark->tone));
    int previous_x = 0;
    int previous_y = 0;
    for (uint32_t i = 0U; i < points->count && i < MESH_UI_SERIES_MAX; ++i) {
        const struct mesh_ui_point *point = &points->items[i];
        const int x = r.x + (int)(((int64_t)point->x * span) / MESH_UI_ANIM_ONE);
        /* Up from the bottom: permille of the domain is a height, and a height on a panel whose
           origin is its top corner is a subtraction. */
        const int y = r.y + travel - (int)(((int64_t)point->y * travel) / MESH_UI_ANIM_ONE);
        if (!point->gap && i > 0U) {
            fb_spark_segment(state, previous_x, previous_y, x, y, stroke, ink);
        }
        previous_x = x;
        previous_y = y;
    }

    /*
     * The newest reading, marked.
     *
     * A line has two ends and nothing about a stroke says which of them is now. On a trend that
     * is the whole reading - a line that falls left to right and one that rises are the same
     * picture read backwards - so the end that is the present carries a square three times the
     * stroke, which is the smallest mark that is still findable against the line it ends.
     */
    const int mark = stroke * 3;
    /* Centred on the stroke's own centre, then held inside the box: the newest reading is at the
       trailing edge by construction, so an uncentred square would hang over whatever the slot
       was measured to keep clear of. */
    int mark_x = previous_x + stroke / 2 - mark / 2;
    int mark_y = previous_y + stroke / 2 - mark / 2;
    if (mark_x > r.x + r.w - mark) {
        mark_x = r.x + r.w - mark;
    }
    if (mark_x < r.x) {
        mark_x = r.x;
    }
    if (mark_y > r.y + r.h - mark) {
        mark_y = r.y + r.h - mark;
    }
    if (mark_y < r.y) {
        mark_y = r.y;
    }
    fb_fill_rect(state, mark_x, mark_y, mark, mark, ink);
}

/* ---- the proportion bar --------------------------------------------------------------------- */

/*
 * The two halves of the seam layout.h keeps with theme.h, held equal where they finally meet.
 *
 * A part is layout's idea and the colour it takes is the theme's, so neither header includes the
 * other and each states its own count - the same split MESH_WAYPOINT_NAME_MAX makes across the
 * store's seam. This is the one translation unit that sees both, so this is where the two are
 * proved to agree, at compile time rather than by a test that has to be remembered.
 */
MESH_UI_STATIC_ASSERT((int)MESH_UI_PROPORTION_PARTS == (int)MESH_UI_SERIES_COLORS,
                      "a composition may have exactly as many parts as there are series colours");

int fb_proportion_thickness(const struct mesh_ui_backend_fb_state *state, int scale) {
    return fb_meter_thickness(state, scale);
}

void fb_draw_proportion(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_proportion *bar) {
    if (bar == NULL || bar->count < 2U || bar->rect.w <= 0 || bar->rect.h <= 0) {
        return;
    }
    const struct fb_rect r = bar->rect;

    int32_t widths[MESH_UI_PROPORTION_PARTS];
    const uint32_t parts = mesh_ui_proportion_split(bar->values, bar->count, r.w, widths);
    if (parts == 0U) {
        /* Nothing was heard at all, so there is no whole to divide. An empty bar here would say
           the parts were all zero, which is a reading; this is the absence of one. */
        return;
    }

    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);
    /* The meter's ground under a cursor fill, for the meter's reason - see `selected`. */
    if (bar->selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad, radius + pad,
                           fb_color(state, MESH_UI_COLOR_BG));
    }

    /*
     * Widest first, each part a pill from the bar's left edge to where that part ends.
     *
     * Not one rectangle per part, which is the obvious way and loses both caps: a plain rect over
     * the last part squares off the round end the bar shares with every other bar on the card,
     * and the first part's round end has nothing to sit in. Drawn this way the outermost fill
     * lays the right-hand cap, each narrower one lands on top with its own left-hand cap in the
     * same place, and the part drawn last owns the left end - so the two ends of the bar are the
     * meter's ends and the boundaries between parts are the only new edges on it.
     *
     * At this radius - a pill on a bar a few pixels tall clamps to a pixel or two - a boundary
     * comes out as a softened vertical edge rather than as a visible bulge.
     */
    int end = r.w;
    for (uint32_t i = parts; i-- > 0U;) {
        if (widths[i] > 0 && end > 0) {
            fb_fill_round_rect(state, r.x, r.y, end, r.h, radius,
                               mesh_ui_theme_series(state->theme, i));
        }
        end -= widths[i];
    }

    /*
     * And a gap cut at each boundary, in the ground the bar is drawn on.
     *
     * The meter's band notches, doing the same job one level along: two parts of a composition
     * are two fills meeting with nothing between them, and the palette only promises they are
     * 1.4:1 apart - which is a difference the eye finds reliably when there is an edge to find it
     * at, and less reliably across a seam it has to decide is there. A gap is that edge, and it
     * is drawn in the absence of ink for the reason the notches are: an ink of its own would be
     * one more pair every theme had to be validated for, to say what a hole already says.
     *
     * In the caller's ground rather than in MESH_UI_COLOR_BG, which is where this differs from
     * the band notch it is otherwise copying. A notch divides a bar the eye has already found;
     * these gaps have to be *invisible*, and a bar on a card whose gaps are the body's colour has
     * stripes in it rather than divisions. Under a cursor fill it is the pad above that is
     * behind the bar, so that is what the gaps take there.
     *
     * It costs each part half a pixel of length at one end. That is the same price the band marks
     * pay and it is the right way round: the boundary is what the picture is *for*.
     */
    const int gap = fb_space(state, MESH_UI_SPACE_XS) > 0 ? fb_space(state, MESH_UI_SPACE_XS) : 1;
    const struct mesh_ui_rgb ground =
        bar->selected ? fb_color(state, MESH_UI_COLOR_BG) : bar->ground;
    int boundary = 0;
    for (uint32_t i = 0; i + 1U < parts; ++i) {
        boundary += widths[i];
        if (widths[i] == 0) {
            continue; /* a part that is not there has no edge of its own */
        }
        int x = r.x + boundary - gap / 2;
        int w = gap;
        /* Held inside the bar, so the gap at the last boundary cannot eat the round end. */
        if (x < r.x) {
            w -= r.x - x;
            x = r.x;
        }
        if (x + w > r.x + r.w) {
            w = r.x + r.w - x;
        }
        if (w > 0) {
            fb_fill_rect(state, x, r.y, w, r.h, ground);
        }
    }
}

/* ---- the chart ------------------------------------------------------------------------------ */

/* The two lines of chrome under the plot: what the horizontal covers, then what the lines are.
   Both are the axis - a picture whose axes are unnamed is the sparkline, which is a different
   component with a different job. */
#define FB_CHART_FOOTER_LINES 2

/*
 * How thick a line on a chart is, which is deliberately not the sparkline's stroke.
 *
 * A series colour promises 1.4:1 against the grounds and against the other series, and that is a
 * *fill's* contract - it was measured on a bar several pixels tall, and it is the reason
 * MESH_UI_SERIES_COLORS may never be used as an ink. A stroke half the glyph scale wide is not a
 * fill; at the contrast the palette guarantees, a hairline in one of these colours is a line the
 * reader has to hunt for on the very theme that exists so nobody has to.
 *
 * So a chart's lines are drawn at least twice as thick as a row's, which is what a chart has the
 * room for and what makes the colour the palette was validated for the colour that is actually
 * on the panel.
 */
static int fb_chart_stroke(int scale) {
    const int stroke = scale > 0 ? scale : 1;
    return stroke > 2 ? stroke : 2;
}

/* The hairline the axis and the threshold rules are drawn at: a mark rather than a reading, so
   it is as thin as this panel can draw and still be seen. */
static int fb_chart_rule(int scale) {
    const int rule = (scale > 0 ? scale : 1) / 2;
    return rule > 1 ? rule : 1;
}

int fb_chart_min_height(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout) {
    (void)state;
    /* The chrome, and a plot at least as tall again as the chrome under it. Below that the
       picture is shorter than its own caption, which reads as a rendering fault rather than as a
       small chart. */
    return layout->line * (FB_CHART_FOOTER_LINES * 2);
}

/*
 * One threshold, drawn across the plot as a broken rule.
 *
 * Broken rather than solid, and that is the whole of what tells it from the axis and from the
 * data: a chart with three solid horizontals on it has three things that look like readings. The
 * meter answers the same question by cutting a notch in its own track, which is a gap in a thing
 * the eye has already found; there is no track here to cut, so the mark has to be visibly a mark.
 */
static void fb_chart_threshold(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_rect *plot, int travel, int32_t permille, int rule,
                               struct mesh_ui_rgb ink) {
    if (permille < 0 || permille > MESH_UI_ANIM_ONE) {
        /* Outside the domain the lines are drawn on. Nothing is clamped to an edge here: a
           threshold pinned to the top of a chart is a threshold the trend can never be seen
           crossing, which is worse than one the reader can see is off the picture. */
        return;
    }
    const int y = plot->y + travel - (int)(((int64_t)permille * travel) / MESH_UI_ANIM_ONE);
    const int dash = rule * 3;
    for (int x = plot->x; x < plot->x + plot->w; x += dash * 2) {
        const int w = (x + dash > plot->x + plot->w) ? plot->x + plot->w - x : dash;
        fb_fill_rect(state, x, y, w, rule, ink);
    }
}

/*
 * The legend: a swatch and a word per line, in the order the lines were handed over.
 *
 * The swatch carries the colour and the word is in the body's own ink, which is the rule a
 * series colour never gets out of - it is a fill, so it fills a square, and the text beside it is
 * text. Naming the parts in their own colours is what every spreadsheet does and it is four more
 * contrast pairs per theme, to say what a swatch already says.
 */
static void fb_chart_legend(const struct mesh_ui_backend_fb_state *state,
                            const struct fb_layout *layout, const struct fb_chart *chart, int x,
                            int y) {
    const int scale = layout->small;
    const int adv = fb_char_adv(state, scale);
    const int cap = mesh_ui_font_cap(fb_font(state), scale);
    const int line = fb_line_adv(state, scale);
    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);

    for (uint32_t i = 0U; i < chart->count && i < FB_CHART_LINES; ++i) {
        const enum mesh_str_id label = chart->lines[i].label;
        if (label == MESH_STR_NONE) {
            continue;
        }
        const char *word = mesh_str(label);
        const int width = cap + adv + (int)mesh_ui_text_cells(word) * adv;
        if (x + width > chart->rect.x + chart->rect.w) {
            /* Out of line. The entry is dropped whole rather than cut, for the reason a bubble's
               trailing run drops a chip rather than truncating one: half a word beside a colour
               is a legend that names the wrong thing, and the reader has no way to tell. */
            return;
        }
        /* Vertically centred on the capitals beside it rather than on the cell, which is the
           icon slot's rule - a square sized to the cell stands a seventh taller than the word it
           is labelling on any face with real descenders. */
        fb_fill_round_rect(state, x, y + (line - cap) / 2, cap, cap,
                           fb_radius(state, MESH_UI_SHAPE_SM),
                           mesh_ui_theme_series(state->theme, i));
        fb_draw_text(state, x + cap + adv, y, word, scale, ink, ground);
        x += width + adv * 2;
    }
}

void fb_draw_chart(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_chart *chart) {
    if (chart == NULL || chart->rect.w <= 0 || chart->rect.h <= 0) {
        return;
    }
    if (chart->rect.h < fb_chart_min_height(state, layout)) {
        return; /* see fb_chart_min_height(): there is no clipped chart */
    }

    const int scale = layout->small;
    const int adv = fb_char_adv(state, scale);
    const int rule = fb_chart_rule(state->scale);

    /*
     * The room the vertical's two ends want, taken off the left before anything is placed.
     *
     * Measured from the labels themselves rather than reserved as a fixed column: this is the
     * only screen on the panel, so a chart of percentages should not be inset as far as one of
     * five-digit counts. The gap after them is one cell, which is the same gap a list row leaves
     * between its label column and its value.
     */
    const size_t top_cells = chart->top != NULL ? mesh_ui_text_cells(chart->top) : 0U;
    const size_t bottom_cells = chart->bottom != NULL ? mesh_ui_text_cells(chart->bottom) : 0U;
    const size_t axis_cells = top_cells > bottom_cells ? top_cells : bottom_cells;
    const int gutter = axis_cells > 0U ? (int)(axis_cells + 1U) * adv : 0;

    struct fb_rect plot = {
        .x = chart->rect.x + gutter,
        .y = chart->rect.y,
        .w = chart->rect.w - gutter,
        .h = chart->rect.h - FB_CHART_FOOTER_LINES * layout->line,
    };
    if (plot.w <= 0 || plot.h <= rule) {
        return;
    }

    const struct mesh_ui_rgb furniture = fb_color(state, MESH_UI_COLOR_METER_TRACK);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);

    /*
     * The frame: the two axes and nothing else.
     *
     * Two rather than four, because the two that are not drawn would be saying something. A rule
     * along the top of a chart reads as the domain's ceiling and this one has a label saying
     * where that is; a rule up the right-hand edge reads as the present, which is where the
     * lines end anyway. What is left is the pair that say "measured from here".
     */
    const int interior = plot.h - rule;
    fb_fill_rect(state, plot.x, plot.y, rule, plot.h, furniture);
    fb_fill_rect(state, plot.x, plot.y + interior, plot.w, rule, furniture);

    /*
     * The height a reading travels over, which is the interior less the stroke - so a reading at
     * the top of its domain draws its whole line inside the plot rather than half outside it.
     * The sparkline's arithmetic, with a thicker pen.
     */
    const int stroke = fb_chart_stroke(state->scale);
    const int travel = interior > stroke ? interior - stroke : 0;
    const int span = plot.w > 1 ? plot.w - 1 : 0;

    /* The thresholds, under the lines: a mark the data can be seen crossing has to be behind it,
       or the mark is what is on top of the reading. */
    if (chart->band != NULL) {
        fb_chart_threshold(state, &plot, travel,
                           mesh_ui_scale_permille(chart->scale, chart->band->warn), rule,
                           furniture);
        fb_chart_threshold(state, &plot, travel,
                           mesh_ui_scale_permille(chart->scale, chart->band->bad), rule, furniture);
    }

    /* The two ends of the vertical, against the plot's own top and bottom. */
    const int label_line = fb_line_adv(state, scale);
    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
    if (chart->top != NULL) {
        fb_draw_text(state, plot.x - (int)(top_cells + 1U) * adv, plot.y, chart->top, scale, ink,
                     ground);
    }
    if (chart->bottom != NULL) {
        fb_draw_text(state, plot.x - (int)(bottom_cells + 1U) * adv, plot.y + interior - label_line,
                     chart->bottom, scale, ink, ground);
    }

    /*
     * The lines, in the order they were handed over, each in the series colour of its position.
     *
     * By position rather than by anything about the data, which is the palette's whole contract:
     * slice 0 is the same colour on every frame and every theme, so the legend under the plot
     * goes on meaning what it said the last time this screen was opened.
     */
    for (uint32_t i = 0U; i < chart->count && i < FB_CHART_LINES; ++i) {
        const struct mesh_ui_polyline *points = chart->lines[i].points;
        if (points == NULL || points->count < 2U) {
            continue; /* one reading is a level; the sparkline's rule, unchanged */
        }
        const struct mesh_ui_rgb colour = mesh_ui_theme_series(state->theme, i);
        int previous_x = 0;
        int previous_y = 0;
        for (uint32_t j = 0U; j < points->count && j < MESH_UI_SERIES_MAX; ++j) {
            const struct mesh_ui_point *point = &points->items[j];
            const int x = plot.x + (int)(((int64_t)point->x * span) / MESH_UI_ANIM_ONE);
            const int y = plot.y + travel - (int)(((int64_t)point->y * travel) / MESH_UI_ANIM_ONE);
            if (!point->gap && j > 0U) {
                fb_spark_segment(state, previous_x, previous_y, x, y, stroke, colour);
            }
            previous_x = x;
            previous_y = y;
        }
    }

    /*
     * And the chrome under it: what the horizontal covers, then what the lines are.
     *
     * The span is centred on the plot rather than tucked under either end, because it names the
     * whole axis rather than a point on it - "last 45m" under the left-hand end reads as a label
     * for that end, which is the one place on the axis it is not true of.
     */
    int y = plot.y + plot.h;
    if (chart->span != NULL) {
        const int width = (int)mesh_ui_text_cells(chart->span) * adv;
        const int x = plot.x + (plot.w - width) / 2;
        fb_draw_text(state, x > plot.x ? x : plot.x, y, chart->span, scale, ink, ground);
    }
    y += layout->line;
    fb_chart_legend(state, layout, chart, plot.x, y);
}

/* ---- the text field ------------------------------------------------------------------------ */

/* The box's own height, without the label over it or the counter under it. */
static int fb_text_field_box_h(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_layout *layout, const struct fb_text_field *field) {
    const uint32_t lines = field->lines > 0U ? field->lines : 1U;
    return (int)lines * layout->line + state->scale;
}

/* The label's line, and the counter's. Both are chrome: they report on the field rather than
   being part of what is in it, so they are drawn at the smaller scale the footer and the tab
   strip use. Zero when the field carries neither. */
static int fb_text_field_label_h(const struct mesh_ui_backend_fb_state *state,
                                 const struct fb_layout *layout,
                                 const struct fb_text_field *field) {
    return (field->label != NULL && field->label[0] != '\0') ? fb_line_adv(state, layout->small)
                                                             : 0;
}

static int fb_text_field_counter_h(const struct mesh_ui_backend_fb_state *state,
                                   const struct fb_layout *layout,
                                   const struct fb_text_field *field) {
    return (field->counter != NULL && field->counter[0] != '\0') ? fb_line_adv(state, layout->small)
                                                                 : 0;
}

int fb_text_field_height(const struct mesh_ui_backend_fb_state *state,
                         const struct fb_layout *layout, const struct fb_text_field *field) {
    if (field == NULL) {
        return 0;
    }
    return fb_text_field_label_h(state, layout, field) + fb_text_field_box_h(state, layout, field) +
           fb_text_field_counter_h(state, layout, field) + state->scale;
}

void fb_draw_text_field(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, int *y, const struct fb_text_field *field) {
    if (field == NULL || y == NULL) {
        return;
    }
    const int scale = state->scale;
    const int margin = fb_margin(state);
    const int box_x = fb_gutter(state);
    const int box_w = (int)state->var.xres - margin;
    const int box_h = fb_text_field_box_h(state, layout, field);
    const uint32_t lines = field->lines > 0U ? field->lines : 1U;
    int top = *y;

    if (fb_text_field_label_h(state, layout, field) > 0) {
        fb_draw_text(state, margin, top, field->label, layout->small,
                     fb_tone_color(state, field->error ? MESH_UI_TONE_ERROR : MESH_UI_TONE_DIM),
                     fb_color(state, MESH_UI_COLOR_BG));
        top += fb_text_field_label_h(state, layout, field);
    }

    /*
     * The edge first and the fill inside it, exactly as a card is built: there is no stroke
     * primitive here, and a rounded outline is the shape drawn once in the edge colour with the
     * shape drawn again a hairline smaller over the top of it.
     */
    const int edge = fb_edge(state);
    const int radius = fb_radius(state, MESH_UI_SHAPE_MD);
    fb_fill_round_rect(state, box_x, top, box_w, box_h, radius + edge,
                       fb_color(state, field->error ? MESH_UI_COLOR_ERROR : MESH_UI_COLOR_OUTLINE));
    fb_fill_round_rect(state, box_x + edge, top + edge, box_w - 2 * edge, box_h - 2 * edge, radius,
                       fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));

    /*
     * The value, with the caret on the end of it, showing the *tail* when it is longer than the
     * box holds: the caret is where the next press lands, so it is the end that has to stay on
     * screen. Measured and cut in cells, so a draft of emoji scrolls a glyph at a time rather
     * than splitting one down the middle.
     */
    char shown[MESH_UI_LINE_MAX];
    snprintf(shown, sizeof shown, "%s%s", field->value != NULL ? field->value : "",
             field->caret ? "_" : "");
    const size_t visible = layout->cols * (size_t)lines;
    const char *tail = shown;
    const size_t width = mesh_ui_text_cells(shown);
    if (width > visible) {
        tail = shown + mesh_ui_text_cell_offset(shown, width - visible);
    }
    /* The value sits a scale down from the box's own top edge, which is the inset every other
       container here gives its contents. */
    fb_draw_wrapped(state, top + scale, tail, layout->cols, (int)lines,
                    fb_tone_color(state, MESH_UI_TONE_STRONG),
                    fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));
    top += box_h;

    if (fb_text_field_counter_h(state, layout, field) > 0) {
        /* Against the box's own trailing edge rather than the body's - it belongs to the field,
           and a figure that does not line up with the container it reports on reads as loose. */
        const int adv = fb_char_adv(state, layout->small);
        const int x = box_x + box_w - (int)mesh_ui_text_cells(field->counter) * adv;
        fb_draw_text(state, x, top, field->counter, layout->small,
                     fb_tone_color(state, field->error ? MESH_UI_TONE_ERROR : MESH_UI_TONE_DIM),
                     fb_color(state, MESH_UI_COLOR_BG));
        top += fb_text_field_counter_h(state, layout, field);
    }

    *y = top + scale;
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
