#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_install.h"

#include "mesh/transport/serial_usb.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How often the USB tree is worth reading. A re-enumeration takes about a second and the drive
   arrives about a second after that, so half a second is four looks at the interesting part
   and no busy loop in between. */
#define INSTALL_POLL_MS 500U
/* How long a radio gets to go away and come back as a bootloader. Generous, because the same
   deadline covers the double-tap the user does by hand when the admin verb never lands. */
#define INSTALL_ARM_TIMEOUT_MS 20000U
#define INSTALL_WAIT_TIMEOUT_MS 60000U
/* And how long the bootloader gets to reset itself once it has every block. Measured at 1 s on
   a T114; this is twenty times that. */
#define INSTALL_RESTART_TIMEOUT_MS 20000U

static const char *const k_state_names[MESH_FIRMWARE_INSTALL_STATE_COUNT] = {
    "idle", "arming", "waiting", "writing", "restarting", "done", "failed",
};

static const char *const k_error_names[MESH_FIRMWARE_INSTALL_ERROR_COUNT] = {
    "none", "unavailable", "wrong image", "arm", "no bootloader", "mounted", "write", "no restart",
};

const char *mesh_firmware_install_state_name(enum mesh_firmware_install_state state) {
    return state < MESH_FIRMWARE_INSTALL_STATE_COUNT ? k_state_names[state] : "?";
}

const char *mesh_firmware_install_error_name(enum mesh_firmware_install_error error) {
    return error < MESH_FIRMWARE_INSTALL_ERROR_COUNT ? k_error_names[error] : "?";
}

/* ---- the shape every step ends in ----------------------------------------------------------
 */

static void install_release(struct mesh_firmware_install *install) {
    mesh_usb_msc_write_cancel(&install->write);
    free(install->image);
    install->image = NULL;
    install->image_len = 0U;
}

static void install_finish(struct mesh_firmware_install *install,
                           enum mesh_firmware_install_state state,
                           enum mesh_firmware_install_error error) {
    install_release(install);
    install->state = state;
    install->error = error;
    const mesh_firmware_install_done_fn on_done = install->on_done;
    void *const userdata = install->userdata;
    install->on_done = NULL;
    if (on_done != NULL) {
        on_done(userdata, install);
    }
}

/*
 * A start that never started: `state` and `error` say why, and no callback will arrive.
 *
 * Separate from install_fail() because that one reports, and a refusal from start() must not -
 * the caller is still inside its own call and learns the outcome from the return value. What
 * they share is the contract that a refusal is a row rather than silence, which is why every
 * negative return from start() comes through here.
 */
static void install_refuse(struct mesh_firmware_install *install,
                           enum mesh_firmware_install_error error) {
    install_release(install);
    install->state = MESH_FIRMWARE_INSTALL_FAILED;
    install->error = error;
    install->on_done = NULL;
}

static void install_fail(struct mesh_firmware_install *install,
                         enum mesh_firmware_install_error error) {
    mesh_log_error("firmware", "The install failed: %s", mesh_firmware_install_error_name(error));
    install_finish(install, MESH_FIRMWARE_INSTALL_FAILED, error);
}

/* ---- reading the USB tree ------------------------------------------------------------------
 */

/* "2-1:1.1" names an interface of the USB device "2-1". A board resets in place, so the device
   is what stays the same across a re-enumeration; the interface numbers need not. */
static bool install_port_of(const char *interface_id, char *out, size_t out_len) {
    if (interface_id == NULL || interface_id[0] == '\0') {
        return false;
    }
    const char *const colon = strchr(interface_id, ':');
    const size_t len = colon != NULL ? (size_t)(colon - interface_id) : strlen(interface_id);
    if (len == 0U || len >= out_len) {
        return false;
    }
    memcpy(out, interface_id, len);
    out[len] = '\0';
    return true;
}

static bool install_on_our_port(const struct mesh_firmware_install *install,
                                const struct mesh_serial_device_info *device) {
    if (install->port[0] == '\0') {
        return true;
    }
    char port[64];
    return install_port_of(device->id, port, sizeof port) && strcmp(port, install->port) == 0;
}

/* Finds the bootloader this install is waiting for, or NULL. */
static bool install_find_bootloader(const struct mesh_firmware_install *install,
                                    struct mesh_serial_device_info *out) {
    struct mesh_serial_device_info devices[MESH_SERIAL_MAX_DEVICES];
    const size_t count = mesh_serial_usb_scan(devices, MESH_SERIAL_MAX_DEVICES);
    for (size_t i = 0; i < count; ++i) {
        if (devices[i].role == MESH_SERIAL_ROLE_BOOTLOADER &&
            install_on_our_port(install, &devices[i])) {
            *out = devices[i];
            return true;
        }
    }
    return false;
}

/* True while a radio - anything that is not a bootloader - is still answering on our port. What
   says the admin verb landed is this going false. */
static bool install_radio_still_there(const struct mesh_firmware_install *install) {
    if (install->port[0] == '\0') {
        return false;
    }
    struct mesh_serial_device_info devices[MESH_SERIAL_MAX_DEVICES];
    const size_t count = mesh_serial_usb_scan(devices, MESH_SERIAL_MAX_DEVICES);
    for (size_t i = 0; i < count; ++i) {
        if (devices[i].role != MESH_SERIAL_ROLE_BOOTLOADER &&
            install_on_our_port(install, &devices[i])) {
            return true;
        }
    }
    return false;
}

/* ---- the steps -----------------------------------------------------------------------------
 */

static void install_enter_waiting(struct mesh_firmware_install *install, uint64_t now_ms) {
    install->state = MESH_FIRMWARE_INSTALL_WAITING;
    install->deadline_ms = now_ms + INSTALL_WAIT_TIMEOUT_MS;
    install->next_poll_ms = now_ms;
    mesh_log_info("firmware", "Waiting for a bootloader to enumerate");
}

static void install_begin_write(struct mesh_firmware_install *install,
                                const struct mesh_serial_device_info *bootloader, uint64_t now_ms) {
    if (mesh_usb_msc_find(bootloader, &install->target) != 0 || install->target.device[0] == '\0') {
        /* The interface is there and `usb-storage` has not published the disk yet, which is the
           ordinary answer for about a second. Keep waiting rather than failing. */
        return;
    }

    /*
     * Off its mountpoints before a byte goes out. On the Brick the platform has mounted the
     * ghost FAT over /mnt/SDCARD by now, and a write racing the VFAT driver over one device
     * interleaves FAT sectors with our blocks - they are discarded for want of the magic, so
     * the radio is safe either way, but it is not a thing to leave in. Taking the shadow off is
     * also what puts the SD card back, because it was stacked on top of it.
     */
    const int unmounted = mesh_usb_msc_unmount(&install->target);
    if (unmounted != 0) {
        install_fail(install, MESH_FIRMWARE_INSTALL_ERROR_MOUNTED);
        return;
    }

    const int started =
        mesh_usb_msc_write_start(&install->write, install->loop, install->image, install->image_len,
                                 install->target.device, now_ms);
    if (started != 0) {
        install_fail(install, MESH_FIRMWARE_INSTALL_ERROR_WRITE);
        return;
    }
    install->state = MESH_FIRMWARE_INSTALL_WRITING;
    mesh_log_info("firmware", "Writing %zu bytes of firmware to %s", install->image_len,
                  install->target.device);
}

static void install_tick_arming(struct mesh_firmware_install *install, uint64_t now_ms) {
    if (now_ms < install->next_poll_ms) {
        return;
    }
    install->next_poll_ms = now_ms + INSTALL_POLL_MS;

    struct mesh_serial_device_info bootloader;
    if (install_find_bootloader(install, &bootloader)) {
        install_enter_waiting(install, now_ms);
        install_begin_write(install, &bootloader, now_ms);
        return;
    }
    if (!install_radio_still_there(install)) {
        /* The port went away, which is the reply this verb has: the radio is on its way into
           its bootloader and has not enumerated as one yet. */
        install_enter_waiting(install, now_ms);
        return;
    }
    if (now_ms >= install->deadline_ms) {
        /* The radio is still sitting there running firmware, so the verb never took. That is a
           refusal about the radio rather than about the bootloader, and it leaves the board
           exactly as it was. */
        install_fail(install, MESH_FIRMWARE_INSTALL_ERROR_ARM);
    }
}

static void install_tick_waiting(struct mesh_firmware_install *install, uint64_t now_ms) {
    if (now_ms >= install->next_poll_ms) {
        install->next_poll_ms = now_ms + INSTALL_POLL_MS;
        struct mesh_serial_device_info bootloader;
        if (install_find_bootloader(install, &bootloader)) {
            install_begin_write(install, &bootloader, now_ms);
            return;
        }
    }
    if (now_ms >= install->deadline_ms) {
        install_fail(install, MESH_FIRMWARE_INSTALL_ERROR_NO_BOOTLOADER);
    }
}

static void install_tick_writing(struct mesh_firmware_install *install, uint64_t now_ms) {
    mesh_usb_msc_write_tick(&install->write, now_ms);
    if (install->write.state == MESH_USB_MSC_WRITE_RUNNING) {
        return;
    }
    if (install->write.state != MESH_USB_MSC_WRITE_DONE) {
        install_fail(install, MESH_FIRMWARE_INSTALL_ERROR_WRITE);
        return;
    }
    mesh_log_info("firmware", "Wrote %llu bytes; waiting for the board to restart",
                  (unsigned long long)install->write.total);
    install->state = MESH_FIRMWARE_INSTALL_RESTARTING;
    install->deadline_ms = now_ms + INSTALL_RESTART_TIMEOUT_MS;
    install->next_poll_ms = now_ms + INSTALL_POLL_MS;
}

static void install_tick_restarting(struct mesh_firmware_install *install, uint64_t now_ms) {
    if (now_ms < install->next_poll_ms) {
        return;
    }
    install->next_poll_ms = now_ms + INSTALL_POLL_MS;

    struct mesh_serial_device_info bootloader;
    if (!install_find_bootloader(install, &bootloader)) {
        /* The bootloader is gone, which is it saying it counted `numBlocks` blocks, flushed and
           reset. Nothing had to tell it the transfer was over - the file said so. */
        mesh_log_info("firmware", "The board restarted into its new firmware");
        install_finish(install, MESH_FIRMWARE_INSTALL_DONE, MESH_FIRMWARE_INSTALL_ERROR_NONE);
        return;
    }
    if (now_ms >= install->deadline_ms) {
        /* Every byte went out and the board is still in DFU, so it never saw a full set of
           blocks. The recovery is the same write again, which is the whole reason this path is
           the safe one. */
        install_fail(install, MESH_FIRMWARE_INSTALL_ERROR_NO_RESTART);
    }
}

/* ---- the public half -----------------------------------------------------------------------
 */

static uint8_t *install_read_image(const char *path, size_t *out_len) {
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size <= 0 || (size_t)size > MESH_FIRMWARE_INSTALL_IMAGE_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *const bytes = malloc((size_t)size);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    const size_t got = fread(bytes, 1U, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(bytes);
        return NULL;
    }
    *out_len = got;
    return bytes;
}

int mesh_firmware_install_start(struct mesh_firmware_install *install, struct mesh_event_loop *loop,
                                const char *image_path, const char *port_id, uint32_t expect_family,
                                mesh_firmware_install_arm_fn arm, void *arm_userdata,
                                mesh_firmware_install_done_fn on_done, void *userdata) {
    if (install == NULL) {
        return -EINVAL;
    }
    /* Before the memset, and the only check that has to be: everything after this point is
       allowed to scribble on the struct, and a running install is not. */
    if (mesh_firmware_install_busy(install)) {
        return -EBUSY;
    }

    /* From here every return fills in `state` and `error`, which is what lets a caller print
       why rather than "could not start the install: none". */
    memset(install, 0, sizeof *install);

    /*
     * A family of 0 does not mean "any family". It means the architecture has no UF2 path at
     * all - every ESP32, portduino - and passing it through as an expectation would check the
     * image against itself, which is the one guard between this board and an image for another.
     */
    if (image_path == NULL || image_path[0] == '\0' || expect_family == 0U) {
        install_refuse(install, MESH_FIRMWARE_INSTALL_ERROR_UNAVAILABLE);
        return -EINVAL;
    }

    size_t len = 0U;
    uint8_t *const image = install_read_image(image_path, &len);
    if (image == NULL) {
        /* Missing, unreadable, empty, or larger than any UF2 for a board this reaches. All
           four are the same sentence to a reader: the image is not there to be written. */
        mesh_log_error("firmware", "The staged image could not be read: %s", image_path);
        install_refuse(install, MESH_FIRMWARE_INSTALL_ERROR_UNAVAILABLE);
        return -EIO;
    }

    install->loop = loop;
    install->image = image;
    install->image_len = len;
    install->on_done = on_done;
    install->userdata = userdata;
    install->arm = arm;
    install->arm_userdata = arm_userdata;
    if (port_id != NULL) {
        (void)install_port_of(port_id, install->port, sizeof install->port);
    }

    const enum mesh_uf2_verdict verdict =
        mesh_uf2_validate(image, len, expect_family, &install->uf2);
    if (verdict != MESH_UF2_OK) {
        mesh_log_error("firmware", "The staged image is not a UF2 for this board (verdict %d)",
                       (int)verdict);
        install_refuse(install, MESH_FIRMWARE_INSTALL_ERROR_WRONG_IMAGE);
        return -EINVAL;
    }

    if (arm != NULL) {
        const int armed = arm(arm_userdata);
        if (armed != 0) {
            mesh_log_error("firmware", "The radio would not take the DFU request: %s",
                           strerror(-armed));
            install_refuse(install, MESH_FIRMWARE_INSTALL_ERROR_ARM);
            return armed;
        }
        install->state = MESH_FIRMWARE_INSTALL_ARMING;
        mesh_log_info("firmware", "Asked the radio to enter DFU; %u blocks are ready",
                      (unsigned)install->uf2.blocks);
    } else {
        install->state = MESH_FIRMWARE_INSTALL_WAITING;
        mesh_log_info("firmware", "Waiting for a bootloader; %u blocks are ready",
                      (unsigned)install->uf2.blocks);
    }
    /* Both deadlines are armed on the first tick, which is where the clock comes from. */
    install->deadline_ms = 0U;
    install->next_poll_ms = 0U;
    return 0;
}

void mesh_firmware_install_tick(struct mesh_firmware_install *install, uint64_t now_ms) {
    if (install == NULL) {
        return;
    }
    if (install->deadline_ms == 0U && mesh_firmware_install_busy(install)) {
        install->deadline_ms =
            now_ms + (install->state == MESH_FIRMWARE_INSTALL_ARMING ? INSTALL_ARM_TIMEOUT_MS
                                                                     : INSTALL_WAIT_TIMEOUT_MS);
        install->next_poll_ms = now_ms;
    }
    switch (install->state) {
    case MESH_FIRMWARE_INSTALL_ARMING:
        install_tick_arming(install, now_ms);
        break;
    case MESH_FIRMWARE_INSTALL_WAITING:
        install_tick_waiting(install, now_ms);
        break;
    case MESH_FIRMWARE_INSTALL_WRITING:
        install_tick_writing(install, now_ms);
        break;
    case MESH_FIRMWARE_INSTALL_RESTARTING:
        install_tick_restarting(install, now_ms);
        break;
    default:
        break;
    }
}

void mesh_firmware_install_cancel(struct mesh_firmware_install *install) {
    if (install == NULL) {
        return;
    }
    install_release(install);
    install->on_done = NULL;
    install->userdata = NULL;
    install->state = MESH_FIRMWARE_INSTALL_IDLE;
    install->error = MESH_FIRMWARE_INSTALL_ERROR_NONE;
}

bool mesh_firmware_install_busy(const struct mesh_firmware_install *install) {
    return install != NULL && install->state != MESH_FIRMWARE_INSTALL_IDLE &&
           install->state != MESH_FIRMWARE_INSTALL_DONE &&
           install->state != MESH_FIRMWARE_INSTALL_FAILED;
}

bool mesh_firmware_install_holds_the_radio(const struct mesh_firmware_install *install) {
    /* Every busy state: from the moment the verb goes out the radio is either on its way down,
       gone, or a bootloader whose CDC auto-connect would happily bind. */
    return mesh_firmware_install_busy(install);
}

unsigned mesh_firmware_install_progress(const struct mesh_firmware_install *install) {
    if (install == NULL) {
        return 0U;
    }
    switch (install->state) {
    case MESH_FIRMWARE_INSTALL_WRITING:
        return mesh_usb_msc_write_progress(&install->write);
    case MESH_FIRMWARE_INSTALL_RESTARTING:
    case MESH_FIRMWARE_INSTALL_DONE:
        /* The bytes are all out; what is left is the board's own second. A bar that fell back
           here would be the download's 100 -> 0 -> 100 again. */
        return 100U;
    default:
        return 0U;
    }
}
