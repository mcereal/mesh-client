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
git submodule update --init --recursive   # inkwell + inkcell + nanopb + protobufs (required), Mbed TLS (TLS)
make test                                 # Debug build + ctest - the default verify step
make debug                                # Debug build only
cmake --preset debug                      # the same configure, for an editor or a bare shell
make format                               # clang-format all tracked .c/.h (needs clang-format 18)
make proto                                # regenerate nanopb sources
make release && make package              # release binary + dist/MeshClient.pak.zip
make linux-cli                            # the static Linux CLI download (musl; needs musl-tools)
make fuzz                                 # libFuzzer over the two decoders that read the air
```

Run `make test` before every push. It is the suite CI runs, plus `scripts/check-strings.py`,
which fails on a prose literal in a renderer, and `scripts/check-layers.py`, which fails on an
include that crosses a layer the wrong way.

The build types live in `CMakePresets.json` - generator (Ninja), build type, and
`CMAKE_EXPORT_COMPILE_COMMANDS`, which is what `.clangd` reads out of `build/debug`. The scripts
pass `-B` over the preset's `binaryDir` so `BUILD_ROOT` still places the container (`build/linux`)
and sanitizer (`build/san`) trees; everything else about a configure comes from the preset.

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
evdev -> inkcell_input -> controller -> nav.c -> mesh_ui_action -> mesh_app_on_ui_action
```

**The systems layer is [inkwell](https://github.com/mcereal/inkwell), a submodule at
`third_party/inkwell`**, and it is the bottom of the stack: the epoll loop, the signals, the
clock, the log, the environment knobs, the whole-file read, the UTF-8 helpers, semver ordering,
the crash report, the codecs (base64, SHA-256, JSON, zip, HTTP/1.1, MQTT 3.1.1, deflate, PNG),
the forked DNS resolver and the vocabulary a link fails in (`inkwell/net/reason.h`). None of it
knows what a radio is - and none of it knows a word a user reads, which is what
`inkwell/net/reason.h` is for: a transport reports a *reason* and a number, and
`src/i18n/net_reason.c` is where this client turns that into a sentence. Its
`docs/extraction.md` is the running map of what is still on the wrong side of that line. `add_subdirectory(third_party/inkwell)` comes *first* in `CMakeLists.txt`, before
inkcell, so this client's pin is the one the whole tree builds - inkcell carries a submodule of
its own and brings it in only when no target of that name exists yet.

**The UI toolkit is [inkcell](https://github.com/mcereal/inkcell), a submodule at
`third_party/inkcell`**, and it stands on inkwell too. The theme, the fonts and glyph tables, the layout arithmetic, the
framebuffer backend and its components, the evdev input layer and the on-screen keyboard's grid
live there - none of them was ever about Meshtastic. What is here is the half that knows what a
node, a channel and a waypoint are: the store, the nav, the settings model, the screen
renderers, the tables.

inkcell never reaches into this client. Four things are pushed down instead, all from
`src/app/app.c` and `src/ui/backends/fb_app.c`:

| What | How |
|---|---|
| The knobs | `inkwell_env_set_prefix("MESHCLIENT")` - the prefix is inkwell's, and inkcell reads `THEME` through it as `MESHCLIENT_THEME` |
| The words | `mesh_i18n_register()` - inkcell's 23 ids and this client's ~870 as one table |
| The loop | `struct inkcell_input_host` over `inkwell_loop` - inkcell owns no loop, and the loop is inkwell's |
| The frame | `struct inkcell_fb_app` - inkcell calls up into `fb_app.c` once a frame |

**Everything that moved is spelled the inkcell way.** The bridge header that stood in for ~850
old `mesh_ui_*` names is gone: a symbol is `inkcell_*` when inkcell owns it and `mesh_*` when
this client does, and which prefix a name carries is now the answer to who owns it. A few names
the bridge had swept up were this client's own all along - `struct mesh_ui_action`,
`mesh_ui_store_handle_key()`, `enum mesh_ui_setting_field`, `fb_render_status()` - and they read
as this client's again.

**`include/mesh/<area>/` is an area's public surface and is flat; `src/<area>/` subdivides by
group.** So `store.h` is included as `mesh/ui/store.h` no matter which group under `src/ui/` its
source is filed in - how the sources are filed is not part of the interface, and moving one
between groups is not an API change. Find a header's source by *filename*, not by path: the two
always share a name.

The one thing that follows a source into its group is a `*_internal.h`, which is not public and
sits beside the files it serves.

**The directory under `src/` is a layer, and `scripts/check-layers.py` holds the direction.** The
allowed edges are the table in that script; the one worth knowing is that **`core` never includes
`ui`**. The session, the message log and the admin queue answer to a radio, not to a screen, and
the moment one of them reads a store record the client can no longer be driven headless.
`src/app/` is the composition root - it owns one of everything and is the single layer allowed to
see every other, because assembling them is what it is for. A new *top-level* directory under
`src/` needs an entry in that script's `ALLOWED` before it will compile clean; a group inside an
existing area does not, because a file's area is its first directory - `src/ui/store/store.c` is
`ui`, exactly as `src/transport/ble/bluez_client.c` is `transport`.

The 42k lines of generated glyph tables that used to be a third of this tree are inkcell's now
(`third_party/inkcell/src/generated/`), and so are the scripts that write them. Nothing under
`src/` is machine-written any more.

The one group that is several headers to one source is the UI store: `src/ui/store/store.c` defines
what `store.h` and its seven subject headers (`store_device.h`, `store_node.h`,
`store_channel.h`, `store_handshake.h`, `store_message.h`, `store_mqtt.h`,
`store_settings.h`) declare. Include
the subject you need - `store.h` is the umbrella and pulls all six in. See
[`docs/ui.md`](docs/ui.md#shape) for what each one owns and what the split does and does
not buy.

The cache `store.h` also declares is not in `store.c`: `src/ui/store/store_file.c` is both halves of
the file on the card, over `store_keys.c` (the key) and `store_fields.c` (the value).
`src/ui/store/store_archive.c` is the *second* file on the card - one append-only log per conversation,
which is what lets a thread go back further than the 64-message transport ring - and shares that
record codec over `store_internal.h`. `src/ui/store/store_trends.c` is the *third* - one append-only
log per node, which is what lets a node's trend outlive the run that watched it, written on every
publish and read back when that node's detail screen is opened. See
[`docs/ui.md`](docs/ui.md#what-the-client-remembers) for which file answers which question.

| Area | Where |
|---|---|
| Event loop | inkwell's `src/runtime/loop.c` (`inkwell/runtime/loop.h`) - epoll, 32 fd sources, **no threads** |
| Transports | `src/transport/` - registry, BLE (BlueZ/D-Bus), serial, TCP; `stream_link.c` is the half serial and TCP share. A link records `struct inkwell_net_failure` and `take_error()` is where it becomes words - see [`docs/transport.md`](docs/transport.md#how-a-failure-reaches-the-user) |
| Session | `src/core/session/session.c` - handshake, node roster, channels, message log, packet ids |
| Admin protocol | `src/core/session/radio_settings.c` - `AdminMessage` get/set queue, passkeys, NodeDB verbs |
| Messaging | `src/core/session/message.c`, `store_forward.c`, `waypoint.c` |
| Key trust | `src/core/session/key_verification.c` - the out-of-band ceremony behind the padlock; `add_contact` lives in `radio_settings.c` |
| Keyboard | the grid, the ring and the edits are inkcell's (`inkcell/ui/keyboard.h`); `src/ui/nav/nav_keyboard.c` is the seven jobs it is opened for and this client's emoji pages |
| Channel sharing | `src/proto/channel_url.c` (the `meshtastic.org/e/#` link), `src/core/session/channel_share.c` (the radio's table either way), inkcell's `src/utils/qr.c` (the code), `src/ui/views/channel_share.c` (what the two screens say) |
| Contact sharing | `src/proto/contact_url.c` (the `meshtastic.org/v/#` link), `src/core/session/contact_share.c` (this radio's record out, a stranger's in), `src/ui/views/contact_share.c` (what the two screens say); the wrapper both links share is `src/proto/link_url.h` |
| App glue | `src/app/*.c` - the composition root: lifecycle/link, `_actions`, `_publish`, `_settings` |
| Self-update | `src/core/update/updater.c`, `version.c`; HTTPS is `src/core/net/fetch.c` over inkwell's `inkwell/codec/http.h` |
| MQTT proxy | inkwell's `inkwell/codec/mqtt.h` (the 3.1.1 wire format), `src/proto/mqtt_topic.c` (where a mesh lives on a broker), `src/core/net/mqtt_proxy.c` (one broker connection), `src/core/net/tls_client.c` (Mbed TLS on the loop), `src/app/app_mqtt.c` (whether to hold one at all) |
| Radio firmware | `src/core/firmware/` - `firmware*.c`, `uf2.c`, `esp_image.c`, `src/transport/*/{usb_msc,ble_ota,ble_hci}.c` - the *other* binary |
| UI | `src/ui/` - see the group map below; **`fb` is the device UI** |
| UI toolkit | `third_party/inkcell/` - theme, fonts, glyphs, layout, widgets, the fb backend, input |
| UI components | inkcell's `include/inkcell/ui/widgets/*.h` (button, chrome, list, item, bubble, card, control, meter, overlay); `inkcell/ui/widgets.h` is the umbrella, `inkcell/ui/fb_draw.h` the toolkit under it |
| This client behind the frame | `src/ui/backends/fb_app.c` - the renderer inkcell calls, the move it cannot work out, the theme it is told |
| Tables the UI reads | `src/ui/tables/` - `actions.c` (button verbs), `status.c` (card verbs), `help.c`, `devices.c`, `nodes.c`, `delivery.c`, `trust.c`, `chrome.c`, `trend.c` (the airtime chart; the frame around it is inkcell's), `duration.c`, `units.c` (metric/imperial lengths) |
| Themes & fonts | inkcell's `src/theme/` and `src/generated/` - palette by role, shape scale, metrics, the glyph tables |
| Strings | `src/i18n/strings.c` registers the catalog; the list is `include/mesh/i18n/catalog.def`, continuing inkcell's 15; `src/i18n/net_reason.c` is the table from an `inkwell_net_reason` to a sentence |
| Geography & map | `src/geo/` (the only directory that includes `<math.h>`), `src/map/`, `src/ui/views/map.c`, `src/ui/nav/nav_map.c`, `src/ui/backends/fb_map.c` |
| Crash reports | inkwell's `inkwell/runtime/crash.h` writes them; `src/utils/crash.c` is this client's name, issues URL and note labels on one |
| Shared utils | `src/utils/` - the crash seam is all that is left here; `json`, `sha256`, `base64`, `zip`, `http`, `inflate` and `png` are inkwell's `codec/`, `text`, `time`, `env`, `log`, `array`, `file` and `version` its `base/`, the crash reporter its `runtime/`, and `qr` is inkcell's |
| Dev tools | `devtools/`, `scripts/` - UI capture, map packs, codegen |

### The groups inside `src/ui/` and `src/core/`

Two areas are large enough to be filed by group. The group is where a source *lives*, not part of
its include path - see the flat-header rule above.

| Group | What is in it |
|---|---|
| `src/ui/store/` | the records and the three files on the card, plus `history.c` and `preferences.c` |
| `src/ui/nav/` | where the reader is and what a press does: `nav*.c`, `route.c`, `controller.c` |
| `src/ui/settings/` | the settings model: fields, rows, the codec |
| `src/ui/tables/` | the vocabulary tables a screen names rather than spells out |
| `src/ui/views/` | per-screen view models - what a screen says, not how it is drawn |
| `src/ui/backends/` | the renderers. **`fb` is the device UI** |
| `src/core/session/` | the Meshtastic conversation: session, messaging, admin, trust, sharing |
| `src/core/firmware/` | the *radio's* firmware - a different binary on a different computer |
| `src/core/net/` | one hostname, one socket, one TLS session, one broker |
| `src/core/update/` | the *client* updating itself |
| `src/core/runtime/` | the loop, signals, process config |

The theme, the fonts, the glyph tables, the easing curves, the cell arithmetic and the evdev
layer are no longer groups here at all - they went to inkcell, which is why every directory left
under `src/ui/` is one that knows what a node, a channel or a waypoint is.

Five subsystems are split across several files sharing one `*_internal.h` (`src/app/app_internal.h`,
`src/ui/nav/nav_internal.h`, `src/ui/settings/settings_internal.h`, `src/ui/store/store_internal.h`,
`src/ui/backends/fb_internal.h`). Those are **not** public API: they declare only what would still
be `static` if the group were one file, and nothing outside the group should include one.

## House rules

These are authoring rules - breaking one compiles and looks fine.

- **Nothing is spelled out in a renderer.** A screen names an *id* and something else answers: a
  string (`MESH_STR_*` -> `src/i18n/strings.c`), an icon (`INKCELL_ICON_*` -> inkcell's
  `icons.def`), a tone/family/role/shape (-> inkcell's `theme.c`). No prose, colour, margin, glyph size or
  corner radius belongs in `src/ui/backends/`. `scripts/check-strings.py` fails the build on prose.
- **Button hints are (button, string id) pairs** in `src/ui/tables/actions.c`, never a sentence. A keycap
  is untranslated - it is what is printed on the case. A keycap that does nothing is a bug.
- **A heading is `struct inkcell_fb_app_bar`**, with slots; the back arrow is *derived* from the action
  table, never declared.
- **A list row is however many *steps* the list model says**, and the model is the authority; the
  screen measures and hands `inkcell_fb_list_begin_heights()` an array.
- **fb text is measured, never counted.** A `strlen` or `%-Ns` is a bug, and so is a cell count
  multiplied by the advance: the UI face is proportional, so `inkcell_fb_char_adv()` is a
  *nominal* width and an estimate. Measure with `inkcell_fb_text_width()`, wrap and fit against
  pixels (`layout->body_w`), and reach for `inkcell_fb_text_cols()` only where a layout really
  does reserve whole columns.
- **A setting explains itself through `src/ui/tables/help.c`**, keyed per section (and per route for
  screens that are not lists of fields), never as a sentence on a screen.
- **Adding a string or a cache key is adding a table row** - `catalog.def`, `store_keys.def`. A
  `.def` is not a header and `make format` does not touch them. An icon or a theme is a row in
  inkcell's `icons.def` or `theme.c`, which is a change to the toolkit and lands there first.

## Before you change something

**[`docs/non-bugs.md`](docs/non-bugs.md) is the list of things that look like bugs and are not.**
It is sectioned by area with a table of contents - read the section you are about to change, not
the file - and most entries cite the test that fails if the rule is undone.

**Do not add to it as a habit.** Most of its entries arrived in the same commit as the code they
describe, which is an author explaining a choice rather than a record of anything going wrong.
An entry needs evidence that somebody *tried to undo the rule*; a `feat` commit should not add
one. If you are writing the code now, put the reasoning in the test name, a comment at the seam,
or the `docs/` page for that area. **An entry that cites a test is written short on purpose**:
the test holds the line and the prose is only the reason. The bar is at the top of that file.

The same goes for `docs/`. It is a short reference set, not a journal: a page says how a thing
works and what will bite, not what each step of building it turned out to cost.

The few that bite soonest:

- **No threads.** Everything is the one epoll loop.
- **BLE is not Nordic UART** and carries no length framing: one bare protobuf per GATT
  write/read. Framing is a *stream* concern - serial and TCP - in `src/proto/stream_framing.c`.
- **A QR code is black on white on every theme.** Several scanners will not read an inverted
  one, so `INKCELL_COLOR_CODE`/`_GROUND` are the one pair in `theme.c` that does not vary. There
  is no *decoder* and there will not be one: the Brick has no camera, so a link arriving is
  typed in.
- **The Brick's face buttons do not report by position.** A is `BTN_EAST`, B is `BTN_SOUTH`, the
  button printed **Y (left)** is `BTN_NORTH`. See inkcell's `src/input/input_profile.c`.
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
- **`launch.sh` does not ship through self-update.** Only the bare binary does, so treat it as a
  compatibility boundary - which is why the CA roots are compiled in rather than shipped beside it.
- **`scripts/gen-{locale,ca-roots}.py` are not part of the build.** Run by hand, commit the
  result. The glyph generators went with the glyphs: emoji, icons and the `ui` face are
  inkcell's, and so are the scripts that rasterise them.
- **`devtools/` is not `Tools/`** - macOS filesystems are case-insensitive.

## Protobufs

`MESH_PROTO_NAMES` in `CMakeLists.txt` is a hardcoded list; **adding a new upstream `.proto` means
adding it there.** `apponly.proto` is in it for `ChannelSet`, which is the only message here that
never goes over the air - it exists to be a URL. `SharedContact` is the other half of that
pattern and the counter-example: it is in `admin.proto` and is both a link payload and the
`add_contact` verb's argument. Headers are included as `meshtastic/<name>.pb.h`. The generator is
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
[`docs/releasing.md`](docs/releasing.md).

## Docs map

| Doc | What it covers |
|---|---|
| [`docs/non-bugs.md`](docs/non-bugs.md) | things that look like bugs and are not, and the tests that hold them |
| [`docs/architecture.md`](docs/architecture.md) | core design and the reasoning behind it |
| [`docs/transport.md`](docs/transport.md) | BLE, serial, TCP, and the Brick USB workaround |
| [`docs/ui.md`](docs/ui.md) | store/nav/backends, fb rendering, fonts and emoji |
| [`docs/i18n.md`](docs/i18n.md) | the string catalog, adding a string, adding a language |
| [`docs/mqtt.md`](docs/mqtt.md) | the MQTT client proxy and its TLS |
| [`docs/help.md`](docs/help.md) | the in-client help screen and what a note may say |
| [`docs/cli.md`](docs/cli.md) | flags, environment variables, on-device controls |
| [`docs/device.md`](docs/device.md) | Brick setup, deploy loop, screenshots, troubleshooting |
| [`docs/testing.md`](docs/testing.md) | test categories and how to run them |
| [`docs/performance.md`](docs/performance.md) | what a press costs and how it was measured |
| [`docs/releasing.md`](docs/releasing.md) | versioning, packaging, release assets |
