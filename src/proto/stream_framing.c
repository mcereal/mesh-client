#include "mesh/proto/stream_framing.h"

#include <errno.h>
#include <string.h>

#define MESH_STREAM_PARSER_CAPACITY (MESH_STREAM_FRAME_HEADER_LEN + MESH_STREAM_FRAME_MAX_PAYLOAD)

void mesh_stream_parser_reset(struct mesh_stream_parser *parser) {
    if (parser == NULL) {
        return;
    }
    parser->len = 0U;
    parser->frames = 0U;
    parser->dropped_bytes = 0U;
}

static void mesh_stream_emit_text(const struct mesh_stream_parser_callbacks *callbacks,
                                  const uint8_t *text, size_t len) {
    if (callbacks != NULL && callbacks->on_text != NULL && len > 0U) {
        callbacks->on_text(text, len, callbacks->ctx);
    }
}

static void mesh_stream_consume(struct mesh_stream_parser *parser, size_t count) {
    if (count >= parser->len) {
        parser->len = 0U;
        return;
    }
    memmove(parser->buffer, parser->buffer + count, parser->len - count);
    parser->len -= count;
}

/* Pulls every complete frame out of the buffer, discarding whatever sits between them. Returns
   with the buffer holding at most one partial frame. */
static void mesh_stream_drain(struct mesh_stream_parser *parser,
                              const struct mesh_stream_parser_callbacks *callbacks) {
    while (parser->len > 0U) {
        if (parser->buffer[0] != MESH_STREAM_FRAME_START1) {
            /* Everything up to the next possible start byte is the radio's log, not a frame. */
            size_t junk = 1U;
            while (junk < parser->len && parser->buffer[junk] != MESH_STREAM_FRAME_START1) {
                ++junk;
            }
            mesh_stream_emit_text(callbacks, parser->buffer, junk);
            parser->dropped_bytes += junk;
            mesh_stream_consume(parser, junk);
            continue;
        }

        if (parser->len < 2U) {
            return; /* a lone 0x94 at the end of a read; it may still become a header */
        }

        if (parser->buffer[1] != MESH_STREAM_FRAME_START2) {
            /* Drop only the start byte: the byte after it may itself begin a real frame. */
            mesh_stream_emit_text(callbacks, parser->buffer, 1U);
            parser->dropped_bytes += 1U;
            mesh_stream_consume(parser, 1U);
            continue;
        }

        if (parser->len < MESH_STREAM_FRAME_HEADER_LEN) {
            return;
        }

        const size_t payload_len =
            ((size_t)parser->buffer[2] << 8U) | (size_t)parser->buffer[3];
        if (payload_len > MESH_STREAM_FRAME_MAX_PAYLOAD) {
            /* 0x94 0xC3 can occur inside a log line; resync past the start byte. */
            mesh_stream_emit_text(callbacks, parser->buffer, 1U);
            parser->dropped_bytes += 1U;
            mesh_stream_consume(parser, 1U);
            continue;
        }

        if (parser->len < MESH_STREAM_FRAME_HEADER_LEN + payload_len) {
            return; /* header seen, payload still in flight */
        }

        if (callbacks != NULL && callbacks->on_frame != NULL) {
            callbacks->on_frame(parser->buffer + MESH_STREAM_FRAME_HEADER_LEN, payload_len,
                                callbacks->ctx);
        }
        parser->frames += 1U;
        mesh_stream_consume(parser, MESH_STREAM_FRAME_HEADER_LEN + payload_len);
    }
}

void mesh_stream_parser_push(struct mesh_stream_parser *parser, const uint8_t *data, size_t len,
                             const struct mesh_stream_parser_callbacks *callbacks) {
    if (parser == NULL || (len > 0U && data == NULL)) {
        return;
    }

    while (len > 0U) {
        size_t space = MESH_STREAM_PARSER_CAPACITY - parser->len;
        if (space == 0U) {
            /* Unreachable: the capacity holds a whole frame, so a drain always makes room.
               Guard anyway so a malformed stream can never wedge the loop. */
            mesh_stream_emit_text(callbacks, parser->buffer, 1U);
            parser->dropped_bytes += 1U;
            mesh_stream_consume(parser, 1U);
            space = 1U;
        }

        const size_t copy = len < space ? len : space;
        memcpy(parser->buffer + parser->len, data, copy);
        parser->len += copy;
        data += copy;
        len -= copy;

        mesh_stream_drain(parser, callbacks);
    }
}

int mesh_stream_frame_encode(const uint8_t *payload, size_t payload_len, uint8_t *out,
                             size_t out_len, size_t *written) {
    if ((payload_len > 0U && payload == NULL) || out == NULL || written == NULL) {
        return -EINVAL;
    }
    if (payload_len > MESH_STREAM_FRAME_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (out_len < MESH_STREAM_FRAME_HEADER_LEN + payload_len) {
        return -ENOSPC;
    }

    out[0] = (uint8_t)MESH_STREAM_FRAME_START1;
    out[1] = (uint8_t)MESH_STREAM_FRAME_START2;
    out[2] = (uint8_t)((payload_len >> 8U) & 0xFFU);
    out[3] = (uint8_t)(payload_len & 0xFFU);
    if (payload_len > 0U) {
        memcpy(out + MESH_STREAM_FRAME_HEADER_LEN, payload, payload_len);
    }
    *written = MESH_STREAM_FRAME_HEADER_LEN + payload_len;
    return 0;
}
