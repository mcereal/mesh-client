# devtools

Host-side tools that are not part of the client and never ship in the pak. `src/` is what runs
on a Brick; this is what runs on the machine you are developing on.

Built by default on a native build (`make debug`), skipped when cross-compiling, and switchable
with `-DMESHCLIENT_BUILD_DEVTOOLS=OFF`.

> The directory is `devtools/`, lower case and deliberately not `tools/`: `Tools/` already holds
> the device-facing pak assets, and macOS filesystems are case-insensitive by default, so the two
> would be the same directory on half the machines this repo is developed on.

## `ui_capture` — `meshclient_uicap`

Drives the HUD through a scripted sequence of button presses and renders every frame off-screen,
so a UI change can be looked at from a container, a CI runner or a cloud session with no Brick
and no `/dev/fb0` anywhere in sight.

It is not a mock. `mesh_ui_store_handle_key()` and `fb_render_snapshot()` are the ones that ship;
only the radio at the other end is invented, and only far enough to give the screens something
to draw.

Run it through `scripts/ui-capture.sh`, which builds it, feeds it a scene and encodes the frames:

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/messages.scene -o messages.gif"
./scripts/ui-capture.sh -o nodes.png -s 4 devtools/ui_capture/scenes/tabs.scene
printf 'scene demo\ntab nodes\nkey down 2\nkey a\n' | ./scripts/ui-capture.sh -o node.gif
```

On macOS the core does not build natively, so go through the container:
`make docker-ui-capture ARGS="..."`.

The scene language and the rest of the workflow are documented in
[`docs/ui.md`](../docs/ui.md#looking-at-a-ui-change); `scenes/` holds worked examples.

## `tile_bench` — what a map tile costs on the device

The measurement step 3 of [`docs/maps-roadmap.md`](../docs/maps-roadmap.md) asks for before any
basemap code is written: fetch, decode and blit of one raster tile on the Brick, for three pack
layouts (a `z/x/y` tree, MBTiles, a single-file indexed pack shaped like PMTiles) and two PNG
decoders (stb_image, Wuffs), with the page cache warm, dropped once or dropped per tile. Unlike
the tools above it runs **on the device**, so it is not part of the CMake build: it is
cross-built on its own, and the decoders and SQLite are fetched at pinned revisions into the
build tree rather than vendored, because none of them is a dependency the client has taken.

```bash
python3 devtools/tile_bench/gen_tiles.py -o build/tile_bench/tiles --rgb   # synthetic tiles
./scripts/docker.sh --cross devtools/tile_bench/build.sh                   # bench + size probes
make deploy-stop && devtools/tile_bench/run-device.sh                      # over adb
devtools/tile_bench/summarize.py build/tile_bench/results/<run>.txt
```

The tiles are synthetic and deliberately so: drawn to cost what a Carto-style palette PNG costs,
and a 24-bit shaded set for the heavy end. The results are in the roadmap.

## `map_pack` — building a raster tile pack

Turns an MBTiles file or a `z/x/y` directory of PNGs into the single-file pack the client reads
(`*.mctp`), and inspects or checks one that already exists. Stdlib only - no image library, no
network - so it runs anywhere Python does.

```bash
map_pack.py build --mbtiles region.mbtiles -o region.mctp \
    --name "Vancouver" --attribution "(c) OpenStreetMap contributors"
map_pack.py build --xyz tiles/ -o region.mctp --max-zoom 16 --bbox 49.1,-123.3,49.4,-122.9
map_pack.py info region.mctp
map_pack.py verify region.mctp
```

The conversion happens on the host because of what the Brick measured: on a FAT32 card with
32 KiB clusters mounted `sync`, a single file with a sorted index reached a cold tile in 0.80 ms
where MBTiles took 4.6 ms and a `z/x/y` tree took 4.6 ms with a 40 ms tail - and SQLite would
have cost 718 KB of binary to carry. The TMS row order MBTiles stores its tiles in is turned the
right way up here too, so the client only ever sees one convention. See
[`docs/maps-roadmap.md`](../docs/maps-roadmap.md#the-pack-format) for the format and
[`docs/cli.md`](../docs/cli.md#looking-inside-a-tile-pack) for `--map-pack`, which reads a pack
back on the device.

`build` refuses anything the device could not draw - a tile that is not a 256x256 PNG, a zoom
above 18, a tile over a megabyte - because a pack that fails at the blit fails where nobody is
watching a terminal. `verify` re-reads every tile the way the reader would and says which of
them are palette PNGs: a 24-bit pack is legal and costs 1.7x the decode and 2.7x the bytes, and
quantising is the style renderer's job rather than this tool's.

## `perf` — `meshclient_perf`

Measures the same text rasterizer with its glyph cache disabled and warm, and verifies that
both outputs match. It also compares full-render reference and optimized transcript navigation
and snackbar animation. `meshclient_uicap --reference` disables transcript caching and partial
redrawing for scene comparisons. Build with `make release`, then run
`build/release/devtools/meshclient_perf` (`./scripts/docker.sh make release` and
`./scripts/docker.sh build/linux/release/devtools/meshclient_perf` on macOS).
See [`docs/performance.md`](../docs/performance.md) for the workload and measurement limits.
