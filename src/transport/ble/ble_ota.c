#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/ble_ota.h"

#include "mesh/transport/ble_bluez.h"
#include "mesh/utils/log.h"
#include "mesh/utils/sha256.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The phone app allows 10 s for an ACK and 60 s for the erase, and both are the loader's
   numbers rather than ours: an erase of a 3 MB partition is seconds of flash time, and an ACK
   is one write and one notification. VERSION gets the ACK's allowance, and the last chunk a
   little more, because the loader hashes the partition before it answers. */
#define OTA_VERSION_TIMEOUT_MS 10000U
#define OTA_START_TIMEOUT_MS 10000U
#define OTA_ERASE_TIMEOUT_MS 60000U
#define OTA_ACK_TIMEOUT_MS 10000U
#define OTA_FINISH_TIMEOUT_MS 30000U

/* The smallest MTU ATT allows. A smaller number is BlueZ reporting none. */
#define OTA_ATT_MTU_MIN 23U

static const char *const k_state_names[MESH_BLE_OTA_STATE_COUNT] = {
    "idle", "version", "starting", "erasing", "sending", "finishing", "done", "failed",
};

static const char *const k_error_names[MESH_BLE_OTA_ERROR_COUNT] = {
    "none", "write", "timeout", "silent", "refused", "hash mismatch", "protocol",
};

const char *mesh_ble_ota_state_name(enum mesh_ble_ota_state state) {
    return state < MESH_BLE_OTA_STATE_COUNT ? k_state_names[state] : "?";
}

const char *mesh_ble_ota_error_name(enum mesh_ble_ota_error error) {
    return error < MESH_BLE_OTA_ERROR_COUNT ? k_error_names[error] : "?";
}

size_t mesh_ble_ota_chunk_for_mtu(uint16_t mtu) {
    /* `- 3` is not a detail: an ATT MTU is the whole PDU, and a Write Request spends one byte on
       the opcode and two on the handle. A chunk one byte longer is a long write - Prepare per
       fragment, then Execute - which the loader receives as several writes and ACKs several
       times. See "What phase 0 measured on BLE" for the line in BlueZ that decides it. */
    if (mtu < OTA_ATT_MTU_MIN) {
        return MESH_BLE_OTA_CHUNK_MIN;
    }
    const size_t payload = (size_t)mtu - 3U;
    return payload > MESH_BLE_OTA_CHUNK_MAX ? MESH_BLE_OTA_CHUNK_MAX : payload;
}

bool mesh_ble_ota_busy(const struct mesh_ble_ota *ota) {
    return ota != NULL && ota->state != MESH_BLE_OTA_IDLE && ota->state != MESH_BLE_OTA_DONE &&
           ota->state != MESH_BLE_OTA_FAILED;
}

static void ota_fail(struct mesh_ble_ota *ota, enum mesh_ble_ota_error error, uint64_t now_ms) {
    ota->state = MESH_BLE_OTA_FAILED;
    ota->error = error;
    ota->finished_ms = now_ms;
    mesh_log_error("ble_ota", "The loader conversation failed: %s%s%s, at %zu of %zu bytes",
                   mesh_ble_ota_error_name(error), ota->reason[0] != '\0' ? ": " : "", ota->reason,
                   ota->acked, ota->image_len);
}

/* ---- what the loader says ------------------------------------------------------------------
 */

static void ota_push(struct mesh_ble_ota *ota, enum mesh_ble_ota_event_kind kind,
                     const char *text) {
    if (ota->event_count >= MESH_BLE_OTA_EVENTS) {
        /* Eight answers unread is a loader talking out of turn, since this side never has more
           than one question outstanding. The tick reports it. */
        ota->events_overflow = true;
        return;
    }
    struct mesh_ble_ota_event *const event =
        &ota->events[(ota->event_head + ota->event_count) % MESH_BLE_OTA_EVENTS];
    event->kind = kind;
    mesh_str_copy(event->text, sizeof event->text, text != NULL ? text : "");
    ota->event_count += 1U;
}

static const char *ota_after(const char *line, size_t prefix) {
    const char *rest = line + prefix;
    while (*rest == ' ') {
        ++rest;
    }
    return rest;
}

static void ota_line(struct mesh_ble_ota *ota, const char *line) {
    if (line[0] == '\0') {
        return;
    }
    if (strcmp(line, "ACK") == 0) {
        ota_push(ota, MESH_BLE_OTA_EVENT_ACK, NULL);
    } else if (strcmp(line, "ERASING") == 0) {
        ota_push(ota, MESH_BLE_OTA_EVENT_ERASING, NULL);
    } else if (strncmp(line, "OK", 2U) == 0 && (line[2] == '\0' || line[2] == ' ')) {
        ota_push(ota, MESH_BLE_OTA_EVENT_OK, ota_after(line, 2U));
    } else if (strncmp(line, "ERR", 3U) == 0) {
        ota_push(ota, MESH_BLE_OTA_EVENT_ERR, ota_after(line, 3U));
    } else {
        /* The loader logs to its UART, not to this characteristic, so anything else is not a
           thing it says - but a line nobody asked about is not worth failing a transfer for. */
        mesh_log_debug("ble_ota", "Ignoring '%s' from the loader", line);
    }
}

void mesh_ble_ota_feed(struct mesh_ble_ota *ota, const uint8_t *data, size_t len) {
    if (ota == NULL || data == NULL) {
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = (char)data[i];
        if (c == '\n') {
            ota->line[ota->line_len] = '\0';
            if (!ota->line_overflow) {
                ota_line(ota, ota->line);
            }
            ota->line_len = 0U;
            ota->line_overflow = false;
        } else if (c == '\r') {
            continue;
        } else if (ota->line_len + 1U < sizeof ota->line) {
            ota->line[ota->line_len++] = c;
        } else {
            ota->line_overflow = true;
        }
    }
}

static void ota_notification(const uint8_t *data, size_t len, void *userdata) {
    mesh_ble_ota_feed((struct mesh_ble_ota *)userdata, data, len);
}

/* ---- what we say ---------------------------------------------------------------------------
 */

static bool ota_can_write(const struct mesh_ble_ota *ota) {
    return ota->write_data == NULL || ota->write_done;
}

/* Drives the write in progress. False once the conversation has failed. */
static bool ota_pump(struct mesh_ble_ota *ota, uint64_t now_ms) {
    if (ota->write_data == NULL || ota->write_done) {
        return true;
    }
    /* The same call issues the write and, while its reply is pending, polls it - so it is made
       with the same bytes until it stops answering -EAGAIN. */
    const int result = mesh_bluez_client_write(
        ota->client, ota->write_path, MESH_BLE_OTA_WRITE_UUID, ota->write_data, ota->write_len);
    if (result == -EAGAIN) {
        return true;
    }
    if (result == 0) {
        ota->write_done = true;
        return true;
    }
    ota->write_error = result;
    mesh_log_warn("ble_ota", "A %zu-byte write was refused: %s", ota->write_len, strerror(-result));
    ota_fail(ota, MESH_BLE_OTA_ERROR_WRITE, now_ms);
    return false;
}

static bool ota_issue(struct mesh_ble_ota *ota, const uint8_t *data, size_t len, uint64_t now_ms) {
    ota->write_data = data;
    ota->write_len = len;
    ota->write_done = false;
    return ota_pump(ota, now_ms);
}

static void ota_send_next_chunk(struct mesh_ble_ota *ota, uint64_t now_ms) {
    const size_t left = ota->image_len - ota->acked;
    const size_t len = left < ota->chunk ? left : ota->chunk;
    const bool last = len == left;
    ota->in_flight = len;
    ota->state = last ? MESH_BLE_OTA_FINISHING : MESH_BLE_OTA_SENDING;
    ota->deadline_ms = now_ms + (last ? OTA_FINISH_TIMEOUT_MS : OTA_ACK_TIMEOUT_MS);
    (void)ota_issue(ota, ota->image + ota->acked, len, now_ms);
}

/* The partition is empty and the stream can start. */
static bool ota_erased(struct mesh_ble_ota *ota, uint64_t now_ms) {
    if (!ota_can_write(ota)) {
        return false;
    }
    ota->sending_since_ms = now_ms;
    mesh_log_info("ble_ota", "Erased; sending %zu bytes in %zu-byte chunks", ota->image_len,
                  ota->chunk);
    ota_send_next_chunk(ota, now_ms);
    return true;
}

/* Acts on one answer. False leaves it queued: it needs a write and the last one's reply is not
   in yet, which happens whenever the loader's notification beats BlueZ's Write Response. */
static bool ota_handle(struct mesh_ble_ota *ota, const struct mesh_ble_ota_event *event,
                       uint64_t now_ms) {
    if (event->kind == MESH_BLE_OTA_EVENT_ERR) {
        mesh_str_copy(ota->reason, sizeof ota->reason, event->text);
        ota_fail(ota,
                 strstr(event->text, "Hash Mismatch") != NULL ? MESH_BLE_OTA_ERROR_HASH_MISMATCH
                                                              : MESH_BLE_OTA_ERROR_REFUSED,
                 now_ms);
        return true;
    }

    switch (ota->state) {
    case MESH_BLE_OTA_VERSION: {
        if (event->kind != MESH_BLE_OTA_EVENT_OK) {
            break;
        }
        if (!ota_can_write(ota)) {
            return false;
        }
        mesh_str_copy(ota->loader_version, sizeof ota->loader_version, event->text);
        mesh_log_info("ble_ota", "The loader answered: %s", ota->loader_version);
        char hex[MESH_SHA256_HEX_LEN];
        mesh_sha256_hex(ota->sha256, hex, sizeof hex);
        snprintf(ota->command, sizeof ota->command, "OTA %zu %s\n", ota->image_len, hex);
        ota->state = MESH_BLE_OTA_STARTING;
        ota->deadline_ms = now_ms + OTA_START_TIMEOUT_MS;
        (void)ota_issue(ota, (const uint8_t *)ota->command, strlen(ota->command), now_ms);
        return true;
    }
    case MESH_BLE_OTA_STARTING:
        if (event->kind == MESH_BLE_OTA_EVENT_ERASING) {
            ota->state = MESH_BLE_OTA_ERASING;
            ota->deadline_ms = now_ms + OTA_ERASE_TIMEOUT_MS;
            mesh_log_info("ble_ota", "The loader is erasing its partition");
            return true;
        }
        /* An OK with no ERASING in front of it is the erase already over. */
        if (event->kind != MESH_BLE_OTA_EVENT_OK) {
            break;
        }
        return ota_erased(ota, now_ms);
    case MESH_BLE_OTA_ERASING:
        if (event->kind != MESH_BLE_OTA_EVENT_OK) {
            break;
        }
        return ota_erased(ota, now_ms);
    case MESH_BLE_OTA_SENDING:
        if (event->kind != MESH_BLE_OTA_EVENT_ACK || ota->in_flight == 0U) {
            break;
        }
        if (!ota_can_write(ota)) {
            return false;
        }
        ota->acked += ota->in_flight;
        ota->in_flight = 0U;
        ota_send_next_chunk(ota, now_ms);
        return true;
    case MESH_BLE_OTA_FINISHING:
        /* The loader answers the last chunk with OK instead of ACK, and only once it has hashed
           the partition, compared it with what the radio stored and switched the boot
           partition. An ACK here is the cadence lost somewhere upstream. */
        if (event->kind != MESH_BLE_OTA_EVENT_OK) {
            break;
        }
        ota->acked += ota->in_flight;
        ota->in_flight = 0U;
        ota->state = MESH_BLE_OTA_DONE;
        ota->finished_ms = now_ms;
        mesh_log_info("ble_ota", "The loader took all %zu bytes and the hash matched",
                      ota->image_len);
        return true;
    default:
        return true;
    }

    /* Everything that broke out of the switch is an answer to a question we did not ask. */
    mesh_str_copy(ota->reason, sizeof ota->reason,
                  event->kind == MESH_BLE_OTA_EVENT_ACK
                      ? "ACK out of turn"
                      : (event->kind == MESH_BLE_OTA_EVENT_ERASING ? "ERASING out of turn"
                                                                   : "OK out of turn"));
    ota_fail(ota, MESH_BLE_OTA_ERROR_PROTOCOL, now_ms);
    return true;
}

/* ---- the public half -----------------------------------------------------------------------
 */

int mesh_ble_ota_attach(struct mesh_ble_ota *ota, struct mesh_bluez_client *client,
                        const char *device_path) {
    if (ota == NULL || client == NULL || device_path == NULL) {
        return -EINVAL;
    }
    memset(ota, 0, sizeof *ota);
    ota->client = client;

    int result = mesh_bluez_client_find_characteristic(client, device_path, MESH_BLE_OTA_WRITE_UUID,
                                                       ota->write_path, sizeof ota->write_path);
    if (result < 0) {
        mesh_log_warn("ble_ota", "No OTA characteristic under %s: %d", device_path, result);
        return result;
    }
    result = mesh_bluez_client_find_characteristic(client, device_path, MESH_BLE_OTA_NOTIFY_UUID,
                                                   ota->notify_path, sizeof ota->notify_path);
    if (result < 0) {
        mesh_log_warn("ble_ota", "No answer characteristic under %s: %d", device_path, result);
        return result;
    }

    uint16_t mtu = 0U;
    result = mesh_bluez_client_characteristic_mtu(client, ota->write_path, &mtu);
    ota->mtu = result == 0 ? mtu : 0U;
    ota->chunk = mesh_ble_ota_chunk_for_mtu(ota->mtu);
    if (result != 0) {
        /* Correct and slow: twenty bytes always fit a Write Request, so the cadence holds. */
        mesh_log_warn("ble_ota", "BlueZ reported no MTU (%d); writing %zu-byte chunks", result,
                      ota->chunk);
    }

    mesh_bluez_client_set_notification_handler(client, ota_notification, ota);
    result = mesh_bluez_client_subscribe(client, ota->notify_path, MESH_BLE_OTA_NOTIFY_UUID);
    if (result < 0) {
        mesh_bluez_client_set_notification_handler(client, NULL, NULL);
        mesh_log_warn("ble_ota", "Could not subscribe to the loader's answers: %d", result);
        return result;
    }
    mesh_log_info("ble_ota", "Loader attached: MTU %u, %zu-byte chunks", (unsigned)ota->mtu,
                  ota->chunk);
    return 0;
}

int mesh_ble_ota_begin(struct mesh_ble_ota *ota, const uint8_t *image, size_t image_len,
                       const uint8_t sha256[32], uint64_t now_ms) {
    if (ota == NULL || ota->client == NULL || ota->write_path[0] == '\0' || image == NULL ||
        image_len == 0U || sha256 == NULL || ota->chunk == 0U) {
        return -EINVAL;
    }
    if (mesh_ble_ota_busy(ota)) {
        return -EBUSY;
    }
    ota->image = image;
    ota->image_len = image_len;
    memcpy(ota->sha256, sha256, sizeof ota->sha256);
    ota->acked = 0U;
    ota->in_flight = 0U;
    ota->error = MESH_BLE_OTA_ERROR_NONE;
    ota->write_error = 0;
    ota->write_data = NULL;
    ota->line_len = 0U;
    ota->line_overflow = false;
    ota->event_head = 0U;
    ota->event_count = 0U;
    ota->events_overflow = false;
    ota->loader_version[0] = '\0';
    ota->reason[0] = '\0';
    ota->sending_since_ms = 0U;
    ota->finished_ms = 0U;

    /* VERSION first, and not as a courtesy: it is how the unified loader is told from anything
       else that happens to advertise the service, since the old one answers nothing at all. */
    mesh_str_copy(ota->command, sizeof ota->command, "VERSION\n");
    ota->state = MESH_BLE_OTA_VERSION;
    ota->deadline_ms = now_ms + OTA_VERSION_TIMEOUT_MS;
    (void)ota_issue(ota, (const uint8_t *)ota->command, strlen(ota->command), now_ms);
    return ota->state == MESH_BLE_OTA_FAILED ? ota->write_error : 0;
}

void mesh_ble_ota_tick(struct mesh_ble_ota *ota, uint64_t now_ms) {
    if (!mesh_ble_ota_busy(ota) || !ota_pump(ota, now_ms)) {
        return;
    }
    if (ota->events_overflow) {
        mesh_str_copy(ota->reason, sizeof ota->reason, "answers out of turn");
        ota_fail(ota, MESH_BLE_OTA_ERROR_PROTOCOL, now_ms);
        return;
    }
    while (ota->event_count > 0U && mesh_ble_ota_busy(ota)) {
        if (!ota_handle(ota, &ota->events[ota->event_head], now_ms)) {
            break;
        }
        ota->event_head = (ota->event_head + 1U) % MESH_BLE_OTA_EVENTS;
        ota->event_count -= 1U;
    }
    if (mesh_ble_ota_busy(ota) && now_ms >= ota->deadline_ms) {
        ota_fail(ota,
                 ota->state == MESH_BLE_OTA_VERSION ? MESH_BLE_OTA_ERROR_SILENT
                                                    : MESH_BLE_OTA_ERROR_TIMEOUT,
                 now_ms);
    }
}

void mesh_ble_ota_detach(struct mesh_ble_ota *ota) {
    if (ota == NULL || ota->client == NULL) {
        return;
    }
    mesh_bluez_client_set_notification_handler(ota->client, NULL, NULL);
    mesh_bluez_client_requests_cancel(ota->client);
    ota->write_data = NULL;
}

unsigned mesh_ble_ota_progress(const struct mesh_ble_ota *ota) {
    if (ota == NULL || ota->image_len == 0U) {
        return 0U;
    }
    if (ota->state == MESH_BLE_OTA_DONE) {
        return 100U;
    }
    return (unsigned)((uint64_t)ota->acked * 100U / (uint64_t)ota->image_len);
}

uint32_t mesh_ble_ota_bytes_per_second(const struct mesh_ble_ota *ota, uint64_t now_ms) {
    if (ota == NULL || ota->sending_since_ms == 0U || ota->acked == 0U) {
        return 0U;
    }
    const uint64_t end = ota->finished_ms != 0U ? ota->finished_ms : now_ms;
    if (end <= ota->sending_since_ms) {
        return 0U;
    }
    return (uint32_t)((uint64_t)ota->acked * 1000U / (end - ota->sending_since_ms));
}
