#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_dfu.h"

#include "inkwell/base/file.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include "mesh/transport/ble_gatt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* GetManagedObjects blocks the loop for as long as bluetoothd takes to answer, so the tree is
   read once a second rather than every turn. */
#define DFU_POLL_MS 1000U
#define DFU_FAST_POLL_MS 250U
/* The trigger, the write's reply and the link going down: the firmware resets as soon as it has
   answered the write, so this is generous. */
#define DFU_ARM_TIMEOUT_MS 15000U
/* The bootloader's start-up, before anything connects to the radio's address. What the Android
   app waits, and less than a connect attempt costs when it is not up yet. */
#define DFU_REBOOT_MS 3000U
#define DFU_LOADER_TIMEOUT_MS 60000U
#define DFU_CONNECT_TIMEOUT_MS 30000U
#define DFU_RESTART_TIMEOUT_MS 90000U
#define DFU_SCAN_MAX 16U

static const char *const k_state_names[MESH_FIRMWARE_DFU_STATE_COUNT] = {
    "idle", "arming", "waiting", "connecting", "sending", "restarting", "done", "failed",
};

static const char *const k_error_names[MESH_FIRMWARE_DFU_ERROR_COUNT] = {
    "none",    "unavailable", "wrong image",        "refused",  "no bootloader",
    "connect", "transfer",    "bootloader refused", "no radio",
};

const char *mesh_firmware_dfu_state_name(enum mesh_firmware_dfu_state state) {
    return state < MESH_FIRMWARE_DFU_STATE_COUNT ? k_state_names[state] : "?";
}

const char *mesh_firmware_dfu_error_name(enum mesh_firmware_dfu_error error) {
    return error < MESH_FIRMWARE_DFU_ERROR_COUNT ? k_error_names[error] : "?";
}

bool mesh_firmware_dfu_busy(const struct mesh_firmware_dfu *dfu) {
    return dfu != NULL && dfu->state != MESH_FIRMWARE_DFU_IDLE &&
           dfu->state != MESH_FIRMWARE_DFU_DONE && dfu->state != MESH_FIRMWARE_DFU_FAILED;
}

bool mesh_firmware_dfu_holds_the_radio(const struct mesh_firmware_dfu *dfu) {
    return mesh_firmware_dfu_busy(dfu);
}

bool mesh_firmware_dfu_radio_in_loader(const struct mesh_firmware_dfu *dfu) {
    if (dfu == NULL || dfu->state != MESH_FIRMWARE_DFU_FAILED) {
        return false;
    }
    switch (dfu->error) {
    case MESH_FIRMWARE_DFU_ERROR_NO_LOADER:
    case MESH_FIRMWARE_DFU_ERROR_CONNECT:
    case MESH_FIRMWARE_DFU_ERROR_TRANSFER:
    case MESH_FIRMWARE_DFU_ERROR_LOADER_REFUSED:
        /* Once the radio dropped after the trigger it is the bootloader's, and once the
           bootloader has had a START the application is erased. */
        return dfu->dropped || dfu->loader_seen;
    default:
        return false;
    }
}

unsigned mesh_firmware_dfu_progress(const struct mesh_firmware_dfu *dfu) {
    if (dfu == NULL) {
        return 0U;
    }
    switch (dfu->state) {
    case MESH_FIRMWARE_DFU_SENDING:
        return mesh_ble_dfu_progress(&dfu->conversation);
    case MESH_FIRMWARE_DFU_RESTARTING:
    case MESH_FIRMWARE_DFU_DONE:
        return 100U;
    default:
        return 0U;
    }
}

/* ---- the shape every step ends in ------------------------------------------------------------
 */

static void dfu_discovery(struct mesh_firmware_dfu *dfu, bool on) {
    if (on && !dfu->discovering) {
        const int result = inkwell_ble_start_discovery(dfu->client);
        if (result == 0) {
            dfu->discovering = true;
        } else {
            inkwell_log_warn("firmware", "Could not start a scan: %d", result);
        }
    } else if (!on && dfu->discovering) {
        (void)inkwell_ble_stop_discovery(dfu->client);
        dfu->discovering = false;
    }
}

static void dfu_release_loader(struct mesh_firmware_dfu *dfu) {
    mesh_ble_dfu_detach(&dfu->conversation);
    inkwell_ble_read_cancel(dfu->client);
    inkwell_ble_connect_cancel(dfu->client);
    if (dfu->connected && dfu->loader_address[0] != '\0') {
        (void)inkwell_ble_disconnect(dfu->client, dfu->loader_address);
    }
    dfu->connected = false;
    dfu->resolved = false;
    dfu->version_checked = false;
    dfu->version_handle[0] = '\0';
}

static void dfu_release(struct mesh_firmware_dfu *dfu) {
    if (dfu->client != NULL) {
        dfu_release_loader(dfu);
        /* The trigger's subscription is on the radio's link, which is not ours to drop. */
        inkwell_ble_set_notification_handler(dfu->client, NULL, NULL);
        inkwell_ble_requests_cancel(dfu->client);
        dfu_discovery(dfu, false);
    }
    mesh_dfu_package_free(&dfu->package);
}

static void dfu_finish(struct mesh_firmware_dfu *dfu, enum mesh_firmware_dfu_state state,
                       enum mesh_firmware_dfu_error error) {
    dfu_release(dfu);
    dfu->state = state;
    dfu->error = error;
    const mesh_firmware_dfu_done_fn on_done = dfu->on_done;
    void *const userdata = dfu->userdata;
    dfu->on_done = NULL;
    if (on_done != NULL) {
        on_done(userdata, dfu);
    }
}

static void dfu_fail(struct mesh_firmware_dfu *dfu, enum mesh_firmware_dfu_error error) {
    dfu->state = MESH_FIRMWARE_DFU_FAILED;
    dfu->error = error;
    inkwell_log_error("firmware", "The nRF52 BLE install failed: %s%s%s%s",
                      mesh_firmware_dfu_error_name(error), dfu->reason[0] != '\0' ? " (" : "",
                      dfu->reason, dfu->reason[0] != '\0' ? ")" : "");
    if (mesh_firmware_dfu_radio_in_loader(dfu)) {
        inkwell_log_error("firmware",
                          "The radio is in its DFU bootloader, off the mesh, with its application "
                          "erased; the same install started again resumes it. If the bootloader "
                          "has since restarted it is waiting on USB instead");
    }
    dfu_finish(dfu, MESH_FIRMWARE_DFU_FAILED, error);
}

static void dfu_refuse(struct mesh_firmware_dfu *dfu, enum mesh_firmware_dfu_error error) {
    dfu->on_done = NULL;
    dfu_release(dfu);
    dfu->state = MESH_FIRMWARE_DFU_FAILED;
    dfu->error = error;
}

/* ---- the steps -------------------------------------------------------------------------------
 */

static void dfu_enter_waiting(struct mesh_firmware_dfu *dfu, uint64_t now_ms, uint64_t settle_ms) {
    dfu->state = MESH_FIRMWARE_DFU_WAITING;
    dfu->deadline_ms = now_ms + DFU_LOADER_TIMEOUT_MS;
    dfu->next_poll_ms = now_ms;
    dfu->connect_after_ms = now_ms + settle_ms;
    dfu->loader_address[0] = '\0';
    dfu_discovery(dfu, true);
    inkwell_log_info("firmware", "Looking for the DFU bootloader of %s", dfu->radio_address);
}

static void dfu_retry(struct mesh_firmware_dfu *dfu, enum mesh_firmware_dfu_error error,
                      uint64_t now_ms) {
    dfu_release_loader(dfu);
    dfu->attempts += 1U;
    if (dfu->attempts >= MESH_FIRMWARE_DFU_ATTEMPTS) {
        dfu_fail(dfu, error);
        return;
    }
    inkwell_log_warn("firmware", "Trying the bootloader again (%u of %u) after: %s",
                     dfu->attempts + 1U, MESH_FIRMWARE_DFU_ATTEMPTS,
                     mesh_firmware_dfu_error_name(error));
    /* Long enough for the link that just broke to be gone: reconnecting a second later found it
       still going down, and the subscribe failed on it with -ENOTCONN. */
    dfu_enter_waiting(dfu, now_ms, DFU_REBOOT_MS);
}

/* Ignores whatever the running firmware's control point says: the trigger is not answered. */
static void dfu_trigger_notification(const uint8_t *data, size_t len, void *userdata) {
    (void)data;
    (void)len;
    (void)userdata;
}

static void dfu_tick_arming(struct mesh_firmware_dfu *dfu, uint64_t now_ms) {
    if (dfu->trigger_handle[0] == '\0') {
        const int found = inkwell_ble_find_characteristic(
            dfu->client, dfu->radio_address, MESH_BLE_DFU_CONTROL_UUID, dfu->trigger_handle,
            sizeof dfu->trigger_handle);
        if (found < 0) {
            snprintf(dfu->reason, sizeof dfu->reason,
                     "the radio's firmware has no DFU service (%d)", found);
            dfu_fail(dfu, MESH_FIRMWARE_DFU_ERROR_REFUSED);
            return;
        }
        inkwell_ble_set_notification_handler(dfu->client, dfu_trigger_notification, dfu);
    }
    /*
     * Notifications on before the write, and not as a courtesy: BLEDfu answers a START written
     * to a control point nobody is subscribed to with a CCCD error and does nothing. It also
     * wants an authenticated bond, so this is where a radio paired without a PIN says so.
     */
    if (!dfu->trigger_subscribed) {
        const int subscribed = inkwell_ble_subscribe(dfu->client, dfu->trigger_handle);
        if (subscribed == -EAGAIN) {
            goto deadline;
        }
        if (subscribed < 0) {
            snprintf(dfu->reason, sizeof dfu->reason,
                     "the radio would not let us at its DFU service (%d); pair it with its PIN",
                     subscribed);
            dfu_fail(dfu, MESH_FIRMWARE_DFU_ERROR_REFUSED);
            return;
        }
        dfu->trigger_subscribed = true;
    }
    if (!dfu->trigger_written) {
        static const uint8_t k_start[2] = {0x01U, 0x04U};
        const int wrote =
            inkwell_ble_write(dfu->client, dfu->trigger_handle, k_start, sizeof k_start);
        if (wrote != -EAGAIN) {
            dfu->trigger_written = true;
            dfu->trigger_result = wrote;
            inkwell_log_info("firmware", "Sent the radio into its bootloader (write %d)", wrote);
        }
    }
    /*
     * The write's reply can lose the race with the reset it causes, so it is not the answer:
     * the link going down is. Asked while the write is still out, for the same reason.
     */
    if (now_ms >= dfu->next_poll_ms) {
        dfu->next_poll_ms = now_ms + DFU_FAST_POLL_MS;
        bool connected = true;
        if (inkwell_ble_device_connected(dfu->client, dfu->radio_address, &connected) == 0 &&
            !connected) {
            inkwell_ble_requests_cancel(dfu->client);
            inkwell_ble_set_notification_handler(dfu->client, NULL, NULL);
            dfu->dropped = true;
            inkwell_log_info("firmware", "The radio went down for its bootloader");
            dfu_enter_waiting(dfu, now_ms, DFU_REBOOT_MS);
            return;
        }
    }

deadline:
    if (now_ms >= dfu->deadline_ms) {
        snprintf(dfu->reason, sizeof dfu->reason,
                 dfu->trigger_written ? "the radio did not restart into its bootloader (write %d)"
                                      : "the radio did not answer the DFU trigger (write %d)",
                 dfu->trigger_result);
        dfu_fail(dfu, MESH_FIRMWARE_DFU_ERROR_REFUSED);
    }
}

static void dfu_begin_connect(struct mesh_firmware_dfu *dfu, const char *address, uint64_t now_ms) {
    inkwell_str_copy(dfu->loader_address, sizeof dfu->loader_address, address);
    const bool same = strcasecmp(address, dfu->radio_address) == 0;
    inkwell_log_info("firmware", "Connecting to the bootloader at %s%s", address,
                     same ? " (bonded, the radio's own address)" : "");
    dfu->state = MESH_FIRMWARE_DFU_CONNECTING;
    dfu->deadline_ms = now_ms + DFU_CONNECT_TIMEOUT_MS;
    dfu->next_poll_ms = now_ms;
    dfu->connected = false;
    /*
     * The order the scan is stopped in follows whose object this is, as firmware_ota.c explains.
     * The radio's own address is a bonded device that outlives any scan, so the scan goes first
     * and the connect has the adapter to itself. The address plus one is a device bluetoothd
     * holds only while the discovery that found it runs, so that one connects first.
     */
    if (same) {
        dfu_discovery(dfu, false);
    }
    const int result = inkwell_ble_connect_begin(dfu->client, dfu->loader_address);
    if (result < 0) {
        inkwell_log_warn("firmware", "Connect to the bootloader would not start: %d", result);
        dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_CONNECT, now_ms);
        return;
    }
    dfu_discovery(dfu, false);
}

static void dfu_tick_waiting(struct mesh_firmware_dfu *dfu, uint64_t now_ms) {
    if (now_ms >= dfu->next_poll_ms) {
        dfu->next_poll_ms = now_ms + DFU_POLL_MS;
        /* An unbonded bootloader moves to the address plus one and says so in its
           advertisement; that one can be taken as soon as it is heard. */
        char plus_one[MESH_FIRMWARE_OTA_ADDRESS_MAX];
        struct inkwell_ble_device devices[DFU_SCAN_MAX];
        size_t count = 0U;
        if (mesh_firmware_ota_offset_address(dfu->radio_address, 1, plus_one, sizeof plus_one) &&
            inkwell_ble_list_by_service(dfu->client, MESH_BLE_DFU_SERVICE_UUID, devices,
                                        DFU_SCAN_MAX, &count) == 0) {
            for (size_t i = 0; i < count; ++i) {
                if (devices[i].in_range && strcasecmp(devices[i].address, plus_one) == 0) {
                    dfu_begin_connect(dfu, devices[i].address, now_ms);
                    return;
                }
            }
        }
    }
    /*
     * A bonded one keeps the radio's address and advertises to us alone, which a scan may never
     * list. So once it has had time to come up, it is connected to directly: the object is the
     * radio's, bonded, and the connect is what finds it.
     */
    if (now_ms >= dfu->connect_after_ms) {
        dfu_begin_connect(dfu, dfu->radio_address, now_ms);
        return;
    }
    if (now_ms >= dfu->deadline_ms) {
        dfu_fail(dfu, MESH_FIRMWARE_DFU_ERROR_NO_LOADER);
    }
}

/* The DFU version, when the characteristic is there: 1 is the application's own DFU service,
   which means the radio never went down or the stack is showing us its old table. */
static int dfu_check_version(struct mesh_firmware_dfu *dfu) {
    if (dfu->version_handle[0] == '\0' &&
        inkwell_ble_find_characteristic(dfu->client, dfu->loader_address, MESH_BLE_DFU_VERSION_UUID,
                                        dfu->version_handle, sizeof dfu->version_handle) < 0) {
        /* Optional in the protocol, and not a reason to stop. */
        return 0;
    }
    uint8_t value[8];
    size_t len = 0U;
    const int read = inkwell_ble_read(dfu->client, dfu->version_handle, value, sizeof value, &len);
    if (read == -EAGAIN) {
        return -EAGAIN;
    }
    if (read < 0 || len < 2U) {
        inkwell_log_warn("firmware", "Could not read the DFU version (%d); carrying on", read);
        return 0;
    }
    const unsigned version = (unsigned)value[0] | (unsigned)value[1] << 8;
    inkwell_log_info("firmware", "DFU version %u", version);
    if (version == MESH_BLE_DFU_VERSION_APPLICATION) {
        return -EALREADY;
    }
    return 0;
}

static void dfu_tick_connecting(struct mesh_firmware_dfu *dfu, uint64_t now_ms) {
    if (!dfu->connected) {
        int result = 0;
        const int polled = inkwell_ble_connect_poll(dfu->client, &result);
        if (polled == 1 && result < 0) {
            inkwell_log_warn("firmware", "The bootloader refused the connection: %d", result);
            dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_CONNECT, now_ms);
            return;
        }
        if (polled == 1) {
            dfu->connected = true;
            if (dfu->request_interval != NULL) {
                const int asked = dfu->request_interval(dfu->client, dfu->loader_address);
                if (asked != 0) {
                    inkwell_log_warn("firmware",
                                     "Could not ask for a fast connection interval (%d); the "
                                     "transfer will run at whatever the link has",
                                     asked);
                }
            }
        }
    }

    if (dfu->connected && now_ms >= dfu->next_poll_ms) {
        dfu->next_poll_ms = now_ms + DFU_FAST_POLL_MS;
        if (!dfu->resolved) {
            bool resolved = false;
            if (inkwell_ble_services_resolved(dfu->client, dfu->loader_address, &resolved) != 0 ||
                !resolved) {
                goto deadline;
            }
            dfu->resolved = true;
        }
        if (!dfu->version_checked) {
            const int version = dfu_check_version(dfu);
            if (version == -EAGAIN) {
                goto deadline;
            }
            if (version == -EALREADY) {
                inkwell_log_warn("firmware", "That was the radio's application, not its "
                                             "bootloader");
                dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_CONNECT, now_ms);
                return;
            }
            dfu->version_checked = true;
        }
        const int attached =
            mesh_ble_dfu_attach(&dfu->conversation, dfu->client, dfu->loader_address);
        if (attached == -EAGAIN) {
            goto deadline;
        }
        if (attached < 0) {
            dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_CONNECT, now_ms);
            return;
        }
        dfu->loader_seen = true;
        dfu->state = MESH_FIRMWARE_DFU_SENDING;
        dfu->next_poll_ms = now_ms + DFU_POLL_MS;
        if (mesh_ble_dfu_begin(&dfu->conversation, dfu->package.init, dfu->package.init_len,
                               dfu->package.image, dfu->package.image_len, now_ms) != 0) {
            dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_TRANSFER, now_ms);
        }
        return;
    }

deadline:
    if (now_ms >= dfu->deadline_ms) {
        dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_CONNECT, now_ms);
    }
}

static void dfu_enter_restarting(struct mesh_firmware_dfu *dfu, uint64_t now_ms) {
    inkwell_log_info("firmware", "Flashed %zu bytes at %u B/s; waiting for the radio to restart",
                     dfu->package.image_len,
                     (unsigned)mesh_ble_dfu_bytes_per_second(&dfu->conversation, now_ms));
    dfu_release_loader(dfu);
    dfu->state = MESH_FIRMWARE_DFU_RESTARTING;
    dfu->deadline_ms = now_ms + DFU_RESTART_TIMEOUT_MS;
    dfu->next_poll_ms = now_ms + DFU_POLL_MS;
    dfu_discovery(dfu, true);
}

static void dfu_tick_sending(struct mesh_firmware_dfu *dfu, uint64_t now_ms) {
    mesh_ble_dfu_tick(&dfu->conversation, now_ms);
    switch (dfu->conversation.state) {
    case MESH_BLE_DFU_DONE:
        dfu_enter_restarting(dfu, now_ms);
        return;
    case MESH_BLE_DFU_FAILED: {
        const struct mesh_ble_dfu *const talk = &dfu->conversation;
        /*
         * A broken transfer the bootloader is still holding, which it will not let go of over
         * BLE. RESET clears it - and on a stock Adafruit bootloader brings it back with no
         * application in *USB* DFU, which is what RESET did to a T1000-E on 2026-09-25. So this
         * stops and says where the way out is instead of trying the one that strands it.
         */
        if (talk->error == MESH_BLE_DFU_ERROR_STALE) {
            inkwell_str_copy(dfu->reason, sizeof dfu->reason,
                             "the bootloader holds a broken transfer; connect the radio by USB "
                             "and copy the UF2 to it");
            dfu_fail(dfu, MESH_FIRMWARE_DFU_ERROR_TRANSFER);
            return;
        }
        if (talk->error == MESH_BLE_DFU_ERROR_REFUSED) {
            snprintf(dfu->reason, sizeof dfu->reason, "the bootloader said %s",
                     mesh_ble_dfu_status_name(talk->status));
            /* Sending the same image again gets the same answer, except to a flash write that
               could not keep up. */
            if (talk->status != MESH_BLE_DFU_STATUS_OPERATION_FAILED) {
                dfu_fail(dfu, MESH_FIRMWARE_DFU_ERROR_LOADER_REFUSED);
                return;
            }
        } else {
            snprintf(dfu->reason, sizeof dfu->reason, "%s at %u%%",
                     mesh_ble_dfu_error_name(talk->error), mesh_ble_dfu_progress(talk));
        }
        dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_TRANSFER, now_ms);
        return;
    }
    default:
        break;
    }

    /* A link that drops mid-stream shows up as a receipt that never comes - thirty seconds -
       and asking BlueZ costs one poll. */
    if (now_ms >= dfu->next_poll_ms) {
        dfu->next_poll_ms = now_ms + DFU_POLL_MS;
        bool connected = true;
        if (inkwell_ble_device_connected(dfu->client, dfu->loader_address, &connected) == 0 &&
            !connected) {
            /* ACTIVATE is answered by the reset that drops the link. */
            if (dfu->conversation.state == MESH_BLE_DFU_ACTIVATING) {
                dfu_enter_restarting(dfu, now_ms);
                return;
            }
            inkwell_log_warn("firmware", "Lost the bootloader at %u%%",
                             mesh_ble_dfu_progress(&dfu->conversation));
            dfu->connected = false;
            snprintf(dfu->reason, sizeof dfu->reason, "the link dropped at %u%%",
                     mesh_ble_dfu_progress(&dfu->conversation));
            dfu_retry(dfu, MESH_FIRMWARE_DFU_ERROR_TRANSFER, now_ms);
        }
    }
}

static void dfu_tick_restarting(struct mesh_firmware_dfu *dfu, uint64_t now_ms) {
    if (now_ms < dfu->next_poll_ms) {
        return;
    }
    dfu->next_poll_ms = now_ms + DFU_POLL_MS;
    /*
     * Heard advertising at its address, under either service. Not the Meshtastic one alone: a
     * bonded bootloader shares the radio's address and BlueZ's record of it, and the services
     * BlueZ found on the bootloader replace the radio's there - so the radio comes back
     * listed under `1530` and nothing else until something connects and looks again. Seen on
     * a T1000-E, 2026-09-25: flashed, booted, advertising at -34 dBm, and reported as "no radio".
     * The bootloader resets as soon as it has activated the image rather than advertising, so
     * the address being heard in this scan is the application.
     */
    static const char *const k_services[] = {MESH_BLE_MESHTASTIC_SERVICE_UUID,
                                             MESH_BLE_DFU_SERVICE_UUID};
    bool back = false;
    for (size_t s = 0; s < sizeof k_services / sizeof k_services[0] && !back; ++s) {
        struct inkwell_ble_device devices[DFU_SCAN_MAX];
        size_t count = 0U;
        if (inkwell_ble_list_by_service(dfu->client, k_services[s], devices, DFU_SCAN_MAX,
                                        &count) != 0) {
            continue;
        }
        for (size_t i = 0; i < count && !back; ++i) {
            back = devices[i].in_range && strcasecmp(devices[i].address, dfu->radio_address) == 0;
        }
    }
    if (back) {
        inkwell_log_info("firmware", "The radio is back at %s", dfu->radio_address);
        dfu->radio_seen = true;
        dfu_finish(dfu, MESH_FIRMWARE_DFU_DONE, MESH_FIRMWARE_DFU_ERROR_NONE);
        return;
    }
    if (now_ms >= dfu->deadline_ms) {
        dfu_fail(dfu, MESH_FIRMWARE_DFU_ERROR_NO_RADIO);
    }
}

/* ---- the public half -------------------------------------------------------------------------
 */

int mesh_firmware_dfu_start(struct mesh_firmware_dfu *dfu,
                            const struct mesh_firmware_dfu_params *params) {
    if (dfu == NULL) {
        return -EINVAL;
    }
    if (mesh_firmware_dfu_busy(dfu)) {
        return -EBUSY;
    }
    memset(dfu, 0, sizeof *dfu);
    if (params == NULL || params->client == NULL || params->package_path == NULL ||
        params->package_path[0] == '\0' || params->radio_address == NULL ||
        params->radio_address[0] == '\0') {
        dfu_refuse(dfu, MESH_FIRMWARE_DFU_ERROR_UNAVAILABLE);
        return -EINVAL;
    }

    size_t len = 0U;
    uint8_t *const zip =
        inkwell_file_read(params->package_path, MESH_FIRMWARE_DFU_PACKAGE_MAX, &len);
    if (zip == NULL) {
        inkwell_log_error("firmware", "The staged package could not be read: %s",
                          params->package_path);
        dfu_refuse(dfu, MESH_FIRMWARE_DFU_ERROR_UNAVAILABLE);
        return -EIO;
    }
    const enum mesh_dfu_package_verdict verdict = mesh_dfu_package_read(zip, len, &dfu->package);
    free(zip);
    if (verdict != MESH_DFU_PACKAGE_OK) {
        inkwell_log_error("firmware", "The staged package is not a DFU package for this board: %s",
                          mesh_dfu_package_verdict_name(verdict));
        inkwell_str_copy(dfu->reason, sizeof dfu->reason, mesh_dfu_package_verdict_name(verdict));
        dfu_refuse(dfu, verdict == MESH_DFU_PACKAGE_NO_MEMORY
                            ? MESH_FIRMWARE_DFU_ERROR_UNAVAILABLE
                            : MESH_FIRMWARE_DFU_ERROR_WRONG_IMAGE);
        return -EINVAL;
    }
    inkwell_log_info("firmware",
                     "DFU package: %zu-byte image, %zu-byte init packet, device type %#x, "
                     "SoftDevice %#x, CRC16 %04x%s",
                     dfu->package.image_len, dfu->package.init_len,
                     (unsigned)dfu->package.device_type, (unsigned)dfu->package.softdevice,
                     (unsigned)dfu->package.crc16, dfu->package.crc_checked ? " (matches)" : "");

    dfu->client = params->client;
    inkwell_str_copy(dfu->radio_address, sizeof dfu->radio_address, params->radio_address);
    dfu->request_interval = params->request_interval;
    dfu->on_done = params->on_done;
    dfu->userdata = params->userdata;
    dfu->state = params->arm ? MESH_FIRMWARE_DFU_ARMING : MESH_FIRMWARE_DFU_WAITING;
    if (!params->arm) {
        inkwell_log_info("firmware", "Resuming at the bootloader of %s", dfu->radio_address);
    }
    /* The clocks start on the first tick, which is where the time comes from. */
    return 0;
}

void mesh_firmware_dfu_tick(struct mesh_firmware_dfu *dfu, uint64_t now_ms) {
    if (!mesh_firmware_dfu_busy(dfu)) {
        return;
    }
    if (!dfu->started) {
        dfu->started = true;
        if (dfu->state == MESH_FIRMWARE_DFU_WAITING) {
            /* Already in its bootloader: nothing to wait out before connecting. */
            dfu->dropped = true;
            dfu_enter_waiting(dfu, now_ms, 0U);
            return;
        }
        dfu->deadline_ms = now_ms + DFU_ARM_TIMEOUT_MS;
        dfu->next_poll_ms = now_ms + DFU_FAST_POLL_MS;
    }
    switch (dfu->state) {
    case MESH_FIRMWARE_DFU_ARMING:
        dfu_tick_arming(dfu, now_ms);
        break;
    case MESH_FIRMWARE_DFU_WAITING:
        dfu_tick_waiting(dfu, now_ms);
        break;
    case MESH_FIRMWARE_DFU_CONNECTING:
        dfu_tick_connecting(dfu, now_ms);
        break;
    case MESH_FIRMWARE_DFU_SENDING:
        dfu_tick_sending(dfu, now_ms);
        break;
    case MESH_FIRMWARE_DFU_RESTARTING:
        dfu_tick_restarting(dfu, now_ms);
        break;
    default:
        break;
    }
}

void mesh_firmware_dfu_cancel(struct mesh_firmware_dfu *dfu) {
    if (dfu == NULL) {
        return;
    }
    dfu_release(dfu);
    dfu->on_done = NULL;
    dfu->userdata = NULL;
    dfu->state = MESH_FIRMWARE_DFU_IDLE;
    dfu->error = MESH_FIRMWARE_DFU_ERROR_NONE;
}
