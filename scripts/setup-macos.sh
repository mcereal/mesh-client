#!/usr/bin/env bash
# Provision a Mac for building and running mesh-client natively, as a development host.
#
# macOS is not a target - nothing ships for it - but the core builds and the whole suite passes
# there, and MESHCLIENT_UI_BACKEND=sdl puts the UI in a window, so a screen can be worked on
# without a Brick. inkwell's loop is kqueue on this host; the fb backend, evdev, BlueZ and the
# USB mass-storage installer compile to refusals. See docs/ui.md for what that leaves.
#
#   - cmake, ninja, pkg-config   the build, as on Linux
#   - sdl2                       the window; without it the sdl backend reports unavailable and
#                                there is no way to see the UI on this host at all
#   - .venv/                     the Python the generators need - nanopb's (protobuf,
#                                grpcio-tools) and Mbed TLS's (jinja2, jsonschema) - plus
#                                clang-format 18, which Homebrew does not pin. A venv because
#                                Homebrew's Python refuses a pip install outside one (PEP 668).
#                                scripts/build.sh and `make format` find it by path.
#
# Usage:
#   scripts/setup-macos.sh              # install everything that is missing
#   scripts/setup-macos.sh --check      # report what is missing, install nothing
#
# Safe to re-run: every step is skipped when already satisfied.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

say() { printf '  %s\n' "$*"; }
missing=0

echo "Homebrew packages:"
if ! command -v brew >/dev/null 2>&1; then
    say "missing: Homebrew (https://brew.sh) - nothing else here can be installed without it"
    exit 1
fi
for formula in cmake ninja pkg-config sdl2; do
    if brew list --formula "$formula" >/dev/null 2>&1; then
        say "$formula"
    elif [ "$CHECK_ONLY" -eq 1 ]; then
        say "missing: $formula"
        missing=1
    else
        say "installing $formula"
        brew install "$formula"
    fi
done

echo "Python (.venv):"
VENV="$REPO_ROOT/.venv"
PY_PACKAGES="protobuf grpcio-tools jinja2 jsonschema clang-format==18.1.3"
if [ -x "$VENV/bin/python3" ] &&
    "$VENV/bin/python3" -c 'import google.protobuf, grpc_tools, jinja2, jsonschema' 2>/dev/null &&
    [ -x "$VENV/bin/clang-format" ]; then
    say "ready ($("$VENV/bin/python3" --version))"
elif [ "$CHECK_ONLY" -eq 1 ]; then
    say "missing: .venv with $PY_PACKAGES"
    missing=1
else
    [ -x "$VENV/bin/python3" ] || python3 -m venv "$VENV"
    say "installing $PY_PACKAGES"
    # shellcheck disable=SC2086
    "$VENV/bin/pip" install -q $PY_PACKAGES
fi

echo "Submodules:"
# As scripts/setup-linux.sh does it: '-' is a submodule never initialised, '+' one checked out at
# something other than the recorded commit - which is what an existing clone has straight after
# pulling a revision that bumped inkwell or inkcell, and a build against the old one fails on
# whatever the bump added. A file-exists check would call that clone ready.
if git submodule status --recursive 2>/dev/null | grep '^[-+]' >/dev/null; then
    if [ "$CHECK_ONLY" -eq 1 ]; then
        say "missing: submodules are not initialised or are out of sync"
        missing=1
    else
        say "syncing submodules to the recorded commits"
        git submodule update --init --recursive
    fi
else
    say "in sync"
fi

if [ "$missing" -ne 0 ]; then
    echo "Something is missing; run scripts/setup-macos.sh without --check." >&2
    exit 1
fi
echo "Ready: make test, then MESHCLIENT_UI_BACKEND=sdl ./build/debug/meshclient -f"
