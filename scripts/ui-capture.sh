#!/usr/bin/env bash
# Render the HUD from a scene script, as a GIF (or a PNG for a single frame).
#
# The companion to `deploy-device.sh shot`, for the case where there is no device: it drives the
# real navigation model and the real framebuffer renderer off-screen, so a UI change can be
# looked at from a container, a CI runner or a cloud session. A clip rather than a still,
# because most UI changes are about a transition - a thread opening, the keyboard coming up -
# and no still shows that.
#
# Usage: scripts/ui-capture.sh [options] [SCENE]
#
#   SCENE                 a scene script; '-' or omitted reads stdin.
#                         Examples in devtools/ui_capture/scenes/.
#
# Options:
#   -o, --out FILE        output path; .png captures a single frame, anything else is a GIF.
#                         Default: the scene's name with .gif, or ui-capture.gif from stdin.
#   -s, --scale N         glyph scale 2..6 (the device default is 4)
#   -g, --geometry WxH    the panel to render into. Default 1024x768, the Brick's. A change
#                         that has to hold up on another screen is reviewable by rendering the
#                         same scene twice - the renderer measures everything it draws, so the
#                         layout is what moves, not the picture's scale.
#   -t, --theme NAME      dark|light|contrast|colorblind; a scene's own `theme` still wins
#   -d, --downscale N     shrink the output by an integer factor. Default 2 for a GIF, 1 for a
#                         PNG - the panel is 1024x768 and a full-size clip is four times the file
#                         for no more legibility.
#       --delay MS        default per-frame delay; a scene's own `delay`/`hold` still win
#       --frames DIR      keep the intermediate PPM frames here (implies --keep)
#       --keep            do not delete the frames
#       --no-build        use the binary as it is instead of rebuilding it first
#   -h, --help
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INVOKE_DIR="${PWD}"
cd "${REPO_ROOT}"

BUILD_ROOT="${BUILD_ROOT:-build}"
BUILD_DIR="${BUILD_ROOT}/debug"
UICAP="${BUILD_DIR}/devtools/meshclient_uicap"

usage() { sed -n '2,/^set -/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }
die() { echo "ui-capture: $*" >&2; exit 1; }

OUT=""
SCALE=""
GEOMETRY=""
THEME=""
DOWNSCALE=""
DELAY=""
FRAMES_DIR=""
KEEP=0
BUILD=1
SCENE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--out) OUT="${2:-}"; shift 2 ;;
        -s|--scale) SCALE="${2:-}"; shift 2 ;;
        # Normalised so the width can be read back below; the binary takes either spelling.
        -g|--geometry) GEOMETRY="${2:-}"; GEOMETRY="${GEOMETRY//X/x}"; shift 2 ;;
        -t|--theme) THEME="${2:-}"; shift 2 ;;
        -d|--downscale) DOWNSCALE="${2:-}"; shift 2 ;;
        --delay) DELAY="${2:-}"; shift 2 ;;
        --frames) FRAMES_DIR="${2:-}"; KEEP=1; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --no-build) BUILD=0; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option $1" ;;
        *)
            [[ -z "${SCENE}" ]] || die "one scene at a time (got '${SCENE}' and '$1')"
            SCENE="$1"; shift ;;
    esac
done

command -v python3 >/dev/null 2>&1 || die "python3 is needed to encode the frames"

if [[ ${BUILD} -eq 1 ]]; then
    if [[ -d "${BUILD_DIR}" ]]; then
        cmake --build "${BUILD_DIR}" --target meshclient_uicap >/dev/null
    else
        ./scripts/build.sh debug >/dev/null
    fi
fi
[[ -x "${UICAP}" ]] || die "${UICAP} is missing; run 'make debug' (or './scripts/docker.sh make debug' on macOS)"

# A scene named on the command line belongs to the directory the user was standing in.
SCENE_PATH="-"
if [[ -n "${SCENE}" && "${SCENE}" != "-" ]]; then
    if [[ "${SCENE}" == /* ]]; then SCENE_PATH="${SCENE}"; else SCENE_PATH="${INVOKE_DIR}/${SCENE}"; fi
    [[ -f "${SCENE_PATH}" ]] || SCENE_PATH="${REPO_ROOT}/${SCENE}"
    [[ -f "${SCENE_PATH}" ]] || die "no scene file at ${SCENE}"
fi

if [[ -z "${OUT}" ]]; then
    if [[ "${SCENE_PATH}" == "-" ]]; then
        OUT="ui-capture.gif"
    else
        OUT="$(basename "${SCENE_PATH}")"
        OUT="${OUT%.*}.gif"
    fi
fi
[[ "${OUT}" == /* ]] || OUT="${INVOKE_DIR}/${OUT}"

if [[ -z "${FRAMES_DIR}" ]]; then
    FRAMES_DIR="$(mktemp -d)"
    trap 'if [[ ${KEEP} -eq 0 ]]; then rm -rf "${FRAMES_DIR}"; fi' EXIT
else
    [[ "${FRAMES_DIR}" == /* ]] || FRAMES_DIR="${INVOKE_DIR}/${FRAMES_DIR}"
    mkdir -p "${FRAMES_DIR}"
fi

CAP_ARGS=(--out "${FRAMES_DIR}" --quiet)
[[ -n "${SCALE}" ]] && CAP_ARGS+=(--scale "${SCALE}")
[[ -n "${GEOMETRY}" ]] && CAP_ARGS+=(--geometry "${GEOMETRY}")
[[ -n "${THEME}" ]] && CAP_ARGS+=(--theme "${THEME}")
[[ -n "${DELAY}" ]] && CAP_ARGS+=(--delay "${DELAY}")
[[ "${SCENE_PATH}" != "-" ]] && CAP_ARGS+=(--script "${SCENE_PATH}")

"${UICAP}" "${CAP_ARGS[@]}"

FRAME_COUNT="$(wc -l < "${FRAMES_DIR}/frames.txt" | tr -d ' ')"
[[ "${FRAME_COUNT}" -gt 0 ]] || die "the scene produced no frames"

if [[ "${OUT}" == *.png ]]; then
    [[ -z "${DOWNSCALE}" ]] && DOWNSCALE=1
    if [[ "${FRAME_COUNT}" -gt 1 ]]; then
        echo "ui-capture: the scene produced ${FRAME_COUNT} frames; the PNG holds the last one." >&2
    fi
    LAST="$(tail -n 1 "${FRAMES_DIR}/frames.txt" | cut -f1)"
    python3 scripts/frames.py png --downscale "${DOWNSCALE}" \
        --out "${OUT}" "${FRAMES_DIR}/${LAST}"
else
    # Halving is right for the Brick's 1024px panel and wrong for a small one, where it is the
    # difference between a legible clip and a thumbnail. The threshold is the width below which
    # a downscaled frame stops being readable rather than merely smaller.
    if [[ -z "${DOWNSCALE}" ]]; then
        DOWNSCALE=2
        if [[ -n "${GEOMETRY}" && "${GEOMETRY%%x*}" -lt 800 ]]; then DOWNSCALE=1; fi
    fi
    python3 scripts/frames.py gif --downscale "${DOWNSCALE}" --manifest "${FRAMES_DIR}/frames.txt" \
        --out "${OUT}"
fi

[[ ${KEEP} -eq 1 ]] && echo "ui-capture: frames kept in ${FRAMES_DIR}"
exit 0
