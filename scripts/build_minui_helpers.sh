#!/usr/bin/env bash
# Build and stage MinUI helper binaries from the NextUI submodule.
#
# Usage:
#   scripts/build_minui_helpers.sh [--platform tg5040] [--cross aarch64-linux-gnu-]
#
# Environment variables:
#   PLATFORM      Target platform (defaults to tg5040).
#   CROSS_COMPILE Toolchain prefix (defaults to aarch64-linux-gnu-).
#   PREFIX        Install prefix for NextUI libs (defaults to $BUILD_ROOT/nextui-sysroot/$PLATFORM).
#   MESHCLIENT_ALLOW_HOST_HELPERS=1
#                 Stage helpers into the pak tree even when they are not built for the
#                 device's architecture. Off by default; see the note below.
#
# The binaries under Tools/tg5040/MeshClient.pak/bin/tg5040/ are committed aarch64
# artifacts. When no cross toolchain is present this script used to fall back to the host
# compiler and overwrite them in place, so a `make package` on an x86_64 Linux host left
# x86_64 binaries staged in the tracked tree - a `git add -A` after that would ship a pak
# that cannot run on the Brick. Every install into that tree is now checked against the
# platform's expected ELF machine and skipped (with a warning) on a mismatch.
#
# The script orchestrates the NextUI makefiles to build the Settings helper and
# copies the resulting binaries (and supporting shared objects) into the Mesh
# Client pak tree under `Tools/tg5040/MeshClient.pak/bin/tg5040/`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NEXTUI_ROOT="${REPO_ROOT}/third_party/nextui"
WORKSPACE_DIR="${NEXTUI_ROOT}/workspace"

if [[ ! -d "${NEXTUI_ROOT}" ]]; then
    echo "NextUI submodule not found at ${NEXTUI_ROOT}" >&2
    exit 1
fi

if [[ ! -d "${WORKSPACE_DIR}" ]]; then
    echo "NextUI workspace missing. Did you run 'git submodule update --init --recursive'?" >&2
    exit 1
fi

PLATFORM="${PLATFORM:-tg5040}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
# Keep the NextUI sysroot out of the submodule tree so it stays clean.
PREFIX="${PREFIX:-${REPO_ROOT}/${BUILD_ROOT:-build}/nextui-sysroot/${PLATFORM}}"
PREFIX_LOCAL="${PREFIX}"

OUTPUT_DIR="${REPO_ROOT}/Tools/tg5040/MeshClient.pak/bin/${PLATFORM}"
mkdir -p "${OUTPUT_DIR}"

info() { printf '[minui-build] %s\n' "$*"; }
warn() { printf '[minui-build][warn] %s\n' "$*" >&2; }

# ELF e_machine values (offset 0x12, 2 bytes little-endian). Read with od so this needs no
# file(1) or readelf, neither of which is guaranteed in the build containers.
declare -A EXPECTED_ELF_MACHINE=([tg5040]=183)  # 183 = EM_AARCH64
ELF_MACHINE_NAMES="3=x86 62=x86_64 183=aarch64 40=arm"

elf_machine() {
    od -An -tu1 -j18 -N2 "$1" 2>/dev/null | awk 'NF >= 2 {print $1 + $2 * 256}'
}

elf_machine_name() {
    local value="$1" pair
    for pair in ${ELF_MACHINE_NAMES}; do
        [[ "${pair%%=*}" == "${value}" ]] && { printf '%s' "${pair#*=}"; return; }
    done
    printf 'machine %s' "${value:-unknown}"
}

skipped_wrong_arch=0

# Copy into the tracked pak tree only when the binary really is for the device. On a
# mismatch the committed artifact is left exactly as it is.
install_device_binary() {
    local src="$1" dest="$2"
    local expected="${EXPECTED_ELF_MACHINE[${PLATFORM}]:-}"
    local actual
    actual="$(elf_machine "${src}")"

    if [[ -n "${expected}" && "${actual}" != "${expected}" ]]; then
        if [[ "${MESHCLIENT_ALLOW_HOST_HELPERS:-0}" == "1" ]]; then
            warn "$(basename "${dest}") is $(elf_machine_name "${actual}"), not $(elf_machine_name "${expected}") - staging anyway (MESHCLIENT_ALLOW_HOST_HELPERS=1)"
        else
            warn "Refusing to stage $(basename "${dest}"): built for $(elf_machine_name "${actual}"), ${PLATFORM} needs $(elf_machine_name "${expected}")"
            warn "  keeping the committed device binary; set CROSS_COMPILE to a ${PLATFORM} toolchain to rebuild it"
            skipped_wrong_arch=1
            return 1
        fi
    fi

    cp "${src}" "${dest}"
    chmod +x "${dest}"
    return 0
}

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    warn "${CROSS_COMPILE}gcc not found, falling back to host gcc"
    CROSS_COMPILE=""
fi

info "Using PLATFORM=${PLATFORM} CROSS_COMPILE=${CROSS_COMPILE} PREFIX=${PREFIX}"

# Build shared dependencies (libmsettings, etc.)
if [[ -n "${CROSS_COMPILE}" ]]; then
    info "Building NextUI platform prerequisites"
    make -C "${WORKSPACE_DIR}/tg5040/libmsettings" \
        CROSS_COMPILE="${CROSS_COMPILE}" \
        PREFIX="${PREFIX}" \
        PREFIX_LOCAL="${PREFIX_LOCAL}" \
        build || warn "libmsettings build failed (dependency may need toolchain setup)"

    info "Building settings helper"
    make -C "${WORKSPACE_DIR}/all/settings" \
        PLATFORM="${PLATFORM}" \
        CROSS_COMPILE="${CROSS_COMPILE}" \
        PREFIX="${PREFIX}" \
        PREFIX_LOCAL="${PREFIX_LOCAL}" || warn "Settings helper build failed"

    SETTINGS_BIN="${WORKSPACE_DIR}/all/settings/build/${PLATFORM}/settings.elf"
    if [[ -f "${SETTINGS_BIN}" ]]; then
        if install_device_binary "${SETTINGS_BIN}" "${OUTPUT_DIR}/minui-settings"; then
            info "Copied settings helper to ${OUTPUT_DIR}/minui-settings"
        fi
    else
        warn "settings.elf not produced; skipping copy"
    fi

    found_lib=false
    for lib in "${WORKSPACE_DIR}/tg5040/libmsettings/libmsettings.so" \
               "${PREFIX}/lib/libmsettings.so"; do
        if [[ -f "${lib}" ]]; then
            if install_device_binary "${lib}" "${OUTPUT_DIR}/libmsettings.so"; then
                info "Copied $(basename "${lib}") to ${OUTPUT_DIR}"
            fi
            found_lib=true
            break
        fi
    done
    if [[ "${found_lib}" == false ]]; then
        warn "libmsettings.so not found"
    fi
else
    warn "Skipping NextUI builds; CROSS_COMPILE not set"
fi

# Build meshclient-specific helpers (minimal CLI fallbacks compiled for device)
HELPER_SRC_DIR="${REPO_ROOT}/src/minui_helpers"
if [[ -d "${HELPER_SRC_DIR}" ]]; then
    # Compile into a scratch dir first so a host-arch build never lands in the pak tree.
    STAGING_DIR="${REPO_ROOT}/${BUILD_ROOT:-build}/minui-helpers/${PLATFORM}"
    mkdir -p "${STAGING_DIR}"

    for helper in list presenter; do
        src="${HELPER_SRC_DIR}/${helper}_helper.c"
        staged="${STAGING_DIR}/minui-${helper}"
        if [[ -f "${src}" ]]; then
            info "Compiling minui-${helper} helper"
            if "${CROSS_COMPILE}gcc" -O2 -static -o "${staged}" "${src}"; then
                install_device_binary "${staged}" "${OUTPUT_DIR}/minui-${helper}" || true
            else
                warn "Failed to compile ${helper} helper with ${CROSS_COMPILE}gcc"
                rm -f "${staged}"
            fi
        fi
    done
else
    warn "MinUI helper sources missing at ${HELPER_SRC_DIR}"
fi

if [[ "${skipped_wrong_arch}" -eq 1 ]]; then
    info "MinUI helper staging complete (committed device binaries preserved)."
else
    info "MinUI helper staging complete."
fi
