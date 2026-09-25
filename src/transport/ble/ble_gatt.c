#include "mesh/transport/ble_gatt.h"

#include "inkwell/base/log.h"
#include "mesh/transport/ble_dfu.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

int mesh_ble_list_meshtastic(struct inkwell_ble_central *central,
                             struct inkwell_ble_device *devices, size_t capacity, size_t *count) {
    const int result = inkwell_ble_list_by_service(central, MESH_BLE_MESHTASTIC_SERVICE_UUID,
                                                   devices, capacity, count);
    if (result < 0 || devices == NULL || count == NULL || *count >= capacity) {
        return result;
    }
    /*
     * And a bonded radio whose record has lost the Meshtastic service.
     *
     * An nRF52's DFU bootloader comes up at the radio's own address, bonded, and BlueZ's GATT
     * discovery of it replaces the services on that one record - so the radio it turns back
     * into advertises under a record listing the bootloader's `1530` and nothing of Meshtastic
     * until something connects and looks again. Nothing would: auto-connect only reached for
     * what this listed. Seen on a T1000-E after a BLE install and again after a USB one,
     * 2026-09-25, advertising at -34 dBm and never connected to.
     *
     * Only a bonded one. Every Meshtastic nRF52 carries `1530` for its buttonless trigger, but
     * so does anything else built on Adafruit's stack, and a stranger is not a radio of ours.
     */
    struct inkwell_ble_device dfu[16];
    size_t dfu_count = 0U;
    if (inkwell_ble_list_by_service(central, MESH_BLE_DFU_SERVICE_UUID, dfu,
                                    sizeof dfu / sizeof dfu[0], &dfu_count) < 0) {
        return result;
    }
    for (size_t i = 0; i < dfu_count && *count < capacity; ++i) {
        bool listed = !dfu[i].paired;
        for (size_t j = 0; j < *count && !listed; ++j) {
            listed = strcasecmp(devices[j].address, dfu[i].address) == 0;
        }
        if (!listed) {
            devices[(*count)++] = dfu[i];
        }
    }
    return result;
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
