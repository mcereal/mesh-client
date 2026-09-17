#ifndef MESH_UI_BACKENDS_FB_WIDGETS_CHROME_H
#define MESH_UI_BACKENDS_FB_WIDGETS_CHROME_H

/*
 * The frame around a screen's content: the bar over it, the two bars under it, the notices that
 * drop in between, and the hairline that separates any of them from the body.
 *
 * None of it is the screen. A screen names a title, a trail, a set of tabs and a notice, and
 * what each costs in pixels is decided here - which is why the empty state and the rule live
 * with the app bar rather than with the list they sit above.
 */

/*
 * Not public API. include/mesh/ui/backends/fb.h is; fb_widgets.h is the umbrella over this file
 * and its siblings, and nothing outside src/ui/backends/ should include either.
 */

#include "fb_internal.h"
#include "fb_widgets_button.h"

#include "mesh/ui/actions.h"
#include "mesh/ui/icon.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the navigation bar ---------------------------------------------------------------------
 *
 * The chrome across the top: a recessed bar, one chip per tab, and the rule that closes it off.
 *
 * The bar is the point. The strip used to float on the body's own ground, which left the tabs
 * reading as the first row of content rather than as the frame around it; a recessed tier
 * behind them says "this is chrome" before a word of it is read, which is what every phone's
 * navigation bar is doing. It is the theme's lowest surface, so a palette decides how far from
 * the ground that is - on the high-contrast theme it is barely anywhere, which is correct.
 *
 * Consumes the room it occupies: `layout->body_y` comes back pointing at the first body row.
 */
void fb_draw_nav_bar(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                     const struct fb_chip *tabs, size_t count, size_t active);

/* ---- the screen progress bar -----------------------------------------------------------------
 *
 * A hairline across the panel, under the navigation bar's rule: the client is waiting on
 * something it has already asked for.
 *
 * Not a new drawing - it is fb_draw_meter() at FB_METER_INDETERMINATE, full bleed and one
 * hairline tall - and that is deliberate rather than lazy. There is exactly one "a thing is
 * working" motion in this UI, and a second implementation of a travelling pill would be a
 * second one to keep in step with the theme's timings.
 *
 * What it is for: open Settings before the radio has answered and eight sections say "not
 * loaded", which reads identically whether a request is on its way back or nothing was ever
 * sent. This is the difference, and because it is chrome it answers for every screen at once
 * rather than for the one that happened to be waiting.
 *
 * **It never moves the body.** The navigation bar already leaves a gap between its rule and the
 * first body row, and the bar hangs in that gap - so `layout` is const here, the rows a list
 * gets are the same rows whether or not anything is in flight, and a save going out does not
 * reflow the screen it was saved from. It is the same rule the card's focus ring is drawn by:
 * an indicator that changes the layout is an indicator that moves what it is pointing at.
 *
 * Which states count is not this file's business - see mesh_ui_chrome_busy() in
 * include/mesh/ui/chrome.h, which is where the UI layer answers it for every backend.
 */
void fb_draw_progress(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      bool busy);

/* ---- the banner ------------------------------------------------------------------------------
 *
 * The persistent notice: something is true of the whole client and stays true until it is
 * resolved.
 *
 * The snackbar is the transient half of this and is correctly transient - it is for what just
 * happened. What it cannot say is what is *still the case*: a release waiting to be installed
 * was visible only inside Settings > About, so the one screen that already knew was the only
 * screen that said so. A banner is the other half: it costs body rows, it does not go away on a
 * timer, and it sits in the chrome under the tab strip where a statement about the client
 * belongs.
 *
 * Why it is above the screen's own app bar rather than below it, which is where Material puts
 * one. The navigation bar is this client's app-level chrome and the top app bar is the
 * *screen's* heading; a banner is a statement about the client, so it goes with the first. The
 * practical half of the same answer: drawn below the app bar it would have to be called by
 * every screen renderer, and the four overlays would each need their own copy - which is the
 * duplication fb_render_snapshot()'s single tail exists to prevent.
 *
 * Nothing here animates, and that is a decision rather than an omission. The container consumes
 * body rows, so a height that eased open would reflow the list underneath it for the length of
 * the animation - and unlike the snackbar, which arrives over the UI and has to be *noticed*,
 * a banner is read whenever the eye next reaches the top of the panel.
 *
 * Which banner, if any, is mesh_ui_chrome_banner()'s answer (include/mesh/ui/chrome.h).
 */
struct fb_banner {
    /* The leading symbol, in the container's own ink. */
    enum mesh_ui_icon icon;
    /* The headline: what is true. NULL or empty draws nothing at all, which is what makes "no
       banner" a struct rather than a branch at the call site. */
    const char *text;
    /* What to do about it, on a second line at the label scale. NULL for none - and dropped
       before the headline is when the body cannot spare the row for it. */
    const char *supporting;
    /* A fact stated in its own units against the trailing edge of the headline: a version
       number. Untranslated by design, so it is a string rather than an id. NULL for none. */
    const char *detail;
    /* The container's fill and the ink on it, taken together from one theme call. A family
       rather than a tone for the reason a badge takes one: this thing fills something. */
    enum mesh_ui_family family;
};

/*
 * Draws it at the top of the body and consumes the rows it took, so a screen renderer that
 * follows lays out against a shorter body without knowing this happened.
 *
 * Recomputes `rows` from the body's real bottom rather than deducting a row count, for the
 * reason fb_draw_app_bar() does - see the comment on its tail.
 */
void fb_draw_banner(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                    const struct fb_banner *banner);

/* ---- the action bar -------------------------------------------------------------------------
 *
 * The chrome across the bottom: what the buttons do here, as keycaps, over the line that says
 * what the radio is doing.
 *
 * This was two lines of plain text, and it was the last screen-level renderer laying out its
 * own pixels - which is also why it was the piece of chrome that most made the UI read as a
 * terminal rather than as a handheld OS. A hint sentence is a row of controls written down as
 * words: the letters in it are things on the case, and the verbs after them are what those
 * things do. Drawing it as keycaps says both without the eye having to parse a sentence to
 * find the one letter it was looking for.
 *
 * The keycap is FB_BUTTON_FILLED at MESH_UI_SHAPE_SM, which is the component set's existing
 * answer for "a place to press" - the on-screen keyboard's keys are the same button - so a
 * keycap here and a key there cannot drift apart.
 *
 * **What it draws is `struct mesh_ui_button_action`, never a sentence.** The bar has to iterate the
 * pairs, so the pairs have to exist before the drawing does; that is why the hint catalog
 * entries were retired in favour of the table in src/ui/tables/actions.c. See
 * include/mesh/ui/actions.h.
 *
 * It owns the whole bottom bar - the surface, the rule above it, the keycaps and the status
 * line - for the same reason the card owns its own inset: a screen that placed the status line
 * itself would be back to computing a y coordinate in a screen renderer.
 */

struct fb_action_bar {
    const struct mesh_ui_button_action *items;
    size_t count;
    /*
     * The line under the keycaps: the transport state, and either the radio it is attached to
     * or how to quit.
     *
     * It used to share a row with the transient notice, which took it whenever there was one -
     * so every action blanked the answer to "is there a radio attached?" for four seconds. The
     * notice has somewhere of its own now (fb_draw_snackbar), and this row says one thing,
     * always. Clipped to the panel rather than wrapped: the bar is a fixed height.
     */
    const char *status;
    enum mesh_ui_tone status_tone;
};

/* The room the bar wants at the foot of the panel - what a caller subtracts from the panel
   height to find where the body ends. */
int fb_action_bar_height(const struct mesh_ui_backend_fb_state *state,
                         const struct fb_layout *layout);

/*
 * Draws it, with its top edge at `layout->footer_y`.
 *
 * Actions that do not fit are dropped from the *end*, which is why struct mesh_ui_action_bar
 * is documented as being in priority order: on a narrow panel or in a long translation, the
 * press the screen is for survives and "L/R tabs" - true everywhere, and therefore the least
 * worth the room - is what goes.
 */
void fb_draw_action_bar(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, const struct fb_action_bar *bar);

/* ---- the top app bar ------------------------------------------------------------------------
 *
 * The heading a screen opens with: where you are, how to get out, and one fact about the whole
 * screen. Consumes the body rows it occupies, so a screen calls this and then lays its list out
 * against the layout it hands back.
 *
 * It used to take a `const char *`, which meant a screen's heading was a *string* and
 * everything a heading had to carry got glued into it. Settings built "Settings > %s%s%s" out
 * of two catalog entries and an unsaved marker, and that is a whole-sentence string id doing
 * structural work in the same way the button hints were: the `>` separators handed a translator
 * the breadcrumb's grammar along with its words, and a badge glued into a title with %s cannot
 * be a badge. What is left in the catalog is one word per level.
 *
 * The four slots, and what each is for:
 *
 *   leading    the back affordance. Not a field - it is layout->back, from the action bar's own
 *              table, so the arrow and the B keycap cannot disagree about whether B leaves.
 *   overline   the trail of levels above this one, separated by a drawn chevron. Two levels is
 *              the deepest anything here goes ("Settings > Modules" over "Telemetry").
 *   title      what this screen is. One line, at MESH_UI_TYPE_TITLE.
 *   trailing   a badge: a fact about the screen rather than about any row of it.
 */

/* Settings is the deepest trail in the tree and it is two levels; three is one level of slack
   so that a screen growing one is a call-site change rather than a component change. */
#define FB_APP_BAR_TRAIL_MAX 3U

struct fb_app_bar {
    /* Outermost first: {"Settings", "Modules"} above a title of "Telemetry". Each entry is one
       level's own name - the separators belong to the component. */
    const char *trail[FB_APP_BAR_TRAIL_MAX];
    size_t trail_count;
    const char *title;
    /* The trailing capsule, empty for most screens. `badge_family` is what it is filled with:
       the warning family for edits the radio has not been told about, which is the one this
       exists for. */
    const char *badge;
    enum mesh_ui_family badge_family;
};

void fb_draw_app_bar(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                     const struct fb_app_bar *bar);

/*
 * How far down the body the bar pushes it, without drawing anything.
 *
 * Beside fb_action_bar_height() and for the same reason: a screen that has to know how big its
 * content area will be *before* it has something to put in the bar cannot get there by drawing
 * the bar first. The map is the one such screen - its badge counts the markers on the panel,
 * which is not answerable until the panel has been measured - and the alternative is drawing the
 * bar twice, once with a wrong number and once over the top of it.
 *
 * A trail is one extra line, which is the only thing about a bar that changes its height. What
 * the title says, and whether there is a badge, do not.
 */
int fb_app_bar_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      size_t trail_count);

/*
 * What a screen says instead of a list when it has nothing to show, under the icon of whatever
 * the list would have held.
 *
 * The icon is drawn large and dim above the words, which is the shape an empty state has
 * everywhere: the screen is blank, so the one thing on it can afford to be the size that says
 * "this is empty on purpose" rather than "this failed to load".
 */
void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   enum mesh_ui_icon icon, const char *text);

/* How tall a hairline is at `scale` - what a caller stacking something under one has to clear.
   Beside the call that draws one because two expressions for one thickness is how a bar ends up
   overlapping the rule above it. */
int fb_rule_height(const struct mesh_ui_backend_fb_state *state, int scale);

/* A hairline separator - under the tab strip, above a detail pane. The role says which of the
   theme's two rule colours it is: MESH_UI_COLOR_RULE for a separator inside the body,
   MESH_UI_COLOR_RULE_STRONG for the one that closes the chrome off. */
void fb_draw_rule(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int scale,
                  enum mesh_ui_color role);

/* "Messages (12)", or "Messages (12, +40 older)" when a ring has dropped some. */
void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_CHROME_H */
