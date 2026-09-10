#include "mesh/ui/status.h"

#include <string.h>

/*
 * The table.
 *
 * Written out as a table rather than composed, for the reason the action-bar tables in
 * src/ui/actions.c are: what a screen offers in a given state should be readable in one place
 * and checkable against the nav that handles the press.
 *
 * **The order is the order the cards draw**, and that is the whole of what the cursor walks -
 * Link, then Mesh, then Radio. It reads as the obvious arrangement and it was not reachable
 * until the cursor stopped being an index: the trend arrived last and had to be appended, so
 * Down walked Link, Radio, Mesh and the highlight went to the bottom card and then back up to
 * the middle one. Now that the cursor remembers a verb, where a verb sits in this list is a
 * question about reading order and nothing else. A verb added here goes where its card is.
 *
 * **A row states its own condition and nothing else's**, which is the other half of the same
 * change. Disconnect needs a link to drop. Refresh needs one because a refresh is a request
 * over the air - mesh_session_refresh_settings() answers -ENOTCONN without one and the app
 * toasts "not connected" - and it needs the handshake on top of that, because re-reading a
 * configuration nothing has sent yet reports nothing and reads as broken. The trend needs
 * neither: what it opens is a picture of what this client already watched, and the history
 * outlives the radio going away.
 *
 * That last line used to read `synced && has_trend`, and the two redundant conditions were
 * load-bearing for the *list* rather than true of the press: with the cursor an index, a verb
 * whose condition did not imply the conditions of the verbs before it could slide in ahead of
 * a parked cursor and change what A did without the cursor moving. Restating them was the
 * cheapest way to make that checkable by reading this function. The cursor is a verb now, so
 * the honest condition is also the safe one.
 *
 * The Mesh card offers one verb and for most of this screen's life it offered none. What
 * changed is that the card acquired a picture: the airtime bar is a reading somebody can now
 * want to see properly, and a chart is a screen rather than a row. Every *other* verb about the
 * mesh still belongs somewhere else - trace a node, ask it for a fix, or one of the destructive
 * ones that wants the confirmation dialog and is tied to a settings section. A card with no
 * verb is simply skipped by the cursor, which is what makes the flat list work.
 */

/* What a row needs to be true before it is offered, as the three facts the caller holds. */
enum status_need {
    STATUS_NEED_LINK = 1U << 0,  /* a radio is attached */
    STATUS_NEED_SYNC = 1U << 1,  /* it has answered the config handshake */
    STATUS_NEED_TREND = 1U << 2, /* there is a line to draw */
};

struct status_entry {
    uint8_t card; /* enum mesh_ui_status_card */
    uint8_t verb; /* enum mesh_ui_status_verb */
    enum mesh_str_id label;
    uint8_t needs; /* enum status_need, ORed */
};

static const struct status_entry k_status_verbs[] = {
    {(uint8_t)MESH_UI_STATUS_CARD_LINK, (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT,
     MESH_STR_ACTION_DISCONNECT, STATUS_NEED_LINK},
    {(uint8_t)MESH_UI_STATUS_CARD_MESH, (uint8_t)MESH_UI_STATUS_VERB_TREND, MESH_STR_ACTION_TREND,
     STATUS_NEED_TREND},
    {(uint8_t)MESH_UI_STATUS_CARD_RADIO, (uint8_t)MESH_UI_STATUS_VERB_REFRESH,
     MESH_STR_ACTION_REFRESH, STATUS_NEED_LINK | STATUS_NEED_SYNC},
};

#define STATUS_VERB_TABLE_COUNT (sizeof k_status_verbs / sizeof k_status_verbs[0])

/*
 * Where a verb sits in the table, whether or not it is currently offered.
 *
 * This is the order the cursor walks and the order a shrinking list is repaired against, so it
 * has to be answerable for a verb that has just gone - which is exactly the question the list
 * on offer cannot answer. A verb that is in no table ranks past the end, so a cursor holding
 * one lands on the last verb rather than the first.
 */
static uint32_t status_rank(uint8_t verb) {
    for (uint32_t i = 0U; i < STATUS_VERB_TABLE_COUNT; ++i) {
        if (k_status_verbs[i].verb == verb) {
            return i;
        }
    }
    return (uint32_t)STATUS_VERB_TABLE_COUNT;
}

void mesh_ui_status_actions(struct mesh_ui_status_actions *out, bool connected, bool synced,
                            bool has_trend) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);

    uint8_t have = 0U;
    have |= connected ? (uint8_t)STATUS_NEED_LINK : 0U;
    have |= synced ? (uint8_t)STATUS_NEED_SYNC : 0U;
    have |= has_trend ? (uint8_t)STATUS_NEED_TREND : 0U;

    for (uint32_t i = 0U; i < STATUS_VERB_TABLE_COUNT; ++i) {
        const struct status_entry *entry = &k_status_verbs[i];
        if ((entry->needs & have) != entry->needs) {
            continue;
        }
        if (out->count >= MESH_UI_STATUS_ACTIONS_MAX) {
            break;
        }
        out->items[out->count].card = entry->card;
        out->items[out->count].verb = entry->verb;
        out->items[out->count].label = entry->label;
        ++out->count;
    }
}

const struct mesh_ui_status_action *
mesh_ui_status_find(const struct mesh_ui_status_actions *actions, uint8_t verb) {
    if (actions == NULL) {
        return NULL;
    }
    for (uint32_t i = 0U; i < actions->count && i < MESH_UI_STATUS_ACTIONS_MAX; ++i) {
        if (actions->items[i].verb == verb) {
            return &actions->items[i];
        }
    }
    return NULL;
}

uint8_t mesh_ui_status_verb_resolve(const struct mesh_ui_status_actions *actions, uint8_t verb) {
    if (actions == NULL || actions->count == 0U) {
        return verb;
    }
    if (mesh_ui_status_find(actions, verb) != NULL) {
        return verb;
    }
    /* The list is a subsequence of the table, so the last entry ranking before the missing verb
       is the nearest one above it on the screen. Nothing before it means the cursor was on the
       first verb of a list that has lost it, and the first survivor is where it belongs. */
    const uint32_t rank = status_rank(verb);
    uint8_t nearest = actions->items[0].verb;
    for (uint32_t i = 0U; i < actions->count && i < MESH_UI_STATUS_ACTIONS_MAX; ++i) {
        if (status_rank(actions->items[i].verb) >= rank) {
            break;
        }
        nearest = actions->items[i].verb;
    }
    return nearest;
}

uint8_t mesh_ui_status_verb_step(const struct mesh_ui_status_actions *actions, uint8_t verb,
                                 int delta) {
    if (actions == NULL || actions->count == 0U || delta == 0) {
        return verb;
    }
    /* From wherever the cursor actually stands, which is not necessarily where it remembers
       standing: a verb can have gone since the last frame, and a press should move from the
       button the reader can see highlighted. */
    const uint8_t from = mesh_ui_status_verb_resolve(actions, verb);
    for (uint32_t i = 0U; i < actions->count && i < MESH_UI_STATUS_ACTIONS_MAX; ++i) {
        if (actions->items[i].verb != from) {
            continue;
        }
        if (delta < 0) {
            return i == 0U ? from : actions->items[i - 1U].verb;
        }
        return (i + 1U >= actions->count) ? from : actions->items[i + 1U].verb;
    }
    return from;
}

uint32_t mesh_ui_status_card_actions(const struct mesh_ui_status_actions *actions,
                                     enum mesh_ui_status_card card, uint32_t *out_first) {
    uint32_t first = 0U;
    uint32_t count = 0U;
    if (actions != NULL) {
        for (uint32_t i = 0U; i < actions->count && i < MESH_UI_STATUS_ACTIONS_MAX; ++i) {
            if (actions->items[i].card != (uint8_t)card) {
                continue;
            }
            if (count == 0U) {
                first = i;
            }
            ++count;
        }
    }
    if (out_first != NULL) {
        *out_first = first;
    }
    return count;
}
