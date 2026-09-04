#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Finding and opening a USB serial port on the Brick.
 *
 * The Brick's kernel (TinaLinux 4.9.191) has CONFIG_USB_ACM off with no module, so a native-USB
 * node - a Heltec nRF52840 (239a:4405), say - enumerates as CDC-ACM and then gets no driver and
 * no /dev/ttyACM*. Only cp210x, ch341, ftdi_sio and the generic usbserial driver exist, which
 * cover UART-bridge boards but not native-USB nodes.
 *
 * The workaround, verified end to end on the device: write "VID PID" to
 * /sys/bus/usb-serial/drivers/generic/new_id. The generic driver rejects the control interface
 * ("no bulk out") and attaches the data interface as /dev/ttyUSB*. The node then stays silent
 * until DTR is asserted, because TinyUSB discards output while the host has not set the line
 * state - and the generic driver cannot set it. So one CDC SET_CONTROL_LINE_STATE goes out
 * through usbfs (/dev/bus/usb/BBB/DDD) against the unbound control interface. Neither survives a
 * reboot, so the transport redoes both at start.
 *
 * UART-bridge boards (ESP32 dev kits) need none of this: their driver is present, the tty is
 * already there, and DTR is a normal TIOCMBIS.
 *
 * Everything here is mockable so tests never touch sysfs, usbfs or a real tty.
 */

#define MESH_SERIAL_MAX_DEVICES 8U

struct mesh_serial_device_info {
    /* Stable identifier for the UI and for reconnects: the sysfs interface name ("1-1:1.1"). */
    char id[64];
    /* "/dev/ttyUSB0"; empty until the interface has a driver bound. */
    char path[64];
    /* USB product string, falling back to "USB serial". */
    char name[64];
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t busnum;
    uint8_t devnum;
    /* bInterfaceNumber of the CDC control interface on the same device, or -1 when this is a
       UART bridge with nothing to set the line state on. */
    int control_interface;
    /* A tty node exists for this interface right now. */
    bool bound;
    /* Bound by the generic usbserial driver, which cannot drive DTR: it has to go through
       usbfs after the port is open. */
    bool needs_line_state;
};

/* Scans /sys/bus/usb/devices for USB serial candidates: interfaces already bound to a usb-serial
   driver, plus unbound CDC-Data interfaces that could be bound. Returns how many entries were
   written (at most `capacity`). */
size_t mesh_serial_usb_scan(struct mesh_serial_device_info *out, size_t capacity);

/* Binds an unbound CDC-Data interface to the generic usbserial driver and waits (up to about a
   second) for its tty to appear, filling in `device->path` and `device->bound`. Returns 0, or a
   negative errno. Already-bound devices return 0 immediately. */
int mesh_serial_usb_bind(struct mesh_serial_device_info *device);

/* Sends one CDC SET_CONTROL_LINE_STATE to the device's control interface through usbfs.
   Returns 0, -ENOTSUP when the device has no control interface, or a negative errno. */
int mesh_serial_usb_set_line_state(const struct mesh_serial_device_info *device, bool dtr,
                                   bool rts);

/* Opens the tty raw and non-blocking at 115200 (meaningless over USB CDC, honoured by bridges).
   Returns the fd or a negative errno. */
int mesh_serial_port_open(const char *path);
void mesh_serial_port_close(int fd);

/* TIOCMBIS/TIOCMBIC on the tty. Returns 0, or a negative errno; -ENOTTY when the fd is not a
   tty, which is the case for the generic-driver ports that need the usbfs path instead. */
int mesh_serial_port_set_dtr(int fd, bool on);

struct mesh_serial_usb_mock_config {
    const struct mesh_serial_device_info *devices;
    size_t device_count;
    int scan_result;       /* < 0 makes the scan report nothing */
    int bind_result;       /* returned by mesh_serial_usb_bind */
    int line_state_result; /* returned by mesh_serial_usb_set_line_state */
    /* Path a successful bind reports for a device the scan found unbound. */
    const char *bound_path;
    /* When >= 0, mesh_serial_port_open dup()s this instead of opening a tty: tests hand it one
       end of a socketpair and script the radio from the other. */
    int open_fd;
    int open_result; /* < 0 makes every open fail with this */
};

void mesh_serial_usb_mock_enable(const struct mesh_serial_usb_mock_config *config);
void mesh_serial_usb_mock_disable(void);
/* How many times the transport asked for a bind and a line-state assert; lets a test prove the
   Brick workaround runs exactly once per connect. */
size_t mesh_serial_usb_mock_bind_calls(void);
size_t mesh_serial_usb_mock_line_state_calls(void);

#ifdef __cplusplus
}
#endif
