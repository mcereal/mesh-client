#pragma once

#include "mesh/core/message.h"

#include "meshtastic/mesh.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Store & Forward, from the client side.
 *
 * A router on the mesh keeps the last few hours of text traffic and hands it back on request.
 * The client already had rows for the module's *configuration* - whether ours is a server, how
 * many records it keeps - and never spoke to one, which is the half that matters on a handheld:
 * a Brick spends most of its life switched off, and everything said on the mesh while it was
 * off is gone unless somebody asks for it.
 *
 * The exchange is one portnum, STORE_FORWARD_APP, carrying a `StoreAndForward` whose `rr` says
 * which half of the conversation it is. Requests from us are the CLIENT_* values; everything
 * the router says back is a ROUTER_* one:
 *
 *   CLIENT_PING     -> ROUTER_PONG        "is there a router out there"
 *   CLIENT_HISTORY  -> ROUTER_HISTORY     "how much is coming", then
 *                      ROUTER_TEXT_*      one packet per message being replayed
 *                      ROUTER_BUSY        "not now"
 *                      ROUTER_ERROR       "something is wrong with me"
 *   (unprompted)       ROUTER_HEARTBEAT   a router announcing itself on its own timer
 *
 * This file owns the wire and the state machine; it does not own a transport and it does not
 * own the message log. mesh_store_forward_ingest() hands a replayed message back to the caller
 * as a `struct mesh_message` and the session decides whether the log already has it, exactly as
 * mesh_waypoint_encode() hands back bytes for the session to write.
 */

/*
 * How long a broadcast ping waits for a router to own up before we call the mesh empty.
 *
 * A pong crosses the mesh once in each direction, so this is the traceroute timeout's reasoning
 * with fewer hops in it: generous enough for a router three hops out, short enough that a user
 * who pressed a button gets an answer while still looking at the screen.
 */
#define MESH_STORE_FORWARD_SEEK_TIMEOUT_MS 30000U

/* How long a sent history request waits for the router's ROUTER_HISTORY. */
#define MESH_STORE_FORWARD_REQUEST_TIMEOUT_MS 60000U

/*
 * How long a running replay waits between messages before it is called finished.
 *
 * A gap rather than a deadline, because the router paces the replay itself: it announces a
 * count and then trickles that many packets out over the airtime it can afford, so a replay of
 * thirty messages legitimately takes minutes and a replay that stopped after four looks
 * identical until nothing more arrives.
 */
#define MESH_STORE_FORWARD_REPLAY_GAP_MS 90000U

/*
 * Where a history request has got to.
 *
 * Each of these is a different thing to tell the user, which is why there are eight of them and
 * not a bool: "no router answered" and "the router never replied" are the same silence from two
 * different places, and only one of them is worth pressing again.
 */
enum mesh_store_forward_state {
    MESH_STORE_FORWARD_IDLE = 0,  /* nothing has been asked for on this connection */
    MESH_STORE_FORWARD_SEEKING,   /* a broadcast ping is out, looking for a router */
    MESH_STORE_FORWARD_REQUESTED, /* the history request went to a router; nothing back yet */
    MESH_STORE_FORWARD_REPLAYING, /* the router said how much is coming and it is arriving */
    MESH_STORE_FORWARD_DONE,      /* the replay finished, or stopped arriving */
    MESH_STORE_FORWARD_EMPTY,     /* the router answered and had nothing in the window */
    MESH_STORE_FORWARD_BUSY,      /* ROUTER_BUSY: it is serving somebody else */
    MESH_STORE_FORWARD_FAILED,    /* ROUTER_ERROR, or we could not encode/send the request */
    MESH_STORE_FORWARD_NO_ROUTER, /* the ping went out and nothing answered it */
    MESH_STORE_FORWARD_TIMEOUT,   /* a known router was asked and said nothing */
};

/*
 * What the client knows about Store & Forward on this mesh, and what its last request did.
 *
 * Reset with the handshake, like the traceroute next door: the router is a node number and a
 * channel index, and both of those are read against the radio's own channel table - so carrying
 * them across a radio swap would aim the next request at whatever slot that number names on the
 * new radio. It is the waypoint book's rule for the same reason.
 */
struct mesh_store_forward {
    uint8_t state; /* enum mesh_store_forward_state */

    /*
     * The router we know about, 0 for none. Learned from a heartbeat it broadcast on its own
     * timer, from the pong answering our ping, or from any reply to a request - whichever
     * arrives first. `router_channel` is the channel it was heard on, so a request goes back
     * the way the announcement came rather than assuming the primary.
     */
    uint32_t router;
    uint8_t router_channel;
    /* Our clock when the router was last heard from, epoch seconds; 0 when we have no clock. */
    uint32_t router_heard;
    /* The heartbeat's own period in seconds, 0 when no heartbeat has said. */
    uint32_t heartbeat_period;
    /* The router said it is not the primary Store & Forward router on this mesh. */
    bool router_secondary;

    /*
     * A router answered the ping and the history request it earned has not gone out yet.
     *
     * Set here rather than sent here: this file owns the wire and the state machine and holds
     * no transport, so the send belongs to whoever does - which is also what keeps a reply
     * arriving on the link's read path from writing back down it re-entrantly. The session
     * clears it on the next tick, the way an admin request is queued in one place and drained
     * in another.
     */
    bool followup;

    uint32_t packet_id; /* the request we sent, so a stale reply can be told from ours */
    uint64_t sent_ms;   /* monotonic; when the request or the ping went out */
    uint64_t last_ms;   /* monotonic; when the router last said anything about this request */

    uint32_t expected; /* ROUTER_HISTORY's `history_messages`: how many are coming */
    uint32_t received; /* how many ROUTER_TEXT_* packets have arrived since */
    uint32_t stored;   /* how many of those were new to the message log */

    /*
     * ROUTER_HISTORY's `last_request`, which the .proto describes as "index in the packet
     * history of the last message sent in a previous request ... can be set in a subsequent
     * request to avoid getting packets the server already sent to the client". Kept so a second
     * press does not replay the first press's messages, and reset with the connection because
     * it indexes a table inside a router we may not be talking to next time.
     */
    uint32_t cursor;

    /*
     * `seq` counts requests rather than events, so a UI can tell "the same replay, further
     * along" from "a new one" without holding a clock. It is the radio notice's trick.
     */
    uint32_t seq;

    /* The router's own statistics, when it has sent any (ROUTER_STATS). */
    bool has_stats;
    uint32_t messages_total;
    uint32_t messages_saved;
    uint32_t messages_max;
    uint32_t return_max;
    uint32_t return_window; /* minutes, per the .proto */
};

/* What one inbound STORE_FORWARD_APP packet turned out to be. */
enum mesh_store_forward_event {
    MESH_STORE_FORWARD_EVENT_NONE = 0, /* understood and there is nothing for the caller to do */
    MESH_STORE_FORWARD_EVENT_ROUTER,   /* a router owned up: heartbeat, or the pong we asked for */
    MESH_STORE_FORWARD_EVENT_HISTORY,  /* the router said how many messages are coming */
    MESH_STORE_FORWARD_EVENT_TEXT,     /* a replayed message; the out-param is filled in */
    MESH_STORE_FORWARD_EVENT_REFUSED,  /* busy or in error: the state says which */
    MESH_STORE_FORWARD_EVENT_STATS,    /* the router's own counters */
};

void mesh_store_forward_reset(struct mesh_store_forward *sf);

/* Whether this packet is one for mesh_store_forward_ingest() at all: STORE_FORWARD_APP with a
   decoded payload. Asked by the session so a packet is claimed in one place rather than two. */
bool mesh_store_forward_is_frame(const meshtastic_MeshPacket *packet);

/* What a request needs that the state does not carry. */
struct mesh_store_forward_request {
    uint32_t dest; /* MESH_MESSAGE_BROADCAST_ADDR for the ping that looks for a router */
    uint32_t packet_id;
    uint8_t channel;
    /*
     * A window in minutes, or 0 to let the router use the one it is configured with.
     *
     * 0 is what this client sends, and deliberately: the .proto gives the field no unit and
     * the router's own `return_window` is the only number either end can agree on - a client
     * that guessed would be asking for a window in whatever unit it guessed at. The Brick also
     * has no clock to work out how long it was off for, which is the number a window would
     * want.
     */
    uint32_t window_minutes;
    /* ROUTER_HISTORY's `last_request` from a previous exchange, 0 for "from the beginning". */
    uint32_t cursor;
    /* True for the broadcast CLIENT_PING, false for CLIENT_HISTORY. */
    bool ping;
};

/*
 * Encodes one request as a ToRadio protobuf ready for a single BLE GATT write (BLE applies no
 * stream framing). `from` is left unset - the firmware fills in its own node number.
 *
 * want_ack is off on both. A ping's answer is the pong and a history request's answer is the
 * replay, so a Routing ack on top would be a second round trip across the mesh to learn what
 * the first one already tells us. It is the traceroute's rule.
 *
 * Returns 0 and sets *written, or a negative errno.
 */
int mesh_store_forward_encode(const struct mesh_store_forward_request *request, uint8_t *out,
                              size_t out_len, size_t *written);

/*
 * Records that a request just went out. `ping` picks which wait it starts.
 *
 * Separate from the encode because the send can fail between them, and a state machine that
 * had already moved on would sit waiting for a reply to a packet that never left.
 */
void mesh_store_forward_sent(struct mesh_store_forward *sf, uint32_t packet_id, uint32_t dest,
                             uint8_t channel, uint64_t now_ms, bool ping);

/* Records that a request could not be sent at all. */
void mesh_store_forward_send_failed(struct mesh_store_forward *sf);

/*
 * Folds one inbound STORE_FORWARD_APP packet in.
 *
 * `message` is filled in and MESH_STORE_FORWARD_EVENT_TEXT returned when the packet was a
 * replayed message; it is left alone otherwise. The message is built from the envelope the
 * router preserved - `from`, `channel` and `rx_time` are the original sender's, not the
 * router's, which is what makes a replay land in the right conversation with the right date -
 * and the text is sanitised on the way through, because it came off the air.
 *
 * `now` is our wall clock in seconds (0 when we have none) and `now_ms` the monotonic clock the
 * timeouts are measured on. Returns an enum mesh_store_forward_event, or a negative errno on
 * bad input. Never fails on malformed radio content.
 */
int mesh_store_forward_ingest(struct mesh_store_forward *sf, const meshtastic_MeshPacket *packet,
                              uint32_t my_node_num, uint32_t now, uint64_t now_ms,
                              struct mesh_message *message);

/*
 * Records that a replayed message was new to the log rather than one we already had.
 *
 * The session is the only thing that can answer that, and the count is worth keeping apart
 * from `received`: a replay of thirty messages that stored none of them is the feature working
 * perfectly, and a screen saying "30 messages" about it would be lying.
 */
void mesh_store_forward_stored(struct mesh_store_forward *sf);

/*
 * Advances the timeouts. Returns true when the state changed, so a caller can publish.
 *
 * A ping that found nobody becomes NO_ROUTER, a request nobody answered becomes TIMEOUT, and a
 * replay that stopped arriving becomes DONE with however many messages it managed - which is
 * the honest answer: the router sent what it sent, and the ones that did arrive are in the log.
 *
 * A timeout also forgets the router. A router that was asked and said nothing is a router we no
 * longer know is there, and remembering it would have every press from now on go to the same
 * silence rather than going looking for one that is - which is the difference between a state
 * the user can get out of and one they cannot.
 */
bool mesh_store_forward_tick(struct mesh_store_forward *sf, uint64_t now_ms);

/* "replaying", "no router", ... for logs and the CLI backend. Never NULL. */
const char *mesh_store_forward_state_to_string(enum mesh_store_forward_state state);

#ifdef __cplusplus
}
#endif
