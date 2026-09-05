#!/usr/bin/env bash
#
# The release build, run from semantic-release's prepare step rather than from a workflow step.
#
# Why the order matters: the version the client reports comes from `project(meshclient VERSION
# ...)` in CMakeLists.txt, and semantic-release rewrites that line during prepare. A build that
# runs before prepare therefore bakes in the *previous* release's number, which is invisible
# until something reads it - and the About screen and the self-updater both do. So the sed and
# this script are two halves of one prepareCmd, in that order, and nothing builds before them.
#
# Expects the cross toolchain on PATH and the static libdbus already built; the workflow's
# earlier steps set CROSS_COMPILE, DBUS_CFLAGS and DBUS_LIBS up for exactly that.
set -euo pipefail

VERSION="${1:-}"
if [[ -z "${VERSION}" ]]; then
    echo "usage: $0 <version>" >&2
    exit 1
fi

# CMake's project(VERSION) accepts numeric components only - it errors outright on a SemVer
# prerelease - but the beta and rc channels release exactly those (1.13.0-beta.1). So the
# project() line gets the numeric part and the full tag is passed to the build separately, as
# MESHCLIENT_VERSION_OVERRIDE. Keeping CMakeLists.txt numeric also keeps this rewrite idempotent:
# a suffix left in the file would not match the pattern on the next release and the version
# would compound instead of being replaced.
NUMERIC="${VERSION%%-*}"   # drop any -beta.1 / -rc.2
NUMERIC="${NUMERIC%%+*}"   # and any +build metadata
if [[ ! "${NUMERIC}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Cannot derive a numeric CMake version from '${VERSION}'." >&2
    exit 1
fi

sed -i "s/^project(meshclient VERSION [0-9]\+\.[0-9]\+\.[0-9]\+/project(meshclient VERSION ${NUMERIC}/" CMakeLists.txt

# Guard the ordering this script exists to enforce: the rewrite above must have landed, or
# every published asset would carry the previous release's number.
BAKED=$(sed -n 's/^project(meshclient VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
if [[ "${BAKED}" != "${NUMERIC}" ]]; then
    echo "CMakeLists.txt says ${BAKED:-<none>} but the release is ${VERSION} (${NUMERIC})." >&2
    exit 1
fi

# The Pak Store reads pak.json from the repo root and requires its `version` to match the
# release tag, so it is stamped here alongside the CMake rewrite above and committed by
# @semantic-release/git. Prereleases are deliberately skipped: the store wants a plain vX.Y.Z,
# and the beta and rc channels release 1.14.0-beta.1. Leaving those alone means pak.json only
# ever carries the last stable tag, on every branch, so a beta merging into main cannot put a
# prerelease version in front of the store even for the minute before the stable stamp lands.
if [[ "${VERSION}" == "${NUMERIC}" ]]; then
    sed -i -E "s/(\"version\"[[:space:]]*:[[:space:]]*)\"[^\"]*\"/\1\"v${VERSION}\"/" pak.json
    PAK_VERSION=$(sed -n -E 's/.*"version"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' pak.json)
    if [[ "${PAK_VERSION}" != "v${VERSION}" ]]; then
        echo "pak.json says ${PAK_VERSION:-<none>} but the release is v${VERSION}." >&2
        exit 1
    fi
else
    echo "Prerelease ${VERSION}: leaving pak.json on its last stable version."
fi

PLATFORM="${PLATFORM:-tg5040}"
CC_BIN="${CROSS_COMPILE:-}gcc"
CXX_BIN="${CROSS_COMPILE:-}g++"
# nanopb's generator runs on the host, so it must not be the cross toolchain's python3.
SYSTEM_PYTHON=$(which -a python3 | grep -v aarch64 | head -n1)

cmake -S . -B build/release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CC_BIN}" \
    -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_EXE_LINKER_FLAGS="-static ${DBUS_LDFLAGS:-}" \
    -DCMAKE_C_FLAGS="-Os ${DBUS_CFLAGS:-}" \
    -DPython3_EXECUTABLE="${SYSTEM_PYTHON}" \
    -DMESHCLIENT_VERSION_OVERRIDE="${VERSION}" \
    -DMESHCLIENT_RELEASE_BUILD=ON
cmake --build build/release

# Prove the version actually made it into the binary rather than trusting the build. This is
# the check that would have caught the old ordering, so it runs on every release.
#
# `grep -c`, not `grep -q`: under `set -o pipefail` a -q grep exits at the first match, strings
# takes SIGPIPE, and the pipeline reports 141 - so the check would fail on every build,
# including the good ones.
VERSION_HITS=$(strings build/release/meshclient | grep -cF -- "${VERSION}" || true)
if [[ "${VERSION_HITS}" -eq 0 ]]; then
    echo "build/release/meshclient does not contain the string ${VERSION}." >&2
    echo "The version rewrite must happen before this script; see .releaserc.json." >&2
    exit 1
fi

# The same two steps `make package` would run, called directly: going through the make target
# would re-enter cmake and rebuild, and the configure above is the one that matters.
export CROSS_COMPILE PLATFORM
./scripts/build_minui_helpers.sh
./scripts/package.sh release

# Two assets, for two different jobs. The pak zip is a fresh install: it holds the contents of
# the pak (launch.sh, pak.json and the minui helpers alongside the binary), unpacked into a
# Tools/<platform>/MeshClient.pak/ the installer creates - which is what the Pak Store does with
# it, and what installing by hand has to do too. The bare
# binary is what the in-app updater downloads - one file it can verify and rename into place,
# with no unzip on the device and no way for an interrupted download to leave a half-populated
# pak. Anything outside the binary (launch.sh, the helpers) still needs the zip.
ASSET_NAME="meshclient-${PLATFORM}-aarch64"
cp build/release/meshclient "dist/${ASSET_NAME}"
chmod +x "dist/${ASSET_NAME}"

( cd dist && sha256sum MeshClient.pak.zip > MeshClient.pak.zip.sha256 )
( cd dist && sha256sum "${ASSET_NAME}" > "${ASSET_NAME}.sha256" )

echo "Release ${VERSION} built:"
ls -l dist/
