#include "mesh/core/store_forward.h"

#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include "meshtastic/portnums.pb.h"
#include "meshtastic/storeforward.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <string.h>

/* Big enough for a ToRadio holding a StoreAndForward with no `text` in it: a request carries an
   rr, a window and a cursor, and the largest of those is three varints inside two submessages.
   Sized like the traceroute's body buffer rather than computed, for the same reason. */
#define STORE_FORWARD_BODY_MAX 64U

void mesh_store_forward_reset(struct mesh_store_forward *sf) {
    if (sf == NULL) {
        return;
    }
    memset(sf, 0, sizeof(*sf));
}

bool mesh_store_forward_is_frame(const meshtastic_MeshPacket *packet) {
    return packet != NULL && packet->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
           packet->decoded.portnum == meshtastic_PortNum_STORE_FORWARD_APP;
}

int mesh_store_forward_encode(const struct mesh_store_forward_request *request, uint8_t *out,
                              size_t out_len, size_t *written) {
    if (request == NULL || out == NULL || written == NULL || out_len == 0U) {
        return -EINVAL;
    }
    if (request->dest == 0U) {
        return -EINVAL;
    }
    /* A history request is a question addressed to one router. Broadcasting it would have every
       router on the mesh replay its whole window at us at once, which is the one thing a shared
       band cannot afford - so the broadcast address is the ping's alone. */
    if (!request->ping && request->dest == MESH_MESSAGE_BROADCAST_ADDR) {
        return -EINVAL;
    }

    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_default;
    if (request->ping) {
        sf.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_PING;
    } else {
        sf.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
        sf.which_variant = meshtastic_StoreAndForward_history_tag;
        sf.variant.history.window = request->window_minutes;
        sf.variant.history.last_request = request->cursor;
    }

    uint8_t body[STORE_FORWARD_BODY_MAX];
    pb_ostream_t body_stream = pb_ostream_from_buffer(body, sizeof body);
    if (!pb_encode(&body_stream, meshtastic_StoreAndForward_fields, &sf)) {
        mesh_log_error("store-forward", "Failed to encode request: %s", PB_GET_ERROR(&body_stream));
        return -EIO;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *packet = &to_radio.packet;
    packet->to = request->dest;
    packet->id = request->packet_id;
    packet->channel = request->channel;
    /* The reply is the acknowledgement; see the header. */
    packet->want_ack = false;
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_STORE_FORWARD_APP;
    packet->decoded.want_response = true;
    memcpy(packet->decoded.payload.bytes, body, body_stream.bytes_written);
    packet->decoded.payload.size = (pb_size_t)body_stream.bytes_written;

    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("store-forward", "Failed to encode ToRadio: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    *written = stream.bytes_written;
    return 0;
}

void mesh_store_forward_sent(struct mesh_store_forward *sf, uint32_t dest, uint8_t channel,
                             uint64_t now_ms, bool ping) {
    if (sf == NULL) {
        return;
    }
    sf->sent_ms = now_ms;
    sf->last_ms = now_ms;
    sf->expected = 0U;
    sf->received = 0U;
    sf->stored = 0U;
    sf->seq++;
    sf->followup = false;
    if (ping) {
        sf->state = (uint8_t)MESH_STORE_FORWARD_SEEKING;
        return;
    }
    sf->state = (uint8_t)MESH_STORE_FORWARD_REQUESTED;
    sf->router = dest;
    sf->router_channel = channel;
}

void mesh_store_forward_send_failed(struct mesh_store_forward *sf) {
    if (sf == NULL) {
        return;
    }
    sf->followup = false;
    sf->state = (uint8_t)MESH_STORE_FORWARD_FAILED;
}

void mesh_store_forward_stored(struct mesh_store_forward *sf) {
    if (sf == NULL) {
        return;
    }
    if (sf->stored < UINT32_MAX) {
        sf->stored++;
    }
}

/*
 * A router owned up.
 *
 * Reached only for a frame the ingest has already decided may speak for the current state, so
 * the newest announcement always wins here: mid-request, everything from a node that is not the
 * one we asked was dropped before this.
 *
 * A router that says it is `secondary` is remembered anyway. It is a router, and on a mesh whose
 * primary is off it is the only one - the flag is worth showing, not worth refusing over.
 */
static void store_forward_note_router(struct mesh_store_forward *sf,
                                      const meshtastic_MeshPacket *packet, uint32_t now) {
    /*
     * A different node is a different router, and almost everything we hold about one is only
     * true of that one. The cursor is the sharp case: the .proto calls it an index into *the
     * server's* packet history, so handing router A's to router B asks B to skip to a position
     * in a table it does not have - it would silently miss messages, which is this feature
     * failing in the one direction nothing on the screen could show. The rank and the statistics
     * are milder and wrong the same way: they would be drawn under the new router's name.
     */
    if (sf->router != packet->from) {
        sf->cursor = 0U;
        sf->router_secondary = false;
        sf->heartbeat_period = 0U;
        sf->has_stats = false;
        sf->messages_total = 0U;
        sf->messages_saved = 0U;
        sf->messages_max = 0U;
        sf->return_max = 0U;
        sf->return_window = 0U;
    }
    sf->router = packet->from;
    sf->router_channel = packet->channel;
    sf->router_heard = packet->has_rx_time && packet->rx_time != 0U ? packet->rx_time : now;
}

/* The replayed message, in the client's own terms. The envelope is the original sender's - the
   router preserves `from`, `channel` and `rx_time` when it replays - so this is the same
   message it was, arriving late. */
static bool store_forward_build_message(const meshtastic_MeshPacket *packet,
                                        const meshtastic_StoreAndForward *sf, bool broadcast,
                                        uint32_t my_node_num, struct mesh_message *out) {
    memset(out, 0, sizeof(*out));
    mesh_text_sanitise(sf->variant.text.bytes, sf->variant.text.size, out->text, sizeof(out->text));
    if (out->text[0] == '\0') {
        return false;
    }
    out->kind = (uint8_t)MESH_MESSAGE_KIND_TEXT;
    /*
     * Deliberately not the replay packet's id. That id belongs to the router's own packet, and
     * two routers replaying the same message would give it two of them - it identifies the
     * delivery, never the message. Nothing correlates a replayed message by id (an ack cannot
     * arrive for something that happened hours ago), so it is left at 0 rather than filled with
     * a number that would look like one.
     */
    out->packet_id = 0U;
    out->from = packet->from;
    /*
     * The router tells us which it was through `rr` rather than through `to`, because `to` on
     * the replay packet is us either way. ROUTER_TEXT_BROADCAST goes back to the channel
     * conversation it was said in; ROUTER_TEXT_DIRECT was addressed to us.
     */
    out->to = broadcast ? MESH_MESSAGE_BROADCAST_ADDR : my_node_num;
    out->channel = packet->channel;
    /*
     * No date, and it is not an oversight: there is no date to be had.
     *
     * `rx_time` is "the time this message was received", and mesh.proto says of it that the
     * field "is _never_ sent on the radio link itself (to save space)" - so the router's copy of
     * when the message was originally heard does not travel, and what arrives here is *our own*
     * radio stamping the moment the replay landed. Copying that would date everything the router
     * hands back to the minute it was fetched, and the StoreAndForward `text` variant carries no
     * timestamp of its own to use instead.
     *
     * It also has to be 0 for the de-duplication to work at all. Two stamps that are both
     * non-zero and never equal - the live copy's real arrival and the replay's - is precisely
     * the case mesh_message_log_holds_replay() reads as two different messages, so a window
     * dated this way would be appended under the copies it duplicates. Left unknown, the same
     * comparison falls through to what was said, which is the only thing both copies share.
     *
     * The cost is that a sender who said the same short thing twice on one channel gets one
     * bubble back instead of two. That is the trade the no-clock case already makes, and it is
     * the right way round: losing a second "ok" beats replaying four hours on top of itself.
     */
    out->rx_time = 0U;
    out->direction =
        (uint8_t)((my_node_num != 0U && packet->from == my_node_num) ? MESH_MESSAGE_OUTBOUND
                                                                     : MESH_MESSAGE_INBOUND);
    /*
     * No SNR, no hop count and no padlock either, for one reason: all four describe how *this*
     * packet reached us, and this packet came one hop from a router that is not the sender.
     * Drawing the router's link under the sender's name would be a measurement of the wrong
     * link. The delivery state is left at NONE for the same reason a broadcast's is: there is
     * nothing left to wait for.
     */
    return true;
}

int mesh_store_forward_ingest(struct mesh_store_forward *state, const meshtastic_MeshPacket *packet,
                              uint32_t my_node_num, uint32_t now, uint64_t now_ms,
                              struct mesh_message *message) {
    if (state == NULL || packet == NULL) {
        return -EINVAL;
    }
    if (!mesh_store_forward_is_frame(packet)) {
        return -EINVAL;
    }

    const meshtastic_Data *data = &packet->decoded;
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);
    if (!pb_decode(&stream, meshtastic_StoreAndForward_fields, &sf)) {
        /* Untrusted radio content: logged and dropped, never an error the caller has to
           handle. The packet is still claimed - it was a Store & Forward frame. */
        mesh_log_debug("store-forward", "Bad frame from 0x%08x: %s", packet->from,
                       PB_GET_ERROR(&stream));
        return MESH_STORE_FORWARD_EVENT_NONE;
    }

    /*
     * While a request is running, only the router we asked may say anything *about* it.
     *
     * Without this, a second router's heartbeat - or a ROUTER_HISTORY from an exchange we had
     * given up on - resets the count we are part way through, the cursor and the state, because
     * every arm below writes them. On a mesh with one router it never happens, which is exactly
     * why it would have been found late.
     *
     * The test is deliberately not applied to a replayed message, and cannot be: a ROUTER_TEXT_*
     * frame carries the *original sender* on the envelope - that is the whole point of it - so
     * `from` says nothing about which router relayed it, and gating on it would reject every
     * message the feature exists to collect. Two routers replaying at once therefore merge into
     * one count, which is honest (the messages did arrive, and the log de-duplicates them) and
     * is the milder of the two failures by a wide margin.
     */
    const bool router_authored =
        sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT ||
        sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG ||
        sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY ||
        sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY ||
        sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_ERROR ||
        sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS;
    const bool running = state->state == (uint8_t)MESH_STORE_FORWARD_REQUESTED ||
                         state->state == (uint8_t)MESH_STORE_FORWARD_REPLAYING;
    if (router_authored && running && state->router != 0U && packet->from != state->router) {
        mesh_log_debug("store-forward", "Ignoring a frame from 0x%08x; 0x%08x is answering",
                       packet->from, state->router);
        return MESH_STORE_FORWARD_EVENT_NONE;
    }

    /*
     * A ping that was answered. Either announcement will do - the pong we asked for, or a
     * heartbeat that happened to land in the same half minute - because both say the same
     * thing, which is that there is a router at that node number and it is awake.
     */
    if (state->state == (uint8_t)MESH_STORE_FORWARD_SEEKING &&
        (sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG ||
         sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT)) {
        state->followup = true;
    }

    switch (sf.rr) {
    case meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT:
        store_forward_note_router(state, packet, now);
        if (sf.which_variant == meshtastic_StoreAndForward_heartbeat_tag) {
            state->heartbeat_period = sf.variant.heartbeat.period;
            state->router_secondary = sf.variant.heartbeat.secondary != 0U;
        }
        return MESH_STORE_FORWARD_EVENT_ROUTER;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG:
        store_forward_note_router(state, packet, now);
        state->last_ms = now_ms;
        return MESH_STORE_FORWARD_EVENT_ROUTER;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY: {
        store_forward_note_router(state, packet, now);
        state->last_ms = now_ms;
        if (sf.which_variant == meshtastic_StoreAndForward_history_tag) {
            state->expected = sf.variant.history.history_messages;
            /*
             * The cursor the next request carries. The reply's `window` is deliberately not
             * kept: the .proto gives it no unit, upstream's own request and response do not
             * obviously agree on one, and a number the client cannot name is not a number to
             * put on a screen.
             */
            state->cursor = sf.variant.history.last_request;
        }
        state->received = 0U;
        state->stored = 0U;
        state->state = (uint8_t)(state->expected == 0U ? MESH_STORE_FORWARD_EMPTY
                                                       : MESH_STORE_FORWARD_REPLAYING);
        mesh_log_info("store-forward", "Router 0x%08x is replaying %u message(s)", packet->from,
                      state->expected);
        return MESH_STORE_FORWARD_EVENT_HISTORY;
    }

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_DIRECT:
    case meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST: {
        if (sf.which_variant != meshtastic_StoreAndForward_text_tag || message == NULL) {
            return MESH_STORE_FORWARD_EVENT_NONE;
        }
        const bool broadcast =
            sf.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST;
        if (!store_forward_build_message(packet, &sf, broadcast, my_node_num, message)) {
            return MESH_STORE_FORWARD_EVENT_NONE;
        }
        state->last_ms = now_ms;
        if (state->received < UINT32_MAX) {
            state->received++;
        }
        /*
         * A replay can arrive without a ROUTER_HISTORY in front of it - the announcement is one
         * packet on a lossy band and the messages are the payload. Treated as a replay running
         * rather than dropped: the messages are the feature, and the count they are counted
         * against catches up on its own.
         */
        if (state->state != (uint8_t)MESH_STORE_FORWARD_REPLAYING) {
            state->state = (uint8_t)MESH_STORE_FORWARD_REPLAYING;
        }
        if (state->expected != 0U && state->received >= state->expected) {
            state->state = (uint8_t)MESH_STORE_FORWARD_DONE;
        }
        return MESH_STORE_FORWARD_EVENT_TEXT;
    }

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY:
        store_forward_note_router(state, packet, now);
        state->last_ms = now_ms;
        state->state = (uint8_t)MESH_STORE_FORWARD_BUSY;
        mesh_log_info("store-forward", "Router 0x%08x is busy", packet->from);
        return MESH_STORE_FORWARD_EVENT_REFUSED;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_ERROR:
        store_forward_note_router(state, packet, now);
        state->last_ms = now_ms;
        state->state = (uint8_t)MESH_STORE_FORWARD_FAILED;
        mesh_log_warn("store-forward", "Router 0x%08x reports an error", packet->from);
        return MESH_STORE_FORWARD_EVENT_REFUSED;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS:
        store_forward_note_router(state, packet, now);
        if (sf.which_variant == meshtastic_StoreAndForward_stats_tag) {
            state->has_stats = true;
            state->messages_total = sf.variant.stats.messages_total;
            state->messages_saved = sf.variant.stats.messages_saved;
            state->messages_max = sf.variant.stats.messages_max;
            state->return_max = sf.variant.stats.return_max;
            state->return_window = sf.variant.stats.return_window;
        }
        return MESH_STORE_FORWARD_EVENT_STATS;

    default:
        /*
         * The CLIENT_* half, which is somebody else's request crossing our radio, and anything
         * upstream adds next. Claimed and ignored: a request addressed to a router is not a
         * message, and this client is not one.
         */
        return MESH_STORE_FORWARD_EVENT_NONE;
    }
}

bool mesh_store_forward_tick(struct mesh_store_forward *sf, uint64_t now_ms) {
    if (sf == NULL) {
        return false;
    }
    const uint64_t since = now_ms > sf->last_ms ? now_ms - sf->last_ms : 0U;
    switch ((enum mesh_store_forward_state)sf->state) {
    case MESH_STORE_FORWARD_SEEKING:
        if (since >= MESH_STORE_FORWARD_SEEK_TIMEOUT_MS) {
            sf->followup = false;
            sf->state = (uint8_t)MESH_STORE_FORWARD_NO_ROUTER;
            mesh_log_info("store-forward", "No router answered the ping");
            return true;
        }
        return false;
    case MESH_STORE_FORWARD_REQUESTED:
        if (since >= MESH_STORE_FORWARD_REQUEST_TIMEOUT_MS) {
            sf->state = (uint8_t)MESH_STORE_FORWARD_TIMEOUT;
            mesh_log_info("store-forward", "Router 0x%08x did not answer the history request",
                          sf->router);
            /* And it is forgotten, so the next press goes looking rather than asking the same
               silence again. The cursor goes with it: it indexes a table inside that router. */
            sf->router = 0U;
            sf->router_channel = 0U;
            sf->cursor = 0U;
            return true;
        }
        return false;
    case MESH_STORE_FORWARD_REPLAYING:
        if (since >= MESH_STORE_FORWARD_REPLAY_GAP_MS) {
            sf->state = (uint8_t)MESH_STORE_FORWARD_DONE;
            mesh_log_info("store-forward", "Replay stopped after %u of %u message(s)", sf->received,
                          sf->expected);
            return true;
        }
        return false;
    default:
        return false;
    }
}

const char *mesh_store_forward_state_to_string(enum mesh_store_forward_state state) {
    switch (state) {
    case MESH_STORE_FORWARD_IDLE:
        return "idle";
    case MESH_STORE_FORWARD_SEEKING:
        return "seeking";
    case MESH_STORE_FORWARD_REQUESTED:
        return "requested";
    case MESH_STORE_FORWARD_REPLAYING:
        return "replaying";
    case MESH_STORE_FORWARD_DONE:
        return "done";
    case MESH_STORE_FORWARD_EMPTY:
        return "empty";
    case MESH_STORE_FORWARD_BUSY:
        return "busy";
    case MESH_STORE_FORWARD_FAILED:
        return "failed";
    case MESH_STORE_FORWARD_NO_ROUTER:
        return "no router";
    case MESH_STORE_FORWARD_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}
