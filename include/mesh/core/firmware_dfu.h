#pragma once

/*
 * The nRF52 BLE handover: from a staged `-ota.zip` to a radio running what is in it - the
 * counterpart of firmware_ota.h, which is the same job for an ESP32.
 *
 *   arming      the running firmware's own DFU service - Adafruit's `BLEDfu`, which Meshtastic
 *               keeps behind an authenticated bond - is sent START over the link that is already
 *               up. The radio hands its bond to the bootloader and reboots into it. Nothing is
 *               asked of mesh_session: this is a GATT write, not an admin verb.
 *   waiting     the link drops and the bootloader comes up. **At the radio's own address**, not
 *               the address plus one the ESP32 loader uses: a bonded bootloader keeps the address
 *               and the bond so the link can encrypt, and advertises only to the peer it was
 *               handed. An unbonded one does move to plus one, and is looked for there too.
 *   connecting  connect, ask for 7.5 ms, wait for services, read the DFU version to be sure it is
 *               the bootloader answering and not the application again.
 *   sending     the Legacy DFU conversation (mesh/transport/ble_dfu.h), which ends with the
 *               bootloader having checked the image against the init packet's CRC16.
 *   restarting  the radio advertising Meshtastic again where it was.
 *
 * **What is at stake is different from the ESP32's.** The bootloader erases the application
 * before it takes the first byte, so from START the radio has no firmware to go back to - and a
 * stock Adafruit bootloader that resets with no application comes up in *USB* DFU, not BLE. While
 * it stays up it waits in BLE DFU for as long as it is left there, and a broken transfer is
 * picked up again from the start; that is what the retries are for. `radio_in_loader()` says
 * when an install ended with it there.
 */

#include "mesh/core/dfu_package.h"
#include "mesh/core/firmware_ota.h"
#include "mesh/transport/ble_dfu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_ble_central;

/* The -ota.zip: an image under 1 MB and a few hundred bytes around it. */
#define MESH_FIRMWARE_DFU_PACKAGE_MAX (2U * 1024U * 1024U)
/* Whole transfers before giving up, each from START: a legacy bootloader has no resume. */
#define MESH_FIRMWARE_DFU_ATTEMPTS 3U

enum mesh_firmware_dfu_state {
    MESH_FIRMWARE_DFU_IDLE = 0,
    MESH_FIRMWARE_DFU_ARMING,
    MESH_FIRMWARE_DFU_WAITING,
    MESH_FIRMWARE_DFU_CONNECTING,
    MESH_FIRMWARE_DFU_SENDING,
    MESH_FIRMWARE_DFU_RESTARTING,
    MESH_FIRMWARE_DFU_DONE,
    MESH_FIRMWARE_DFU_FAILED,
    MESH_FIRMWARE_DFU_STATE_COUNT,
};

enum mesh_firmware_dfu_error {
    MESH_FIRMWARE_DFU_ERROR_NONE = 0,
    /* A bad argument, or a package that could not be read. Nothing was started. */
    MESH_FIRMWARE_DFU_ERROR_UNAVAILABLE,
    /* Not a DFU package, or not one whose image matches its own init packet. Refused before the
       radio is asked anything. */
    MESH_FIRMWARE_DFU_ERROR_WRONG_IMAGE,
    /* The radio has no DFU service, would not let us write to it, or did not reboot when told
       to - an unauthenticated bond, most often. It is untouched and still running. */
    MESH_FIRMWARE_DFU_ERROR_REFUSED,
    /* No bootloader came up. */
    MESH_FIRMWARE_DFU_ERROR_NO_LOADER,
    /* The bootloader was there and would not take a connection, every attempt. */
    MESH_FIRMWARE_DFU_ERROR_CONNECT,
    /* The transfer broke every attempt. */
    MESH_FIRMWARE_DFU_ERROR_TRANSFER,
    /* The bootloader refused the image in a way sending it again will not change: too big, not
       supported, or a CRC that does not match. */
    MESH_FIRMWARE_DFU_ERROR_LOADER_REFUSED,
    /* The bootloader took the image and the radio never came back where it was. */
    MESH_FIRMWARE_DFU_ERROR_NO_RADIO,
    MESH_FIRMWARE_DFU_ERROR_COUNT,
};

struct mesh_firmware_dfu;

typedef void (*mesh_firmware_dfu_done_fn)(void *userdata, const struct mesh_firmware_dfu *dfu);

struct mesh_firmware_dfu_params {
    struct inkwell_ble_central *client; /* borrowed; the install's own, not the transport's */
    const char *package_path;
    /* The radio's BLE address. Required: the bootloader takes it over. */
    const char *radio_address;
    /* False for a radio already in its bootloader, which is not sent the trigger. */
    bool arm;
    mesh_firmware_ota_interval_fn request_interval; /* NULL: leave the interval alone */
    mesh_firmware_dfu_done_fn on_done;
    void *userdata;
};

struct mesh_firmware_dfu {
    struct inkwell_ble_central *client;
    enum mesh_firmware_dfu_state state;
    enum mesh_firmware_dfu_error error;
    char reason[MESH_FIRMWARE_OTA_REASON_MAX];

    struct mesh_dfu_package package;

    char radio_address[MESH_FIRMWARE_OTA_ADDRESS_MAX];
    /* Where the bootloader answered: the radio's address when bonded, plus one when not. */
    char loader_address[MESH_FIRMWARE_OTA_ADDRESS_MAX];

    /* Arming's steps, in order. */
    char trigger_handle[INKWELL_BLE_HANDLE_MAX];
    bool trigger_subscribed;
    bool trigger_written;
    int trigger_result;
    bool dropped; /* the radio's link went down after the trigger */

    /* The bootloader was found, or the trigger was taken - either way the application may be
       gone from here on. */
    bool loader_seen;
    bool radio_seen;

    struct mesh_ble_dfu conversation;
    char version_handle[INKWELL_BLE_HANDLE_MAX];
    bool version_checked;
    bool resolved;
    unsigned attempts;
    bool started;
    bool discovering;
    bool connected;

    uint64_t deadline_ms;
    uint64_t next_poll_ms;
    uint64_t connect_after_ms;

    mesh_firmware_ota_interval_fn request_interval;
    mesh_firmware_dfu_done_fn on_done;
    void *userdata;
};

/*
 * Reads and checks the package and, with `arm`, sends the radio into its bootloader. 0 or
 * -errno; on 0 `on_done` is called exactly once, later, from a tick. On a negative return nothing
 * was started and `state` and `error` say why.
 */
int mesh_firmware_dfu_start(struct mesh_firmware_dfu *dfu,
                            const struct mesh_firmware_dfu_params *params);

void mesh_firmware_dfu_tick(struct mesh_firmware_dfu *dfu, uint64_t now_ms);

/* Stops, disconnects and frees the package, reporting nothing. Safe on a zeroed struct. */
void mesh_firmware_dfu_cancel(struct mesh_firmware_dfu *dfu);

bool mesh_firmware_dfu_busy(const struct mesh_firmware_dfu *dfu);
bool mesh_firmware_dfu_holds_the_radio(const struct mesh_firmware_dfu *dfu);

/* True when the install ended with the radio in its bootloader and the application erased, or
   possibly so: off the mesh until the same install is run again. */
bool mesh_firmware_dfu_radio_in_loader(const struct mesh_firmware_dfu *dfu);

/* 0-100 over the image. */
unsigned mesh_firmware_dfu_progress(const struct mesh_firmware_dfu *dfu);

const char *mesh_firmware_dfu_state_name(enum mesh_firmware_dfu_state state);
const char *mesh_firmware_dfu_error_name(enum mesh_firmware_dfu_error error);

#ifdef __cplusplus
}
#endif
