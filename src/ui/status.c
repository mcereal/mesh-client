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
 * The Mesh card offers one, and for most of this screen's life it offered none. What changed is
 * that the card acquired a picture: the airtime bar and the trend line under it are a reading
 * somebody can now want to see properly, and a chart is a screen rather than a row. Every
 * *other* verb about the mesh still belongs somewhere else - trace a node, ask it for a fix, or
 * one of the destructive ones that wants the confirmation dialog and is tied to a settings
 * section. A card with no verb is simply skipped by the cursor, which is what makes the flat
 * list work.
 *
 * The trend verb is the one entry here that is not a request over the air, and gating it on the
 * link is therefore a rule about the *list* rather than about the press. What it opens outlives
 * the radio going away - the history is ours - so a client with a trend and no link could
 * honestly offer it. It does not, because offering it there is what puts a verb in front of the
 * cursor: with the link down the list would be [trend] alone, and a radio arriving would slide
 * disconnect in underneath a cursor sitting on index 0. That is the same trap the paragraph
 * above describes, and the same answer - remove the case rather than compensate for it.
 */
void mesh_ui_status_actions(struct mesh_ui_status_actions *out, bool connected, bool synced,
                            bool has_trend) {
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

    /*
     * And last, so that everything already on the list keeps its index.
     *
     * `synced` as well as `has_trend`, though the readings cannot arrive without it: the
     * condition on a verb has to imply the conditions on the verbs before it, or refresh
     * appearing later inserts itself in front of this one. Restating it here is a line of code
     * against a class of bug, and it is what makes the rule checkable by reading this function
     * rather than by reasoning about which report arrives first.
     *
     * It is on the Mesh card because the readings are: the airtime figure, the banded bar and
     * the small line are all on that card, and a verb opening the large version of the picture
     * belongs beside the small one rather than on the card about the radio's own health.
     */
    if (synced && has_trend) {
        out->items[out->count].card = (uint8_t)MESH_UI_STATUS_CARD_MESH;
        out->items[out->count].verb = (uint8_t)MESH_UI_STATUS_VERB_TREND;
        out->items[out->count].label = MESH_STR_ACTION_TREND;
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
