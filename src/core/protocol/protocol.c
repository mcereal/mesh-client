#include "mesh/core/protocol.h"

#include <errno.h>

bool mesh_protocol_bound(const struct mesh_protocol *protocol) {
    return protocol != NULL && protocol->ops != NULL;
}

const char *mesh_protocol_name(const struct mesh_protocol *protocol) {
    return mesh_protocol_bound(protocol) && protocol->ops->name != NULL ? protocol->ops->name
                                                                        : "none";
}

const struct mesh_stream_framing *
mesh_protocol_stream_framing(const struct mesh_protocol *protocol) {
    return mesh_protocol_bound(protocol) ? protocol->ops->stream_framing : NULL;
}

const struct mesh_ble_profile *mesh_protocol_ble_profile(const struct mesh_protocol *protocol) {
    return mesh_protocol_bound(protocol) ? protocol->ops->ble_profile : NULL;
}

void mesh_protocol_attach(const struct mesh_protocol *protocol, mesh_protocol_send_fn send,
                          void *send_ctx) {
    if (mesh_protocol_bound(protocol) && protocol->ops->attach != NULL) {
        protocol->ops->attach(protocol->self, send, send_ctx);
    }
}

void mesh_protocol_detach(const struct mesh_protocol *protocol) {
    if (mesh_protocol_bound(protocol) && protocol->ops->detach != NULL) {
        protocol->ops->detach(protocol->self);
    }
}

int mesh_protocol_begin(const struct mesh_protocol *protocol) {
    if (!mesh_protocol_bound(protocol) || protocol->ops->begin == NULL) {
        return -ENOTCONN;
    }
    return protocol->ops->begin(protocol->self);
}

void mesh_protocol_receive(const struct mesh_protocol *protocol, const uint8_t *frame, size_t len) {
    if (mesh_protocol_bound(protocol) && protocol->ops->receive != NULL) {
        protocol->ops->receive(protocol->self, frame, len);
    }
}

void mesh_protocol_frame_failed(const struct mesh_protocol *protocol, uint32_t frame_id) {
    if (mesh_protocol_bound(protocol) && protocol->ops->frame_failed != NULL) {
        protocol->ops->frame_failed(protocol->self, frame_id);
    }
}

void mesh_protocol_tick(const struct mesh_protocol *protocol, uint64_t now_ms) {
    if (mesh_protocol_bound(protocol) && protocol->ops->tick != NULL) {
        protocol->ops->tick(protocol->self, now_ms);
    }
}

bool mesh_protocol_silent(const struct mesh_protocol *protocol) {
    return mesh_protocol_bound(protocol) && protocol->ops->silent != NULL &&
           protocol->ops->silent(protocol->self);
}

int mesh_protocol_keepalive(const struct mesh_protocol *protocol) {
    if (!mesh_protocol_bound(protocol)) {
        return -ENOTCONN;
    }
    if (protocol->ops->keepalive == NULL) {
        return -ENOTSUP;
    }
    return protocol->ops->keepalive(protocol->self);
}

/* ---- the tap ---------------------------------------------------------------------------- */

static void mesh_protocol_tap_moved(struct mesh_protocol_tap *tap) {
    if (tap->on_traffic != NULL) {
        tap->on_traffic(tap->ctx);
    }
}

/* What the inner protocol sends through: the link's own send, counted when the link took it. */
static int mesh_protocol_tap_send(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id) {
    struct mesh_protocol_tap *tap = (struct mesh_protocol_tap *)ctx;
    if (tap->send == NULL) {
        return -ENOTCONN;
    }
    const int sent = tap->send(tap->send_ctx, frame, len, frame_id);
    if (sent == 0) {
        ++tap->sent;
        mesh_protocol_tap_moved(tap);
    }
    return sent;
}

static void mesh_protocol_tap_attach(void *self, mesh_protocol_send_fn send, void *send_ctx) {
    struct mesh_protocol_tap *tap = (struct mesh_protocol_tap *)self;
    tap->send = send;
    tap->send_ctx = send_ctx;
    mesh_protocol_attach(&tap->inner, mesh_protocol_tap_send, tap);
}

static void mesh_protocol_tap_detach(void *self) {
    struct mesh_protocol_tap *tap = (struct mesh_protocol_tap *)self;
    mesh_protocol_detach(&tap->inner);
    tap->send = NULL;
    tap->send_ctx = NULL;
}

static int mesh_protocol_tap_begin(void *self) {
    return mesh_protocol_begin(&((struct mesh_protocol_tap *)self)->inner);
}

static void mesh_protocol_tap_receive(void *self, const uint8_t *frame, size_t len) {
    struct mesh_protocol_tap *tap = (struct mesh_protocol_tap *)self;
    ++tap->received;
    mesh_protocol_receive(&tap->inner, frame, len);
    mesh_protocol_tap_moved(tap);
}

static void mesh_protocol_tap_frame_failed(void *self, uint32_t frame_id) {
    mesh_protocol_frame_failed(&((struct mesh_protocol_tap *)self)->inner, frame_id);
}

static void mesh_protocol_tap_tick(void *self, uint64_t now_ms) {
    mesh_protocol_tick(&((struct mesh_protocol_tap *)self)->inner, now_ms);
}

static bool mesh_protocol_tap_silent(const void *self) {
    return mesh_protocol_silent(&((const struct mesh_protocol_tap *)self)->inner);
}

static int mesh_protocol_tap_keepalive(void *self) {
    return mesh_protocol_keepalive(&((struct mesh_protocol_tap *)self)->inner);
}

struct mesh_protocol mesh_protocol_tap_bind(struct mesh_protocol_tap *tap,
                                            const struct mesh_protocol *inner) {
    if (tap == NULL || !mesh_protocol_bound(inner)) {
        return (struct mesh_protocol){0};
    }
    tap->inner = *inner;
    tap->send = NULL;
    tap->send_ctx = NULL;
    /* The inner table's data, and a call of ours for every call it has - and NULL for every
       one it does not, so an optional call stays optional: a link asks keepalive() for
       -ENOTSUP, and a forwarder that always existed would turn that into -ENOTCONN. The three
       the counting rides on are always ours; forwarding to a call the inner table lacks is
       already a no-op. */
    const struct mesh_protocol_ops *from = inner->ops;
    tap->ops = *from;
    tap->ops.attach = mesh_protocol_tap_attach;
    tap->ops.detach = mesh_protocol_tap_detach;
    tap->ops.begin = from->begin != NULL ? mesh_protocol_tap_begin : NULL;
    tap->ops.receive = mesh_protocol_tap_receive;
    tap->ops.frame_failed = from->frame_failed != NULL ? mesh_protocol_tap_frame_failed : NULL;
    tap->ops.tick = from->tick != NULL ? mesh_protocol_tap_tick : NULL;
    tap->ops.silent = from->silent != NULL ? mesh_protocol_tap_silent : NULL;
    tap->ops.keepalive = from->keepalive != NULL ? mesh_protocol_tap_keepalive : NULL;
    return (struct mesh_protocol){.ops = &tap->ops, .self = tap};
}
