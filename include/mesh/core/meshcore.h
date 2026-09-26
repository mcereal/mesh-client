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
 * MeshCore's companion protocol: what an app says to a radio running MeshCore's
 * `companion_radio` firmware, over BLE (the Nordic UART service) or a serial port.
 *
 * Every frame is one code byte and packed little-endian fields. The app sends `CMD_*` and the
 * radio answers each with one `RESP_CODE_*` - or, for the contact list, a start, one record per
 * contact and an end. Anything at 0x80 and up is a `PUSH_CODE_*` the radio sends on its own, and
 * one can land between a command and its answer. Checked against MeshCore's
 * examples/companion_radio/MyMesh.cpp at companion-v1.17.1 (firmware version code 13).
 *
 * The file is two halves. The codec is pure: bytes to structs and back, no state, which is what
 * the tests and the fuzzer drive. The conversation below it is the `struct mesh_protocol` a link
 * carries, and it keeps what it learns in a `struct mesh_session` used as a model - the roster,
 * the channel table and the message log every screen already reads (see
 * mesh_session_model_sync_begin()). Nothing Meshtastic's is sent on its account: the session's
 * own send path is a refusal while MeshCore holds the link.
 */

#define MESH_MESHCORE_MAX_FRAME 176U
#define MESH_MESHCORE_PUBKEY_LEN 32U
#define MESH_MESHCORE_PREFIX_LEN 6U
#define MESH_MESHCORE_NAME_LEN 32U
#define MESH_MESHCORE_SECRET_LEN 16U
/* What an app says it understands in DEVICE_QUERY. 3 is the first with SNR on a message. */
#define MESH_MESHCORE_APP_VERSION 3U
/* MAX_TEXT_LEN in the firmware: ten AES blocks. A channel message spends some of it on the
   sender's name, which the radio prefixes itself. */
#define MESH_MESHCORE_TEXT_MAX 160U

enum mesh_meshcore_cmd {
    MESH_MESHCORE_CMD_APP_START = 1,
    MESH_MESHCORE_CMD_SEND_TXT_MSG = 2,
    MESH_MESHCORE_CMD_SEND_CHANNEL_TXT_MSG = 3,
    MESH_MESHCORE_CMD_GET_CONTACTS = 4,
    MESH_MESHCORE_CMD_GET_DEVICE_TIME = 5,
    MESH_MESHCORE_CMD_SET_DEVICE_TIME = 6,
    MESH_MESHCORE_CMD_SEND_SELF_ADVERT = 7,
    MESH_MESHCORE_CMD_SET_ADVERT_NAME = 8,
    MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE = 10,
    MESH_MESHCORE_CMD_SET_RADIO_PARAMS = 11,
    MESH_MESHCORE_CMD_SET_RADIO_TX_POWER = 12,
    MESH_MESHCORE_CMD_RESET_PATH = 13,
    MESH_MESHCORE_CMD_SET_ADVERT_LATLON = 14,
    MESH_MESHCORE_CMD_REMOVE_CONTACT = 15,
    MESH_MESHCORE_CMD_REBOOT = 19,
    MESH_MESHCORE_CMD_GET_BATT_AND_STORAGE = 20,
    MESH_MESHCORE_CMD_DEVICE_QUERY = 22,
    MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY = 30,
    MESH_MESHCORE_CMD_GET_CHANNEL = 31,
};

enum mesh_meshcore_resp {
    MESH_MESHCORE_RESP_OK = 0,
    MESH_MESHCORE_RESP_ERR = 1,
    MESH_MESHCORE_RESP_CONTACTS_START = 2,
    MESH_MESHCORE_RESP_CONTACT = 3,
    MESH_MESHCORE_RESP_END_OF_CONTACTS = 4,
    MESH_MESHCORE_RESP_SELF_INFO = 5,
    MESH_MESHCORE_RESP_SENT = 6,
    MESH_MESHCORE_RESP_CONTACT_MSG_RECV = 7,
    MESH_MESHCORE_RESP_CHANNEL_MSG_RECV = 8,
    MESH_MESHCORE_RESP_CURR_TIME = 9,
    MESH_MESHCORE_RESP_NO_MORE_MESSAGES = 10,
    MESH_MESHCORE_RESP_BATT_AND_STORAGE = 12,
    MESH_MESHCORE_RESP_DEVICE_INFO = 13,
    MESH_MESHCORE_RESP_DISABLED = 15,
    MESH_MESHCORE_RESP_CONTACT_MSG_RECV_V3 = 16,
    MESH_MESHCORE_RESP_CHANNEL_MSG_RECV_V3 = 17,
    MESH_MESHCORE_RESP_CHANNEL_INFO = 18,
    MESH_MESHCORE_RESP_CHANNEL_DATA_RECV = 27,
};

enum mesh_meshcore_push {
    MESH_MESHCORE_PUSH_ADVERT = 0x80,
    MESH_MESHCORE_PUSH_PATH_UPDATED = 0x81,
    MESH_MESHCORE_PUSH_SEND_CONFIRMED = 0x82,
    MESH_MESHCORE_PUSH_MSG_WAITING = 0x83,
    MESH_MESHCORE_PUSH_LOG_RX_DATA = 0x88,
    MESH_MESHCORE_PUSH_NEW_ADVERT = 0x8A,
    MESH_MESHCORE_PUSH_CONTACT_DELETED = 0x8F,
    MESH_MESHCORE_PUSH_CONTACTS_FULL = 0x90,
};

enum mesh_meshcore_err {
    MESH_MESHCORE_ERR_UNSUPPORTED_CMD = 1,
    MESH_MESHCORE_ERR_NOT_FOUND = 2,
    MESH_MESHCORE_ERR_TABLE_FULL = 3,
    MESH_MESHCORE_ERR_BAD_STATE = 4,
    MESH_MESHCORE_ERR_FILE_IO = 5,
    MESH_MESHCORE_ERR_ILLEGAL_ARG = 6,
};

/* What a node advertises itself as. */
enum mesh_meshcore_adv_type {
    MESH_MESHCORE_ADV_NONE = 0,
    MESH_MESHCORE_ADV_CHAT = 1,
    MESH_MESHCORE_ADV_REPEATER = 2,
    MESH_MESHCORE_ADV_ROOM = 3,
    MESH_MESHCORE_ADV_SENSOR = 4,
};

enum mesh_meshcore_txt_type {
    MESH_MESHCORE_TXT_PLAIN = 0,
    MESH_MESHCORE_TXT_CLI_DATA = 1,
    /* A room server's relay of somebody's post: a 4-byte author prefix precedes the text. */
    MESH_MESHCORE_TXT_SIGNED_PLAIN = 2,
};

/* A path length with no path: the packet came direct, or no route is known yet. Otherwise the
   low six bits are the hop count and the top two the hash size less one. */
#define MESH_MESHCORE_PATH_NONE 0xFFU
#define MESH_MESHCORE_PATH_HOPS(len) ((uint8_t)((len) & 0x3FU))

/* RESP_CODE_SELF_INFO, the answer to APP_START: the radio's own identity and radio settings. */
struct mesh_meshcore_self_info {
    uint8_t adv_type;
    uint8_t tx_power_dbm;
    uint8_t max_tx_power_dbm;
    uint8_t public_key[MESH_MESHCORE_PUBKEY_LEN];
    int32_t latitude_e6;
    int32_t longitude_e6;
    uint8_t multi_acks;
    uint8_t advert_loc_policy;
    uint8_t telemetry_modes;
    uint8_t manual_add_contacts;
    uint32_t frequency_khz;
    uint32_t bandwidth_hz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    char name[MESH_MESHCORE_NAME_LEN + 1U];
};

/* RESP_CODE_DEVICE_INFO, the answer to DEVICE_QUERY. */
struct mesh_meshcore_device_info {
    uint8_t firmware_version;
    uint16_t max_contacts;
    uint8_t max_channels;
    /* The stored PIN; 0 when the radio makes up a new one each boot and shows it on its screen. */
    uint32_t ble_pin;
    char build_date[13];
    char model[41];
    char version[21];
};

/* One contact record: RESP_CODE_CONTACT and PUSH_CODE_NEW_ADVERT carry the same 148 bytes. */
struct mesh_meshcore_contact {
    uint8_t public_key[MESH_MESHCORE_PUBKEY_LEN];
    uint8_t type; /* enum mesh_meshcore_adv_type */
    uint8_t flags;
    uint8_t out_path_len; /* MESH_MESHCORE_PATH_NONE when no route is known */
    char name[MESH_MESHCORE_NAME_LEN + 1U];
    uint32_t last_advert; /* the advert's own timestamp, on the sender's clock */
    int32_t latitude_e6;
    int32_t longitude_e6;
    uint32_t lastmod; /* the radio's clock when the record last changed */
};

/* One queued message out of SYNC_NEXT_MESSAGE, in any of the four shapes it arrives in. */
struct mesh_meshcore_message {
    bool channel;
    uint8_t channel_index;
    /* A direct message's sender. Channel messages carry no key; the name is in the text. */
    uint8_t sender_prefix[MESH_MESHCORE_PREFIX_LEN];
    /* The pre-V3 shapes carry none. A queue can hold both: a message is encoded as it
       arrives, against whatever version the last app to connect asked for. */
    bool has_snr;
    int8_t snr_q4; /* SNR x4 */
    uint8_t path_len;
    uint8_t txt_type;
    uint32_t timestamp; /* the sender's clock */
    bool has_author;
    uint8_t author_prefix[4];
    const uint8_t *text; /* into the frame; not NUL-terminated */
    size_t text_len;
};

/* RESP_CODE_SENT: a direct message left, and the ack that will prove it arrived. */
struct mesh_meshcore_sent {
    bool flood;
    uint32_t expected_ack;
    uint32_t timeout_ms;
};

/* RESP_CODE_CHANNEL_INFO. An unused slot has an empty name and an all-zero secret. */
struct mesh_meshcore_channel {
    uint8_t index;
    char name[MESH_MESHCORE_NAME_LEN + 1U];
    uint8_t secret[MESH_MESHCORE_SECRET_LEN];
};

/* ------------------------------------------------------------------------------- codec */

/* Each returns 0, or -EBADMSG when the frame is not that shape or is too short for it. The
   strings are NUL-terminated copies; the message text points into `frame`. */
int mesh_meshcore_decode_self_info(const uint8_t *frame, size_t len,
                                   struct mesh_meshcore_self_info *out);
int mesh_meshcore_decode_device_info(const uint8_t *frame, size_t len,
                                     struct mesh_meshcore_device_info *out);
int mesh_meshcore_decode_contact(const uint8_t *frame, size_t len,
                                 struct mesh_meshcore_contact *out);
int mesh_meshcore_decode_message(const uint8_t *frame, size_t len,
                                 struct mesh_meshcore_message *out);
int mesh_meshcore_decode_sent(const uint8_t *frame, size_t len, struct mesh_meshcore_sent *out);
int mesh_meshcore_decode_channel(const uint8_t *frame, size_t len,
                                 struct mesh_meshcore_channel *out);

/* Each writes one command into `out` and returns its length, or -EINVAL / -EMSGSIZE / -ENOSPC. */
int mesh_meshcore_encode_app_start(const char *app_name, uint8_t *out, size_t out_len);
int mesh_meshcore_encode_device_query(uint8_t version, uint8_t *out, size_t out_len);
int mesh_meshcore_encode_u32(uint8_t cmd, uint32_t value, uint8_t *out, size_t out_len);
int mesh_meshcore_encode_byte(uint8_t cmd, uint8_t value, uint8_t *out, size_t out_len);
int mesh_meshcore_encode_key(uint8_t cmd, const uint8_t key[MESH_MESHCORE_PUBKEY_LEN], uint8_t *out,
                             size_t out_len);
int mesh_meshcore_encode_text(const uint8_t prefix[MESH_MESHCORE_PREFIX_LEN], uint8_t attempt,
                              uint32_t timestamp, const char *text, uint8_t *out, size_t out_len);
int mesh_meshcore_encode_channel_text(uint8_t channel, uint32_t timestamp, const char *text,
                                      uint8_t *out, size_t out_len);
/* SET_ADVERT_NAME: the name, unterminated; the firmware keeps at most MESH_MESHCORE_NAME_LEN
   bytes of it, so a longer one is refused here rather than cut there. */
int mesh_meshcore_encode_name(const char *name, uint8_t *out, size_t out_len);
/* SET_RADIO_PARAMS: frequency in kHz, bandwidth in Hz, spreading factor and coding rate. */
int mesh_meshcore_encode_radio_params(uint32_t frequency_khz, uint32_t bandwidth_hz,
                                      uint8_t spreading_factor, uint8_t coding_rate, uint8_t *out,
                                      size_t out_len);
/* SET_ADVERT_LATLON: degrees times a million, as SELF_INFO reports them. */
int mesh_meshcore_encode_latlon(int32_t latitude_e6, int32_t longitude_e6, uint8_t *out,
                                size_t out_len);
/* REBOOT carries the word, so a stray byte cannot reboot a radio. */
int mesh_meshcore_encode_reboot(uint8_t *out, size_t out_len);

/*
 * The roster's number for a MeshCore key: its first four bytes, big-endian.
 *
 * MeshCore names a node by its 32-byte key and addresses one by a prefix of it; the client's
 * roster, messages and screens name one by 32 bits. Taking the prefix means a direct message -
 * which carries only a six-byte prefix - resolves to the right entry without a lookup, and the
 * "!hex" a screen falls back to is the start of the key a MeshCore user already recognises. The
 * two numbers the roster reserves (0, and the broadcast address) are nudged off.
 */
uint32_t mesh_meshcore_node_id(const uint8_t *key, size_t key_len);

/* ------------------------------------------------------------------------ conversation */

#define MESH_MESHCORE_QUEUE_LEN 16U
#define MESH_MESHCORE_PENDING_SENDS 8U
/* A command the radio has not answered in this long is given up on, and two in a row is a link
   whose far end has gone. */
#define MESH_MESHCORE_REPLY_TIMEOUT_MS 10000U
/* How many times a direct message is tried before it is marked failed; the last goes flooded. */
#define MESH_MESHCORE_SEND_ATTEMPTS 3U

enum mesh_meshcore_phase {
    MESH_MESHCORE_IDLE = 0,
    MESH_MESHCORE_HANDSHAKE, /* device query, app start, clock */
    MESH_MESHCORE_CONTACTS,
    MESH_MESHCORE_CHANNELS,
    MESH_MESHCORE_READY,
};

struct mesh_meshcore_request {
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    uint8_t len;
    /* The message log entry this command carries, 0 for none. */
    uint32_t packet_id;
};

/* One direct message waiting for its ack. */
struct mesh_meshcore_pending {
    uint32_t packet_id; /* 0 for a free slot */
    uint32_t expected_ack;
    uint64_t deadline_ms; /* 0 while the SENT reply is still to come */
    uint32_t timestamp;
    uint8_t attempt;
    /* Whole, not a prefix: the last attempt resets the route, which names the contact by key. */
    uint8_t key[MESH_MESHCORE_PUBKEY_LEN];
    char text[MESH_MESHCORE_TEXT_MAX + 1U];
};

struct mesh_meshcore {
    struct mesh_session *model;
    mesh_protocol_send_fn send;
    void *send_ctx;

    uint8_t phase; /* enum mesh_meshcore_phase */
    struct mesh_meshcore_request queue[MESH_MESHCORE_QUEUE_LEN];
    size_t queue_head;
    size_t queue_count;
    /* The command at the head of the queue has been written and not yet answered. */
    bool awaiting;
    uint64_t awaiting_since_ms;
    uint8_t timeouts; /* consecutive commands given up on */
    uint64_t now_ms;

    bool has_device;
    struct mesh_meshcore_device_info device;
    bool has_self;
    struct mesh_meshcore_self_info self;
    uint32_t self_node;
    uint8_t channel_probe;
    /* The contact list's newest lastmod, so a refresh asks only for what changed. */
    uint32_t contacts_since;
    bool battery_valid;
    uint16_t battery_mv;
    /*
     * The radio's clock, for a message's timestamp when ours is not credible (a Brick with no
     * network boots into 1970). Read with GET_DEVICE_TIME and advanced by our monotonic clock;
     * `radio_clock` is 0 until it has been read. `last_timestamp` keeps stamps strictly
     * increasing: the timestamp is inside what is encrypted, so two identical texts sent in the
     * same second would otherwise be one packet to every node that deduplicates.
     */
    uint32_t radio_clock;
    uint64_t radio_clock_at_ms;
    uint32_t last_timestamp;

    struct mesh_meshcore_pending pending[MESH_MESHCORE_PENDING_SENDS];
    uint32_t next_packet_id;

    /* A settings save in flight: how many of its commands are still unanswered, and the first
       refusal among those that were. Settled into the model's write counters once the last
       one is answered. */
    uint8_t writes_outstanding;
    int32_t write_error;
};

/*
 * A settings save, as MeshCore's commands take it: each group is written only when its `set_`
 * flag is, and a group is written whole - the radio parameters are one command.
 */
struct mesh_meshcore_settings_write {
    bool set_name;
    char name[MESH_MESHCORE_NAME_LEN + 1U];
    bool set_radio;
    uint32_t frequency_khz;
    uint32_t bandwidth_hz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    bool set_tx_power;
    int8_t tx_power_dbm;
    bool set_position;
    int32_t latitude_e6;
    int32_t longitude_e6;
};

/* `model` is the session the conversation fills; it must outlive every link that carries this. */
void mesh_meshcore_init(struct mesh_meshcore *meshcore, struct mesh_session *model);
/* This conversation as a link carries it: MeshCore's framing, the Nordic UART profile. */
struct mesh_protocol mesh_meshcore_protocol(struct mesh_meshcore *meshcore);
bool mesh_meshcore_ready(const struct mesh_meshcore *meshcore);

/*
 * The most bytes of text a message to `dest` may carry. MESH_MESHCORE_TEXT_MAX for a node; for a
 * channel, less the "Name: " the firmware writes in front of it, since it cuts whatever no longer
 * fits (BaseChatMesh::sendGroupMessage) rather than refusing it. Before the radio has named
 * itself the longest name it could have is assumed.
 */
size_t mesh_meshcore_text_max(const struct mesh_meshcore *meshcore, uint32_t dest);

/*
 * Sends `text` to `dest` - a node, or MESH_MESSAGE_BROADCAST_ADDR for channel `channel` - and
 * logs it in the model as ours. Returns the log entry's id through `out_packet_id` and 0, or
 * -ENOTCONN without a link, -ENOENT for a node whose key the roster does not hold, -EMSGSIZE for
 * text past mesh_meshcore_text_max(), -ENOBUFS when the command queue is full.
 */
int mesh_meshcore_send_text(struct mesh_meshcore *meshcore, uint32_t dest, uint8_t channel,
                            const char *text, uint32_t *out_packet_id);
/* Announces this radio: flooded across the mesh, or to the nodes in earshot only. */
int mesh_meshcore_send_advert(struct mesh_meshcore *meshcore, bool flood);

/*
 * Queues the save's commands and then APP_START, whose SELF_INFO is the read-back. Returns how
 * many commands were queued (> 0), -EINVAL for a save that writes nothing or carries a value
 * the codec refuses, -ENOTCONN without a link, -EBUSY while an earlier save is unanswered, or
 * -ENOBUFS when the queue cannot take it whole. The outcome lands in the model's
 * mesh_radio_settings: writes_acked once every command was answered OK, else writes_failed with
 * the first MeshCore error code (or MESH_RADIO_SETTINGS_WRITE_TIMEOUT) in last_write_error.
 */
int mesh_meshcore_write_settings(struct mesh_meshcore *meshcore,
                                 const struct mesh_meshcore_settings_write *write);
/* Asks for SELF_INFO again, which re-projects the settings. 1 when asked, 0 when already
   asked, -ENOTCONN without a link. */
int mesh_meshcore_refresh_settings(struct mesh_meshcore *meshcore);
/* Reboots the radio. The link drops without an answer and auto-connect brings it back. */
int mesh_meshcore_reboot(struct mesh_meshcore *meshcore);

#ifdef __cplusplus
}
#endif
