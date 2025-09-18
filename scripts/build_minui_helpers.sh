#!/usr/bin/env bash
# Build and stage MinUI helper binaries from the NextUI submodule.
#
# Usage:
#   scripts/build_minui_helpers.sh [--platform tg5040] [--cross aarch64-linux-gnu-]
#
# Environment variables:
#   PLATFORM      Target platform (defaults to tg5040).
#   CROSS_COMPILE Toolchain prefix (defaults to aarch64-linux-gnu-).
#   PREFIX        Install prefix for NextUI libs (defaults to a local sysroot).
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
PREFIX="${PREFIX:-${WORKSPACE_DIR}/${PLATFORM}/sysroot}"
PREFIX_LOCAL="${PREFIX}"

OUTPUT_DIR="${REPO_ROOT}/Tools/tg5040/MeshClient.pak/bin/${PLATFORM}"
mkdir -p "${OUTPUT_DIR}"

info() { printf '[minui-build] %s\n' "$*"; }
warn() { printf '[minui-build][warn] %s\n' "$*" >&2; }

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
        cp "${SETTINGS_BIN}" "${OUTPUT_DIR}/minui-settings"
        chmod +x "${OUTPUT_DIR}/minui-settings"
        info "Copied settings helper to ${OUTPUT_DIR}/minui-settings"
    else
        warn "settings.elf not produced; skipping copy"
    fi

    found_lib=false
    for lib in "${WORKSPACE_DIR}/tg5040/libmsettings/libmsettings.so" \
               "${PREFIX}/lib/libmsettings.so"; do
        if [[ -f "${lib}" ]]; then
            cp "${lib}" "${OUTPUT_DIR}/libmsettings.so"
            info "Copied $(basename "${lib}") to ${OUTPUT_DIR}"
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
    for helper in list presenter; do
        src="${HELPER_SRC_DIR}/${helper}_helper.c"
        out="${OUTPUT_DIR}/minui-${helper}"
        if [[ -f "${src}" ]]; then
            info "Compiling minui-${helper} helper"
            if ! "${CROSS_COMPILE}gcc" -O2 -o "${out}" "${src}"; then
                warn "Failed to compile ${helper} helper with ${CROSS_COMPILE}gcc"
                rm -f "${out}"
            fi
        fi
    done
else
    warn "MinUI helper sources missing at ${HELPER_SRC_DIR}"
fi

info "MinUI helper staging complete."
