#pragma once

/*
 * The Nordic DFU package an nRF52 release publishes beside its UF2:
 * `firmware-<target>-<ver>-ota.zip`.
 *
 * It is a zip inside the release zip, and what is in it is three members:
 *
 *   manifest.json   names the two files below under `manifest.application`
 *   <name>.dat      the legacy init packet - device type, revision, app version, the SoftDevices
 *                   the image will run on, and the image's CRC16 - fourteen bytes for a release
 *                   built against one SoftDevice
 *   <name>.bin      the application, as the bootloader writes it to flash
 *
 * Read whole and in memory, because the image is under a megabyte and the bootloader is sent the
 * init packet before the first byte of it. The check this makes is the one the bootloader makes
 * at the very end - the image's CRC16 against the one in the init packet - and it is made here
 * first so a package that would fail it never takes a radio off the mesh.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An nRF52840's application region is under 1 MB with a SoftDevice and a bootloader beside it. */
#define MESH_DFU_IMAGE_MAX (1024U * 1024U)
/* A legacy init packet is 14 bytes for one SoftDevice and grows two bytes a SoftDevice. A Secure
   DFU init packet is a signed protobuf of a few hundred, and a legacy bootloader refuses it. */
#define MESH_DFU_INIT_MAX 64U
/* The manifest is a few hundred bytes of JSON. */
#define MESH_DFU_MANIFEST_MAX 4096U

enum mesh_dfu_package_verdict {
    MESH_DFU_PACKAGE_OK = 0,
    /* No end-of-central-directory record, or a directory that is not one. */
    MESH_DFU_PACKAGE_NOT_A_ZIP,
    /* No manifest.json, or one that does not parse. */
    MESH_DFU_PACKAGE_NO_MANIFEST,
    /* The manifest names no `application`: a bootloader or SoftDevice package, which must never
       be sent as an application. */
    MESH_DFU_PACKAGE_NO_APPLICATION,
    /* A file the manifest names is not in the zip, or will not inflate to what the directory
       says, or fails its CRC32. */
    MESH_DFU_PACKAGE_CORRUPT,
    /* An init packet too long to be the legacy one, or an image too big for the part. */
    MESH_DFU_PACKAGE_NOT_LEGACY,
    /* The image's CRC16 is not the one its init packet names. */
    MESH_DFU_PACKAGE_CRC_MISMATCH,
    MESH_DFU_PACKAGE_NO_MEMORY,
    MESH_DFU_PACKAGE_VERDICT_COUNT,
};

struct mesh_dfu_package {
    uint8_t init[MESH_DFU_INIT_MAX];
    size_t init_len;
    uint8_t *image; /* owned; mesh_dfu_package_free() */
    size_t image_len;
    /* Out of the init packet, for the log. */
    uint16_t device_type;
    uint16_t softdevice;
    uint16_t crc16;
    /* The init packet had the shape that carries a CRC16, and the image matched it. False for a
       shape this does not know, which is passed through for the bootloader to judge. */
    bool crc_checked;
};

/* Reads the package in `zip`. On anything but OK nothing is owned and `out` is zeroed. */
enum mesh_dfu_package_verdict mesh_dfu_package_read(const uint8_t *zip, size_t len,
                                                    struct mesh_dfu_package *out);

void mesh_dfu_package_free(struct mesh_dfu_package *package);

const char *mesh_dfu_package_verdict_name(enum mesh_dfu_package_verdict verdict);

/* Nordic's crc16_compute(): CRC-16/CCITT-FALSE, which is what an init packet carries. */
uint16_t mesh_dfu_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
