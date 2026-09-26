#include "mesh/core/protocol.h"

#include <errno.h>

bool mesh_protocol_bound(const struct mesh_protocol *protocol) {
    return protocol != NULL && protocol->ops != NULL;
}

const char *mesh_protocol_name(const struct mesh_protocol *protocol) {
    return mesh_protocol_bound(protocol) && protocol->ops->name != NULL ? protocol->ops->name
                                                                         : "none";
}

const struct mesh_stream_framing *mesh_protocol_stream_framing(
    const struct mesh_protocol *protocol) {
    return mesh_protocol_bound(protocol) ? protocol->ops->stream_framing : NULL;
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

void mesh_protocol_receive(const struct mesh_protocol *protocol, const uint8_t *frame,
                           size_t len) {
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
