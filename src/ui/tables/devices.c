#define _POSIX_C_SOURCE 200809L

/*
 * The Devices tab's row table. See include/mesh/ui/devices.h for why the last row is not a
 * device.
 */

#include "mesh/ui/devices.h"

/* Whether discovery already published a network row - which it does exactly while a network
   link is up, and never otherwise, because nothing enumerates a host. */
static bool mesh_ui_devices_hold_network(const struct mesh_ui_device *devices, size_t count) {
    if (devices == NULL) {
        return false;
    }
    for (size_t i = 0; i < count && i < MESH_UI_MAX_DEVICES; ++i) {
        if (devices[i].kind == (uint8_t)MESH_UI_DEVICE_TCP) {
            return true;
        }
    }
    return false;
}

uint32_t mesh_ui_devices_row_count(const struct mesh_ui_device *devices, size_t count) {
    if (devices == NULL) {
        count = 0U;
    }
    if (count > MESH_UI_MAX_DEVICES) {
        count = MESH_UI_MAX_DEVICES;
    }
    return mesh_ui_devices_hold_network(devices, count) ? (uint32_t)count : (uint32_t)count + 1U;
}

bool mesh_ui_devices_row(const struct mesh_ui_device *devices, size_t count,
                         const char *network_host, uint32_t index,
                         struct mesh_ui_devices_row *out) {
    if (out == NULL || index >= mesh_ui_devices_row_count(devices, count)) {
        return false;
    }
    if (count > MESH_UI_MAX_DEVICES) {
        count = MESH_UI_MAX_DEVICES;
    }
    if (devices != NULL && index < (uint32_t)count) {
        out->type = (uint8_t)MESH_UI_DEVICES_ROW_DEVICE;
        out->device = &devices[index];
        out->host = "";
        return true;
    }
    out->type = (uint8_t)MESH_UI_DEVICES_ROW_NETWORK;
    out->device = NULL;
    out->host = network_host != NULL ? network_host : "";
    return true;
}
