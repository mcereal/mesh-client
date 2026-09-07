#include "mesh/ui/status.h"

#include <string.h>

/*
 * The table.
 *
 * Written out per state rather than composed, for the reason the action-bar tables in
 * src/ui/actions.c are: what a screen offers in a given state should be readable in one place
 * and checkable against the nav that handles the press.
 *
 * Two conditions, and each is the one that makes its verb mean anything. Disconnect needs a
 * link to drop; a refresh needs a radio that has answered the handshake, because re-reading a
 * configuration nothing has sent yet is a press that reports nothing and looks broken.
 *
 * The Mesh card offers none, and it is not an omission. Every verb this client has about the
 * mesh either belongs to a node - trace it, ask it for a fix - or is destructive enough to want
 * the confirmation dialog, which is still tied to a settings section; both are somewhere else
 * on purpose. A card with no verb is simply skipped by the cursor, which is what makes the
 * flat list work.
 */
void mesh_ui_status_actions(struct mesh_ui_status_actions *out, bool connected, bool synced) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);

    if (connected) {
        out->items[out->count].card = (uint8_t)MESH_UI_STATUS_CARD_LINK;
        out->items[out->count].verb = (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT;
        out->items[out->count].label = MESH_STR_ACTION_DISCONNECT;
        ++out->count;
    }
    if (synced) {
        out->items[out->count].card = (uint8_t)MESH_UI_STATUS_CARD_RADIO;
        out->items[out->count].verb = (uint8_t)MESH_UI_STATUS_VERB_REFRESH;
        out->items[out->count].label = MESH_STR_ACTION_REFRESH;
        ++out->count;
    }
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
