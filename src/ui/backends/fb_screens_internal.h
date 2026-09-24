#ifndef MESH_UI_BACKENDS_FB_SCREENS_INTERNAL_H
#define MESH_UI_BACKENDS_FB_SCREENS_INTERNAL_H

/* The screens are drawn with this client's half of the backend - the app context, the basemap,
   the snapshot - over inkcell's toolkit, which fb_internal.h pulls in. It used to arrive by way
   of inkcell/ui/widgets.h; now that the components are a library's, the path has to be said. */
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
 * Not public API, and not part of inkcell/ui/widgets.h. It names no type of its own, so it is
 * included after inkcell/ui/widgets.h, which is where the types below come from. Nothing outside
 * src/ui/backends/fb_screens_*.c should include it.
 */

/* ---- the Messages tab -------------------------------------------------------------------- */

/* Level one: all traffic, the channels, and whoever we have direct messages with. */
void fb_render_conversations(struct inkcell_draw_state *state,
                             const struct mesh_ui_snapshot *snapshot,
                             struct inkcell_fb_layout *layout);

/* Level two: the open conversation, as a transcript of bubbles. */
void fb_render_thread(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                      struct inkcell_fb_layout *layout);

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

void fb_render_reactions(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                         struct inkcell_fb_layout *layout);
void fb_render_compose(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout);
void fb_render_picker(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                      struct inkcell_fb_layout *layout);
/* The one renderer that takes the state as const: the keyboard has no animated slot on it. */
void fb_render_keyboard(const struct inkcell_draw_state *state,
                        const struct mesh_ui_snapshot *snapshot, struct inkcell_fb_layout *layout);

/* ---- the Nodes tab ----------------------------------------------------------------------- */

/* The roster, or one node's detail over it. The map is fb_render_map() in fb_internal.h. */
void fb_render_nodes(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                     struct inkcell_fb_layout *layout);

/*
 * One node's detail. Drawn by the renderer above, and again by the chart below it: a chart whose
 * reading has gone - the node stopped reporting it, the history was forgotten under us - draws
 * the detail for the one frame before mesh_ui_nav_clamp() closes it.
 */
void fb_render_node_detail(struct inkcell_draw_state *state,
                           const struct mesh_ui_snapshot *snapshot,
                           struct inkcell_fb_layout *layout);

/*
 * That node's verbs, over its detail - the screen the detail used to open with. See
 * include/mesh/ui/node_detail.h on MESH_UI_NODE_ACTION_OPEN_ACTIONS for why they moved.
 */
void fb_render_node_actions(struct inkcell_draw_state *state,
                            const struct mesh_ui_snapshot *snapshot,
                            struct inkcell_fb_layout *layout);

/*
 * One of that node's readings over time, drawn over its detail.
 *
 * Filed with the Status tab's chart it is the twin of rather than with the list it is opened
 * from: both drive inkcell_fb_draw_chart() over a whole body from one description, and that is the
 * file they belong to. See fb_screens_chart.c.
 */
void fb_render_node_trend(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                          struct inkcell_fb_layout *layout);

/* ---- the other tabs ---------------------------------------------------------------------- */

void fb_render_waypoints(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                         struct inkcell_fb_layout *layout);
void fb_render_devices(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout);
void fb_render_status(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                      struct inkcell_fb_layout *layout);
void fb_render_settings(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                        struct inkcell_fb_layout *layout);

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
extern const struct inkcell_band fb_air_band;

/* ---- what is drawn over a tab ------------------------------------------------------------ */

/*
 * What this client's layers are called, across frames.
 *
 * A layer's own memory - how far in it is - is kept on the backend under one of these, so the
 * ids have to outlive a frame and be this client's to choose (inkcell/ui/overlay.h). Zero is
 * INKCELL_OVERLAY_NONE and is not a layer.
 *
 * Only what is drawn *over* a screen is in here. A tab, a detail and a sheet that replaces the
 * body are places, and a place is a route rather than a layer.
 */
enum fb_overlay_id {
    /* "Save LoRa?", "Reboot the radio?" - the question a settings row raised. */
    FB_OVERLAY_CONFIRM = 1,
    /* The radio's half of the key-verification ceremony, which it can raise at any moment. */
    FB_OVERLAY_VERIFY,
    /* One node's verbs, over that node's detail. */
    FB_OVERLAY_NODE_ACTIONS,
    /* The faces one message can be answered with, over the transcript it is in. */
    FB_OVERLAY_REACTIONS,
    /* A window's right-click menu: the verbs of the row under the cursor, at the pointer. */
    FB_OVERLAY_CONTEXT,
};

/*
 * What this client's lists are called, for the glide.
 *
 * A window that moves a row moves every row in it by a row's height at once, which on this
 * panel is the whole body flicking; a list that glides draws its content displaced and eases
 * the displacement to nothing. What has to be remembered between frames is where the window
 * was, and there is one slot - so an id is only ever compared with the one the last frame left
 * behind, and a list that finds another's window in it takes it over without gliding.
 *
 * One per list rather than one per screen, because two of these screens draw a different list
 * depending on what is open: a list inheriting the window of the one it replaced would glide
 * from a place it was never at. Zero is not a list.
 */
enum fb_list_id {
    FB_LIST_CONVERSATIONS = 1,
    FB_LIST_NODES,
    FB_LIST_NODE_DETAIL,
    FB_LIST_NODE_ACTIONS,
    FB_LIST_WAYPOINTS,
    FB_LIST_WAYPOINT_DETAIL,
    FB_LIST_DEVICES,
    FB_LIST_SETTINGS,
    FB_LIST_HELP,
    FB_LIST_PICKER,
    FB_LIST_REACTIONS,
};

/*
 * The last question a layer was asked to put, so that it can finish leaving after the nav has
 * stopped asking it.
 *
 * A layer is on the panel until its travel says otherwise, and the app has to keep describing
 * its content for exactly that long - which the nav cannot do: mesh_ui_nav_confirm_close()
 * clears `confirm_action` the moment an answer is given, and a verification sheet's stage is
 * gone as soon as the exchange ends. Without this the two would vanish on the frame they were
 * answered instead of going away, which is what they did when they were screens.
 *
 * It is a copy of the *words*, not of the snapshot: a dialog is an icon and four short strings,
 * which is the one shape of overlay small enough to be copied. The snackbar keeps its text on
 * the backend for the same reason and says as much in inkcell/ui/fb_draw.h; anything larger
 * belongs to whoever can still describe it.
 */
struct fb_overlay_memo {
    char headline[96];
    char text[256];
    char accept[48];
    char cancel[48];
    enum inkcell_icon icon;
    uint32_t cursor;
    bool destructive;
    bool valid;
    /*
     * What the layer is *about*, for the overlays whose content is a lookup rather than a
     * sentence: the node a sheet of verbs belongs to, the message a column of faces answers.
     *
     * The same job as the four strings above and a cheaper one. A sheet's rows are built from
     * the roster and the catalog, both of which outlive the press - so all that has to survive
     * the nav closing the sheet is which subject to build them for.
     */
    uint32_t subject;
};

/*
 * The slot layer `id` describes itself into, or NULL when there is nowhere to keep one.
 *
 * NULL is a frame drawn with no memo behind it, which a caller reads as "describe it or do not
 * draw it" - the dialog is still put, it simply cannot go away slowly.
 */
struct fb_overlay_memo *fb_overlay_memo(struct inkcell_draw_state *state, enum fb_overlay_id id);

/*
 * The subject to draw layer `id` for: `subject` while the app still wants it up, and the one
 * it was last put with while it walks out.
 *
 * Zero is nothing to draw - a layer that never opened, or one whose subject left the roster
 * under it. Writes through the memo, so a caller asks once a frame and uses the answer.
 */
uint32_t fb_overlay_subject(struct inkcell_draw_state *state, enum fb_overlay_id id, bool up,
                            uint32_t subject);

/*
 * What this client's scrolled bodies are called.
 *
 * A scroll is where a body has got to, in pixels, and it belongs to the screen rather than to
 * the toolkit - so it is kept here beside the layers' own memory and handed out by id. One
 * entry per body that is positioned rather than windowed; a list walking a row index at a time
 * needs none.
 */
enum fb_scroll_id {
    /* The help notes, whose heights are a property of their *words*: the one body in this
       client that a row model genuinely cannot describe. */
    FB_SCROLL_HELP,
    FB_SCROLL_COUNT,
};

/*
 * What a list is for, which is what decides how it is set - see fb_list_look().
 *
 * Three kinds, because that is how many this client draws: somewhere to go, something to change
 * where it stands, and a column of subjects to read down. A screen names its kind and never a
 * density or a focus style, for the reason it names a string id rather than a sentence: one
 * answer in one place, so the Settings tab and the device list cannot drift into two looks for
 * the same kind of list.
 */
enum fb_list_role {
    /* Places to go: the settings sections, the radios, a picker. Short, and every row opens
       something - so it can afford the room. */
    FB_LIST_ROLE_MENU = 0,
    /* A settings section's fields, grouped under their headings and changed on the row. */
    FB_LIST_ROLE_FORM,
    /* Many subjects, each a name and a line under it: nodes, conversations, waypoints. The list
       that runs to a hundred rows, where a row's height is what it costs. */
    FB_LIST_ROLE_FEED,
};

/*
 * The look for a list of `role`, at this frame's width.
 *
 * Every list takes the accent cursor - a light lift and a capsule down the leading edge, with
 * the row's own inks kept - and tiered type, so a row's second line and its trailing figure
 * recede by size rather than by colour alone. A menu and a form stand in inset sections; a feed
 * stays on the panel.
 *
 * Density follows the panel as well as the role. A menu and a form are comfortable
 * everywhere: they are a dozen or two rows, and air is what makes them read as a settings
 * screen rather than a file listing - the difference between a handheld app and a launcher's
 * option page. A feed is compact on the handheld's own compact width, where a row of air is a
 * node that is not on screen, and comfortable once the width class says the list is a column
 * in a larger window. Separators come with comfortable rows only - a compact step has
 * no leading for a hairline to stand in (see struct inkcell_fb_list_style).
 */
struct inkcell_fb_list_style fb_list_look(const struct inkcell_draw_state *state,
                                          enum fb_list_role role);

/*
 * A list of `count` items at `per_item` steps each, set as `role` says.
 *
 * inkcell_fb_list_begin_styled() takes a height per item rather than a count, so the uniform
 * lists - the conversations, the radios, the waypoints, the picker - write theirs into `steps`,
 * which is the caller's and is borrowed for the life of the list, exactly as the heights a
 * screen measures itself are. A list longer than `capacity` is opened unstyled rather than
 * with heights nobody wrote: the look is lost, the rows and the cursor are not.
 */
struct inkcell_fb_list fb_list_begin_steps(const struct inkcell_draw_state *state,
                                           const struct inkcell_fb_layout *layout, uint32_t count,
                                           uint32_t cursor, uint8_t per_item, uint8_t *steps,
                                           size_t capacity, enum fb_list_role role);

/*
 * A layout for a region of the frame that is not the body: a sheet's content, a scrolled
 * body's full extent.
 *
 * The components measure themselves against a layout, and either of those is a layout's worth
 * of room that simply is not the body - the same line advance and the same columns, in a band
 * of its own. Stated as a derivation rather than as a second kind of thing, because the
 * alternative would be every component in the toolkit growing a second entry point for the one
 * caller that draws into a panel.
 */
struct inkcell_fb_layout fb_layout_in(const struct inkcell_fb_layout *layout,
                                      struct inkcell_fb_rect box);

/* The scroll kept for `id`, or NULL when there is nowhere to keep one - a frame with no memo
   behind it, which a caller reads as "draw it settled". */
struct inkcell_scroll *fb_scroll(struct inkcell_draw_state *state, enum fb_scroll_id id);

/*
 * Says whether the body this frame drew is still travelling.
 *
 * Written by whoever drew a viewport and read back by fb_app_pending(), which is how a moving
 * body asks for the next frame. A frame that draws no viewport says false, which is what stops
 * the client repainting for a scroll that has gone away with its screen.
 */
void fb_scroll_report(struct inkcell_draw_state *state, bool moving);

/*
 * Opens a bottom sheet over the body and hands back a layout for what goes in it.
 *
 * `content_h` is the height the content would like; a sheet taller than the body is fitted to
 * it, so a screen asks for what it has rather than measuring the room first. `out` is the
 * content rectangle as a layout - a list drawn through it lands inside the sheet, because a
 * sheet is full bleed and its content lines up with the rows of the screen it came up over.
 *
 * Returns false once the sheet has finished leaving, which is when a screen stops describing
 * it. Pairs with fb_sheet_end() on true, and nothing on false - the layer's own `if`.
 */
bool fb_sheet_begin(struct inkcell_draw_state *state, const struct inkcell_fb_layout *layout,
                    enum fb_overlay_id id, bool up, const struct inkcell_fb_sheet *sheet,
                    int content_h, struct inkcell_overlay_frame *frame,
                    struct inkcell_fb_layout *out);
void fb_sheet_end(struct inkcell_draw_state *state, struct inkcell_overlay_frame *frame);

void fb_render_help(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                    struct inkcell_fb_layout *layout);
/*
 * The two questions, each on a layer over the screen that raised it.
 *
 * Called on every frame rather than chosen between, because a layer that is leaving is not in
 * the snapshot any more and the call is what walks it out - see struct fb_dialog_memo. Both are
 * no-ops on a frame where the question is neither up nor still on its way out.
 */
void fb_render_confirm(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout);
void fb_render_verify(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                      struct inkcell_fb_layout *layout);
/* The two code sheets: this radio's channels, and this radio's contact record. */
void fb_render_share(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                     struct inkcell_fb_layout *layout);
void fb_render_contact(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout);
/* The radio's airtime, over the Status cards that offered it. */
void fb_render_trend(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                     struct inkcell_fb_layout *layout);

#endif /* MESH_UI_BACKENDS_FB_SCREENS_INTERNAL_H */
