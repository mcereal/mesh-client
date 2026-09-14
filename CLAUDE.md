# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this is

A C17 Meshtastic client for the TrimUI Brick (NextUI/MinUI, platform key `tg5040`), shipped as a
`MeshClient.pak`. Single-threaded epoll event loop, BlueZ-over-D-Bus BLE transport, nanopb for
Meshtastic protobufs.

`AGENTS.md` is the contributor guide (style, tests, PR expectations) and applies here too.

## Commands

**The core is Linux-only** (`epoll`, `timerfd`, `eventfd`). On macOS, build and test through the
containers; on a Linux host - including a Claude Code on the web session - `make setup`
provisions the prerequisites and the plain targets work directly.
`.claude/hooks/session-start.sh` runs that setup automatically for remote sessions.

```bash
git submodule update --init --recursive   # nanopb, protobufs; CMake FATAL_ERRORs without them
make test                                 # Debug build + ctest - the default verify step
make debug                                # Debug build only
make format                               # clang-format all tracked .c/.h (needs clang-format 18)
make proto                                # regenerate nanopb sources
make release && make package              # release binary + dist/MeshClient.pak.zip
make fuzz                                 # libFuzzer over the two decoders that read the air
```

Run `make test` before every push. It is the suite CI runs, plus `scripts/check-strings.py`,
which fails on a prose literal in a renderer.

```bash
make docker-test                          # the same, in the dev container (use this on macOS)
make docker-shell                         # bash in the dev container
make docker-pak                           # cross container -> dist/MeshClient.pak.zip
make docker-image                         # force dev image rebuild after editing docker/
```

**A UI change is shown, not described.** `make ui-capture` drives the HUD through a scripted
sequence of presses and renders every frame off-screen, through the real nav model and the real
`fb_render_snapshot()` - so a change is reviewable as a picture from a container or a cloud
session, and `-g WxH` re-measures the same scene into another panel. Scenes live in
`devtools/ui_capture/scenes/`; the command list is in [`docs/ui.md`](docs/ui.md#looking-at-a-ui-change).

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/messages.scene -o messages.gif"
make ui-capture ARGS="-t light -g 1280x800 <scene> -o wide.gif"   # theme and geometry
make docker-ui-capture ARGS="..."         # on macOS
make screenshots                          # re-render the five listing stills in .github/resources
make demo-pack                            # draw a synthetic tile pack into build/demo.mctp
```

Device deploys go over SSH via `scripts/deploy-device.sh` (dropbear, busybox only: transfers are
`tar | ssh tar`, no rsync/scp). Host settings live in the gitignored `.brick.env`; the script
quotes for POSIX `sh`, not bash - keep it that way. See [`docs/device.md`](docs/device.md).

```bash
make brick                                # docker-pak + push to the Brick
make deploy / deploy-logs / deploy-check  # push / tail the log / report BlueZ, D-Bus, fb0
make deploy-start / deploy-stop           # start as Tools > MeshClient does / stop every client
make deploy-run                           # deploy-start + follow the log
make deploy-shot ARGS="-d 10 -o x.png"    # screenshot /dev/fb0 (page 0; -P 1 is the launcher)
```

**An on-device test ends with `make deploy-stop`.** Start the client with `make deploy-start`,
never `launch.sh` from a device shell: that runs it *beside* NextUI's launcher, which keeps
painting `fb0` and acting on every button. Killing the host side of a run does not stop the
client on the device.

Sanitizers: `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or `UBSAN`, or both). CI
runs the suite under both and cross-builds the pak on every pull request; see
[`docs/testing.md`](docs/testing.md#what-ci-runs).

### Tests

One binary with a name filter, not per-test CTest entries. Cases live in `tests/suites/<area>.c`
and **register themselves** - write one with `MESH_TEST_CASE(name, category)` and it runs. A new
suite file goes in `MESHCLIENT_TEST_SUITES` in `tests/CMakeLists.txt`. **Tests must not touch
real BlueZ** - use `mesh_bluez_client_mock_enable`. A helper used by one suite stays `static` in
it and moves to `tests/support/` when a second suite needs it.

```bash
./build/debug/tests/meshclient_core_tests --list
./build/debug/tests/meshclient_core_tests --filter ble_transport   # substring match
./build/debug/tests/meshclient_core_tests --suite ui_nav
```

Inside the container there is no BlueZ, so the BLE transport sits in `waiting-for-bluez` and the
CLI backend is selected; without D-Bus headers at build time it compiles out and reports
`disabled`.

## Where things live

Data flows one direction; input goes the other way.

```
link (transport) -> mesh_session -> mesh_app -> UI store -> controller -> backend
evdev -> mesh_ui_input -> controller -> nav.c -> mesh_ui_action -> mesh_app_on_ui_action
```

`include/mesh/` mirrors `src/` one-for-one, so a header sits in the directory named after the
source file that defines it.

| Area | Where |
|---|---|
| Event loop | `src/core/event_loop.c` - epoll, 32 fd sources, **no threads** |
| Transports | `src/transport/` - registry, BLE (BlueZ/D-Bus), serial, TCP; `stream_link.c` is the half serial and TCP share |
| Session | `src/core/session.c` - handshake, node roster, channels, message log, packet ids |
| Admin protocol | `src/core/radio_settings.c` - `AdminMessage` get/set queue, passkeys, NodeDB verbs |
| Messaging | `src/core/message.c`, `store_forward.c`, `waypoint.c` |
| Key trust | `src/core/key_verification.c` - the out-of-band ceremony behind the padlock; `add_contact` lives in `radio_settings.c` |
| App glue | `src/core/app*.c` - lifecycle/link, `_actions`, `_publish`, `_settings` |
| Self-update | `src/core/updater.c`, `version.c`, `fetch.c` |
| Radio firmware | `src/core/firmware*.c`, `uf2.c`, `esp_image.c`, `src/transport/*/{usb_msc,ble_ota,ble_hci}.c` - the *other* binary; see [`docs/radio-firmware-roadmap.md`](docs/radio-firmware-roadmap.md) |
| UI | `src/ui/` - store/controller, `nav*.c`, `settings*.c`, `layout.c`, `backends/{fb*,cli,stub}.c`; **`fb` is the device UI** |
| UI components | `src/ui/layout.c`, `src/ui/backends/fb_widgets.c` - cell-measured line builder, scroll window, cards, lists, meters, charts |
| Tables the UI reads | `actions.c` (button verbs), `status.c` (card verbs), `help.c`, `devices.c`, `nodes.c`, `delivery.c`, `trust.c`, `chrome.c`, `trend.c`, `duration.c` |
| Themes & fonts | `src/ui/theme.c`, `font*.c`, `icon*.c` - palette by role, shape scale, metrics |
| Strings | `src/i18n/strings.c`, `include/mesh/i18n/catalog.def` |
| Geography & map | `src/geo/` (the only directory that includes `<math.h>`), `src/map/`, `src/ui/{map,nav_map}.c`, `backends/fb_map.c` |
| Crash reports | `src/utils/crash.c` - local only, deliberately not a service |
| Shared utils | `src/utils/` - `text`, `time`, `env`, `json`, `log`, `sha256`, `array` |
| Dev tools | `devtools/`, `scripts/` - UI capture, map packs, codegen |

Four subsystems are split across several files sharing one `*_internal.h` (`src/core/app_internal.h`,
`src/ui/nav_internal.h`, `src/ui/settings_internal.h`, `src/ui/backends/fb_internal.h`). Those are
**not** public API: they declare only what would still be `static` if the group were one file, and
nothing outside the group should include one.

## House rules

These are authoring rules - breaking one compiles and looks fine.

- **Nothing is spelled out in a renderer.** A screen names an *id* and something else answers: a
  string (`MESH_STR_*` -> `src/i18n/strings.c`), an icon (`MESH_UI_ICON_*` -> `src/ui/icon.c`), a
  tone/family/role/shape (-> `src/ui/theme.c`). No English prose, colour, margin, glyph size or
  corner radius belongs in `src/ui/backends/`. `scripts/check-strings.py` fails the build on prose.
- **Button hints are (button, string id) pairs** in `src/ui/actions.c`, never a sentence. A keycap
  is untranslated - it is what is printed on the case. A keycap that does nothing is a bug.
- **A heading is `struct fb_app_bar`**, with slots; the back arrow is *derived* from the action
  table, never declared.
- **A list row is however many *steps* the list model says**, and the model is the authority; the
  screen measures and hands `fb_list_begin_heights()` an array.
- **fb layout is measured in cells, not bytes.** A `strlen` or `%-Ns` there is a bug.
- **A setting explains itself through `src/ui/help.c`**, keyed per section (and per route for
  screens that are not lists of fields), never as a sentence on a screen.
- **Adding a string, icon or theme is adding a table row** - `catalog.def`, `icons.def`,
  `theme.c`. Neither `.def` is a header and `make format` does not touch them.

## Before you change something

**[`docs/non-bugs.md`](docs/non-bugs.md) is the list of things that look like bugs and are not.**
Most entries cite the test that fails if the rule is undone. Read it before changing session,
settings, map, UI-layout, messaging or updater behaviour.

**Do not add to it as a habit.** 93 of its 137 entries arrived in the same commit as the code they
describe, which is an author explaining a choice rather than a record of anything going wrong -
and it is why that file is 900 lines and this one was 1248. An entry needs evidence that somebody
*tried to undo the rule*; a `feat` commit should not add one. If you are writing the code now, put
the reasoning in the test name, a comment at the seam, or the `docs/` page for that area. The bar
and the reasoning are at the top of that file.

The few that bite soonest:

- **No threads.** Everything is the one epoll loop.
- **BLE is not Nordic UART** and carries no length framing: one bare protobuf per GATT
  write/read. Framing is a *stream* concern - serial and TCP - in `src/proto/stream_framing.c`.
- **The Brick's face buttons do not report by position.** A is `BTN_EAST`, B is `BTN_SOUTH`, the
  button printed **Y (left)** is `BTN_NORTH`. See `src/ui/input_profile.c`.
- **A radio reboot after a settings write is expected.** The link drops and auto-connect returns.
- **The node roster deliberately outlives the connection**, and a NodeDB reset does not clear it.
- **The framebuffer needs all three steps** - draw page 0, `FBIOPAN_DISPLAY`, mirror into page 1 -
  or the screen is black.
- **Never `TERM` or `kill $(pidof nextui.elf)`** on device: SDL turns it into a quit event and the
  Brick powers off. `deploy-start` uses `SIGKILL` deliberately.
- **Only the release build is a release.** Do not stamp a local build to test the updater; lift
  the guard (`MESHCLIENT_UPDATE_ALLOW_DEV=1`).
- **Do not edit `project(meshclient VERSION x.y.z ...)`** in `CMakeLists.txt`, or bump versions by
  hand; the release workflow rewrites that line.
- **`launch.sh` and the pak's CA bundle do not ship through self-update.** Only the bare binary
  does, so treat both as a compatibility boundary.
- **`scripts/gen-{emoji,icons,font,locale}.py` are not part of the build.** Run by hand, commit
  the result.
- **`devtools/` is not `Tools/`** - macOS filesystems are case-insensitive.

## Protobufs

`MESH_PROTO_NAMES` in `CMakeLists.txt` is a hardcoded list; **adding a new upstream `.proto` means
adding it there.** Headers are included as `meshtastic/<name>.pb.h`. The generator is
`nanopb_generator` from PATH, falling back to `third_party/nanopb/generator/nanopb_generator.py`
(needs `pip install protobuf grpcio-tools`).

## Releasing

semantic-release on `main`/`beta`/`rc`, driven by Conventional Commits. **A merge to `main`
releases nothing**: a release is `workflow_dispatch` with a **Release channel** input, and a
Sunday cron as the safety net, so a day's pull requests batch into one release. `main` is
deliberately missing from the release workflow's `push` trigger - adding it back is how every
merged pull request became a release.

```bash
make ship                                 # a release off main: tag, assets, CHANGELOG, pak.json
make ship-beta                            # a prerelease of the same commit; commits nothing
```

Both dispatch on `main`; `beta` and `rc` are plumbing the workflow points at `main`, not branches
to work on (they keep their push trigger for a prerelease-per-merge flow). A prerelease publishes
a tag and the assets and **writes no file back** - `release.config.mjs` drops the changelog and
git plugins for it, which is what keeps the channel branch a pure fast-forward of `main`. Both
fields the Pak Store reads out of `pak.json` (`version`, `changelog`) are generated during a
stable release; hand edits are overwritten. See
[`docs/semantic-release.md`](docs/semantic-release.md).

## Docs map

| Doc | What it covers |
|---|---|
| [`docs/non-bugs.md`](docs/non-bugs.md) | things that look like bugs and are not, and the tests that hold them |
| [`docs/architecture.md`](docs/architecture.md) | core design and the reasoning behind it |
| [`docs/transport.md`](docs/transport.md) | BLE, serial, TCP, and the Brick USB workaround |
| [`docs/ui.md`](docs/ui.md) | store/nav/backends, fb rendering, fonts and emoji |
| [`docs/i18n.md`](docs/i18n.md) | the string catalog, adding a string, adding a language |
| [`docs/help.md`](docs/help.md) | the in-client help screen and what a note may say |
| [`docs/cli.md`](docs/cli.md) | flags, environment variables, on-device controls |
| [`docs/device.md`](docs/device.md) | Brick setup, deploy loop, screenshots, troubleshooting |
| [`docs/testing.md`](docs/testing.md) | test categories and how to run them |
| [`docs/performance.md`](docs/performance.md) | what a press costs and how it was measured |
| [`docs/steamdeck.md`](docs/steamdeck.md) | building and running on a Steam Deck |
| [`docs/portability.md`](docs/portability.md) | other handhelds, and the five questions a device has to answer |
| [`docs/semantic-release.md`](docs/semantic-release.md) | versioning, packaging, release assets |
| [`docs/settings-roadmap.md`](docs/settings-roadmap.md) | radio settings phases and admin verbs |
| [`docs/radio-firmware-roadmap.md`](docs/radio-firmware-roadmap.md) | updating the radio's firmware over USB and BLE |
| [`docs/maps-roadmap.md`](docs/maps-roadmap.md) | the basemap, the tile pack format, what is still open |
| [`docs/components-roadmap.md`](docs/components-roadmap.md) | UI component set audit and the order to close its gaps |
