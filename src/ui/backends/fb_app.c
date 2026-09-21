/*
 * What this client hands inkcell, and the two questions it answers that a snapshot cannot.
 *
 * inkcell owns the panel: the pages, the flip, the damage and the clock. It calls up into here
 * once a frame, with a snapshot it treats as an opaque pointer. Everything that knows what a
 * Meshtastic client is about is on this side of that call.
 *
 * The two facts pushed down are the ones no snapshot carries, because both are about the
 * *previous* frame rather than about the present one:
 *
 *   - Which way the reader moved. A snapshot says where they are; only something that remembers
 *     where they were can tell an entrance from an exit, and what counts as "further in" is this
 *     client's tab order and screen stack rather than anything a UI toolkit could know.
 *   - Which theme they chose. The app publishes it in the client info and the backend adopts it
 *     on the next frame, rather than anything reaching in to push at it - which is what keeps a
 *     frame a function of the snapshot.
 */

#include "fb_internal.h"

#include "mesh/ui/backends/fb.h"
#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct fb_app *fb_app_of(const struct inkcell_draw_state *state) {
    return state != NULL ? (struct fb_app *)state->app.ctx : NULL;
}

static void fb_app_render(struct inkcell_draw_state *state, const void *snapshot_ptr, void *ctx) {
    const struct mesh_ui_snapshot *const snapshot = (const struct mesh_ui_snapshot *)snapshot_ptr;
    struct fb_app *const app = (struct fb_app *)ctx;
    if (state == NULL || snapshot == NULL || app == NULL) {
        return;
    }

    /* The theme the snapshot names, if this build knows it and is not already drawing with it. */
    (void)inkcell_fb_state_set_theme_by_id(state, snapshot->settings.client.theme);

    /*
     * The move, worked out here because the comparison is about this client's places.
     *
     * First sight adopts, the rule the animation table follows for an id it has not seen: a
     * screen that slid in on the frame the client came up would be announcing itself rather than
     * reporting a move.
     *
     * The route *under the layers*, because what is drawn over a screen is not a move: the two
     * questions, a node's verbs and a message's faces all arrive on layers of their own over
     * the body they are about, and a body that slid out from under an arriving panel would be
     * two things travelling at once. Everything else that asks where the user is still wants
     * the whole route.
     */
    struct mesh_ui_route route;
    mesh_ui_route_under_layers(&snapshot->nav, &route);
    if (!app->route_valid) {
        app->route = route;
        app->route_valid = true;
    } else {
        const enum inkcell_transition move = mesh_ui_route_move(&app->route, &route);
        app->route = route;
        inkcell_fb_transition_begin(state, move);
    }

    fb_render_snapshot(state, snapshot);
}

static bool fb_app_pending(void *ctx) {
    const struct fb_app *const app = (const struct fb_app *)ctx;
    if (app == NULL) {
        return false;
    }
    /* A tile still to decode and a body still travelling are the same answer to inkcell's
       question - the next frame will show something this one could not - and neither is an
       entry in the animation table. See `scrolling` on struct fb_app. */
    return app->scrolling || (app->basemap != NULL && app->basemap->pending);
}

static void fb_app_frame_begin(void *ctx) {
    struct fb_app *const app = (struct fb_app *)ctx;
    if (app != NULL && app->basemap != NULL) {
        app->basemap->pending = false;
    }
}

/*
 * Drops everything this client hung off the backend's state.
 *
 * Called on shutdown, and again whenever a memo has to go - a reference render, a geometry
 * change. Both caches are this client's: a thread cache is laid-out message threads and a
 * render cache is a drawn screen, and neither is a thing a UI toolkit could have an opinion
 * about. inkcell keeps the two slots because the state is what a renderer is handed.
 *
 * The basemap goes too, and it has to go through the state rather than through `ctx`, because
 * closing a pack is what fb_basemap_close() does and it is written against the state like
 * everything else in fb_map.c.
 */
static void fb_app_drop_caches(struct inkcell_draw_state *state, void *ctx) {
    (void)ctx;
    if (state == NULL) {
        return;
    }
    fb_thread_cache_free(state);
    fb_render_cache_free(state);
}

/*
 * Teardown: the memos, and the pack the memos were drawn over.
 *
 * The basemap is here and not in fb_app_drop_caches() because it is not a memo - a tile pack is
 * a file this client opened, and a reference render wanting a fresh frame is no reason to close
 * it. Getting that wrong left every frame after the first reference render drawing a map with
 * nothing under it.
 */
static void fb_app_close(struct inkcell_draw_state *state, void *ctx) {
    fb_app_drop_caches(state, ctx);
    fb_basemap_close(state);
    free(ctx);
}

/*
 * An off-screen page with this client behind it.
 *
 * inkcell opens a capture with nothing installed - it has no way to know what an application
 * draws - so this is the wrapper that supplies it, the same way mesh_app_select_fb() does for
 * the device backend. Without it a captured frame is a cleared panel, which is a picture of
 * nothing that looks a lot like a picture of a bug.
 */
int mesh_ui_capture_open(struct inkcell_capture **out, uint32_t width, uint32_t height, int scale) {
    const int result = inkcell_capture_open(out, width, height, scale);
    if (result == 0 && out != NULL && *out != NULL) {
        inkcell_fb_set_app(inkcell_capture_state(*out), fb_app_vtable());
    }
    return result;
}

/*
 * Opens a tile pack on a capture, so a captured map has a basemap under it.
 *
 * The harness half of fb_basemap_open(): a scene names its pack, because a frame that quietly
 * picked up whatever pack the developer happened to have installed would render differently on
 * two machines. It installs the app first, since a capture is opened by inkcell with nothing
 * behind it.
 */
int mesh_ui_capture_open_map_pack(struct inkcell_capture *capture, const char *path) {
    struct inkcell_draw_state *const state = inkcell_capture_state(capture);
    if (state == NULL) {
        return -EINVAL;
    }
    return fb_basemap_open(state, path);
}

/*
 * One app per backend, not one per process.
 *
 * It looks like there is only ever one panel, and on the device there is - but a capture is a
 * backend too, and the suite opens several, sometimes two at once to compare them. Sharing one
 * static across them shares the remembered route, so a capture opened after another one had
 * been to the same screen saw no move and drew a transition in place; and it shares the
 * basemap, so closing either capture closed the pack the other was drawing over.
 *
 * The vtable itself is static because inkcell_fb_set_app() copies it; only what it points at
 * has to outlive the call, which is the allocation below and fb_app_close() freeing it.
 */
const struct inkcell_fb_app *fb_app_vtable(void) {
    static struct inkcell_fb_app vtable;
    struct fb_app *const app = calloc(1U, sizeof *app);
    vtable = (struct inkcell_fb_app){
        .ctx = app,
        .render = fb_app_render,
        .pending = fb_app_pending,
        .frame_begin = fb_app_frame_begin,
        .drop_caches = fb_app_drop_caches,
        .close = fb_app_close,
    };
    return &vtable;
}
