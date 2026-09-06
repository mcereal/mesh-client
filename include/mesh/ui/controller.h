#pragma once

#include "mesh/core/event_loop.h"
#include "mesh/ui/store.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_backend;
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
    const struct mesh_ui_backend *backend;
    void *backend_state;
    void *backend_userdata;
    struct mesh_event_loop *loop;
    bool registered;
    /* Armed after a frame the backend says is still moving, disarmed the moment it settles.
       -1 when the timer could not be created, which costs animation and nothing else. */
    int frame_timer_fd;
    mesh_ui_action_handler on_action;
    void *action_userdata;
};

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct mesh_ui_backend *backend, void *backend_userdata,
                            struct mesh_event_loop *loop);
void mesh_ui_controller_shutdown(struct mesh_ui_controller *controller);

void mesh_ui_controller_set_action_handler(struct mesh_ui_controller *controller,
                                           mesh_ui_action_handler handler, void *userdata);

/* Feed one logical button press through the store's navigation model. Repaints happen via
   the store's eventfd on the next loop turn; actions go to the handler above right away. */
void mesh_ui_controller_handle_key(struct mesh_ui_controller *controller, enum mesh_ui_key key);

#ifdef __cplusplus
}
#endif
