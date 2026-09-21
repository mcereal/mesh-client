#include "mesh/core/version.h"

#include "inkwell/base/version.h"

#ifndef MESHCLIENT_VERSION
#define MESHCLIENT_VERSION "0.0.0-dev"
#endif

const char *mesh_version_string(void) { return MESHCLIENT_VERSION; }

/*
 * Only the release build defines MESHCLIENT_RELEASE_BUILD, and it is what the updater gates on
 * - not the shape of the version string. A local build reports "<version>-dev" and answers
 * false here, so the updater never offers to replace a binary someone just built.
 */
bool mesh_version_is_release(void) {
#ifdef MESHCLIENT_RELEASE_BUILD
    return true;
#else
    return false;
#endif
}

bool mesh_version_is_prerelease(void) { return inkwell_version_is_prerelease(MESHCLIENT_VERSION); }

bool mesh_version_is_newer_than_running(const char *candidate) {
    if (!mesh_version_is_release()) {
        return false;
    }
    return inkwell_version_compare(candidate, mesh_version_string()) > 0;
}
