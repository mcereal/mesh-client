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
    case MESH_UI_SCREEN_WAYPOINTS:
        /* `place` - the same pin the Position settings section wears, and deliberately the same
           id: icons.def's rule is one id per job, and both are saying "somewhere on Earth". A
           second sprite drawing the same rune would be a second answer to one question. */
        return INKCELL_ICON_POSITION;
    case MESH_UI_SCREEN_DEVICES:
        return INKCELL_ICON_DEVICES;
    case MESH_UI_SCREEN_STATUS:
        return INKCELL_ICON_STATUS;
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
 * The line under the keycaps: what the transport is doing, and either the radio it found or
 * how to leave.
 *
 * `tone` is the second half of the same sentence - a link that is up is worth saying in the
 * success colour, and one that is not is not worth shouting about - so the two are decided
 * together here rather than by the widget, which has no idea what the words mean.
 */
static void fb_link_summary(const struct mesh_ui_snapshot *snapshot, struct inkcell_line *line,
                            enum inkcell_tone *tone) {
    inkcell_line_reset(line);
    const char *status = snapshot->transport_status[0] != '\0'
                             ? snapshot->transport_status
                             : inkcell_str(MESH_STR_HEADER_TRANSPORT_STARTING);
    const struct mesh_ui_device *device = mesh_ui_snapshot_connected_device(snapshot);
    if (device != NULL) {
        inkcell_line_str(line, MESH_STR_HEADER_STATUS_CONNECTED, status, fb_device_label(device));
        *tone = INKCELL_TONE_SUCCESS;
        return;
    }
    inkcell_line_str(line, MESH_STR_HEADER_STATUS_QUIT, status, inkcell_input_quit_hint());
    *tone = INKCELL_TONE_DIM;
}

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
};

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

/*
 * A right-click menu: the row's own verbs, at the pointer.
 *
 * The verbs are the action bar's - `actions`, the one table's answer for the row the cursor is
 * on - so the menu cannot offer what a press would not do. Only the face buttons that act on
 * something: B is a way back, not a verb about the row, and help is about the screen. Each item
 * sits at its button's index, which is what numbers it (MESH_UI_FOCUS_MENU + button) and so
 * what the nav presses when it is clicked.
 *
 * Drawn over everything, the action bar included, with one target under the whole panel first,
 * so a click that misses the menu lands on that and puts it down.
 */
static void fb_render_context(struct inkcell_draw_state *state,
                              const struct mesh_ui_snapshot *snapshot,
                              const struct inkcell_action_bar *actions) {
    struct inkcell_fb_menu_item items[INKCELL_BUTTON_COUNT];
    memset(items, 0, sizeof items);
    size_t offered = 0U;
    for (size_t i = 0; i < actions->count; ++i) {
        const enum inkcell_button button = actions->items[i].button;
        if (button == INKCELL_BUTTON_A || button == INKCELL_BUTTON_X ||
            button == INKCELL_BUTTON_Y || button == INKCELL_BUTTON_START) {
            items[button].label = inkcell_str(actions->items[i].label);
            offered += 1U;
        }
    }
    const struct inkcell_fb_menu menu = {
        .items = items,
        .count = INKCELL_BUTTON_COUNT,
        .cursor = UINT32_MAX,
        .focus_base = (uint32_t)MESH_UI_FOCUS_MENU,
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
     * closed.
     */
    const struct inkcell_fb_rect body = {
        .x = 0,
        .y = layout->nav_y,
        .w = inkcell_fb_panel_width(state),
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
        const bool moving = inkcell_fb_transition_offset(state) != 0;
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

    struct inkcell_fb_layout layout;
    memset(&layout, 0, sizeof layout);
    layout.small = inkcell_theme_type_scale(state->theme, INKCELL_TYPE_LABEL, state->scale);
    layout.line = inkcell_fb_line_adv(state, state->scale);
    layout.cols = inkcell_fb_cols(state, state->scale);
    /* The same room as `cols`, in the unit anything laying out real text measures in. */
    layout.body_w = inkcell_fb_panel_width(state) - 2 * inkcell_fb_margin(state);

    inkcell_fb_draw_nav_bar(state, &layout, fb_tab_chips(snapshot), MESH_UI_SCREEN_COUNT,
                            (size_t)snapshot->nav.screen);

    /*
     * The two things the *client* says about itself, rather than what any screen says about
     * itself. Both are drawn here, once, around whichever screen is up - and both take their
     * content from src/ui/tables/chrome.c for the reason the action bar takes its verbs from
     * src/ui/tables/actions.c: which states are worth a notice is a fact about the client, not
     * about a framebuffer, and it is a unit test's business rather than a screenshot's.
     *
     * The bar goes first and costs nothing: it hangs in the gap the navigation bar already
     * leaves, so `layout` is unchanged by it and a request going out never reflows a list.
     */
    inkcell_fb_draw_progress(state, &layout, mesh_ui_chrome_busy(snapshot));

    layout.footer_y = inkcell_fb_panel_height(state) - inkcell_fb_action_bar_height(state, &layout);
    const int body_height = layout.footer_y - layout.body_y - inkcell_fb_gutter(state);
    layout.rows = body_height > 0 ? (uint32_t)(body_height / layout.line) : 0U;

    /*
     * The banner does cost rows, so it is drawn after the body has been measured and hands back
     * what is left - exactly as the top app bar under it does. Nothing below this line knows it
     * happened, which is the whole point: a screen renderer lays out against `layout`.
     */
    struct mesh_ui_banner banner;
    if (mesh_ui_chrome_banner(snapshot, &banner)) {
        const struct inkcell_fb_banner drawn = {
            .icon = banner.icon,
            .text = inkcell_str(banner.text),
            .supporting =
                banner.supporting != INKCELL_STR_NONE ? inkcell_str(banner.supporting) : NULL,
            .detail = banner.detail,
            .family = banner.family,
        };
        inkcell_fb_draw_banner(state, &layout, &drawn);
    }

    /*
     * Which buttons mean something here is no longer decided in this function.
     *
     * It used to be a branch per screen alongside the one below - the same conditions written
     * out twice, once to pick a renderer and once to pick a hint sentence, which is two places
     * to remember when a screen grows a press. mesh_ui_actions_for() answers from the snapshot
     * instead, walking the same chain of overlays, and it is a unit test's business rather than
     * a screenshot's.
     *
     * It is asked *before* the screen draws because both ends of the frame read it now: the
     * action bar at the bottom says what B does, and the top app bar's leading slot draws an
     * arrow when what B does is leave. Two answers from one table is the whole point - a screen
     * deciding its own back arrow would be free to disagree with the keycap under it.
     */
    struct inkcell_action_bar actions;
    mesh_ui_actions_for(snapshot, &actions);
    layout.back = mesh_ui_action_bar_goes_back(&actions);
    if (state->pointer) {
        mesh_ui_actions_drop_tabs(&actions);
    }

    /*
     * And whether this frame is part of a move between two places, which is the one thing about
     * it that is not a function of the snapshot - see inkcell_fb_transition_offset().
     *
     * The band is everything below the navigation bar and above the action bar, because those
     * two are the same on both sides of any move: the strip still names the tab it named, the
     * keycaps still say what the buttons do. What is inside the band and drawn *before* the
     * transform - the screen progress bar and the banner - stays put for the same reason, being
     * about the client rather than about the screen that is arriving.
     *
     * It is also declared as animation damage, so the rows it moves through are presented.
     * Everything in there is a function of the clock while a move is running, and the
     * partial-redraw path assumes the opposite of anything it has not been told about - which
     * is why fb_render_begin() does not enter the band at all while one is.
     */
    const int slide = inkcell_fb_transition_offset(state);
    if (slide != 0) {
        inkcell_fb_animation_damage(state, 0, layout.nav_y, inkcell_fb_panel_width(state),
                                    layout.footer_y - layout.nav_y);
        inkcell_fb_shift_begin(state, slide, layout.nav_y, layout.footer_y);
    }

    /*
     * One tail for every path through this function, which is what lets the chrome below it be
     * written once. The overlays used to draw the footer and return, and each of the four
     * carried its own copy of that call - so anything drawn over the whole frame (the notice
     * below is the first) had to be added in five places or be missing from four screens.
     */
    struct mesh_ui_route body;
    mesh_ui_route_under_layers(&snapshot->nav, &body);
    if (body.level == MESH_UI_ROUTE_HELP) {
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
            /* The map, under any node detail opened from it and over the list it was opened
               from - the same order src/ui/tables/actions.c names the presses in. */
            if (snapshot->nav.map_open && !snapshot->nav.node_detail_open) {
                fb_render_map(state, snapshot, &layout);
            } else {
                fb_render_nodes(state, snapshot, &layout);
            }
            break;
        case MESH_UI_SCREEN_WAYPOINTS:
            fb_render_waypoints(state, snapshot, &layout);
            break;
        case MESH_UI_SCREEN_DEVICES:
            fb_render_devices(state, snapshot, &layout);
            break;
        case MESH_UI_SCREEN_SETTINGS:
            fb_render_settings(state, snapshot, &layout);
            break;
        case MESH_UI_SCREEN_STATUS:
        default:
            /* The chart over the cards, the way the map is drawn over the node list - and the
               screen is tested as well as the flag for the same reason: `trend_open` says where
               the Status tab is standing, not what is on the panel. */
            if (snapshot->nav.trend_open) {
                fb_render_trend(state, snapshot, &layout);
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

    struct inkcell_line summary;
    enum inkcell_tone summary_tone = INKCELL_TONE_DIM;
    fb_link_summary(snapshot, &summary, &summary_tone);
    const struct inkcell_fb_action_bar bar = {
        .items = actions.items,
        .count = actions.count,
        .status = inkcell_line_text(&summary),
        .status_tone = summary_tone,
    };
    inkcell_fb_draw_action_bar(state, &layout, &bar);
    fb_render_context(state, snapshot, &actions);

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
