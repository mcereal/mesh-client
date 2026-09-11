#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_ota.h"

#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/ble_hci.h"
#include "mesh/utils/file.h"
#include "mesh/utils/log.h"
#include "mesh/utils/sha256.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* GetManagedObjects blocks the loop for as long as bluetoothd takes to answer, so the tree is
   read once a second rather than every turn. */
#define OTA_POLL_MS 1000U
/* A little quicker while connecting, where every poll is an asynchronous Properties.Get. */
#define OTA_CONNECT_POLL_MS 250U
/* The firmware answers and reboots one second later ("Reboot in 1 seconds, hard coded"). */
#define OTA_ARM_TIMEOUT_MS 10000U
/* A reboot, the loader's own start-up and half a second of NimBLE, then a scan window. */
#define OTA_LOADER_TIMEOUT_MS 60000U
#define OTA_CONNECT_TIMEOUT_MS 30000U
/* Two seconds of the loader flushing its last answer, the application's boot, and a scan. */
#define OTA_RESTART_TIMEOUT_MS 90000U
#define OTA_SCAN_MAX 16U

static const char *const k_state_names[MESH_FIRMWARE_OTA_STATE_COUNT] = {
    "idle", "arming", "waiting", "connecting", "sending", "restarting", "done", "failed",
};

static const char *const k_error_names[MESH_FIRMWARE_OTA_ERROR_COUNT] = {
    "none",     "unavailable", "wrong image",    "arm",           "refused",  "no loader",
    "connect",  "transfer",    "loader refused", "hash mismatch", "no radio",
};

const char *mesh_firmware_ota_state_name(enum mesh_firmware_ota_state state) {
    return state < MESH_FIRMWARE_OTA_STATE_COUNT ? k_state_names[state] : "?";
}

const char *mesh_firmware_ota_error_name(enum mesh_firmware_ota_error error) {
    return error < MESH_FIRMWARE_OTA_ERROR_COUNT ? k_error_names[error] : "?";
}

bool mesh_firmware_ota_busy(const struct mesh_firmware_ota *ota) {
    return ota != NULL && ota->state != MESH_FIRMWARE_OTA_IDLE &&
           ota->state != MESH_FIRMWARE_OTA_DONE && ota->state != MESH_FIRMWARE_OTA_FAILED;
}

bool mesh_firmware_ota_holds_the_radio(const struct mesh_firmware_ota *ota) {
    return mesh_firmware_ota_busy(ota);
}

bool mesh_firmware_ota_radio_in_loader(const struct mesh_firmware_ota *ota) {
    if (ota == NULL || ota->state != MESH_FIRMWARE_OTA_FAILED) {
        return false;
    }
    switch (ota->error) {
    case MESH_FIRMWARE_OTA_ERROR_CONNECT:
    case MESH_FIRMWARE_OTA_ERROR_TRANSFER:
    case MESH_FIRMWARE_OTA_ERROR_LOADER_REFUSED:
    case MESH_FIRMWARE_OTA_ERROR_HASH_MISMATCH:
        return true;
    case MESH_FIRMWARE_OTA_ERROR_NO_LOADER:
        /* Nothing answered the scan, but the radio said it was on its way - or a loader was
           there earlier, before a retry went looking for it again. */
        return ota->go_ahead || ota->loader_seen;
    default:
        return false;
    }
}

unsigned mesh_firmware_ota_progress(const struct mesh_firmware_ota *ota) {
    if (ota == NULL) {
        return 0U;
    }
    switch (ota->state) {
    case MESH_FIRMWARE_OTA_SENDING:
        return mesh_ble_ota_progress(&ota->conversation);
    case MESH_FIRMWARE_OTA_RESTARTING:
    case MESH_FIRMWARE_OTA_DONE:
        return 100U;
    default:
        return 0U;
    }
}

bool mesh_firmware_ota_offset_address(const char *address, int delta, char *out, size_t out_len) {
    uint8_t bytes[6];
    if (out == NULL || out_len < 18U || !mesh_ble_hci_parse_address(address, bytes)) {
        return false;
    }
    /* Forty-eight bits, least significant octet first, so a carry out of the last octet runs
       into the one before it and 00 minus one borrows from it. */
    uint64_t value = 0U;
    for (size_t i = 6U; i-- > 0U;) {
        value = value << 8 | bytes[i];
    }
    value = (value + (uint64_t)(int64_t)delta) & 0xFFFFFFFFFFFFULL;
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(value >> 40 & 0xFFU),
             (unsigned)(value >> 32 & 0xFFU), (unsigned)(value >> 24 & 0xFFU),
             (unsigned)(value >> 16 & 0xFFU), (unsigned)(value >> 8 & 0xFFU),
             (unsigned)(value & 0xFFU));
    return true;
}

enum mesh_firmware_ota_answer mesh_firmware_ota_classify(const char *text) {
    if (text == NULL) {
        return MESH_FIRMWARE_OTA_ANSWER_UNRELATED;
    }
    /* AdminModule's own sentences, at 81b3ce8. Prefixes rather than whole strings, because two
       of them end in a detail ("...partition.", "...support BLE") that is the firmware's to
       reword. */
    if (strncmp(text, "Rebooting to BLE OTA", 20U) == 0) {
        return MESH_FIRMWARE_OTA_ANSWER_GO_AHEAD;
    }
    if (strncmp(text, "Cannot start OTA", 16U) == 0 ||
        strncmp(text, "OTA Loader does not support", 27U) == 0 ||
        strncmp(text, "Unable to switch to the OTA partition", 37U) == 0) {
        return MESH_FIRMWARE_OTA_ANSWER_REFUSED;
    }
    return MESH_FIRMWARE_OTA_ANSWER_UNRELATED;
}

/* "/org/bluez/hci0" + "9C:13:9E:9D:0A:DA" -> "/org/bluez/hci0/dev_9C_13_9E_9D_0A_DA" */
static void ota_device_path(const char *adapter_path, const char *address, char *out,
                            size_t out_len) {
    const int written = snprintf(out, out_len, "%s/dev_%s", adapter_path, address);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return;
    }
    for (char *c = out + strlen(adapter_path); *c != '\0'; ++c) {
        if (*c == ':') {
            *c = '_';
        }
    }
}

/* ---- the shape every step ends in ----------------------------------------------------------
 */

static void ota_discovery(struct mesh_firmware_ota *ota, bool on) {
    if (on && !ota->discovering) {
        const int result = mesh_bluez_client_start_discovery(ota->client, ota->adapter_path);
        /* Discovery is counted per D-Bus client, so another client's scan does not make ours an
           error - but a failure here is a scan that never hears the loader, so say so. */
        if (result == 0) {
            ota->discovering = true;
        } else {
            mesh_log_warn("firmware", "Could not start a scan: %d", result);
        }
    } else if (!on && ota->discovering) {
        (void)mesh_bluez_client_stop_discovery(ota->client, ota->adapter_path);
        ota->discovering = false;
    }
}

/* Lets go of the loader: its notifications, any write in flight, and the link itself. */
static void ota_release_loader(struct mesh_firmware_ota *ota) {
    mesh_ble_ota_detach(&ota->conversation);
    mesh_bluez_client_connect_cancel(ota->client);
    if (ota->connected && ota->loader_path[0] != '\0') {
        (void)mesh_bluez_client_disconnect(ota->client, ota->loader_path);
    }
    ota->connected = false;
}

static void ota_release(struct mesh_firmware_ota *ota) {
    if (ota->client != NULL) {
        ota_release_loader(ota);
        ota_discovery(ota, false);
    }
    free(ota->image);
    ota->image = NULL;
    ota->image_len = 0U;
}

static void ota_finish(struct mesh_firmware_ota *ota, enum mesh_firmware_ota_state state,
                       enum mesh_firmware_ota_error error) {
    ota_release(ota);
    ota->state = state;
    ota->error = error;
    const mesh_firmware_ota_done_fn on_done = ota->on_done;
    void *const userdata = ota->userdata;
    ota->on_done = NULL;
    if (on_done != NULL) {
        on_done(userdata, ota);
    }
}

static void ota_fail(struct mesh_firmware_ota *ota, enum mesh_firmware_ota_error error) {
    /* Stated before the release and the callback, so the log can already answer the question
       the banner will ask. */
    ota->state = MESH_FIRMWARE_OTA_FAILED;
    ota->error = error;
    mesh_log_error("firmware", "The BLE install failed: %s%s%s%s",
                   mesh_firmware_ota_error_name(error), ota->reason[0] != '\0' ? " (" : "",
                   ota->reason, ota->reason[0] != '\0' ? ")" : "");
    if (mesh_firmware_ota_radio_in_loader(ota)) {
        mesh_log_error("firmware", "The radio is in its OTA loader, off the mesh, and stays "
                                   "there until it is sent this image; the same install started "
                                   "again resumes it");
    }
    ota_finish(ota, MESH_FIRMWARE_OTA_FAILED, error);
}

/* A start that never started: `state` and `error` say why and no callback will arrive. */
static void ota_refuse(struct mesh_firmware_ota *ota, enum mesh_firmware_ota_error error) {
    ota->on_done = NULL;
    ota_release(ota);
    ota->state = MESH_FIRMWARE_OTA_FAILED;
    ota->error = error;
}

/* ---- the steps -----------------------------------------------------------------------------
 */

static void ota_enter_waiting(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    ota->state = MESH_FIRMWARE_OTA_WAITING;
    ota->deadline_ms = now_ms + OTA_LOADER_TIMEOUT_MS;
    ota->next_poll_ms = now_ms;
    ota->ambiguity_logged = false;
    ota_discovery(ota, true);
    mesh_log_info("firmware", "Looking for the OTA loader%s%s", ota->radio_address[0] ? " of " : "",
                  ota->radio_address);
}

/* The transfer, or the connect, broke. Try the loader again while there are tries left. */
static void ota_retry(struct mesh_firmware_ota *ota, enum mesh_firmware_ota_error error,
                     uint64_t now_ms) {
    ota_release_loader(ota);
    ota->attempts += 1U;
    if (ota->attempts >= MESH_FIRMWARE_OTA_ATTEMPTS) {
        ota_fail(ota, error);
        return;
    }
    mesh_log_warn("firmware", "Trying the loader again (%u of %u) after: %s", ota->attempts + 1U,
                  MESH_FIRMWARE_OTA_ATTEMPTS, mesh_firmware_ota_error_name(error));
    ota_enter_waiting(ota, now_ms);
}

static void ota_tick_arming(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    if (now_ms < ota->deadline_ms) {
        return;
    }
    /* Nothing said, either way. The notification can lose the race with the reboot it
       announces, so silence is not a refusal - the loader turning up is the answer that counts,
       and waiting's own clock is what gives up. */
    mesh_log_warn("firmware", "The radio did not answer the OTA request; looking for a loader "
                              "anyway");
    ota_enter_waiting(ota, now_ms);
}

static const struct mesh_bluez_device_info *
ota_pick_loader(struct mesh_firmware_ota *ota, const struct mesh_bluez_device_info *devices,
                size_t count) {
    char expected[MESH_FIRMWARE_OTA_ADDRESS_MAX] = {0};
    if (ota->loader_address[0] != '\0') {
        mesh_str_copy(expected, sizeof expected, ota->loader_address);
    } else if (ota->radio_address[0] != '\0') {
        (void)mesh_firmware_ota_offset_address(ota->radio_address, 1, expected, sizeof expected);
    }

    const struct mesh_bluez_device_info *heard = NULL;
    size_t heard_count = 0U;
    for (size_t i = 0; i < count; ++i) {
        if (!devices[i].in_range) {
            continue;
        }
        /* The address is corroboration rather than the filter, but when it does corroborate,
           it settles which of several loaders is ours. */
        if ((expected[0] != '\0' && strcasecmp(devices[i].address, expected) == 0) ||
            (ota->radio_address[0] != '\0' &&
             strcasecmp(devices[i].address, ota->radio_address) == 0)) {
            return &devices[i];
        }
        heard = &devices[i];
        heard_count += 1U;
    }
    if (heard_count == 1U) {
        /* One loader in earshot, at an address the rule did not predict. Taking it costs
           nothing if it is somebody else's: that loader holds another image's hash and refuses
           ours in so many words. */
        if (expected[0] != '\0') {
            mesh_log_warn("firmware", "The only loader in range is %s, not %s as expected",
                          heard->address, expected);
        }
        return heard;
    }
    if (heard_count > 1U && !ota->ambiguity_logged) {
        mesh_log_warn("firmware", "%zu OTA loaders in range and none at %s; waiting", heard_count,
                      expected[0] != '\0' ? expected : "a known address");
        ota->ambiguity_logged = true;
    }
    return NULL;
}

static void ota_begin_connect(struct mesh_firmware_ota *ota,
                              const struct mesh_bluez_device_info *loader, uint64_t now_ms) {
    ota_discovery(ota, false);
    ota->loader_seen = true;
    mesh_str_copy(ota->loader_address, sizeof ota->loader_address, loader->address);
    ota_device_path(ota->adapter_path, ota->loader_address, ota->loader_path,
                    sizeof ota->loader_path);
    if (ota->radio_address[0] == '\0') {
        /* A board already in its loader, found without knowing whose it is. The rule runs
           backwards as well, and it is what the restart is then watched for. */
        (void)mesh_firmware_ota_offset_address(ota->loader_address, -1, ota->radio_address,
                                               sizeof ota->radio_address);
    }
    mesh_log_info("firmware", "Found the OTA loader at %s (%s, %d dBm)", loader->address,
                  loader->name[0] != '\0' ? loader->name : "unnamed", (int)loader->rssi);

    ota->state = MESH_FIRMWARE_OTA_CONNECTING;
    ota->deadline_ms = now_ms + OTA_CONNECT_TIMEOUT_MS;
    ota->next_poll_ms = now_ms;
    ota->connected = false;
    const int result = mesh_bluez_client_connect_begin(ota->client, ota->loader_path);
    if (result < 0) {
        mesh_log_warn("firmware", "Connect to the loader would not start: %d", result);
        ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_CONNECT, now_ms);
    }
}

static void ota_tick_waiting(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    if (now_ms >= ota->next_poll_ms) {
        ota->next_poll_ms = now_ms + OTA_POLL_MS;
        struct mesh_bluez_device_info devices[OTA_SCAN_MAX];
        size_t count = 0U;
        if (mesh_bluez_client_list_by_service(ota->client, MESH_BLE_OTA_SERVICE_UUID, devices,
                                              OTA_SCAN_MAX, &count) == 0) {
            const struct mesh_bluez_device_info *const loader =
                ota_pick_loader(ota, devices, count);
            if (loader != NULL) {
                ota_begin_connect(ota, loader, now_ms);
                return;
            }
        }
    }
    if (now_ms >= ota->deadline_ms) {
        ota_fail(ota, MESH_FIRMWARE_OTA_ERROR_NO_LOADER);
    }
}

static void ota_tick_connecting(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    if (!ota->connected) {
        int result = 0;
        const int polled = mesh_bluez_client_connect_poll(ota->client, &result);
        if (polled == 1 && result < 0) {
            mesh_log_warn("firmware", "The loader refused the connection: %d", result);
            ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_CONNECT, now_ms);
            return;
        }
        if (polled == 1) {
            ota->connected = true;
            /* Before the service discovery rather than after it: at the Brick's 30 ms that
               discovery is itself slow, and there is nothing to lose by asking now. */
            if (ota->request_interval != NULL) {
                const int asked = ota->request_interval(ota->hci_dev, ota->loader_address);
                if (asked != 0) {
                    mesh_log_warn("firmware",
                                  "Could not ask for a fast connection interval (%d); the "
                                  "transfer will run at whatever the link has",
                                  asked);
                }
            }
        }
    }

    if (ota->connected && now_ms >= ota->next_poll_ms) {
        ota->next_poll_ms = now_ms + OTA_CONNECT_POLL_MS;
        bool resolved = false;
        if (mesh_bluez_client_services_resolved(ota->client, ota->loader_path, &resolved) == 0 &&
            resolved) {
            const int attached =
                mesh_ble_ota_attach(&ota->conversation, ota->client, ota->loader_path);
            if (attached < 0) {
                ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_CONNECT, now_ms);
                return;
            }
            if (mesh_ble_ota_begin(&ota->conversation, ota->image, ota->image_len, ota->sha256,
                                   now_ms) != 0) {
                ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_TRANSFER, now_ms);
                return;
            }
            ota->state = MESH_FIRMWARE_OTA_SENDING;
            ota->next_poll_ms = now_ms + OTA_POLL_MS;
            return;
        }
    }

    if (now_ms >= ota->deadline_ms) {
        ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_CONNECT, now_ms);
    }
}

static void ota_enter_restarting(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    mesh_log_info("firmware", "Flashed %zu bytes at %u B/s; waiting for the radio to restart",
                  ota->image_len,
                  (unsigned)mesh_ble_ota_bytes_per_second(&ota->conversation, now_ms));
    ota_release_loader(ota);
    if (ota->radio_address[0] == '\0') {
        ota->radio_seen = false;
        ota_finish(ota, MESH_FIRMWARE_OTA_DONE, MESH_FIRMWARE_OTA_ERROR_NONE);
        return;
    }
    ota->state = MESH_FIRMWARE_OTA_RESTARTING;
    ota->deadline_ms = now_ms + OTA_RESTART_TIMEOUT_MS;
    ota->next_poll_ms = now_ms + OTA_POLL_MS;
    ota_discovery(ota, true);
}

static void ota_tick_sending(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    mesh_ble_ota_tick(&ota->conversation, now_ms);
    switch (ota->conversation.state) {
    case MESH_BLE_OTA_DONE:
        ota_enter_restarting(ota, now_ms);
        return;
    case MESH_BLE_OTA_FAILED:
        mesh_str_copy(ota->reason, sizeof ota->reason, ota->conversation.reason);
        switch (ota->conversation.error) {
        case MESH_BLE_OTA_ERROR_REFUSED:
            /* The loader's own no: another image's hash, a partition it could not begin.
               Resending the same bytes answers the same way, so there is no retry. */
            ota_fail(ota, MESH_FIRMWARE_OTA_ERROR_LOADER_REFUSED);
            return;
        case MESH_BLE_OTA_ERROR_HASH_MISMATCH:
            ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_HASH_MISMATCH, now_ms);
            return;
        default:
            ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_TRANSFER, now_ms);
            return;
        }
    default:
        break;
    }

    /* A link that drops mid-stream shows up as a write refused or an ACK that never comes -
       but the second of those costs ten seconds, and asking BlueZ costs one poll. */
    if (now_ms >= ota->next_poll_ms) {
        ota->next_poll_ms = now_ms + OTA_POLL_MS;
        bool connected = true;
        if (mesh_bluez_client_device_connected(ota->client, ota->loader_path, &connected) == 0 &&
            !connected) {
            mesh_log_warn("firmware", "Lost the loader at %u%%",
                          mesh_ble_ota_progress(&ota->conversation));
            ota->connected = false;
            ota_retry(ota, MESH_FIRMWARE_OTA_ERROR_TRANSFER, now_ms);
        }
    }
}

static void ota_tick_restarting(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    if (now_ms < ota->next_poll_ms) {
        return;
    }
    ota->next_poll_ms = now_ms + OTA_POLL_MS;

    /* The radio advertising where it was, heard in this scan. Discovery was stopped for the
       transfer and started again for this, so an RSSI on the radio's address is an advertisement
       from the firmware the loader just wrote rather than one left over from before. */
    struct mesh_bluez_device_info devices[OTA_SCAN_MAX];
    size_t count = 0U;
    bool back = false;
    if (mesh_bluez_client_list_meshtastic(ota->client, devices, OTA_SCAN_MAX, &count) == 0) {
        for (size_t i = 0; i < count && !back; ++i) {
            back = devices[i].in_range && strcasecmp(devices[i].address, ota->radio_address) == 0;
        }
    }
    if (back) {
        mesh_log_info("firmware", "The radio is back at %s", ota->radio_address);
        ota->radio_seen = true;
        ota_finish(ota, MESH_FIRMWARE_OTA_DONE, MESH_FIRMWARE_OTA_ERROR_NONE);
        return;
    }
    if (now_ms >= ota->deadline_ms) {
        ota_fail(ota, MESH_FIRMWARE_OTA_ERROR_NO_RADIO);
    }
}

/* ---- the public half -----------------------------------------------------------------------
 */

int mesh_firmware_ota_start(struct mesh_firmware_ota *ota,
                            const struct mesh_firmware_ota_params *params) {
    if (ota == NULL) {
        return -EINVAL;
    }
    if (mesh_firmware_ota_busy(ota)) {
        return -EBUSY;
    }
    memset(ota, 0, sizeof *ota);
    if (params == NULL || params->client == NULL || params->adapter_path == NULL ||
        params->image_path == NULL || params->image_path[0] == '\0') {
        ota_refuse(ota, MESH_FIRMWARE_OTA_ERROR_UNAVAILABLE);
        return -EINVAL;
    }

    uint16_t chip = 0U;
    if (!mesh_esp_chip_for_architecture(params->architecture, &chip)) {
        mesh_log_error("firmware", "'%s' is not an ESP32; it has no BLE install",
                       params->architecture != NULL ? params->architecture : "?");
        ota_refuse(ota, MESH_FIRMWARE_OTA_ERROR_UNAVAILABLE);
        return -EINVAL;
    }

    ota->image = mesh_file_read(params->image_path, MESH_FIRMWARE_OTA_IMAGE_MAX, &ota->image_len);
    if (ota->image == NULL) {
        mesh_log_error("firmware", "The staged image could not be read: %s", params->image_path);
        ota_refuse(ota, MESH_FIRMWARE_OTA_ERROR_UNAVAILABLE);
        return -EIO;
    }
    const enum mesh_esp_image_verdict verdict =
        mesh_esp_image_validate(ota->image, ota->image_len, chip, &ota->esp);
    if (verdict != MESH_ESP_IMAGE_OK) {
        mesh_log_error("firmware", "The staged image is not an application for chip %#x: %s "
                                   "(it says chip %#x)",
                       (unsigned)chip, mesh_esp_image_verdict_name(verdict),
                       (unsigned)ota->esp.chip_id);
        ota_refuse(ota, MESH_FIRMWARE_OTA_ERROR_WRONG_IMAGE);
        return -EINVAL;
    }
    struct mesh_sha256 hasher;
    mesh_sha256_init(&hasher);
    mesh_sha256_update(&hasher, ota->image, ota->image_len);
    mesh_sha256_final(&hasher, ota->sha256);

    ota->client = params->client;
    mesh_str_copy(ota->adapter_path, sizeof ota->adapter_path, params->adapter_path);
    ota->hci_dev = mesh_ble_hci_adapter_index(params->adapter_path);
    if (params->radio_address != NULL && params->radio_address[0] != '\0') {
        mesh_str_copy(ota->radio_address, sizeof ota->radio_address, params->radio_address);
    }
    ota->request_interval = params->request_interval;
    ota->on_done = params->on_done;
    ota->userdata = params->userdata;
    ota->arm = params->arm;
    ota->arm_userdata = params->arm_userdata;

    char hex[MESH_SHA256_HEX_LEN];
    mesh_sha256_hex(ota->sha256, hex, sizeof hex);
    if (params->arm != NULL) {
        const int armed = params->arm(params->arm_userdata, ota->sha256);
        if (armed != 0) {
            mesh_log_error("firmware", "The radio would not take the OTA request: %s",
                           strerror(-armed));
            ota_refuse(ota, MESH_FIRMWARE_OTA_ERROR_ARM);
            return armed;
        }
        ota->state = MESH_FIRMWARE_OTA_ARMING;
        mesh_log_info("firmware", "Asked the radio into its OTA loader for %zu bytes, sha256 %s",
                      ota->image_len, hex);
    } else {
        ota->state = MESH_FIRMWARE_OTA_WAITING;
        mesh_log_info("firmware", "Resuming at the loader with %zu bytes, sha256 %s",
                      ota->image_len, hex);
    }
    /* The clocks start on the first tick, which is where the time comes from. */
    return 0;
}

void mesh_firmware_ota_radio_said(struct mesh_firmware_ota *ota, const char *text) {
    if (ota == NULL || (ota->state != MESH_FIRMWARE_OTA_ARMING &&
                        ota->state != MESH_FIRMWARE_OTA_WAITING) ||
        ota->loader_seen) {
        return;
    }
    switch (mesh_firmware_ota_classify(text)) {
    case MESH_FIRMWARE_OTA_ANSWER_GO_AHEAD:
        mesh_log_info("firmware", "The radio is rebooting into its OTA loader");
        /* Arming ends on the next tick rather than on its clock. Only a flag here, because
           entering waiting starts a scan and needs the time, which a tick has and this does
           not. */
        ota->go_ahead = true;
        break;
    case MESH_FIRMWARE_OTA_ANSWER_REFUSED:
        mesh_str_copy(ota->reason, sizeof ota->reason, text);
        ota_fail(ota, MESH_FIRMWARE_OTA_ERROR_REFUSED);
        break;
    default:
        break;
    }
}

void mesh_firmware_ota_tick(struct mesh_firmware_ota *ota, uint64_t now_ms) {
    if (!mesh_firmware_ota_busy(ota)) {
        return;
    }
    if (!ota->started) {
        ota->started = true;
        if (ota->state == MESH_FIRMWARE_OTA_WAITING) {
            ota_enter_waiting(ota, now_ms);
            return;
        }
        ota->deadline_ms = now_ms + OTA_ARM_TIMEOUT_MS;
    }
    switch (ota->state) {
    case MESH_FIRMWARE_OTA_ARMING:
        if (ota->go_ahead) {
            ota_enter_waiting(ota, now_ms);
        } else {
            ota_tick_arming(ota, now_ms);
        }
        break;
    case MESH_FIRMWARE_OTA_WAITING:
        ota_tick_waiting(ota, now_ms);
        break;
    case MESH_FIRMWARE_OTA_CONNECTING:
        ota_tick_connecting(ota, now_ms);
        break;
    case MESH_FIRMWARE_OTA_SENDING:
        ota_tick_sending(ota, now_ms);
        break;
    case MESH_FIRMWARE_OTA_RESTARTING:
        ota_tick_restarting(ota, now_ms);
        break;
    default:
        break;
    }
}

void mesh_firmware_ota_cancel(struct mesh_firmware_ota *ota) {
    if (ota == NULL) {
        return;
    }
    ota_release(ota);
    ota->on_done = NULL;
    ota->userdata = NULL;
    ota->state = MESH_FIRMWARE_OTA_IDLE;
    ota->error = MESH_FIRMWARE_OTA_ERROR_NONE;
}
