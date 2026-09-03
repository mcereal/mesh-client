#!/usr/bin/env bash
# Build a static aarch64 meshclient and assemble dist/MeshClient.pak.zip.
# Runs inside the `cross` container (scripts/docker.sh --cross scripts/cross-build.sh, or make docker-pak).
# Mirrors the Release build in .github/workflows/semantic-release.yml.
set -euo pipefail

if [[ ! -f /opt/cross/env.sh ]]; then
    echo "cross toolchain not found; run this inside the cross container (make docker-pak)" >&2
    exit 1
fi
# shellcheck disable=SC1091
source /opt/cross/env.sh

BUILD_ROOT="${BUILD_ROOT:-build}"
BUILD_DIR="${BUILD_ROOT}/release"
DBUS_CFLAGS="-I${CROSS_DBUS_PREFIX}/include/dbus-1.0 -I${CROSS_DBUS_PREFIX}/lib/dbus-1.0/include"

cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_EXE_LINKER_FLAGS="-static -L${CROSS_DBUS_PREFIX}/lib" \
    -DCMAKE_C_FLAGS="-Os ${DBUS_CFLAGS}" \
    -DPython3_EXECUTABLE="$(command -v python3)" \
    "$@"
cmake --build "$BUILD_DIR"

file "$BUILD_DIR/meshclient"

export CROSS_COMPILE PLATFORM
BUILD_ROOT="$BUILD_ROOT" ./scripts/build_minui_helpers.sh
BUILD_ROOT="$BUILD_ROOT" ./scripts/package.sh release
(cd dist && sha256sum MeshClient.pak.zip > MeshClient.pak.zip.sha256 && cat MeshClient.pak.zip.sha256)
