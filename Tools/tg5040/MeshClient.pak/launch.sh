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
exec >>"$LOG_FILE" 2>&1

printf '[%s] Launching %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$PAK_NAME"

export HOME="$USERDATA_PATH"
export PATH="$PATH:$PAK_DIR/bin/$PLATFORM:$PAK_DIR/bin/shared"
# Default to CLI backend until MinUI helpers are packaged for tg5040 builds
export MESHCLIENT_UI_BACKEND="${MESHCLIENT_UI_BACKEND:-cli}"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=/var/run/dbus/system_bus_socket"
unset DBUS_SESSION_BUS_ADDRESS || true

if ! command -v meshclient >/dev/null 2>&1; then
    echo "meshclient binary not found in PATH" >&2
    exit 1
fi

exec meshclient --foreground --log-level debug "$@"
