#include "mesh/transport/serial_usb.h"

#include <stddef.h>

bool mesh_serial_device_is_bootloader(const struct inkwell_serial_port_info *device) {
    return device != NULL && device->kind == INKWELL_SERIAL_NATIVE && device->mass_storage;
}

bool mesh_serial_device_is_radio(const struct inkwell_serial_port_info *device) {
    return device != NULL && !mesh_serial_device_is_bootloader(device);
}
