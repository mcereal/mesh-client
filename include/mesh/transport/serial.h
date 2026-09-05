#pragma once

#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/transport/serial_usb.h"
#include "mesh/transport/transport.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The USB serial link. Same shape as the BLE link: it owns a connection and a
 * `struct mesh_session`, hands every FromRadio protobuf to the session, and installs a send
 * path while connected. What differs is the wire: Meshtastic's serial API is a byte stream with
 * 0x94 0xC3 length framing (mesh/proto/stream_framing.h) and the radio's text log interleaved
 * between frames, rather than BLE's one-bare-protobuf-per-GATT-operation.
 */

struct mesh_transport *mesh_serial_transport(void);

/* Rescans sysfs for USB serial ports. Returns how many are cached. */
size_t mesh_serial_transport_refresh_devices(struct mesh_transport *transport);
/* Borrowed view of the cache; valid until the next refresh or tick. */
const struct mesh_serial_device_info *
mesh_serial_transport_devices(struct mesh_transport *transport, size_t *count);
size_t mesh_serial_transport_get_devices(struct mesh_transport *transport,
                                         struct mesh_serial_device_info *out, size_t capacity);

/* Connects to a port by sysfs id ("1-1:1.1") or device node ("/dev/ttyUSB0"). Binds the generic
   usbserial driver and asserts DTR first when the device needs it. Returns 0 with the link in
   `waking` - the radio gets a moment to resync before the handshake goes out - or a negative
   errno. */
int mesh_serial_transport_connect(struct mesh_transport *transport, const char *identifier);
int mesh_serial_transport_disconnect(struct mesh_transport *transport);

/* The device node of the connected port, or NULL when the link is down or still waking. */
const char *mesh_serial_transport_connected_port(struct mesh_transport *transport);
/* True between the port opening and the handshake going out. */
bool mesh_serial_transport_is_connecting(struct mesh_transport *transport);

struct mesh_serial_transport_stats {
    size_t frames_received;
    size_t bytes_received;
    /* Bytes the parser discarded resyncing: almost all of it is the radio's own text log. */
    size_t junk_bytes;
};

struct mesh_serial_transport_stats mesh_serial_transport_stats(struct mesh_transport *transport);

/* The session this link feeds. NULL only when the transport is unusable. */
struct mesh_session *mesh_serial_transport_session(struct mesh_transport *transport);
struct mesh_handshake_status
mesh_serial_transport_handshake_status(struct mesh_transport *transport);

/* Reads whatever the port has ready and folds complete frames into the session. Called from the
   event loop; exposed so tests can drive it against a socketpair. Returns the number of bytes
   read, 0 when nothing was ready, or a negative errno (the link is reset on a fatal one). */
int mesh_serial_transport_pump(struct mesh_transport *transport);

#ifdef __cplusplus
}
#endif
