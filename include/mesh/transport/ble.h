#pragma once

#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_transport *mesh_ble_transport(void);
const struct mesh_bluez_device_info *mesh_ble_transport_devices(struct mesh_transport *transport, size_t *count);
size_t mesh_ble_transport_get_devices(struct mesh_transport *transport, struct mesh_bluez_device_info *out,
                                      size_t capacity);
size_t mesh_ble_transport_refresh_devices(struct mesh_transport *transport);
int mesh_ble_transport_connect(struct mesh_transport *transport, const char *address);
int mesh_ble_transport_disconnect(struct mesh_transport *transport);

#ifdef __cplusplus
}
#endif
