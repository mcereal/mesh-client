# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C17 Meshtastic client for the TrimUI Brick (NextUI/MinUI, platform key `tg5040`), shipped as a
`MeshClient.pak`. Single-threaded epoll event loop, BlueZ-over-D-Bus BLE transport, nanopb for
Meshtastic protobufs. `AGENTS.md` is the contributor guide (style, tests, PR expectations) and
still applies; this file adds what's not obvious from it.

## Commands

**The core is Linux-only** (`epoll`, `timerfd`, `eventfd`). This repo is developed on macOS, so
build and test through the containers. On a Linux host (including a Claude Code on the web
session) `make setup` provisions the same prerequisites natively and the plain `make debug` /
`make test` targets work directly - no Docker needed; the `.claude/hooks/session-start.sh`
SessionStart hook runs that setup automatically for remote sessions. `make docker-*` wraps `scripts/docker.sh`, which builds the
image from `docker/Dockerfile` on first use and bind-mounts the repo at `/src`. Container builds
use `BUILD_ROOT=build/linux` so outputs land in `build/linux/{debug,release}`.

```bash
git submodule update --init --recursive   # nanopb, meshtastic/protobufs, NextUI — CMake FATAL_ERRORs without them
make docker-test                          # Debug build + ctest in the dev container (the default verify step)
make docker-debug                         # Debug build only
make docker-shell                         # bash in the dev container; run cmake/ctest/binaries by hand
make docker-pak                           # cross container: static aarch64 build → dist/MeshClient.pak.zip
make docker-image / docker-cross-image    # force image rebuild after editing docker/
make format                               # clang-format all tracked .c/.h (runs fine on the host)
make brick                                # docker-pak + push to the Brick over SSH (needs .brick.env, see docs/device.md)
make deploy / deploy-logs / deploy-check  # push only / tail device log / report BlueZ, D-Bus, fb0 state on device
make deploy-run ARGS="--list-devices"     # run launch.sh on the device headless, streaming output
```

Device deploys go through `scripts/deploy-device.sh` over SSH (dropbear "SSH Server" pak on the
Brick, busybox only: transfers are `tar | ssh tar`, no rsync/scp). Host settings live in the
gitignored `.brick.env`. The script quotes for POSIX `sh`, not bash; keep it that way.

Inside the container (or on a Linux host) the plain targets apply: `make debug`, `make test`,
`make release && make package`, `make proto`, `make run`,
`cmake --build $BUILD_ROOT/debug --target meshclient`,
`make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or UBSAN).

Single test: the whole suite is one binary with a name filter, not per-test CTest entries.

```bash
./scripts/docker.sh ./build/linux/debug/tests/meshclient_core_tests --list
./scripts/docker.sh ./build/linux/debug/tests/meshclient_core_tests --filter ble_transport
```

Manual checks: `meshclient --list-devices` (BLE advertisers and USB ports), `--status --json`,
`--status --status-output PATH`, and `--serial[=ID] --status` to drive a USB node instead of BLE.
Inside the container there is no BlueZ, so the BLE transport sits in `waiting-for-bluez` and the
CLI backend is selected; without D-Bus headers at build time it compiles out entirely
(`MESH_HAVE_DBUS` is set only if `pkg-config dbus-1` succeeds) and reports `disabled`.

## Architecture

Data flows one direction: link (transport) → `mesh_session` → `mesh_app` → UI store → controller → backend.

- `src/core/event_loop.c` — epoll loop with a fixed table of 32 fd sources. Everything (D-Bus
  watches, timerfd discovery refresh, UI store eventfd, minui-list child stdout) registers here.
  No threads anywhere; do not add them.
- `src/transport/transport_registry.c` — `struct mesh_transport_ops {start, stop, status, tick}`
  plus the optional `set_session` and `take_error`.
  BLE and serial are registered today; HTTP is planned to plug in here. Both links own a
  `struct mesh_session`, so everything past the connect is shared.
- `src/transport/ble/bluez_client.c` — raw libdbus wrapper for `org.bluez` (adapter discovery,
  `GetManagedObjects`, GATT Connect/StartNotify/Write). Has a compile-time-independent mock
  (`mesh_bluez_client_mock_enable`) that tests use to script results and capture writes; there is
  no real BlueZ in CI.
- `src/core/session.c` — the Meshtastic conversation, independent of how bytes travel
  (`struct mesh_session`): the `want_config_id` handshake, a node-summary cache decoded from
  `FromRadio` (256 entries; every inbound `MeshPacket` also refreshes its sender's
  `last_heard`/SNR/hops, adding the sender by id if the sync never delivered its NodeInfo), the
  radio's channel table (`FromRadio.channel` by slot, role DISABLED kept so indices stay
  meaningful), the message log, the radio settings and admin queue pump, and packet ids. A link
  calls `mesh_session_attach(send_fn)` when its connection is usable, hands every FromRadio
  protobuf to `mesh_session_handle_from_radio`, calls `mesh_session_tick` each turn, and
  `mesh_session_detach`es when the link drops (handshake and settings reset, messages survive).
  The session never sees GATT, ttys or framing; a link never decodes a protobuf. Today the BLE
  transport owns one session (`mesh_ble_transport_session`); the `mesh_ble_transport_*`
  send/settings/messages/handshake functions are thin wrappers over it.
- `src/transport/ble/ble_transport.c` — the BLE link: state machine (`disabled` →
  `waiting-for-bluez` → `waiting-for-adapter` → `running`), Meshtastic service UUID filtering,
  an outbound ToRadio packet queue (the session's send path), and the FromNum-notify →
  FromRadio-read drain loop. BLE is **not** Nordic UART and has no length
  framing: one bare protobuf per GATT write/read. `src/proto/framing.c` is a homegrown varint
  prefix that nothing on the wire uses; serial framing is `src/proto/stream_framing.c`.
  Nodes in PIN mode must be paired with BlueZ out of band (`bluetoothctl pair`) before connect.
  `mesh_ble_transport_connect` sends `Device1.Connect` without blocking (reply matched by
  serial in `bluez_client.c`, 30 s cap) and returns 0 with the link in `connecting`; `tick()`
  then waits for `ServicesResolved` (250 ms polls, 20 s cap) before wiring the characteristics.
  `connected_address()` is NULL until then; `status()` reads `connecting`/`connected` meanwhile.
  Everything else on the bus (reads, writes, StartNotify) is still a blocking call. BlueZ never
  tells us about a dropped link (only characteristic properties are watched), so `tick()` reads
  `Device1.Connected` every 2 s while CONNECTED (`mesh_ble_transport_check_link`) and a failed
  GATT write also resets the link; queued messages are marked FAILED either way, and the
  message log survives the reset. Auto-connect then reconnects.
- `src/proto/stream_framing.c` — Meshtastic's serial/TCP framing: `0x94 0xC3 len_hi len_lo` plus
  one raw protobuf, 512-byte cap. The parser is incremental and resync-tolerant because the
  firmware interleaves its text log with the frames on the same port; junk between frames goes to
  a text callback (logged as `radio: ...` at debug), a split frame is held until the rest lands.
- `src/transport/serial/serial_usb.c` — finding and opening a USB port, and the Brick-specific
  part. The Brick's kernel has `CONFIG_USB_ACM` off, so a native-USB node gets no `/dev/ttyACM*`;
  the transport writes `VID PID` to `/sys/bus/usb-serial/drivers/generic/new_id` (the generic
  driver refuses the control interface and takes the data one as `/dev/ttyUSB0`), then sends one
  CDC `SET_CONTROL_LINE_STATE` through usbfs because the node discards output until DTR is
  asserted and the generic driver cannot assert it. Neither survives a reboot, so both happen on
  every connect. UART-bridge boards (cp210x/ch341/ftdi_sio) skip both and take a normal
  `TIOCMBIS`. Mockable (`mesh_serial_usb_mock_enable`) so tests never touch sysfs or usbfs; the
  mock's `open_fd` lets a test hand the link one end of a socketpair. Note the `MESH_IOCTL_REQUEST`
  shim: glibc's `ioctl` takes `unsigned long`, musl's takes `int`, and the USBDEVFS codes have the
  high bit set - the release build is musl, the dev container glibc, so both need narrowing.
- `src/transport/serial/serial_transport.c` — the serial link: sysfs scan (rescanned every 3 s
  while idle, never while a port is held), bind + DTR, open the tty raw at 115200, register the fd
  with the epoll loop, send the 32-byte `0xC3` resync burst the Meshtastic clients send, wait
  100 ms, then attach the session and run the same `want_config_id` handshake as BLE. Outbound
  packets are framed into a queue with a partial-write cursor; `EPOLLOUT` is armed only while
  that queue has a remainder. EOF or a fatal read/write error resets the link and marks queued
  messages FAILED, leaving the message log intact.
- `src/core/radio_settings.c` — transport-agnostic view of the connected radio's configuration
  (`struct mesh_radio_settings`: every `Config`/`ModuleConfig` section, owner `User`,
  `DeviceMetadata`) and the `AdminMessage` plumbing: encodes `ADMIN_APP` requests addressed to
  our own node with `want_response`, decodes replies (correlated by `Data.request_id`), keeps the
  `session_passkey` every reply carries (firmware 2.5+ rejects a `set_*` without it), and runs
  a one-at-a-time request queue with a 5 s timeout. The session feeds it handshake fragments
  and admin packets, sends a metadata+owner probe once `config_complete_id` arrives, and pumps
  the queue from `mesh_session_tick()`. Admin replies never reach the message log. Writes
  (`mesh_radio_settings_queue_write`) always go out as `get_owner` (fresh passkey; the firmware
  rotates it after 150 s), the `set_*` carrying the **whole** section (the firmware replaces, it
  does not merge), then the matching `get_*`. A `set_*` is answered by a `ROUTING_APP` packet
  quoting our id: `error_reason` NONE is the ack, `ADMIN_BAD_SESSION_KEY`/`BAD_REQUEST` a
  rejection; `ingest` claims those too and counts them in `writes_acked`/`writes_failed`. The
  full channel table (`has_channel[]`/`channels[]`, keys included, never persisted) is kept
  for `set_channel`, which must carry the whole `Channel`; `get_channel_request` is index+1.
  `MESH_ADMIN_SET_TIME` is the odd one out: `mesh_session_sync_clock` pushes the Brick's own
  `time(NULL)` at the radio once per connection (`set_time_only`, behind a `get_owner` for the
  passkey, no read-back - there is no `get_time`), so a node with no GPS stops sitting at 00:00
  and its packets carry a real `rx_time`. It is gated on `MESH_RADIO_CLOCK_MIN_EPOCH` and left
  out of `mesh_admin_request_is_write` on purpose, so it never counts as a save or toasts over
  one. `set_time_only` is UTC; the node shows local time only once `DeviceConfig.tzdef` is set,
  which is the Device section's one editable row. Most
  sections reboot the radio 7 s after a set (owner, module configs, display when
  `screen_on_secs`/`flip_screen` change), so the link drops and auto-connect reconnects; that is
  expected, not a bug. Phase status is in `docs/settings-roadmap.md`.
- `src/core/message.c` — transport-agnostic messaging: builds `TEXT_MESSAGE_APP` packets into a
  `ToRadio`, folds inbound `MeshPacket`s into a fixed ring (`mesh_message_log`), and correlates
  `ROUTING_APP` replies with the outbound message they ack. Message text is untrusted radio
  input: `mesh_message_ingest` sanitises control bytes so backends can draw it directly.
- `src/core/app.c` — `mesh_app_publish_ui_state()` copies BLE discovery/handshake state into the
  UI store every loop iteration, persists the handshake cache and preferences under `$HOME`
  (`~/.meshclient/ui_prefs`, `ui_prefs.handshake`), and picks the UI backend.
  `mesh_app_autoconnect()` runs every foreground turn: preferred node if in range, else the
  strongest advertiser after 30 s, exponential backoff on failure; `MESHCLIENT_AUTOCONNECT=0`
  turns it off. A BLE connect returns 0 several seconds before it is a connection, so neither
  the backoff nor the UI can key off that return value: `mesh_app_report_link_errors()` runs
  between `tick()` and `mesh_app_autoconnect()` (a retry restarts the link and clears the
  reason the last attempt failed), pops a transport's `take_error()` line, toasts it when the
  user asked for the connect, and counts the attempt against the backoff. Only an established
  link clears the backoff. The `--status`/`--list-devices` paths in `main.c` do their own connect.
- `src/ui/settings.c` — the Settings tab as data: sections → items (label, formatted value,
  kind, and for editable rows a `field` id). Backends draw the list; `nav.c` walks it
  (`settings_section` open or `MESH_UI_SETTINGS_NO_SECTION`, X yields
  `MESH_UI_ACTION_REFRESH_SETTINGS`). The UI's `struct mesh_ui_settings` in `store.h` is a
  flattened copy without nanopb types, filled by `mesh_app_flatten_settings` in `app.c`.
  Editing is driven by the `k_fields` table here (label, kind, enum names, number presets, text
  byte cap per `enum mesh_ui_setting_field`): the nav keeps pending edits in
  `nav.settings_edits` (Left/Right/A change the row, the keyboard is retargeted for text via
  `keyboard_field`, Y emits `MESH_UI_ACTION_SAVE_SETTINGS`, B asks once then discards), the
  item builder renders them in place marked `dirty`, and `mesh_app_build_settings_write` in
  `app.c` maps each field back onto the nanopb section. Adding an editable field means: the
  enum + table row here, the flatten in `app.c`, the `mesh_app_apply_setting_edit` case, and
  the `item_field` call in the section builder. Sections without fields stay read-only.
  Channels are a two-level list (`nav.settings_channel` is the open slot or
  `MESH_UI_SETTINGS_NO_CHANNEL`; `mesh_ui_settings_channel_at_row` maps a list row to a slot)
  and the Key row is kind `KEY`: `number` is an `enum mesh_ui_psk_choice` (keep, default,
  random 128/256, none, typed hex in `text`), resolved to bytes in `app.c`. Sections named by
  `mesh_ui_settings_section_needs_confirm` (Bluetooth, Channels) get the `confirm_open`
  overlay between Y and the write; the action's `channel` carries the slot. LoRa and Security
  are behind it too. Keys are shown and typed as base64 (`mesh_ui_settings_key_text`/`_parse`,
  hex accepted); each KEY field has a choice mask (`mesh_ui_settings_key_choices`) and a length
  rule (`_key_len_ok`). A new private key is clamped in `app.c` and sent with the public key
  cleared, which the firmware fills in; admin keys are compacted before the write.
- `src/ui/store.c` + `controller.c` — store owns `mesh_ui_snapshot` and signals via eventfd;
  controller drains it and calls `backend->present(snapshot)`. Backends implement the three-function
  `struct mesh_ui_backend` in `include/mesh/ui/backend.h` and live in `src/ui/backends/`.
- Input goes the other way: `src/ui/input.c` reads every `/dev/input/event*`, maps evdev codes
  (BTN_SOUTH.., ABS_HAT0X/Y for the d-pad, arrow keys on a keyboard) to `enum mesh_ui_key`, and
  calls the handler the app installed; quit keys stop the loop before mapping. The handler feeds
  `mesh_ui_controller_handle_key` → `mesh_ui_store_handle_key` → `src/ui/nav.c`, which owns the
  tab/cursor/compose-target model (`struct mesh_ui_nav`, carried inside every snapshot, clamped
  against the lists on each consume) and returns a `mesh_ui_action` (connect, send text) that the
  controller hands to `mesh_app_on_ui_action` in `app.c`. Backends are stateless: they draw the
  cursor from `snapshot->nav`. Nav logic has no fd or device dependency, so test it directly.
  The Messages tab is two levels, the shape the Settings tab already uses: `thread_open` clear
  lists conversations (`mesh_ui_nav_conversation_count`/`_at`: all traffic, each enabled
  channel, each node with direct messages, then "New message"), set shows the one named by
  `target_node`/`target_channel` (or everything, when `inbox`), with the list's cursor parked in
  `conversation_list_cursor` and B backing out. `mesh_ui_nav_filter_messages` is the one place
  that filter lives, so the Messages cursor indexes the filtered list. Unread counts come from
  `struct mesh_ui_read_state` in the store (persisted with the message cache): one
  `packet_id` per conversation meaning "read up to here", chosen over a timestamp or an index
  because ids survive both the ring evicting older messages and the cache merging history back
  in - and a mark whose message has been evicted correctly reads as "everything in view is
  newer". `mesh_ui_store_mark_open_conversation_read` runs from `consume_updates`, so opening a
  thread clears its badge and a message landing in the thread you are sitting in never raises
  one; the all-traffic view marks nothing (it is a view, not a conversation) and its badge is
  the sum of the rest. **Only opening a thread
  moves the target** - the Nodes tab opens the node's conversation rather than retargeting what
  Messages was showing, and Compose is an overlay (`compose_open`) over the open thread rather
  than a tab, so it can never be reached with a stale destination. The on-screen keyboard is
  `keyboard_open` plus `kb_row/kb_col/kb_layer` and `draft`, all in the nav; while it is open
  every key goes to the keyboard handler and tabs do not switch. The `picker_open` overlay
  ("New message") works the same way; its rows come from `mesh_ui_nav_picker_row` (channels,
  then nodes), and picking one opens that conversation. `app.c` ranks nodes before publishing
  (`mesh_app_node_rank`: us, message peers, RF nodes by `last_heard`, MQTT nodes) so the UI's
  128-node budget always holds whoever you are talking to; on an MQTT-fed mesh last_heard alone
  buries them. The post-stop publish in `mesh_app_run` only touches the transport line, because
  the shutdown save would otherwise persist an empty handshake and unresolved peer names.
  Button codes: the Brick's A is `BTN_EAST` (305) and B is `BTN_SOUTH` (304), the reverse of
  the Linux `BTN_A`/`BTN_B` aliases. X and Y do not report by position at all - the button
  printed **Y, on the left, is `BTN_NORTH` (307)**, so X on the top is `BTN_WEST` (308).
  Reading them positionally leaves Y unreachable and silently fires X in its place, which cost
  a round of "the save does nothing" debugging: Y saves a settings section, X refreshes it, so
  every save became a refresh and the edits stayed pending. All four verified from the device
  log by pressing the button; `input_brick_face_buttons` pins them. Do not "fix" any of it back.
- `src/utils/text.c` — the UTF-8 helpers everything that touches radio text shares.
  `mesh_text_sanitise` (folds C0 controls, replaces malformed bytes with `?`, never splits a
  sequence at the buffer boundary) is what `message.c` runs on message bodies and `session.c`
  runs on `long_name`/`short_name`; `mesh_text_utf8_length`/`_offset`/`_truncate` are what the
  fb backend measures lines with. Names are radio input exactly like message text is - whoever
  owns the node picks the bytes - and `User.short_name` is `char[5]`, sized for one four-byte
  emoji and its NUL, so multi-byte names are the norm, not an edge case.
- `src/ui/font5x7.c` — the framebuffer font, keyed by codepoint rather than by byte. ASCII plus
  Latin-1 Supplement and Latin Extended-A; accented letters are **composed** from a base letter
  and a mark (`k_composed`) rather than drawn, so adding one is a line. Lowercase leaves rows 0
  and 1 of the cell free and the mark goes there; capitals and ascenders fill all seven rows, so
  their mark collapses to a one-row silhouette in `glyph.above`, which `fb_draw_glyph` hangs in
  the gap `fb_line_adv` leaves between lines. Consequences of a seven-row cell, all deliberate:
  circumflex/caron/macron/ring are indistinguishable over a capital, and marks that sit *under*
  a letter have nowhere to go, so `Ç` draws as `C`. Anything with no glyph - emoji, CJK - gets
  the replacement box - but only for what `emoji.c` below does not cover.
- `src/ui/emoji.c` + the generated `src/ui/emoji_glyphs.c` — colour emoji sprites, and the
  **display-cell walker** the whole UI measures with. On a real mesh a good share of nodes are
  named entirely in emoji, so without these those rows are indistinguishable boxes.
  `mesh_ui_text_cell_next` is the one place that decides what one drawn column contains, and a
  cell is neither a byte nor always a codepoint: a flag is a regional-indicator pair, a family
  is a ZWJ sequence, and selectors/skin tones attach to what precedes them. Two rules earn
  their keep: matching happens with variation selectors **filtered out** (the font spells its
  keycap `0039 20E3`, people type `0039 FE0F 20E3`), and a single codepoint the text font can
  draw is drawn by the text font (the emoji font claims `#`, `*` and the ten digits because
  they lead keycaps - without this the 9 of "Dog Tracker K9" becomes a grey keycap tile).
  Sequences always win; that is what the extra codepoints mean.
  `scripts/gen-emoji.py` rasterises Noto Color Emoji into the committed table and is **not part
  of the build** - run it by hand, commit the result, the way `Tools/` holds committed aarch64
  helpers. 5626 sprites over 3963 unique 16x16 bitmaps (a third are duplicates), one shared
  255-colour palette, run-length encoded, ~920 KB. The pixels are emitted as one string literal
  on purpose: a braced initialiser of a million integers costs minutes of compile time, the
  literal costs about two seconds, and the file carries its own
  `#pragma GCC diagnostic ignored "-Woverlength-strings"` plus a `.clang-format-ignore` entry.
  Emoji ignore the row's text colour - carrying their own is the point of having them.
- `src/minui_helpers/` — tiny native fallbacks for `minui-list` / `minui-presenter` used when the
  NextUI cross toolchain isn't available; they honor `MESHCLIENT_MINUI_SELECTION`.

The fb backend draws into page 0 of the Brick's 1024x16384 framebuffer, then `FBIOPAN_DISPLAY`s
to it and mirrors the frame into page 1, because the Allwinner display engine keeps showing the
page NextUI's SDL last flipped to (page 1 in practice). The layer blends with per-pixel alpha,
so `compose_color` always writes an opaque alpha byte. Drop any of these and the screen is black.

Its text is measured in **cells, not bytes**: `fb_draw_text` walks `mesh_ui_text_cell_next` and
spends one cell per character or emoji, and `fb_fit`/`fb_width` (over
`mesh_ui_text_cell_truncate`/`mesh_ui_text_cells`) are the only right way to clip or
right-align a line. A `strlen` in layout code is a bug - it used to mean an emoji name counted
four columns and drew four question marks, and it still means padding computed from bytes
pushes right-aligned metrics off the edge. `%-Ns` has the same problem and is why the Nodes tab
pads its short-name field by hand. `fb_draw_emoji` draws a sprite across the full character
advance rather than the glyph's five columns, so an emoji stands as tall as the capitals next
to it; the sprites' own transparent margins keep neighbours apart.

Backend selection (`MESHCLIENT_UI_BACKEND=auto|minui|fb|cli|stub`, in `app.c`): `auto` prefers
minui if helpers are on PATH, then fb (`/dev/fb0`), then cli. `launch.sh` forces `fb` on device.

An SDL2 backend was tried in 1.1.11 and replaced by `fb` in 1.1.12; it is gone from the tree
(recoverable from commit 62fcb09 if ever wanted).

## Protobufs

`CMakeLists.txt` has a hardcoded `MESH_PROTO_NAMES` list (mesh, portnums, interdevice, config,
module_config, telemetry, channel, device_ui, xmodem, atak, admin, connection_status). Adding a new upstream `.proto` means adding
it there. Generated headers are included as `meshtastic/<name>.pb.h`. The generator is
`nanopb_generator` from PATH, falling back to `third_party/nanopb/generator/nanopb_generator.py`
via Python3 (needs `pip install protobuf grpcio-tools`).

## Release and packaging

- semantic-release on `main`/`beta`/`rc`. Conventional Commits decide the bump: `feat` minor,
  `fix`/`perf`/`refactor`/`revert` patch, `docs`/`chore`/`ci`/`test`/`build`/`style` no release.
  Recent history is almost entirely `fix:` commits, each producing a release.
- The release workflow rewrites `project(meshclient VERSION x.y.z ...)` in `CMakeLists.txt` with
  `sed`. Do not change that line's shape and do not bump it by hand. `package.json` version is
  unused. Keep `conventional-changelog-conventionalcommits` on 9.x until semantic-release's
  notes generator ships `conventional-changelog-writer` 9; 10.x fails `generateNotes` with
  "Missing helper" (Dependabot is told to ignore it). Local dry runs need Node 24.10+, e.g.
  `docker run --rm -v "$PWD":/src -w /src -e GITHUB_TOKEN=$(gh auth token) node:24 bash -c
  'npm install && npx semantic-release --dry-run --branches <pushed-branch>'`.
- Release builds cross-compile with the Bootlin `aarch64--musl` toolchain and statically link a
  from-source libdbus built with meson and `message_bus=false` (library only, no daemon, no expat);
  see `.github/workflows/semantic-release.yml`. CI (`ci.yml`) is host-only gcc/clang on
  ubuntu-24.04 and does not cross-compile. Version pins for the toolchain and dbus live in both
  that workflow and `docker/setup-cross.sh`; bump them together. `make docker-pak` reproduces the
  release build locally via `scripts/cross-build.sh`; on an arm64 host the `cross` image uses
  native `musl-gcc` (exposed as `aarch64-linux-musl-gcc`) instead of downloading Bootlin.
- `scripts/build_minui_helpers.sh` stages the helper binaries into the tracked
  `Tools/tg5040/MeshClient.pak/bin/tg5040/` tree, which holds committed **aarch64** artifacts.
  Without a cross toolchain it falls back to the host compiler, so every install is checked
  against the platform's expected ELF machine (`e_machine` 183 for tg5040) and skipped with a
  warning on a mismatch - a native `make package` on x86_64 leaves the committed binaries
  alone instead of replacing them with host-arch ones. `MESHCLIENT_ALLOW_HOST_HELPERS=1`
  overrides this. It warns rather than failing, so CI's host-only `make package` stays green.
- `scripts/package.sh` copies `$BUILD_ROOT/release/meshclient` into `dist/MeshClient.pak/bin/shared/`
  plus everything under `Tools/tg5040/MeshClient.pak/bin/{shared,tg5040}/` and `launch.sh`.
  `launch.sh` must stay POSIX sh; it sets `HOME` to the pak userdata dir, points
  `DBUS_SYSTEM_BUS_ADDRESS` at the system socket, and tees output to
  `/.userdata/tg5040/logs/MeshClient.txt`.

## Tests

One harness, `tests/test_main.c`, with a `k_test_cases` table tagged by category (`unit` today;
`integration`/`hardware` reserved). Register new cases in that table; use
`record_failure`/`record_success`. Tests must not touch real BlueZ — use the bluez mock. New
CTest labels need a matching `add_test` in `tests/CMakeLists.txt`. Verified state as of 2026-09-04: 71 unit tests, all passing in
the dev container with zero compiler warnings. `message_encode_text_golden` pins the
`TEXT_MESSAGE_APP` wire format against a hand-derived byte vector (not against our own encoder),
so a protobuf regeneration that changes field numbers or wire types fails loudly.

`make format` rewrites every tracked `.c`/`.h` using `.clang-format`, and the tree is kept
normalised against it - on a clean tree the command is a no-op. It is normalised with
**clang-format 18** (what `ubuntu:24.04` ships, so the dev container and CI agree); a
different major version reflows things and produces spurious churn, so run `make format`
inside `make docker-shell` if your host's clang-format is a different major. Nothing gates on
formatting in CI, precisely because host versions vary.
