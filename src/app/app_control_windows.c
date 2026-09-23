#include "mesh/app/control.h"

#include <errno.h>
#include <string.h>

int mesh_app_control_open(struct mesh_app_control *control, struct inkwell_loop *loop,
                          struct mesh_ui_controller *controller, const char *path) {
    (void)loop;
    (void)controller;
    (void)path;
    if (control == NULL) {
        return -EINVAL;
    }
    memset(control, 0, sizeof *control);
    return -ENOTSUP;
}

void mesh_app_control_close(struct mesh_app_control *control) { (void)control; }

int mesh_app_control_send(const char *path, const char *commands, FILE *out) {
    (void)path;
    (void)commands;
    (void)out;
    return -ENOTSUP;
}
