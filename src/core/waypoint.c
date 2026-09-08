#include "mesh/core/waypoint.h"

#include "mesh/core/message.h"
#include "mesh/geo/coords.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include "meshtastic/portnums.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <string.h>

enum mesh_waypoint_state mesh_waypoint_state(const struct mesh_waypoint *waypoint, uint32_t now) {
    if (waypoint == NULL || waypoint->expire == 0U) {
        return MESH_WAYPOINT_LIVE;
    }
    if (waypoint->expire < MESH_WAYPOINT_TOMBSTONE_BEFORE) {
        return MESH_WAYPOINT_DELETED;
    }
    /* No clock, no opinion: see the header. */
    if (now == 0U || now < waypoint->expire) {
        return MESH_WAYPOINT_LIVE;
    }
    return MESH_WAYPOINT_EXPIRED;
}

void mesh_waypoint_book_reset(struct mesh_waypoint_book *book) {
    if (book == NULL) {
        return;
    }
    memset(book, 0, sizeof *book);
}

const struct mesh_waypoint *mesh_waypoint_book_at(const struct mesh_waypoint_book *book,
                                                  size_t index) {
    if (book == NULL || index >= book->count) {
        return NULL;
    }
    return &book->entries[index];
}

struct mesh_waypoint *mesh_waypoint_book_find(struct mesh_waypoint_book *book, uint32_t id) {
    if (book == NULL || id == 0U) {
        return NULL;
    }
    for (size_t i = 0; i < book->count; ++i) {
        if (book->entries[i].id == id) {
            return &book->entries[i];
        }
    }
    return NULL;
}

const struct mesh_waypoint *mesh_waypoint_book_get(const struct mesh_waypoint_book *book,
                                                   uint32_t id) {
    if (book == NULL || id == 0U) {
        return NULL;
    }
    for (size_t i = 0; i < book->count; ++i) {
        if (book->entries[i].id == id) {
            return &book->entries[i];
        }
    }
    return NULL;
}

bool mesh_waypoint_book_forget(struct mesh_waypoint_book *book, uint32_t id) {
    struct mesh_waypoint *entry = mesh_waypoint_book_find(book, id);
    if (entry == NULL) {
        return false;
    }
    const size_t index = (size_t)(entry - book->entries);
    /* A table, not a ring: close the gap so the order stays arrival order. */
    for (size_t i = index + 1U; i < book->count; ++i) {
        book->entries[i - 1U] = book->entries[i];
    }
    book->count--;
    memset(&book->entries[book->count], 0, sizeof book->entries[book->count]);
    return true;
}

/*
 * Which entry goes when the table is full.
 *
 * The least recently heard, with one exception: a place this client shared is not evicted while
 * somebody else's is available to go instead. The mesh can re-broadcast theirs and cannot
 * re-broadcast ours, so ours is the copy whose loss is permanent.
 */
static size_t mesh_waypoint_evictable(const struct mesh_waypoint_book *book) {
    size_t victim = 0U;
    bool victim_found = false;
    size_t fallback = 0U;
    for (size_t i = 0; i < book->count; ++i) {
        if (!book->entries[i].ours &&
            (!victim_found || book->entries[i].heard < book->entries[victim].heard)) {
            victim = i;
            victim_found = true;
        }
        if (book->entries[i].heard < book->entries[fallback].heard) {
            fallback = i;
        }
    }
    return victim_found ? victim : fallback;
}

struct mesh_waypoint *mesh_waypoint_book_store(struct mesh_waypoint_book *book,
                                               const struct mesh_waypoint *waypoint) {
    if (book == NULL || waypoint == NULL || waypoint->id == 0U) {
        return NULL;
    }

    struct mesh_waypoint *slot = mesh_waypoint_book_find(book, waypoint->id);
    if (slot == NULL) {
        if (book->count < MESH_WAYPOINT_BOOK_CAPACITY) {
            slot = &book->entries[book->count++];
        } else {
            slot = &book->entries[mesh_waypoint_evictable(book)];
            if (book->dropped < UINT32_MAX) {
                book->dropped++;
            }
        }
    }

    *slot = *waypoint;
    /* The two fields a backend draws straight into a framebuffer; terminate them whatever the
       caller handed us. */
    slot->name[MESH_WAYPOINT_NAME_MAX] = '\0';
    slot->description[MESH_WAYPOINT_DESCRIPTION_MAX] = '\0';
    return slot;
}

int mesh_waypoint_ingest(struct mesh_waypoint_book *book, const meshtastic_MeshPacket *packet,
                         uint32_t my_node_num, uint32_t heard) {
    if (book == NULL || packet == NULL) {
        return -EINVAL;
    }
    if (packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet->decoded.portnum != meshtastic_PortNum_WAYPOINT_APP) {
        return 0;
    }

    const meshtastic_Data *data = &packet->decoded;
    meshtastic_Waypoint decoded = meshtastic_Waypoint_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);
    if (!pb_decode(&stream, meshtastic_Waypoint_fields, &decoded)) {
        mesh_log_debug("waypoint", "Bad WAYPOINT_APP from 0x%08x: %s", packet->from,
                       PB_GET_ERROR(&stream));
        return 0;
    }
    if (decoded.id == 0U) {
        /* Without an id there is nothing to key on: two copies would be two places, and an edit
           would be a third. */
        mesh_log_debug("waypoint", "Ignoring a waypoint with no id from 0x%08x", packet->from);
        return 0;
    }

    struct mesh_waypoint waypoint;
    memset(&waypoint, 0, sizeof waypoint);
    waypoint.id = decoded.id;
    waypoint.expire = decoded.expire;
    waypoint.locked_to = decoded.locked_to;
    waypoint.icon = decoded.icon;
    waypoint.from = packet->from;
    waypoint.heard = heard;
    waypoint.channel = packet->channel;
    waypoint.ours = (my_node_num != 0U && packet->from == my_node_num);
    /* Coordinates are optional on the wire and are not range-checked by anything upstream, so
       both questions are asked here: did they say, and is what they said on Earth. A waypoint
       without a place is still a name somebody shared - it lists, it just cannot be walked to. */
    if (decoded.has_latitude_i && decoded.has_longitude_i &&
        mesh_geo_coords_valid(decoded.latitude_i, decoded.longitude_i)) {
        waypoint.has_coords = true;
        waypoint.latitude_i = decoded.latitude_i;
        waypoint.longitude_i = decoded.longitude_i;
    }
    /* nanopb NUL-terminates a decoded string field, but the bytes inside it came off the air
       and can be anything; sanitising is what lets a backend draw them without re-checking. */
    mesh_text_sanitise_str(decoded.name, waypoint.name, sizeof waypoint.name);
    mesh_text_sanitise_str(decoded.description, waypoint.description, sizeof waypoint.description);

    if (mesh_waypoint_state(&waypoint, 0U) == MESH_WAYPOINT_DELETED) {
        /* A withdrawal. Honoured whoever sent it: the alternative is a client that keeps
           showing a place its own mesh has agreed is gone. */
        const bool had = mesh_waypoint_book_forget(book, waypoint.id);
        mesh_log_info("waypoint", "Waypoint %u withdrawn by 0x%08x%s", waypoint.id, packet->from,
                      had ? "" : " (not one we held)");
        return had ? 1 : 0;
    }

    const bool known = mesh_waypoint_book_find(book, waypoint.id) != NULL;
    if (mesh_waypoint_book_store(book, &waypoint) == NULL) {
        return -ENOMEM;
    }
    mesh_log_info("waypoint", "%s waypoint %u \"%s\" from 0x%08x on channel %u",
                  known ? "Updated" : "Received", waypoint.id, waypoint.name, packet->from,
                  (unsigned)waypoint.channel);
    return 1;
}

int mesh_waypoint_encode(const struct mesh_waypoint_request *request, uint8_t *out, size_t out_len,
                         size_t *written) {
    if (request == NULL || request->waypoint == NULL || out == NULL || written == NULL) {
        return -EINVAL;
    }
    const struct mesh_waypoint *waypoint = request->waypoint;
    if (waypoint->id == 0U) {
        return -EINVAL;
    }

    meshtastic_Waypoint payload = meshtastic_Waypoint_init_default;
    payload.id = waypoint->id;
    payload.expire = waypoint->expire;
    payload.locked_to = waypoint->locked_to;
    payload.icon = waypoint->icon;
    if (waypoint->has_coords) {
        payload.has_latitude_i = true;
        payload.latitude_i = waypoint->latitude_i;
        payload.has_longitude_i = true;
        payload.longitude_i = waypoint->longitude_i;
    }
    mesh_str_copy(payload.name, sizeof payload.name, waypoint->name);
    mesh_str_copy(payload.description, sizeof payload.description, waypoint->description);

    uint8_t body[MESH_MESSAGE_TEXT_MAX];
    pb_ostream_t body_stream = pb_ostream_from_buffer(body, sizeof body);
    if (!pb_encode(&body_stream, meshtastic_Waypoint_fields, &payload)) {
        mesh_log_error("waypoint", "Failed to encode waypoint %u: %s", waypoint->id,
                       PB_GET_ERROR(&body_stream));
        return -EIO;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;

    meshtastic_MeshPacket *packet = &to_radio.packet;
    /* A waypoint is shared with a channel rather than sent to a node, and a broadcast is never
       acked directly by the mesh - so want_ack stays clear, as it does for a broadcast text. */
    packet->to = MESH_MESSAGE_BROADCAST_ADDR;
    packet->channel = request->channel;
    packet->id = request->packet_id;
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_WAYPOINT_APP;
    memcpy(packet->decoded.payload.bytes, body, body_stream.bytes_written);
    packet->decoded.payload.size = (pb_size_t)body_stream.bytes_written;

    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("waypoint", "Failed to encode waypoint packet: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    *written = stream.bytes_written;
    return 0;
}
