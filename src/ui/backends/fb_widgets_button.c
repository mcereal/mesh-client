#define _POSIX_C_SOURCE 200809L

/*
 * The button, the chip strip and the badge. Everything here is built from fb_draw.c's
 * primitives and knows nothing about the snapshot: a button takes what it draws, not where it
 * came from, which is what lets one component serve every screen that wants one.
 */

#include "fb_widgets_button.h"

#include "mesh/ui/emoji.h"

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

/*
 * The sprite a button draws as its whole face, when it has one.
 *
 * "One emoji and nothing else" is the test, not "starts with an emoji": a label with anything
 * after the sprite is a line of text that happens to open with a picture, and it is measured
 * and centred as text like any other.
 */
static bool fb_button_face_sprite(const struct fb_button *button, uint16_t *sprite) {
    if (!button->emoji_face || mesh_ui_icon_is_valid(button->icon) || button->label == NULL) {
        return false;
    }
    const struct mesh_ui_text_cell cell = mesh_ui_text_cell_next(button->label);
    if (!cell.is_emoji || button->label[cell.bytes] != '\0') {
        return false;
    }
    *sprite = cell.sprite;
    return true;
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

    /* Square and centred, inset by the padding a key already leaves around its label, so the
       sprite stops short of the fill's rounded corners on the smaller of the two axes. */
    uint16_t face = 0;
    if (fb_button_face_sprite(button, &face)) {
        const int shorter = button->rect.w < button->rect.h ? button->rect.w : button->rect.h;
        const int box = fb_emoji_box_fit(shorter - 2 * fb_space(state, MESH_UI_SPACE_MD));
        fb_draw_emoji_box(state, button->rect.x + (button->rect.w - box) / 2,
                          button->rect.y + (button->rect.h - box) / 2, box, face);
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

/* ---- the chip strip ------------------------------------------------------------------------ */

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

/* The capsule itself: a pill of `paint.fill` with the words on it in `paint.ink`. Both callers
   below are this with a different pair, which is the whole of what tells a badge from a chip -
   so the shape, the radius and the half-cell inset are stated once. */
static void fb_fill_capsule_text(const struct mesh_ui_backend_fb_state *state,
                                 const struct fb_rect *box, int text_y, const char *text,
                                 struct mesh_ui_paint paint, int scale) {
    if (text == NULL || text[0] == '\0' || box->w <= 0) {
        return;
    }
    fb_fill_round_rect(state, box->x, box->y, box->w, box->h, fb_radius(state, MESH_UI_SHAPE_FULL),
                       paint.fill);
    fb_draw_text(state, box->x + fb_char_adv(state, scale) / 2, text_y, text, scale, paint.ink,
                 paint.fill);
}

void fb_draw_badge(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                   int text_y, const char *text, enum mesh_ui_family family, int scale) {
    /* One call for both halves: whatever the theme says reads on that family's own fill - on
       the dark palette that is the ground colour, because white on its yellow is unreadable at
       this glyph size. */
    fb_fill_capsule_text(state, box, text_y, text,
                         fb_paint(state, family, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST), scale);
}

void fb_draw_state_chip(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                        int text_y, const char *text, enum mesh_ui_tone tone,
                        enum mesh_ui_color ground, struct mesh_ui_rgb ink, int scale) {
    const enum mesh_ui_family family = mesh_ui_tone_family(tone);
    if (family != MESH_UI_FAMILY_COUNT) {
        /* The container at rest and the family's full strength on the cursor's own fill, which
           is the leading disc's rule one screen up and fb_button_paint()'s for a tonal control.
           `ground` is what says which: a container and the cursor fill are both quiet fills on
           the body ground and therefore near each other, so a chip that kept its container
           there would be a state that disappears on the row being pointed at. */
        const bool on_cursor =
            ground == MESH_UI_COLOR_SURFACE_SEL || ground == MESH_UI_COLOR_SURFACE_ACTIVE;
        fb_fill_capsule_text(state, box, text_y, text,
                             fb_paint(state, family,
                                      on_cursor ? MESH_UI_SLOT_BASE : MESH_UI_SLOT_CONTAINER,
                                      MESH_UI_STATE_REST),
                             scale);
        return;
    }
    if (text == NULL || text[0] == '\0' || box->w <= 0) {
        return;
    }
    /*
     * The neutral one, drawn as a ring rather than a fill, and that is the design rather than a
     * workaround for a palette.
     *
     * "The state it is normally in" is not one of the six things a family means, so there is no
     * container to fill it with; the nearest neutral fill is the cursor surface, and on the
     * light theme that sits 1.18:1 from a card - a pill nobody can see. An edge can be found on
     * every theme by contract (it is what says a card is there at all), and it says the right
     * thing besides: a filled chip is a state worth reporting and an outlined one is a state
     * worth *checking*, which is Material's own distinction between the two and the difference
     * between "verified" and "not verified" being two pills of equal weight.
     *
     * Two fills rather than a stroke, on fb_draw_card()'s terms: the ring, then the row's own
     * ground inset by one hairline. The words keep the row's ink, because the inside of the
     * capsule is the same colour they were already legible on.
     */
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);
    const int edge = fb_edge(state);
    const struct mesh_ui_rgb fill = fb_color(state, ground);
    fb_fill_round_rect(state, box->x, box->y, box->w, box->h, radius,
                       fb_color(state, MESH_UI_COLOR_OUTLINE));
    if (box->w > 2 * edge && box->h > 2 * edge) {
        fb_fill_round_rect(state, box->x + edge, box->y + edge, box->w - 2 * edge,
                           box->h - 2 * edge, radius, fill);
    }
    fb_draw_text(state, box->x + fb_char_adv(state, scale) / 2, text_y, text, scale, ink, fill);
}
