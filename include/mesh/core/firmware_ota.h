#pragma once

/*
 * The BLE handover: from a staged ESP32 `.bin` to a radio running it. Phase 4 of
 * docs/radio-firmware-roadmap.md, and the USB install's counterpart in firmware_install.h.
 *
 * It is the half with the hazard. Once the radio has rebooted into its OTA loader, its boot
 * partition points at the loader and stays there - no timer, no fallback, no way back but a
 * finished transfer - so everything here is shaped by having to finish, or to come back and
 * finish:
 *
 *   arming      `ota_request` with the image's SHA-256 has gone down the link, and the radio
 *               has either said "Rebooting to BLE OTA", said why not, or said nothing. A refusal
 *               leaves it untouched and running; silence is not taken as either, because the
 *               loader appearing is the evidence that counts.
 *   waiting     scanning for the loader's service. It is a different peripheral from the radio
 *               - its address is conventionally the radio's plus one - so it is looked for by
 *               what it offers, and the address is corroboration.
 *   connecting  connect, ask for a 7.5 ms connection interval (mesh/transport/ble_hci.h: the
 *               Brick would hold the loader at 30 ms, which is a seventeen-minute transfer), wait
 *               for services, find the two characteristics.
 *   sending     the loader conversation (mesh/transport/ble_ota.h), which ends with the loader
 *               saying the hash matched and the boot partition is switched.
 *   restarting  the radio advertising again where it was: the loader restarts itself two
 *               seconds after its last answer.
 *
 * A transfer that breaks - the link drops, an answer does not come - is not a failure yet: the
 * loader forgets a half-written image when the link goes and starts over on the next `OTA`, so
 * the recovery is to find it again and resend, and this does that itself up to
 * MESH_FIRMWARE_OTA_ATTEMPTS times. What it cannot do on its own is survive the client being
 * quit or the Brick going to sleep; that is `mesh_firmware_ota_radio_in_loader()`, and the way
 * back is the same install started again with no arm callback, which begins at `waiting`.
 *
 * **This does not go through mesh_session**, exactly as the USB install does not: the only thing
 * that needs the session is the admin verb, and it arrives as a callback. A board already in
 * its loader is installed to by passing none.
 */

#include "mesh/core/esp_image.h"
#include "mesh/transport/ble_ota.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_bluez_client;

/* Well clear of the largest app partition an ESP32 ships with (3 MB on an 8 MB part). */
#define MESH_FIRMWARE_OTA_IMAGE_MAX (8U * 1024U * 1024U)
/* Tries at the loader before giving up on it. Each one re-erases and resends the whole image,
   because that is what the loader does with a link that came back. */
#define MESH_FIRMWARE_OTA_ATTEMPTS 3U
#define MESH_FIRMWARE_OTA_ADDRESS_MAX 32U
#define MESH_FIRMWARE_OTA_PATH_MAX 128U
#define MESH_FIRMWARE_OTA_REASON_MAX 128U

enum mesh_firmware_ota_state {
    MESH_FIRMWARE_OTA_IDLE = 0,
    MESH_FIRMWARE_OTA_ARMING,
    MESH_FIRMWARE_OTA_WAITING,
    MESH_FIRMWARE_OTA_CONNECTING,
    MESH_FIRMWARE_OTA_SENDING,
    MESH_FIRMWARE_OTA_RESTARTING,
    MESH_FIRMWARE_OTA_DONE,
    MESH_FIRMWARE_OTA_FAILED,
    MESH_FIRMWARE_OTA_STATE_COUNT,
};

enum mesh_firmware_ota_error {
    MESH_FIRMWARE_OTA_ERROR_NONE = 0,
    /* A bad argument, an architecture with no ESP32 image, or an image that could not be read.
       Nothing was started. */
    MESH_FIRMWARE_OTA_ERROR_UNAVAILABLE,
    /* Not an ESP32 application, or one for another chip. Refused before the radio is asked
       anything, because the loader only finds out after the whole transfer. */
    MESH_FIRMWARE_OTA_ERROR_WRONG_IMAGE,
    /* The ota_request could not be queued. The radio is untouched. */
    MESH_FIRMWARE_OTA_ERROR_ARM,
    /* The radio said no, in its own words (`reason`): no loader partition, an old loader, an
       S3 built without Wi-Fi. It is untouched and still running. */
    MESH_FIRMWARE_OTA_ERROR_REFUSED,
    /* No loader appeared. After a "Rebooting to BLE OTA" that means one is out there that we
       did not hear; without one, most likely the request never landed. */
    MESH_FIRMWARE_OTA_ERROR_NO_LOADER,
    /* The loader was found and would not take a connection, every attempt. */
    MESH_FIRMWARE_OTA_ERROR_CONNECT,
    /* The transfer broke every attempt: the link, a timeout, an answer out of turn. */
    MESH_FIRMWARE_OTA_ERROR_TRANSFER,
    /* The loader refused in its own words (`reason`) - the hash it holds is not this image's,
       or it could not begin. Resending the same image changes neither. */
    MESH_FIRMWARE_OTA_ERROR_LOADER_REFUSED,
    /* Every byte arrived and hashed to something else, every attempt. */
    MESH_FIRMWARE_OTA_ERROR_HASH_MISMATCH,
    /* The loader said the image was flashed and the radio never came back where it was. */
    MESH_FIRMWARE_OTA_ERROR_NO_RADIO,
    MESH_FIRMWARE_OTA_ERROR_COUNT,
};

/* How the radio answered an ota_request, read off its ClientNotification text. */
enum mesh_firmware_ota_answer {
    MESH_FIRMWARE_OTA_ANSWER_UNRELATED = 0,
    MESH_FIRMWARE_OTA_ANSWER_GO_AHEAD,
    MESH_FIRMWARE_OTA_ANSWER_REFUSED,
};

struct mesh_firmware_ota;

/* Queues the ota_request for `sha256`. Returns 0, or -errno. */
typedef int (*mesh_firmware_ota_arm_fn)(void *userdata, const uint8_t sha256[32]);
/* Asks for a fast connection interval on the open link to `address`. The result is logged and
   nothing waits on it: a link that stays slow is a slow transfer, not a broken one. */
typedef int (*mesh_firmware_ota_interval_fn)(int hci_dev, const char *address);
typedef void (*mesh_firmware_ota_done_fn)(void *userdata, const struct mesh_firmware_ota *ota);

struct mesh_firmware_ota_params {
    struct mesh_bluez_client *client; /* borrowed; the install's own, not the transport's */
    const char *adapter_path;         /* "/org/bluez/hci0" */
    const char *image_path;
    /* The connected board's architecture, in either spelling. The image is checked against
       the chip it names before the radio is asked anything. */
    const char *architecture;
    /* The radio's BLE address, which the loader's is derived from and the restart is watched
       for. Empty means "a board already in its loader", whose address is not known. */
    const char *radio_address;
    mesh_firmware_ota_arm_fn arm; /* NULL: the radio is already in its loader */
    void *arm_userdata;
    mesh_firmware_ota_interval_fn request_interval; /* NULL: leave the interval alone */
    mesh_firmware_ota_done_fn on_done;
    void *userdata;
};

struct mesh_firmware_ota {
    struct mesh_bluez_client *client;
    char adapter_path[MESH_FIRMWARE_OTA_PATH_MAX];
    int hci_dev;

    enum mesh_firmware_ota_state state;
    enum mesh_firmware_ota_error error;
    /* The radio's words or the loader's, whichever refused - untranslated, like a log line. */
    char reason[MESH_FIRMWARE_OTA_REASON_MAX];
    /* The radio said "Rebooting to BLE OTA". From here it is in the loader. */
    bool go_ahead;
    /* The loader was found at least once, which is proof of the same thing. */
    bool loader_seen;
    /* The radio was heard again after the loader finished. False on a DONE only when there was
       no radio address to watch for. */
    bool radio_seen;

    /* Read whole before the radio is asked anything, and hashed - so the hash the radio is held
       to is the hash of the bytes that will actually be sent. */
    uint8_t *image;
    size_t image_len;
    uint8_t sha256[32];
    struct mesh_esp_image_info esp;

    char radio_address[MESH_FIRMWARE_OTA_ADDRESS_MAX];
    char loader_address[MESH_FIRMWARE_OTA_ADDRESS_MAX];
    char loader_path[MESH_FIRMWARE_OTA_PATH_MAX];

    struct mesh_ble_ota conversation;
    unsigned attempts;
    bool started;
    bool discovering;
    bool connected;
    bool ambiguity_logged;

    uint64_t deadline_ms;
    uint64_t next_poll_ms;

    mesh_firmware_ota_arm_fn arm;
    void *arm_userdata;
    mesh_firmware_ota_interval_fn request_interval;
    mesh_firmware_ota_done_fn on_done;
    void *userdata;
};

/*
 * Reads and checks the image, hashes it, and - with an arm callback - asks the radio into its
 * loader. Returns 0, or -errno; on 0 `on_done` is called exactly once, later, from a tick. On a
 * negative return nothing was started, no callback will arrive, and `state` and `error` still
 * say why, because a refusal is a row.
 */
int mesh_firmware_ota_start(struct mesh_firmware_ota *ota,
                            const struct mesh_firmware_ota_params *params);

/*
 * What the radio said, as its ClientNotification text. A go-ahead moves arming along without
 * waiting out its clock; a refusal ends the install with the radio untouched. Anything else,
 * and anything arriving once the loader has been found, is ignored.
 */
void mesh_firmware_ota_radio_said(struct mesh_firmware_ota *ota, const char *text);

enum mesh_firmware_ota_answer mesh_firmware_ota_classify(const char *text);

void mesh_firmware_ota_tick(struct mesh_firmware_ota *ota, uint64_t now_ms);

/* Stops, disconnects from the loader and frees the image, reporting nothing. Safe on a zeroed
   struct. A radio already in its loader stays there - which is what radio_in_loader() says. */
void mesh_firmware_ota_cancel(struct mesh_firmware_ota *ota);

bool mesh_firmware_ota_busy(const struct mesh_firmware_ota *ota);

/* True while the radio must be left alone - every busy state, as on the USB path: from the
   moment the request goes out the radio is on its way down or already a loader. */
bool mesh_firmware_ota_holds_the_radio(const struct mesh_firmware_ota *ota);

/*
 * True when this install has ended and left the radio in its loader: off the mesh, advertising
 * the loader's service, waiting for an image with the hash it was given. It is what the banner
 * the roadmap asks for will read, and what tells a user the same command started again is the
 * way out - and it is false for every failure that left the radio running.
 */
bool mesh_firmware_ota_radio_in_loader(const struct mesh_firmware_ota *ota);

/* 0-100 over the stream, which is the only step long enough to watch. */
unsigned mesh_firmware_ota_progress(const struct mesh_firmware_ota *ota);

/* `address` plus `delta` in its last byte, carrying up: the loader's address is the radio's
   plus one, the convention the phone app also relies on. False for text that is not one. */
bool mesh_firmware_ota_offset_address(const char *address, int delta, char *out, size_t out_len);

const char *mesh_firmware_ota_state_name(enum mesh_firmware_ota_state state);
const char *mesh_firmware_ota_error_name(enum mesh_firmware_ota_error error);

#ifdef __cplusplus
}
#endif
