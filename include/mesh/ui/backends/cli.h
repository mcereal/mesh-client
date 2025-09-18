#pragma once

#include "mesh/ui/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_backend_cli_context {
    struct mesh_ui_snapshot last_snapshot;
    bool has_snapshot;
    unsigned int updates_emitted;
};

const struct mesh_ui_backend *mesh_ui_backend_cli(void);

#ifdef __cplusplus
}
#endif

