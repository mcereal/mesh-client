#pragma once

#include "mesh/ui/backend.h"

#include <stdbool.h>

struct mesh_event_loop;

struct mesh_ui_backend_fb_context {
    struct mesh_event_loop *loop;
};

const struct mesh_ui_backend *mesh_ui_backend_fb(void);
bool mesh_ui_backend_fb_is_available(void);

