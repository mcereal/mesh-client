#!/usr/bin/env bash
# Push tile_bench and the generated tile sets to a Brick over adb, run the measurement matrix,
# and keep what it printed. Run from the repository root after build.sh and gen_tiles.py:
#
#   devtools/tile_bench/run-device.sh [--no-push] [CASE...]
#
# CASE is any of: verify tiles rgb udisk view gap (default: all of them). The client must not be
# running (`make deploy-stop`), unless measuring contention on purpose - see docs/maps-roadmap.md.
#
# Over adb because it needs no WiFi and moves bytes with the sync protocol; the Brick's adbd does
# not return exit codes, so success is read from the RESULT lines, never from `adb shell`.
set -euo pipefail

BIN="${BUILD_ROOT:-build/linux}/tile_bench/tile_bench"
TILES="${TILE_BENCH_TILES:-build/tile_bench/tiles}"
SD=/mnt/SDCARD/.userdata/tg5040/tile_bench
UD=/mnt/UDISK/tile_bench
OUT_DIR=build/tile_bench/results
mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/$(date +%Y%m%d-%H%M%S)${TILE_BENCH_TAG:+-$TILE_BENCH_TAG}.txt"

PUSH=1
if [[ "${1:-}" == "--no-push" ]]; then
    PUSH=0
    shift
fi
CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] || CASES=(verify tiles rgb udisk view gap)

[[ -x "$BIN" ]] || { echo "no $BIN; run devtools/tile_bench/build.sh in the cross container" >&2; exit 1; }
[[ -f "$TILES/palette/pack.mctp" ]] || { echo "no tiles; run devtools/tile_bench/gen_tiles.py" >&2; exit 1; }

sh_dev() { adb shell "$1" | tr -d '\r'; }

timed_push() {
    local label="$1" src="$2" dest="$3"
    local t0 t1
    t0=$(python3 -c 'import time; print(time.time())')
    adb push "$src" "$dest" >/dev/null
    t1=$(python3 -c 'import time; print(time.time())')
    python3 -c "print('PUSH $label %.1f s' % ($t1 - $t0))" | tee -a "$LOG"
}

{
    echo "# tile_bench $(date -u +%Y-%m-%dT%H:%M:%SZ) $(git rev-parse --short HEAD)"
    sh_dev 'uname -r; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor; ps | grep -c "[m]eshclient"'
} | tee -a "$LOG"

if [[ $PUSH -eq 1 ]]; then
    sh_dev "rm -rf $SD $UD; mkdir -p $SD/palette $SD/rgb $UD/palette"
    adb push "$BIN" "$UD/tile_bench" >/dev/null
    sh_dev "chmod +x $UD/tile_bench"
    for f in manifest.txt pack.mctp tiles.mbtiles; do
        timed_push "sd:palette/$f" "$TILES/palette/$f" "$SD/palette/$f"
    done
    timed_push "sd:palette/xyz ($(find "$TILES/palette/xyz" -type f | wc -l | tr -d ' ') files)" \
        "$TILES/palette/xyz" "$SD/palette/"
    adb push "$TILES/rgb/manifest.txt" "$SD/rgb/manifest.txt" >/dev/null
    timed_push "sd:rgb/pack.mctp" "$TILES/rgb/pack.mctp" "$SD/rgb/pack.mctp"
    adb push "$TILES/palette/manifest.txt" "$UD/palette/manifest.txt" >/dev/null
    timed_push "udisk:palette/pack.mctp" "$TILES/palette/pack.mctp" "$UD/palette/pack.mctp"
    sh_dev "du -sk $SD/palette/xyz $SD/palette/pack.mctp $SD/palette/tiles.mbtiles" | tee -a "$LOG"
fi

bench() { sh_dev "$UD/tile_bench $*" | tee -a "$LOG"; }

for c in "${CASES[@]}"; do
    case "$c" in
    verify)
        bench -d $SD/palette -l pack -m verify -n 500
        bench -d $SD/rgb -l pack -m verify -n 300
        ;;
    tiles)
        for layout in xyz pack mbtiles; do
            for dec in none stb wuffs; do
                for mode in cold-each cold-once warm; do
                    bench -d $SD/palette -l $layout -D $dec -m $mode -n 200
                done
            done
        done
        ;;
    rgb)
        for dec in stb wuffs; do
            for mode in cold-each warm; do
                bench -d $SD/rgb -l pack -D $dec -m $mode -n 200
            done
        done
        ;;
    udisk)
        for mode in cold-each cold-once warm; do
            bench -d $UD/palette -l pack -D wuffs -m $mode -n 200
        done
        ;;
    view)
        for layout in xyz pack mbtiles; do
            for dec in stb wuffs; do
                bench -d $SD/palette -l $layout -D $dec -m view -r 20
            done
        done
        bench -d $SD/rgb -l pack -D wuffs -m view -r 20
        ;;
    gap)
        # A press after the reader has been looking at the map for a while: schedutil has let the
        # clock fall, and the first tile pays for the ramp.
        bench -d $SD/palette -l pack -D wuffs -m cold-each -n 60 -g 1000
        bench -d $SD/palette -l pack -D wuffs -m warm -n 60 -g 1000
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
done
echo "results: $LOG"
