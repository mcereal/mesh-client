#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE=${1:-debug}
shift || true

case "${BUILD_TYPE}" in
  debug|Debug)
    CMAKE_BUILD_TYPE=Debug
    BUILD_SUBDIR=debug
    ;;
  release|Release)
    CMAKE_BUILD_TYPE=Release
    BUILD_SUBDIR=release
    ;;
  relwithdebinfo|RelWithDebInfo)
    CMAKE_BUILD_TYPE=RelWithDebInfo
    BUILD_SUBDIR=relwithdebinfo
    ;;
  *)
    echo "Unknown build type: ${BUILD_TYPE}" >&2
    echo "Usage: $0 [debug|release|relwithdebinfo] [-- CMake args]" >&2
    exit 1
    ;;
esac

BUILD_DIR="build/${BUILD_SUBDIR}"
mkdir -p "${BUILD_DIR}"

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" "$@"
cmake --build "${BUILD_DIR}"
