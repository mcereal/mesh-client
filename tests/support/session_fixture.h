#ifndef MESH_TEST_SUPPORT_SESSION_FIXTURE_H
#define MESH_TEST_SUPPORT_SESSION_FIXTURE_H

/* Feeding packets into a session, and reading back what it decided. */

#include "mesh/core/session.h"

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool mesh_test_session_feed_from_radio(struct mesh_session *session,
                                       const meshtastic_FromRadio *from_radio);

bool mesh_test_session_feed_app_packet(struct mesh_session *session, uint32_t from,
                                       meshtastic_PortNum portnum, const uint8_t *payload,
                                       size_t len);

const struct mesh_node_summary *mesh_test_session_find_node(const struct mesh_session *session,
                                                            uint32_t node_id);

/* A send path that keeps the last ToRadio the session handed it. */
struct mesh_test_trace_capture {
    uint8_t packet[MESH_SESSION_MAX_PACKET];
    size_t len;
    unsigned calls;
};

int mesh_test_trace_capture_fn(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id);

#endif /* MESH_TEST_SUPPORT_SESSION_FIXTURE_H */
