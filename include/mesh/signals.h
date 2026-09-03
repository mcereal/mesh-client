#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* SIGINT/SIGTERM/SIGHUP delivered through a signalfd on the event loop, so a shutdown runs
   the normal path (preferences and the handshake cache get flushed) instead of the default
   kill action. Nothing here runs in signal context. */
struct mesh_signals {
    struct mesh_event_loop *loop;
    int fd;
};

int mesh_signals_init(struct mesh_signals *signals, struct mesh_event_loop *loop);
void mesh_signals_shutdown(struct mesh_signals *signals);

#ifdef __cplusplus
}
#endif
