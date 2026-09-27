#pragma once

/*
 * The serial handover: from a staged ESP32 app image to a radio running it, through the chip's
 * ROM bootloader - the ESP32's USB path, as firmware_install.h is the nRF52's.
 *
 * It is the one handover that asks nothing of the firmware on the radio. The UF2 path needs an
 * admin verb to send the board into its bootloader and the BLE path an admin verb and a loader
 * partition; this needs two control lines, which reach the ROM whatever the flash holds (see
 * mesh/transport/esp_loader.h). So there is no arm callback and nothing waits on the session:
 * the link lets go of the port, the loader takes it, and the radio restarts into the new image.
 *
 * Two regions go down, in this order:
 *
 *   0x10000  the app image, into `app0`
 *   0xE000   `otadata`, erased
 *
 * `app0` at 0x10000 is where the Arduino ESP32 partition tables put it, and every ESP32 board
 * both Meshtastic and MeshCore publish uses one of those - the offsets are in the `.mt.json`'s
 * own partition list and have never differed. Erasing `otadata` is what makes the write
 * *count*: a radio last updated over BLE may be booting `app1`, and blank `otadata` sends the
 * bootloader to the first app partition. `nvs` is not touched, so settings and keys survive.
 *
 * **A failed write is a radio in its loader**, in the sense the banner means: off the mesh until
 * a write finishes. The ROM is still there - nothing is stranded the way the BLE loader can
 * strand a radio - but an app half-written will not boot, so once the first erase has gone out
 * a failure says so, and the same install started again finishes it.
 *
 * A native-USB ESP32 (VID 0x303A) resets a different way and is refused with its own error
 * rather than tried; see esp_loader.h.
 */

#include "mesh/transport/esp_loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* Clear of the largest app partition either firmware ships (3.2 MB on an 8 MB part). */
#define MESH_FIRMWARE_SERIAL_IMAGE_MAX (8U * 1024U * 1024U)
#define MESH_FIRMWARE_SERIAL_APP_OFFSET 0x10000U
#define MESH_FIRMWARE_SERIAL_OTADATA_OFFSET 0xE000U
#define MESH_FIRMWARE_SERIAL_OTADATA_LEN 0x2000U
#define MESH_FIRMWARE_SERIAL_WHERE_MAX 64U
#define MESH_FIRMWARE_SERIAL_REASON_MAX 128U

enum mesh_firmware_serial_state {
    MESH_FIRMWARE_SERIAL_IDLE = 0,
    /* The loader is resetting the chip into its ROM and finding it there. */
    MESH_FIRMWARE_SERIAL_WAITING,
    /* Erasing, writing and verifying. */
    MESH_FIRMWARE_SERIAL_WRITING,
    /* The last region verified; the chip is being reset into it. */
    MESH_FIRMWARE_SERIAL_RESTARTING,
    MESH_FIRMWARE_SERIAL_DONE,
    MESH_FIRMWARE_SERIAL_FAILED,
    MESH_FIRMWARE_SERIAL_STATE_COUNT,
};

enum mesh_firmware_serial_error {
    MESH_FIRMWARE_SERIAL_ERROR_NONE = 0,
    /* The staged image could not be read, or is not an app image for this chip. Nothing was
       asked of the radio. */
    MESH_FIRMWARE_SERIAL_ERROR_IMAGE,
    /* No port to write through: the one named is gone, or with none named, there is not
       exactly one bridge to choose. */
    MESH_FIRMWARE_SERIAL_ERROR_NO_PORT,
    /* The port is the chip's own USB, which this does not reset. */
    MESH_FIRMWARE_SERIAL_ERROR_NATIVE_USB,
    /* The loader failed; `loader.error` says how. */
    MESH_FIRMWARE_SERIAL_ERROR_LOADER,
    MESH_FIRMWARE_SERIAL_ERROR_COUNT,
};

struct mesh_firmware_serial {
    enum mesh_firmware_serial_state state;
    enum mesh_firmware_serial_error error;
    char reason[MESH_FIRMWARE_SERIAL_REASON_MAX];
    /* The tty the loader opened, for the log and for a row. */
    char path[MESH_FIRMWARE_SERIAL_WHERE_MAX];

    struct mesh_esp_loader loader;
    uint8_t *image;
    size_t image_len;
    uint8_t otadata[MESH_FIRMWARE_SERIAL_OTADATA_LEN];
    /* The first erase went out: from here a failure leaves an app that will not boot. */
    bool flash_touched;
    /* An earlier attempt on this struct erased the app and none has finished since. It keeps
       a failed retry a radio in its loader, and lets a named port that is gone fall back to
       the only bridge. */
    bool damaged;
};

/*
 * Reads `image_path`, checks it is an app image for `chip` (an `INKWELL_ESP_CHIP_*`), finds
 * the port and starts the loader. `where` is the serial transport's own id for the port the
 * radio was on; empty means "the only bridge there is". A named port that is gone is refused,
 * unless an earlier attempt on `serial` left the app erased - then the only bridge is taken,
 * since a resume on another socket is another id.
 *
 * The link must already have let go of the port. Returns 0, or a negative errno with `state`
 * FAILED and `error` saying which refusal - on a negative return nothing was sent to the radio.
 */
int mesh_firmware_serial_start(struct mesh_firmware_serial *serial, struct inkwell_loop *loop,
                               const char *image_path, const char *where, uint16_t chip,
                               uint64_t now_ms);

void mesh_firmware_serial_tick(struct mesh_firmware_serial *serial, uint64_t now_ms);
/* Stops, closes the port and frees the image. Safe on a zeroed struct. */
void mesh_firmware_serial_cancel(struct mesh_firmware_serial *serial);

bool mesh_firmware_serial_busy(const struct mesh_firmware_serial *serial);
/* Busy, and so the port and the radio are the loader's. */
bool mesh_firmware_serial_holds_the_radio(const struct mesh_firmware_serial *serial);
/* Failed after this attempt's first erase, or an earlier one's: the radio will not boot until
   a write finishes. */
bool mesh_firmware_serial_radio_in_loader(const struct mesh_firmware_serial *serial);
unsigned mesh_firmware_serial_progress(const struct mesh_firmware_serial *serial);

const char *mesh_firmware_serial_error_name(enum mesh_firmware_serial_error error);

#ifdef __cplusplus
}
#endif
