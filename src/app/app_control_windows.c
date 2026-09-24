#include "mesh/app/control.h"

#include <errno.h>
#include <string.h>

int mesh_app_control_open(struct mesh_app_control *control, struct inkwell_loop *loop,
                          const struct mesh_app_control_host *host, const char *path) {
    (void)loop;
    (void)host;
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
