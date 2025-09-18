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
};

#ifdef __cplusplus
}
#endif

