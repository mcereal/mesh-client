#ifndef MESH_ENV_H
#define MESH_ENV_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One reading of the MESHCLIENT_* environment knobs.
 *
 * These are the documented escape hatches (docs/cli.md) that people reach for on a device with
 * no keyboard, so "MESHCLIENT_AUTOCONNECT=false" and "MESHCLIENT_DISABLE_BLE=no" have to mean
 * what they look like. Three call sites had each grown their own parser, and they disagreed:
 * one accepted "off" but not "yes", another the reverse, a third only "0". Anything a user
 * typed that landed in the gap was silently read as the opposite of what they meant.
 */

/* Truthy: "1", "true", "yes", "on". Falsy: "0", "false", "no", "off". Case-insensitive.
   Unset, empty and unrecognised all yield `fallback`; an unrecognised value also warns, since
   it means the user asked for something and did not get it. `label` names the knob in that
   warning (e.g. "BLE"), and may be NULL to use the variable's own name. */
bool mesh_env_bool(const char *name, const char *label, bool fallback);

/* Decimal integer within [min, max]. Unset and empty yield `fallback` quietly; a value that is
   not a number, or is out of range, yields `fallback` and warns. */
long mesh_env_int(const char *name, long min, long max, long fallback);

#ifdef __cplusplus
}
#endif

#endif /* MESH_ENV_H */
