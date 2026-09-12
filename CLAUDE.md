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
make demo-pack                            # draw a synthetic tile pack into build/demo.mctp
make screenshots                          # re-render the five listing stills in .github/resources
```

**A UI change is shown, not described.** `make ui-capture` drives the HUD through a scripted
sequence of button presses and renders every frame off-screen - the real nav model and the real
`fb_render_snapshot()`, drawing into memory instead of `/dev/fb0` - so a change is reviewable as
a picture from a container or a cloud session. A GIF rather than a still because most UI changes
are about a transition, and `-g WxH` renders the same scene into a panel other than the Brick's -
the frame is measured into whatever geometry it is given, so what moves between two of those is
the layout rather than the picture's scale. Scene scripts and the command list are in
[`docs/ui.md`](docs/ui.md#looking-at-a-ui-change); examples in `devtools/ui_capture/scenes/`.

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/messages.scene -o messages.gif"
make ui-capture ARGS="-t light devtools/ui_capture/scenes/messages.scene -o light.gif"
make ui-capture ARGS="-g 1280x800 devtools/ui_capture/scenes/messages.scene -o wide.gif"
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
make deploy-start                         # start the UI as Tools > MeshClient does; NextUI steps aside
make deploy-stop                          # stop every MeshClient on device, bring NextUI back
make deploy-run                           # deploy-start + follow the log; stops the client on exit
make deploy-run ARGS="--list-devices"     # print-and-exit flags run headless, output here
```

**An on-device test ends with `make deploy-stop`.** Starting the client is `make deploy-start`
(or `deploy-run`), never `launch.sh` from a device shell: that runs the client *beside* NextUI's
launcher, which keeps painting `fb0` and acting on every button, so the user's first L1 flashes
the launcher's menu through the HUD. Killing the host side of a run does not stop the client on
the device, so a test that does not end with `deploy-stop` leaves one running behind the
launcher. See [`docs/device.md`](docs/device.md#starting-it-from-the-mac).

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

Verified 2026-09-11: 569 unit tests, all passing, zero compiler warnings - under the host
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
| Transports | `src/transport/` | registry + BLE (BlueZ/D-Bus) + serial (USB) + TCP (network). `stream_link.c` is the half the serial and TCP links share - descriptor, frame parser, framed write queue with its partial-write cursor and `EPOLLOUT` arming - because their wire is one format and only the getting-connected half differs |
| Session | `src/core/session.c` | handshake, node roster, channels, message log, packet ids |
| Admin protocol | `src/core/radio_settings.c` | `AdminMessage` get/set queue, passkeys, radio actions, NodeDB verbs, the module table, and the four verbs that are not a section (connection status, device UI, canned messages, ringtone) |
| Store & Forward | `src/core/store_forward.c` | the client half of `STORE_FORWARD_APP`: finding a router, asking it for the traffic that arrived while the Brick was off, and folding the replay back into the message log |
| Messaging | `src/core/message.c` | text packets, message ring, ack correlation, and the two fields that make one a threaded reply or a tapback (`reply_id`, `emoji`) |
| Waypoints | `src/core/waypoint.c` | the mesh's shared places: a table keyed by waypoint id, WAYPOINT_APP encode/ingest, and the expiry-in-the-past convention every client deletes with |
| App glue | `src/core/app*.c` | `app` lifecycle/link, `_actions` UI actions, `_publish` to store, `_settings` writes |
| Self-update | `src/core/updater.c`, `version.c` | forks curl, SemVer, digest-verified install |
| Fetching | `src/core/fetch.c` | one HTTPS GET as a forked curl/wget read through the loop: fetcher probing, the CA bundle the Brick has no system store for, a deadline, a cap and a reap that never blocks. Shared by the two things that reach the network |
| Radio firmware | `src/core/firmware.c`, `firmware_catalog.c`, `firmware_download.c`, `firmware_fetch.c`, `firmware_install.c`, `firmware_ota.c`, `firmware_update.c`, `uf2.c`, `esp_image.c`, `src/utils/zip.c`, `src/transport/serial/usb_msc.c`, `src/transport/ble/ble_ota.c`, `ble_hci.c` | the *other* binary. `firmware.c` says which board this is (upstream's `deviceHardware`), what the newest release is (its firmware index) and which bus - if any - could carry an install; `firmware_fetch.c` turns that into a zip URL and a member name; `firmware_download.c` range-reads the member out of a 46 MB zip and inflates it through the device's own `gzip`, which is also where the CRC gets checked; `uf2.c` refuses a file that is not for this chip; `firmware_install.c` sends the radio into DFU, waits for its bootloader to enumerate and writes the blocks, and `usb_msc.c` is the drive half of that - finding the block device, taking the platform's mounts off it, and the forked child that copies. The BLE half is the ESP32's: `firmware_ota.c` sends `ota_request` holding the radio to the image's SHA-256, finds the loader at the radio's address plus one, streams and watches the radio come back, retrying a broken transfer from the start; `ble_ota.c` is the loader's text protocol (one chunk per GATT write, one outstanding, `mtu - 3` bytes); `ble_hci.c` asks for a 7.5 ms interval with a raw `LE Connection Update`, because BlueZ has no call for it and the Brick would hold the loader at 30 ms; `esp_image.c` refuses an image for another chip before the radio is asked anything. `firmware_update.c` is the **composition**: the download, the arm and the handover in order, with those three modules' state enums folded onto one ladder a row can name and their errors onto the four a reader acts on - and it is what makes Settings > About radio > Install firmware a press rather than a command line. The two questions it answers for the app are deliberately separate: a download holds the *antenna* (Wi-Fi and Bluetooth are one part) and a handover holds the *radio*, and collapsing them would leave the BLE link down through the one step that is waiting for it to come back. `--fetch-firmware` runs the download and `--install-firmware` runs the whole thing from the CLI. See [`docs/radio-firmware-roadmap.md`](docs/radio-firmware-roadmap.md) |
| UI | `src/ui/` | store/controller + `nav*.c` + `settings*.c` + `layout.c` + `history.c` + `backends/{fb*,cli,stub}.c`; **`fb` is the device UI**. `backends/fb_map.c` is the one screen renderer that places things at coordinates rather than describing rows, which is why it is its own file |
| UI components | `src/ui/layout.c`, `src/ui/backends/fb_widgets.c` | cell-measured line builder + scroll window (counted in **steps**, so one row may be taller than its neighbours); cards (filled/elevated/outlined, with verbs on the heading line - and, in a *scrolling* list, as a surface behind a run of rows the list already places, cut square where the window cut it), buttons, chips, badges, list items (leading/marker/supporting/trailing slots, an optional full-width bar on a second step), section subheaders (indented to their list's leading gutter, carrying the card's icon when the list is a column of cards and nothing when it is not, and never a row the cursor stops on), switches, selection controls (checkbox/radio), segmented buttons (which fall back to the chosen word when the row is too narrow), meters (with domains and drawn threshold bands), sliders (a settings number on the scale of the values it could have had, with a value the scale cannot place drawn as a track with no handle), signal staircases, sparklines (a reading over time, on the bar's own domain, from a sample ring the client keeps),
proportion bars (a whole and the disjoint parts it is made of, in the theme's categorical
series palette rather than in tones, with the parts filling the track exactly and a part that
is there never rounded away to nothing), the chart (`fb_draw_chart()`: the same readings a
sparkline draws in a row, in a whole body, with its domain's ends labelled, its time span named
and picked, its thresholds ruled across the plot, its legend saying where each line has got to,
and more than one line on it - the one component here that is a screen rather than a slot), bubbles (whose trailing run is four typed slots the component measures, never a string a screen assembled), the top app bar, the navigation bar, the screen progress bar, the banner, the action bar, the snackbar |
| Button hints | `src/ui/actions.c`, `include/mesh/ui/actions.h` | what the buttons do here, as (button, verb) pairs the action bar iterates |
| The pad | `src/ui/input.c`, `src/ui/input_profile.c` | `input.c` is the evdev reader - which nodes are worth watching, key repeat, the quit keys, and the codes a *convention* decides (keyboard keys, shoulders, START/SELECT, the hat). `input_profile.c` is what the **case** decides: a row per device holding both which evdev code each printed face button reports *and* what is printed on it, because a port that corrected one without the other leaves the action bar naming a key that does something else. `MESHCLIENT_INPUT_PROFILE` picks one |
| Status verbs | `src/ui/status.c`, `include/mesh/ui/status.h` | which Status card carries which verb, in the order the cards draw — read by `nav.c`, `actions.c` and the renderer alike, and walked by a cursor that names a *verb* (`nav->status_verb`) rather than a position |
| Help | `src/ui/help.c`, `include/mesh/ui/help.h` | what the client can explain about where the user is standing, as a title, a subject and a list of paragraphs — ids the whole way down. A settings section's notes live on the things they describe (a section's beside its icon in `settings.c`, a field's in its own `k_fields` row) and this assembles them; a *feature's* are a table here, keyed on the route under the help screen |
| The trend screens | `src/ui/backends/fb_screens.c` (`fb_render_chart`, and the two descriptions `fb_render_trend`/`fb_render_node_trend` hand it), `src/ui/status.c`, `src/ui/route.c` | The two charts, drawn by the one component that fills a body and by the one renderer that composes it, carrying no cursor. The airtime one is a level of the Status tab (`nav->trend_open`); a node's is a level of its detail (`nav->node_trend`, which holds the *reading* rather than a flag). Both are `MESH_UI_ROUTE_TREND` - the node's fills `subject` and `slot`, which is what makes one node's temperature and its humidity two places |
| Chart frames | `src/ui/trend.c`, `include/mesh/ui/trend.h` | How far back a chart looks and how far up it goes: the span the reader picked, the window it cuts, and the rung of the domain ladder the ceiling contracts to. Both chart screens ask it, which is what makes them one picture drawn twice |
| Durations | `src/ui/duration.c`, `include/mesh/ui/duration.h` | "4m ago" and "3h 20m", once. A UI file with no pixels in it, because what a ladder of unit thresholds answers with is a *string id* |
| Waypoints UI | `src/ui/waypoints.c`, `src/ui/nav_waypoints.c` | the list's order (nearest first, from our own fix), a place's detail rows, and the distance/compass formatting the same two screens read |
| Tapbacks | `src/ui/reactions.c`, `include/mesh/ui/reactions.h` | the fixed emoji set X offers over a bubble: the glyph, which goes on the air unchanged, and the catalog id that names it |
| Delivery marks | `src/ui/delivery.c`, `include/mesh/ui/delivery.h` | which mark an outbound message's ack state gets — the clock, the double tick or the alert circle a bubble's corner draws, and the word a backend with no sprites says for the same state |
| Client-level chrome | `src/ui/chrome.c`, `include/mesh/ui/chrome.h` | what the frame says about the *client* rather than about a screen: whether anything is in flight (the progress bar) and which persistent banner it carries |
| Animation | `src/ui/anim.c`, `src/ui/controller.c` | fixed-point easing + a table keyed per control; the repaint timerfd that feeds it |
| Screen transitions | `src/ui/route.c`, `include/mesh/ui/route.h` | where the nav *is*, as a comparable place - so which way a move went is derived rather than recorded. The backend slides the body from it (`fb_transition_offset`, `fb_shift_begin`) |
| Icons | `src/ui/icon.c`, `src/ui/icon_glyphs.c`, `include/mesh/ui/icons.def` | monochrome Material Symbols, tinted by the theme, in the row slots |
| Themes | `src/ui/theme.c`, `src/ui/font.c` | palette by role, surface tiers, the shape scale, metrics, font registry, and the two palettes that are not roles - the hashed avatar tints and the positional `series` colours a chart divides a whole with; `MESHCLIENT_THEME` or Settings > About picks one |
| Fonts | `src/ui/font_ui.c` + generated `font_ui_glyphs.c`, `src/ui/font5x7.c` | a glyph is **coverage**, resampled from the font's master into the cell; `ui` (JetBrains Mono) is the default, `5x7` is the pixel one |
| Text | `src/utils/text.c`, `src/ui/{font5x7,emoji}.c` | UTF-8 sanitising, cell-based measurement |
| Strings | `src/i18n/strings.c`, `include/mesh/i18n/catalog.def` | the string catalog and the locale registry |
| Dev tools | `devtools/`, `scripts/{ui-capture.sh,frames.py}` | off-screen UI capture; PNG/GIF encoding, stdlib only |
| Geography | `src/geo/` | `mesh_geo_coords_valid()` — the bounds test every coordinate ingress asks, so the air, the cache and the keyboard cannot disagree about where Earth ends — `mesh_geo_vector_between()`, the haversine distance and initial bearing the Waypoints tab reads a range from, and `mesh_geo_mercator_forward()`, the projection the map places a marker with. **The only directory in the tree that includes `<math.h>`**, and the reason libm is linked |
| The map | `src/map/viewport.c`, `src/map/tile.c`, `src/map/source_pack.c`, `src/map/tile_image.c`, `src/map/tile_cache.c`, `src/ui/map.c`, `src/ui/nav_map.c`, `src/ui/backends/fb_map.c` | Where the map is looking (centre, integer zoom, pan, fit, metres per pixel), which tiles that box is standing on (`mesh_map_viewport_tiles()`), where a tile's bytes come from (`source.h`, and the single-file `MCTPACK2` pack behind it), what those bytes decode to (`mesh_map_tile_decode()`, the one file that includes Wuffs), which decoded tiles are still held (`tile_cache.c`: byte-budgeted, LRU, and holding the *holes* as well as the pixels), the markers built from the map's own roster (`handshake.map_nodes` - **not** the node list's 128) and the waypoint book, the presses, and the drawing. A tile is read, decoded, kept and **drawn**: `fb_map.c` holds the fill loop and `fb_blit_bgra()` is the blit, one tile read per frame with the graticule showing through wherever there is not one. Where a pack comes from is `fb_basemap_open_default()` (`MESHCLIENT_MAP_PACK`, else `$HOME/.meshclient/map.mctp`); `devtools/map_pack/map_pack.py synth` draws one to try it with. See [`docs/maps-roadmap.md`](docs/maps-roadmap.md). `viewport.c` deliberately has **no `<math.h>`**: everything transcendental about a map is a property of the projection, one directory down |
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
  write/read. Framing is a *stream* concern - serial and TCP, which are one wire format - and it
  is `src/proto/stream_framing.c`.
- **The Brick's face buttons do not report by position.** A is `BTN_EAST` (305), B is `BTN_SOUTH`
  (304), the button printed **Y (left) is `BTN_NORTH` (307)**, so X (top) is `BTN_WEST` (308).
  Pinned in `input_brick_face_buttons`. This is the `brick` row of `src/ui/input_profile.c` and
  **not a default the rest of the client may assume**: the `xbox` row is the ordinary Linux
  convention, where A is `BTN_SOUTH` - the same code the Brick calls B. The two disagree about
  exactly the buttons that confirm and go back, which is why the codes and the keycaps live in
  one row: correcting one without the other is invisible, because the binding still works and
  the action bar goes on promising the first. The pad impersonates an Xbox 360 controller, and the rest
  of the case follows from that: **L2/R2 are the analog triggers `ABS_Z`/`ABS_RZ`**, not buttons
  (there is no `BTN_TL2`/`BTN_TR2` in the bitmap at all), and **F1/F2 are the stick clicks**
  `BTN_THUMBL`/`BTN_THUMBR` - a 360 pad has two sticks and the Brick has none, so those were the
  free codes. The pad also *declares* a `KEY_F1`, a `KEY_F2` and two volume keys it never sends,
  which is why the map is measured with `make deploy-input-map` rather than read off the
  capability bitmaps. The whole table is in [`docs/device.md`](docs/device.md#the-buttons-and-what-they-report).
- **The TCP link refuses a hostname, and that is the design rather than a missing feature.**
  `getaddrinfo()` blocks and this client is one epoll loop with no threads in it, so a DNS lookup
  is however many seconds of frozen UI; there is no non-blocking resolver in POSIX, and
  `getaddrinfo_a` starts threads. A target is therefore a numeric literal and `meshtastic.local`
  gets a refusal a user can read. Lifting it means the forked-child shape `fetch.c` already uses
  for HTTPS, and that belongs with the on-device way of *typing* an address, which does not exist
  either. See [`docs/transport.md`](docs/transport.md#an-address-not-a-name).
- **The connecting socket is deliberately not the stream link's, and `struct mesh_stream_link`
  must not grow a connecting state.** A link is an *established* stream: it watches for
  readability and reads. A non-blocking connect is the opposite - it reports by becoming
  **writable**, and reading it would be meaningless - so the TCP transport holds that descriptor
  itself and hands it over at the moment the connect completes. That is what keeps the link free
  of a state in which its own `pump()` must not be called.
- **The stream link never resets itself, and that is not an omission.** `pump()` and `flush()`
  report a fatal error and stop; the transport that owns the link decides what it means. "port
  closed" and "the radio closed the connection" are the same `-ENOTCONN` and two different
  sentences, and only the transport knows which one it is - the same rule that keeps a renderer
  naming a string id rather than a sentence.
- **The network arm of auto-connect has a retry stamp of its own, and sharing the general one
  would break it.** A configured host is the one candidate that can be absent without being
  *gone*: a cable is plugged in or it is not and a node is advertising or it is not, but an
  address somebody wrote down stays written down with the WiFi off. Without
  `autoconnect_tcp_retry_at_ms` that arm runs first on every turn, fails five seconds later on
  its own connect deadline, and Bluetooth is never reached at all.
- **A network link's Devices row is synthesised rather than discovered, and its `in_range` is
  false while it is connected.** `mesh_app_publish_ui_state()` has always built a row for
  "connected, but in nobody's list"; for BLE and USB that is a connect which beat its own
  discovery and the real row replaces it a moment later, and for TCP there is no discovery to
  catch up, so it is the only row that link will ever have. It must state
  `MESH_UI_DEVICE_TCP`: the slot is `memset` to zero, `MESH_UI_DEVICE_BLE` is `0`, and a
  renderer asks that one field three separate questions - which disc, whether the trailing edge
  is an RSSI, and whether `Y` may forget it. Unstated, the link drew a Bluetooth disc, reported
  `0dBm` - the absent-reading-as-a-number this client refuses everywhere else, and the
  *strongest* signal on the screen - and offered to forget a bond that was never made. `name` is
  left empty for the same reason: `MESH_STR_DEVICES_CONNECTED_NAME` is a placeholder held until
  an advertisement arrives, and a host has none coming, so it would have stood permanently where
  the address goes. And `in_range` is false because the Status card counts that field and means
  *earshot* by it - a host answers from anywhere, which is evidence of no distance at all, so the
  renderer's TCP arm sits ahead of the in-range one rather than letting it say "not in range".
- **The heartbeat is the TCP link's and not the session's tick.** `ToRadio.heartbeat` is what
  stops a radio dropping a client that has had nothing to say, and a quiet mesh is the ordinary
  case - but a BLE link needs none of it, since the GATT connection is its own liveness. So it is
  something a transport asks for on its own schedule (`mesh_session_send_heartbeat()`) rather
  than something `mesh_session_tick()` does to every link.
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
- **A direction on the map is one step, and landing on a marker changes where that step ends
  rather than how long it is.** It looks like snapping bolted onto a pan and it is the other way
  round: a pan of a fixed number of pixels leaves the crosshair on a lattice (a fifth of the
  body across, a fifth down) and the crosshair captures a disc of
  `MESH_UI_MAP_SELECT_RADIUS_PX`, so a sixth of the plane was selectable and five markers in six
  could not be aimed at *at all* at a given zoom - which on the device reads as the crosshair
  skipping over nodes, and as zooming sometimes fixing it, because a zoom re-phases the lattice.
  `mesh_ui_map_step()` takes the nearest marker in the 45-degree quadrant around the press (the
  four tile the plane, so nothing is in a direction no press names) and the view centres on its
  own coordinates, which is what makes the landing exact. The fallback pan is not a leftover: it
  is what crosses open grid, and what walks a marker beyond the step's reach into it. The
  selection is still *derived* from the centre - the nav grew no selection field, and must not.
  **The reach is one pan step, per axis, and is the same number the fallback pans by**
  (`MESH_UI_MAP_PAN_STEP_X/Y`, declared once in `map.h` so the two cannot drift apart). A reach
  of the whole declared panel is what this replaced, and it was the first correction overshot:
  on a mesh with a few dozen positioned nodes every tap had a marker somewhere in its quadrant
  to answer with, so the view cycled through the roster and the ground *between* two nodes was
  unreachable - the same complaint as the lattice, from the other side. Bounding the cross axis
  matters as much as the along one, or a press that said "north" answers with a sideways lurch
  onto something mostly east.
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
- **The tile pack is a format of the client's own, and that is a measurement rather than a
  preference.** Reading MBTiles or PMTiles directly is the obvious thing and it lost on this
  hardware: on the Brick's FAT32 card with 32 KiB clusters mounted `sync`, a single file with a
  sorted index reached a cold tile in 0.80 ms where MBTiles took 4.6 ms and a `z/x/y` tree took
  4.6 ms with a 40 ms tail - and SQLite would have cost 718 KB on a 2.88 MB binary. So the
  conversion is a host step (`devtools/map_pack`) and the device reads one file. The pack's
  **zoom range and coverage are derived from its index, never read out of its header**, which is
  the app bar's back arrow one layer down: a header field saying what a file contains is a field
  that can be wrong, and the way it goes wrong is a builder that names a city and packs a suburb.
  Everything else a read would have to trust is checked **once, at open** - extents inside the
  file, tiles inside the world, and the index's own sort order, because a `bsearch` over an
  unsorted index does not fail, it misses. See [`docs/maps-roadmap.md`](docs/maps-roadmap.md).
- **The basemap is drawn *over* the graticule rather than instead of it, and a tile still coming
  looks exactly like a tile that is not there.** A tile is opaque, so drawing the grid first
  means it survives in precisely the places there is no tile - a pack's edge, its holes, and the
  second before one arrives - and nothing has to decide whether to draw a grid. Telling MISS from
  ABSENT *in ink* was tried in the design and is worse than either: a placeholder square covers
  the markers for the two thirds of a second a view takes to fill, which is the whole of the time
  anybody is looking at it. The two states differ in what the client **does** - one asks for
  another frame, the other stops asking - which is where the distinction pays for itself.
- **One tile is read per frame, and a tile the pack does not hold costs no read at all.** A cold
  tile is 2-5 ms on the Brick's card and a view stands on twenty of them, so a frame that filled
  the panel would be a tenth of a second with the BLE link unread. `mesh_map_source_has()` answers
  out of the index in RAM, so an absence is learned for the price of a `bsearch` and the frame's
  one read always goes to a tile that will arrive. What asks for the next frame is
  `fb_basemap_pending()` through `fb_state_animating()`: a frame is otherwise a function of the
  snapshot, and no press and no packet says a tile is on its way.
- **A marker's name is drawn five times.** A glyph carries coverage, not a mask, so text is
  blended against a colour the caller says it has just filled - which is a guess the moment a name
  stands on a street. The four extra runs are the halo in the ground colour, and they are drawn
  only where a tile is; the scale bar and the pack's attribution take a chip instead, because they
  are chrome pinned to a corner rather than things on the map.
- **The tile cache remembers which tiles are *not* in the pack, and that table is not an
  optimisation.** The fill loop gets one read per turn of the event loop, because a cold tile is
  2-5 ms on the Brick's card. Every pack is a rectangle of the world with holes in it, so
  without a record of what has already been asked for and refused, one hole on screen spends
  that single read every turn forever and a view with any sea in it never finishes filling the
  tiles *around* it. The holes live in a table of their own because a hole costs twelve bytes
  and a tile costs 256 KiB, and sharing the slots would let a sparse view evict the picture to
  remember the sea. What makes the pair safe is that **a key is in at most one of the two
  tables** - otherwise the cache answers `READY` or `ABSENT` for one tile depending on which it
  looks at first. It is also why a lookup answers with a *state* rather than a pointer that may
  be NULL: `MISS` is a tile still on its way and `ABSENT` is the final picture, and they are
  drawn differently.
- **A cached tile is in the decoder's pixel format, not the panel's, and `src/map/` may not
  learn which the panel is.** `struct fb_state` reads `bytes_per_pixel` off the kernel and it is
  not always 4, so a cache holding panel-format pixels would be the map layer including the
  framebuffer - and would make a cached tile the property of *one* backend, since the capture
  harness renders at four bytes a pixel whatever the device is doing. The blit converts per
  drawn pixel, joining the switch `fb_fill_packed()` already makes.
- **Opening a different tile pack must clear the cache.** A key is three numbers about the world,
  not about a file: two packs of the same city both hold a tile at (14, 8192, 5461) and they are
  different pictures. Carried across a swap, the map draws the old pack's streets under the new
  pack's attribution, and every pixel of it is a real tile in the right place - so there is
  nothing on the frame that looks wrong.
- **The tile decoder's memory is static, constant, and sized for a colour type no pack
  contains.** A decode on an event loop must not pause to find memory or fail for want of it, so
  `src/map/tile_image.c` holds two fixed blocks - 48 KiB for Wuffs' decoder and 256 KiB of
  scratch - and allocates nothing per tile. Both numbers are **asked for and checked rather than
  known**: Wuffs' decoder struct is opaque in C and upstream says its size is not stable across
  versions, and the scratch is `width * bytes_per_pixel * height + width`, which scales with the
  *file's colour type* rather than with the tile size. That second one is why the buffer is
  sized for 8-bit RGBA (262,400 bytes) when a pack builder quantises to palette (65,792): a
  style with transparency is an ordinary thing to publish, and a buffer sized from the palette
  case refuses every 24-bit tile with the constant looking perfectly reasonable. Sixteen bits a
  channel needs twice again and is refused with `-ENOTSUP` on purpose.
- **The pak is built without `--gc-sections`, so a third-party module ships whole.**
  `scripts/cross-build.sh` uses plain `-Os` with no `-ffunction-sections`, which means nothing
  is dropped *within* an object file and any code compiled into one is code that ships. It is
  why `src/map/wuffs_png.h` names Wuffs' BASE **sub-modules** rather than BASE (31 KB, where a
  build that collected sections would have saved 128 bytes), and why the decoder costs 335 KB
  where the spike's probe - which did collect sections - predicted 106 KB. Read a size
  measurement's build flags before believing it about this binary. See
  [`docs/maps-roadmap.md`](docs/maps-roadmap.md).
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
- **START on the conversation list is not A, and it is the second screen to spend it.** START
  stands in for A everywhere else; here it mutes the row under the cursor. It is spent for the
  map's reason - there is no other key left. A opens, Y writes, X deletes, SELECT explains and
  the shoulders walk the tabs, which is every button but B, and B means "back" on every screen
  in the client. The action bar names the press, and it names it for the row rather than for the
  key: "unmute" on a muted conversation, and nothing at all on the two rows that are not
  conversations, because a keycap that does nothing is what that table exists to prevent.
- **A mute rides `struct mesh_ui_read_mark`, and the eviction there prefers an unmuted slot.**
  A mute and a read mark share a key and a lifetime - both are what the client remembers about
  one conversation - so a table of its own would be a second array keyed on the same three
  fields with a second eviction rule to keep in step. What is not shared is how much losing one
  costs: a read mark is bookkeeping the client rebuilds by being read again, and a mute is a
  choice, so a mute that vanished because thirty-two other conversations were opened would be a
  setting silently undoing itself. The saved line grew a fifth field rather than a new one, and
  the loader takes four or five - and it keeps a mark carrying *either* half, because a
  conversation muted before it was ever read has no packet id to be saved under.
- **"Muted" has two inputs and one predicate.** `mesh_ui_store_conversation_muted()` reads this
  client's own flag *and* upstream's per-node `is_muted`, whose whole definition is that the node
  "will not trigger a notification" - a radio told to stop announcing a node and a Brick that
  announced it anyway are two answers to one question. Only the local half is what START
  toggles, and only a direct conversation has the other: the NodeDB has nothing to say about a
  channel. One predicate rather than a field, so the tab badge, the snackbar and the row's own
  bell cannot disagree.
- **A muted conversation still counts its own unread, and is missing only from the total.** The
  row goes on saying how much has piled up, one family quieter: muting is asking not to be
  interrupted, not asking to be kept in the dark. What a mute takes away is
  `mesh_ui_nav_unread_total()`, which is what the Messages tab's badge and the all-traffic row
  read - and that is also what keeps the badge worth looking at, since one busy channel outruns
  everything else on a mesh and a permanently badged tab says as much as an unbadged one.
- **Only the Messages tab carries a badge, and that is a rule rather than a start.** A badge has
  to be **clearable by going there**, exactly as a banner has to be able to resolve. Unread
  messages are, because opening the conversation marks them read; a count of nodes or of
  waypoints would be a number that never went down however often it was looked at.
- **The transcript's unread line is captured by the press that opened the thread, not derived
  from the read mark.** `mesh_ui_store_mark_open_conversation_read()` runs from
  `consume_updates`, so by the time a frame is built the mark already says "all of it" and a
  line drawn against it would sit under the newest bubble every time. `nav.thread_unread_from`
  is the one copy of where the reader *was*, it is the reply target's pattern, and it
  deliberately does not move while they are in there - a message arriving into an open thread
  lands below the line rather than moving it. It is part of the thread row cache's key for the
  same reason: leaving a conversation and coming straight back is the same log, the same indices
  and the same target with the line somewhere else.
- **The unread line takes the separator slot from a date when both want it.** A bubble has one
  row above it. The date is recoverable from the clock in the bubble's own trailing run, and
  "this is where you stopped" is sayable in one place only.
- **A read mark never lands on a reaction.** The unread count walks the log ignoring reactions
  and looks for the marked packet to know where "read" stops, so a mark on a tapback is a packet
  that walk can never meet: `mark_seen` stays false and every message in view goes on being
  counted. A conversation whose newest entry was a tapback stayed badged however often it was
  opened. It is also what lets the transcript find its own line, which looks for the bubble whose
  predecessor is the marked one - and a reaction never gets a bubble.
- **Unmuting is reported from what it achieved, not from what it set.** Both halves can be on at
  once - muted here, then muted on the radio from the Nodes tab or by another client - and the
  local half really does clear, while the row goes on drawing itself muted because the radio is
  still muting that node. So the toast is chosen by asking
  `mesh_ui_store_conversation_muted()` again *after* the write. "Unmuted" on a row that is still
  muted is the one thing worse than a press that does nothing, which is a press that lies.
- **A notification cursor that has gone missing re-places itself in silence.** When the packet
  `ui_message_announced_id` names is no longer in the log, where we had got to is unknowable, so
  the reporter takes its place again from the newest and announces none of it - exactly as a
  launch does. The reachable way in is a *delete*, not the ring: the log holds 64 and the
  reporter runs on every publish, so an eviction would need 64 messages between two turns of the
  loop. Read as "everything since is new", deleting a conversation announced whatever inbound
  message happened to be last - somebody else's, already announced, arriving a second after the
  user pressed delete.
- **An unmute that empties a mark takes the mark with it.** A conversation muted before it was
  ever opened has `packet_id` 0, so clearing the mute leaves a record holding no read position
  and no mute - and one whose stamp has just been refreshed, which under the eviction above is
  the *last* unmuted mark to go. A genuine read position would be thrown away ahead of it, which
  on the device reads as a conversation you had read coming back unread.
- **A press replaces the snackbar and an arrival queues behind it, and that is two functions on
  purpose.** `mesh_ui_nav_set_toast()` supersedes what is showing because it is the client
  answering the button just pressed, and what it replaces is usually the earlier half of the same
  story - "Connecting to NodeSeven" giving way to "NodeSeven needs pairing" is one sentence
  finishing, and four seconds of the optimistic half before the true one is worse than losing it.
  `mesh_ui_nav_post_toast()` is for news the user did not ask for: it has nothing to supersede,
  and overwriting the answer to a press with it is how a button comes to look as though it did
  nothing. A full queue drops its *oldest waiting* entry - a backlog is only worth keeping while
  it is still news - and a repeat of what is already up is dropped, because two identical notices
  in a row are one notice standing for eight seconds.
- **A direct message announces itself and a broadcast never does.** It is the alert/detection
  line one step further out: a channel is a room full of people talking, and a notice per line
  would make the client unusable on any real mesh. Every unseen direct message is announced
  rather than only the newest, which is where this parts company with the alerts - three alerts
  arriving together are one situation and the last describes it, while three messages from three
  people are three things somebody said to you. That is what the queue is for. A launch announces
  none of it: the log is seeded from the cache before the first publish, so the first pass adopts
  what it finds silently - and the priming is taken *before* the empty-log guard, or a client
  that starts with no cache spends it on the first message that genuinely arrived.
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
- **A card in a scrolling list is wider than the rows standing in it, and its ends are square
  wherever the window cut it.** Both look like off-by-ones and neither is. A card has to contain
  the widest thing in it, and in a list that is the *cursor's highlight*: drawn to the same
  rectangle - both are measured from the row gutter - the highlight lands exactly on the hairline
  and paints it out for the length of one row, so the card loses its sides on precisely the row
  being read and nowhere else. The edge is therefore spent outward, past the box
  `fb_row_box()` states, and the highlight fills the card's interior, which is where Material
  puts a state layer inside a container - which is also what makes the card, rather than the
  row, the widest thing a list draws and so what the scroll rail clears. The square end is the same rule about honesty
  one level up: a rounded corner halfway down a scroll is a card claiming to *end* where the panel
  merely stopped, and a reader cannot tell that from a card that really did. The cut end keeps its
  inset along with its corners, or the hairline runs across the cut and says it again in a
  straight line. `fb_fill_round_rect_ends()` draws it, and
  `ui_capture_node_detail_cards_survive_the_cursor` is what catches the first of the two - it is
  invisible in a still of a resting screen and invisible in a count of how much card fill is on
  the panel. It also takes the *highlight's* corner radius rather than `fb_draw_card()`'s, which
  is the width rule on the other axis: a card still curving where the highlight has reached full
  width lets the cursor's ends stand outside it on the first and last row of every group.
- **A list's rows, its cards and its scroll rail are one rectangle asked for once, and the rail
  has a gutter of its own that nothing else may enter.** `fb_row_box()` states where a list's
  rows stand - the fill the cursor highlights, and the span its words are drawn in - and
  `fb_rail_gutter()` is the strip kept clear beside it. Both are corrections of the same
  arithmetic. The rectangle was derived three times (the highlight in `fb_draw_row_fill_on()`,
  the list item's own copy, and the card surfaces under a grouped list) with a fourth opinion in
  `fb_list_rail()` about the room left over, and the four agreed right up until the cards began
  spending their hairline outward: the card's edge then ended on one pixel and the rail's track
  began on the next, so on the node detail - the one screen that is a column of cards - the rail
  read as part of the card rather than as a control beside it. The gutter is reserved on **every
  list and whether or not the rail draws**, which is the other half: taken only when a list
  outgrows its window, it would be a layout that reflows the moment a node reports one more
  reading. It costs no text column at the device's scale, because the strip is narrower than a
  cell. `ui_capture_node_detail_cards_survive_the_cursor` measures the gap.
- **A group's heading stands between two cards rather than inside either, and a card in a list is
  padded at the bottom only.** Both are where the column's air comes from, and there is very
  little of it: a card here is painted round row boxes that were laid out for a flat list, so the
  only room to pad it with is whatever a heading's step is not using - a line advance less a
  label's, nine pixels at the device's scale. Split three ways between a card's bottom, the break
  and the next card's top, none of the three was big enough to see: two cards read as one box with
  a rule across it, and the last row of each sat on its own edge. So the heading takes
  `FB_LIST_NO_CARD` and stands in the break - centred in that step, because a label sat on the
  bottom of it names the card that ended rather than the one it opens - and the two edges spend
  the rest. The bottom takes the inset because a row's box carries its line's leading at the *top*
  while its descenders run to the bottom edge, so padding both ends alike leaves a card
  top-heavy by exactly that leading; the top takes the hairline instead, spent outward for the
  reason the sides spend it. The grouping still costs no rows, which is what keeps the nav and
  every count in the `ui_nav_nodes` suite out of it.
- **A card's rows are drawn against the card, and a control on one takes the row's *resting*
  ground rather than its current one.** The first is a glyph carrying coverage rather than a mask:
  text told the wrong ground keeps its shape and gains a halo, so `fb_draw_row_fill_on()` takes
  the ground and `fb_list_ground()` answers. The second is the opposite-looking rule and it is not
  an inconsistency. A switch's ring and a meter's track bed are laid *to escape* the cursor fill -
  both controls are contracted against what the row rests on, and on two of the four themes that
  fill is the resting track's own colour - so handing them the current ground makes the control
  vanish on the row being pointed at, which is the bug they were added to prevent. Words blend
  against the fill; a patch under a control replaces it. `fb_draw_trailing()` derives the first
  from the second so the two cannot be passed the wrong way round.
- **A group heading is a row of the list and is not a row the cursor may stand on.** The node
  detail and an open settings section both draw `fb_list_subheader()`, and the cursor used to
  land on one: a full-width highlight under a dimmed word, with A doing nothing and the action
  bar still promising "select" - the keycap-that-does-nothing `actions.c` exists to prevent, and
  the first thing a reader met once the node detail's verbs got a heading of their own.
  `mesh_ui_nav_skip_headings()` steps over them, and `mesh_ui_nav_cursor_to_first_row()` is what
  the four places that open a level write instead of 0. Both **ask the built rows** rather than
  reasoning about where a heading falls, because which rows exist depends on what the node has
  reported and what the radio has sent - the same reason every other question about these two
  screens is answered by building. Two consequences worth knowing before "fixing" a count: a
  press is no longer the same number as a row (a walk to row *n* costs *n* presses minus the
  headings passed), which is why the capture scenes' `key down` counts are checked against a
  render, and UP off the first real row stays put rather than parking on the title above it.
- **The Status cursor is a verb, not a position, and `cursor[MESH_UI_SCREEN_STATUS]` is unused.**
  Status is the one screen with no rows: its cards offer verbs and Up/Down walk those, so what
  the nav holds is `status_verb` (`enum mesh_ui_status_verb`) and the row cursor stays at 0.
  Reading that array entry is reading a position nothing maintains. It was an index once, and
  the index is what forced two rules that are now gone: the verb list had to be append-only, and
  every verb had to restate the conditions of the verbs before it. `mesh_ui_status_verb_resolve()`
  answers where a cursor whose verb has gone stands, and the verb it is holding is deliberately
  *kept* while the screen offers none - a link that drops and comes back lands the reader on
  their own button rather than at the top.
- **A chart carries one vertical, so a node's temperature and its humidity are two screens.**
  `struct fb_chart` has a single `scale` and a single pair of axis labels, and every line in it is
  projected against that one domain - which is exactly why the airtime chart can draw two lines:
  channel utilisation and transmit share are both permille of the same air. Degrees Celsius and
  relative humidity are not, so a single plot of the pair would have labelled an axis only one of
  them was measured against, which is the auto-scaling lie one step worse - an axis with numbers
  on it gets believed. So the press on a node's row names a *reading*, `nav->node_trend` holds it,
  and the route carries it in `slot`. Drawing them together is the fix that looks obvious and is
  the bug.
- **Both chart screens are one renderer, and a third caller adds a description rather than a
  function.** `fb_render_chart()` takes a `struct fb_chart_screen` - the series, their labels, the
  domain, the band, and what unit the axis is *worded* in - and does everything else: the span,
  the window, the ceiling, the projection, the caption, the picker. The airtime chart and a
  node's were forty duplicated lines apart, which is forty lines in which two screens meant to be
  one picture can quietly stop being one. `actions_trend()` is shared for the same reason.
- **A chart's span picker only narrows, and it is anchored at the newest reading rather than at
  the clock.** A span wider than the readings leaves the window at the readings' own ends, so the
  caption under the axis names what was drawn rather than what was asked for. Anchored at "now",
  a link that dropped twenty minutes ago would answer every span but All with an empty plot -
  which says nothing about a radio that was reporting perfectly well until it went away. The
  window is also cut **before** the ceiling is picked (`mesh_ui_trend_frame()` does both, in that
  order, which is why it is one function): the other way round, narrowing to the last quarter
  hour leaves the axis held open by a busy spell that is no longer on the panel.
- **A narrowed window drops the readings outside it rather than clamping them.**
  `mesh_ui_series_project_over()` holds an outside sample at the edge it fell off, which is right
  for a window taken from the series themselves - nothing is ever outside one. Over a window the
  *reader* narrowed it draws every older reading at one x: a vertical stroke up the side of the
  plot, in the data's own colour, that is not a reading of anything.
  `mesh_ui_series_project_within()` is the one a chart asks for.
- **`nav->trend_span` is one field for both charts, and that is a claim about what it is.** Every
  other level flag on the nav says where a tab is standing, one per tab, so each tab keeps its
  own place. A span is not a place - it is how the reader likes their charts read, which is the
  theme's kind of setting - so narrowing the airtime chart and opening a node's temperature finds
  the same span picked. `mesh_ui_nav_init()` sets it to `MESH_UI_TREND_SPAN_ALL` rather than
  leaving the zero, which is the narrowest: All is what the screen did before there was a picker.
- **A node chart takes its whole statement off the row it was opened from, and must not switch on
  the reading itself.** `fb_render_node_trend()` rebuilds the detail's rows and finds the one
  whose `trend_reading` matches the nav, then reads the series, the domain, the band and the words
  out of it. Those four have to agree, and they agree by being one row: a renderer that looked up
  the series by reading and the scale by a switch of its own would be the node detail's opinion
  about what a temperature is measured between and the chart's, and the first thing it would get
  wrong is the day one of them changed. Same rule as `status.c`'s verb table, one tab over.
- **The temperature and humidity bands are about the node, not about the weather.** Nothing in
  this client knows whether 35 degrees of air is pleasant, and a band coloured for *that* would be
  an opinion it has no business having. `MESH_UI_TEMPERATURE_WARM` and `MESH_UI_HUMIDITY_DAMP` are
  where a sealed box on a pole starts derating and where condensation starts forming on the board
  inside it - a question about the radio that reported the reading, which is the only one the
  client can answer and the one a solar repeater in a field is opened for.
- **A node's air is pushed into the history on its own test, not on the battery's.** DeviceMetrics
  and EnvironmentMetrics are two Telemetry variants arriving in two packets on two schedules, so
  `mesh_ui_store_note_roster()` compares `environment` separately from `metrics`. Keyed on the
  battery's struct, a sensor reading would be sampled once per battery report and dropped whenever
  the battery held still - a temperature series on the wrong clock. And an EnvironmentMetrics
  carrying neither reading takes no slot, or a barometer would evict the node somebody is watching.
- **The chart swallows the d-pad and A, and does not take the shoulders.** It is the map's split
  in reverse. The map takes the four directions because Left there means "look west"; the chart
  takes Up and Down because there is nothing on it to move - and what a press would otherwise
  fall through to is the Status cards, where Down moves a cursor nobody can see and A runs
  whichever verb it lands on. **Left and Right it takes because there *is* something to move**:
  the span picker over the plot is the only control on the screen, and until it existed those two
  presses fell through to the tabs. The shoulders stay the tab switch they are everywhere, which
  is what pays for the d-pad here exactly as it does on the map, and `trend_open` outliving a
  change of tab is why the key handler checks `nav->screen` as well.
- **Two lines on one chart are projected over a window neither of them owns.**
  `mesh_ui_series_project()` stretches a series across its own span, which is right for a line
  drawn alone and wrong beside a second one: a series that stopped reporting is drawn as though
  it were still arriving. `mesh_ui_series_window()` is the union of the clocks and
  `mesh_ui_series_project_over()` places every line on it.
- **A chart's line is drawn thicker than a sparkline's on purpose.** A series colour promises
  1.4:1 and that was measured on the width of a bar - the palette is a fill's contract, never an
  ink's - so a hairline in one of those colours is a line the reader has to hunt for. The room a
  chart has is spent making the mark wide enough to be the fill the palette was validated for.
- **The framebuffer needs all three steps** — draw page 0, `FBIOPAN_DISPLAY`, mirror into page 1
  — or the screen is black.
- **`deploy-start` kills NextUI's launcher with `SIGKILL`, and `TERM` there powers the Brick
  off.** SDL turns `TERM`/`INT` into a quit event, `nextui.elf` answers it with `PWR_powerOff()`,
  and `PLAT_powerOff()` deletes `/tmp/nextui_exec` and touches `/tmp/poweroff` - so the launch
  loop runs the pending pak and shuts the device down when it exits. Measured twice, by accident.
  Do not "gentle" the kill, and never `kill $(pidof nextui.elf)` in a device shell.
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
- **The Status verb list is written in the order the cards draw, and that is the whole of its
  ordering rule.** Link, then Mesh, then Radio - so Down walks down the screen. It was ordered by
  when each verb was added, because the cursor was an index and a verb arriving anywhere but the
  end changed what A did; the trend therefore sat after the Radio card's refresh while its card
  is the middle one. A verb added to `k_status_verbs[]` goes where its card is, and states its
  own condition and nothing else's - which is why the trend needs neither the link nor the
  handshake, only a line to draw.
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
- **The settings edit buffer's width is a `sizeof`, not a number, and raising it by hand is
  what that replaced.** `MESH_UI_SETTING_TEXT_MAX` is the size of a union of every TEXT and KEY
  field's bytes plus its NUL (`include/mesh/ui/settings_text.def`, read once in `nav.h` for the
  width and once in `settings_internal.h` for the per-field limits a `k_fields` row names), so
  the buffer *is* the widest field and a wider field widens it by being listed. It was a
  constant raised twice, once per module that outgrew it, and the failure when it was too small
  was silent: `mesh_ui_nav_settings_commit_text()` cuts what does not fit, so a radio that would
  have taken the whole string was sent part of one with nothing on the frame saying so. A field
  written with a bare limit the def does not know about fails
  `settings_text_fields_fit_the_edit_buffer` rather than being truncated at the keyboard.
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
- **A column of cards reserves room for its last card, and the reservation yields rather than
  erasing the card making it.** Cards are drawn top down and each takes what it wants, so the
  last one pays for everything above it - and `fb_draw_card()` pays by refusing the card
  outright rather than by clipping it. That is worse than losing rows, because a card carries
  *verbs*: `mesh_ui_status_actions()` offers `refresh` from the link state alone and has no idea
  what was drawn, so the cursor walked onto a button that was not on the frame. It is the rule
  below reached from the layout side instead of the row-count side, and it was already happening
  on `main` - the airtime block cost four rows, two of them a trend line arriving on the second
  LocalStats report a few minutes after connecting. `fb_draw_card_reserving()` is the fix; the
  Status screen is the one caller, which is why its Radio card is built into a local of its own
  and drawn after the Mesh card that reserved for it. A reservation that cannot be afforded is
  dropped, because two cards missing is not an improvement on one. **How much to reserve is a
  reading rather than a constant**: `fb_card_min_height()` promises the card exists and
  `fb_card_height()` promises it can say everything, and the Status screen picks between them on
  `radio_tone` - the same reading that picks that card's variant. A card whose every row appears
  only when something is broken is the wrong card to hand a minimum, and under one the radio's
  own account of why nothing worked was clipped while the Mesh card kept its message ring.
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
- **The Status card's counters are split by direction, and only the received side gets a bar.**
  Sent is tx, relayed and dropped; Heard is new, dupe and bad with a divided bar under it. They
  look like the same kind of row and they are not. `num_packets_rx` is documented as everything
  received, good and bad, with the duplicates among it - so those three are a *partition* of it
  and a divided bar is a true picture. `num_tx_relay` is a **subset** of `num_packets_tx` rather
  than a sibling of it, so the Sent row's three numbers add up to a whole that does not exist,
  and a bar there would be `fb_draw_proportion()`'s one way of being wrong quietly: overlapping
  parts still sum to something, and the picture drawn from them is confident. The partition is
  checked at the call site as well as reasoned about - two counters off the air have no promise
  of agreeing with a third, and a remainder that comes out negative skips the row rather than
  clamping to zero, which would draw a bar claiming every packet the radio heard was malformed.
  The received *total* is deliberately not a fourth number on the Heard row: it is the sum of the
  three and the length of the bar, and the row that used to state it also restated two of the
  three parts under a heading that read as a fault.
- **Both counter rows take their tone from a share, never from a count.** These are lifetime
  totals since the radio booted, so a colour read off an absolute lights once and then stays lit
  for the rest of the connection - twelve malformed packets in six thousand is what the row this
  replaced spent its warning on. A ratio recovers as the radio runs well. The live half is
  elsewhere on purpose: the Radio card's TX queue row goes to the error family the moment the
  radio is refusing sends *now*, which is the alarm, where these are the tally.
- **Two rows of the Link card stand down while a radio is attached, and it is not a missing
  else.** `fb_link_summary()` builds the line under the keycaps out of `transport_status` and
  `fb_device_label()` of the connected device, on every frame of every screen - so with a link up
  it reads "running: Home Base" and the card's Transport and Radio rows were the same two
  expressions a dozen rows further up, at the top of the one column here that runs out of room.
  With no radio that line says the quit hint instead, so the rows come back and the card is where
  "not connected" is written. It is the banner's rule on a card row: whether anything else is
  saying it is a question about the state, not about the row.
- **A series colour is not a tone, and the avatar tints are not a series palette.** Both are
  tables of colours in `theme.c` and they answer different questions. A tone means good, bad or
  caution; a series colour means *which part*, and nothing else. An avatar tint is picked by a
  hash so it only owes variety, and a theme may state fewer of them; a series colour is picked by
  position, so slice 0 is the same colour on every frame and every theme states all four. The
  contract is luminance rather than hue - 1.4:1 against the grounds *and against each other* -
  which is why the colour-blind theme spends four of Okabe-Ito's eight rather than any four: its
  sky blue and its orange are 1.02:1 apart in lightness, so as adjacent slices they are one slice.
- **A trend's axes are not its data, and the one exception is stated on the axis.** A sparkline's
  x is *time* and its y is the reading's own `struct mesh_ui_scale` - the same one the bar beside
  it fills against - never the range the samples happen to span. What a *chart* may do, and a
  sparkline may not, is contract that domain's **ceiling** to a rung of a fixed ladder
  (`mesh_ui_trend_domain()`: a hundredth, a fiftieth, a twentieth, a tenth, a quarter, a half,
  all of it) so that a mesh at 1.1% busy is a shape rather than a flat line along the bottom of
  an empty rectangle. Three things make that not auto-scaling: the **floor never moves**, so a
  fall of two percent is two percent of something; the rungs are fractions of the *domain* rather
  than of the data, so two visits inside one rung are comparable; and the ceiling is the axis's
  own top label, so a contracted plot says so. A sparkline is excluded because it shares its
  domain with the bar beside it and has nowhere to write down that it has moved. A threshold
  above the contracted ceiling is not drawn at all - and that test is made against the *reading*,
  because `mesh_ui_scale_permille()` clamps and a test made against the projection can never
  fire. Every spreadsheet does the opposite, and on the two readings this draws
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
| [`docs/steamdeck.md`](docs/steamdeck.md) | building and running on a Steam Deck: the distrobox toolchain, and what a UI on its panel would take |
| [`docs/settings-roadmap.md`](docs/settings-roadmap.md) | radio settings phases and admin verbs |
| [`docs/radio-firmware-roadmap.md`](docs/radio-firmware-roadmap.md) | updating the *radio's* firmware - UF2 over USB, OTA over BLE - what is reachable and in what order. Phases 0-4 have shipped and are confirmed on hardware, so both buses work from the CLI; phase 5's press is built - Settings > About radio installs firmware, with a confirm sheet per bus and a banner for a radio left in its loader - and has **not** been run against a radio yet, and its edges (startup recovery, a battery floor, the variant picker) are still open |
| [`docs/components-roadmap.md`](docs/components-roadmap.md) | UI component set audit and the order to close its gaps |
| [`docs/semantic-release.md`](docs/semantic-release.md) | versioning, packaging, release assets |
| [`docs/testing.md`](docs/testing.md) | test categories and how to run them |
