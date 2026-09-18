#!/usr/bin/env bash
#
# The desktop and server build: one static Linux binary, for the half of this client that is not
# a handheld UI.
#
# `src/core` never includes `src/ui`, so the session, the transports, the admin queue and the
# firmware flasher already run with no framebuffer under them - that is what `--status`,
# `--send-text` and `--install-firmware` are. What was missing was a way to *get* that as
# something a person can run on a laptop or a server: the only published binary was
# meshclient-tg5040-aarch64, which is the in-app updater's payload and reads as a handheld file.
#
# Static against musl for the same reason the device build is: the result depends on a kernel
# and nothing else, so one file downloaded from a release runs on whatever the target's libc and
# libdbus happen to be. Build for the machine it is run on - this is not a cross build, and
# there is no second architecture to pick.
#
#   ./scripts/linux-cli-build.sh              -> dist/meshclient-linux-<arch> (+ .sha256)
#
# Needs musl-gcc (musl-tools), meson, ninja and wget. The static libdbus it links is built once
# into $MUSL_DBUS_PREFIX and reused, because it is ~2 minutes and never changes between runs.
set -euo pipefail

cd "$(dirname "$0")/.."

DBUS_VERSION=1.16.2   # the version docker/setup-cross.sh pins for the device build
MUSL_DBUS_PREFIX="${MUSL_DBUS_PREFIX:-$(pwd)/build/musl-dbus}"
BUILD_DIR="${BUILD_ROOT:-build}/linux-cli"
OUT_DIR="${OUT_DIR:-dist}"

ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64|aarch64) ;;
    *)
        echo "No musl CLI build for ${ARCH}; x86_64 and aarch64 are what releases carry." >&2
        exit 1
        ;;
esac
ASSET_NAME="meshclient-linux-${ARCH}"

if ! command -v musl-gcc >/dev/null 2>&1; then
    echo "musl-gcc not found; install musl-tools (apt-get install musl-tools)." >&2
    exit 1
fi

# musl-dev ships no kernel headers, and this client reads linux/input.h, linux/fb.h and
# linux/serial.h. docker/setup-cross.sh solves that by symlinking them into the musl include
# root, which is a write to /usr/include and therefore needs root; -idirafter gets the same
# headers without touching the system, because it is searched *after* musl's own. Anything musl
# defines still comes from musl - only the uapi headers, which musl does not ship and which are
# libc-agnostic, fall through to the system copy. The multiarch directory is the second entry
# because Debian files <asm/...> there rather than in /usr/include.
MULTIARCH="$(gcc -print-multiarch 2>/dev/null || true)"
KERNEL_HEADERS="-idirafter /usr/include"
if [[ -n "${MULTIARCH}" && -d "/usr/include/${MULTIARCH}" ]]; then
    KERNEL_HEADERS="${KERNEL_HEADERS} -idirafter /usr/include/${MULTIARCH}"
fi

# ---------------------------------------------------------------------------
# Static libdbus, once.
# ---------------------------------------------------------------------------
if [[ ! -f "${MUSL_DBUS_PREFIX}/lib/libdbus-1.a" ]]; then
    for tool in meson ninja wget; do
        command -v "${tool}" >/dev/null 2>&1 || {
            echo "building libdbus needs ${tool}." >&2
            exit 1
        }
    done

    SRC_DIR="${MUSL_DBUS_PREFIX}/src"
    mkdir -p "${SRC_DIR}"
    (
        cd "${SRC_DIR}"
        wget -q "https://dbus.freedesktop.org/releases/dbus/dbus-${DBUS_VERSION}.tar.xz"
        tar -xf "dbus-${DBUS_VERSION}.tar.xz"
        cd "dbus-${DBUS_VERSION}"

        # libdbus only: message_bus=false skips the daemon, so no XML parser is needed.
        # dbus >= 1.16 is meson-only.
        #
        # -Dsystem_socket is the option this build cannot leave at its default. libdbus bakes
        # the system bus address in at *build* time, derived from --prefix, so a libdbus built
        # under a scratch prefix sends dbus_bus_get(DBUS_BUS_SYSTEM) looking for a socket under
        # that scratch path - which exists on no machine, and which is a BLE transport that
        # reports "Failed to connect to system bus" on a host where BlueZ is running fine.
        # Tools/tg5040/MeshClient.pak/launch.sh covers this on the device by exporting
        # DBUS_SYSTEM_BUS_ADDRESS, and a bare binary downloaded from a release has no launch.sh
        # to do that for it. So name the path every distribution actually uses; the environment
        # variable still overrides it where something needs it to.
        CC=musl-gcc meson setup build \
            --prefix="${MUSL_DBUS_PREFIX}" --libdir=lib --buildtype=release \
            -Dsystem_socket=/run/dbus/system_bus_socket \
            -Ddefault_library=static -Dmessage_bus=false -Dtools=false \
            -Dmodular_tests=disabled -Dintrusive_tests=false -Dinstalled_tests=false \
            -Dxml_docs=disabled -Ddoxygen_docs=disabled -Dducktype_docs=disabled \
            -Dqt_help=disabled -Dselinux=disabled -Dapparmor=disabled -Dsystemd=disabled \
            -Dlibaudit=disabled -Dx11_autolaunch=disabled
        ninja -C build
        ninja -C build install
    ) >"${MUSL_DBUS_PREFIX}/setup.log" 2>&1 || {
        echo "static libdbus failed to build:" >&2
        cat "${MUSL_DBUS_PREFIX}/setup.log" >&2
        exit 1
    }
    rm -rf "${SRC_DIR}" "${MUSL_DBUS_PREFIX}/setup.log"
fi

# ---------------------------------------------------------------------------
# The client.
# ---------------------------------------------------------------------------
# shellcheck source=scripts/cmake-tree.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake-tree.sh"
mesh_reset_stale_tree "${BUILD_DIR}"

# nanopb's generator runs on the host. The filter is for the release workflow rather than for a
# developer's shell: that job puts the aarch64 cross toolchain on PATH before calling
# scripts/release-build.sh, which calls this, and Bootlin's toolchain ships a python3 of its own
# that shadows the system one. scripts/release-build.sh does the same, for the same reason.
SYSTEM_PYTHON="$(which -a python3 | grep -v aarch64 | head -n1)"

CMAKE_ARGS=(
    -S . -B "${BUILD_DIR}" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=musl-gcc
    -DCMAKE_EXE_LINKER_FLAGS="-static -L${MUSL_DBUS_PREFIX}/lib"
    -DCMAKE_C_FLAGS="-Os -fno-omit-frame-pointer ${KERNEL_HEADERS} -I${MUSL_DBUS_PREFIX}/include/dbus-1.0 -I${MUSL_DBUS_PREFIX}/lib/dbus-1.0/include"
    -DPython3_EXECUTABLE="${SYSTEM_PYTHON}"
    -DBUILD_TESTING=OFF
    -DMESHCLIENT_RELEASE_BUILD=ON
    # The asset this binary is published as, and therefore the one it may replace itself with.
    # Without it the updater's default applies, which is the handheld's aarch64 binary: a
    # desktop client would check for an update, find one, and install a binary for another
    # architecture over itself. RELEASE_BUILD=ON is what arms that, so the two belong together.
    -DMESHCLIENT_UPDATE_ASSET="${ASSET_NAME}"
)
if [[ -n "${MESHCLIENT_VERSION_OVERRIDE:-}" ]]; then
    CMAKE_ARGS+=(-DMESHCLIENT_VERSION_OVERRIDE="${MESHCLIENT_VERSION_OVERRIDE}")
fi

PKG_CONFIG_PATH="${MUSL_DBUS_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
    cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target meshclient

# ---------------------------------------------------------------------------
# Assert what the download promises, rather than trusting the configure above.
# ---------------------------------------------------------------------------
BINARY="${BUILD_DIR}/meshclient"

# `grep -c` rather than `grep -q` on each of these, for the reason scripts/release-build.sh
# gives at its own version check: under `set -o pipefail` a -q grep exits at the first match,
# the process feeding it takes SIGPIPE, and the pipeline reports 141 - so the check fails on
# every build, the good ones included.

# A dynamic binary here is the failure that matters: it builds, it runs on the machine that
# built it, and it is the download that does not run anywhere else.
if [[ "$(file "${BINARY}" | grep -c "statically linked" || true)" -eq 0 ]]; then
    echo "${BINARY} is not static:" >&2
    file "${BINARY}" >&2
    exit 1
fi

# BLE is the transport this build could silently lose: with no dbus-1.pc on PKG_CONFIG_PATH
# CMakeLists.txt compiles MESH_HAVE_DBUS out and carries on, so a libdbus step that quietly did
# not take would publish a working binary that simply has no Bluetooth in it.
if [[ "$(strings "${BINARY}" | grep -c "org\.bluez" || true)" -eq 0 ]]; then
    echo "${BINARY} has no BlueZ in it; the static libdbus step did not take." >&2
    exit 1
fi

# And that the updater points at this build's own asset rather than the handheld's. A -D that
# does not reach the compile is silent - the #ifndef in updater.c simply keeps its default - and
# what it costs is a desktop client installing an aarch64 binary over itself.
if [[ "$(strings "${BINARY}" | grep -cF -- "${ASSET_NAME}" || true)" -eq 0 ]]; then
    echo "${BINARY} does not name ${ASSET_NAME} as its update asset." >&2
    exit 1
fi

"${BINARY}" --version

mkdir -p "${OUT_DIR}"
cp "${BINARY}" "${OUT_DIR}/${ASSET_NAME}"
chmod +x "${OUT_DIR}/${ASSET_NAME}"
( cd "${OUT_DIR}" && sha256sum "${ASSET_NAME}" > "${ASSET_NAME}.sha256" )

echo "built ${OUT_DIR}/${ASSET_NAME}"
ls -l "${OUT_DIR}/${ASSET_NAME}"
