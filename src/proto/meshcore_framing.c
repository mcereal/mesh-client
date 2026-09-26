#include "mesh/proto/stream_framing.h"

#include <errno.h>
#include <string.h>

/* The shape is stream_framing.c's - the same parser struct, the same resync rules - over a
   one-byte marker and a little-endian length. Kept apart because the two headers share no
   byte, and a parser that tried both would read one protocol's log as the other's frames. */

static void mesh_meshcore_emit_text(const struct mesh_stream_parser_callbacks *callbacks,
                                    const uint8_t *text, size_t len) {
    if (callbacks != NULL && callbacks->on_text != NULL && len > 0U) {
        callbacks->on_text(text, len, callbacks->ctx);
    }
}

static void mesh_meshcore_consume(struct mesh_stream_parser *parser, size_t count) {
    if (count >= parser->len) {
        parser->len = 0U;
        return;
    }
    memmove(parser->buffer, parser->buffer + count, parser->len - count);
    parser->len -= count;
}

static void mesh_meshcore_drain(struct mesh_stream_parser *parser,
                                const struct mesh_stream_parser_callbacks *callbacks) {
    while (parser->len > 0U) {
        if (parser->buffer[0] != (uint8_t)MESH_MESHCORE_FRAME_FROM_RADIO) {
            size_t junk = 1U;
            while (junk < parser->len &&
                   parser->buffer[junk] != (uint8_t)MESH_MESHCORE_FRAME_FROM_RADIO) {
                ++junk;
            }
            mesh_meshcore_emit_text(callbacks, parser->buffer, junk);
            parser->dropped_bytes += junk;
            mesh_meshcore_consume(parser, junk);
            continue;
        }

        if (parser->len < MESH_MESHCORE_FRAME_HEADER_LEN) {
            return;
        }

        const size_t payload_len = (size_t)parser->buffer[1] | ((size_t)parser->buffer[2] << 8U);
        if (payload_len == 0U || payload_len > MESH_MESHCORE_FRAME_MAX_PAYLOAD) {
            /* A '>' in the radio's log, or a header no firmware writes: drop the marker only,
               since the next byte may begin a real frame. */
            mesh_meshcore_emit_text(callbacks, parser->buffer, 1U);
            parser->dropped_bytes += 1U;
            mesh_meshcore_consume(parser, 1U);
            continue;
        }

        if (parser->len < MESH_MESHCORE_FRAME_HEADER_LEN + payload_len) {
            return;
        }

        if (callbacks != NULL && callbacks->on_frame != NULL) {
            callbacks->on_frame(parser->buffer + MESH_MESHCORE_FRAME_HEADER_LEN, payload_len,
                                callbacks->ctx);
        }
        parser->frames += 1U;
        mesh_meshcore_consume(parser, MESH_MESHCORE_FRAME_HEADER_LEN + payload_len);
    }
}

void mesh_meshcore_parser_push(struct mesh_stream_parser *parser, const uint8_t *data, size_t len,
                               const struct mesh_stream_parser_callbacks *callbacks) {
    if (parser == NULL || (len > 0U && data == NULL)) {
        return;
    }
    while (len > 0U) {
        size_t space = MESH_STREAM_PARSER_CAPACITY - parser->len;
        if (space == 0U) {
            mesh_meshcore_emit_text(callbacks, parser->buffer, 1U);
            parser->dropped_bytes += 1U;
            mesh_meshcore_consume(parser, 1U);
            space = 1U;
        }
        const size_t copy = len < space ? len : space;
        memcpy(parser->buffer + parser->len, data, copy);
        parser->len += copy;
        data += copy;
        len -= copy;
        mesh_meshcore_drain(parser, callbacks);
    }
}

int mesh_meshcore_frame_encode(const uint8_t *payload, size_t payload_len, uint8_t *out,
                               size_t out_len, size_t *written) {
    if ((payload_len > 0U && payload == NULL) || out == NULL || written == NULL) {
        return -EINVAL;
    }
    if (payload_len > MESH_MESHCORE_FRAME_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (out_len < MESH_MESHCORE_FRAME_HEADER_LEN + payload_len) {
        return -ENOSPC;
    }
    out[0] = (uint8_t)MESH_MESHCORE_FRAME_TO_RADIO;
    out[1] = (uint8_t)(payload_len & 0xFFU);
    out[2] = (uint8_t)((payload_len >> 8U) & 0xFFU);
    if (payload_len > 0U) {
        memcpy(out + MESH_MESHCORE_FRAME_HEADER_LEN, payload, payload_len);
    }
    *written = MESH_MESHCORE_FRAME_HEADER_LEN + payload_len;
    return 0;
}

const struct mesh_stream_framing mesh_stream_framing_meshcore = {
    .name = "meshcore",
    .max_frame = MESH_MESHCORE_FRAME_HEADER_LEN + MESH_MESHCORE_FRAME_MAX_PAYLOAD,
    .push = mesh_meshcore_parser_push,
    .encode = mesh_meshcore_frame_encode,
    .wake_byte = -1,
};
