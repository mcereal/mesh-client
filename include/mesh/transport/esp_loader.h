#pragma once

/*
 * The conversation with an ESP32's ROM bootloader over a USB serial bridge: from a port to
 * flash that holds the given images and a chip running them.
 *
 * This is the serial counterpart of ble_ota.h, and the difference between them is the reason
 * it exists. The BLE loader is a partition the *firmware* ships, reached through an admin
 * verb the firmware understands - so it only helps a radio already running firmware that has
 * one. The ROM bootloader is in the chip's mask ROM. It answers whatever the flash holds -
 * Meshtastic, MeshCore, nothing, half an image - which makes it the one route that can move a
 * board from one firmware to another, or bring back one a failed write left blank.
 *
 *   resetting   two control lines, as esptool's classic reset drives them: RTS alone (EN
 *               low: in reset), then DTR alone (EN released with the boot strap held low),
 *               then neither. On the transistor pair nearly every bridge board carries, that
 *               is a reset into download mode. Nothing else on the chip is asked.
 *   syncing     SYNC until one is answered, and the reset again if none is.
 *   identifying the chip's magic word, refused when it is not the chip the images are for.
 *   baud        CHANGE_BAUDRATE, then this side follows. Only worth it up to a point: the ROM
 *               writes each 1 KB block before answering it, so the flash, not the wire, is the
 *               ceiling - 460800 and 921600 both took 37 s over 644 KB on a Heltec V3.
 *   attaching   SPI_ATTACH and SPI_SET_PARAMS, so the ROM reaches the flash at all.
 *   writing     per region, FLASH_BEGIN (which erases before it answers), then its blocks,
 *               then SPI_FLASH_MD5 against the digest taken before the first was sent.
 *   restarting  RTS alone and released: a plain reset, with the strap high, into the new image.
 *
 * **An interrupted write is safe to start again**, which is the property BLE does not have.
 * The ROM is still in the mask whatever the flash holds, so every failure here leaves a chip
 * the same conversation can reach - there is no loader to be stranded in.
 *
 * A native-USB ESP32 (its own USB Serial/JTAG, VID 0x303A) is reset by a different sequence
 * and re-enumerates on the way; this speaks only to a bridge. The caller decides which a port
 * is - `enum inkwell_serial_kind` says - before starting.
 *
 * It sits on inkwell's serial port and codec, not on mesh_session: a ROM bootloader is not a
 * node and speaks no protocol a node does.
 */

#include "inkwell/codec/esp_rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* A bootloader, a partition table and an application, or one merged image, or an application
   and the otadata erased beside it: four covers every layout a release ships. */
#define MESH_ESP_LOADER_REGIONS 4U
/* The rate the write runs at once the ROM is found. The ROM is the ceiling above this. */
#define MESH_ESP_LOADER_WRITE_BAUD 460800U
/* Accepts whichever chip answers. */
#define MESH_ESP_LOADER_ANY_CHIP 0xFFFFU
#define MESH_ESP_LOADER_REASON_MAX 96U

enum mesh_esp_loader_state {
    MESH_ESP_LOADER_IDLE = 0,
    MESH_ESP_LOADER_RESETTING,
    MESH_ESP_LOADER_SYNCING,
    MESH_ESP_LOADER_IDENTIFYING,
    MESH_ESP_LOADER_BAUD,
    MESH_ESP_LOADER_ATTACHING,
    MESH_ESP_LOADER_ERASING,
    MESH_ESP_LOADER_WRITING,
    MESH_ESP_LOADER_VERIFYING,
    MESH_ESP_LOADER_RESTARTING,
    MESH_ESP_LOADER_DONE,
    MESH_ESP_LOADER_FAILED,
    MESH_ESP_LOADER_STATE_COUNT,
};

enum mesh_esp_loader_error {
    MESH_ESP_LOADER_ERROR_NONE = 0,
    /* The port would not open, or its lines would not move. Nothing was asked of the chip. */
    MESH_ESP_LOADER_ERROR_PORT,
    /* No SYNC was ever answered: not an ESP32, not a board whose bridge drives EN and the
       strap, or a chip held in reset by something else on the port. Untouched. */
    MESH_ESP_LOADER_ERROR_SILENT,
    /* The ROM answered and is not the chip these images are for. Untouched. */
    MESH_ESP_LOADER_ERROR_WRONG_CHIP,
    /* The ROM refused a request; `rom_error` says why in its own number. */
    MESH_ESP_LOADER_ERROR_REFUSED,
    /* An answer that did not come in time. */
    MESH_ESP_LOADER_ERROR_TIMEOUT,
    /* Every block was taken and the flash does not hash to what was sent. */
    MESH_ESP_LOADER_ERROR_VERIFY,
    /* The port went away mid-conversation - unplugged, or reset out from under us. */
    MESH_ESP_LOADER_ERROR_IO,
    MESH_ESP_LOADER_ERROR_COUNT,
};

/* One contiguous write. `data` is borrowed until the conversation ends. */
struct mesh_esp_loader_region {
    uint32_t offset;
    const uint8_t *data;
    size_t len;
};

struct mesh_esp_loader {
    struct inkwell_loop *loop; /* borrowed; NULL means the tick is the only pump */
    int fd;
    bool port_open;
    bool watched;

    enum mesh_esp_loader_state state;
    enum mesh_esp_loader_error error;
    uint8_t rom_error;
    char reason[MESH_ESP_LOADER_REASON_MAX];

    uint16_t expect_chip;
    uint16_t chip;
    unsigned baud;
    uint32_t flash_size;

    struct mesh_esp_loader_region regions[MESH_ESP_LOADER_REGIONS];
    uint8_t digests[MESH_ESP_LOADER_REGIONS][16];
    size_t region_count;
    size_t region;
    uint32_t block;
    size_t bytes_total;
    size_t bytes_written;

    struct inkwell_esp_rom_reader reader;
    uint8_t tx[INKWELL_ESP_ROM_REQUEST_MAX];
    size_t tx_len;
    size_t tx_sent;
    /* The op whose answer is due; an answer to any other is a leftover and is ignored. */
    uint8_t awaiting;

    unsigned step;     /* where in the reset sequence, or which SYNC this is */
    unsigned attempts; /* resets tried */
    uint64_t step_at_ms;
    uint64_t deadline_ms;
    uint64_t started_ms;
    uint64_t finished_ms;
};

/*
 * Opens `path` and starts: reset, sync, identify, write every region in order, verify each,
 * reset into it. `expect_chip` is an `INKWELL_ESP_CHIP_*`, or MESH_ESP_LOADER_ANY_CHIP.
 * `flash_size` is what SPI_SET_PARAMS tells the ROM, and 0 lets the regions decide: the
 * smallest power of two from 4 MB up that holds the furthest one.
 *
 * With a `loop`, the port is watched and every answer is acted on as it lands - a 20 ms tick
 * would add a third to every block's round trip. Without one, the tick does it all.
 *
 * Returns 0, or -EINVAL for no regions, an empty one or more than MESH_ESP_LOADER_REGIONS, or
 * a negative errno from the open (and `error` says PORT).
 */
int mesh_esp_loader_start(struct mesh_esp_loader *loader, struct inkwell_loop *loop,
                          const char *path, uint16_t expect_chip,
                          const struct mesh_esp_loader_region *regions, size_t region_count,
                          uint32_t flash_size, uint64_t now_ms);

/* The timers: the reset's steps, SYNC's retries, every deadline. Call every loop turn. */
void mesh_esp_loader_tick(struct mesh_esp_loader *loader, uint64_t now_ms);

/* Reads whatever the port has and acts on it. The loop's callback is this; a caller without a
   loop need not call it, since the tick does. */
void mesh_esp_loader_pump(struct mesh_esp_loader *loader, uint64_t now_ms);

/* Closes the port and stops. A chip left in download mode stays there until reset - which is
   harmless: the next start resets it, and so does a power cycle. Safe on a zeroed struct. */
void mesh_esp_loader_cancel(struct mesh_esp_loader *loader);

bool mesh_esp_loader_busy(const struct mesh_esp_loader *loader);
/* 0-100 over the bytes written, erases and verification counted as nothing. */
unsigned mesh_esp_loader_progress(const struct mesh_esp_loader *loader);

const char *mesh_esp_loader_state_name(enum mesh_esp_loader_state state);
const char *mesh_esp_loader_error_name(enum mesh_esp_loader_error error);

#ifdef __cplusplus
}
#endif
