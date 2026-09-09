#pragma once

/*
 * What firmware exists for the radio we are talking to, read out of the two documents upstream
 * publishes. Pure functions over bytes somebody else fetched: nothing here opens a socket,
 * keeps state or knows what a Brick is, which is what makes it the half of
 * docs/radio-firmware-roadmap.md that is testable without a radio.
 *
 * Two documents, because the radio does not tell us enough on its own. `DeviceMetadata` carries
 * a `hw_model` and a `firmware_version` and no build target - and the build target is what
 * names the file:
 *
 *   api.meshtastic.org/resource/deviceHardware   one entry per board: the model number, the
 *                                                platformio target, the architecture, whether
 *                                                it is still supported. The list the web
 *                                                flasher uses.
 *   api.meshtastic.org/github/firmware/list      the release index: `stable` and `alpha`,
 *                                                newest first.
 *
 * **`hw_model` is not unique**, and that is the one thing this must get right. Nine models map
 * to more than one board: model 48 is four of them, and `DIY_V1` and `HYDRA` share 39. Flashing
 * the wrong variant of the right board is the failure this feature must not have, so the lookup
 * returns *every* candidate and says how many there were rather than picking one. Choosing is a
 * question for the user, once, and remembered - never a guess made here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "heltec-mesh-node-t114" is 21; the longest upstream ships is under 32. */
#define MESH_FIRMWARE_TARGET_MAX 40U
/* "LILYGO T-LoRa V2.1-1.6". Bounded by what a settings row can show, not by the document. */
#define MESH_FIRMWARE_BOARD_NAME_MAX 48U
/* "esp32-s3", "nrf52840". */
#define MESH_FIRMWARE_ARCH_MAX 16U
/* "2.7.26.54e0d8d" - three numbers and the build's short hash. */
#define MESH_FIRMWARE_VERSION_MAX 24U
#define MESH_FIRMWARE_URL_MAX 256U
/*
 * How many boards one `hw_model` may resolve to before we stop collecting them. Four is what
 * the widest model (HELTEC_WIRELESS_TRACKER, 48) actually has; `found` says whether a document
 * held more, so the number growing upstream is visible rather than silent.
 */
#define MESH_FIRMWARE_BOARDS_MAX 4U

/*
 * Which bus, if any, this client could put firmware onto a board over.
 *
 * A property of the architecture and nothing else, which is why it is derived here rather than
 * stored: nRF52840 and RP2040 come up as a UF2 bootloader on USB, ESP32 and ESP32-S3 reboot
 * into the unified OTA loader and take an image over two GATT characteristics, and the rest -
 * ESP32-C3 and C6, whose loader partition holds a build current firmware will not boot into,
 * and portduino, which is a Linux process - have no path from here at all.
 */
enum mesh_firmware_path {
    MESH_FIRMWARE_PATH_NONE = 0,
    MESH_FIRMWARE_PATH_USB, /* a UF2 written to the bootloader's mass storage */
    MESH_FIRMWARE_PATH_BLE, /* the ESP32 unified OTA loader */
    MESH_FIRMWARE_PATH_COUNT,
};

struct mesh_firmware_board {
    uint32_t hw_model;
    char target[MESH_FIRMWARE_TARGET_MAX];
    char name[MESH_FIRMWARE_BOARD_NAME_MAX];
    char architecture[MESH_FIRMWARE_ARCH_MAX];
    /* Upstream still builds for it. A board that is not is still identified, and still says
       what it is running - it just has no newer firmware coming. */
    bool actively_supported;
    /* The document's own `requiresDfu`: this board is flashed through a bootloader rather than
       over its serial port. Every nRF52840 sets it. */
    bool requires_dfu;
    enum mesh_firmware_path path;
};

struct mesh_firmware_boards {
    struct mesh_firmware_board entries[MESH_FIRMWARE_BOARDS_MAX];
    /* How many are in `entries`. */
    uint8_t count;
    /* How many the document held, which may be more than we kept. Anything above 1 is the
       ambiguous case and must be resolved by asking, never by taking entries[0]. */
    uint8_t found;
};

/* Which list of the release index to read. There is no "any": a client following stable must
   never be handed an alpha because it happened to be newer. */
enum mesh_firmware_channel {
    MESH_FIRMWARE_CHANNEL_STABLE = 0,
    MESH_FIRMWARE_CHANNEL_ALPHA,
    MESH_FIRMWARE_CHANNEL_COUNT,
};

struct mesh_firmware_release {
    /* The tag without its leading 'v': "2.7.26.54e0d8d". */
    char version[MESH_FIRMWARE_VERSION_MAX];
    /*
     * That release's manifest - `firmware-<version>.json`, which lists every target it built.
     * Empty when the release has not published one, which happens: a release can appear in the
     * index before its assets do, and one whose newest asset is still a per-platform zip has no
     * manifest at all. Phase 1 only reports the version, so an empty URL is not a failure here.
     */
    char manifest_url[MESH_FIRMWARE_URL_MAX];
};

/*
 * Collects every board in `deviceHardware` claiming `hw_model`.
 *
 * True when the document parsed, whatever it held - `out->found == 0` is "this radio's model
 * is not in the list", which is a different answer from "the list was not readable" and the two
 * are different rows. `len` may be 0 for a NUL-terminated string.
 */
bool mesh_firmware_boards_parse(const char *json, size_t len, uint32_t hw_model,
                                struct mesh_firmware_boards *out);

/*
 * Reads the newest release on `channel` out of the release index. False when the document had
 * no such list or no entry with a version in it.
 *
 * The index is newest first and this takes the first entry, which is upstream's own ordering
 * rather than a comparison of ours: these version strings end in a build hash, and sorting a
 * list on a field that is not ordered is how a client offers a release that is not the newest.
 */
bool mesh_firmware_release_parse(const char *json, size_t len, enum mesh_firmware_channel channel,
                                 struct mesh_firmware_release *out);

/* The verdict for an `architecture` string as the hardware document spells it. Unknown
   architectures - including ones upstream adds after this ships - are NONE, which is the answer
   that refuses rather than the one that guesses. */
enum mesh_firmware_path mesh_firmware_path_for_architecture(const char *architecture);

/*
 * Orders two Meshtastic firmware versions: <0, 0 or >0, as strcmp does.
 *
 * Meshtastic versions are `major.minor.patch.hash`, and only the first three of those are
 * ordered - the fourth is the build's git hash, which sorts like a hash, which is to say not at
 * all. So two versions whose numbers match compare *equal* even when the hashes differ. That is
 * deliberate: the alternative is inventing an order for the pair, and the direction that would
 * go wrong is offering an update to something that is not newer.
 *
 * A missing or unparseable version sorts below everything, so a radio that has not said what it
 * is running is never told it is up to date.
 */
int mesh_firmware_version_compare(const char *left, const char *right);

#ifdef __cplusplus
}
#endif
