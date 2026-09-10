#define _POSIX_C_SOURCE 200809L

/*
 * The USB handover, against fixture trees rather than a radio.
 *
 * Three seams make the whole of it reachable from a suite, and all three are *readings* with a
 * default that is the real path: `MESHCLIENT_SYSFS_USB` is the USB tree the bootloader is found
 * in, `MESHCLIENT_SYSFS_BLOCK` is where the drive it published appears, and
 * `MESHCLIENT_PROC_MOUNTS` is what the platform has mounted on it. The fourth,
 * `MESHCLIENT_DEV_ROOT`, is what turns "/dev/sda" into a file in a temporary directory - which
 * is the only reason the *write* can be tested at all, and it costs nothing in honesty because
 * a block device takes exactly the `open`, `write` and `fdatasync` a file does.
 *
 * So nothing here is mocked. The scan really walks sysfs, the drive is really found by chasing
 * a symlink, the mount list is really parsed, and the write really forks a child and copies
 * bytes through a pipe-reported byte count.
 *
 * The trees are laid out as the Brick's were measured on 2026-09-10: a T114 at `239a:4405` with
 * a CDC pair, the same board at `239a:0071` with a mass-storage interface beside it, and
 * `/dev/sda` hanging off the bootloader's device about a second later.
 */

#include "framework/mesh_test.h"
#include "support/uf2_fixture.h"

#include "mesh/core/firmware_install.h"
#include "mesh/core/uf2.h"
#include "mesh/transport/serial_usb.h"
#include "mesh/transport/usb_msc.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- fixture trees -------------------------------------------------------------------------
 */

static bool fixture_put(const char *path, const char *contents) {
    FILE *const file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    const bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

static bool fixture_append(const char *path, const char *contents) {
    FILE *const file = fopen(path, "a");
    if (file == NULL) {
        return false;
    }
    const bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

static void fixture_remove(const char *root) {
    char command[PATH_MAX + 16];
    if (snprintf(command, sizeof command, "rm -rf '%s'", root) < (int)sizeof command) {
        (void)system(command);
    }
}

static bool usb_interface(const char *root, const char *name, const char *cls, const char *subclass,
                          const char *protocol) {
    char dir[PATH_MAX];
    char file[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, name) >= (int)sizeof dir) {
        return false;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    const char *const attrs[4][2] = {{"bInterfaceClass", cls},
                                     {"bInterfaceSubClass", subclass},
                                     {"bInterfaceProtocol", protocol},
                                     {"bInterfaceNumber", "00"}};
    for (size_t i = 0; i < 4U; ++i) {
        if (snprintf(file, sizeof file, "%s/%s", dir, attrs[i][0]) >= (int)sizeof file ||
            !fixture_put(file, attrs[i][1])) {
            return false;
        }
    }
    return true;
}

static bool usb_device(const char *root, const char *name, const char *pid) {
    char dir[PATH_MAX];
    char file[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, name) >= (int)sizeof dir) {
        return false;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    const char *const attrs[5][2] = {{"idVendor", "239a"},
                                     {"idProduct", pid},
                                     {"product", "HT-n5262"},
                                     {"busnum", "2"},
                                     {"devnum", "3"}};
    for (size_t i = 0; i < 5U; ++i) {
        if (snprintf(file, sizeof file, "%s/%s", dir, attrs[i][0]) >= (int)sizeof file ||
            !fixture_put(file, attrs[i][1])) {
            return false;
        }
    }
    return true;
}

/* The board in its bootloader on USB device `name`: a CDC pair with a Bulk-Only drive beside
   it, which is the whole of what tells a UF2 bootloader from a radio. */
static bool usb_bootloader(const char *root, const char *name) {
    char iface[80];
    bool ok = usb_device(root, name, "0071");
    ok = ok && snprintf(iface, sizeof iface, "%s:1.0", name) < (int)sizeof iface &&
         usb_interface(root, iface, "02", "02", "00");
    ok = ok && snprintf(iface, sizeof iface, "%s:1.1", name) < (int)sizeof iface &&
         usb_interface(root, iface, "0a", "00", "00");
    ok = ok && snprintf(iface, sizeof iface, "%s:1.2", name) < (int)sizeof iface &&
         usb_interface(root, iface, "08", "06", "50");
    return ok;
}

/* The same board running firmware: the CDC pair on its own. */
static bool usb_radio(const char *root, const char *name) {
    char iface[80];
    bool ok = usb_device(root, name, "4405");
    ok = ok && snprintf(iface, sizeof iface, "%s:1.0", name) < (int)sizeof iface &&
         usb_interface(root, iface, "02", "02", "00");
    ok = ok && snprintf(iface, sizeof iface, "%s:1.1", name) < (int)sizeof iface &&
         usb_interface(root, iface, "0a", "00", "00");
    return ok;
}

/*
 * A block device that belongs to USB device `usb_name`.
 *
 * `/sys/block/sda` is a symlink into the device tree, and the path it points at walks through
 * every USB device between the controller and the disk - which is how a drive is tied to the
 * bootloader that published it without reading a single attribute.
 */
static bool block_device(const char *root, const char *name, const char *usb_name,
                         const char *sectors) {
    char target[PATH_MAX];
    char link[PATH_MAX];
    char size[PATH_MAX];
    if (snprintf(target, sizeof target, "%s/tree/usb2/%s/%s:1.2/host0/target0:0:0/0:0:0:0/block/%s",
                 root, usb_name, usb_name, name) >= (int)sizeof target ||
        snprintf(link, sizeof link, "%s/%s", root, name) >= (int)sizeof link) {
        return false;
    }
    char command[PATH_MAX + 32];
    if (snprintf(command, sizeof command, "mkdir -p '%s'", target) >= (int)sizeof command ||
        system(command) != 0) {
        return false;
    }
    if (symlink(target, link) != 0 && errno != EEXIST) {
        return false;
    }
    if (snprintf(size, sizeof size, "%s/size", target) >= (int)sizeof size ||
        !fixture_put(size, sectors)) {
        return false;
    }
    return true;
}

struct install_fixture {
    char usb[64];
    char block[64];
    char dev[64];
    char mounts[128];
};

static bool fixture_open(struct install_fixture *fixture) {
    memset(fixture, 0, sizeof *fixture);
    char usb[] = "/tmp/meshclient-usb-XXXXXX";
    char block[] = "/tmp/meshclient-blk-XXXXXX";
    char dev[] = "/tmp/meshclient-dev-XXXXXX";
    if (mkdtemp(usb) == NULL || mkdtemp(block) == NULL || mkdtemp(dev) == NULL) {
        return false;
    }
    snprintf(fixture->usb, sizeof fixture->usb, "%s", usb);
    snprintf(fixture->block, sizeof fixture->block, "%s", block);
    snprintf(fixture->dev, sizeof fixture->dev, "%s", dev);
    snprintf(fixture->mounts, sizeof fixture->mounts, "%s/mounts", dev);
    return setenv("MESHCLIENT_SYSFS_USB", fixture->usb, 1) == 0 &&
           setenv("MESHCLIENT_SYSFS_BLOCK", fixture->block, 1) == 0 &&
           setenv("MESHCLIENT_DEV_ROOT", fixture->dev, 1) == 0 &&
           setenv("MESHCLIENT_PROC_MOUNTS", fixture->mounts, 1) == 0 &&
           fixture_put(fixture->mounts, "");
}

static void fixture_close(struct install_fixture *fixture) {
    (void)unsetenv("MESHCLIENT_SYSFS_USB");
    (void)unsetenv("MESHCLIENT_SYSFS_BLOCK");
    (void)unsetenv("MESHCLIENT_DEV_ROOT");
    (void)unsetenv("MESHCLIENT_PROC_MOUNTS");
    fixture_remove(fixture->usb);
    fixture_remove(fixture->block);
    fixture_remove(fixture->dev);
}

/* The scan's entry for the bootloader's CDC-Data interface, which is what a caller holds. */
static bool find_device(const char *id, struct mesh_serial_device_info *out) {
    struct mesh_serial_device_info devices[MESH_SERIAL_MAX_DEVICES];
    const size_t count = mesh_serial_usb_scan(devices, MESH_SERIAL_MAX_DEVICES);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(devices[i].id, id) == 0) {
            *out = devices[i];
            return true;
        }
    }
    return false;
}

/* ---- finding the drive ---------------------------------------------------------------------
 */

/*
 * The drive belongs to the USB *device*, not to the interface the caller happens to hold, and
 * the match has to survive the names a real bus produces. "2-1" must not match "12-1" and must
 * not match "2-1.4" - a hub between the Brick and the radio makes both of those real - which is
 * what the slashes around the needle are for.
 */
MESH_TEST_CASE(usb_msc_finds_the_drive_the_bootloader_published, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    bool built = usb_bootloader(fixture.usb, "2-1");
    /* A second board, on a port whose name has the first one's as a prefix. */
    built = built && usb_bootloader(fixture.usb, "2-1.4");
    built = built && block_device(fixture.block, "sda", "2-1", "65801");
    built = built && block_device(fixture.block, "sdb", "2-1.4", "65801");
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the trees");

    struct mesh_serial_device_info device;
    MESH_TEST_FAIL_IF_CLEANUP(!find_device("2-1:1.1", &device), fixture_close(&fixture),
                              "the scan should offer the bootloader's CDC-Data interface");
    MESH_TEST_FAIL_IF_CLEANUP(device.role != MESH_SERIAL_ROLE_BOOTLOADER, fixture_close(&fixture),
                              "and should call it a bootloader");

    struct mesh_usb_msc_target target;
    const int found = mesh_usb_msc_find(&device, &target);
    char expected[128];
    snprintf(expected, sizeof expected, "%s/sda", fixture.dev);
    const bool right = found == 0 && strcmp(target.device, expected) == 0;
    const uint64_t size = target.size_bytes;

    struct mesh_serial_device_info sibling;
    const bool has_sibling = find_device("2-1.4:1.1", &sibling);
    struct mesh_usb_msc_target other;
    char expected_other[128];
    snprintf(expected_other, sizeof expected_other, "%s/sdb", fixture.dev);
    const bool sibling_right = has_sibling && mesh_usb_msc_find(&sibling, &other) == 0 &&
                               strcmp(other.device, expected_other) == 0;
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!right, "the drive on 2-1 should be found and named");
    MESH_TEST_FAIL_IF(size != 65801ULL * 512ULL, "and sized from its sector count");
    MESH_TEST_FAIL_IF(!sibling_right, "a port whose name has ours as a prefix keeps its own drive");
    record_success(test_name);
}

/* A bootloader that has enumerated and whose drive `usb-storage` has not published yet. It is
   the ordinary state for about a second after a reset, so it has to be a wait rather than a
   refusal - which is why it is -ENOENT and not an error. */
MESH_TEST_CASE(usb_msc_says_not_yet_before_the_drive_appears, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");
    MESH_TEST_FAIL_IF_CLEANUP(!usb_bootloader(fixture.usb, "2-1"), fixture_close(&fixture),
                              "could not build the USB tree");

    struct mesh_serial_device_info device;
    const bool have = find_device("2-1:1.1", &device);
    struct mesh_usb_msc_target target;
    const int found = have ? mesh_usb_msc_find(&device, &target) : 0;
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!have, "the bootloader should still be in the scan");
    MESH_TEST_FAIL_IF(found != -ENOENT, "a bootloader with no drive yet is -ENOENT, not an error");
    record_success(test_name);
}

/*
 * What the platform has mounted on the drive, and the two shapes it arrives in.
 *
 * On the Brick both happen: OpenWrt's `/sbin/block` mounts a partition at /mnt/exUDISK, and
 * when that declines the TrimUI patch mounts the whole device over `/mnt/SDCARD` - the SD
 * card the pak, the binary, the CA bundle and the log are all on. Both have to come off, and
 * neither /dev/sdb nor /dev/sdaa is either of them.
 */
MESH_TEST_CASE(usb_msc_lists_every_mount_of_the_drive_and_nothing_else, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    bool built = usb_bootloader(fixture.usb, "2-1");
    built = built && block_device(fixture.block, "sda", "2-1", "65801");
    char mounts[1024];
    snprintf(mounts, sizeof mounts,
             "/dev/root / squashfs ro 0 0\n"
             "%s/sdaa /mnt/decoy vfat rw 0 0\n"
             "%s/sda1 /mnt/exUDISK vfat rw 0 0\n"
             "%s/sdb /mnt/other vfat rw 0 0\n"
             "%s/sda /mnt/SD\\040CARD vfat rw,iocharset=utf8 0 0\n",
             fixture.dev, fixture.dev, fixture.dev, fixture.dev);
    built = built && fixture_put(fixture.mounts, mounts);
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the trees");

    struct mesh_serial_device_info device;
    struct mesh_usb_msc_target target;
    memset(&target, 0, sizeof target);
    const bool have = find_device("2-1:1.1", &device) && mesh_usb_msc_find(&device, &target) == 0;
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!have, "the drive should be found");
    MESH_TEST_FAIL_IF(target.mount_count != 2U,
                      "the partition and the whole device are ours; sdaa and sdb are not");
    MESH_TEST_FAIL_IF(strcmp(target.mounts[0], "/mnt/exUDISK") != 0,
                      "in the order /proc/mounts lists them, which is the order they stacked");
    MESH_TEST_FAIL_IF(strcmp(target.mounts[1], "/mnt/SD CARD") != 0,
                      "with the \\040 /proc/mounts escapes a space as put back");
    MESH_TEST_FAIL_IF(target.too_many_mounts, "two is not too many");
    record_success(test_name);
}

/* More mountpoints than the struct can name. Nothing is unmounted at all, because a drive taken
   half off is worse than one left alone: the writeback we were avoiding is still possible. */
/*
 * The mount the Brick spells differently, which is the shadow that matters.
 *
 * `/proc/mounts` on the device carries the shadow over `/mnt/SDCARD` as **`/dev//dev/sda`** -
 * the hotplug script does `mount /dev/${DEVNAME}` and `${DEVNAME}` there already begins with
 * `/dev/` - and `/dev/dev` is a real directory on that platform rather than a link, so the two
 * spellings are two paths to one drive that no amount of string tidying reconciles. A matcher
 * that compares text misses it, and what follows from missing it is not a bad write but a
 * refusal: the drive stays mounted, the claim comes back `-EBUSY`, and the install says
 * "mounted" about a drive it could have had. Seen on 2026-09-10 after a board was re-plugged.
 *
 * What decides it is the object rather than the path. A suite cannot make a block device, so
 * what is reachable here is the other half of the same test - one file, two spellings.
 */
MESH_TEST_CASE(usb_msc_knows_one_drive_spelled_two_ways, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    bool built = usb_bootloader(fixture.usb, "2-1");
    built = built && block_device(fixture.block, "sda", "2-1", "65801");

    /* The drive itself, and a second path that goes through a link and lands on it. */
    char drive[PATH_MAX];
    char alias[PATH_MAX];
    built = built && snprintf(drive, sizeof drive, "%s/sda", fixture.dev) < (int)sizeof drive &&
            fixture_put(drive, "");
    built = built && snprintf(alias, sizeof alias, "%s/dev", fixture.dev) < (int)sizeof alias &&
            (symlink(fixture.dev, alias) == 0 || errno == EEXIST);

    char mounts[1024];
    snprintf(mounts, sizeof mounts,
             "/dev/root / squashfs ro 0 0\n"
             "%s/dev/sda /mnt/SDCARD vfat rw,iocharset=utf8 0 0\n",
             fixture.dev);
    built = built && fixture_put(fixture.mounts, mounts);
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the trees");

    struct mesh_serial_device_info device;
    struct mesh_usb_msc_target target;
    memset(&target, 0, sizeof target);
    const bool have = find_device("2-1:1.1", &device) && mesh_usb_msc_find(&device, &target) == 0;
    const size_t count = target.mount_count;
    char point[MESH_USB_MSC_PATH_MAX];
    mesh_str_copy(point, sizeof point, count > 0U ? target.mounts[0] : "");
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!have, "the drive should be found");
    MESH_TEST_FAIL_IF(count != 1U, "a path that reaches our drive is a mount of our drive");
    MESH_TEST_FAIL_IF(strcmp(point, "/mnt/SDCARD") != 0, "and it is the shadow over the card");
    record_success(test_name);
}

MESH_TEST_CASE(usb_msc_refuses_a_drive_it_could_only_half_unmount, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    bool built = usb_bootloader(fixture.usb, "2-1");
    built = built && block_device(fixture.block, "sda", "2-1", "65801");
    /* One line per mountpoint, appended rather than accumulated: snprintf returns what it
       *would* have written, so adding its result to an offset walks past the buffer the moment
       anything truncates - and then the next call gets an out-of-range pointer and a length
       that underflowed. Writing each line separately has no offset to get wrong. */
    char line[256];
    for (unsigned i = 0; built && i < MESH_USB_MSC_MOUNTS_MAX + 1U; ++i) {
        built = snprintf(line, sizeof line, "%s/sda /mnt/m%u vfat rw 0 0\n", fixture.dev, i) <
                (int)sizeof line;
        built = built && fixture_append(fixture.mounts, line);
    }
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the trees");

    struct mesh_serial_device_info device;
    struct mesh_usb_msc_target target;
    memset(&target, 0, sizeof target);
    const bool have = find_device("2-1:1.1", &device) && mesh_usb_msc_find(&device, &target) == 0;
    const bool flagged = target.too_many_mounts;
    const int unmounted = mesh_usb_msc_unmount(&target);
    const size_t left = target.mount_count;
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!have, "the drive should still be found");
    MESH_TEST_FAIL_IF(!flagged, "a drive with more mountpoints than we can name says so");
    MESH_TEST_FAIL_IF(unmounted != -E2BIG, "and refuses rather than unmounting some of them");
    MESH_TEST_FAIL_IF(left != MESH_USB_MSC_MOUNTS_MAX, "with nothing taken off");
    record_success(test_name);
}

/* ---- the write -----------------------------------------------------------------------------
 */

/* Drives a write to completion, or gives up. Real time, because the child is a real child. */
static bool write_settle(struct mesh_usb_msc_write *write) {
    const uint64_t give_up = mesh_time_monotonic_ms() + 10000U;
    while (write->state == MESH_USB_MSC_WRITE_RUNNING && mesh_time_monotonic_ms() < give_up) {
        mesh_usb_msc_write_tick(write, mesh_time_monotonic_ms());
    }
    return write->state == MESH_USB_MSC_WRITE_DONE;
}

/*
 * Every byte lands, and the count that reaches the caller is a count of bytes that reached the
 * device.
 *
 * The second half is the interesting one. A buffered write to a block device is absorbed by the
 * page cache at memory speed, so a child that did not sync would report 100% in milliseconds
 * and then sit in close() for the thirteen seconds the bus actually takes - a bar that lies in
 * exactly the direction that makes somebody pull the cable. The `fdatasync` per chunk is what
 * makes the number mean something, and more than one chunk is what proves the loop runs.
 */
MESH_TEST_CASE(usb_msc_write_lands_every_byte, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    /* Deliberately not a UF2: this is a byte copier, and the bootloader is what reads blocks.
       A size that is not a whole number of chunks is what catches a loop that rounds. */
    const size_t len = (32768U * 3U) + 777U;
    uint8_t *const image = malloc(len);
    MESH_TEST_FAIL_IF_CLEANUP(image == NULL, fixture_close(&fixture), "out of memory");
    for (size_t i = 0; i < len; ++i) {
        image[i] = (uint8_t)((i * 31U) & 0xFFU);
    }

    char path[128];
    snprintf(path, sizeof path, "%s/sda", fixture.dev);

    struct mesh_usb_msc_write write;
    memset(&write, 0, sizeof write);
    const int started =
        mesh_usb_msc_write_start(&write, NULL, image, len, path, mesh_time_monotonic_ms());
    MESH_TEST_FAIL_IF_CLEANUP(started != 0, (free(image), fixture_close(&fixture)),
                              "the write should start");
    const bool done = write_settle(&write);

    bool same = false;
    FILE *const file = fopen(path, "rb");
    if (file != NULL) {
        uint8_t *const back = malloc(len + 1U);
        if (back != NULL) {
            const size_t got = fread(back, 1U, len + 1U, file);
            same = got == len && memcmp(back, image, len) == 0;
            free(back);
        }
        fclose(file);
    }
    const uint64_t written = write.written;
    const unsigned progress = mesh_usb_msc_write_progress(&write);
    mesh_usb_msc_write_cancel(&write);
    free(image);
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!done, "the write should finish");
    MESH_TEST_FAIL_IF(!same, "and the bytes on the device should be the bytes handed over");
    MESH_TEST_FAIL_IF(written != (uint64_t)len, "with the reported count matching the image");
    MESH_TEST_FAIL_IF(progress != 100U, "and the bar at 100 once the child is gone");
    record_success(test_name);
}

/* A write to somewhere that cannot be opened fails as a write rather than as a crash, and says
   which end refused. */
MESH_TEST_CASE(usb_msc_write_reports_a_drive_it_cannot_open, unit) {
    const uint8_t image[64] = {0};
    struct mesh_usb_msc_write write;
    memset(&write, 0, sizeof write);
    const int started =
        mesh_usb_msc_write_start(&write, NULL, image, sizeof image,
                                 "/proc/meshclient/definitely-not-here", mesh_time_monotonic_ms());
    const enum mesh_usb_msc_write_state state = write.state;
    mesh_usb_msc_write_cancel(&write);

    /* The open is the parent's now, because the claim it takes has to be held from before the
       fork. So a drive that will not open costs no child at all, and the caller is told why by
       the errno the open gave rather than by an exit code standing in for it. */
    MESH_TEST_FAIL_IF(started != -ENOENT, "a drive that will not open is refused by start");
    MESH_TEST_FAIL_IF(state == MESH_USB_MSC_WRITE_RUNNING, "and nothing is left running");
    record_success(test_name);
}

/*
 * The claim, and the flag pair that must never meet.
 *
 * `mesh_usb_msc_claim()` asks for `O_EXCL` on a block device - an exclusive claim, which is
 * what keeps the platform's hotplug mount off the drive for the length of the write - and for
 * `O_CREAT` on anything else, which is what makes the write testable against a file. Putting
 * both on one open turns the claim into the unrelated "fail if it exists", so the way this
 * would silently stop working is a file that already exists being refused.
 *
 * A real block device is not something a suite can conjure, so what is checked here is the half
 * that is reachable: the file path opens, twice, and lands its bytes where it was pointed.
 */
MESH_TEST_CASE(usb_msc_claim_opens_a_file_that_is_already_there, unit) {
    char path[] = "/tmp/meshclient-claim-XXXXXX";
    const int seeded = mkstemp(path);
    MESH_TEST_FAIL_IF(seeded < 0, "could not seed a file to claim");
    (void)!write(seeded, "old", 3U);
    close(seeded);

    const int first = mesh_usb_msc_claim(path);
    if (first >= 0) {
        close(first);
    }
    const int second = mesh_usb_msc_claim(path);
    if (second >= 0) {
        close(second);
    }
    const int missing = mesh_usb_msc_claim("");
    (void)unlink(path);

    MESH_TEST_FAIL_IF(first < 0, "a file that is already there should be claimable");
    MESH_TEST_FAIL_IF(second < 0, "and claimable again - O_CREAT and O_EXCL must not meet");
    MESH_TEST_FAIL_IF(missing != -EINVAL, "and an empty path is a refusal rather than an open");
    record_success(test_name);
}

/*
 * A cancel on a struct that was zeroed and never started.
 *
 * This is the download's `kill(0)` bug asked of the second child: a zeroed struct holds 0 where
 * a pid goes and 0 where an fd goes, and 0 means "the whole process group" to kill() and
 * "stdin" to close(). An app that cleans up its modules cleans this one up too.
 */
MESH_TEST_CASE(usb_msc_write_survives_a_cancel_it_never_started, unit) {
    struct mesh_usb_msc_write write;
    memset(&write, 0, sizeof write);
    mesh_usb_msc_write_cancel(&write);
    mesh_usb_msc_write_tick(&write, 1000U);
    mesh_usb_msc_write_cancel(&write);

    /* If stdin had been closed, this would fail. */
    MESH_TEST_FAIL_IF(fcntl(STDIN_FILENO, F_GETFD) < 0,
                      "a cancel must not close the client's stdin");
    MESH_TEST_FAIL_IF(mesh_usb_msc_write_progress(&write) != 0U, "and nothing was written");
    record_success(test_name);
}

/* ---- the install ---------------------------------------------------------------------------
 */

struct install_run {
    bool finished;
    enum mesh_firmware_install_state state;
    enum mesh_firmware_install_error error;
    unsigned arm_calls;
    int arm_result;
};

static void install_done(void *userdata, const struct mesh_firmware_install *install) {
    struct install_run *const run = (struct install_run *)userdata;
    run->finished = true;
    run->state = install->state;
    run->error = install->error;
}

static int install_arm(void *userdata) {
    struct install_run *const run = (struct install_run *)userdata;
    run->arm_calls += 1U;
    return run->arm_result;
}

/* Writes the fixture image to a path the install can be pointed at. */
static bool stage_image(const char *dir, size_t blocks, char *out, size_t out_len) {
    size_t len = 0U;
    uint8_t *const image = mesh_test_uf2_whole(blocks, &len);
    if (image == NULL) {
        return false;
    }
    if (snprintf(out, out_len, "%s/firmware.uf2", dir) >= (int)out_len) {
        free(image);
        return false;
    }
    FILE *const file = fopen(out, "wb");
    if (file == NULL) {
        free(image);
        return false;
    }
    const bool ok = fwrite(image, 1U, len, file) == len;
    free(image);
    return fclose(file) == 0 && ok;
}

/*
 * The whole handover, end to end: arm, watch the radio go away, watch a bootloader arrive, find
 * its drive, write the blocks, and watch the bootloader go away again.
 *
 * The fixture tree is edited between ticks, which is what a re-enumeration is: the radio's
 * directory is removed and the bootloader's appears, and at the end the bootloader's is removed
 * because a board that counted `numBlocks` blocks resets itself.
 */
MESH_TEST_CASE(install_writes_the_image_and_waits_for_the_board_to_restart, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    char image_path[128];
    bool built = usb_radio(fixture.usb, "2-1");
    built = built && stage_image(fixture.dev, 2U, image_path, sizeof image_path);
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the fixtures");

    struct install_run run;
    memset(&run, 0, sizeof run);
    struct mesh_firmware_install install;
    memset(&install, 0, sizeof install);

    const int started =
        mesh_firmware_install_start(&install, NULL, image_path, "2-1:1.1", MESH_UF2_FAMILY_NRF52840,
                                    install_arm, &run, install_done, &run);
    MESH_TEST_FAIL_IF_CLEANUP(started != 0, fixture_close(&fixture), "the install should start");
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_ARMING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "and should be arming once the verb has gone out");
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_firmware_install_holds_the_radio(&install),
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "with the radio held from the moment the verb goes out");

    /* One clock, and it is the real one plus a nudge per step: the write below is a real child
       whose deadline is measured against the same monotonic clock, so a simulated `now` here
       would have the write time out before it started. Each nudge is what walks past the poll
       gate, which is the only thing here that cares how much time passed. */
    uint64_t offset = 1000U;
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + offset);
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_ARMING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "a radio still answering means the reset has not happened yet");

    /* The board goes into DFU: the CDC pair comes back beside a mass-storage interface, with a
       new product id, and `usb-storage` publishes the disk a moment later. */
    char command[256];
    snprintf(command, sizeof command, "rm -rf '%s'/2-1*", fixture.usb);
    MESH_TEST_FAIL_IF_CLEANUP(system(command) != 0,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not take the radio off the bus");
    offset += 1000U;
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + offset);
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_WAITING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "a port that went away is the reply this verb has");

    MESH_TEST_FAIL_IF_CLEANUP(!usb_bootloader(fixture.usb, "2-1"),
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not put the bootloader on the bus");
    offset += 1000U;
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + offset);
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_WAITING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "a bootloader with no drive yet is still a wait");

    MESH_TEST_FAIL_IF_CLEANUP(!block_device(fixture.block, "sda", "2-1", "65801"),
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not publish the drive");
    offset += 1000U;
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + offset);
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_WRITING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "the drive arriving is what starts the write");

    /* The write is a real child, so this half runs on the real clock. */
    const uint64_t give_up = mesh_time_monotonic_ms() + 10000U;
    while (install.state == MESH_FIRMWARE_INSTALL_WRITING && mesh_time_monotonic_ms() < give_up) {
        mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + offset);
    }
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_RESTARTING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "a finished write waits for the board rather than declaring done");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_install_progress(&install) != 100U,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "with the bar at 100 rather than falling back to 0");

    /* The bootloader counted `numBlocks` blocks and reset itself; nothing told it to. The bus
       is empty for the moment it takes to come back, and that on its own is not the answer -
       a pulled cable looks exactly like it. */
    MESH_TEST_FAIL_IF_CLEANUP(system(command) != 0,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not take the bootloader off the bus");
    offset += 1000U;
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + offset);
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_RESTARTING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "an empty bus is still a wait: the board has not come back yet");

    /* And it comes back running what we wrote, which is the half that says so. */
    MESH_TEST_FAIL_IF_CLEANUP(!usb_radio(fixture.usb, "2-1"),
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not put the radio back on the bus");
    offset += 1000U;
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + offset);

    /* What actually landed on the drive. */
    char device_path[128];
    snprintf(device_path, sizeof device_path, "%s/sda", fixture.dev);
    size_t expect_len = 0U;
    uint8_t *const expect = mesh_test_uf2_whole(2U, &expect_len);
    bool same = false;
    FILE *const file = fopen(device_path, "rb");
    if (file != NULL && expect != NULL) {
        uint8_t back[2048];
        const size_t got = fread(back, 1U, sizeof back, file);
        same = got == expect_len && memcmp(back, expect, expect_len) == 0;
    }
    if (file != NULL) {
        fclose(file);
    }
    free(expect);

    const struct install_run finished = run;
    const unsigned blocks = install.uf2.blocks;
    const bool holds = mesh_firmware_install_holds_the_radio(&install);
    mesh_firmware_install_cancel(&install);
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!finished.finished, "the install should have reported");
    MESH_TEST_FAIL_IF(finished.state != MESH_FIRMWARE_INSTALL_DONE,
                      "a bootloader that went away is the board running what we wrote");
    MESH_TEST_FAIL_IF(finished.error != MESH_FIRMWARE_INSTALL_ERROR_NONE, "with no error");
    MESH_TEST_FAIL_IF(finished.arm_calls != 1U, "the verb goes out once");
    MESH_TEST_FAIL_IF(blocks != 2U, "the staged image is two blocks");
    MESH_TEST_FAIL_IF(!same, "and every byte of it should be on the drive");
    MESH_TEST_FAIL_IF(holds, "a finished install lets go of the radio");
    record_success(test_name);
}

/*
 * The guard that protects the board rather than the download.
 *
 * A UF2 block carries no checksum and the bootloader flashes what it is given, so the family id
 * in the file is the last thing standing between a T114 and an image for another chip. Two
 * halves: a file for the wrong family is refused, and an expectation of **0** is refused too -
 * 0 means "this architecture has no UF2 path", and letting it through as "any family will do"
 * would turn the check off exactly where it matters.
 */
MESH_TEST_CASE(install_refuses_an_image_that_is_not_for_this_board, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    char image_path[128];
    MESH_TEST_FAIL_IF_CLEANUP(!stage_image(fixture.dev, 2U, image_path, sizeof image_path),
                              fixture_close(&fixture), "could not stage the image");

    struct mesh_firmware_install install;
    memset(&install, 0, sizeof install);
    const int wrong_family = mesh_firmware_install_start(
        &install, NULL, image_path, "2-1:1.1", MESH_UF2_FAMILY_RP2040, NULL, NULL, NULL, NULL);
    const enum mesh_firmware_install_error wrong_error = install.error;

    memset(&install, 0, sizeof install);
    const int no_family = mesh_firmware_install_start(&install, NULL, image_path, "2-1:1.1", 0U,
                                                      NULL, NULL, NULL, NULL);

    const enum mesh_firmware_install_error no_family_error = install.error;

    /* A struct that was zeroed and never started, so anything left at 0 reads as "none" - which
       is exactly how a refusal comes to print as "could not start the install: none". */
    memset(&install, 0, sizeof install);
    const int missing =
        mesh_firmware_install_start(&install, NULL, "/nowhere/at/all.uf2", "2-1:1.1",
                                    MESH_UF2_FAMILY_NRF52840, NULL, NULL, NULL, NULL);
    const enum mesh_firmware_install_error missing_error = install.error;
    const enum mesh_firmware_install_state missing_state = install.state;
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(wrong_family == 0, "a T114 image offered as an RP2040 one is refused");
    MESH_TEST_FAIL_IF(wrong_error != MESH_FIRMWARE_INSTALL_ERROR_WRONG_IMAGE,
                      "and the refusal says which board rather than which file");
    MESH_TEST_FAIL_IF(no_family != -EINVAL,
                      "an architecture with no UF2 path is refused rather than accepting anything");
    MESH_TEST_FAIL_IF(no_family_error != MESH_FIRMWARE_INSTALL_ERROR_UNAVAILABLE,
                      "and says so rather than leaving the error at none");
    MESH_TEST_FAIL_IF(missing != -EIO, "and an image that is not there is refused before anything");
    /*
     * The contract in the header: a negative start still fills in `state` and `error`. It is the
     * whole difference between a row that says "the image is not there" and one that says
     * nothing at all, and the failure mode is silent - a zeroed struct reads as NONE, which is
     * a legible-looking answer that happens to be a lie.
     */
    MESH_TEST_FAIL_IF(missing_error != MESH_FIRMWARE_INSTALL_ERROR_UNAVAILABLE,
                      "an unreadable image is UNAVAILABLE, not none");
    MESH_TEST_FAIL_IF(missing_state != MESH_FIRMWARE_INSTALL_FAILED,
                      "and the state says failed rather than idle");
    record_success(test_name);
}

/*
 * The radio that never went anywhere, and the bootloader that never arrived. Two failures with
 * two names, because they are two different sentences: one leaves the board running its old
 * firmware and the other leaves a user reaching for the reset button.
 */
MESH_TEST_CASE(install_tells_a_radio_that_stayed_from_a_bootloader_that_never_came, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    char image_path[128];
    bool built = usb_radio(fixture.usb, "2-1");
    built = built && stage_image(fixture.dev, 2U, image_path, sizeof image_path);
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the fixtures");

    /* The verb went out and the radio is still sitting there answering. */
    struct install_run run;
    memset(&run, 0, sizeof run);
    struct mesh_firmware_install install;
    memset(&install, 0, sizeof install);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_install_start(&install, NULL, image_path, "2-1:1.1",
                                                          MESH_UF2_FAMILY_NRF52840, install_arm,
                                                          &run, install_done, &run) != 0,
                              fixture_close(&fixture), "the install should start");
    for (uint64_t now = 1000U; now <= 61000U && !run.finished; now += 1000U) {
        mesh_firmware_install_tick(&install, now);
    }
    const struct install_run stayed = run;
    mesh_firmware_install_cancel(&install);

    /* And the verb the radio refused outright: nothing is armed and the board is untouched. */
    memset(&run, 0, sizeof run);
    run.arm_result = -ENOTCONN;
    memset(&install, 0, sizeof install);
    const int refused =
        mesh_firmware_install_start(&install, NULL, image_path, "2-1:1.1", MESH_UF2_FAMILY_NRF52840,
                                    install_arm, &run, install_done, &run);
    const enum mesh_firmware_install_error refused_error = install.error;
    mesh_firmware_install_cancel(&install);

    /* Nothing on the bus at all: the double-tap that never came. */
    char command[256];
    snprintf(command, sizeof command, "rm -rf '%s'/2-1*", fixture.usb);
    (void)system(command);
    memset(&run, 0, sizeof run);
    memset(&install, 0, sizeof install);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_install_start(&install, NULL, image_path, "",
                                                          MESH_UF2_FAMILY_NRF52840, NULL, NULL,
                                                          install_done, &run) != 0,
                              fixture_close(&fixture), "an unarmed install should start too");
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_WAITING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "with no verb to send, it waits from the first moment");
    for (uint64_t now = 1000U; now <= 61000U && !run.finished; now += 1000U) {
        mesh_firmware_install_tick(&install, now);
    }
    const struct install_run never = run;
    mesh_firmware_install_cancel(&install);
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!stayed.finished || stayed.error != MESH_FIRMWARE_INSTALL_ERROR_ARM,
                      "a radio still answering after the timeout is the verb not landing");
    MESH_TEST_FAIL_IF(refused == 0 || refused_error != MESH_FIRMWARE_INSTALL_ERROR_ARM,
                      "a verb the link refused fails before anything is armed");
    MESH_TEST_FAIL_IF(!never.finished || never.error != MESH_FIRMWARE_INSTALL_ERROR_NO_BOOTLOADER,
                      "and a bus with nothing on it is a bootloader that never came");
    record_success(test_name);
}

/*
 * The write that ends early because the board reset, which is what every recovery looks like.
 *
 * A UF2 bootloader counts the distinct blocks it has been given and resets the moment it holds
 * `numBlocks` of them - nothing tells it the transfer is over, the file does. So the write
 * *after* an interrupted one finishes partway through: the blocks the first attempt missed
 * arrive, the count completes, and the drive disappears mid-copy. Watched on a T114 on
 * 2026-09-10, where a first write raced by the platform's own mount left the board a few blocks
 * short and the second reset it at sector 832 with `device firmware changed` in the kernel log.
 * The client called that a write failure, so the one path this whole half rests on reported as
 * broken every time it worked.
 *
 * The stall below stands in for the drive going away, because a fixture writes to a file and a
 * file does not vanish under its writer. What is being asked is the decision either produces:
 * a write that did not finish, plus a bootloader that is no longer on the bus.
 */
MESH_TEST_CASE(install_reads_a_write_that_ended_with_the_bootloader_as_the_board_restarting, unit) {
    /* A drive whose writes fail rather than a drive that disappears, because a suite cannot
       take a device away from a running child. `/dev/full` opens and then refuses every byte,
       which is the same thing from the child's side: a real fork, a real open, a real failure
       partway. */
    MESH_TEST_FAIL_IF(access("/dev/full", W_OK) != 0, "this needs /dev/full to refuse a write");

    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    char image_path[128];
    char drive[PATH_MAX];
    bool built = usb_bootloader(fixture.usb, "2-1");
    built = built && block_device(fixture.block, "sda", "2-1", "65801");
    built = built && stage_image(fixture.dev, 2U, image_path, sizeof image_path);
    built = built && snprintf(drive, sizeof drive, "%s/sda", fixture.dev) < (int)sizeof drive &&
            symlink("/dev/full", drive) == 0;
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the fixtures");

    struct install_run run;
    memset(&run, 0, sizeof run);
    struct mesh_firmware_install install;
    memset(&install, 0, sizeof install);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_install_start(&install, NULL, image_path, "2-1:1.1",
                                                          MESH_UF2_FAMILY_NRF52840, NULL, NULL,
                                                          install_done, &run) != 0,
                              fixture_close(&fixture), "the install should start");
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms());
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_WRITING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "the drive is there, so the write should be running");

    /* The board resets: the bootloader leaves the bus, and the write it was taking stops. */
    char command[256];
    snprintf(command, sizeof command, "rm -rf '%s'/2-1*", fixture.usb);
    MESH_TEST_FAIL_IF_CLEANUP(system(command) != 0,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not take the bootloader off the bus");

    const uint64_t give_up = mesh_time_monotonic_ms() + 10000U;
    while (install.state == MESH_FIRMWARE_INSTALL_WRITING && mesh_time_monotonic_ms() < give_up) {
        mesh_firmware_install_tick(&install, mesh_time_monotonic_ms());
    }
    const enum mesh_firmware_install_state after_write = install.state;
    const bool wrote_nothing = install.write.written < install.write.total;

    /* And it comes back running what it was given. */
    MESH_TEST_FAIL_IF_CLEANUP(!usb_radio(fixture.usb, "2-1"),
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not put the radio back on the bus");
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms() + 1000U);
    const struct install_run recovered = run;
    mesh_firmware_install_cancel(&install);
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!wrote_nothing, "the drive should have refused the image");
    MESH_TEST_FAIL_IF(after_write != MESH_FIRMWARE_INSTALL_RESTARTING,
                      "a write that stopped with the bootloader gone is the board restarting");
    MESH_TEST_FAIL_IF(!recovered.finished, "the install should have reported");
    MESH_TEST_FAIL_IF(recovered.state != MESH_FIRMWARE_INSTALL_DONE ||
                          recovered.error != MESH_FIRMWARE_INSTALL_ERROR_NONE,
                      "and a radio answering where the bootloader was is done, not a failure");
    record_success(test_name);
}

/*
 * The cable pulled, which ends the same way and must not read the same way.
 *
 * The bootloader disappearing is half the evidence and it is the half a yank produces too. This
 * is the outcome the USB path's whole safety argument is about - the board is still sitting in
 * its bootloader wherever it now is, and the recovery is to plug it back in and write again -
 * so calling it done would be the one lie available here.
 */
MESH_TEST_CASE(install_does_not_call_a_pulled_cable_a_finished_install, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    char image_path[128];
    bool built = usb_bootloader(fixture.usb, "2-1");
    built = built && block_device(fixture.block, "sda", "2-1", "65801");
    built = built && stage_image(fixture.dev, 2U, image_path, sizeof image_path);
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the fixtures");

    struct install_run run;
    memset(&run, 0, sizeof run);
    struct mesh_firmware_install install;
    memset(&install, 0, sizeof install);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_install_start(&install, NULL, image_path, "2-1:1.1",
                                                          MESH_UF2_FAMILY_NRF52840, NULL, NULL,
                                                          install_done, &run) != 0,
                              fixture_close(&fixture), "the install should start");
    mesh_firmware_install_tick(&install, mesh_time_monotonic_ms());
    MESH_TEST_FAIL_IF_CLEANUP(install.state != MESH_FIRMWARE_INSTALL_WRITING,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "the drive is there, so the write should be running");

    char command[256];
    snprintf(command, sizeof command, "rm -rf '%s'/2-1*", fixture.usb);
    MESH_TEST_FAIL_IF_CLEANUP(system(command) != 0,
                              (mesh_firmware_install_cancel(&install), fixture_close(&fixture)),
                              "could not take the board off the bus");

    /* Nothing comes back, however long it is given. */
    uint64_t now = mesh_time_monotonic_ms() + 120000U;
    for (unsigned tick = 0; tick < 80U && !run.finished; ++tick) {
        mesh_firmware_install_tick(&install, now);
        now += 1000U;
    }
    const struct install_run pulled = run;
    mesh_firmware_install_cancel(&install);
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!pulled.finished, "the install should have reported");
    MESH_TEST_FAIL_IF(pulled.state != MESH_FIRMWARE_INSTALL_FAILED,
                      "an empty bus is not a board running new firmware");
    MESH_TEST_FAIL_IF(pulled.error != MESH_FIRMWARE_INSTALL_ERROR_NO_RADIO,
                      "and it says the board never came back, not that the write was bad");
    record_success(test_name);
}

/*
 * The board that took every block and stayed in its bootloader.
 *
 * It means it never counted `numBlocks` of them, so the image it holds is partial - and because
 * this is the USB path, the whole recovery is to write it again. Reporting it as done would be
 * the one lie this path can tell.
 */
MESH_TEST_CASE(install_refuses_to_call_it_done_while_the_bootloader_is_still_there, unit) {
    struct install_fixture fixture;
    MESH_TEST_FAIL_IF(!fixture_open(&fixture), "could not lay out the fixture trees");

    char image_path[128];
    bool built = usb_bootloader(fixture.usb, "2-1");
    built = built && block_device(fixture.block, "sda", "2-1", "65801");
    built = built && stage_image(fixture.dev, 2U, image_path, sizeof image_path);
    MESH_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build the fixtures");

    struct install_run run;
    memset(&run, 0, sizeof run);
    struct mesh_firmware_install install;
    memset(&install, 0, sizeof install);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_firmware_install_start(&install, NULL, image_path, "2-1:1.1",
                                                          MESH_UF2_FAMILY_NRF52840, NULL, NULL,
                                                          install_done, &run) != 0,
                              fixture_close(&fixture), "the install should start");

    const uint64_t give_up = mesh_time_monotonic_ms() + 15000U;
    uint64_t now = mesh_time_monotonic_ms();
    while (!run.finished && mesh_time_monotonic_ms() < give_up) {
        mesh_firmware_install_tick(&install, now);
        /* Real time while the child runs, and a jump once it is done so the restart window
           expires without the suite sitting through twenty seconds of it. */
        now = install.state == MESH_FIRMWARE_INSTALL_RESTARTING ? now + 5000U
                                                                : mesh_time_monotonic_ms();
    }
    const struct install_run stuck = run;
    mesh_firmware_install_cancel(&install);
    fixture_close(&fixture);

    MESH_TEST_FAIL_IF(!stuck.finished, "the install should have reported");
    MESH_TEST_FAIL_IF(stuck.error != MESH_FIRMWARE_INSTALL_ERROR_NO_RESTART,
                      "a bootloader still sitting there never saw a full set of blocks");
    record_success(test_name);
}
