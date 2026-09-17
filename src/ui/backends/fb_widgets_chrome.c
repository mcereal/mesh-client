#define _POSIX_C_SOURCE 200809L

/*
 * The chrome: the navigation bar, the action bar, the app bar and its trail, the empty state
 * under it, the hairline, and the banner and progress bar that drop in below the title.
 *
 * The bars are strips of fb_widgets_button.h's shapes given a place on the panel - which is the
 * whole of the split: what a chip looks like is one question, and whether it belongs at the top
 * of the screen or the bottom is another.
 */

#include "fb_widgets_chrome.h"
#include "fb_widgets_button.h"
#include "fb_widgets_meter.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/emoji.h"
#include "mesh/utils/text.h"

/* ---- the navigation bar --------------------------------------------------------------------- */

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

/* ---- the action bar ------------------------------------------------------------------------- */

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

void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped) {
    if (dropped > 0U) {
        mesh_str_format(out, out_len, MESH_STR_LIST_TITLE_COUNT_OLDER, name, count, dropped);
    } else {
        mesh_str_format(out, out_len, MESH_STR_LIST_TITLE_COUNT, name, count);
    }
}
