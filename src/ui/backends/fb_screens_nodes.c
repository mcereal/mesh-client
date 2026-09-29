#define _POSIX_C_SOURCE 200809L

/*
 * The Nodes tab: the roster, and one node's detail over it.
 *
 * Two levels of the same list-of-rows shape the Settings tab draws, so the two screens scroll
 * and clip identically. The two screens this tab can also be showing are files of their own for
 * the reason each gives: the map is fb_map.c, because it places things at coordinates rather
 * than describing rows, and a node's chart is fb_screens_chart.c, beside the Status tab's chart
 * it is the twin of.
 */

#include "inkcell/ui/layout.h"
#include "inkcell/ui/widgets.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/chrome.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/history.h"
#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/nodes.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/trust.h"
#include "mesh/ui/units.h"

#include <stdio.h>
#include <string.h>

/*
 * One node's detail: the same list-of-rows shape the Settings tab draws, so the two screens
 * scroll and clip identically. Headings are dimmed and get no value column; the action row ends
 * in the chevron a settings row that opens something ends in, for the same reason - it is the
 * only thing on the screen A does anything to.
 */
/*
 * The detail's app bar and the sheet's trail both ask mesh_ui_node_title(). One answer rather than
 * two, because the trail names the screen the sheet is *inside* and a second spelling of it would
 * be a breadcrumb that disagreed with the bar it leads back to.
 */
static void fb_node_title(const struct mesh_ui_node_summary *node, char *out, size_t out_len) {
    mesh_ui_node_title(node, node->node_id, out, out_len);
}

/*
 * One verb, drawn.
 *
 * Shared by the two screens that hold verbs - the detail's single row that opens the sheet, and
 * every row of the sheet itself - because they are the same row. A sheet whose verbs were drawn
 * by a second copy of this would be a second opinion about what a destructive row looks like,
 * which is the drift node_detail.c's `tone` and `icon` fields exist to prevent one layer down.
 */
static void fb_node_action_row(struct inkcell_draw_state *state, struct inkcell_fb_list *list,
                               uint32_t index, const struct mesh_ui_node_item *item,
                               uint32_t node_id) {
    /*
     * What the row is about, on its leading edge, and what it costs, in its ink - both
     * read off the item rather than decided here, which is the whole of why
     * node_detail.c grew the two tables. A boolean says its state with the switch the
     * settings rows use instead of spelling "Yes" into the value column; everything
     * else keeps the chevron that means "this row does something".
     */
    /*
     * Keyed on the node and the verb, never on the row.
     *
     * The animation table is twelve slots reused by least-recently-touched, so an id is
     * a claim that two draws are the *same control* - which a row index is not. Closing
     * a pinned node and opening an unpinned one lands the second node's pin row on the
     * first node's slot at the same `i`, so its knob starts where the other node's was
     * and slides across on the frame the screen opens: a control announcing a change
     * nobody made. A traceroute completing under an open detail does it the other way,
     * inserting rows and moving the mute and ignore switches onto each other's slots.
     *
     * The verb is unique within the frame - a node offers each of the three at most
     * once - and the node is what makes two nodes' switches different controls, which
     * is the pair `struct inkcell_fb_switch` asks for. The id is folded rather than truncated
     * so two node numbers agreeing in their low bits are not one control; it sits above
     * everything the settings fields and this screen's meters can reach.
     */
    const uint32_t node_key = (node_id ^ (node_id >> 20)) & 0x000FFFFFU;
    struct inkcell_fb_switch sw = {
        .id = 0x05000000U | ((uint32_t)item->action << 20) | node_key,
        .family = item->tone == (uint8_t)INKCELL_TONE_WARNING ? INKCELL_FAMILY_WARNING
                                                              : INKCELL_FAMILY_PRIMARY,
        .on = item->on,
    };
    /*
     * The disc rather than a bare icon, and it is what took the accent off the words.
     *
     * Every verb here used to draw in the primary, so a card of eleven of them was a
     * wall of one colour with the destructive row somewhere in it. The colour is still
     * on the row - it is what a verb is about, and the eye finds "Remove" by its red
     * long before it reads the word - but it is in a container at the leading edge,
     * where Material puts it and where it does not compete with the label beside it.
     * Which family the disc wears is the row's own tone, read by the component; see
     * INKCELL_FB_LEADING_TONAL.
     */
    const struct inkcell_fb_list_item row = {
        .leading = {.kind = INKCELL_FB_LEADING_TONAL, .icon = (enum inkcell_icon)item->icon},
        .text = item->label,
        .tone = (enum inkcell_tone)item->tone,
        .trailing = item->toggle ? (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_SWITCH,
                                                                .sw = &sw}
                                 : (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_ICON,
                                                                .icon = INKCELL_ICON_CHEVRON},
        /* A row whose press cannot be walked back gets the leading bar as well as the
           ink, in its own tone - the accent edge is drawn in the row's family, so the
           one row on the screen that deletes something is the one row marked in red on
           both of its edges. */
        .label_plain = true,
        .accent_edge = item->tone == (uint8_t)INKCELL_TONE_ERROR,
    };
    inkcell_fb_list_item(state, list, index, &row);
}

/*
 * How wide one column of a node's facts has to be, in body columns, before the detail stands in
 * two: the widest label this screen asks, a value beside it and a reading's trend against the
 * edge. The same unit and the same reasoning as the Status cards' (FB_STATUS_COLUMN_COLS) - a
 * window with the text turned up keeps one column for exactly as long as two would wrap - and
 * smaller, because a node's rows are a short label and a short answer where a card's are
 * sentences.
 */
#define FB_NODE_COLUMN_COLS 30U

/*
 * Whether this frame has room for the detail in two columns, and where they are: the pane split
 * down the middle.
 *
 * Never on a compact frame, so the Brick draws the one column it always has - and the frame is
 * asked rather than the pane, because the detail pane of a split window is itself a compact
 * region: judged by its own width it is the Brick's panel again, give or take two columns, and
 * the column count alone cannot tell the two apart. What can is that one of them is a pane
 * beside a list on a wide window and the other is the whole of a handheld.
 */
static bool fb_node_detail_columns(struct inkcell_draw_state *state,
                                   struct inkcell_box columns[2]) {
    const struct inkcell_box region = inkcell_fb_region(state);
    const int half = region.w / 2;
    if (!fb_frame_is_split(state) && inkcell_fb_width_class(state) == INKCELL_WIDTH_COMPACT) {
        return false;
    }
    columns[0] = (struct inkcell_box){region.x, region.y, half, region.h};
    columns[1] = (struct inkcell_box){region.x + half, region.y, region.w - half, region.h};
    const struct inkcell_box was = inkcell_fb_set_region(state, columns[0]);
    const bool room = inkcell_fb_cols(state, state->scale) >= FB_NODE_COLUMN_COLS;
    (void)inkcell_fb_set_region(state, was);
    return room;
}

/*
 * Where the second column starts: the heading that comes nearest to halving the rows' steps, so
 * the two columns end as close to level as whole cards allow. Always at a heading, because a card
 * split across two columns would be one subject in two places. 0 when no heading past the first
 * row exists, which is one column.
 */
static uint32_t fb_node_detail_split(const struct mesh_ui_node_item *items, const uint8_t *heights,
                                     uint32_t count) {
    uint32_t total = 0U;
    for (uint32_t r = 0U; r < count; ++r) {
        total += heights[r];
    }
    uint32_t best = 0U;
    uint32_t best_gap = UINT32_MAX;
    uint32_t above = 0U;
    for (uint32_t r = 0U; r < count; ++r) {
        if (r > 0U && items[r].kind == MESH_UI_NODE_ROW_HEADING) {
            const uint32_t gap = 2U * above > total ? 2U * above - total : total - 2U * above;
            if (gap < best_gap) {
                best = r;
                best_gap = gap;
            }
        }
        above += heights[r];
    }
    return best;
}

/*
 * One row of the detail. `i` is its place in the list being drawn and `item_index` its place
 * among the node's items, which differ in the second column - the meter's id is the item's, so a
 * reading keeps its animation when the detail moves between one column and two.
 */
static void fb_node_detail_row(struct inkcell_draw_state *state, struct inkcell_fb_list *list,
                               uint32_t i, uint32_t item_index,
                               const struct mesh_ui_node_item *item, uint32_t node_id,
                               size_t label_cols);

/*
 * The rows [from, to) as one list against `layout`, the cursor's list when the cursor is among
 * them. A list the cursor is not in still opens on a cursor - the model always has one - so it
 * opens at its top and is drawn under inkcell_fb_set_cursor_elsewhere(), which keeps its first
 * row from lighting up as though the keys were there. Only the cursor's list glides: there is one
 * glide slot, and the other column does not move.
 */
static void fb_node_detail_list(struct inkcell_draw_state *state,
                                const struct inkcell_fb_layout *layout,
                                const struct mesh_ui_node_item *items, uint32_t from, uint32_t to,
                                const uint8_t *heights, const uint8_t *cards, uint32_t cursor,
                                const struct mesh_ui_node_span *span, uint32_t node_id,
                                size_t label_cols, enum fb_list_id glide) {
    const uint32_t count = to - from;
    const bool here = cursor >= from && cursor < to;
    const uint32_t at = here ? cursor - from : 0U;
    const bool spanned = here && span->first >= from && span->last < to;
    struct inkcell_fb_list list = inkcell_fb_list_begin_focus(
        layout, count, at, heights + from, cards + from, spanned ? span->first - from : at,
        spanned ? span->last - from : at, spanned && span->card);
    bool was = false;
    if (here) {
        inkcell_fb_list_glide(state, &list, glide);
    } else {
        was = inkcell_fb_set_cursor_elsewhere(state, true);
    }
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        fb_node_detail_row(state, &list, i, from + i, &items[from + i], node_id, label_cols);
    }
    if (!here) {
        (void)inkcell_fb_set_cursor_elsewhere(state, was);
    }
}

/*
 * A layout for one column: the frame's rows and top, with the column's own width - so the label
 * column is fitted to what the column holds rather than to the pane. Asked with the region
 * narrowed to the column, which is also where the rows will be placed.
 */
static struct inkcell_fb_layout fb_node_column_layout(struct inkcell_draw_state *state,
                                                      const struct inkcell_fb_layout *layout,
                                                      struct inkcell_box column) {
    const struct inkcell_box was = inkcell_fb_set_region(state, column);
    struct inkcell_fb_layout out = *layout;
    out.cols = inkcell_fb_cols(state, state->scale);
    out.body_w = inkcell_fb_content_column(state).w;
    (void)inkcell_fb_set_region(state, was);
    return out;
}

/* Mutable state, as every screen drawing a inkcell_fb_list_item is: an item may carry a control
   that animates, and where such a control has got to is kept on the backend. */
void fb_render_node_detail(struct inkcell_draw_state *state,
                           const struct mesh_ui_snapshot *snapshot,
                           struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, nav->node_detail_node);
    if (node == NULL) {
        fb_draw_app_bar(
            state, layout,
            &(const struct inkcell_fb_app_bar){.title = inkcell_str(MESH_STR_TAB_NODES)});
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_NODES, inkcell_str(MESH_STR_NODES_GONE));
        return;
    }

    const bool is_self = hs->has_my_info && node->node_id == hs->my_info.node_num;
    /*
     * Two columns or one, decided before the heading is drawn. A node's facts are label and value
     * rows read a line at a time, not running text, so on a frame with room the reading measure
     * is the wrong cap - it left a pane two thirds empty beside a column of short answers - and
     * the room is for a second column instead, as the Status cards have. With two the measure is
     * off for the heading too, so it stands over both columns, and fb_render_snapshot() puts it
     * back once the action bar under them is drawn.
     */
    struct inkcell_box columns[2];
    const bool wide = fb_node_detail_columns(state, columns);
    if (wide) {
        (void)inkcell_fb_set_measured(state, false);
    }
    char title[96];
    fb_node_title(node, title, sizeof title);
    /*
     * "Nodes > %s" was a breadcrumb inside a translated string, and the "Nodes >" half of it
     * was the navigation bar's job all along: the tab is up there, selected, three rows above.
     * So the trail is empty and the node keeps the whole title line for a name the radio chose
     * and we cannot bound. What the breadcrumb was really carrying - that B leaves - is the
     * leading arrow now, which says it in a cell rather than in seven.
     */
    /*
     * And, in the heading's badge, the one thing that is true of the whole node: how long ago
     * anything was heard from it.
     *
     * It is on the "Signal" group's first row too, and that is the point rather than a
     * duplication - this screen is a hundred and twenty rows long and that row is below the
     * fold from the moment the reader starts walking, while "is this node still there?" is the
     * question every other row on the screen is qualified by. The app bar does not scroll, so
     * the qualifier does not either. Same rule the Settings bar's unsaved badge follows: one
     * fact, about the screen rather than about a row.
     *
     * Our own node gets none. Nothing heard it - it is us - so an age there would be this
     * client reporting how long ago it last spoke to itself.
     */
    char heard[24];
    heard[0] = '\0';
    if (!is_self && node->last_heard != 0U) {
        /* The same shorthand the Nodes list puts against the row this was opened from, so the
           two screens cannot report the node's age in two different spellings. */
        inkcell_fb_format_age(node->last_heard, heard, sizeof heard);
    }
    fb_draw_app_bar(state, layout,
                    &(const struct inkcell_fb_app_bar){.title = title,
                                                       .badge = heard[0] != '\0' ? heard : NULL,
                                                       .badge_family = INKCELL_FAMILY_SECONDARY});

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(
        node, is_self, inkwell_time_wall_s(),
        mesh_ui_snapshot_traceroute_view(snapshot, node->node_id), &snapshot->handshake,
        &snapshot->history, mesh_ui_units_imperial(snapshot->settings.units), items,
        MESH_UI_NODE_ITEMS_MAX);
    if (count == 0U) {
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_NODES,
                              inkcell_str(MESH_STR_NODES_DETAIL_EMPTY));
        return;
    }

    /* The label column holds the widest question this node is answered for, measured - not a
       count tuned to one language's words. Headings and the action row do not use it. */
    const char *labels[MESH_UI_NODE_ITEMS_MAX];
    size_t labelled = 0U;
    for (uint32_t r = 0U; r < count; ++r) {
        if (items[r].kind != MESH_UI_NODE_ROW_HEADING && items[r].kind != MESH_UI_NODE_ROW_ACTION) {
            labels[labelled++] = items[r].label;
        }
    }
    const size_t label_cols = inkcell_fb_field_label_cols_fit(state, layout, labels, labelled);
    /*
     * The one list on the device whose rows are not all the same height, and the reason the
     * window learned to count steps: a reading gets a bar with the row to itself, so the
     * decibels and the percentages on this screen can be judged rather than merely read.
     *
     * Measured here, from the same `kind` the loop below draws from, and handed to the model
     * before anything is placed - which is what stops the window and the draw from ever being
     * a row apart.
     */
    uint8_t heights[MESH_UI_NODE_ITEMS_MAX];
    /*
     * Which card each row stands on, measured in the same pass.
     *
     * A heading opens a card and everything under it belongs to that card until the next one -
     * which is the whole of the grouping rule, and it is derived rather than declared because
     * the groups are already in the rows. node_detail.c emits a heading per subject and nothing
     * else in this screen is a group, so a `group` field on the item would be a second way of
     * saying what `kind` says, with the drift that implies the first time a group is added.
     *
     * The heading itself stands on no card, and that is where the column gets its air. A card
     * is a surface painted round row boxes that were laid out for a flat list, so the only room
     * it has to be padded with is whatever a group's own heading step is not using - and split
     * three ways between a card's bottom, the break, and the next card's top, none of the three
     * was big enough to see. Standing the heading in the break instead gives the step to the
     * two edges that need it: the card above closes under its last row and the card below opens
     * at its first, with the heading centred between them naming the group it opens. It is also
     * what every settings list on a phone does with a section label, and it still costs no rows.
     *
     * The ordinal wraps well below INKCELL_FB_LIST_NO_CARD: the row budget is 128 and a node's
     * headings are a dozen at the very most, so the counter cannot reach it.
     */
    uint8_t cards[MESH_UI_NODE_ITEMS_MAX];
    /*
     * Starting at 0 rather than at INKCELL_FB_LIST_NO_CARD, which is what gives the row above the
     * first heading a surface to stand on. That row is the one that opens the node's verbs, and it
     * is deliberately unheaded - a heading names a group the reader can skip past and this is one
     * row - so without this it would be the only thing on a card screen drawn on the bare panel,
     * and the only row whose highlight had no edge around it.
     *
     * Every heading then steps the ordinal unconditionally, which is what keeps that leading card
     * and the first headed one apart: two adjacent rows carrying the same ordinal are one card, so
     * a heading that left the counter alone would have drawn the Actions row and the Identity rows
     * as a single surface with a title through the middle of it. A node with no verbs to offer
     * leaves ordinal 0 unused, and an ordinal nothing carries costs nothing - what makes a card is
     * the rows that name it.
     */
    uint8_t card = 0U;
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = mesh_ui_node_item_steps(&items[r]);
        if (items[r].kind == MESH_UI_NODE_ROW_HEADING) {
            card = (uint8_t)(card + 1U);
            cards[r] = INKCELL_FB_LIST_NO_CARD;
            continue;
        }
        cards[r] = card;
    }
    /*
     * Which column a row's words start in, and this screen answers it per *card* rather than
     * once for the whole list.
     *
     * The leading slot's rule is that it is declared for a whole list or for none of it, because
     * a list that indents only the rows carrying a symbol starts its text in two columns. A card
     * is the run of rows that rule is about here: every row of one is the same kind of row, and
     * what separates two of them is a heading with air on either side - so two cards starting
     * their words in two columns is not an eye running down a column and losing it, it is two
     * panels.
     *
     * There are exactly two shapes on this screen and nothing in between:
     *
     *   - a card of verbs, where every row leads with a disc and the heading over them is a disc
     *     too. The symbol is never missing here: an action with no icon would be a row that is
     *     not about anything, which is what node_detail.c's table exists to prevent.
     *   - a card of facts - a label and a value, and never a symbol on either - whose rows start
     *     at the card's own padding under a heading that is the card's header.
     *
     * Which is also the end of the gutter that used to be reserved across a hundred rows of
     * facts for icons that were never coming to fill it.
     */
    /*
     * What the cursor is standing on, from the rows just built - with the history, because that
     * is what decides which readings are presses. A row A acts on is highlighted with its group
     * kept in view; a card of facts is focused as a whole, a page at a time when it is taller
     * than the panel. See mesh_ui_node_detail_step() for the stops the nav walks.
     */
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_NODES];
    /* And the window the nav pages a tall card by on the next press: this one. */
    state->page_rows = layout->rows;
    struct mesh_ui_node_span span = {.first = cursor, .last = cursor, .card = false};
    (void)mesh_ui_node_detail_span(items, count, layout->rows, cursor < count ? cursor : count - 1U,
                                   &span);
    /*
     * The rows, in one list or two. Two lists rather than one laid out twice, because each column
     * windows on its own: the cursor's column keeps the cursor in view as it always did, and the
     * other shows from its top. The d-pad walks the same stops in the same order either way -
     * down the first column and on into the second, which is the order the facts are read in -
     * so the nav and its tests are the one-column nav unchanged.
     */
    const uint32_t split = wide ? fb_node_detail_split(items, heights, count) : 0U;
    if (split == 0U) {
        fb_node_detail_list(state, layout, items, 0U, count, heights, cards,
                            cursor < count ? cursor : count - 1U, &span, node->node_id, label_cols,
                            FB_LIST_NODE_DETAIL);
        return;
    }
    const struct inkcell_box whole = inkcell_fb_region(state);
    for (size_t c = 0U; c < 2U; ++c) {
        const struct inkcell_fb_layout column = fb_node_column_layout(state, layout, columns[c]);
        const uint32_t from = c == 0U ? 0U : split;
        const uint32_t to = c == 0U ? split : count;
        /* Fitted per column: the label column is what the rows beside it need. */
        size_t fitted = 0U;
        for (uint32_t r = from; r < to; ++r) {
            if (items[r].kind != MESH_UI_NODE_ROW_HEADING &&
                items[r].kind != MESH_UI_NODE_ROW_ACTION) {
                labels[fitted++] = items[r].label;
            }
        }
        (void)inkcell_fb_set_region(state, columns[c]);
        fb_node_detail_list(state, &column, items, from, to, heights, cards,
                            cursor < count ? cursor : count - 1U, &span, node->node_id,
                            inkcell_fb_field_label_cols_fit(state, &column, labels, fitted),
                            c == 0U ? FB_LIST_NODE_DETAIL : FB_LIST_NODE_DETAIL_SIDE);
    }
    (void)inkcell_fb_set_region(state, whole);
}

static void fb_node_detail_row(struct inkcell_draw_state *state, struct inkcell_fb_list *list,
                               uint32_t i, uint32_t item_index,
                               const struct mesh_ui_node_item *item, uint32_t node_id,
                               size_t label_cols) {
    if (item->kind == MESH_UI_NODE_ROW_HEADING) {
        /*
         * The card's own symbol, from the group rather than from here, in the disc that
         * makes this line a card *header* rather than a label floating above a panel.
         *
         * Every group gets one, including the groups whose rows carry nothing in a leading
         * slot - which is not the two-column start the slot's rule is about. A heading is
         * not one of the rows: it stands in the break between two cards with air above and
         * below it, so the column it starts in is the card's title column and the column its
         * rows start in is the card's content. Every phone app sets those two apart the same
         * way, and it is what gives a hundred and twenty rows of facts a set of landmarks
         * the eye can find without reading any of them.
         */
        inkcell_fb_list_subheader_icon(
            state, list, i, item->label,
            (struct inkcell_fb_leading){.kind = INKCELL_FB_LEADING_TONAL,
                                        .icon = (enum inkcell_icon)item->icon});
    } else if (item->kind == MESH_UI_NODE_ROW_ACTION) {
        fb_node_action_row(state, list, i, item, node_id);
    } else if (item->kind == MESH_UI_NODE_ROW_METER) {
        /*
         * The figure and, beside it, where that figure sits between its own two ends - which
         * is the half of a reading that decibels and percentages do not carry. The row still
         * says the number; the bar is what says whether the number is a problem.
         *
         * Keyed on the row index above everything the settings rows can reach, for the
         * reason a meter row there is: a reading is a fact rather than a control, so it has
         * no field of its own to be identified by.
         */
        struct inkcell_fb_meter meter = {
            .id = 0x04000000U | item_index,
            .kind = INKCELL_FB_METER_DETERMINATE,
            .value = item->number,
            .scale = item->scale,
            .band = item->banded ? &item->band : NULL,
            .tone = INKCELL_TONE_SUCCESS,
        };
        /*
         * And, where the client has been watching one, which way the reading has been
         * going - in the trailing slot, beside the figure rather than under it.
         *
         * The two pictures on this row are deliberately different sizes, because they are
         * different weights of question. Where a battery sits between flat and full is what
         * the screen is for, so it gets the row's second step and the bands with it;
         * whether it has been falling is a glance, so it gets six cells against the edge.
         * A trend given the full-width treatment would have taken a third step and said the
         * word "battery" three times down one screen.
         */
        struct inkcell_polyline points;
        inkcell_series_project(item->trend, item->scale, &points);
        struct inkcell_fb_sparkline trend = {.points = &points, .tone = INKCELL_TONE_PRIMARY};
        const struct inkcell_fb_list_item row = {
            .label = item->label,
            .label_cols = label_cols,
            /* The question recedes and the answer keeps the row - see the INFO row below,
               which is the same statement about the same kind of row. */
            .label_quiet = true,
            .value = item->value,
            .tone = INKCELL_TONE_NORMAL,
            .meter = &meter,
            .trailing = {.kind = INKCELL_FB_TRAILING_SPARK, .spark = &trend},
        };
        inkcell_fb_list_item(state, list, i, &row);
    } else {
        /*
         * A stated fact, and the two halves of it are not one tier.
         *
         * The label is the question - "Long name", "SNR", "Hops away" - and it repeats down
         * a column the reader is scanning for the *answers*; the value is what they opened
         * the node to find out. Drawn in one ink they were typographically identical, which
         * is what made a card of them read as a block of text with no way into it, and it
         * is the complaint this screen earns before any other: a hundred and twenty rows of
         * facts, all the same weight.
         *
         * So the label takes the quiet tier and the value keeps the row's own. It is stated
         * per row rather than assumed by the component because the opposite row exists and
         * is just as common: on a settings field the label is what the reader is choosing
         * and the value is merely where it stands, so there the label leads. Which of the
         * two a row is, is the row's to say.
         */
        const struct inkcell_fb_list_item row = {
            .label = item->label,
            .label_cols = label_cols,
            .label_quiet = true,
            .value = item->value,
            /*
             * The row's own ink rather than a flat normal, which every row here still gets:
             * INKCELL_TONE_NORMAL is 0, so a builder that says nothing says exactly what
             * this used to hard-code. What it buys is the one fact on this screen that is a
             * *judgement* rather than a reading - a key somebody proved is theirs, which is
             * worth a colour for the reason no temperature is.
             */
            .tone = (enum inkcell_tone)item->tone,
            /*
             * And, on the handful of rows whose answer is one of a set rather than a figure,
             * the shape that says so. Which rows those are is node_detail.c's to decide -
             * a renderer testing the value text would be a second table - and which colour
             * follows from the tone above, so the two cannot be paired wrongly from here.
             */
            .value_chip = item->chip,
        };
        inkcell_fb_list_item(state, list, i, &row);
    }
}

/*
 * A node's verbs, over its detail.
 *
 * The screen the detail used to open with. Thirteen rows of it led the node's own list, so a
 * reader who pressed A on a node to find out what it *was* met a full panel of verbs, with
 * "Remove from radio" on screen above the battery level - and every fact about the node below
 * the fold. The verbs are the same rows drawn by the same call (fb_node_action_row); what
 * changed is that they are a press away rather than in front of everything.
 *
 * One card and no headings, which is why this measures nothing the detail has to: every row is
 * one step, every row stands on the same surface, and the group walking, the paging and the
 * card-of-facts focus are all answers to a hundred rows of mixed kinds that this screen does not
 * have. What it keeps is the app bar's trail - the node's name over "Actions" - which is the
 * Settings tab's shape for a level inside a level and says whose verbs these are without
 * spending a row on it.
 */
void fb_render_node_actions(struct inkcell_draw_state *state,
                            const struct mesh_ui_snapshot *snapshot,
                            struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    /*
     * Whose verbs these are, which outlives the nav closing the sheet.
     *
     * mesh_ui_nav_close_node_detail() clears the node when the *detail* goes, and the sheet is
     * a level of that detail - so B on the sheet leaves the node where it was and B twice in
     * quick succession does not. The subject is remembered for exactly as long as there is
     * some of the sheet left on the panel; see fb_overlay_subject().
     */
    const bool up = nav->node_actions_open;
    const uint32_t which =
        fb_overlay_subject(state, FB_OVERLAY_NODE_ACTIONS, up, nav->node_detail_node);
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, which);
    if (node == NULL) {
        /* The node left the roster under the open sheet; the nav closes both levels on the next
           publish. Nothing is drawn, which is now an answer a layer can give: the detail
           underneath is still on the panel and says so itself. */
        return;
    }

    struct mesh_ui_node_item items[MESH_UI_NODE_ACTIONS_MAX];
    const uint32_t count = mesh_ui_node_actions_build(
        node, hs->has_my_info && node->node_id == hs->my_info.node_num,
        mesh_ui_snapshot_traceroute_view(snapshot, node->node_id), nav->node_remove_armed,
        snapshot->settings.protocol_lacks, items, MESH_UI_NODE_ACTIONS_MAX);
    if (count == 0U) {
        return;
    }

    /*
     * The sheet the comments have been calling it since the verbs moved off the detail.
     *
     * It was a screen: an app bar, the node's name on the trail over the word "Actions", and
     * the detail it is about replaced by a column of verbs. What a reader picked the verb
     * *for* was the row they had been looking at, and it was gone. Now it comes up from the
     * bottom edge over that detail, dimmed and still there, with the node's name as the
     * sheet's own title - the trail that had to spell out the level it was standing at.
     */
    char name[96];
    fb_node_title(node, name, sizeof name);
    const struct inkcell_fb_sheet sheet = {.title = name,
                                           .detail = inkcell_str(MESH_STR_NODE_HEAD_ACTIONS)};

    struct inkcell_overlay_frame frame;
    struct inkcell_fb_layout inner;
    if (!fb_sheet_begin(state, layout, FB_OVERLAY_NODE_ACTIONS, up, &sheet,
                        (int)count * layout->line, &frame, &inner)) {
        return;
    }

    uint8_t heights[MESH_UI_NODE_ACTIONS_MAX];
    uint8_t cards[MESH_UI_NODE_ACTIONS_MAX];
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = 1U;
        cards[r] = 0U; /* one card, so one ordinal - see the detail's derivation for why */
    }
    const uint32_t cursor =
        nav->node_actions_cursor < count ? nav->node_actions_cursor : count - 1U;
    /* The cursor is a row here and never a card: a verb is a control and gets the row's own
       cue, which is the `card` half of the detail's span being false for exactly the rows this
       screen is made of. A list of verbs to choose from is a menu, and is set as one. */
    const struct inkcell_fb_list_style look = fb_list_look(state, FB_LIST_ROLE_MENU);
    struct inkcell_fb_list list =
        inkcell_fb_list_begin_styled(state, &inner, count, cursor, heights, cards, &look);
    inkcell_fb_list_glide(state, &list, FB_LIST_NODE_ACTIONS);
    inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_SHEET_ROWS);
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        fb_node_action_row(state, &list, i, &items[i], node->node_id);
    }
    fb_sheet_end(state, &frame);
}

void fb_render_node_pane(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                         struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    /* The chart over the detail, the way the detail is drawn over the list. The reading is
       checked rather than a flag because it *is* the flag: MESH_UI_HISTORY_NONE is closed. */
    if (nav->node_trend != MESH_UI_HISTORY_NONE) {
        fb_render_node_trend(state, snapshot, layout);
    } else {
        fb_render_node_detail(state, snapshot, layout);
    }
    /*
     * And the verbs, on a layer over whichever of the two is up rather than in place of
     * the detail. Called on every frame because a sheet that is leaving is not in the
     * snapshot any more - the dialogs above it work the same way and for the same reason.
     *
     * Below the chart rather than above it because the two cannot both be up - a chart
     * opens from a reading and the sheet from the one action row - so what this order
     * states is which is the deeper level rather than a race being resolved.
     */
    fb_render_node_actions(state, snapshot, layout);
}

static void fb_render_node_sort(struct inkcell_draw_state *state,
                                const struct mesh_ui_snapshot *snapshot,
                                struct inkcell_fb_layout *layout);

void fb_render_nodes(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                     struct inkcell_fb_layout *layout) {
    if (snapshot->nav.node_detail_open) {
        fb_render_node_pane(state, snapshot, layout);
    } else {
        struct inkcell_fb_layout under = *layout;
        fb_render_node_list(state, snapshot, &under);
        /* Over the whole body rather than the list's share of it, which the chip bar took the
           top of: a sheet rises from the panel's edge, not from a list's. */
        fb_render_node_sort(state, snapshot, layout);
    }
}

/*
 * The chip bar: the search, the three filters and the sort, as one row of chips under the
 * heading - see MESH_UI_NODES_CHIP_ROW for why it is one row and why it is fixed there.
 *
 * Drawn here rather than as a row of the list, and it consumes the layout it is drawn into, so
 * the list below starts under it and scrolls without taking it along.
 *
 * The filters are the set with the chosen one filled - the tab strip's shape, and the reason
 * all three are always on the panel. The search and the sort are verbs rather than a choice,
 * so they are drawn as the neutral filled pill a control wears at rest; each carries in its own
 * words what it is currently doing, which is the whole of why the list's state reads at a
 * glance. The sort sits against the trailing edge, apart from the filters, because it answers a
 * different question.
 *
 * A narrow panel or a large glyph scale drops words before it drops chips: first the sort's
 * "Sort:" prefix, then the search's word while nothing is typed - its magnifier still says
 * what it is. Past that the row is clipped at the panel's edge, which no panel this client
 * lays out at reaches.
 */
static void fb_node_chip_bar(struct inkcell_draw_state *state, struct inkcell_fb_layout *layout,
                             const struct mesh_ui_nav *nav, const char *sort_value,
                             bool sort_unavailable, bool focused) {
    const int scale = layout->small;
    const int gap = inkcell_fb_space_at(state, INKCELL_SPACE_SM, scale);
    /* Air under the bar as well as the pill's own gap, so the first node's avatar does not sit
       against the chips - the bar is chrome over the list, not the list's first row. */
    const int below = inkcell_fb_space(state, INKCELL_SPACE_MD);
    const int pill_h = inkcell_fb_line_adv(state, scale);
    const int band_h = gap + pill_h + below;
    const int text_y = layout->body_y + gap + inkcell_step_px(scale);

    char sort_chip[64];
    inkcell_str_format(sort_chip, sizeof sort_chip, MESH_STR_NODES_SORT_CHIP, sort_value);
    const char *find =
        nav->node_query[0] != '\0' ? nav->node_query : inkcell_str(MESH_STR_NODES_FIND_ROW);
    const char *sort = sort_chip;

    struct {
        enum inkcell_icon icon;
        const char *label;
    } chips[MESH_UI_NODES_CHIP_COUNT];
    chips[MESH_UI_NODES_CHIP_FIND].icon = INKCELL_ICON_SEARCH;
    chips[MESH_UI_NODES_CHIP_FIND].label = find;
    for (uint32_t f = 0; f < (uint32_t)MESH_UI_NODE_FILTER_COUNT; ++f) {
        chips[MESH_UI_NODES_CHIP_FILTER_FIRST + f].icon = INKCELL_ICON_NONE;
        chips[MESH_UI_NODES_CHIP_FILTER_FIRST + f].label =
            inkcell_str(mesh_ui_node_filter_label((enum mesh_ui_node_filter)f));
    }
    chips[MESH_UI_NODES_CHIP_SORT].icon = INKCELL_ICON_NONE;
    chips[MESH_UI_NODES_CHIP_SORT].label = sort;

    /* The words, dropped a step at a time until the bar fits. */
    for (int step = 0; step < 3; ++step) {
        int need = 0;
        for (size_t c = 0; c < (size_t)MESH_UI_NODES_CHIP_COUNT; ++c) {
            need += inkcell_fb_chip_width(state, chips[c].icon, chips[c].label, scale);
        }
        need += gap; /* the gap between the filters and the sort */
        if (need <= layout->body_w) {
            break;
        }
        if (step == 0) {
            chips[MESH_UI_NODES_CHIP_SORT].label = sort_value;
        } else if (step == 1 && nav->node_query[0] == '\0') {
            chips[MESH_UI_NODES_CHIP_FIND].label = "";
        }
    }

    const uint8_t filter = nav->node_filter;
    int x = layout->body_x;
    for (size_t c = 0; c < (size_t)MESH_UI_NODES_CHIP_COUNT; ++c) {
        const bool is_filter =
            c >= (size_t)MESH_UI_NODES_CHIP_FILTER_FIRST && c < (size_t)MESH_UI_NODES_CHIP_SORT;
        if (c == (size_t)MESH_UI_NODES_CHIP_SORT) {
            /* Against the trailing edge, unless the filters already reach it. */
            const int w = inkcell_fb_chip_width(state, chips[c].icon, chips[c].label, scale) -
                          inkcell_fb_char_adv(state, scale);
            const int end = layout->body_x + layout->body_w;
            if (end - w > x + gap) {
                x = end - w;
            } else {
                x += gap;
            }
        }
        const bool active =
            is_filter && (size_t)filter == c - (size_t)MESH_UI_NODES_CHIP_FILTER_FIRST;
        const bool chip_focused = focused && nav->node_chip == (uint8_t)c;
        /* A search with something in it is doing something, so it wears the accent too: the
           reader looking at the list should see at once that it has been narrowed. The sort
           that cannot measure anything is the warning family's, for the same reason. */
        const bool lit =
            active || (c == (size_t)MESH_UI_NODES_CHIP_FIND && nav->node_query[0] != '\0');
        const struct inkcell_fb_button button = {
            .rect = inkcell_fb_chip_box(state, x, text_y, chips[c].icon, chips[c].label, scale),
            .icon = chips[c].icon,
            .label = chips[c].label,
            .focused = chip_focused,
            .variant = lit || (c == (size_t)MESH_UI_NODES_CHIP_SORT && sort_unavailable)
                           ? INKCELL_FB_BUTTON_TONAL
                           : (is_filter ? INKCELL_FB_BUTTON_TEXT : INKCELL_FB_BUTTON_FILLED),
            .family = c == (size_t)MESH_UI_NODES_CHIP_SORT && sort_unavailable
                          ? INKCELL_FAMILY_WARNING
                          : INKCELL_FAMILY_PRIMARY,
            .shape = INKCELL_SHAPE_FULL,
            .idle_tone = INKCELL_TONE_DIM,
            .ground = INKCELL_COLOR_BG,
            .scale = scale,
            .focus_id = (uint32_t)MESH_UI_FOCUS_NODE_CHIPS + (uint32_t)c,
        };
        inkcell_fb_draw_button(state, &button);
        x += inkcell_fb_chip_width(state, chips[c].icon, chips[c].label, scale);
    }

    layout->body_y += band_h;
    layout->rows = inkcell_fb_layout_rows(state, layout);
}

/*
 * The sort sheet: the five orders, the current one checked, over the list it orders.
 *
 * Called on every frame the list is, for the node verbs' reason: a sheet that is leaving is not
 * in the snapshot any more, and it still has frames to draw.
 */
static void fb_render_node_sort(struct inkcell_draw_state *state,
                                const struct mesh_ui_snapshot *snapshot,
                                struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const bool up = nav->node_sort_open && !nav->node_detail_open;
    const uint32_t count = (uint32_t)MESH_UI_NODE_SORT_COUNT;
    const struct inkcell_fb_sheet sheet = {.title = inkcell_str(MESH_STR_NODES_SORT_TITLE)};
    const struct inkcell_fb_list_style look = fb_list_look(state, FB_LIST_ROLE_MENU);
    /* Tall enough for every order at the list's own step, which a roomy density makes more
       than a body line: five choices are few enough that none of them should need a scroll.
       One step over, because the sheet's content is measured in whole body lines and the list
       in whole steps inside that, and each rounds down. */
    const int pad = look.density == INKCELL_FB_LIST_COMFORTABLE
                        ? inkcell_fb_space(state, INKCELL_SPACE_LG) / 2
                        : 0;

    struct inkcell_overlay_frame frame;
    struct inkcell_fb_layout inner;
    if (!fb_sheet_begin(state, layout, FB_OVERLAY_NODE_SORT, up, &sheet,
                        (int)(count + 1U) * (layout->line + 2 * pad), &frame, &inner)) {
        return;
    }
    uint8_t heights[MESH_UI_NODE_SORT_COUNT];
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = 1U;
    }
    const uint32_t cursor = nav->node_sort_cursor < count ? nav->node_sort_cursor : count - 1U;
    struct inkcell_fb_list list =
        inkcell_fb_list_begin_styled(state, &inner, count, cursor, heights, NULL, &look);
    inkcell_fb_list_glide(state, &list, FB_LIST_NODE_SORT);
    inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_SHEET_ROWS);
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        /* Keyed on the row, above everything else a radio here reaches. The sheet closes on the
           press that changes the answer, so this never animates in place. */
        struct inkcell_fb_selection sel = {
            .id = 0x05000000U | i,
            .on = i == (uint32_t)nav->node_sort,
        };
        const struct inkcell_fb_list_item row = {
            .text = inkcell_str(mesh_ui_node_sort_label((enum mesh_ui_node_sort)i)),
            .trailing = {.kind = INKCELL_FB_TRAILING_RADIO, .sel = &sel},
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
    fb_sheet_end(state, &frame);
}

void fb_render_node_list(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                         struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (!snapshot->handshake_valid || snapshot->handshake.node_count == 0U) {
        fb_draw_app_bar(
            state, layout,
            &(const struct inkcell_fb_app_bar){.title = inkcell_str(MESH_STR_TAB_NODES)});
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_NODES,
                              inkcell_str(snapshot->handshake_valid
                                              ? MESH_STR_NODES_EMPTY_WAITING
                                              : MESH_STR_NODES_EMPTY_DISCONNECTED));
        return;
    }

    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const enum mesh_ui_node_filter filter = (enum mesh_ui_node_filter)nav->node_filter;
    const enum mesh_ui_node_sort sort = (enum mesh_ui_node_sort)nav->node_sort;
    /*
     * The list, built once for this frame: the rows the filter kept, in the order the sort puts
     * them. Every row below is read out of it rather than walked for, so the node a row draws
     * and the node mesh_ui_nav_node_at_row() opens are the same node by construction.
     */
    struct mesh_ui_node_view view_rows;
    mesh_ui_node_view_build_query(hs, filter, nav->node_query, sort, &view_rows);
    const uint32_t count = view_rows.count;
    /*
     * The heading's "of" is measured against this list with nothing narrowing it, and against
     * nothing bigger.
     *
     * It used to take the largest of three totals - this list, the roster the client holds, and
     * the radio's own NodeDB count - so a radio that had counted nodes it never sent read
     * "Nodes (22 of 42)" over a list with every filter off. That is a number the reader cannot
     * act on: nothing on the frame says which twenty are missing or how to see them, and it
     * reads as a filter the reader did not set. The gap between the radio's count and the
     * client's is real, and the Radio tab's roster card is where it is said.
     *
     * So the number is only ever the reader's own doing: a filter chip or a search, both of
     * which are on the chip row under the heading, which is where the reader looks for why.
     */
    uint32_t unfiltered = count;
    if (filter != MESH_UI_NODE_FILTER_ALL || nav->node_query[0] != '\0') {
        struct mesh_ui_node_view all_rows;
        mesh_ui_node_view_build(hs, MESH_UI_NODE_FILTER_ALL, sort, &all_rows);
        unfiltered = all_rows.count;
    }
    char title[96];
    mesh_ui_chrome_list_title(title, sizeof title, inkcell_str(MESH_STR_TAB_NODES), count,
                              unfiltered, 0U);
    fb_draw_app_bar(state, layout, &(const struct inkcell_fb_app_bar){.title = title});

    const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;
    /* The discs come from the nav layer, which wants a store rather than the handshake alone -
       the same view the conversation list and the picker build, so all three ask one function. */
    struct mesh_ui_store view;
    mesh_ui_store_view(snapshot, &view);
    /*
     * The chip bar, and under it the nodes the filter kept in the order the sort put them. The
     * nav counts the bar as row 0 (MESH_UI_NODES_LEAD_ROWS); the list drawn here holds only the
     * nodes, so the list's index is the nav's row less that.
     *
     * A filter that keeps nothing still draws the bar and then says so where the first node
     * would be - so the chip that emptied the list is still on the frame, and the press that
     * puts it back is one A away. That row is the *note's*, not a node's: mesh_ui_nav_row_count()
     * does not count it and the cursor cannot reach it, exactly as the empty states elsewhere
     * are not rows.
     */
    const bool nothing_matched = (count == 0U);
    /*
     * A sort that cannot measure anything says so on its own chip, and nowhere else.
     *
     * A distance sort with no fix of our own leaves the list in the order it was published in,
     * and a chip reading "Distance" over a list that did not move is the control and the rows
     * disagreeing about one fact - the failure the Direct chip's note is written against,
     * arriving from the other side. It is said where the reader just pressed, on the chip that
     * already says what the sort is set to, because a row of its own under the bar would be a
     * row the nav does not count and the cursor cannot reach.
     */
    const bool sort_unavailable = !mesh_ui_node_sort_available(hs, sort);
    char sort_value[40];
    if (sort_unavailable) {
        inkcell_str_format(sort_value, sizeof sort_value, MESH_STR_NODES_SORT_NO_FIX,
                           inkcell_str(mesh_ui_node_sort_label(sort)));
    } else {
        inkwell_str_copy(sort_value, sizeof sort_value, inkcell_str(mesh_ui_node_sort_label(sort)));
    }
    /* The bar has the cursor when the list is what the reader is on and row 0 is where they
       are; a detail open beside the list has the tab's cursor for its own rows. */
    const bool on_chips =
        !nav->node_detail_open && nav->cursor[MESH_UI_SCREEN_NODES] == MESH_UI_NODES_CHIP_ROW;
    fb_node_chip_bar(state, layout, nav, sort_value, sort_unavailable, on_chips);

    const uint32_t rows = count + (nothing_matched ? 1U : 0U);
    /* Every node is two steps - a name and the line under it. Declared before the list is
       opened, because the model is the authority on every height and can only be if it is told
       first. */
    uint8_t node_heights[MESH_UI_MAX_HANDSHAKE_NODES + 1U];
    for (uint32_t r = 0; r < rows && r < (uint32_t)(sizeof node_heights); ++r) {
        node_heights[r] = 2U;
    }
    /* With a node open - which is only drawn beside it, on a split frame - the tab's cursor
       indexes the detail's rows, and the list's own place is the open node's row, found by
       which node it is: the roster re-ranks under an open detail on every publish. */
    uint32_t cursor = nav->cursor[MESH_UI_SCREEN_NODES];
    cursor = cursor >= MESH_UI_NODES_LEAD_ROWS ? cursor - MESH_UI_NODES_LEAD_ROWS : 0U;
    if (nav->node_detail_open) {
        const uint32_t at = mesh_ui_node_view_find(hs, &view_rows, nav->node_detail_node);
        const uint32_t parked = nav->node_list_cursor >= MESH_UI_NODES_LEAD_ROWS
                                    ? nav->node_list_cursor - MESH_UI_NODES_LEAD_ROWS
                                    : 0U;
        cursor = at < count ? at : parked;
    }
    const struct inkcell_fb_list_style look = fb_list_look(state, FB_LIST_ROLE_FEED);
    struct inkcell_fb_list list =
        inkcell_fb_list_begin_styled(state, layout, rows, cursor, node_heights, NULL, &look);
    /*
     * With the cursor up on the chip bar, no row is the cursor's. The window is still derived
     * from row 0, which is what keeps the top of the list in view under the bar - but nothing
     * is highlighted and nothing claims the focus ring, which is on a chip. Past the end is how
     * a list says "none of mine": inkcell_list_is_cursor() is an equality with the index.
     */
    if (on_chips) {
        list.model.cursor = UINT32_MAX;
    }
    /* The rows glide and are click targets only while they are what the reader is on. A detail
       open beside them glides its own window - there is one glide slot, and two lists taking it
       in turn every frame would leave neither gliding - and registers its own rows. Their ids
       are the nav's rows, the bar's included, so a click names the row the d-pad would. */
    if (!nav->node_detail_open) {
        inkcell_fb_list_glide(state, &list, FB_LIST_NODES);
        inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_ROWS + MESH_UI_NODES_LEAD_ROWS);
    } else {
        /* A pointer can still reach another node: see MESH_UI_FOCUS_PANE_ROWS. */
        inkcell_fb_list_targets(&list, (uint32_t)MESH_UI_FOCUS_PANE_ROWS + MESH_UI_NODES_LEAD_ROWS);
    }
    const bool imperial = mesh_ui_units_imperial(snapshot->settings.units);
    char right[32];
    char age[8];
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        if (nothing_matched) {
            /* The row that is not a row: what the filter did, where the nodes would be. Dim
               because it is not something to press. A query says so in its own words, since
               the filter chip may well be on All. */
            inkcell_fb_list_row(state, &list, i,
                                inkcell_str(nav->node_query[0] != '\0'
                                                ? MESH_STR_NODES_FIND_NONE
                                                : MESH_STR_NODES_FILTER_NONE),
                                INKCELL_TONE_DIM);
            continue;
        }
        /* Through the view, never by subtracting from the raw roster: the row-to-node mapping
           is mesh_ui_nav_node_at_row()'s question and this is the same answer, so the node the
           cursor opens and the node this row drew cannot be two different nodes. */
        const struct mesh_ui_node_summary *node = mesh_ui_node_view_at(hs, &view_rows, i);
        if (node == NULL) {
            continue;
        }
        inkcell_fb_format_age(node->last_heard, age, sizeof age);

        /*
         * The right-hand column is the age, and the signal bars beside it when the node was
         * heard directly - the two things a roster is scanned down the edge for, so they keep
         * the edge to themselves.
         *
         * How the node reaches us used to share that column ("3hop 2h", "mqtt 1h", "off radio
         * 5m"), which made it a column of three kinds of word that could not be compared down
         * the list. That moved to the quiet line under the name - mesh_ui_node_row_facts() -
         * and so did the decibel figure for a node whose hops were never reported.
         *
         * The bars are only drawn when mesh_ui_node_signal_heard() says there is a reading
         * *to this node*: an SNR is measured on the packet that arrived, so for a node reached
         * over several hops it describes the last relay and for one over MQTT nothing on the
         * air at all - rungs there would be reporting somebody else's link as this node's.
         */
        const bool is_me = (me != 0U && node->node_id == me);
        const bool direct = !is_me && node->in_nodedb &&
                            !(node->has_hops_away && node->hops_away > 0U) && !node->via_mqtt &&
                            mesh_ui_node_signal_heard(node);
        if (is_me) {
            /* Ourselves: a radio does not hear its own packets, so the 0.0 its NodeDB entry
               carries is no reading, and how long ago it last heard itself is not an age. */
            inkwell_str_copy(right, sizeof right, inkcell_str(MESH_STR_NODES_ROW_SELF));
        } else {
            inkwell_str_copy(right, sizeof right, age);
        }

        /* The name the detail screen's app bar uses, so a row and the screen it opens call the
           node the same thing - and the name sort compares, so a list sorted by name reads
           down this column in order. */
        char name[48];
        fb_node_title(node, name, sizeof name);
        /*
         * A node discovered since the reader last came to the tab says so first on its quiet
         * line, and the row is drawn strong - the conversation list's unread row, for the same
         * reason: it is the row the eye should land on. Measured against the mark as it stood
         * when the reader arrived (nav.nodes_new_after), so the word stays until they have left
         * and come back rather than going the frame the tab opens.
         */
        const bool is_new = mesh_ui_node_is_new(node, nav->nodes_new_after);
        char facts[96];
        mesh_ui_node_row_facts(hs, node, imperial, facts, sizeof facts);
        if (is_new) {
            /* Joined here rather than inside the facts, which are about the node: this is about
               the reader. The separator only when there is something to join it to. */
            char line[sizeof facts + 16U];
            snprintf(line, sizeof line, "%s%s%s", inkcell_str(MESH_STR_NODES_ROW_NEW),
                     facts[0] != '\0' ? inkcell_str(MESH_STR_NODES_ROW_SEPARATOR) : "", facts);
            inkwell_str_copy(facts, sizeof facts, line);
        }

        /*
         * The disc carries the node's initials and is tinted by node number, both answered by
         * the nav layer - so a node the user has learned to find by colour in Messages is the
         * same two cells and the same colour here.
         *
         * It resolves the *node*, not `short_name`: a node with no short name is shown as the
         * "----" placeholder, which has no letters in it and would give an empty disc, while
         * Messages falls through to the long name and then to the "!hex" id. What a row
         * displays and what identifies it are different questions.
         *
         * Ourselves is the one row that takes a stated fill instead of a tint. That is what the
         * '*' in the marker column used to say, and a disc says it without spending a cell of
         * the name: a node list is read by scanning the left edge, which is exactly where the
         * disc already is.
         */
        uint32_t tint = 0U;
        mesh_ui_nav_target_avatar(&view, node->node_id, 0U, initials, sizeof initials, &tint);

        /*
         * The star sits in the marker gutter, beside the name rather than in place of the
         * identity the disc is carrying: being pinned is a fact about the node, and a fact one
         * cell wide before the words is exactly what that slot is. It is reserved on every row
         * of this list and filled on the pinned ones, so the names stay in one column.
         *
         * Never on ourselves, which is what the old marker column got right by ordering the two
         * cases. A radio can carry a stale `is_favorite` on its own NodeDB entry, and nav.c and
         * node_detail.c both refuse to pin our own node - so a star there would advertise a
         * preference that no press can clear.
         */
        /* Dim behind the words, so a list that is mostly off-radio reads as one at a glance.
           The open thread's node keeps the accent whatever its NodeDB state: which node you
           are talking to is the one thing the cursor colour is for. */
        enum inkcell_tone tone = INKCELL_TONE_NORMAL;
        if (node->node_id == nav->target_node) {
            tone = INKCELL_TONE_PRIMARY;
        } else if (is_new) {
            tone = INKCELL_TONE_STRONG;
        } else if (!node->in_nodedb) {
            tone = INKCELL_TONE_DIM;
        }

        const struct inkcell_fb_list_item row = {
            .leading =
                {
                    .kind = INKCELL_FB_LEADING_AVATAR,
                    .label = initials,
                    .tint = tint,
                    .role = is_me ? INKCELL_COLOR_PRIMARY : INKCELL_COLOR_COUNT,
                },
            .text = name,
            .supporting = facts[0] != '\0' ? facts : NULL,
            /* The accent on the line that opens with "new", because strong ink alone is a step
               most themes barely draw: the word is what says it, the colour is what finds it. */
            .supporting_tone = is_new ? INKCELL_TONE_PRIMARY : INKCELL_TONE_NORMAL,
            .supporting_quiet = true,
            .marker_icon = (node->is_favorite && !is_me) ? INKCELL_ICON_PINNED : INKCELL_ICON_NONE,
            .marker_slot = true,
            .tone = tone,
            .trailing =
                direct
                    ? (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_SIGNAL,
                                                   .text = right,
                                                   .signal = inkcell_signal_level(node->snr)}
                    : (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_TEXT, .text = right},
            .divider = true,
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
}
