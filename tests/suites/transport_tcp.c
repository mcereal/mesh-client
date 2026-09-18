#define _POSIX_C_SOURCE 200809L

/* The network transport: target parsing, and the link against a loopback listener. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"
#include "support/serial_fixture.h"

#include "mesh/app/app.h"
#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/tcp.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/store_device.h"
#include "mesh/ui/store_message.h"
#include "mesh/utils/time.h"

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
#include <time.h>
#include <unistd.h>

/*
 * A listener on the loopback, standing in for a radio's TCP API.
 *
 * Port 0 rather than 4403: the test must not collide with a meshtasticd somebody is actually
 * running on the machine, and it must not need one either.
 */
struct tcp_test_radio {
    int listener;
    /* The v6 loopback on the same port, for the cases that connect by name. -1 otherwise. */
    int listener6;
    int accepted;
    char target[MESH_TCP_TARGET_MAX];
};

static void tcp_test_radio_init(struct tcp_test_radio *radio) {
    radio->listener = -1;
    radio->listener6 = -1;
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

/*
 * The same listener, reachable by the name `localhost` rather than by an address.
 *
 * Both loopbacks, on one port, because which of them `localhost` resolves to is the resolver's
 * business and not ours: /etc/hosts orders the two differently on different images, and
 * AI_ADDRCONFIG drops whichever family the machine has no address in. Binding both is what makes
 * a test of the *name* path independent of that - the v6 half is best-effort, since a host with
 * no IPv6 at all is exactly the host whose resolver will not offer it either.
 */
static bool tcp_test_radio_listen_by_name(struct tcp_test_radio *radio) {
    if (!tcp_test_radio_listen(radio)) {
        return false;
    }

    struct sockaddr_in bound;
    socklen_t bound_len = (socklen_t)sizeof bound;
    if (getsockname(radio->listener, (struct sockaddr *)&bound, &bound_len) != 0) {
        return false;
    }
    const uint16_t port = ntohs(bound.sin_port);

    radio->listener6 = socket(AF_INET6, SOCK_STREAM, 0);
    if (radio->listener6 >= 0) {
        /* v6-only, so this cannot collide with the v4 listener already holding the port. */
        const int on = 1;
        (void)setsockopt(radio->listener6, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on);

        struct sockaddr_in6 address6;
        memset(&address6, 0, sizeof address6);
        address6.sin6_family = AF_INET6;
        address6.sin6_port = htons(port);
        address6.sin6_addr = in6addr_loopback;
        if (bind(radio->listener6, (const struct sockaddr *)&address6, sizeof address6) != 0 ||
            listen(radio->listener6, 1) != 0) {
            close(radio->listener6);
            radio->listener6 = -1;
        }
    }

    snprintf(radio->target, sizeof radio->target, "localhost:%u", (unsigned)port);
    return true;
}

/* Accepts on whichever of the two listeners the client actually arrived on. */
static bool tcp_test_radio_accept_either(struct tcp_test_radio *radio) {
    for (unsigned turn = 0U; turn < 100U; ++turn) {
        const int fds[2] = {radio->listener, radio->listener6};
        for (unsigned which = 0U; which < 2U; ++which) {
            if (fds[which] < 0) {
                continue;
            }
            (void)fcntl(fds[which], F_SETFL, O_NONBLOCK);
            const int taken = accept(fds[which], NULL, NULL);
            if (taken >= 0) {
                radio->accepted = taken;
                (void)fcntl(radio->accepted, F_SETFL, O_NONBLOCK);
                return true;
            }
        }
        struct timespec pause = {0, 10 * 1000 * 1000};
        (void)nanosleep(&pause, NULL);
    }
    return false;
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
    if (radio->listener6 >= 0) {
        close(radio->listener6);
        radio->listener6 = -1;
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
    /*
     * Not a stuck "connected" - and not "no-host" either, which it would have been before
     * connect() started adopting its target. This link was pointed at a host and still knows
     * which one; it simply is not connected to it, which is what "running" says.
     */
    if (strcmp(transport->ops->status(transport), "running") != 0) {
        record_failure(test_name, "status should fall back to the transport's own state");
        goto cleanup;
    }
    /*
     * And the host outlives the link, so auto-connect has somewhere to go back to. A press that
     * reached the link but not this would be reconnected to whatever the startup configuration
     * named - here, nothing at all.
     */
    if (mesh_tcp_transport_configured_target(transport) == NULL ||
        strcmp(mesh_tcp_transport_configured_target(transport), radio.target) != 0) {
        record_failure(test_name, "a dropped link should still know the host it was pointed at");
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
 * A name resolves and the link comes up on the other side of it.
 *
 * The seam this covers is the handoff: an OK answer from the forked resolver has to arrive back
 * on the loop and go into the same socket-and-connect the literal path uses, with the link
 * ending up in exactly the state a numeric target leaves it in. Everything about `localhost`
 * that the machine gets to decide - which family, in which order - is absorbed by the fixture
 * listening on both.
 */
MESH_TEST_CASE(tcp_transport_connects_by_name, unit) {
    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "the loop did not start");
        return;
    }
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_transport *transport = mesh_tcp_transport();
    bool started = false;

    if (!tcp_test_radio_listen_by_name(&radio)) {
        record_failure(test_name, "the listener did not come up");
        goto cleanup;
    }

    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "tcp start failed");
        goto cleanup;
    }
    started = true;

    if (mesh_tcp_transport_connect(transport, radio.target) != 0) {
        record_failure(test_name, "connecting to a name should be accepted");
        goto cleanup;
    }
    /* Nothing is connected yet: the address is still out with a forked child. */
    if (mesh_tcp_transport_connected_target(transport) != NULL) {
        record_failure(test_name, "a name must not report a link before it resolves");
        goto cleanup;
    }

    for (unsigned turn = 0U; turn < 200U && mesh_tcp_transport_connected_target(transport) == NULL;
         ++turn) {
        (void)mesh_event_loop_run(&loop, 20);
        transport->ops->tick(transport);
    }

    const char *connected = mesh_tcp_transport_connected_target(transport);
    if (connected == NULL) {
        char error[MESH_TRANSPORT_ERROR_MAX];
        if (transport->ops->take_error(transport, error, sizeof error)) {
            record_failure(test_name, error);
        } else {
            record_failure(test_name, "the link never came up");
        }
        goto cleanup;
    }
    /* The link is named by what the user typed, not by what it resolved to. */
    if (strcmp(connected, radio.target) != 0) {
        record_failure(test_name, "the link should be named by the target as typed");
        goto cleanup;
    }
    /* And the far end really was reached - the handshake is on the wire. */
    if (!tcp_test_radio_accept_either(&radio)) {
        record_failure(test_name, "the radio never saw the connection");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    if (started) {
        transport->ops->stop(transport);
    }
    mesh_event_loop_shutdown(&loop);
    tcp_test_radio_close(&radio);
}

/*
 * A name is taken, looked up, and the failure that follows is reported in words.
 *
 * This used to be the case that proved a hostname was *refused*. Resolving one means
 * getaddrinfo(), which blocks - so for as long as the client had nowhere to put a blocking call
 * a refusal a user could read beat a UI that froze. src/core/net/resolve.c is that somewhere: the
 * lookup is a forked child read back through the loop, so the name is now accepted and the
 * connect simply starts a step later.
 *
 * `.invalid` is reserved by RFC 6761 to never resolve, so what this drives is the whole unhappy
 * path - accepted, looked up, failed, dropped, explained - without asking anything of the DNS on
 * the machine running it.
 */
MESH_TEST_CASE(tcp_transport_takes_a_name, unit) {
    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "the loop did not start");
        return;
    }
    struct mesh_transport *transport = mesh_tcp_transport();
    struct mesh_app_config config = mesh_app_config_default();
    bool started = false;
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "tcp start failed");
        goto cleanup;
    }
    started = true;

    if (mesh_tcp_transport_connect(transport, "meshclient-nothing-here.invalid:4403") != 0) {
        record_failure(test_name, "a hostname should be accepted");
        goto cleanup;
    }
    /* Resolving is the first half of connecting, and reads as one thing from outside. */
    if (!mesh_tcp_transport_is_connecting(transport)) {
        record_failure(test_name, "a name being looked up should read as connecting");
        goto cleanup;
    }
    /*
     * And it is the address the user wrote down from the moment it was asked for, not from the
     * moment it resolves - the same rule an IP to a switched-off radio follows.
     */
    const char *configured = mesh_tcp_transport_configured_target(transport);
    if (configured == NULL || strcmp(configured, "meshclient-nothing-here.invalid:4403") != 0) {
        record_failure(test_name, "a name should be adopted as the configured target");
        goto cleanup;
    }

    for (unsigned turn = 0U; turn < 200U && mesh_tcp_transport_is_connecting(transport); ++turn) {
        (void)mesh_event_loop_run(&loop, 50);
        transport->ops->tick(transport);
    }
    if (mesh_tcp_transport_is_connecting(transport)) {
        record_failure(test_name, "the lookup never finished");
        goto cleanup;
    }
    if (mesh_tcp_transport_connected_target(transport) != NULL) {
        record_failure(test_name, "a name that did not resolve must leave no link behind");
        goto cleanup;
    }

    char error[MESH_TRANSPORT_ERROR_MAX];
    if (!transport->ops->take_error(transport, error, sizeof error) || error[0] == '\0') {
        record_failure(test_name, "the failure should reach the user in words");
        goto cleanup;
    }
    if (transport->ops->take_error(transport, error, sizeof error)) {
        record_failure(test_name, "take_error is one-shot");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    if (started) {
        transport->ops->stop(transport);
    }
    mesh_event_loop_shutdown(&loop);
}

/*
 * The refusals that survive the resolver, and the one it creates.
 *
 * A malformed target is still refused before anything is forked - "not an address and port" is a
 * shape, and no lookup would make `:not-a-port` into one. A name with no event loop to read a
 * child through is the new refusal: there is nowhere to put the blocking call, which is the old
 * limitation surviving exactly where it is still true.
 */
MESH_TEST_CASE(tcp_transport_refuses_what_it_cannot_resolve, unit) {
    struct mesh_transport *transport = mesh_tcp_transport();
    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, NULL) != 0) {
        record_failure(test_name, "tcp start failed");
        return;
    }

    if (mesh_tcp_transport_connect(transport, "192.168.1.50:not-a-port") != -EINVAL) {
        record_failure(test_name, "a malformed target should be refused");
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

    /* No loop, so there is nothing to read a forked lookup through. */
    if (mesh_tcp_transport_connect(transport, "meshtastic.local") != -ENOTSUP) {
        record_failure(test_name, "a name with no loop should be refused");
        goto cleanup;
    }
    if (!transport->ops->take_error(transport, error, sizeof error) || error[0] == '\0') {
        record_failure(test_name, "that refusal should reach the user too");
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
/*
 * A press on a network row reaches the network transport.
 *
 * The dispatch it goes through used to be "serial, or else Bluetooth", and each of those casts
 * transport->state to its own struct - so the wrong arm is not a connect that fails but a read
 * of one transport's state through another's type, which is the disconnect chain's bug in the
 * other direction. Nothing offers this press yet (a row is connectable only while it is not
 * connected, and a network link's one row exists only while it is), so this pins the routing
 * ahead of the row that will use it.
 */
MESH_TEST_CASE(tcp_connect_routes_to_the_network_transport, unit) {
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_app app;
    memset(&app, 0, sizeof app);
    const char *failure = NULL;
    bool app_ready = false;
    char home_dir[] = "/tmp/mesh_tcp_routeXXXXXX";
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

    /* No host configured: the press is what supplies the address, which is also what proves it
       landed in the network preference rather than the Bluetooth one. */
    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_ble = false;
    config.enable_serial = false;
    config.enable_tcp = true;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }

    if (app.ui_controller.on_action == NULL) {
        failure = "the app should have installed a UI action handler";
        goto cleanup;
    }

    /* Through the handler a press goes through rather than the routing function underneath it,
       so the log line, the device history and the dispatch are all on the path under test. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_CONNECT;
    action.kind = (uint8_t)MESH_UI_DEVICE_TCP;
    snprintf(action.identifier, sizeof action.identifier, "%s", radio.target);
    app.ui_controller.on_action(app.ui_controller.action_userdata, &action);

    struct mesh_transport *transport = mesh_tcp_transport();
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        (void)mesh_event_loop_run(&app.loop, 200);
    }
    if (mesh_tcp_transport_connected_target(transport) == NULL) {
        failure = "the press should have reached the network transport";
        goto cleanup;
    }
    if (!tcp_test_radio_accept(&radio)) {
        failure = "the listener did not see the connection";
        goto cleanup;
    }

    if (strcmp(app.config.preferred_tcp_host, radio.target) != 0) {
        failure = "the address should become the network preference";
        goto cleanup;
    }
    if (app.config.preferred_ble_device[0] != '\0') {
        failure = "an address is not a radio to look for over the air";
        goto cleanup;
    }
    /* The recent-device history ranks a scan, and a host is in no scan. */
    if (mesh_ui_preferences_device_rank(&app.ui_preferences, radio.target,
                                        (uint8_t)MESH_UI_DEVICE_TCP) >= 0) {
        failure = "a network host does not belong in the scan-ranking history";
        goto cleanup;
    }

    /*
     * And the press has to survive the link, or it is a choice that lasts until the first drop.
     * Auto-connect goes back to the host the *transport* was pointed at, so a press that
     * reached only the app's preference would send it to whatever the startup configuration
     * named - which here is nothing, so the network arm would be skipped entirely and the host
     * the user picked would never be reconnected.
     */
    (void)mesh_tcp_transport_disconnect(transport);
    if (mesh_tcp_transport_connected_target(transport) != NULL) {
        failure = "the link should be down before auto-connect is asked to rebuild it";
        goto cleanup;
    }

    mesh_app_autoconnect(&app);
    if (mesh_tcp_transport_connected_target(transport) == NULL &&
        !mesh_tcp_transport_is_connecting(transport)) {
        failure = "auto-connect should go back to the host the press chose";
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

/*
 * A network link ending is not the Bluetooth radio rebooting.
 *
 * Auto-connect waits half a minute for the preferred node after a drop, rather than the five
 * seconds another radio of ours in earshot normally costs it, because the commonest way a link
 * ends is a settings write rebooting that radio. A cable or a host says nothing about whether
 * the radio over the air is on its way back, and arming that wait for every transport left a
 * client that had once been on a host sitting through half a minute of "connecting..." before
 * it would take the radio in the room. app_autoconnect_holds_the_slot_for_a_rebooting_radio is
 * the same rule from the other side.
 */
MESH_TEST_CASE(tcp_link_leaves_the_bluetooth_grace_short, unit) {
    struct tcp_test_radio radio;
    tcp_test_radio_init(&radio);
    struct mesh_app app;
    memset(&app, 0, sizeof app);
    const char *failure = NULL;
    bool app_ready = false;
    bool mock_enabled = false;
    char home_dir[] = "/tmp/mesh_tcp_graceXXXXXX";
    bool home_made = false;

    /* The preferred radio is the one left at home - an rssi of 0 is a bond with nothing
       advertising behind it - and the other is in earshot and ours, so it is what the short
       grace hands the slot to. */
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = 0, .paired = true},
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -70, .paired = true},
    };
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 2U,
    };

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
    mesh_bluez_client_mock_enable(&mock_config);
    mock_enabled = true;

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_ble = true;
    config.enable_serial = false;
    config.enable_tcp = true;
    snprintf(config.preferred_ble_device, sizeof config.preferred_ble_device, "%s",
             mock_devices[0].address);

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    mesh_ble_transport_refresh_devices(ble);
    (void)mesh_ui_preferences_note_device(&app.ui_preferences, mock_devices[1].address,
                                          (uint8_t)MESH_UI_DEVICE_BLE);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_CONNECT;
    action.kind = (uint8_t)MESH_UI_DEVICE_TCP;
    snprintf(action.identifier, sizeof action.identifier, "%s", radio.target);
    app.ui_controller.on_action(app.ui_controller.action_userdata, &action);

    struct mesh_transport *tcp = mesh_tcp_transport();
    if (mesh_tcp_transport_connected_target(tcp) == NULL) {
        (void)mesh_event_loop_run(&app.loop, 200);
    }
    if (mesh_tcp_transport_connected_target(tcp) == NULL) {
        failure = "the press should have reached the network transport";
        goto cleanup;
    }
    if (!tcp_test_radio_accept(&radio)) {
        failure = "the listener did not see the connection";
        goto cleanup;
    }

    /* The turn that arms the long wait for a Bluetooth link must not arm it for this one. */
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (app.autoconnect_after_link) {
        failure = "a network link should not lengthen the wait for a radio";
        goto cleanup;
    }

    (void)mesh_tcp_transport_disconnect(tcp);
    if (mesh_tcp_transport_connected_target(tcp) != NULL) {
        failure = "the link should be down before auto-connect is asked what to do next";
        goto cleanup;
    }

    /*
     * With the network arm held off, so the turn reaches the Bluetooth one: auto-connect goes
     * back to the host the press chose first, which tcp_connect_routes_to_the_network_transport
     * already holds. Ten seconds is past the short grace and well inside the long one, so which
     * of the two applies is the whole of what the next turn answers.
     */
    const uint64_t now = mesh_time_monotonic_ms();
    app.autoconnect_tcp_retry_at_ms = now + 60000U;
    app.autoconnect_started_ms = now - 10000U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "the radio in earshot should be taken once the short grace is spent";
        goto cleanup;
    }
    (void)mesh_ble_transport_disconnect(ble);

    record_success(test_name);

cleanup:
    if (failure != NULL) {
        record_failure(test_name, failure);
    }
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    if (mock_enabled) {
        mesh_bluez_client_mock_disable();
    }
    tcp_test_radio_close(&radio);
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (home_made) {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
}

/*
 * A target the transport refused is remembered by nobody.
 *
 * The link adopts a target only once it has parsed it *and* got a socket, so several refusals
 * leave `configured` behind: a typo or a name, the transport turned off by configuration, a
 * connect already running, no descriptors left. Remembering the press through any of those
 * gives the preferences file a host the link is not reaching for - invisible while the client
 * runs, because the Devices row and auto-connect both read the transport, and then loaded on
 * the next launch as the host to retry.
 *
 * Disabled is the case under test because it is the one an enumeration of error codes misses:
 * -EINVAL is the refusal anybody thinks of, and -ENODEV, -EBUSY and -EMFILE are the three that
 * were remembered. What makes it right is not a longer list - it is that mesh_app_link_connect()
 * asks the transport which host it ended up pointed at.
 */
MESH_TEST_CASE(tcp_refused_target_is_remembered_by_nobody, unit) {
    struct mesh_app app;
    memset(&app, 0, sizeof app);
    const char *failure = NULL;
    bool app_ready = false;
    char home_dir[] = "/tmp/mesh_tcp_refusedXXXXXX";
    bool home_made = false;

    if (mkdtemp(home_dir) == NULL) {
        record_failure(test_name, "mkdtemp failed");
        return;
    }
    home_made = true;
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_ble = false;
    config.enable_serial = false;
    config.enable_tcp = false;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }

    /* A perfectly good address, so nothing about the *text* is what stops it being kept. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_CONNECT;
    action.kind = (uint8_t)MESH_UI_DEVICE_TCP;
    snprintf(action.identifier, sizeof action.identifier, "127.0.0.1:4403");
    app.ui_controller.on_action(app.ui_controller.action_userdata, &action);

    if (mesh_tcp_transport_configured_target(mesh_tcp_transport()) != NULL) {
        failure = "a disabled transport should not have adopted the target";
        goto cleanup;
    }
    if (app.config.preferred_tcp_host[0] != '\0') {
        failure = "a target the transport refused must not become the app's preference";
        goto cleanup;
    }
    if (app.ui_preferences.network_host[0] != '\0') {
        failure = "a target the transport refused must not reach the preferences file";
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
    if (home_made) {
        rmdir(home_dir);
    }
}

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
