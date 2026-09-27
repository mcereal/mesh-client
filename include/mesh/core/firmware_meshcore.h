#pragma once

/*
 * What firmware exists for a MeshCore radio, read out of the three documents MeshCore's own
 * web flasher reads. Pure functions over bytes somebody else fetched, as firmware_catalog.h is
 * for Meshtastic's two - and they fill the same `struct mesh_firmware_board` and
 * `struct mesh_firmware_release`, so everything after the check does not care whose they were.
 *
 *   flasher.meshcore.io/config.json          one entry per device: its name, its chip family,
 *                                            and per role a pattern for each file a release
 *                                            publishes for it
 *   .../git/matching-refs/tags/companion-v   every companion release's tag, unordered
 *   .../releases/tags/<tag>                  one release: every asset's name, size and URL
 *
 * **The radio names itself in words, not a number.** DEVICE_INFO carries the board's own
 * manufacturer string ("Heltec V3", "RAK 4631"), and the flasher's list is keyed on a display
 * name that is sometimes the same words in another case ("Heltec v3") and sometimes not
 * ("RAK WisBlock / WisMesh (RAK 4631)"). So a model matches a device by name ignoring case, or
 * through the alias table in the .c for the ones that differ. A model that matches nothing is
 * the unknown-board refusal, never a nearest guess.
 *
 * **A device can be several builds.** The Heltec T114 is one entry with a companion build for
 * the board with a display and another for the one without, and the radio's name does not say
 * which it is. Every matching build is a candidate and more than one is the ambiguous refusal,
 * the same rule firmware_catalog.h holds for a Meshtastic `hw_model` - flashing the wrong
 * variant of the right board is the failure this must not have.
 *
 * **Which build is also the radio's own role and transport.** A companion that answers over
 * USB is running the `_usb` build, one on BLE the `_ble` build, and a write keeps it that way;
 * `usb` picks which role's pattern is read. Switching between them is a different question.
 *
 * A pattern is a regular expression and this has no regex engine. What it has is the fact
 * every companion pattern is `<literal prefix>.*?<suffix>`: the prefix is the build's own
 * name, which is what `board.target` carries, and an asset is that name followed by '-'.
 */

#include "mesh/core/firmware_catalog.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "companion-v1.17.1". */
#define MESH_FIRMWARE_MESHCORE_TAG_MAX 40U

/*
 * The devices `model` names, and for each the companion build its role reads: `usb` for the
 * USB companion, otherwise the BLE one. Each is a board whose `target` is the build's asset
 * prefix ("Heltec_v3_companion_radio_usb"), `name` the flasher's device name and
 * `architecture` "esp32" for the ESP32 family - which chip is read off the image itself - or
 * "nrf52840", and `meshcore` set: both install over USB, the nRF52 by the 1200-baud touch.
 *
 * `found` counts every match, as mesh_firmware_boards_parse() does. False on a document that
 * is not the flasher's shape.
 */
bool mesh_firmware_meshcore_boards_parse(const char *json, size_t len, const char *model, bool usb,
                                         struct mesh_firmware_boards *out);

/* Whether a radio calling itself `model` is the flasher's device `device`: the same words
   ignoring case, or a row of the alias table. */
bool mesh_firmware_meshcore_names(const char *model, const char *device);

/*
 * The same board under the other firmware, from a table of pairs checked by hand: the
 * flasher's device name for Meshtastic's `platformioTarget` ("heltec-v3" is "Heltec v3"), and
 * back. NULL for a board with no twin, which is not offered a switch - a board is never
 * matched across firmwares by its name looking alike.
 */
const char *mesh_firmware_meshcore_device_for_target(const char *target);
const char *mesh_firmware_meshcore_target_for_device(const char *device);

/*
 * The newest companion release in a list of tag refs, as "companion-v1.17.1" into `tag` and
 * "1.17.1" into `version`. Newest by version, since the list is ordered by name and
 * "companion-v1.9.0" sorts after "companion-v1.17.1". False when there is none.
 */
bool mesh_firmware_meshcore_latest_tag(const char *json, size_t len, char *tag, size_t tag_len,
                                       char *version, size_t version_len);

/*
 * The asset of one release that `board` installs from: its build's app image - the `.bin`
 * that is not `-merged` - for the ESP32 family, its `.uf2` for an nRF52. With
 * `release->wipe` set, which the caller does for a board coming from Meshtastic, an ESP32's is
 * the `-merged.bin` instead: the whole flash. Fills `release->image_name`, `image_url` and
 * `image_bytes`. False when the release publishes none, or more than one.
 */
bool mesh_firmware_meshcore_asset_parse(const char *json, size_t len,
                                        const struct mesh_firmware_board *board,
                                        struct mesh_firmware_release *release);

#ifdef __cplusplus
}
#endif
