#pragma once

#include "mesh/config.h"
#include "mesh/event_loop.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_transport;

struct mesh_transport_ops {
    int (*start)(struct mesh_transport *transport, const struct mesh_app_config *config,
                 struct mesh_event_loop *loop);
    void (*stop)(struct mesh_transport *transport);
    const char *(*status)(const struct mesh_transport *transport);
    void (*tick)(struct mesh_transport *transport);
};

struct mesh_transport {
    const char *name;
    void *state;
    const struct mesh_transport_ops *ops;
};

#define MESH_TRANSPORT_REGISTRY_MAX 8

struct mesh_transport_registry {
    struct mesh_transport *transports[MESH_TRANSPORT_REGISTRY_MAX];
    size_t count;
    bool started;
};

void mesh_transport_registry_init(struct mesh_transport_registry *registry);
int mesh_transport_registry_register(struct mesh_transport_registry *registry,
                                     struct mesh_transport *transport);
int mesh_transport_registry_start_all(struct mesh_transport_registry *registry,
                                      const struct mesh_app_config *config,
                                      struct mesh_event_loop *loop);
void mesh_transport_registry_stop_all(struct mesh_transport_registry *registry);
void mesh_transport_registry_tick(struct mesh_transport_registry *registry);
int mesh_transport_registry_handle_command(struct mesh_transport_registry *registry,
                                           const char *command, const char *arg);

#ifdef __cplusplus
}
#endif
