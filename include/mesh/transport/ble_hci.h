#pragma once

/* The OTA transfer's connection-interval policy over inkwell's BLE central. */

#include "inkwell/ble/central.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ask for the 7.5 ms interval the loader is designed for. A refusal only makes the transfer
   slower, so callers log the result and continue. */
int mesh_ble_ota_request_interval(struct inkwell_ble_central *central, const char *address);

#ifdef __cplusplus
}
#endif
