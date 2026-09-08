/*
 * mesh_session_handle_from_radio(), fed whatever arrives.
 *
 * The other door: every FromRadio protobuf, however it travelled. BLE hands one over per GATT
 * read and the serial link hands over one per frame, and neither looks inside first - the
 * session is where bytes from an unknown radio first become fields we act on.
 *
 * nanopb is careful about its own bounds, so the interesting failures are downstream of the
 * decode: what the session does with a NodeInfo it has 256 slots for, a channel index the
 * firmware never promised to keep under 8, a traceroute with more hops than RouteDiscovery can
 * hold. Those arrays live *inside* struct mesh_session, which is exactly the case a sanitizer
 * cannot see: writing one slot past `nodes[255]` lands on the next field of a struct the
 * allocator handed out whole, so nothing faults, nothing is poisoned, and the corruption is
 * discovered later somewhere else. The counts are therefore checked by hand after every input.
 *
 * The session is re-initialised per input rather than carried across them, so a crash reproduces
 * from the one file the fuzzer reports rather than from the history that led to it.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "fuzz_state.h"

#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/utils/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 170 KB of session; far too much for a stack frame, and re-initialised per input anyway. */
static struct mesh_session g_session;

static void fuzz_broke(const char *what) {
    fprintf(stderr, "session contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

/* The link the session thinks it has. It answers every send with success so the handshake and
   the admin queue keep running rather than stalling on the first reply the session tries to
   make - the send path itself is ours, not the radio's, and is not what this is fuzzing. */
static int fuzz_send(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id) {
    (void)ctx;
    (void)packet_id;
    if (packet == NULL && len > 0U) {
        fuzz_broke("a send with a length and no packet");
    }
    if (len > MESH_SESSION_MAX_PACKET) {
        fuzz_broke("a ToRadio longer than the session's own maximum");
    }
    return 0;
}

static void fuzz_check_bounds(const struct mesh_session *session) {
    if (session->handshake.node_count > MESH_SESSION_MAX_NODES) {
        fuzz_broke("more nodes than the roster holds");
    }
    if (session->handshake.channel_count > MESH_SESSION_MAX_CHANNELS) {
        fuzz_broke("more channels than the table holds");
    }
    if (session->messages.count > MESH_MESSAGE_LOG_CAPACITY) {
        fuzz_broke("more messages than the ring holds");
    }
    if (session->messages.head >= MESH_MESSAGE_LOG_CAPACITY) {
        fuzz_broke("a ring head outside the ring");
    }
    if (session->traceroute.route_count > MESH_TRACEROUTE_MAX_HOPS ||
        session->traceroute.snr_count > MESH_TRACEROUTE_MAX_HOPS ||
        session->traceroute.back_count > MESH_TRACEROUTE_MAX_HOPS ||
        session->traceroute.snr_back_count > MESH_TRACEROUTE_MAX_HOPS) {
        fuzz_broke("a traceroute longer than RouteDiscovery can carry");
    }
    /* The radio's own words, sanitised into a row's worth: the field is only ever read as a C
       string, so an unterminated one runs off the end of the struct. */
    if (memchr(session->notification.text, '\0', sizeof session->notification.text) == NULL) {
        fuzz_broke("a notification that is not a string");
    }
}

int LLVMFuzzerInitialize(int *argc, char ***argv);

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    /* Nearly every input is a protobuf that does not decode, and the session says so at WARN.
       Left on, the run is a write() to stderr per input and the fuzzer spends its time in the
       terminal rather than in the parser. The log is not what this is testing. */
    mesh_log_set_level(MESH_LOG_LEVEL_NONE);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mesh_session_init(&g_session);
    mesh_session_attach(&g_session, fuzz_send, NULL);

    /* An attached session that has not asked the radio for anything answers half these packets
       with an early return, and the two most interesting decoders are behind that: a
       config_complete is only acted on while a want_config is in flight and echoes it, and a
       TRACEROUTE_APP payload is only decoded while a trace is pending and echoes its request.
       Both are states a real session spends most of a connection in, so the harness starts each
       input in them - and the ids are pinned (fuzz_state.h) so the corpus can quote them. */
    g_session.handshake.request_in_flight = true;
    g_session.handshake.request_id = MESH_FUZZ_CONFIG_REQUEST_ID;
    g_session.traceroute.state = (uint8_t)MESH_TRACEROUTE_PENDING;
    g_session.traceroute.target = MESH_FUZZ_TRACEROUTE_TARGET;
    g_session.traceroute.packet_id = MESH_FUZZ_TRACEROUTE_REQUEST_ID;

    /* Deliberately not capped at MESH_SESSION_MAX_PACKET. Both links bound what they hand over
       - the frame parser by its buffer, BLE by its read size - but the session takes a pointer
       and a length and is the thing being tested, so it gets to answer for the length it was
       given rather than for the one a caller promised. */
    mesh_session_handle_from_radio(&g_session, data, size);
    fuzz_check_bounds(&g_session);

    /* Again, into the state the first one left. A radio repeats itself constantly - the same
       NodeInfo arrives on every sync - and the paths that merge a repeat into an existing entry
       are different code from the ones that create it. */
    mesh_session_handle_from_radio(&g_session, data, size);
    fuzz_check_bounds(&g_session);
    return 0;
}
