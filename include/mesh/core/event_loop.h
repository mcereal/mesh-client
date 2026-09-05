#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*mesh_event_callback)(int fd, uint32_t events, void *userdata);

#define MESH_EVENT_LOOP_MAX_SOURCES 32

struct mesh_event_source {
    int fd;
    uint32_t events;
    mesh_event_callback callback;
    void *userdata;
    bool active;
};

struct mesh_event_loop {
    int epoll_fd;
    int wake_fd;
    bool running;
    bool stop_requested;
    struct mesh_event_source sources[MESH_EVENT_LOOP_MAX_SOURCES];
};

int mesh_event_loop_init(struct mesh_event_loop *loop);
void mesh_event_loop_shutdown(struct mesh_event_loop *loop);

int mesh_event_loop_add_fd(struct mesh_event_loop *loop, int fd, uint32_t events,
                           mesh_event_callback callback, void *userdata);
int mesh_event_loop_update_fd(struct mesh_event_loop *loop, int fd, uint32_t events);
int mesh_event_loop_remove_fd(struct mesh_event_loop *loop, int fd);

int mesh_event_loop_run(struct mesh_event_loop *loop, int timeout_ms);
void mesh_event_loop_request_stop(struct mesh_event_loop *loop);

#ifdef __cplusplus
}
#endif
