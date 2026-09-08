#!/usr/bin/env bash
# Push, run, and inspect MeshClient.pak on a TrimUI Brick over SSH.
#
# Runs on the development host (macOS or Linux). The device side needs NextUI with WiFi
# configured and the "SSH Server" pak (dropbear) from the Pak Store running; see
# docs/device.md for the one-time setup. Only busybox tools are assumed on the device:
# no rsync, no scp needed — transfers go through `tar | ssh tar`.
#
# Usage: scripts/deploy-device.sh [options] <command> [-- args...]
#
# Commands:
#   push               Copy dist/MeshClient.pak to <sdcard>/Tools/<platform>/MeshClient.pak (default)
#   run [-- args]      Run launch.sh on the device in the foreground, streaming output.
#                      Extra args go to meshclient, e.g. `run -- --list-devices`.
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
#   setup-key          Install ~/.ssh/id_*.pub into the device's authorized_keys (asks password once)
#
# Options:
#   -H, --host HOST    Device IP or hostname            (env BRICK_HOST)
#   -u, --user USER    SSH user, default root           (env BRICK_USER)
#   -p, --port PORT    SSH port, default 22             (env BRICK_PORT)
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
BRICK_VARS=(BRICK_HOST BRICK_USER BRICK_PORT BRICK_PLATFORM BRICK_SDCARD BRICK_SSH_OPTS)
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

BRICK_HOST="${BRICK_HOST:-}"
BRICK_USER="${BRICK_USER:-root}"
BRICK_PORT="${BRICK_PORT:-22}"
BRICK_PLATFORM="${BRICK_PLATFORM:-tg5040}"
BRICK_SDCARD="${BRICK_SDCARD:-/mnt/SDCARD}"
BRICK_SSH_OPTS="${BRICK_SSH_OPTS:--o StrictHostKeyChecking=accept-new -o ConnectTimeout=5}"
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
        -H|--host) BRICK_HOST="$2"; shift 2 ;;
        -u|--user) BRICK_USER="$2"; shift 2 ;;
        -p|--port) BRICK_PORT="$2"; shift 2 ;;
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

[[ -n "${BRICK_HOST}" ]] || die "no device host. Set BRICK_HOST in .brick.env or pass --host (see docs/device.md)"

TARGET="${BRICK_USER}@${BRICK_HOST}"
REMOTE_TOOLS="${BRICK_SDCARD}/Tools/${BRICK_PLATFORM}"
REMOTE_PAK="${REMOTE_TOOLS}/${PAK_NAME}.pak"
REMOTE_LOG="${BRICK_SDCARD}/.userdata/${BRICK_PLATFORM}/logs/${PAK_NAME}.txt"

# shellcheck disable=SC2206
SSH_OPTS=(${BRICK_SSH_OPTS} -p "${BRICK_PORT}")

# Run a command string on the device. ssh joins its arguments and hands the string to
# the remote login shell (busybox sh), so multi-line scripts work as-is.
ssh_cmd() {
    if [[ ${DRY_RUN} -eq 1 ]]; then
        printf 'ssh %s %s %s\n' "${SSH_OPTS[*]}" "${TARGET}" "$(sq "$1")"
        return 0
    fi
    ssh "${SSH_OPTS[@]}" "${TARGET}" "$1"
}

ssh_tty() {
    if [[ ${DRY_RUN} -eq 1 ]]; then
        printf 'ssh -t %s %s %s\n' "${SSH_OPTS[*]}" "${TARGET}" "${1:-}"
        return 0
    fi
    if [[ -n "${1:-}" ]]; then
        ssh -t "${SSH_OPTS[@]}" "${TARGET}" "$1"
    else
        ssh -t "${SSH_OPTS[@]}" "${TARGET}"
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
    echo "Pushing ${LOCAL_PAK} -> ${TARGET}:${REMOTE_PAK}"
    echo "  meshclient sha256 ${local_sum}"

    # Stage into MeshClient.pak.new, then swap, so a half-finished transfer never
    # leaves a broken pak in Tools/ that NextUI would try to launch.
    local remote_script
    remote_script="set -e
mkdir -p $(sq "${REMOTE_TOOLS}")
rm -rf $(sq "${REMOTE_PAK}.new")
mkdir $(sq "${REMOTE_PAK}.new")
tar -C $(sq "${REMOTE_PAK}.new") -xf -
rm -rf $(sq "${REMOTE_PAK}")
mv $(sq "${REMOTE_PAK}.new") $(sq "${REMOTE_PAK}")
chmod +x $(sq "${REMOTE_PAK}/launch.sh") $(sq "${REMOTE_PAK}/bin/shared/meshclient")
sync
sha256sum $(sq "${REMOTE_PAK}/bin/shared/meshclient") 2>/dev/null | cut -d' ' -f1"

    if [[ ${DRY_RUN} -eq 1 ]]; then
        printf 'tar -C %s -cf - . | ' "${LOCAL_PAK}"
        ssh_cmd "${remote_script}"
        return 0
    fi

    local remote_sum
    remote_sum="$(tar -C "${LOCAL_PAK}" -cf - . | ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}")"
    if [[ -n "${remote_sum}" && "${remote_sum}" != "${local_sum}" ]]; then
        die "checksum mismatch after push (device ${remote_sum})"
    fi
    echo "Deployed. Launch it from Tools > ${PAK_NAME} on the device, or: $0 run"
}

cmd_run() {
    echo "Running ${REMOTE_PAK}/launch.sh ${PASSTHRU[*]+"${PASSTHRU[*]}"} (Ctrl-C to stop)"
    echo "Note: launch.sh forces --foreground and the fb backend; the NextUI launcher may repaint over it."
    local remote_cmd="cd $(sq "${REMOTE_PAK}") && exec $(sq "${REMOTE_PAK}/launch.sh")"
    local arg
    for arg in ${PASSTHRU[@]+"${PASSTHRU[@]}"}; do
        remote_cmd+=" $(sq "${arg}")"
    done
    ssh_tty "${remote_cmd}"
}

cmd_logs() {
    echo "Tailing ${TARGET}:${REMOTE_LOG} (Ctrl-C to stop)"
    # The logs dir is created by launch.sh on first run; make it so tailing before that just waits.
    ssh_cmd "mkdir -p $(sq "$(dirname "${REMOTE_LOG}")") && touch $(sq "${REMOTE_LOG}") && tail -n 50 -f $(sq "${REMOTE_LOG}")"
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
    echo "Checking ${TARGET}"
    ssh_cmd "${remote_script}"
}

# Screenshot or film the device's screen by reading its framebuffer.
#
# NextUI's own screenshot shortcut lives inside minarch and captures that process's GL surface,
# so it cannot see a pak like ours drawing straight to /dev/fb0. Reading fb0 catches whatever is
# actually on the panel - our HUD, the launcher, a crash - and needs nothing on the device
# beyond the SSH server that is already there.
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
# FB_WIDTH / FB_HEIGHT / FB_PAGE_BYTES, and FB_HAS_GZIP for the callers that stream.
fb_geometry() {
    local probe="cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel;"
    probe+=" command -v gzip >/dev/null 2>&1 && echo gzip || echo raw"

    local width bpp
    if [[ ${DRY_RUN} -eq 1 ]]; then
        ssh_cmd "${probe}"
        width=1024
        bpp=32
        FB_HAS_GZIP=0
    else
        local reply
        reply="$(ssh "${SSH_OPTS[@]}" "${TARGET}" "${probe}" | tr '\n' ' ')"
        # "1024,16384", "32" and "gzip" on their own lines; splitting on whitespace beats
        # trimming a trailing newline out of a suffix match.
        local fields=(${reply})
        width="${fields[0]%%,*}"
        bpp="${fields[1]:-}"
        [[ "${fields[2]:-raw}" == "gzip" ]] && FB_HAS_GZIP=1 || FB_HAS_GZIP=0
    fi
    [[ "${bpp}" == "32" ]] || die "fb0 reports '${bpp}' bits per pixel; only 32 is converted"

    FB_WIDTH="${width}"
    FB_HEIGHT="${BRICK_FB_HEIGHT:-768}"
    FB_PAGE_BYTES=$((FB_WIDTH * 4 * FB_HEIGHT))
}

# Reads `count` consecutive pages of page `page` into `dest`, through one SSH connection.
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

    local remote_script="${loop}"
    if [[ ${FB_HAS_GZIP} -eq 1 ]]; then
        remote_script="{ ${loop} ; } | gzip -1"
    fi

    if [[ ${DRY_RUN} -eq 1 ]]; then
        ssh_cmd "${remote_script}"
        return 0
    fi

    if [[ ${FB_HAS_GZIP} -eq 1 ]]; then
        ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}" | gzip -dc > "${dest}"
    else
        ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}" > "${dest}"
    fi

    local read_bytes expected=$((FB_PAGE_BYTES * count))
    read_bytes="$(wc -c < "${dest}" | tr -d ' ')"
    [[ "${read_bytes}" == "${expected}" ]] ||
        die "read ${read_bytes} bytes of ${expected}; can ${BRICK_USER} read /dev/fb0?"
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

# Film the screen: the same page read over and over down one SSH connection, encoded as a GIF.
#
# The frame rate is whatever the device and the link manage - a page is 3 MB, and a Brick over
# WiFi gets a handful of frames a second - so what comes back is not real time. -r sets how fast
# it plays back rather than how fast it was shot; slow it down when the capture was slow.
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

    local remote_dir="/tmp/meshclient-inputmap"
    local start_script="pkill -f 'cat /dev/input/event' 2>/dev/null;"
    start_script+=" rm -rf ${remote_dir}; mkdir -p ${remote_dir};"
    start_script+=" for d in /dev/input/event*; do"
    start_script+=" (cat \"\$d\" > ${remote_dir}/\"\$(basename \"\$d\")\".bin 2>/dev/null &); done;"
    start_script+=" sleep 1; ls ${remote_dir} | tr '\\n' ' '"

    if [[ ${DRY_RUN} -eq 1 ]]; then
        ssh_cmd "${start_script}"
        ssh_cmd "pkill -f 'cat /dev/input/event'; tar cf - -C ${remote_dir} ."
        return 0
    fi

    echo "Recording: $(ssh_cmd "${start_script}")"
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
    ssh_cmd "pkill -f 'cat /dev/input/event' 2>/dev/null; sleep 1; tar cf - -C ${remote_dir} ." |
        tar xf - -C "${capture}"
    ssh_cmd "rm -rf ${remote_dir}" || true

    echo
    echo "== what each device can emit =="
    ssh_cmd "cat /proc/bus/input/devices" | python3 "${REPO_ROOT}/scripts/input-map.py" caps

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
    ssh_tty
}

cmd_setup_key() {
    local pub
    pub="$(ls ~/.ssh/id_ed25519.pub ~/.ssh/id_rsa.pub 2>/dev/null | head -n1 || true)"
    [[ -n "${pub}" ]] || die "no public key in ~/.ssh (id_ed25519.pub or id_rsa.pub). Run ssh-keygen -t ed25519."
    echo "Installing ${pub} for ${TARGET} (you will be asked for the device password once)"
    local remote_script
    remote_script='mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh" && cat >> "$HOME/.ssh/authorized_keys" && chmod 600 "$HOME/.ssh/authorized_keys" && echo "key installed in $HOME/.ssh/authorized_keys"'
    if [[ ${DRY_RUN} -eq 1 ]]; then
        printf 'cat %s | ' "${pub}"
        ssh_cmd "${remote_script}"
        return 0
    fi
    ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}" < "${pub}"
    echo "If the key does not persist across reboots, the root filesystem is read-only;"
    echo "see docs/device.md for the SSH Server pak's authorized_keys location."
}

case "${COMMAND}" in
    push) cmd_push ;;
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
