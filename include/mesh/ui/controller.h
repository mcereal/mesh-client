#pragma once

#include "inkwell/runtime/loop.h"
#include "mesh/ui/store.h"

#ifdef __cplusplus
extern "C" {
#endif

struct inkcell_backend;
struct inkcell_surface;
struct mesh_ui_action;

/* Invoked from mesh_ui_controller_handle_key() when a press asks for something outside the
   UI: connect to a device, send a message. */
typedef void (*mesh_ui_action_handler)(void *userdata, const struct mesh_ui_action *action);

/*
 * How often a backend with something in flight is woken.
 *
 * ~33 ms is 30 frames a second, which is as fine as a 3.2" panel and a knob travelling a
 * finger's width can show. It is a ceiling rather than a heartbeat: the timer is armed only
 * while the backend reports it is still moving, so a HUD sitting still costs exactly the
 * wake-ups it did before any of this existed - which on a handheld running off a battery is
 * the only version of this worth shipping.
 */
#define MESH_UI_FRAME_INTERVAL_MS 33U

struct mesh_ui_controller {
    struct mesh_ui_store *store;
    struct mesh_ui_snapshot snapshot;
    bool snapshot_valid;
    const struct inkcell_backend *backend;
    void *backend_state;
    void *backend_userdata;
    struct inkwell_loop *loop;
    bool registered;
    /* Armed after a frame the backend says is still moving, disarmed the moment it settles.
       -1 when the timer could not be created, which costs animation and nothing else. */
    int frame_timer_fd;
    /* Whether that timer holds a deadline that has not fired - what lets a request for a
       frame join one already on its way rather than push it back. */
    bool frame_armed;
    mesh_ui_action_handler on_action;
    void *action_userdata;
};

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct inkcell_backend *backend, void *backend_userdata,
                            struct inkwell_loop *loop);
void mesh_ui_controller_shutdown(struct mesh_ui_controller *controller);

/*
 * Whether the backend named at init actually opened.
 *
 * A backend whose init() refused is dropped and the controller carries on without one, drawing
 * nothing - which is the right answer for a run that has no UI to speak of and the wrong one
 * for a client that had another backend it could have used instead. init() returning 0
 * therefore does not mean there is a panel, and this is the question that does.
 *
 * Asked by the composition root, which is the only layer that knows what the second choice
 * would have been. False also for a controller opened with no backend at all.
 */
bool mesh_ui_controller_has_backend(const struct mesh_ui_controller *controller);

void mesh_ui_controller_set_action_handler(struct mesh_ui_controller *controller,
                                           mesh_ui_action_handler handler, void *userdata);

/* Feed one logical button press through the store's navigation model. Repaints happen via
   the store's eventfd on the next loop turn; actions go to the handler above right away. */
void mesh_ui_controller_handle_key(struct mesh_ui_controller *controller, enum inkcell_key key);

/* The same for a click on `target`, an id the last frame registered - a tab, a row, a dialog's
   answer (mesh/ui/focus.h). What a window's pointer hands on; see inkcell/ui/pointer.h. */
void mesh_ui_controller_handle_click(struct mesh_ui_controller *controller, uint32_t target);

/* A secondary click - a right-click - on `target` at (x, y) in the frame's pixels: a row's menu,
   or an open one put down. NONE is a click on nothing, which still puts one down. */
void mesh_ui_controller_handle_context(struct mesh_ui_controller *controller, uint32_t target,
                                       int x, int y);

/* One more frame of the last snapshot, for a backend whose window changed under it - the SDL
   window moving its tabs clear of the title-bar buttons after a resize. The store has nothing
   new to say, so this is the frame timer's path rather than the store's. */
void mesh_ui_controller_request_frame(struct mesh_ui_controller *controller);

/*
 * Whether the panel has stopped moving: no frame armed, so the last one presented is the one that
 * will stay up. What something taking a picture waits for - a slide caught halfway is not a
 * picture of the screen it was going to.
 */
bool mesh_ui_controller_settled(const struct mesh_ui_controller *controller);

/* The last frame presented, from the backend's `frame` hook. False with no frame yet, or with a
   backend that draws no pixels. Good until the next present. */
bool mesh_ui_controller_frame(const struct mesh_ui_controller *controller,
                              struct inkcell_surface *out);

#ifdef __cplusplus
}
#endif
