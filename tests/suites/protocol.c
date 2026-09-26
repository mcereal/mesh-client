#define _POSIX_C_SOURCE 200809L

/*
 * The link's side of mesh/core/protocol.h: what a link does with a protocol it was handed, and
 * what an unbound one answers.
 *
 * The protocol and the framing below are fakes on purpose. Every other stream-link case drives
 * the Meshtastic session, which is exactly the case a hard-coded call would also pass; these are
 * the ones that fail if the link stops going through the table.
 */

#include "framework/mesh_test.h"

#include "mesh/core/protocol.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/transport/stream_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ a framing nobody ships */

/* 0x7E, one length byte, the payload: short enough to read at a glance and unlike Meshtastic's
   in both the start byte and the width of the length, so a link that fell back to the real
   framing would fail both directions below. */
#define FAKE_START 0x7EU
#define FAKE_HEADER 2U

static void fake_push(struct mesh_stream_parser *parser, const uint8_t *data, size_t len,
                      const struct mesh_stream_parser_callbacks *callbacks) {
    for (size_t i = 0; i < len; ++i) {
        if (parser->len == 0U && data[i] != FAKE_START) {
            parser->dropped_bytes += 1U;
            continue;
        }
        parser->buffer[parser->len++] = data[i];
        if (parser->len >= FAKE_HEADER && parser->len == FAKE_HEADER + parser->buffer[1]) {
            parser->frames += 1U;
            callbacks->on_frame(parser->buffer + FAKE_HEADER, parser->buffer[1], callbacks->ctx);
            parser->len = 0U;
        }
    }
}

static int fake_encode(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len,
                       size_t *written) {
    if (payload_len > 0xFFU) {
        return -EMSGSIZE;
    }
    if (out_len < FAKE_HEADER + payload_len) {
        return -ENOSPC;
    }
    out[0] = FAKE_START;
    out[1] = (uint8_t)payload_len;
    memcpy(out + FAKE_HEADER, payload, payload_len);
    *written = FAKE_HEADER + payload_len;
    return 0;
}

static const struct mesh_stream_framing k_fake_framing = {
    .name = "fake",
    .max_frame = FAKE_HEADER + 0xFFU,
    .push = fake_push,
    .encode = fake_encode,
    .wake_byte = -1,
};

/* One more byte than the parser holds: a framing the link must refuse rather than overrun. */
static const struct mesh_stream_framing k_oversized_framing = {
    .name = "oversized",
    .max_frame = MESH_STREAM_PARSER_CAPACITY + 1U,
    .push = fake_push,
    .encode = fake_encode,
    .wake_byte = -1,
};

/* ------------------------------------------------------------------ a protocol that records */

struct fake_protocol {
    uint8_t received[64];
    size_t received_len;
    size_t frames;
    uint32_t failed_id;
};

static void fake_receive(void *self, const uint8_t *frame, size_t len) {
    struct fake_protocol *fake = (struct fake_protocol *)self;
    fake->frames += 1U;
    fake->received_len = len < sizeof fake->received ? len : sizeof fake->received;
    memcpy(fake->received, frame, fake->received_len);
}

static void fake_frame_failed(void *self, uint32_t frame_id) {
    ((struct fake_protocol *)self)->failed_id = frame_id;
}

static const struct mesh_protocol_ops k_fake_ops = {
    .name = "fake",
    .stream_framing = &k_fake_framing,
    .receive = fake_receive,
    .frame_failed = fake_frame_failed,
};

static const struct mesh_protocol_ops k_oversized_ops = {
    .name = "oversized",
    .stream_framing = &k_oversized_framing,
    .receive = fake_receive,
};

static bool make_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) != 0) {
            (void)close(fds[0]);
            (void)close(fds[1]);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ cases */

/*
 * A link with no protocol yet is an ordinary state during start-up, so every call on one is a
 * quiet no-op that answers "no conversation" - and a keepalive the protocol does not have is
 * told apart from one that failed, because the TCP link drops the connection on the second.
 */
MESH_TEST_CASE(protocol_unbound_answers_no_conversation, unit) {
    const struct mesh_protocol unbound = {NULL, NULL};
    MESH_TEST_FAIL_IF(mesh_protocol_bound(&unbound) || mesh_protocol_bound(NULL),
                      "a protocol with no ops table is not bound");
    MESH_TEST_FAIL_IF(mesh_protocol_begin(&unbound) != -ENOTCONN,
                      "beginning an unbound protocol reports no connection");
    MESH_TEST_FAIL_IF(mesh_protocol_keepalive(&unbound) != -ENOTCONN,
                      "an unbound protocol has nobody to keep alive");
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&unbound), "an unbound protocol is not silent");
    MESH_TEST_FAIL_IF(mesh_protocol_stream_framing(&unbound) != NULL,
                      "an unbound protocol has no framing");
    MESH_TEST_FAIL_IF(strcmp(mesh_protocol_name(&unbound), "none") != 0,
                      "an unbound protocol is named for log lines");
    mesh_protocol_receive(&unbound, (const uint8_t *)"x", 1U);
    mesh_protocol_detach(&unbound);

    struct fake_protocol state;
    memset(&state, 0, sizeof state);
    const struct mesh_protocol fake = {&k_fake_ops, &state};
    MESH_TEST_FAIL_IF(mesh_protocol_keepalive(&fake) != -ENOTSUP,
                      "a protocol without a keepalive says so rather than failing a send");
    MESH_TEST_FAIL_IF(mesh_protocol_begin(&fake) != -ENOTCONN,
                      "a protocol without begin() cannot start a conversation");
    record_success(test_name);
}

/* The Meshtastic session is one protocol among any, and brings Meshtastic's framing with it. */
MESH_TEST_CASE(protocol_meshtastic_session_brings_its_framing, unit) {
    static struct mesh_session session;
    mesh_session_init(&session);
    const struct mesh_protocol protocol = mesh_session_protocol(&session);
    MESH_TEST_FAIL_IF(!mesh_protocol_bound(&protocol) || protocol.self != &session,
                      "the session is wrapped, not copied");
    MESH_TEST_FAIL_IF(strcmp(mesh_protocol_name(&protocol), "meshtastic") != 0,
                      "the table is named for the protocol");
    MESH_TEST_FAIL_IF(mesh_protocol_stream_framing(&protocol) != &mesh_stream_framing_meshtastic,
                      "a Meshtastic session frames streams the Meshtastic way");
    MESH_TEST_FAIL_IF(mesh_stream_framing_meshtastic.wake_byte != (int)MESH_STREAM_FRAME_START2,
                      "the serial wake burst is still a run of START2");
    MESH_TEST_FAIL_IF(mesh_stream_framing_meshtastic.max_frame > MESH_STREAM_PARSER_CAPACITY,
                      "Meshtastic's largest frame fits the parser");
    MESH_TEST_FAIL_IF(mesh_protocol_begin(&protocol) != -ENOTCONN,
                      "a session with no send path cannot begin a handshake");

    const struct mesh_protocol none = mesh_session_protocol(NULL);
    MESH_TEST_FAIL_IF(mesh_protocol_bound(&none), "no session is no protocol");
    record_success(test_name);
}

/*
 * The stream link frames and parses in whatever its protocol asks for. Both directions, over a
 * framing that is not Meshtastic's, so a link that still called mesh_stream_frame_encode() or
 * mesh_stream_parser_push() by name would put 0x94 0xC3 on the wire and hear nothing back.
 */
MESH_TEST_CASE(stream_link_speaks_its_protocols_framing, unit) {
    int fds[2];
    if (!make_pair(fds)) {
        record_failure(test_name, "could not make a non-blocking socket pair");
        return;
    }

    struct fake_protocol state;
    memset(&state, 0, sizeof state);
    const struct mesh_protocol protocol = {&k_fake_ops, &state};
    struct mesh_stream_link link;
    mesh_stream_link_init(&link, "test", &protocol);
    if (mesh_stream_link_open_socket(&link, (inkwell_socket)fds[0], NULL, NULL, NULL) != 0) {
        record_failure(test_name, "could not open the link");
        (void)close(fds[0]);
        (void)close(fds[1]);
        return;
    }

    const uint8_t payload[] = {0x01U, 0x02U, 0x03U};
    MESH_TEST_FAIL_IF_CLEANUP(mesh_stream_link_send(&link, payload, sizeof payload, 7U) != 0,
                              (mesh_stream_link_close(&link), (void)close(fds[1])),
                              "a frame in the protocol's framing sends");
    uint8_t wire[16];
    const ssize_t got = recv(fds[1], wire, sizeof wire, 0);
    MESH_TEST_FAIL_IF_CLEANUP(got != (ssize_t)(FAKE_HEADER + sizeof payload) ||
                                  wire[0] != FAKE_START || wire[1] != sizeof payload ||
                                  memcmp(wire + FAKE_HEADER, payload, sizeof payload) != 0,
                              (mesh_stream_link_close(&link), (void)close(fds[1])),
                              "the bytes on the wire are the protocol's framing, not Meshtastic's");

    /* Noise first, then a frame: the framing decides what is junk. */
    const uint8_t inbound[] = {'l', 'o', 'g', FAKE_START, 2U, 0xAAU, 0xBBU};
    MESH_TEST_FAIL_IF_CLEANUP(send(fds[1], inbound, sizeof inbound, 0) != (ssize_t)sizeof inbound,
                              (mesh_stream_link_close(&link), (void)close(fds[1])),
                              "could not write the inbound frame");
    (void)mesh_stream_link_pump(&link);
    MESH_TEST_FAIL_IF_CLEANUP(state.frames != 1U || state.received_len != 2U ||
                                  state.received[0] != 0xAAU || state.received[1] != 0xBBU,
                              (mesh_stream_link_close(&link), (void)close(fds[1])),
                              "a frame in the protocol's framing reaches the protocol");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_stream_link_stats(&link).junk_bytes != 3U,
                              (mesh_stream_link_close(&link), (void)close(fds[1])),
                              "what the framing discarded is counted as junk");

    mesh_stream_link_close(&link);
    (void)close(fds[1]);
    record_success(test_name);
}

/*
 * A framing whose largest frame does not fit the parser is bound as no framing at all. The
 * alternative is a parser that writes past its buffer the first time a long frame arrives, which
 * is the kind of bug that shows up as a crash on somebody else's radio months later.
 */
MESH_TEST_CASE(stream_link_refuses_a_framing_that_does_not_fit, unit) {
    struct fake_protocol state;
    memset(&state, 0, sizeof state);
    const struct mesh_protocol oversized = {&k_oversized_ops, &state};
    struct mesh_stream_link link;
    mesh_stream_link_init(&link, "test", &oversized);
    MESH_TEST_FAIL_IF(link.framing != NULL, "an oversized framing is not bound");

    int fds[2];
    if (!make_pair(fds)) {
        record_failure(test_name, "could not make a non-blocking socket pair");
        return;
    }
    if (mesh_stream_link_open_socket(&link, (inkwell_socket)fds[0], NULL, NULL, NULL) != 0) {
        record_failure(test_name, "could not open the link");
        (void)close(fds[0]);
        (void)close(fds[1]);
        return;
    }
    const uint8_t payload[] = {0x01U};
    MESH_TEST_FAIL_IF_CLEANUP(mesh_stream_link_send(&link, payload, sizeof payload, 0U) !=
                                  -ENOTCONN,
                              (mesh_stream_link_close(&link), (void)close(fds[1])),
                              "a link with no usable framing sends nothing");

    /* And the same link rebinds cleanly once it is handed something that fits. */
    const struct mesh_protocol fits = {&k_fake_ops, &state};
    mesh_stream_link_set_protocol(&link, &fits);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_stream_link_send(&link, payload, sizeof payload, 0U) != 0,
                              (mesh_stream_link_close(&link), (void)close(fds[1])),
                              "rebinding to a framing that fits restores the send path");

    mesh_stream_link_close(&link);
    (void)close(fds[1]);
    record_success(test_name);
}
