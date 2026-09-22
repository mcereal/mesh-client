#pragma once

#include "inkwell/io/serial.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which USB serial ports are a radio.
 *
 * inkwell's `io/serial.h` finds the ports - sysfs on the Brick, the I/O Registry on a Mac - and
 * does the Brick's workaround for a kernel without CDC-ACM (the generic-driver bind, the usbfs
 * DTR). It reports what the USB tree says about each port and nothing about what that makes it.
 * This is that half.
 *
 * The difference that matters is a bootloader. A UART bridge (CP2102, CH341, FTDI) is a separate
 * chip: USB says nothing about what is wired to its far side, so a bridge is never a bootloader,
 * whatever the board behind it is doing, and has to be assumed a radio. A native-USB node is the
 * MCU's own peripheral, and there the question is real: an Adafruit UF2 bootloader presents a
 * mass-storage endpoint beside its CDC pair, and that sibling interface is the whole tell.
 *
 * It matters because a bootloader speaks no protobuf and will never answer a handshake. Without
 * this the client binds one, asserts DTR, auto-connects and asks for a config sync that nothing
 * replies to, which reads on the frame as a connected radio with the progress bar turning
 * forever.
 */

#define MESH_SERIAL_MAX_DEVICES 8U

/* The rate Meshtastic's serial API runs at. Meaningless over a native node's USB CDC; a bridge
   passes it to the UART. */
#define MESH_SERIAL_BAUD 115200U

/* A UF2 bootloader: a native port with a mass-storage interface beside it. Not a radio - it is
   the drive a .uf2 is written to. */
bool mesh_serial_device_is_bootloader(const struct inkwell_serial_port_info *device);

/* Everything else: what a handshake can be attempted against. Asked by the transport before it
   opens a port and by auto-connect before it picks one, so that the two cannot disagree about
   which devices are candidates. */
bool mesh_serial_device_is_radio(const struct inkwell_serial_port_info *device);

#ifdef __cplusplus
}
#endif
