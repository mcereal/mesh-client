#pragma once

#include "inkcell/ui/backend.h"

#include "mesh/ui/store.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_backend_cli_context {
    struct mesh_ui_snapshot last_snapshot;
    bool has_snapshot;
    unsigned int updates_emitted;
    FILE *tty_stream;
};

const struct inkcell_backend *mesh_ui_backend_cli(void);

#ifdef __cplusplus
}
#endif
