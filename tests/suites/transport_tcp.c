#define _POSIX_C_SOURCE 200809L

/* The network transport: target parsing, and the link against a loopback listener. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"
#include "support/serial_fixture.h"

#include "mesh/core/app.h"
#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/transport/tcp.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/store.h"

#include <pb_decode.h>

#include "meshtastic/mesh.pb.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * A listener on the loopback, standing in for a radio's TCP API.
 *
 * Port 0 rather than 4403: the test must not collide with a meshtasticd somebody is actually
 * running on the machine, and it must not need one either.
 */
struct tcp_test_radio {
    int listener;
    int accepted;
    char target[MESH_TCP_TARGET_MAX];
};

static void tcp_test_radio_init(struct tcp_test_radio *radio) {
    radio->listener = -1;
    radio->accepted = -1;
    radio->target[0] = '\0';
}

static bool tcp_test_radio_listen(struct tcp_test_radio *radio) {
    radio->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (radio->listener < 0) {
        return false;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(radio->listener, (const struct sockaddr *)&address, sizeof address) != 0 ||
        listen(radio->listener, 1) != 0) {
        return false;
    }

    socklen_t len = (socklen_t)sizeof address;
    if (getsockname(radio->listener, (struct sockaddr *)&address, &len) != 0) {
        return false;
    }
    snprintf(radio->target, sizeof radio->target, "127.0.0.1:%u",
             (unsigned)ntohs(address.sin_port));
    return true;
}

/* Takes the client's connection. The transport connects non-blocking, so the accept can be
   ready either side of the call returning. */
static bool tcp_test_radio_accept(struct tcp_test_radio *radio) {
    radio->accepted = accept(radio->listener, NULL, NULL);
    if (radio->accepted < 0) {
        return false;
    }
    (void)fcntl(radio->accepted, F_SETFL, O_NONBLOCK);
    return true;
}

static void tcp_test_radio_close(struct tcp_test_radio *radio) {
    if (radio->accepted >= 0) {
        close(radio->accepted);
        radio->accepted = -1;
    }
    if (radio->listener >= 0) {
        close(radio->listener);
        radio->listener = -1;
    }
}

/* ---- the target ------------------------------------------------------------------------------ */

MESH_TEST_CASE(tcp_target_split_shapes, unit) {
    char host[MESH_TCP_TARGET_MAX];
    uint16_t port = 0U;

    if (mesh_tcp_target_split("192.168.1.50", host, sizeof host, &port) != 0 ||
        strcmp(host, "192.168.1.50") != 0 || port != MESH_TCP_DEFAULT_PORT) {
        record_failure(test_name, "a bare address should take the default port");
        return;
    }

    if (mesh_tcp_target_split("192.168.1.50:4404", host, sizeof host, &port) != 0 ||
        strcmp(host, "192.168.1.50") != 0 || port != 4404U) {
        record_failure(test_name, "host:port should split");
        return;
    }

    /*
     * A bare v6 literal is all colons, so the last one is not a port separator - there is
     * nowhere for a port to go without brackets, and reading "fd00::1" as host "fd00:" port ":1"
     * is the bug this shape exists to prevent.
     */
    if (mesh_tcp_target_split("fd00::1", host, sizeof host, &port) != 0 ||
        strcmp(host, "fd00::1") != 0 || port != MESH_TCP_DEFAULT_PORT) {
        record_failure(test_name, "a bare v6 literal should keep all of its colons");
        return;
    }

    if (mesh_tcp_target_split("[fd00::1]:4404", host, sizeof host, &port) != 0 ||
        strcmp(host, "fd00::1") != 0 || port != 4404U) {
        record_failure(test_name, "a bracketed v6 literal should split off its port");
        return;
    }

    if (mesh_tcp_target_split("[fd00::1]", host, sizeof host, &port) != 0 ||
        strcmp(host, "fd00::1") != 0 || port != MESH_TCP_DEFAULT_PORT) {
        record_failure(test_name, "brackets without a port are still a v6 literal");
        return;
    }

    /* A name parses here on purpose: this splits a target, it does not resolve one. What
       refuses a name is the connect, which says so in words. */
    if (mesh_tcp_target_split("meshtastic.local:4403", host, sizeof host, &port) != 0 ||
        strcmp(host, "meshtastic.local") != 0 || port != 4403U) {
        record_failure(test_name, "splitting is not resolving; a name should split");
        return;
    }

    static const char *const rejected[] = {
        "",                   /* nothing at all */
        "192.168.1.50:",      /* a separator with no port after it */
        "192.168.1.50:0",     /* port 0 is not a port anything listens on */
        "192.168.1.50:65536", /* one past the top of the range */
        "192.168.1.50:4403x", /* trailing rubbish after a good number */
        "192.168.1.50:-1",    /* strtoul would wrap this into something enormous */
        "[fd00::1",           /* an opening bracket with no close */
        "[]:4403",            /* brackets around nothing */
        "[fd00::1]4403",      /* a close bracket followed by something that is not ':' */
    };
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; ++i) {
        if (mesh_tcp_target_split(rejected[i], host, sizeof host, &port) == 0) {
            record_failure(test_name, "a malformed target was accepted");
            return;
        }
    }

    /* Longer than the field it has to land in. */
    char oversized[MESH_TCP_TARGET_MAX + 16];
    memset(oversized, '1', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';
    if (mesh_tcp_target_split(oversized, host, sizeof host, &port) == 0) {
        record_failure(test_name, "a target longer than the field should be refused");
        return;
    }

    record_success(test_name);
}

/*
 * The two declarations of the target length are in headers that must not include each other -
 * mesh/core/config.h is reached from everywhere and mesh/transport/tcp.h drags the generated
 * protobuf headers behind it - so this is what holds them honest. The same arrangement as
 * MESH_WAYPOINT_NAME_MAX and MESH_UI_MESSAGE_TEXT_MAX.
 */
MESH_TEST_CASE(tcp_target_max_agrees_with_the_config, unit) {
    struct mesh_app_config config = mesh_app_config_default();
    if (sizeof config.preferred_tcp_host != (size_t)MESH_TCP_TARGET_MAX) {
        record_failure(test_name, "preferred_tcp_host and MESH_TCP_TARGET_MAX have drifted apart");
        return;
    }
    record_success(test_name);
}

/* ---- the link -------------------------------------------------------------------------------- */

/*
 * The whole connect against a real loopback listener: the handshake goes out framed, a framed
 * FromRadio comes back into the session, and the heartbeat that keeps the radio from dropping us
 * is on the wire byte for byte.
 */
MESH_TEST_CASE(tcp_transport_connect_loopback, unit) {
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_event_loop loop;
    bool loop_up = false;
    struct mesh_transport *transport = mesh_tcp_transport();
    bool started = false;

    if (!tcp_test_radio_listen(&radio)) {
        record_failure(test_name, "could not listen on the loopback");
        goto cleanup;
    }
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }
    loop_up = true;

    struct mesh_app_config config = mesh_app_config_default();
    snprintf(config.preferred_tcp_host, sizeof config.preferred_tcp_host, "%s", radio.target);
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "tcp start failed");
        goto cleanup;
    }
    started = true;

    if (strcmp(transport->ops->status(transport), "running") != 0) {
        record_failure(test_name, "status should be running once a host is configured");
        goto cleanup;
    }
    if (mesh_tcp_transport_configured_target(transport) == NULL ||
        strcmp(mesh_tcp_transport_configured_target(transport), radio.target) != 0) {
        record_failure(test_name, "the configured host should be readable before any connect");
        goto cleanup;
    }

    if (mesh_tcp_transport_connect(transport, radio.target) != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup;
    }

    /* A loopback connect usually completes inside connect(); when it does not, the socket
       becomes writable and the loop finishes it. Either way one turn is enough. */
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        (void)mesh_event_loop_run(&loop, 200);
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL ||
        strcmp(mesh_tcp_transport_connected_target(transport), radio.target) != 0) {
        record_failure(test_name, "the link should be connected");
        goto cleanup;
    }
    if (mesh_tcp_transport_is_connecting(transport)) {
        record_failure(test_name, "a connected link is no longer connecting");
        goto cleanup;
    }
    if (strcmp(transport->ops->status(transport), "connected") != 0) {
        record_failure(test_name, "status should be connected");
        goto cleanup;
    }

    if (!tcp_test_radio_accept(&radio)) {
        record_failure(test_name, "the listener did not see the connection");
        goto cleanup;
    }

    /* The handshake goes out as soon as the connect completes: there is no wake burst and no
       settle here, unlike a tty. */
    uint8_t request[128];
    ssize_t got = mesh_test_serial_read(radio.accepted, request, sizeof request);
    if (got < (ssize_t)MESH_STREAM_FRAME_HEADER_LEN || request[0] != MESH_STREAM_FRAME_START1 ||
        request[1] != MESH_STREAM_FRAME_START2) {
        record_failure(test_name, "want_config_id should go out framed");
        goto cleanup;
    }
    const size_t request_len = ((size_t)request[2] << 8U) | (size_t)request[3];
    if (request_len == 0U || request_len + MESH_STREAM_FRAME_HEADER_LEN != (size_t)got) {
        record_failure(test_name, "framed length does not match what was written");
        goto cleanup;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_zero;
    pb_istream_t decode =
        pb_istream_from_buffer(request + MESH_STREAM_FRAME_HEADER_LEN, request_len);
    if (!pb_decode(&decode, meshtastic_ToRadio_fields, &to_radio) ||
        to_radio.which_payload_variant != meshtastic_ToRadio_want_config_id_tag ||
        to_radio.want_config_id == 0U) {
        record_failure(test_name, "the framed packet is not a want_config_id ToRadio");
        goto cleanup;
    }

    /* The radio answers with MyNodeInfo, framed, split across two writes - a socket hands over
       whatever has arrived, not whatever was sent in one call. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x433D1A2CU;
    uint8_t encoded[256];
    size_t encoded_len = 0U;
    if (!mesh_test_encode_from_radio(&from_radio, encoded, sizeof encoded, &encoded_len)) {
        record_failure(test_name, "failed to encode the reply");
        goto cleanup;
    }
    uint8_t reply[300];
    size_t reply_len = 0U;
    mesh_stream_frame_encode(encoded, encoded_len, reply, sizeof reply, &reply_len);
    if (write(radio.accepted, reply, 3U) != 3 ||
        write(radio.accepted, reply + 3U, reply_len - 3U) != (ssize_t)(reply_len - 3U)) {
        record_failure(test_name, "failed to write the reply into the socket");
        goto cleanup;
    }

    if (mesh_tcp_transport_pump(transport) <= 0) {
        record_failure(test_name, "pump should have read the reply");
        goto cleanup;
    }

    const struct mesh_handshake_status *status =
        mesh_session_handshake(mesh_tcp_transport_session(transport));
    if (status == NULL || !status->has_my_info || status->my_info.my_node_num != 0x433D1A2CU) {
        record_failure(test_name, "MyNodeInfo did not reach the session");
        goto cleanup;
    }

    struct mesh_tcp_transport_stats stats = mesh_tcp_transport_stats(transport);
    if (stats.frames_received != 1U || stats.junk_bytes != 0U) {
        record_failure(test_name, "a socket carries frames and nothing between them");
        goto cleanup;
    }

    if (mesh_tcp_transport_disconnect(transport) != 0 ||
        mesh_tcp_transport_connected_target(transport) != NULL) {
        record_failure(test_name, "disconnect should drop the link");
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    if (started) {
        transport->ops->stop(transport);
    }
    if (loop_up) {
        mesh_event_loop_shutdown(&loop);
    }
    tcp_test_radio_close(&radio);
}

/*
 * The golden frame, derived from the wire format rather than from our own encoder.
 *
 * A heartbeat is the one ToRadio with nothing variable in it - upstream's clients send nonce 0
 * and nothing echoes it back - so the bytes can be written out by hand from mesh.proto:
 *
 *     94 c3   START1, START2
 *     00 02   payload length, big endian
 *     3a      field 7 (heartbeat), wire type 2 (length-delimited): (7 << 3) | 2
 *     00      the Heartbeat submessage, with its one optional field unset, is empty
 *
 * A protobuf regeneration that moves ToRadio.heartbeat to another field number, or a framing
 * change, fails here rather than against a radio.
 */
MESH_TEST_CASE(tcp_heartbeat_golden_frame, unit) {
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_event_loop loop;
    bool loop_up = false;
    struct mesh_transport *transport = mesh_tcp_transport();
    bool started = false;

    if (!tcp_test_radio_listen(&radio)) {
        record_failure(test_name, "could not listen on the loopback");
        goto cleanup;
    }
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }
    loop_up = true;

    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "tcp start failed");
        goto cleanup;
    }
    started = true;

    if (mesh_tcp_transport_connect(transport, radio.target) != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        (void)mesh_event_loop_run(&loop, 200);
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL || !tcp_test_radio_accept(&radio)) {
        record_failure(test_name, "the link should be up before the heartbeat goes out");
        goto cleanup;
    }

    /* Drain the handshake, which carries a per-process nonce and so cannot be pinned. */
    uint8_t discard[128];
    (void)mesh_test_serial_read(radio.accepted, discard, sizeof discard);

    if (mesh_session_send_heartbeat(mesh_tcp_transport_session(transport)) != 0) {
        record_failure(test_name, "the heartbeat should have gone out");
        goto cleanup;
    }

    static const uint8_t expected[] = {0x94U, 0xC3U, 0x00U, 0x02U, 0x3AU, 0x00U};
    uint8_t beat[16];
    const ssize_t got = mesh_test_serial_read(radio.accepted, beat, sizeof beat);
    if (got != (ssize_t)sizeof expected || memcmp(beat, expected, sizeof expected) != 0) {
        record_failure(test_name, "the heartbeat frame is not the hand-derived bytes");
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    if (started) {
        transport->ops->stop(transport);
    }
    if (loop_up) {
        mesh_event_loop_shutdown(&loop);
    }
    tcp_test_radio_close(&radio);
}

/* The radio going away shows up as EOF on the socket; the link has to reset rather than spin. */
MESH_TEST_CASE(tcp_transport_link_drop, unit) {
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_event_loop loop;
    bool loop_up = false;
    struct mesh_transport *transport = mesh_tcp_transport();
    bool started = false;

    if (!tcp_test_radio_listen(&radio)) {
        record_failure(test_name, "could not listen on the loopback");
        goto cleanup;
    }
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }
    loop_up = true;

    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "tcp start failed");
        goto cleanup;
    }
    started = true;

    if (mesh_tcp_transport_connect(transport, radio.target) != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        (void)mesh_event_loop_run(&loop, 200);
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL || !tcp_test_radio_accept(&radio)) {
        record_failure(test_name, "the link should be up before it is dropped");
        goto cleanup;
    }

    /* Drain what the transport wrote before closing: a socket closed with unread data sends RST
       rather than FIN, and that is a different path from the one under test. */
    uint8_t discard[256];
    (void)mesh_test_serial_read(radio.accepted, discard, sizeof discard);
    close(radio.accepted);
    radio.accepted = -1;
    mesh_test_serial_sleep_ms(20);

    if (mesh_tcp_transport_pump(transport) != -ENOTCONN) {
        record_failure(test_name, "pump should report the radio gone");
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) != NULL ||
        mesh_tcp_transport_is_connecting(transport)) {
        record_failure(test_name, "the link should have reset itself");
        goto cleanup;
    }
    if (mesh_session_attached(mesh_tcp_transport_session(transport))) {
        record_failure(test_name, "a dropped link must detach the session");
        goto cleanup;
    }
    /* Back to the status the transport had before the connect, not a stuck "connected". */
    if (strcmp(transport->ops->status(transport), "no-host") != 0) {
        record_failure(test_name, "status should fall back to the transport's own state");
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    if (started) {
        transport->ops->stop(transport);
    }
    if (loop_up) {
        mesh_event_loop_shutdown(&loop);
    }
    tcp_test_radio_close(&radio);
}

/*
 * A peer that vanished must come back as an error, not as a signal.
 *
 * Writing to a socket whose far end has gone raises SIGPIPE, and its default disposition kills
 * the process - so a radio dropping off the WiFi between two turns of the loop would take the
 * whole client down before the -EIO this code handles could ever be returned. The suppression is
 * `send(MSG_NOSIGNAL)` in stream_link.c; if it regresses, this case does not fail politely, it
 * kills the test binary, which is the loudest way a suite can report it.
 *
 * Two writes, not one: after a clean FIN the first still goes into the send buffer, and it is
 * the RST that comes back which makes the next one EPIPE.
 */
MESH_TEST_CASE(tcp_transport_survives_a_peer_that_vanished, unit) {
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_event_loop loop;
    bool loop_up = false;
    struct mesh_transport *transport = mesh_tcp_transport();
    bool started = false;

    if (!tcp_test_radio_listen(&radio)) {
        record_failure(test_name, "could not listen on the loopback");
        goto cleanup;
    }
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }
    loop_up = true;

    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "tcp start failed");
        goto cleanup;
    }
    started = true;

    if (mesh_tcp_transport_connect(transport, radio.target) != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        (void)mesh_event_loop_run(&loop, 200);
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL || !tcp_test_radio_accept(&radio)) {
        record_failure(test_name, "the link should be up before the peer goes away");
        goto cleanup;
    }

    /* Drained first, so the close is a FIN rather than an RST: the harder of the two cases, and
       the one a radio rebooting actually produces. */
    uint8_t discard[256];
    (void)mesh_test_serial_read(radio.accepted, discard, sizeof discard);
    close(radio.accepted);
    radio.accepted = -1;
    mesh_test_serial_sleep_ms(20);

    struct mesh_session *session = mesh_tcp_transport_session(transport);
    int result = 0;
    for (int i = 0; i < 20 && result >= 0; ++i) {
        result = mesh_session_send_heartbeat(session);
        mesh_test_serial_sleep_ms(10);
    }

    if (result >= 0) {
        record_failure(test_name, "writing at a socket whose peer has gone should fail");
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) != NULL) {
        record_failure(test_name, "a failed write should reset the link");
        goto cleanup;
    }
    if (mesh_session_attached(session)) {
        record_failure(test_name, "a reset link must detach the session");
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    if (started) {
        transport->ops->stop(transport);
    }
    if (loop_up) {
        mesh_event_loop_shutdown(&loop);
    }
    tcp_test_radio_close(&radio);
}

/*
 * A name is refused, and the refusal says what to do instead.
 *
 * This is the one deliberate limitation of the link: resolving a name means getaddrinfo(), which
 * blocks, and this client is one epoll loop. A refusal a user can read beats a UI that freezes.
 */
MESH_TEST_CASE(tcp_transport_refuses_a_name, unit) {
    struct mesh_transport *transport = mesh_tcp_transport();
    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, NULL) != 0) {
        record_failure(test_name, "tcp start failed");
        return;
    }

    if (mesh_tcp_transport_connect(transport, "meshtastic.local") != -EINVAL) {
        record_failure(test_name, "a hostname should be refused");
        goto cleanup;
    }

    char error[MESH_TRANSPORT_ERROR_MAX];
    if (!transport->ops->take_error(transport, error, sizeof error) || error[0] == '\0') {
        record_failure(test_name, "the refusal should reach the user in words");
        goto cleanup;
    }
    if (transport->ops->take_error(transport, error, sizeof error)) {
        record_failure(test_name, "take_error is one-shot");
        goto cleanup;
    }

    /* Not an address and not a shape either: a different refusal, and still a refusal. */
    if (mesh_tcp_transport_connect(transport, "192.168.1.50:not-a-port") != -EINVAL) {
        record_failure(test_name, "a malformed target should be refused");
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) != NULL ||
        mesh_tcp_transport_is_connecting(transport)) {
        record_failure(test_name, "a refused connect must leave no link behind");
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    transport->ops->stop(transport);
}

/* Disabled by configuration is a refusal with its own reason, not a silent no-op. */
MESH_TEST_CASE(tcp_transport_refuses_when_disabled, unit) {
    struct mesh_transport *transport = mesh_tcp_transport();
    struct mesh_app_config config = mesh_app_config_default();
    config.enable_tcp = false;
    snprintf(config.preferred_tcp_host, sizeof config.preferred_tcp_host, "127.0.0.1");
    if (transport->ops->start(transport, &config, NULL) != 0) {
        record_failure(test_name, "tcp start failed");
        return;
    }

    if (strcmp(transport->ops->status(transport), "disabled") != 0) {
        record_failure(test_name, "status should say disabled");
        goto cleanup;
    }
    if (mesh_tcp_transport_connect(transport, "127.0.0.1") != -ENODEV) {
        record_failure(test_name, "a disabled transport should refuse to connect");
        goto cleanup;
    }
    /* Disabled means disabled: the configured host is not adopted either, so nothing else can
       read one off this transport and try it. */
    if (mesh_tcp_transport_configured_target(transport) != NULL) {
        record_failure(test_name, "a disabled transport holds no target");
        goto cleanup;
    }

    char error[MESH_TRANSPORT_ERROR_MAX];
    if (!transport->ops->take_error(transport, error, sizeof error)) {
        record_failure(test_name, "the refusal should reach the user in words");
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    transport->ops->stop(transport);
}

/* Nothing configured is the resting state of this link, and it is not an error. */
MESH_TEST_CASE(tcp_transport_idles_without_a_host, unit) {
    struct mesh_transport *transport = mesh_tcp_transport();
    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, NULL) != 0) {
        record_failure(test_name, "tcp start failed");
        return;
    }

    if (strcmp(transport->ops->status(transport), "no-host") != 0) {
        record_failure(test_name, "status should say there is nothing to connect to");
        goto cleanup;
    }
    if (mesh_tcp_transport_configured_target(transport) != NULL) {
        record_failure(test_name, "no host was configured");
        goto cleanup;
    }

    char error[MESH_TRANSPORT_ERROR_MAX];
    if (transport->ops->take_error(transport, error, sizeof error)) {
        record_failure(test_name, "having nothing to do is not a failure to report");
        goto cleanup;
    }

    /* A tick with no link and no target must do nothing at all rather than reach for a socket. */
    transport->ops->tick(transport);
    if (mesh_tcp_transport_is_connecting(transport)) {
        record_failure(test_name, "tick must not start a connect on its own");
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    transport->ops->stop(transport);
}

/*
 * What the Devices tab is told about a link that nothing enumerated.
 *
 * A network host is in no scan, so the only row a TCP link ever gets is the one
 * mesh_app_publish_ui_state() synthesises for "connected, but in nobody's list". That slot is
 * memset to zero and MESH_UI_DEVICE_BLE is 0, so until the kind was stated the row came out a
 * Bluetooth radio: a Bluetooth disc, `0dBm` against its trailing edge - an absent RSSI read as
 * a number, which is the strongest reading on the screen - and Y offering to forget a bond that
 * was never made. The placeholder name went with it, standing permanently where the address
 * belongs, because the advertisement it waits for is never coming.
 *
 * Four assertions, one per wrong answer that field gave.
 */
MESH_TEST_CASE(tcp_link_is_published_as_a_network_device, unit) {
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_app app;
    memset(&app, 0, sizeof app);
    const char *failure = NULL;
    bool app_ready = false;
    char home_dir[] = "/tmp/mesh_tcp_publishXXXXXX";
    bool home_made = false;

    if (!tcp_test_radio_listen(&radio)) {
        record_failure(test_name, "could not listen on the loopback");
        goto cleanup;
    }
    if (mkdtemp(home_dir) == NULL) {
        record_failure(test_name, "mkdtemp failed");
        goto cleanup;
    }
    home_made = true;
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    /* The network link alone, so the row under test is the only row. */
    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_ble = false;
    config.enable_serial = false;
    config.enable_tcp = true;
    snprintf(config.preferred_tcp_host, sizeof config.preferred_tcp_host, "%s", radio.target);

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *transport = mesh_tcp_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    if (mesh_tcp_transport_connect(transport, radio.target) != 0) {
        failure = "connect failed";
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        (void)mesh_event_loop_run(&app.loop, 200);
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        failure = "the link should be connected";
        goto cleanup;
    }
    if (!tcp_test_radio_accept(&radio)) {
        failure = "the listener did not see the connection";
        goto cleanup;
    }

    mesh_app_publish_ui_state(&app);

    if (app.ui_store.device_count != 1U) {
        failure = "a connected network link should be the one row";
        goto cleanup;
    }
    const struct mesh_ui_device *row = &app.ui_store.devices[0];
    if (row->kind != (uint8_t)MESH_UI_DEVICE_TCP) {
        failure = "a network link is not a Bluetooth radio";
        goto cleanup;
    }
    if (strcmp(row->identifier, radio.target) != 0) {
        failure = "the row should be identified by the address it was configured with";
        goto cleanup;
    }
    /* Empty, so every label falls back to the address rather than to a placeholder that is
       waiting for an advertisement no host will ever send. */
    if (row->name[0] != '\0') {
        failure = "a network link has no name but its address";
        goto cleanup;
    }
    if (row->in_range) {
        failure = "a host answers from anywhere, so it is no evidence of earshot";
        goto cleanup;
    }
    if (!row->connected) {
        failure = "the link is up and the row should say so";
        goto cleanup;
    }
    if (mesh_ui_device_forgettable(row)) {
        failure = "there is no bond behind a network link to forget";
        goto cleanup;
    }

    record_success(test_name);

cleanup:
    if (failure != NULL) {
        record_failure(test_name, failure);
    }
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    tcp_test_radio_close(&radio);
    if (home_made) {
        rmdir(home_dir);
    }
}
