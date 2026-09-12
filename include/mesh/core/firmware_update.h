#pragma once

/*
 * One press, from "there is newer firmware for this radio" to "the radio is running it".
 *
 * This is the core half of phase 5 of docs/radio-firmware-roadmap.md. Everything it does has
 * already shipped: firmware_fetch.c gets the image, firmware_install.c writes a `.uf2` to a
 * bootloader over USB, firmware_ota.c streams an app image to the ESP32 loader over BLE. What
 * did not exist was anything that ran the three of them in order without a person typing
 * `--install-firmware` - so the whole feature was a command line, and this is the piece that
 * makes it a row.
 *
 * **One state enum across both buses, not two.** The two handovers really are different - one
 * writes blocks to a drive and the other streams chunks to a GATT characteristic - but a row on
 * a screen is answering "where has this got to", and to that question "writing" and "sending"
 * are the same sentence with different nouns in it. So the sub-modules' states are folded onto
 * one ladder here, the ladder is what the UI names, and the difference between the buses is
 * carried where it is actually load-bearing: in the confirm sheet, which has to say the thing
 * that is true (see the roadmap's §The UI), and in `detail`, which is whatever the radio or the
 * loader said in its own words.
 *
 * **The download holds the antenna; it does not hold the cable.** The Brick's Wi-Fi and its
 * Bluetooth are one part, so 0.5 MB over HTTPS and a BLE link cannot both be happening - that
 * is the hold `mesh_updater_holds_the_radio()` already takes for the client's own update. A
 * *serial* link needs no antenna at all, so the USB path keeps its radio the whole way through
 * and can arm the moment the image lands. The BLE path cannot: its link has to go down for the
 * download and come back for the `ota_request`, which is what MESH_FIRMWARE_UPDATE_READY is -
 * the image is staged and verified and we are waiting for the radio to answer again.
 *
 * **It owns its own fetcher and, on the BLE path, its own D-Bus connection.** The fetcher
 * because a check and a download are two presses that must not take each other's child; the
 * D-Bus connection for the reason firmware_ota.h gives - `dbus_bus_get()` hands every caller
 * one shared connection and whoever pops a message off it has taken it from everybody else, so
 * an install on the shared one would eat the transport's replies while that transport was still
 * carrying the request that started it.
 */

#include "mesh/core/fetch.h"
#include "mesh/core/firmware_catalog.h"
#include "mesh/core/firmware_fetch.h"
#include "mesh/core/firmware_install.h"
#include "mesh/core/firmware_ota.h"
#include "mesh/transport/ble_bluez.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* The radio's own words, or a sub-module's name for what went wrong. Untranslated, like a log
   line or a region code - see docs/i18n.md. */
#define MESH_FIRMWARE_UPDATE_DETAIL_MAX 128U
/* Where the image is staged. MESHCLIENT_FIRMWARE_STAGING overrides it, and on the Brick it
   must not be the SD card: plugging an nRF52 bootloader in gets its ghost FAT mounted over
   /mnt/SDCARD, taking the pak and everything in it with it. */
#define MESH_FIRMWARE_UPDATE_STAGING_DEFAULT "/tmp"

/*
 * Where the job has got to, as a row says it.
 *
 * One per thing a screen can name, and deliberately no more: `firmware_fetch` distinguishes
 * reading the release manifest from reading the board's `.mt.json`, and both are "working out
 * which file", which is one sentence. The two that carry a fraction are DOWNLOADING and
 * WRITING, and they are the two long enough for a bar to be worth drawing.
 */
enum mesh_firmware_update_state {
    MESH_FIRMWARE_UPDATE_IDLE = 0,
    /* Reading the release's manifest and the board's own, to learn which member of which zip. */
    MESH_FIRMWARE_UPDATE_RESOLVING,
    /* Range-reading the image out of a 46 MB zip and inflating it. Has a fraction. */
    MESH_FIRMWARE_UPDATE_DOWNLOADING,
    /*
     * The image is staged, checked against its CRC and read as an image for this chip, and we
     * are waiting for a radio to arm on.
     *
     * On the USB path this is instantaneous: the serial link never went away. On the BLE path
     * it is the reconnect - the download had the antenna, so the radio has been gone for the
     * length of it and auto-connect is bringing it back.
     */
    MESH_FIRMWARE_UPDATE_READY,
    /* The admin verb is out: enter_dfu_mode_request, or ota_request holding the radio to the
       image's SHA-256. */
    MESH_FIRMWARE_UPDATE_ARMING,
    /* Watching for what the radio turned into - a bootloader on the USB tree, or the loader
       advertising at the radio's address plus one. */
    MESH_FIRMWARE_UPDATE_WAITING,
    /* Blocks to a drive, or chunks to a characteristic. Has a fraction. */
    MESH_FIRMWARE_UPDATE_WRITING,
    /* The image is over and we are waiting for a radio to be there again. */
    MESH_FIRMWARE_UPDATE_RESTARTING,
    MESH_FIRMWARE_UPDATE_DONE,
    MESH_FIRMWARE_UPDATE_FAILED,
    MESH_FIRMWARE_UPDATE_STATE_COUNT,
};

/*
 * Why it stopped, coarsely - one row's worth.
 *
 * The three sub-modules have twenty-odd errors between them and every one of them is a real
 * distinction *to the module that raised it*. To a reader standing in Settings they are four
 * questions: did it never start, did the bytes not arrive, did the radio refuse, or did the
 * handover break. So this is the coarse answer and `detail` carries the fine one, which is the
 * same split the roadmap asks for - a string id, plus the firmware's own words where it
 * supplied any.
 */
enum mesh_firmware_update_error {
    MESH_FIRMWARE_UPDATE_ERROR_NONE = 0,
    /* Nothing was started: no fetcher, no release to install, or a board this client has no
       path to. The rows above already say which, which is what phase 1 is for. */
    MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE,
    /* A document or the image itself did not arrive, or did not survive its CRC. Nothing has
       been asked of the radio and it is still running. */
    MESH_FIRMWARE_UPDATE_ERROR_DOWNLOAD,
    /* The bytes arrived and are not an image for this board. The one guard standing between a
       T114 and a file for another nRF52840, so it refuses rather than trying. */
    MESH_FIRMWARE_UPDATE_ERROR_WRONG_IMAGE,
    /* No radio came back to arm on, or it would not take the request. Untouched either way. */
    MESH_FIRMWARE_UPDATE_ERROR_NO_RADIO,
    /* The radio said no, in its own words (`detail`): no loader partition, an old loader, a
       build without the space for one. Untouched and still running. */
    MESH_FIRMWARE_UPDATE_ERROR_REFUSED,
    /*
     * The handover itself broke: the bootloader never appeared, the write stalled, the loader
     * refused the hash, the transfer died three times. **The radio is not necessarily running**
     * - see mesh_firmware_update_radio_in_loader(), which is the difference between "try again
     * whenever" and "that radio is off the mesh until this finishes".
     */
    MESH_FIRMWARE_UPDATE_ERROR_HANDOVER,
    MESH_FIRMWARE_UPDATE_ERROR_COUNT,
};

struct mesh_firmware_update;

/*
 * What this module cannot do for itself, because doing it would mean knowing what a session is.
 *
 * The two arms are the two buses' admin verbs; `radio_ready` is how READY ends, and it is a
 * question rather than a callback the app pushes because the module is the one holding the
 * deadline. `release_link` is called once, at the moment the radio has said everything it is
 * going to - after that the app must take the link down and stop scanning, or the transport
 * will spend the handover reconnecting to a radio that is now a loader.
 */
struct mesh_firmware_update_hooks {
    /* USB: enter_dfu_mode_request down the serial link. */
    mesh_firmware_install_arm_fn arm_usb;
    /* BLE: ota_request carrying the image's SHA-256. */
    mesh_firmware_ota_arm_fn arm_ble;
    /* True when a radio is connected and has said what it is - i.e. when arming could work. */
    bool (*radio_ready)(void *userdata);
    /* The radio has said all it will. Drop the link and leave the bus alone. May be NULL. */
    void (*release_link)(void *userdata);
    /* The open BLE link's connection interval, for the loader's sake. May be NULL. */
    mesh_firmware_ota_interval_fn request_interval;
    void *userdata;
};

typedef void (*mesh_firmware_update_done_fn)(void *userdata,
                                             const struct mesh_firmware_update *update);

struct mesh_firmware_update {
    struct mesh_event_loop *loop; /* borrowed; may be NULL */
    /* Its own, so a check running next door cannot take the child out from under a download. */
    struct mesh_fetch fetch;

    enum mesh_firmware_update_state state;
    enum mesh_firmware_update_error error;
    char detail[MESH_FIRMWARE_UPDATE_DETAIL_MAX];

    /* What was asked for, kept so a row can name it while the job runs and a caller can tell
       whether the answer it is holding is about the radio in front of it. */
    struct mesh_firmware_board board;
    char version[MESH_FIRMWARE_VERSION_MAX];
    uint32_t hw_model;
    enum mesh_firmware_path path;
    char staging[MESH_FETCH_PATH_MAX];
    /*
     * Where the radio is, in the terms its own bus uses: the serial transport's **id** on the
     * USB path, its BLE address on the other. Both are what the handover watches for the radio
     * coming back on, and on the USB path it is also what stops a write following a board that
     * moved to another port.
     *
     * The id rather than the tty, and the caller owes that distinction: `/dev/ttyACM0` is a
     * *label* that goes away with the radio, and the bootloader is looked for on the USB device
     * - so a label here makes the handover wait out its timeout and report that no bootloader
     * came. Sized for a port id, which is the longer of the two.
     */
    char where[64];

    struct mesh_firmware_fetch image;
    struct mesh_firmware_install usb;
    struct mesh_firmware_ota ble;
    /* The install's own D-Bus connection, opened when the BLE handover starts and closed with
       it. `bluez_open` rather than testing the struct, which has no idle spelling. */
    struct mesh_bluez_client bluez;
    bool bluez_open;
    bool link_released;

    /* When READY gives up waiting for a radio to arm on. */
    uint64_t deadline_ms;

    struct mesh_firmware_update_hooks hooks;
    mesh_firmware_update_done_fn on_done;
    void *userdata;

    /* Bumped whenever anything above changes, so app.c can publish without diffing. */
    uint32_t revision;
};

/* `loop` may be NULL, in which case the module reports itself unavailable. Returns 0, or
   -errno. */
int mesh_firmware_update_init(struct mesh_firmware_update *update, struct mesh_event_loop *loop);
void mesh_firmware_update_shutdown(struct mesh_firmware_update *update);

/* The CA bundle to verify with, resolved once per process by whoever found it - the Brick has
   no system store, so without it every HTTPS request exits 60. */
void mesh_firmware_update_use_ca_bundle(struct mesh_firmware_update *update, const char *path);

/* True when a fetcher was found, i.e. when a press could do anything at all. */
bool mesh_firmware_update_available(const struct mesh_firmware_update *update);

/*
 * Starts the whole thing for `board` at `release`.
 *
 * `board` is what the check identified - the single board, never one of several candidates, for
 * the reason mesh_firmware_board() answers NULL when it is ambiguous. `where` is the serial
 * transport's own id on the USB path - not the tty - and the radio's BLE address on the other,
 * and both may be empty: that
 * is the recovery case, a board already sitting in its bootloader or its loader, and it means
 * "any" rather than "none".
 *
 * Returns 0, or -errno. On 0 `on_done` is called exactly once, later, from a tick. On a
 * negative return nothing was started and no callback will arrive, but `state` and `error` are
 * filled in anyway, because a refusal is a row.
 */
int mesh_firmware_update_start(struct mesh_firmware_update *update,
                               const struct mesh_firmware_board *board,
                               const struct mesh_firmware_release *release, const char *where,
                               const struct mesh_firmware_update_hooks *hooks,
                               mesh_firmware_update_done_fn on_done, void *userdata);

/* Drives every step. Call every loop turn. */
void mesh_firmware_update_tick(struct mesh_firmware_update *update, uint64_t now_ms);

/*
 * What the radio said, as its ClientNotification text - the channel the BLE path's go-ahead and
 * refusal both arrive on. Ignored on the USB path and whenever nothing is arming.
 */
void mesh_firmware_update_radio_said(struct mesh_firmware_update *update, const char *text);

/* Stops everything, frees the image and reports nothing. Safe on a zeroed struct. A radio
   already in a loader stays in it - which is what radio_in_loader() is for. */
void mesh_firmware_update_cancel(struct mesh_firmware_update *update);

bool mesh_firmware_update_busy(const struct mesh_firmware_update *update);

/*
 * The same question asked of a state alone, for a caller holding the published byte rather than
 * the module.
 *
 * The UI is that caller twice over - the rows that take the section over while a job runs, and
 * the screen progress bar - and two switches over the same ladder is how they come to disagree
 * about whether DONE is still work. `mesh_firmware_update_busy()` is written in terms of this.
 */
bool mesh_firmware_update_state_busy(enum mesh_firmware_update_state state);

/*
 * True while the antenna is being used for something that is not the radio.
 *
 * Only the download: the two documents and the image come over Wi-Fi, and the Brick's Wi-Fi and
 * its Bluetooth are one part behind one antenna. Derived rather than held, like
 * mesh_updater_holds_the_radio(), so a failure lifts it by failing.
 */
bool mesh_firmware_update_holds_the_antenna(const struct mesh_firmware_update *update);

/* True while the radio must be left alone entirely: it is on its way into a loader, already in
   one, or coming back out. Auto-connect bringing a link up here would bind a bootloader's CDC
   or take the loader's connection. */
bool mesh_firmware_update_holds_the_radio(const struct mesh_firmware_update *update);

/*
 * True when this job has left the radio in the ESP32 OTA loader: off the mesh, advertising the
 * loader's service, waiting for an image with the hash it was given. There is no way back out
 * of it except finishing, which is what the banner says and why the banner exists.
 *
 * Always false on the USB path, and that absence is the feature: an interrupted write leaves a
 * bootloader any computer can talk to.
 */
bool mesh_firmware_update_radio_in_loader(const struct mesh_firmware_update *update);

/* 0-100 over whichever step has a fraction, and 0 for the ones that do not. */
unsigned mesh_firmware_update_progress(const struct mesh_firmware_update *update);

const char *mesh_firmware_update_state_name(enum mesh_firmware_update_state state);
const char *mesh_firmware_update_error_name(enum mesh_firmware_update_error error);

#ifdef __cplusplus
}
#endif
