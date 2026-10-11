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
 * serial, TCP or Bluetooth LE. Version 7 of `draft/companion.md` in ternmesh/spec, checked
 * against that repository's `vectors/companion.json` (tests/data/tern_companion.json here).
 *
 * Every frame is a type byte, a sequence byte and big-endian fields in a fixed order. The
 * client sends requests (0x01-0x3F) one at a time and the node answers each (0x40-0x7F) with
 * the request's sequence byte; news (0x80-0xBF) arrives whenever the node has some, numbered
 * by a count the node starts at 0 when it answers HELLO. Each news frame but STATE, the three
 * _GONEs and ASKED is a whole record, so a client that applies them in order is right after each.
 *
 * Both ends speak the lesser of their two versions, and a frame is read by that version: a type
 * it does not define is unknown however its bytes read, and a field it does not define is not
 * there. Version 1 added END_SESSION and ASKED, version 2 groups, version 3 SYNCED's count.
 *
 * Written from the specification alone. Like meshcore.h the file is two halves: the codec is
 * pure - bytes to a struct and back - and the conversation below it is the struct
 * mesh_protocol a link carries, which keeps what it learns in a struct mesh_session used as a
 * model (mesh_session_model_sync_begin()).
 */

#define MESH_TERN_VERSION 7U
#define MESH_TERN_MAX_FRAME 180U
#define MESH_TERN_ADDRESS_LEN 32U
#define MESH_TERN_NAME_MAX 31U
#define MESH_TERN_TEXT_MAX 128U
#define MESH_TERN_FIRMWARE_MAX 31U
#define MESH_TERN_REGION_MAX 15U
#define MESH_TERN_GROUP_LEN 8U
#define MESH_TERN_DIGEST_LEN 32U
#define MESH_TERN_LINK_MAX 102U
/* UPDATE_CHUNK: the most `data` an UPDATE_DATA carries, which a client sends all but the last. */
#define MESH_TERN_UPDATE_CHUNK 172U

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
    MESH_TERN_END_SESSION = 0x1A, /* version 1 */
    MESH_TERN_MAKE_GROUP = 0x20,  /* this and the rest of the group requests: version 2 */
    MESH_TERN_LEAVE_GROUP = 0x21,
    MESH_TERN_NAME_GROUP = 0x22,
    MESH_TERN_SEND_GROUP = 0x23,
    MESH_TERN_SEND_INVITE = 0x24,
    MESH_TERN_JOIN = 0x25,
    MESH_TERN_GROUP_LINK = 0x26, /* this and JOIN_LINK: version 7 */
    MESH_TERN_JOIN_LINK = 0x27,
    MESH_TERN_UPDATE_BEGIN = 0x30, /* the three update requests: version 4 */
    MESH_TERN_UPDATE_DATA = 0x31,
    MESH_TERN_UPDATE_END = 0x32,
    MESH_TERN_SET_POSITION = 0x33, /* the three position requests: version 5 */
    MESH_TERN_SHARE = 0x34,
    MESH_TERN_SHARE_GROUP = 0x35,
    /* Answers, node to client. */
    MESH_TERN_OK = 0x40,
    MESH_TERN_ERROR = 0x41,
    MESH_TERN_INFO = 0x42,
    MESH_TERN_SYNCED = 0x43,
    MESH_TERN_QUEUED = 0x44,
    MESH_TERN_MADE = 0x45,     /* version 2 */
    MESH_TERN_UPDATING = 0x46, /* version 4 */
    MESH_TERN_LINK = 0x47,     /* version 7 */
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
    MESH_TERN_ASKED = 0x89, /* version 1 */
    MESH_TERN_GROUP = 0x8A, /* this and the rest: version 2 */
    MESH_TERN_GROUP_GONE = 0x8B,
    MESH_TERN_GROUP_MESSAGE = 0x8C,
    MESH_TERN_INVITE = 0x8D,
    MESH_TERN_POSITION = 0x8E, /* this and the next three: version 5 */
    MESH_TERN_GROUP_POSITION = 0x8F,
    MESH_TERN_SHARING = 0x90,
    MESH_TERN_GROUP_SHARING = 0x91,
    MESH_TERN_CARD = 0x92, /* this and CARD_GONE: version 6 */
    MESH_TERN_CARD_GONE = 0x93,
};

#define MESH_TERN_IS_REQUEST(type) ((type) >= 0x01U && (type) <= 0x3FU)
#define MESH_TERN_IS_ANSWER(type) ((type) >= 0x40U && (type) <= 0x7FU)
#define MESH_TERN_IS_NEWS(type) ((type) >= 0x80U && (type) <= 0xBFU)

enum mesh_tern_setting {
    MESH_TERN_SETTING_REGION = 1,    /* str up to 15: a profile name */
    MESH_TERN_SETTING_ROLE = 2,      /* u8: 0 leaf, 1 relay */
    MESH_TERN_SETTING_POWER = 3,     /* i8: dBm */
    MESH_TERN_SETTING_PASSKEY = 4,   /* u32: 0..999999, or 0xFFFFFFFF for a random one */
    MESH_TERN_SETTING_CARDS = 5,     /* u8: 1 to send presence cards, 0 not; version 6 */
    MESH_TERN_SETTING_CARD_NAME = 6, /* str up to 31: the name the cards carry; version 6 */
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
    MESH_TERN_ERR_NOT_HELD = 9,      /* version 2: a group not held, or an invite */
    MESH_TERN_ERR_NOT_THERE = 10,    /* version 4: no update under way, or not at that offset */
    MESH_TERN_ERR_NOT_AN_IMAGE = 11, /* version 4: an image the node does not run, discarded */
    MESH_TERN_ERR_NOT_CONTACT = 12,  /* version 5: an address that is not a contact */
};

/* Why a node refused first contact (ASKED); any other value is a refusal it cannot name. */
enum mesh_tern_asked {
    MESH_TERN_ASKED_NOT_CONTACT = 1,
    MESH_TERN_ASKED_NO_ROOM = 2,
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
#define MESH_TERN_MESSAGE_READ 0x01U   /* MESSAGE, GROUP_MESSAGE and INVITE flags */
#define MESH_TERN_POWER_CHARGING 0x01U /* POWER flags */
#define MESH_TERN_POWER_EXTERNAL 0x02U
#define MESH_TERN_PERCENT_UNKNOWN 255U
#define MESH_TERN_PASSKEY_RANDOM 0xFFFFFFFFU
/* Positions (version 5). A precision is 1 to 24 - a cell of 360 / 2^precision degrees each way -
   or 0 for none held, or sharing off. */
#define MESH_TERN_PRECISION_MAX 24U
#define MESH_TERN_SHARE_ALTITUDE 0x01U /* SHARE and SHARING fields */
#define MESH_TERN_SHARE_ACCURACY 0x02U
#define MESH_TERN_NO_ALTITUDE (-32768)

/*
 * Any frame, as its fields. Each type fills the members its table in the draft names, under the
 * same names, with a few folded together: the first string a frame carries (text, name, link,
 * firmware, region, or SET's region or card name) is `text`; the one address (to, address,
 * contact) is `address`. A number a frame carries narrower in one type than in another - `wait`, a
 * message's u16 or AIRTIME's u32; `heard`, a NEIGHBOUR's u16 or a CARD's u32; `accuracy` and `age`,
 * wider in one of SET_POSITION and POSITION than the other - is kept in the wider member, and
 * written clamped to the narrower field. SET's value is `text`, `role`, `power`, `passkey` or
 * `cards` as `setting` says. Each string is NUL-terminated after its length, for the reader's
 * convenience; the frame carries no NUL. SYNCED's `news` is read only from version 3, INFO's
 * `board` and `release` from 4, SELF's `cards` and `card_name` from 6.
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
    uint8_t why;       /* ASKED */
    uint8_t news;      /* SYNCED: the node's count as it answers */
    uint8_t cards;     /* SELF, SET 5: whether the node sends presence cards */
    uint8_t precision; /* positions and sharing */
    uint8_t fields;    /* SHARE, SHARING: MESH_TERN_SHARE_* */
    int8_t power;
    int8_t snr;        /* quarter dB */
    int16_t altitude;  /* metres; MESH_TERN_NO_ALTITUDE for none */
    uint16_t accuracy; /* metres, 0 for none */
    uint16_t interval; /* seconds */
    uint16_t minutes;
    uint16_t millivolts;
    int32_t lat; /* 10^-7 degree */
    int32_t lon;
    uint32_t age;    /* seconds */
    uint32_t heard;  /* seconds ago */
    uint32_t size;   /* UPDATE_BEGIN */
    uint32_t offset; /* UPDATE_DATA, UPDATING */
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
    uint32_t from; /* GROUP_MESSAGE: the writer's routing id, 0 for this node */
    uint8_t group[MESH_TERN_GROUP_LEN];
    uint8_t address[MESH_TERN_ADDRESS_LEN];
    uint8_t digest[MESH_TERN_DIGEST_LEN]; /* UPDATE_BEGIN */
    uint8_t text_len;
    char text[MESH_TERN_TEXT_MAX + 1U];
    uint8_t board_len; /* INFO */
    char board[MESH_TERN_FIRMWARE_MAX + 1U];
    uint8_t release_len;
    char release[MESH_TERN_FIRMWARE_MAX + 1U];
    uint8_t card_name_len; /* SELF */
    char card_name[MESH_TERN_NAME_MAX + 1U];
    uint8_t data_len; /* UPDATE_DATA */
    uint8_t data[MESH_TERN_UPDATE_CHUNK];
};

enum mesh_tern_decode_result {
    MESH_TERN_DECODE_OK = 0,
    /* A type, or a setting, the version read by does not define: ERROR 1 for a request. A
       later version's type is this however its fields read, cut short included. */
    MESH_TERN_DECODE_UNKNOWN = MESH_TERN_ERR_UNKNOWN,
    /* Cut short, a string too long for its field or not UTF-8: ERROR 2 for a request. */
    MESH_TERN_DECODE_MALFORMED = MESH_TERN_ERR_MALFORMED,
    /* Under two bytes: not a frame at all, and not answered. */
    MESH_TERN_DECODE_SHORT = 3,
};

/* The version that first defines frame type `type`, or a value past MESH_TERN_VERSION for one
   no version this client knows does. */
uint8_t mesh_tern_since(uint8_t type);

/* The same for SET's setting `setting`. */
uint8_t mesh_tern_setting_since(uint8_t setting);

/* The version a whole frame needs: its type's, or for a SET the later of that and its setting's.
   A frame under two bytes, or a SET with no setting, needs only its type's. */
uint8_t mesh_tern_frame_since(const uint8_t *frame, size_t len);

/* Reads a frame's fields as `version` defines them - the version both ends speak, and for an
   INFO the lesser of that and the version it carries. Bytes after the last field it defines are
   ignored: that is how a later version adds one. */
enum mesh_tern_decode_result mesh_tern_decode(const uint8_t *frame, size_t len, uint8_t version,
                                              struct mesh_tern_frame *out);

/* Writes a frame from its fields, as `version` defines it (INFO by the lesser of that and its
   own `version`). Returns its length, or a negative errno: -EINVAL for a type or setting that
   version does not define or a string or bytes longer than its field, -ENOSPC when `out` is too
   small. The text is not checked for UTF-8; a writer is
   trusted to give text. */
int mesh_tern_encode(const struct mesh_tern_frame *frame, uint8_t version, uint8_t *out,
                     size_t out_len);

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
#define MESH_TERN_MAX_GROUPS 16U
/* The newest group messages and invites, which no screen shows yet and the log does not hold. */
#define MESH_TERN_MAX_GROUP_ITEMS 32U
/* The least ids of messages, group messages and invites that may yet change (`open`). */
#define MESH_TERN_MAX_OPEN 64U
/* Requests waiting their turn behind the one on the wire. */
#define MESH_TERN_QUEUE_LEN 8U
/* SENDs answered that nobody has taken yet (mesh_tern_take_queued()). */
#define MESH_TERN_QUEUED_IDS 8U
/* Positions held from group members, and presence cards held: lists a crowd can fill. */
#define MESH_TERN_MAX_GROUP_POSITIONS 32U
#define MESH_TERN_MAX_CARDS 64U

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

struct mesh_tern_group {
    uint8_t id[MESH_TERN_GROUP_LEN];
    bool synced;
    char name[MESH_TERN_NAME_MAX + 1U];
};

/*
 * A GROUP_MESSAGE or an INVITE, as the node last sent it - `kind` is the news type. Neither goes
 * in the message log: a group is not a channel this client could send on, and an invite is not
 * text to read as a direct message. They are kept whole here for the screens that will show
 * them. `from` is a group message's writer, `contact` an invite's other end, and `text` the
 * group message's text or the invite's name for its group.
 */
struct mesh_tern_group_item {
    uint32_t id;
    uint8_t kind;
    uint8_t flags;
    uint8_t state;
    uint8_t reason;
    uint32_t time;
    uint32_t from;
    uint8_t group[MESH_TERN_GROUP_LEN];
    uint8_t contact[MESH_TERN_ADDRESS_LEN];
    char text[MESH_TERN_TEXT_MAX + 1U];
};

/* One of the node's ids whose record may still change unseen: a message or invite waiting or
   sent, a group message waiting, or any of the three received and not yet read. */
struct mesh_tern_open {
    uint32_t id;
    uint8_t kind; /* the news type that last named it, MESSAGE for one only a STATE has */
    uint8_t state;
    uint8_t flags;
};

/* SELF, as the node last sent it. */
struct mesh_tern_self {
    uint8_t address[MESH_TERN_ADDRESS_LEN];
    uint8_t role;
    int8_t power;
    uint32_t time;
    char region[MESH_TERN_REGION_MAX + 1U];
    /* Version 6: whether the node sends presence cards, and the name they carry. */
    uint8_t cards;
    char card_name[MESH_TERN_NAME_MAX + 1U];
};

/*
 * A position the node holds (version 5): a contact's (POSITION), which is also put on that node's
 * roster entry, or a group member's (GROUP_POSITION), which is not - `from` is what the member
 * claimed, a routing id nothing proved. `fixed` is when the fix was taken, by this client's clock,
 * from the record's `age`, or 0 with no credible clock.
 */
struct mesh_tern_position {
    uint8_t address[MESH_TERN_ADDRESS_LEN]; /* POSITION: the contact */
    uint8_t group[MESH_TERN_GROUP_LEN];     /* GROUP_POSITION: the group */
    uint32_t from;                          /* GROUP_POSITION: the claimed writer */
    uint8_t precision;                      /* 1 to 24 */
    int32_t lat;                            /* the cell's centre, 10^-7 degree */
    int32_t lon;
    int16_t altitude; /* MESH_TERN_NO_ALTITUDE for none */
    uint8_t accuracy; /* metres, 0 for none */
    uint32_t fixed;
    bool synced;
};

/* How the node shares its position, as SHARE or SHARE_GROUP set it (version 5). */
struct mesh_tern_share {
    uint8_t precision; /* 1 to 24; 0 turns sharing off */
    uint8_t fields;    /* MESH_TERN_SHARE_* */
    uint16_t interval; /* seconds */
    uint16_t minutes;  /* how long, from now; 0 until turned off */
};

/* SHARING or GROUP_SHARING: sharing on with a contact (`address`) or a group (`group`). `ends` is
   when it runs out by this client's clock, 0 for until turned off or with no credible clock. */
struct mesh_tern_sharing {
    bool group_kind;
    uint8_t address[MESH_TERN_ADDRESS_LEN];
    uint8_t group[MESH_TERN_GROUP_LEN];
    struct mesh_tern_share share;
    uint32_t ends;
    bool synced;
};

/* A presence card the node holds (version 6). `name` is what its sender claimed, never the
   user's name for the address; `heard` when the node accepted it, by this client's clock. */
struct mesh_tern_card {
    uint8_t address[MESH_TERN_ADDRESS_LEN];
    uint32_t heard;
    bool synced;
    char name[MESH_TERN_NAME_MAX + 1U];
};

/* A firmware update over the companion link (version 4), one image at a time. */
enum mesh_tern_update_phase {
    MESH_TERN_UPDATE_IDLE = 0,
    MESH_TERN_UPDATE_BEGINNING, /* UPDATE_BEGIN out: asking where to go on from */
    MESH_TERN_UPDATE_SENDING,
    MESH_TERN_UPDATE_ENDING,  /* UPDATE_END out: too late to cancel */
    MESH_TERN_UPDATE_WAITING, /* the link went; goes on after the next connection's sync */
    MESH_TERN_UPDATE_FINISHED,
};

enum mesh_tern_update_outcome {
    MESH_TERN_UPDATE_NONE = 0,
    MESH_TERN_UPDATE_RESTARTING,  /* UPDATE_END answered OK: the node restarts into the image */
    MESH_TERN_UPDATE_UNCONFIRMED, /* UPDATE_END unanswered, and never sent again: read release */
    MESH_TERN_UPDATE_REFUSED,     /* ERROR `code` - 5 no room or not updatable, 11 not an image */
    MESH_TERN_UPDATE_CONFUSED,    /* an answer no update request is answered with */
    MESH_TERN_UPDATE_CANCELLED,
};

struct mesh_tern_update {
    enum mesh_tern_update_phase phase;
    enum mesh_tern_update_outcome outcome;
    uint8_t code; /* REFUSED's error */
    /* The image, the caller's until the update finishes, and its SHA-256. */
    const uint8_t *image;
    uint32_t size;
    uint8_t digest[MESH_TERN_DIGEST_LEN];
    uint32_t held;     /* what the node has said it holds */
    uint8_t not_there; /* ERROR 10s in a row with nothing held between */
    bool cancel_wanted;
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
    /* News arrived out of count, so some was missed: the sync under way proves nothing about
       what is gone, and another is asked once it is through. */
    bool missed_news;
    /* The greatest id held when news was first missed, until a sync that asked again from it
       finishes. A connection starting counts as news missed: what changed while there was none
       was not sent. */
    bool has_missed_since;
    uint32_t missed_since;

    /* The version this client says HELLO with: MESH_TERN_VERSION, or less for a test playing an
       older client. */
    uint8_t version;
    /* The lesser of that and the node's, once INFO has said the node's: what is read and sent. */
    bool has_agreed;
    uint8_t agreed;
    /* The version spoken at the last sync that finished. A sync at a later one asks from 0,
       once: the node may hold records the earlier version was never sent, under lower ids. */
    bool has_synced_version;
    uint8_t synced_version;

    uint8_t node_version;
    char firmware[MESH_TERN_FIRMWARE_MAX + 1U];
    /* Version 4: the hardware the node's firmware is built for, empty when it cannot be updated
       over this protocol, and that firmware's version, empty for a build with none. */
    char board[MESH_TERN_FIRMWARE_MAX + 1U];
    char release[MESH_TERN_FIRMWARE_MAX + 1U];
    bool has_self;
    struct mesh_tern_self self;
    uint32_t self_node;
    bool has_airtime;
    struct mesh_tern_airtime airtime;

    struct mesh_tern_contact contacts[MESH_TERN_MAX_CONTACTS];
    size_t contact_count;
    struct mesh_tern_neighbour neighbours[MESH_TERN_MAX_NEIGHBOURS];
    size_t neighbour_count;
    struct mesh_tern_group groups[MESH_TERN_MAX_GROUPS];
    size_t group_count;
    struct mesh_tern_group_item group_items[MESH_TERN_MAX_GROUP_ITEMS];
    size_t group_item_count;
    struct mesh_tern_open open[MESH_TERN_MAX_OPEN];
    size_t open_count;
    /* Version 5: the positions held from contacts and from group members, and with whom the
       node shares its own. Version 6: the presence cards held - who is about. */
    struct mesh_tern_position positions[MESH_TERN_MAX_CONTACTS];
    size_t position_count;
    struct mesh_tern_position group_positions[MESH_TERN_MAX_GROUP_POSITIONS];
    size_t group_position_count;
    struct mesh_tern_sharing sharing[MESH_TERN_MAX_CONTACTS + MESH_TERN_MAX_GROUPS];
    size_t sharing_count;
    struct mesh_tern_card cards[MESH_TERN_MAX_CARDS];
    size_t card_count;

    /* Version 7: the join code a GROUP_LINK was answered with, until mesh_tern_take_link() takes
       it. The link is the group's secret, so it is held no longer than that. */
    bool has_link;
    char link[MESH_TERN_LINK_MAX + 1U];

    struct mesh_tern_update update;

    /* The last first contact the node refused (ASKED), for a screen that offers to save it. */
    bool has_asked;
    uint8_t asked_address[MESH_TERN_ADDRESS_LEN];
    uint8_t asked_why;

    /* The greatest id this conversation holds, of a message, group message or invite, which the
       next SYNC asks after. Kept across a reconnect to the same node, cleared when another
       answers. */
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

/* Marks every received message up to and including `through` - a packet id in the log - as
   read, on the node and so on every client driving it. 0 or a negative errno as
   mesh_tern_send_text(). */
int mesh_tern_mark_read(struct mesh_tern *tern, uint32_t through);

/* Saves `address` as a contact under `name` (empty is a name), or renames it. 0 or a negative
   errno as mesh_tern_send_text(), -EMSGSIZE for a name past MESH_TERN_NAME_MAX. */
int mesh_tern_save_contact(struct mesh_tern *tern, const uint8_t address[MESH_TERN_ADDRESS_LEN],
                           const char *name);

/*
 * Version 1 and later. Ends the session the node shares with `node` - a node number, as
 * mesh_tern_send_text() takes - so first contact is made again. -EOPNOTSUPP when the version
 * spoken does not define it, which is refused here rather than sent: a node of an earlier
 * version could not say what it changed. Otherwise as mesh_tern_send_text().
 */
int mesh_tern_end_session(struct mesh_tern *tern, uint32_t node);

/*
 * Version 2 and later: groups, each named by the id the node gives it. What becomes of each
 * request arrives as news - GROUP, GROUP_GONE, GROUP_MESSAGE, INVITE - into the conversation's
 * lists. 0 once the request is queued, or a negative errno: -EOPNOTSUPP below version 2, and the
 * rest as mesh_tern_send_text(). `send_invite` invites `node` (a node number with an address
 * known); `join` takes the group a received invite, by the node's id for it, is to.
 */
int mesh_tern_make_group(struct mesh_tern *tern, const char *name);
int mesh_tern_leave_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN]);
int mesh_tern_name_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                         const char *name);
int mesh_tern_send_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                         const char *text);
int mesh_tern_send_invite(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                          uint32_t node);
int mesh_tern_join(struct mesh_tern *tern, uint32_t invite_id);

/*
 * SET: one of the node's settings. `mesh_tern_set_value()` is a number - a role, a power in dBm
 * (as a two's complement u32), a passkey, or cards on (1) or off (0) - and `mesh_tern_set_text()`
 * a string, a region or the cards' name. -EOPNOTSUPP for a setting the version spoken lacks
 * (cards are version 6), -EINVAL for one whose value is the other kind; else as
 * mesh_tern_send_text(). The cards and their name are the one thing a node puts on the air in
 * clear, so neither is sent but at the user's word.
 */
int mesh_tern_set_value(struct mesh_tern *tern, uint8_t setting, uint32_t value);
int mesh_tern_set_text(struct mesh_tern *tern, uint8_t setting, const char *text);

/*
 * Version 5: positions. `set_position` gives the node this client's position, in 10^-7 degree,
 * metres (MESH_TERN_NO_ALTITUDE for none), metres of accuracy (0 for none) and the fix's age in
 * seconds; the node rounds and sends it only to whom sharing is on with. `share` turns sharing
 * with the contact `node` on, changes it or (precision 0) turns it off, and `share_group` the same
 * for a group: only ever at the user's word. What the node holds then arrives as SHARING and
 * POSITION news into the conversation's lists. 0 once queued, -EOPNOTSUPP below version 5, -EINVAL
 * for a latitude or longitude out of range, else as mesh_tern_send_text().
 */
int mesh_tern_set_position(struct mesh_tern *tern, int32_t lat, int32_t lon, int16_t altitude,
                           uint16_t accuracy, uint16_t age);
int mesh_tern_share(struct mesh_tern *tern, uint32_t node, const struct mesh_tern_share *share);
int mesh_tern_share_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                          const struct mesh_tern_share *share);

/*
 * Version 7: join codes. `group_link` asks the node for the join code of a group it holds -
 * only when the user asked to see or share it - and the answer is had once from
 * mesh_tern_take_link(), which wipes it: a client that keeps no secret has none to lose.
 * `join_link` hands the node a code the user gave, and the group arrives as GROUP news; the frame
 * is wiped from the queue once answered. -EOPNOTSUPP below version 7, else as
 * mesh_tern_send_text(), -EMSGSIZE for a link past MESH_TERN_LINK_MAX.
 */
int mesh_tern_group_link(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN]);
bool mesh_tern_take_link(struct mesh_tern *tern, char *out, size_t out_len);
int mesh_tern_join_link(struct mesh_tern *tern, const char *link);

/*
 * Version 4: a firmware update over the companion link. `image` stays the caller's, unchanged,
 * until the update has finished (or was never begun). Begins at once on a ready link; when the
 * link drops part of the way it waits, and goes on from wherever the node says after the next
 * connection's first sync. An UPDATE_END given up on is never sent again: the node may be
 * restarting into the image, and its next INFO's release says. 0 once begun, -EOPNOTSUPP below
 * version 4, -EBUSY with an update under way, -ENOTCONN before a sync, -EINVAL for an empty
 * image. `mesh_tern_update_cancel()` stops before UPDATE_END and is false once that is out.
 */
int mesh_tern_update_start(struct mesh_tern *tern, const uint8_t *image, size_t size);
bool mesh_tern_update_cancel(struct mesh_tern *tern);
/* The update as it stands; IDLE when none was begun. */
const struct mesh_tern_update *mesh_tern_update_status(const struct mesh_tern *tern);

/*
 * The packet id in the log of the node's message `message_id`: the id scoped to the node, so two
 * nodes counting from 1 never share one. Read back with the same call. 0 for 0. Only meaningful
 * once SELF has named the node, which every sync does first.
 */
uint32_t mesh_tern_packet_id(const struct mesh_tern *tern, uint32_t message_id);

/*
 * How the oldest send not yet taken ended, or false when none has: its ticket, and the
 * message's packet id in the log (mesh_tern_packet_id()), or 0 when the node refused it. A send the
 * node took is handed out only once its MESSAGE is in the log, so a caller can watch it there
 * at once.
 */
bool mesh_tern_take_queued(struct mesh_tern *tern, struct mesh_tern_queued *out);

#ifdef __cplusplus
}
#endif
