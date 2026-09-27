#include "mesh/transport/serial_usb.h"

#include <stddef.h>

bool mesh_serial_device_is_bootloader(const struct inkwell_serial_port_info *device) {
    return device != NULL && device->kind == INKWELL_SERIAL_NATIVE && device->mass_storage;
}

/* Espressif's own USB, the vendor of every chip with a USB Serial/JTAG of its own. */
#define ESP_NATIVE_USB_VID 0x303AU

bool mesh_serial_device_reaches_esp_rom(const struct inkwell_serial_port_info *device) {
    return device != NULL && device->kind != INKWELL_SERIAL_NATIVE &&
           device->vendor_id != ESP_NATIVE_USB_VID;
}

bool mesh_serial_device_is_radio(const struct inkwell_serial_port_info *device) {
    return device != NULL && !mesh_serial_device_is_bootloader(device);
}
