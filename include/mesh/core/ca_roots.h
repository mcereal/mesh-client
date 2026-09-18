#pragma once

/*
 * The CA roots compiled into this binary: Mozilla's set, as DER.
 *
 * The Brick has no system certificate store, and a bundle shipped in the pak does not ship through
 * self-update - so an install that is only ever updated in place keeps the roots its first pak
 * carried, and the day one of them is withdrawn the connection that stops working is the one the
 * fix would come down. In the binary, every release refreshes them.
 *
 * Written by scripts/gen-ca-roots.py into src/core/generated/ca_roots.c; not part of the build.
 * `mesh_tls_client_start()` verifies against these when no bundle file is named.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ca_root {
    /* The label Mozilla gives the root, for a log line when one fails to parse. */
    const char *name;
    /* Static for the life of the process, which is what lets Mbed TLS point into it rather than
       copy it. */
    const unsigned char *der;
    size_t len;
};

extern const struct mesh_ca_root mesh_ca_roots[];
extern const size_t mesh_ca_root_count;

#ifdef __cplusplus
}
#endif
