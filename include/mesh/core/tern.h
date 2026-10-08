#pragma once

#include "mesh/core/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_session;

/*
 * Tern's companion protocol: what a client says to a node running Tern's firmware, over USB
 * serial, TCP or Bluetooth LE. Draft 0 of `draft/companion.md` in ternmesh/spec, checked
 * against that repository's `vectors/companion.json` (tests/data/tern_companion.json here).
 *
 * Every frame is a type byte, a sequence byte and big-endian fields in a fixed order. The
 * client sends requests (0x01-0x3F) one at a time and the node answers each (0x40-0x7F) with
 * the request's sequence byte; news (0x80-0xBF) arrives whenever the node has some, numbered
 * by a count the node starts at 0 when it answers HELLO. Each news frame but STATE and the two
 * _GONEs is a whole record, so a client that applies them in order is right after each.
 *
 * Written from the specification alone. Like meshcore.h the file is two halves: the codec is
 * pure - bytes to a struct and back - and the conversation below it is the struct
 * mesh_protocol a link carries, which keeps what it learns in a struct mesh_session used as a
 * model (mesh_session_model_sync_begin()).
 */

#define MESH_TERN_VERSION 0U
#define MESH_TERN_MAX_FRAME 180U
#define MESH_TERN_ADDRESS_LEN 32U
#define MESH_TERN_NAME_MAX 31U
#define MESH_TERN_TEXT_MAX 128U
#define MESH_TERN_FIRMWARE_MAX 31U
#define MESH_TERN_REGION_MAX 15U

/* The specification's timing parameters. */
#define MESH_TERN_ANSWER_WAIT_MS 5000U
#define MESH_TERN_IDLE_MS 20000U

enum mesh_tern_type {
    /* Requests, client to node. */
    MESH_TERN_HELLO = 0x01,
    MESH_TERN_SYNC = 0x02,
    MESH_TERN_PING = 0x03,
    MESH_TERN_SET_TIME = 0x04,
    MESH_TERN_SET = 0x05,
    MESH_TERN_SEND = 0x10,
    MESH_TERN_READ = 0x11,
    MESH_TERN_SAVE_CONTACT = 0x18,
    MESH_TERN_REMOVE_CONTACT = 0x19,
    /* Answers, node to client. */
    MESH_TERN_OK = 0x40,
    MESH_TERN_ERROR = 0x41,
    MESH_TERN_INFO = 0x42,
    MESH_TERN_SYNCED = 0x43,
    MESH_TERN_QUEUED = 0x44,
    /* News, node to client. */
    MESH_TERN_SELF = 0x80,
    MESH_TERN_CONTACT = 0x81,
    MESH_TERN_CONTACT_GONE = 0x82,
    MESH_TERN_MESSAGE = 0x83,
    MESH_TERN_STATE = 0x84,
    MESH_TERN_NEIGHBOUR = 0x85,
    MESH_TERN_NEIGHBOUR_GONE = 0x86,
    MESH_TERN_AIRTIME = 0x87,
    MESH_TERN_POWER = 0x88,
};

#define MESH_TERN_IS_REQUEST(type) ((type) >= 0x01U && (type) <= 0x3FU)
#define MESH_TERN_IS_ANSWER(type) ((type) >= 0x40U && (type) <= 0x7FU)
#define MESH_TERN_IS_NEWS(type) ((type) >= 0x80U && (type) <= 0xBFU)

enum mesh_tern_setting {
    MESH_TERN_SETTING_REGION = 1,  /* str up to 15: a profile name */
    MESH_TERN_SETTING_ROLE = 2,    /* u8: 0 leaf, 1 relay */
    MESH_TERN_SETTING_POWER = 3,   /* i8: dBm */
    MESH_TERN_SETTING_PASSKEY = 4, /* u32: 0..999999, or 0xFFFFFFFF for a random one */
};

enum mesh_tern_error {
    MESH_TERN_ERR_UNKNOWN = 1,
    MESH_TERN_ERR_MALFORMED = 2,
    MESH_TERN_ERR_REFUSED = 3,
    MESH_TERN_ERR_ADDRESS = 4,
    MESH_TERN_ERR_FULL = 5,
    MESH_TERN_ERR_HELLO_FIRST = 6,
    MESH_TERN_ERR_MTU = 7,
    MESH_TERN_ERR_NOT_NOW = 8,
};

/* Where a message is. A sent one only goes forward, but for sent back to waiting on a retry. */
enum mesh_tern_state {
    MESH_TERN_STATE_WAITING = 0,
    MESH_TERN_STATE_SENT = 1,
    MESH_TERN_STATE_DELIVERED = 2,
    MESH_TERN_STATE_NOT_DELIVERED = 3,
    MESH_TERN_STATE_RECEIVED = 4,
};

/* What a waiting message waits for; 0 in every other state. */
enum mesh_tern_reason {
    MESH_TERN_WAIT_UNNAMED = 0,
    MESH_TERN_WAIT_ROUTE = 1,
    MESH_TERN_WAIT_SESSION = 2,
    MESH_TERN_WAIT_REGION = 3,
    MESH_TERN_WAIT_BUDGET = 4,
    MESH_TERN_WAIT_RADIO = 5,
};

#define MESH_TERN_ROLE_LEAF 0U
#define MESH_TERN_ROLE_RELAY 1U
#define MESH_TERN_MESSAGE_READ 0x01U   /* MESSAGE flags */
#define MESH_TERN_POWER_CHARGING 0x01U /* POWER flags */
#define MESH_TERN_POWER_EXTERNAL 0x02U
#define MESH_TERN_PERCENT_UNKNOWN 255U
#define MESH_TERN_PASSKEY_RANDOM 0xFFFFFFFFU

/*
 * Any frame, as its fields. Each type fills the members its table in the draft names, under the
 * same names, with three folded together: the one string a frame carries (text, name, firmware,
 * region, or SET's region) is `text`; the one address (to, address, contact) is `address`; and
 * `wait` is MESSAGE's and STATE's u16 or AIRTIME's u32. SET's value is `text`, `role`, `power`
 * or `passkey` as `setting` says. `text` is NUL-terminated after `text_len` bytes, for the
 * reader's convenience; the frame carries no NUL.
 */
struct mesh_tern_frame {
    uint8_t type;
    uint8_t seq;
    uint8_t version;
    uint8_t setting;
    uint8_t code;
    uint8_t role;
    uint8_t session;
    uint8_t flags;
    uint8_t state;
    uint8_t reason;
    uint8_t percent;
    int8_t power;
    int8_t snr; /* quarter dB */
    uint16_t heard;
    uint16_t millivolts;
    uint32_t after;
    uint32_t time;
    uint32_t ref;
    uint32_t through;
    uint32_t id;
    uint32_t routing_id;
    uint32_t period;
    uint32_t allowed;
    uint32_t used;
    uint32_t wait;
    uint32_t passkey;
    uint8_t address[MESH_TERN_ADDRESS_LEN];
    uint8_t text_len;
    char text[MESH_TERN_TEXT_MAX + 1U];
};

enum mesh_tern_decode_result {
    MESH_TERN_DECODE_OK = 0,
    /* A type, or a setting, this version does not define: ERROR 1 for a request. */
    MESH_TERN_DECODE_UNKNOWN = MESH_TERN_ERR_UNKNOWN,
    /* Cut short, a string too long for its field or not UTF-8: ERROR 2 for a request. */
    MESH_TERN_DECODE_MALFORMED = MESH_TERN_ERR_MALFORMED,
    /* Under two bytes: not a frame at all, and not answered. */
    MESH_TERN_DECODE_SHORT = 3,
};

/* Reads a frame's fields. Bytes after the last field this version defines are ignored - that is
   how a later version adds one. */
enum mesh_tern_decode_result mesh_tern_decode(const uint8_t *frame, size_t len,
                                              struct mesh_tern_frame *out);

/* Writes a frame from its fields. Returns its length, or a negative errno: -EINVAL for a type
   or setting this version does not define or a string longer than its field, -ENOSPC when
   `out` is too small. The text is not checked for UTF-8; a writer is trusted to give text. */
int mesh_tern_encode(const struct mesh_tern_frame *frame, uint8_t *out, size_t out_len);

/* Whether `len` bytes are UTF-8 with no overlong form, surrogate or code point past U+10FFFF -
   the test a `str` field has to pass. */
bool mesh_tern_utf8_valid(const uint8_t *text, size_t len);

/*
 * The routing id the radio protocol knows the node with address `address` by: the first of the
 * eight big-endian words of SHA-256("tern routing id" || address) that is neither 0 nor
 * 0xFFFFFFFF (draft/routing.md). It is this client's node number for a Tern node, which is
 * what lets a NEIGHBOUR - which carries only a routing id - and a CONTACT or MESSAGE - which
 * carry an address - land on the same roster entry.
 */
uint32_t mesh_tern_routing_id(const uint8_t address[MESH_TERN_ADDRESS_LEN]);

/* ------------------------------------------------------------------------- the conversation */

/* What the conversation keeps of the node's lists, for sending by node number and for knowing
   what a sync left out. Each sized past what the firmware's first port holds. */
#define MESH_TERN_MAX_CONTACTS 64U
#define MESH_TERN_MAX_NEIGHBOURS 64U
/* Requests waiting their turn behind the one on the wire. */
#define MESH_TERN_QUEUE_LEN 8U
/* SENDs answered that nobody has taken yet (mesh_tern_take_queued()). */
#define MESH_TERN_QUEUED_IDS 8U

struct mesh_tern_contact {
    uint8_t address[MESH_TERN_ADDRESS_LEN];
    uint32_t node_id;
    uint8_t session;
    bool synced; /* sent by the sync now running */
    char name[MESH_TERN_NAME_MAX + 1U];
};

struct mesh_tern_neighbour {
    uint32_t routing_id;
    uint8_t role;
    bool synced;
};

/* SELF, as the node last sent it. */
struct mesh_tern_self {
    uint8_t address[MESH_TERN_ADDRESS_LEN];
    uint8_t role;
    int8_t power;
    uint32_t time;
    char region[MESH_TERN_REGION_MAX + 1U];
};

/* AIRTIME, as the node last sent it: the region's limit, as the node is spending it. */
struct mesh_tern_airtime {
    uint32_t period_s;   /* 0 for a profile with no limit */
    uint32_t allowed_ms; /* in a period; 0 with no limit */
    uint32_t used_ms;    /* in the period ending now */
    uint32_t wait_ms;    /* until the longest frame could go */
};

enum mesh_tern_phase {
    MESH_TERN_IDLE = 0, /* no link */
    MESH_TERN_HELLO_SENT,
    MESH_TERN_SYNCING,
    MESH_TERN_READY,
};

struct mesh_tern_request {
    uint8_t frame[MESH_TERN_MAX_FRAME];
    uint8_t len;
    uint32_t ticket; /* a SEND's, for mesh_tern_take_queued(); 0 otherwise */
};

/* How a SEND ended: the node's id for the message, or 0 for one it refused. */
struct mesh_tern_queued {
    uint32_t ticket;
    uint32_t id;
};

struct mesh_tern {
    struct mesh_session *model;
    mesh_protocol_send_fn send;
    void *send_ctx;
    enum mesh_tern_phase phase;

    /* Requests, the head of which is on the wire once `awaiting` is set. */
    struct mesh_tern_request queue[MESH_TERN_QUEUE_LEN];
    size_t queue_head;
    size_t queue_count;
    bool awaiting;
    uint64_t awaiting_since_ms;
    uint8_t next_seq;
    /* When the last answer came, which the ping is counted from. */
    uint64_t answered_ms;
    uint64_t now_ms;
    /* A request went unanswered: the node is gone and the link should be dropped. */
    bool gone;

    /* The node's news count: the `seq` the next news should carry. */
    uint8_t news_expected;
    /* News arrived out of count, so some was missed; another SYNC is asked once this one is
       through. */
    bool missed_news;

    uint8_t node_version;
    char firmware[MESH_TERN_FIRMWARE_MAX + 1U];
    bool has_self;
    struct mesh_tern_self self;
    uint32_t self_node;
    bool has_airtime;
    struct mesh_tern_airtime airtime;

    struct mesh_tern_contact contacts[MESH_TERN_MAX_CONTACTS];
    size_t contact_count;
    struct mesh_tern_neighbour neighbours[MESH_TERN_MAX_NEIGHBOURS];
    size_t neighbour_count;

    /* The greatest message id this conversation holds, which the next SYNC asks after. Kept
       across a reconnect to the same node, cleared when another answers. */
    uint32_t newest_id;

    uint32_t next_ticket;
    struct mesh_tern_queued queued[MESH_TERN_QUEUED_IDS];
    size_t queued_count;
};

void mesh_tern_init(struct mesh_tern *tern, struct mesh_session *model);
struct mesh_protocol mesh_tern_protocol(struct mesh_tern *tern);

/* Through the first sync, with the link still up. */
bool mesh_tern_ready(const struct mesh_tern *tern);

/*
 * Sends `text` to the node `dest` - a roster number whose address the conversation knows, from
 * a contact or from a message. Returns 0 once the SEND is queued, -ENOTCONN with no link,
 * -ENOENT for a node with no known address, -EMSGSIZE for text past MESH_TERN_TEXT_MAX,
 * -EINVAL for empty text and -ENOBUFS with the queue full. `out_ticket` (may be NULL) names
 * this send in mesh_tern_take_queued().
 *
 * Nothing is logged here. The node gives the message its id, and the message arrives as MESSAGE
 * news under that id - the one the log, the cache and a sync after a restart all agree on - so
 * the bubble is the node's record from the start rather than a guess the client has to
 * reconcile later.
 */
int mesh_tern_send_text(struct mesh_tern *tern, uint32_t dest, const char *text,
                        uint32_t *out_ticket);

/* Marks every received message up to and including `through` - a packet id, which is the
   node's message id - as read, on the node and so on every client driving it. 0 or a negative
   errno as mesh_tern_send_text(). */
int mesh_tern_mark_read(struct mesh_tern *tern, uint32_t through);

/*
 * How the oldest send not yet taken ended, or false when none has: its ticket, and the node's
 * id for the message - its packet id in the log - or 0 when the node refused it. A send the
 * node took is handed out only once its MESSAGE is in the log, so a caller can watch it there
 * at once.
 */
bool mesh_tern_take_queued(struct mesh_tern *tern, struct mesh_tern_queued *out);

#ifdef __cplusplus
}
#endif
