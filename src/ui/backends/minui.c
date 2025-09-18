#include "mesh/ui/backends/minui.h"

#include "mesh/log.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static const char *k_minui_presenter = "minui-presenter";
static const char *k_minui_list = "minui-list";
static const char *k_minui_keyboard = "minui-keyboard";

bool mesh_ui_backend_minui_is_available(void) {
    return access(k_minui_presenter, X_OK) == 0 || access(k_minui_list, X_OK) == 0 || access(k_minui_keyboard, X_OK) == 0;
}

static int mesh_ui_backend_minui_init(void **state, void *userdata) {
    (void)state;
    struct mesh_ui_backend_minui_context *context = (struct mesh_ui_backend_minui_context *)userdata;
    if (context != NULL) {
        context->warned_placeholder = false;
    }
    mesh_log_info("ui", "MinUI backend initialised (placeholder mode)");
    return 0;
}

static void mesh_ui_backend_minui_shutdown(void *state, void *userdata) {
    (void)state;
    (void)userdata;
}

static void mesh_ui_backend_minui_present(void *state, const struct mesh_ui_snapshot *snapshot, void *userdata) {
    (void)state;
    (void)snapshot;
    struct mesh_ui_backend_minui_context *context = (struct mesh_ui_backend_minui_context *)userdata;
    if (context != NULL && !context->warned_placeholder) {
        mesh_log_warn("ui", "MinUI backend placeholder active; packaging helpers still pending");
        context->warned_placeholder = true;
    }
}

const struct mesh_ui_backend *mesh_ui_backend_minui(void) {
    static const struct mesh_ui_backend k_backend = {
        .name = "minui",
        .init = mesh_ui_backend_minui_init,
        .shutdown = mesh_ui_backend_minui_shutdown,
        .present = mesh_ui_backend_minui_present,
    };
    return &k_backend;
}

