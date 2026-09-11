#!/usr/bin/env bash
# Cross-build tile_bench and the size probes for the Brick. Runs inside the cross container:
#
#   scripts/docker.sh --cross devtools/tile_bench/build.sh
#
# The three third-party sources are fetched at pinned revisions and checked against a digest,
# into the build tree rather than the repository: this is a measurement, and none of them is a
# dependency the client has taken. Outputs land in $BUILD_ROOT/tile_bench.
set -euo pipefail

if [[ ! -f /opt/cross/env.sh ]]; then
    echo "cross toolchain not found; run this inside the cross container" >&2
    exit 1
fi
# shellcheck disable=SC1091
source /opt/cross/env.sh

HERE=devtools/tile_bench
OUT="${BUILD_ROOT:-build}/tile_bench"
TP="$OUT/third_party"
mkdir -p "$TP"

STB_URL=https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h
STB_SHA=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
WUFFS_URL=https://raw.githubusercontent.com/google/wuffs/0f214ba59c20c0c9c7ba841ecc3683f863965312/release/c/wuffs-v0.4.c
WUFFS_SHA=1f8039ef82911604c063f6ac2ed57254bdb17d742aebdeae06356530d4a0fde7
SQLITE_URL=https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
SQLITE_SHA=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d

fetch() {
    local url="$1" sha="$2" dest="$3"
    if [[ ! -f "$dest" ]] || ! echo "$sha  $dest" | sha256sum -c --status; then
        python3 -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$url" "$dest"
    fi
    echo "$sha  $dest" | sha256sum -c --quiet
}
fetch "$STB_URL" "$STB_SHA" "$TP/stb_image.h"
fetch "$WUFFS_URL" "$WUFFS_SHA" "$TP/wuffs-v0.4.c"
fetch "$SQLITE_URL" "$SQLITE_SHA" "$TP/sqlite.zip"
if [[ ! -f "$TP/sqlite3.c" ]]; then
    python3 - "$TP" <<'PY'
import sys, zipfile
with zipfile.ZipFile(f"{sys.argv[1]}/sqlite.zip") as z:
    for name in z.namelist():
        base = name.rsplit("/", 1)[-1]
        if base in ("sqlite3.c", "sqlite3.h"):
            open(f"{sys.argv[1]}/{base}", "wb").write(z.read(name))
PY
fi

CC="${CROSS_COMPILE}gcc"
STRIP="${CROSS_COMPILE}strip"
# -Os because that is what scripts/cross-build.sh builds the client with, so a size and a speed
# measured here are the ones the client would get.
OPT="${TILE_BENCH_OPT:--Os}"
CFLAGS=(-std=c17 "$OPT" ${CROSS_CFLAGS:-} -ffunction-sections -fdata-sections -I"$TP" -I"$HERE")
LDFLAGS=(-static -Wl,--gc-sections)
SQLITE_DEFS=(-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DQS=0)

# Third-party code is compiled without our warnings; ours is compiled with them.
"$CC" "${CFLAGS[@]}" -w -c "$HERE/stb_impl.c" -o "$OUT/stb.o"
"$CC" "${CFLAGS[@]}" -w -c "$HERE/wuffs_impl.c" -o "$OUT/wuffs.o"
[[ -f "$OUT/sqlite3.o" && "$OUT/sqlite3.o" -nt "$TP/sqlite3.c" ]] ||
    "$CC" "${CFLAGS[@]}" "${SQLITE_DEFS[@]}" -w -c "$TP/sqlite3.c" -o "$OUT/sqlite3.o"

"$CC" "${CFLAGS[@]}" -Wall -Wextra -Werror -o "$OUT/tile_bench" "$HERE/tile_bench.c" \
    "$OUT/stb.o" "$OUT/wuffs.o" "$OUT/sqlite3.o" "${LDFLAGS[@]}" -lm
"$STRIP" "$OUT/tile_bench"

# The loaded size, summed over the PT_LOAD segments - not the file size, which aarch64's 64 KiB
# segment alignment moves in page-sized jumps (a 15 KB decoder measured as 344 bytes that way).
loaded_size() {
    python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
phoff, = struct.unpack_from("<Q", d, 0x20)
phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
total = 0
for i in range(phnum):
    p_type, _, _, _, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
    if p_type == 1:
        total += memsz
print(total)
PY
}
probe() {
    local name="$1" def="$2"
    shift 2
    "$CC" "${CFLAGS[@]}" -Wall -Wextra ${def:+-D$def} -o "$OUT/probe_$name" "$HERE/probe.c" "$@" \
        "${LDFLAGS[@]}" -lm
    "$STRIP" "$OUT/probe_$name"
    loaded_size "$OUT/probe_$name"
}
base=$(probe none "")
stb=$(probe stb PROBE_STB "$OUT/stb.o")
wuffs=$(probe wuffs PROBE_WUFFS "$OUT/wuffs.o")
sqlite=$(probe sqlite PROBE_SQLITE "$OUT/sqlite3.o")
{
    echo "loaded (text+data+bss) size added to a static program, ${OPT}, sections collected:"
    printf '  %-8s %8d bytes\n' stb $((stb - base)) wuffs $((wuffs - base)) sqlite $((sqlite - base))
} | tee "$OUT/sizes.txt"
file "$OUT/tile_bench"
