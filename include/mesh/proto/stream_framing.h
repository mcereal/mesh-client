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
 * BLE uses no framing at all - one bare protobuf per GATT operation.
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

/* What a parser can hold: one whole frame of the largest framing this client speaks. Every
   framing below must fit in it, which is checked where a link is handed one. */
#define MESH_STREAM_PARSER_CAPACITY (MESH_STREAM_FRAME_HEADER_LEN + MESH_STREAM_FRAME_MAX_PAYLOAD)

/* The state a framing's push() carries between reads. Nothing in it is Meshtastic's: it is a
   buffer, a fill level and two counters, and a second framing parses into the same struct. */
struct mesh_stream_parser {
    uint8_t buffer[MESH_STREAM_PARSER_CAPACITY];
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

/*
 * A framing, as a table a stream link is handed rather than a format it is written against.
 *
 * The functions above are Meshtastic's framing, and for as long as they were the only one the
 * stream link called them by name. A second mesh protocol frames its serial and TCP bytes
 * differently - its own header, its own length, its own idea of what sits between frames - and
 * nothing else about the link changes: the descriptor, the outbound queue, the read bound and
 * the fatal-error rules are the same. So the link holds one of these and calls through it, and
 * a new framing is a new table in `src/proto/` rather than an edit to every stream transport.
 *
 * Which framing a link uses is the protocol's to say (mesh/core/protocol.h), not the port's: the
 * same USB serial device speaks whichever protocol its firmware does.
 */
struct mesh_stream_framing {
    /* For log lines and tests. A literal. */
    const char *name;
    /* The largest frame on the wire, header included. It must be at most
       MESH_STREAM_PARSER_CAPACITY, since that is what the parser and the link's slots hold. */
    size_t max_frame;
    /* Feeds one read's worth of bytes; the contract is mesh_stream_parser_push()'s. The parser
       is reset by the link, which is framing-independent. */
    void (*push)(struct mesh_stream_parser *parser, const uint8_t *data, size_t len,
                 const struct mesh_stream_parser_callbacks *callbacks);
    /* Wraps one payload; the contract, and the errno values, are mesh_stream_frame_encode()'s. */
    int (*encode)(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len,
                  size_t *written);
    /* The byte a serial link sends a run of before its first frame, to push a radio's parser
       out of whatever half-frame a previous client left it in - or -1 for a framing that has no
       such thing, in which case the link skips the burst and the settle after it. */
    int wake_byte;
};

/* 0x94 0xC3, a big-endian length, one protobuf - the functions above, as a table. */
extern const struct mesh_stream_framing mesh_stream_framing_meshtastic;

/*
 * MeshCore's companion framing: one direction byte, a 16-bit *little*-endian length, the frame.
 *
 *     '<' len_lo len_hi <frame>      app to radio
 *     '>' len_lo len_hi <frame>      radio to app
 *
 * The direction byte is the only marker, so the parser reads '>' as a header and everything
 * else as junk, exactly as the Meshtastic parser reads 0x94. The firmware refuses a frame
 * longer than MAX_FRAME_SIZE (src/helpers/BaseSerialInterface.h), and a header claiming more
 * is taken for junk and resynced past. No wake burst: the firmware's receiver is a three-state
 * machine that a stray byte cannot wedge.
 */
#define MESH_MESHCORE_FRAME_TO_RADIO '<'
#define MESH_MESHCORE_FRAME_FROM_RADIO '>'
#define MESH_MESHCORE_FRAME_HEADER_LEN 3U
#define MESH_MESHCORE_FRAME_MAX_PAYLOAD 176U

void mesh_meshcore_parser_push(struct mesh_stream_parser *parser, const uint8_t *data, size_t len,
                               const struct mesh_stream_parser_callbacks *callbacks);
int mesh_meshcore_frame_encode(const uint8_t *payload, size_t payload_len, uint8_t *out,
                               size_t out_len, size_t *written);

extern const struct mesh_stream_framing mesh_stream_framing_meshcore;

#ifdef __cplusplus
}
#endif
