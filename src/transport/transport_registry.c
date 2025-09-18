#include "mesh/transport/transport.h"

#include "mesh/log.h"

#include <errno.h>
#include <string.h>

void mesh_transport_registry_init(struct mesh_transport_registry *registry) {
    if (registry == NULL) {
        return;
    }
    memset(registry, 0, sizeof *registry);
}

static bool contains_transport(const struct mesh_transport_registry *registry, const struct mesh_transport *transport) {
    for (size_t i = 0; i < registry->count; ++i) {
        if (registry->transports[i] == transport) {
            return true;
        }
    }
    return false;
}

int mesh_transport_registry_register(struct mesh_transport_registry *registry, struct mesh_transport *transport) {
    if (registry == NULL || transport == NULL) {
        return -EINVAL;
    }

    if (registry->count >= MESH_TRANSPORT_REGISTRY_MAX) {
        return -ENOSPC;
    }

    if (contains_transport(registry, transport)) {
        return -EEXIST;
    }

    registry->transports[registry->count++] = transport;
    return 0;
}

int mesh_transport_registry_start_all(struct mesh_transport_registry *registry, const struct mesh_app_config *config,
                                      struct mesh_event_loop *loop) {
    if (registry == NULL || config == NULL || loop == NULL) {
        return -EINVAL;
    }

    if (registry->started) {
        return 0;
    }

    for (size_t i = 0; i < registry->count; ++i) {
        struct mesh_transport *transport = registry->transports[i];
        if (transport == NULL || transport->ops == NULL || transport->ops->start == NULL) {
            mesh_log_warn("transport", "Skipping invalid transport at index %zu", i);
            continue;
        }

        int result = transport->ops->start(transport, config, loop);
        if (result < 0) {
            mesh_log_error("transport", "Failed to start %s: %d", transport->name, result);
            mesh_transport_registry_stop_all(registry);
            return result;
        }
    }

    registry->started = true;
    return 0;
}

void mesh_transport_registry_stop_all(struct mesh_transport_registry *registry) {
    if (registry == NULL || !registry->started) {
        return;
    }

    for (size_t i = registry->count; i > 0; --i) {
        struct mesh_transport *transport = registry->transports[i - 1U];
        if (transport != NULL && transport->ops != NULL && transport->ops->stop != NULL) {
            transport->ops->stop(transport);
        }
    }

    registry->started = false;
}
