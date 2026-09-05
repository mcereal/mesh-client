#ifndef MESH_TEST_SUPPORT_PROTO_FIXTURE_H
#define MESH_TEST_SUPPORT_PROTO_FIXTURE_H

/* Builders for the protobuf messages a radio would put on the wire. */

#include "meshtastic/admin.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

meshtastic_MeshPacket mesh_test_make_decoded_packet(uint32_t from, uint32_t to, uint8_t channel,
                                                    uint32_t id, meshtastic_PortNum portnum,
                                                    const void *payload, size_t payload_len);

meshtastic_MeshPacket mesh_test_make_routing_reply(uint32_t request_id,
                                                   meshtastic_Routing_Error error);

bool mesh_test_encode_from_radio(const meshtastic_FromRadio *message, uint8_t *out, size_t cap,
                                 size_t *out_len);

bool mesh_test_make_admin_reply(uint32_t my_node, uint32_t request_id,
                                const meshtastic_AdminMessage *admin, meshtastic_MeshPacket *out);

#endif /* MESH_TEST_SUPPORT_PROTO_FIXTURE_H */
