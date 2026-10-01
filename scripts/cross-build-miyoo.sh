#!/usr/bin/env bash
# Build a static armhf meshclient for the Miyoo Mini / Mini Plus (Onion OS) and assemble
# dist/miyoomini/MeshClient/, the folder that goes in /mnt/SDCARD/App/.
# Runs inside the `cross-armhf` container (make docker-miyoo).
#
# No Bluetooth: the device has none, and PKG_CONFIG_LIBDIR is emptied so the container's own
# (arm64) libdbus headers are not found and inkwell builds its BLE refusal instead.
set -euo pipefail

CROSS=arm-linux-gnueabihf-
if ! command -v "${CROSS}gcc" >/dev/null 2>&1; then
    echo "${CROSS}gcc not found; run this inside the cross-armhf container (make docker-miyoo)" >&2
    exit 1
fi

BUILD_ROOT="${BUILD_ROOT:-build}"
BUILD_DIR="${BUILD_ROOT}/miyoomini"
# shellcheck source=scripts/cmake-tree.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake-tree.sh"
mesh_reset_stale_tree "$BUILD_DIR"

# The SSD202D is a dual Cortex-A7 with NEON.
PKG_CONFIG_LIBDIR= cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CROSS}gcc" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_C_FLAGS="-Os -fno-omit-frame-pointer -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard" \
    -DPython3_EXECUTABLE="$(command -v python3)" \
    -DINKCELL_WITH_SDL=OFF \
    "$@"
cmake --build "$BUILD_DIR"

DESCRIPTION="$(file -b "$BUILD_DIR/meshclient")"
echo "$DESCRIPTION"
case "$DESCRIPTION" in
    *"ARM, EABI5"*"statically linked"*) ;;
    *) echo "not a static 32-bit ARM binary: $DESCRIPTION" >&2; exit 1 ;;
esac

OUT=dist/miyoomini/MeshClient
rm -rf "$OUT"
mkdir -p "$OUT/licenses"
cp "$BUILD_DIR/meshclient" "$OUT/meshclient"
"${CROSS}strip" "$OUT/meshclient"
cp Tools/miyoomini/MeshClient/launch.sh Tools/miyoomini/MeshClient/config.json "$OUT/"
cp packaging/icon/meshclient.png "$OUT/icon.png"
cp licenses/*.txt "$OUT/licenses/"
chmod +x "$OUT/launch.sh" "$OUT/meshclient"
echo "Built $OUT"
