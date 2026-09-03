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
#define MESH_MESSAGE_LOG_CAPACITY 32U

enum mesh_message_direction {
    MESH_MESSAGE_INBOUND = 0,
    MESH_MESSAGE_OUTBOUND,
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
    uint8_t ack;        /* enum mesh_message_ack */
    uint8_t ack_error;  /* meshtastic_Routing_Error, meaningful when ack == FAILED */
    bool has_hops_away; /* hop_start/hop_limit were both usable */
    uint8_t hops_away;
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

#ifdef __cplusplus
}
#endif
