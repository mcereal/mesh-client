#pragma once

#include "mesh/config.h"
#include "mesh/event_loop.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_transport;
struct mesh_session;

struct mesh_transport_ops {
    int (*start)(struct mesh_transport *transport, const struct mesh_app_config *config,
                 struct mesh_event_loop *loop);
    void (*stop)(struct mesh_transport *transport);
    const char *(*status)(const struct mesh_transport *transport);
    void (*tick)(struct mesh_transport *transport);
    /* Optional. Points the link at a session the caller owns instead of the one embedded in
       the transport, so every link feeds the same node cache, message log and settings and
       switching transports does not lose the conversation. Call before start(). */
    void (*set_session)(struct mesh_transport *transport, struct mesh_session *session);
    /* Optional. Pops the last connection failure as a line fit to show the user, or returns
       false when there is none pending. A link's connect can fail long after connect() has
       returned 0 (BlueZ resolves services asynchronously), so the errno the caller got back
       is not enough to explain what went wrong; this is how that reaches the UI. One-shot:
       the message is cleared by the read. */
    bool (*take_error)(struct mesh_transport *transport, char *out, size_t out_len);
};

/* Cap on a take_error() line. Sized to fit MESH_UI_NAV_TOAST_MAX without the transports
   having to include the UI headers. */
#define MESH_TRANSPORT_ERROR_MAX 64U

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
/* Hands `session` to every registered transport that supports it. Call before start_all(). */
void mesh_transport_registry_set_session(struct mesh_transport_registry *registry,
                                         struct mesh_session *session);
/* Pops the first pending connection failure from any registered transport into `out`.
   Returns true if one was taken. */
bool mesh_transport_registry_take_error(struct mesh_transport_registry *registry, char *out,
                                        size_t out_len);

#ifdef __cplusplus
}
#endif
