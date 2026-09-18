#!/bin/sh
set -eu

# Absolute, so `./launch.sh` from inside the pak (what `make deploy-run` does) names the log and
# userdata dirs the same way a NextUI launch by full path does. With a bare `.` the
# `${PAK_NAME%.*}` strip below would leave an empty name.
PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
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
export LD_LIBRARY_PATH="/usr/trimui/lib:$PAK_DIR/bin/$PLATFORM:$PAK_DIR/bin/shared:${LD_LIBRARY_PATH:-}"
export MESHCLIENT_UI_BACKEND="${MESHCLIENT_UI_BACKEND:-fb}"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=/var/run/dbus/system_bus_socket"
unset DBUS_SESSION_BUS_ADDRESS || true

if ! command -v meshclient >/dev/null 2>&1; then
    echo "meshclient binary not found in PATH" >&2 | tee -a "$LOG_FILE"
    exit 1
fi

# `debug` used to be hardcoded here, which made every shipped install run at the second-most
# verbose level for its whole life: at that level the BLE scan alone writes a line per visible
# radio per second, into a file nothing was cutting back. `info` is what a user's run wants;
# MESHCLIENT_LOG_LEVEL is how a developer asks for more without editing the pak.
LOG_LEVEL="${MESHCLIENT_LOG_LEVEL:-info}"

# `set -o pipefail` is not in POSIX sh and busybox ash does not take it, so the client's status is
# carried out of the pipeline by hand. Without this `$?` is *tee's* status - which is success
# almost regardless of how the client died, and was reported as the client's own.
STATUS_FILE="$USERDATA_PATH/.launch-status"
rm -f "$STATUS_FILE"
# The `if` also keeps `set -e` out of it: a bare failing command here would take the subshell
# down before the status was ever recorded, which is the same lost status by another route.
{
    if meshclient --foreground --log-level "$LOG_LEVEL" "$@" 2>&1; then
        echo 0 >"$STATUS_FILE"
    else
        echo $? >"$STATUS_FILE"
    fi
} | tee -a "$LOG_FILE"
STATUS="$(cat "$STATUS_FILE" 2>/dev/null || true)"
rm -f "$STATUS_FILE"
# `exit` takes a number, so anything else here would fail the launcher itself rather than report
# how the client ended - which is a worse answer than the 1 that stands in for "cannot tell".
case "$STATUS" in
    '' | *[!0-9]*) STATUS=1 ;;
esac

# Appended, not just printed: the launch banner above goes into the log, so the line saying how
# the run ended belongs in the same file. On stdout alone it was the one half nobody could read.
printf '[%s] MeshClient exited with status %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$STATUS" \
    >>"$LOG_FILE"
exit "$STATUS"
