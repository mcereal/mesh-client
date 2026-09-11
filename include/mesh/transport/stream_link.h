#pragma once

#include "mesh/core/event_loop.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The half of a stream transport that is the same for every stream transport.
 *
 * Meshtastic's serial and TCP APIs are one wire format (mesh/proto/stream_framing.h) over two
 * different ways of getting a file descriptor. What differs between them is how that descriptor
 * comes to exist - a sysfs scan, a driver bind and a DTR assert on one side, a hostname and a
 * connect() on the other - and everything after it is identical: read into the frame parser,
 * hand each frame to the session, frame outbound packets into a queue with a partial-write
 * cursor, and keep EPOLLOUT armed exactly while that queue has a remainder.
 *
 * This owns the identical half. A transport owns one of these, opens it with a descriptor it
 * obtained however it likes, and keeps its own connection policy to itself.
 *
 * It is a component rather than a seam: it holds no policy, decides nothing about when to
 * connect or what to do when a link dies, and never resets itself. pump() and flush() report a
 * fatal error and stop; the transport that owns the link decides what that means and says so in
 * its own words, because "port closed" and "the radio hung up" are the same errno and two
 * different sentences.
 */

/* Outbound packets held while the descriptor is not draining. Eight is what the serial link
   carried before this was lifted out of it: a NodeDB sync is inbound, so the outbound queue only
   ever holds what the user and the admin queue produced in one turn of the loop. */
#define MESH_STREAM_LINK_MAX_OUTBOUND 8U

/* One framed ToRadio packet, header included, with a cursor for partial writes. */
struct mesh_stream_link_packet {
    size_t length;
    size_t sent;
    uint32_t packet_id; /* message log id to fail if this never reaches the radio; 0 = none */
    uint8_t data[MESH_STREAM_FRAME_HEADER_LEN + MESH_STREAM_FRAME_MAX_PAYLOAD];
};

struct mesh_stream_link {
    int fd;
    bool fd_registered;
    bool want_write; /* EPOLLOUT is armed because the write queue has a remainder */
    struct mesh_event_loop *loop;

    struct mesh_stream_parser parser;
    size_t frames_received;
    size_t bytes_received;

    struct mesh_stream_link_packet write_queue[MESH_STREAM_LINK_MAX_OUTBOUND];
    size_t write_queue_head;
    size_t write_queue_len;

    /* The conversation this link feeds. Borrowed: the transport owns it, or the app does. */
    struct mesh_session *session;
    /* What this link's log lines are filed under ("serial", "tcp"). Borrowed and never freed;
       callers pass a literal. */
    const char *tag;
};

struct mesh_stream_link_stats {
    size_t frames_received;
    size_t bytes_received;
    /* Bytes the parser discarded resyncing: on a serial port almost all of it is the radio's
       own text log. */
    size_t junk_bytes;
};

/* Puts the link in its closed state. Call once before any other function; `tag` and `session`
   survive an open/close cycle, so they are set here rather than at open. */
void mesh_stream_link_init(struct mesh_stream_link *link, const char *tag,
                           struct mesh_session *session);
void mesh_stream_link_set_session(struct mesh_stream_link *link, struct mesh_session *session);

/*
 * Adopts `fd` - which must already be open and non-blocking - and watches it for readability.
 * The link owns the descriptor from here: close() is mesh_stream_link_close()'s to call.
 *
 * `callback` and `userdata` go to the event loop unchanged, so the transport keeps its own
 * dispatch. A NULL `loop` opens the link unwatched, which is what a test driving pump() by hand
 * wants. Returns 0, or a negative errno with the descriptor left alone for the caller to close.
 */
int mesh_stream_link_open(struct mesh_stream_link *link, int fd, struct mesh_event_loop *loop,
                          mesh_event_callback callback, void *userdata);
/* Unwatches and closes the descriptor, resets the parser, and fails every queued packet against
   the session. Safe on a closed link. */
void mesh_stream_link_close(struct mesh_stream_link *link);
bool mesh_stream_link_is_open(const struct mesh_stream_link *link);

/*
 * Reads what is ready and folds complete frames into the session, bounded so a NodeDB sync
 * cannot starve UI input. Returns the byte count, 0 when nothing was ready, or:
 *
 *   -ENOTCONN  the far end is gone (EOF), or the link is not open
 *   -EIO       the read failed
 *
 * Both are fatal and neither closes the link: the transport that owns it decides.
 */
int mesh_stream_link_pump(struct mesh_stream_link *link);
/* Writes as much of the queue as the descriptor will take. Returns 0, -ENOTCONN when the link
   is closed, or -EIO on a failed write, which is fatal in the same way. */
int mesh_stream_link_flush(struct mesh_stream_link *link);
/* Frames one ToRadio packet, queues it and flushes. Returns 0, -ENOSPC when the queue is full,
   -EMSGSIZE when the packet is too big to frame, -ENOTCONN, or -EIO. */
int mesh_stream_link_send(struct mesh_stream_link *link, const uint8_t *packet, size_t len,
                          uint32_t packet_id);
/*
 * Writes `len` bytes straight at the descriptor, outside the frame queue. The one caller is the
 * serial link's wake burst, which is deliberately not a frame: a run of bare START2 bytes cannot
 * complete one, which is exactly what forces the radio's own parser to resync. Best effort, and
 * a short write is not retried. Returns the bytes written or a negative errno.
 */
int mesh_stream_link_write_raw(struct mesh_stream_link *link, const uint8_t *data, size_t len);

struct mesh_stream_link_stats mesh_stream_link_stats(const struct mesh_stream_link *link);

#ifdef __cplusplus
}
#endif
