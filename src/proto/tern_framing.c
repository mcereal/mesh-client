#include "mesh/proto/stream_framing.h"

#include <errno.h>
#include <string.h>

/* The shape is meshcore_framing.c's - the same parser struct, the same resync rules - over a
   two-byte magic, a big-endian length and a CRC after the frame. The CRC is what makes the
   resync honest: a header found in noise is caught when its check fails, and only its first
   byte is given up as text, since the next may begin a real frame. */

uint16_t mesh_tern_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

static void mesh_tern_emit_text(const struct mesh_stream_parser_callbacks *callbacks,
                                const uint8_t *text, size_t len) {
    if (callbacks != NULL && callbacks->on_text != NULL && len > 0U) {
        callbacks->on_text(text, len, callbacks->ctx);
    }
}

static void mesh_tern_consume(struct mesh_stream_parser *parser, size_t count) {
    if (count >= parser->len) {
        parser->len = 0U;
        return;
    }
    memmove(parser->buffer, parser->buffer + count, parser->len - count);
    parser->len -= count;
}

/* The 0xF5 at the front is not a frame's: give it up as text and look again after it. */
static void mesh_tern_reject_magic(struct mesh_stream_parser *parser,
                                   const struct mesh_stream_parser_callbacks *callbacks) {
    mesh_tern_emit_text(callbacks, parser->buffer, 1U);
    parser->dropped_bytes += 1U;
    mesh_tern_consume(parser, 1U);
}

static void mesh_tern_drain(struct mesh_stream_parser *parser,
                            const struct mesh_stream_parser_callbacks *callbacks) {
    while (parser->len > 0U) {
        if (parser->buffer[0] != MESH_TERN_FRAME_MAGIC1) {
            size_t junk = 1U;
            while (junk < parser->len && parser->buffer[junk] != MESH_TERN_FRAME_MAGIC1) {
                ++junk;
            }
            mesh_tern_emit_text(callbacks, parser->buffer, junk);
            parser->dropped_bytes += junk;
            mesh_tern_consume(parser, junk);
            continue;
        }
        if (parser->len < 2U) {
            return;
        }
        if (parser->buffer[1] != MESH_TERN_FRAME_MAGIC2) {
            mesh_tern_reject_magic(parser, callbacks);
            continue;
        }
        if (parser->len < MESH_TERN_FRAME_HEADER_LEN) {
            return;
        }
        const size_t payload_len = ((size_t)parser->buffer[2] << 8U) | (size_t)parser->buffer[3];
        if (payload_len < MESH_TERN_FRAME_MIN_PAYLOAD ||
            payload_len > MESH_TERN_FRAME_MAX_PAYLOAD) {
            mesh_tern_reject_magic(parser, callbacks);
            continue;
        }
        const size_t whole = MESH_TERN_FRAME_HEADER_LEN + payload_len + MESH_TERN_FRAME_CRC_LEN;
        if (parser->len < whole) {
            return;
        }
        /* Over the length and the frame, not the magic. */
        const uint16_t crc = mesh_tern_crc16(parser->buffer + 2U, 2U + payload_len);
        const size_t at = MESH_TERN_FRAME_HEADER_LEN + payload_len;
        const uint16_t carried =
            (uint16_t)(((uint16_t)parser->buffer[at] << 8U) | (uint16_t)parser->buffer[at + 1U]);
        if (crc != carried) {
            mesh_tern_reject_magic(parser, callbacks);
            continue;
        }
        if (callbacks != NULL && callbacks->on_frame != NULL) {
            callbacks->on_frame(parser->buffer + MESH_TERN_FRAME_HEADER_LEN, payload_len,
                                callbacks->ctx);
        }
        parser->frames += 1U;
        mesh_tern_consume(parser, whole);
    }
}

void mesh_tern_parser_push(struct mesh_stream_parser *parser, const uint8_t *data, size_t len,
                           const struct mesh_stream_parser_callbacks *callbacks) {
    if (parser == NULL || (len > 0U && data == NULL)) {
        return;
    }
    while (len > 0U) {
        size_t space = MESH_STREAM_PARSER_CAPACITY - parser->len;
        if (space == 0U) {
            mesh_tern_reject_magic(parser, callbacks);
            space = 1U;
        }
        const size_t copy = len < space ? len : space;
        memcpy(parser->buffer + parser->len, data, copy);
        parser->len += copy;
        data += copy;
        len -= copy;
        mesh_tern_drain(parser, callbacks);
    }
}

int mesh_tern_frame_encode(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len,
                           size_t *written) {
    if (payload == NULL || out == NULL || written == NULL ||
        payload_len < MESH_TERN_FRAME_MIN_PAYLOAD) {
        return -EINVAL;
    }
    if (payload_len > MESH_TERN_FRAME_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    const size_t whole = MESH_TERN_FRAME_HEADER_LEN + payload_len + MESH_TERN_FRAME_CRC_LEN;
    if (out_len < whole) {
        return -ENOSPC;
    }
    out[0] = MESH_TERN_FRAME_MAGIC1;
    out[1] = MESH_TERN_FRAME_MAGIC2;
    out[2] = (uint8_t)(payload_len >> 8U);
    out[3] = (uint8_t)(payload_len & 0xFFU);
    memcpy(out + MESH_TERN_FRAME_HEADER_LEN, payload, payload_len);
    const uint16_t crc = mesh_tern_crc16(out + 2U, 2U + payload_len);
    out[MESH_TERN_FRAME_HEADER_LEN + payload_len] = (uint8_t)(crc >> 8U);
    out[MESH_TERN_FRAME_HEADER_LEN + payload_len + 1U] = (uint8_t)(crc & 0xFFU);
    *written = whole;
    return 0;
}

const struct mesh_stream_framing mesh_stream_framing_tern = {
    .name = "tern",
    .max_frame = MESH_TERN_FRAME_HEADER_LEN + MESH_TERN_FRAME_MAX_PAYLOAD + MESH_TERN_FRAME_CRC_LEN,
    .push = mesh_tern_parser_push,
    .encode = mesh_tern_frame_encode,
    .wake_byte = -1,
};
