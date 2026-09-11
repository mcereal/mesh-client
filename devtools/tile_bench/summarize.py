#!/usr/bin/env python3
"""Turn a run-device.sh log into the markdown tables docs/maps-roadmap.md quotes.

    devtools/tile_bench/summarize.py build/tile_bench/results/<run>.txt
"""

import sys


def parse(path):
    results, pushes = [], []
    where = "?"
    for line in open(path):
        line = line.strip()
        if line.startswith("== "):
            d = line.split()[1].rstrip("/")
            where = ("ext4 " if d.startswith("/mnt/UDISK") else "") + d.rsplit("/", 1)[-1]
        elif line.startswith("RESULT "):
            r = dict(kv.split("=", 1) for kv in line.split()[1:])
            r["layout"] = f"{r.get('layout', '?')} ({where})"
            results.append(r)
        elif line.startswith("PUSH "):
            pushes.append(line[5:])
    return results, pushes


def tiles_table(rows):
    out = [
        "| Layout | Decoder | Cache | Fetch p50 | Fetch p99 | Decode p50 | Decode p99 | Tile p50 | Tile p99 | Tile max |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in rows:
        gap = f" +{r['gap_ms']} ms idle" if r.get("gap_ms", "0") != "0" else ""
        out.append(
            f"| {r['layout']} | {r['decoder']} | {r['mode']}{gap} | {r['fetch_p50']} | {r['fetch_p99']} | "
            f"{r['decode_p50']} | {r['decode_p99']} | {r['tile_p50']} | {r['tile_p99']} | {r['tile_max']} |"
        )
    return out


def view_table(rows):
    out = [
        "| Layout | Decoder | View p50 | View p90 | View max | Pan p50 | Pan p90 | Pan max |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in rows:
        out.append(
            f"| {r['layout']} | {r['decoder']} | {r['view_p50']} | {r['view_p90']} | {r['view_max']} | "
            f"{r['pan_p50']} | {r['pan_p90']} | {r['pan_max']} |"
        )
    return out


def main(argv):
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    results, pushes = parse(argv[0])
    print("All times in milliseconds.\n")
    for r in (r for r in results if r.get("mode") == "verify"):
        print(f"- verify {r['layout']}: {r['n']} tiles, {r['bad']} disagree")
    for p in pushes:
        print(f"- push {p}")
    per_tile = [r for r in results if r.get("mode") in ("warm", "cold-once", "cold-each")]
    views = [r for r in results if r.get("mode") == "view"]
    if per_tile:
        print()
        print("\n".join(tiles_table(per_tile)))
    if views:
        print()
        print("\n".join(view_table(views)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
