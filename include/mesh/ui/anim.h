#ifndef MESH_UI_ANIM_H
#define MESH_UI_ANIM_H

/*
 * Animation: a value moving from where it was to where it now is, over time.
 *
 * A frame in this client is a function of a snapshot, and a snapshot has no notion of "was".
 * A switch that slides, rather than jumping, needs exactly one thing the snapshot cannot
 * carry: the position it is coming from. That is all this module is - a start value, a target,
 * a start time and a duration, plus the arithmetic that turns a clock reading into a position
 * between them.
 *
 * It knows nothing about pixels, colours or the framebuffer. A widget asks for a progress
 * value from 0 to MESH_UI_ANIM_ONE and decides for itself what to do with it, the same way a
 * widget asks the theme for a tone rather than a colour. That is what lets the whole of the
 * timing behaviour be unit-tested with no display anywhere near it.
 *
 * Fixed point, not float. The Brick has an FPU, so this is not about speed - it is that a
 * position expressed in thousandths is exactly reproducible, which is what lets a test pin the
 * curve to numbers and a capture render the same frame twice.
 *
 * Where the state lives
 * ---------------------
 * In the *backend*, keyed by an id the screen supplies - not in the store and not in the nav
 * model. Where a knob has got to is not something the application knows or should be asked
 * about: it is presentation, it dies with the frame buffer it is drawn into, and a second
 * backend animating differently (or not at all) is a legitimate thing for a backend to decide.
 * The keyed table is `struct mesh_ui_anim_table` below - the immediate-mode trick, where the
 * caller passes identity in and the toolkit remembers the rest.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Progress is 0..1000, so a position reads as a permille and the arithmetic stays in 32 bits
 * with room to multiply by a pixel count.
 */
#define MESH_UI_ANIM_ONE 1000

/*
 * How a value gets from one end to the other.
 *
 * The names are the CSS ones because they are the names everybody already has for these
 * curves, and because a reader who has met them in a stylesheet knows what each one feels like
 * without reading the arithmetic.
 */
enum mesh_ui_ease {
    MESH_UI_EASE_LINEAR = 0, /* constant speed: what a progress bar wants */
    MESH_UI_EASE_OUT,        /* fast, then settling: what a control the user just pressed wants */
    MESH_UI_EASE_IN_OUT,     /* eases at both ends: what a thing moving of its own accord wants */
    MESH_UI_EASE_COUNT
};

/*
 * Applies a curve to a linear progress. Both in and out are 0..MESH_UI_ANIM_ONE, and both ends
 * are exact - ease(0) is 0 and ease(ONE) is ONE for every curve, so a finished animation lands
 * on its target rather than a rounding error away from it.
 */
int32_t mesh_ui_ease(enum mesh_ui_ease ease, int32_t progress);

/* One value in flight. Copyable; zeroed means "sitting at 0 with nothing to do". */
struct mesh_ui_anim {
    int32_t from;         /* where it started, 0..ONE */
    int32_t to;           /* where it is heading, 0..ONE */
    uint64_t start_ms;    /* when it left `from` */
    uint32_t duration_ms; /* 0 means it is already there */
    uint8_t ease;         /* enum mesh_ui_ease */
};

/*
 * Aims at `to`, starting from wherever it currently is.
 *
 * Reversing mid-flight is the case worth getting right: a switch flicked twice in quick
 * succession must turn round from where the knob actually is, not snap back to the end it came
 * from. Taking the current value as the new `from` is the whole of what makes that work.
 *
 * Re-aiming at the target it is already heading for is a no-op, so a widget can call this
 * every frame with the state it sees and only an actual change starts anything.
 */
void mesh_ui_anim_to(struct mesh_ui_anim *anim, uint64_t now_ms, int32_t to, uint32_t duration_ms,
                     enum mesh_ui_ease ease);

/* Jumps to `to` with nothing in flight. What a first paint does: a switch that animates on
   the frame a screen opens is a switch announcing itself rather than a change. */
void mesh_ui_anim_set(struct mesh_ui_anim *anim, int32_t to);

/* Eased position at `now_ms`, 0..MESH_UI_ANIM_ONE. Clamps outside the window, so a clock that
   jumped forwards (a capture stepping time, a device waking) lands on the target. */
int32_t mesh_ui_anim_value(const struct mesh_ui_anim *anim, uint64_t now_ms);

/* Whether it still has somewhere to be at `now_ms`. This is what asks for the next frame. */
bool mesh_ui_anim_active(const struct mesh_ui_anim *anim, uint64_t now_ms);

/*
 * A small table of animations kept by id.
 *
 * A screen renderer is rebuilt from scratch every frame and has nowhere of its own to keep
 * "what this row looked like last time", so the toolkit keeps it: the caller passes an id that
 * identifies the control - a settings field, a row index - and gets back the animation that
 * belongs to it, created on first sight.
 *
 * The size is what a frame can plausibly show at once. Beyond that the oldest-touched entry is
 * reused, which is the correct failure: the control that has been off screen longest loses its
 * memory of a slide nobody is watching, and re-seeds the next time it is drawn.
 */
#define MESH_UI_ANIM_SLOTS 12U

/*
 * How long after its last frame a *looping* value keeps asking to be redrawn.
 *
 * A loop never finishes, so unlike a transition it cannot say "I have arrived" - and a table
 * entry left behind by a widget that has scrolled off screen would otherwise pin the repaint
 * timer on for the rest of the run. Every frame that draws the widget touches its slot, so the
 * loop stays alive exactly as long as something is drawing it and stops one beat after nothing
 * is. Long enough to survive a frame the loop itself did not ask for, short enough that the
 * cost of being wrong is a single wasted repaint.
 */
#define MESH_UI_ANIM_LOOP_STALE_MS 250U

struct mesh_ui_anim_slot {
    uint32_t id; /* 0 means free */
    uint64_t touched_ms;
    struct mesh_ui_anim anim;
    /* A sawtooth rather than a transition: see mesh_ui_anim_loop(). The two are mutually
       exclusive on a slot, because they are two answers to "where is this value now" and an id
       identifies one control. */
    bool loop;
    uint32_t loop_period_ms;
    uint64_t loop_epoch_ms;
};

struct mesh_ui_anim_table {
    struct mesh_ui_anim_slot slots[MESH_UI_ANIM_SLOTS];
};

/*
 * The table's whole interface: "this control, with this id, is now at `to`; where should I
 * draw it?"
 *
 * On first sight of an id the value is adopted rather than animated to, so nothing slides on
 * the frame a screen opens. Afterwards a changed `to` starts a transition, and an unchanged
 * one just reports where the existing transition has reached.
 *
 * `id` of 0 is not a key - it reports the target unanimated - so a caller with nothing
 * meaningful to key on gets a correct static widget instead of one sharing a slot with
 * everything else that had nothing to key on.
 */
int32_t mesh_ui_anim_track(struct mesh_ui_anim_table *table, uint32_t id, uint64_t now_ms,
                           int32_t to, uint32_t duration_ms, enum mesh_ui_ease ease);

/*
 * A value that runs 0 -> MESH_UI_ANIM_ONE over `period_ms` and then starts again.
 *
 * Everything above answers "this value has changed, where is it on the way?". A loop answers a
 * different question, and it is the question an *indeterminate* progress bar asks: nothing has
 * changed, nothing is going to, and the widget still has to move to say that work is happening.
 * A spinner is the same idea in the same place.
 *
 * The position is derived from the clock modulo the period rather than accumulated, so a frame
 * the loop missed does not leave it behind and a capture stepping time in jumps lands exactly
 * where the arithmetic says. The epoch is taken on first sight of the id, which is what makes
 * the bar start at the left when it appears rather than wherever the monotonic clock happens
 * to be.
 *
 * Unlike a transition this is never "finished", so mesh_ui_anim_table_active() reports it as
 * running for MESH_UI_ANIM_LOOP_STALE_MS after the last frame that drew it - see there.
 *
 * `id` of 0 is not a key, exactly as above: it reports 0 and animates nothing.
 */
int32_t mesh_ui_anim_loop(struct mesh_ui_anim_table *table, uint32_t id, uint64_t now_ms,
                          uint32_t period_ms);

/* Whether anything in the table is still moving - what a backend answers when the event loop
   asks whether it needs waking again. */
bool mesh_ui_anim_table_active(const struct mesh_ui_anim_table *table, uint64_t now_ms);

/* Forgets everything. A theme or scale change re-measures every widget, so the positions in
   here describe geometry that no longer exists. */
void mesh_ui_anim_table_reset(struct mesh_ui_anim_table *table);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_ANIM_H */
