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

# -fno-omit-frame-pointer is for src/utils/crash.c and costs about 20 KB of text.
# At -Os both GCC and Clang drop the frame pointer, and without it the crash handler's stack
# walk has no chain to follow - so the one build that actually runs on a Brick would be the one
# build whose reports have no backtrace in them, while every debug and test build produced a
# full one. The PC alone still resolves to a line, so this buys the call chain rather than the
# crash site; at well under one percent of the binary that is the right way round.
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_EXE_LINKER_FLAGS="-static -L${CROSS_DBUS_PREFIX}/lib" \
    -DCMAKE_C_FLAGS="-Os -fno-omit-frame-pointer ${CROSS_CFLAGS:-} ${DBUS_CFLAGS}" \
    -DPython3_EXECUTABLE="$(command -v python3)" \
    "$@"
cmake --build "$BUILD_DIR"

# The Brick runs one shape of binary: aarch64, and static, because the device has no libdbus
# and no glibc to find one with. `file` was printing that and nothing was reading it, so a
# host-toolchain configure would have produced an x86 binary and packaged it without complaint.
DESCRIPTION="$(file -b "$BUILD_DIR/meshclient")"
echo "$DESCRIPTION"
case "$DESCRIPTION" in
    *"ARM aarch64"*) ;;
    *) echo "not an aarch64 binary: $DESCRIPTION" >&2; exit 1 ;;
esac
case "$DESCRIPTION" in
    *"statically linked"*) ;;
    *) echo "not statically linked: $DESCRIPTION" >&2; exit 1 ;;
esac

export PLATFORM
BUILD_ROOT="$BUILD_ROOT" ./scripts/package.sh release
# Both assets the release publishes, so `make docker-pak` produces locally what CI uploads:
# the pak zip for a fresh install, and the bare binary the in-app updater downloads.
ASSET_NAME="meshclient-${PLATFORM:-tg5040}-aarch64"
cp "$BUILD_DIR/meshclient" "dist/${ASSET_NAME}"
chmod +x "dist/${ASSET_NAME}"
(cd dist && sha256sum MeshClient.pak.zip > MeshClient.pak.zip.sha256 && cat MeshClient.pak.zip.sha256)
(cd dist && sha256sum "${ASSET_NAME}" > "${ASSET_NAME}.sha256" && cat "${ASSET_NAME}.sha256")
