#pragma once

/*
 * The client's own version, as opposed to the connected radio's firmware version.
 *
 * MESHCLIENT_VERSION is a compile definition fed by CMake from `project(meshclient VERSION ...)`,
 * which the release workflow rewrites. The build is the only place that knows it, so nothing
 * below hardcodes a number - a build without the definition reports "dev" and the updater
 * refuses to compare against it rather than offering to "upgrade" a working tree to a release.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "1.12.0", or "dev" for a build with no version baked in. */
const char *mesh_version_string(void);

/* True when this build carries a real release version (i.e. not "dev"). */
bool mesh_version_is_release(void);

/*
 * Orders two semantic versions: negative when a < b, 0 when equal, positive when a > b.
 *
 * Tolerates a leading 'v' or 'V' (GitHub tags carry one, CMake does not) and missing minor or
 * patch components ("1.2" == "1.2.0"). Build metadata after '+' is ignored, as SemVer requires.
 * A prerelease sorts below the release it precedes ("1.2.0-rc.1" < "1.2.0"), and two
 * prereleases are compared dot-part by dot-part with numeric parts ordered numerically - which
 * is what keeps the beta and rc channels from offering to "update" sideways.
 *
 * Anything unparseable compares as lower than anything parseable, so a garbled tag from the
 * network can never look newer than what is running.
 */
int mesh_version_compare(const char *a, const char *b);

/* True when `candidate` is a strictly newer release than this build. False for a dev build,
   for an unparseable candidate, and for anything not actually newer. */
bool mesh_version_is_newer_than_running(const char *candidate);

#ifdef __cplusplus
}
#endif
