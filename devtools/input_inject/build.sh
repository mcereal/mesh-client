#!/usr/bin/env bash
# Cross-build the input injector for the Brick. Runs inside the cross container:
#
#   scripts/docker.sh --cross devtools/input_inject/build.sh
#
# Static, because the device has no toolchain and no matching libc for a host build. Output
# lands in $BUILD_ROOT/input_inject.
set -euo pipefail

if [[ ! -f /opt/cross/env.sh ]]; then
    echo "cross toolchain not found; run this inside the cross container" >&2
    exit 1
fi
# shellcheck disable=SC1091
source /opt/cross/env.sh

HERE=devtools/input_inject
OUT="${BUILD_ROOT:-build}/input_inject"
mkdir -p "$OUT"

"${CROSS_COMPILE}gcc" -std=c17 -Os ${CROSS_CFLAGS:-} -Wall -Wextra -Werror \
    -o "$OUT/input_inject" "$HERE/input_inject.c" -static
"${CROSS_COMPILE}strip" "$OUT/input_inject"
file "$OUT/input_inject"
