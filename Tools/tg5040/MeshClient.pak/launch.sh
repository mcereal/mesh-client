#!/bin/sh
set -eu

PAK_DIR="$(dirname "$0")"
PAK_NAME="$(basename "$PAK_DIR")"
PAK_NAME="${PAK_NAME%.*}"
PLATFORM="${PLATFORM:-tg5040}"
SDCARD_PATH="${SDCARD_PATH:-/mnt/SDCARD}"

LOGS_PATH="$SDCARD_PATH/.userdata/$PLATFORM/logs"
USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM/$PAK_NAME"
mkdir -p "$LOGS_PATH" "$USERDATA_PATH"

LOG_FILE="$LOGS_PATH/$PAK_NAME.txt"

printf '[%s] Launching %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$PAK_NAME" >>"$LOG_FILE"

export HOME="$USERDATA_PATH"
export PATH="$PATH:$PAK_DIR/bin/$PLATFORM:$PAK_DIR/bin/shared"
export MESHCLIENT_UI_BACKEND="${MESHCLIENT_UI_BACKEND:-minui}"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=/var/run/dbus/system_bus_socket"
unset DBUS_SESSION_BUS_ADDRESS || true

if ! command -v meshclient >/dev/null 2>&1; then
    echo "meshclient binary not found in PATH" >&2 | tee -a "$LOG_FILE"
    exit 1
fi

meshclient --foreground --log-level debug "$@" 2>&1 | tee -a "$LOG_FILE"
STATUS=$?
printf '[%s] MeshClient exited with status %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$STATUS"
exit "$STATUS"
