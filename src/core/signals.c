#define _POSIX_C_SOURCE 200809L

#include "mesh/signals.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"

#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

/* The signal mask is process-wide, so the saved copy is file-static rather than per-instance.
   Keeping sigset_t out of signals.h also spares every includer the POSIX feature macros. */
static sigset_t s_saved_mask;
static bool s_mask_saved;

static void mesh_signals_restore_mask(void) {
    if (!s_mask_saved) {
        return;
    }
    if (sigprocmask(SIG_SETMASK, &s_saved_mask, NULL) < 0) {
        mesh_log_warn("signals", "Failed to restore signal mask: %s", strerror(errno));
    }
    s_mask_saved = false;
}

static int mesh_signals_event_callback(int fd, uint32_t events, void *userdata) {
    struct mesh_signals *signals = (struct mesh_signals *)userdata;
    if (signals == NULL || (events & EPOLLIN) == 0U) {
        return 0;
    }

    struct signalfd_siginfo info;
    for (;;) {
        const ssize_t bytes = read(fd, &info, sizeof info);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                mesh_log_warn("signals", "signalfd read failed: %s", strerror(errno));
            }
            break;
        }
        if (bytes != (ssize_t)sizeof info) {
            break;
        }

        mesh_log_info("signals", "Received signal %u; shutting down", info.ssi_signo);
        mesh_event_loop_request_stop(signals->loop);
        break;
    }

    return 0;
}

int mesh_signals_init(struct mesh_signals *signals, struct mesh_event_loop *loop) {
    if (signals == NULL || loop == NULL) {
        return -EINVAL;
    }

    signals->loop = loop;
    signals->fd = -1;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);

    /* The signals must be blocked for signalfd to receive them rather than the default
       disposition running first. Every failure path below has to put the mask back: leaving
       them blocked with no signalfd to read them would make the process ignore SIGTERM. */
    if (sigprocmask(SIG_BLOCK, &mask, &s_saved_mask) < 0) {
        mesh_log_warn("signals", "sigprocmask failed: %s", strerror(errno));
        return -errno;
    }
    s_mask_saved = true;

    signals->fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signals->fd < 0) {
        mesh_log_warn("signals", "signalfd failed: %s", strerror(errno));
        const int saved_errno = errno;
        mesh_signals_restore_mask();
        return -saved_errno;
    }

    const int add_result =
        mesh_event_loop_add_fd(loop, signals->fd, EPOLLIN, mesh_signals_event_callback, signals);
    if (add_result < 0) {
        mesh_log_warn("signals", "Failed to watch signalfd: %d", add_result);
        close(signals->fd);
        signals->fd = -1;
        mesh_signals_restore_mask();
        return add_result;
    }

    return 0;
}

void mesh_signals_shutdown(struct mesh_signals *signals) {
    if (signals == NULL) {
        return;
    }

    if (signals->fd >= 0) {
        if (signals->loop != NULL) {
            mesh_event_loop_remove_fd(signals->loop, signals->fd);
        }
        close(signals->fd);
        signals->fd = -1;
    }

    mesh_signals_restore_mask();
    signals->loop = NULL;
}
