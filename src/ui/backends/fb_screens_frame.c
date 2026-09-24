#define _POSIX_C_SOURCE 200809L

/*
 * The frame: the chrome around every screen, and the one branch that picks which screen it is.
 *
 * This is the whole of what the client says about itself rather than about a tab - the strip of
 * tabs, the progress bar, the banner, the keycaps and the line under them, the snackbar - drawn
 * in that order around whichever renderer fb_render_snapshot() calls. Every screen renderer
 * lays out against the `layout` this file hands it and none of them knows any of it happened.
 *
 * Adding a screen is a fb_render_* in a file of its own, a declaration in
 * fb_screens_internal.h, and a case here.
 */

#include "inkcell/ui/input.h"
#include "inkcell/ui/layout.h"
#include "inkcell/ui/widgets.h"
#include "inkwell/base/time.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/actions.h"
#include "mesh/ui/chrome.h"
#include "mesh/ui/commands.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The icon a screen is known by: on its tab, and again on the empty state that stands in for
 * its list. One answer in one place, because a tab and its empty screen showing two different
 * symbols for the same thing is exactly the drift a table like this prevents.
 */
static enum inkcell_icon fb_screen_icon(enum mesh_ui_screen screen) {
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return INKCELL_ICON_MESSAGES;
    case MESH_UI_SCREEN_NODES:
        return INKCELL_ICON_NODES;
    case MESH_UI_SCREEN_RADIO:
        /* The antenna the Devices tab wore, rather than the Status gauge: the tab is named for
           the thing, and the thing is a radio. */
        return INKCELL_ICON_DEVICES;
    case MESH_UI_SCREEN_SETTINGS:
        return INKCELL_ICON_SETTINGS;
    default:
        return INKCELL_ICON_NONE;
    }
}

/*
 * The tab strip and the two lines under the body were both written out here, and both were the
 * last screen-level renderers laying out their own pixels. They are components now
 * (inkcell_fb_draw_nav_bar, inkcell_fb_draw_action_bar), so what is left in this file is the
 * *content*: which tabs there are, which one is up, and what the bottom line has to say about the
 * radio.
 */

/* One chip per screen, in tab order. Static because the set never changes and the strip only
   reads it; what moves is which index is active. */
/*
 * The tab strip, and the one number on it that is about a screen the user is not looking at.
 *
 * It used to be a function of the screen enum alone, which is why an arriving message was
 * invisible from every tab but Messages: the conversation list badged its own rows, and nothing
 * carried that off the screen. The badge is the navigation bar doing what a navigation bar is
 * for - saying which of the places you are not is worth going to.
 *
 * Only the Messages tab has one, and that is not a simplification waiting to be generalised. A
 * badge has to be *clearable by going there*, exactly as a banner has to be able to resolve:
 * unread messages are, because opening the conversation marks them read. A count of nodes or of
 * waypoints would be a number that never went down however often it was looked at.
 *
 * The count is mesh_ui_nav_unread_total(), so a muted conversation contributes nothing to it -
 * see the note there for why a permanently badged tab is the same as an unbadged one.
 */
static const struct inkcell_fb_chip *fb_tab_chips(const struct mesh_ui_snapshot *snapshot) {
    static struct inkcell_fb_chip chips[MESH_UI_SCREEN_COUNT];
    /* Static because the strip points at it for the length of the draw, and the frame is built
       and drawn on one turn of the one loop this client has. The chips array above is static
       for the same reason and has always been. */
    static char unread[8];

    struct mesh_ui_store view;
    mesh_ui_store_view(snapshot, &view);
    const uint32_t total = mesh_ui_nav_unread_total(&view);
    unread[0] = '\0';
    if (total > 0U) {
        /* "99+" past two figures, which is the conversation row's rule and for the stronger
           reason: this capsule is competing with five tabs for the width of the panel. */
        if (total > 99U) {
            snprintf(unread, sizeof unread, "%s", inkcell_str(MESH_STR_MESSAGES_UNREAD_OVERFLOW));
        } else {
            snprintf(unread, sizeof unread, "%u", (unsigned)total);
        }
    }

    for (int i = 0; i < MESH_UI_SCREEN_COUNT; ++i) {
        const enum mesh_ui_screen screen = (enum mesh_ui_screen)i;
        chips[i].icon = fb_screen_icon(screen);
        chips[i].label = mesh_ui_screen_name(screen);
        chips[i].badge = (screen == MESH_UI_SCREEN_MESSAGES) ? unread : "";
        /* What a click on the tab names - see src/ui/nav/nav_click.c. */
        chips[i].focus_id = (uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)i;
    }
    return chips;
}

/*
 * What the foot of the frame says about the link: the radio it found, or what the transport is
 * doing and how to leave.
 *
 * `tone` is the second half of the same sentence - a link that is up is worth saying in the
 * success colour, and one that is not is not worth shouting about - so the two are decided
 * together here rather than by the widget, which has no idea what the words mean.
 *
 * It rides the end of the one-row footer now rather than a line of its own, so the healthy case
 * is the radio's name alone: "running: Home Base" on every frame was a log line, and the colour
 * already says it is running. Anything else keeps the whole sentence, because a link that is
 * not up is the case where the transport's own words are the useful part - and the bar drops
 * the status first when the row runs short, so a long one costs no verb its place.
 */
static void fb_link_summary(const struct mesh_ui_snapshot *snapshot, struct inkcell_line *line,
                            enum inkcell_tone *tone) {
    inkcell_line_reset(line);
    const char *status = snapshot->transport_status[0] != '\0'
                             ? snapshot->transport_status
                             : inkcell_str(MESH_STR_HEADER_TRANSPORT_STARTING);
    const struct mesh_ui_device *device = mesh_ui_snapshot_connected_device(snapshot);
    if (device != NULL) {
        inkcell_line_printf(line, "%s", fb_device_label(device));
        *tone = INKCELL_TONE_SUCCESS;
        return;
    }
    inkcell_line_str(line, MESH_STR_HEADER_STATUS_QUIT, status, inkcell_input_quit_hint());
    *tone = INKCELL_TONE_DIM;
}

/*
 * The link, as the heading's status mark, for a frame that has no foot to say it at the end of:
 * the radio's name in the success tone while one is attached, and what the transport is doing,
 * dimmed, while none is. A pointer frame only - see fb_heading_begin(). The quit hint the foot
 * adds while nothing is attached has the window's close box to stand for it there.
 */
static struct inkcell_fb_bar_status fb_link_status(const struct mesh_ui_snapshot *snapshot) {
    const struct mesh_ui_device *device = mesh_ui_snapshot_connected_device(snapshot);
    if (device != NULL) {
        return (struct inkcell_fb_bar_status){.icon = INKCELL_ICON_LINK,
                                              .tone = INKCELL_TONE_SUCCESS,
                                              .text = fb_device_label(device)};
    }
    return (struct inkcell_fb_bar_status){
        .icon = INKCELL_ICON_LINK,
        .tone = INKCELL_TONE_DIM,
        .text = snapshot->transport_status[0] != '\0'
                    ? snapshot->transport_status
                    : inkcell_str(MESH_STR_HEADER_TRANSPORT_STARTING),
    };
}

/*
 * What the frame hands the first heading drawn on it, for a pointer: the screen's verbs, and the
 * link's state that the foot would otherwise have ended in. On the device it hands nothing - the
 * keycaps and the link stay at the foot. See fb_draw_app_bar().
 *
 * Worked out once per frame, before any screen draws, because both are facts about the frame
 * rather than about a screen: the link is the client's, and the verbs are the command set every
 * other presentation of them is projected from. A screen renderer says its title and nothing
 * else, as it always has.
 */
struct fb_heading {
    /* A heading has taken these already this frame - a sheet's bar over a list's is the second
       one drawn, and the verbs and the link are said once. */
    bool claimed;
    /* Headings to let by before claiming: the list pane's, on a split frame whose detail is
       where the reader is - the verbs are the thread's then, not the list's. */
    uint8_t pass;
    struct inkcell_fb_bar_status status;
    struct inkcell_fb_bar_action actions[MESH_UI_HEADING_ACTIONS_MAX];
    size_t count;
};

/* The same snapshot and geometry can only move inside the bounds declared by animated
   widgets. Re-run composition through a clip so overlapping chrome is restored in draw order. */
struct inkcell_fb_render_cache {
    struct mesh_ui_snapshot snapshot;
    const struct inkcell_theme *theme;
    const struct inkcell_i18n_locale *locale;
    int scale;
    int width, height; /* the panel the memo was measured against */
    time_t second;
    bool valid;
    /* Whether the last frame was part of a move between two places - see fb_render_begin(). */
    bool moved;
    /* And what the layers were last told to say, which is a memo of a different kind: the one
       above is this frame compared with the last, and these outlive the answer that closed
       them. One per enum fb_overlay_id, indexed by it. See struct fb_overlay_memo. */
    struct fb_overlay_memo overlays[FB_OVERLAY_REACTIONS + 1];
    /* And where the bodies that are positioned rather than windowed have got to. Kept here for
       the same reason: a scroll is a fact about a screen that outlives one frame, and a screen
       renderer is a function with nowhere of its own to put one. */
    struct inkcell_scroll scrolls[FB_SCROLL_COUNT];
    /*
     * And the boxes the frame drew, which is the one memo here that is not a memory at all.
     *
     * It is rebuilt from nothing every frame - that is the whole of its honesty - and it is
     * kept on the cache for the same reason the scrolls are: a press happens between two
     * frames, so the map has to outlive the call that built it, and a renderer is a function
     * with nowhere of its own to put one.
     */
    struct inkcell_focus_item focus_storage[MESH_UI_FOCUS_MAX];
    struct inkcell_focus_map focus;
    /* Where the last frame's content stood: the panel less the tab rail, when the width class
       put one beside it - and on a split frame, the pane the reader is in. See
       fb_render_content(). */
    struct inkcell_box content;
    /* Rebuilt every frame, like the focus map: see struct fb_heading. */
    struct fb_heading heading;
    /* Whether the last frame stood its list and detail side by side - see fb_render_split_pair().
     */
    bool split;
};

struct inkcell_box fb_render_content(const struct inkcell_draw_state *state) {
    const struct inkcell_fb_render_cache *const cache = state != NULL ? state->render_cache : NULL;
    if (cache == NULL || inkcell_box_is_empty(cache->content)) {
        return (struct inkcell_box){0, 0, inkcell_fb_panel_width(state),
                                    inkcell_fb_panel_height(state)};
    }
    return cache->content;
}

/*
 * Whether something is drawn over the screen that answers for itself - a dialog, a sheet, the
 * tapbacks, a menu. The command set is that layer's then, and its answers are on it; the same
 * verbs in the heading under its scrim would be a second copy of the dialog nobody can press.
 */
static bool fb_layer_up(const struct mesh_ui_nav *nav) {
    return nav->confirm_open || nav->verify_open || nav->reaction_open || nav->context_open ||
           nav->node_actions_open || nav->help_open;
}

/*
 * Whether the screen under the layers draws an app bar for the verbs to go into. Status is the
 * one that does not - its cards are its heading, and each card carries its own verb - and help
 * draws a large title of its own. A pointer on either keeps the keycap bar, which is the only
 * other place those verbs are said.
 */
static bool fb_route_headed(const struct mesh_ui_nav *nav) {
    struct mesh_ui_route body;
    mesh_ui_route_under_layers(nav, &body);
    return body.level != MESH_UI_ROUTE_HELP &&
           !(body.level == MESH_UI_ROUTE_LIST && nav->screen == MESH_UI_SCREEN_RADIO);
}

static void fb_heading_begin(struct fb_heading *heading, const struct inkcell_draw_state *state,
                             const struct mesh_ui_snapshot *snapshot) {
    memset(heading, 0, sizeof *heading);
    if (!state->pointer || fb_layer_up(&snapshot->nav) || !fb_route_headed(&snapshot->nav)) {
        return;
    }
    struct mesh_ui_heading_action verbs[MESH_UI_HEADING_ACTIONS_MAX];
    heading->count = mesh_ui_actions_heading(snapshot, verbs, MESH_UI_HEADING_ACTIONS_MAX);
    bool emphasized = false;
    for (size_t i = 0U; i < heading->count; ++i) {
        /* The first verb the screen can be for is its pill - see mesh_ui_heading_action. */
        const bool pill = verbs[i].primary && !emphasized;
        emphasized = emphasized || pill;
        heading->actions[i] = (struct inkcell_fb_bar_action){
            .icon = verbs[i].icon,
            .label = inkcell_str(verbs[i].label),
            .emphasized = pill,
            .family = verbs[i].destructive ? INKCELL_FAMILY_ERROR : INKCELL_FAMILY_PRIMARY,
            .focus_id = (uint32_t)MESH_UI_FOCUS_BAR + (uint32_t)verbs[i].id,
        };
    }
    /* The verbs took the foot away, so the link the foot ended in comes up with them. */
    if (heading->count > 0U) {
        heading->status = fb_link_status(snapshot);
    }
}

struct inkcell_fb_app_bar_fit fb_draw_app_bar(const struct inkcell_draw_state *state,
                                              struct inkcell_fb_layout *layout,
                                              const struct inkcell_fb_app_bar *bar) {
    struct inkcell_fb_render_cache *const cache = state != NULL ? state->render_cache : NULL;
    if (cache == NULL || bar == NULL || cache->heading.claimed ||
        bar->mode != INKCELL_FB_APP_BAR_NORMAL) {
        return inkcell_fb_draw_app_bar(state, layout, bar);
    }
    if (cache->heading.pass > 0U) {
        cache->heading.pass--;
        return inkcell_fb_draw_app_bar(state, layout, bar);
    }
    struct inkcell_fb_app_bar drawn = *bar;
    if (drawn.status.icon == INKCELL_ICON_NONE) {
        drawn.status = cache->heading.status;
    }
    if (drawn.action_count == 0U) {
        drawn.actions = cache->heading.actions;
        drawn.action_count = cache->heading.count;
    }
    cache->heading.claimed = true;
    return inkcell_fb_draw_app_bar(state, layout, &drawn);
}

struct fb_overlay_memo *fb_overlay_memo(struct inkcell_draw_state *state, enum fb_overlay_id id) {
    struct inkcell_fb_render_cache *const cache = state != NULL ? state->render_cache : NULL;
    if (cache == NULL || (unsigned)id >= (sizeof cache->overlays / sizeof cache->overlays[0])) {
        return NULL;
    }
    return &cache->overlays[id];
}

struct inkcell_scroll *fb_scroll(struct inkcell_draw_state *state, enum fb_scroll_id id) {
    struct inkcell_fb_render_cache *const cache = state != NULL ? state->render_cache : NULL;
    if (cache == NULL || (unsigned)id >= (unsigned)FB_SCROLL_COUNT) {
        return NULL;
    }
    return &cache->scrolls[id];
}

void fb_scroll_report(struct inkcell_draw_state *state, bool moving) {
    struct fb_app *const app = fb_app_of(state);
    if (app != NULL) {
        app->scrolling = moving;
    }
}

uint32_t fb_overlay_subject(struct inkcell_draw_state *state, enum fb_overlay_id id, bool up,
                            uint32_t subject) {
    struct fb_overlay_memo *const memo = fb_overlay_memo(state, id);
    if (memo == NULL) {
        /* No memo behind this frame: the layer is whatever the app says it is right now, which
           is right while it is up and is the end of it when it is not. */
        return up ? subject : 0U;
    }
    if (up && subject != 0U) {
        memo->subject = subject;
    } else if (!inkcell_fb_overlay_showing(state, (uint32_t)id)) {
        /* Nothing of it left on the panel. Forgotten here rather than when the app let go of
           it, which is the lifetime rule the layer itself follows one call down. */
        memo->subject = 0U;
    }
    return memo->subject;
}

/*
 * See fb_screens_internal.h. `nav_y` is the region's own top: there is no navigation bar inside
 * a sheet or a scrolled body, and nothing in either may ask whether there is a screen behind it
 * to go back to - that is the frame's question and the frame has already answered it.
 */
struct inkcell_fb_layout fb_layout_in(const struct inkcell_fb_layout *layout,
                                      struct inkcell_fb_rect box) {
    struct inkcell_fb_layout out = *layout;
    out.nav_y = box.y;
    out.body_y = box.y;
    out.footer_y = box.y + box.h;
    out.body_w = box.w;
    out.rows = out.line > 0 ? (uint32_t)(box.h / out.line) : 0U;
    out.back = false;
    return out;
}

struct inkcell_fb_list_style fb_list_look(const struct inkcell_draw_state *state,
                                          enum fb_list_role role) {
    const bool roomy = role != FB_LIST_ROLE_FEED ||
                       (state != NULL && inkcell_fb_width_class(state) != INKCELL_WIDTH_COMPACT);
    return (struct inkcell_fb_list_style){
        .appearance =
            role == FB_LIST_ROLE_FEED ? INKCELL_FB_LIST_PLAIN : INKCELL_FB_LIST_INSET_GROUPED,
        .density = roomy ? INKCELL_FB_LIST_COMFORTABLE : INKCELL_FB_LIST_COMPACT,
        .focus = INKCELL_FB_LIST_FOCUS_ACCENT,
        .type = INKCELL_FB_LIST_TYPE_TIERED,
        .separators = roomy,
    };
}

struct inkcell_fb_list fb_list_begin_steps(const struct inkcell_draw_state *state,
                                           const struct inkcell_fb_layout *layout, uint32_t count,
                                           uint32_t cursor, uint8_t per_item, uint8_t *steps,
                                           size_t capacity, enum fb_list_role role) {
    if (steps == NULL || count > capacity) {
        return inkcell_fb_list_begin_rows(layout, count, cursor, per_item);
    }
    memset(steps, per_item, count);
    const struct inkcell_fb_list_style look = fb_list_look(state, role);
    return inkcell_fb_list_begin_styled(state, layout, count, cursor, steps, NULL, &look);
}

/*
 * A right-click menu: the row's own verbs, at the pointer.
 *
 * The verbs are the active command set's row operations, so the menu cannot offer what the
 * screen would not do. B is a way back rather than a verb about the row, and help is about the
 * screen. The rows retain the established A, X, Y, Start order, while their explicit focus ids
 * are `MESH_UI_FOCUS_MENU + command` - display position and command identity are independent.
 *
 * Drawn over everything, the action bar included, with one target under the whole panel first,
 * so a click that misses the menu lands on that and puts it down.
 */
static void fb_render_context(struct inkcell_draw_state *state,
                              const struct mesh_ui_snapshot *snapshot) {
    struct mesh_ui_command_set commands;
    mesh_ui_commands_for(snapshot, &commands);
    struct inkcell_fb_menu_item items[MESH_UI_CONTEXT_COMMANDS_MAX];
    uint32_t focus_ids[MESH_UI_CONTEXT_COMMANDS_MAX];
    memset(items, 0, sizeof items);
    memset(focus_ids, 0, sizeof focus_ids);
    size_t offered = 0U;
    for (size_t i = 0; i < commands.count; ++i) {
        const struct mesh_ui_command *const command = &commands.items[i];
        const int order = mesh_ui_command_context_order(command);
        if (order >= 0 && (size_t)order < MESH_UI_CONTEXT_COMMANDS_MAX) {
            items[order].label = inkcell_str(command->label);
            focus_ids[order] = (uint32_t)MESH_UI_FOCUS_MENU + (uint32_t)command->id;
            offered += 1U;
        }
    }
    const struct inkcell_fb_menu menu = {
        .items = items,
        .count = MESH_UI_CONTEXT_COMMANDS_MAX,
        .cursor = UINT32_MAX,
        .focus_ids = focus_ids,
    };
    const struct inkcell_fb_rect panel = {
        .x = 0, .y = 0, .w = inkcell_fb_panel_width(state), .h = inkcell_fb_panel_height(state)};
    const struct inkcell_fb_rect box = inkcell_fb_menu_box(state, &menu, panel.w);
    struct inkcell_overlay_frame frame;
    if (!inkcell_fb_overlay_begin(state,
                                  &(struct inkcell_overlay){
                                      .id = (uint32_t)FB_OVERLAY_CONTEXT,
                                      .up = snapshot->nav.context_open && offered > 0U,
                                      .placement = INKCELL_OVERLAY_ANCHOR,
                                      .travel = INKCELL_OVERLAY_TRAVEL_NEAR,
                                      .w = box.w,
                                      .h = box.h,
                                      .anchor = {.x = snapshot->nav.context_x,
                                                 .y = snapshot->nav.context_y,
                                                 .w = 0,
                                                 .h = 0},
                                      .modal = true,
                                      /* Over the body and not of it, and the menu's own corner
                                         - inkcell_fb_draw_menu() fills with SHAPE_MD. */
                                      .elevation = INKCELL_ELEVATION_FLOATING,
                                      .shape = INKCELL_SHAPE_MD,
                                  },
                                  &frame)) {
        return;
    }
    if (snapshot->nav.context_open) {
        inkcell_fb_target_register(state, (uint32_t)MESH_UI_FOCUS_MENU_DISMISS, &panel);
    }
    inkcell_fb_draw_menu(state, frame.box, &menu);
    inkcell_fb_overlay_end(state, &frame);
}

bool fb_sheet_begin(struct inkcell_draw_state *state, const struct inkcell_fb_layout *layout,
                    enum fb_overlay_id id, bool up, const struct inkcell_fb_sheet *sheet,
                    int content_h, struct inkcell_overlay_frame *frame,
                    struct inkcell_fb_layout *out) {
    if (state == NULL || layout == NULL || sheet == NULL || frame == NULL || out == NULL) {
        return false;
    }
    /*
     * The region a sheet belongs to, and therefore what its scrim dims: the body, and not the
     * tab strip above it or the keycaps below. The dialog states the argument at length - the
     * application has not been replaced, and the keycaps in particular are how the sheet gets
     * closed. Beside a rail it is the region, for the same reason: the rail is the tab strip.
     */
    const struct inkcell_fb_rect body = {
        .x = inkcell_fb_region(state).x,
        .y = layout->nav_y,
        .w = inkcell_fb_region(state).w,
        .h = layout->footer_y - layout->nav_y,
    };
    if (!inkcell_fb_overlay_begin(state,
                                  &(struct inkcell_overlay){
                                      .id = (uint32_t)id,
                                      .up = up,
                                      .placement = INKCELL_OVERLAY_BOTTOM,
                                      /* The whole way, from off the panel. A container that
                                         slid in from just below its resting place reads as a
                                         nudge; a sheet is something arriving. */
                                      .travel = INKCELL_OVERLAY_TRAVEL_OFF_PANEL,
                                      .w = body.w,
                                      .h = inkcell_fb_sheet_height(state, sheet, content_h),
                                      .bounds = body,
                                      .scrim = true,
                                      .modal = true,
                                      /* Over everything and waiting on an answer, which is
                                         the modal level. The sheet's top corners are
                                         inkcell_fb_draw_sheet()'s SHAPE_LG; its square foot
                                         is off the bottom of the body anyway. */
                                      .elevation = INKCELL_ELEVATION_MODAL,
                                      .shape = INKCELL_SHAPE_LG,
                                  },
                                  frame)) {
        return false;
    }
    *out = fb_layout_in(layout, inkcell_fb_draw_sheet(state, frame->box, sheet));
    return true;
}

void fb_sheet_end(struct inkcell_draw_state *state, struct inkcell_overlay_frame *frame) {
    inkcell_fb_overlay_end(state, frame);
}

void fb_render_cache_free(struct inkcell_draw_state *state) {
    free(state->render_cache);
    state->render_cache = NULL;
}

static void fb_render_begin(struct inkcell_draw_state *state,
                            const struct mesh_ui_snapshot *snapshot) {
    state->clip_active = false;
    /* Allocated whether or not partial redraw is on: what hangs off it is no longer only the
       comparison below. A capture renders with partial redraw disabled, and a dialog that could
       not remember its own words there would be a dialog that never leaves in the one tool that
       looks at it - see struct fb_dialog_memo. */
    if (state->render_cache == NULL) {
        state->render_cache = calloc(1U, sizeof *state->render_cache);
    }
    struct inkcell_fb_render_cache *cache = state->render_cache;
    if (!state->partial_disabled && cache != NULL) {
        const time_t second = (time_t)inkwell_time_wall_s();
        /*
         * A move between two places is never drawn under the band, and neither is the frame
         * that lands it.
         *
         * A slide runs on a snapshot that is not changing, so the comparison below would pass
         * on every frame of one - and it does enter the clip whenever a layer on the panel (a
         * snackbar, a dialog walking out) carried its box into this frame as damage. The clear
         * is then cut to that box, and the arriving screen, which declares the whole body as
         * damage and is let through the band because of it, is drawn over the screen it is
         * replacing rather than over the ground: every word of both, until something repaints
         * the panel. The frame after the last one is the same case with the body one small
         * step out of place, which is why the memo is kept.
         */
        /*
         * The focus ring travelling is a move too, and for the same reason: its path runs over
         * rows the snapshot says have not changed, so a band cut to what the last frame declared
         * would leave the ring painted where it was a frame ago. A journey is a motion token
         * long, so this costs a handful of whole frames per press.
         */
        const bool moving = inkcell_fb_transition_offset(state) != 0 ||
                            inkcell_anim_active(&state->focus_ring.travel, state->now_ms);
        const bool moved = cache->moved;
        cache->moved = moving;
        cache->snapshot.update_flags = snapshot->update_flags;
        state->clip_active =
            !moving && !moved && cache->valid && state->animation_damage.valid &&
            cache->theme == state->theme && cache->locale == inkcell_i18n_locale() &&
            cache->scale == state->scale && cache->width == inkcell_fb_panel_width(state) &&
            cache->height == inkcell_fb_panel_height(state) && cache->second == second &&
            memcmp(&cache->snapshot, snapshot, sizeof *snapshot) == 0;
        state->clip = state->animation_damage;
        cache->snapshot = *snapshot;
        cache->theme = state->theme;
        cache->locale = inkcell_i18n_locale();
        cache->scale = state->scale;
        cache->width = inkcell_fb_panel_width(state);
        cache->height = inkcell_fb_panel_height(state);
        cache->second = second;
        cache->valid = true;
    }
    state->animation_damage.valid = false;
}

/*
 * Whether the place the reader is standing has a list and a detail that could stand side by side.
 *
 * Messages (the conversations and the thread open from one of them), Nodes (the roster and
 * whatever is open over one node: its detail, a chart of a reading, the sheet of its verbs) and
 * Settings (the section list and the open section, a module or a channel slot included: the list
 * keeps the top-level row they are under). The share and contact sheets are not: they are raised
 * by a row and take the body, as on the Brick. The scaffold decides whether there is room
 * (inkcell/ui/widgets/scaffold.h); this only says the screen has the two halves.
 */
static bool fb_route_split(const struct mesh_ui_route *body) {
    switch (body->screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return body->level == MESH_UI_ROUTE_LIST || body->level == MESH_UI_ROUTE_THREAD;
    case MESH_UI_SCREEN_NODES:
        return body->level == MESH_UI_ROUTE_LIST || body->level == MESH_UI_ROUTE_NODE ||
               body->level == MESH_UI_ROUTE_NODE_ACTIONS || body->level == MESH_UI_ROUTE_TREND;
    case MESH_UI_SCREEN_SETTINGS:
        return body->level == MESH_UI_ROUTE_LIST || body->level == MESH_UI_ROUTE_SECTION ||
               body->level == MESH_UI_ROUTE_CHANNEL;
    default:
        return false;
    }
}

/*
 * And whether this frame is one: the route's answer, less a node opened from the map. That detail
 * is the same route as one opened from the list, but B takes it back to the map rather than to the
 * roster, so the roster beside it would be a place the reader did not come from and cannot reach.
 */
static bool fb_frame_split(const struct mesh_ui_snapshot *snapshot,
                           const struct mesh_ui_route *body) {
    return fb_route_split(body) &&
           !(body->screen == MESH_UI_SCREEN_NODES && snapshot->nav.map_open);
}

bool fb_render_split_pair(const struct inkcell_draw_state *state, const struct mesh_ui_route *from,
                          const struct mesh_ui_route *to) {
    const struct inkcell_fb_render_cache *const cache = state != NULL ? state->render_cache : NULL;
    return cache != NULL && cache->split && from != NULL && to != NULL &&
           from->screen == to->screen && fb_route_split(from) && fb_route_split(to);
}

/*
 * A tab in two panes: its list, and beside it whatever is open from the list or a note saying
 * where it will be.
 *
 * The nav is the one-pane nav, unchanged, and that is the whole design. With nothing open the
 * d-pad walks the list and A opens a row; with one open every press is the detail's, and B
 * closes it - exactly as on the Brick, where the detail replaces the list. What the width buys is
 * that the list does not go anywhere: it stays in its pane, the row the detail came from still
 * under its cursor, so the reader can see where they are and where B will take them.
 *
 * So the back arrow moves with the reader. With a detail open it is the detail's heading that
 * leaves, and the list's heading - one level up, and still in view - draws none.
 */
static void fb_render_split(struct inkcell_draw_state *state,
                            const struct mesh_ui_snapshot *snapshot,
                            const struct inkcell_fb_scaffold_frame *frame,
                            struct inkcell_fb_layout *layout, bool back) {
    struct inkcell_fb_render_cache *const cache = state->render_cache;
    const struct mesh_ui_nav *nav = &snapshot->nav;
    bool reading;
    switch (nav->screen) {
    case MESH_UI_SCREEN_NODES:
        reading = nav->node_detail_open;
        break;
    case MESH_UI_SCREEN_SETTINGS:
        reading = nav->settings_section != MESH_UI_SETTINGS_NO_SECTION;
        break;
    case MESH_UI_SCREEN_MESSAGES:
    default:
        reading = nav->thread_open;
        break;
    }
    if (cache != NULL && reading) {
        cache->heading.pass = 1U;
    }
    layout->back = back && !reading;
    switch (nav->screen) {
    case MESH_UI_SCREEN_NODES:
        fb_render_node_list(state, snapshot, layout);
        break;
    case MESH_UI_SCREEN_SETTINGS:
        fb_render_settings_list(state, snapshot, layout);
        break;
    case MESH_UI_SCREEN_MESSAGES:
    default:
        fb_render_conversations(state, snapshot, layout);
        break;
    }

    /* Where the reader is, for whatever reads the frame's body back - the pane they are in. */
    if (cache != NULL) {
        const struct inkcell_box pane = reading ? frame->detail : frame->list;
        cache->content.x = pane.x;
        cache->content.w = pane.w;
    }
    struct inkcell_fb_layout detail = inkcell_fb_scaffold_detail(state, frame, NULL);
    if (reading) {
        detail.back = back;
        switch (nav->screen) {
        case MESH_UI_SCREEN_NODES:
            fb_render_node_pane(state, snapshot, &detail);
            break;
        case MESH_UI_SCREEN_SETTINGS:
            fb_render_settings(state, snapshot, &detail);
            break;
        case MESH_UI_SCREEN_MESSAGES:
        default:
            fb_render_thread(state, snapshot, &detail);
            break;
        }
    } else {
        switch (nav->screen) {
        case MESH_UI_SCREEN_NODES:
            inkcell_fb_draw_empty(state, &detail, INKCELL_ICON_NODES,
                                  inkcell_str(MESH_STR_NODES_PICK));
            break;
        case MESH_UI_SCREEN_SETTINGS:
            inkcell_fb_draw_empty(state, &detail, INKCELL_ICON_SETTINGS,
                                  inkcell_str(MESH_STR_SETTINGS_PICK));
            break;
        case MESH_UI_SCREEN_MESSAGES:
        default:
            inkcell_fb_draw_empty(state, &detail, INKCELL_ICON_MESSAGES,
                                  inkcell_str(MESH_STR_MESSAGES_PICK));
            break;
        }
    }
    /* The layers that follow - a message's faces, a question - are about the detail, so they
       stand over its pane rather than over the list beside it. */
    *layout = detail;
}

void fb_render_snapshot(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot) {
    /* The theme and the move are settled before this call, and what the *last* frame wanted of
       the basemap has already been forgotten: all three are inkcell calling up into fb_app.c,
       which is where the facts a snapshot does not carry are pushed down. */
    fb_render_begin(state, snapshot);

    /*
     * The frame's record of what it drew, opened before anything is.
     *
     * A screen that registers nothing simply leaves the map empty, and a press falls back to
     * the arithmetic it always used - so this is pushed in for every frame rather than by the
     * screens that have adopted it. What it costs an unregistered screen is one memset.
     */
    struct inkcell_fb_render_cache *const cache = state->render_cache;
    if (cache != NULL) {
        inkcell_focus_begin(&cache->focus, cache->focus_storage, MESH_UI_FOCUS_MAX);
        inkcell_fb_set_focus_map(state, &cache->focus);
    }

    inkcell_fb_clear(state, inkcell_fb_color(state, INKCELL_COLOR_BG));

    /*
     * Which buttons mean something here is no longer decided in this function.
     *
     * It used to be a branch per screen alongside the one below - the same conditions written
     * out twice, once to pick a renderer and once to pick a hint sentence, which is two places
     * to remember when a screen grows a press. mesh_ui_actions_for() answers from the snapshot
     * instead, walking the same chain of overlays, and it is a unit test's business rather than
     * a screenshot's.
     *
     * It is asked *before* anything is drawn because both ends of the frame read it: the
     * action bar at the bottom says what B does, and the top app bar's leading slot draws an
     * arrow when what B does is leave. Two answers from one table is the whole point - a screen
     * deciding its own back arrow would be free to disagree with the keycap under it.
     */
    struct inkcell_action_bar actions;
    mesh_ui_actions_for(snapshot, &actions);
    const bool back = mesh_ui_action_bar_goes_back(&actions);
    /* One row at the foot, spent on the screen's own verbs: the tabs and the way back are
       already on the panel as the tab strip and the heading's arrow. See
       mesh_ui_actions_compact() for why nothing else is dropped. */
    mesh_ui_actions_compact(&actions, back);
    if (cache != NULL) {
        fb_heading_begin(&cache->heading, state, snapshot);
    }
    /*
     * And with a pointer, no foot at all: the verbs are the heading's actions, where a pointer
     * reader looks for them, and a row of keycaps would be a legend for buttons the reader is not
     * holding. The link moves up with them as the heading's status mark.
     *
     * Only where the heading has taken the verbs. Status, whose cards are its heading, and any
     * layer the heading does not speak for - a dialog, a sheet, the tapbacks, help - keep the
     * bar, since its answers are not in the heading.
     */
    const bool footless = state->pointer && cache != NULL && cache->heading.count > 0U;

    /*
     * The two things the *client* says about itself, rather than what any screen says about
     * itself: the progress hairline and the banner. Both take their content from
     * src/ui/tables/chrome.c for the reason the action bar takes its verbs from
     * src/ui/tables/actions.c - which states are worth a notice is a fact about the client, not
     * about a framebuffer, and it is a unit test's business rather than a screenshot's.
     */
    struct mesh_ui_banner banner;
    struct inkcell_fb_banner drawn_banner;
    const bool has_banner = mesh_ui_chrome_banner(snapshot, &banner);
    if (has_banner) {
        drawn_banner = (struct inkcell_fb_banner){
            .icon = banner.icon,
            .text = inkcell_str(banner.text),
            .supporting =
                banner.supporting != INKCELL_STR_NONE ? inkcell_str(banner.supporting) : NULL,
            .detail = banner.detail,
            .family = banner.family,
        };
    }

    /*
     * Where all of that goes is inkcell's answer, not this file's: the tabs, the hairline, the
     * banner, the body and the keycaps are placed by the scaffold from the frame's width class.
     *
     * Compact keeps the tab strip across the top rather than the scaffold's bottom bar, and that
     * is about the case rather than the screen: the Brick's L1 and R1 step through the tabs and
     * sit on the top edge, and the keycap bar already owns the bottom one. On anything wider -
     * the SDL window on a desktop - the tabs become a rail down the leading edge and the body
     * keeps the rest, which is the room a medium window has to spare. On the Brick this is the
     * frame the hand-built sequence it replaced drew, to the pixel.
     *
     * A screen with a list and a detail says so (fb_route_split()), and a window with room for a
     * measured detail beside the list then draws both. Everywhere else - the Brick above all -
     * it is one pane, and the nav is the same either way: see fb_render_split().
     */
    struct mesh_ui_route body;
    mesh_ui_route_under_layers(&snapshot->nav, &body);
    const struct inkcell_fb_scaffold scaffold = {
        .destinations = fb_tab_chips(snapshot),
        .count = MESH_UI_SCREEN_COUNT,
        .active = (size_t)snapshot->nav.screen,
        .compact_nav = INKCELL_FB_COMPACT_NAV_TOP,
        .footer = !footless,
        /* One row rather than two: the keycaps and the link's state share it, and the body gets
           the other back. The two-line bar - every press spelled out over a log line - is the
           frame that read as a launcher's HUD rather than an app. */
        .footer_kind = footless ? INKCELL_FB_FOOTER_NONE : INKCELL_FB_FOOTER_COMPACT,
        .back = back,
        .split = fb_frame_split(snapshot, &body),
        .busy = mesh_ui_chrome_busy(snapshot),
        .banner = has_banner ? &drawn_banner : NULL,
    };
    struct inkcell_fb_scaffold_frame frame;
    inkcell_fb_scaffold_begin(state, &scaffold, &frame);
    struct inkcell_fb_layout layout = frame.layout;
    if (cache != NULL) {
        cache->content = frame.content;
    }

    /*
     * And whether this frame is part of a move between two places, which is the one thing about
     * it that is not a function of the snapshot - see inkcell_fb_transition_offset().
     *
     * The band is the body - below the tabs, above the keycaps and beside the rail - because
     * those are the same on both sides of any move. What is drawn *before* it, the hairline and
     * the banner, stays put for the same reason, being about the client rather than about the
     * screen that is arriving. The scaffold also declares the band as animation damage: the
     * partial-redraw path assumes nothing it has not been told about is moving, which is why
     * fb_render_begin() does not enter the band at all while a move is running.
     */
    const int slide = inkcell_fb_scaffold_transition(state, &frame);

    /*
     * One tail for every path through this function, which is what lets the chrome below it be
     * written once. The overlays used to draw the footer and return, and each of the four
     * carried its own copy of that call - so anything drawn over the whole frame (the notice
     * below is the first) had to be added in five places or be missing from four screens.
     */
    if (cache != NULL) {
        cache->split = frame.split;
    }
    if (frame.split) {
        fb_render_split(state, snapshot, &frame, &layout, back);
    } else if (body.level == MESH_UI_ROUTE_HELP) {
        fb_render_help(state, snapshot, &layout);
    } else if (body.level == MESH_UI_ROUTE_PICKER) {
        fb_render_picker(state, snapshot, &layout);
    } else if (body.level == MESH_UI_ROUTE_KEYBOARD) {
        fb_render_keyboard(state, snapshot, &layout);
    } else if (body.level == MESH_UI_ROUTE_COMPOSE) {
        fb_render_compose(state, snapshot, &layout);
    } else if (body.level == MESH_UI_ROUTE_SHARE) {
        /* Under every overlay above and over the tab's own screen, the same order nav.c takes
           the keys in: it is a level of the Settings tab raised by a row, not a question, and
           the two things above it that a radio can raise at any moment - a pairing PIN and a key
           verification - must not end up behind a code somebody is scanning. */
        fb_render_share(state, snapshot, &layout);
    } else if (body.level == MESH_UI_ROUTE_CONTACT) {
        /* Beside the share sheet and under the same overlays, for the same reason: it is a
           level of the Settings tab raised by a row, not a question. */
        fb_render_contact(state, snapshot, &layout);
    } else {
        switch (snapshot->nav.screen) {
        case MESH_UI_SCREEN_MESSAGES:
            if (!snapshot->nav.thread_open) {
                fb_render_conversations(state, snapshot, &layout);
            } else {
                fb_render_thread(state, snapshot, &layout);
            }
            break;
        case MESH_UI_SCREEN_NODES:
            /* A place or the list of them first, over the roster or the map; then the map, under
               any node detail opened from it and over the list it was opened from - the same
               order src/ui/tables/actions.c names the presses in. */
            if (mesh_ui_nav_waypoints_showing(&snapshot->nav)) {
                fb_render_waypoints(state, snapshot, &layout);
            } else if (snapshot->nav.map_open && !snapshot->nav.node_detail_open) {
                fb_render_map(state, snapshot, &layout);
            } else {
                fb_render_nodes(state, snapshot, &layout);
            }
            break;
        case MESH_UI_SCREEN_SETTINGS:
            fb_render_settings(state, snapshot, &layout);
            break;
        case MESH_UI_SCREEN_RADIO:
        default:
            /* The device list, the chart or a page over the cards, the way the map is drawn over
               the node list - and the screen is tested as well as the flag for the same reason: the
               flags say where the Radio tab is standing, not what is on the panel. */
            if (snapshot->nav.devices_open) {
                fb_render_devices(state, snapshot, &layout);
            } else if (snapshot->nav.trend_open) {
                fb_render_trend(state, snapshot, &layout);
            } else if (snapshot->nav.radio_page != MESH_UI_RADIO_PAGE_NONE) {
                fb_render_radio_page(state, snapshot, &layout);
            } else {
                fb_render_status(state, snapshot, &layout);
            }
            break;
        }
    }
    inkcell_fb_shift_end(state);

    /*
     * The layers, over whichever of the above raised them.
     *
     * Outside the chain rather than branches of it, and after the shift rather than inside it:
     * what is drawn over a screen is not a place, so the screen it is about stays on the panel,
     * dimmed behind a scrim, rather than being replaced by the asking. That is what a layer
     * buys - and it is why all three are called on every frame instead of chosen between, since
     * a layer that is leaving is no longer anything the snapshot says.
     *
     * The order is the stack, because drawn last is on top, and it is the order nav.c takes the
     * keys in: the faces a message can be answered with are the shallowest of the three, and
     * the verification sheet - which a radio can raise at any moment, over anything - is above
     * the settings confirm that will wait.
     *
     * The node's sheet of verbs is not here. It belongs to one tab rather than to the frame,
     * so fb_render_nodes() draws it over its own detail, where the order it stacks in is a
     * fact about that tab.
     *
     * **None of the three is drawn over help**, which is the one thing about this order that is
     * not a matter of taste. mesh_ui_nav_handle_key() gives help every press before it reaches
     * any of these, so a layer drawn over an open help screen would be a panel the reader can
     * see and cannot answer - and a verification sheet is the one that would be caught by it,
     * since a radio raises that at a moment of its own choosing. What is drawn and what the
     * press reaches have to be the same thing; each of the three reads `help_open` and treats
     * it as the app no longer wanting it up, so one already on the panel walks out rather than
     * being cut off mid-frame.
     */
    fb_render_reactions(state, snapshot, &layout);
    fb_render_confirm(state, snapshot, &layout);
    fb_render_verify(state, snapshot, &layout);

    /*
     * The focus ring, over whichever of the above drew the cursor.
     *
     * The screens do not say where it goes: the row, answer or key that drew itself focused
     * marked its own box in the map (inkcell_focus_mark()), and the last one marked is the one
     * on top - a sheet's row over the list under it, a dialog's answer over both. A frame that
     * marked nothing (the map, a help page) has no ring, and the next one to mark adopts rather
     * than flying in from wherever the ring was last seen.
     *
     * The ring is what finds the cursor now, which is what lets a focused row be a quiet lift
     * rather than a solid bar - see inkcell_fb_focus_fill().
     *
     * Not while a screen is sliding in. The rows are moving because the *place* changed, not
     * because the cursor did, and a ring that chased them across the panel would be reporting a
     * press that never happened. It is cleared for the move and adopts the row where the screen
     * lands.
     */
    if (cache != NULL) {
        inkcell_fb_draw_focus_ring(state, &cache->focus,
                                   slide != 0 ? INKCELL_FOCUS_NONE
                                              : inkcell_focus_marked(&cache->focus));
    }

    struct inkcell_line summary;
    enum inkcell_tone summary_tone = INKCELL_TONE_DIM;
    fb_link_summary(snapshot, &summary, &summary_tone);
    const struct inkcell_fb_action_bar bar = {
        .items = actions.items,
        .count = actions.count,
        .status = inkcell_line_text(&summary),
        .status_tone = summary_tone,
        /* A leads when it is the screen's own press, and is drawn as the one that matters. Not
           when some other key leads - X on the device list is a disconnect, and a tonal pill
           round the verb that drops the link would be recommending it. */
        .emphasize_first = actions.count > 0U && actions.items[0].button == INKCELL_BUTTON_A,
    };
    inkcell_fb_scaffold_end(state, &frame, footless ? NULL : &bar);
    fb_render_context(state, snapshot);

    /*
     * Last, because it is over the UI rather than in it: a notice that a screen could paint
     * over is a notice that is only visible on the screens that happen not to reach the bottom
     * of the body.
     */
    const struct inkcell_fb_snackbar snackbar = {
        .text = snapshot->nav.toast,
        .until_ms = snapshot->nav.toast_until_ms,
    };
    inkcell_fb_draw_snackbar(state, &layout, &snackbar);
}
