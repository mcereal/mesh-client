#pragma once

#include "mesh/ui/backend.h"

#include <stdbool.h>

struct mesh_event_loop;

struct mesh_ui_backend_sdl_context {
    struct mesh_event_loop *loop;
    bool initialised;
};

const struct mesh_ui_backend *mesh_ui_backend_sdl(void);
bool mesh_ui_backend_sdl_is_available(void);

