#ifndef MESH_UI_BACKENDS_FB_SCREENS_INTERNAL_H
#define MESH_UI_BACKENDS_FB_SCREENS_INTERNAL_H

/* The screens are drawn with this client's half of the backend - the app context, the basemap,
   the snapshot - over inkcell's toolkit, which fb_internal.h pulls in. It used to arrive by way
   of fb_widgets.h; now that the components are a library's, the path has to be said. */
#include "fb_internal.h"

/*
 * The screens, as the frame that dispatches between them sees them.
 *
 * fb_screens_frame.c draws the chrome, works out what the body may use and then calls exactly
 * one of these; every other fb_screens_*.c defines the ones for its own screen. Everything here
 * would still be `static` if the eleven were one file, which they were - it is declared only
 * because a screen is the unit worth reading alone.
 *
 * Five of the names here are not the frame calling a screen but one screen reaching another, and
 * each is declared rather than duplicated because two screens have to give one answer:
 *
 *   fb_device_label()      what a radio is called, on its row and under the keycaps
 *   fb_thread_quote()      one message in a line, in a bubble and in the reaction sheet's heading
 *   fb_air_band            the airtime thresholds, on the Status card's bar and its chart
 *   fb_render_node_trend() a node's chart, opened from its detail
 *   fb_render_node_actions() that node's verbs, over its detail
 *   fb_render_node_detail() that detail, drawn again by the chart when its reading has gone
 *
 * Every renderer takes the same two things - an immutable snapshot and the layout the chrome
 * left - and returns nothing: a screen describes its content and the components draw it.
 * `state` is mutable on all but one because an item may carry a control that animates, and
 * where such a control has got to is kept on the backend.
 *
 * Not public API, and not part of fb_widgets.h. It names no type of its own, so it is included
 * after fb_widgets.h, which is where the types below come from. Nothing outside
 * src/ui/backends/fb_screens_*.c should include it.
 */

/* ---- the Messages tab -------------------------------------------------------------------- */

/* Level one: all traffic, the channels, and whoever we have direct messages with. */
void fb_render_conversations(struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/* Level two: the open conversation, as a transcript of bubbles. */
void fb_render_thread(struct mesh_ui_backend_fb_state *state,
                      const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/*
 * One message in a line, for a screen that is *about* that message rather than showing it.
 *
 * The transcript writes it into a bubble's quote slot and the reaction sheet uses it as a
 * heading, and they have to be the same sentence: the reader picked the message out of the
 * transcript, and a sheet quoting it differently is a sheet about some other message. Writes an
 * empty string when there is nothing to quote.
 */
void fb_thread_quote(struct mesh_ui_message_view messages, uint32_t reply_id, char *out,
                     size_t out_len);

/* ---- the sheets that put a message together ---------------------------------------------- */

void fb_render_reactions(struct mesh_ui_backend_fb_state *state,
                         const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_render_compose(struct mesh_ui_backend_fb_state *state,
                       const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_render_picker(struct mesh_ui_backend_fb_state *state,
                      const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
/* The one renderer that takes the state as const: the keyboard has no animated slot on it. */
void fb_render_keyboard(const struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/* ---- the Nodes tab ----------------------------------------------------------------------- */

/* The roster, or one node's detail over it. The map is fb_render_map() in fb_internal.h. */
void fb_render_nodes(struct mesh_ui_backend_fb_state *state,
                     const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/*
 * One node's detail. Drawn by the renderer above, and again by the chart below it: a chart whose
 * reading has gone - the node stopped reporting it, the history was forgotten under us - draws
 * the detail for the one frame before mesh_ui_nav_clamp() closes it.
 */
void fb_render_node_detail(struct mesh_ui_backend_fb_state *state,
                           const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/*
 * That node's verbs, over its detail - the screen the detail used to open with. See
 * include/mesh/ui/node_detail.h on MESH_UI_NODE_ACTION_OPEN_ACTIONS for why they moved.
 */
void fb_render_node_actions(struct mesh_ui_backend_fb_state *state,
                            const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/*
 * One of that node's readings over time, drawn over its detail.
 *
 * Filed with the Status tab's chart it is the twin of rather than with the list it is opened
 * from: both drive fb_draw_chart() over a whole body from one description, and that is the file
 * they belong to. See fb_screens_chart.c.
 */
void fb_render_node_trend(struct mesh_ui_backend_fb_state *state,
                          const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/* ---- the other tabs ---------------------------------------------------------------------- */

void fb_render_waypoints(struct mesh_ui_backend_fb_state *state,
                         const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_render_devices(struct mesh_ui_backend_fb_state *state,
                       const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_render_status(struct mesh_ui_backend_fb_state *state,
                      const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_render_settings(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

/* What to call a device: the advertised name when it has one, otherwise whatever we addressed
   it by. The Devices tab's rows and the line under the keycaps name a radio the same way, which
   is not a coincidence worth leaving to two functions. */
const char *fb_device_label(const struct mesh_ui_device *device);

/*
 * Where a busy mesh stops being healthy, in permille of the air.
 *
 * Stated in fb_screens_status.c, where four things on one card read it, and ruled across the
 * chart that card opens - so the amber the reader saw on the bar is the line they watch the
 * trend cross. A threshold that is drawn has to be the threshold that is compared.
 */
extern const struct mesh_ui_band fb_air_band;

/* ---- what is drawn over a tab ------------------------------------------------------------ */

void fb_render_help(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot,
                    struct fb_layout *layout);
void fb_render_confirm(struct mesh_ui_backend_fb_state *state,
                       const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_render_verify(struct mesh_ui_backend_fb_state *state,
                      const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
/* The two code sheets: this radio's channels, and this radio's contact record. */
void fb_render_share(struct mesh_ui_backend_fb_state *state,
                     const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_render_contact(struct mesh_ui_backend_fb_state *state,
                       const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
/* The radio's airtime, over the Status cards that offered it. */
void fb_render_trend(struct mesh_ui_backend_fb_state *state,
                     const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);

#endif /* MESH_UI_BACKENDS_FB_SCREENS_INTERNAL_H */
