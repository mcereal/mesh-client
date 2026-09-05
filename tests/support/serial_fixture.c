#define _POSIX_C_SOURCE 200809L

#include "support/serial_fixture.h"

#include "mesh/transport/serial_usb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* One unbound native-USB node, as the Brick sees a Heltec nRF52840 before we bind it. */
struct mesh_serial_device_info mesh_test_serial_device(void) {
    struct mesh_serial_device_info device;
    memset(&device, 0, sizeof device);
    snprintf(device.id, sizeof device.id, "%s", "1-1:1.1");
    snprintf(device.name, sizeof device.name, "%s", "Heltec Mesh Node");
    device.vendor_id = 0x239AU;
    device.product_id = 0x4405U;
    device.busnum = 1U;
    device.devnum = 3U;
    device.control_interface = 0;
    device.bound = false;
    device.needs_line_state = true;
    return device;
}

/* Reads whatever the radio end of the socketpair has, with a short grace period. */
ssize_t mesh_test_serial_read(int fd, uint8_t *out, size_t cap) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        const ssize_t got = read(fd, out, cap);
        if (got >= 0) {
            return got;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    return 0;
}

void mesh_test_serial_sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000 * 1000};
    (void)nanosleep(&ts, NULL);
}
