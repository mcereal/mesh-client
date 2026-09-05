#ifndef MESH_TEST_SUPPORT_SERIAL_FIXTURE_H
#define MESH_TEST_SUPPORT_SERIAL_FIXTURE_H

/* A scripted USB-serial node, as the Brick's serial transport sees one. */

#include "mesh/transport/serial_usb.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct mesh_serial_device_info mesh_test_serial_device(void);

ssize_t mesh_test_serial_read(int fd, uint8_t *out, size_t cap);

void mesh_test_serial_sleep_ms(long ms);

#endif /* MESH_TEST_SUPPORT_SERIAL_FIXTURE_H */
