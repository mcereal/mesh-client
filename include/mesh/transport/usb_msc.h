#pragma once

/*
 * Writing a UF2 to a bootloader's drive.
 *
 * The whole of the USB handover is one sentence: **write the `.uf2`'s 512-byte blocks to the
 * bootloader's mass-storage endpoint, in any order, and wait for the device to reboot.** The
 * Adafruit bootloader's `tud_msc_write10_cb` hands every block to `write_block()`, which looks
 * for the UF2 magic and flashes the payload at the address the block itself names; the LBA is
 * passed through and never used to decide anything, and the FAT the drive appears to have is a
 * fiction. Completion is carried in the file - each block states its own `blockNo` and the
 * `numBlocks` of the image - so nothing has to tell the bootloader the transfer is over.
 *
 * Which means there is no protocol here, and no filesystem: this module finds the block device,
 * gets the platform's own mounts off it, and copies bytes.
 *
 * Three things measured on the Brick on 2026-09-10 shape the rest:
 *
 *   - The kernel binds `usb-storage` unprompted and `/dev/sda` appears about a second after the
 *     board re-enumerates. So the fast path is a `write()` to a block device and the 350 lines
 *     of Bulk-Only Transport over usbfs are a fallback for a kernel that is not this one.
 *   - **The platform mounts the drive over `/mnt/SDCARD`** - `/etc/hotplug.d/block/10-mount`
 *     does it by hand when OpenWrt's own mount declines - which shadows the pak, the binary,
 *     the CA bundle and the log. So the drive has to come off both of its mountpoints before a
 *     byte goes out, or the VFAT driver's writeback interleaves FAT sectors with our blocks.
 *     Unmounting is also the *restore*: the shadow is a stacked mount and taking it off reveals
 *     the SD card that was underneath.
 *   - A full-speed USB link moves about 113 KB/s and the flash program hides entirely behind
 *     it, so 1.4 MB takes 13 seconds however it is chunked. That is too long to block the one
 *     loop the UI draws on, which is why the write is a forked child reporting through a pipe -
 *     the same shape `fetch.c` and the inflate in `firmware_download.c` already have.
 *
 * The image is passed as **bytes rather than a path**, and that is the mount shadow again: by
 * the time the drive is writable the file the image was staged into may be underneath a
 * bootloader's ghost FAT. The caller reads it once, before the radio is sent into DFU, and the
 * child inherits the buffer through fork.
 */

#include "mesh/transport/serial_usb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* How many mountpoints one drive may be found at. The Brick manages two - `/mnt/exUDISK` from
   `/etc/config/fstab` and `/mnt/SDCARD` from the TrimUI patch that runs when the first one
   declines - and a device with more than this is one to refuse rather than half-unmount. */
#define MESH_USB_MSC_MOUNTS_MAX 4U
#define MESH_USB_MSC_PATH_MAX 128U

struct mesh_usb_msc_target {
    /* "/dev/sda", or empty when the bootloader has published no block device yet. */
    char device[96];
    /* The whole device's size, from sysfs `size` in 512-byte sectors. 0 when unreadable. */
    uint64_t size_bytes;
    /* Every mountpoint the platform has this device (or a partition of it) at, in the order
       /proc/mounts lists them - which is the order they were mounted, so unmounting walks it
       backwards. */
    char mounts[MESH_USB_MSC_MOUNTS_MAX][MESH_USB_MSC_PATH_MAX];
    size_t mount_count;
    /* Set when the device has more mountpoints than this struct can hold. Nothing is unmounted
       in that case: half-unmounting a device is worse than refusing it. */
    bool too_many_mounts;
};

/*
 * Finds the block device the bootloader named by `device` publishes, and what is mounted on it.
 *
 * `device` is the entry `mesh_serial_usb_scan()` returned - its `id` names a USB *interface*
 * ("2-1:1.2"), and the drive belongs to the USB *device* that interface is part of ("2-1"), so
 * the match is on the device rather than on the interface. It does not have to be the
 * mass-storage interface: a UF2 bootloader publishes one drive and the CDC pair beside it
 * belongs to the same device.
 *
 * Returns 0 with `out` filled in, -ENOENT when no block device belongs to that USB device yet
 * (which is the ordinary answer for about a second after the board re-enumerates, and is what
 * the caller polls on), or another -errno.
 */
int mesh_usb_msc_find(const struct mesh_serial_device_info *device,
                      struct mesh_usb_msc_target *out);

/*
 * Takes the drive off every mountpoint `find` reported, latest first.
 *
 * A lazy unmount is deliberately not offered: `MNT_DETACH` returns success while leaving the
 * VFAT driver able to write back, which is precisely the interleaving this exists to prevent.
 * A mountpoint that will not come off is a refusal with a reason.
 *
 * Returns 0, or -errno from the first mountpoint that refused. On success `out->mount_count`
 * is 0, so the drive can be asked again without unmounting twice.
 */
int mesh_usb_msc_unmount(struct mesh_usb_msc_target *target);

/*
 * Opens the drive for writing and claims it, returning an fd or -errno.
 *
 * `mesh_usb_msc_write_start()` calls this before it forks, and the unmount above is not a
 * substitute for it. The Brick published `/dev/sda` and had it mounted over `/mnt/SDCARD`
 * **130 ms after the write began** on 2026-09-10: the install had unmounted an empty list and
 * won a race it could equally have lost, and the platform's mount then interleaved FAT sectors
 * with the UF2 blocks for the rest of the write. The board never restarted.
 *
 * `O_EXCL` on a block device is an exclusive claim rather than anything about creation, and
 * `mount` takes the same claim - so a mount attempted while this fd is open fails with `-EBUSY`
 * and the race has no second party. `-EBUSY` coming back from here means the other order
 * happened; the retry unmounts and asks again.
 *
 * A path that is not a block device gets `O_CREAT` and no claim, which is what makes the write
 * testable against a file. The two flags must never meet: `O_CREAT | O_EXCL` is the unrelated
 * "fail if it exists".
 */
int mesh_usb_msc_claim(const char *device_path);

enum mesh_usb_msc_write_state {
    MESH_USB_MSC_WRITE_IDLE = 0,
    MESH_USB_MSC_WRITE_RUNNING,
    MESH_USB_MSC_WRITE_DONE,
    MESH_USB_MSC_WRITE_FAILED,
};

struct mesh_usb_msc_write {
    struct mesh_event_loop *loop; /* borrowed; may be NULL, and then progress arrives on tick */
    enum mesh_usb_msc_write_state state;
    /* -errno when the state is FAILED. */
    int error;

    pid_t child;
    int progress_fd;
    /* The claim on the block device, held by the parent for the length of the write. The child
       inherits it and writes through it; this copy is what keeps the platform's hotplug mount
       off the drive even if the child dies halfway. */
    int device_fd;
    /* The child reports a running byte count per chunk, one decimal line each; a read can land
       mid-line, so the tail is kept. */
    char pending[32];
    size_t pending_len;

    uint64_t written;
    uint64_t total;
    /* Silence, not a total budget: a write that is making progress is allowed to take as long
       as it takes, and one that has stopped is over well before a 1.4 MB budget would expire. */
    uint64_t idle_deadline_ms;
};

/*
 * Forks a child that copies `len` bytes of `image` to `device_path`, in chunks, syncing each
 * one so the byte count it reports is a byte count that has actually reached the bus.
 *
 * `device_path` is "/dev/sda" in the field and a plain file in a test, which is the whole of
 * the seam this module needs: the fast path is a `write()` and a file takes one just as a block
 * device does.
 *
 * `loop` may be NULL, in which case progress is read on each tick instead of when the pipe
 * wakes the loop. Passing one is what keeps the bar moving on a loop that is otherwise idle.
 *
 * Returns 0, or -errno. On 0 the state is RUNNING and the caller ticks until it is not.
 */
int mesh_usb_msc_write_start(struct mesh_usb_msc_write *write, struct mesh_event_loop *loop,
                             const uint8_t *image, size_t len, const char *device_path,
                             uint64_t now_ms);

/* Reads whatever the child has reported, reaps it when it is gone, and enforces the silence
   deadline. Call every loop turn. */
void mesh_usb_msc_write_tick(struct mesh_usb_msc_write *write, uint64_t now_ms);

/* Kills anything in flight and reports nothing. Safe on a zeroed struct. */
void mesh_usb_msc_write_cancel(struct mesh_usb_msc_write *write);

/* 0-100 over the image. 100 only once the child is gone and the bytes are acknowledged. */
unsigned mesh_usb_msc_write_progress(const struct mesh_usb_msc_write *write);

#ifdef __cplusplus
}
#endif
