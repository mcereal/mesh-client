#pragma once

/*
 * The conversation with the ESP32 OTA loader, once it is connected: phase 4 of
 * docs/radio-firmware-roadmap.md, and the one piece of it that is a protocol.
 *
 * `meshtastic/esp32-unified-ota` runs a text protocol over two characteristics of its own
 * service - everything we say goes to `...0005`, every answer comes back as a notification on
 * `...0003` - and it is stop-and-wait:
 *
 *   VERSION\n                 -> OK <hw> <fw> <reboot_count> v<loader version>\n
 *   OTA <size> <sha256-hex>\n -> ERASING\n, then OK\n once the partition is erased
 *   <= one write of image     -> ACK\n per write, except the last, which gets OK\n
 *
 * Three things about it are load-bearing, and all three are in this file rather than in its
 * caller because each is a property of the loader:
 *
 * - **One chunk is one GATT write, and one write is one ACK.** The loader ACKs per call to its
 *   processor, and a chunk BlueZ turns into a long write arrives as several - so a chunk is
 *   `mtu - 3` bytes at most, the most a single Write Request carries, and the MTU is read off
 *   the link rather than assumed.
 * - **One chunk outstanding at a time.** The loader drops what does not fit its 4 KB buffer,
 *   silently as far as the wire is concerned, and ACKs per processor call rather than per write
 *   - so two writes in its buffer at once are two chunks and one ACK, and the cadence is gone.
 * - **Erasing takes seconds.** `esp_ota_begin()` erases the whole partition before it answers.
 *
 * On success the loader has checked the image against the SHA-256 the radio stored when it was
 * sent here, switched the boot partition and will restart two seconds later. On `ERR Hash
 * Mismatch` it has deliberately corrupted what it wrote and stays in the loader.
 *
 * It sits on the bluez client and not on mesh_session: a loader is not a Meshtastic node, speaks
 * no protobuf and has no node number.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_bluez_client;

#define MESH_BLE_OTA_SERVICE_UUID "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define MESH_BLE_OTA_WRITE_UUID "62EC0272-3EC5-11EB-B378-0242AC130005"
#define MESH_BLE_OTA_NOTIFY_UUID "62EC0272-3EC5-11EB-B378-0242AC130003"

/* The loader's own ceiling, and the default ATT MTU's payload for a link that reports none. */
#define MESH_BLE_OTA_CHUNK_MAX 512U
#define MESH_BLE_OTA_CHUNK_MIN 20U

#define MESH_BLE_OTA_TEXT_MAX 96U
#define MESH_BLE_OTA_PATH_MAX 160U
#define MESH_BLE_OTA_EVENTS 8U

enum mesh_ble_ota_state {
    MESH_BLE_OTA_IDLE = 0,
    MESH_BLE_OTA_VERSION,   /* VERSION sent: is this the loader that answers? */
    MESH_BLE_OTA_STARTING,  /* OTA <size> <hash> sent */
    MESH_BLE_OTA_ERASING,   /* the loader said ERASING; the next OK is the partition empty */
    MESH_BLE_OTA_SENDING,   /* a chunk out, an ACK due */
    MESH_BLE_OTA_FINISHING, /* the last chunk out, the loader hashing it */
    MESH_BLE_OTA_DONE,
    MESH_BLE_OTA_FAILED,
    MESH_BLE_OTA_STATE_COUNT,
};

enum mesh_ble_ota_error {
    MESH_BLE_OTA_ERROR_NONE = 0,
    /* BlueZ refused a write: the link is going or gone. `write_error` says how. */
    MESH_BLE_OTA_ERROR_WRITE,
    /* An answer that did not come. */
    MESH_BLE_OTA_ERROR_TIMEOUT,
    /* Nothing answered VERSION. The unified loader always does; the old `bleota` loader sits
       in its own state machine and says nothing - though the firmware refuses to boot into that
       one, so the ordinary cause is a link that is not really up. */
    MESH_BLE_OTA_ERROR_SILENT,
    /* The loader said ERR, in its own words (`reason`) - a hash that does not match the one
       the radio stored, a partition it could not begin. Sending again does not change either. */
    MESH_BLE_OTA_ERROR_REFUSED,
    /* The loader took every byte and the hash did not match. It has corrupted the partition on
       purpose and is still waiting, so the recovery is the same image again. */
    MESH_BLE_OTA_ERROR_HASH_MISMATCH,
    /* An answer out of turn: an ACK with nothing outstanding, an OK mid-stream. */
    MESH_BLE_OTA_ERROR_PROTOCOL,
    MESH_BLE_OTA_ERROR_COUNT,
};

enum mesh_ble_ota_event_kind {
    MESH_BLE_OTA_EVENT_OK = 0,
    MESH_BLE_OTA_EVENT_ACK,
    MESH_BLE_OTA_EVENT_ERASING,
    MESH_BLE_OTA_EVENT_ERR,
};

struct mesh_ble_ota_event {
    enum mesh_ble_ota_event_kind kind;
    char text[MESH_BLE_OTA_TEXT_MAX]; /* what followed OK or ERR */
};

struct mesh_ble_ota {
    struct mesh_bluez_client *client; /* borrowed */
    char write_path[MESH_BLE_OTA_PATH_MAX];
    char notify_path[MESH_BLE_OTA_PATH_MAX];
    uint16_t mtu; /* 0 when BlueZ reported none */
    size_t chunk;

    enum mesh_ble_ota_state state;
    enum mesh_ble_ota_error error;
    int write_error;

    /* Borrowed for the life of the transfer. */
    const uint8_t *image;
    size_t image_len;
    uint8_t sha256[32];
    size_t acked;     /* bytes the loader has taken */
    size_t in_flight; /* the chunk written and not answered yet */

    /* The write in progress. A new one is only issued once this one's reply is in: the client
       has one write slot, and polling it with new bytes would be reading the old reply. */
    const uint8_t *write_data;
    size_t write_len;
    bool write_done;
    char command[112];

    /* Notifications are only recorded where they arrive - inside the client's dispatch - and
       acted on from the tick, so a write is never issued from inside a D-Bus callback. */
    char line[MESH_BLE_OTA_TEXT_MAX];
    size_t line_len;
    bool line_overflow;
    struct mesh_ble_ota_event events[MESH_BLE_OTA_EVENTS];
    size_t event_head;
    size_t event_count;
    bool events_overflow;

    char loader_version[MESH_BLE_OTA_TEXT_MAX]; /* what followed VERSION's OK */
    char reason[MESH_BLE_OTA_TEXT_MAX];         /* the loader's own ERR line */

    uint64_t deadline_ms;
    uint64_t sending_since_ms;
    uint64_t finished_ms;
};

/* The chunk for an MTU: `mtu - 3`, the payload of one Write Request, capped at the loader's 512
   and floored at the default MTU's 20 when BlueZ reported none. */
size_t mesh_ble_ota_chunk_for_mtu(uint16_t mtu);

/*
 * Finds the loader's two characteristics under `device_path`, reads the link's MTU, subscribes
 * to the notifications and takes the client's notification handler. Returns 0 or -errno.
 */
int mesh_ble_ota_attach(struct mesh_ble_ota *ota, struct mesh_bluez_client *client,
                        const char *device_path);

/*
 * Starts the conversation: VERSION, then the OTA command, then the stream. `image` is borrowed
 * until the transfer ends; `sha256` is the digest of exactly those bytes, which the loader
 * compares against the one the radio stored. Returns 0, or -EINVAL for an empty image or a
 * conversation that was never attached.
 */
int mesh_ble_ota_begin(struct mesh_ble_ota *ota, const uint8_t *image, size_t image_len,
                       const uint8_t sha256[32], uint64_t now_ms);

/* Bytes of a notification from `...0003`. Lines are assembled across notifications. */
void mesh_ble_ota_feed(struct mesh_ble_ota *ota, const uint8_t *data, size_t len);

void mesh_ble_ota_tick(struct mesh_ble_ota *ota, uint64_t now_ms);

/* Gives the client's notification handler back and forgets any write in flight. */
void mesh_ble_ota_detach(struct mesh_ble_ota *ota);

bool mesh_ble_ota_busy(const struct mesh_ble_ota *ota);
unsigned mesh_ble_ota_progress(const struct mesh_ble_ota *ota);
/* Over the stream only - erasing is not throughput - and 0 before the first chunk. */
uint32_t mesh_ble_ota_bytes_per_second(const struct mesh_ble_ota *ota, uint64_t now_ms);

const char *mesh_ble_ota_state_name(enum mesh_ble_ota_state state);
const char *mesh_ble_ota_error_name(enum mesh_ble_ota_error error);

#ifdef __cplusplus
}
#endif
