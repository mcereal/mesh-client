#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/usb_msc.h"

#include "mesh/core/event_loop.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#define MESH_USB_MSC_SYSFS_BLOCK_DEFAULT "/sys/block"
#define MESH_USB_MSC_PROC_MOUNTS_DEFAULT "/proc/mounts"
#define MESH_USB_MSC_DEV_ROOT_DEFAULT "/dev"

/*
 * Both of these are readings rather than actions, so both get an environment seam and neither
 * gets a mock: a fixture tree and a fixture mounts file exercise the real walk. The acting half
 * needs none - an unmount is `umount2()` and the write takes a path, so a test hands it a
 * temporary file and gets the whole of the real code path.
 */
static const char *sysfs_block_root(void) {
    const char *const from_env = getenv("MESHCLIENT_SYSFS_BLOCK");
    return (from_env != NULL && from_env[0] != '\0') ? from_env : MESH_USB_MSC_SYSFS_BLOCK_DEFAULT;
}

static const char *proc_mounts_path(void) {
    const char *const from_env = getenv("MESHCLIENT_PROC_MOUNTS");
    return (from_env != NULL && from_env[0] != '\0') ? from_env : MESH_USB_MSC_PROC_MOUNTS_DEFAULT;
}

/* Where a block device's node lives. The third seam, and the one that makes the *whole* install
   testable rather than only its readings: with it pointed at a temporary directory the write
   goes to a file, which is the same `open`, the same `write` and the same `fdatasync` a block
   device takes. */
static const char *dev_root(void) {
    const char *const from_env = getenv("MESHCLIENT_DEV_ROOT");
    return (from_env != NULL && from_env[0] != '\0') ? from_env : MESH_USB_MSC_DEV_ROOT_DEFAULT;
}

/* How much goes out per write, and how much has to land before the count moves. 32 KB is about
   a third of a second on this bus, which is a bar that moves smoothly and a cancel that is
   never more than that far away. */
#define MESH_USB_MSC_CHUNK 32768U
/* The child's exit codes, so the parent can say which end failed rather than "status 1". */
#define MESH_USB_MSC_EXIT_OPEN 10
#define MESH_USB_MSC_EXIT_WRITE 11
#define MESH_USB_MSC_EXIT_SYNC 12
/* Silence that means the write has stopped rather than slowed. A stalled chunk on a bus that
   moves 32 KB in a third of a second is not a slow chunk. */
#define MESH_USB_MSC_IDLE_TIMEOUT_MS 30000U

/* ---- finding the drive ---------------------------------------------------------------------
 */

/* "2-1:1.2" names an interface of the USB device "2-1"; the drive belongs to the device. */
static bool usb_device_of(const char *interface_id, char *out, size_t out_len) {
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

/*
 * A block device belongs to a USB device when the sysfs path it is a symlink to walks through
 * that device's directory - `.../usb2/2-1/2-1:1.2/host0/target0:0:0/0:0:0:0/block/sda`.
 *
 * Matching on "/2-1/" rather than on "2-1" is what keeps "2-1" from matching "12-1" (no leading
 * slash) or "2-1.4" (no trailing one), which are both real names on a bus with a hub on it.
 */
static bool block_belongs_to(const char *root, const char *name, const char *device_name) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", root, name) >= (int)sizeof path) {
        return false;
    }
    char target[PATH_MAX];
    const ssize_t len = readlink(path, target, sizeof target - 1U);
    if (len < 0) {
        return false;
    }
    target[len] = '\0';

    char needle[80];
    if (snprintf(needle, sizeof needle, "/%s/", device_name) >= (int)sizeof needle) {
        return false;
    }
    return strstr(target, needle) != NULL;
}

/* The whole device's size, in sectors of 512, from <root>/<name>/size. 0 when unreadable. */
static uint64_t block_size_bytes(const char *root, const char *name) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s/size", root, name) >= (int)sizeof path) {
        return 0U;
    }
    FILE *const file = fopen(path, "re");
    if (file == NULL) {
        return 0U;
    }
    unsigned long long sectors = 0ULL;
    const bool ok = fscanf(file, "%llu", &sectors) == 1;
    fclose(file);
    return ok ? (uint64_t)sectors * 512ULL : 0U;
}

/*
 * True when `source` is this drive: the whole device, or a partition of it.
 *
 * The Brick's hotplug script mounts `/dev/sda` itself, and OpenWrt's `/sbin/block` may have
 * mounted `/dev/sda1` first. Both have to come off, and neither `/dev/sdaa` nor `/dev/sdb` is
 * either of them.
 */
static bool mount_source_is_device(const char *source, const char *device) {
    const size_t len = strlen(device);
    if (strncmp(source, device, len) != 0) {
        return false;
    }
    const char *const tail = source + len;
    if (tail[0] == '\0') {
        return true;
    }
    for (const char *at = tail; *at != '\0'; ++at) {
        if (*at < '0' || *at > '9') {
            return false;
        }
    }
    return true;
}

/* /proc/mounts escapes a space in a path as \040 and three other characters the same way. */
static void unescape_mount_path(char *path) {
    char *out = path;
    for (const char *in = path; *in != '\0'; ++in) {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '3' && in[2] >= '0' && in[2] <= '7' &&
            in[3] >= '0' && in[3] <= '7') {
            *out++ = (char)(((in[1] - '0') << 6) | ((in[2] - '0') << 3) | (in[3] - '0'));
            in += 3;
            continue;
        }
        *out++ = *in;
    }
    *out = '\0';
}

static void read_mounts(struct mesh_usb_msc_target *target) {
    FILE *const file = fopen(proc_mounts_path(), "re");
    if (file == NULL) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, file) != NULL) {
        char source[128];
        char point[MESH_USB_MSC_PATH_MAX];
        if (sscanf(line, "%127s %127s", source, point) != 2) {
            continue;
        }
        unescape_mount_path(source);
        unescape_mount_path(point);
        if (!mount_source_is_device(source, target->device)) {
            continue;
        }
        if (target->mount_count >= MESH_USB_MSC_MOUNTS_MAX) {
            target->too_many_mounts = true;
            break;
        }
        mesh_str_copy(target->mounts[target->mount_count],
                      sizeof target->mounts[target->mount_count], point);
        target->mount_count += 1U;
    }
    fclose(file);
}

int mesh_usb_msc_find(const struct mesh_serial_device_info *device,
                      struct mesh_usb_msc_target *out) {
    if (device == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);

    char device_name[64];
    if (!usb_device_of(device->id, device_name, sizeof device_name)) {
        return -EINVAL;
    }

    const char *const root = sysfs_block_root();
    DIR *const dir = opendir(root);
    if (dir == NULL) {
        return -errno;
    }

    char found[64] = {0};
    const struct dirent *entry = NULL;
    while (found[0] == '\0' && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (block_belongs_to(root, entry->d_name, device_name)) {
            mesh_str_copy(found, sizeof found, entry->d_name);
        }
    }
    closedir(dir);

    if (found[0] == '\0') {
        /* The ordinary answer for about a second after the board re-enumerates: the interface
           is there and `usb-storage` has not published the disk yet. */
        return -ENOENT;
    }

    if (snprintf(out->device, sizeof out->device, "%s/%s", dev_root(), found) >=
        (int)sizeof out->device) {
        out->device[0] = '\0';
        return -ENAMETOOLONG;
    }
    out->size_bytes = block_size_bytes(root, found);
    read_mounts(out);
    return 0;
}

int mesh_usb_msc_unmount(struct mesh_usb_msc_target *target) {
    if (target == NULL) {
        return -EINVAL;
    }
    if (target->too_many_mounts) {
        /* Nothing is unmounted: a drive with more mountpoints than this can name is a drive
           we would only half take off, and half is worse than none. */
        return -E2BIG;
    }
    /* Backwards, because /proc/mounts lists them in the order they were mounted and the later
       one is stacked on top - on the Brick, the bootloader's ghost FAT over the SD card. Taking
       the shadow off is also the restore: what it was hiding is still mounted underneath. */
    while (target->mount_count > 0U) {
        const char *const point = target->mounts[target->mount_count - 1U];
        if (umount2(point, 0) != 0 && errno != EINVAL && errno != ENOENT) {
            const int err = -errno;
            mesh_log_error("firmware", "Could not unmount %s from %s: %s", target->device, point,
                           strerror(-err));
            return err;
        }
        mesh_log_info("firmware", "Unmounted %s from %s", target->device, point);
        target->mount_count -= 1U;
    }
    return 0;
}

/* ---- the write -----------------------------------------------------------------------------
 */

static void write_release_fd(struct mesh_usb_msc_write *write) {
    /* `<= 0`, for the reason the pid test below is: a zeroed struct holds 0, and 0 is stdin -
       never a pipe this module opened. A `< 0` test here closes the client's own stdin the
       first time a cancel arrives before a start. */
    if (write->progress_fd <= 0) {
        return;
    }
    if (write->loop != NULL) {
        (void)mesh_event_loop_remove_fd(write->loop, write->progress_fd);
    }
    close(write->progress_fd);
    write->progress_fd = -1;
}

/* One decimal byte count per line. A read can land mid-line, so the tail is carried. */
static void write_consume(struct mesh_usb_msc_write *write, const char *bytes, size_t len,
                          uint64_t now_ms) {
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] != '\n') {
            if (write->pending_len + 1U < sizeof write->pending) {
                write->pending[write->pending_len++] = bytes[i];
            }
            continue;
        }
        write->pending[write->pending_len] = '\0';
        char *end = NULL;
        const unsigned long long value = strtoull(write->pending, &end, 10);
        if (end != write->pending && value <= write->total && (uint64_t)value >= write->written) {
            write->written = (uint64_t)value;
            write->idle_deadline_ms = now_ms + MESH_USB_MSC_IDLE_TIMEOUT_MS;
        }
        write->pending_len = 0U;
    }
}

/* Returns false at EOF. Never blocks: the fd is non-blocking. */
static bool write_drain(struct mesh_usb_msc_write *write, uint64_t now_ms) {
    if (write->progress_fd <= 0) {
        return false;
    }
    for (;;) {
        char buffer[256];
        const ssize_t got = read(write->progress_fd, buffer, sizeof buffer);
        if (got > 0) {
            write_consume(write, buffer, (size_t)got, now_ms);
            continue;
        }
        if (got == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

static int write_on_progress(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    struct mesh_usb_msc_write *const write = (struct mesh_usb_msc_write *)userdata;
    if (write != NULL) {
        /* Draining only; reaping is the tick's, so nothing here can block. The clock is read
           rather than passed because a count that arrived is what pushes the silence deadline
           out, and the loop does not carry one. */
        (void)write_drain(write, mesh_time_monotonic_ms());
    }
    return 0;
}

/*
 * The child. Everything in here runs after fork() in a single-threaded process, so it is
 * limited to what is safe there: `write`, `open`, `fdatasync`, `_exit`.
 *
 * `fdatasync` per chunk is the whole reason the byte count means anything. Buffered writes to a
 * block device are absorbed by the page cache and return at memory speed, so an unsynced child
 * would report 100% in a few milliseconds and then sit in `close()` for thirteen seconds - the
 * progress bar lying in exactly the direction that makes a user pull the cable.
 */
static void write_child(const uint8_t *image, size_t len, const char *device_path, int pipe_fd) {
    const int out = open(device_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (out < 0) {
        _exit(MESH_USB_MSC_EXIT_OPEN);
    }
    size_t at = 0U;
    while (at < len) {
        const size_t chunk = len - at > MESH_USB_MSC_CHUNK ? MESH_USB_MSC_CHUNK : len - at;
        size_t sent = 0U;
        while (sent < chunk) {
            const ssize_t put = write(out, image + at + sent, chunk - sent);
            if (put > 0) {
                sent += (size_t)put;
                continue;
            }
            if (put < 0 && errno == EINTR) {
                continue;
            }
            _exit(MESH_USB_MSC_EXIT_WRITE);
        }
        if (fdatasync(out) != 0) {
            _exit(MESH_USB_MSC_EXIT_SYNC);
        }
        at += chunk;

        char line[32];
        const int line_len = snprintf(line, sizeof line, "%llu\n", (unsigned long long)at);
        if (line_len > 0) {
            /* A parent that stopped reading is not a reason to stop writing: the bytes are
               what matters and the count is a courtesy. */
            (void)!write(pipe_fd, line, (size_t)line_len);
        }
    }
    _exit(0);
}

int mesh_usb_msc_write_start(struct mesh_usb_msc_write *write, struct mesh_event_loop *loop,
                             const uint8_t *image, size_t len, const char *device_path,
                             uint64_t now_ms) {
    if (write == NULL || image == NULL || len == 0U || device_path == NULL ||
        device_path[0] == '\0') {
        return -EINVAL;
    }
    if (write->state == MESH_USB_MSC_WRITE_RUNNING) {
        return -EBUSY;
    }

    int fds[2];
    if (pipe(fds) < 0) {
        return -errno;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        const int err = -errno;
        close(fds[0]);
        close(fds[1]);
        return err;
    }
    if (pid == 0) {
        close(fds[0]);
        write_child(image, len, device_path, fds[1]);
        _exit(MESH_USB_MSC_EXIT_WRITE); /* not reached */
    }

    close(fds[1]);
    memset(write, 0, sizeof *write);
    write->loop = loop;
    write->state = MESH_USB_MSC_WRITE_RUNNING;
    write->child = pid;
    write->progress_fd = fds[0];
    write->total = (uint64_t)len;
    write->idle_deadline_ms = now_ms + MESH_USB_MSC_IDLE_TIMEOUT_MS;

    const int flags = fcntl(write->progress_fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(write->progress_fd, F_SETFL, flags | O_NONBLOCK);
    }
    if (loop != NULL &&
        mesh_event_loop_add_fd(loop, write->progress_fd, EPOLLIN, write_on_progress, write) != 0) {
        /* Not fatal: without the loop the tick still drains the pipe, it just does so at
           whatever rate the caller ticks. */
        write->loop = NULL;
    }
    return 0;
}

static void write_finish(struct mesh_usb_msc_write *write, int status) {
    write->child = -1;
    write_release_fd(write);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        write->written = write->total;
        write->state = MESH_USB_MSC_WRITE_DONE;
        write->error = 0;
        return;
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    switch (code) {
    case MESH_USB_MSC_EXIT_OPEN:
        write->error = -EACCES;
        mesh_log_error("firmware", "The bootloader's drive could not be opened for writing");
        break;
    case MESH_USB_MSC_EXIT_SYNC:
        write->error = -EIO;
        mesh_log_error("firmware", "The drive stopped acknowledging writes");
        break;
    default:
        write->error = -EIO;
        mesh_log_error("firmware", "The write ended after %llu of %llu bytes (status %d)",
                       (unsigned long long)write->written, (unsigned long long)write->total, code);
        break;
    }
    write->state = MESH_USB_MSC_WRITE_FAILED;
}

void mesh_usb_msc_write_tick(struct mesh_usb_msc_write *write, uint64_t now_ms) {
    if (write == NULL || write->state != MESH_USB_MSC_WRITE_RUNNING) {
        return;
    }
    /*
     * `<= 0`, not `< 0`: a zeroed struct holds 0 where a pid goes, and kill() reads 0 as the
     * whole process group. The download's inflate learned this the expensive way - see
     * docs/radio-firmware-roadmap.md - and this is the second child with the same shape.
     */
    if (write->child <= 0) {
        return;
    }

    (void)write_drain(write, now_ms);

    if (now_ms >= write->idle_deadline_ms) {
        (void)kill(write->child, SIGKILL);
        (void)waitpid(write->child, NULL, 0);
        write->child = -1;
        write_release_fd(write);
        write->state = MESH_USB_MSC_WRITE_FAILED;
        write->error = -ETIMEDOUT;
        mesh_log_error("firmware", "The write stalled at %llu of %llu bytes",
                       (unsigned long long)write->written, (unsigned long long)write->total);
        return;
    }

    int status = 0;
    const pid_t reaped = waitpid(write->child, &status, WNOHANG);
    if (reaped == write->child) {
        /* Once more after the exit: the child's last count can still be in the pipe. */
        (void)write_drain(write, now_ms);
        write_finish(write, status);
    } else if (reaped < 0) {
        write->child = -1;
        write_release_fd(write);
        write->state = MESH_USB_MSC_WRITE_FAILED;
        write->error = -ECHILD;
    }
}

void mesh_usb_msc_write_cancel(struct mesh_usb_msc_write *write) {
    if (write == NULL) {
        return;
    }
    if (write->child > 0) {
        (void)kill(write->child, SIGKILL);
        (void)waitpid(write->child, NULL, 0);
        write->child = -1;
    }
    write_release_fd(write);
    write->state = MESH_USB_MSC_WRITE_IDLE;
    write->error = 0;
}

unsigned mesh_usb_msc_write_progress(const struct mesh_usb_msc_write *write) {
    if (write == NULL || write->total == 0U) {
        return 0U;
    }
    if (write->state == MESH_USB_MSC_WRITE_DONE) {
        return 100U;
    }
    const uint64_t percent = write->written * 100ULL / write->total;
    /* Never 100 while the child is alive: the last chunk is not written until it is. */
    return percent >= 100ULL ? 99U : (unsigned)percent;
}
