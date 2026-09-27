#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_serial.h"

#include "mesh/core/esp_image.h"

#include "mesh/transport/serial_usb.h"

#include "inkwell/base/file.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/codec/esp_image.h"
#include "inkwell/io/serial.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SERIAL_PORTS_MAX 16U

const char *mesh_firmware_serial_error_name(enum mesh_firmware_serial_error error) {
    switch (error) {
    case MESH_FIRMWARE_SERIAL_ERROR_NONE:
        return "none";
    case MESH_FIRMWARE_SERIAL_ERROR_IMAGE:
        return "not an image for this chip";
    case MESH_FIRMWARE_SERIAL_ERROR_NO_PORT:
        return "no serial port";
    case MESH_FIRMWARE_SERIAL_ERROR_NATIVE_USB:
        return "the chip's own USB, which this cannot reset";
    case MESH_FIRMWARE_SERIAL_ERROR_LOADER:
        return "the ROM loader failed";
    case MESH_FIRMWARE_SERIAL_ERROR_COUNT:
    default:
        return "?";
    }
}

bool mesh_firmware_serial_busy(const struct mesh_firmware_serial *serial) {
    return serial != NULL && serial->state != MESH_FIRMWARE_SERIAL_IDLE &&
           serial->state != MESH_FIRMWARE_SERIAL_DONE &&
           serial->state != MESH_FIRMWARE_SERIAL_FAILED;
}

bool mesh_firmware_serial_holds_the_radio(const struct mesh_firmware_serial *serial) {
    return mesh_firmware_serial_busy(serial);
}

bool mesh_firmware_serial_radio_in_loader(const struct mesh_firmware_serial *serial) {
    return serial != NULL && serial->state == MESH_FIRMWARE_SERIAL_FAILED &&
           (serial->damaged || serial->flash_touched || serial->loader.erase_sent);
}

unsigned mesh_firmware_serial_progress(const struct mesh_firmware_serial *serial) {
    return serial != NULL ? mesh_esp_loader_progress(&serial->loader) : 0U;
}

static void serial_release_image(struct mesh_firmware_serial *serial) {
    free(serial->image);
    serial->image = NULL;
    serial->image_len = 0U;
}

static int serial_refuse(struct mesh_firmware_serial *serial, enum mesh_firmware_serial_error error,
                         const char *reason, int result) {
    serial_release_image(serial);
    serial->state = MESH_FIRMWARE_SERIAL_FAILED;
    serial->error = error;
    inkwell_str_copy(serial->reason, sizeof serial->reason, reason);
    inkwell_log_error("firmware-serial", "%s: %s", mesh_firmware_serial_error_name(error), reason);
    return result;
}

/*
 * The port to write through, as a tty path. By the transport's id when there is one - the id
 * is what survives the radio resetting, where a label would not - and otherwise the one bridge
 * on the bus. A named id that is gone falls back to that bridge only when finishing a write an
 * earlier attempt left undone: the cable back in another socket is another id, and the only
 * bridge there is is the radio. A fresh install's id is the user's, and a stale one is refused
 * rather than taken to mean some other ESP32.
 */
static int serial_find_port(struct mesh_firmware_serial *serial, const char *where) {
    struct inkwell_serial_port_info ports[SERIAL_PORTS_MAX];
    const size_t count = inkwell_serial_scan(ports, SERIAL_PORTS_MAX);
    const struct inkwell_serial_port_info *found = NULL;
    if (where != NULL && where[0] != '\0') {
        for (size_t i = 0; i < count && found == NULL; ++i) {
            if (strcasecmp(ports[i].id, where) == 0 || strcmp(ports[i].path, where) == 0) {
                found = &ports[i];
            }
        }
    }
    size_t bridges = 0U;
    const bool named = where != NULL && where[0] != '\0';
    if (found == NULL && (!named || serial->damaged)) {
        for (size_t i = 0; i < count; ++i) {
            if (ports[i].kind == INKWELL_SERIAL_BRIDGE) {
                found = bridges == 0U ? &ports[i] : found;
                bridges += 1U;
            }
        }
        if (bridges != 1U) {
            found = NULL;
        }
    }
    if (found == NULL || found->path[0] == '\0') {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_NO_PORT,
                             bridges > 1U ? "more than one serial bridge; connect the radio first"
                                          : "the radio's serial port is not there",
                             -ENODEV);
    }
    if (!mesh_serial_device_reaches_esp_rom(found)) {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_NATIVE_USB, found->name, -ENOTSUP);
    }
    inkwell_str_copy(serial->path, sizeof serial->path, found->path);
    return 0;
}

/*
 * The regions a whole-flash image goes down as: itself at 0x0, then each data partition its
 * table declares past its end, erased. Those hold whatever the last firmware kept there - its
 * settings, its keys, its filesystem, possibly at another firmware's offsets - so the new one
 * starts blank rather than reading the old one's bytes as its own.
 */
static bool serial_whole_regions(struct mesh_firmware_serial *serial, uint16_t chip,
                                 struct mesh_esp_loader_region *regions, size_t *count,
                                 char *reason, size_t reason_len) {
    struct mesh_esp_whole_image whole;
    const char *why = NULL;
    if (!mesh_esp_whole_image_read(serial->image, serial->image_len, chip, &whole, &why)) {
        snprintf(reason, reason_len, "not a whole-flash image for this chip: %s", why);
        return false;
    }
    regions[0] = (struct mesh_esp_loader_region){0U, serial->image, serial->image_len};
    *count = 1U;
    for (size_t i = 0; i < whole.count; ++i) {
        const struct mesh_esp_partition *const partition = &whole.partitions[i];
        if (partition->type != MESH_ESP_PARTITION_DATA ||
            (uint64_t)partition->offset + partition->size <= serial->image_len) {
            continue;
        }
        if (*count >= MESH_ESP_LOADER_REGIONS) {
            snprintf(reason, reason_len, "more data partitions than the loader takes");
            return false;
        }
        regions[(*count)++] =
            (struct mesh_esp_loader_region){partition->offset, NULL, partition->size};
        inkwell_log_info("firmware-serial", "Will erase %s: 0x%06x, %u bytes", partition->label,
                         (unsigned)partition->offset, (unsigned)partition->size);
    }
    return true;
}

static int serial_start(struct mesh_firmware_serial *serial, struct inkwell_loop *loop,
                        const char *image_path, const char *where, uint16_t chip, bool whole,
                        uint64_t now_ms) {
    if (serial == NULL || image_path == NULL) {
        return -EINVAL;
    }
    /* An app an earlier attempt erased stays erased until a write finishes, whatever this
       attempt does - so the banner and the resume outlive a retry that fails before its own
       first erase. */
    const bool damaged = serial->state != MESH_FIRMWARE_SERIAL_DONE &&
                         (serial->damaged || serial->flash_touched || serial->loader.erase_sent);
    mesh_firmware_serial_cancel(serial);
    memset(serial, 0, sizeof *serial);
    serial->damaged = damaged;

    size_t len = 0U;
    serial->image = inkwell_file_read(image_path, MESH_FIRMWARE_SERIAL_IMAGE_MAX, &len);
    if (serial->image == NULL || len == 0U) {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_IMAGE,
                             "the staged image could not be read", -EIO);
    }
    serial->image_len = len;
    /* The headers are the only thing in an image that names a chip, and the last look at the
       file before a flash is erased to take it. */
    struct mesh_esp_loader_region regions[MESH_ESP_LOADER_REGIONS];
    size_t region_count = 0U;
    if (whole) {
        char reason[MESH_FIRMWARE_SERIAL_REASON_MAX];
        if (!serial_whole_regions(serial, chip, regions, &region_count, reason, sizeof reason)) {
            return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_IMAGE, reason, -EINVAL);
        }
    } else {
        struct inkwell_esp_image_info info;
        const enum inkwell_esp_image_verdict verdict =
            inkwell_esp_image_validate(serial->image, serial->image_len, chip, &info);
        if (verdict != INKWELL_ESP_IMAGE_OK) {
            char reason[MESH_FIRMWARE_SERIAL_REASON_MAX];
            snprintf(reason, sizeof reason, "%s (chip 0x%04x)",
                     inkwell_esp_image_verdict_name(verdict), (unsigned)info.chip_id);
            return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_IMAGE, reason, -EINVAL);
        }
        memset(serial->otadata, 0xFF, sizeof serial->otadata);
        regions[0] = (struct mesh_esp_loader_region){MESH_FIRMWARE_SERIAL_APP_OFFSET, serial->image,
                                                     serial->image_len};
        regions[1] = (struct mesh_esp_loader_region){MESH_FIRMWARE_SERIAL_OTADATA_OFFSET,
                                                     serial->otadata, sizeof serial->otadata};
        region_count = 2U;
    }
    const int port = serial_find_port(serial, where);
    if (port < 0) {
        return port;
    }

    const int started = mesh_esp_loader_start(&serial->loader, loop, serial->path, chip, regions,
                                              region_count, 0U, now_ms);
    if (started < 0) {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_LOADER, serial->loader.reason,
                             started);
    }
    serial->state = MESH_FIRMWARE_SERIAL_WAITING;
    inkwell_log_info("firmware-serial", "Writing %zu bytes to %s through its ROM%s",
                     serial->image_len, serial->path, whole ? ", the whole flash" : "");
    return 0;
}

int mesh_firmware_serial_start(struct mesh_firmware_serial *serial, struct inkwell_loop *loop,
                               const char *image_path, const char *where, uint16_t chip,
                               uint64_t now_ms) {
    return serial_start(serial, loop, image_path, where, chip, false, now_ms);
}

int mesh_firmware_serial_start_whole(struct mesh_firmware_serial *serial, struct inkwell_loop *loop,
                                     const char *image_path, const char *where, uint16_t chip,
                                     uint64_t now_ms) {
    return serial_start(serial, loop, image_path, where, chip, true, now_ms);
}

void mesh_firmware_serial_tick(struct mesh_firmware_serial *serial, uint64_t now_ms) {
    if (!mesh_firmware_serial_busy(serial)) {
        return;
    }
    mesh_esp_loader_tick(&serial->loader, now_ms);
    switch (serial->loader.state) {
    case MESH_ESP_LOADER_ERASING:
    case MESH_ESP_LOADER_WRITING:
    case MESH_ESP_LOADER_VERIFYING:
        serial->flash_touched = true;
        serial->state = MESH_FIRMWARE_SERIAL_WRITING;
        return;
    case MESH_ESP_LOADER_RESTARTING:
        serial->state = MESH_FIRMWARE_SERIAL_RESTARTING;
        return;
    case MESH_ESP_LOADER_DONE:
        serial_release_image(serial);
        serial->damaged = false;
        serial->state = MESH_FIRMWARE_SERIAL_DONE;
        return;
    case MESH_ESP_LOADER_FAILED:
        (void)serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_LOADER,
                            serial->loader.reason[0] != '\0'
                                ? serial->loader.reason
                                : mesh_esp_loader_error_name(serial->loader.error),
                            0);
        return;
    default:
        return;
    }
}

void mesh_firmware_serial_cancel(struct mesh_firmware_serial *serial) {
    if (serial == NULL) {
        return;
    }
    const bool busy = mesh_firmware_serial_busy(serial);
    mesh_esp_loader_cancel(&serial->loader);
    serial_release_image(serial);
    if (!busy) {
        return;
    }
    /* Stopped after the first erase is stopped with an app that will not boot: that is a
       failure the banner and the resume answer, not an idle radio. */
    if (serial->damaged || serial->flash_touched || serial->loader.erase_sent) {
        serial->state = MESH_FIRMWARE_SERIAL_FAILED;
        serial->error = MESH_FIRMWARE_SERIAL_ERROR_LOADER;
        inkwell_str_copy(serial->reason, sizeof serial->reason, "cancelled after the erase");
        return;
    }
    serial->state = MESH_FIRMWARE_SERIAL_IDLE;
}
