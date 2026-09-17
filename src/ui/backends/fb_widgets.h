#ifndef MESH_UI_BACKENDS_FB_WIDGETS_H
#define MESH_UI_BACKENDS_FB_WIDGETS_H

/*
 * The components the screens are assembled from.
 *
 * The layering under src/ui/backends/ is:
 *
 *   fb_draw.c     pixels, glyphs, the theme lookups, page geometry   "how to put ink down"
 *   fb_widgets_*  buttons, chips, list rows, field rows, rules       "what things look like"
 *   fb_screens.c  one renderer per screen                            "what is on this screen"
 *
 * A screen renderer should read as a description of its content: what the list holds, what
 * each row says, which rows are actions. If it is computing a pixel coordinate, a scroll
 * offset or a padding width, that belongs down here instead - those are the three things every
 * screen used to re-derive, and the three things that were subtly wrong in a different way on
 * each of them.
 *
 * This header is the umbrella: one file per group of components, and this pulls all nine in.
 * A file that wants one group can include that group's header instead, the way the UI store's
 * subjects are included - which of them a component is filed under is not part of its
 * interface, and a `struct fb_meter` is `struct fb_meter` from either door.
 *
 *   fb_widgets_button.h    the button, the chip, a strip of chips, the badge
 *   fb_widgets_chrome.h    the app bar, the navigation and action bars, the banner, the rule
 *   fb_widgets_list.h      the list window, its cards and rail, the subheader and the note
 *   fb_widgets_item.h      one list row and its slots, and the conversation cell
 *   fb_widgets_bubble.h    the transcript: a message, and the separator between two of them
 *   fb_widgets_card.h      a card, built row by row and then drawn
 *   fb_widgets_control.h   the switch, the checkbox and radio, the segmented button, the field
 *   fb_widgets_meter.h     a quantity as a length, and a reading over time
 *   fb_widgets_overlay.h   the dialog, the snackbar, the QR code
 *
 * Tones - what a thing *is*, rather than which colour to draw it - live in
 * include/mesh/ui/theme.h as `enum mesh_ui_tone`, because they are the UI's vocabulary rather
 * than this backend's. A screen names one, the theme answers, and fb_tone_color() on the state
 * is the only place the two meet.
 *
 * A widget takes a tone, never a colour, for the same reason a stylesheet has a token called
 * "danger" instead of the hex for red: it is what lets a theme change the answer.
 *
 * `struct fb_rect` is not here: a box in pixels is the drawing layer's vocabulary rather than
 * any one component's, so it sits in fb_internal.h with the rest of the geometry.
 *
 * Not public API. include/mesh/ui/backends/fb.h is; nothing outside src/ui/backends/ should
 * include this.
 */

#include "fb_widgets_bubble.h"
#include "fb_widgets_button.h"
#include "fb_widgets_card.h"
#include "fb_widgets_chrome.h"
#include "fb_widgets_control.h"
#include "fb_widgets_item.h"
#include "fb_widgets_list.h"
#include "fb_widgets_meter.h"
#include "fb_widgets_overlay.h"

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_H */
