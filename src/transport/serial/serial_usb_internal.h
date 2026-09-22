#pragma once

#include "mesh/transport/serial_usb.h"

/* The macOS scan, over the I/O Registry. Returns 0 everywhere else. Only serial_usb.c calls it:
   mesh_serial_usb_scan() is the one entry point, and it decides which tree to read. */
size_t mesh_serial_usb_scan_iokit(struct mesh_serial_device_info *out, size_t capacity);
