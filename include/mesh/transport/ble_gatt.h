#pragma once

#include "inkwell/ble/central.h"
#include "mesh/proto/ble_profile.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A protocol's GATT contract (mesh/proto/ble_profile.h), over inkwell's central.
 *
 * The central underneath is general and knows none of this. The profile is the vocabulary of
 * one firmware - which service to scan for, which characteristics a link needs - and what is
 * here is looking those up on a real radio.
 *
 * BLE carries no length framing: one frame per GATT write, read or notification. Framing is a
 * stream concern (mesh/proto/stream_framing.h).
 */

/* The handles of the characteristics a link writes, watches and reads. */
struct mesh_ble_chars {
    char write[INKWELL_BLE_HANDLE_MAX];
    char notify[INKWELL_BLE_HANDLE_MAX];
    char read[INKWELL_BLE_HANDLE_MAX]; /* empty for a NOTIFY profile */
    char log[INKWELL_BLE_HANDLE_MAX];  /* empty when the profile or the radio has none */
};

/* Every radio the stack holds that advertises `profile`'s service - and, for the profile that
   adopts them, every bonded one whose record lists only an nRF52's DFU service. */
int mesh_ble_list_profile(struct inkwell_ble_central *central,
                          const struct mesh_ble_profile *profile,
                          struct inkwell_ble_device *devices, size_t capacity, size_t *count);

/* The same for Meshtastic, which is what the firmware installer scans for. */
int mesh_ble_list_meshtastic(struct inkwell_ble_central *central,
                             struct inkwell_ble_device *devices, size_t capacity, size_t *count);

/* Looks up `profile`'s characteristics under `address`. Write and notify are required, and read
   is too for a PULL profile; the log characteristic is optional, because older firmware does
   not expose it. -EINVAL for a profile mesh_ble_profile_usable() refuses. */
int mesh_ble_find_characteristics(struct inkwell_ble_central *central, const char *address,
                                  const struct mesh_ble_profile *profile,
                                  struct mesh_ble_chars *out);

#ifdef __cplusplus
}
#endif
