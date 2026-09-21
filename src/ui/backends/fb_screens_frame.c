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

#include "fb_widgets.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/actions.h"
#include "mesh/ui/chrome.h"
#include "mesh/ui/input.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"
#include "mesh/utils/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The icon a screen is known by: on its tab, and again on the empty state that stands in for
 * its list. One answer in one place, because a tab and its empty screen showing two different
 * symbols for the same thing is exactly the drift a table like this prevents.
 */
static enum mesh_ui_icon fb_screen_icon(enum mesh_ui_screen screen) {
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return MESH_UI_ICON_MESSAGES;
    case MESH_UI_SCREEN_NODES:
        return MESH_UI_ICON_NODES;
    case MESH_UI_SCREEN_WAYPOINTS:
        /* `place` - the same pin the Position settings section wears, and deliberately the same
           id: icons.def's rule is one id per job, and both are saying "somewhere on Earth". A
           second sprite drawing the same rune would be a second answer to one question. */
        return MESH_UI_ICON_POSITION;
    case MESH_UI_SCREEN_DEVICES:
        return MESH_UI_ICON_DEVICES;
    case MESH_UI_SCREEN_STATUS:
        return MESH_UI_ICON_STATUS;
    case MESH_UI_SCREEN_SETTINGS:
        return MESH_UI_ICON_SETTINGS;
    default:
        return MESH_UI_ICON_NONE;
    }
}

/*
 * The tab strip and the two lines under the body were both written out here, and both were the
 * last screen-level renderers laying out their own pixels. They are components now
 * (fb_draw_nav_bar, fb_draw_action_bar), so what is left in this file is the *content*: which
 * tabs there are, which one is up, and what the bottom line has to say about the radio.
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
static const struct fb_chip *fb_tab_chips(const struct mesh_ui_snapshot *snapshot) {
    static struct fb_chip chips[MESH_UI_SCREEN_COUNT];
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
            snprintf(unread, sizeof unread, "%s", mesh_str(MESH_STR_MESSAGES_UNREAD_OVERFLOW));
        } else {
            snprintf(unread, sizeof unread, "%u", (unsigned)total);
        }
    }

    for (int i = 0; i < MESH_UI_SCREEN_COUNT; ++i) {
        const enum mesh_ui_screen screen = (enum mesh_ui_screen)i;
        chips[i].icon = fb_screen_icon(screen);
        chips[i].label = mesh_ui_screen_name(screen);
        chips[i].badge = (screen == MESH_UI_SCREEN_MESSAGES) ? unread : "";
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
static void fb_link_summary(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_line *line,
                            enum mesh_ui_tone *tone) {
    mesh_ui_line_reset(line);
    const char *status = snapshot->transport_status[0] != '\0'
                             ? snapshot->transport_status
                             : mesh_str(MESH_STR_HEADER_TRANSPORT_STARTING);
    const struct mesh_ui_device *device = mesh_ui_snapshot_connected_device(snapshot);
    if (device != NULL) {
        mesh_ui_line_str(line, MESH_STR_HEADER_STATUS_CONNECTED, status, fb_device_label(device));
        *tone = MESH_UI_TONE_SUCCESS;
        return;
    }
    mesh_ui_line_str(line, MESH_STR_HEADER_STATUS_QUIT, status, mesh_ui_input_quit_hint());
    *tone = MESH_UI_TONE_DIM;
}

/* The same snapshot and geometry can only move inside the bounds declared by animated
   widgets. Re-run composition through a clip so overlapping chrome is restored in draw order. */
struct fb_render_cache {
    struct mesh_ui_snapshot snapshot;
    const struct mesh_ui_theme *theme;
    const struct mesh_i18n_locale *locale;
    int scale;
    uint32_t width, height;
    time_t second;
    bool valid;
    /* And what the layers were last told to say, which is a memo of a different kind: the one
       above is this frame compared with the last, and these outlive the answer that closed
       them. One per enum fb_overlay_id, indexed by it. See struct fb_dialog_memo. */
    struct fb_dialog_memo dialogs[FB_OVERLAY_VERIFY + 1];
};

struct fb_dialog_memo *fb_dialog_memo(struct mesh_ui_backend_fb_state *state,
                                      enum fb_overlay_id id) {
    struct fb_render_cache *const cache = state != NULL ? state->render_cache : NULL;
    if (cache == NULL || (unsigned)id >= (sizeof cache->dialogs / sizeof cache->dialogs[0])) {
        return NULL;
    }
    return &cache->dialogs[id];
}

void fb_render_cache_free(struct mesh_ui_backend_fb_state *state) {
    free(state->render_cache);
    state->render_cache = NULL;
}

static void fb_render_begin(struct mesh_ui_backend_fb_state *state,
                            const struct mesh_ui_snapshot *snapshot) {
    state->clip_active = false;
    /* Allocated whether or not partial redraw is on: what hangs off it is no longer only the
       comparison below. A capture renders with partial redraw disabled, and a dialog that could
       not remember its own words there would be a dialog that never leaves in the one tool that
       looks at it - see struct fb_dialog_memo. */
    if (state->render_cache == NULL) {
        state->render_cache = calloc(1U, sizeof *state->render_cache);
    }
    struct fb_render_cache *cache = state->render_cache;
    if (!state->partial_disabled && cache != NULL) {
        const time_t second = (time_t)mesh_time_wall_s();
        cache->snapshot.update_flags = snapshot->update_flags;
        state->clip_active = cache->valid && state->animation_damage.valid &&
                             cache->theme == state->theme && cache->locale == mesh_i18n_locale() &&
                             cache->scale == state->scale && cache->width == state->var.xres &&
                             cache->height == state->var.yres && cache->second == second &&
                             memcmp(&cache->snapshot, snapshot, sizeof *snapshot) == 0;
        state->clip = state->animation_damage;
        cache->snapshot = *snapshot;
        cache->theme = state->theme;
        cache->locale = mesh_i18n_locale();
        cache->scale = state->scale;
        cache->width = state->var.xres;
        cache->height = state->var.yres;
        cache->second = second;
        cache->valid = true;
    }
    state->animation_damage.valid = false;
}

void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot) {
    /* The theme and the move are settled before this call, and what the *last* frame wanted of
       the basemap has already been forgotten: all three are inkcell calling up into fb_app.c,
       which is where the facts a snapshot does not carry are pushed down. */
    fb_render_begin(state, snapshot);

    fb_clear(state, fb_color(state, MESH_UI_COLOR_BG));

    struct fb_layout layout;
    memset(&layout, 0, sizeof layout);
    layout.small = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
    layout.line = fb_line_adv(state, state->scale);
    layout.cols = fb_cols(state, state->scale);
    /* The same room as `cols`, in the unit anything laying out real text measures in. */
    layout.body_w = (int)state->var.xres - 2 * fb_margin(state);

    fb_draw_nav_bar(state, &layout, fb_tab_chips(snapshot), MESH_UI_SCREEN_COUNT,
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
    fb_draw_progress(state, &layout, mesh_ui_chrome_busy(snapshot));

    layout.footer_y = (int)state->var.yres - fb_action_bar_height(state, &layout);
    const int body_height = layout.footer_y - layout.body_y - fb_gutter(state);
    layout.rows = body_height > 0 ? (uint32_t)(body_height / layout.line) : 0U;

    /*
     * The banner does cost rows, so it is drawn after the body has been measured and hands back
     * what is left - exactly as the top app bar under it does. Nothing below this line knows it
     * happened, which is the whole point: a screen renderer lays out against `layout`.
     */
    struct mesh_ui_banner banner;
    if (mesh_ui_chrome_banner(snapshot, &banner)) {
        const struct fb_banner drawn = {
            .icon = banner.icon,
            .text = mesh_str(banner.text),
            .supporting = banner.supporting != MESH_STR_NONE ? mesh_str(banner.supporting) : NULL,
            .detail = banner.detail,
            .family = banner.family,
        };
        fb_draw_banner(state, &layout, &drawn);
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
    struct mesh_ui_action_bar actions;
    mesh_ui_actions_for(snapshot, &actions);
    layout.back = mesh_ui_action_bar_goes_back(&actions);

    /*
     * And whether this frame is part of a move between two places, which is the one thing about
     * it that is not a function of the snapshot - see fb_transition_offset().
     *
     * The band is everything below the navigation bar and above the action bar, because those
     * two are the same on both sides of any move: the strip still names the tab it named, the
     * keycaps still say what the buttons do. What is inside the band and drawn *before* the
     * transform - the screen progress bar and the banner - stays put for the same reason, being
     * about the client rather than about the screen that is arriving.
     *
     * It is also declared as animation damage, so the frame after this one repaints the whole
     * band. Everything in there is a function of the clock while a move is running, and the
     * partial-redraw path assumes the opposite of anything it has not been told about.
     */
    const int slide = fb_transition_offset(state);
    if (slide != 0) {
        fb_animation_damage(state, 0, layout.nav_y, (int)state->var.xres,
                            layout.footer_y - layout.nav_y);
        fb_shift_begin(state, slide, layout.nav_y, layout.footer_y);
    }

    /*
     * One tail for every path through this function, which is what lets the chrome below it be
     * written once. The overlays used to draw the footer and return, and each of the four
     * carried its own copy of that call - so anything drawn over the whole frame (the notice
     * below is the first) had to be added in five places or be missing from four screens.
     */
    if (snapshot->nav.help_open) {
        fb_render_help(state, snapshot, &layout);
    } else if (snapshot->nav.picker_open) {
        fb_render_picker(state, snapshot, &layout);
    } else if (snapshot->nav.keyboard_open) {
        fb_render_keyboard(state, snapshot, &layout);
    } else if (snapshot->nav.compose_open) {
        fb_render_compose(state, snapshot, &layout);
    } else if (snapshot->nav.reaction_open) {
        fb_render_reactions(state, snapshot, &layout);
    } else if (snapshot->nav.share_open) {
        /* Under every overlay above and over the tab's own screen, the same order nav.c takes
           the keys in: it is a level of the Settings tab raised by a row, not a question, and
           the two things above it that a radio can raise at any moment - a pairing PIN and a key
           verification - must not end up behind a code somebody is scanning. */
        fb_render_share(state, snapshot, &layout);
    } else if (snapshot->nav.contact_open) {
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
    fb_shift_end(state);

    /*
     * The two questions, over whichever of the above raised them.
     *
     * Outside the chain rather than a branch of it, and after the shift rather than inside it:
     * a question is not a place, so the screen it is about stays on the panel, dimmed behind a
     * scrim, rather than being replaced by the asking. That is what a layer buys - and it is
     * why both are called on every frame instead of chosen between, since a layer that is
     * leaving is no longer anything the snapshot says.
     *
     * The confirm first so that the verification sheet is over it, which is the order nav.c
     * takes the keys in and the order a stack of layers reads: drawn last is on top. They
     * cannot both be up in practice - the app closes the sheet whenever the exchange ends -
     * and this says which wins if they ever are.
     */
    fb_render_confirm(state, snapshot, &layout);
    fb_render_verify(state, snapshot, &layout);

    struct mesh_ui_line summary;
    enum mesh_ui_tone summary_tone = MESH_UI_TONE_DIM;
    fb_link_summary(snapshot, &summary, &summary_tone);
    const struct fb_action_bar bar = {
        .items = actions.items,
        .count = actions.count,
        .status = mesh_ui_line_text(&summary),
        .status_tone = summary_tone,
    };
    fb_draw_action_bar(state, &layout, &bar);

    /*
     * Last, because it is over the UI rather than in it: a notice that a screen could paint
     * over is a notice that is only visible on the screens that happen not to reach the bottom
     * of the body.
     */
    const struct fb_snackbar snackbar = {
        .text = snapshot->nav.toast,
        .until_ms = snapshot->nav.toast_until_ms,
    };
    fb_draw_snackbar(state, &layout, &snackbar);
}
