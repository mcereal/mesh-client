#pragma once

#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/transport/transport.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The TCP link: a Meshtastic node reached over the network rather than over a cable or the air.
 *
 * Same shape as the other two - it owns a connection and a `struct mesh_session`, hands every
 * FromRadio protobuf to the session, and installs a send path while connected. The wire is the
 * same byte stream the serial API uses (mesh/proto/stream_framing.h), which is why both sit on
 * `struct mesh_stream_link` and only the getting-a-descriptor half is written twice.
 *
 * What this reaches: an ESP32 node on WiFi with its network module enabled, and `meshtasticd` on
 * anything that runs Linux. Both listen on MESH_TCP_DEFAULT_PORT.
 *
 * What it deliberately does not do is resolve a hostname. `getaddrinfo()` blocks, this client is
 * one epoll loop with no threads in it, and a DNS lookup that takes five seconds is five seconds
 * of frozen UI - so an address is a numeric literal here, v4 or v6, and a name is refused in
 * words rather than paid for in a stall. Resolving one needs the forked-child shape
 * `src/core/fetch.c` already uses for HTTPS; see docs/transport.md.
 */

/* Upstream's port for the TCP client API, shared by the firmware and by meshtasticd. */
#define MESH_TCP_DEFAULT_PORT 4403

/*
 * A target as the user writes it: "192.168.1.50", "192.168.1.50:4403", or a bracketed v6
 * literal "[fd00::1]:4403". Sized for a full v6 literal with a port and brackets, with room to
 * spare, so `preferred_tcp_host` in mesh_app_config and this agree.
 */
#define MESH_TCP_TARGET_MAX 64U

struct mesh_transport *mesh_tcp_transport(void);

/*
 * Connects to `target`. Returns 0 with the link in `connecting` - a non-blocking connect is
 * rarely finished when it returns - or a negative errno:
 *
 *   -ENODEV   the transport is disabled by configuration
 *   -EBUSY    a connect is already running or a link is already up
 *   -EINVAL   `target` is not an address and a port this client can parse
 *
 * A name that is not a numeric address lands on -EINVAL, and take_error() says which.
 */
int mesh_tcp_transport_connect(struct mesh_transport *transport, const char *target);
int mesh_tcp_transport_disconnect(struct mesh_transport *transport);

/* The target of the established link, or NULL when it is down or still connecting. */
const char *mesh_tcp_transport_connected_target(struct mesh_transport *transport);
/* True between the socket being created and the handshake going out. */
bool mesh_tcp_transport_is_connecting(struct mesh_transport *transport);

/*
 * The host this transport was configured to reach, whether or not it is up, or NULL when none
 * was configured. This is a TCP link's whole answer to discovery: a network has no equivalent of
 * a sysfs scan or a BLE advertisement, so the one thing that can be listed is the address
 * somebody already wrote down.
 */
const char *mesh_tcp_transport_configured_target(struct mesh_transport *transport);

struct mesh_tcp_transport_stats {
    size_t frames_received;
    size_t bytes_received;
    /* Bytes the parser discarded resyncing. Unlike a serial port this should stay at 0: nothing
       interleaves a log with the frames on a socket, so anything here is a protocol fault. */
    size_t junk_bytes;
};

struct mesh_tcp_transport_stats mesh_tcp_transport_stats(struct mesh_transport *transport);

/* The session this link feeds. NULL only when the transport is unusable. */
struct mesh_session *mesh_tcp_transport_session(struct mesh_transport *transport);
struct mesh_handshake_status mesh_tcp_transport_handshake_status(struct mesh_transport *transport);

/* Reads whatever the socket has ready and folds complete frames into the session. Called from
   the event loop; exposed so tests can drive it against a loopback listener. Returns the number
   of bytes read, 0 when nothing was ready, or a negative errno (the link is reset on a fatal
   one). */
int mesh_tcp_transport_pump(struct mesh_transport *transport);

/*
 * Splits "host", "host:port" or "[v6]:port" into its parts, defaulting the port to
 * MESH_TCP_DEFAULT_PORT. Returns 0, or -EINVAL when it is not one of those shapes or the port is
 * not 1-65535. Exposed for the tests and for anything that wants to validate a target before
 * offering to connect to it; it does not check that the host is an address.
 */
int mesh_tcp_target_split(const char *target, char *host, size_t host_len, uint16_t *port);

#ifdef __cplusplus
}
#endif
