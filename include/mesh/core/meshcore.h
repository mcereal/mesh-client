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
/* The radio's private key as EXPORT_PRIVATE_KEY gives it and IMPORT_PRIVATE_KEY takes it: an
   Ed25519 key in its expanded 64-byte form, from which the firmware derives the public one. */
#define MESH_MESHCORE_PRVKEY_LEN 64U
/* What an app says it understands in DEVICE_QUERY. 3 is the first with SNR on a message. */
#define MESH_MESHCORE_APP_VERSION 3U
/* MAX_TEXT_LEN in the firmware: ten AES blocks. A channel message spends some of it on the
   sender's name, which the radio prefixes itself. */
#define MESH_MESHCORE_TEXT_MAX 160U
/* A repeater's or room server's password, as its prefs keep it: 16 bytes with the NUL. */
#define MESH_MESHCORE_PASSWORD_MAX 15U

enum mesh_meshcore_cmd {
    MESH_MESHCORE_CMD_APP_START = 1,
    MESH_MESHCORE_CMD_SEND_TXT_MSG = 2,
    MESH_MESHCORE_CMD_SEND_CHANNEL_TXT_MSG = 3,
    MESH_MESHCORE_CMD_GET_CONTACTS = 4,
    MESH_MESHCORE_CMD_GET_DEVICE_TIME = 5,
    MESH_MESHCORE_CMD_SET_DEVICE_TIME = 6,
    MESH_MESHCORE_CMD_SEND_SELF_ADVERT = 7,
    MESH_MESHCORE_CMD_SET_ADVERT_NAME = 8,
    MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT = 9,
    MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE = 10,
    MESH_MESHCORE_CMD_SET_RADIO_PARAMS = 11,
    MESH_MESHCORE_CMD_SET_RADIO_TX_POWER = 12,
    MESH_MESHCORE_CMD_RESET_PATH = 13,
    MESH_MESHCORE_CMD_SET_ADVERT_LATLON = 14,
    MESH_MESHCORE_CMD_REMOVE_CONTACT = 15,
    MESH_MESHCORE_CMD_IMPORT_CONTACT = 18,
    MESH_MESHCORE_CMD_REBOOT = 19,
    MESH_MESHCORE_CMD_GET_BATT_AND_STORAGE = 20,
    MESH_MESHCORE_CMD_DEVICE_QUERY = 22,
    /* The radio's private key out, and one in. Both are compiled in only with
       ENABLE_PRIVATE_KEY_EXPORT / _IMPORT, which the firmware's own build turns on and a
       variant may turn off; without them the answer is RESP_DISABLED. An import the radio
       takes becomes its identity at once, with its contacts' shared secrets worked out again. */
    MESH_MESHCORE_CMD_EXPORT_PRIVATE_KEY = 23,
    MESH_MESHCORE_CMD_IMPORT_PRIVATE_KEY = 24,
    MESH_MESHCORE_CMD_SEND_LOGIN = 26,
    MESH_MESHCORE_CMD_SEND_STATUS_REQ = 27,
    MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY = 30,
    MESH_MESHCORE_CMD_GET_CHANNEL = 31,
    MESH_MESHCORE_CMD_SET_CHANNEL = 32,
    /* A tag, an auth code, flags whose low two bits say each hop is named by 1 << n bytes, then
       the hops. Sent direct along them; answered by a TRACE_DATA carrying the tag. */
    MESH_MESHCORE_CMD_SEND_TRACE_PATH = 36,
    /* A u32: 0 for a new random PIN each boot, else six digits; read at boot, not before. */
    MESH_MESHCORE_CMD_SET_DEVICE_PIN = 37,
    MESH_MESHCORE_CMD_SET_OTHER_PARAMS = 38,
    MESH_MESHCORE_CMD_SEND_TELEMETRY_REQ = 39,
    /* The key, then a request the node itself reads - its first byte is enum
       mesh_meshcore_req_type. Answered by a BINARY_RESPONSE carrying the tag SENT named. */
    MESH_MESHCORE_CMD_SEND_BINARY_REQ = 50,
    MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ = 52,
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
    MESH_MESHCORE_RESP_PRIVATE_KEY = 14, /* the answer to EXPORT_PRIVATE_KEY: the 64 bytes */
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
    MESH_MESHCORE_PUSH_LOGIN_SUCCESS = 0x85,
    MESH_MESHCORE_PUSH_LOGIN_FAIL = 0x86,
    MESH_MESHCORE_PUSH_STATUS_RESPONSE = 0x87,
    MESH_MESHCORE_PUSH_LOG_RX_DATA = 0x88,
    MESH_MESHCORE_PUSH_TRACE_DATA = 0x89,
    MESH_MESHCORE_PUSH_NEW_ADVERT = 0x8A,
    MESH_MESHCORE_PUSH_TELEMETRY_RESPONSE = 0x8B,
    MESH_MESHCORE_PUSH_BINARY_RESPONSE = 0x8C,
    MESH_MESHCORE_PUSH_PATH_DISCOVERY_RESPONSE = 0x8D,
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

/* What a SEND_BINARY_REQ asks a repeater for: the first byte of the request it reads. */
enum mesh_meshcore_req_type {
    MESH_MESHCORE_REQ_GET_NEIGHBOURS = 0x06,
};

/* A path length with no path: the packet came direct, or no route is known yet. Otherwise the
   low six bits are the hop count and the top two the hash size less one. */
#define MESH_MESHCORE_PATH_NONE 0xFFU
/* MAX_PATH_SIZE: the route a contact record carries, in bytes. */
#define MESH_MESHCORE_PATH_MAX 64U
#define MESH_MESHCORE_PATH_HOPS(len) ((uint8_t)((len) & 0x3FU))
/* A contact's flags: bit 0 is the favourite; the bits above it are what it may ask for. */
#define MESH_MESHCORE_CONTACT_FAVORITE 0x01U

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
    uint8_t out_path[MESH_MESHCORE_PATH_MAX];
    char name[MESH_MESHCORE_NAME_LEN + 1U];
    uint32_t last_advert; /* the advert's own timestamp, on the sender's clock */
    int32_t latitude_e6;
    int32_t longitude_e6;
    uint32_t lastmod; /* the radio's clock when the record last changed */
};

/*
 * A node's readings, out of the Cayenne LPP a TELEMETRY_RESPONSE carries. Channel 1 is the node
 * itself - its battery, and the MCU's own temperature - and the channels after it are its
 * sensors, so a sensor's temperature is preferred over the MCU's when both are there.
 */
struct mesh_meshcore_telemetry {
    bool has_battery;
    float battery_v;
    bool has_temperature;
    float temperature_c;
    bool has_humidity;
    float humidity_pct;
    bool has_pressure;
    float pressure_hpa;
    bool has_lux;
    float lux;
    bool has_voltage; /* a sensor's, not the battery */
    float voltage_v;
    bool has_current;
    float current_a;
    bool has_position;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_m;
};

/*
 * A repeater's or room server's own counters: the stats struct a STATUS_RESPONSE carries,
 * copied out of the node's memory as it lies there, little-endian. The first 48 bytes are the
 * same for both kinds; what follows is a repeater's receive airtime and errors, or a room
 * server's post counts, and an older firmware sends less of either.
 */
#define MESH_MESHCORE_STATUS_LEN 48U

struct mesh_meshcore_status {
    uint16_t battery_mv;
    uint16_t tx_queue_len;
    int16_t noise_floor; /* dBm */
    int16_t last_rssi;   /* dBm, of the last packet it received */
    uint32_t packets_recv;
    uint32_t packets_sent;
    uint32_t air_time_secs; /* spent transmitting, since boot */
    uint32_t uptime_secs;
    uint32_t sent_flood;
    uint32_t sent_direct;
    uint32_t recv_flood;
    uint32_t recv_direct;
    uint16_t err_events;
    int16_t last_snr_q4; /* SNR x4 */
    uint16_t direct_dups;
    uint16_t flood_dups;
    /* A repeater's tail. */
    bool has_rx_air_time;
    uint32_t rx_air_time_secs;
    bool has_recv_errors;
    uint32_t recv_errors;
    /* A room server's. */
    bool has_posts;
    uint16_t posted;
    uint16_t post_pushes;
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

/*
 * A repeater's answer to REQ_GET_NEIGHBOURS: the nodes it hears straight off the air, as many as
 * it had room for, each named by as many bytes of its key as the request asked for (four here -
 * the roster's number for a node), with how long ago it was last heard and the SNR of that.
 * `total` is how many the repeater holds, which can be more than `count`: its answer is one
 * packet, and a request asks for a page.
 */
#define MESH_MESHCORE_NEIGHBOURS_MAX 10U
#define MESH_MESHCORE_NEIGHBOUR_PREFIX_LEN 4U

struct mesh_meshcore_neighbour {
    uint8_t prefix[MESH_MESHCORE_NEIGHBOUR_PREFIX_LEN];
    uint32_t heard_secs_ago;
    int8_t snr_q4; /* SNR in quarter decibels */
};

struct mesh_meshcore_neighbours {
    uint16_t total;
    uint8_t count;
    struct mesh_meshcore_neighbour entries[MESH_MESHCORE_NEIGHBOURS_MAX];
};

/*
 * A TRACE_DATA: the tag the trace was sent with, how many bytes name each hop (1, 2, 4 or 8 -
 * the width the trace asked for), the hops as it was sent along them, and one SNR per hop - what
 * that hop heard the trace at - then the SNR this radio heard it back at, so `hops` + 1 readings.
 */
struct mesh_meshcore_trace {
    uint32_t tag;
    uint8_t hash_size;
    uint8_t hops;
    uint8_t hashes[MESH_MESHCORE_MAX_FRAME];
    int8_t snr_q4[MESH_MESHCORE_MAX_FRAME]; /* quarter decibels */
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
/* SEND_TXT_MSG: `txt_type` is enum mesh_meshcore_txt_type - PLAIN for a message, CLI_DATA for a
   command to a repeater, which the radio sends with no ack to wait for. */
int mesh_meshcore_encode_text(const uint8_t prefix[MESH_MESHCORE_PREFIX_LEN], uint8_t txt_type,
                              uint8_t attempt, uint32_t timestamp, const char *text, uint8_t *out,
                              size_t out_len);
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
/* SET_CHANNEL: the slot, the name in a 32-byte field the firmware keeps NUL-terminated - so at
   most 31 bytes of it, and a longer one is refused - and the 16-byte secret. The firmware has no
   256-bit channel yet and refuses the longer frame that would carry one. */
int mesh_meshcore_encode_set_channel(uint8_t index, const char *name,
                                     const uint8_t secret[MESH_MESHCORE_SECRET_LEN], uint8_t *out,
                                     size_t out_len);
/* ADD_UPDATE_CONTACT: the contact record as RESP_CONTACT carries it - key, type, flags, path
   length and the 64-byte path, the name in 32 bytes, the advert's timestamp, the position. */
int mesh_meshcore_encode_contact(const struct mesh_meshcore_contact *contact, uint8_t *out,
                                 size_t out_len);
/*
 * Cayenne LPP, as MeshCore's CayenneLPP library writes it: channel, type, a big-endian value
 * whose size the type fixes. Reads to the end, to a channel 0 (the library's end-of-data), or
 * to the first type it does not know the size of - everything before that is kept. 0, or
 * -EINVAL for a NULL argument.
 */
int mesh_meshcore_decode_lpp(const uint8_t *lpp, size_t len, struct mesh_meshcore_telemetry *out);
/*
 * A STATUS_RESPONSE's stats, from after the push's key prefix: the shared 48 bytes, then the
 * tail `adv_type` says the node's kind writes (enum mesh_meshcore_adv_type), as much of it as
 * arrived. 0, or -EBADMSG for fewer than MESH_MESHCORE_STATUS_LEN bytes.
 */
int mesh_meshcore_decode_status(const uint8_t *stats, size_t len, uint8_t adv_type,
                                struct mesh_meshcore_status *out);
/*
 * SEND_BINARY_REQ for a repeater's neighbours: its whole key, then REQ_GET_NEIGHBOURS asking for
 * the first `count` of them newest first, each named by MESH_MESHCORE_NEIGHBOUR_PREFIX_LEN bytes
 * of its key. `nonce` fills the four bytes the firmware leaves for making each request a
 * different packet, so a repeated one is not taken for a replay.
 */
int mesh_meshcore_encode_neighbours_req(const uint8_t key[MESH_MESHCORE_PUBKEY_LEN], uint8_t count,
                                        uint32_t nonce, uint8_t *out, size_t out_len);
/*
 * A neighbours answer, from after a BINARY_RESPONSE's tag: the total, the count and the entries.
 * Keeps at most MESH_MESHCORE_NEIGHBOURS_MAX; 0, or -EBADMSG for an answer shorter than the
 * entries it counts.
 */
int mesh_meshcore_decode_neighbours(const uint8_t *data, size_t len,
                                    struct mesh_meshcore_neighbours *out);
/*
 * SEND_TRACE_PATH along `hop_count` hops of `hash_size` bytes each (1, 2, 4 or 8, the only widths
 * the flags can say), under `tag` and no auth code. -EINVAL for another width or no hops, and
 * -ENOSPC for a path the frame - or the firmware's 64 hops - cannot carry.
 */
int mesh_meshcore_encode_trace(uint32_t tag, uint8_t hash_size, const uint8_t *hops,
                               uint8_t hop_count, uint8_t *out, size_t out_len);
/*
 * A whole TRACE_DATA push. 0, or -EBADMSG for a frame shorter than the hops and readings it
 * counts, or one whose byte count is not whole hops.
 */
int mesh_meshcore_decode_trace(const uint8_t *frame, size_t len, struct mesh_meshcore_trace *out);
/* REBOOT carries the word, so a stray byte cannot reboot a radio. */
int mesh_meshcore_encode_reboot(uint8_t *out, size_t out_len);

/* EXPORT_PRIVATE_KEY is the code alone; IMPORT_PRIVATE_KEY the code and the 64 bytes. The
   length written, or -EINVAL / -ENOSPC. */
int mesh_meshcore_encode_export_private_key(uint8_t *out, size_t out_len);
int mesh_meshcore_encode_import_private_key(const uint8_t key[MESH_MESHCORE_PRVKEY_LEN],
                                            uint8_t *out, size_t out_len);
/* RESP_PRIVATE_KEY into `out`. 0, or -EBADMSG for another frame or one too short. */
int mesh_meshcore_decode_private_key(const uint8_t *frame, size_t len,
                                     uint8_t out[MESH_MESHCORE_PRVKEY_LEN]);

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
/*
 * How many of the radio's contact records are kept whole: every one a radio can have. DEVICE_INFO
 * reports its limit as one byte of half the count, so no companion firmware can say more than
 * 510, and the book is sized past that rather than to what today's builds happen to keep. A
 * record that still arrives with the book full is counted in `contacts_unkept`, and a backup is
 * refused rather than written short.
 */
#define MESH_MESHCORE_CONTACTS_MAX 512U
/* Channel slots kept whole: as many as the session shows, which the walk never goes past. */
#define MESH_MESHCORE_CHANNELS_KEPT 8U
#define MESH_MESHCORE_PENDING_SENDS 8U
#define MESH_MESHCORE_HEARD_ADVERTS 16U
/* A command the radio has not answered in this long is given up on, and two in a row is a link
   whose far end has gone. */
#define MESH_MESHCORE_REPLY_TIMEOUT_MS 10000U
/* How long a command that went unanswered keeps its place in line after it is marked failed. A
   repeater's replies carry nothing naming the command, only the order they come in, so a late
   reply to it must still find it first - or it would settle the next command in its stead. */
#define MESH_MESHCORE_COMMAND_LATE_MS 60000U
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
    /* A GET_CONTACT_BY_KEY asked for a favourite: what to write back when the record arrives,
       one of enum mesh_meshcore_favorite_intent. Kept with the lookup, so two pins in flight
       are two lookups that each know their own answer. */
    uint8_t favorite;
    /* An ADD_UPDATE_CONTACT for a node the roster had never heard - one from a link, not on
       the roster when it was asked - so its OK does not make it heard. */
    bool never_heard;
    /* An ADD_UPDATE_CONTACT written back from a backup: its answer, whatever it is, settles
       `contact_restore_outstanding`. */
    bool restore;
};

enum mesh_meshcore_favorite_intent {
    MESH_MESHCORE_FAVORITE_NONE = 0,
    MESH_MESHCORE_FAVORITE_SET = 1,
    MESH_MESHCORE_FAVORITE_CLEAR = 2,
};

/* One direct message waiting for its ack. */
struct mesh_meshcore_pending {
    uint32_t packet_id; /* 0 for a free slot */
    uint32_t expected_ack;
    uint64_t deadline_ms; /* 0 while the SENT reply is still to come */
    uint32_t timestamp;
    uint8_t attempt;
    /* A command to a repeater rather than a message: sent once and never acked, it is answered
       by the repeater's reply instead, and is not tried again - a retry of "reboot" would be a
       second reboot. */
    bool command;
    /* When it was sent, as a count: a repeater's replies come back in the order its commands
       went, and the packet id is a xorshift, not a clock. */
    uint32_t sequence;
    /* Whole, not a prefix: the last attempt resets the route, which names the contact by key. */
    uint8_t key[MESH_MESHCORE_PUBKEY_LEN];
    char text[MESH_MESHCORE_TEXT_MAX + 1U];
};

/* How a request to another node ended, for whoever has to say so. */
enum mesh_meshcore_answer {
    MESH_MESHCORE_ANSWER_READINGS = 1, /* its readings arrived */
    MESH_MESHCORE_ANSWER_GUEST,        /* logged in, without admin rights */
    MESH_MESHCORE_ANSWER_ADMIN,        /* logged in as its admin */
    MESH_MESHCORE_ANSWER_REFUSED,      /* the password was not one it takes */
    MESH_MESHCORE_ANSWER_SILENT,       /* no answer by the radio's deadline */
    MESH_MESHCORE_ANSWER_UNSENT,       /* the radio would not send it */
    MESH_MESHCORE_ANSWER_STATUS,       /* its status arrived */
    MESH_MESHCORE_ANSWER_ROUTE,        /* the routes to it and back arrived */
    MESH_MESHCORE_ANSWER_NEIGHBOURS,   /* the nodes it hears arrived */
};

/*
 * A command given up on, still in line for its reply: marked failed and said so, its send slot
 * already free for the next message. Commands waiting and commands here are never more than
 * MESH_MESHCORE_PENDING_SENDS together - a new one is refused first - so none is dropped. A reply
 * from its repeater before `until_ms` is its answer, not the next command's. Kept apart from the
 * send slots so that a repeater gone quiet cannot hold every one of them - and every direct message
 * behind them - for the length of the wait.
 */
struct mesh_meshcore_late_command {
    uint32_t packet_id; /* 0 for a free entry */
    uint32_t sequence;
    uint64_t until_ms;
    uint8_t prefix[MESH_MESHCORE_PREFIX_LEN];
};

/* How many of the notices below are kept for the publish to read: several commands can time
   out in one tick, and each is worth saying. */
#define MESH_MESHCORE_NOTICES_KEPT 8U

struct mesh_meshcore_notice {
    uint32_t node_id;
    /* The request: a login, a status, readings, a path discovery or a neighbours request - or
       SEND_TXT_MSG for a command to a repeater that got no reply. */
    uint8_t cmd;
    uint8_t answer; /* enum mesh_meshcore_answer */
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
    /*
     * The radio's contact list and channel slots, record for record, as it last said them.
     *
     * The roster is a projection of these - a name, a position, a hop count - and drops what a
     * screen does not draw: the route the radio learned, the advert stamp, the flags above the
     * favourite, a channel's full 31-byte name. A backup needs exactly those, because on a
     * MeshCore companion the contact list is state the radio *keeps* and a reset loses. So the
     * records are kept here as they arrive (RESP_CONTACT, and an add or update the radio
     * accepted) and dropped as they leave (a removal it accepted, CONTACT_DELETED), and a fresh
     * connection starts them again, as it starts `contacts_since`.
     */
    struct mesh_meshcore_contact contacts[MESH_MESHCORE_CONTACTS_MAX];
    size_t contact_count;
    uint32_t contacts_unkept; /* records that arrived with the book full */
    bool has_channel[MESH_MESHCORE_CHANNELS_KEPT];
    struct mesh_meshcore_channel channels[MESH_MESHCORE_CHANNELS_KEPT];
    /* The contact list's newest lastmod, so a refresh asks only for what changed. */
    uint32_t contacts_since;
    /* The newest adverts from nodes the radio did not add, as they arrived: adding one sends
       the sender's own stamp and name back, which the roster does not keep (its last_heard is
       the radio's clock, and a stamp ahead of the sender's would refuse its next advert). */
    struct mesh_meshcore_contact heard[MESH_MESHCORE_HEARD_ADVERTS];
    uint32_t heard_age[MESH_MESHCORE_HEARD_ADVERTS]; /* when each was kept; 0 for an empty slot */
    uint32_t heard_clock;
    /* A request to another node - a login, its status or its readings - is answered until this
       monotonic time, 0 for none outstanding: the radio keeps one, and any new one orphans it. */
    uint64_t request_until_ms;
    uint8_t request_cmd;                              /* which of them */
    uint32_t request_node;                            /* asked of whom */
    uint8_t request_prefix[MESH_MESHCORE_PREFIX_LEN]; /* whose answer frees it */
    /* A binary request's answer names no node, only the tag its SENT carried: 0 until then.
       A trace's names none either, and carries the tag it was sent with, set when it is asked. */
    uint32_t request_tag;
    /* How the last of them ended, and a count that moves each time one does. `notice_log`
       holds the last MESH_MESHCORE_NOTICES_KEPT, notice n at n % that - `notice` is the newest. */
    uint32_t notices;
    struct mesh_meshcore_notice notice;
    struct mesh_meshcore_notice notice_log[MESH_MESHCORE_NOTICES_KEPT];
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
    uint32_t pending_sequence; /* the last `sequence` handed out */
    struct mesh_meshcore_late_command late[MESH_MESHCORE_PENDING_SENDS];
    uint32_t next_packet_id;

    /* A settings save in flight: how many of its commands are still unanswered, and the first
       refusal among those that were. Settled into the model's write counters once the last
       one is answered. */
    uint8_t writes_outstanding;
    int32_t write_error;
    /* A contact written back from a backup (mesh_meshcore_restore_contact()) and not yet
       answered, and how many such writes the radio has refused, left unanswered or lost with
       the link since this conversation was set up. A count rather than a flag, so a restore
       reads what its own writes did as the difference from where it started. */
    bool contact_restore_outstanding;
    uint32_t contact_restores_refused;
    /* The link's answer to the last frame it refused outright, for a caller that must say so. */
    int send_error;
    /*
     * The radio's private key asked out, or one sent in (mesh_meshcore_export_identity(),
     * mesh_meshcore_import_identity()): where that stands, enum mesh_meshcore_identity_state,
     * the radio's error code when it refused, and - once an export has arrived - the key, until
     * mesh_meshcore_take_identity() takes it and wipes it. One at a time.
     */
    uint8_t identity_state;
    uint8_t identity_error;
    /* An import taken whose restart the link would not send (IMPORTED only): the radio holds
       the new key and will not restart on its own. */
    bool identity_restart_unsent;
    uint8_t identity_key[MESH_MESHCORE_PRVKEY_LEN];
};

enum mesh_meshcore_identity_state {
    MESH_MESHCORE_IDENTITY_IDLE = 0,
    MESH_MESHCORE_IDENTITY_ASKED,    /* sent, and not yet answered */
    MESH_MESHCORE_IDENTITY_EXPORTED, /* the key has arrived, in `identity_key` */
    /* The radio took the key sent, and the conversation has sent the restart that resyncs it -
       or says in `identity_restart_unsent` that it could not. */
    MESH_MESHCORE_IDENTITY_IMPORTED,
    MESH_MESHCORE_IDENTITY_DISABLED, /* the firmware was built without the command */
    MESH_MESHCORE_IDENTITY_REFUSED,  /* the radio refused it: `identity_error` says why */
    MESH_MESHCORE_IDENTITY_LOST,     /* never sent, unanswered, or the link went first */
    /* An import sent and never answered - timed out, or the link went while it was out. The radio
       may hold the key or not; the link is called silent, and the sync after the reconnect says
       which. */
    MESH_MESHCORE_IDENTITY_UNKNOWN,
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
    /* SET_OTHER_PARAMS, written whole: the four bytes SELF_INFO reports them as. */
    bool set_other;
    uint8_t manual_add_contacts;
    uint8_t telemetry_modes;
    uint8_t advert_loc_policy;
    uint8_t multi_acks;
    /* The Bluetooth PIN DEVICE_INFO reports: 0 or 100000..999999, as the firmware takes it. */
    bool set_pin;
    uint32_t ble_pin;
    /* One channel slot, whole: an empty name and an all-zero secret is an unused slot. */
    bool set_channel;
    uint8_t channel_index;
    char channel_name[MESH_MESHCORE_NAME_LEN];
    uint8_t channel_secret[MESH_MESHCORE_SECRET_LEN];
};

/* `model` is the session the conversation fills; it must outlive every link that carries this. */
void mesh_meshcore_init(struct mesh_meshcore *meshcore, struct mesh_session *model);
/* This conversation as a link carries it: MeshCore's framing, the Nordic UART profile. */
struct mesh_protocol mesh_meshcore_protocol(struct mesh_meshcore *meshcore);
bool mesh_meshcore_ready(const struct mesh_meshcore *meshcore);

/* The radio's contact records as last synced, whole; see `contacts` above. */
size_t mesh_meshcore_contact_count(const struct mesh_meshcore *meshcore);
const struct mesh_meshcore_contact *mesh_meshcore_contact_at(const struct mesh_meshcore *meshcore,
                                                             size_t index);

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
 * -ENOTCONN until the handshake has named the radio, -ENOENT for a node whose key the roster does
 * not hold, -EMSGSIZE for text past mesh_meshcore_text_max(), -ENOBUFS when the command queue is
 * full, and -EBUSY for a command to a repeater while MESH_MESHCORE_PENDING_SENDS commands are
 * still in line for their replies.
 */
int mesh_meshcore_send_text(struct mesh_meshcore *meshcore, uint32_t dest, uint8_t channel,
                            const char *text, uint32_t *out_packet_id);
/* Announces this radio: flooded across the mesh, or to the nodes in earshot only. */
int mesh_meshcore_send_advert(struct mesh_meshcore *meshcore, bool flood);
/*
 * Asks the radio to take a contact off its list; the roster drops it once the radio says OK,
 * and keeps it if the radio refuses or never answers. It returns on its own once the node next
 * adverts, if the radio adds contacts by itself. 1 when asked; -EINVAL for 0 or this radio,
 * -ENOTCONN until the handshake has named the radio, -ENOENT for a node that is not one of the
 * radio's contacts, -ENOBUFS when the command queue is full.
 */
int mesh_meshcore_remove_contact(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Asks the radio to forget the route it holds for a contact, so the next message floods and the
 * answer teaches it a fresh one - the way out of a route through a repeater that has gone. The
 * roster reads the contact as having no route once the radio says OK. 1 when asked; -EINVAL for
 * 0 or this radio, -ENOTCONN until the handshake has named the radio, -ENOENT for a node that is
 * not one of the radio's contacts, -EALREADY for one with no route to forget, -ENOBUFS when the
 * command queue is full.
 */
int mesh_meshcore_reset_path(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Asks the radio to make a heard node a contact, from what the roster holds of its advert: the
 * key, the name, the kind of node and where it said it was, with no route known yet so the
 * first message floods. The node joins the radio's list when the radio says OK. 1 when asked;
 * -EINVAL for 0 or this radio, -ENOTCONN until the handshake has named the radio, -ENOENT for a
 * node with no whole key, -EEXIST for one that is already a contact, -EADDRINUSE while an add
 * for a different key under the same four bytes waits for its OK, -ENOBUFS when the queue is
 * full.
 */
int mesh_meshcore_add_contact(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Marks a contact the radio's favourite, or not: bit 0 of its flags, which the radio's own
 * apps pin and which an auto-add that finds the list full will not overwrite. The radio's
 * record is read back first and written whole with only that bit changed; the roster's flag
 * follows the radio's OK. 1 when asked, 0 when the flag is already so; -EINVAL for 0 or this
 * radio, -ENOTCONN until the handshake has named the radio, -ENOENT for a node that is not one
 * of the radio's contacts, -ENOBUFS when the command queue is full.
 */
int mesh_meshcore_set_favorite(struct mesh_meshcore *meshcore, uint32_t node_id, bool favorite);
/*
 * Asks a contact for its readings now: SEND_TELEMETRY_REQ, answered - if the node lets this
 * radio ask - by a TELEMETRY_RESPONSE whose battery, environment and position land on the
 * node's record. The radio keeps one request outstanding - this, a status or a login - and a
 * new one orphans the last; how it ended is `notice`. 0 when asked; -EINVAL for 0 or this radio,
 * -ENOTCONN until the handshake has named the radio, -ENOENT for a node that is not one of the
 * radio's contacts, -EBUSY while the last request's answer is still due, -ENOBUFS when the
 * queue is full.
 */
int mesh_meshcore_request_telemetry(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Logs in to a repeater or room server among the radio's contacts: SEND_LOGIN with `password`,
 * blank for a guest. The node answers LOGIN_SUCCESS, with whether it took us as its admin, or
 * LOGIN_FAIL, and `notice` says which - or that it said nothing. The same one request as
 * mesh_meshcore_request_telemetry(), and the same returns; -EINVAL too for a password longer
 * than MESH_MESHCORE_PASSWORD_MAX.
 */
int mesh_meshcore_login(struct mesh_meshcore *meshcore, uint32_t node_id, const char *password);
/*
 * Asks a repeater or room server among the radio's contacts for its status: SEND_STATUS_REQ,
 * answered by a STATUS_RESPONSE whose counters land on the node's record (its `relay`, and its
 * battery and uptime). A node answers only a client on its access list, so one not logged in to
 * first says nothing. The same one request as mesh_meshcore_request_telemetry(), and the same
 * returns.
 */
int mesh_meshcore_request_status(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Finds the routes to a contact and back: SEND_PATH_DISCOVERY_REQ, flooded, answered by a
 * PATH_DISCOVERY_RESPONSE naming the repeaters each way by a prefix of their keys. They land in
 * the model's traceroute, where Meshtastic's trace does, as the path out and the path back;
 * silence, or a radio that would not send it, is a trace that timed out. The same one request as
 * mesh_meshcore_request_telemetry(), and the same returns.
 */
int mesh_meshcore_discover_path(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Traces the route the radio already keeps to a repeater or room server, and back: SEND_TRACE_PATH
 * along its path out, the node itself, then the same hops reversed. Every node on the way adds
 * the SNR it heard the trace at, and the TRACE_DATA that comes back puts one on every link each
 * way into the model's traceroute - which a path discovery cannot. Only a node that forwards
 * can be a stop, so a companion cannot be traced to. The same one request as
 * mesh_meshcore_request_telemetry(), and the same returns; -EINVAL too for a node that is not a
 * repeater or room server, and -EAGAIN for one the radio has no route to yet - or one too long
 * to go out and back in a trace's 64 hops - which a path discovery measures instead.
 */
int mesh_meshcore_trace_path(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Asks a repeater among the radio's contacts which nodes it hears: SEND_BINARY_REQ with
 * REQ_GET_NEIGHBOURS, answered by a BINARY_RESPONSE whose list lands on the node's `neighbors` -
 * the record Meshtastic's NeighborInfo fills - so the node's screen, and every other node's
 * "heard by", read it unchanged. Like a status, a repeater answers only a client on its access
 * list. The same one request as mesh_meshcore_request_telemetry(), and the same returns.
 */
int mesh_meshcore_request_neighbours(struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Whether a direct message to `node_id` goes as a command: the node is a repeater, which takes
 * text only from its admin and only as a command to run. mesh_meshcore_send_text() sends it so,
 * with no retries, and the repeater's reply lands in the conversation as its answer.
 */
bool mesh_meshcore_is_command_peer(const struct mesh_meshcore *meshcore, uint32_t node_id);
/*
 * Asks the radio to make a contact of a node known only from a link: its whole key, its name and
 * the kind of node it is (enum mesh_meshcore_adv_type), with no route and no advert stamp, so the
 * first message floods and the node's next advert is taken. The node joins the roster, a
 * contact, when the radio says OK. 1 when asked; -EINVAL for a missing key, a kind outside
 * 1-4 or this radio's own key, -ENOTCONN until the handshake has named the radio, -EEXIST for a
 * node that is already a contact - a link carries no route, and writing it over a contact's
 * record would drop the one the radio has learned - -EADDRINUSE for a key whose first four bytes,
 * the roster's number for a node, are already this radio's or another node's, and -ENOBUFS
 * when the queue is full.
 */
int mesh_meshcore_import_contact(struct mesh_meshcore *meshcore,
                                 const uint8_t key[MESH_MESHCORE_PUBKEY_LEN], const char *name,
                                 uint8_t adv_type);

/*
 * Hands the radio a signed contact card - an advert packet, `len` bytes, whose key is `key` - with
 * IMPORT_CONTACT. The radio checks it as an advert heard on the air and treats it as one: the
 * node joins or refreshes the roster through the advert push that follows, if its signature
 * holds. 1 when asked; -EINVAL for a card too short to carry a key and a signature, too long for
 * a frame, or this radio's own; -ENOTCONN until the handshake has named the radio;
 * -EADDRINUSE for a key whose first four bytes are already this radio's or another node's;
 * -ENOBUFS when the queue is full.
 */
int mesh_meshcore_import_card(struct mesh_meshcore *meshcore, const uint8_t *packet, size_t len,
                              const uint8_t key[MESH_MESHCORE_PUBKEY_LEN]);
/*
 * Writes a contact back from a backup: ADD_UPDATE_CONTACT with the record as given - its key,
 * type, flags, route, name, advert stamp and position - over whatever the radio holds under that
 * key, or as a new contact. It joins the book and the roster when the radio says OK, as any add
 * does, and not as heard: it came off a card, not the air.
 *
 * **One at a time.** A restore can be hundreds of contacts and the command queue is sixteen,
 * shared with messages and every verb a screen has; filling it would refuse the user's own
 * presses for as long as the restore took. So a second is refused until the radio has answered
 * the first, and the caller sends the next once `contact_restore_outstanding` is false again. A
 * refusal, a command that went unanswered, or one the link took with it adds one to
 * `contact_restores_refused`.
 *
 * 1 when asked; -EINVAL for a missing contact or this radio's own key, -ENOTCONN until the
 * handshake has named the radio, -EBUSY while the last one is unanswered, -ENOBUFS when the
 * queue is full.
 */
int mesh_meshcore_restore_contact(struct mesh_meshcore *meshcore,
                                  const struct mesh_meshcore_contact *contact);
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
/*
 * Joins a channel from a link: writes `name` and `secret` into the first slot the sync read as
 * unused, through mesh_meshcore_write_settings(), and says which through `out_slot`. The same
 * returns as that call, and -EINVAL for an empty name or one past 31 bytes, -ENOTCONN until the
 * handshake is done, -EEXIST - with its slot - when a slot already holds that name and secret,
 * -ENOSPC when every slot read is in use.
 */
int mesh_meshcore_import_channel(struct mesh_meshcore *meshcore, const char *name,
                                 const uint8_t secret[MESH_MESHCORE_SECRET_LEN], uint8_t *out_slot);
/*
 * The firmware's own bounds on the radio parameters and the transmit power, which a save checks
 * before it sends anything: a value outside them is answered with ILLEGAL_ARG, and a refusal
 * said about the value is more use than an error code back from the radio. The power's ceiling
 * is the one this radio reported (SELF_INFO's max_tx_power_dbm).
 */
bool mesh_meshcore_radio_params_valid(uint32_t frequency_khz, uint32_t bandwidth_hz,
                                      uint8_t spreading_factor, uint8_t coding_rate);
bool mesh_meshcore_tx_power_valid(const struct mesh_meshcore *meshcore, int32_t dbm);
/* Asks for SELF_INFO again, which re-projects the settings. 1 when asked, 0 when already
   asked, -ENOTCONN without a link. */
int mesh_meshcore_refresh_settings(struct mesh_meshcore *meshcore);
/* Reboots the radio. The link drops without an answer and auto-connect brings it back. */
int mesh_meshcore_reboot(struct mesh_meshcore *meshcore);

/*
 * Asks the radio for its private key, or gives it one, for a backup that carries the radio's
 * identity and a restore of it. The answer lands in `identity_state`. 1 when sent, -EBUSY while
 * the last one is unsettled (ASKED, or an EXPORTED key not yet taken), -EAGAIN before the radio
 * has finished syncing, -ENOTCONN without a link, or the queue's -ENOBUFS.
 *
 * The import's frame carries the key, and is wiped off the queue once answered - as a login's
 * is. What the radio does with it is take it at once: the conversation's own idea of the radio
 * is then out of date, so the caller restarts the radio, and the sync that follows reads it as
 * the node the key makes it.
 */
int mesh_meshcore_export_identity(struct mesh_meshcore *meshcore);
int mesh_meshcore_import_identity(struct mesh_meshcore *meshcore,
                                  const uint8_t key[MESH_MESHCORE_PRVKEY_LEN]);
/* An exported key into `out`, wiped from the conversation, and the state back to IDLE. False
   when none has arrived. */
bool mesh_meshcore_take_identity(struct mesh_meshcore *meshcore,
                                 uint8_t out[MESH_MESHCORE_PRVKEY_LEN]);
/* A settled answer - anything but ASKED and an untaken key - forgotten, back to IDLE. */
void mesh_meshcore_identity_clear(struct mesh_meshcore *meshcore);

#ifdef __cplusplus
}
#endif
