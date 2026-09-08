#!/usr/bin/env bash
# Regenerate the listing screenshots in .github/resources/screenshots.
#
# These are the five stills the README and the Pak Store listing carry, and they go stale the
# way a screenshot always does - the UI moved on and nothing failed. This renders them from
# scene scripts instead, off-screen and with no device: `devtools/ui_capture/scenes/shots/`
# holds one scene per shot, each named for the file it writes, so refreshing the set after a UI
# change is running this rather than finding a Brick.
#
# The renderer is the one that ships (fb_render_snapshot, drawing into memory rather than into
# /dev/fb0), at the panel's own 1024x768 and the device's own glyph scale - so a shot from here
# is the frame the device would put on the panel, not an approximation of it.
#
# Usage: scripts/screenshots.sh [options] [NAME ...]
#
#   NAME                  which shots to render (messages, nodes, waypoints, status,
#                         settings).
#                         Default: all of them.
#
# Options:
#   -t, --theme NAME      dark|light|contrast|colorblind; a scene's own `theme` still wins
#   -o, --out DIR         where to write them. Default .github/resources/screenshots
#   -h, --help
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

SCENE_DIR="devtools/ui_capture/scenes/shots"
OUT_DIR=".github/resources/screenshots"
THEME=""

usage() { sed -n '2,/^set -/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }
die() { echo "screenshots: $*" >&2; exit 1; }

NAMES=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--theme) THEME="${2:-}"; shift 2 ;;
        -o|--out) OUT_DIR="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option $1" ;;
        *) NAMES+=("$1"); shift ;;
    esac
done

if [[ ${#NAMES[@]} -eq 0 ]]; then
    for scene in "${SCENE_DIR}"/*.scene; do
        NAMES+=("$(basename "${scene}" .scene)")
    done
fi

mkdir -p "${OUT_DIR}"

# Build once here rather than four times: every shot after the first is --no-build.
BUILD_ARGS=()
for name in "${NAMES[@]}"; do
    scene="${SCENE_DIR}/${name}.scene"
    [[ -f "${scene}" ]] || die "no scene for '${name}' (looked for ${scene})"
    args=("${BUILD_ARGS[@]}")
    if [[ -n "${THEME}" ]]; then
        args+=(-t "${THEME}")
    fi
    ./scripts/ui-capture.sh "${args[@]}" -o "${OUT_DIR}/${name}.png" "${scene}"
    BUILD_ARGS=(--no-build)
done
