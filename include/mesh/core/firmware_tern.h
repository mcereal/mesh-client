#pragma once

/*
 * What firmware exists for a Tern node, read out of the one document the project publishes for
 * it: `ternmesh.org/firmware/latest.json`, the feed the phone apps and the site's own updater
 * read. Pure functions over bytes somebody else fetched, as firmware_catalog.h and
 * firmware_meshcore.h are, filling the same `struct mesh_firmware_board` and
 * `struct mesh_firmware_release`.
 *
 *   { "release": "0.5.0-alpha.1",
 *     "images": [ { "board": "heltec-v3", "region": "EU868",
 *                   "file": "tern-heltec-v3-eu868-0.5.0-alpha.1-app.bin",
 *                   "size": 773264, "sha256": "<64 lowercase hex>" } ] }
 *
 * One release, and one application image per board and region, `file` relative to the feed's
 * own directory. Fields may be added and none removed, so an unknown key is skipped.
 *
 * **The node names both halves itself**, which is what makes this the one firmware this client
 * does not have to guess at: INFO's `board` is the name the feed is keyed on ("heltec-v3"), empty
 * for a build that cannot be updated over its link, and SELF's `region` is the profile the image
 * was built for ("EU868"). An image for another region would put the node on frequencies its
 * owner did not choose, so the region is matched as strictly as the board - and a node with
 * either empty matches nothing, which is the unknown-board refusal rather than a nearest guess.
 *
 * The image is the application only, written over the companion link: the node keeps its
 * address, keys, contacts and settings, and refuses an image that is not one (ERROR 11) before
 * it restarts. So the board this answers with has path MESH_FIRMWARE_PATH_LINK and no
 * architecture - nothing here reads the chip, because the node does.
 */

#include "mesh/core/firmware_catalog.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "EU868", "TH920": a profile name, as SELF carries it. */
#define MESH_FIRMWARE_TERN_REGION_MAX 16U
/* The largest image the feed is believed about. A Heltec V3's is 0.75 MB; the apps refuse past
   16 MB, and so does this. */
#define MESH_FIRMWARE_TERN_IMAGE_MAX (16U * 1024U * 1024U)

/*
 * Reads the feed for the node that calls itself `board` in `region`.
 *
 * True when the document is the feed's shape and names a release, whatever it holds for this
 * node: `release->version` is then the release, and `boards` the images that match - `found`
 * 0 for none, 1 for the one this node takes, more for a feed that lists the same board and
 * region twice, which is the ambiguous refusal. With exactly one, `release` names its file:
 * `image_url` is `base_url` (the feed's directory, ending in '/') followed by `file`,
 * `image_bytes` its size and `image_sha256` its digest. An entry that is not the shape the
 * apps accept - a file name with a path in it, no size, a digest that is not 64 hex digits - is
 * skipped rather than failing the document, as the Android app does.
 */
bool mesh_firmware_tern_parse(const char *json, size_t len, const char *board, const char *region,
                              const char *base_url, struct mesh_firmware_boards *boards,
                              struct mesh_firmware_release *release);

/*
 * Orders two Tern releases: <0, 0 or >0, as strcmp does. They are SemVer without the 'v' -
 * "0.5.0-alpha.1" - and a prerelease is below the release it leads to, which is the ordering
 * mesh_firmware_version_compare() cannot give: it reads three numbers and stops. Anything that
 * is not SemVer sorts below everything, so a node with no release is never up to date.
 */
int mesh_firmware_tern_version_compare(const char *left, const char *right);

#ifdef __cplusplus
}
#endif
