#define _POSIX_C_SOURCE 200809L

#include "support/proto_fixture.h"

#include <pb_encode.h>

#include "meshtastic/admin.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <stdbool.h>
#include <string.h>

/* Builds a decoded MeshPacket carrying `payload` on `portnum`, as the radio would hand it to
   us inside a FromRadio. */
meshtastic_MeshPacket mesh_test_make_decoded_packet(uint32_t from, uint32_t to, uint8_t channel,
                                                    uint32_t id, meshtastic_PortNum portnum,
                                                    const void *payload, size_t payload_len) {
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_default;
    packet.from = from;
    packet.to = to;
    packet.channel = channel;
    packet.id = id;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = portnum;
    if (payload_len > sizeof(packet.decoded.payload.bytes)) {
        payload_len = sizeof(packet.decoded.payload.bytes);
    }
    memcpy(packet.decoded.payload.bytes, payload, payload_len);
    packet.decoded.payload.size = (pb_size_t)payload_len;
    return packet;
}

/* Wraps a Routing reply the way the firmware does: a ROUTING_APP Data whose request_id names
   the message being answered. */
meshtastic_MeshPacket mesh_test_make_routing_reply(uint32_t request_id,
                                                   meshtastic_Routing_Error error) {
    meshtastic_Routing routing = meshtastic_Routing_init_default;
    routing.which_variant = meshtastic_Routing_error_reason_tag;
    routing.error_reason = error;

    uint8_t payload[64];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    (void)pb_encode(&stream, meshtastic_Routing_fields, &routing);

    meshtastic_MeshPacket packet = mesh_test_make_decoded_packet(
        1U, 2U, 0U, 500U, meshtastic_PortNum_ROUTING_APP, payload, stream.bytes_written);
    packet.decoded.request_id = request_id;
    return packet;
}

/* ---- radio settings / admin ---------------------------------------------------------------- */

bool mesh_test_encode_from_radio(const meshtastic_FromRadio *message, uint8_t *out, size_t cap,
                                 size_t *out_len) {
    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, message)) {
        return false;
    }
    *out_len = stream.bytes_written;
    return true;
}

/* Builds the ADMIN_APP reply a radio would send for `admin`, quoting `request_id`. */
bool mesh_test_make_admin_reply(uint32_t my_node, uint32_t request_id,
                                const meshtastic_AdminMessage *admin, meshtastic_MeshPacket *out) {
    uint8_t payload[meshtastic_AdminMessage_size];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_AdminMessage_fields, admin)) {
        return false;
    }
    *out = mesh_test_make_decoded_packet(
        my_node, my_node, 0U, 0x5150U, meshtastic_PortNum_ADMIN_APP, payload, stream.bytes_written);
    out->decoded.request_id = request_id;
    return true;
}
