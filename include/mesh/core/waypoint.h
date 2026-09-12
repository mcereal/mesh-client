#pragma once

#include "mesh/utils/time.h"

#include "meshtastic/mesh.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Waypoints: the places on the mesh, as opposed to the nodes on it.
 *
 * A WAYPOINT_APP packet carries a `Waypoint` - an id, a coordinate, a name, a description, an
 * emoji and an expiry - broadcast on a channel like a text message. Every phone app can make
 * one and share it, and this client discarded them.
 *
 * The book below is the client's copy of what the mesh has shared, and it is a *table keyed by
 * id* rather than a log: upstream edits a waypoint by re-broadcasting it with the same id, so a
 * second copy is the same place moved or renamed, not a second place. That is the whole
 * difference from the message ring next door, where two packets are two things that happened.
 *
 * The one convention that is not in the .proto: a waypoint is deleted by broadcasting it with
 * `expire` set to a moment already past. See mesh_waypoint_state().
 */

/*
 * Upstream's own limits (proto/meshtastic/meshtastic/mesh.options), as *characters*.
 *
 * `max_size` is the generated buffer rather than the string: nanopb emits `char name[30]` for
 * `max_size:30`, pb_enc_string() writes at most `data_size - 1` bytes and pb_dec_string()
 * refuses anything that would not leave room for the terminator - so the wire carries 29
 * characters and a thirtieth is one no client can send or receive. That is the same reading
 * settings_text.def takes of every other string field on the radio (CHANNEL_NAME is 11 of a
 * `max_size:12`, STATUS_TEXT 79 of an 80), and it is not the reading the message payload takes,
 * because a `bytes` field's max_size has no terminator in it.
 *
 * Stated one too high, the limit is refused nowhere: the keyboard takes the character, the book
 * keeps it and the list draws it, and the encoder drops it on the way out with nothing on the
 * frame saying so - the place on this Brick and the place on every other client one character
 * apart. The two assertions below pin both numbers to the generated struct so a regeneration
 * that widens or narrows either field fails the build rather than the mesh.
 */
#define MESH_WAYPOINT_NAME_MAX 29U
#define MESH_WAYPOINT_DESCRIPTION_MAX 99U

/* Spelled through a macro because of the extern "C" block above, which says this header may be
   included from C++ - and `_Static_assert` is a C keyword g++ rejects outright. The same bridge
   mesh/ui/theme.h writes for the same reason, and written again here rather than shared: a
   three-line macro is not a module boundary, and a core header reaching into the UI's for one
   would be. */
#ifdef __cplusplus
#define MESH_WAYPOINT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define MESH_WAYPOINT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

MESH_WAYPOINT_STATIC_ASSERT(MESH_WAYPOINT_NAME_MAX + 1U == sizeof(((meshtastic_Waypoint *)0)->name),
                            "a waypoint's name is its wire buffer less the NUL");
MESH_WAYPOINT_STATIC_ASSERT(MESH_WAYPOINT_DESCRIPTION_MAX + 1U ==
                                sizeof(((meshtastic_Waypoint *)0)->description),
                            "a waypoint's description is its wire buffer less the NUL");

/*
 * How many places the client remembers.
 *
 * Smaller than the node roster on purpose: nodes arrive whether anyone wants them or not and a
 * roster that evicts loses a name for good, while a waypoint is something a person deliberately
 * shared and a mesh that has shared more than thirty-two of them has a map problem, not a
 * capacity problem.
 */
#define MESH_WAYPOINT_BOOK_CAPACITY 32U

/*
 * The `expire` this client broadcasts to say a waypoint is gone.
 *
 * Deleting is not a verb on the wire - there is no "unshare" packet - so every app does it by
 * re-broadcasting the waypoint with an expiry in the past, and the receiver drops it. 1 is one
 * second after the epoch: unambiguously past, and unmistakable for a real expiry somebody set.
 */
#define MESH_WAYPOINT_EXPIRE_DELETED 1U

/*
 * Below this, an `expire` is a tombstone rather than a date.
 *
 * The same floor mesh_session_wall_clock() uses to decide whether a clock is a clock. It exists
 * because the Brick has no wall clock of its own: with `now` unknown, "has this expired?" is
 * unanswerable in general - but "was this expiry a moment in 1970?" is answerable without any
 * clock at all, and that is the case a delete is. So a tombstone is honoured always, and a real
 * expiry only once we know what time it is.
 */
#define MESH_WAYPOINT_TOMBSTONE_BEFORE MESH_TIME_CLOCK_MIN_EPOCH

/* One shared place, in the client's own terms rather than nanopb's. */
struct mesh_waypoint {
    uint32_t id; /* the sender's; a re-broadcast with this id is an edit of this entry */
    int32_t latitude_i;
    int32_t longitude_i;
    /* When it stops being a place. 0 means never, which is what the apps send by default. */
    uint32_t expire;
    /* The only node allowed to change it; 0 means anyone on the mesh may. */
    uint32_t locked_to;
    /* The designator the sender picked, as a Unicode code point. 0 when they picked none. */
    uint32_t icon;
    uint32_t from;   /* the node that shared it, 0 when the packet did not say */
    uint32_t heard;  /* our clock when this copy arrived; 0 when we could not tell */
    uint8_t channel; /* the channel it was shared on, and the one an edit goes back out on */
    bool ours;       /* this client sent it, so the list can say which places are yours */
    bool has_coords; /* both coordinates were present and inside mesh_geo_coords_valid() */
    char name[MESH_WAYPOINT_NAME_MAX + 1U];
    char description[MESH_WAYPOINT_DESCRIPTION_MAX + 1U];
};

/*
 * What the client knows about the mesh's places.
 *
 * `dropped` counts entries the table evicted to make room, for the same reason the message ring
 * counts its own: a list that quietly stops being complete should be able to say so.
 */
struct mesh_waypoint_book {
    struct mesh_waypoint entries[MESH_WAYPOINT_BOOK_CAPACITY];
    size_t count;
    uint32_t dropped;
};

/* Whether a waypoint is still a place, asked with whatever clock the caller has. */
enum mesh_waypoint_state {
    MESH_WAYPOINT_LIVE = 0, /* still a place */
    MESH_WAYPOINT_EXPIRED,  /* its own expiry has passed on a clock we trust */
    MESH_WAYPOINT_DELETED,  /* an expiry from before there were clocks: the sender withdrew it */
};

/*
 * `now` is our wall clock in seconds, or 0 when we do not have one - which on a Brick is most
 * of the time. A waypoint whose expiry is a real date is LIVE under an unknown clock: saying it
 * expired would be a guess, and the mesh's own copy is the authority anyway.
 */
enum mesh_waypoint_state mesh_waypoint_state(const struct mesh_waypoint *waypoint, uint32_t now);

void mesh_waypoint_book_reset(struct mesh_waypoint_book *book);

/* Index 0 is the first entry; order is arrival, and the UI sorts its own list. NULL past the
   end. The returned pointer is invalidated by the next ingest, store or forget. */
const struct mesh_waypoint *mesh_waypoint_book_at(const struct mesh_waypoint_book *book,
                                                  size_t index);

/* The entry with this id, or NULL. Ids are the sender's, so 0 is not one. */
struct mesh_waypoint *mesh_waypoint_book_find(struct mesh_waypoint_book *book, uint32_t id);
/* The same for a caller holding a borrowed view - which is what mesh_session_waypoints() hands
   back, and what every reader outside the session has. */
const struct mesh_waypoint *mesh_waypoint_book_get(const struct mesh_waypoint_book *book,
                                                   uint32_t id);

/*
 * Puts one in, replacing the entry with the same id.
 *
 * When the table is full the least recently heard entry goes, and one of ours is never chosen
 * over one of somebody else's while any of theirs is older: a place this client shared is the
 * one the user can no longer get back from the mesh.
 *
 * Returns the stored slot, or NULL when the waypoint has no id. The returned pointer is
 * invalidated by the next call.
 */
struct mesh_waypoint *mesh_waypoint_book_store(struct mesh_waypoint_book *book,
                                               const struct mesh_waypoint *waypoint);

/* Drops the entry with this id. True when there was one. */
bool mesh_waypoint_book_forget(struct mesh_waypoint_book *book, uint32_t id);

/*
 * Drops every entry whose own expiry has passed. Returns how many went.
 *
 * `now` is a *credible* wall clock or 0 - mesh_time_wall_credible_s()'s answer, not
 * mesh_time_wall_s()'s. With 0 this removes nothing, which is the honest reading: a place with
 * a date on it outlives a client that cannot read dates, and the mesh's own copy is the
 * authority. A tombstone needs no clock and is handled at ingest, where it arrives.
 */
uint32_t mesh_waypoint_book_prune(struct mesh_waypoint_book *book, uint32_t now);

/*
 * Folds one inbound WAYPOINT_APP packet into the book.
 *
 * `heard` is our clock when it arrived (0 when unknown) and `my_node_num` is what marks a
 * waypoint as ours - the radio echoes our own sends back to us, so the entry we already hold
 * for one is refreshed rather than duplicated, exactly as a text message is.
 *
 * `heard` is also what the expiry is read against, so a place that had already expired when it
 * reached us is dropped rather than stored - and, when it is one we were holding, taken away.
 * With no clock to read it against it is stored, which is the same answer prune gives.
 *
 * Returns 1 when the book changed, 0 when the packet was understood and added nothing (a
 * withdrawal of something we never had), and a negative errno on bad input. Never fails on
 * malformed radio content: the payload is untrusted, so it is sanitised rather than trusted.
 */
int mesh_waypoint_ingest(struct mesh_waypoint_book *book, const meshtastic_MeshPacket *packet,
                         uint32_t my_node_num, uint32_t heard);

/* What a send needs that the waypoint itself does not carry. */
struct mesh_waypoint_request {
    const struct mesh_waypoint *waypoint;
    uint32_t packet_id;
    uint8_t channel;
};

/* Encodes a waypoint as a ToRadio protobuf ready for one BLE GATT write (BLE applies no stream
   framing). Broadcast, never acked: a waypoint goes to the channel, not to a node. `from` is
   left unset - the firmware fills in its own node number. Returns 0 and sets *written. */
int mesh_waypoint_encode(const struct mesh_waypoint_request *request, uint8_t *out, size_t out_len,
                         size_t *written);

#ifdef __cplusplus
}
#endif
