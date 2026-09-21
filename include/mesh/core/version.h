#pragma once

/*
 * The client's own version, as opposed to the connected radio's firmware version.
 *
 * Which version *this build* is, and whether it may be replaced - the half of a version that
 * only a build system can answer. The arithmetic is inkwell/base/version.h, and a caller
 * comparing two strings that are not this build's should use that directly.
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

/* True only for a build the release script stamped (CMake's MESHCLIENT_RELEASE_BUILD). A
   local build reports "<version>-dev" and answers false, which is what stops the updater from
   replacing a binary someone just built with whatever happens to be on GitHub. */
bool mesh_version_is_release(void);

/* True when the running version carries a prerelease suffix, i.e. this build came off the
   beta or rc channel. The updater asks GitHub a different question in that case: the
   `releases/latest` endpoint deliberately skips prereleases, so a beta client polling it would
   only ever be offered stable. */
bool mesh_version_is_prerelease(void);

/* True when `candidate` is a strictly newer release than this build. False for a dev build,
   for an unparseable candidate, and for anything not actually newer. The one call that binds
   inkwell_version_compare() to the version this binary was stamped with. */
bool mesh_version_is_newer_than_running(const char *candidate);

#ifdef __cplusplus
}
#endif
