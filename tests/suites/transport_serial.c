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
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

    /*
     * The port and the id are two different answers and only one of them names a place on the
     * bus. This device came out of the scan unbound, so before the connect it had a sysfs id
     * and no tty at all; the bind gave it "/dev/ttyUSB0". A caller that wants to know whether
     * *this board* came back - a firmware install watching for it to re-enumerate as a
     * bootloader - needs the id, and reading the label instead is a wait that times out.
     */
    if (mesh_serial_transport_connected_id(transport) == NULL ||
        strcmp(mesh_serial_transport_connected_id(transport), "1-1:1.1") != 0) {
        record_failure(test_name, "the connected id should be the sysfs interface, not the tty");
        goto cleanup_transport;
    }
    if (strcmp(mesh_serial_transport_connected_port(transport),
               mesh_serial_transport_connected_id(transport)) == 0) {
        record_failure(test_name, "the path and the id must not be the same string here");
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

/* ---- what is on the other end of the cable ----------------------------------------------------
 */

/*
 * The role detection, against a sysfs tree laid out exactly as the Brick's was measured on
 * 2026-09-10 with a T114 and a Heltec V3. The mock replaces mesh_serial_usb_scan() whole, so it
 * can prove what the transport does with a role and nothing about how the role is decided;
 * MESHCLIENT_SYSFS_USB is the seam that lets the reading itself be tested.
 */

static bool fixture_write(const char *path, const char *contents) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    const bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

/* One interface directory: <root>/<name>/{bInterfaceClass,SubClass,Protocol,Number}, plus a
   driver symlink when a driver has claimed it. */
static bool fixture_interface(const char *root, const char *name, const char *cls,
                              const char *subclass, const char *protocol, const char *number,
                              const char *driver) {
    char dir[PATH_MAX];
    char file[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, name) >= (int)sizeof dir) {
        return false;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    struct {
        const char *attr;
        const char *value;
    } attrs[] = {{"bInterfaceClass", cls},
                 {"bInterfaceSubClass", subclass},
                 {"bInterfaceProtocol", protocol},
                 {"bInterfaceNumber", number}};
    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; ++i) {
        if (snprintf(file, sizeof file, "%s/%s", dir, attrs[i].attr) >= (int)sizeof file ||
            !fixture_write(file, attrs[i].value)) {
            return false;
        }
    }
    if (driver != NULL) {
        char parent[PATH_MAX];
        char target[PATH_MAX];
        if (snprintf(parent, sizeof parent, "%s/drivers", root) >= (int)sizeof parent ||
            snprintf(target, sizeof target, "%s/%s", parent, driver) >= (int)sizeof target ||
            snprintf(file, sizeof file, "%s/driver", dir) >= (int)sizeof file) {
            return false;
        }
        if ((mkdir(parent, 0755) != 0 && errno != EEXIST) ||
            (mkdir(target, 0755) != 0 && errno != EEXIST)) {
            return false;
        }
        if (symlink(target, file) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

/* The device directory the interfaces hang off: <root>/<name>/{idVendor,idProduct,...}. */
static bool fixture_device(const char *root, const char *name, const char *vid, const char *pid,
                           const char *product) {
    char dir[PATH_MAX];
    char file[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, name) >= (int)sizeof dir) {
        return false;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    struct {
        const char *attr;
        const char *value;
    } attrs[] = {
        {"idVendor", vid}, {"idProduct", pid}, {"product", product},
        {"busnum", "2"},   {"devnum", "3"},
    };
    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; ++i) {
        if (snprintf(file, sizeof file, "%s/%s", dir, attrs[i].attr) >= (int)sizeof file ||
            !fixture_write(file, attrs[i].value)) {
            return false;
        }
    }
    return true;
}

static void fixture_remove(const char *root) {
    char command[PATH_MAX + 16];
    if (snprintf(command, sizeof command, "rm -rf '%s'", root) < (int)sizeof command) {
        (void)system(command);
    }
}

static const struct mesh_serial_device_info *find_by_id(const struct mesh_serial_device_info *list,
                                                        size_t count, const char *id) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(list[i].id, id) == 0) {
            return &list[i];
        }
    }
    return NULL;
}

MESH_TEST_CASE(serial_scan_reads_the_role_off_sysfs, unit) {
    char root[] = "/tmp/meshclient-sysfs-XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(root) == NULL, "could not make a fixture sysfs tree");

    bool built = true;
    /* 2-1: a T114 running firmware - the MCU's own USB, a CDC pair and no drive. */
    built = built && fixture_device(root, "2-1", "239a", "4405", "HT-n5262");
    built = built && fixture_interface(root, "2-1:1.0", "02", "02", "00", "00", NULL);
    built = built && fixture_interface(root, "2-1:1.1", "0a", "00", "00", "01", NULL);
    /* 3-1: the same board in its UF2 bootloader - a different product id, and a mass-storage
       Bulk-Only interface beside the CDC pair. */
    built = built && fixture_device(root, "3-1", "239a", "0071", "HT-n5262");
    built = built && fixture_interface(root, "3-1:1.0", "02", "02", "00", "00", NULL);
    built = built && fixture_interface(root, "3-1:1.1", "0a", "00", "00", "01", NULL);
    built = built && fixture_interface(root, "3-1:1.2", "08", "06", "50", "02", "usb-storage");
    /* 4-1: a Heltec V3 - a CP2102 bridge, one vendor-class interface, driver already bound. */
    built = built && fixture_device(root, "4-1", "10c4", "ea60", "CP2102 USB to UART Bridge");
    built = built && fixture_interface(root, "4-1:1.0", "ff", "00", "00", "00", "cp210x");
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_remove(root), "could not lay out the fixture tree");

    MESH_TEST_FAIL_IF_CLEANUP(setenv("MESHCLIENT_SYSFS_USB", root, 1) != 0, fixture_remove(root),
                              "could not point the scan at the fixture");

    struct mesh_serial_device_info devices[MESH_SERIAL_MAX_DEVICES];
    const size_t count = mesh_serial_usb_scan(devices, MESH_SERIAL_MAX_DEVICES);
    (void)unsetenv("MESHCLIENT_SYSFS_USB");
    fixture_remove(root);

    MESH_TEST_FAIL_IF(count != 3U, "the scan should offer all three interfaces, bootloader too");

    const struct mesh_serial_device_info *node = find_by_id(devices, count, "2-1:1.1");
    const struct mesh_serial_device_info *boot = find_by_id(devices, count, "3-1:1.1");
    const struct mesh_serial_device_info *bridge = find_by_id(devices, count, "4-1:1.0");
    MESH_TEST_FAIL_IF(node == NULL || boot == NULL || bridge == NULL,
                      "the scan lost one of the three devices");

    MESH_TEST_FAIL_IF(node->role != MESH_SERIAL_ROLE_NODE,
                      "a CDC pair with no drive is a node running firmware");
    MESH_TEST_FAIL_IF(boot->role != MESH_SERIAL_ROLE_BOOTLOADER,
                      "a CDC pair with a Bulk-Only drive beside it is a UF2 bootloader");
    MESH_TEST_FAIL_IF(bridge->role != MESH_SERIAL_ROLE_BRIDGE,
                      "a vendor-class interface on a serial driver is a UART bridge");

    /* The predicate the transport and auto-connect both ask, so they cannot disagree. */
    MESH_TEST_FAIL_IF(!mesh_serial_device_is_radio(node) || !mesh_serial_device_is_radio(bridge),
                      "a node and a bridge are both things a session can be attempted on");
    MESH_TEST_FAIL_IF(mesh_serial_device_is_radio(boot), "a bootloader is not a radio");

    /* A bridge has its driver and normal DTR; a native node needs the Brick's usbfs poke. The
       role must not have disturbed the reading that decides which. */
    MESH_TEST_FAIL_IF(!node->needs_line_state,
                      "an unbound native node still needs the usbfs line state");
    MESH_TEST_FAIL_IF(bridge->needs_line_state || bridge->control_interface >= 0,
                      "a bridge has no CDC control interface to poke");
    MESH_TEST_FAIL_IF(boot->control_interface != 0,
                      "the bootloader's control interface should still be found at 0");

    record_success(test_name);
}

/*
 * The refusal. A bootloader presents the same CDC pair the firmware did, so every step of the
 * connect would succeed and the handshake would then be asked of something that speaks no
 * protobuf - which draws as a connected radio with the progress bar turning forever.
 */
MESH_TEST_CASE(serial_transport_refuses_a_bootloader, unit) {
    struct mesh_serial_device_info device = mesh_test_serial_device();
    device.role = MESH_SERIAL_ROLE_BOOTLOADER;
    device.product_id = 0x0071U;

    struct mesh_serial_usb_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.devices = &device;
    mock.device_count = 1U;
    mock.bound_path = "/dev/ttyUSB0";
    mock.open_fd = -1;
    mesh_serial_usb_mock_enable(&mock);

    struct mesh_event_loop loop;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_event_loop_init(&loop) != 0, mesh_serial_usb_mock_disable(),
                              "event loop init failed");

    struct mesh_transport *transport = mesh_serial_transport();
    struct mesh_app_config config = mesh_app_config_default();
    MESH_TEST_FAIL_IF_CLEANUP(transport->ops->start(transport, &config, &loop) != 0,
                              (mesh_event_loop_shutdown(&loop), mesh_serial_usb_mock_disable()),
                              "serial start failed");

    const int result = mesh_serial_transport_connect(transport, "1-1:1.1");
    const size_t binds = mesh_serial_usb_mock_bind_calls();
    const size_t line_states = mesh_serial_usb_mock_line_state_calls();
    const bool connecting = mesh_serial_transport_is_connecting(transport);
    char reason[MESH_TRANSPORT_ERROR_MAX] = {0};
    const bool said_why = transport->ops->take_error(transport, reason, sizeof reason);

    transport->ops->stop(transport);
    mesh_event_loop_shutdown(&loop);
    mesh_serial_usb_mock_disable();

    MESH_TEST_FAIL_IF(result != -ENOTSUP, "connecting to a bootloader should be refused");
    MESH_TEST_FAIL_IF(binds != 0U, "a bootloader should never be bound to a serial driver");
    MESH_TEST_FAIL_IF(line_states != 0U, "a bootloader should never be sent DTR");
    MESH_TEST_FAIL_IF(connecting, "the refusal must not leave the link waking");
    MESH_TEST_FAIL_IF(!said_why, "the refusal should say why, not fail silently");

    record_success(test_name);
}
