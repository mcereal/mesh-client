#!/usr/bin/env bash
# Run a command inside the mesh-client build container with the repo bind-mounted at /src.
#
# Usage:
#   scripts/docker.sh [--cross] [--rebuild] [cmd ...]     (no cmd = interactive shell)
#
# Examples:
#   scripts/docker.sh make debug test        # host-arch Debug build + unit tests
#   scripts/docker.sh --cross scripts/cross-build.sh   # static aarch64 MeshClient.pak into dist/
#   scripts/docker.sh                        # bash inside the dev container
#
# Images are built on first use from docker/Dockerfile and reused afterwards.
# Container builds use BUILD_ROOT=build/linux so a host CMake cache in build/ is never touched.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET=dev
REBUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cross) TARGET=cross; shift ;;
        --dev) TARGET=dev; shift ;;
        --rebuild) REBUILD=1; shift ;;
        --) shift; break ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) break ;;
    esac
done

IMAGE="meshclient-${TARGET}"

if [[ $REBUILD -eq 1 ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[docker.sh] building image $IMAGE (target=$TARGET)" >&2
    docker build --target "$TARGET" -t "$IMAGE" -f "$REPO_ROOT/docker/Dockerfile" "$REPO_ROOT"
fi

TTY_FLAGS=()
if [[ -t 0 && -t 1 ]]; then
    TTY_FLAGS=(-it)
fi

# Pass through env vars that the build/package scripts honour.
ENV_FLAGS=()
for var in MESHCLIENT_UI_BACKEND MESHCLIENT_RUN_MODE MESHCLIENT_DISABLE_BLE MESHCLIENT_PREFERRED_BLE_DEVICE \
           MESHCLIENT_MINUI_SELECTION CMAKE_ARGS PLATFORM; do
    if [[ -n "${!var:-}" ]]; then
        ENV_FLAGS+=(-e "$var=${!var}")
    fi
done

if [[ $# -eq 0 ]]; then
    set -- bash
fi

# ${arr[@]+"${arr[@]}"} keeps `set -u` happy on bash 3.2 (macOS) when the array is empty.
exec docker run --rm ${TTY_FLAGS[@]+"${TTY_FLAGS[@]}"} \
    -v "$REPO_ROOT":/src -w /src \
    -e BUILD_ROOT=build/linux \
    ${ENV_FLAGS[@]+"${ENV_FLAGS[@]}"} \
    "$IMAGE" "$@"
