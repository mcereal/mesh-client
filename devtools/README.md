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

## `perf` — `meshclient_perf`

Measures the same text rasterizer with its glyph cache disabled and warm, and verifies that
both outputs match. It also compares full-render reference and optimized transcript navigation
and snackbar animation. `meshclient_uicap --reference` disables transcript caching and partial
redrawing for scene comparisons. Build with `make release`, then run
`build/release/devtools/meshclient_perf` (`./scripts/docker.sh make release` and
`./scripts/docker.sh build/linux/release/devtools/meshclient_perf` on macOS).
See [`docs/performance.md`](../docs/performance.md) for the workload and measurement limits.
