#pragma once

#include "inkwell/net/stream.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The half of a stream transport that knows this is a radio.
 *
 * Meshtastic's serial and TCP APIs are one wire format (mesh/proto/stream_framing.h) over two
 * different ways of getting a file descriptor. What differs between them is how that descriptor
 * comes to exist - a sysfs scan, a driver bind and a DTR assert on one side, a hostname and a
 * connect() on the other - and everything after it is identical.
 *
 * **Most of that identical part is inkwell's now.** `struct inkwell_stream` reads what is ready
 * without starving the loop, queues what goes out, and keeps EPOLLOUT armed exactly while that
 * queue has a remainder - none of which has anything to do with a radio, and all of which any
 * program talking a protocol over a descriptor needs. What is left here is the three things
 * that do know what a radio is:
 *
 *   - the frame parser, because 0x94 0xC3 is Meshtastic's and nobody else's;
 *   - the session the frames go to;
 *   - the two numbers that size the outbound queue, which are this protocol's largest frame
 *     and how many of them this client is willing to hold while a port is not draining.
 *
 * A transport owns one of these, opens it with a descriptor it obtained however it likes, and
 * keeps its own connection policy to itself. pump() and flush() report a fatal error and stop;
 * the transport that owns the link decides what that means and says so in its own words,
 * because "port closed" and "the radio hung up" are the same errno and two different sentences.
 */

/* Outbound packets held while the descriptor is not draining. Eight is what the serial link
   carried before this was lifted out of it: a NodeDB sync is inbound, so the outbound queue only
   ever holds what the user and the admin queue produced in one turn of the loop.

   It is this client's number, which is why inkwell_stream takes it as storage rather than
   declaring one: eight Meshtastic frames is a few kilobytes on a handheld, and would be a
   silly answer for something moving a file. */
#define MESH_STREAM_LINK_MAX_OUTBOUND 8U
#define MESH_STREAM_LINK_SLOT_BYTES (MESH_STREAM_FRAME_HEADER_LEN + MESH_STREAM_FRAME_MAX_PAYLOAD)

/* What the descriptor is, which decides how a write to it is made. inkwell's enum under this
   client's spelling; see inkwell/net/stream.h for why a stream has to be told rather than
   guess, and get it wrong towards SOCKET and every write fails with ENOTSOCK. */
enum mesh_stream_link_kind {
    MESH_STREAM_LINK_FILE = (int)INKWELL_STREAM_FILE,
    MESH_STREAM_LINK_SOCKET = (int)INKWELL_STREAM_SOCKET,
};

struct mesh_stream_link {
    /* The descriptor, the read bound, the outbound queue and the EPOLLOUT arithmetic. */
    struct inkwell_stream stream;
    /* ...over storage this client sizes, because the slot is one Meshtastic frame. */
    struct inkwell_stream_slot slots[MESH_STREAM_LINK_MAX_OUTBOUND];
    uint8_t slot_bytes[MESH_STREAM_LINK_MAX_OUTBOUND * MESH_STREAM_LINK_SLOT_BYTES];

    struct mesh_stream_parser parser;
    size_t frames_received;

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
 * `kind` says what the descriptor is, which decides how writes are made; see above, and get it
 * wrong towards SOCKET and every write fails with ENOTSOCK.
 *
 * `callback` and `userdata` go to the event loop unchanged, so the transport keeps its own
 * dispatch. A NULL `loop` opens the link unwatched, which is what a test driving pump() by hand
 * wants. Returns 0, or a negative errno with the descriptor left alone for the caller to close.
 */
int mesh_stream_link_open(struct mesh_stream_link *link, int fd, enum mesh_stream_link_kind kind,
                          struct inkwell_loop *loop, inkwell_loop_callback callback,
                          void *userdata);
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
