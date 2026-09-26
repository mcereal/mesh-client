#include "mesh/transport/ble_gatt.h"

#include "inkwell/base/log.h"
#include "mesh/transport/ble_dfu.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

int mesh_ble_list_profile(struct inkwell_ble_central *central,
                          const struct mesh_ble_profile *profile,
                          struct inkwell_ble_device *devices, size_t capacity, size_t *count) {
    if (profile == NULL || profile->service_uuid == NULL) {
        return -EINVAL;
    }
    const int result =
        inkwell_ble_list_by_service(central, profile->service_uuid, devices, capacity, count);
    /* A service shared with things that are not radios: keep only what is named like one. A
       radio whose name has not been heard yet is left out until it is, rather than guessed. */
    if (result >= 0 && profile->name_prefix != NULL && devices != NULL && count != NULL) {
        const size_t prefix_len = strlen(profile->name_prefix);
        size_t kept = 0U;
        for (size_t i = 0; i < *count; ++i) {
            if (strncmp(devices[i].name, profile->name_prefix, prefix_len) == 0) {
                devices[kept++] = devices[i];
            }
        }
        *count = kept;
    }
    if (result < 0 || !profile->adopts_bonded_dfu || devices == NULL || count == NULL ||
        *count >= capacity) {
        return result;
    }
    /*
     * And a bonded radio whose record has lost the protocol's service.
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

int mesh_ble_list_meshtastic(struct inkwell_ble_central *central,
                             struct inkwell_ble_device *devices, size_t capacity, size_t *count) {
    return mesh_ble_list_profile(central, &mesh_ble_profile_meshtastic, devices, capacity, count);
}

int mesh_ble_find_characteristics(struct inkwell_ble_central *central, const char *address,
                                  const struct mesh_ble_profile *profile,
                                  struct mesh_ble_chars *out) {
    if (central == NULL || address == NULL || out == NULL || !mesh_ble_profile_usable(profile)) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);

    const struct {
        const char *uuid;
        const char *role;
        char *handle;
    } required[] = {
        {profile->write_uuid, "write", out->write},
        {profile->notify_uuid, "notify", out->notify},
        {profile->inbound == MESH_BLE_INBOUND_PULL ? profile->read_uuid : NULL, "read", out->read},
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        if (required[i].uuid == NULL) {
            continue;
        }
        const int result = inkwell_ble_find_characteristic(
            central, address, required[i].uuid, required[i].handle, INKWELL_BLE_HANDLE_MAX);
        if (result < 0) {
            inkwell_log_warn("ble", "%s %s characteristic (%s) not found on %s", profile->name,
                             required[i].role, required[i].uuid, address);
            return result;
        }
    }
    if (profile->log_uuid != NULL &&
        inkwell_ble_find_characteristic(central, address, profile->log_uuid, out->log,
                                        sizeof out->log) < 0) {
        out->log[0] = '\0';
    }
    return 0;
}
