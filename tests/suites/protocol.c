#define _POSIX_C_SOURCE 200809L

/*
 * The link's side of mesh/core/protocol.h: what a link does with a protocol it was handed, and
 * what an unbound one answers.
 *
 * The protocol, the framing and the BLE profile below are fakes on purpose. Every other link
 * case drives the Meshtastic session, which is exactly the case a hard-coded call would also
 * pass; these are the ones that fail if a link stops going through the table.
 */

#include "framework/mesh_test.h"
#include "support/ble_fixture.h"

#include "inkwell/ble/central.h"
#include "mesh/core/protocol.h"
#include "mesh/core/session.h"
#include "mesh/proto/ble_profile.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/stream_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

/* ------------------------------------------------------------------ a BLE profile nobody ships */

/* The Nordic UART shape: write RX, subscribe to TX, and every notification is a frame. Unlike
   Meshtastic's in every UUID and in the inbound model, so a link that fell back to FromNum and
   FromRadio would find nothing and read nothing. Not the real Nordic UART UUIDs: those are
   MeshCore's, a known profile, and the scan would tag the radio with that one first. */
#define FAKE_NUS_SERVICE "FA4E0001-B5A3-F393-E0A9-E50E24DCCA9E"
#define FAKE_NUS_RX "FA4E0002-B5A3-F393-E0A9-E50E24DCCA9E"
#define FAKE_NUS_TX "FA4E0003-B5A3-F393-E0A9-E50E24DCCA9E"

static const struct mesh_ble_profile k_fake_nus = {
    .name = "fake-nus",
    .service_uuid = FAKE_NUS_SERVICE,
    .write_uuid = FAKE_NUS_RX,
    .notify_uuid = FAKE_NUS_TX,
    .inbound = MESH_BLE_INBOUND_NOTIFY,
    .max_frame = 172U,
};

/* ------------------------------------------------------------------ a protocol that records */

struct fake_protocol {
    uint8_t received[64];
    size_t received_len;
    size_t frames;
    uint32_t failed_id;
    mesh_protocol_send_fn send;
    void *send_ctx;
};

static void fake_attach(void *self, mesh_protocol_send_fn send, void *send_ctx) {
    struct fake_protocol *fake = (struct fake_protocol *)self;
    fake->send = send;
    fake->send_ctx = send_ctx;
}

static void fake_detach(void *self) {
    struct fake_protocol *fake = (struct fake_protocol *)self;
    fake->send = NULL;
    fake->send_ctx = NULL;
}

/* Opens the way a pulled protocol does: one short command, nothing framed around it. */
static const uint8_t k_fake_hello[] = {0x01U, 0x03U};

static int fake_begin(void *self) {
    struct fake_protocol *fake = (struct fake_protocol *)self;
    return fake->send != NULL ? fake->send(fake->send_ctx, k_fake_hello, sizeof k_fake_hello, 0U)
                              : -ENOTCONN;
}

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

static const struct mesh_protocol_ops k_fake_ble_ops = {
    .name = "fake-ble",
    .ble_profile = &k_fake_nus,
    .attach = fake_attach,
    .detach = fake_detach,
    .begin = fake_begin,
    .receive = fake_receive,
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
    MESH_TEST_FAIL_IF(mesh_protocol_ble_profile(&unbound) != NULL,
                      "an unbound protocol has no BLE profile");
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

/* The Meshtastic session is one protocol among any, and brings Meshtastic's framing and GATT
   profile with it. */
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
    MESH_TEST_FAIL_IF(mesh_protocol_ble_profile(&protocol) != &mesh_ble_profile_meshtastic,
                      "a Meshtastic session is found over BLE under Meshtastic's profile");
    MESH_TEST_FAIL_IF(!mesh_ble_profile_usable(&mesh_ble_profile_meshtastic),
                      "Meshtastic's profile is one a link can carry");
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

/*
 * What a link can carry: a PULL profile needs somewhere to read from, and no profile may carry a
 * frame larger than the link's queue slot. Refused here, a bad table fails on connect with a
 * log line rather than as a write past a buffer.
 */
MESH_TEST_CASE(ble_profile_usable_refuses_what_a_link_cannot_carry, unit) {
    MESH_TEST_FAIL_IF(!mesh_ble_profile_usable(&k_fake_nus),
                      "a NOTIFY profile needs no read characteristic");
    MESH_TEST_FAIL_IF(mesh_ble_profile_usable(NULL), "no profile is not usable");

    struct mesh_ble_profile pull_without_read = k_fake_nus;
    pull_without_read.inbound = MESH_BLE_INBOUND_PULL;
    MESH_TEST_FAIL_IF(mesh_ble_profile_usable(&pull_without_read),
                      "a PULL profile with nothing to read is refused");

    struct mesh_ble_profile oversized = k_fake_nus;
    oversized.max_frame = MESH_BLE_MAX_PACKET_SIZE + 1U;
    MESH_TEST_FAIL_IF(mesh_ble_profile_usable(&oversized),
                      "a frame larger than the link's slot is refused");

    struct mesh_ble_profile no_write = k_fake_nus;
    no_write.write_uuid = NULL;
    MESH_TEST_FAIL_IF(mesh_ble_profile_usable(&no_write), "a profile with no write is refused");
    record_success(test_name);
}

/*
 * The BLE link scans for, connects under, writes to and hears from whatever profile its protocol
 * names - here one shaped like the Nordic UART Service, where a notification *is* the frame.
 *
 * Two radios in the room, one advertising each profile: the scan tags each with the one it found,
 * which is what will let the app choose a protocol per radio. Then a connect to the fake one must
 * write the protocol's opening command to RX, subscribe to TX (the mock only delivers a
 * notification on the handle the link subscribed to), and hand a notified value straight to the
 * protocol without a single read - a link that still drained FromRadio would read, and one still
 * subscribed to FromNum would hear nothing.
 */
MESH_TEST_CASE(ble_link_speaks_its_protocols_profile, unit) {
    const char *failure = NULL;
    struct fake_protocol state;
    memset(&state, 0, sizeof state);
    const struct mesh_protocol protocol = {&k_fake_ble_ops, &state};

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:A1", "Companion", -40);
    (void)mesh_test_ble_rig_add_device(&rig, "AA:BB:CC:DD:EE:A2", "Meshtastic", -50);
    static const char *const services[] = {FAKE_NUS_SERVICE, MESH_BLE_MESHTASTIC_SERVICE_UUID};
    rig.mock.device_service_uuids = services;

    char rx_path[128];
    char tx_path[128];
    char fromnum_path[128];
    snprintf(rx_path, sizeof rx_path, "%s/%s", rig.devices[0].address, FAKE_NUS_RX);
    snprintf(tx_path, sizeof tx_path, "%s/%s", rig.devices[0].address, FAKE_NUS_TX);
    snprintf(fromnum_path, sizeof fromnum_path, "%s/%s", rig.devices[0].address,
             MESH_BLE_FROMNUM_UUID);

    /* The transport is a process-wide singleton: whatever is bound here is unbound below, or
       every later BLE case would run against this fake. */
    rig.ble->ops->set_protocol(rig.ble, &protocol);
    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_ble_transport_session(rig.ble) != NULL) {
        failure = "a transport handed a protocol does not report its fallback session";
        goto cleanup;
    }

    (void)mesh_ble_transport_refresh_devices(rig.ble);
    if (mesh_ble_transport_device_profile(rig.ble, rig.devices[0].address) != &k_fake_nus) {
        failure = "the companion radio is tagged with the profile it advertises";
        goto cleanup;
    }
    if (mesh_ble_transport_device_profile(rig.ble, rig.devices[1].address) !=
        &mesh_ble_profile_meshtastic) {
        failure = "the Meshtastic radio is still found, and tagged as Meshtastic";
        goto cleanup;
    }
    if (mesh_ble_transport_device_profile(rig.ble, "00:00:00:00:00:00") != NULL) {
        failure = "a radio not in the listing has no profile";
        goto cleanup;
    }

    if (mesh_ble_transport_connect(rig.ble, rig.devices[0].address) != 0 ||
        mesh_ble_transport_connected_address(rig.ble) == NULL) {
        failure = "connect under the protocol's profile should succeed";
        goto cleanup;
    }
    if (strcmp(rig.write_path, rx_path) != 0 || rig.write_len != sizeof k_fake_hello ||
        memcmp(rig.write_capture, k_fake_hello, sizeof k_fake_hello) != 0) {
        failure = "the protocol's opening command goes to the profile's write characteristic, bare";
        goto cleanup;
    }

    inkwell_ble_mock_emit_notification(fromnum_path, (const uint8_t *)"\x01\x00\x00\x00", 4U);
    if (state.frames != 0U) {
        failure = "the link is subscribed to the profile's characteristic, not FromNum";
        goto cleanup;
    }

    const uint8_t notified[] = {0x83U, 0x10U, 0x20U};
    inkwell_ble_mock_emit_notification(tx_path, notified, sizeof notified);
    if (state.frames != 1U || state.received_len != sizeof notified ||
        memcmp(state.received, notified, sizeof notified) != 0) {
        failure = "a notified value reaches the protocol as one whole frame";
        goto cleanup;
    }
    uint8_t oversized[173];
    memset(oversized, 0x66, sizeof oversized);
    inkwell_ble_mock_emit_notification(tx_path, oversized, sizeof oversized);
    if (state.frames != 1U) {
        failure = "a notification over the profile's own limit never reaches the protocol";
        goto cleanup;
    }
    if (rig.read_index != 0U) {
        failure = "a NOTIFY profile is never read";
        goto cleanup;
    }
    if (mesh_ble_transport_stats(rig.ble).frames_received != 1U) {
        failure = "a notified frame is counted like a read one";
        goto cleanup;
    }

    uint8_t too_long[173];
    memset(too_long, 0x55, sizeof too_long);
    if (state.send == NULL ||
        state.send(state.send_ctx, too_long, sizeof too_long, 0U) != -EMSGSIZE) {
        failure = "a frame over the profile's own limit is refused before it is queued";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    rig.ble->ops->set_protocol(rig.ble, NULL);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}
