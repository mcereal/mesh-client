#include "mesh/core/key_verification.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include <string.h>

/*
 * The ceremony as state. See the header for what the ceremony is; what follows is only the
 * bookkeeping, and the two rules worth stating here rather than there:
 *
 * - **Every arrival adopts.** A notification is the radio saying what it is doing *now*, and it
 *   runs one exchange at a time, so there is no case in which the thing already in this slot is
 *   more current than the thing arriving. That includes a nonce that does not match: it means
 *   the exchange this client thought it was in has been replaced, which is a fact to record
 *   rather than a message to drop.
 * - **Nothing here sends.** Every function moves the slot and says whether it moved; the
 *   session reads the result and queues the admin step. A state machine that also sent would be
 *   a state machine whose tests need a radio.
 */

/* Moves the stage and stamps it. Returns whether anything a reader can see changed, which is
   what every entry point hands back to its caller. */
static bool stage_set(struct mesh_key_verification *state, enum mesh_key_verification_stage stage,
                      uint32_t now) {
    const bool moved = state->stage != (uint8_t)stage;
    state->stage = (uint8_t)stage;
    state->changed = now;
    if (moved) {
        state->seq += 1U;
    }
    return moved;
}

/* The radio's own name for the far end, sanitised. An empty one is left empty rather than
   filled in from the roster: the point of the name on a verification sheet is that it is what
   the *other* user's screen is showing, and a name we supplied would be this client agreeing
   with itself. */
static void adopt_name(struct mesh_key_verification *state, const char *name) {
    if (name == NULL || name[0] == '\0') {
        return;
    }
    inkwell_text_sanitise_str(name, state->remote_name, sizeof state->remote_name);
}

void mesh_key_verification_reset(struct mesh_key_verification *state) {
    if (state == NULL) {
        return;
    }
    /* `seq` survives, because it is a counter for the life of the session rather than a field
       of the exchange: a reader watching it must not see it go backwards when one ends. */
    const uint32_t seq = state->seq;
    memset(state, 0, sizeof *state);
    state->stage = (uint8_t)MESH_KEY_VERIFICATION_IDLE;
    state->seq = seq;
}

bool mesh_key_verification_active(const struct mesh_key_verification *state) {
    return state != NULL && state->stage != (uint8_t)MESH_KEY_VERIFICATION_IDLE;
}

bool mesh_key_verification_asks(const struct mesh_key_verification *state) {
    if (state == NULL) {
        return false;
    }
    return state->stage == (uint8_t)MESH_KEY_VERIFICATION_ENTER_NUMBER ||
           state->stage == (uint8_t)MESH_KEY_VERIFICATION_COMPARE;
}

bool mesh_key_verification_begin(struct mesh_key_verification *state, uint32_t node_id,
                                 const char *name, uint32_t now) {
    if (state == NULL || node_id == 0U) {
        return false;
    }
    if (mesh_key_verification_active(state) && state->remote_node != node_id) {
        inkwell_log_info("verify", "Dropping the exchange with 0x%08x to start one with 0x%08x",
                         state->remote_node, node_id);
    }
    mesh_key_verification_reset(state);
    state->we_initiated = true;
    state->remote_node = node_id;
    state->started = now;
    /* The nonce is the radio's and does not exist yet: this is the one moment when an exchange
       is live here and has not begun on the wire. */
    state->nonce = 0U;
    adopt_name(state, name);
    (void)stage_set(state, MESH_KEY_VERIFICATION_WAITING, now);
    return true;
}

/* What the three arrivals have in common: adopt the exchange the radio is describing, keeping
   whatever the slot already knew that the notification does not carry. `initiated` is what this
   arrival proves about which end we are - a request for a number only reaches the end that
   started it, an inform only the end that did not. */
static void adopt(struct mesh_key_verification *state, uint64_t nonce, const char *name,
                  bool initiated, uint32_t now) {
    if (!mesh_key_verification_active(state) || state->nonce != nonce) {
        if (mesh_key_verification_active(state)) {
            inkwell_log_info("verify", "Exchange %llu replaces %llu (node 0x%08x)",
                             (unsigned long long)nonce, (unsigned long long)state->nonce,
                             state->remote_node);
        }
        /*
         * The node number is the one thing an arrival never carries: a notification names the
         * far end by its long name only. So whether the slot's node survives this is the whole
         * of what has to be got right here, and there are exactly two cases behind the one
         * condition above.
         *
         * A slot whose nonce is **0** is our own initiation, still waiting for the radio to
         * answer - the one moment an exchange exists here and not on the wire. The nonce
         * arriving is that same exchange acquiring one, so the node the user pressed on is
         * still the node, and losing it would leave the ceremony with nothing to address.
         *
         * A slot with a nonce is a *different* exchange being replaced, and its node must go
         * with it. Keeping it was a bug: the sheet would show the new peer's name while every
         * step went to the old peer's node, and a yes would mark the old node's key verified
         * from a code that belonged to somebody else - which is the exact failure this whole
         * feature exists to prevent. Clearing it hands the question back to the session, which
         * resolves the name against the roster (and refuses when it cannot).
         */
        const bool same_exchange_gaining_a_nonce = state->nonce == 0U;
        const uint32_t node = same_exchange_gaining_a_nonce ? state->remote_node : 0U;
        const uint32_t started = state->started;
        mesh_key_verification_reset(state);
        state->remote_node = node;
        state->started = started != 0U ? started : now;
        state->we_initiated = initiated;
    }
    state->nonce = nonce;
    adopt_name(state, name);
}

bool mesh_key_verification_on_number_request(struct mesh_key_verification *state, uint64_t nonce,
                                             const char *name, uint32_t now) {
    if (state == NULL) {
        return false;
    }
    /* Only the end that initiated is ever asked for the number, so this arrival settles which
       end we are even when the press that started it was lost with a reconnect. */
    adopt(state, nonce, name, true, now);
    state->we_initiated = true;
    return stage_set(state, MESH_KEY_VERIFICATION_ENTER_NUMBER, now);
}

bool mesh_key_verification_on_number_inform(struct mesh_key_verification *state, uint64_t nonce,
                                            const char *name, uint32_t security_number,
                                            uint32_t now) {
    if (state == NULL) {
        return false;
    }
    /* The mirror of the above: only the end that did *not* initiate generates the number. */
    adopt(state, nonce, name, false, now);
    state->we_initiated = false;
    state->security_number = security_number;
    return stage_set(state, MESH_KEY_VERIFICATION_SHOW_NUMBER, now);
}

bool mesh_key_verification_on_final(struct mesh_key_verification *state, uint64_t nonce,
                                    const char *name, const char *characters, uint32_t now) {
    if (state == NULL) {
        return false;
    }
    /* Both ends get this one, so it proves nothing about which we are and must not overwrite
       what an earlier arrival established. A cold final - the client reconnected mid-ceremony -
       adopts as a responder, which is the safer of the two: it is the end that answers rather
       than the end that is waiting for an answer. */
    const bool initiated = mesh_key_verification_active(state) ? state->we_initiated : false;
    adopt(state, nonce, name, initiated, now);
    inkwell_text_sanitise_str(characters, state->characters, sizeof state->characters);
    /* The number has been read out and typed by now; keeping it on the sheet beside the
       characters would be two things to compare where there is one. */
    state->security_number = 0U;
    return stage_set(state, MESH_KEY_VERIFICATION_COMPARE, now);
}

bool mesh_key_verification_touch(struct mesh_key_verification *state, uint32_t now) {
    if (state == NULL || !mesh_key_verification_active(state) || now == 0U) {
        return false;
    }
    state->changed = now;
    return true;
}

bool mesh_key_verification_settle(struct mesh_key_verification *state,
                                  struct mesh_key_verification *out) {
    if (state == NULL || !mesh_key_verification_active(state)) {
        return false;
    }
    if (out != NULL) {
        *out = *state;
    }
    mesh_key_verification_reset(state);
    state->seq += 1U; /* the sheet has to close, and a reader watching seq is how it knows */
    return true;
}

bool mesh_key_verification_tick(struct mesh_key_verification *state, uint32_t now,
                                struct mesh_key_verification *out) {
    if (state == NULL || !mesh_key_verification_active(state)) {
        return false;
    }
    /* Both clocks have to be real. `changed` is 0 on an exchange that moved while the client
       had no clock, and `now` is 0 on a client that still has none; either way five minutes is
       not a thing this client can measure, and closing the sheet on a guess would take the
       question away mid-answer. */
    if (now == 0U || state->changed == 0U || now < state->changed) {
        return false;
    }
    if (now - state->changed < MESH_KEY_VERIFICATION_TIMEOUT_SECONDS) {
        return false;
    }
    inkwell_log_info("verify", "Giving up on the exchange with 0x%08x after %us",
                     state->remote_node, (unsigned)(now - state->changed));
    return mesh_key_verification_settle(state, out);
}
