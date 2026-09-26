#include "mesh/transport/stream_link.h"

#include "inkwell/base/log.h"

#include <errno.h>
#include <string.h>

/*
 * This file is what is left of the stream link after the descriptor went to inkwell: the frame
 * parser, the protocol, and the two callbacks that join them to a byte stream. Which framing
 * the parser speaks is the protocol's, so nothing below names one.
 *
 * The seam is worth reading as a pair. Going out, this frames a packet and hands inkwell the
 * bytes; going in, inkwell hands back whatever arrived and this pushes it at the parser. Neither
 * direction lets the framing down or the descriptor up.
 */

static void mesh_stream_link_on_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct mesh_stream_link *link = (struct mesh_stream_link *)ctx;
    link->frames_received += 1U;
    mesh_protocol_receive(&link->protocol, payload, len);
}

/* Whatever sits between frames is the radio's own log. Surface it at debug, one line at a time,
   with control bytes stripped so it cannot scribble on the terminal. */
static void mesh_stream_link_on_text(const uint8_t *text, size_t len, void *ctx) {
    struct mesh_stream_link *link = (struct mesh_stream_link *)ctx;
    char line[160];
    size_t out = 0U;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = text[i];
        if (byte == '\n' || byte == '\r') {
            if (out > 0U) {
                line[out] = '\0';
                inkwell_log_debug(link->tag, "radio: %s", line);
                out = 0U;
            }
            continue;
        }
        if (out + 1U >= sizeof line) {
            line[out] = '\0';
            inkwell_log_debug(link->tag, "radio: %s", line);
            out = 0U;
        }
        line[out++] = (byte >= 0x20U && byte < 0x7FU) ? (char)byte : '.';
    }
    if (out > 0U) {
        line[out] = '\0';
        inkwell_log_debug(link->tag, "radio: %s", line);
    }
}

/* Bytes off the descriptor go at the parser, which calls back with frames and with the radio's
   text. Registered once at init, because inkwell keeps the sink across an open/close cycle. */
static void mesh_stream_link_on_bytes(void *userdata, const uint8_t *bytes, size_t len) {
    struct mesh_stream_link *link = (struct mesh_stream_link *)userdata;
    if (link->framing == NULL) {
        return;
    }
    const struct mesh_stream_parser_callbacks callbacks = {
        .on_frame = mesh_stream_link_on_frame,
        .on_text = mesh_stream_link_on_text,
        .ctx = link,
    };
    link->framing->push(&link->parser, bytes, len, &callbacks);
}

/* A packet that was queued and never went out. The id is the message log's, handed to inkwell
   with the bytes and handed back here unchanged - inkwell never learns what it names. */
static void mesh_stream_link_on_dropped(void *userdata, uint32_t packet_id) {
    struct mesh_stream_link *link = (struct mesh_stream_link *)userdata;
    mesh_protocol_frame_failed(&link->protocol, packet_id);
}

void mesh_stream_link_init(struct mesh_stream_link *link, const char *tag,
                           const struct mesh_protocol *protocol) {
    if (link == NULL) {
        return;
    }
    memset(link, 0, sizeof *link);
    link->tag = tag != NULL ? tag : "link";
    mesh_stream_link_set_protocol(link, protocol);
    (void)inkwell_stream_init(&link->stream, link->tag, link->slots, MESH_STREAM_LINK_MAX_OUTBOUND,
                              link->slot_bytes, MESH_STREAM_LINK_SLOT_BYTES);
    inkwell_stream_set_sink(&link->stream, mesh_stream_link_on_bytes, mesh_stream_link_on_dropped,
                            link);
    mesh_stream_parser_reset(&link->parser);
}

void mesh_stream_link_set_protocol(struct mesh_stream_link *link,
                                   const struct mesh_protocol *protocol) {
    if (link == NULL) {
        return;
    }
    link->protocol = protocol != NULL ? *protocol : (struct mesh_protocol){NULL, NULL};
    const struct mesh_stream_framing *framing = mesh_protocol_stream_framing(&link->protocol);
    if (framing != NULL && (framing->push == NULL || framing->encode == NULL ||
                            framing->max_frame > MESH_STREAM_PARSER_CAPACITY)) {
        inkwell_log_warn(link->tag, "%s framing does not fit this link; frames are dropped",
                         framing->name != NULL ? framing->name : "unnamed");
        framing = NULL;
    }
    link->framing = framing;
    mesh_stream_parser_reset(&link->parser);
}

bool mesh_stream_link_is_open(const struct mesh_stream_link *link) {
    return link != NULL && inkwell_stream_is_open(&link->stream);
}

int mesh_stream_link_flush(struct mesh_stream_link *link) {
    if (link == NULL) {
        return -EINVAL;
    }
    return inkwell_stream_flush(&link->stream);
}

int mesh_stream_link_send(struct mesh_stream_link *link, const uint8_t *packet, size_t len,
                          uint32_t packet_id) {
    if (link == NULL) {
        return -EINVAL;
    }
    if (link->framing == NULL) {
        return -ENOTCONN;
    }
    /*
     * Framed here and not down there. A header is one protocol's, and a byte stream that knew
     * about it would be a byte stream with one protocol's opinion in it.
     */
    uint8_t framed[MESH_STREAM_LINK_SLOT_BYTES];
    size_t written = 0U;
    const int encoded = link->framing->encode(packet, len, framed, sizeof framed, &written);
    if (encoded < 0) {
        return encoded;
    }
    return inkwell_stream_send(&link->stream, framed, written, packet_id);
}

int mesh_stream_link_write_raw(struct mesh_stream_link *link, const uint8_t *data, size_t len) {
    if (link == NULL) {
        return -EINVAL;
    }
    return inkwell_stream_write_raw(&link->stream, data, len);
}

int mesh_stream_link_pump(struct mesh_stream_link *link) {
    if (link == NULL) {
        return -EINVAL;
    }
    return inkwell_stream_pump(&link->stream);
}

int mesh_stream_link_open(struct mesh_stream_link *link, int fd, enum mesh_stream_link_kind kind,
                          struct inkwell_loop *loop, inkwell_loop_callback callback,
                          void *userdata) {
    if (link == NULL) {
        return -EINVAL;
    }
    const int opened = inkwell_stream_open(&link->stream, fd, (enum inkwell_stream_kind)kind, loop,
                                           callback, userdata);
    if (opened < 0) {
        return opened;
    }
    /* The parser is reset on open rather than on close as well, so a link reopened on a new
       descriptor cannot inherit half a frame from the last one. */
    mesh_stream_parser_reset(&link->parser);
    link->frames_received = 0U;
    return 0;
}

int mesh_stream_link_open_socket(struct mesh_stream_link *link, inkwell_socket socket,
                                 struct inkwell_loop *loop, inkwell_loop_callback callback,
                                 void *userdata) {
    if (link == NULL) {
        return -EINVAL;
    }
    const int opened = inkwell_stream_open_socket(&link->stream, socket, loop, callback, userdata);
    if (opened < 0) {
        return opened;
    }
    mesh_stream_parser_reset(&link->parser);
    link->frames_received = 0U;
    return 0;
}

void mesh_stream_link_close(struct mesh_stream_link *link) {
    if (link == NULL) {
        return;
    }
    /* inkwell reports the queue on the way through, which is what fails those packets against
       the session - including on an already-closed link, where a transport that failed between
       queueing and opening still owes them a verdict. */
    inkwell_stream_close(&link->stream);
    mesh_stream_parser_reset(&link->parser);
}

struct mesh_stream_link_stats mesh_stream_link_stats(const struct mesh_stream_link *link) {
    struct mesh_stream_link_stats stats = {0U, 0U, 0U};
    if (link == NULL) {
        return stats;
    }
    stats.frames_received = link->frames_received;
    stats.bytes_received = inkwell_stream_bytes_received(&link->stream);
    stats.junk_bytes = link->parser.dropped_bytes;
    return stats;
}
