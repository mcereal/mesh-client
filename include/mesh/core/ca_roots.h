#pragma once

/*
 * The CA roots compiled into this binary: Mozilla's set, as DER.
 *
 * This is *this client's* answer to what it trusts, handed to the TLS client at startup through
 * `mesh_tls_set_roots()`. Nothing below that call knows where a root came from, which is the
 * point: compiling them in is a decision about how this product ships, not about how TLS works.
 *
 * And the decision is a real one. The Brick has no system certificate store, and a bundle shipped
 * in the pak does not ship through self-update - so an install that is only ever updated in place
 * keeps the roots its first pak carried, and the day one of them is withdrawn the connection that
 * stops working is the one the fix would come down. In the binary, every release refreshes them.
 *
 * Written by scripts/gen-ca-roots.py into src/core/generated/ca_roots.c; not part of the build.
 */

#include <stddef.h>

#include "mesh/core/tls_client.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const struct mesh_tls_ca_root mesh_ca_roots[];
extern const size_t mesh_ca_root_count;

#ifdef __cplusplus
}
#endif
