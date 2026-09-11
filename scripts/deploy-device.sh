#!/usr/bin/env bash
# Push, run, and inspect MeshClient.pak on a TrimUI Brick over SSH or USB (adb).
#
# Runs on the development host (macOS or Linux). Two transports, one command set:
#
#   ssh  - the Brick on WiFi with the "SSH Server" pak (dropbear) running. Only busybox tools
#          are assumed on the device: no rsync, no scp - transfers go through `tar | ssh tar`.
#   adb  - the Brick on the USB-C DATA port (the one that also charges) with its adb gadget up
#          (adbd runs by default on NextUI). No WiFi, no SSH server, no key needed. This is the
#          reliable path when the LAN route to the device is flaky - see docs/device.md.
#
# The transport is chosen by BRICK_TRANSPORT (auto|ssh|adb); `auto` (the default) uses adb when
# a device is attached, otherwise ssh. The Brick's adbd is old - no `exec-out`, no no-pty shell,
# and it does not report remote exit codes - so the adb path moves every byte with the native
# `adb push`/`adb pull` sync protocol and uses `adb shell` only for text, verifying by checksum.
#
# Usage: scripts/deploy-device.sh [options] <command> [-- args...]
#
# Commands:
#   push               Copy dist/MeshClient.pak to <sdcard>/Tools/<platform>/MeshClient.pak (default)
#   start [-- args]    Start MeshClient the way Tools > MeshClient does: NextUI's launcher steps
#                      aside, so it stops painting the screen and acting on the buttons, and comes
#                      back when the client exits. Stops any MeshClient already running first.
#                      Returns once the client is up; extra args go to meshclient.
#   stop               Stop every MeshClient on the device, however it was started, and wait for
#                      the launcher to come back. End every on-device test with this.
#   run [-- args]      `start`, then follow the log until the client exits; Ctrl-C stops it.
#                      Args that make meshclient print and exit (--list-devices, --status,
#                      --send-text, --fetch-firmware, --install-firmware, --version, --help) run
#                      it directly instead, with its output here, e.g. `run -- --list-devices`.
#   logs               Tail the on-device log (<sdcard>/.userdata/<platform>/logs/MeshClient.txt)
#   check              Report what the device has: SD card, BlueZ, D-Bus socket, adapter, fb0, RAM
#   shot [-- args]     Screenshot whatever is on the screen, straight off /dev/fb0, as a PNG.
#                      Args: -o FILE, -d SECS (delay before each), -n COUNT, -P PAGE,
#                      -s N (shrink the PNG by an integer factor).
#   clip [-- args]     Film the screen the same way and write an animated GIF, for a change
#                      that is about a transition rather than one screen.
#                      Args: -o FILE, -d SECS (delay before recording), -n COUNT (frames),
#                      -P PAGE, -s N (downscale, default 2), -r MS (playback delay per frame),
#                      -i SECS (pause between frames on the device).
#   input-map [-- args] Record every /dev/input/event* while you press buttons, then print what
#                      each one reports. Args: -t SECS (record for a fixed time instead of
#                      waiting for Enter), -k (keep the raw capture directory).
#   shell              Interactive shell on the device
#   setup-key          Install ~/.ssh/id_*.pub into the device's authorized_keys (SSH only)
#
# Options:
#   -t, --transport T  Transport: auto|ssh|adb                 (env BRICK_TRANSPORT, default auto)
#   -H, --host HOST    Device IP or hostname (ssh)             (env BRICK_HOST)
#   -u, --user USER    SSH user, default root (ssh)            (env BRICK_USER)
#   -p, --port PORT    SSH port, default 22 (ssh)              (env BRICK_PORT)
#   -s, --serial SN    adb device serial, if more than one     (env BRICK_ADB_SERIAL)
#   -n, --dry-run      Print the commands instead of running them
#   -h, --help
#
# Settings persist in .brick.env at the repo root (gitignored); copy .brick.env.example.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Everything below runs from the repo root, so remember where the user actually was: a file
# they name on the command line belongs in their directory, not in the tree.
INVOKE_DIR="${PWD}"
cd "${REPO_ROOT}"

# .brick.env supplies defaults only: values already in the environment win, flags win over both.
BRICK_VARS=(BRICK_TRANSPORT BRICK_HOST BRICK_USER BRICK_PORT BRICK_PLATFORM BRICK_SDCARD \
    BRICK_SSH_OPTS BRICK_ADB BRICK_ADB_SERIAL)
if [[ -f .brick.env ]]; then
    for v in "${BRICK_VARS[@]}"; do
        eval "_env_set_${v}=\${${v}+set}; _env_val_${v}=\${${v}-}"
    done
    # shellcheck disable=SC1091
    source .brick.env
    for v in "${BRICK_VARS[@]}"; do
        eval "if [[ -n \${_env_set_${v}} ]]; then ${v}=\"\${_env_val_${v}}\"; fi"
    done
fi

BRICK_TRANSPORT="${BRICK_TRANSPORT:-auto}"
BRICK_HOST="${BRICK_HOST:-}"
BRICK_USER="${BRICK_USER:-root}"
BRICK_PORT="${BRICK_PORT:-22}"
BRICK_PLATFORM="${BRICK_PLATFORM:-tg5040}"
BRICK_SDCARD="${BRICK_SDCARD:-/mnt/SDCARD}"
BRICK_SSH_OPTS="${BRICK_SSH_OPTS:--o StrictHostKeyChecking=accept-new -o ConnectTimeout=5}"
BRICK_ADB="${BRICK_ADB:-adb}"
BRICK_ADB_SERIAL="${BRICK_ADB_SERIAL:-}"
PAK_NAME="MeshClient"
LOCAL_PAK="dist/${PAK_NAME}.pak"
DRY_RUN=0

usage() {
    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

die() {
    echo "deploy-device: $*" >&2
    exit 1
}

COMMAND=""
PASSTHRU=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--transport) BRICK_TRANSPORT="$2"; shift 2 ;;
        -H|--host) BRICK_HOST="$2"; shift 2 ;;
        -u|--user) BRICK_USER="$2"; shift 2 ;;
        -p|--port) BRICK_PORT="$2"; shift 2 ;;
        -s|--serial) BRICK_ADB_SERIAL="$2"; shift 2 ;;
        -n|--dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; PASSTHRU=("$@"); break ;;
        -*) die "unknown option: $1 (see --help)" ;;
        *)
            if [[ -z "${COMMAND}" ]]; then
                COMMAND="$1"; shift
            else
                die "unexpected argument: $1 (use -- to pass args to meshclient)"
            fi
            ;;
    esac
done
COMMAND="${COMMAND:-push}"

REMOTE_TOOLS="${BRICK_SDCARD}/Tools/${BRICK_PLATFORM}"
REMOTE_PAK="${REMOTE_TOOLS}/${PAK_NAME}.pak"
# Where a push is assembled before it becomes the pak. Dot-prefixed and *not* ending in .pak,
# because Tools/<platform>/ is a directory the launcher globs: a transfer that dies mid-tar
# leaves this behind, and named "MeshClient.pak.new" it shows up in the launcher as a second,
# broken tool until the next deploy cleans it up.
REMOTE_STAGE="${REMOTE_TOOLS}/.${PAK_NAME}.pak.new"
REMOTE_LOG="${BRICK_SDCARD}/.userdata/${BRICK_PLATFORM}/logs/${PAK_NAME}.txt"

# --- Transport selection ----------------------------------------------------------------------

# Serials of the adb devices that are online right now, one per line.
adb_online_serials() {
    "${BRICK_ADB}" devices 2>/dev/null | awk 'NR>1 && $2=="device"{print $1}'
}

adb_available() {
    command -v "${BRICK_ADB}" >/dev/null 2>&1
}

# Does the one attached adb device look like a Brick? `/usr/trimui` is a TrimUI-firmware marker
# that no phone or emulator carries. This gates `auto` only: an unrelated Android device left
# plugged in must not capture a deploy meant for the Brick on WiFi. An explicit `--transport adb`
# skips the probe - the user named the target. Best-effort: a device that cannot answer is
# treated as not-a-Brick, so auto falls back to ssh rather than writing to something unknown.
adb_is_brick() {
    local probe=("${BRICK_ADB}")
    [[ -n "${1:-}" ]] && probe+=(-s "$1")
    "${probe[@]}" shell '[ -d /usr/trimui ] && echo brick' 2>/dev/null | tr -d '\r' | grep -q brick
}

# Resolve TRANSPORT (ssh|adb) from BRICK_TRANSPORT. `auto` prefers a physically-attached Brick
# over the LAN, which is the whole point: the cable is the reliable path. It prefers adb only
# when exactly one device is attached and it identifies as a Brick, so a phone on the same USB
# hub cannot steal a deploy.
case "${BRICK_TRANSPORT}" in
    ssh) TRANSPORT=ssh ;;
    adb) TRANSPORT=adb ;;
    auto)
        _auto_serials="$(adb_available && adb_online_serials || true)"
        _auto_n="$(printf '%s' "${_auto_serials}" | grep -c . || true)"
        if [[ "${_auto_n}" == "1" ]] && adb_is_brick "${_auto_serials}"; then
            TRANSPORT=adb
        elif [[ -n "${BRICK_HOST}" ]]; then
            TRANSPORT=ssh
        elif [[ "${_auto_n}" -ge 1 ]]; then
            # No host to fall back to; use the attached device(s) and let the checks below ask
            # for a serial if there is more than one.
            TRANSPORT=adb
        else
            TRANSPORT=ssh
        fi
        ;;
    *) die "unknown transport: ${BRICK_TRANSPORT} (auto|ssh|adb)" ;;
esac

# Per-transport wiring: SSH_OPTS/TARGET for ssh, ADB_CMD/serial for adb, and a DEV_LABEL both
# use in messages.
if [[ "${TRANSPORT}" == "ssh" ]]; then
    [[ -n "${BRICK_HOST}" ]] || die "no device host. Set BRICK_HOST in .brick.env or pass --host (see docs/device.md)"
    TARGET="${BRICK_USER}@${BRICK_HOST}"
    DEV_LABEL="${TARGET}"
    # shellcheck disable=SC2206
    SSH_OPTS=(${BRICK_SSH_OPTS} -p "${BRICK_PORT}")
else
    adb_available || die "adb not found. Install it (macOS: brew install --cask android-platform-tools) or set BRICK_ADB (see docs/device.md)"
    ADB_CMD=("${BRICK_ADB}")
    if [[ -n "${BRICK_ADB_SERIAL}" ]]; then
        ADB_CMD+=(-s "${BRICK_ADB_SERIAL}")
    else
        _n="$(adb_online_serials | wc -l | tr -d ' ')"
        if [[ "${DRY_RUN}" -ne 1 ]]; then
            [[ "${_n}" != "0" ]] || die "no adb device attached. Plug the USB-C DATA port (the one that also charges) into this host, or use --transport ssh"
            [[ "${_n}" == "1" ]] || die "${_n} adb devices attached; set BRICK_ADB_SERIAL or pass --serial (adb devices)"
        fi
    fi
    DEV_LABEL="adb:${BRICK_ADB_SERIAL:-$(adb_online_serials | head -n1)}"
    DEV_LABEL="${DEV_LABEL:-adb}"
fi

# --- Remote primitives ------------------------------------------------------------------------
#
# Three text/interactive primitives and two binary ones. The remote *script strings* are shared:
# both transports hand the string to the device's busybox sh, so anything built with sq() below
# runs unchanged either way. Only the invocation and the byte-moving differ.
#
# The Brick's adb allocates a pty for every `adb shell`, so its stdout arrives with CRLF line
# endings and is not binary-safe; remote_exec strips the \r for the text callers, and no binary
# ever crosses `adb shell` - it goes through push/pull instead.

# Run a command string on the device, non-interactive, text stdout to our stdout (callers may
# capture it). ssh joins its arguments and hands the string to the remote shell.
remote_exec() {
    if [[ ${DRY_RUN} -eq 1 ]]; then
        if [[ "${TRANSPORT}" == "adb" ]]; then
            printf 'adb %s shell %s\n' "${BRICK_ADB_SERIAL:+-s ${BRICK_ADB_SERIAL}}" "$(sq "$1")"
        else
            printf 'ssh %s %s %s\n' "${SSH_OPTS[*]}" "${TARGET}" "$(sq "$1")"
        fi
        return 0
    fi
    if [[ "${TRANSPORT}" == "adb" ]]; then
        "${ADB_CMD[@]}" shell "$1" | tr -d '\r'
    else
        ssh "${SSH_OPTS[@]}" "${TARGET}" "$1"
    fi
}

# Streaming text with no capture (tail -f, a live run): no \r strip, because tr would block-buffer
# and stall the stream. A cosmetic \r in a terminal is harmless.
remote_stream() {
    if [[ ${DRY_RUN} -eq 1 ]]; then
        remote_exec "$1"
        return 0
    fi
    if [[ "${TRANSPORT}" == "adb" ]]; then
        "${ADB_CMD[@]}" shell "$1"
    else
        ssh "${SSH_OPTS[@]}" "${TARGET}" "$1"
    fi
}

# Interactive session, with or without a command.
remote_tty() {
    if [[ ${DRY_RUN} -eq 1 ]]; then
        if [[ "${TRANSPORT}" == "adb" ]]; then
            printf 'adb %s shell %s\n' "${BRICK_ADB_SERIAL:+-s ${BRICK_ADB_SERIAL}}" "${1:-}"
        else
            printf 'ssh -t %s %s %s\n' "${SSH_OPTS[*]}" "${TARGET}" "${1:-}"
        fi
        return 0
    fi
    if [[ "${TRANSPORT}" == "adb" ]]; then
        if [[ -n "${1:-}" ]]; then "${ADB_CMD[@]}" shell "$1"; else "${ADB_CMD[@]}" shell; fi
    else
        if [[ -n "${1:-}" ]]; then ssh -t "${SSH_OPTS[@]}" "${TARGET}" "$1"; else ssh -t "${SSH_OPTS[@]}" "${TARGET}"; fi
    fi
}

# Pull one device file to a local path, binary-safe. adb uses the sync protocol; ssh reads it
# back over `cat` (binary over ssh is fine).
remote_pull_file() {
    local remote="$1" local_dest="$2"
    if [[ ${DRY_RUN} -eq 1 ]]; then
        if [[ "${TRANSPORT}" == "adb" ]]; then
            printf 'adb pull %s %s\n' "${remote}" "${local_dest}"
        else
            printf 'ssh %s %s %s > %s\n' "${SSH_OPTS[*]}" "${TARGET}" "$(sq "cat $(sq "${remote}")")" "${local_dest}"
        fi
        return 0
    fi
    if [[ "${TRANSPORT}" == "adb" ]]; then
        "${ADB_CMD[@]}" pull "${remote}" "${local_dest}" >/dev/null
    else
        ssh "${SSH_OPTS[@]}" "${TARGET}" "cat $(sq "${remote}")" > "${local_dest}"
    fi
}

# Pull the contents of a device directory into a local directory, binary-safe.
remote_pull_dir() {
    local remote="$1" local_dest="$2"
    if [[ ${DRY_RUN} -eq 1 ]]; then
        if [[ "${TRANSPORT}" == "adb" ]]; then
            printf 'adb pull %s/. %s\n' "${remote}" "${local_dest}"
        else
            printf 'ssh %s %s %s | tar xf - -C %s\n' "${SSH_OPTS[*]}" "${TARGET}" "$(sq "tar cf - -C $(sq "${remote}") .")" "${local_dest}"
        fi
        return 0
    fi
    if [[ "${TRANSPORT}" == "adb" ]]; then
        "${ADB_CMD[@]}" pull "${remote}/." "${local_dest}" >/dev/null
    else
        ssh "${SSH_OPTS[@]}" "${TARGET}" "tar cf - -C $(sq "${remote}") ." | tar xf - -C "${local_dest}"
    fi
}

# sha256 hex digest of a local file; Linux ships sha256sum, macOS ships Perl's shasum.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        die "need sha256sum or shasum on the host"
    fi
}

# POSIX single-quote a string for the device's /bin/sh (no bash $'...' forms).
sq() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

cmd_push() {
    [[ -d "${LOCAL_PAK}" ]] || die "${LOCAL_PAK} not found. Run 'make docker-pak' first."
    [[ -x "${LOCAL_PAK}/bin/shared/meshclient" ]] || die "${LOCAL_PAK}/bin/shared/meshclient missing or not executable"

    local local_sum
    local_sum="$(sha256_of "${LOCAL_PAK}/bin/shared/meshclient")"
    echo "Pushing ${LOCAL_PAK} -> ${DEV_LABEL}:${REMOTE_PAK} (${TRANSPORT})"
    echo "  meshclient sha256 ${local_sum}"

    # Stage beside the pak under a dot-prefixed name that does NOT end in .pak, then swap, so a
    # half-finished transfer never leaves a broken pak in Tools/ that NextUI would try to launch
    # (Tools/<platform>/ is globbed by the launcher). Both the new stage name and the legacy
    # "MeshClient.pak.new" are cleaned up - a Brick that saw a failed deploy before either fix
    # may hold one. `sync` and a chmod (harmless on the SD card's FAT mount, load-bearing on a
    # real fs) precede reading the checksum back.
    local swap_script
    swap_script="set -e
rm -rf $(sq "${REMOTE_PAK}")
mv $(sq "${REMOTE_STAGE}") $(sq "${REMOTE_PAK}")
chmod +x $(sq "${REMOTE_PAK}/launch.sh") $(sq "${REMOTE_PAK}/bin/shared/meshclient")
sync"

    local remote_sum
    if [[ "${TRANSPORT}" == "adb" ]]; then
        # adb's sync protocol is the only binary-safe channel this adbd has; stage the tree with
        # it (no gzip - the sync protocol carries the transfer and USB is fast), then swap and
        # checksum over a text shell.
        local stage_script="rm -rf $(sq "${REMOTE_STAGE}") $(sq "${REMOTE_PAK}.new"); mkdir -p $(sq "${REMOTE_STAGE}")"
        if [[ ${DRY_RUN} -eq 1 ]]; then
            remote_exec "${stage_script}"
            printf 'adb push %s/. %s\n' "${LOCAL_PAK}" "${REMOTE_STAGE}"
            remote_exec "${swap_script}"
            remote_exec "sha256sum $(sq "${REMOTE_PAK}/bin/shared/meshclient") | cut -d' ' -f1"
            return 0
        fi
        remote_exec "${stage_script}" >/dev/null
        "${ADB_CMD[@]}" push "${LOCAL_PAK}/." "${REMOTE_STAGE}" >/dev/null
        remote_exec "${swap_script}" >/dev/null
        remote_sum="$(remote_exec "sha256sum $(sq "${REMOTE_PAK}/bin/shared/meshclient") 2>/dev/null | cut -d' ' -f1")"
    else
        # ssh: one connection, a compressed tar in, checksum out. Compressed because the transfer
        # is the slow part and the pak is mostly one static binary (2.86 MB -> 1.17 MB), which on
        # a Brick whose Wi-Fi is having a bad day is the difference between a push that lands and
        # one that dies mid-stream. The device inflates with busybox `gunzip` (the applet the
        # radio-firmware download already relies on - docs/radio-firmware-roadmap.md); `tar -xzf`
        # is avoided because busybox tar only understands -z when built with FEATURE_TAR_GZIP.
        local remote_script
        remote_script="set -e
mkdir -p $(sq "${REMOTE_TOOLS}")
rm -rf $(sq "${REMOTE_STAGE}") $(sq "${REMOTE_PAK}.new")
mkdir $(sq "${REMOTE_STAGE}")
gunzip -c | tar -C $(sq "${REMOTE_STAGE}") -xf -
${swap_script}
sha256sum $(sq "${REMOTE_PAK}/bin/shared/meshclient") 2>/dev/null | cut -d' ' -f1"
        if [[ ${DRY_RUN} -eq 1 ]]; then
            printf 'tar -C %s -czf - . | ' "${LOCAL_PAK}"
            remote_exec "${remote_script}"
            return 0
        fi
        remote_sum="$(tar -C "${LOCAL_PAK}" -czf - . | ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}")"
    fi

    remote_sum="$(printf '%s' "${remote_sum}" | tr -d '[:space:]')"
    if [[ -z "${remote_sum}" ]]; then
        # An empty checksum is the only failure signal the adb path has: this adbd does not
        # report remote exit codes, so a failed swap, chmod or checksum returns "success" with
        # no output. Treat it as a failed deploy rather than printing "Deployed" over a pak that
        # may be missing or half-written. Over ssh the remote script runs under `set -e` and a
        # real failure surfaces on its own, so a rare empty result there is only a warning.
        if [[ "${TRANSPORT}" == "adb" ]]; then
            die "push not verified: the device returned no checksum, so the swap or checksum step failed (this adbd does not report exit codes). The pak may be incomplete; re-run."
        fi
        echo "  (device did not return a checksum; skipped verification)"
    elif [[ "${remote_sum}" != "${local_sum}" ]]; then
        die "checksum mismatch after push (device ${remote_sum})"
    fi
    echo "Deployed. Launch it from Tools > ${PAK_NAME} on the device, or: $0 start"
    # A client that was running before the push is still the previous build: the swap replaced
    # the file, not the process holding the old one open.
    if [[ ${DRY_RUN} -ne 1 ]]; then
        local running
        running="$(remote_exec 'pidof meshclient || true')"
        if [[ -n "${running}" ]]; then
            echo "Note: MeshClient (pid ${running}) is still running the previous build."
            echo "      '$0 start' restarts it on this one; '$0 stop' ends it."
        fi
    fi
}

# --- Running the client -----------------------------------------------------------------------
#
# NextUI's launch loop (.system/<platform>/paks/MinUI.pak/launch.sh on the card) runs nextui.elf,
# and when that exits it evals whatever /tmp/next holds - the pak's launch.sh, when Tools >
# MeshClient was pressed - then starts nextui.elf again, for as long as /tmp/nextui_exec exists.
# That hand-off is the only way a pak gets the device to itself. A client started straight from a
# shell runs *beside* the launcher, which goes on painting fb0 and, since nothing grabs the pad,
# acting on every button: L1 flips MeshClient's tab and NextUI's page at once and the panel
# flickers between the two. So `start` does what the Tools menu does: it writes /tmp/next in
# nextui.elf's own format and takes the launcher off the screen.
#
# The launcher is killed with SIGKILL, and must never get TERM or INT. SDL turns either into
# SDL_QUIT, nextui.elf answers SDL_QUIT with PWR_powerOff(), and PLAT_powerOff() deletes
# /tmp/nextui_exec and touches /tmp/poweroff on its way out - so the loop runs the pak once more
# and powers the Brick off the moment it exits. That was measured, twice. KILL cannot be caught:
# the launcher dies without taking that path, and the loop just runs /tmp/next.
#
# Every script below reports failure as a line starting ERROR:, because this adbd returns no exit
# codes.

# Stop every meshclient on the device and, when one was stopped, wait for the launcher to come
# back - the loop restarts it once the pak exits.
stop_script() {
    cat <<'EOF'
pids="$(pidof meshclient)"
if [ -z "$pids" ]; then
    echo "No MeshClient running."
    exit 0
fi
echo "Stopping MeshClient (pid $pids)"
kill -TERM $pids
i=0
while [ $i -lt 20 ] && pidof meshclient >/dev/null; do sleep 0.5; i=$((i + 1)); done
pids="$(pidof meshclient)"
if [ -n "$pids" ]; then
    echo "Still running after 10s; killing pid $pids"
    kill -KILL $pids
    sleep 1
fi
if pidof meshclient >/dev/null; then
    echo "ERROR: meshclient is still running (pid $(pidof meshclient))"
    exit 0
fi
if [ -f /tmp/nextui_exec ]; then
    i=0
    while [ $i -lt 20 ] && ! pidof nextui.elf >/dev/null; do sleep 0.5; i=$((i + 1)); done
    if pidof nextui.elf >/dev/null; then
        echo "Stopped; NextUI is back on screen."
    else
        echo "ERROR: stopped, but NextUI's launcher did not come back within 10s"
    fi
else
    echo "Stopped."
fi
EOF
}

# Hand $1 - a command in /tmp/next's format - to NextUI's launch loop, and wait for meshclient.
start_script() {
    cat <<EOF
if [ ! -f /tmp/nextui_exec ]; then
    echo "ERROR: NextUI's launch loop is not running (no /tmp/nextui_exec), so nothing would run the pak"
    exit 0
fi
i=0
while [ \$i -lt 20 ] && ! pidof nextui.elf >/dev/null; do sleep 0.5; i=\$((i + 1)); done
if ! pidof nextui.elf >/dev/null; then
    echo "ERROR: NextUI's launcher is not running - is a game or another pak open? Exit it first"
    exit 0
fi
printf '%s' $(sq "$1") > /tmp/next
kill -KILL \$(pidof nextui.elf)
i=0
while [ \$i -lt 30 ] && ! pidof meshclient >/dev/null; do sleep 0.5; i=\$((i + 1)); done
if pidof meshclient >/dev/null; then
    echo "STARTED \$(pidof meshclient)"
else
    echo "ERROR: MeshClient did not start within 15s (the launcher comes back on its own); see ${REMOTE_LOG}"
fi
EOF
}

cmd_stop() {
    local out
    out="$(remote_exec "$(stop_script)")"
    printf '%s\n' "${out}"
    [[ ${DRY_RUN} -eq 1 ]] || ! grep -q '^ERROR:' <<<"${out}"
}

cmd_start() {
    local next_cmd arg out
    # nextui.elf's own format for a pak: the quoted path to its launch.sh. Args follow it the way a
    # ROM follows an emulator's path.
    next_cmd="$(sq "${REMOTE_PAK}/launch.sh")"
    for arg in ${PASSTHRU[@]+"${PASSTHRU[@]}"}; do
        next_cmd+=" $(sq "${arg}")"
    done
    # One client at a time: a second one fights the first for the radio link and the panel.
    cmd_stop || die "could not stop the running MeshClient"
    echo "Starting ${PAK_NAME} through NextUI's launch loop, as Tools > ${PAK_NAME} does"
    out="$(remote_exec "$(start_script "${next_cmd}")")"
    if [[ ${DRY_RUN} -eq 1 ]]; then
        printf '%s\n' "${out}"
        return 0
    fi
    grep -q '^STARTED' <<<"${out}" || die "${out#ERROR: }"
    echo "MeshClient is running (pid ${out#STARTED }). End the test with: $0 stop"
}

# Does this invocation make meshclient print something and exit, rather than bring up the UI?
run_is_headless() {
    local arg
    for arg in ${PASSTHRU[@]+"${PASSTHRU[@]}"}; do
        case "${arg}" in
            --list-devices|--status|-s|--status-output|--status-output=*| \
            --send-text|--send-text=*|--fetch-firmware|--fetch-firmware=*| \
            --install-firmware|--install-firmware=*|--version|-V|--help|-h) return 0 ;;
        esac
    done
    return 1
}

# A headless run draws nothing, so it needs no hand-off - and it keeps its output and exit status
# here, which a run through the launch loop cannot. It still stops a running client first unless
# all it does is print a version or the usage: two clients cannot share one radio link.
cmd_run_direct() {
    local arg needs_radio=1
    for arg in ${PASSTHRU[@]+"${PASSTHRU[@]}"}; do
        case "${arg}" in --version|-V|--help|-h) needs_radio=0 ;; esac
    done
    if [[ ${needs_radio} -eq 1 ]]; then
        cmd_stop || die "could not stop the running MeshClient"
    fi
    echo "Running ${REMOTE_PAK}/launch.sh ${PASSTHRU[*]+"${PASSTHRU[*]}"} (Ctrl-C to stop)"
    local remote_cmd="cd $(sq "${REMOTE_PAK}") && exec $(sq "${REMOTE_PAK}/launch.sh")"
    for arg in ${PASSTHRU[@]+"${PASSTHRU[@]}"}; do
        remote_cmd+=" $(sq "${arg}")"
    done
    remote_tty "${remote_cmd}"
}

RUN_STREAM_PID=""

run_cleanup() {
    trap - EXIT INT TERM
    # The stream only ends by itself when the client does, so it has to be ended here.
    if [[ -n "${RUN_STREAM_PID}" ]]; then
        kill "${RUN_STREAM_PID}" 2>/dev/null || true
    fi
    echo
    cmd_stop || true
}

cmd_run() {
    if run_is_headless; then
        cmd_run_direct
        return
    fi
    # Follow from the first line this run writes rather than from the end, so start-up is shown.
    local lines=0
    if [[ ${DRY_RUN} -ne 1 ]]; then
        lines="$(remote_exec "cat $(sq "${REMOTE_LOG}") 2>/dev/null | wc -l" | tr -d '[:space:]')"
    fi
    # Whatever ends this - the client exiting, Ctrl-C, the task being killed - ends the client too.
    trap run_cleanup EXIT
    trap 'exit 130' INT TERM
    cmd_start
    echo "Following ${REMOTE_LOG}; Ctrl-C stops the client."
    # In the background and waited on, not in the foreground: bash defers a trap until the
    # foreground command returns, and this one returns only when the client exits - so a TERM
    # sent to this script alone would never reach run_cleanup. `wait` returns on a trapped signal.
    # stdin is passed on explicitly because an asynchronous command's defaults to /dev/null.
    remote_stream "tail -n +$((${lines:-0} + 1)) -f $(sq "${REMOTE_LOG}") & t=\$!; while pidof meshclient >/dev/null; do sleep 1; done; kill \$t 2>/dev/null; echo 'MeshClient exited.'" <&0 &
    RUN_STREAM_PID=$!
    wait "${RUN_STREAM_PID}" || true
    RUN_STREAM_PID=""
}

cmd_logs() {
    echo "Tailing ${DEV_LABEL}:${REMOTE_LOG} (Ctrl-C to stop)"
    # The logs dir is created by launch.sh on first run; make it so tailing before that just waits.
    remote_stream "mkdir -p $(sq "$(dirname "${REMOTE_LOG}")") && touch $(sq "${REMOTE_LOG}") && tail -n 50 -f $(sq "${REMOTE_LOG}")"
}

cmd_check() {
    local remote_script
    remote_script='
r() { printf "  %-22s %s\n" "$1" "$2"; }
echo "device:"
r kernel "$(uname -r -m 2>/dev/null)"
r uptime "$(cut -d" " -f1 /proc/uptime 2>/dev/null)s"
r mem_free_kb "$(awk "/MemAvailable/ {print \$2}" /proc/meminfo 2>/dev/null)"
echo "storage:"
if grep -q " '"${BRICK_SDCARD}"' " /proc/mounts 2>/dev/null; then r sdcard "mounted at '"${BRICK_SDCARD}"'"; else r sdcard "NOT MOUNTED at '"${BRICK_SDCARD}"'"; fi
if [ -d '"${REMOTE_TOOLS}"' ]; then r tools_dir "'"${REMOTE_TOOLS}"'"; else r tools_dir "missing '"${REMOTE_TOOLS}"'"; fi
if [ -x '"${REMOTE_PAK}"'/bin/shared/meshclient ]; then
  r pak_installed "yes"
  r pak_sha256 "$(sha256sum '"${REMOTE_PAK}"'/bin/shared/meshclient 2>/dev/null | cut -d" " -f1)"
else
  r pak_installed "no"
fi
if [ -f '"${REMOTE_LOG}"' ]; then r log_lines "$(wc -l < '"${REMOTE_LOG}"')"; else r log_lines "no log yet"; fi
echo "bluetooth:"
if pidof bluetoothd >/dev/null 2>&1; then r bluetoothd "running (pid $(pidof bluetoothd))"; else r bluetoothd "NOT RUNNING"; fi
if [ -S /var/run/dbus/system_bus_socket ]; then r dbus_socket "/var/run/dbus/system_bus_socket"; else r dbus_socket "MISSING /var/run/dbus/system_bus_socket"; fi
if pidof dbus-daemon >/dev/null 2>&1; then r dbus_daemon "running"; else r dbus_daemon "NOT RUNNING"; fi
adapters="$(ls /sys/class/bluetooth 2>/dev/null | tr "\n" " ")"
r hci_adapters "${adapters:-none}"
for h in $adapters; do
  [ -f /sys/class/bluetooth/$h/address ] && r "${h}_address" "$(cat /sys/class/bluetooth/$h/address)"
done
if command -v bluetoothctl >/dev/null 2>&1; then r bluetoothctl "$(command -v bluetoothctl)"; else r bluetoothctl "not found"; fi
echo "display:"
if [ -c /dev/fb0 ]; then r fb0 "present"; else r fb0 "MISSING"; fi
[ -r /sys/class/graphics/fb0/virtual_size ] && r fb0_virtual_size "$(cat /sys/class/graphics/fb0/virtual_size)"
[ -r /sys/class/graphics/fb0/bits_per_pixel ] && r fb0_bpp "$(cat /sys/class/graphics/fb0/bits_per_pixel)"
echo "network:"
r ip "$(ip -4 -o addr show 2>/dev/null | awk "!/ lo /{print \$4}" | tr "\n" " ")"
'
    echo "Checking ${DEV_LABEL} (${TRANSPORT})"
    remote_exec "${remote_script}"
}

# Screenshot or film the device's screen by reading its framebuffer.
#
# NextUI's own screenshot shortcut lives inside minarch and captures that process's GL surface,
# so it cannot see a pak like ours drawing straight to /dev/fb0. Reading fb0 catches whatever is
# actually on the panel - our HUD, the launcher, a crash - and needs nothing on the device
# beyond the transport already in use.
#
# fb0 on a Brick is 1024x16384: a stack of 768-row pages the display engine flips between. The
# fb backend draws page 0 and mirrors into page 1 (see src/ui/backends/fb.c), so page 0 is what
# MeshClient drew; -P 1 is for catching something else, like the launcher.
#
# Encoding is scripts/frames.py's job, out of the Python standard library, so a stock macOS host
# needs neither Pillow nor ffmpeg. The off-screen renderer behind `scripts/ui-capture.sh` hands
# it the same pixels, so a frame off the device and a frame off a build host go through one
# encoder.

# Geometry from the device rather than hardcoded; `check` reads the same two files. Sets
# FB_WIDTH / FB_HEIGHT / FB_PAGE_BYTES, and FB_HAS_GZIP for the ssh path that streams (the adb
# path writes to a device file and pulls it, so it never gzips).
fb_geometry() {
    local probe="cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel;"
    probe+=" command -v gzip >/dev/null 2>&1 && echo gzip || echo raw"

    local width bpp
    if [[ ${DRY_RUN} -eq 1 ]]; then
        remote_exec "${probe}"
        width=1024
        bpp=32
        FB_HAS_GZIP=0
    else
        local reply
        reply="$(remote_exec "${probe}" | tr '\n' ' ')"
        # "1024,16384", "32" and "gzip" on their own lines; splitting on whitespace beats
        # trimming a trailing newline out of a suffix match.
        local fields=(${reply})
        width="${fields[0]%%,*}"
        bpp="${fields[1]:-}"
        if [[ "${TRANSPORT}" == "adb" ]]; then
            FB_HAS_GZIP=0
        else
            [[ "${fields[2]:-raw}" == "gzip" ]] && FB_HAS_GZIP=1 || FB_HAS_GZIP=0
        fi
    fi
    [[ "${bpp}" == "32" ]] || die "fb0 reports '${bpp}' bits per pixel; only 32 is converted"

    FB_WIDTH="${width}"
    FB_HEIGHT="${BRICK_FB_HEIGHT:-768}"
    FB_PAGE_BYTES=$((FB_WIDTH * 4 * FB_HEIGHT))
}

# Reads `count` consecutive frames of page `page` into `dest`. ssh streams the frames back down
# one connection; adb writes them to a device file and pulls it (its shell stdout is not binary
# safe), which the fast USB link makes cheap.
fb_read_pages() {
    local dest="$1" count="$2" page="$3" interval="$4"
    # A row per block rather than a page per block: busybox dd stops at the first short read, and
    # a page-sized read() off a device file is not guaranteed to come back whole. One short frame
    # desynchronises every frame after it, and the only symptom is a clip that looks like static.
    local row_bytes=$((FB_WIDTH * 4))
    local loop="i=0; while [ \$i -lt ${count} ]; do"
    loop+=" dd if=/dev/fb0 bs=${row_bytes} skip=$((page * FB_HEIGHT)) count=${FB_HEIGHT} 2>/dev/null;"
    [[ "${interval}" != "0" ]] && loop+=" sleep ${interval};"
    loop+=" i=\$((i+1)); done"

    if [[ "${TRANSPORT}" == "adb" ]]; then
        local devcap="/tmp/mc-fbcap"
        local remote_script="{ ${loop} ; } > ${devcap}"
        if [[ ${DRY_RUN} -eq 1 ]]; then
            remote_exec "${remote_script}"
            remote_pull_file "${devcap}" "${dest}"
            remote_exec "rm -f ${devcap}"
            return 0
        fi
        remote_exec "${remote_script}" >/dev/null
        remote_pull_file "${devcap}" "${dest}"
        remote_exec "rm -f ${devcap}" >/dev/null
    else
        local remote_script="${loop}"
        [[ ${FB_HAS_GZIP} -eq 1 ]] && remote_script="{ ${loop} ; } | gzip -1"
        if [[ ${DRY_RUN} -eq 1 ]]; then
            remote_exec "${remote_script}"
            return 0
        fi
        if [[ ${FB_HAS_GZIP} -eq 1 ]]; then
            ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}" | gzip -dc > "${dest}"
        else
            ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}" > "${dest}"
        fi
    fi

    local read_bytes expected=$((FB_PAGE_BYTES * count))
    read_bytes="$(wc -c < "${dest}" | tr -d ' ')"
    [[ "${read_bytes}" == "${expected}" ]] ||
        die "read ${read_bytes} bytes of ${expected}; can the device read /dev/fb0?"
}

cmd_shot() {
    local out="" delay=0 count=1 page=0 downscale=1
    local args=(${PASSTHRU[@]+"${PASSTHRU[@]}"})
    local i=0
    while [[ ${i} -lt ${#args[@]} ]]; do
        case "${args[${i}]}" in
            -o|--out) out="${args[$((i + 1))]:-}"; i=$((i + 2)) ;;
            -d|--delay) delay="${args[$((i + 1))]:-0}"; i=$((i + 2)) ;;
            -n|--count) count="${args[$((i + 1))]:-1}"; i=$((i + 2)) ;;
            -P|--page) page="${args[$((i + 1))]:-0}"; i=$((i + 2)) ;;
            -s|--downscale) downscale="${args[$((i + 1))]:-1}"; i=$((i + 2)) ;;
            *) die "shot: unknown argument: ${args[${i}]} (-o FILE, -d SECS, -n COUNT, -P PAGE, -s N)" ;;
        esac
    done
    [[ -n "${out}" ]] || out="shot-$(date +%Y%m%d-%H%M%S).png"
    [[ "${out}" == /* ]] || out="${INVOKE_DIR}/${out}"
    command -v python3 >/dev/null 2>&1 || die "shot needs python3 on the host to write the PNG"

    fb_geometry
    echo "fb0 ${FB_WIDTH}x${FB_HEIGHT} @ 32bpp, page ${page}"

    local raw shot_index target_out
    raw="$(mktemp)"
    trap 'rm -f "${raw}"' RETURN
    for ((shot_index = 1; shot_index <= count; shot_index++)); do
        if [[ "${delay}" != "0" ]]; then
            echo "shot ${shot_index}/${count} in ${delay}s - set the screen up now..."
            sleep "${delay}"
        fi
        if [[ ${count} -gt 1 ]]; then
            target_out="${out%.png}-${shot_index}.png"
        else
            target_out="${out}"
        fi

        fb_read_pages "${raw}" 1 "${page}" 0
        if [[ ${DRY_RUN} -eq 1 ]]; then
            printf '  ... > %s\n' "${target_out}"
            continue
        fi
        python3 "${REPO_ROOT}/scripts/frames.py" png --raw "${FB_WIDTH}x${FB_HEIGHT}" \
            --downscale "${downscale}" --out "${target_out}" "${raw}"
    done
}

# Film the screen: the same page read over and over, encoded as a GIF.
#
# The frame rate is whatever the device and the link manage - a page is 3 MB - so what comes back
# is not real time. -r sets how fast it plays back rather than how fast it was shot; slow it down
# when the capture was slow. (Over USB the capture is much faster than over WiFi.)
cmd_clip() {
    local out="" delay=0 count=24 page=0 downscale=2 rate=200 interval=0
    local args=(${PASSTHRU[@]+"${PASSTHRU[@]}"})
    local i=0
    while [[ ${i} -lt ${#args[@]} ]]; do
        case "${args[${i}]}" in
            -o|--out) out="${args[$((i + 1))]:-}"; i=$((i + 2)) ;;
            -d|--delay) delay="${args[$((i + 1))]:-0}"; i=$((i + 2)) ;;
            -n|--count) count="${args[$((i + 1))]:-24}"; i=$((i + 2)) ;;
            -P|--page) page="${args[$((i + 1))]:-0}"; i=$((i + 2)) ;;
            -s|--downscale) downscale="${args[$((i + 1))]:-2}"; i=$((i + 2)) ;;
            -r|--rate) rate="${args[$((i + 1))]:-200}"; i=$((i + 2)) ;;
            -i|--interval) interval="${args[$((i + 1))]:-0}"; i=$((i + 2)) ;;
            *) die "clip: unknown argument: ${args[${i}]} (-o FILE, -d SECS, -n COUNT, -P PAGE, -s N, -r MS, -i SECS)" ;;
        esac
    done
    [[ -n "${out}" ]] || out="clip-$(date +%Y%m%d-%H%M%S).gif"
    [[ "${out}" == /* ]] || out="${INVOKE_DIR}/${out}"
    command -v python3 >/dev/null 2>&1 || die "clip needs python3 on the host to write the GIF"

    fb_geometry
    echo "fb0 ${FB_WIDTH}x${FB_HEIGHT} @ 32bpp, page ${page}, ${count} frames"

    if [[ "${delay}" != "0" ]]; then
        echo "recording in ${delay}s - get to the screen you want..."
        sleep "${delay}"
    fi

    local raw
    raw="$(mktemp)"
    trap 'rm -f "${raw}"' RETURN
    fb_read_pages "${raw}" "${count}" "${page}" "${interval}"
    if [[ ${DRY_RUN} -eq 1 ]]; then
        printf '  ... > %s\n' "${out}"
        return 0
    fi
    python3 "${REPO_ROOT}/scripts/frames.py" gif --raw "${FB_WIDTH}x${FB_HEIGHT}" \
        --downscale "${downscale}" --delay "${rate}" --out "${out}" "${raw}"
}

# Set by cmd_input_map once the device is holding readers for us, and read by the trap below.
INPUT_MAP_RECORDING=0
INPUT_MAP_DIR=""
INPUT_MAP_STOP=""

# Ctrl-C between starting the readers and collecting from them. Without this the device keeps
# four `cat` processes writing into /tmp, and an ordinary cancellation leaks them until the next
# run or a reboot.
input_map_abort() {
    trap - INT TERM
    if [[ ${INPUT_MAP_RECORDING} -eq 1 ]]; then
        echo
        echo "Interrupted; stopping the readers on the device."
        remote_exec "${INPUT_MAP_STOP}; rm -rf ${INPUT_MAP_DIR}" >/dev/null 2>&1 || true
        INPUT_MAP_RECORDING=0
    fi
    exit 130
}

# Which button is which, measured rather than assumed.
#
# Two halves, because a press can only answer one of them: the capability bitmaps say what a
# device *can* emit - an absent code and an unpressed button look identical in a capture - and
# the capture says which physical button emits what. The Brick needs both, because its pad
# declares a KEY_F1/KEY_F2 and a pair of volume keys that it never actually sends.
#
# Nothing is grabbed (no EVIOCGRAB anywhere), so whatever is on screen keeps its input and the
# recording is invisible to it. `cat` rather than a formatter on the device: busybox hexdump
# block-buffers into a pipe, so a live read would arrive thousands of events late.
cmd_input_map() {
    # The prompt tells the user how long to pause and the decoder splits on it; one constant so
    # the two cannot drift.
    local INPUT_MAP_GAP=0.45
    local seconds="" keep=0
    local args=(${PASSTHRU[@]+"${PASSTHRU[@]}"})
    local i=0
    while [[ ${i} -lt ${#args[@]} ]]; do
        case "${args[${i}]}" in
            -t|--time) seconds="${args[$((i + 1))]:-}"; i=$((i + 2)) ;;
            -k|--keep) keep=1; i=$((i + 1)) ;;
            *) die "input-map: unknown argument: ${args[${i}]} (-t SECS, -k)" ;;
        esac
    done

    command -v python3 >/dev/null 2>&1 || die "input-map needs python3 on the host to decode"

    INPUT_MAP_DIR="/tmp/meshclient-inputmap"
    local pid_file="${INPUT_MAP_DIR}/readers.pid"

    # Readers are stopped by pid, never by pattern.
    #
    # This was `pkill -f 'cat /dev/input/event'`, and it silently did nothing: the device has no
    # pkill, and the error went to /dev/null with everything else. The symptom was a reader count
    # that climbed 5, 9, 13 across runs - four leaked `cat`s per invocation, each holding an fd to
    # a file the next run had already unlinked. A pattern is the wrong tool here even where pkill
    # exists, because the remote shell's own command line contains the pattern and `-f` matches
    # the shell that is running the kill.
    local start_script="[ -f ${pid_file} ] && kill \$(cat ${pid_file}) 2>/dev/null;"
    start_script+=" rm -rf ${INPUT_MAP_DIR}; mkdir -p ${INPUT_MAP_DIR};"
    start_script+=" for d in /dev/input/event*; do"
    # The subshell is what detaches the reader, so it outlives this session; $! inside it is the
    # cat's own pid, which is the whole point of writing it from in there.
    start_script+=" (cat \"\$d\" > ${INPUT_MAP_DIR}/\"\$(basename \"\$d\")\".bin 2>/dev/null &"
    start_script+=" echo \$! >> ${pid_file}); done;"
    start_script+=" sleep 1; ls ${INPUT_MAP_DIR} | grep '\\.bin$' | tr '\\n' ' '"

    INPUT_MAP_STOP="kill \$(cat ${pid_file} 2>/dev/null) 2>/dev/null; sleep 1"

    if [[ ${DRY_RUN} -eq 1 ]]; then
        remote_exec "${start_script}"
        remote_exec "${INPUT_MAP_STOP}"
        remote_pull_dir "${INPUT_MAP_DIR}" "<tmp>"
        return 0
    fi

    echo "Recording: $(remote_exec "${start_script}")"
    # From here the device is holding four processes of ours, and every way out has to end them:
    # a Ctrl-C at the prompt below, or during the timed sleep, otherwise leaves them writing to
    # /tmp on the device until the next run or a reboot.
    INPUT_MAP_RECORDING=1
    trap input_map_abort INT TERM
    echo
    echo "Press the buttons you want to identify, ONE AT A TIME, about a second apart."
    echo "The pause is what separates them: a gap of ${INPUT_MAP_GAP}s ends a press."
    if [[ -n "${seconds}" ]]; then
        echo "Recording for ${seconds}s."
        sleep "${seconds}"
    else
        echo
        read -r -p "Press Enter here when you are done. " _
    fi

    local capture
    capture="$(mktemp -d)"
    remote_exec "${INPUT_MAP_STOP}" >/dev/null
    remote_pull_dir "${INPUT_MAP_DIR}" "${capture}"
    INPUT_MAP_RECORDING=0
    trap - INT TERM
    remote_exec "rm -rf ${INPUT_MAP_DIR}" >/dev/null || true

    echo
    echo "== what each device can emit =="
    remote_exec "cat /proc/bus/input/devices" | python3 "${REPO_ROOT}/scripts/input-map.py" caps

    echo
    echo "== what you pressed =="
    # An empty capture is a real answer ("you pressed nothing this device can see"), so the
    # decoder's non-zero status is passed on - but the temporary directory is cleaned up first,
    # which set -e would otherwise skip straight past.
    local status=0
    python3 "${REPO_ROOT}/scripts/input-map.py" presses "${capture}" --gap "${INPUT_MAP_GAP}" ||
        status=$?

    if [[ ${keep} -eq 1 ]]; then
        echo "raw capture kept in ${capture}"
    else
        rm -rf "${capture}"
    fi
    return ${status}
}

cmd_shell() {
    remote_tty
}

cmd_setup_key() {
    [[ "${TRANSPORT}" == "ssh" ]] || die "setup-key is SSH only; the adb (USB) transport needs no key"
    local pub
    pub="$(ls ~/.ssh/id_ed25519.pub ~/.ssh/id_rsa.pub 2>/dev/null | head -n1 || true)"
    [[ -n "${pub}" ]] || die "no public key in ~/.ssh (id_ed25519.pub or id_rsa.pub). Run ssh-keygen -t ed25519."
    echo "Installing ${pub} for ${TARGET} (you will be asked for the device password once)"
    local remote_script
    remote_script='mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh" && cat >> "$HOME/.ssh/authorized_keys" && chmod 600 "$HOME/.ssh/authorized_keys" && echo "key installed in $HOME/.ssh/authorized_keys"'
    if [[ ${DRY_RUN} -eq 1 ]]; then
        printf 'cat %s | ' "${pub}"
        remote_exec "${remote_script}"
        return 0
    fi
    ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}" < "${pub}"
    echo "If the key does not persist across reboots, the root filesystem is read-only;"
    echo "see docs/device.md for the SSH Server pak's authorized_keys location."
}

case "${COMMAND}" in
    push) cmd_push ;;
    start) cmd_start ;;
    stop) cmd_stop || die "stop failed" ;;
    run) cmd_run ;;
    logs) cmd_logs ;;
    check) cmd_check ;;
    shot) cmd_shot ;;
    clip) cmd_clip ;;
    input-map) cmd_input_map ;;
    shell) cmd_shell ;;
    setup-key) cmd_setup_key ;;
    *) die "unknown command: ${COMMAND} (see --help)" ;;
esac
