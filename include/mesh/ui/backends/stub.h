#pragma once

#include "mesh/ui/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_backend_stub_context {
    struct mesh_ui_snapshot last_snapshot;
    bool has_snapshot;
    size_t present_calls;
};

const struct mesh_ui_backend *mesh_ui_backend_stub(void);

#ifdef __cplusplus
}
#endif

