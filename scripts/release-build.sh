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

# Guard the ordering this script exists to enforce: if CMakeLists.txt does not already carry the
# version being released, the sed in prepareCmd did not run and every asset would be mislabelled.
BAKED=$(sed -n 's/^project(meshclient VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
if [[ "${BAKED}" != "${VERSION}" ]]; then
    echo "CMakeLists.txt says ${BAKED:-<none>} but the release is ${VERSION}." >&2
    echo "The version rewrite must happen before this script; see .releaserc.json." >&2
    exit 1
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
    -DPython3_EXECUTABLE="${SYSTEM_PYTHON}"
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

# Two assets, for two different jobs. The pak zip is a fresh install: unzip it into
# Tools/<platform>/ and you have launch.sh and the minui helpers alongside the binary. The bare
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
