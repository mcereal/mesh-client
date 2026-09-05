#define _POSIX_C_SOURCE 200809L

/* Stream framing and the USB-serial transport. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"
#include "support/serial_fixture.h"

#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/serial_usb.h"
#include "mesh/transport/transport.h"

#include <pb_decode.h>

#include "meshtastic/mesh.pb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct stream_capture {
    size_t frame_count;
    size_t frame_len[8];
    uint8_t frames[8][32];
    size_t text_bytes;
};

static void stream_capture_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct stream_capture *capture = (struct stream_capture *)ctx;
    if (capture->frame_count >= 8U || len > 32U) {
        return;
    }
    memcpy(capture->frames[capture->frame_count], payload, len);
    capture->frame_len[capture->frame_count] = len;
    capture->frame_count += 1U;
}

static void stream_capture_text(const uint8_t *text, size_t len, void *ctx) {
    (void)text;
    ((struct stream_capture *)ctx)->text_bytes += len;
}

/* ---- serial transport ------------------------------------------------------------------------ */

MESH_TEST_CASE(stream_frame_encode, unit) {
    const uint8_t payload[] = {0x08U, 0x96U, 0x01U};
    uint8_t frame[16];
    size_t written = 0U;

    if (mesh_stream_frame_encode(payload, sizeof payload, frame, sizeof frame, &written) != 0) {
        record_failure(test_name, "encode failed");
        return;
    }
    MESH_TEST_FAIL_IF(written != MESH_STREAM_FRAME_HEADER_LEN + sizeof payload,
                      "unexpected frame length");
    MESH_TEST_FAIL_IF(frame[0] != MESH_STREAM_FRAME_START1 ||
                          frame[1] != MESH_STREAM_FRAME_START2 || frame[2] != 0x00U ||
                          frame[3] != (uint8_t)sizeof payload,
                      "header is not 0x94 0xC3 with a big-endian length");
    MESH_TEST_FAIL_IF(memcmp(frame + MESH_STREAM_FRAME_HEADER_LEN, payload, sizeof payload) != 0,
                      "payload was not copied verbatim");

    uint8_t big[MESH_STREAM_FRAME_MAX_PAYLOAD + 1U];
    memset(big, 0, sizeof big);
    uint8_t sink[MESH_STREAM_FRAME_HEADER_LEN + sizeof big];
    MESH_TEST_FAIL_IF(mesh_stream_frame_encode(big, sizeof big, sink, sizeof sink, &written) !=
                          -EMSGSIZE,
                      "oversized payload should be rejected");
    MESH_TEST_FAIL_IF(mesh_stream_frame_encode(payload, sizeof payload, frame, 4U, &written) !=
                          -ENOSPC,
                      "a short output buffer should return -ENOSPC");

    record_success(test_name);
}

/*
 * The radio interleaves its own log with the frames on the same port, and a reader gets
 * arbitrary chunk boundaries. The parser has to skip the log, resync on a header that turns out
 * to be log text, and hold a frame that arrives in pieces.
 */
MESH_TEST_CASE(stream_parser_resync, unit) {
    struct mesh_stream_parser parser;
    mesh_stream_parser_reset(&parser);
    struct stream_capture capture;
    memset(&capture, 0, sizeof capture);
    const struct mesh_stream_parser_callbacks callbacks = {
        .on_frame = stream_capture_frame,
        .on_text = stream_capture_text,
        .ctx = &capture,
    };

    const uint8_t log_line[] = {'I', 'N', 'F', 'O', ' ', 'b', 'o', 'o', 't', '\n'};
    mesh_stream_parser_push(&parser, log_line, sizeof log_line, &callbacks);
    MESH_TEST_FAIL_IF(capture.frame_count != 0U || capture.text_bytes != sizeof log_line,
                      "log text should be reported as text, not frames");

    /* A 0x94 0xC3 inside a log line, with a length no frame could have. */
    const uint8_t false_start[] = {MESH_STREAM_FRAME_START1, MESH_STREAM_FRAME_START2, 0xFFU, 0xFFU,
                                   'x'};
    mesh_stream_parser_push(&parser, false_start, sizeof false_start, &callbacks);
    MESH_TEST_FAIL_IF(capture.frame_count != 0U, "an impossible length should not produce a frame");

    const uint8_t payload_a[] = {0x01U, 0x02U, 0x03U};
    const uint8_t payload_b[] = {0x0AU, 0x0BU};
    uint8_t stream[64];
    size_t total = 0U;
    size_t written = 0U;
    mesh_stream_frame_encode(payload_a, sizeof payload_a, stream, sizeof stream, &written);
    total += written;
    /* Back-to-back frames with a scrap of log between them. */
    stream[total++] = '.';
    mesh_stream_frame_encode(payload_b, sizeof payload_b, stream + total, sizeof stream - total,
                             &written);
    total += written;

    /* One byte at a time: every split lands mid-header and mid-payload at some point. */
    for (size_t i = 0; i < total; ++i) {
        mesh_stream_parser_push(&parser, &stream[i], 1U, &callbacks);
    }

    MESH_TEST_FAIL_IF(capture.frame_count != 2U,
                      "expected both frames to survive a byte-at-a-time feed");
    MESH_TEST_FAIL_IF(capture.frame_len[0] != sizeof payload_a ||
                          memcmp(capture.frames[0], payload_a, sizeof payload_a) != 0 ||
                          capture.frame_len[1] != sizeof payload_b ||
                          memcmp(capture.frames[1], payload_b, sizeof payload_b) != 0,
                      "frame payloads did not round-trip");
    MESH_TEST_FAIL_IF(parser.frames != 2U || parser.dropped_bytes == 0U,
                      "parser counters did not track frames and junk");

    record_success(test_name);
}

/*
 * The whole serial connect, against a socketpair standing in for the tty: the Brick's bind and
 * usbfs DTR each happen once, the radio gets the resync burst, the handshake goes out framed,
 * and a framed FromRadio comes back into the session.
 */
MESH_TEST_CASE(serial_transport_connect_mock, unit) {
    int pair[2] = {-1, -1};
    MESH_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0, "socketpair failed");
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    const struct mesh_serial_device_info devices[] = {mesh_test_serial_device()};
    struct mesh_serial_usb_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.devices = devices;
    mock.device_count = 1U;
    mock.bound_path = "/dev/ttyUSB0";
    mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&mock);

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }

    struct mesh_transport *transport = mesh_serial_transport();
    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "serial start failed");
        goto cleanup_loop;
    }
    if (strcmp(transport->ops->status(transport), "running") != 0) {
        record_failure(test_name, "status should be running once a port is found");
        goto cleanup_transport;
    }

    if (mesh_serial_transport_connect(transport, "1-1:1.1") != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup_transport;
    }
    if (mesh_serial_usb_mock_bind_calls() != 1U || mesh_serial_usb_mock_line_state_calls() != 1U) {
        record_failure(test_name, "connect should bind the port and assert DTR exactly once");
        goto cleanup_transport;
    }
    if (!mesh_serial_transport_is_connecting(transport) ||
        mesh_serial_transport_connected_port(transport) != NULL) {
        record_failure(test_name, "the link should be waking, not yet connected");
        goto cleanup_transport;
    }

    uint8_t wake[64];
    ssize_t got = mesh_test_serial_read(pair[1], wake, sizeof wake);
    if (got != 32) {
        record_failure(test_name, "expected a 32-byte resync burst before the handshake");
        goto cleanup_transport;
    }
    for (ssize_t i = 0; i < got; ++i) {
        if (wake[i] != MESH_STREAM_FRAME_START2) {
            record_failure(test_name, "the resync burst should be bare START2 bytes");
            goto cleanup_transport;
        }
    }

    /* The handshake waits out the settle window, then goes out from tick(). */
    mesh_test_serial_sleep_ms(150);
    transport->ops->tick(transport);
    if (mesh_serial_transport_connected_port(transport) == NULL) {
        record_failure(test_name, "the link should be connected once the radio has woken");
        goto cleanup_transport;
    }

    uint8_t request[128];
    got = mesh_test_serial_read(pair[1], request, sizeof request);
    if (got < (ssize_t)MESH_STREAM_FRAME_HEADER_LEN || request[0] != MESH_STREAM_FRAME_START1 ||
        request[1] != MESH_STREAM_FRAME_START2) {
        record_failure(test_name, "want_config_id should go out framed");
        goto cleanup_transport;
    }
    const size_t request_len = ((size_t)request[2] << 8U) | (size_t)request[3];
    if (request_len == 0U || request_len + MESH_STREAM_FRAME_HEADER_LEN != (size_t)got) {
        record_failure(test_name, "framed length does not match what was written");
        goto cleanup_transport;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_zero;
    pb_istream_t decode =
        pb_istream_from_buffer(request + MESH_STREAM_FRAME_HEADER_LEN, request_len);
    if (!pb_decode(&decode, meshtastic_ToRadio_fields, &to_radio) ||
        to_radio.which_payload_variant != meshtastic_ToRadio_want_config_id_tag ||
        to_radio.want_config_id == 0U) {
        record_failure(test_name, "the framed packet is not a want_config_id ToRadio");
        goto cleanup_transport;
    }

    /* The radio answers with MyNodeInfo, framed. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x433D1A2CU;
    uint8_t encoded[256];
    size_t encoded_len = 0U;
    if (!mesh_test_encode_from_radio(&from_radio, encoded, sizeof encoded, &encoded_len)) {
        record_failure(test_name, "failed to encode the reply");
        goto cleanup_transport;
    }
    uint8_t reply[300];
    size_t reply_len = 0U;
    mesh_stream_frame_encode(encoded, encoded_len, reply, sizeof reply, &reply_len);
    /* A log line ahead of the frame, and the frame itself split across two writes: both are
       what a read() off the real port hands over. */
    const uint8_t noise[] = {'D', 'E', 'B', 'U', 'G', '\n'};
    if (write(pair[1], noise, sizeof noise) != (ssize_t)sizeof noise ||
        write(pair[1], reply, 3U) != 3 ||
        write(pair[1], reply + 3U, reply_len - 3U) != (ssize_t)(reply_len - 3U)) {
        record_failure(test_name, "failed to write the reply into the port");
        goto cleanup_transport;
    }

    if (mesh_serial_transport_pump(transport) <= 0) {
        record_failure(test_name, "pump should have read the reply");
        goto cleanup_transport;
    }

    const struct mesh_handshake_status *status =
        mesh_session_handshake(mesh_serial_transport_session(transport));
    if (status == NULL || !status->has_my_info || status->my_info.my_node_num != 0x433D1A2CU) {
        record_failure(test_name, "MyNodeInfo did not reach the session");
        goto cleanup_transport;
    }

    struct mesh_serial_transport_stats stats = mesh_serial_transport_stats(transport);
    if (stats.frames_received != 1U || stats.junk_bytes == 0U) {
        record_failure(test_name, "stats should count one frame and the log bytes around it");
        goto cleanup_transport;
    }

    if (mesh_serial_transport_disconnect(transport) != 0 ||
        mesh_serial_transport_connected_port(transport) != NULL) {
        record_failure(test_name, "disconnect should drop the link");
        goto cleanup_transport;
    }

    record_success(test_name);

cleanup_transport:
    transport->ops->stop(transport);
cleanup_loop:
    mesh_event_loop_shutdown(&loop);
cleanup:
    mesh_serial_usb_mock_disable();
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
}

/* Unplugging the node shows up as EOF on the tty; the link has to reset rather than spin. */
MESH_TEST_CASE(serial_transport_link_drop, unit) {
    int pair[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        record_failure(test_name, "socketpair failed");
        return;
    }
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct mesh_serial_device_info device = mesh_test_serial_device();
    device.bound = true;
    device.needs_line_state = false;
    snprintf(device.path, sizeof device.path, "%s", "/dev/ttyUSB0");
    device.control_interface = -1;
    const struct mesh_serial_device_info devices[] = {device};

    struct mesh_serial_usb_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.devices = devices;
    mock.device_count = 1U;
    mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&mock);

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }

    struct mesh_transport *transport = mesh_serial_transport();
    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "serial start failed");
        goto cleanup_loop;
    }

    if (mesh_serial_transport_connect(transport, "/dev/ttyUSB0") != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup_transport;
    }
    /* An already-bound bridge needs neither the generic driver nor the usbfs DTR path. */
    if (mesh_serial_usb_mock_bind_calls() != 0U || mesh_serial_usb_mock_line_state_calls() != 0U) {
        record_failure(test_name, "a bound port should not be rebound or poked over usbfs");
        goto cleanup_transport;
    }

    mesh_test_serial_sleep_ms(150);
    transport->ops->tick(transport);
    if (mesh_serial_transport_connected_port(transport) == NULL) {
        record_failure(test_name, "expected a connected link");
        goto cleanup_transport;
    }

    /* Drain what the transport wrote: a socketpair closed with unread data sends RST, and we
       want the clean EOF that an unplugged tty gives. */
    uint8_t drain[256];
    while (read(pair[1], drain, sizeof drain) > 0) {
    }
    close(pair[1]);
    pair[1] = -1;
    if (mesh_serial_transport_pump(transport) != -ENOTCONN) {
        record_failure(test_name, "pump should report the port gone");
        goto cleanup_transport;
    }
    if (mesh_serial_transport_connected_port(transport) != NULL ||
        strcmp(transport->ops->status(transport), "running") != 0) {
        record_failure(test_name, "the link should be reset and the transport back to scanning");
        goto cleanup_transport;
    }
    if (mesh_session_attached(mesh_serial_transport_session(transport))) {
        record_failure(test_name, "the session should have been detached");
        goto cleanup_transport;
    }

    record_success(test_name);

cleanup_transport:
    transport->ops->stop(transport);
cleanup_loop:
    mesh_event_loop_shutdown(&loop);
cleanup:
    mesh_serial_usb_mock_disable();
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
}
