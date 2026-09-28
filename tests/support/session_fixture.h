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

/* A session observer that copies what it is told, since what it is handed lives only for the
   call. Install with mesh_session_set_observer(session, mesh_test_event_record_fn, &record). */
#define MESH_TEST_EVENT_MAX 32U

struct mesh_test_event {
    enum mesh_session_event_kind kind;
    uint32_t node_id;   /* NODE_*; the sender for MESSAGE */
    uint32_t packet_id; /* MESSAGE */
    uint8_t direction;  /* MESSAGE */
    bool via_mqtt;
    bool has_hops;
    uint8_t hops;
    bool has_position; /* NODE_*: the record held a fix when it was announced */
    uint32_t radio;    /* RADIO */
};

struct mesh_test_event_record {
    struct mesh_test_event events[MESH_TEST_EVENT_MAX];
    size_t count;
};

void mesh_test_event_record_fn(void *ctx, const struct mesh_session *session,
                               const struct mesh_session_event *event);
/* How many recorded events are of `kind`, and about `node_id` when it is not 0. */
size_t mesh_test_event_count(const struct mesh_test_event_record *record,
                             enum mesh_session_event_kind kind, uint32_t node_id);

#endif /* MESH_TEST_SUPPORT_SESSION_FIXTURE_H */
