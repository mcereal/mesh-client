#define _POSIX_C_SOURCE 200809L

#include "support/session_fixture.h"

#include "mesh/core/message.h"
#include "mesh/core/session.h"

#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* Feeds one FromRadio protobuf into the session. Returns false when it would not encode. */
bool mesh_test_session_feed_from_radio(struct mesh_session *session,
                                       const meshtastic_FromRadio *from_radio) {
    uint8_t buffer[512];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof buffer);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, from_radio)) {
        return false;
    }
    mesh_session_handle_from_radio(session, buffer, stream.bytes_written);
    return true;
}

/* Wraps an already-encoded app payload in a MeshPacket from `from` and feeds it in. */
bool mesh_test_session_feed_app_packet(struct mesh_session *session, uint32_t from,
                                       meshtastic_PortNum portnum, const uint8_t *payload,
                                       size_t len) {
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    from_radio.packet.from = from;
    from_radio.packet.to = MESH_MESSAGE_BROADCAST_ADDR;
    from_radio.packet.id = 0x51EEU;
    from_radio.packet.has_rx_time = true;
    from_radio.packet.rx_time = 1750000000U;
    from_radio.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    from_radio.packet.decoded.portnum = portnum;
    if (len > sizeof from_radio.packet.decoded.payload.bytes) {
        return false;
    }
    memcpy(from_radio.packet.decoded.payload.bytes, payload, len);
    from_radio.packet.decoded.payload.size = (pb_size_t)len;
    return mesh_test_session_feed_from_radio(session, &from_radio);
}

const struct mesh_node_summary *mesh_test_session_find_node(const struct mesh_session *session,
                                                            uint32_t node_id) {
    for (size_t i = 0; i < session->handshake.node_count; ++i) {
        if (session->handshake.nodes[i].node_id == node_id) {
            return &session->handshake.nodes[i];
        }
    }
    return NULL;
}

int mesh_test_trace_capture_fn(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id) {
    (void)packet_id;
    struct mesh_test_trace_capture *capture = (struct mesh_test_trace_capture *)ctx;
    if (len > sizeof capture->packet) {
        return -EMSGSIZE;
    }
    memcpy(capture->packet, packet, len);
    capture->len = len;
    capture->calls++;
    return 0;
}
