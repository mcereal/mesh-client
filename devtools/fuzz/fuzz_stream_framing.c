/*
 * The serial link's frame parser, fed whatever arrives.
 *
 * This is one of the two doors bytes we did not write come in through: a radio on the other end
 * of a USB cable, interleaving its own text log with framed protobufs on the same port. Treating
 * anything that is not a well-formed header as junk and resyncing past it is most of what the
 * parser does, and it is the part worth fuzzing - a resync that loses a byte drops a message
 * silently, and one that gains a byte reads past the end of the parser's own buffer.
 *
 * Memory safety is not the only oracle here, because a parser can be perfectly memory-safe and
 * still be wrong. Two identities hold over any sequence of pushes, and both are checked on every
 * input:
 *
 *   1. Every byte pushed is accounted for exactly once - inside a delivered payload, among the
 *      four header bytes a delivered frame consumes, counted as dropped, or still buffered
 *      waiting for the rest of its frame.
 *   2. Every byte handed to the text callback is a byte counted as dropped, and the reverse:
 *      `stream_framing.c` increments the counter at each of the three sites that emit text, so
 *      an emit that forgot its counter, or a drop that forgot the radio's log, breaks this.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/proto/stream_framing.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fuzz_tally {
    size_t payload_bytes; /* bytes delivered inside frames */
    size_t frames;
    size_t text_bytes;
    size_t checksum; /* forces a read of every payload byte */
};

static void fuzz_broke(const char *what) {
    fprintf(stderr, "stream framing contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

static void fuzz_on_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct fuzz_tally *tally = (struct fuzz_tally *)ctx;
    if (len > MESH_STREAM_FRAME_MAX_PAYLOAD) {
        fuzz_broke("a payload longer than the parser's own maximum");
    }
    if (payload == NULL && len > 0U) {
        fuzz_broke("a payload length with no payload");
    }
    /* Read every byte. A length that outruns the buffer is only a bug once somebody reads it,
       and the sanitizer can only see the read. */
    for (size_t i = 0; i < len; ++i) {
        tally->checksum += payload[i];
    }
    tally->payload_bytes += len;
    tally->frames += 1U;
}

static void fuzz_on_text(const uint8_t *text, size_t len, void *ctx) {
    struct fuzz_tally *tally = (struct fuzz_tally *)ctx;
    if (text == NULL && len > 0U) {
        fuzz_broke("a text run with no text");
    }
    for (size_t i = 0; i < len; ++i) {
        tally->checksum += text[i];
    }
    tally->text_bytes += len;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0U) {
        return 0;
    }

    /* The first byte chooses how the rest is split across push() calls, because a stream
       parser's interesting states are the ones that straddle a read: half a header, or a length
       whose payload has not arrived yet. Handing the whole input over in one call would never
       build them, and the transport never does either - it pushes one read() at a time. */
    const size_t chunk = (size_t)(data[0] % 64U) + 1U;
    const uint8_t *stream = data + 1;
    const size_t stream_len = size - 1U;

    struct mesh_stream_parser parser;
    mesh_stream_parser_reset(&parser);
    struct fuzz_tally tally = {0};
    const struct mesh_stream_parser_callbacks callbacks = {
        .on_frame = fuzz_on_frame,
        .on_text = fuzz_on_text,
        .ctx = &tally,
    };

    for (size_t offset = 0; offset < stream_len; offset += chunk) {
        const size_t remaining = stream_len - offset;
        mesh_stream_parser_push(&parser, stream + offset, remaining < chunk ? remaining : chunk,
                                &callbacks);
        if (parser.len > MESH_STREAM_FRAME_HEADER_LEN + MESH_STREAM_FRAME_MAX_PAYLOAD) {
            fuzz_broke("more buffered than the buffer holds");
        }
    }

    const size_t accounted = tally.payload_bytes +
                             (tally.frames * (size_t)MESH_STREAM_FRAME_HEADER_LEN) +
                             parser.dropped_bytes + parser.len;
    if (accounted != stream_len) {
        fprintf(stderr,
                "pushed %zu, accounted %zu (payload %zu, frames %zu, dropped %zu, held %zu)\n",
                stream_len, accounted, tally.payload_bytes, tally.frames, parser.dropped_bytes,
                parser.len);
        fuzz_broke("a byte was lost or counted twice");
    }
    if (tally.text_bytes != parser.dropped_bytes) {
        fprintf(stderr, "text %zu, dropped %zu\n", tally.text_bytes, parser.dropped_bytes);
        fuzz_broke("a dropped byte the radio's log never saw, or the reverse");
    }
    if (tally.frames != parser.frames) {
        fuzz_broke("the frame counter and the frames delivered disagree");
    }
    return 0;
}
