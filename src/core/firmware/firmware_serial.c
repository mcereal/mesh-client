#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_serial.h"

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

/* Espressif's own USB, the vendor of every chip with a USB Serial/JTAG of its own. */
#define ESP_NATIVE_USB_VID 0x303AU
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
    return serial != NULL && serial->state == MESH_FIRMWARE_SERIAL_FAILED && serial->flash_touched;
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
 * on the bus, which is what a recovery with no id can safely assume and nothing more.
 */
static int serial_find_port(struct mesh_firmware_serial *serial, const char *where) {
    struct inkwell_serial_port_info ports[SERIAL_PORTS_MAX];
    const size_t count = inkwell_serial_scan(ports, SERIAL_PORTS_MAX);
    const struct inkwell_serial_port_info *found = NULL;
    size_t bridges = 0U;
    for (size_t i = 0; i < count; ++i) {
        const bool named = where != NULL && where[0] != '\0';
        if (named ? strcasecmp(ports[i].id, where) == 0 || strcmp(ports[i].path, where) == 0
                  : ports[i].kind == INKWELL_SERIAL_BRIDGE) {
            found = &ports[i];
            bridges += 1U;
            if (named) {
                break;
            }
        }
    }
    if (found == NULL || bridges != 1U || found->path[0] == '\0') {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_NO_PORT,
                             bridges > 1U ? "more than one serial bridge; connect the radio first"
                                          : "the radio's serial port is not there",
                             -ENODEV);
    }
    if (found->kind == INKWELL_SERIAL_NATIVE || found->vendor_id == ESP_NATIVE_USB_VID) {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_NATIVE_USB, found->name, -ENOTSUP);
    }
    inkwell_str_copy(serial->path, sizeof serial->path, found->path);
    return 0;
}

int mesh_firmware_serial_start(struct mesh_firmware_serial *serial, struct inkwell_loop *loop,
                               const char *image_path, const char *where, uint16_t chip,
                               uint64_t now_ms) {
    if (serial == NULL || image_path == NULL) {
        return -EINVAL;
    }
    mesh_firmware_serial_cancel(serial);
    memset(serial, 0, sizeof *serial);

    size_t len = 0U;
    serial->image = inkwell_file_read(image_path, MESH_FIRMWARE_SERIAL_IMAGE_MAX, &len);
    if (serial->image == NULL || len == 0U) {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_IMAGE,
                             "the staged image could not be read", -EIO);
    }
    serial->image_len = len;
    /* The header is the only thing in an app image that names a chip, and the last look at
       the file before a flash is erased to take it. */
    struct inkwell_esp_image_info info;
    const enum inkwell_esp_image_verdict verdict =
        inkwell_esp_image_validate(serial->image, serial->image_len, chip, &info);
    if (verdict != INKWELL_ESP_IMAGE_OK) {
        char reason[MESH_FIRMWARE_SERIAL_REASON_MAX];
        snprintf(reason, sizeof reason, "%s (chip 0x%04x)", inkwell_esp_image_verdict_name(verdict),
                 (unsigned)info.chip_id);
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_IMAGE, reason, -EINVAL);
    }
    const int port = serial_find_port(serial, where);
    if (port < 0) {
        return port;
    }

    memset(serial->otadata, 0xFF, sizeof serial->otadata);
    const struct mesh_esp_loader_region regions[2] = {
        {MESH_FIRMWARE_SERIAL_APP_OFFSET, serial->image, serial->image_len},
        {MESH_FIRMWARE_SERIAL_OTADATA_OFFSET, serial->otadata, sizeof serial->otadata},
    };
    const int started =
        mesh_esp_loader_start(&serial->loader, loop, serial->path, chip, regions, 2U, 0U, now_ms);
    if (started < 0) {
        return serial_refuse(serial, MESH_FIRMWARE_SERIAL_ERROR_LOADER, serial->loader.reason,
                             started);
    }
    serial->state = MESH_FIRMWARE_SERIAL_WAITING;
    inkwell_log_info("firmware-serial", "Writing %zu bytes to %s through its ROM",
                     serial->image_len, serial->path);
    return 0;
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
    mesh_esp_loader_cancel(&serial->loader);
    serial_release_image(serial);
    if (mesh_firmware_serial_busy(serial)) {
        serial->state = MESH_FIRMWARE_SERIAL_IDLE;
    }
}
