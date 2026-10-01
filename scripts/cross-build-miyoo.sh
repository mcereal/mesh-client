#!/usr/bin/env bash
# Build a static armhf meshclient for the Miyoo Mini / Mini Plus (Onion OS) and assemble
# dist/miyoomini/MeshClient/, the folder that goes in /mnt/SDCARD/App/.
# Runs inside the `cross-armhf` container (make docker-miyoo), and on an Ubuntu runner with
# gcc-arm-linux-gnueabihf installed - the CI job and the release both call it.
#
# A release passes MESHCLIENT_VERSION_OVERRIDE (and -DMESHCLIENT_RELEASE_BUILD=ON as an argument);
# then the version is checked into the binary. Besides the folder, it leaves the two release assets:
#   dist/meshclient-miyoomini-armhf   the bare binary the in-app updater replaces itself with
#   dist/MeshClient-miyoomini.zip     App/MeshClient/, unzipped at the card's root for a fresh install
#
# No Bluetooth: the device has none, and pkg-config is given nowhere to look, so neither the
# container's own libdbus nor the release runner's aarch64 one (on PKG_CONFIG_PATH) is found and
# inkwell builds its BLE refusal instead.
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
# MESHCLIENT_UPDATE_ASSET names this build's own release asset. Left unset, the updater's default
# is the Brick's aarch64 binary, which it would install over this one - and the next launch would
# fail on the format. No release carries an armhf asset yet, so a check reports "No usable release
# asset" and installs nothing until one does.
#
# PYTHON3 is for the release runner, whose PATH carries the aarch64 toolchain's own python3 first;
# nanopb's generator runs on the host. -g is split off below, as the Brick's release build does.
ASSET_NAME=meshclient-miyoomini-armhf
VERSION_ARGS=()
if [[ -n "${MESHCLIENT_VERSION_OVERRIDE:-}" ]]; then
    VERSION_ARGS+=("-DMESHCLIENT_VERSION_OVERRIDE=${MESHCLIENT_VERSION_OVERRIDE}")
fi
PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR= cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CROSS}gcc" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_C_FLAGS="-Os -g -fno-omit-frame-pointer -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard" \
    -DPython3_EXECUTABLE="${PYTHON3:-$(command -v python3)}" \
    -DINKCELL_WITH_SDL=OFF \
    -DMESHCLIENT_UPDATE_ASSET="${ASSET_NAME}" \
    "${VERSION_ARGS[@]}" \
    "$@"
cmake --build "$BUILD_DIR"

DESCRIPTION="$(file -b "$BUILD_DIR/meshclient")"
echo "$DESCRIPTION"
case "$DESCRIPTION" in
    *"ARM, EABI5"*"statically linked"*) ;;
    *) echo "not a static 32-bit ARM binary: $DESCRIPTION" >&2; exit 1 ;;
esac

# The name the updater asks for, and the version, have to be in the binary - not just on the
# configure line. `grep -c` rather than `-q`: see release-build.sh.
for want in "${ASSET_NAME}" ${MESHCLIENT_VERSION_OVERRIDE:+"${MESHCLIENT_VERSION_OVERRIDE}"}; do
    if [[ "$(strings "$BUILD_DIR/meshclient" | grep -cF -- "$want" || true)" -eq 0 ]]; then
        echo "$BUILD_DIR/meshclient does not contain the string $want" >&2
        exit 1
    fi
done

# Debug info out to dist/symbols for Sentry; the symbol table stays, as on the Brick.
mkdir -p dist/symbols
"${CROSS}objcopy" --only-keep-debug "$BUILD_DIR/meshclient" "dist/symbols/${ASSET_NAME}.debug"
"${CROSS}objcopy" --strip-debug "$BUILD_DIR/meshclient"

OUT=dist/miyoomini/MeshClient
rm -rf "$OUT"
mkdir -p "$OUT/licenses"
cp "$BUILD_DIR/meshclient" "$OUT/meshclient"
cp Tools/miyoomini/MeshClient/launch.sh Tools/miyoomini/MeshClient/config.json "$OUT/"
cp packaging/icon/meshclient.png "$OUT/icon.png"
cp licenses/*.txt "$OUT/licenses/"
chmod +x "$OUT/launch.sh" "$OUT/meshclient"

# The updater's asset is the same binary the folder carries. No env.sh in the zip: that is the
# user's file, and unzipping over an install must not replace it.
cp "$OUT/meshclient" "dist/${ASSET_NAME}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/App"
cp -R "$OUT" "$STAGE/App/"
rm -f dist/MeshClient-miyoomini.zip
( DIST="$PWD/dist" && cd "$STAGE" && zip -qr "$DIST/MeshClient-miyoomini.zip" App )
( cd dist && sha256sum "${ASSET_NAME}" > "${ASSET_NAME}.sha256" &&
    sha256sum MeshClient-miyoomini.zip > MeshClient-miyoomini.zip.sha256 )
echo "Built $OUT, dist/${ASSET_NAME} and dist/MeshClient-miyoomini.zip"
