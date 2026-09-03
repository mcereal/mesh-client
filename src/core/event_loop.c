#include "mesh/event_loop.h"

#include "mesh/log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int find_source_index(const struct mesh_event_loop *loop, int fd) {
    for (int i = 0; i < MESH_EVENT_LOOP_MAX_SOURCES; ++i) {
        if (loop->sources[i].active && loop->sources[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

static int wake_callback(int fd, uint32_t events, void *userdata) {
    (void)events;
    struct mesh_event_loop *loop = (struct mesh_event_loop *)userdata;
    uint64_t value = 0;
    ssize_t result = read(fd, &value, sizeof value);
    if (result < 0 && errno != EAGAIN) {
        mesh_log_warn("loop", "wake read failed: %s", strerror(errno));
    }
    loop->running = false;
    loop->stop_requested = true;
    return 0;
}

int mesh_event_loop_init(struct mesh_event_loop *loop) {
    if (loop == NULL) {
        return -EINVAL;
    }

    memset(loop, 0, sizeof *loop);
    loop->epoll_fd = -1;
    loop->wake_fd = -1;

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        mesh_log_error("loop", "epoll_create1 failed: %s", strerror(errno));
        return -errno;
    }

    loop->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wake_fd < 0) {
        mesh_log_error("loop", "eventfd failed: %s", strerror(errno));
        close(loop->epoll_fd);
        loop->epoll_fd = -1;
        return -errno;
    }

    int result = mesh_event_loop_add_fd(loop, loop->wake_fd, EPOLLIN, wake_callback, loop);
    if (result < 0) {
        mesh_log_error("loop", "Failed to register wake FD: %d", result);
        close(loop->wake_fd);
        close(loop->epoll_fd);
        loop->wake_fd = -1;
        loop->epoll_fd = -1;
        return result;
    }

    loop->running = false;
    loop->stop_requested = false;

    return 0;
}

void mesh_event_loop_shutdown(struct mesh_event_loop *loop) {
    if (loop == NULL) {
        return;
    }

    for (int i = 0; i < MESH_EVENT_LOOP_MAX_SOURCES; ++i) {
        if (loop->sources[i].active) {
            mesh_event_loop_remove_fd(loop, loop->sources[i].fd);
        }
    }

    if (loop->wake_fd >= 0) {
        close(loop->wake_fd);
        loop->wake_fd = -1;
    }

    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
        loop->epoll_fd = -1;
    }

    loop->running = false;
    loop->stop_requested = false;
}

int mesh_event_loop_add_fd(struct mesh_event_loop *loop, int fd, uint32_t events,
                           mesh_event_callback callback, void *userdata) {
    if (loop == NULL || fd < 0 || callback == NULL) {
        return -EINVAL;
    }

    if (find_source_index(loop, fd) >= 0) {
        return -EEXIST;
    }

    int slot = -1;
    for (int i = 0; i < MESH_EVENT_LOOP_MAX_SOURCES; ++i) {
        if (!loop->sources[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return -ENOSPC;
    }

    struct mesh_event_source *source = &loop->sources[slot];
    source->fd = fd;
    source->events = events;
    source->callback = callback;
    source->userdata = userdata;
    source->active = true;

    struct epoll_event event;
    event.events = events;
    event.data.ptr = source;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        int error = -errno;
        source->active = false;
        mesh_log_error("loop", "epoll_ctl add failed: %s", strerror(errno));
        return error;
    }

    return 0;
}

int mesh_event_loop_update_fd(struct mesh_event_loop *loop, int fd, uint32_t events) {
    if (loop == NULL) {
        return -EINVAL;
    }

    int index = find_source_index(loop, fd);
    if (index < 0) {
        return -ENOENT;
    }

    struct mesh_event_source *source = &loop->sources[index];
    source->events = events;

    struct epoll_event event;
    event.events = events;
    event.data.ptr = source;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
        mesh_log_error("loop", "epoll_ctl mod failed: %s", strerror(errno));
        return -errno;
    }

    return 0;
}

int mesh_event_loop_remove_fd(struct mesh_event_loop *loop, int fd) {
    if (loop == NULL) {
        return -EINVAL;
    }

    int index = find_source_index(loop, fd);
    if (index < 0) {
        return -ENOENT;
    }

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        mesh_log_error("loop", "epoll_ctl del failed: %s", strerror(errno));
        return -errno;
    }

    loop->sources[index].active = false;
    loop->sources[index].fd = -1;
    loop->sources[index].callback = NULL;
    loop->sources[index].userdata = NULL;
    loop->sources[index].events = 0;

    return 0;
}

int mesh_event_loop_run(struct mesh_event_loop *loop, int timeout_ms) {
    if (loop == NULL) {
        return -EINVAL;
    }

    loop->running = true;
    loop->stop_requested = false;

    struct epoll_event events[8];
    while (loop->running) {
        int ready =
            epoll_wait(loop->epoll_fd, events, (int)(sizeof events / sizeof events[0]), timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            mesh_log_error("loop", "epoll_wait failed: %s", strerror(errno));
            loop->running = false;
            return -errno;
        }

        if (ready == 0) {
            // Timeout without events; allow caller to regain control.
            break;
        }

        for (int i = 0; i < ready; ++i) {
            struct mesh_event_source *source = (struct mesh_event_source *)events[i].data.ptr;
            if (source == NULL || !source->active) {
                continue;
            }
            source->callback(source->fd, events[i].events, source->userdata);
        }
    }

    loop->running = false;
    return 0;
}

void mesh_event_loop_request_stop(struct mesh_event_loop *loop) {
    if (loop == NULL) {
        return;
    }

    loop->running = false;
    loop->stop_requested = true;
    if (loop->wake_fd >= 0) {
        const uint64_t value = 1;
        ssize_t written = write(loop->wake_fd, &value, sizeof value);
        if (written < 0 && errno != EAGAIN) {
            mesh_log_warn("loop", "wake write failed: %s", strerror(errno));
        }
    }
}
