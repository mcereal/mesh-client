#include "mesh/core/dfu_package.h"

#include "inkwell/base/log.h"

#include "inkwell/codec/inflate.h"
#include "inkwell/codec/json.h"
#include "inkwell/codec/zip.h"

#include <stdlib.h>
#include <string.h>

static const char *const k_verdict_names[MESH_DFU_PACKAGE_VERDICT_COUNT] = {
    "ok",
    "not a zip",
    "no manifest",
    "no application",
    "corrupt",
    "not a legacy package",
    "crc16 mismatch",
    "no memory",
};

const char *mesh_dfu_package_verdict_name(enum mesh_dfu_package_verdict verdict) {
    return verdict < MESH_DFU_PACKAGE_VERDICT_COUNT ? k_verdict_names[verdict] : "?";
}

uint16_t mesh_dfu_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; data != NULL && i < len; ++i) {
        crc = (uint16_t)((uint8_t)(crc >> 8) | (uint16_t)(crc << 8));
        crc ^= data[i];
        crc ^= (uint8_t)(crc & 0xFFU) >> 4;
        crc ^= (uint16_t)((uint16_t)(crc << 8) << 4);
        crc ^= (uint16_t)((uint16_t)((crc & 0xFFU) << 4) << 1);
    }
    return crc;
}

/* The directory of a zip held whole: where it is, and how many entries it claims. */
struct package_zip {
    const uint8_t *bytes;
    size_t len;
    const uint8_t *central;
    size_t central_len;
    uint32_t entries;
};

static bool package_open(struct package_zip *zip, const uint8_t *bytes, size_t len) {
    const size_t window_len = len < INKWELL_ZIP_TAIL_WINDOW ? len : INKWELL_ZIP_TAIL_WINDOW;
    const uint64_t window_offset = (uint64_t)(len - window_len);
    struct inkwell_zip_end end;
    if (!inkwell_zip_find_end(bytes + window_offset, window_len, window_offset, &end)) {
        return false;
    }
    /* The whole file is in hand, so the directory is wherever the record says - no second read
       for a directory that did not fit the tail window. */
    if (end.central_offset > len || end.central_size > len - end.central_offset) {
        return false;
    }
    zip->bytes = bytes;
    zip->len = len;
    zip->central = bytes + end.central_offset;
    zip->central_len = end.central_size;
    zip->entries = end.entries;
    return true;
}

/*
 * One member, inflated into a buffer of its own and checked against the directory's size and
 * CRC32. NULL with `*verdict` set when it is absent, too big or damaged.
 */
static uint8_t *package_member(const struct package_zip *zip, const char *name, size_t max,
                               size_t *out_len, enum mesh_dfu_package_verdict *verdict) {
    *verdict = MESH_DFU_PACKAGE_CORRUPT;
    struct inkwell_zip_entry entry;
    if (inkwell_zip_find_member(zip->central, zip->central_len, zip->entries, name, &entry) !=
        INKWELL_ZIP_FOUND) {
        return NULL;
    }
    if (entry.uncompressed_size > max) {
        *verdict = MESH_DFU_PACKAGE_NOT_LEGACY;
        return NULL;
    }
    uint64_t data = 0U;
    if (entry.local_header_offset > zip->len ||
        !inkwell_zip_local_data_start(zip->bytes + entry.local_header_offset,
                                      zip->len - (size_t)entry.local_header_offset, &entry,
                                      &data) ||
        data > zip->len || entry.compressed_size > zip->len - data) {
        return NULL;
    }
    /* One byte more than the member, so an empty one is still an allocation to hand back. */
    uint8_t *const out = malloc((size_t)entry.uncompressed_size + 1U);
    if (out == NULL) {
        *verdict = MESH_DFU_PACKAGE_NO_MEMORY;
        return NULL;
    }
    size_t got = 0U;
    if (entry.method == INKWELL_ZIP_METHOD_STORE) {
        if (entry.compressed_size != entry.uncompressed_size) {
            free(out);
            return NULL;
        }
        memcpy(out, zip->bytes + data, entry.compressed_size);
        got = entry.compressed_size;
    } else if (entry.method == INKWELL_ZIP_METHOD_DEFLATE) {
        if (inkwell_inflate(zip->bytes + data, entry.compressed_size, out, entry.uncompressed_size,
                            &got) != INKWELL_INFLATE_OK) {
            free(out);
            return NULL;
        }
    } else {
        free(out);
        return NULL;
    }
    uint32_t crc = 0U;
    if (got != entry.uncompressed_size || !inkwell_crc32(out, got, &crc) || crc != entry.crc32) {
        free(out);
        return NULL;
    }
    *out_len = got;
    *verdict = MESH_DFU_PACKAGE_OK;
    return out;
}

/* `manifest.application.bin_file` and `.dat_file`. False when there is no application. */
static bool package_manifest(const char *text, size_t len, char *bin, size_t bin_len, char *dat,
                             size_t dat_len) {
    struct inkwell_json json;
    inkwell_json_init(&json, text, len);
    if (!inkwell_json_object_find(&json, "manifest") ||
        !inkwell_json_object_find(&json, "application")) {
        return false;
    }
    /* A copy per key, because object_find walks forward and the two can come in either order. */
    struct inkwell_json at_bin = json;
    struct inkwell_json at_dat = json;
    return inkwell_json_object_find(&at_bin, "bin_file") &&
           inkwell_json_read_string(&at_bin, bin, bin_len) && bin[0] != '\0' &&
           inkwell_json_object_find(&at_dat, "dat_file") &&
           inkwell_json_read_string(&at_dat, dat, dat_len) && dat[0] != '\0';
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

/*
 * The init packet's own fields, and the CRC16 at its end.
 *
 * The legacy layout is device type (2), revision (2), application version (4), a count of
 * SoftDevices (2), that many SoftDevice ids (2 each), and the image's CRC16 (2) - which is the
 * shape adafruit-nrfutil writes for `dfu_version` 0.5, the one every Meshtastic release uses.
 */
static void package_read_init(struct mesh_dfu_package *package) {
    const uint8_t *const init = package->init;
    if (package->init_len < 12U) {
        return;
    }
    package->device_type = read_le16(init);
    const size_t softdevices = read_le16(init + 8);
    package->softdevice = softdevices > 0U ? read_le16(init + 10) : 0U;
    if (package->init_len != 12U + 2U * softdevices) {
        return;
    }
    package->crc16 = read_le16(init + package->init_len - 2U);
    package->crc_checked = true;
}

enum mesh_dfu_package_verdict mesh_dfu_package_read(const uint8_t *zip_bytes, size_t len,
                                                    struct mesh_dfu_package *out) {
    if (out == NULL) {
        return MESH_DFU_PACKAGE_NOT_A_ZIP;
    }
    memset(out, 0, sizeof *out);
    struct package_zip zip;
    if (zip_bytes == NULL || !package_open(&zip, zip_bytes, len)) {
        return MESH_DFU_PACKAGE_NOT_A_ZIP;
    }

    enum mesh_dfu_package_verdict verdict = MESH_DFU_PACKAGE_OK;
    size_t manifest_len = 0U;
    char *const manifest = (char *)package_member(&zip, "manifest.json", MESH_DFU_MANIFEST_MAX,
                                                  &manifest_len, &verdict);
    if (manifest == NULL) {
        return verdict == MESH_DFU_PACKAGE_NO_MEMORY ? verdict : MESH_DFU_PACKAGE_NO_MANIFEST;
    }
    char bin[INKWELL_ZIP_NAME_MAX];
    char dat[INKWELL_ZIP_NAME_MAX];
    const bool application =
        package_manifest(manifest, manifest_len, bin, sizeof bin, dat, sizeof dat);
    free(manifest);
    if (!application) {
        return MESH_DFU_PACKAGE_NO_APPLICATION;
    }

    size_t init_len = 0U;
    uint8_t *const init = package_member(&zip, dat, MESH_DFU_INIT_MAX, &init_len, &verdict);
    if (init == NULL) {
        return verdict;
    }
    memcpy(out->init, init, init_len);
    out->init_len = init_len;
    free(init);

    out->image = package_member(&zip, bin, MESH_DFU_IMAGE_MAX, &out->image_len, &verdict);
    if (out->image == NULL) {
        memset(out, 0, sizeof *out);
        return verdict;
    }
    /* The bootloader takes whole words and nothing else, so an image that is not a multiple of
       four is not one it could ever finish. */
    if (out->image_len == 0U || out->image_len % 4U != 0U) {
        mesh_dfu_package_free(out);
        return MESH_DFU_PACKAGE_NOT_LEGACY;
    }

    package_read_init(out);
    if (out->crc_checked && mesh_dfu_crc16(out->image, out->image_len) != out->crc16) {
        mesh_dfu_package_free(out);
        return MESH_DFU_PACKAGE_CRC_MISMATCH;
    }
    if (!out->crc_checked) {
        inkwell_log_warn("firmware",
                         "The %zu-byte init packet is not the shape that carries a CRC16; the "
                         "bootloader will be the one to check the image",
                         out->init_len);
    }
    return MESH_DFU_PACKAGE_OK;
}

void mesh_dfu_package_free(struct mesh_dfu_package *package) {
    if (package == NULL) {
        return;
    }
    free(package->image);
    memset(package, 0, sizeof *package);
}
