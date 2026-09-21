#pragma once

#include "mesh/core/mqtt_proxy.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What this client says about a broker connection.
 *
 * `src/core/net/mqtt_proxy.c` holds one socket to one broker and does not name a single word a
 * user reads: a failure is `struct mesh_mqtt_proxy_failure` and a state is an enum. This is the
 * other side of that line - the two tables that turn either into a sentence, in whichever
 * language is in force.
 *
 * They are here rather than beside the proxy because the proxy is on its way down to inkwell,
 * where there is no catalog to name an id in. What a broker connection *is* generalises to any
 * application; what to say about one does not.
 */

/* The state as a sentence: one entry per `enum mesh_mqtt_proxy_state`, never NULL. */
const char *mesh_ui_mqtt_state_str(enum mesh_mqtt_proxy_state state);

/*
 * Why the last attempt failed, written into `out`, or "" when none has.
 *
 * Takes the proxy rather than the failure record because two of the sentences need something
 * else off it: a bad address has to be shown as the address, since it never became a host, and
 * a TLS failure carries the library's own account of itself, which no number could rebuild.
 *
 * Always NUL-terminates when out_len > 0.
 */
void mesh_ui_mqtt_failure_text(const struct mesh_mqtt_proxy *proxy, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
