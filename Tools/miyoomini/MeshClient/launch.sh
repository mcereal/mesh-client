#!/bin/sh
# Onion OS launcher for the Miyoo Mini / Mini Plus. Lives at /mnt/SDCARD/App/MeshClient/.
#
# The panel is mounted upside down (FB_ROTATE=180) and the pad reports keyboard codes, which
# the `miyoo` input profile maps. MENU (KEY_ESC) leaves. There is no Bluetooth on this device,
# so a radio is reached over USB serial or TCP.
APP_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="$APP_DIR/data"
LOG_FILE="$APP_DIR/MeshClient.txt"
mkdir -p "$DATA_DIR"

printf '[%s] Launching MeshClient\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" >>"$LOG_FILE"
{
    echo "--- device"
    uname -a
    cat /proc/cpuinfo | grep -E 'Hardware|model name' | head -2
    cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null
    for d in /sys/class/input/event*; do echo "$d: $(cat "$d/device/name" 2>/dev/null)"; done
    ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
    echo "---"
} >>"$LOG_FILE" 2>&1

export HOME="$DATA_DIR"
export MESHCLIENT_UI_BACKEND=fb
export MESHCLIENT_FB_ROTATE=180
export MESHCLIENT_INPUT_PROFILE=miyoo
export MESHCLIENT_DISABLE_BLE=1
# Per-device settings that are not the pak's to decide - MESHCLIENT_TCP_HOST, MESHCLIENT_PROTOCOL,
# MESHCLIENT_LOG_LEVEL - go in env.sh beside this file. `set -a` exports every assignment in it,
# so a plain `MESHCLIENT_TCP_HOST=radio:5000` reaches the client as well as an `export` does.
if [ -f "$APP_DIR/env.sh" ]; then
    set -a
    . "$APP_DIR/env.sh"
    set +a
fi

cd "$APP_DIR"
"$APP_DIR/meshclient" --foreground --log-level "${MESHCLIENT_LOG_LEVEL:-debug}" >>"$LOG_FILE" 2>&1
STATUS=$?
printf '[%s] MeshClient exited with status %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$STATUS" >>"$LOG_FILE"
# The client's status, not printf's: Onion and a hand-run launch both read this one.
exit "$STATUS"
