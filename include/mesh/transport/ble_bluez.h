#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_bluez_client {
    void *connection;
    bool connected;
};

int mesh_bluez_client_init(struct mesh_bluez_client *client);
void mesh_bluez_client_shutdown(struct mesh_bluez_client *client);
int mesh_bluez_client_check_ready(struct mesh_bluez_client *client);

#ifdef __cplusplus
}
#endif
