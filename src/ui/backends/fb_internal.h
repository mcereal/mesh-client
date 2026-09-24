#ifndef MESH_UI_BACKENDS_FB_INTERNAL_H
#define MESH_UI_BACKENDS_FB_INTERNAL_H

/*
 * This client's half of the framebuffer backend.
 *
 * The drawing toolkit and the components moved to inkcell (third_party/inkcell) - pixels,
 * glyphs, the palette, the page geometry, buttons, lists, cards, meters. What is left here is
 * everything that knows what a Meshtastic client is about:
 *
 *   fb_screens_*  one renderer per screen, drawn out of a snapshot (fb_screens_frame.c draws
 *                 the chrome and dispatches between them)
 *   fb_map.c      the one screen that places things at coordinates, and the tile pack under it
 *
 * inkcell owns the panel and calls up into this through `struct inkcell_fb_app` - see
 * fb_app_install(). It never sees a store, a route or a node; the two facts a frame needs that
 * a snapshot does not carry are pushed down instead, which is what fb_app_render() does with
 * the theme id and the transition.
 */

#include "inkcell/ui/fb_draw.h"
#include "inkcell/ui/widgets/chrome.h"

#include "mesh/map/source.h"
#include "mesh/map/tile_cache.h"
#include "mesh/ui/route.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The pictures under the map: a tile pack, the tiles decoded out of it, and the buffer one
 * tile's bytes are read into.
 *
 * It hangs off this client's app context rather than off the store, and that is the same
 * boundary the rest of the map already draws. A backend is handed a `const` snapshot: it cannot
 * write a tile back into the nav, and the nav must not learn how wide this panel's body is - so
 * which tiles are wanted is a question only the thing that measured the body can ask, and what
 * it gets back is pixels in a format only a thing that knows the panel can use. Both halves live
 * here.
 *
 * Allocated on open, so a client with no pack pays one pointer. mesh/map/ is what is inside it;
 * src/ui/backends/fb_map.c is the only file that touches these fields.
 */
struct fb_basemap {
    struct mesh_map_source source;
    struct mesh_map_tile_cache cache;
    /* One tile's encoded bytes. A caller's local in the flag that describes a pack, and a
       long-lived block here, because the fill loop reads one on the frame path and a megabyte
       is not a thing to put on the stack under a renderer. */
    uint8_t *encoded;
    bool open;
    /*
     * Whether the last frame wanted a tile it did not have.
     *
     * This is what asks for the next frame: a frame is otherwise a function of the snapshot, and
     * no press and no packet says that a tile is still on its way. One tile is read per frame
     * (one decode per turn of the event loop), so a full view fills over about twenty frames
     * with input serviced between them rather than in one stall the length of a dropped
     * connection.
     */
    bool pending;
};

/*
 * What this client hands inkcell: the renderer, and the state the renderer needs that the
 * snapshot does not carry.
 *
 * `route` is the place the last frame was drawn for. A snapshot says where the user *is*, never
 * that they have just arrived, so the only thing that can tell an entrance from an exit is
 * something that remembers the previous frame - and what counts as "further in" is this client's
 * question rather than the toolkit's, which is why it is answered here and pushed down through
 * inkcell_fb_transition_begin().
 */
struct fb_app {
    /* NULL until a pack is opened, which is every run that was not pointed at one. */
    struct fb_basemap *basemap;
    struct mesh_ui_route route;
    bool route_valid;
    /*
     * Whether a body drawn on the last frame was still travelling.
     *
     * A scroll lives on the screen rather than on the backend's state - it is not keyed by
     * anything the toolkit issued and a screen may have several - so
     * inkcell_fb_state_animating() cannot see one, and says so at length in
     * inkcell/ui/widgets/scroll.h. This is the half of that a screen owes back: the renderer
     * writes it every frame it draws a viewport, and fb_app_pending() reports it alongside the
     * basemap still filling. Without it a body eases to wherever the last frame it was asked
     * for left it.
     */
    bool scrolling;
};

/* The app behind `state`, or NULL when nothing installed one. */
struct fb_app *fb_app_of(const struct inkcell_draw_state *state);

/* The vtable inkcell's backend is opened with - see struct inkcell_backend_fb_context. One
   static app, because there is one panel. */
const struct inkcell_fb_app *fb_app_vtable(void);

/* ---- fb_map.c ------------------------------------------------------------------------------ */

/* The map over the node list: the graticule, the markers and what the crosshair is on. Its own
   file because it is the one screen that places things at coordinates rather than describing
   rows - see the paragraph at the top of it. */
void fb_render_map(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                   struct inkcell_fb_layout *layout);

/*
 * Opens the tile pack at `path` and hangs it off the app. 0, or -errno from the pack reader.
 *
 * A second open closes the first and **clears the cache with it**: a key is three numbers about
 * the world rather than about a file, so two packs of the same city hold different pictures at
 * the same key - carried across a swap, the map draws the old pack's streets under the new
 * pack's attribution and nothing on the frame looks wrong.
 */
int fb_basemap_open(struct inkcell_draw_state *state, const char *path);

/*
 * Opens whatever pack this device has, if any: MESHCLIENT_MAP_PACK when it is set, otherwise the
 * conventional file a sideload lands at. Missing is the ordinary case and is not an error - the
 * map draws its graticule and says nothing.
 *
 * The device backend calls this and the capture harness deliberately does not: a scene names its
 * pack, because a frame that quietly picked up whatever pack the developer had installed would
 * render differently on two machines.
 */
void fb_basemap_open_default(struct inkcell_draw_state *state);

/* Closes the pack, releases the cache and the read buffer, and leaves the app with no basemap.
   Safe on an app that never opened one. */
void fb_basemap_close(struct inkcell_draw_state *state);

/* Whether the frame just drawn wanted a tile it did not have - what inkcell adds to its own
   animations when it decides whether another frame is owed. */
bool fb_basemap_pending(const struct inkcell_draw_state *state);

/* Clears that, so a frame drawing any other screen stops the map asking for the next one. */
void fb_basemap_frame_begin(struct inkcell_draw_state *state);

/* ---- the memos this client hangs off inkcell's state ---------------------------------------
 *
 * Two slots inkcell keeps for an application and neither allocates nor frees - see the note on
 * them in inkcell/ui/fb_draw.h. Both are dropped together, from fb_app_close().
 */

/* The laid-out message thread, memoised so scrolling a conversation does not re-wrap every
   bubble above the window on every frame. */
void fb_thread_cache_free(struct inkcell_draw_state *state);

/* The last drawn screen, memoised so a frame that changed nothing is a compare rather than a
   redraw. */
void fb_render_cache_free(struct inkcell_draw_state *state);

/* The box the last frame's content stood in - the panel, less the rail when a wider width class
   put the tabs down the leading edge. The whole panel before anything has been drawn. */
struct inkcell_box fb_render_content(const struct inkcell_draw_state *state);

/*
 * Whether a move from `from` to `to` stays inside the two panes the last frame drew side by side
 * - the conversations and a thread, on a window wide enough for both. Such a move is not a place
 * arriving: both halves were already on the panel, so fb_app.c starts no slide for it, which
 * would carry the list that stayed put across the window with the thread that changed.
 */
bool fb_render_split_pair(const struct inkcell_draw_state *state, const struct mesh_ui_route *from,
                          const struct mesh_ui_route *to);

/*
 * A screen's heading. Every screen - the map included - draws its app bar through this rather than
 * inkcell's call, and says only what it always said - its title, its trail, its badge.
 *
 * The first heading on a frame is also handed what the frame knows and the screen does not: for
 * a pointer, the screen's verbs as app bar actions and the link's status mark, since that frame
 * has no foot to say them (see fb_heading_begin() in fb_screens_frame.c). A bar that sets either
 * itself keeps its own; a second heading on the same frame - a sheet's over a list's - gets
 * neither, so the link and the verbs are said once.
 */
struct inkcell_fb_app_bar_fit fb_draw_app_bar(const struct inkcell_draw_state *state,
                                              struct inkcell_fb_layout *layout,
                                              const struct inkcell_fb_app_bar *bar);

/* ---- fb_screens_frame.c -------------------------------------------------------------------- */

/* Draws one whole frame: chrome, then whichever screen the snapshot says is up. */
void fb_render_snapshot(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot);

#endif /* MESH_UI_BACKENDS_FB_INTERNAL_H */
