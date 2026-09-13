#!/usr/bin/env bash
# Drive a real run of the client on a Brick from a script of presses, and keep what its latency
# probe printed. Run from the repository root after devtools/input_inject/build.sh:
#
#   devtools/input_inject/run-device.sh [--every MS] [--tag NAME] [--cold] [--] TOKEN...
#   devtools/input_inject/run-device.sh --every 600 --tag map r1 a wait:6 x:3 right:20 down:10
#
# The order is the whole of this script and it is not negotiable in either direction:
#
#   1. the client is stopped, because two clients cannot share one radio link;
#   2. the injector is started, and creates its uinput device;
#   3. the client is started, and finds that device in its ordinary startup scan - it scans
#      once, so a pad created afterwards is a pad it never watches;
#   4. the script plays;
#   5. the client is stopped with SIGTERM, which is what makes it print the report - a KILL
#      would take the measurement with it.
#
# The injector is held open by an adb shell this script owns. adbd tears a process group down
# when its shell returns, which is exactly what is wanted here: the virtual pad cannot outlive
# the run and be found by the next one.
set -euo pipefail

EVERY=400
AFTER=22000
HOLD=4000
TAG=""
COLD=0
DEVICE_BIN=/mnt/UDISK/input_inject
REMOTE_LOG=/mnt/SDCARD/.userdata/tg5040/logs/MeshClient.txt
OUT_DIR=build/input_inject/results

while [[ $# -gt 0 ]]; do
    case "$1" in
        --every) EVERY="$2"; shift 2 ;;
        --after) AFTER="$2"; shift 2 ;;
        --hold) HOLD="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        # Drop the page cache before the client starts, so the pack is read off the card rather
        # than out of RAM. A pack pushed minutes ago is entirely in page cache, which is the
        # difference between a 0.05 ms read and the 0.80 ms one devtools/tile_bench measured.
        --cold) COLD=1; shift ;;
        --) shift; break ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[[ $# -gt 0 ]] || { echo "no script; see the header of $0" >&2; exit 2; }

BIN="${BUILD_ROOT:-build/linux}/input_inject/input_inject"
[[ -x "$BIN" ]] || { echo "no $BIN; run devtools/input_inject/build.sh in the cross container" >&2; exit 1; }

mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/$(date +%Y%m%d-%H%M%S)${TAG:+-$TAG}.txt"

adb push "$BIN" "$DEVICE_BIN" >/dev/null
adb shell "chmod +x $DEVICE_BIN" >/dev/null

./scripts/deploy-device.sh stop >/dev/null

# Where the client's log has got to, so only this run's lines are kept.
lines=$(adb shell "wc -l < $REMOTE_LOG 2>/dev/null || echo 0" | tr -d '\r ')

if [[ $COLD -eq 1 ]]; then
    adb shell 'sync; echo 3 > /proc/sys/vm/drop_caches' >/dev/null
    echo "Page cache dropped"
fi

echo "Injector up (${AFTER} ms before the first press, ${EVERY} ms between)"
adb shell "$DEVICE_BIN --after $AFTER --every $EVERY --hold $HOLD $*" &
INJECTOR=$!
trap 'kill $INJECTOR 2>/dev/null || true' EXIT

# Long enough for the uinput device to exist before the client scans for it, and short enough to
# leave most of --after for the client's own startup and BLE handshake.
sleep 2
./scripts/deploy-device.sh start -- --trace-latency >/dev/null

# Read while the client is up, not after it: NextUI's launch loop pins `performance` for a pak
# and hands the launcher back `schedutil`, so the governor asked for afterwards is the
# launcher's rather than the one the measurement ran under.
GOVERNOR=$(adb shell 'cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor' | tr -d '\r')

wait $INJECTOR
trap - EXIT

./scripts/deploy-device.sh stop >/dev/null
{
    echo "# input_inject $(date -u +%Y-%m-%dT%H:%M:%SZ) $(git rev-parse --short HEAD)"
    echo "# every=${EVERY}ms cold=${COLD} script: $*"
    echo "# governor while running: ${GOVERNOR}"
    adb shell "tail -n +$((lines + 1)) $REMOTE_LOG" | tr -d '\r'
} > "$LOG"

echo "Kept $LOG"
grep -E '\(latency\)' "$LOG" || echo "no latency lines - was the client built with the probe?"
