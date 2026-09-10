#pragma once

/*
 * Getting one board's firmware image out of a release zip, without downloading the zip.
 *
 * This is phase 2 of docs/radio-firmware-roadmap.md and it stops one step short of doing
 * anything: it ends with the image on disk, checked, and nothing written to a radio. What it
 * costs is **0.6 MB instead of 58** for the T114's 1.4 MB `.uf2`, and 1.4 instead of 170 for
 * the Heltec V3's - a ratio that gets better as upstream's zips grow rather than worse.
 *
 * Four steps, each one a range request, in the order a zip has to be read:
 *
 *   1. HEAD          how long is the file. Not decoration: the CDN in front of these answers
 *                    `501 Unsupported client range` to a suffix range, so the tail window has
 *                    to be asked for as an explicit `bytes=<start>-<end>`.
 *   2. the tail      64 KB, holding the end-of-central-directory record and - for every
 *                    release zip measured - the whole central directory with it. When it does
 *                    not, the directory is fetched on its own and this is five steps.
 *   3. a local header  30 bytes, for the two field lengths that place the data.
 *   4. the member    ~0.5 MB, deflated.
 *
 * Then the inflate, and the inflate is also where the download's integrity check comes from.
 * The client links libdbus and libm and nothing else, so there is no zlib here; the member is
 * wrapped in a gzip envelope - ten bytes in front, the CRC32 and the uncompressed size from
 * the central directory behind - and handed to the device's own `gzip -dc`, which inflates it
 * *and checks both*. Measured on a Brick against a real member: busybox 1.27.2 inflated 1.4 MB
 * in 0.03 s, answered `gzip: crc error` to a truncated member and `gzip: incorrect length` to
 * a wrong size. So the envelope is not a way of avoiding a dependency that happens to verify -
 * it is the verification, and both of its failure modes are known to fire.
 *
 * **The radio link must be down for the duration.** The Brick's Wi-Fi and its Bluetooth are
 * one part behind one antenna, which is why mesh_updater_holds_the_radio() exists; a couple of
 * megabytes of curl was measured to break a live BLE link 36 ms in. This module does not take
 * that hold itself - it has no idea a radio exists - so its caller does, exactly as the
 * self-updater's does.
 */

#include "mesh/core/fetch.h"
#include "mesh/utils/zip.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* Enough for a release asset URL: the tag and the file name both carry the version. */
#define MESH_FIRMWARE_DOWNLOAD_URL_MAX 320U

/* Where the download is. Every one of these is a different thing to say on a screen, which is
   why they are states rather than a boolean and a percentage. */
enum mesh_firmware_download_state {
    MESH_FIRMWARE_DOWNLOAD_IDLE = 0,
    MESH_FIRMWARE_DOWNLOAD_MEASURING, /* HEAD: how long is the zip */
    MESH_FIRMWARE_DOWNLOAD_READING,   /* the tail window, and the directory if it did not fit */
    MESH_FIRMWARE_DOWNLOAD_LOCATING,  /* the local header */
    MESH_FIRMWARE_DOWNLOAD_FETCHING,  /* the member - the only step with a bar worth drawing */
    MESH_FIRMWARE_DOWNLOAD_INFLATING,
    MESH_FIRMWARE_DOWNLOAD_READY,
    MESH_FIRMWARE_DOWNLOAD_FAILED,
    MESH_FIRMWARE_DOWNLOAD_STATE_COUNT,
};

/* Why it failed. Told apart because they are different sentences and, more to the point,
   because two of them mean "try again" and the rest do not. */
enum mesh_firmware_download_error {
    MESH_FIRMWARE_DOWNLOAD_ERROR_NONE = 0,
    /* The device has neither curl nor wget, or there is no loop to read one through. */
    MESH_FIRMWARE_DOWNLOAD_ERROR_UNAVAILABLE,
    /* A step's fetch failed, timed out or exited non-zero. Retryable. */
    MESH_FIRMWARE_DOWNLOAD_ERROR_NETWORK,
    /* The staging directory could not be written to, or a staged file could not be read back. */
    MESH_FIRMWARE_DOWNLOAD_ERROR_STAGING,
    /* What came back is not a zip: no end-of-central-directory record, or a directory that did
       not walk. Also what a captive portal's login page looks like from here. */
    MESH_FIRMWARE_DOWNLOAD_ERROR_NOT_A_ZIP,
    /* The zip is fine and does not contain that file. This release does not build for this
       board - a real answer, and not the same row as any of the above. */
    MESH_FIRMWARE_DOWNLOAD_ERROR_NO_MEMBER,
    /* Compressed with something that is neither deflate nor store. */
    MESH_FIRMWARE_DOWNLOAD_ERROR_UNSUPPORTED,
    /* `gzip -dc` refused it: `crc error`, or `incorrect length`. The bytes arrived and they
       are not the bytes the directory described. Retryable, once. */
    MESH_FIRMWARE_DOWNLOAD_ERROR_INFLATE,
    MESH_FIRMWARE_DOWNLOAD_ERROR_COUNT,
};

/* Called once per started download, from the loop, when it has finished or failed. */
struct mesh_firmware_download;
typedef void (*mesh_firmware_download_done_fn)(void *userdata,
                                               const struct mesh_firmware_download *download);

struct mesh_firmware_download {
    /* Borrowed. The caller owns the fetcher and may not use it while a download is running:
       one child at a time is the fetcher's rule, not this module's. */
    struct mesh_fetch *fetch;

    enum mesh_firmware_download_state state;
    enum mesh_firmware_download_error error;

    char zip_url[MESH_FIRMWARE_DOWNLOAD_URL_MAX];
    char member[MESH_ZIP_NAME_MAX];
    /* The directory staged files are written into. On a Brick this is on /mnt/UDISK and
       deliberately **not** beside the client's own .update staging on /mnt/SDCARD: the USB
       path's bootloader mounts a ghost drive over that card the moment the radio reboots, so
       an image staged there vanishes from its own path between being written and being read. */
    char staging[MESH_FETCH_PATH_MAX];

    /* Filled in as the steps answer. */
    uint64_t zip_size;
    uint64_t central_offset;
    uint64_t window_offset;
    size_t window_len;
    /* Set when the tail window did not hold the whole central directory and it was fetched on
       its own - which no release zip measured needs, and which decides both what the next read
       lands in and how it is walked. */
    bool directory_only;
    struct mesh_zip_entry entry;
    bool located;
    uint64_t data_offset;

    /*
     * The child doing the inflate, or not a pid. Reaped by mesh_firmware_download_tick().
     *
     * "Not a pid" is anything <= 0 rather than -1 alone, because a zeroed struct is how this is
     * meant to be started and 0 is what a zeroed struct holds - and kill(0, ...) is the whole
     * process group, not a no-op.
     */
    pid_t inflater;
    uint64_t inflate_deadline_ms;

    /* Stable for the duration of a request, because the fetcher holds pointers to them. */
    char active_path[MESH_FETCH_PATH_MAX];
    char range[64];

    mesh_firmware_download_done_fn on_done;
    void *userdata;
};

/*
 * Starts fetching `member` - a **basename**, because the path in front of it moves between
 * releases - out of the zip at `zip_url`, staging into `staging_dir`.
 *
 * Returns 0, or -errno: -EINVAL for a missing argument, -EBUSY when one is already running,
 * -ENOTSUP with no fetcher. On any error nothing was spawned and `on_done` will not be called;
 * on 0 it is called exactly once, later, from the loop.
 */
int mesh_firmware_download_start(struct mesh_firmware_download *download, struct mesh_fetch *fetch,
                                 const char *zip_url, const char *member, const char *staging_dir,
                                 mesh_firmware_download_done_fn on_done, void *userdata);

/*
 * Drives the deadline on the inflate and reaps it. Call every loop turn, beside
 * mesh_fetch_tick() - the fetch half of this drives itself off that, and the child that
 * inflates is ours rather than the fetcher's.
 */
void mesh_firmware_download_tick(struct mesh_firmware_download *download, uint64_t now_ms);

/* Kills anything in flight, removes the staged intermediates and does not report an outcome.
   For a caller that has decided the answer itself - a cancelled press, a shutdown. */
void mesh_firmware_download_cancel(struct mesh_firmware_download *download);

/* True while a download is running. */
bool mesh_firmware_download_busy(const struct mesh_firmware_download *download);

/*
 * How far the member has landed, 0-100, or 0 before the fetch of it begins.
 *
 * Measured against the **compressed** size from the central directory, because what is landing
 * is the zip member; dividing by the uncompressed size would stop the bar at 35% and call it
 * done. It is a stat() on a file this process named, which is the same trick the self-updater
 * uses and the reason neither of them has to scrape curl's terminal meter.
 */
unsigned mesh_firmware_download_progress(const struct mesh_firmware_download *download);

/*
 * Where the finished image is, or NULL until it is. Valid while the download is not restarted;
 * the file is the caller's to use and to delete.
 */
const char *mesh_firmware_download_image_path(const struct mesh_firmware_download *download,
                                              char *out, size_t out_len);

/* What the image should be, from the central directory - the size the caller checks a UF2's
   block count against, and the CRC the inflate already checked. 0 before the directory is
   read. */
uint64_t mesh_firmware_download_image_size(const struct mesh_firmware_download *download);

#ifdef __cplusplus
}
#endif
