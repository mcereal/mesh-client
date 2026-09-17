#ifndef MESH_UI_BACKENDS_FB_WIDGETS_OVERLAY_H
#define MESH_UI_BACKENDS_FB_WIDGETS_OVERLAY_H

/*
 * What is drawn over a screen rather than in it: the dialog that owns the body while it is up,
 * the snackbar that slides in over the action bar, and the QR code a screen hands a link to.
 */

/*
 * Not public API. include/mesh/ui/backends/fb.h is; fb_widgets.h is the umbrella over this file
 * and its siblings, and nothing outside src/ui/backends/ should include either.
 */

#include "fb_internal.h"

#include "mesh/ui/icon.h"
#include "mesh/ui/theme.h"
#include "mesh/utils/qr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the dialog -------------------------------------------------------------------------------
 *
 * A raised panel that asks one question and offers two answers.
 *
 * The confirmation screen was a title, four lines of wrapped text on the bare ground, and the
 * two answers as ordinary list rows under them - which is to say it looked exactly like every
 * other list in the app, at the one moment the app is asking rather than showing. A dialog is
 * the shape that difference has everywhere else: the question lifted onto its own surface, and
 * the answers as *buttons* rather than as rows.
 *
 * It fills the body rather than floating over it, and that is deliberate rather than a
 * shortcut. A dialog elsewhere dims what is behind it with a scrim, and a scrim is alpha; the
 * Brick's display engine composites fb0 against its own layer, so there is nothing to blend
 * against and no scrim to draw. What stands in for it is that nothing else is on screen -
 * fb_render_confirm() is a screen, not an overlay - so there is nothing left to dim.
 *
 * The action row is the reason the two answers move off the list. Both are one press away
 * whichever is under the cursor, so the pair reads as a choice rather than as a menu; and the
 * accept is a tonal button while the cancel shows no fill at all, which is how every dialog
 * says which answer it is proposing without the words having to.
 */

struct fb_dialog {
    /* Over the headline, drawn large in the primary - or in the error colour when
       `destructive`. MESH_UI_ICON_NONE for none, and the panel closes up the room it would
       have taken. */
    enum mesh_ui_icon icon;
    const char *headline;
    /* The supporting paragraph, wrapped across the panel. "" for a question that needs none. */
    const char *text;
    const char *accept;
    const char *cancel;
    /* 0 is accept, 1 is cancel - the same index nav.confirm_cursor carries, which every
       direction toggles. */
    uint32_t cursor;
    /* What it goes through with cannot be undone: the whole dialog switches from the primary
       family to the error one, so the icon, the headline and the accept button's fill all
       change together rather than each being decided separately. */
    bool destructive;
};

/* Draws the dialog into the body. It owns the whole of it, so there is no `y` to advance. */
void fb_draw_dialog(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    const struct fb_dialog *dialog);

/* ---- the snackbar -------------------------------------------------------------------------
 *
 * The transient notice: "Sent to BRVO", "Not connected", "Rebooting".
 *
 * It used to be the footer's second line, in the accent, sharing that row with the link
 * summary - which meant the two competed for one line and the notice won, so for four seconds
 * after every action the frame stopped saying whether there was a radio attached. Neither is
 * secondary to the other; they were only sharing a row because a row was what a notice had.
 *
 * So it is a container of its own, over the body rather than in the chrome, and it arrives by
 * sliding up from below the panel and leaves the same way. That is the whole point of the
 * shape: a notice that appears in place has to be *noticed* to be read, and on a handheld the
 * eye is usually somewhere else at the moment it appears. Movement is what gets it back.
 *
 * Everything about it that cannot come from the snapshot - where it has slid to, what it says
 * while it slides back out after the store has forgotten it - is remembered on the state, next
 * to the animation table and for the same reason. See struct mesh_ui_backend_fb_state.
 *
 * Drawn last of everything on the frame, because it is over the UI rather than in it.
 */
struct fb_snackbar {
    /* The notice; NULL or empty means there is none, which is also what makes one already on
       screen start sliding back out. */
    const char *text;
    /*
     * When the store means to take it away, which the widget uses as the notice's *identity*
     * rather than as a deadline - expiry is the nav's business and it has already done it by
     * the time the text arrives empty.
     *
     * It is here because two notices can read the same: pressing send twice with no radio
     * attached raises "Not connected" twice, and the second one has to arrive rather than sit
     * there looking like the first never left. A deadline moves every time a notice is raised,
     * so it tells them apart when the words cannot.
     */
    uint64_t until_ms;
};

/*
 * Draws the notice, advancing it towards its resting place - or off the bottom of the panel
 * when there is none left to show. Nothing is drawn once it has gone.
 *
 * Mutable state, like every animated component here: the position it is coming from and the
 * words it is still carrying both live on the state.
 */
void fb_draw_snackbar(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      const struct fb_snackbar *bar);

/* ---- the QR code ----------------------------------------------------------------------------
 *
 * A matrix of modules, drawn as squares in the one colour pair that does not follow the theme.
 *
 * It is the only widget here whose audience is not a person: what reads it is a phone camera
 * held by somebody standing next to the Brick, so every decision about it is about scanning
 * rather than about looking. Two of them are worth stating.
 *
 * **The module size is a whole number of pixels.** A code scaled to fill the room available
 * would put module boundaries between pixels, and a reader thresholding a photograph of that
 * finds edges where the code has none. So the scale is the largest integer that fits and the
 * code is centred in whatever is left over, which is why a smaller code may not fill its box.
 *
 * **The quiet zone is part of the code.** The standard asks for four modules of clear margin,
 * and a reader that cannot find it will not lock on however sharp the modules are - so the
 * margin is drawn in the code's own ground rather than left to whatever the screen behind it
 * happens to be.
 */
struct fb_qr {
    /* The matrix. NULL, or one that failed to encode, draws nothing at all. */
    const struct mesh_qr *code;
    /* The box to fit it in. The code is centred inside it and never drawn larger. */
    struct fb_rect box;
};

/* The side of the square this code would actually occupy inside `box`, quiet zone included, or
   0 when there is no room for even one pixel per module. Asked before drawing by a screen that
   has to put something underneath it. */
int fb_qr_side(const struct fb_qr *qr);

/* Draws it centred in its box. Nothing is drawn when fb_qr_side() is 0. */
void fb_draw_qr(const struct mesh_ui_backend_fb_state *state, const struct fb_qr *qr);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_OVERLAY_H */
