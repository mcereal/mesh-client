#include "mesh/ui/backends/stub.h"

#include <string.h>

static int mesh_ui_backend_stub_init(void **state, void *userdata) {
    if (state != NULL) {
        *state = NULL;
    }
    if (userdata != NULL) {
        struct mesh_ui_backend_stub_context *context = (struct mesh_ui_backend_stub_context *)userdata;
        context->has_snapshot = false;
        context->present_calls = 0U;
        memset(&context->last_snapshot, 0, sizeof context->last_snapshot);
    }
    return 0;
}

static void mesh_ui_backend_stub_shutdown(void *state, void *userdata) {
    (void)state;
    if (userdata != NULL) {
        struct mesh_ui_backend_stub_context *context = (struct mesh_ui_backend_stub_context *)userdata;
        context->has_snapshot = false;
    }
}

static void mesh_ui_backend_stub_present(void *state, const struct mesh_ui_snapshot *snapshot, void *userdata) {
    (void)state;
    if (userdata == NULL || snapshot == NULL) {
        return;
    }
    struct mesh_ui_backend_stub_context *context = (struct mesh_ui_backend_stub_context *)userdata;
    context->present_calls++;
    context->last_snapshot = *snapshot;
    context->has_snapshot = true;
}

const struct mesh_ui_backend *mesh_ui_backend_stub(void) {
    static const struct mesh_ui_backend k_backend = {
        .name = "stub",
        .init = mesh_ui_backend_stub_init,
        .shutdown = mesh_ui_backend_stub_shutdown,
        .present = mesh_ui_backend_stub_present,
    };
    return &k_backend;
}

