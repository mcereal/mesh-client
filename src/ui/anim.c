#include "mesh/ui/anim.h"

#include <string.h>

/*
 * The curves.
 *
 * Each is the standard easing polynomial evaluated in fixed point. `t` is 0..ONE, and every
 * intermediate is kept in int64_t because a cubic of a four-digit number is twelve digits
 * before it is scaled back down - which is a full 32-bit overflow away from being right.
 *
 * If the algebra is unfamiliar: linear is a straight line, ease-out is that line bent so most
 * of the distance is covered early and the last stretch is a settle, and ease-in-out is
 * symmetric - slow, quick, slow. Bending the line is the whole difference between a control
 * that feels mechanical and one that feels answered.
 */
static int32_t ease_out_cubic(int32_t t) {
    /* 1 - (1 - t)^3 */
    const int64_t inv = MESH_UI_ANIM_ONE - t;
    const int64_t cubed = inv * inv * inv;
    return (int32_t)(MESH_UI_ANIM_ONE -
                     cubed / ((int64_t)MESH_UI_ANIM_ONE * (int64_t)MESH_UI_ANIM_ONE));
}

static int32_t ease_in_out_cubic(int32_t t) {
    if (t < MESH_UI_ANIM_ONE / 2) {
        /* 4t^3 */
        const int64_t cubed = (int64_t)t * t * t;
        return (int32_t)(4 * cubed / ((int64_t)MESH_UI_ANIM_ONE * (int64_t)MESH_UI_ANIM_ONE));
    }
    /* 1 - (-2t + 2)^3 / 2 */
    const int64_t inv = 2 * (int64_t)(MESH_UI_ANIM_ONE - t);
    const int64_t cubed = inv * inv * inv;
    return (int32_t)(MESH_UI_ANIM_ONE -
                     cubed / (2 * (int64_t)MESH_UI_ANIM_ONE * (int64_t)MESH_UI_ANIM_ONE));
}

static int32_t clamp_progress(int32_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > MESH_UI_ANIM_ONE) {
        return MESH_UI_ANIM_ONE;
    }
    return value;
}

int32_t mesh_ui_ease(enum mesh_ui_ease ease, int32_t progress) {
    const int32_t t = clamp_progress(progress);
    /* Both ends exactly, whatever the arithmetic below would round to. A switch that stops one
       permille short of its target is a switch drawn a pixel out for the rest of the frame's
       life, because nothing will wake to correct it. */
    if (t == 0 || t == MESH_UI_ANIM_ONE) {
        return t;
    }
    switch (ease) {
    case MESH_UI_EASE_OUT:
        return clamp_progress(ease_out_cubic(t));
    case MESH_UI_EASE_IN_OUT:
        return clamp_progress(ease_in_out_cubic(t));
    case MESH_UI_EASE_LINEAR:
    case MESH_UI_EASE_COUNT:
    default:
        return t;
    }
}

int32_t mesh_ui_anim_value(const struct mesh_ui_anim *anim, uint64_t now_ms) {
    if (anim == NULL) {
        return 0;
    }
    if (anim->duration_ms == 0U) {
        return clamp_progress(anim->to); /* nothing in flight: it is wherever it was put */
    }
    if (now_ms <= anim->start_ms) {
        return clamp_progress(anim->from);
    }

    const uint64_t elapsed = now_ms - anim->start_ms;
    if (elapsed >= (uint64_t)anim->duration_ms) {
        return clamp_progress(anim->to);
    }

    const int32_t linear = (int32_t)((elapsed * (uint64_t)MESH_UI_ANIM_ONE) / anim->duration_ms);
    const int32_t eased = mesh_ui_ease((enum mesh_ui_ease)anim->ease, linear);
    const int32_t span = anim->to - anim->from;
    return clamp_progress(anim->from + (int32_t)(((int64_t)span * eased) / MESH_UI_ANIM_ONE));
}

bool mesh_ui_anim_active(const struct mesh_ui_anim *anim, uint64_t now_ms) {
    if (anim == NULL || anim->duration_ms == 0U || anim->from == anim->to) {
        return false;
    }
    return now_ms < anim->start_ms + (uint64_t)anim->duration_ms;
}

void mesh_ui_anim_set(struct mesh_ui_anim *anim, int32_t to) {
    if (anim == NULL) {
        return;
    }
    memset(anim, 0, sizeof *anim);
    anim->from = clamp_progress(to);
    anim->to = anim->from;
}

void mesh_ui_anim_to(struct mesh_ui_anim *anim, uint64_t now_ms, int32_t to, uint32_t duration_ms,
                     enum mesh_ui_ease ease) {
    if (anim == NULL) {
        return;
    }
    const int32_t target = clamp_progress(to);
    if (anim->to == target) {
        /* Already there or already on the way. This is the common case by far: a widget calls
           this every frame with the state it can see, and only a change starts anything. */
        return;
    }
    if (duration_ms == 0U) {
        mesh_ui_anim_set(anim, target);
        return;
    }

    /* From where it actually is, which for a reversal mid-flight is somewhere in the middle.
       Sampling the current value before overwriting the window is the whole trick. */
    anim->from = mesh_ui_anim_value(anim, now_ms);
    anim->to = target;
    anim->start_ms = now_ms;
    anim->duration_ms = duration_ms;
    anim->ease = (uint8_t)(ease < MESH_UI_EASE_COUNT ? ease : MESH_UI_EASE_LINEAR);
}

/* ---- the keyed table --------------------------------------------------------------------- */

static struct mesh_ui_anim_slot *table_slot(struct mesh_ui_anim_table *table, uint32_t id,
                                            uint64_t now_ms, bool *created) {
    struct mesh_ui_anim_slot *free_slot = NULL;
    struct mesh_ui_anim_slot *oldest = &table->slots[0];

    for (size_t i = 0; i < MESH_UI_ANIM_SLOTS; ++i) {
        struct mesh_ui_anim_slot *slot = &table->slots[i];
        if (slot->id == id) {
            slot->touched_ms = now_ms;
            *created = false;
            return slot;
        }
        if (slot->id == 0U && free_slot == NULL) {
            free_slot = slot;
        }
        if (slot->touched_ms < oldest->touched_ms) {
            oldest = slot;
        }
    }

    struct mesh_ui_anim_slot *slot = free_slot != NULL ? free_slot : oldest;
    memset(slot, 0, sizeof *slot);
    slot->id = id;
    slot->touched_ms = now_ms;
    *created = true;
    return slot;
}

int32_t mesh_ui_anim_track(struct mesh_ui_anim_table *table, uint32_t id, uint64_t now_ms,
                           int32_t to, uint32_t duration_ms, enum mesh_ui_ease ease) {
    if (table == NULL || id == 0U) {
        return clamp_progress(to);
    }

    bool created = false;
    struct mesh_ui_anim_slot *slot = table_slot(table, id, now_ms, &created);
    /* An id that was looping and is now being tracked is a widget that changed its mind about
       which question it is asking - an indeterminate meter that learned its total, which is
       exactly what the updater does when it starts reading bytes. Adopt the target rather than
       transitioning from a sawtooth position that meant something else. */
    if (created || slot->loop) {
        slot->loop = false;
        mesh_ui_anim_set(&slot->anim, to);
    } else {
        mesh_ui_anim_to(&slot->anim, now_ms, to, duration_ms, ease);
    }
    return mesh_ui_anim_value(&slot->anim, now_ms);
}

int32_t mesh_ui_anim_loop(struct mesh_ui_anim_table *table, uint32_t id, uint64_t now_ms,
                          uint32_t period_ms) {
    if (table == NULL || id == 0U || period_ms == 0U) {
        return 0;
    }

    bool created = false;
    struct mesh_ui_anim_slot *slot = table_slot(table, id, now_ms, &created);
    /* Fresh, or a slot that was carrying a transition for this id: either way the loop starts
       from its own beginning rather than from wherever it would have been had it always been
       running. A bar that appears mid-stride reads as one that was already there. */
    if (created || !slot->loop || slot->loop_period_ms != period_ms) {
        memset(&slot->anim, 0, sizeof slot->anim);
        slot->loop = true;
        slot->loop_period_ms = period_ms;
        slot->loop_epoch_ms = now_ms;
    }

    /* Modulo the period rather than accumulated: a frame that never happened costs nothing, and
       a capture that steps the clock by a second lands where the arithmetic says it should. */
    const uint64_t elapsed = now_ms > slot->loop_epoch_ms ? now_ms - slot->loop_epoch_ms : 0U;
    return (int32_t)((elapsed % (uint64_t)period_ms) * (uint64_t)MESH_UI_ANIM_ONE /
                     (uint64_t)period_ms);
}

bool mesh_ui_anim_table_active(const struct mesh_ui_anim_table *table, uint64_t now_ms) {
    if (table == NULL) {
        return false;
    }
    for (size_t i = 0; i < MESH_UI_ANIM_SLOTS; ++i) {
        const struct mesh_ui_anim_slot *slot = &table->slots[i];
        if (slot->id == 0U) {
            continue;
        }
        /* A loop has no end to reach, so what keeps it running is that something is still
           drawing it - and what stops it is that nothing has for a beat. See
           MESH_UI_ANIM_LOOP_STALE_MS. */
        if (slot->loop) {
            if (now_ms <= slot->touched_ms + (uint64_t)MESH_UI_ANIM_LOOP_STALE_MS) {
                return true;
            }
            continue;
        }
        if (mesh_ui_anim_active(&slot->anim, now_ms)) {
            return true;
        }
    }
    return false;
}

void mesh_ui_anim_table_reset(struct mesh_ui_anim_table *table) {
    if (table != NULL) {
        memset(table, 0, sizeof *table);
    }
}
