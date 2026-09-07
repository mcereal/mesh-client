#include "mesh/ui/status.h"

#include <string.h>

/*
 * The table.
 *
 * Written out per state rather than composed, for the reason the action-bar tables in
 * src/ui/actions.c are: what a screen offers in a given state should be readable in one place
 * and checkable against the nav that handles the press.
 *
 * Both verbs need a link, and that is the whole of the gating. Disconnect needs one to drop.
 * Refresh needs one because a refresh is a request over the air - mesh_session_refresh_settings()
 * answers -ENOTCONN without one and the app toasts "not connected" - so offering it while the
 * radio is away is offering a press whose only outcome is a complaint. It needs the handshake
 * on top of that, because re-reading a configuration nothing has sent yet reports nothing and
 * reads as broken.
 *
 * **The list only ever grows at the end, and that is a requirement rather than a coincidence.**
 * The cursor on this screen is an index into it, so a verb inserted *ahead* of the cursor would
 * silently change what the next A press does: a client holding a cached configuration would
 * offer refresh alone, and auto-connect arriving would slide disconnect in underneath a cursor
 * still sitting on index 0 - so a press meant to re-read the settings would drop the link that
 * had just come up. Gating both on `connected` removes the case rather than compensating for
 * it: the list is empty, then [disconnect], then [disconnect, refresh], and nothing is ever
 * inserted before something already on it. A verb added here has to keep that true, or the
 * cursor has to start remembering which verb it was on rather than which index.
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
    if (!connected) {
        return;
    }

    out->items[out->count].card = (uint8_t)MESH_UI_STATUS_CARD_LINK;
    out->items[out->count].verb = (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT;
    out->items[out->count].label = MESH_STR_ACTION_DISCONNECT;
    ++out->count;

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
