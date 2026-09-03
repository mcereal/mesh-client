#!/usr/bin/env bash
# Provision a native Linux host (or a Claude Code cloud session) for building mesh-client.
#
# The core is Linux-only (epoll/timerfd/eventfd), so on macOS everything goes through
# scripts/docker.sh. On a Linux host the same build works natively once these are present:
#
#   - git submodules      nanopb, meshtastic/protobufs, NextUI (CMake FATAL_ERRORs without them)
#   - libdbus-1-dev       sets MESH_HAVE_DBUS; without it the BLE transport compiles out
#   - python protobuf     needed by nanopb_generator to regenerate the .pb.c/.pb.h sources
#
# Usage:
#   scripts/setup-linux.sh              # install everything that is missing
#   scripts/setup-linux.sh --check      # report what is missing, install nothing
#
# Safe to re-run: every step is skipped when already satisfied.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

MISSING=0
say()  { printf '  %s\n' "$*"; }
need() { MISSING=1; printf '  MISSING: %s\n' "$*"; }

# Package installs need root. Prefer sudo when we are not already root; if neither is
# available we report what is missing instead of failing, so --check stays useful for
# unprivileged users.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    fi
fi

run_privileged() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif [ -n "$SUDO" ]; then
        $SUDO "$@"
    else
        say "cannot run '$*' (no root and no sudo)"
        return 1
    fi
}

echo "==> Toolchain"
for tool in cmake pkg-config python3; do
    if command -v "$tool" >/dev/null 2>&1; then
        say "$tool: $(command -v "$tool")"
    else
        need "$tool"
    fi
done
if command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1; then
    say "C compiler: present"
else
    need "a C compiler (build-essential or clang)"
fi

echo "==> Git submodules"
# An uninitialised submodule shows a leading '-' in `git submodule status`.
if git submodule status --recursive 2>/dev/null | grep -q '^-'; then
    if [ "$CHECK_ONLY" -eq 1 ]; then
        need "git submodules are not initialised"
    else
        say "initialising submodules"
        git submodule update --init --recursive
    fi
else
    say "already initialised"
fi

echo "==> libdbus-1-dev"
if pkg-config --exists dbus-1 2>/dev/null; then
    say "dbus-1 $(pkg-config --modversion dbus-1)"
elif [ "$CHECK_ONLY" -eq 1 ]; then
    need "libdbus-1-dev (BLE transport compiles out without it)"
elif command -v apt-get >/dev/null 2>&1; then
    say "installing via apt-get"
    export DEBIAN_FRONTEND=noninteractive
    run_privileged apt-get update -qq
    run_privileged apt-get install -y -qq --no-install-recommends \
        build-essential clang clang-format cmake ninja-build pkg-config \
        libdbus-1-dev protobuf-compiler python3 python3-pip git zip
else
    need "libdbus-1-dev (no apt-get here; install your distro's dbus development package)"
fi

echo "==> Python protobuf (nanopb generator)"
if python3 -c 'import google.protobuf' >/dev/null 2>&1; then
    say "protobuf $(python3 -c 'import google.protobuf; print(google.protobuf.__version__)')"
elif [ "$CHECK_ONLY" -eq 1 ]; then
    need "python protobuf/grpcio-tools (nanopb codegen will fail)"
else
    say "installing via pip"
    # --break-system-packages: Debian/Ubuntu mark the system interpreter as externally
    # managed (PEP 668). This mirrors what docker/Dockerfile does for the dev image.
    python3 -m pip install --no-cache-dir --break-system-packages -q protobuf grpcio-tools \
        || python3 -m pip install --no-cache-dir -q protobuf grpcio-tools
fi

echo
if [ "$CHECK_ONLY" -eq 1 ] && [ "$MISSING" -eq 1 ]; then
    echo "Some prerequisites are missing. Run scripts/setup-linux.sh to install them."
    exit 1
fi

echo "Ready. Next: make test   (Debug build + unit tests)"
