#pragma once

#include "inkwell/runtime/loop.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkcell_backend;
struct inkcell_focus_map;
struct inkcell_surface;

/*
 * The half of the controller that never knew what it was presenting.
 *
 * A store publishes on change and wakes the loop; this drains it into a snapshot, hands the
 * snapshot to a backend's present(), and arms a frame timer only while the backend says the frame
 * it just drew is still moving - so a screen sitting still costs no wake-ups at all. None of that
 * reads the snapshot. It needs somewhere to put one, a descriptor that says one is waiting, and a
 * callback that fills it, which is the whole of `struct mesh_ui_frame_config` below.
 *
 * What a press *means* is not here. The controller built on this resolves keys, commands and
 * clicks against the snapshot it presented, and that half is the application's.
 */

/* Fills `snapshot` with what the store has published since the last drain. True when it did;
   false leaves `snapshot` as the last frame drew it. */
typedef bool (*mesh_ui_frame_drain_fn)(void *userdata, void *snapshot);

/* Called on a timer frame that found nothing new, before the last snapshot is presented again:
   the place to clear whatever the snapshot says *changed*, because this frame changed nothing. */
typedef void (*mesh_ui_frame_unchanged_fn)(void *userdata, void *snapshot);

/* Asks the store to publish once even though nothing has changed. */
typedef void (*mesh_ui_frame_refresh_fn)(void *userdata);

struct mesh_ui_frame_config {
    struct inkwell_loop *loop;
    /* Optional. A backend whose init() refuses is dropped, and the scheduler carries on
       drawing nothing - see mesh_ui_frame_scheduler_has_backend(). */
    const struct inkcell_backend *backend;
    void *backend_userdata;
    /* Where drained snapshots are kept. Owned by the caller and sized by it; the scheduler only
       passes the pointer to drain() and present(). */
    void *snapshot;
    /* Readable when the store has something to publish. -1 for none. */
    int wake_fd;
    mesh_ui_frame_drain_fn drain;
    mesh_ui_frame_unchanged_fn unchanged; /* optional */
    mesh_ui_frame_refresh_fn refresh;     /* optional */
    void *source_userdata;
    /*
     * How long a moving backend waits for its next frame. A ceiling rather than a heartbeat: the
     * timer is armed only while the backend reports it is still moving. The number is the
     * caller's - how fine a panel can show motion is a fact about the panel.
     */
    uint32_t interval_ms;
};

struct mesh_ui_frame_scheduler {
    struct mesh_ui_frame_config config;
    const struct inkcell_backend *backend;
    void *backend_state;
    void *backend_userdata;
    /* Whether `config.snapshot` holds something drain() filled - false until the first. */
    bool presented;
    bool registered;
    /* Armed after a frame the backend says is still moving, disarmed the moment it settles.
       -1 when the timer could not be created, which costs animation and nothing else. */
    int timer_fd;
    /* Whether that timer holds a deadline that has not fired - what lets a request for a
       frame join one already on its way rather than push it back. */
    bool armed;
};

int mesh_ui_frame_scheduler_init(struct mesh_ui_frame_scheduler *scheduler,
                                 const struct mesh_ui_frame_config *config);
void mesh_ui_frame_scheduler_shutdown(struct mesh_ui_frame_scheduler *scheduler);

/*
 * Whether the backend named at init actually opened.
 *
 * A backend whose init() refused is dropped and the scheduler carries on without one, drawing
 * nothing - which is the right answer for a run that has no UI to speak of and the wrong one
 * for a program that had another backend it could have used instead. init() returning 0
 * therefore does not mean there is a panel, and this is the question that does.
 */
bool mesh_ui_frame_scheduler_has_backend(const struct mesh_ui_frame_scheduler *scheduler);

/* The name of the backend that opened, for a screen that says what is drawing it. NULL with
   none. */
const char *mesh_ui_frame_scheduler_backend_name(const struct mesh_ui_frame_scheduler *scheduler);

/* Whether a snapshot has been drained and presented at least once - what a press resolved
   against "the frame the reader saw" needs before there is one. */
bool mesh_ui_frame_scheduler_presented(const struct mesh_ui_frame_scheduler *scheduler);

/* One more frame of the last snapshot, for a backend whose window changed under it. A frame
   already on its way is the frame asked for, and is left where it is. */
void mesh_ui_frame_scheduler_request_frame(struct mesh_ui_frame_scheduler *scheduler);

/*
 * Whether the panel has stopped moving: no frame armed, so the last one presented is the one that
 * will stay up. What something taking a picture waits for - a slide caught halfway is not a
 * picture of the screen it was going to.
 */
bool mesh_ui_frame_scheduler_settled(const struct mesh_ui_frame_scheduler *scheduler);

/* The last frame presented, from the backend's `frame` hook. False with no frame yet, or with a
   backend that draws no pixels. Good until the next present. */
bool mesh_ui_frame_scheduler_frame(const struct mesh_ui_frame_scheduler *scheduler,
                                   struct inkcell_surface *out);

/*
 * Two facts about the frame on the panel now, read back rather than guessed at: how many rows
 * its paged list had room for, and the boxes it registered. A press between two frames is
 * answered against both. False when the backend has no such hook, which leaves the caller to
 * whatever it did before.
 */
bool mesh_ui_frame_scheduler_page_rows(const struct mesh_ui_frame_scheduler *scheduler,
                                       uint32_t *rows);
bool mesh_ui_frame_scheduler_focus_map(const struct mesh_ui_frame_scheduler *scheduler,
                                       const struct inkcell_focus_map **map);

#ifdef __cplusplus
}
#endif
