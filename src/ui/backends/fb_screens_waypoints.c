#define _POSIX_C_SOURCE 200809L

/*
 * The Waypoints tab: the list of places, and one of them opened.
 *
 * Two levels, the Nodes tab's shape - what differs is that a row's trailing column is a *range*
 * rather than a signal, because how far away a place is and which way it lies is the whole of
 * what a client with no map open can say about a point. src/ui/views/waypoints.c works both
 * out; this draws them.
 */

#include "inkcell/ui/layout.h"
#include "inkcell/ui/widgets.h"
#include "inkcell/utils/text.h"
#include "inkcell/utils/time.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/waypoints.h"

#include <stdio.h>
#include <string.h>

/* ---- the waypoints ---------------------------------------------------------------------------
 *
 * The list of places, and one of them opened. Two levels, the Nodes tab's shape - what differs
 * is that a row's trailing column is a *range* rather than a signal, because how far away a
 * place is and which way it lies is the whole of what a client with no map can say about a
 * point. src/ui/views/waypoints.c works both out; this draws them.
 */

static void fb_render_waypoint_detail(struct inkcell_backend_fb_state *state,
                                      const struct mesh_ui_snapshot *snapshot,
                                      struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_waypoint *waypoint =
        mesh_ui_waypoint_find(&snapshot->waypoints, nav->waypoint_detail_id);
    if (waypoint == NULL) {
        inkcell_fb_draw_app_bar(
            state, layout,
            &(const struct inkcell_fb_app_bar){.title = inkcell_str(MESH_STR_TAB_WAYPOINTS)});
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_POSITION,
                              inkcell_str(MESH_STR_WAYPOINTS_GONE));
        return;
    }

    /* The place's own name has the whole title line, exactly as a node's does: the tab is up
       there in the navigation bar already, so a trail would be repeating it. */
    inkcell_fb_draw_app_bar(
        state, layout,
        &(const struct inkcell_fb_app_bar){.title = waypoint->name[0] != '\0'
                                                        ? waypoint->name
                                                        : inkcell_str(MESH_STR_WAYPOINTS_UNNAMED)});

    struct mesh_ui_waypoint_item items[MESH_UI_WAYPOINT_ITEMS_MAX];
    const uint32_t count = mesh_ui_waypoint_detail_build(
        waypoint, snapshot->handshake_valid ? &snapshot->handshake : NULL, &snapshot->settings,
        /* The credible clock rather than the machine's: this screen's "Expires" row subtracts
           from it, and a Brick that has not been told the date would otherwise report every
           deadline as tens of thousands of days away rather than saying it cannot tell. */
        inkcell_time_wall_credible_s(), nav->waypoint_delete_armed, items,
        MESH_UI_WAYPOINT_ITEMS_MAX);
    if (count == 0U) {
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_POSITION,
                              inkcell_str(MESH_STR_WAYPOINTS_GONE));
        return;
    }

    const size_t label_cols = inkcell_fb_field_label_cols(state, layout, 12U);
    /* The note is the one row here that is a sentence rather than a fact, so it takes a second
       step and puts the sharer's words on it. Measured from the same `kind` the loop draws
       from, and handed to the model before anything is placed - the node detail's rule. */
    uint8_t heights[MESH_UI_WAYPOINT_ITEMS_MAX];
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = items[r].kind == MESH_UI_WAYPOINT_ITEM_NOTE ? 2U : 1U;
    }
    struct inkcell_fb_list list = inkcell_fb_list_begin_heights(
        layout, count, nav->cursor[MESH_UI_SCREEN_WAYPOINTS], heights);
    inkcell_fb_list_glide(state, &list, FB_LIST_WAYPOINT_DETAIL);
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        const struct mesh_ui_waypoint_item *item = &items[i];
        if (item->kind == MESH_UI_WAYPOINT_ITEM_HEADING) {
            inkcell_fb_list_subheader(state, &list, i, item->label);
            continue;
        }
        if (item->kind == MESH_UI_WAYPOINT_ITEM_NOTE) {
            /*
             * The sharer's own sentence, wrapped across the row's whole width rather than
             * squeezed into a value column sized for a coordinate.
             *
             * Two steps, and the wrap is the layout's own walk rather than a split at some
             * character count: upstream caps a description at ninety-nine characters and the row
             * is nearly sixty cells wide, so two lines hold every description that can exist -
             * which is why the second line is the item's `supporting` slot rather than the
             * beginning of a third row nothing would measure.
             */
            char first[MESH_UI_WAYPOINT_VALUE_MAX];
            char second[MESH_UI_WAYPOINT_VALUE_MAX];
            first[0] = '\0';
            second[0] = '\0';
            struct inkcell_wrap wrap;
            inkcell_wrap_begin(&wrap, item->value, inkcell_fb_row_cols(state, state->scale));
            if (inkcell_wrap_next(&wrap)) {
                inkcell_str_copy(first, sizeof first, wrap.line);
            }
            if (inkcell_wrap_next(&wrap)) {
                inkcell_str_copy(second, sizeof second, wrap.line);
            }
            const struct inkcell_fb_list_item row = {
                .text = first,
                .tone = INKCELL_TONE_DIM,
                .supporting = second[0] != '\0' ? second : NULL,
                .supporting_quiet = true,
            };
            inkcell_fb_list_item(state, &list, i, &row);
            continue;
        }
        if (item->kind == MESH_UI_WAYPOINT_ITEM_ACTION) {
            const struct inkcell_fb_list_item row = {
                .text = item->label,
                /* The armed delete is the one row on this screen that shouts, because the next
                   press takes the place off the mesh for everybody. */
                .tone = (item->action == (uint8_t)MESH_UI_WAYPOINT_ACTION_DELETE &&
                         nav->waypoint_delete_armed)
                            ? INKCELL_TONE_ERROR
                            : INKCELL_TONE_PRIMARY,
                .trailing = {.kind = INKCELL_FB_TRAILING_ICON, .icon = INKCELL_ICON_CHEVRON},
            };
            inkcell_fb_list_item(state, &list, i, &row);
            continue;
        }
        /* A stated fact, and its label is the question - the node detail's rule, and the same
           screen shape one tab over. Coordinates, a range and a bearing are what the reader
           opened the place to read; "Latitude" is what they already knew they were asking. */
        const struct inkcell_fb_list_item row = {
            .label = item->label,
            .label_cols = label_cols,
            .label_quiet = true,
            .value = item->value,
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
}

void fb_render_waypoints(struct inkcell_backend_fb_state *state,
                         const struct mesh_ui_snapshot *snapshot,
                         struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (nav->waypoint_detail_open) {
        fb_render_waypoint_detail(state, snapshot, layout);
        return;
    }

    struct mesh_ui_store view;
    mesh_ui_store_view(snapshot, &view);
    const uint32_t count = mesh_ui_waypoint_count(&view);
    const uint32_t places = count > 0U ? count - 1U : 0U;

    char title[96];
    if (places > 0U) {
        inkcell_str_format(title, sizeof title, MESH_STR_WAYPOINTS_TITLE_COUNT, places);
    } else {
        inkcell_str_copy(title, sizeof title, inkcell_str(MESH_STR_TAB_WAYPOINTS));
    }
    inkcell_fb_draw_app_bar(state, layout, &(const struct inkcell_fb_app_bar){.title = title});

    /*
     * The empty state still draws the list, because the list is never empty: the last row makes
     * a place, and a screen that replaced it with a picture would take away the only thing
     * there is to do here. The picture goes above the one row instead - which is why this is a
     * banner-shaped sentence rather than inkcell_fb_draw_empty()'s full-body one.
     */
    struct inkcell_fb_list list =
        inkcell_fb_list_begin_rows(layout, count, nav->cursor[MESH_UI_SCREEN_WAYPOINTS], 2U);
    inkcell_fb_list_glide(state, &list, FB_LIST_WAYPOINTS);
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        struct mesh_ui_waypoint_row waypoint;
        if (!mesh_ui_waypoint_row(&view, i, &waypoint)) {
            break;
        }
        const bool is_new = (waypoint.type == MESH_UI_WAYPOINT_ROW_NEW);
        /*
         * A place's supporting line is who shared it; the new row's is why it cannot be
         * pressed, and only when there is nothing left to say does it fall back to the empty
         * list's sentence. That order is the point: "no places have been shared yet" and "this
         * radio has no position yet" are both true on a fresh Brick, and the second is the one
         * that answers the press.
         */
        const char *supporting = waypoint.shared[0] != '\0' ? waypoint.shared : NULL;
        if (is_new && supporting == NULL && places == 0U) {
            supporting = inkcell_str(MESH_STR_WAYPOINTS_EMPTY);
        }
        const struct inkcell_fb_list_item row = {
            .leading = {.kind = INKCELL_FB_LEADING_ICON,
                        .icon = is_new ? INKCELL_ICON_COMPOSE : INKCELL_ICON_POSITION},
            .text = waypoint.name,
            /* Ours in the accent, the way the node list marks our own radio: a place you shared
               is one you can withdraw, and that is worth seeing from the list. The row that
               makes a place is dim for the same reason the "New message" row is - it is a
               button among things, not a thing. */
            .tone = is_new ? INKCELL_TONE_DIM
                           : (waypoint.ours ? INKCELL_TONE_PRIMARY : INKCELL_TONE_NORMAL),
            /* The range, in the column a node row puts its signal in - the same question asked
               of a place instead of a link. Empty when there is no answer, which draws nothing
               rather than a zero. */
            .trailing = {.kind = INKCELL_FB_TRAILING_TEXT, .text = waypoint.range},
            /* The row that makes a place has nothing shared to report, so it says what the
               screen would otherwise have had to say in a picture: that nothing is here yet.
               Once something is, it stops saying it - a list with places in it is not empty,
               and the row is then only a button. */
            .supporting = supporting,
            .supporting_quiet = true,
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
}
