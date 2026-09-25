#ifndef MESH_UI_TOAST_H
#define MESH_UI_TOAST_H

/*
 * A snackbar's notices: the one line on screen, how long it stands, and what is waiting behind it.
 *
 * One snackbar and one notice at a time, standing for a few seconds. Everything interesting is
 * about what happens when two things want it at once, and the answer depends on who asked:
 *
 *   - **A press replaces.** A notice raised by a press is the program answering the button that
 *     was just pressed, and what it replaces is usually the earlier half of the same story -
 *     "connecting" giving way to "refused" is one sentence finishing, not two events, and making
 *     the reader watch the optimistic half before the true one is worse than losing it.
 *
 *   - **An arrival waits.** A notice about something that *arrived* is news nobody asked for.
 *     Overwriting the answer to a press with it is how a button comes to look as though it did
 *     nothing, so it waits its turn - behind the others already waiting, which is what stops a
 *     burst of arrivals showing only whichever came last.
 *
 *   - **A press dismisses, and the next one waiting takes the snackbar.** Only the one showing has
 *     been seen; what is queued behind it is news the press did not answer.
 *
 * And one decision about time: **a notice can be raised undated.** Whoever handles a press may
 * have no clock to hand, and the drivers of one program need not share one - a device ticks with
 * a monotonic clock, a capture harness with a synthetic one that moves only when a scene says so.
 * A deadline read from the wrong clock means "four seconds" on one and "never" on the other. So a
 * press raises its notice undated, and whoever ticks the frames dates it from their own clock
 * before anything is drawn.
 *
 * The struct is a plain value - no pointers - so a snapshot of whatever holds it is a copy. Nothing
 * allocates, and the text is the application's, already translated.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest notice, terminator included. A longer one is cut to fit. */
#define MESH_UI_TOAST_TEXT_MAX 64U

/*
 * Notices waiting behind the one on screen.
 *
 * With one snackbar standing four seconds, a second notice used to overwrite the first and the
 * reader saw one of them. Three is twelve seconds of backlog at the far end: long enough that
 * nothing in a burst is simply lost, short enough that a notice is still about something that just
 * happened. A fourth would be news sixteen seconds old.
 */
#define MESH_UI_TOAST_QUEUE 3U

/* How long a notice stands, in the clock's milliseconds. */
#define MESH_UI_TOAST_STAND_MS 4000U

struct mesh_ui_toast {
    /* What is showing; empty when nothing is. */
    char text[MESH_UI_TOAST_TEXT_MAX];
    /* When it stops standing, or 0 while it is undated. A backend tells one notice from the next
       by this, which is why a promoted notice always gets a deadline of its own. */
    uint64_t until_ms;
    /*
     * What is waiting, oldest first, and undated by construction: a queued notice has not started
     * standing, and takes its deadline from the tick that promotes it - so the queue needs no
     * clock.
     *
     * Full means the *oldest waiting* one goes, never the newest. A backlog is only worth keeping
     * while it is still news, and a burst whose tail was dropped would show the three oldest
     * things that happened and silently withhold what happened last.
     */
    char queue[MESH_UI_TOAST_QUEUE][MESH_UI_TOAST_TEXT_MAX];
    uint8_t queued;
};

/* Nothing showing, nothing waiting. */
void mesh_ui_toast_init(struct mesh_ui_toast *toast);

/* A notice a press raised, dated from `now_ms`: it replaces what is showing and leaves what is
   waiting alone. NULL or empty clears the notice *and* the backlog - "say nothing" - since a queue
   that outlived it would start talking again a moment later. */
void mesh_ui_toast_set(struct mesh_ui_toast *toast, uint64_t now_ms, const char *text);

/* The same, raised undated, by whoever has no clock to hand. mesh_ui_toast_date() gives it its
   deadline, and must before the next tick: a tick retires an undated notice at once, since its
   deadline of 0 has already passed. */
void mesh_ui_toast_raise(struct mesh_ui_toast *toast, const char *text);

/* A notice something arriving raised: shown now if nothing is, and otherwise queued behind what is
   waiting. A repeat of what is showing, or of the newest thing waiting, is dropped - one event
   reported twice in a row is one notice, not two. */
void mesh_ui_toast_post(struct mesh_ui_toast *toast, uint64_t now_ms, const char *text);

/* Takes down what is showing, for a press, and puts up the next one waiting, undated. Returns true
   when anything was showing. */
bool mesh_ui_toast_dismiss(struct mesh_ui_toast *toast);

/* Dates an undated notice from `now_ms`. A no-op on a dated notice or on none. */
void mesh_ui_toast_date(struct mesh_ui_toast *toast, uint64_t now_ms);

/* Retires a notice whose time is up and promotes the next one waiting, dated from `now_ms`.
   Returns true when what is showing changed. */
bool mesh_ui_toast_tick(struct mesh_ui_toast *toast, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
