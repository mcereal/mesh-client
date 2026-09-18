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

#include "fb_widgets.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/history.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/nodes.h"
#include "mesh/ui/trust.h"
#include "mesh/ui/units.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <stdio.h>
#include <string.h>

/*
 * One node's detail: the same list-of-rows shape the Settings tab draws, so the two screens
 * scroll and clip identically. Headings are dimmed and get no value column; the action row ends
 * in the chevron a settings row that opens something ends in, for the same reason - it is the
 * only thing on the screen A does anything to.
 */
/*
 * What a node is called on a screen about it: its long name, its short name, or the "!0a1b2c3d"
 * form the phone apps fall back to when a node has sent no User at all.
 *
 * Asked by the detail's app bar and by the sheet's trail. One answer rather than two, because
 * the trail names the screen the sheet is *inside* and a second spelling of it would be a
 * breadcrumb that disagreed with the bar it leads back to.
 */
static void fb_node_title(const struct mesh_ui_node_summary *node, char *out, size_t out_len) {
    const char *name = node->long_name[0] != '\0'    ? node->long_name
                       : node->short_name[0] != '\0' ? node->short_name
                                                     : NULL;
    if (name != NULL) {
        mesh_str_copy(out, out_len, name);
        return;
    }
    mesh_str_format(out, out_len, MESH_STR_NODE_VAL_USER_ID_HEX, node->node_id);
}

/*
 * One verb, drawn.
 *
 * Shared by the two screens that hold verbs - the detail's single row that opens the sheet, and
 * every row of the sheet itself - because they are the same row. A sheet whose verbs were drawn
 * by a second copy of this would be a second opinion about what a destructive row looks like,
 * which is the drift node_detail.c's `tone` and `icon` fields exist to prevent one layer down.
 */
static void fb_node_action_row(struct mesh_ui_backend_fb_state *state, struct fb_list *list,
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
     * is the pair `struct fb_switch` asks for. The id is folded rather than truncated
     * so two node numbers agreeing in their low bits are not one control; it sits above
     * everything the settings fields and this screen's meters can reach.
     */
    const uint32_t node_key = (node_id ^ (node_id >> 20)) & 0x000FFFFFU;
    struct fb_switch sw = {
        .id = 0x05000000U | ((uint32_t)item->action << 20) | node_key,
        .family = item->tone == (uint8_t)MESH_UI_TONE_WARNING ? MESH_UI_FAMILY_WARNING
                                                              : MESH_UI_FAMILY_PRIMARY,
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
     * FB_LEADING_TONAL.
     */
    const struct fb_list_item row = {
        .leading = {.kind = FB_LEADING_TONAL, .icon = (enum mesh_ui_icon)item->icon},
        .text = item->label,
        .tone = (enum mesh_ui_tone)item->tone,
        .trailing = item->toggle ? (struct fb_trailing){.kind = FB_TRAILING_SWITCH, .sw = &sw}
                                 : (struct fb_trailing){.kind = FB_TRAILING_ICON,
                                                        .icon = MESH_UI_ICON_CHEVRON},
        /* A row whose press cannot be walked back gets the leading bar as well as the
           ink, in its own tone - the accent edge is drawn in the row's family, so the
           one row on the screen that deletes something is the one row marked in red on
           both of its edges. */
        .label_plain = true,
        .accent_edge = item->tone == (uint8_t)MESH_UI_TONE_ERROR,
    };
    fb_list_item(state, list, index, &row);
}

/* Mutable state, as every screen drawing a fb_list_item is: an item may carry a control
   that animates, and where such a control has got to is kept on the backend. */
void fb_render_node_detail(struct mesh_ui_backend_fb_state *state,
                           const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, nav->node_detail_node);
    if (node == NULL) {
        fb_draw_app_bar(state, layout,
                        &(const struct fb_app_bar){.title = mesh_str(MESH_STR_TAB_NODES)});
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES, mesh_str(MESH_STR_NODES_GONE));
        return;
    }

    const bool is_self = hs->has_my_info && node->node_id == hs->my_info.node_num;
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
        fb_format_age(node->last_heard, heard, sizeof heard);
    }
    fb_draw_app_bar(state, layout,
                    &(const struct fb_app_bar){.title = title,
                                               .badge = heard[0] != '\0' ? heard : NULL,
                                               .badge_family = MESH_UI_FAMILY_SECONDARY});

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(
        node, is_self, mesh_time_wall_s(),
        mesh_ui_snapshot_traceroute_view(snapshot, node->node_id), &snapshot->handshake,
        &snapshot->history, mesh_ui_units_imperial(snapshot->settings.units), items,
        MESH_UI_NODE_ITEMS_MAX);
    if (count == 0U) {
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES, mesh_str(MESH_STR_NODES_DETAIL_EMPTY));
        return;
    }

    const size_t label_cols = fb_field_label_cols(state, layout, 16U);
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
     * The ordinal wraps well below FB_LIST_NO_CARD: the row budget is 128 and a node's headings
     * are a dozen at the very most, so the counter cannot reach it.
     */
    uint8_t cards[MESH_UI_NODE_ITEMS_MAX];
    /*
     * Starting at 0 rather than at FB_LIST_NO_CARD, which is what gives the row above the first
     * heading a surface to stand on. That row is the one that opens the node's verbs, and it is
     * deliberately unheaded - a heading names a group the reader can skip past and this is one
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
            cards[r] = FB_LIST_NO_CARD;
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
    struct fb_list list = fb_list_begin_focus(layout, count, cursor, heights, cards, span.first,
                                              span.last, span.card);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_node_item *item = &items[i];
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
            fb_list_subheader_icon(state, &list, i, item->label,
                                   (struct fb_leading){.kind = FB_LEADING_TONAL,
                                                       .icon = (enum mesh_ui_icon)item->icon});
        } else if (item->kind == MESH_UI_NODE_ROW_ACTION) {
            fb_node_action_row(state, &list, i, item, node->node_id);
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
            struct fb_meter meter = {
                .id = 0x04000000U | i,
                .kind = FB_METER_DETERMINATE,
                .value = item->number,
                .scale = item->scale,
                .band = item->banded ? &item->band : NULL,
                .tone = MESH_UI_TONE_SUCCESS,
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
            struct mesh_ui_polyline points;
            mesh_ui_series_project(item->trend, item->scale, &points);
            struct fb_sparkline trend = {.points = &points, .tone = MESH_UI_TONE_PRIMARY};
            const struct fb_list_item row = {
                .label = item->label,
                .label_cols = label_cols,
                /* The question recedes and the answer keeps the row - see the INFO row below,
                   which is the same statement about the same kind of row. */
                .label_quiet = true,
                .value = item->value,
                .tone = MESH_UI_TONE_NORMAL,
                .meter = &meter,
                .trailing = {.kind = FB_TRAILING_SPARK, .spark = &trend},
            };
            fb_list_item(state, &list, i, &row);
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
            const struct fb_list_item row = {
                .label = item->label,
                .label_cols = label_cols,
                .label_quiet = true,
                .value = item->value,
                /*
                 * The row's own ink rather than a flat normal, which every row here still gets:
                 * MESH_UI_TONE_NORMAL is 0, so a builder that says nothing says exactly what
                 * this used to hard-code. What it buys is the one fact on this screen that is a
                 * *judgement* rather than a reading - a key somebody proved is theirs, which is
                 * worth a colour for the reason no temperature is.
                 */
                .tone = (enum mesh_ui_tone)item->tone,
                /*
                 * And, on the handful of rows whose answer is one of a set rather than a figure,
                 * the shape that says so. Which rows those are is node_detail.c's to decide -
                 * a renderer testing the value text would be a second table - and which colour
                 * follows from the tone above, so the two cannot be paired wrongly from here.
                 */
                .value_chip = item->chip,
            };
            fb_list_item(state, &list, i, &row);
        }
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
void fb_render_node_actions(struct mesh_ui_backend_fb_state *state,
                            const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, nav->node_detail_node);
    if (node == NULL) {
        /* The node left the roster under the open sheet; the nav closes both levels on the next
           publish. Until then this says what the detail says in the same case. */
        fb_draw_app_bar(state, layout,
                        &(const struct fb_app_bar){.title = mesh_str(MESH_STR_TAB_NODES)});
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES, mesh_str(MESH_STR_NODES_GONE));
        return;
    }

    char name[96];
    fb_node_title(node, name, sizeof name);
    fb_draw_app_bar(state, layout,
                    &(const struct fb_app_bar){.trail = {name},
                                               .trail_count = 1U,
                                               .title = mesh_str(MESH_STR_NODE_HEAD_ACTIONS)});

    struct mesh_ui_node_item items[MESH_UI_NODE_ACTIONS_MAX];
    const uint32_t count =
        mesh_ui_node_actions_build(node, hs->has_my_info && node->node_id == hs->my_info.node_num,
                                   mesh_ui_snapshot_traceroute_view(snapshot, node->node_id),
                                   nav->node_remove_armed, items, MESH_UI_NODE_ACTIONS_MAX);
    if (count == 0U) {
        fb_draw_empty(state, layout, MESH_UI_ICON_ACTIONS, mesh_str(MESH_STR_NODES_DETAIL_EMPTY));
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
    /* The cursor is a row here and never a card, so the span is the row itself: a verb is a
       control and gets the row highlight, which is the `card` half of the detail's span being
       false for exactly the rows this screen is made of. */
    struct fb_list list =
        fb_list_begin_focus(layout, count, cursor, heights, cards, cursor, cursor, false);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        fb_node_action_row(state, &list, i, &items[i], node->node_id);
    }
}

void fb_render_nodes(struct mesh_ui_backend_fb_state *state,
                     const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (nav->node_detail_open) {
        /* The chart over the detail, the way the detail is drawn over the list. The reading is
           checked rather than a flag because it *is* the flag: MESH_UI_HISTORY_NONE is closed. */
        if (nav->node_trend != MESH_UI_HISTORY_NONE) {
            fb_render_node_trend(state, snapshot, layout);
        } else if (nav->node_actions_open) {
            /* The verbs, over the detail. Below the chart rather than above it because the two
               cannot both be up - a chart opens from a reading and the sheet from the one action
               row - so the order states which is the deeper level rather than resolving a race:
               the chart is opened *through* the detail's own rows, the sheet replaces them. */
            fb_render_node_actions(state, snapshot, layout);
        } else {
            fb_render_node_detail(state, snapshot, layout);
        }
        return;
    }
    if (!snapshot->handshake_valid || snapshot->handshake.node_count == 0U) {
        fb_draw_app_bar(state, layout,
                        &(const struct fb_app_bar){.title = mesh_str(MESH_STR_TAB_NODES)});
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES,
                      mesh_str(snapshot->handshake_valid ? MESH_STR_NODES_EMPTY_WAITING
                                                         : MESH_STR_NODES_EMPTY_DISCONNECTED));
        return;
    }

    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    /*
     * Two counts, and keeping them apart is what the filter row cost this screen.
     *
     * `held` is the published roster - what the client knows and what "of" is measured against.
     * `count` is what this list is about to draw, which is however much of it the filter
     * keeps. Every number below is one or the other on purpose: conflated, the title said
     * "Nodes 42" over three pinned rows, which is the arithmetic-no-screen-should-show rule
     * reached from the other end.
     */
    const enum mesh_ui_node_filter filter = (enum mesh_ui_node_filter)nav->node_filter;
    const enum mesh_ui_node_sort sort = (enum mesh_ui_node_sort)nav->node_sort;
    const uint32_t held =
        hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES : hs->node_count;
    /*
     * The list, built once for this frame: the rows the filter kept, in the order the sort puts
     * them. Every row below is read out of it rather than walked for, so the node a row draws
     * and the node mesh_ui_nav_node_at_row() opens are the same node by construction.
     */
    struct mesh_ui_node_view view_rows;
    mesh_ui_node_view_build(hs, filter, sort, &view_rows);
    const uint32_t count = view_rows.count;
    char title[96];
    /* Counted from the rows this screen is about to draw, so the two numbers are always in the
       same scope: the session roster holds twice what the UI carries, and a title reading
       "128 nodes, 200 off radio" would be arithmetic no screen should show. */
    const uint32_t off_radio = mesh_ui_handshake_off_radio(hs);
    /*
     * Three things can be bigger than this list, and the honest "of" is whichever is biggest.
     * The radio's database is one; the roster is the second, and it is the one that used to go
     * unsaid - it holds 256 and the UI publishes its best 128, so a busy mesh quietly dropped
     * half of what the client knew with the title still reading "128 nodes". After a NodeDB
     * reset the radio's number is the smaller of the two, which is exactly when taking the max
     * matters rather than preferring either.
     *
     * The third is the roster this screen is *itself* holding back, which is the filter. It
     * needs no title of its own: the heading already says "n of m" whenever something is
     * bigger than what is drawn, and the filter row under it is where the reader looks for
     * why. Which of the three is doing the holding back is deliberately not spelled out - one
     * sentence that says "there is more than this" is worth more than three that compete.
     */
    const uint32_t known_by_radio = hs->has_my_info ? hs->my_info.nodedb_entries : 0U;
    uint32_t known = hs->nodes_known > known_by_radio ? hs->nodes_known : known_by_radio;
    if (held > known) {
        known = held;
    }
    if (known > count) {
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_OF, count, known);
    } else if (off_radio > 0U) {
        /* The count the Status screen shows is the radio's; this one is ours, and after a
           NodeDB reset the two are nothing alike. Saying how much of the gap is nodes only we
           remember is what keeps "81 here, 2 there" from reading as a bug. */
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_OFF_RADIO, count, off_radio);
    } else {
        fb_title_count(title, sizeof title, mesh_str(MESH_STR_TAB_NODES), count, 0U);
    }
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

    const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;
    /* The discs come from the nav layer, which wants a store rather than the handshake alone -
       the same view the conversation list and the picker build, so all three ask one function. */
    struct mesh_ui_store view;
    mesh_ui_store_view(snapshot, &view);
    /*
     * The filter row, the sort row and the map row on the front of the list, and then the nodes
     * the filter kept in the order the sort put them. The count is the same arithmetic
     * mesh_ui_nav_row_count() does, and it is written out here rather than shared because the
     * nav's answer already carries the empty-roster case this branch cannot reach.
     *
     * A filter that keeps nothing still draws its lead rows and then says so on the row where
     * the first node would be - so the strip that emptied the list is still on the frame, and
     * the press that puts it back is one A away. The extra row is the *note's*, not a node's:
     * mesh_ui_nav_row_count() does not count it and the cursor cannot reach it, exactly as the
     * empty states elsewhere are not rows.
     */
    const bool nothing_matched = (count == 0U);
    /*
     * A sort that cannot measure anything says so in its own value column, and nowhere else.
     *
     * A distance sort with no fix of our own leaves the list in the order it was published in,
     * and a row reading "Distance" over a list that did not move is the control and the rows
     * disagreeing about one fact - the failure the Direct chip's note is written against,
     * arriving from the other side. It is said where the reader just pressed, in the column that
     * already says what the sort is set to, because the two alternatives are both worse: a row
     * of its own past the lead rows is a row the nav does not count and the cursor cannot reach,
     * which is only safe when there are no node rows to draw under it, and a supporting line
     * that appears with the state would change the height of the row the cursor is standing on.
     */
    const bool sort_unavailable = !mesh_ui_node_sort_available(hs, sort);
    char sort_value[40];
    if (sort_unavailable) {
        mesh_str_format(sort_value, sizeof sort_value, MESH_STR_NODES_SORT_NO_FIX,
                        mesh_str(mesh_ui_node_sort_label(sort)));
    } else {
        mesh_str_copy(sort_value, sizeof sort_value, mesh_str(mesh_ui_node_sort_label(sort)));
    }
    const uint32_t rows = count + MESH_UI_NODES_LEAD_ROWS + (nothing_matched ? 1U : 0U);
    /*
     * Every row here is two steps - a name and the line under it - except the strip, which is
     * one.
     *
     * That is the variable-height list model earning its keep rather than a special case. A
     * chip is a capsule one line advance tall and there is nothing under it to say, so a
     * two-step strip would spend a whole node row on air - on the one list in this client that
     * runs to a hundred and twenty-eight rows, which is precisely where a row costs the most.
     * Declared before the list is opened, because the model is the authority on every height
     * and can only be if it is told first.
     */
    uint8_t node_heights[MESH_UI_MAX_HANDSHAKE_NODES + MESH_UI_NODES_LEAD_ROWS + 1U];
    for (uint32_t r = 0; r < rows && r < (uint32_t)(sizeof node_heights); ++r) {
        node_heights[r] = (r == MESH_UI_NODES_FILTER_ROW || r == MESH_UI_NODES_SORT_ROW) ? 1U : 2U;
    }
    struct fb_list list =
        fb_list_begin_heights(layout, rows, nav->cursor[MESH_UI_SCREEN_NODES], node_heights);
    /*
     * The filter and the sort are one control group, drawn as two Settings field rows.
     *
     * Both were bespoke before this: the filter a chip strip that filled the row on its own, the
     * sort a label and a word. Neither carried the pencil that everything else in the client
     * puts in a row's gutter to say "this is set here", so the strip read as a caption about the
     * list rather than a control over it, and the sort read as a stated fact. The screen was
     * two rows of what looked like status above a list, and the presses that worked them were
     * named only at the bottom of the panel.
     *
     * So they are the shape this client already has for "one of a small set, chosen on the row":
     * a label naming the axis, MESH_UI_ICON_EDIT in the gutter, and the value column. Nothing
     * here is a new component - it is `struct fb_list_item` with the trailing slot the Settings
     * tab's enums already use, which is what makes the two screens answer Left and Right with
     * the same picture as well as the same key.
     *
     * One label column for both rows, measured from the longer of the two words, so the pencils
     * line up and the group reads as one block rather than as two rows that happen to adjoin.
     */
    const size_t control_label_cols = fb_field_label_cols(state, layout, 6U);
    /*
     * The filter gets the whole set and the sort gets the chosen word, and that split is a
     * measurement rather than a preference - it is FB_SEGMENTED_MAX, stated once in the
     * component and read here.
     *
     * Three filters are inside it, so all three are on the panel: the reader sees that "Direct"
     * and "Pinned" exist without pressing anything, which is the single biggest thing the chip
     * strip got right and the reason the set is still shown rather than stepped. Five sorts are
     * outside it, so the sort is the word - five equal shares of a value column are five clipped
     * words, which is the same answer the Settings tab gives a region or a modem preset. The
     * component decides between the set and the word from the room it is given, so a narrow
     * panel or a large glyph scale falls back to the word here too rather than to three pills
     * with no labels in them.
     */
    /* The set is copied into a fixed array, so the bound is checked where the two constants meet
       rather than trusted. A fourth filter is free; a fifth is a write past `labels` that only
       shows up as whatever sits after it on the stack, which is the one way this row could fail
       without looking wrong. The same guard mesh_ui_node_view::order states about the roster. */
    MESH_UI_STATIC_ASSERT((unsigned)MESH_UI_NODE_FILTER_COUNT <= FB_SEGMENTED_MAX,
                          "a filter has been added that the segmented button cannot hold");
    struct fb_segmented filter_segments = {
        .count = (size_t)MESH_UI_NODE_FILTER_COUNT,
        .active = (size_t)filter,
        .value = mesh_str(mesh_ui_node_filter_label(filter)),
    };
    for (uint32_t f = 0; f < (uint32_t)MESH_UI_NODE_FILTER_COUNT; ++f) {
        filter_segments.labels[f] =
            mesh_str(mesh_ui_node_filter_label((enum mesh_ui_node_filter)f));
    }
    struct mesh_ui_line line;
    char right[32];
    char age[8];
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
    /* How many of what the client knows has somewhere to be drawn, which is what the map row's
       supporting line says. Counted once, outside the loop: it walks the whole roster and the
       waypoint book, and the row it is for is drawn at most once. */
    struct mesh_ui_map_view markers;
    mesh_ui_map_build(&view, &markers);
    char map_line[48];
    if (markers.count > 0U) {
        mesh_str_format_plural(map_line, sizeof map_line, MESH_STR_MAP_ROW_MARKERS_ONE,
                               markers.count, markers.count);
    } else {
        /* The row stays and says why it cannot be pressed, rather than disappearing - the
           Waypoints tab's "New waypoint here" rule, and for its reason: a row that vanishes
           explains nothing to the reader wondering where the map went. */
        mesh_str_copy(map_line, sizeof map_line, mesh_str(MESH_STR_MAP_ROW_EMPTY));
    }

    uint32_t i;
    while (fb_list_next(&list, &i)) {
        if (i == MESH_UI_NODES_FILTER_ROW) {
            const struct fb_list_item filter_row = {
                .label = mesh_str(MESH_STR_NODES_FILTER_ROW),
                .label_cols = control_label_cols,
                .marker_icon = MESH_UI_ICON_EDIT,
                /* No value column: the set is the value, and the word for the chosen one is
                   inside the control, which is what lets it decide between the two forms. The
                   Settings tab's segmented rows say this the same way. */
                .trailing = {.kind = FB_TRAILING_SEGMENTED, .segmented = &filter_segments},
            };
            fb_list_item(state, &list, i, &filter_row);
            continue;
        }
        if (i == MESH_UI_NODES_SORT_ROW) {
            const struct fb_list_item sort_row = {
                .label = mesh_str(MESH_STR_NODES_SORT_ROW),
                .label_cols = control_label_cols,
                /* The same pencil the filter above it wears, and the same one a Settings field
                   wears: the gutter is where this client says a row is set rather than read. */
                .marker_icon = MESH_UI_ICON_EDIT,
                /* The order, and what it could not do - never dim, whatever it says. On this
                   list a dim row is one that cannot be pressed, and this one always can: the
                   press steps on to a sort that works. */
                .value = sort_value,
            };
            fb_list_item(state, &list, i, &sort_row);
            continue;
        }
        if (nothing_matched && i > MESH_UI_NODES_MAP_ROW) {
            /* The row that is not a row: what the filter did, where the nodes would be. Dim
               because it is not something to press - the same tone the map row takes when it
               has nothing to open. */
            fb_list_row(state, &list, i, mesh_str(MESH_STR_NODES_FILTER_NONE), MESH_UI_TONE_DIM);
            continue;
        }
        if (i == MESH_UI_NODES_MAP_ROW) {
            const struct fb_list_item map_row = {
                .leading = {.kind = FB_LEADING_ICON, .icon = MESH_UI_ICON_MAP},
                .text = mesh_str(MESH_STR_MAP_ROW),
                /* Dim when there is nothing to put on it, for the reason the "New message" row
                   is dim: it is a button among things, and one that cannot be pressed. */
                .tone = markers.count > 0U ? MESH_UI_TONE_NORMAL : MESH_UI_TONE_DIM,
                .trailing = {.kind = FB_TRAILING_ICON, .icon = MESH_UI_ICON_CHEVRON},
                .supporting = map_line,
                .supporting_quiet = true,
            };
            fb_list_item(state, &list, i, &map_row);
            continue;
        }
        /* Through the view, never by subtracting from the raw roster: the row-to-node mapping
           is mesh_ui_nav_node_at_row()'s question and this is the same answer, so the node the
           cursor opens and the node this row drew cannot be two different nodes. */
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_view_at(hs, &view_rows, i - MESH_UI_NODES_LEAD_ROWS);
        if (node == NULL) {
            continue;
        }
        const char *short_name =
            node->short_name[0] != '\0' ? node->short_name : mesh_str(MESH_STR_NODES_NO_SHORT_NAME);
        const char *long_name = node->long_name[0] != '\0' ? node->long_name : "";
        fb_format_age(node->last_heard, age, sizeof age);

        /*
         * A node the radio's NodeDB no longer carries says so in the column that would
         * otherwise hold its signal, because that is the more useful fact: the SNR is from
         * whenever we last heard it, while "off radio" is why a DM to it may never leave -
         * there is no stored key to encrypt with. The detail screen spells the same thing out.
         *
         * The branches that say something instead of a signal are the ones where there is no
         * signal *to this node* to say. An SNR is measured on the packet that arrived, so for a
         * node reached over several hops it describes the last relay and for one arriving over
         * MQTT it describes nothing on the air at all - a staircase there would be reporting
         * somebody else's link as this node's.
         *
         * The last branch is the one that matters and it is not the same test as the others:
         * `hops_away` unset means the firmware did not say, which is not the same as zero, and
         * an SNR of 0.0 is the session layer's own "no reading". Either would give a node
         * nothing was ever heard from three of four rungs. mesh_ui_node_signal_heard() is the
         * whole of that question, and everything it declines falls through to the figure this
         * column drew before - which is the right way round, because printing a number that
         * describes something else is unhelpful where drawing it is a claim.
         */
        bool direct = false;
        if (!node->in_nodedb) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_OFF_RADIO, age);
        } else if (node->has_hops_away && node->hops_away > 0U) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_HOPS, (unsigned)node->hops_away,
                            age);
        } else if (node->via_mqtt) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_MQTT, age);
        } else if (mesh_ui_node_signal_heard(node)) {
            /*
             * Heard directly, with a reading of its own: rungs and the age, and the decibels go
             * to the node's own screen.
             *
             * The figure was the column's whole content and it is the part a list cannot use.
             * "4.2dB" has to be read and then held against a threshold to mean anything, and a
             * list is forty-two of them - whereas rungs are compared against the rungs above
             * and below without being read, which is the only thing a column of signals is
             * scanned for.
             */
            direct = true;
            mesh_str_copy(right, sizeof right, age);
        } else {
            /* Hops the firmware never reported, or no reading behind the figure. Exactly the
               column this list drew before, which is why MESH_STR_NODES_ROW_SNR keeps its
               entry - and what the CLI backend, which has no staircase, draws throughout. */
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_SNR, (double)node->snr, age);
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
        const bool is_me = (me != 0U && node->node_id == me);
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
        mesh_ui_line_reset(&line);
        mesh_ui_line_column(&line, short_name, 4U);
        if (long_name[0] != '\0') {
            mesh_ui_line_printf(&line, " %s", long_name);
        }

        /* Dim behind the words, so a list that is mostly off-radio reads as one at a glance.
           The open thread's node keeps the accent whatever its NodeDB state: which node you
           are talking to is the one thing the cursor colour is for. */
        enum mesh_ui_tone tone = MESH_UI_TONE_NORMAL;
        if (node->node_id == nav->target_node) {
            tone = MESH_UI_TONE_PRIMARY;
        } else if (!node->in_nodedb) {
            tone = MESH_UI_TONE_DIM;
        }

        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .label = initials,
                    .tint = tint,
                    .role = is_me ? MESH_UI_COLOR_PRIMARY : MESH_UI_COLOR_COUNT,
                },
            .text = mesh_ui_line_text(&line),
            .marker_icon = (node->is_favorite && !is_me) ? MESH_UI_ICON_PINNED : MESH_UI_ICON_NONE,
            .marker_slot = true,
            .tone = tone,
            .trailing = direct ? (struct fb_trailing){.kind = FB_TRAILING_SIGNAL,
                                                      .text = right,
                                                      .signal = mesh_ui_signal_level(node->snr)}
                               : (struct fb_trailing){.kind = FB_TRAILING_TEXT, .text = right},
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
    }
}
