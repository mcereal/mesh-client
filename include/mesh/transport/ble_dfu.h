#pragma once

/*
 * The conversation with an nRF52's bootloader, once it is connected - Nordic's **Legacy** DFU,
 * which is what the Adafruit bootloader every Meshtastic nRF52 board ships with speaks (SDK 11,
 * and the OTAFIX fork of it). The ESP32's counterpart is mesh/transport/ble_ota.h.
 *
 * Two characteristics of service `1530`: a control point (`1531`) that takes opcodes as Write
 * Requests and answers them as notifications, and a packet characteristic (`1532`) that takes
 * data as Write Commands and answers nothing. In order:
 *
 *   START_DFU [01 04]               control: an application image, nothing else
 *   sizes (12 bytes)                packet: SoftDevice 0, bootloader 0, application `len`
 *                                   <- [10 01 01] once the application bank is erased
 *   INIT [02 00], init, INIT [02 01]  the init packet between two control writes
 *                                   <- [10 02 01]
 *   PRN [08 n n]                    a receipt every n packets
 *   RECEIVE [03]
 *   image, one packet at a time     <- [11 bytes-received] every n packets
 *                                   <- [10 03 01] after the last
 *   VALIDATE [04]                   <- [10 04 01]: the CRC16 matched the init packet's
 *   ACTIVATE [05]                   the bootloader resets into the new image
 *
 * What is load-bearing, and why each is here rather than in the caller:
 *
 * - **Packets are whole words.** The bootloader refuses a data write whose length is not a
 *   multiple of four, so a packet is `mtu - 3` floored to four, capped at 244 - and 20 on a link
 *   that negotiated nothing.
 * - **The erase is the long wait.** The bootloader is single-bank and erases the whole
 *   application region before it answers START, with the SoftDevice time-slicing every page
 *   against the radio - tens of seconds of silence on a healthy link.
 * - **Receipts are the flow control.** Write Commands are not acknowledged, and the SDK 11
 *   flash path drops what arrives faster than it can write; ten packets between receipts is the
 *   ceiling Nordic's own library holds legacy bootloaders to.
 * - **START answered INVALID_STATE is a stale session**, left by a transfer that broke. It is
 *   not the image's fault. RESET clears it, but a stock bootloader with no application comes
 *   back from a reset in USB DFU only - so nothing here sends one.
 *
 * It sits on inkwell's BLE central and not on mesh_session: a bootloader speaks no protobuf.
 */

#include "inkwell/ble/central.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_BLE_DFU_SERVICE_UUID "00001530-1212-EFDE-1523-785FEABCD123"
#define MESH_BLE_DFU_CONTROL_UUID "00001531-1212-EFDE-1523-785FEABCD123"
#define MESH_BLE_DFU_PACKET_UUID "00001532-1212-EFDE-1523-785FEABCD123"
#define MESH_BLE_DFU_VERSION_UUID "00001534-1212-EFDE-1523-785FEABCD123"

#define MESH_BLE_DFU_PACKET_MIN 20U
#define MESH_BLE_DFU_PACKET_MAX 244U
/*
 * The gap between image packets, in ms. BlueZ answers a Write Command as soon as it has queued
 * it, so without a gap ten packets go in one burst - which a stock Adafruit bootloader refused
 * with OPERATION_FAILED. A phone's stack paces them at about one per connection event and
 * finishes a T1000-E in two or three minutes; 10 ms is a little gentler than that, because a
 * transfer that breaks on a stock bootloader leaves it wanting USB. MESHCLIENT_DFU_PACKET_GAP_MS
 * overrides it.
 */
#define MESH_BLE_DFU_PACE_MS 10U
/* Packets between receipts. Nordic caps legacy bootloaders at ten. */
#define MESH_BLE_DFU_PRN 10U
#define MESH_BLE_DFU_EVENTS 8U
/* Control writes and init-packet chunks queued at once: START and the sizes, or the init
   packet's two brackets and its chunks, or PRN and RECEIVE. */
#define MESH_BLE_DFU_OUTBOX 8U

/* The value of `1534` that means the application's own DFU service rather than a bootloader's:
   the buttonless trigger lives there. A bootloader says 5 or more. */
#define MESH_BLE_DFU_VERSION_APPLICATION 1U

enum mesh_ble_dfu_state {
    MESH_BLE_DFU_IDLE = 0,
    MESH_BLE_DFU_STARTING,   /* START and the sizes out; the bank erasing */
    MESH_BLE_DFU_INIT,       /* the init packet out, its answer due */
    MESH_BLE_DFU_SENDING,    /* the image, a receipt every MESH_BLE_DFU_PRN packets */
    MESH_BLE_DFU_RECEIVED,   /* the last packet out; "all received" due */
    MESH_BLE_DFU_VALIDATING, /* VALIDATE out; the CRC16 check due */
    MESH_BLE_DFU_ACTIVATING, /* ACTIVATE out; the bootloader resetting under it */
    MESH_BLE_DFU_DONE,
    MESH_BLE_DFU_FAILED,
    MESH_BLE_DFU_STATE_COUNT,
};

enum mesh_ble_dfu_error {
    MESH_BLE_DFU_ERROR_NONE = 0,
    /* BlueZ refused a write: the link is going or gone. `write_error` says how. */
    MESH_BLE_DFU_ERROR_WRITE,
    /* An answer that did not come. */
    MESH_BLE_DFU_ERROR_TIMEOUT,
    /* START answered INVALID_STATE: a transfer that broke is still holding the bootloader. */
    MESH_BLE_DFU_ERROR_STALE,
    /* The bootloader answered a step with a status other than success (`status`). */
    MESH_BLE_DFU_ERROR_REFUSED,
    /* A receipt counting bytes other than the ones sent. */
    MESH_BLE_DFU_ERROR_RECEIPT,
    /* An answer out of turn. */
    MESH_BLE_DFU_ERROR_PROTOCOL,
    MESH_BLE_DFU_ERROR_COUNT,
};

/* The bootloader's status byte, the third of a response. */
enum mesh_ble_dfu_status {
    MESH_BLE_DFU_STATUS_SUCCESS = 1,
    MESH_BLE_DFU_STATUS_INVALID_STATE = 2,
    MESH_BLE_DFU_STATUS_NOT_SUPPORTED = 3,
    MESH_BLE_DFU_STATUS_DATA_SIZE = 4,
    MESH_BLE_DFU_STATUS_CRC_ERROR = 5,
    MESH_BLE_DFU_STATUS_OPERATION_FAILED = 6,
};

enum mesh_ble_dfu_event_kind {
    MESH_BLE_DFU_EVENT_RESPONSE = 0, /* [10 op status] */
    MESH_BLE_DFU_EVENT_RECEIPT,      /* [11 u32] */
    MESH_BLE_DFU_EVENT_UNKNOWN,
};

struct mesh_ble_dfu_event {
    enum mesh_ble_dfu_event_kind kind;
    uint8_t opcode;
    uint8_t status;
    uint32_t bytes;
};

struct mesh_ble_dfu_write {
    bool packet; /* the packet characteristic; otherwise the control point */
    uint8_t data[MESH_BLE_DFU_PACKET_MIN];
    uint8_t len;
};

struct inkwell_loop;

struct mesh_ble_dfu {
    struct inkwell_ble_central *client; /* borrowed */
    char control_handle[INKWELL_BLE_HANDLE_MAX];
    char packet_handle[INKWELL_BLE_HANDLE_MAX];
    uint16_t mtu; /* 0 when BlueZ reported none */
    size_t packet;
    bool attaching;
    /* A tick is running; one arriving from the stack's callbacks inside it is dropped. */
    bool in_tick;
    /* The image is paced rather than sent as fast as the stack answers: one packet per
       `pace_ms`, on a timer of its own when the client is on a loop, else one per tick. */
    struct inkwell_loop *loop;
    int pace_fd;
    unsigned pace_ms;
    bool pace_credit; /* one image packet may go */

    enum mesh_ble_dfu_state state;
    enum mesh_ble_dfu_error error;
    int write_error;
    uint8_t status; /* the refusing status, on REFUSED and STALE */

    /* Borrowed for the life of the transfer. */
    const uint8_t *init;
    size_t init_len;
    const uint8_t *image;
    size_t image_len;
    size_t sent;      /* image bytes written */
    size_t confirmed; /* image bytes the bootloader has counted in a receipt */
    unsigned since_receipt;
    unsigned logged_decile; /* the last tenth of the image the log reported */

    /* Everything but the image goes through here, in order, one write at a time. */
    struct mesh_ble_dfu_write outbox[MESH_BLE_DFU_OUTBOX];
    size_t outbox_head;
    size_t outbox_count;

    /* The write in progress, until its reply is in: the client has one write slot. */
    const char *write_handle;
    const uint8_t *write_data;
    size_t write_len;
    bool write_done;
    /* The outbox entry being written, so it leaves the queue only once it has gone. */
    bool writing_outbox;

    /* Recorded where they arrive - inside the client's dispatch - and acted on from the tick. */
    struct mesh_ble_dfu_event events[MESH_BLE_DFU_EVENTS];
    size_t event_head;
    size_t event_count;
    bool events_overflow;

    uint64_t deadline_ms;
    uint64_t sending_since_ms;
    uint64_t finished_ms;
};

/* The packet for an MTU: `mtu - 3` floored to a whole word, capped at 244, and 20 when BlueZ
   reported none. */
size_t mesh_ble_dfu_packet_for_mtu(uint16_t mtu);

/*
 * Finds the control point and the packet characteristic on `address`, reads the MTU, subscribes
 * to the control point and takes the client's notification handler and its `requests_ready`,
 * through which the conversation moves on as soon as the stack answers rather than on the next
 * tick - so `client` has to be one nothing else is using. 0 or -errno, and -EAGAIN
 * while the subscribe is waiting on the stack: call again with the same arguments.
 */
int mesh_ble_dfu_attach(struct mesh_ble_dfu *dfu, struct inkwell_ble_central *client,
                        const char *address);

/* Starts the transfer. `init` and `image` are borrowed until it ends. 0 or -EINVAL. */
int mesh_ble_dfu_begin(struct mesh_ble_dfu *dfu, const uint8_t *init, size_t init_len,
                       const uint8_t *image, size_t image_len, uint64_t now_ms);

/* Bytes of one control-point notification. */
void mesh_ble_dfu_feed(struct mesh_ble_dfu *dfu, const uint8_t *data, size_t len);

void mesh_ble_dfu_tick(struct mesh_ble_dfu *dfu, uint64_t now_ms);

/* Lets go of the notifications and any write in flight. The link is the caller's. */
void mesh_ble_dfu_detach(struct mesh_ble_dfu *dfu);

bool mesh_ble_dfu_busy(const struct mesh_ble_dfu *dfu);
unsigned mesh_ble_dfu_progress(const struct mesh_ble_dfu *dfu);
uint32_t mesh_ble_dfu_bytes_per_second(const struct mesh_ble_dfu *dfu, uint64_t now_ms);

const char *mesh_ble_dfu_state_name(enum mesh_ble_dfu_state state);
const char *mesh_ble_dfu_error_name(enum mesh_ble_dfu_error error);
const char *mesh_ble_dfu_status_name(uint8_t status);

#ifdef __cplusplus
}
#endif
