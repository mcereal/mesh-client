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
make fuzz                                 # libFuzzer over the two decoders that read the air
make ui-capture ARGS="<scene> -o x.gif"   # render a UI scene to a GIF, no device needed
make screenshots                          # re-render the five listing stills in .github/resources
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
make docker-screenshots                   # ditto for the listing stills
printf 'scene demo\ntab nodes\nkey down 2\nkey a\n' | ./scripts/ui-capture.sh -o node.gif
```

The README's and `pak.json`'s five stills are scenes too, one per shot in
`devtools/ui_capture/scenes/shots/`, and `make screenshots` re-renders all five - so a UI change
refreshes the listing pictures with a command rather than with a Brick on the desk.

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

Sanitizers: `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or `UBSAN`, or both).
CI runs the suite under both, and cross-builds the pak, on every pull request - see
[`docs/testing.md`](docs/testing.md#what-ci-runs).

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

Verified 2026-09-09: 391 unit tests, all passing, zero compiler warnings - under the host
toolchain *and* the cross one, which are not the same check: see
[`docs/testing.md`](docs/testing.md#what-ci-runs).
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
| Admin protocol | `src/core/radio_settings.c` | `AdminMessage` get/set queue, passkeys, radio actions, NodeDB verbs, the module table, and the four verbs that are not a section (connection status, device UI, canned messages, ringtone) |
| Store & Forward | `src/core/store_forward.c` | the client half of `STORE_FORWARD_APP`: finding a router, asking it for the traffic that arrived while the Brick was off, and folding the replay back into the message log |
| Messaging | `src/core/message.c` | text packets, message ring, ack correlation, and the two fields that make one a threaded reply or a tapback (`reply_id`, `emoji`) |
| Waypoints | `src/core/waypoint.c` | the mesh's shared places: a table keyed by waypoint id, WAYPOINT_APP encode/ingest, and the expiry-in-the-past convention every client deletes with |
| App glue | `src/core/app*.c` | `app` lifecycle/link, `_actions` UI actions, `_publish` to store, `_settings` writes |
| Self-update | `src/core/updater.c`, `version.c` | forks curl, SemVer, digest-verified install |
| Fetching | `src/core/fetch.c` | one HTTPS GET as a forked curl/wget read through the loop: fetcher probing, the CA bundle the Brick has no system store for, a deadline, a cap and a reap that never blocks. Shared by the two things that reach the network |
| Radio firmware | `src/core/firmware.c`, `firmware_catalog.c` | the *other* binary: which board this is (upstream's `deviceHardware`), what the newest release is (its firmware index), and which bus - if any - could carry an install. Reports; installs nothing. See [`docs/radio-firmware-roadmap.md`](docs/radio-firmware-roadmap.md) |
| UI | `src/ui/` | store/controller + `nav*.c` + `settings*.c` + `layout.c` + `history.c` + `backends/{fb*,cli,stub}.c`; **`fb` is the device UI**. `backends/fb_map.c` is the one screen renderer that places things at coordinates rather than describing rows, which is why it is its own file |
| UI components | `src/ui/layout.c`, `src/ui/backends/fb_widgets.c` | cell-measured line builder + scroll window (counted in **steps**, so one row may be taller than its neighbours); cards (filled/elevated/outlined, with verbs on the heading line), buttons, chips, badges, list items (leading/marker/supporting/trailing slots, an optional full-width bar on a second step), section subheaders, switches, selection controls (checkbox/radio), segmented buttons (which fall back to the chosen word when the row is too narrow), meters (with domains and drawn threshold bands), sliders (a settings number on the scale of the values it could have had, with a value the scale cannot place drawn as a track with no handle), signal staircases, sparklines (a reading over time, on the bar's own domain, from a sample ring the client keeps), bubbles (whose trailing run is four typed slots the component measures, never a string a screen assembled), the top app bar, the navigation bar, the screen progress bar, the banner, the action bar, the snackbar |
| Button hints | `src/ui/actions.c`, `include/mesh/ui/actions.h` | what the buttons do here, as (button, verb) pairs the action bar iterates |
| Status verbs | `src/ui/status.c`, `include/mesh/ui/status.h` | which Status card carries which verb — read by `nav.c`, `actions.c` and the renderer alike |
| Help | `src/ui/help.c`, `include/mesh/ui/help.h` | what the client can explain about where the user is standing, as a title, a subject and a list of paragraphs — ids the whole way down. A settings section's notes live on the things they describe (a section's beside its icon in `settings.c`, a field's in its own `k_fields` row) and this assembles them; a *feature's* are a table here, keyed on the route under the help screen |
| Waypoints UI | `src/ui/waypoints.c`, `src/ui/nav_waypoints.c` | the list's order (nearest first, from our own fix), a place's detail rows, and the distance/compass formatting the same two screens read |
| Tapbacks | `src/ui/reactions.c`, `include/mesh/ui/reactions.h` | the fixed emoji set X offers over a bubble: the glyph, which goes on the air unchanged, and the catalog id that names it |
| Delivery marks | `src/ui/delivery.c`, `include/mesh/ui/delivery.h` | which mark an outbound message's ack state gets — the clock, the double tick or the alert circle a bubble's corner draws, and the word a backend with no sprites says for the same state |
| Client-level chrome | `src/ui/chrome.c`, `include/mesh/ui/chrome.h` | what the frame says about the *client* rather than about a screen: whether anything is in flight (the progress bar) and which persistent banner it carries |
| Animation | `src/ui/anim.c`, `src/ui/controller.c` | fixed-point easing + a table keyed per control; the repaint timerfd that feeds it |
| Screen transitions | `src/ui/route.c`, `include/mesh/ui/route.h` | where the nav *is*, as a comparable place - so which way a move went is derived rather than recorded. The backend slides the body from it (`fb_transition_offset`, `fb_shift_begin`) |
| Icons | `src/ui/icon.c`, `src/ui/icon_glyphs.c`, `include/mesh/ui/icons.def` | monochrome Material Symbols, tinted by the theme, in the row slots |
| Themes | `src/ui/theme.c`, `src/ui/font.c` | palette by role, surface tiers, the shape scale, metrics, font registry; `MESHCLIENT_THEME` or Settings > About picks one |
| Fonts | `src/ui/font_ui.c` + generated `font_ui_glyphs.c`, `src/ui/font5x7.c` | a glyph is **coverage**, resampled from the font's master into the cell; `ui` (JetBrains Mono) is the default, `5x7` is the pixel one |
| Text | `src/utils/text.c`, `src/ui/{font5x7,emoji}.c` | UTF-8 sanitising, cell-based measurement |
| Strings | `src/i18n/strings.c`, `include/mesh/i18n/catalog.def` | the string catalog and the locale registry |
| Dev tools | `devtools/`, `scripts/{ui-capture.sh,frames.py}` | off-screen UI capture; PNG/GIF encoding, stdlib only |
| Geography | `src/geo/` | `mesh_geo_coords_valid()` — the bounds test every coordinate ingress asks, so the air, the cache and the keyboard cannot disagree about where Earth ends — `mesh_geo_vector_between()`, the haversine distance and initial bearing the Waypoints tab reads a range from, and `mesh_geo_mercator_forward()`, the projection the map places a marker with. **The only directory in the tree that includes `<math.h>`**, and the reason libm is linked |
| The map | `src/map/viewport.c`, `src/ui/map.c`, `src/ui/nav_map.c`, `src/ui/backends/fb_map.c` | Where the map is looking (centre, integer zoom, pan, fit, metres per pixel), the markers built from the map's own roster (`handshake.map_nodes` - **not** the node list's 128) and the waypoint book, the presses, and the drawing. No basemap yet — see [`docs/maps-roadmap.md`](docs/maps-roadmap.md). `viewport.c` deliberately has **no `<math.h>`**: everything transcendental about a map is a property of the projection, one directory down |
| Shared utils | `src/utils/` | `text` (UTF-8 + `mesh_str_copy`), `time` (`mesh_time_monotonic_ms`), `env` (`mesh_env_bool`/`_int`), `json` (a cursor that walks structure, because a release note eventually contains the keys a scanner would look for), `log`, `sha256`, `array` |

`include/mesh/` mirrors `src/` one-for-one — `core/`, `transport/`, `ui/`, `proto/`, `geo/`, `utils/` —
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

**No delivery state is spelled out in a renderer either, and none of them is a word.** `"ok"`,
`".."` and `"!!"` are gone from the string catalog: each was two cells of punctuation standing
for a state, which is the marker-gutter mistake one level in. A bubble's corner draws a clock, a
double tick or an alert circle, and which state gets which is [`src/ui/delivery.c`](src/ui/delivery.c)
rather than a switch in the transcript - the same rule `status.c` and `chrome.c` follow. The
words stay in the catalog because the mark has to be nameable: a text backend has no sprites.
And a *reason* is not a mark - it is a sentence, so it goes under the message as a wrapped
supporting line inside the bubble, which is also what stopped it from pushing the corner run
past the bubble's own width. See [`docs/ui.md`](docs/ui.md#struct-fb_bubble--the-transcripts-one-component).

**No row marker is spelled out in a renderer either.** A row that opens something, one with an
unsaved edit, a channel, a node the radio has forgotten: each names an *icon*
(`MESH_UI_ICON_CHEVRON`) in one of the list item's slots, and `src/ui/icon.c` answers with a
monochrome sprite the row draws in its own ink. The set is one line per icon in
`include/mesh/ui/icons.def`; adding one is a line there plus `scripts/gen-icons.py`. The `"> "`,
`"* "`, `"#"` and `"+"` markers this replaced are gone from the fb backend - see
[`docs/ui.md`](docs/ui.md#srcuiiconc--the-generated-srcuiicon_glyphsc).

**No row height is spelled out in a renderer either.** A list row is however many *steps* the
list model says it is, and a step is one body row. The screen measures - it is the only thing
that knows whether a row carries a bar - and hands `fb_list_begin_heights()` an array, exactly
as the transcript hands `mesh_ui_transcript_window()` one; from there the **model is the
authority**, and every entry point that advances a row advances by `fb_list_row_height()`
rather than by what the item it was given looks like. A screen that forgets to declare a tall
row draws it short, which is visible, rather than over the row beneath it, which is not. The
window, the highlight and the scroll thumb are three sums of the same heights - see
[`docs/ui.md`](docs/ui.md#rows-that-are-not-all-the-same-height).

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

**No setting explains itself in a renderer either, and most settings do not explain themselves
at all.** What a setting *does* is a property of the setting, so it is `note` on its own
`k_fields` row and a table beside `k_section_icons[]` — never a sentence on a screen. SELECT
opens `src/ui/help.c`'s answer for wherever the nav is. Four things about it are rules rather
than observations: **help is per section, not per row**, because most fields need no note and a
key that did nothing on two rows in three is the keycap `actions.c` refuses everywhere else;
**every section has a note and a field's is optional**, which is what makes the press always
worth offering; **the bar and the press ask the same function**, so a keycap that does
nothing is not expressible; and **a screen that is not a list of fields is keyed on the route**
(`k_help_features[]`, read through `mesh_ui_route_under_help()`) rather than on the nav's flags,
so a new way of reaching a screen arrives with the right help already attached. A note is at most
200 characters and a locale may leave it untranslated — the one class of string that may. See
[`docs/help.md`](docs/help.md).

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
  Pinned in `input_brick_face_buttons`. The pad impersonates an Xbox 360 controller, and the rest
  of the case follows from that: **L2/R2 are the analog triggers `ABS_Z`/`ABS_RZ`**, not buttons
  (there is no `BTN_TL2`/`BTN_TR2` in the bitmap at all), and **F1/F2 are the stick clicks**
  `BTN_THUMBL`/`BTN_THUMBR` - a 360 pad has two sticks and the Brick has none, so those were the
  free codes. The pad also *declares* a `KEY_F1`, a `KEY_F2` and two volume keys it never sends,
  which is why the map is measured with `make deploy-input-map` rather than read off the
  capability bitmaps. The whole table is in [`docs/device.md`](docs/device.md#the-buttons-and-what-they-report).
- **The power button is deliberately not a quit key.** It was one until the Brick was measured:
  the PMIC (`axp2202-pek`, its own input device) really does emit `KEY_POWER`, so a tap of the
  button - this hardware's sleep gesture - tore the client down instead of suspending it. Sleep
  is the launcher's business. `MESHCLIENT_QUIT_KEYS` still overrides the set.
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
- **The BLE device list is not a list of nodes in range, and `rssi` is not a range test.** The
  enumeration behind it is `GetManagedObjects`, a walk of every device object BlueZ *holds* -
  and a bond outlives the radio being in the room, so a node switched off in another building
  sits in that list all day with its address, its name and `Paired` intact. What it does not
  have is an `RSSI` property: bluetoothd drops that from a device it has not heard in the
  current discovery session. Hence `mesh_bluez_device_info.in_range`, and hence the filter every
  selection path applies before it looks at anything else. Reading the absence as a number is
  worse than useless: 0 is a *high* RSSI, so an out-of-range bond beat every node that actually
  answered - which is what sent the client after the radio left at home while the one in the
  user's pocket advertised into an empty list. A row in the Devices tab says "not in range"
  for the same reason rather than "0dBm".
- **Key repeat is generated in `input.c`, not by the kernel.** Autorepeat is an EV_KEY/EV_REP
  feature and the d-pad is an absolute axis (`ABS_HAT0X/Y`), which never repeats however long it
  is held. The timerfd in `mesh_ui_input` is what makes holding down scroll a long node list, and
  it deliberately drops the kernel's own `value == 2` for a direction: a direction repeats
  because of our timer or not at all.
- **fb layout is measured in cells, not bytes.** A `strlen` or `%-Ns` there is a bug.
- **The map's d-pad does not move a cursor, and its shoulders do move tabs.** It is the one
  screen where Left and Right are not the tab keys: `nav_map.c` takes the four directions ahead
  of the routing in `nav.c` that turns them into a change of tab, because Left on a map means
  "look west". The shoulders are deliberately *not* taken, which is what pays for it - the two
  pairs are the same press on every other screen, and splitting them here is what lets the map
  have the d-pad without the tab strip above the body going dead. The action bar still says
  "L/R tabs" here and still means it.
- **A direction on the map stops on the next marker that way, and only pans when there is
  none.** It looks like snapping bolted onto a pan and it is the other way round: a pan of a
  fixed number of pixels leaves the crosshair on a lattice (a fifth of the body across, a fifth
  down) and the crosshair captures a disc of `MESH_UI_MAP_SELECT_RADIUS_PX`, so a sixth of the
  plane was selectable and five markers in six could not be aimed at *at all* at a given zoom -
  which on the device reads as the crosshair skipping over nodes, and as zooming sometimes
  fixing it, because a zoom re-phases the lattice. `mesh_ui_map_step()` takes the nearest marker
  in the 45-degree quadrant around the press (the four tile the plane, so nothing on the panel
  is unreachable) and the view centres on its own coordinates, which is what makes the landing
  exact. The fallback pan is not a leftover: it is what crosses open grid, and what walks a
  marker beyond the step's reach into it. The selection is still *derived* from the centre - the
  nav grew no selection field, and must not.
- **`map_open` outliving a change of tab is deliberate, and the key handler must still check
  `nav->screen`.** Every tab keeps its own place, so coming back to Nodes shows the view that was
  left — which means the flag says *where the Nodes tab is standing*, not *what the reader is
  looking at*. Two presses make the difference: a shoulder walks off the tab with the map still
  open behind it, and A on a waypoint marker jumps to the Waypoints tab outright. Read as "a map
  is open somewhere", the arrows pan a map nobody can see and the first B on that place closes it
  instead of the place. `mesh_ui_nav_map_key()` gates on the screen for that reason.
- **The map clips its artwork to its own body, and `visible` is not enough on its own.** A
  placement can only honestly speak for a marker's *centre*, but a marker is not a point once it
  is drawn: a rounded-position footprint is the widest thing the map places, so a marker centred
  a pixel inside the top edge paints most of itself over the app bar. The clip goes on the fb
  state, where `fb_fill_packed()` already honours one — every fill, glyph and icon span goes
  through that one function — and it *intersects* the partial-redraw path's clip rather than
  replacing it.
- **The map draws a different roster from the Nodes list, and it is not a subset.** The list
  publishes the best 128 of the session's 256 (`handshake.nodes`), because a rank says how
  likely you are to talk to a node; a marker is on the panel or it is not, so the map gets
  `handshake.map_nodes` - every *positioned* node the session holds, as a 36-byte
  `struct mesh_ui_map_node` rather than the 532-byte summary, which is why the snapshot grew by
  9 KB instead of 68. Two consequences. A handshake nobody published (a cache load before the
  first publish, a fixture, the capture harness) has `map_node_count == 0` and
  `mesh_ui_map_build()` falls back to the rows - a default, not a second opinion, because
  publish scans a superset of the rows it copies. And **map-only nodes now exist**: a node
  ranked 200th has a marker and no row, so `mesh_ui_node_detail_find()` cannot answer for it and
  A on that marker deliberately does nothing (`marker->openable`, from the published `has_row`),
  exactly as A on empty grid does. Without that guard the detail opens, cannot be filled, and is
  clamped shut on the next press. Resolving it properly is the app/store seam
  [`docs/maps-roadmap.md`](docs/maps-roadmap.md#the-roster-decision-taken) describes.
- **The map has no selection field on the nav, and must not grow one.** What A opens is the
  marker nearest the middle of the view, derived every frame by `mesh_ui_map_selected()`. That is
  the app bar's back arrow and the transition route again: a second opinion about the nav is a
  second opinion that can be wrong, and here it would let the ring a backend draws and the node a
  press opens name two different nodes. It works because the distance is measured **in pixels
  from the middle of the view**, through `mesh_map_viewport_offset()` — which is the same
  arithmetic a placement does with the panel left off the end, so `mesh_map_viewport_place()` is
  written in terms of it. Two properties come out of that and both are load-bearing. It needs no
  box: the store owns the nav and a backend is handed a `const` snapshot, so the two genuinely
  cannot ask each other how wide the body is, and anything box-dependent there is the bug. And it
  is measured *in the projection* rather than across the ground, which is the only reading that
  gets the poles right — a fix beyond the display limit is drawn at the limit, so a marker at 88
  degrees north and a view framed on it are the same point on the picture and three degrees apart
  on Earth. A geodesic distance there refuses a marker sitting under the crosshair.
- **The map's fit is computed against a declared box, not a measured one.** For the same reason:
  the nav cannot learn a backend's body size. `MESH_UI_MAP_FIT_WIDTH` is deliberately *smaller*
  than any body this client draws into, because a fit computed for a small box and drawn into a
  larger one leaves extra air, where the opposite clips a marker off the edge.
- **A reply is aimed by the press that opened the sheet, not by the cursor when it sends.** A on
  a bubble records that packet id in `nav.reply_to`, and whatever is written or picked over the
  thread carries it; the transcript keeps moving underneath, so reading the cursor at send time
  would re-aim the reply at whatever a message arriving mid-compose had slid under it. Y clears
  it on purpose - the action bar calls Y *write*, and a new message to a conversation is not an
  answer to the last thing said in it.
- **A reaction deliberately goes out without want_ack.** It has no bubble - the transcript
  filters it out and draws it as a chip on the message it names - so a delivery mark it earned
  would be one nothing on the frame could ever draw, bought with a retransmit round on a shared
  band. And a reaction with no `reply_id` is `-EINVAL` rather than an ordinary message: the whole
  of what a tapback is, is what it is about.
- **A bubble's trailing run is typed slots, not a string, and it is dropped rather than
  truncated.** The reactions, the padlock, the clock and the delivery mark were once
  concatenated by the screen; the bubble measured that string, clamped its *box* to three
  quarters of the body, and right-aligned the *string* inside the box it had clamped - so a
  failure reason or a fourth reaction chip came out of the left edge, painting a line of the
  message on bare background outside its own bubble. `fb_bubble_run()` now measures the parts
  once for the measure and the draw and drops them off the *front* until they fit, so a reaction
  chip is what is lost and the mark saying the message failed is what survives. Concatenating
  them back into one string reintroduces the bug, and `ui_capture_bubble_contains_its_own_ink`
  is what catches it.
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
- **The screen progress bar deliberately costs no body row, and the banner deliberately costs
  rows.** They are the moving half and the settled half of one idea: a request already sent must
  not reflow the list it went out from, so the bar hangs in the gap the navigation bar already
  leaves and takes a `const` layout; a banner is content about the client, so it consumes rows
  and hands back what is left, exactly as the app bar does. Which states raise either is
  `src/ui/chrome.c`, never a renderer.
- **A banner says only what nothing else on the frame says, and must be able to resolve.** That
  is why there is no "radio disconnected" banner - the status line under the keycaps already
  says it on every frame - why the update banner stands down inside Settings > About, and why an
  update a build cannot install raises nothing. There is no dismissal, on purpose: dismissal is
  a nav change, and refusing it is what keeps the table to states that go away on their own.
- **The Status cursor is an index into the verbs its cards offer, so that list may only ever
  grow at its end.** Both verbs are gated on the link being up for that reason as much as for
  their own: a verb appearing *ahead* of the cursor changes what the next A press does without
  the cursor moving. See `mesh_ui_status_actions()`.
- **A card's focus ring is painted inward and is not part of its layout.** The card's edge is in
  the content inset and in the box height, so a ring that widened it would make a card grow when
  the cursor arrived and shift every card below it.
- **A slider's stops are evenly spaced, and its zero may not be on it at all.** A `NUMBER`
  setting steps a preset list that climbs geometrically, so the handle is placed in *stop* space -
  placing it in value space crowds eight of screen-on's ten choices into the first sixth of the
  track. And most of those lists open with a 0 the field reads as a word: "the firmware's
  default", and on LoRa's transmit power "as much as this radio has". Neither is a quantity, so
  `SCALE_PRESETS_AFTER_ZERO()` stands it outside the track and the row draws its stops with no
  handle anywhere - drawn the other way, `max` reported itself at the empty end of its own bar.
  The same goes for a list that merely *starts* above zero (map reporting, neighbour info): the
  test is anything under the first stop, not the field's word for it. And the stops are cut into
  the track **after** both halves are filled, exactly as a meter's band boundaries are - painted
  before the fill they are gaps the fill closes, and a full track shows none of them.
  Which lists are a scale at all is stated per field (`SCALE_PRESETS` / `NAMED_PRESETS`) and is
  not derivable: `{0, 1, ... 7}` is a hop limit under one field and a GPIO pin under the next.
- **Three settings rows are shown and cannot be pressed, and that is the point.** The radio's
  screen and settings locks (`store_ui_config` can turn them on and no verb turns them off, and
  the PIN behind them is not on the wire), the **ringtone** (RTTTL is 231 bytes against a
  `MESH_UI_SETTING_TEXT_MAX` of 80, and a d-pad keyboard is not a way to enter one), and the
  radio's **language** - which is the only enum in the client whose wire values are not
  `0..n-1`: they run 0..19 and then jump to 30 and 31, while every enum row steps by
  `(value + 1) % count`, is named by value, and is drawn by a segmented button that names by
  *index*. Offering it is an index/value split across three files, not a row. All three follow
  the rule the MQTT proxy row set: a setting that cannot be pressed is still the answer to why
  the radio is behaving as it is.
- **`DeviceUIConfig` is kept whole and written back whole.** It carries a touchscreen
  `calibration_data` blob and a map home point the client has no rows for, so a
  `store_ui_config` built from the rows alone would erase a screen's calibration - invisibly,
  until somebody touched their radio.
- **The canned message slots are six of 32 characters because the wire is one 200-byte
  string.** The count and the length are one decision, not two: six plus five separators is
  197 and seven would not fit. Empty slots are listed (the Channels rule), and a radio holding
  more than six keeps them - the save copies the tail across and closes the gaps an emptied
  slot leaves, the way a cleared admin key is compacted.
- **A card that can end up with no rows must not be given a verb.** A card with no rows is not
  drawn, and a verb on an undrawn card leaves the action bar naming a press whose button is not
  on the frame. That is why the Radio card says "no report yet" rather than disappearing when
  the radio has told us nothing about itself.
- **A replayed message has no date, and that is what makes the de-duplication work.**
  `mesh.proto` says of `rx_time` that the field "is _never_ sent on the radio link itself (to
  save space)", so the stamp on a Store & Forward replay is *our own* radio marking when the
  replay landed, not when the message was said - and the `StoreAndForward` `text` variant
  carries no timestamp to use instead. Copying it would date the whole window at the minute it
  was fetched, and would defeat `mesh_message_log_holds_replay()`, whose stamp comparison only
  falls through to the text when one side is 0. The SNR, the hop count and the padlock are left
  off for the same reason the roster is not touched: they measure the router's link, not the
  sender's.
- **A replayed message has a packet id, and it is not its own.** A Store & Forward router wraps
  the message in a packet of its own, so the id on the copy identifies the *delivery*; the
  original we may already be holding has a different one. That is why
  `mesh_message_log_holds_replay()` matches on what was said - sender, channel, text, and the
  stamp when both copies carry one - rather than on the id, and why the replayed entry's own
  `packet_id` is left at 0 rather than filled with a number that would look like a correlation
  handle. Without it, one press puts a second copy of the last four hours under the first.
- **A replay counts twice, and the two numbers are different facts.** A router hands back its
  whole configured window, which for a client that was off for ten minutes is mostly traffic it
  heard live - so `received` is the router's work and `stored` is the user's gain, and a row
  saying "30 messages" about a replay that added none of them would be describing the wrong one.
- **The history request looks for a router before it asks one, and never broadcasts itself.** A
  router announces itself every fifteen minutes by default, so a client that could only ask one
  it had already heard from would be useless in the minutes after a boot: with none known the
  press broadcasts a `CLIENT_PING` and the real request follows the pong. A broadcast
  `CLIENT_HISTORY` would have every router on the mesh replay its window at once, and
  `mesh_store_forward_encode()` refuses one.
- **The history cursor belongs to one router, and hearing another drops it.** The `.proto`
  calls it an index into *the server's* packet history, so sending router A's `last_request` to
  router B asks B to skip to a position in a table it does not have - B would silently return
  fewer messages, which is this feature failing in the one direction no screen could show. The
  rank and the statistics go with it. While a request is running, an announcement or a refusal
  from any other node is ignored; a replayed *message* cannot be checked that way, because its
  envelope names the sender rather than the router that relayed it.
- **The follow-up request goes out from the tick, not from the ingest that armed it.** The pong
  arrives on the link's read path, and writing back down the link on the same turn is what the
  admin queue's queue-here-drain-there split exists to avoid.
- **Deleting a waypoint is broadcasting it again with an expiry in the past.** There is no
  "unshare" packet: every Meshtastic client withdraws a place by re-sending the `Waypoint` with
  `expire` already gone, and the receiver drops it. So `mesh_session_forget_waypoint()` sending
  what looks like another copy of the thing it is deleting is the protocol, not a bug - and it
  only sends one when `locked_to` says this client may, because asking the whole mesh to forget
  somebody else's place is not ours to do. The local entry goes either way.
- **A waypoint's expiry is only honoured once we know what time it is, and a tombstone always
  is.** The Brick has no RTC battery, so with no network it boots into the epoch and
  `time(NULL)` is a small positive number - which means "has this expired?" is usually
  unanswerable, and answering it from that clock would report every deadline as decades away.
  But "was this expiry a moment in 1970?" needs no clock at all, and that is exactly what a
  withdrawal is. Hence the two halves of `mesh_waypoint_state()`, and hence
  **`mesh_time_wall_credible_s()`**: `mesh_time_wall_s()` answers whatever the machine says,
  which is what an *age* wants because every caller that draws one already refuses a negative or
  enormous one, and a *deadline* has no such safety. Expiry is read against the credible clock
  in all three places that read it - ingest (a place that arrived expired is dropped),
  `mesh_session_tick()` (nothing re-announces an expiry, so the clock is the only thing that can
  retire one) and the detail's countdown.
- **The waypoint book is a table keyed by id, not a ring.** Upstream *edits* a waypoint by
  re-broadcasting it with the same id, so the second copy lands on the first. The message log
  next door does the opposite on purpose - two packets are two things that happened - and a
  waypoint arriving twice is one place that moved.
- **A place this client made is kept even when the send fails, and `-ENOTCONN` still stores
  it.** Naming a place is not a message that failed to go out: it is something the user made, it
  is theirs with or without a radio, and the next share re-broadcasts the same id. That is why
  `mesh_session_send_waypoint()` does not check the link before storing.
- **A radio swap takes the waypoints with the roster, and a reconnect does not.** This is the
  roster's rule rather than the message log's, and the channel index is why: a message's channel
  is a label on something that already happened, while a waypoint's is an index into the channel
  table the swap has just discarded - so a place carried across would have "Share it again"
  broadcast on whatever slot that number names on the *new* radio.
- **Waypoints are deliberately not persisted with the roster.** The roster is what we *know* and
  is worth keeping because a node the radio evicted is gone for good; a waypoint lives on the
  mesh and its sharer can withdraw it. A cache would put back places the mesh had already agreed
  were gone, because the withdrawal that arrived while the client was off is a packet nobody
  replays.
- **The Waypoints list is never empty, and the row that makes a place is the last one.** Every
  other tab can fall through to a picture when it has nothing; this one always has something to
  do, and a screen that replaced that row with an empty state would take it away. The row says
  why it cannot be pressed - "no position yet" - rather than disappearing, which is the ordinary
  case on a Brick, whose radio often has no fix.
- **`MESH_WAYPOINT_NAME_MAX` is declared twice and that is the seam working.** `store.h` is
  nanopb-free by construction and `mesh/core/waypoint.h` takes a `meshtastic_MeshPacket`;
  including one from the other to reach three numbers would drag the generated protobuf headers
  into every backend and every test that draws a screen. `MESH_UI_MESSAGE_TEXT_MAX` already does
  the same, and `waypoint_limits_agree_across_the_seam` is what holds the two honest.
- **A fix carries two clocks and the row says which one it is answering with.** `Position` has
  `timestamp` (when the GPS solved) and `time` (the sender's own clock, which upstream leaves
  off the mesh to save space, so it is usually 0); neither is when the packet reached us, which
  is why `mesh_node_position` also keeps `received`. The node detail draws **Fix** against the
  node's own dating and **Fix heard** against ours, and the label change is the point - falling
  back silently would put our arrival time under a heading that reads as the node's. `last_heard`
  is not a candidate for either: it advances on any packet, so a chatty node that has not moved
  in a day would report a one-minute-old fix.
- **A `precision_bits` of 0 on a received fix means "the node did not say", not "off".** The
  same `mesh_ui_settings_format_precision()` renders 0 as *off* for the channel's own
  `position_precision`, where it is a setting with an off state; on a fix off the air it is an
  absent field, so the node detail guards on `> 0` and draws no row rather than asking. And the
  row reads a distance rather than a bit count because a bit count is not a fact a reader can
  act on - the one table answers for both screens so a rounded location cannot be described two
  ways.
- **`(0, 0)` is a valid coordinate.** It is where a node with a half-initialised GPS most often
  claims to be, and it is also a real point in the Gulf of Guinea. `mesh_geo_coords_valid()` is
  a *range* check and nothing more; rejecting Null Island there would be a guess about the
  sender's firmware in a range check's clothes. A packet with no coordinates at all is the
  separate question, and its answer is to keep the last fix rather than to erase it.
- **A font's cell height is not its cap height.** Anything sized to stand beside the text - an
  icon in a row slot - uses `mesh_ui_font_cap()`. They are equal for `5x7`, whose capitals fill
  its cell, and they are not for a face with real ascenders and descenders; using the cell there
  makes every icon a seventh too big and overflows the confirm dialog's panel.
- **Neither `include/mesh/i18n/catalog.def` nor `include/mesh/ui/icons.def` is a header, and
  `make format` does not touch either.** Each is included several times with the macros defined
  differently each time, which is what keeps the enum, the table and - for the catalog - the
  translation template from drifting apart. Their `.def` extension is why clang-format leaves
  the tables alone.
- **A trend's axes are not its data.** A sparkline's x is *time* and its y is the reading's own
  `struct mesh_ui_scale` - the same one the bar beside it fills against - never the range the
  samples happen to span. Every spreadsheet does the opposite, and on the two readings this draws
  it is wrong both times: a battery that fell two percent overnight becomes a cliff, and a quiet
  mesh becomes a mesh in trouble. A silence longer than the series' own `gap_ms` breaks the line
  rather than sloping across it, for the same reason - and so does a reading that was *refused*
  rather than missing, which the clock cannot see: a node on external power reports punctually
  and reports something that is not a level, so `mesh_ui_series_break()` is how the source says
  the next reading starts a segment. And **the history is never persisted**: the
  roster is what we know and survives a restart, a trend is what we *watched*, and the hours the
  client was not running are not a silence it can draw.
- **A history sample is stamped with the client's clock, and a new reading is detected by the
  report having changed.** The radio's own `time` fields are our clock when the packet landed,
  and a Brick has no wall clock - so on the device they are 0 on every report, and a series keyed
  on either question would hold exactly one sample forever.
- **The radio's firmware rows say short values, never sentences.** A settings row has no
  supporting line to wrap onto and the value column is about two dozen cells, so
  `"%s available (radio has %s)"` came out as `2.7.26.54e0d8d available (` - a row that reads as
  a bug. What carries the difference is the row's *name*: once the answer is a version worth
  having the label reads "Newer firmware" and the value is the bare version, which is the move
  About's own `Install %s` row made first. The long form of a failure goes to the log, where a
  sentence has room.
- **`devtools/` is not `Tools/`.** `Tools/` holds the device-facing pak assets, and macOS
  filesystems are case-insensitive by default, so a `tools/` directory would collide with it.
- **A screen transition is derived from the nav, not declared by it.** Nothing records that a
  press went in or out. `mesh_ui_route_of()` reads the nav and says which *place* it is showing,
  the backend remembers the last one, and the difference between two is the direction - so a new
  way to open a level animates correctly without being told to. A field on `struct mesh_ui_nav`
  is the thing this deliberately is not: eleven call sites open a level and a forgotten one
  animates backwards, which fails no build and is invisible in a screenshot. The cursor, the
  draft and every armed press are excluded on purpose - a route that moved with the cursor would
  restart the slide on every press of Down.
- **Only the arriving screen is drawn, and it travels a quarter of the panel, not all of it.**
  There is no alpha here and nothing can read back what is on the panel, so the outgoing screen
  cannot be carried along. A full-panel travel therefore leaves the body *empty* on the frame the
  press lands - one blank frame, every time. A quarter is also Material's shared-axis
  displacement, and it keeps the arriving screen legible for the whole move.
- **The navigation bar, the action bar, the progress bar and the banner do not slide with the
  body.** The transform wraps the screen renderer only. A frame-wide offset would be the client
  saying the whole application had been replaced when one level of one tab did; the two client
  bars are drawn before the transform because the client did not go anywhere.
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
| [`docs/help.md`](docs/help.md) | the in-client help screen, where a note lives and what it may say |
| [`docs/cli.md`](docs/cli.md) | flags, environment variables, on-device controls |
| [`docs/device.md`](docs/device.md) | Brick setup, deploy loop, screenshots, troubleshooting |
| [`docs/settings-roadmap.md`](docs/settings-roadmap.md) | radio settings phases and admin verbs |
| [`docs/radio-firmware-roadmap.md`](docs/radio-firmware-roadmap.md) | updating the *radio's* firmware - UF2 over USB, OTA over BLE - what is reachable and in what order. Phase 1 (report only) has shipped |
| [`docs/components-roadmap.md`](docs/components-roadmap.md) | UI component set audit and the order to close its gaps |
| [`docs/semantic-release.md`](docs/semantic-release.md) | versioning, packaging, release assets |
| [`docs/testing.md`](docs/testing.md) | test categories and how to run them |
