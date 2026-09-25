#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/ble_dfu.h"

#include "inkwell/base/env.h"
#include "inkwell/base/log.h"
#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/timer.h"

#include <errno.h>
#include <string.h>

/*
 * The bootloader erases the whole application bank before it answers START, and the SoftDevice
 * time-slices every page of that against the radio: 30-50 s on a healthy link, measured by the
 * Android app against a stock Adafruit bootloader. 90 s is that with margin.
 */
#define DFU_START_TIMEOUT_MS 90000U
#define DFU_COMMAND_TIMEOUT_MS 30000U
/* A receipt is ten packets of flash writes behind; thirty seconds without one is a stall. */
#define DFU_RECEIPT_TIMEOUT_MS 30000U
/* "All received" and VALIDATE are both a CRC over the whole bank. */
#define DFU_VALIDATE_TIMEOUT_MS 60000U
/* ACTIVATE is answered by a reset rather than by a notification, so this is only how long the
   write is given before the reset is assumed. */
#define DFU_ACTIVATE_TIMEOUT_MS 10000U
/* Writes a single tick may complete before it hands the loop back. On BlueZ every write is a
   D-Bus round trip, so this only matters when replies are already waiting. */
#define DFU_WRITES_PER_TICK 32U

#define DFU_ATT_MTU_MIN 23U

enum {
    DFU_OP_START = 0x01,
    DFU_OP_INIT = 0x02,
    DFU_OP_RECEIVE = 0x03,
    DFU_OP_VALIDATE = 0x04,
    DFU_OP_ACTIVATE = 0x05,
    DFU_OP_PRN = 0x08,
    DFU_OP_RESPONSE = 0x10,
    DFU_OP_RECEIPT = 0x11,
    DFU_IMAGE_APPLICATION = 0x04,
    DFU_INIT_BEGIN = 0x00,
    DFU_INIT_COMPLETE = 0x01,
};

static const char *const k_state_names[MESH_BLE_DFU_STATE_COUNT] = {
    "idle", "starting", "init", "sending", "received", "validating", "activating", "done", "failed",
};

static const char *const k_error_names[MESH_BLE_DFU_ERROR_COUNT] = {
    "none", "write", "timeout", "stale session", "refused", "receipt mismatch", "protocol",
};

const char *mesh_ble_dfu_state_name(enum mesh_ble_dfu_state state) {
    return state < MESH_BLE_DFU_STATE_COUNT ? k_state_names[state] : "?";
}

const char *mesh_ble_dfu_error_name(enum mesh_ble_dfu_error error) {
    return error < MESH_BLE_DFU_ERROR_COUNT ? k_error_names[error] : "?";
}

const char *mesh_ble_dfu_status_name(uint8_t status) {
    switch (status) {
    case MESH_BLE_DFU_STATUS_SUCCESS:
        return "success";
    case MESH_BLE_DFU_STATUS_INVALID_STATE:
        return "invalid state";
    case MESH_BLE_DFU_STATUS_NOT_SUPPORTED:
        return "not supported";
    case MESH_BLE_DFU_STATUS_DATA_SIZE:
        return "data size exceeds limit";
    case MESH_BLE_DFU_STATUS_CRC_ERROR:
        return "CRC error";
    case MESH_BLE_DFU_STATUS_OPERATION_FAILED:
        return "operation failed";
    default:
        return "unknown status";
    }
}

size_t mesh_ble_dfu_packet_for_mtu(uint16_t mtu) {
    if (mtu < DFU_ATT_MTU_MIN) {
        return MESH_BLE_DFU_PACKET_MIN;
    }
    /* One Write Command carries `mtu - 3`; a longer one is not split, it is refused. And the
       bootloader answers a data write that is not whole words with NOT_SUPPORTED. */
    size_t payload = (size_t)mtu - 3U;
    if (payload > MESH_BLE_DFU_PACKET_MAX) {
        payload = MESH_BLE_DFU_PACKET_MAX;
    }
    payload -= payload % 4U;
    return payload < MESH_BLE_DFU_PACKET_MIN ? MESH_BLE_DFU_PACKET_MIN : payload;
}

bool mesh_ble_dfu_busy(const struct mesh_ble_dfu *dfu) {
    return dfu != NULL && dfu->state != MESH_BLE_DFU_IDLE && dfu->state != MESH_BLE_DFU_DONE &&
           dfu->state != MESH_BLE_DFU_FAILED;
}

static void dfu_fail(struct mesh_ble_dfu *dfu, enum mesh_ble_dfu_error error, uint64_t now_ms) {
    const enum mesh_ble_dfu_state was = dfu->state;
    dfu->state = MESH_BLE_DFU_FAILED;
    dfu->error = error;
    dfu->finished_ms = now_ms;
    dfu->outbox_count = 0U;
    inkwell_log_error(
        "ble_dfu", "The bootloader conversation failed while %s: %s%s%s, at %zu of %zu bytes",
        mesh_ble_dfu_state_name(was), mesh_ble_dfu_error_name(error), dfu->status != 0U ? ": " : "",
        dfu->status != 0U ? mesh_ble_dfu_status_name(dfu->status) : "", dfu->confirmed,
        dfu->image_len);
}

static void dfu_done(struct mesh_ble_dfu *dfu, uint64_t now_ms) {
    dfu->state = MESH_BLE_DFU_DONE;
    dfu->finished_ms = now_ms;
    dfu->outbox_count = 0U;
    inkwell_log_info("ble_dfu", "The bootloader validated all %zu bytes and is activating them",
                     dfu->image_len);
}

/* ---- what the bootloader says ----------------------------------------------------------------
 */

void mesh_ble_dfu_feed(struct mesh_ble_dfu *dfu, const uint8_t *data, size_t len) {
    if (dfu == NULL || data == NULL || len == 0U) {
        return;
    }
    if (dfu->event_count >= MESH_BLE_DFU_EVENTS) {
        /* Eight answers unread. A receipt every ten packets and one question at a time never
           comes near it, so this is a bootloader talking out of turn; the tick reports it. */
        dfu->events_overflow = true;
        return;
    }
    struct mesh_ble_dfu_event *const event =
        &dfu->events[(dfu->event_head + dfu->event_count) % MESH_BLE_DFU_EVENTS];
    memset(event, 0, sizeof *event);
    if (data[0] == DFU_OP_RESPONSE && len >= 3U) {
        event->kind = MESH_BLE_DFU_EVENT_RESPONSE;
        event->opcode = data[1];
        event->status = data[2];
    } else if (data[0] == DFU_OP_RECEIPT && len >= 5U) {
        event->kind = MESH_BLE_DFU_EVENT_RECEIPT;
        event->bytes = (uint32_t)data[1] | (uint32_t)data[2] << 8 | (uint32_t)data[3] << 16 |
                       (uint32_t)data[4] << 24;
    } else {
        event->kind = MESH_BLE_DFU_EVENT_UNKNOWN;
        event->opcode = data[0];
    }
    dfu->event_count += 1U;
}

/*
 * The conversation runs on the stack's own clock, not the app's.
 *
 * Driven only from the caller's tick, a Write Command cost one turn of the foreground loop - a
 * 20 ms bound and a frame, because the progress bar keeps the loop from going idle - and the
 * image went at 612 B/s: a T1000-E took fourteen minutes. So whenever a write's reply or a
 * notification arrives, the conversation is ticked right there and the next write goes out.
 *
 * Safe because inkwell's BlueZ backend pops its messages itself rather than dispatching through
 * libdbus, so a send from inside a reply's handling cannot re-enter anything, and a tick issues
 * writes and changes this struct's state and nothing else. `in_tick` keeps it from nesting.
 */
static void dfu_kick(struct mesh_ble_dfu *dfu) {
    if (dfu != NULL && !dfu->in_tick && mesh_ble_dfu_busy(dfu)) {
        mesh_ble_dfu_tick(dfu, inkwell_time_monotonic_ms());
    }
}

static int dfu_pace_fired(int fd, uint32_t events, void *userdata) {
    (void)events;
    (void)inkwell_timer_read(fd);
    struct mesh_ble_dfu *const dfu = (struct mesh_ble_dfu *)userdata;
    dfu->pace_credit = true;
    dfu_kick(dfu);
    return 0;
}

static void dfu_pace(struct mesh_ble_dfu *dfu, bool on) {
    if (dfu->pace_fd >= 0) {
        (void)(on ? inkwell_timer_arm_every(dfu->pace_fd, dfu->pace_ms)
                  : inkwell_timer_disarm(dfu->pace_fd));
    }
}

static void dfu_notification(const uint8_t *data, size_t len, void *userdata) {
    struct mesh_ble_dfu *const dfu = (struct mesh_ble_dfu *)userdata;
    mesh_ble_dfu_feed(dfu, data, len);
    dfu_kick(dfu);
}

static void dfu_requests_ready(void *userdata) { dfu_kick((struct mesh_ble_dfu *)userdata); }

/* ---- what we say -----------------------------------------------------------------------------
 */

static void dfu_queue(struct mesh_ble_dfu *dfu, bool packet, const uint8_t *data, size_t len) {
    if (dfu->outbox_count >= MESH_BLE_DFU_OUTBOX || len > MESH_BLE_DFU_PACKET_MIN) {
        /* Sized for the longest sequence this file queues, so reaching here is a bug here. */
        inkwell_log_error("ble_dfu", "Dropped a %zu-byte write: the outbox is full", len);
        return;
    }
    struct mesh_ble_dfu_write *const write =
        &dfu->outbox[(dfu->outbox_head + dfu->outbox_count) % MESH_BLE_DFU_OUTBOX];
    write->packet = packet;
    memcpy(write->data, data, len);
    write->len = (uint8_t)len;
    dfu->outbox_count += 1U;
}

static void dfu_control(struct mesh_ble_dfu *dfu, uint8_t a, uint8_t b, uint8_t c, size_t len) {
    const uint8_t bytes[3] = {a, b, c};
    dfu_queue(dfu, false, bytes, len);
}

static void dfu_issue(struct mesh_ble_dfu *dfu, const char *handle, const uint8_t *data, size_t len,
                      bool outbox) {
    dfu->write_handle = handle;
    dfu->write_data = data;
    dfu->write_len = len;
    dfu->write_done = false;
    dfu->writing_outbox = outbox;
}

/* The next write, if there is one to make and the slot is free. False when there is none. */
static bool dfu_issue_next(struct mesh_ble_dfu *dfu) {
    if (dfu->write_data != NULL && !dfu->write_done) {
        return false;
    }
    if (dfu->outbox_count > 0U) {
        const struct mesh_ble_dfu_write *const write = &dfu->outbox[dfu->outbox_head];
        dfu_issue(dfu, write->packet ? dfu->packet_handle : dfu->control_handle, write->data,
                  write->len, true);
        return true;
    }
    /*
     * Image packets go one per pace credit and never on the stack's answers. BlueZ answers a
     * Write Command as soon as it has queued it rather than when it has gone, so writing the
     * next one from that answer is a burst - and a stock Adafruit bootloader answered the first
     * ten of them with OPERATION_FAILED, which is Nordic's "data sent too fast" (T1000-E,
     * 2026-09-25). Control writes and the init packet still go at once.
     */
    if (dfu->state == MESH_BLE_DFU_SENDING && dfu->sent < dfu->image_len &&
        dfu->since_receipt < MESH_BLE_DFU_PRN && dfu->pace_credit) {
        dfu->pace_credit = false;
        const size_t left = dfu->image_len - dfu->sent;
        dfu_issue(dfu, dfu->packet_handle, dfu->image + dfu->sent,
                  left < dfu->packet ? left : dfu->packet, false);
        return true;
    }
    return false;
}

/* What a finished write means. Only image packets move anything along. */
static void dfu_wrote(struct mesh_ble_dfu *dfu, uint64_t now_ms) {
    if (dfu->writing_outbox) {
        dfu->outbox_head = (dfu->outbox_head + 1U) % MESH_BLE_DFU_OUTBOX;
        dfu->outbox_count -= 1U;
        if (dfu->state == MESH_BLE_DFU_ACTIVATING && dfu->outbox_count == 0U) {
            dfu_done(dfu, now_ms);
        }
        return;
    }
    dfu->sent += dfu->write_len;
    dfu->since_receipt += 1U;
    dfu->deadline_ms = now_ms + DFU_RECEIPT_TIMEOUT_MS;
    if (dfu->sent >= dfu->image_len) {
        dfu->state = MESH_BLE_DFU_RECEIVED;
        dfu->deadline_ms = now_ms + DFU_VALIDATE_TIMEOUT_MS;
    }
}

/* Drives the write in flight. False once the conversation has failed. */
static bool dfu_pump(struct mesh_ble_dfu *dfu, uint64_t now_ms) {
    if (dfu->write_data == NULL || dfu->write_done) {
        return true;
    }
    /* The same call issues the write and, while its reply is pending, polls it. */
    const int result =
        inkwell_ble_write(dfu->client, dfu->write_handle, dfu->write_data, dfu->write_len);
    if (result == -EAGAIN) {
        return true;
    }
    dfu->write_done = true;
    if (result == 0) {
        dfu_wrote(dfu, now_ms);
        return true;
    }
    /* ACTIVATE is answered by the bootloader resetting, which can beat the write's own reply:
       failing to be acknowledged is not a failure. */
    if (dfu->state == MESH_BLE_DFU_ACTIVATING) {
        dfu_done(dfu, now_ms);
        return false;
    }
    if (!mesh_ble_dfu_busy(dfu)) {
        dfu->outbox_count = 0U;
        return false;
    }
    dfu->write_error = result;
    inkwell_log_warn("ble_dfu", "A %zu-byte write was refused: %s", dfu->write_len,
                     strerror(-result));
    dfu_fail(dfu, MESH_BLE_DFU_ERROR_WRITE, now_ms);
    return false;
}

static void dfu_queue_init(struct mesh_ble_dfu *dfu) {
    dfu_control(dfu, DFU_OP_INIT, DFU_INIT_BEGIN, 0U, 2U);
    for (size_t at = 0U; at < dfu->init_len; at += MESH_BLE_DFU_PACKET_MIN) {
        const size_t left = dfu->init_len - at;
        dfu_queue(dfu, true, dfu->init + at,
                  left < MESH_BLE_DFU_PACKET_MIN ? left : MESH_BLE_DFU_PACKET_MIN);
    }
    dfu_control(dfu, DFU_OP_INIT, DFU_INIT_COMPLETE, 0U, 2U);
}

static void dfu_protocol(struct mesh_ble_dfu *dfu, const struct mesh_ble_dfu_event *event,
                         uint64_t now_ms) {
    inkwell_log_warn("ble_dfu", "Out of turn while %s: kind %d, opcode %#x, status %u, bytes %u",
                     mesh_ble_dfu_state_name(dfu->state), (int)event->kind, (unsigned)event->opcode,
                     (unsigned)event->status, (unsigned)event->bytes);
    dfu_fail(dfu, MESH_BLE_DFU_ERROR_PROTOCOL, now_ms);
}

static void dfu_handle_receipt(struct mesh_ble_dfu *dfu, const struct mesh_ble_dfu_event *event,
                               uint64_t now_ms) {
    /* A receipt after the last packet is the bootloader's count running on while "all
       received" is on its way; it has nothing left to pace. */
    if (dfu->state != MESH_BLE_DFU_SENDING) {
        return;
    }
    if (event->bytes > dfu->sent || event->bytes < dfu->confirmed) {
        dfu_fail(dfu, MESH_BLE_DFU_ERROR_RECEIPT, now_ms);
        return;
    }
    dfu->confirmed = event->bytes;
    if (dfu->since_receipt < MESH_BLE_DFU_PRN) {
        return;
    }
    /* The window is full and this is its receipt: it has to count exactly what went out, or a
       packet was lost - and a Write Command lost is lost silently. */
    if (event->bytes != dfu->sent) {
        dfu_fail(dfu, MESH_BLE_DFU_ERROR_RECEIPT, now_ms);
        return;
    }
    dfu->since_receipt = 0U;
    dfu->deadline_ms = now_ms + DFU_RECEIPT_TIMEOUT_MS;
    const unsigned decile = mesh_ble_dfu_progress(dfu) / 10U;
    if (decile > dfu->logged_decile) {
        dfu->logged_decile = decile;
        inkwell_log_info("ble_dfu", "%u%% at %u B/s", decile * 10U,
                         (unsigned)mesh_ble_dfu_bytes_per_second(dfu, now_ms));
    }
}

static void dfu_handle_response(struct mesh_ble_dfu *dfu, const struct mesh_ble_dfu_event *event,
                                uint64_t now_ms) {
    if (event->status != MESH_BLE_DFU_STATUS_SUCCESS) {
        dfu->status = event->status;
        dfu_fail(dfu,
                 event->opcode == DFU_OP_START && event->status == MESH_BLE_DFU_STATUS_INVALID_STATE
                     ? MESH_BLE_DFU_ERROR_STALE
                     : MESH_BLE_DFU_ERROR_REFUSED,
                 now_ms);
        return;
    }
    if (dfu->state == MESH_BLE_DFU_STARTING && event->opcode == DFU_OP_START) {
        inkwell_log_info("ble_dfu", "The bank is erased; sending the %zu-byte init packet",
                         dfu->init_len);
        dfu_queue_init(dfu);
        dfu->state = MESH_BLE_DFU_INIT;
        dfu->deadline_ms = now_ms + DFU_COMMAND_TIMEOUT_MS;
    } else if (dfu->state == MESH_BLE_DFU_INIT && event->opcode == DFU_OP_INIT) {
        inkwell_log_info("ble_dfu", "Init packet accepted; sending %zu bytes in %zu-byte packets",
                         dfu->image_len, dfu->packet);
        dfu_control(dfu, DFU_OP_PRN, (uint8_t)(MESH_BLE_DFU_PRN & 0xFFU),
                    (uint8_t)(MESH_BLE_DFU_PRN >> 8), 3U);
        dfu_control(dfu, DFU_OP_RECEIVE, 0U, 0U, 1U);
        dfu->state = MESH_BLE_DFU_SENDING;
        dfu->pace_credit = false;
        dfu_pace(dfu, true);
        dfu->since_receipt = 0U;
        dfu->sending_since_ms = now_ms;
        dfu->deadline_ms = now_ms + DFU_RECEIPT_TIMEOUT_MS;
    } else if (dfu->state == MESH_BLE_DFU_RECEIVED && event->opcode == DFU_OP_RECEIVE) {
        dfu->confirmed = dfu->image_len;
        dfu_control(dfu, DFU_OP_VALIDATE, 0U, 0U, 1U);
        dfu->state = MESH_BLE_DFU_VALIDATING;
        dfu->deadline_ms = now_ms + DFU_VALIDATE_TIMEOUT_MS;
    } else if (dfu->state == MESH_BLE_DFU_VALIDATING && event->opcode == DFU_OP_VALIDATE) {
        dfu_control(dfu, DFU_OP_ACTIVATE, 0U, 0U, 1U);
        dfu->state = MESH_BLE_DFU_ACTIVATING;
        dfu->deadline_ms = now_ms + DFU_ACTIVATE_TIMEOUT_MS;
    } else {
        dfu_protocol(dfu, event, now_ms);
    }
}

/* ---- the public half -------------------------------------------------------------------------
 */

static int dfu_finish_attach(struct mesh_ble_dfu *dfu) {
    const int result = inkwell_ble_subscribe(dfu->client, dfu->control_handle);
    if (result == -EAGAIN) {
        return -EAGAIN;
    }
    dfu->attaching = false;
    if (result < 0) {
        inkwell_ble_set_notification_handler(dfu->client, NULL, NULL);
        inkwell_log_warn("ble_dfu", "Could not subscribe to the control point: %d", result);
        return result;
    }
    inkwell_log_info("ble_dfu", "Bootloader attached: MTU %u, %zu-byte packets every %u ms",
                     (unsigned)dfu->mtu, dfu->packet, dfu->pace_ms);
    return 0;
}

int mesh_ble_dfu_attach(struct mesh_ble_dfu *dfu, struct inkwell_ble_central *client,
                        const char *address) {
    if (dfu == NULL || client == NULL || address == NULL) {
        return -EINVAL;
    }
    if (dfu->attaching && dfu->client == client) {
        return dfu_finish_attach(dfu);
    }
    mesh_ble_dfu_detach(dfu);
    memset(dfu, 0, sizeof *dfu);
    dfu->client = client;
    dfu->pace_fd = -1;
    dfu->pace_ms = (unsigned)inkwell_env_int("DFU_PACKET_GAP_MS", 1, 1000, MESH_BLE_DFU_PACE_MS);
    if (client->loop != NULL) {
        const int fd = inkwell_timer_open();
        if (fd >= 0 &&
            inkwell_loop_add_fd(client->loop, fd, INKWELL_LOOP_IN, dfu_pace_fired, dfu) == 0) {
            dfu->loop = client->loop;
            dfu->pace_fd = fd;
        } else if (fd >= 0) {
            inkwell_timer_close(fd);
        }
    }

    int result = inkwell_ble_find_characteristic(client, address, MESH_BLE_DFU_CONTROL_UUID,
                                                 dfu->control_handle, sizeof dfu->control_handle);
    if (result < 0) {
        inkwell_log_warn("ble_dfu", "No DFU control point on %s: %d", address, result);
        return result;
    }
    result = inkwell_ble_find_characteristic(client, address, MESH_BLE_DFU_PACKET_UUID,
                                             dfu->packet_handle, sizeof dfu->packet_handle);
    if (result < 0) {
        inkwell_log_warn("ble_dfu", "No DFU packet characteristic on %s: %d", address, result);
        return result;
    }

    uint16_t mtu = 0U;
    result = inkwell_ble_characteristic_mtu(client, dfu->packet_handle, &mtu);
    dfu->mtu = result == 0 ? mtu : 0U;
    dfu->packet = mesh_ble_dfu_packet_for_mtu(dfu->mtu);

    inkwell_ble_set_notification_handler(client, dfu_notification, dfu);
    /* The install's own central, whose wake-ups nothing else is listening for. */
    client->requests_ready = dfu_requests_ready;
    client->userdata = dfu;
    dfu->attaching = true;
    return dfu_finish_attach(dfu);
}

int mesh_ble_dfu_begin(struct mesh_ble_dfu *dfu, const uint8_t *init, size_t init_len,
                       const uint8_t *image, size_t image_len, uint64_t now_ms) {
    if (dfu == NULL || dfu->client == NULL || dfu->control_handle[0] == '\0' || init == NULL ||
        init_len == 0U || image == NULL || image_len == 0U || image_len > UINT32_MAX ||
        dfu->packet == 0U) {
        return -EINVAL;
    }
    /* The whole init packet goes into the outbox between its two brackets. */
    if ((init_len + MESH_BLE_DFU_PACKET_MIN - 1U) / MESH_BLE_DFU_PACKET_MIN + 2U >
        MESH_BLE_DFU_OUTBOX) {
        return -EINVAL;
    }
    if (mesh_ble_dfu_busy(dfu)) {
        return -EBUSY;
    }
    dfu->init = init;
    dfu->init_len = init_len;
    dfu->image = image;
    dfu->image_len = image_len;
    dfu->sent = 0U;
    dfu->confirmed = 0U;
    dfu->since_receipt = 0U;
    dfu->logged_decile = 0U;
    dfu->error = MESH_BLE_DFU_ERROR_NONE;
    dfu->write_error = 0;
    dfu->status = 0U;
    dfu->event_head = 0U;
    dfu->event_count = 0U;
    dfu->events_overflow = false;
    dfu->sending_since_ms = 0U;
    dfu->finished_ms = 0U;

    dfu_control(dfu, DFU_OP_START, DFU_IMAGE_APPLICATION, 0U, 2U);
    const uint32_t len = (uint32_t)image_len;
    const uint8_t sizes[12] = {
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        (uint8_t)(len & 0xFFU),
        (uint8_t)(len >> 8 & 0xFFU),
        (uint8_t)(len >> 16 & 0xFFU),
        (uint8_t)(len >> 24),
    };
    dfu_queue(dfu, true, sizes, sizeof sizes);
    dfu->state = MESH_BLE_DFU_STARTING;
    dfu->deadline_ms = now_ms + DFU_START_TIMEOUT_MS;
    inkwell_log_info("ble_dfu", "Starting a %zu-byte application; the bootloader erases first",
                     image_len);
    mesh_ble_dfu_tick(dfu, now_ms);
    return dfu->state == MESH_BLE_DFU_FAILED ? dfu->write_error : 0;
}

static void dfu_tick(struct mesh_ble_dfu *dfu, uint64_t now_ms);

void mesh_ble_dfu_tick(struct mesh_ble_dfu *dfu, uint64_t now_ms) {
    if (dfu == NULL || dfu->client == NULL || dfu->in_tick) {
        return;
    }
    dfu->in_tick = true;
    /* With no timer of its own, the caller's tick is the pace. */
    if (dfu->pace_fd < 0) {
        dfu->pace_credit = true;
    }
    dfu_tick(dfu, now_ms);
    if (!mesh_ble_dfu_busy(dfu) || dfu->state != MESH_BLE_DFU_SENDING) {
        dfu_pace(dfu, false);
    }
    dfu->in_tick = false;
}

/* Every write that can go now: the one in flight polled, then the next, until one is waiting on
   the stack or there is nothing left that may be sent. False once the conversation has ended. */
static bool dfu_drain(struct mesh_ble_dfu *dfu, uint64_t now_ms) {
    for (unsigned i = 0U; i < DFU_WRITES_PER_TICK; ++i) {
        if (!dfu_pump(dfu, now_ms)) {
            return false;
        }
        if (dfu->write_data != NULL && !dfu->write_done) {
            break;
        }
        if (!dfu_issue_next(dfu)) {
            break;
        }
    }
    return true;
}

static void dfu_tick(struct mesh_ble_dfu *dfu, uint64_t now_ms) {
    if (!dfu_drain(dfu, now_ms)) {
        return;
    }
    if (!mesh_ble_dfu_busy(dfu)) {
        return;
    }
    if (dfu->events_overflow) {
        dfu_fail(dfu, MESH_BLE_DFU_ERROR_PROTOCOL, now_ms);
        return;
    }
    while (dfu->event_count > 0U && mesh_ble_dfu_busy(dfu)) {
        const struct mesh_ble_dfu_event event = dfu->events[dfu->event_head];
        dfu->event_head = (dfu->event_head + 1U) % MESH_BLE_DFU_EVENTS;
        dfu->event_count -= 1U;
        if (event.kind == MESH_BLE_DFU_EVENT_RECEIPT) {
            dfu_handle_receipt(dfu, &event, now_ms);
        } else if (event.kind == MESH_BLE_DFU_EVENT_RESPONSE) {
            dfu_handle_response(dfu, &event, now_ms);
        } else {
            inkwell_log_debug("ble_dfu", "Ignoring a notification starting %#x",
                              (unsigned)event.opcode);
        }
    }
    /* Whatever the answers queued - the init packet, the next window - goes now rather than on
       the next reply. */
    if (mesh_ble_dfu_busy(dfu) && !dfu_drain(dfu, now_ms)) {
        return;
    }
    if (!mesh_ble_dfu_busy(dfu) || now_ms < dfu->deadline_ms) {
        return;
    }
    if (dfu->state == MESH_BLE_DFU_ACTIVATING) {
        dfu_done(dfu, now_ms);
        return;
    }
    dfu_fail(dfu, MESH_BLE_DFU_ERROR_TIMEOUT, now_ms);
}

void mesh_ble_dfu_detach(struct mesh_ble_dfu *dfu) {
    if (dfu == NULL || dfu->client == NULL) {
        return;
    }
    inkwell_ble_set_notification_handler(dfu->client, NULL, NULL);
    if (dfu->client->requests_ready == dfu_requests_ready) {
        dfu->client->requests_ready = NULL;
        dfu->client->userdata = NULL;
    }
    inkwell_ble_requests_cancel(dfu->client);
    if (dfu->pace_fd >= 0) {
        (void)inkwell_loop_remove_fd(dfu->loop, dfu->pace_fd);
        inkwell_timer_close(dfu->pace_fd);
        dfu->pace_fd = -1;
        dfu->loop = NULL;
    }
    dfu->write_data = NULL;
    dfu->outbox_count = 0U;
    dfu->attaching = false;
}

unsigned mesh_ble_dfu_progress(const struct mesh_ble_dfu *dfu) {
    if (dfu == NULL || dfu->image_len == 0U) {
        return 0U;
    }
    if (dfu->state == MESH_BLE_DFU_DONE) {
        return 100U;
    }
    return (unsigned)((uint64_t)dfu->confirmed * 100U / (uint64_t)dfu->image_len);
}

uint32_t mesh_ble_dfu_bytes_per_second(const struct mesh_ble_dfu *dfu, uint64_t now_ms) {
    if (dfu == NULL || dfu->sending_since_ms == 0U || dfu->sent == 0U) {
        return 0U;
    }
    const uint64_t end = dfu->finished_ms != 0U ? dfu->finished_ms : now_ms;
    if (end <= dfu->sending_since_ms) {
        return 0U;
    }
    return (uint32_t)((uint64_t)dfu->sent * 1000U / (end - dfu->sending_since_ms));
}
