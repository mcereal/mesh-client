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
#                      Args: -o FILE, -d SECS (delay before each), -n COUNT, -P PAGE.
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
echo "helpers:"
for b in minui-list minui-presenter minui-keyboard; do
  p="$(command -v $b 2>/dev/null || ls '"${REMOTE_TOOLS}"'/*/bin/*/$b 2>/dev/null | head -n1)"
  r "$b" "${p:-not found}"
done
echo "network:"
r ip "$(ip -4 -o addr show 2>/dev/null | awk "!/ lo /{print \$4}" | tr "\n" " ")"
'
    echo "Checking ${TARGET}"
    ssh_cmd "${remote_script}"
}

# Screenshot the device's screen by reading its framebuffer.
#
# NextUI's own screenshot shortcut lives inside minarch and captures that process's GL surface,
# so it cannot see a pak like ours drawing straight to /dev/fb0. Reading fb0 catches whatever is
# actually on the panel - our HUD, the launcher, a crash - and needs nothing on the device
# beyond the SSH server that is already there.
#
# fb0 on a Brick is 1024x16384: a stack of 768-row pages the display engine flips between. The
# fb backend draws page 0 and mirrors into page 1 (see src/ui/backends/fb.c), so page 0 is what
# MeshClient drew; -P 1 is for catching something else, like the launcher.
cmd_shot() {
    local out="" delay=0 count=1 page=0
    local args=(${PASSTHRU[@]+"${PASSTHRU[@]}"})
    local i=0
    while [[ ${i} -lt ${#args[@]} ]]; do
        case "${args[${i}]}" in
            -o|--out) out="${args[$((i + 1))]:-}"; i=$((i + 2)) ;;
            -d|--delay) delay="${args[$((i + 1))]:-0}"; i=$((i + 2)) ;;
            -n|--count) count="${args[$((i + 1))]:-1}"; i=$((i + 2)) ;;
            -P|--page) page="${args[$((i + 1))]:-0}"; i=$((i + 2)) ;;
            *) die "shot: unknown argument: ${args[${i}]} (-o FILE, -d SECS, -n COUNT, -P PAGE)" ;;
        esac
    done
    [[ -n "${out}" ]] || out="shot-$(date +%Y%m%d-%H%M%S).png"
    [[ "${out}" == /* ]] || out="${INVOKE_DIR}/${out}"
    command -v python3 >/dev/null 2>&1 || die "shot needs python3 on the host to write the PNG"

    # Geometry from the device rather than hardcoded; `check` reads the same two files.
    local geometry width bpp
    if [[ ${DRY_RUN} -eq 1 ]]; then
        ssh_cmd "cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel"
        width=1024
        bpp=32
    else
        geometry="$(ssh "${SSH_OPTS[@]}" "${TARGET}" \
            "cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel" |
            tr '\n' ' ')"
        # "1024,16384" and "32" on their own lines; splitting on whitespace beats trimming
        # a trailing newline out of a suffix match.
        local fields=(${geometry})
        width="${fields[0]%%,*}"
        bpp="${fields[1]:-}"
    fi
    [[ "${bpp}" == "32" ]] || die "fb0 reports '${bpp}' bits per pixel; shot only converts 32"
    local height="${BRICK_FB_HEIGHT:-768}"
    local page_bytes=$((width * 4 * height))
    echo "fb0 ${width}x${height} @ ${bpp}bpp, page ${page}"

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

        local remote_script="dd if=/dev/fb0 bs=${page_bytes} skip=${page} count=1 2>/dev/null"
        if [[ ${DRY_RUN} -eq 1 ]]; then
            ssh_cmd "${remote_script}"
            printf '  ... > %s\n' "${target_out}"
            continue
        fi
        ssh "${SSH_OPTS[@]}" "${TARGET}" "${remote_script}" > "${raw}"

        local read_bytes
        read_bytes="$(wc -c < "${raw}" | tr -d ' ')"
        [[ "${read_bytes}" == "${page_bytes}" ]] ||
            die "read ${read_bytes} bytes of ${page_bytes}; can ${BRICK_USER} read /dev/fb0?"

        # Little-endian XRGB8888 (B,G,R,X in memory) to a PNG, with nothing but the Python
        # standard library so a stock macOS host needs no Pillow and no ffmpeg.
        MESH_SHOT_W="${width}" MESH_SHOT_H="${height}" MESH_SHOT_RAW="${raw}" \
            MESH_SHOT_OUT="${target_out}" python3 - <<'PYSHOT'
import os, struct, zlib

width = int(os.environ["MESH_SHOT_W"])
height = int(os.environ["MESH_SHOT_H"])
out = os.environ["MESH_SHOT_OUT"]
raw = open(os.environ["MESH_SHOT_RAW"], "rb").read()
stride = width * 4

rows = bytearray()
for y in range(height):
    row = raw[y * stride:(y + 1) * stride]
    rows.append(0)  # PNG filter type 0 (none), one byte per scanline
    pixels = bytearray(width * 3)
    pixels[0::3] = row[2::4]  # R
    pixels[1::3] = row[1::4]  # G
    pixels[2::3] = row[0::4]  # B
    rows += pixels


def chunk(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload +
            struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
png += chunk(b"IEND", b"")
with open(out, "wb") as handle:
    handle.write(png)
print(f"wrote {out} ({width}x{height})")
PYSHOT
    done
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
    shell) cmd_shell ;;
    setup-key) cmd_setup_key ;;
    *) die "unknown command: ${COMMAND} (see --help)" ;;
esac
