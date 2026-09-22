#pragma once

#include "inkwell/ble/central.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Meshtastic's BLE GATT contract (https://meshtastic.org/docs/development/device/client-api/),
 * over inkwell's central.
 *
 * Not the Nordic UART Service: ToRadio takes one raw protobuf per write with no length framing,
 * and inbound is pull-based - FromNum notifies, and the client then reads FromRadio until it
 * returns empty.
 *
 * The central underneath is general and knows none of this. What is here is the vocabulary of
 * one firmware: which service to scan for, and which four characteristics a link needs.
 */
#define MESH_BLE_MESHTASTIC_SERVICE_UUID "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD"
#define MESH_BLE_TORADIO_UUID "F75C76D2-129E-4DAD-A1DD-7866124401E7"
#define MESH_BLE_FROMRADIO_UUID "2C55E69E-4993-11ED-B878-0242AC120002"
#define MESH_BLE_FROMNUM_UUID "ED9DA18C-A800-4F66-A670-AA7547E34453"
#define MESH_BLE_LOGRADIO_UUID "5A3D6E49-06E6-4423-9944-E9DE8CDF9547"

/* Largest payload the firmware accepts on ToRadio / returns from FromRadio - which happens to
   be ATT's own limit as well. */
#define MESH_BLE_MAX_PACKET_SIZE 512U

/* The handles of the characteristics a Meshtastic link reads, writes and watches. */
struct mesh_ble_meshtastic_chars {
    char toradio[INKWELL_BLE_HANDLE_MAX];
    char fromradio[INKWELL_BLE_HANDLE_MAX];
    char fromnum[INKWELL_BLE_HANDLE_MAX];
    char logradio[INKWELL_BLE_HANDLE_MAX]; /* empty when the node does not expose it */
};

/* Every node the stack holds that advertises the Meshtastic service. */
int mesh_ble_list_meshtastic(struct inkwell_ble_central *central,
                             struct inkwell_ble_device *devices, size_t capacity, size_t *count);

/* Looks up all four characteristics under `address`. The first three are required; LogRadio is
   optional, because older firmware does not expose it. */
int mesh_ble_find_meshtastic_characteristics(struct inkwell_ble_central *central,
                                             const char *address,
                                             struct mesh_ble_meshtastic_chars *out);

#ifdef __cplusplus
}
#endif
