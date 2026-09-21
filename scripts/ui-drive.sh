#!/usr/bin/env bash
# Drive a running MeshClient by its keys and look at what it drew - in a window on a Mac, with no
# display at all in a container or a cloud session, or on a Brick over adb/ssh.
#
# The client listens on a control socket (include/mesh/app/control.h); this starts one that does,
# sends it commands, and turns every `shot` into a PNG on this machine.
#
# Usage: scripts/ui-drive.sh [--brick] <command> [args]
#
#   start [-b BACKEND] [-- meshclient args]
#                      Start a client in the background with the socket open. BACKEND is sdl
#                      (a window) or headless (no display); the default is sdl on a Mac or
#                      where $DISPLAY/$WAYLAND_DISPLAY is set, headless otherwise. The log is
#                      build/ui-drive.log. With --brick: `deploy-device.sh start` with the socket.
#   send 'COMMANDS'    Send commands, `;`-separated, and print each answer:
#                        key NAME...  (up down left right a b x y l1 r1 l2 r2 start select)
#                        wait MS      shot FILE.png      screen      ping      quit
#                      A shot's FILE is on this machine, relative to where you ran this.
#   shot FILE.png      Shorthand for send 'shot FILE.png'.
#   stop               Stop the client. With --brick, `deploy-device.sh stop` - end every
#                      on-device session with it.
#
# Options:
#   --brick            Drive the Brick rather than a client on this machine. The transport and
#                      the device are .brick.env's; see scripts/deploy-device.sh.
#   -h, --help
#
# Environment:
#   MESHCLIENT_UI_CONTROL  the socket on this machine (default /tmp/meshclient-ui-$USER.sock)
#   MESHCLIENT_BIN         the client (default build/debug/meshclient; `make debug` builds it)
#
# Examples:
#   scripts/ui-drive.sh start
#   scripts/ui-drive.sh send 'key r1 r1; shot waypoints.png; screen'
#   scripts/ui-drive.sh stop
#   scripts/ui-drive.sh --brick start && scripts/ui-drive.sh --brick send 'key a; shot a.png'
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INVOKE_DIR="${PWD}"
SOCKET="${MESHCLIENT_UI_CONTROL:-/tmp/meshclient-ui-${USER:-$(id -un)}.sock}"
BIN="${MESHCLIENT_BIN:-${REPO_ROOT}/build/debug/meshclient}"
LOG="${REPO_ROOT}/build/ui-drive.log"
DEPLOY="${REPO_ROOT}/scripts/deploy-device.sh"
FRAMES="${REPO_ROOT}/scripts/frames.py"
BRICK=0

usage() {
    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

die() {
    echo "ui-drive: $*" >&2
    exit 1
}

# One command to the local socket, answers on stdout. The client is its own sender.
local_send() {
    [[ -x "${BIN}" ]] || die "${BIN} not found; run 'make debug'"
    "${BIN}" --ui-control "${SOCKET}" --ui-send "$1"
}

local_alive() {
    [[ -S "${SOCKET}" ]] && local_send ping >/dev/null 2>&1
}

cmd_start() {
    local backend=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -b|--backend) backend="$2"; shift 2 ;;
            --) shift; break ;;
            *) die "start: unknown argument $1 (-b BACKEND, -- meshclient args)" ;;
        esac
    done

    if [[ ${BRICK} -eq 1 ]]; then
        [[ -z "${backend}" ]] || die "start: the Brick draws on its panel; -b is for this machine"
        "${DEPLOY}" start -- --ui-control "${BRICK_UI_CONTROL:-/tmp/meshclient-ui.sock}" "$@"
        "${DEPLOY}" ui-send -- ping >/dev/null ||
            die "the client started but its socket did not answer; see 'make deploy-logs'"
        echo "Driving the Brick. End with: $0 --brick stop"
        return
    fi

    [[ -x "${BIN}" ]] || die "${BIN} not found; run 'make debug'"
    local_alive && die "a client is already listening at ${SOCKET}; '$0 stop' first"
    if [[ -z "${backend}" ]]; then
        if [[ "$(uname -s)" == Darwin || -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
            backend=sdl
        else
            backend=headless
        fi
    fi

    mkdir -p "$(dirname "${LOG}")"
    # Detached from this shell, so the client outlives the command that started it - which is
    # the whole point: every later `send` is a separate process.
    MESHCLIENT_UI_BACKEND="${backend}" nohup "${BIN}" -f --ui-control "${SOCKET}" "$@" \
        >"${LOG}" 2>&1 </dev/null &
    local pid=$! i
    for ((i = 0; i < 50; i++)); do
        local_alive && break
        kill -0 "${pid}" 2>/dev/null || die "the client exited during start-up; see ${LOG}"
        sleep 0.2
    done
    local_alive || die "no answer at ${SOCKET} after 10s; see ${LOG}"
    echo "MeshClient (pid ${pid}, ${backend}) listening at ${SOCKET}; log in ${LOG}"
}

# Every `shot X.png` becomes a shot into a PPM where the client can write it, and comes back as
# X.png here. On a Brick that is /tmp on the device, pulled over the same transport.
cmd_send() {
    [[ $# -ge 1 ]] || die "send: nothing to send"
    local part verb rest out rewritten="" status=0
    local -a parts=() remote=() wanted=()
    local n=0 tmpdir
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "${tmpdir}"' RETURN

    IFS=';' read -ra parts <<<"${1//$'\n'/;}"
    for part in ${parts[@]+"${parts[@]}"}; do
        read -r verb rest <<<"${part}" || true
        if [[ "${verb}" == shot ]]; then
            [[ -n "${rest}" ]] || die "send: shot needs a file"
            out="${rest}"
            [[ "${out}" == /* ]] || out="${INVOKE_DIR}/${out}"
            if [[ ${BRICK} -eq 1 ]]; then
                remote+=("/tmp/ui-drive-${n}.ppm")
            else
                remote+=("${tmpdir}/${n}.ppm")
            fi
            wanted+=("${out}")
            part="shot ${remote[${n}]}"
            n=$((n + 1))
        fi
        rewritten+="${part};"
    done

    if [[ ${BRICK} -eq 1 ]]; then
        "${DEPLOY}" ui-send -- "${rewritten}" || status=$?
    else
        local_send "${rewritten}" || status=$?
    fi

    local i
    for ((i = 0; i < n; i++)); do
        if [[ ${BRICK} -eq 1 ]]; then
            "${DEPLOY}" pull -- "${remote[${i}]}" "${tmpdir}/${i}.ppm" 2>/dev/null || continue
            remote[${i}]="${tmpdir}/${i}.ppm"
        fi
        [[ -s "${remote[${i}]}" ]] || continue
        mkdir -p "$(dirname "${wanted[${i}]}")"
        python3 "${FRAMES}" png --out "${wanted[${i}]}" "${remote[${i}]}" >/dev/null
        echo "wrote ${wanted[${i}]}"
    done
    return "${status}"
}

cmd_stop() {
    if [[ ${BRICK} -eq 1 ]]; then
        "${DEPLOY}" stop
        return
    fi
    if ! local_alive; then
        echo "No client listening at ${SOCKET}."
        return 0
    fi
    local_send quit >/dev/null
    local i
    for ((i = 0; i < 50; i++)); do
        [[ -S "${SOCKET}" ]] || { echo "Stopped."; return 0; }
        sleep 0.2
    done
    die "the client did not exit within 10s; its socket is still at ${SOCKET}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --brick) BRICK=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) break ;;
    esac
done
[[ $# -ge 1 ]] || { usage >&2; exit 1; }
command="$1"
shift
case "${command}" in
    start) cmd_start "$@" ;;
    send) cmd_send "$*" ;;
    shot) [[ $# -eq 1 ]] || die "shot FILE.png"; cmd_send "shot $1" ;;
    stop) cmd_stop ;;
    *) die "unknown command: ${command} (see --help)" ;;
esac
