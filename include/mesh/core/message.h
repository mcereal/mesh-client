#pragma once

#include "meshtastic/mesh.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MeshPacket.to for a channel broadcast rather than a direct message. */
#define MESH_MESSAGE_BROADCAST_ADDR 0xFFFFFFFFU

/* Upstream Data.payload caps at 233 bytes; text is stored NUL-terminated on top of that. */
#define MESH_MESSAGE_TEXT_MAX 233U

/* The newest N messages are kept. The Brick has 1 GB and no swap, so the inbox is a fixed
   ring rather than a growing list; evictions are counted so the UI can say what it lost. */
#define MESH_MESSAGE_LOG_CAPACITY 64U

enum mesh_message_direction {
    MESH_MESSAGE_INBOUND = 0,
    MESH_MESSAGE_OUTBOUND,
};

/*
 * Which port a message arrived on. Three of them are "same as Text Message" upstream - the
 * payload is plain text and the packet is addressed to a channel like any other - so they
 * belong in the conversation rather than in a screen of their own. What they are not is
 * interchangeable: an alert is the firmware's word for something that needs acting on, and a
 * detection is a sensor announcing itself, and a transcript that drew all three identically
 * would be hiding the only part that matters.
 *
 * Anything we send is TEXT: the client has no reason to originate an alert or a detection.
 */
enum mesh_message_kind {
    MESH_MESSAGE_KIND_TEXT = 0,
    MESH_MESSAGE_KIND_ALERT,     /* ALERT_APP: the firmware's critical alert */
    MESH_MESSAGE_KIND_DETECTION, /* DETECTION_SENSOR_APP: "<name> detected" */
};

/* Delivery state of an outbound message. Inbound messages are always MESH_MESSAGE_ACK_NONE. */
enum mesh_message_ack {
    MESH_MESSAGE_ACK_NONE = 0,  /* nothing to wait for: broadcast, or want_ack unset */
    MESH_MESSAGE_ACK_PENDING,   /* sent with want_ack, no Routing reply seen yet */
    MESH_MESSAGE_ACK_DELIVERED, /* Routing reply with error_reason == NONE */
    MESH_MESSAGE_ACK_FAILED,    /* Routing reply carrying an error */
};

struct mesh_message {
    uint32_t packet_id;
    uint32_t from;
    uint32_t to;
    uint32_t rx_time; /* seconds since epoch as reported by the radio; 0 when unknown */
    float rx_snr;
    uint8_t channel;
    uint8_t direction;  /* enum mesh_message_direction */
    uint8_t kind;       /* enum mesh_message_kind */
    uint8_t ack;        /* enum mesh_message_ack */
    uint8_t ack_error;  /* meshtastic_Routing_Error, meaningful when ack == FAILED */
    bool has_hops_away; /* hop_start/hop_limit were both usable */
    uint8_t hops_away;
    /* The radio decrypted this with the sender's public key rather than with a channel PSK, so
       it was addressed to us and to nobody else. Worth showing: on a default-key channel every
       node on the mesh can read a "direct" message, and the two look identical without this. */
    bool pki_encrypted;
    /*
     * A reaction (Data.emoji set) carries an emoji in its payload and names the message it is
     * about in reply_id. It is kept in the log because it is traffic that happened, but it is
     * not a message: the UI attaches it to its target rather than giving it a bubble.
     *
     * reply_id is also set on an ordinary message that is a threaded reply, which is why the
     * two are separate flags rather than one.
     */
    uint32_t reply_id;
    bool is_reaction;
    /* Sanitised text: control bytes are folded to spaces or '?' by mesh_message_ingest, so
       backends can draw this straight into a framebuffer without re-checking it. */
    char text[MESH_MESSAGE_TEXT_MAX + 1U];
};

struct mesh_message_log {
    struct mesh_message entries[MESH_MESSAGE_LOG_CAPACITY];
    size_t head; /* index of the oldest entry */
    size_t count;
    uint32_t dropped; /* entries evicted by the ring since the last reset */
};

struct mesh_message_text_request {
    uint32_t dest; /* MESH_MESSAGE_BROADCAST_ADDR for a channel broadcast */
    uint32_t packet_id;
    const char *text;
    uint8_t channel;
    uint8_t hop_limit; /* 0 leaves the firmware default in place */
    bool want_ack;
};

void mesh_message_log_reset(struct mesh_message_log *log);

/* Appends a copy, evicting the oldest entry when full. Returns the stored slot, or NULL on
   bad input. The returned pointer is invalidated by the next append. */
struct mesh_message *mesh_message_log_append(struct mesh_message_log *log,
                                             const struct mesh_message *message);

/* Index 0 is the oldest retained message. Returns NULL when index is out of range. */
const struct mesh_message *mesh_message_log_at(const struct mesh_message_log *log, size_t index);

/* Finds the newest entry with this packet id, or NULL. */
struct mesh_message *mesh_message_log_find(struct mesh_message_log *log, uint32_t packet_id);

/*
 * Whether `message` belongs to one conversation, named the way the UI names a destination:
 * `peer` of MESH_MESSAGE_BROADCAST_ADDR means the channel conversation on `channel`, and
 * anything else means the direct exchange with that node, in either direction.
 */
bool mesh_message_in_conversation(const struct mesh_message *message, uint32_t peer,
                                  uint8_t channel);

/*
 * Drops every message in that conversation, compacting the ring. Returns how many went.
 *
 * `dropped` is deliberately untouched: it counts what the ring took away from the user, and a
 * delete is the user taking it away themselves - counting it there would have the UI report
 * "+3 older" for history somebody asked to be rid of.
 */
uint32_t mesh_message_log_forget(struct mesh_message_log *log, uint32_t peer, uint8_t channel);

/* Applies a delivery result to the outbound entry with this packet id. Returns true when a
   matching entry was updated. */
bool mesh_message_log_mark_ack(struct mesh_message_log *log, uint32_t packet_id,
                               enum mesh_message_ack ack, uint8_t error);

/* Encodes a text message as a ToRadio protobuf ready for one BLE GATT write (BLE applies no
   stream framing). `from` is deliberately left unset: the firmware fills in its own node
   number. Returns 0 on success and sets *written. */
int mesh_message_encode_text(const struct mesh_message_text_request *request, uint8_t *out,
                             size_t out_len, size_t *written);

/* Folds an inbound MeshPacket into the log. Text messages are appended; Routing replies
   update the ack state of the outbound message they refer to. Everything else is ignored.

   Packets we sent are echoed back to us by the radio, so an outbound entry already carrying
   this packet id is refreshed in place rather than duplicated.

   Returns 1 when a message was appended, 0 when the packet was understood but added nothing,
   and a negative errno on bad input. Never fails on malformed radio content - the payload is
   untrusted and is sanitised, not trusted. */
int mesh_message_ingest(struct mesh_message_log *log, const meshtastic_MeshPacket *packet,
                        uint32_t my_node_num);

/* "delivered", "pending", ... for status output. */
const char *mesh_message_ack_to_string(enum mesh_message_ack ack);
/* Plain English for the Routing_Error in `ack_error`, so a failed message says why. */
const char *mesh_message_ack_error_to_string(uint8_t error);

#ifdef __cplusplus
}
#endif
