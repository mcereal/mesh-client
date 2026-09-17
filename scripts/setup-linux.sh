#!/usr/bin/env bash
# Provision a native Linux host (or a Claude Code cloud session) for building mesh-client.
#
# The core is Linux-only (epoll/timerfd/eventfd), so on macOS everything goes through
# scripts/docker.sh. On a Linux host the same build works natively once these are present:
#
#   - cmake >= 3.21       CMakePresets.json is version 3; below that nothing configures
#   - ninja               the generator every build tree here is configured with
#   - git submodules      nanopb, meshtastic/protobufs (CMake FATAL_ERRORs without them)
#   - libdbus-1-dev       sets MESH_HAVE_DBUS; without it the BLE transport compiles out
#   - python protobuf     needed by nanopb_generator to regenerate the .pb.c/.pb.h sources
#   - libclang-rt-18-dev  clang's sanitizer runtimes; only the ASan/UBSan builds need them
#
# Usage:
#   scripts/setup-linux.sh              # install everything that is missing
#   scripts/setup-linux.sh --check      # report what is missing, install nothing
#
# Safe to re-run: every step is skipped when already satisfied. Exits non-zero if anything is
# still missing when it finishes, so `make setup` fails loudly rather than claiming success.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

say() { printf '  %s\n' "$*"; }

# Package installs need root. Prefer sudo when we are not already root; if neither is
# available we report what is missing instead of failing, so --check stays useful for
# unprivileged users.
run_privileged() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        say "cannot run '$*' (no root and no sudo)"
        return 1
    fi
}

# apt package list, mirroring the dev stage of docker/Dockerfile.
#
# libclang-rt-18-dev is the one that is easy to leave out, and this list had: Ubuntu's clang
# package does not pull the sanitizer runtimes in, so without it the documented way to reproduce
# what CI found - `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` - fails at the link
# step looking for libclang_rt.asan-x86_64.a, on a host `make setup` reported as ready. The
# container and CI both install it; a native Linux host is the only place that did not.
APT_PACKAGES="build-essential clang clang-format libclang-rt-18-dev cmake ninja-build pkg-config
              libdbus-1-dev dbus-daemon protobuf-compiler python3 python3-pip git zip"

apt_install_done=0
# Install the whole toolchain in one shot the first time anything turns out to be missing.
# Doing this per-component would re-run apt-get update repeatedly, and gating it on any single
# component (D-Bus, say) would leave a host that has D-Bus but no compiler silently broken.
ensure_apt_packages() {
    [ "$apt_install_done" -eq 1 ] && return 0
    apt_install_done=1

    if ! command -v apt-get >/dev/null 2>&1; then
        say "no apt-get here; install the equivalents for your distro"
        return 1
    fi

    say "installing build prerequisites via apt-get"
    export DEBIAN_FRONTEND=noninteractive
    run_privileged apt-get update -qq
    # shellcheck disable=SC2086  # word splitting is intended for the package list
    run_privileged apt-get install -y -qq --no-install-recommends $APT_PACKAGES
}

have_compiler() {
    command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1
}

# Present is not the same as usable, and these two are why this script checks more than
# `command -v`. Each one is a host that builds nothing while every check passes.
CMAKE_REQUIRED_MAJOR=3
CMAKE_REQUIRED_MINOR=21

cmake_version() {
    command -v cmake >/dev/null 2>&1 || return 1
    cmake --version 2>/dev/null | sed -n '1s/^cmake version \([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p'
}

# CMakePresets.json is version 3, which arrived in CMake 3.21. Every documented build route
# goes through it, so an older cmake stops before configuring anything - and `command -v cmake`
# says yes the whole time.
cmake_too_old() {
    local version major minor
    version="$(cmake_version)" || return 1
    [ -n "$version" ] || return 1
    major="${version%%.*}"
    minor="${version#*.}"
    if [ "$major" -lt "$CMAKE_REQUIRED_MAJOR" ]; then
        return 0
    fi
    if [ "$major" -eq "$CMAKE_REQUIRED_MAJOR" ] && [ "$minor" -lt "$CMAKE_REQUIRED_MINOR" ]; then
        return 0
    fi
    return 1
}

# Ubuntu's clang package does not pull the sanitizer runtimes in, so `command -v clang` says
# yes and `-fsanitize=address` fails at the link step. Ask clang where its runtime directory is
# rather than guessing: the file is libclang_rt.asan-x86_64.a here and -aarch64.a elsewhere.
# No clang, nothing to be missing.
sanitizer_runtime_missing() {
    local dir lib
    command -v clang >/dev/null 2>&1 || return 1
    dir="$(clang -print-runtime-dir 2>/dev/null)" || return 1
    [ -n "$dir" ] || return 1
    for lib in "$dir"/libclang_rt.asan*; do
        if [ -e "$lib" ]; then
            return 1
        fi
    done
    return 0
}

toolchain_missing() {
    for tool in cmake ninja pkg-config python3; do
        command -v "$tool" >/dev/null 2>&1 || return 0
    done
    have_compiler || return 0
    cmake_too_old && return 0
    sanitizer_runtime_missing && return 0
    return 1
}

echo "==> Toolchain"
if toolchain_missing; then
    for tool in cmake ninja pkg-config python3; do
        command -v "$tool" >/dev/null 2>&1 || say "missing: $tool"
    done
    have_compiler || say "missing: a C compiler"
    if cmake_too_old; then
        say "too old: cmake $(cmake_version), and CMakePresets.json needs ${CMAKE_REQUIRED_MAJOR}.${CMAKE_REQUIRED_MINOR}"
    fi
    if sanitizer_runtime_missing; then
        say "missing: libclang-rt-18-dev (clang's sanitizer runtimes)"
    fi
    if [ "$CHECK_ONLY" -eq 0 ]; then
        ensure_apt_packages || true
    fi
else
    for tool in cmake ninja pkg-config python3; do
        say "$tool: $(command -v "$tool")"
    done
    say "C compiler: present"
fi

echo "==> Git submodules"
# `git submodule status` marks an uninitialised submodule with a leading '-' and an
# initialised-but-stale one (the checkout does not match the recorded gitlink, e.g. after
# pulling a revision that bumped nanopb) with '+'. Both need `git submodule update`.
if git submodule status --recursive 2>/dev/null | grep '^[-+]' >/dev/null; then
    if [ "$CHECK_ONLY" -eq 1 ]; then
        say "missing: submodules are not initialised or are out of sync"
    else
        say "syncing submodules to the recorded commits"
        git submodule update --init --recursive
    fi
else
    say "in sync"
fi

echo "==> libdbus-1-dev"
if pkg-config --exists dbus-1 2>/dev/null; then
    say "dbus-1 $(pkg-config --modversion dbus-1)"
elif [ "$CHECK_ONLY" -eq 1 ]; then
    say "missing: libdbus-1-dev (BLE transport compiles out without it)"
else
    ensure_apt_packages || true
fi

echo "==> Python protobuf (nanopb generator)"
if python3 -c 'import google.protobuf' >/dev/null 2>&1; then
    say "protobuf $(python3 -c 'import google.protobuf; print(google.protobuf.__version__)')"
elif [ "$CHECK_ONLY" -eq 1 ]; then
    say "missing: python protobuf/grpcio-tools (nanopb codegen will fail)"
else
    say "installing via pip"
    # --break-system-packages: Debian/Ubuntu mark the system interpreter as externally
    # managed (PEP 668). This mirrors what docker/Dockerfile does for the dev image.
    python3 -m pip install --no-cache-dir --break-system-packages -q protobuf grpcio-tools \
        || python3 -m pip install --no-cache-dir -q protobuf grpcio-tools \
        || say "pip install failed"
fi

# Mbed TLS 4.x generates error.c, the SSL debug helpers and the PSA driver wrappers during the
# build rather than committing them, so these are needed to configure at all - not just to
# regenerate something. A 3.x checkout did not need them.
echo "==> Python jinja2 + jsonschema (Mbed TLS generated sources)"
if python3 -c 'import jinja2, jsonschema' >/dev/null 2>&1; then
    say "jinja2 $(python3 -c 'import jinja2; print(jinja2.__version__)')"
elif [ "$CHECK_ONLY" -eq 1 ]; then
    say "missing: python jinja2/jsonschema (the TLS build will fail to generate sources)"
else
    say "installing via pip"
    python3 -m pip install --no-cache-dir --break-system-packages -q jinja2 jsonschema \
        || python3 -m pip install --no-cache-dir -q jinja2 jsonschema \
        || say "pip install failed"
fi

# Re-check from scratch so the verdict reflects reality after any installs above, rather than
# the state we started with.
echo
REMAINING=0
report_missing() { REMAINING=1; printf '  STILL MISSING: %s\n' "$*"; }

# Reported but not fatal: an ordinary build and the whole test suite are fine without the
# sanitizer runtimes, and only `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` is not.
# Turning a host that cannot install them - anything not apt-based - away from building at all
# would be the wrong trade, but going on to print "Ready" was how this went unnoticed.
report_note() { printf '  NOTE: %s\n' "$*"; }

for tool in cmake ninja pkg-config python3; do
    command -v "$tool" >/dev/null 2>&1 || report_missing "$tool"
done
have_compiler || report_missing "a C compiler"
if cmake_too_old; then
    report_missing "cmake ${CMAKE_REQUIRED_MAJOR}.${CMAKE_REQUIRED_MINOR} or newer (found $(cmake_version); CMakePresets.json is version 3)"
fi
if sanitizer_runtime_missing; then
    report_note "libclang-rt-18-dev is absent, so the ASan/UBSan builds will not link"
fi
pkg-config --exists dbus-1 2>/dev/null || report_missing "libdbus-1-dev"
python3 -c 'import google.protobuf' >/dev/null 2>&1 || report_missing "python protobuf"
# Fatal rather than a note: without these the Mbed TLS build cannot generate its sources, so
# CMake does not get as far as configuring. A pip install that failed above lands here too.
python3 -c 'import jinja2, jsonschema' >/dev/null 2>&1 \
    || report_missing "python jinja2/jsonschema (Mbed TLS generated sources)"
if git submodule status --recursive 2>/dev/null | grep '^[-+]' >/dev/null; then
    report_missing "git submodules"
fi

if [ "$REMAINING" -eq 1 ]; then
    if [ "$CHECK_ONLY" -eq 1 ]; then
        echo "Run scripts/setup-linux.sh to install the above."
    else
        echo "Setup incomplete; the build will not work until the above are resolved."
    fi
    exit 1
fi

echo "Ready. Next: make test   (Debug build + unit tests)"
