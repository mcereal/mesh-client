#pragma once

/*
 * The transcript and the places on it: what has been said, what has been shared, and how far
 * the reader has got through each conversation.
 *
 * Every record here is already resolved for display - a peer's short name is joined in at
 * publish rather than by the backend - which is what lets a renderer draw a thread without
 * reaching for the node roster. See mesh/ui/store.h for the whole.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Newest messages carried to the backends. Matches the transport ring so a per-conversation
   view has the same history the radio gave us; the Brick shows a screenful at a time. */
#define MESH_UI_MAX_MESSAGES 64U
/*
 * The waypoint book's own capacity and upstream's two string limits, restated on this side of
 * the seam - as MESH_UI_MESSAGE_TEXT_MAX already restates the message payload's.
 *
 * The store is nanopb-free by construction, and mesh/core/waypoint.h is not: it takes a
 * meshtastic_MeshPacket. Including it here to reach three numbers would drag the generated
 * protobuf headers into every backend and every test that draws a screen. The numbers are
 * pinned against the core's in the waypoints suite, which is the check that keeps two
 * declarations of one limit honest.
 */
#define MESH_UI_MAX_WAYPOINTS 32U
#define MESH_UI_WAYPOINT_NAME_MAX 30U
#define MESH_UI_WAYPOINT_DESCRIPTION_MAX 100U
#define MESH_UI_MESSAGE_TEXT_MAX 234U

/* One line of conversation, already resolved for display: peer_name is the short name from
   the NodeDB when we know it, so backends never have to join against the node list. */
struct mesh_ui_message {
    uint32_t packet_id;
    uint32_t peer; /* the other end: sender for inbound, destination for outbound */
    uint32_t rx_time;
    char peer_name[16];
    char text[MESH_UI_MESSAGE_TEXT_MAX];
    uint8_t channel;
    uint8_t direction; /* enum mesh_message_direction */
    /* enum mesh_message_kind: an ordinary text message, the firmware's critical alert, or a
       detection sensor announcing itself. All three arrive as text on a channel; only the
       transcript's labelling tells them apart. */
    uint8_t kind;
    uint8_t ack; /* enum mesh_message_ack */
    /* meshtastic_Routing_Error behind an ack of FAILED, so the row can say why rather than
       just marking it failed. Meaningless for anything else. */
    uint8_t ack_error;
    bool broadcast;
    /* Decrypted with our public key rather than a channel PSK: addressed to us and readable by
       nobody else. On a default-key channel a "direct" message is not that, and the transcript
       has no other way to say so. */
    bool pki_encrypted;
    /* The message this one answers, and whether it is a reaction rather than a reply. A
       reaction is an annotation on its target, not a line of its own, so it is filtered out of
       the thread (mesh_ui_nav_filter_messages) and drawn on the bubble it belongs to. */
    uint32_t reply_id;
    bool is_reaction;
};

/*
 * One shared place, resolved for display: `from_name` is the sender's short name when we know
 * it, so a backend never joins against the node list, exactly as a message's peer_name is.
 *
 * A copy of struct mesh_waypoint rather than the thing itself, because the store is nanopb-free
 * by construction and this side of the seam is what backends and the cache read. The fields the
 * core does not carry are the two derived ones below.
 */
struct mesh_ui_waypoint {
    uint32_t id;
    int32_t latitude_i;
    int32_t longitude_i;
    bool has_coords;
    uint32_t expire;
    uint32_t locked_to;
    uint32_t icon;
    uint32_t from;
    uint32_t heard;
    uint8_t channel;
    bool ours;
    /*
     * Whether this client may change it - `locked_to` is 0, or it is us.
     *
     * Derived at publish rather than at the press, because the answer needs our own node number
     * and the nav has no business knowing one. It decides whether deleting broadcasts a
     * withdrawal to the mesh or merely drops our copy, and the detail says which it would do
     * before the press rather than after it.
     */
    bool editable;
    char name[MESH_UI_WAYPOINT_NAME_MAX];
    char description[MESH_UI_WAYPOINT_DESCRIPTION_MAX];
    char from_name[16];
};

struct mesh_ui_waypoint_list {
    struct mesh_ui_waypoint entries[MESH_UI_MAX_WAYPOINTS];
    uint32_t count;
    uint32_t dropped; /* places the book evicted to make room for newer ones */
};

struct mesh_ui_message_list {
    struct mesh_ui_message entries[MESH_UI_MAX_MESSAGES];
    uint32_t count;
    uint32_t dropped; /* older messages the transport ring has already discarded */
};

/* Enough for every channel slot plus the peers anyone realistically keeps in view; the oldest
   mark is evicted once they are all taken. */
#define MESH_UI_READ_MARKS_MAX 32U

/*
 * What the client remembers about one conversation, keyed the way the UI names a destination.
 *
 * Two things, and they share a slot because they share a key and a lifetime: how far the user
 * has read, and whether they want to hear about it at all. A mute in a table of its own would
 * be a second array keyed on (kind, channel, node) and a second eviction rule to keep in step
 * with this one - the read mark's key *is* the conversation's identity, so there is nothing a
 * separate table would express that a field here does not.
 *
 * `packet_id` is "everything up to and including this packet has been seen". A packet id
 * rather than a timestamp or an index: ids survive the ring evicting older messages and the
 * cache merging history back in, and a mark whose message has since been evicted correctly
 * reads as "everything still in view arrived after it".
 */
struct mesh_ui_read_mark {
    uint8_t kind; /* enum mesh_ui_conversation_kind: CHANNEL or DIRECT */
    uint8_t channel;
    uint32_t node;
    uint32_t packet_id;
    uint32_t stamp; /* bumped on every write, so the least recently read can be evicted */
    /*
     * The user has asked not to be interrupted by this conversation: no badge on the tab, no
     * snackbar when something arrives. The messages still arrive and the thread still fills.
     *
     * Deliberately not the only input to that question - see mesh_ui_store_conversation_muted(),
     * which also honours the radio's own per-node mute. This is the half the client owns.
     */
    bool muted;
};

/*
 * The table, and the two counters over it - which are two counters because they answer two
 * questions, and answering both with one was a file written every couple of seconds for as long
 * as a conversation was open.
 *
 * `stamp` is the ordering: it hands out the `mark->stamp` the eviction above sorts by, and it
 * moves every time a conversation is *touched*, whether or not that changed anything. It has to,
 * or "least recently read" would mean "least recently read something new in", and a conversation
 * the user keeps coming back to with nothing new in it would be evicted ahead of one they have
 * not opened since.
 *
 * `revision` is what the file holds: kind, channel, node, packet_id and muted, for as many marks
 * as there are. It moves only when one of those changes.
 * mesh_ui_store_mark_open_conversation_read() runs from consume_updates() on every update while a
 * thread is open - a node reporting, a position arriving, a press - and almost all of those find
 * the mark already where it belongs. Watching `stamp` for persistence turned each of them into a
 * dirty cache and a rewrite of the whole snapshot on a card that is mounted `sync`.
 */
struct mesh_ui_read_state {
    struct mesh_ui_read_mark marks[MESH_UI_READ_MARKS_MAX];
    uint32_t count;
    uint32_t stamp;
    uint32_t revision;
};

/* Combines persisted history with this session's live messages into the newest
   MESH_UI_MAX_MESSAGES, cached entries first. A cached entry whose packet id also appears in
   `live` is dropped, so a message re-received after a restart is not shown twice.

   This exists because the transport's message log starts empty on every run: without merging,
   the first publish would push an empty list over the cache loaded at startup and the next
   save would erase the conversation for good. */
void mesh_ui_message_list_merge(const struct mesh_ui_message_list *cached,
                                const struct mesh_ui_message_list *live,
                                struct mesh_ui_message_list *out);

/*
 * Drops every message in one conversation from a list. Returns how many went.
 *
 * `kind` is an enum mesh_ui_conversation_kind: CHANNEL reads `channel`, DIRECT reads `node`,
 * and neither ALL nor NEW names a conversation, so both remove nothing - "delete everything"
 * is not a thing one press on a list row should be able to mean.
 */
uint32_t mesh_ui_message_list_forget(struct mesh_ui_message_list *list, uint8_t kind, uint32_t node,
                                     uint8_t channel);

#ifdef __cplusplus
}
#endif
