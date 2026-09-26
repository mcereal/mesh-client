#pragma once

#include "mesh/proto/ble_profile.h"
#include "mesh/proto/stream_framing.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What a link carries frames for.
 *
 * A link - BLE, serial, TCP - owns a connection and nothing else. Everything it does with the
 * bytes on it is one of eight things: it installs a send path when the connection comes up,
 * starts the conversation, hands over every frame that arrives, reports a frame that never left,
 * ticks the clock, asks whether the far end has gone quiet, sends a keepalive where the
 * connection needs one, and takes the send path away when the connection goes. That list is
 * the whole of what a link knows about a mesh protocol, so it is an interface rather than the
 * eight mesh_session_* calls it used to be spelled as.
 *
 * Meshtastic is the one protocol behind it today: mesh_session_protocol() in
 * mesh/core/session.h wraps a session in one of these. A second protocol is a second ops table
 * over a session of its own, and every link carries it without an edit - which is the point.
 *
 * What the interface deliberately does not say is what a frame *means*. A frame is an opaque
 * payload the link neither encodes nor decodes; the protocol owns that entirely, and so owns
 * which framing a stream link wraps it in (`stream_framing`) and which GATT contract a BLE link
 * finds it under (`ble_profile`), because the same port or radio speaks whichever protocol the
 * firmware on the far side does.
 */

/* Hands one frame (raw, unframed) to the link. `frame_id` is the protocol's handle for it,
   given back through mesh_protocol_frame_failed() if the frame never reaches the far end (0
   when the protocol does not need to know). Returns 0 or a negative errno. */
typedef int (*mesh_protocol_send_fn)(void *ctx, const uint8_t *frame, size_t len,
                                     uint32_t frame_id);

struct mesh_protocol_ops {
    /* For log lines and tests. A literal. */
    const char *name;
    /* How a stream link wraps this protocol's frames. NULL for a protocol that only rides a
       transport with its own message boundaries (a GATT write is one frame). */
    const struct mesh_stream_framing *stream_framing;
    /* The service and characteristics a BLE link finds this protocol under, and how frames
       arrive on them. NULL for a protocol that does not ride BLE. */
    const struct mesh_ble_profile *ble_profile;

    /* Link up: install the send path. Nothing goes out until begin(). */
    void (*attach)(void *self, mesh_protocol_send_fn send, void *send_ctx);
    /* Link down: drop the send path. What the conversation learned is the protocol's to keep
       or clear; the link does not say. */
    void (*detach)(void *self);
    /* Starts the conversation - a handshake, a config request, whatever this protocol opens
       with. Returns 0 or a negative errno, which the link treats as a failed connect. */
    int (*begin)(void *self);
    /* One whole frame off the connection. The bytes are only valid for the call. */
    void (*receive)(void *self, const uint8_t *frame, size_t len);
    /* A frame the link accepted with this id and could not deliver. */
    void (*frame_failed)(void *self, uint32_t frame_id);
    /* Called from the link's own tick, on the loop. */
    void (*tick)(void *self, uint64_t now_ms);
    /* The connection is up but the far end has stopped answering. The link drops the
       connection when this turns true, and the detach that follows is what clears it. */
    bool (*silent)(const void *self);
    /* Optional. Something small that keeps an idle connection from being reaped by whatever
       sits in between. NULL when the protocol has none. */
    int (*keepalive)(void *self);
};

/* A protocol is a table and the state it runs on. Copied by value; the state is borrowed. */
struct mesh_protocol {
    const struct mesh_protocol_ops *ops;
    void *self;
};

/* Every call below is safe on an unbound protocol (`ops` NULL), where it does nothing and
   returns what "no conversation" means: -ENOTCONN, false or NULL. A link that has not been
   handed a protocol yet is an ordinary state during start-up, not a crash waiting to happen. */
bool mesh_protocol_bound(const struct mesh_protocol *protocol);
const char *mesh_protocol_name(const struct mesh_protocol *protocol);
const struct mesh_stream_framing *
mesh_protocol_stream_framing(const struct mesh_protocol *protocol);
const struct mesh_ble_profile *mesh_protocol_ble_profile(const struct mesh_protocol *protocol);

void mesh_protocol_attach(const struct mesh_protocol *protocol, mesh_protocol_send_fn send,
                          void *send_ctx);
void mesh_protocol_detach(const struct mesh_protocol *protocol);
int mesh_protocol_begin(const struct mesh_protocol *protocol);
void mesh_protocol_receive(const struct mesh_protocol *protocol, const uint8_t *frame, size_t len);
void mesh_protocol_frame_failed(const struct mesh_protocol *protocol, uint32_t frame_id);
void mesh_protocol_tick(const struct mesh_protocol *protocol, uint64_t now_ms);
bool mesh_protocol_silent(const struct mesh_protocol *protocol);
/* -ENOTSUP when the protocol has no keepalive, so a link can tell "nothing to send" from a
   send that failed. */
int mesh_protocol_keepalive(const struct mesh_protocol *protocol);

#ifdef __cplusplus
}
#endif
