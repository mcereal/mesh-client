#pragma once

#include "mesh/ui/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_backend_minui_context {
    bool warned_placeholder;
    bool warned_list_missing;
    bool warned_presenter_missing;
};

const struct mesh_ui_backend *mesh_ui_backend_minui(void);
bool mesh_ui_backend_minui_is_available(void);

#ifdef __cplusplus
}
#endif
