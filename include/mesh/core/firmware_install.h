#pragma once

/*
 * The USB handover: from a staged `.uf2` to a radio running it.
 *
 * This is phase 3 of docs/radio-firmware-roadmap.md and the first thing in this client that
 * changes a radio. It is deliberately the half whose worst outcome is "write the blocks again":
 * an interrupted write leaves the board sitting in its bootloader, which any computer on any OS
 * can talk to, so there is no state on the radio this has to be careful to leave in a good
 * place and no banner promising to come back and finish.
 *
 * Four steps, one state each, because each is a different sentence on a screen:
 *
 *   arming      the `enter_dfu_mode_request` has gone down the link and the radio is on its way
 *               down. The one admin verb whose reply is the link dropping, so nothing waits for
 *               an ack; what says it worked is the port going away.
 *   waiting     watching the USB tree for a bootloader - a CDC pair with a mass-storage sibling
 *               (see serial_usb.h) - and then for the block device `usb-storage` publishes about
 *               a second later. Watching for what enumerated rather than counting seconds, so a
 *               board that needs a double-tap by hand is the same path with a slower clock.
 *   writing     the blocks, through a forked child, at about 113 KB/s.
 *   restarting  the bootloader going away again *and a radio answering where it was*, which
 *               together are it saying it had every block it was promised. A board still
 *               sitting there after the last byte is a failure with its own name, because the
 *               recovery for it is the same write once more; a bus with nothing on it is a
 *               different one, because a pulled cable ends the same way and reporting it as
 *               done would be this path's one available lie.
 *
 * The write ending early is not one of the failures. A bootloader that has been given the
 * blocks it was missing resets in the middle of the file - which is what the write *after* an
 * interrupted one looks like every time - so a drive that goes away mid-write is read the same
 * way as one that goes away at the end of it.
 *
 * **This does not go through mesh_session.** A bootloader is not a Meshtastic node: it speaks no
 * protobuf, has no node number and will never answer a handshake. The one thing that does need
 * the session is sending the admin verb, and that arrives as a callback the caller fills in -
 * so the module that talks to a bootloader knows nothing about the radio it used to be, and a
 * board already sitting in its bootloader is installed to by passing no callback at all.
 *
 * It includes the USB transport's headers, which is a seam core files do not usually cross.
 * That is the feature rather than an accident: the handover is the one part of this that is
 * *about* a bus, and phase 4's BLE half will sit on the bluez client for the same reason.
 */

#include "mesh/core/uf2.h"
#include "mesh/transport/usb_msc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* A UF2 for a board with 1 MB of flash is about 2 MB, because a block spends 512 bytes carrying
   256. This is well clear of that and well short of anything that would embarrass a device with
   64 MB of RAM - and it is a refusal rather than a truncation, because half an image written to
   a radio is the one outcome worth spending a check to avoid. */
#define MESH_FIRMWARE_INSTALL_IMAGE_MAX (8U * 1024U * 1024U)

enum mesh_firmware_install_state {
    MESH_FIRMWARE_INSTALL_IDLE = 0,
    MESH_FIRMWARE_INSTALL_ARMING,
    MESH_FIRMWARE_INSTALL_WAITING,
    MESH_FIRMWARE_INSTALL_WRITING,
    MESH_FIRMWARE_INSTALL_RESTARTING,
    MESH_FIRMWARE_INSTALL_DONE,
    MESH_FIRMWARE_INSTALL_FAILED,
    MESH_FIRMWARE_INSTALL_STATE_COUNT,
};

enum mesh_firmware_install_error {
    MESH_FIRMWARE_INSTALL_ERROR_NONE = 0,
    /* A bad argument, or an image that could not be read. Nothing was started. */
    MESH_FIRMWARE_INSTALL_ERROR_UNAVAILABLE,
    /* The file is not a UF2, or is a UF2 for another chip. The guard that protects the board
       rather than the download - and the last one there is, because a UF2 block carries no
       checksum and the bootloader flashes what it is given. */
    MESH_FIRMWARE_INSTALL_ERROR_WRONG_IMAGE,
    /* The admin verb could not be sent. The radio is untouched and still running. */
    MESH_FIRMWARE_INSTALL_ERROR_ARM,
    /* No bootloader appeared, or it published no drive. The row that says so is the one that
       tells a user to double-tap reset, because that is the same thing done by hand. */
    MESH_FIRMWARE_INSTALL_ERROR_NO_BOOTLOADER,
    /* The drive would not come off its mountpoints, so nothing was written: a write racing the
       VFAT driver over one device is not a thing to leave in. */
    MESH_FIRMWARE_INSTALL_ERROR_MOUNTED,
    /* The write itself failed or stalled. The board is still in its bootloader and the
       recovery is to write it again. */
    MESH_FIRMWARE_INSTALL_ERROR_WRITE,
    /* Every block went out and the board is still sitting in its bootloader, which means it
       never counted `numBlocks` of them. Same recovery. */
    MESH_FIRMWARE_INSTALL_ERROR_NO_RESTART,
    /* The bootloader went away and nothing came back on its port. A pulled cable is the
       ordinary cause and the board is in its bootloader wherever it is now, so the recovery is
       to plug it in and write again; a board that reset into something that does not enumerate
       is the other reading, and it needs the reset button. */
    MESH_FIRMWARE_INSTALL_ERROR_NO_RADIO,
    MESH_FIRMWARE_INSTALL_ERROR_COUNT,
};

struct mesh_firmware_install;

/*
 * Sends `enter_dfu_mode_request` to the radio. Returns 0, or -errno.
 *
 * A callback rather than a call because this module must not know what a session is, and
 * because the two ways into a bootloader - the admin verb and a double-tap of reset - are then
 * the same install with and without one.
 */
typedef int (*mesh_firmware_install_arm_fn)(void *userdata);

typedef void (*mesh_firmware_install_done_fn)(void *userdata,
                                              const struct mesh_firmware_install *install);

struct mesh_firmware_install {
    struct mesh_event_loop *loop; /* borrowed; may be NULL */

    enum mesh_firmware_install_state state;
    enum mesh_firmware_install_error error;

    /*
     * The image, read whole before the radio is sent anywhere and held until the write is over.
     *
     * In memory rather than on disk because of a platform bug this feature landed on: plugging
     * a bootloader into a Brick gets its 32 MB ghost FAT mounted **over `/mnt/SDCARD`**, taking
     * the pak, the client's own binary, the CA bundle and the log with it. A write that read
     * its source back off that card would be reading it out from under itself. The child
     * inherits this buffer through fork and needs no file at all.
     */
    uint8_t *image;
    size_t image_len;
    struct mesh_uf2_info uf2;

    /* Which USB device the bootloader has to appear on ("2-1"), derived from the port the radio
       was on. Empty means any bootloader will do, which is what a board already sitting in one
       needs and what the install path never passes. */
    char port[64];

    struct mesh_usb_msc_target target;
    struct mesh_usb_msc_write write;

    /* When the current step gives up, and when the USB tree is next worth reading. */
    uint64_t deadline_ms;
    uint64_t next_poll_ms;

    mesh_firmware_install_arm_fn arm;
    void *arm_userdata;
    mesh_firmware_install_done_fn on_done;
    void *userdata;
};

/*
 * Reads `image_path`, checks it is a UF2 for `expect_family`, sends the radio into DFU and
 * writes it.
 *
 * `expect_family` is the family for the connected board's architecture, from
 * `mesh_uf2_family_for_architecture()`. **0 is refused rather than treated as "any family"**:
 * 0 means the architecture has no UF2 path at all, and passing it through would turn off the
 * one check standing between a T114 and an image for a different board.
 *
 * `port_id` is the serial device the radio was on ("2-1:1.1"); the bootloader is then only
 * accepted on that same USB device, because a board resets in place and a write must not follow
 * one that moved. Empty or NULL accepts any bootloader, which is the recovery case.
 *
 * `arm` may be NULL, meaning the board is already in its bootloader or the user will double-tap
 * it there.
 *
 * Returns 0, or -errno. On 0 `on_done` is called exactly once, later, from a tick. On a
 * negative return nothing was started and no callback will arrive, but `state` and `error` are
 * still filled in - a refusal is a row, and "that file is not a UF2 for this board" is a
 * different row from "the radio would not take the request".
 */
int mesh_firmware_install_start(struct mesh_firmware_install *install, struct mesh_event_loop *loop,
                                const char *image_path, const char *port_id, uint32_t expect_family,
                                mesh_firmware_install_arm_fn arm, void *arm_userdata,
                                mesh_firmware_install_done_fn on_done, void *userdata);

/* Drives every step. Call every loop turn. */
void mesh_firmware_install_tick(struct mesh_firmware_install *install, uint64_t now_ms);

/* Kills anything in flight, frees the image and reports nothing. Safe on a zeroed struct. */
void mesh_firmware_install_cancel(struct mesh_firmware_install *install);

bool mesh_firmware_install_busy(const struct mesh_firmware_install *install);

/* True while the radio must be left alone: the link is going down, or is already gone, and
   auto-connect bringing it back would bind the bootloader's own CDC. Derived rather than held,
   like mesh_updater_holds_the_radio(), so a failure lifts it by failing. */
bool mesh_firmware_install_holds_the_radio(const struct mesh_firmware_install *install);

/* 0-100 over the write, which is the only step long enough to watch. */
unsigned mesh_firmware_install_progress(const struct mesh_firmware_install *install);

const char *mesh_firmware_install_state_name(enum mesh_firmware_install_state state);
const char *mesh_firmware_install_error_name(enum mesh_firmware_install_error error);

#ifdef __cplusplus
}
#endif
