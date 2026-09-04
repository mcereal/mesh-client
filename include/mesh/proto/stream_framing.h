#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Meshtastic's stream framing, used by the serial and TCP client APIs: two magic bytes, a
 * 16-bit big-endian payload length, then one raw ToRadio/FromRadio protobuf.
 *
 *     0x94 0xC3 len_hi len_lo <payload>
 *
 * This is NOT what src/proto/framing.c does (that is a homegrown varint prefix), and BLE uses
 * no framing at all - one bare protobuf per GATT operation.
 *
 * The radio interleaves its own text log with the frames on the same port, so the parser must
 * treat anything that is not a well-formed header as junk and resync. Runs of junk are handed
 * to the optional text callback, which is how the radio's log reaches ours.
 */

#define MESH_STREAM_FRAME_START1 0x94U
#define MESH_STREAM_FRAME_START2 0xC3U
#define MESH_STREAM_FRAME_HEADER_LEN 4U
#define MESH_STREAM_FRAME_MAX_PAYLOAD 512U

/* One decoded protobuf, without the header. */
typedef void (*mesh_stream_frame_fn)(const uint8_t *payload, size_t len, void *ctx);
/* A run of bytes that was not part of a frame: the radio's debug log, or line noise. Not NUL
   terminated and not sanitised. */
typedef void (*mesh_stream_text_fn)(const uint8_t *text, size_t len, void *ctx);

struct mesh_stream_parser_callbacks {
    mesh_stream_frame_fn on_frame;
    mesh_stream_text_fn on_text; /* may be NULL */
    void *ctx;
};

struct mesh_stream_parser {
    uint8_t buffer[MESH_STREAM_FRAME_HEADER_LEN + MESH_STREAM_FRAME_MAX_PAYLOAD];
    size_t len;
    /* Diagnostics: frames delivered, and bytes thrown away resyncing. */
    size_t frames;
    size_t dropped_bytes;
};

void mesh_stream_parser_reset(struct mesh_stream_parser *parser);

/* Feeds one read() worth of bytes. Complete frames are delivered to `callbacks->on_frame` in
   order, junk between them to `callbacks->on_text`. A partial frame is kept for the next call. */
void mesh_stream_parser_push(struct mesh_stream_parser *parser, const uint8_t *data, size_t len,
                             const struct mesh_stream_parser_callbacks *callbacks);

/* Writes header + payload into `out`. Returns 0, -EINVAL on bad arguments, -EMSGSIZE when the
   payload exceeds MESH_STREAM_FRAME_MAX_PAYLOAD, -ENOSPC when `out` is too small. */
int mesh_stream_frame_encode(const uint8_t *payload, size_t payload_len, uint8_t *out,
                             size_t out_len, size_t *written);

#ifdef __cplusplus
}
#endif
