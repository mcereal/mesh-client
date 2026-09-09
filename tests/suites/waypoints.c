#define _POSIX_C_SOURCE 200809L

/*
 * Waypoints: the book, the wire, the geometry and the two screens.
 *
 * The cases here are grouped by what they hold rather than by which file they touch, because
 * the interesting promises cross the seams: an id is what a place *is*, so the same id twice is
 * an edit at every level; an expiry in 1970 is a withdrawal at every level; and a range is only
 * ever the answer the geo module gives, so a screen cannot round it a second way.
 */

#include "framework/mesh_test.h"
#include "support/session_fixture.h"
#include "support/ui_fixture.h"

#include "mesh/core/session.h"
#include "mesh/core/waypoint.h"
#include "mesh/geo/vector.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"
#include "mesh/ui/waypoints.h"
#include "mesh/utils/time.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A fixed wall clock, so an age and an expiry are the same on every machine and in any year. */
#define WP_NOW 1750000000U

/* Two points about eleven kilometres apart, which is a mesh's own scale rather than a
   continent's: Greenwich, and a point due east of it. */
#define WP_HOME_LAT 514800000
#define WP_HOME_LON 0
#define WP_EAST_LAT 514800000
#define WP_EAST_LON 1600000

/* ---- the geometry ---------------------------------------------------------------------------- */

/*
 * Distances and bearings against hand-checked values.
 *
 * The tolerances are the point: a sphere is not an ellipsoid and this deliberately does not
 * pretend to be, so the check is that the answer is right to within the half percent haversine
 * costs - not that it matches a geodesic to the metre. A test tighter than the model would be a
 * test that fails when somebody is right.
 */
MESH_TEST_CASE(geo_vector_distance_and_bearing, unit) {
    struct mesh_geo_vector vector;

    /* Due north, one degree of latitude: a meridian degree is 111.19 km on this sphere. */
    MESH_TEST_FAIL_IF(!mesh_geo_vector_between(0, 0, 10000000, 0, &vector),
                      "a degree north should be measurable");
    MESH_TEST_FAIL_IF(fabs(vector.distance_m - 111194.9) > 200.0,
                      "a degree of latitude should be about 111 km");
    MESH_TEST_FAIL_IF(!vector.has_bearing || fabs(vector.bearing_deg) > 0.01,
                      "due north should bear 0");

    /* Due east along the equator, where a degree of longitude is the same length. */
    MESH_TEST_FAIL_IF(!mesh_geo_vector_between(0, 0, 0, 10000000, &vector),
                      "a degree east should be measurable");
    MESH_TEST_FAIL_IF(fabs(vector.distance_m - 111194.9) > 200.0,
                      "a degree of longitude on the equator should be about 111 km");
    MESH_TEST_FAIL_IF(!vector.has_bearing || fabs(vector.bearing_deg - 90.0) > 0.01,
                      "due east should bear 90");

    /* And the reverse, which is the one an initial bearing gets wrong if the arguments are
       swapped anywhere in the arithmetic. */
    MESH_TEST_FAIL_IF(!mesh_geo_vector_between(0, 10000000, 0, 0, &vector),
                      "the return leg should be measurable");
    MESH_TEST_FAIL_IF(!vector.has_bearing || fabs(vector.bearing_deg - 270.0) > 0.01,
                      "due west should bear 270");

    /* Short range, which is what a mesh actually spans and where the law of cosines would have
       lost its precision: a degree at this latitude is about 11.6 km of longitude. */
    MESH_TEST_FAIL_IF(
        !mesh_geo_vector_between(WP_HOME_LAT, WP_HOME_LON, WP_EAST_LAT, WP_EAST_LON, &vector),
        "a short hop should be measurable");
    MESH_TEST_FAIL_IF(fabs(vector.distance_m - 11128.0) > 120.0,
                      "0.16 degrees of longitude at 51.48N should be about 11 km");
    MESH_TEST_FAIL_IF(!vector.has_bearing || fabs(vector.bearing_deg - 90.0) > 0.2,
                      "a short hop due east should bear about 90");

    /* Standing on it: no distance, and - the part that matters - no direction, rather than the
       due north atan2(0, 0) would otherwise hand back. */
    MESH_TEST_FAIL_IF(
        !mesh_geo_vector_between(WP_HOME_LAT, WP_HOME_LON, WP_HOME_LAT, WP_HOME_LON, &vector),
        "a point should be measurable against itself");
    MESH_TEST_FAIL_IF(vector.distance_m != 0.0, "a point is no distance from itself");
    MESH_TEST_FAIL_IF(vector.has_bearing, "a point has no direction from itself");

    /* An unmeasurable pair is refused rather than answered, and it is coords_valid() that says
       so - the same one question every ingress asks. */
    MESH_TEST_FAIL_IF(mesh_geo_vector_between(2000000000, 0, 0, 0, &vector),
                      "an off-Earth latitude should have no vector");
    MESH_TEST_FAIL_IF(vector.distance_m != 0.0 || vector.has_bearing,
                      "a refused vector should be zeroed");

    record_success(test_name);
}

/* Each point owns the 45 degrees centred on its own name, so the seams are what to check. */
MESH_TEST_CASE(geo_compass_points, unit) {
    const struct {
        double bearing;
        enum mesh_geo_compass point;
    } cases[] = {
        {0.0, MESH_GEO_COMPASS_N},
        {22.4, MESH_GEO_COMPASS_N},
        {22.6, MESH_GEO_COMPASS_NE},
        {45.0, MESH_GEO_COMPASS_NE},
        {90.0, MESH_GEO_COMPASS_E},
        {135.0, MESH_GEO_COMPASS_SE},
        {180.0, MESH_GEO_COMPASS_S},
        {225.0, MESH_GEO_COMPASS_SW},
        {270.0, MESH_GEO_COMPASS_W},
        {315.0, MESH_GEO_COMPASS_NW},
        /* Either side of north, which is the wrap the offset exists for. */
        {337.4, MESH_GEO_COMPASS_NW},
        {337.6, MESH_GEO_COMPASS_N},
        {359.9, MESH_GEO_COMPASS_N},
        /* Out of range both ways rather than indexing off the end of the table. */
        {360.0, MESH_GEO_COMPASS_N},
        {-90.0, MESH_GEO_COMPASS_W},
        {720.0 + 90.0, MESH_GEO_COMPASS_E},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_geo_compass_of(cases[i].bearing) != cases[i].point,
                          "a bearing landed on the wrong compass point");
    }
    record_success(test_name);
}

/* ---- the book ----------------------------------------------------------------------------------
 */

/* Builds a WAYPOINT_APP MeshPacket the way a radio would hand one over. */
static bool wp_packet(meshtastic_MeshPacket *packet, uint32_t from, uint32_t id, const char *name,
                      int32_t latitude_i, int32_t longitude_i, uint32_t expire) {
    memset(packet, 0, sizeof *packet);
    packet->from = from;
    packet->to = 0xFFFFFFFFU;
    packet->channel = 0U;
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_WAYPOINT_APP;

    meshtastic_Waypoint waypoint = meshtastic_Waypoint_init_default;
    waypoint.id = id;
    waypoint.expire = expire;
    waypoint.has_latitude_i = true;
    waypoint.latitude_i = latitude_i;
    waypoint.has_longitude_i = true;
    waypoint.longitude_i = longitude_i;
    snprintf(waypoint.name, sizeof waypoint.name, "%s", name);

    pb_ostream_t stream =
        pb_ostream_from_buffer(packet->decoded.payload.bytes, sizeof packet->decoded.payload.bytes);
    if (!pb_encode(&stream, meshtastic_Waypoint_fields, &waypoint)) {
        return false;
    }
    packet->decoded.payload.size = (pb_size_t)stream.bytes_written;
    return true;
}

/*
 * The one thing that makes the book a table rather than a log: an id.
 *
 * Upstream edits a waypoint by re-broadcasting it with the same id, so the second copy has to
 * land on the first. The message ring next door does the opposite - two packets are two things
 * that happened - and getting these two the same way round is the bug this pins.
 */
MESH_TEST_CASE(waypoint_same_id_is_an_edit, unit) {
    struct mesh_waypoint_book book;
    mesh_waypoint_book_reset(&book);

    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 7U, "Bridge", WP_HOME_LAT, WP_HOME_LON, 0U),
                      "could not encode a waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 1,
                      "a waypoint should have been stored");
    MESH_TEST_FAIL_IF(book.count != 1U, "one waypoint is one entry");

    /* Same id, moved and renamed. */
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 7U, "Ford", WP_EAST_LAT, WP_EAST_LON, 0U),
                      "could not encode the edit");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW + 60U) != 1,
                      "an edit should have been stored");
    MESH_TEST_FAIL_IF(book.count != 1U, "an edit is not a second place");
    const struct mesh_waypoint *entry = mesh_waypoint_book_get(&book, 7U);
    MESH_TEST_FAIL_IF(entry == NULL, "the edited place should still be findable by its id");
    MESH_TEST_FAIL_IF(strcmp(entry->name, "Ford") != 0, "the edit should have taken");
    MESH_TEST_FAIL_IF(entry->longitude_i != WP_EAST_LON, "the place should have moved");

    /* A different id from the same sender is a different place. */
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 8U, "Gate", WP_HOME_LAT, WP_HOME_LON, 0U),
                      "could not encode a second waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 1,
                      "a second waypoint should have been stored");
    MESH_TEST_FAIL_IF(book.count != 2U, "two ids are two places");

    /* An id of 0 is not an id: there would be nothing to key on, and the second copy of one
       would be a third place rather than an edit. */
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 0U, "Nowhere", WP_HOME_LAT, WP_HOME_LON, 0U),
                      "could not encode an id-less waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 0,
                      "a waypoint with no id should be ignored");
    MESH_TEST_FAIL_IF(book.count != 2U, "an id-less waypoint should not be stored");

    record_success(test_name);
}

/*
 * Withdrawal, which is not a verb on the wire.
 *
 * Every client deletes a place by re-broadcasting it with an expiry already past, and the two
 * halves of that have to be told apart: an expiry from 1970 is a tombstone and is honoured with
 * no clock at all, while a real date is only an expiry once we know what time it is. A Brick has
 * no wall clock, so the second case is the common one and answering it with a guess would drop
 * places that are still there.
 */
MESH_TEST_CASE(waypoint_expiry_and_withdrawal, unit) {
    struct mesh_waypoint waypoint;
    memset(&waypoint, 0, sizeof waypoint);

    MESH_TEST_FAIL_IF(mesh_waypoint_state(&waypoint, WP_NOW) != MESH_WAYPOINT_LIVE,
                      "an expiry of 0 means never");

    waypoint.expire = MESH_WAYPOINT_EXPIRE_DELETED;
    MESH_TEST_FAIL_IF(mesh_waypoint_state(&waypoint, WP_NOW) != MESH_WAYPOINT_DELETED,
                      "an expiry in 1970 is a withdrawal");
    MESH_TEST_FAIL_IF(mesh_waypoint_state(&waypoint, 0U) != MESH_WAYPOINT_DELETED,
                      "a withdrawal is honoured with no clock at all");

    waypoint.expire = WP_NOW + 3600U;
    MESH_TEST_FAIL_IF(mesh_waypoint_state(&waypoint, WP_NOW) != MESH_WAYPOINT_LIVE,
                      "an expiry in the future has not happened");
    waypoint.expire = WP_NOW - 1U;
    MESH_TEST_FAIL_IF(mesh_waypoint_state(&waypoint, WP_NOW) != MESH_WAYPOINT_EXPIRED,
                      "an expiry in the past has");
    MESH_TEST_FAIL_IF(mesh_waypoint_state(&waypoint, 0U) != MESH_WAYPOINT_LIVE,
                      "an unknown clock cannot expire anything");

    /* And the same through the wire, which is where it matters. */
    struct mesh_waypoint_book book;
    mesh_waypoint_book_reset(&book);
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 9U, "Camp", WP_HOME_LAT, WP_HOME_LON, 0U),
                      "could not encode a waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 1, "should have stored");

    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 9U, "Camp", WP_HOME_LAT, WP_HOME_LON,
                                 MESH_WAYPOINT_EXPIRE_DELETED),
                      "could not encode a withdrawal");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 1,
                      "a withdrawal of a place we hold changes the book");
    MESH_TEST_FAIL_IF(book.count != 0U, "a withdrawn place should be gone");

    /* A withdrawal of something we never had adds nothing and is not an error. */
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 0,
                      "withdrawing a place we do not hold should change nothing");

    record_success(test_name);
}

/*
 * A dated expiry is honoured once there is a clock to read it against, and only then.
 *
 * Two halves, and the second is the one that is easy to get wrong: a place that had already
 * expired when it reached us is dropped rather than stored, and a place whose deadline passes
 * while we hold it is retired by the tick - because nothing on the mesh re-announces an expiry,
 * so the clock is the only thing that can. With no credible clock both do nothing, which is the
 * honest reading rather than a guess: the Brick has no RTC battery.
 */
MESH_TEST_CASE(waypoint_dated_expiry_is_honoured, unit) {
    struct mesh_waypoint_book book;
    mesh_waypoint_book_reset(&book);

    /* Already expired when it arrives, against the clock it arrived with. */
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(
        !wp_packet(&packet, 0x1111U, 21U, "Gone", WP_HOME_LAT, WP_HOME_LON, WP_NOW - 60U),
        "could not encode an expired waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 0,
                      "a place that arrived expired adds nothing");
    MESH_TEST_FAIL_IF(book.count != 0U, "a place that arrived expired should not be stored");

    /* The same packet with no clock to read it against is stored: a client that cannot read
       dates has no business deciding one has passed. */
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, 0U) != 1,
                      "with no clock the place is stored");
    MESH_TEST_FAIL_IF(book.count != 1U, "with no clock the place is stored");

    /* An expiry we are holding and that has since passed is retired by the prune - and the
       prune with no clock retires nothing, which is the same answer. */
    MESH_TEST_FAIL_IF(mesh_waypoint_book_prune(&book, 0U) != 0U,
                      "with no clock the prune retires nothing");
    MESH_TEST_FAIL_IF(book.count != 1U, "with no clock nothing goes");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_prune(&book, WP_NOW) != 1U,
                      "a deadline that has passed retires the place");
    MESH_TEST_FAIL_IF(book.count != 0U, "an expired place should be gone");

    /* A future deadline and a place that never expires both survive a prune. */
    MESH_TEST_FAIL_IF(
        !wp_packet(&packet, 0x1111U, 22U, "Later", WP_HOME_LAT, WP_HOME_LON, WP_NOW + 3600U),
        "could not encode a future expiry");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 1, "should have stored");
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 23U, "Forever", WP_HOME_LAT, WP_HOME_LON, 0U),
                      "could not encode an endless place");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 1, "should have stored");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_prune(&book, WP_NOW) != 0U,
                      "a deadline that has not passed keeps its place");
    MESH_TEST_FAIL_IF(book.count != 2U, "both should have survived");
    /* And the first of them goes once its hour is up, while the endless one stays. */
    MESH_TEST_FAIL_IF(mesh_waypoint_book_prune(&book, WP_NOW + 7200U) != 1U,
                      "the hour should have retired exactly one place");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_get(&book, 23U) == NULL,
                      "a place with no expiry outlives every clock");

    record_success(test_name);
}

/*
 * A radio swap takes the places with the roster, and a reconnect does not.
 *
 * This is the roster's rule rather than the message log's, and the channel is why: a message's
 * channel is a label on something that already happened, while a waypoint's is an index into
 * the table the swap has just discarded - so "share it again" on a carried-over place would
 * broadcast on whatever slot that number names on the new radio.
 */
MESH_TEST_CASE(waypoint_book_follows_the_radio, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0xAAAA0001U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info),
                      "the first radio should introduce itself");

    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 41U, "Theirs", WP_HOME_LAT, WP_HOME_LON, 0U),
                      "could not encode a waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&session.waypoints, &packet, 0U, WP_NOW) != 1,
                      "the place should have been stored");

    /* The same radio again is a reconnect: the places stay, exactly as the roster does. */
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info),
                      "the same radio should introduce itself again");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_get(&session.waypoints, 41U) == NULL,
                      "a reconnect to the same radio keeps the places");

    /* A different radio is a different mesh and a different channel table. */
    my_info.my_info.my_node_num = 0xBBBB0002U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info),
                      "the second radio should introduce itself");
    MESH_TEST_FAIL_IF(session.waypoints.count != 0U,
                      "a radio swap should take the places with the roster");

    record_success(test_name);
}

/*
 * A coordinate off the air is range-checked like every other, and a place without one still
 * lists: the name somebody shared is real even when the point is not.
 */
MESH_TEST_CASE(waypoint_refuses_a_coordinate_off_earth, unit) {
    struct mesh_waypoint_book book;
    mesh_waypoint_book_reset(&book);

    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x1111U, 11U, "Nowhere", 2000000000, 0, 0U),
                      "could not encode a bad waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &packet, 0U, WP_NOW) != 1,
                      "the place should still be stored");
    const struct mesh_waypoint *entry = mesh_waypoint_book_get(&book, 11U);
    MESH_TEST_FAIL_IF(entry == NULL, "the place should be findable");
    MESH_TEST_FAIL_IF(entry->has_coords, "a coordinate off Earth is not a coordinate");
    MESH_TEST_FAIL_IF(strcmp(entry->name, "Nowhere") != 0, "the name it shared is still real");

    record_success(test_name);
}

/* A full book evicts the least recently heard - but never one of ours while somebody else's is
   available to go instead, because the mesh can re-broadcast theirs and cannot re-broadcast
   ours. */
MESH_TEST_CASE(waypoint_book_keeps_our_own_places, unit) {
    struct mesh_waypoint_book book;
    mesh_waypoint_book_reset(&book);

    /* One of ours, heard first and therefore the oldest thing in the book. */
    struct mesh_waypoint mine;
    memset(&mine, 0, sizeof mine);
    mine.id = 1U;
    mine.ours = true;
    mine.heard = WP_NOW;
    mine.has_coords = true;
    snprintf(mine.name, sizeof mine.name, "%s", "Mine");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_store(&book, &mine) == NULL, "should have stored ours");

    /* Fill the rest with somebody else's, each heard later than the last. */
    for (uint32_t i = 1U; i < MESH_WAYPOINT_BOOK_CAPACITY; ++i) {
        struct mesh_waypoint theirs;
        memset(&theirs, 0, sizeof theirs);
        theirs.id = 100U + i;
        theirs.from = 0x2222U;
        theirs.heard = WP_NOW + i;
        MESH_TEST_FAIL_IF(mesh_waypoint_book_store(&book, &theirs) == NULL, "should have stored");
    }
    MESH_TEST_FAIL_IF(book.count != MESH_WAYPOINT_BOOK_CAPACITY, "the book should be full");

    struct mesh_waypoint extra;
    memset(&extra, 0, sizeof extra);
    extra.id = 999U;
    extra.from = 0x3333U;
    extra.heard = WP_NOW + 1000U;
    MESH_TEST_FAIL_IF(mesh_waypoint_book_store(&book, &extra) == NULL, "should have stored");
    MESH_TEST_FAIL_IF(book.count != MESH_WAYPOINT_BOOK_CAPACITY, "the book should still be full");
    MESH_TEST_FAIL_IF(book.dropped != 1U, "an eviction should be counted");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_get(&book, 1U) == NULL,
                      "our own place should have survived the eviction");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_get(&book, 101U) != NULL,
                      "the oldest of somebody else's should have gone instead");

    record_success(test_name);
}

/* ---- the wire ----------------------------------------------------------------------------------
 */

/*
 * A waypoint goes out as a broadcast on WAYPOINT_APP, and comes back decodable as the same
 * place. The round trip is the check that matters: encoding into the wrong port or dropping the
 * has_ bits off the coordinates both produce a packet that encodes cleanly and means nothing.
 */
MESH_TEST_CASE(waypoint_encode_round_trip, unit) {
    struct mesh_waypoint waypoint;
    memset(&waypoint, 0, sizeof waypoint);
    waypoint.id = 4242U;
    waypoint.has_coords = true;
    waypoint.latitude_i = WP_HOME_LAT;
    waypoint.longitude_i = WP_HOME_LON;
    waypoint.locked_to = 0x1234U;
    snprintf(waypoint.name, sizeof waypoint.name, "%s", "Trailhead");
    snprintf(waypoint.description, sizeof waypoint.description, "%s", "gate on the left");

    const struct mesh_waypoint_request request = {
        .waypoint = &waypoint,
        .packet_id = 77U,
        .channel = 3U,
    };
    uint8_t buffer[MESH_SESSION_MAX_PACKET];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(mesh_waypoint_encode(&request, buffer, sizeof buffer, &written) != 0,
                      "the waypoint should encode");
    MESH_TEST_FAIL_IF(written == 0U, "the encoder should have written something");

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(buffer, written);
    MESH_TEST_FAIL_IF(!pb_decode(&stream, meshtastic_ToRadio_fields, &to_radio),
                      "the packet should decode");
    MESH_TEST_FAIL_IF(to_radio.which_payload_variant != meshtastic_ToRadio_packet_tag,
                      "a waypoint travels as a packet");
    MESH_TEST_FAIL_IF(to_radio.packet.decoded.portnum != meshtastic_PortNum_WAYPOINT_APP,
                      "a waypoint travels on WAYPOINT_APP");
    MESH_TEST_FAIL_IF(to_radio.packet.to != 0xFFFFFFFFU, "a waypoint is shared, not sent");
    MESH_TEST_FAIL_IF(to_radio.packet.want_ack, "a broadcast is never acked");
    MESH_TEST_FAIL_IF(to_radio.packet.channel != 3U, "the channel should have travelled");

    /* And back through the ingest, which is what a receiving client would do with it. */
    struct mesh_waypoint_book book;
    mesh_waypoint_book_reset(&book);
    to_radio.packet.from = 0x9999U;
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&book, &to_radio.packet, 0U, WP_NOW) != 1,
                      "the encoded waypoint should be ingestable");
    const struct mesh_waypoint *back = mesh_waypoint_book_get(&book, 4242U);
    MESH_TEST_FAIL_IF(back == NULL, "the round trip should keep the id");
    MESH_TEST_FAIL_IF(strcmp(back->name, "Trailhead") != 0, "the name should survive");
    MESH_TEST_FAIL_IF(strcmp(back->description, "gate on the left") != 0,
                      "the description should survive");
    MESH_TEST_FAIL_IF(!back->has_coords || back->latitude_i != WP_HOME_LAT ||
                          back->longitude_i != WP_HOME_LON,
                      "the coordinates should survive");
    MESH_TEST_FAIL_IF(back->locked_to != 0x1234U, "the lock should survive");

    record_success(test_name);
}

/*
 * Sharing through the session, and the one thing a re-share must not do.
 *
 * Re-broadcasting somebody else's place is a broadcast from this radio of a waypoint that is
 * still theirs. Stamping it as ours on the way out would have the list read "you" under a name
 * somebody else chose - and it is one line of difference from the case that *should* claim it,
 * which is a place this client has just made.
 */
MESH_TEST_CASE(waypoint_send_keeps_whose_place_it_is, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    /* Somebody else's place arrives off the air. */
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!wp_packet(&packet, 0x2222U, 31U, "Their gate", WP_HOME_LAT, WP_HOME_LON, 0U),
                      "could not encode a waypoint");
    MESH_TEST_FAIL_IF(mesh_waypoint_ingest(&session.waypoints, &packet, 0U, WP_NOW) != 1,
                      "the place should have been stored");

    /* Re-shared unchanged: it goes out, and it is still theirs. */
    struct mesh_waypoint theirs = *mesh_waypoint_book_get(&session.waypoints, 31U);
    MESH_TEST_FAIL_IF(mesh_session_send_waypoint(&session, &theirs, 0U, NULL) != 0,
                      "re-sharing should go out");
    MESH_TEST_FAIL_IF(capture.calls != 1U, "re-sharing should have sent exactly one packet");
    const struct mesh_waypoint *after = mesh_waypoint_book_get(&session.waypoints, 31U);
    MESH_TEST_FAIL_IF(after == NULL, "the place should still be in the book");
    MESH_TEST_FAIL_IF(after->ours, "re-sharing somebody else's place does not make it ours");
    MESH_TEST_FAIL_IF(after->from != 0x2222U, "re-sharing should not rewrite who shared it");

    /* A place we make ourselves has no sender yet, and this is what gives it one. */
    struct mesh_waypoint mine;
    memset(&mine, 0, sizeof mine);
    mine.has_coords = true;
    mine.latitude_i = WP_EAST_LAT;
    mine.longitude_i = WP_EAST_LON;
    snprintf(mine.name, sizeof mine.name, "%s", "Ours");
    uint32_t id = 0U;
    MESH_TEST_FAIL_IF(mesh_session_send_waypoint(&session, &mine, 0U, &id) != 0,
                      "a new place should go out");
    MESH_TEST_FAIL_IF(id == 0U, "the session should have drawn an id for it");
    const struct mesh_waypoint *stored = mesh_waypoint_book_get(&session.waypoints, id);
    MESH_TEST_FAIL_IF(stored == NULL, "a new place should be in the book");
    MESH_TEST_FAIL_IF(!stored->ours, "a place we made is ours");

    /* A place with no place is refused rather than broadcast. */
    struct mesh_waypoint nowhere;
    memset(&nowhere, 0, sizeof nowhere);
    snprintf(nowhere.name, sizeof nowhere.name, "%s", "Nowhere");
    MESH_TEST_FAIL_IF(mesh_session_send_waypoint(&session, &nowhere, 0U, NULL) != -EINVAL,
                      "a waypoint with no coordinates is not one");

    /* And with no link at all the place is still kept: naming a place is not a message that
       failed to send, and the next share re-broadcasts the same id. */
    mesh_session_detach(&session);
    struct mesh_waypoint offline;
    memset(&offline, 0, sizeof offline);
    offline.has_coords = true;
    offline.latitude_i = WP_HOME_LAT;
    offline.longitude_i = WP_HOME_LON;
    snprintf(offline.name, sizeof offline.name, "%s", "Offline");
    uint32_t offline_id = 0U;
    MESH_TEST_FAIL_IF(mesh_session_send_waypoint(&session, &offline, 0U, &offline_id) != -ENOTCONN,
                      "with no link the share should report that");
    MESH_TEST_FAIL_IF(mesh_waypoint_book_get(&session.waypoints, offline_id) == NULL,
                      "a place made with no radio is still a place");

    record_success(test_name);
}

/*
 * The two declarations of upstream's limits - the core's, next to the encoder, and the store's,
 * on the nanopb-free side of the seam - have to agree.
 *
 * They are separate on purpose: mesh/core/waypoint.h takes a meshtastic_MeshPacket, and
 * including it from store.h would drag the generated protobuf headers into every backend. This
 * is what keeps two declarations of one limit honest, and it is why the store's are stated as
 * buffer sizes: one more than the core's, for the NUL.
 */
MESH_TEST_CASE(waypoint_limits_agree_across_the_seam, unit) {
    MESH_TEST_FAIL_IF(MESH_UI_WAYPOINT_NAME_MAX != MESH_WAYPOINT_NAME_MAX + 1U,
                      "the UI's name buffer should hold the core's name and a NUL");
    MESH_TEST_FAIL_IF(MESH_UI_WAYPOINT_DESCRIPTION_MAX != MESH_WAYPOINT_DESCRIPTION_MAX + 1U,
                      "the UI's description buffer should hold the core's and a NUL");
    MESH_TEST_FAIL_IF(MESH_UI_MAX_WAYPOINTS != MESH_WAYPOINT_BOOK_CAPACITY,
                      "the UI list should be exactly as long as the book");
    record_success(test_name);
}

/* ---- the screens -------------------------------------------------------------------------------
 */

/* A store holding our own radio at Greenwich and a place to the east of it. */
static void wp_store_populate(struct mesh_ui_store *store) {
    struct mesh_ui_handshake_state hs;
    memset(&hs, 0, sizeof hs);
    hs.has_my_info = true;
    hs.my_info.node_num = 0x1000U;
    hs.node_count = 1U;
    hs.nodes[0].node_id = 0x1000U;
    snprintf(hs.nodes[0].short_name, sizeof hs.nodes[0].short_name, "%s", "HOME");
    hs.nodes[0].position.valid = true;
    hs.nodes[0].position.latitude_i = WP_HOME_LAT;
    hs.nodes[0].position.longitude_i = WP_HOME_LON;
    mesh_ui_store_set_handshake(store, &hs);
}

static void wp_list_add(struct mesh_ui_waypoint_list *list, uint32_t id, const char *name,
                        bool has_coords, int32_t latitude_i, int32_t longitude_i, uint32_t heard) {
    struct mesh_ui_waypoint *entry = &list->entries[list->count++];
    memset(entry, 0, sizeof *entry);
    entry->id = id;
    entry->has_coords = has_coords;
    entry->latitude_i = latitude_i;
    entry->longitude_i = longitude_i;
    entry->heard = heard;
    entry->editable = true;
    snprintf(entry->name, sizeof entry->name, "%s", name);
}

/*
 * The list's order, and the row that is always on the end of it.
 *
 * Nearest first is the whole reason the tab is useful without a map, and the two groups are the
 * part worth pinning: everything measurable comes before everything that is not, and what is
 * not is ordered by how recently we heard it, because that is the only other thing telling one
 * unplaceable name from another.
 */
MESH_TEST_CASE(waypoint_list_orders_by_range, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "the store should start");
    const char *failure = NULL;
    wp_store_populate(&store);

    struct mesh_ui_waypoint_list list;
    memset(&list, 0, sizeof list);
    /* Far, near and no-coordinates, deliberately stored in the wrong order. */
    wp_list_add(&list, 1U, "Far", true, WP_HOME_LAT, WP_HOME_LON + 3200000, WP_NOW);
    wp_list_add(&list, 2U, "Unplaced old", false, 0, 0, WP_NOW);
    wp_list_add(&list, 3U, "Near", true, WP_HOME_LAT, WP_HOME_LON + 100000, WP_NOW);
    wp_list_add(&list, 4U, "Unplaced new", false, 0, 0, WP_NOW + 600U);
    mesh_ui_store_set_waypoints(&store, &list);

    if (mesh_ui_waypoint_count(&store) != 5U) {
        failure = "four places and the new row make five";
        goto cleanup;
    }

    struct mesh_ui_waypoint_row row;
    if (!mesh_ui_waypoint_row(&store, 0U, &row) || row.id != 3U) {
        failure = "the nearest place should lead";
        goto cleanup;
    }
    if (row.range[0] == '\0') {
        failure = "a measurable place should carry a range";
        goto cleanup;
    }
    if (!mesh_ui_waypoint_row(&store, 1U, &row) || row.id != 1U) {
        failure = "the farther place should follow the nearer one";
        goto cleanup;
    }
    /* Then the unplaceable ones, newest heard first. */
    if (!mesh_ui_waypoint_row(&store, 2U, &row) || row.id != 4U || row.range[0] != '\0') {
        failure = "a place with no coordinates should follow every measurable one";
        goto cleanup;
    }
    if (!mesh_ui_waypoint_row(&store, 3U, &row) || row.id != 2U) {
        failure = "unplaceable places should be ordered by what we heard last";
        goto cleanup;
    }
    if (!mesh_ui_waypoint_row(&store, 4U, &row) || row.type != MESH_UI_WAYPOINT_ROW_NEW) {
        failure = "the last row should be the one that makes a place";
        goto cleanup;
    }
    if (row.range[0] != '\0') {
        failure = "with a fix of our own the new row has nothing to explain";
        goto cleanup;
    }
    if (mesh_ui_waypoint_row(&store, 5U, &row)) {
        failure = "there is no row past the end";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * With no fix of our own there is no range to give, and the row that makes a place says why
 * rather than disappearing. A row that vanished would leave nothing to explain itself - which
 * is the ordinary state of this client, because a Brick has no GPS.
 */
MESH_TEST_CASE(waypoint_list_without_a_fix_of_our_own, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "the store should start");
    const char *failure = NULL;

    struct mesh_ui_waypoint_list list;
    memset(&list, 0, sizeof list);
    wp_list_add(&list, 1U, "Bridge", true, WP_EAST_LAT, WP_EAST_LON, WP_NOW);
    mesh_ui_store_set_waypoints(&store, &list);

    struct mesh_ui_waypoint_row row;
    if (!mesh_ui_waypoint_row(&store, 0U, &row) || row.id != 1U) {
        failure = "the place should still list";
        goto cleanup;
    }
    if (row.range[0] != '\0') {
        failure = "a range needs two points, and we have one";
        goto cleanup;
    }
    if (!mesh_ui_waypoint_row(&store, 1U, &row) || row.type != MESH_UI_WAYPOINT_ROW_NEW) {
        failure = "the new row should still be there";
        goto cleanup;
    }
    if (row.shared[0] == '\0') {
        failure = "the new row should say why it cannot be pressed";
        goto cleanup;
    }
    if (row.range[0] != '\0') {
        failure = "a reason is not a range, and the range column is where a range goes";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* The detail's rows, and the budget they are built into. */
MESH_TEST_CASE(waypoint_detail_rows, unit) {
    struct mesh_ui_waypoint waypoint;
    memset(&waypoint, 0, sizeof waypoint);
    waypoint.id = 5U;
    waypoint.has_coords = true;
    waypoint.latitude_i = WP_EAST_LAT;
    waypoint.longitude_i = WP_EAST_LON;
    waypoint.heard = WP_NOW - 120U;
    waypoint.editable = true;
    snprintf(waypoint.name, sizeof waypoint.name, "%s", "Bridge");
    snprintf(waypoint.description, sizeof waypoint.description, "%s", "cross at the ford");

    struct mesh_ui_handshake_state hs;
    memset(&hs, 0, sizeof hs);
    hs.has_my_info = true;
    hs.my_info.node_num = 0x1000U;
    hs.node_count = 1U;
    hs.nodes[0].node_id = 0x1000U;
    hs.nodes[0].position.valid = true;
    hs.nodes[0].position.latitude_i = WP_HOME_LAT;
    hs.nodes[0].position.longitude_i = WP_HOME_LON;

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);

    struct mesh_ui_waypoint_item items[MESH_UI_WAYPOINT_ITEMS_MAX];
    const uint32_t count = mesh_ui_waypoint_detail_build(&waypoint, &hs, &settings, WP_NOW, false,
                                                         items, MESH_UI_WAYPOINT_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count == 0U, "the detail should have rows");
    MESH_TEST_FAIL_IF(count >= MESH_UI_WAYPOINT_ITEMS_MAX,
                      "the row budget should have room to spare");

    bool has_note = false;
    bool has_share = false;
    bool has_delete = false;
    const char *range = NULL;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].kind == MESH_UI_WAYPOINT_ITEM_NOTE) {
            has_note = true;
            MESH_TEST_FAIL_IF(strcmp(items[i].value, "cross at the ford") != 0,
                              "the note should carry the whole description");
        }
        if (items[i].kind == MESH_UI_WAYPOINT_ITEM_ACTION) {
            if (items[i].action == (uint8_t)MESH_UI_WAYPOINT_ACTION_SHARE) {
                has_share = true;
            }
            if (items[i].action == (uint8_t)MESH_UI_WAYPOINT_ACTION_DELETE) {
                has_delete = true;
            }
        }
        if (items[i].kind == MESH_UI_WAYPOINT_ITEM_INFO && range == NULL &&
            items[i].value[0] != '\0' && strchr(items[i].value, ' ') != NULL) {
            range = items[i].value;
        }
    }
    MESH_TEST_FAIL_IF(!has_note, "a description should get a note row");
    MESH_TEST_FAIL_IF(!has_share, "a place should be shareable again");
    MESH_TEST_FAIL_IF(!has_delete, "a place should be deletable");
    /* The first row of the screen is the range, and it is the geo module's answer rather than
       one this screen worked out: about 11 km, due east. */
    MESH_TEST_FAIL_IF(range == NULL || strstr(range, "km") == NULL,
                      "the range row should read in kilometres");

    /* A place with no description has no note row, rather than an empty one. */
    waypoint.description[0] = '\0';
    const uint32_t bare = mesh_ui_waypoint_detail_build(&waypoint, &hs, &settings, WP_NOW, false,
                                                        items, MESH_UI_WAYPOINT_ITEMS_MAX);
    MESH_TEST_FAIL_IF(bare >= count, "dropping the description should drop rows");
    for (uint32_t i = 0; i < bare; ++i) {
        MESH_TEST_FAIL_IF(items[i].kind == MESH_UI_WAYPOINT_ITEM_NOTE,
                          "a place with nothing to say should have no note row");
    }

    record_success(test_name);
}

/*
 * Both systems of units, taken from the radio's own display preference so the client and the
 * radio never disagree about how far away something is.
 */
MESH_TEST_CASE(waypoint_distance_units, unit) {
    char out[MESH_UI_WAYPOINT_RANGE_MAX];

    mesh_ui_format_distance(250.0, false, out, sizeof out);
    MESH_TEST_FAIL_IF(strstr(out, "250") == NULL || strstr(out, "m") == NULL,
                      "a short metric distance is whole metres");
    mesh_ui_format_distance(11128.0, false, out, sizeof out);
    MESH_TEST_FAIL_IF(strstr(out, "11.1") == NULL || strstr(out, "km") == NULL,
                      "a long metric distance is kilometres to one decimal");

    mesh_ui_format_distance(250.0, true, out, sizeof out);
    MESH_TEST_FAIL_IF(strstr(out, "820") == NULL || strstr(out, "ft") == NULL,
                      "a short imperial distance is whole feet");
    mesh_ui_format_distance(11128.0, true, out, sizeof out);
    MESH_TEST_FAIL_IF(strstr(out, "6.9") == NULL || strstr(out, "mi") == NULL,
                      "a long imperial distance is miles to one decimal");

    /* A range is a distance and a direction; standing on the place is the case with only the
       first of those, and it must not claim north. */
    MESH_TEST_FAIL_IF(!mesh_ui_waypoint_format_range(WP_HOME_LAT, WP_HOME_LON, WP_HOME_LAT,
                                                     WP_HOME_LON, false, out, sizeof out),
                      "a place we are standing on still has a range");
    MESH_TEST_FAIL_IF(strchr(out, 'N') != NULL, "a place we are standing on has no direction");

    MESH_TEST_FAIL_IF(mesh_ui_waypoint_format_range(2000000000, 0, WP_HOME_LAT, WP_HOME_LON, false,
                                                    out, sizeof out),
                      "an unmeasurable pair has no range");
    MESH_TEST_FAIL_IF(out[0] != '\0', "a refused range should be empty");

    record_success(test_name);
}

/*
 * The tab's two levels under the buttons, end to end: open a place, arm its delete, and watch
 * the detail close by itself when the place leaves the list.
 *
 * The last part is the rule the node detail already follows and the reason the open place is
 * named by id: the list is ordered by distance from our own fix, so it re-ranks under the
 * cursor, and a withdrawal from the mesh is a row disappearing while somebody is reading it.
 */
MESH_TEST_CASE(waypoint_nav_opens_arms_and_closes, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "the store should start");
    const char *failure = NULL;
    wp_store_populate(&store);

    struct mesh_ui_waypoint_list list;
    memset(&list, 0, sizeof list);
    wp_list_add(&list, 42U, "Bridge", true, WP_EAST_LAT, WP_EAST_LON, WP_NOW);
    mesh_ui_store_set_waypoints(&store, &list);

    struct mesh_ui_action action;
    if (!mesh_test_open_tab(&store, MESH_UI_SCREEN_WAYPOINTS)) {
        failure = "the Waypoints tab should be reachable with the shoulder buttons";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.waypoint_detail_open || store.nav.waypoint_detail_id != 42U) {
        failure = "A on a place should open it";
        goto cleanup;
    }

    /* Walk to the delete row - the last one the builder emits - and arm it. */
    const uint32_t rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_WAYPOINTS);
    if (rows == 0U) {
        failure = "an open place should have rows";
        goto cleanup;
    }
    for (uint32_t i = 0; i + 1U < rows; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.waypoint_delete_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "the first press on delete should only arm it";
        goto cleanup;
    }
    /* Moving off the row stands it down: the row the question was asked about is the only row
       the answer may apply to. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    if (store.nav.waypoint_delete_armed) {
        failure = "moving the cursor should stand the delete down";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_FORGET_WAYPOINT || action.number != 42U) {
        failure = "the second press should ask the app to forget that place";
        goto cleanup;
    }

    /* The app has not acted yet, so the detail is still open - and it closes on the publish
       that takes the place away, not on the press. */
    if (!store.nav.waypoint_detail_open) {
        failure = "the detail should stay open until the place actually goes";
        goto cleanup;
    }
    memset(&list, 0, sizeof list);
    mesh_ui_store_set_waypoints(&store, &list);
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (store.nav.waypoint_detail_open) {
        failure = "a place leaving the list should close its detail";
        goto cleanup;
    }
    if (snapshot.nav.screen != MESH_UI_SCREEN_WAYPOINTS) {
        failure = "closing a detail should leave the tab where it was";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Making one: the last row raises the keyboard, and Send asks the app to share what was typed.
 *
 * The coordinate is deliberately absent from the action - the nav has no business carrying one,
 * and the app reads it from the session roster when it acts, so a node that moves while its
 * name is being typed is saved where it ends up.
 */
MESH_TEST_CASE(waypoint_nav_names_a_new_place, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "the store should start");
    const char *failure = NULL;
    wp_store_populate(&store);

    struct mesh_ui_action action;
    if (!mesh_test_open_tab(&store, MESH_UI_SCREEN_WAYPOINTS)) {
        failure = "the Waypoints tab should be reachable";
        goto cleanup;
    }
    /* The only row is the one that makes a place. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || !store.nav.keyboard_waypoint) {
        failure = "the new row should raise the keyboard";
        goto cleanup;
    }
    if (store.nav.waypoint_source_node != 0U) {
        failure = "the new row takes our own radio's fix";
        goto cleanup;
    }

    /* Send with nothing typed refuses rather than broadcasting an unnamed place. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_NONE || !store.nav.keyboard_open) {
        failure = "an empty name should not go out, and should leave the keyboard up";
        goto cleanup;
    }

    /* Type one character off the grid, then send. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.draft[0] == '\0') {
        failure = "A on the keyboard should type";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_SHARE_WAYPOINT || action.text[0] == '\0') {
        failure = "Send should ask the app to share the named place";
        goto cleanup;
    }
    if (action.number != 0U) {
        failure = "a new place has no id yet; the session draws it";
        goto cleanup;
    }
    if (store.nav.keyboard_open || store.nav.keyboard_waypoint) {
        failure = "the keyboard should close behind a shared place";
        goto cleanup;
    }
    if (store.nav.screen != MESH_UI_SCREEN_WAYPOINTS) {
        failure = "naming a place should land back on the list it will appear in";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The same press with nowhere to put the place.
 *
 * A refusal the user cannot see is the client looking broken: the row says why on its
 * supporting line, and the press says it again, because whoever pressed A did not read the row.
 * The keyboard staying shut is the other half - naming a place that has no coordinates behind
 * it would throw the typing away at the end.
 */
MESH_TEST_CASE(waypoint_nav_refuses_a_new_place_with_no_fix, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "the store should start");
    const char *failure = NULL;

    /* Our own radio, known and named, with no fix - a Brick's ordinary state. */
    struct mesh_ui_handshake_state hs;
    memset(&hs, 0, sizeof hs);
    hs.has_my_info = true;
    hs.my_info.node_num = 0x1000U;
    hs.node_count = 1U;
    hs.nodes[0].node_id = 0x1000U;
    snprintf(hs.nodes[0].short_name, sizeof hs.nodes[0].short_name, "%s", "HOME");
    mesh_ui_store_set_handshake(&store, &hs);

    struct mesh_ui_action action;
    if (!mesh_test_open_tab(&store, MESH_UI_SCREEN_WAYPOINTS)) {
        failure = "the Waypoints tab should be reachable";
        goto cleanup;
    }
    /* One turn of the loop before the press, which is what puts a clock on the store. */
    mesh_ui_store_tick(&store, 10000U);
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.keyboard_open) {
        failure = "a place with no coordinates should not open the keyboard";
        goto cleanup;
    }
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "nothing should go to the app";
        goto cleanup;
    }
    if (store.nav.toast[0] == '\0') {
        failure = "the refused press should say why";
        goto cleanup;
    }
    /*
     * And the notice is dated by the clock driving the store, not by the host's uptime. The app
     * drives it with CLOCK_MONOTONIC and the capture harness with a synthetic clock that starts
     * at 1000, so a deadline read from the real clock inside the press would stand for four
     * seconds on the device and for longer than any scene in a capture.
     */
    if (store.nav.toast_until_ms != 10000U + 4000U) {
        failure = "the store should date the notice from the clock it was last ticked with";
        goto cleanup;
    }
    mesh_ui_store_tick(&store, 14000U);
    if (store.nav.toast[0] != '\0') {
        failure = "and four seconds of that clock later it should go";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
