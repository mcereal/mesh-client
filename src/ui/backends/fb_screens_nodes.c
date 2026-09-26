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
        inkwell_str_copy(out, out_len, name);
        return;
    }
    inkcell_str_format(out, out_len, MESH_STR_NODE_VAL_USER_ID_HEX, node->node_id);
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
    struct inkcell_fb_list list = inkcell_fb_list_begin_focus(layout, count, cursor, heights, cards,
                                                              span.first, span.last, span.card);
    inkcell_fb_list_glide(state, &list, FB_LIST_NODE_DETAIL);
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
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
            inkcell_fb_list_subheader_icon(
                state, &list, i, item->label,
                (struct inkcell_fb_leading){.kind = INKCELL_FB_LEADING_TONAL,
                                            .icon = (enum inkcell_icon)item->icon});
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
            struct inkcell_fb_meter meter = {
                .id = 0x04000000U | i,
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
            inkcell_fb_list_item(state, &list, i, &row);
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
            inkcell_fb_list_item(state, &list, i, &row);
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

void fb_render_nodes(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                     struct inkcell_fb_layout *layout) {
    if (snapshot->nav.node_detail_open) {
        fb_render_node_pane(state, snapshot, layout);
    } else {
        fb_render_node_list(state, snapshot, layout);
    }
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
    mesh_ui_node_view_build_query(hs, filter, nav->node_query, sort, &view_rows);
    const uint32_t count = view_rows.count;
    char title[96];
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
    /* With nothing held back the heading is the name alone, as on every list: the rows are
       there to be counted. A node only this client remembers says so on its own row, and the
       Status screen's roster card says how many there are, so the gap between the radio's
       count and this list's is explained where each number is. */
    mesh_ui_chrome_list_title(title, sizeof title, inkcell_str(MESH_STR_TAB_NODES), count, known,
                              0U);
    fb_draw_app_bar(state, layout, &(const struct inkcell_fb_app_bar){.title = title});

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
        inkcell_str_format(sort_value, sizeof sort_value, MESH_STR_NODES_SORT_NO_FIX,
                           inkcell_str(mesh_ui_node_sort_label(sort)));
    } else {
        inkwell_str_copy(sort_value, sizeof sort_value, inkcell_str(mesh_ui_node_sort_label(sort)));
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
        /* The lead rows are one step each - the map and the places included, whose count sits in
           the value column rather than on a line of its own: five rows of controls are the top
           of a list whose nodes are what the reader came for. */
        node_heights[r] = r < MESH_UI_NODES_LEAD_ROWS ? 1U : 2U;
    }
    /* With a node open - which is only drawn beside it, on a split frame - the tab's cursor
       indexes the detail's rows, and the list's own place is the open node's row, found by
       which node it is: the roster re-ranks under an open detail on every publish. */
    uint32_t cursor = nav->cursor[MESH_UI_SCREEN_NODES];
    if (nav->node_detail_open) {
        const uint32_t at = mesh_ui_node_view_find(hs, &view_rows, nav->node_detail_node);
        cursor = at < count ? at + MESH_UI_NODES_LEAD_ROWS : nav->node_list_cursor;
    }
    const struct inkcell_fb_list_style look = fb_list_look(state, FB_LIST_ROLE_FEED);
    struct inkcell_fb_list list =
        inkcell_fb_list_begin_styled(state, layout, rows, cursor, node_heights, NULL, &look);
    /* The rows glide and are click targets only while they are what the reader is on. A detail
       open beside them glides its own window - there is one glide slot, and two lists taking it
       in turn every frame would leave neither gliding - and registers its own rows. */
    if (!nav->node_detail_open) {
        inkcell_fb_list_glide(state, &list, FB_LIST_NODES);
        inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_ROWS);
    }
    /*
     * The filter and the sort are one control group, drawn as two Settings field rows.
     *
     * Both were bespoke before this: the filter a chip strip that filled the row on its own, the
     * sort a label and a word. Neither said in the gutter that it was set here, so the strip
     * read as a caption about the list rather than a control over it, and the sort read as a
     * stated fact. The screen was two rows of what looked like status above a list, and the
     * presses that worked them were named only at the bottom of the panel.
     *
     * So they are the shape this client already has for "one of a small set, chosen on the
     * row": a label naming the axis, the value column, and whichever mark the Settings tab
     * would give the same kind of row - which is the whole of why they are drawn this way, and
     * why the answer is not written out twice. Both carry the stepper, and the filter's
     * stands down for its segmented button whenever that button is what gets drawn - which is
     * the row's own answer rather than this screen's, because the fallback to a word is exactly
     * the shape that still needs the mark. Nothing here is a new component - it is
     * `struct inkcell_fb_list_item` with the slots the Settings tab's enums already use.
     *
     * One label column for both rows, measured from the longer of the two words, so the group
     * reads as one block rather than as two rows that happen to adjoin.
     */
    const char *const control_labels[] = {
        inkcell_str(MESH_STR_NODES_FILTER_ROW), inkcell_str(MESH_STR_NODES_SORT_ROW),
        inkcell_str(MESH_STR_NODES_FIND_ROW), inkcell_str(MESH_STR_MAP_ROW),
        inkcell_str(MESH_STR_TAB_WAYPOINTS)};
    const size_t control_label_cols = inkcell_fb_field_label_cols_fit(
        state, layout, control_labels, sizeof control_labels / sizeof control_labels[0]);
    /*
     * The filter gets the whole set and the sort gets the chosen word, and that split is a
     * measurement rather than a preference - it is INKCELL_FB_SEGMENTED_MAX, stated once in the
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
    INKCELL_STATIC_ASSERT((unsigned)MESH_UI_NODE_FILTER_COUNT <= INKCELL_FB_SEGMENTED_MAX,
                          "a filter has been added that the segmented button cannot hold");
    struct inkcell_fb_segmented filter_segments = {
        .count = (size_t)MESH_UI_NODE_FILTER_COUNT,
        .active = (size_t)filter,
        .value = inkcell_str(mesh_ui_node_filter_label(filter)),
    };
    for (uint32_t f = 0; f < (uint32_t)MESH_UI_NODE_FILTER_COUNT; ++f) {
        filter_segments.labels[f] =
            inkcell_str(mesh_ui_node_filter_label((enum mesh_ui_node_filter)f));
    }
    const bool imperial = mesh_ui_units_imperial(snapshot->settings.units);
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
        inkcell_str_format_plural(map_line, sizeof map_line, MESH_STR_MAP_ROW_MARKERS_ONE,
                                  markers.count, markers.count);
    } else {
        /* The row stays and says why it cannot be pressed, rather than disappearing - the
           Waypoints tab's "New waypoint here" rule, and for its reason: a row that vanishes
           explains nothing to the reader wondering where the map went. */
        inkwell_str_copy(map_line, sizeof map_line, inkcell_str(MESH_STR_MAP_ROW_EMPTY));
    }
    const uint32_t places = snapshot->waypoints.count > MESH_UI_MAX_WAYPOINTS
                                ? MESH_UI_MAX_WAYPOINTS
                                : snapshot->waypoints.count;
    char places_line[48];
    if (places > 0U) {
        inkcell_str_format_plural(places_line, sizeof places_line, MESH_STR_WAYPOINTS_ROW_COUNT_ONE,
                                  places, places);
    } else {
        inkwell_str_copy(places_line, sizeof places_line,
                         inkcell_str(MESH_STR_WAYPOINTS_ROW_EMPTY));
    }

    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        if (i == MESH_UI_NODES_FILTER_ROW) {
            const struct inkcell_fb_list_item filter_row = {
                /* An empty icon slot on each control row, so every label and value on the
                   screen starts in the column the map's and the places' do. */
                .leading = {.kind = INKCELL_FB_LEADING_ICON, .icon = INKCELL_ICON_NONE},
                .label = inkcell_str(MESH_STR_NODES_FILTER_ROW),
                .label_cols = control_label_cols,
                /* No value column: the set is the value, and the word for the chosen one is
                   inside the control. The stepper *stands down* for that control rather than
                   being left off, because asking for one is not getting one - too narrow a
                   panel or too large a glyph scale and the segments come back as the chosen
                   word, which is the sort row's shape below and needs the sort row's mark. The
                   Settings tab's segmented rows say this the same way. */
                .marker_icon = INKCELL_ICON_STEPPER,
                .marker_yields_to_control = true,
                .trailing = {.kind = INKCELL_FB_TRAILING_SEGMENTED, .segmented = &filter_segments},
            };
            inkcell_fb_list_item(state, &list, i, &filter_row);
            continue;
        }
        if (i == MESH_UI_NODES_SORT_ROW) {
            const struct inkcell_fb_list_item sort_row = {
                .leading = {.kind = INKCELL_FB_LEADING_ICON, .icon = INKCELL_ICON_NONE},
                .label = inkcell_str(MESH_STR_NODES_SORT_ROW),
                .label_cols = control_label_cols,
                /* Five orders is too many for a segmented button, so this row is the word -
                   and a word cannot say whether it can be changed. The stepper is what a
                   Settings enum in the same position wears, for the same reason. */
                .marker_icon = INKCELL_ICON_STEPPER,
                /* The order, and what it could not do - never dim, whatever it says. On this
                   list a dim row is one that cannot be pressed, and this one always can: the
                   press steps on to a sort that works. */
                .value = sort_value,
            };
            inkcell_fb_list_item(state, &list, i, &sort_row);
            continue;
        }
        if (i == MESH_UI_NODES_FIND_ROW) {
            const struct inkcell_fb_list_item find_row = {
                .leading = {.kind = INKCELL_FB_LEADING_ICON, .icon = INKCELL_ICON_NONE},
                .label = inkcell_str(MESH_STR_NODES_FIND_ROW),
                .label_cols = control_label_cols,
                /* The pencil: a press opens a keyboard for this row, which is that mark's one
                   meaning. The value is what was typed and nothing while nothing is - a word
                   standing in for it would read as a query. */
                .marker_icon = INKCELL_ICON_EDIT,
                .value = nav->node_query,
            };
            inkcell_fb_list_item(state, &list, i, &find_row);
            continue;
        }
        if (nothing_matched && i > MESH_UI_NODES_WAYPOINTS_ROW) {
            /* The row that is not a row: what the filter did, where the nodes would be. Dim
               because it is not something to press - the same tone the map row takes when it
               has nothing to open. A query says so in its own words, since the chip may well
               be on All. */
            inkcell_fb_list_row(state, &list, i,
                                inkcell_str(nav->node_query[0] != '\0'
                                                ? MESH_STR_NODES_FIND_NONE
                                                : MESH_STR_NODES_FILTER_NONE),
                                INKCELL_TONE_DIM);
            continue;
        }
        if (i == MESH_UI_NODES_MAP_ROW) {
            const struct inkcell_fb_list_item map_row = {
                .leading = {.kind = INKCELL_FB_LEADING_ICON, .icon = INKCELL_ICON_MAP},
                .label = inkcell_str(MESH_STR_MAP_ROW),
                .label_cols = control_label_cols,
                /* Dim when there is nothing to put on it, for the reason the "New message" row
                   is dim: it is a button among things, and one that cannot be pressed. */
                .tone = markers.count > 0U ? INKCELL_TONE_NORMAL : INKCELL_TONE_DIM,
                .trailing = {.kind = INKCELL_FB_TRAILING_ICON, .icon = INKCELL_ICON_CHEVRON},
                .value = map_line,
            };
            inkcell_fb_list_item(state, &list, i, &map_row);
            continue;
        }
        if (i == MESH_UI_NODES_WAYPOINTS_ROW) {
            /* Never dim, unlike the map row above it: the list always ends in the row that
               makes a place, so there is always something to press it for. */
            const struct inkcell_fb_list_item places_row = {
                .leading = {.kind = INKCELL_FB_LEADING_ICON, .icon = INKCELL_ICON_POSITION},
                .label = inkcell_str(MESH_STR_TAB_WAYPOINTS),
                .label_cols = control_label_cols,
                .trailing = {.kind = INKCELL_FB_TRAILING_ICON, .icon = INKCELL_ICON_CHEVRON},
                .value = places_line,
            };
            inkcell_fb_list_item(state, &list, i, &places_row);
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
        char facts[96];
        mesh_ui_node_row_facts(hs, node, imperial, facts, sizeof facts);

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
