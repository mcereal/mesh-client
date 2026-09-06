#pragma once

#include "mesh/ui/store.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_backend {
    const char *name;
    int (*init)(void **state, void *userdata);
    void (*shutdown)(void *state, void *userdata);
    void (*present)(void *state, const struct mesh_ui_snapshot *snapshot, void *userdata);
    /*
     * Whether the last frame is still moving, and so whether the backend is owed another one
     * without anything having changed.
     *
     * The store publishes on change, which is all a screen made of text ever needs. A control
     * that animates needs the opposite: several frames from one change. Rather than have the
     * store invent updates nobody asked for, a backend that animates says so here and the
     * controller keeps waking it until it stops - see mesh_ui_controller_init().
     *
     * Optional. A backend that draws everything in one go leaves it NULL and nothing ticks.
     */
    bool (*animating)(void *state, void *userdata);
};

#ifdef __cplusplus
}
#endif
