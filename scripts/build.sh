#!/usr/bin/env bash
set -euo pipefail

# `cmake --preset` reads CMakePresets.json from the working directory, so stand in the repo
# root regardless of where this was called from. BUILD_ROOT stays relative to the same place it
# always was.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BUILD_TYPE=${1:-debug}
shift || true

# `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` is the documented form, and the
# separator is for the reader rather than for cmake, which rejects a bare `--` outright.
if [[ "${1:-}" == "--" ]]; then
    shift
fi

case "${BUILD_TYPE}" in
  debug|Debug)                   PRESET=debug ;;
  release|Release)               PRESET=release ;;
  relwithdebinfo|RelWithDebInfo) PRESET=relwithdebinfo ;;
  *)
    echo "Unknown build type: ${BUILD_TYPE}" >&2
    echo "Usage: $0 [debug|release|relwithdebinfo] [-- CMake args]" >&2
    exit 1
    ;;
esac

# The generator, the build type and CMAKE_EXPORT_COMPILE_COMMANDS all live in
# CMakePresets.json, so `cmake --preset debug` and `make debug` configure the same tree the
# same way - and an editor that reads presets agrees with both without being told anything.
#
# -B is still passed because the preset's binaryDir cannot be one: BUILD_ROOT is how the
# container build (build/linux) and the sanitizer build (build/san) keep their own CMake
# caches, and a preset's paths are fixed at the point it is written. An explicit -B overrides
# binaryDir and changes nothing else about the preset.
BUILD_ROOT="${BUILD_ROOT:-build}"
BUILD_DIR="${BUILD_ROOT}/${PRESET}"

# shellcheck source=scripts/cmake-tree.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake-tree.sh"
mesh_reset_stale_tree "${BUILD_DIR}"

mkdir -p "${BUILD_DIR}"

# On a Mac the generators' Python is the one scripts/setup-macos.sh put in .venv: Homebrew's own
# has none of their modules and will not take them outside a venv. Named explicitly because
# CMake would otherwise find Homebrew's first. Passed before "$@", so a caller can still override.
EXTRA_ARGS=()
if [[ "$(uname -s)" == "Darwin" && -x .venv/bin/python3 ]]; then
    EXTRA_ARGS+=("-DPython3_EXECUTABLE=${PWD}/.venv/bin/python3")
fi

cmake --preset "${PRESET}" -B "${BUILD_DIR}" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" "$@"
cmake --build "${BUILD_DIR}"
