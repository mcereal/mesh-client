#include "mesh/transport/ble_gatt.h"

#include "inkwell/base/log.h"

#include <errno.h>
#include <string.h>

int mesh_ble_list_meshtastic(struct inkwell_ble_central *central,
                             struct inkwell_ble_device *devices, size_t capacity, size_t *count) {
    return inkwell_ble_list_by_service(central, MESH_BLE_MESHTASTIC_SERVICE_UUID, devices, capacity,
                                       count);
}

int mesh_ble_find_meshtastic_characteristics(struct inkwell_ble_central *central,
                                             const char *address,
                                             struct mesh_ble_meshtastic_chars *out) {
    if (central == NULL || address == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);

    static const struct {
        const char *uuid;
        const char *name;
        size_t offset;
    } k_required[] = {
        {MESH_BLE_TORADIO_UUID, "ToRadio", offsetof(struct mesh_ble_meshtastic_chars, toradio)},
        {MESH_BLE_FROMRADIO_UUID, "FromRadio",
         offsetof(struct mesh_ble_meshtastic_chars, fromradio)},
        {MESH_BLE_FROMNUM_UUID, "FromNum", offsetof(struct mesh_ble_meshtastic_chars, fromnum)},
    };
    for (size_t i = 0; i < sizeof k_required / sizeof k_required[0]; ++i) {
        char *handle = (char *)out + k_required[i].offset;
        const int result = inkwell_ble_find_characteristic(central, address, k_required[i].uuid,
                                                           handle, INKWELL_BLE_HANDLE_MAX);
        if (result < 0) {
            inkwell_log_warn("ble", "%s characteristic not found on %s", k_required[i].name,
                             address);
            return result;
        }
    }
    if (inkwell_ble_find_characteristic(central, address, MESH_BLE_LOGRADIO_UUID, out->logradio,
                                        sizeof out->logradio) < 0) {
        out->logradio[0] = '\0';
    }
    return 0;
}
