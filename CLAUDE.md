# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this is

A C17 Meshtastic client for the TrimUI Brick (NextUI/MinUI, platform key `tg5040`), shipped as a
`MeshClient.pak`. Single-threaded epoll event loop, BlueZ-over-D-Bus BLE transport, nanopb for
Meshtastic protobufs.

`AGENTS.md` is the contributor guide (style, tests, PR expectations) and applies here too.

## Commands

**The core is Linux-only** (`epoll`, `timerfd`, `eventfd`). This repo is developed on macOS, so
build and test through the containers. On a Linux host — including a Claude Code on the web
session — `make setup` provisions the same prerequisites natively and the plain targets work
directly, no Docker needed; `.claude/hooks/session-start.sh` runs that setup automatically for
remote sessions.

```bash
git submodule update --init --recursive   # nanopb, meshtastic/protobufs
                                          # CMake FATAL_ERRORs without them
make test                                 # Debug build + ctest — the default verify step
make debug                                # Debug build only
make format                               # clang-format all tracked .c/.h
make proto                                # regenerate nanopb sources
make release && make package              # release binary + dist/MeshClient.pak.zip
make ui-capture ARGS="<scene> -o x.gif"   # render a UI scene to a GIF, no device needed
```

**A UI change is shown, not described.** `make ui-capture` drives the HUD through a scripted
sequence of button presses and renders every frame off-screen - the real nav model and the real
`fb_render_snapshot()`, drawing into memory instead of `/dev/fb0` - so a change is reviewable as
a picture from a container or a cloud session. A GIF rather than a still because most UI changes
are about a transition. Scene scripts and the command list are in
[`docs/ui.md`](docs/ui.md#looking-at-a-ui-change); examples in `devtools/ui_capture/scenes/`.

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/messages.scene -o messages.gif"
make ui-capture ARGS="-t light devtools/ui_capture/scenes/messages.scene -o light.gif"
make docker-ui-capture ARGS="..."         # on macOS
printf 'scene demo\ntab nodes\nkey down 2\nkey a\n' | ./scripts/ui-capture.sh -o node.gif
```

`make docker-*` wraps `scripts/docker.sh`, which builds the image from `docker/Dockerfile` on
first use and bind-mounts the repo at `/src`. Container builds use `BUILD_ROOT=build/linux`, so
outputs land in `build/linux/{debug,release}` and never share a CMake cache with a host
configure.

```bash
make docker-test                          # the same, in the dev container
make docker-debug                         # debug build only, in the dev container
make docker-shell                         # bash in the dev container
make docker-pak                           # cross container -> dist/MeshClient.pak.zip
make docker-image                         # force dev image rebuild after editing docker/
make docker-cross-image                   # force cross image rebuild
```

Device deploys go over SSH via `scripts/deploy-device.sh` (dropbear "SSH Server" pak, busybox
only: transfers are `tar | ssh tar`, no rsync/scp). Host settings live in the gitignored
`.brick.env`; the script quotes for POSIX `sh`, not bash — keep it that way. See
[`docs/device.md`](docs/device.md).

```bash
make brick                                # docker-pak + push to the Brick
make deploy                               # push only
make deploy-logs                          # tail the device log
make deploy-check                         # report BlueZ, D-Bus and fb0 state on device
make deploy-shot ARGS="-d 10 -o x.png"    # screenshot /dev/fb0 (page 0; -P 1 is the launcher)
make deploy-clip ARGS="-d 10 -n 30"       # film /dev/fb0 to a GIF (a few fps; not real time)
make deploy-run ARGS="--list-devices"     # run launch.sh on device headless, streaming output
```

Sanitizers: `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or `UBSAN`).

### Tests

One binary with a name filter, not per-test CTest entries. Cases live in
`tests/suites/<area>.c` and **register themselves** — write one with `MESH_TEST_CASE(name,
category)` and it runs; there is no table to update. A new suite file goes in
`MESHCLIENT_TEST_SUITES` in `tests/CMakeLists.txt`, and new CTest labels still need a matching
`add_test` there. **Tests must not touch real BlueZ** — use `mesh_bluez_client_mock_enable`.

| Path | What lives there |
|---|---|
| `tests/framework/` | `MESH_TEST_CASE`, the guard macros, the registry and `main` |
| `tests/support/` | fixtures shared by more than one suite, prefixed `mesh_test_` |
| `tests/suites/` | the cases themselves, one file per subject area |

A helper used by one suite stays `static` in that suite; it moves to `support/` when a second
suite needs it.

```bash
./build/debug/tests/meshclient_core_tests --list
./build/debug/tests/meshclient_core_tests --filter ble_transport
./build/debug/tests/meshclient_core_tests --suite ui_nav
```

Verified 2026-09-07: 220 unit tests, all passing, zero compiler warnings.
`message_encode_text_golden` pins the `TEXT_MESSAGE_APP` wire format against a hand-derived byte
vector — not against our own encoder — so a protobuf regeneration that changes field numbers or
wire types fails loudly.

Inside the container there is no BlueZ, so the BLE transport sits in `waiting-for-bluez` and the
CLI backend is selected; without D-Bus headers at build time it compiles out entirely and reports
`disabled`.

### Formatting

`make format` rewrites every tracked `.c`/`.h` and is a no-op on a clean tree. It is normalised
with **clang-format 18** (what `ubuntu:24.04` ships, so the dev container and CI agree) and
**refuses to run** under a different major, because the tree drifted that way once. From a host
with another version use `./scripts/docker.sh make format`, or set `CLANG_FORMAT_ANY_VERSION=1`
if you mean it. Nothing gates on formatting in CI, precisely because host versions vary.

## Architecture at a glance

Data flows one direction; input goes the other way.

```
link (transport) -> mesh_session -> mesh_app -> UI store -> controller -> backend
evdev -> mesh_ui_input -> controller -> nav.c -> mesh_ui_action -> mesh_app_on_ui_action
```

| Area | Where | Notes |
|---|---|---|
| Event loop | `src/core/event_loop.c` | epoll, 32 fd sources, **no threads** |
| Transports | `src/transport/` | registry + BLE (BlueZ/D-Bus) + serial (USB) |
| Session | `src/core/session.c` | handshake, node roster, channels, message log, packet ids |
| Admin protocol | `src/core/radio_settings.c` | `AdminMessage` get/set queue, passkeys, radio actions, NodeDB verbs, the module table |
| Messaging | `src/core/message.c` | text packets, message ring, ack correlation |
| App glue | `src/core/app*.c` | `app` lifecycle/link, `_actions` UI actions, `_publish` to store, `_settings` writes |
| Self-update | `src/core/updater.c`, `version.c` | forks curl, SemVer, digest-verified install |
| UI | `src/ui/` | store/controller + `nav*.c` + `settings*.c` + `layout.c` + `backends/{fb*,cli,stub}.c`; **`fb` is the device UI** |
| UI components | `src/ui/layout.c`, `src/ui/backends/fb_widgets.c` | cell-measured line builder + scroll window; cards (filled/elevated/outlined, with verbs on the heading line), buttons, chips, badges, list items (leading/marker/supporting/trailing slots), switches, meters (with domains and drawn threshold bands), signal staircases, bubbles, the top app bar, the navigation bar, the action bar, the snackbar |
| Button hints | `src/ui/actions.c`, `include/mesh/ui/actions.h` | what the buttons do here, as (button, verb) pairs the action bar iterates |
| Status verbs | `src/ui/status.c`, `include/mesh/ui/status.h` | which Status card carries which verb — read by `nav.c`, `actions.c` and the renderer alike |
| Animation | `src/ui/anim.c`, `src/ui/controller.c` | fixed-point easing + a table keyed per control; the repaint timerfd that feeds it |
| Icons | `src/ui/icon.c`, `src/ui/icon_glyphs.c`, `include/mesh/ui/icons.def` | monochrome Material Symbols, tinted by the theme, in the row slots |
| Themes | `src/ui/theme.c`, `src/ui/font.c` | palette by role, surface tiers, the shape scale, metrics, font registry; `MESHCLIENT_THEME` or Settings > About picks one |
| Fonts | `src/ui/font_ui.c` + generated `font_ui_glyphs.c`, `src/ui/font5x7.c` | a glyph is **coverage**, resampled from the font's master into the cell; `ui` (JetBrains Mono) is the default, `5x7` is the pixel one |
| Text | `src/utils/text.c`, `src/ui/{font5x7,emoji}.c` | UTF-8 sanitising, cell-based measurement |
| Strings | `src/i18n/strings.c`, `include/mesh/i18n/catalog.def` | the string catalog and the locale registry |
| Dev tools | `devtools/`, `scripts/{ui-capture.sh,frames.py}` | off-screen UI capture; PNG/GIF encoding, stdlib only |
| Shared utils | `src/utils/` | `text` (UTF-8 + `mesh_str_copy`), `time` (`mesh_time_monotonic_ms`), `env` (`mesh_env_bool`/`_int`), `log`, `sha256`, `array` |

`include/mesh/` mirrors `src/` one-for-one — `core/`, `transport/`, `ui/`, `proto/`, `utils/` —
so a header always sits in the directory named after the source file that defines it.

Four subsystems are split across several files sharing one `*_internal.h` next to them
(`src/core/app_internal.h`, `src/ui/nav_internal.h`, `src/ui/settings_internal.h`,
`src/ui/backends/fb_internal.h`). Those headers are **not** public API — they declare only what
would still be `static` if the group were one file, and nothing outside the group should include
one. A symbol added to an internal header is a seam widened; prefer keeping the call inside the
file that owns the state.

**No button hint is spelled out as a sentence either.** The footer's `"A open node  X pin  Y
write  L/R tabs"` entries are gone. What the buttons do is a table of *(button, string id)*
pairs in `src/ui/actions.c`, and `fb_draw_action_bar()` draws a keycap and a verb per pair -
because a component that draws the parts separately cannot be handed a sentence, and going
looking for the button letters inside a translation is the one thing the i18n layer prevents.
A keycap itself is untranslated: it is what is printed on the case.

**No English sentence is spelled out in a renderer either.** A screen names a *string id*
(`MESH_STR_TOAST_NOT_CONNECTED`) and `src/i18n/strings.c` answers, the same way `theme.c`
answers a colour role. The catalog is one line per string in `include/mesh/i18n/catalog.def`;
adding a string is adding a line there. `scripts/check-strings.py` runs in `make test` and
fails on a literal that looks like prose in a renderer. Logs, region codes, hardware model
names and modem presets stay untranslated on purpose - see
[`docs/i18n.md`](docs/i18n.md).

**No breadcrumb is spelled out in a renderer either.** A screen's heading is not a string: it
is `struct fb_app_bar`, with a *slot* for each thing a heading carries - the back affordance,
the trail of levels above it, the title, and a badge saying one thing about the whole screen.
`"Settings > %s%s%s"` is gone from the catalog, and with it the `>` separators that handed a
translator the breadcrumb's grammar and the `%s` that a count was glued into. Two rules came out
of doing it: **an overline says only what nothing else on the frame says** - the navigation bar
is already naming the tab, so a trail never repeats it - and the **back arrow is derived, not
declared**: `mesh_ui_action_bar_goes_back()` reads the same table the action bar draws from, so
the arrow at the top and the `B` keycap at the bottom cannot disagree. See
[`docs/ui.md`](docs/ui.md#fb_draw_app_bar--the-top-app-bar).

**No row marker is spelled out in a renderer either.** A row that opens something, one with an
unsaved edit, a channel, a node the radio has forgotten: each names an *icon*
(`MESH_UI_ICON_CHEVRON`) in one of the list item's slots, and `src/ui/icon.c` answers with a
monochrome sprite the row draws in its own ink. The set is one line per icon in
`include/mesh/ui/icons.def`; adding one is a line there plus `scripts/gen-icons.py`. The `"> "`,
`"* "`, `"#"` and `"+"` markers this replaced are gone from the fb backend - see
[`docs/ui.md`](docs/ui.md#srcuiiconc--the-generated-srcuiicon_glyphsc).

**No card weight is spelled out in a renderer either, and no card acts on its own.** A card
names one of three *variants* (`FB_CARD_FILLED`, `_ELEVATED`, `_OUTLINED`) and `fb_draw_card()`
answers with a surface tier - there is no alpha on this panel and nothing to cast a shadow into,
so how far a card is off the ground is carried by its fill, which is Material's tonal elevation.
A card can also carry up to three *verbs*, as buttons against the far edge of its heading line,
and which card carries which is a table in [`src/ui/status.c`](src/ui/status.c) rather than a
fact the renderer holds - because `nav.c` walks the cursor over the same list and
`src/ui/actions.c` names the press in the action bar, and three opinions about one list is how
the button under the cursor and the verb in the bar come to disagree. A focused card is
*derived*: it draws its accent ring because one of its buttons is selected, never because a flag
said so - the same correction the app bar's back arrow made.

**No colour, margin, glyph size or corner radius is spelled out in a renderer.** A screen names
a *tone* (`MESH_UI_TONE_WARNING`), a widget that fills something names a *family*
(`MESH_UI_FAMILY_ERROR`) and takes the fill and its ink together from
`mesh_ui_theme_paint()`, a widget drawing neutral furniture names a *role*
(`MESH_UI_COLOR_SURFACE_SEL`), and anything with corners names a *shape*
(`MESH_UI_SHAPE_FULL`). `src/ui/theme.c` answers all four - which is what makes a theme switch
total instead of a hunt.

The palette is Material's shape: six families - primary, secondary, tertiary, success, warning,
error - of four roles each (`BASE`, `ON_BASE`, `CONTAINER`, `ON_CONTAINER`), over a neutral
spine of surfaces and text. **A fill and the label on it always come from one call**, because
a widget that took them separately would be drawing a pair no theme was measured against.
`enum mesh_ui_state` is a modifier rather than a colour: a selected element is its resting fill
with its own ink mixed in, which is why a chat bubble under the cursor is no longer four extra
roles every theme had to state and match by eye.

Geometry is the same: the margin, the glyph scale, the bubble width and the shape scale all
live in `struct mesh_ui_metrics`. Adding a theme is a table entry, and `mesh_ui_theme_validate()`
holds it to a contrast contract in the tests - looping over the families rather than over a
hand-written list of pairs, so a family cannot be added without all six of its contracts being
checked. See [`docs/ui.md`](docs/ui.md#themes).

`src/ui/backends/fb_widgets.h` is the one exception, and it is deliberate: it is a **component
set**, not a seam. The fb backend stacks `fb_draw.c` (ink) → `fb_widgets.c` (buttons, list rows,
field rows) → `fb_screens.c` (one renderer per screen), so a screen renderer describes its
content and never computes a pixel coordinate, a scroll offset or a padding width. Adding a
widget there is intended; see [`docs/ui.md`](docs/ui.md).

**The full design rationale lives in [`docs/architecture.md`](docs/architecture.md)** — read it
before changing session, settings, updater or node-cache behaviour. Transports are in
[`docs/transport.md`](docs/transport.md), the UI layer in [`docs/ui.md`](docs/ui.md).

## Things that look like bugs and are not

Each of these has cost a debugging round already. **Do not "fix" them back.**

- **No threads.** Everything is the one epoll loop.
- **BLE is not Nordic UART** and carries no length framing: one bare protobuf per GATT
  write/read. Framing is a serial-only concern, and it is `src/proto/stream_framing.c`.
- **The Brick's face buttons do not report by position.** A is `BTN_EAST` (305), B is `BTN_SOUTH`
  (304), the button printed **Y (left) is `BTN_NORTH` (307)**, so X (top) is `BTN_WEST` (308).
  Pinned in `input_brick_face_buttons`.
- **The node roster deliberately outlives the connection.** `mesh_session_reset_handshake` keeps
  `handshake.nodes` and clears everything else; it is not a missed `memset`. The radio's NodeDB
  holds 80 entries and evicts, so mirroring it loses nodes for good. The roster is dropped only
  on a radio swap, and `in_nodedb` marks what the radio no longer carries. **A NodeDB reset does
  not clear it either** - that is why the Status screen can say 2 nodes while the Nodes tab says
  81. Clearing it is a separate, local press (Settings > Radio actions > Forget off-radio /
  Forget all cached, `mesh_session_forget_nodes`), which keeps our own node and every pin.
- **A node with no `User` is named after its node number**, exactly as the phone apps do
  (`mesh_session_default_identity`). An empty `User` in a NodeInfo must not blank a name we have.
- **A radio reboot after a settings write is expected.** The link drops and auto-connect
  reconnects.
- **Key repeat is generated in `input.c`, not by the kernel.** Autorepeat is an EV_KEY/EV_REP
  feature and the d-pad is an absolute axis (`ABS_HAT0X/Y`), which never repeats however long it
  is held. The timerfd in `mesh_ui_input` is what makes holding down scroll a long node list, and
  it deliberately drops the kernel's own `value == 2` for a direction: a direction repeats
  because of our timer or not at all.
- **fb layout is measured in cells, not bytes.** A `strlen` or `%-Ns` there is a bug.
- **The framebuffer needs all three steps** — draw page 0, `FBIOPAN_DISPLAY`, mirror into page 1
  — or the screen is black.
- **Only the release build is a release.** Do not stamp a local build to test the updater; lift
  the guard (`MESHCLIENT_UPDATE_ALLOW_DEV=1`, or Settings → About → Dev updates).
- **Do not edit `project(meshclient VERSION x.y.z ...)`** in `CMakeLists.txt` or bump it by hand;
  the release workflow rewrites that line with `sed`.
- **`launch.sh` and the pak's CA bundle do not ship through self-update.** Only the bare binary
  does. Changing either forces a pak reinstall, so treat them as a compatibility boundary.
- **`scripts/gen-emoji.py` is not part of the build.** Run it by hand and commit the result.
  The same goes for `scripts/gen-icons.py`, which rasterises the icon set out of Material
  Symbols, for `scripts/gen-font.py`, which rasterises the `ui` face out of JetBrains Mono, and
  for `scripts/gen-locale.py`, which turns the string catalog into a translation template or a
  locale skeleton.
- **A card's verbs are on its heading line, not in a row under its content.** Every phone puts
  card actions at the bottom, and that is how it was first written. It cost a row of content per
  card carrying a verb, and the screen it cost them on is the one that can outgrow its panel -
  the Status tab lost the TX queue and the reboot count off the end of the Radio card. A heading
  is three or four cells of a line that is otherwise empty; the verbs go in the rest of it, at
  the chrome scale, and cost nothing.
- **The Status cursor is an index into the verbs its cards offer, so that list may only ever
  grow at its end.** Both verbs are gated on the link being up for that reason as much as for
  their own: a verb appearing *ahead* of the cursor changes what the next A press does without
  the cursor moving. See `mesh_ui_status_actions()`.
- **A card's focus ring is painted inward and is not part of its layout.** The card's edge is in
  the content inset and in the box height, so a ring that widened it would make a card grow when
  the cursor arrived and shift every card below it.
- **A card that can end up with no rows must not be given a verb.** A card with no rows is not
  drawn, and a verb on an undrawn card leaves the action bar naming a press whose button is not
  on the frame. That is why the Radio card says "no report yet" rather than disappearing when
  the radio has told us nothing about itself.
- **A font's cell height is not its cap height.** Anything sized to stand beside the text - an
  icon in a row slot - uses `mesh_ui_font_cap()`. They are equal for `5x7`, whose capitals fill
  its cell, and they are not for a face with real ascenders and descenders; using the cell there
  makes every icon a seventh too big and overflows the confirm dialog's panel.
- **Neither `include/mesh/i18n/catalog.def` nor `include/mesh/ui/icons.def` is a header, and
  `make format` does not touch either.** Each is included several times with the macros defined
  differently each time, which is what keeps the enum, the table and - for the catalog - the
  translation template from drifting apart. Their `.def` extension is why clang-format leaves
  the tables alone.
- **`devtools/` is not `Tools/`.** `Tools/` holds the device-facing pak assets, and macOS
  filesystems are case-insensitive by default, so a `tools/` directory would collide with it.
- **The capture harness cannot act on a `mesh_ui_action`.** START in the keyboard raises
  `SEND_TEXT` and the store stops there; sending is `mesh_app`'s job and there is no app behind
  the harness. A scene stands in for the echo with `message out ...`.

## Protobufs

`MESH_PROTO_NAMES` in `CMakeLists.txt` is a hardcoded list; **adding a new upstream `.proto`
means adding it there.** Headers are included as `meshtastic/<name>.pb.h`. The generator is
`nanopb_generator` from PATH, falling back to `third_party/nanopb/generator/nanopb_generator.py`
(needs `pip install protobuf grpcio-tools`).

## Releasing

semantic-release on `main`/`beta`/`rc`, driven by Conventional Commits. The version rewrite, the
prerelease/`VERSION_OVERRIDE` split, `pak.json`, the four release assets and the release-build
guard are all in [`docs/semantic-release.md`](docs/semantic-release.md). Do not bump versions by
hand. Both fields the Pak Store reads out of `pak.json` — `version` and `changelog` — are
generated during the release; hand edits to either are overwritten.

## Docs map

| Doc | What it covers |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | core design and the reasoning behind it |
| [`docs/transport.md`](docs/transport.md) | BLE, serial, and the Brick USB workaround |
| [`docs/ui.md`](docs/ui.md) | store/nav/backends, fb rendering, fonts and emoji |
| [`docs/i18n.md`](docs/i18n.md) | the string catalog, adding a string, adding a language |
| [`docs/cli.md`](docs/cli.md) | flags, environment variables, on-device controls |
| [`docs/device.md`](docs/device.md) | Brick setup, deploy loop, screenshots, troubleshooting |
| [`docs/settings-roadmap.md`](docs/settings-roadmap.md) | radio settings phases and admin verbs |
| [`docs/components-roadmap.md`](docs/components-roadmap.md) | UI component set audit and the order to close its gaps |
| [`docs/semantic-release.md`](docs/semantic-release.md) | versioning, packaging, release assets |
| [`docs/testing.md`](docs/testing.md) | test categories and how to run them |
