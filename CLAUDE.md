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

Manual checks: `meshclient --list-devices`, `--status --json`, `--status --status-output PATH`.
Inside the container there is no BlueZ, so the BLE transport sits in `waiting-for-bluez` and the
CLI backend is selected; without D-Bus headers at build time it compiles out entirely
(`MESH_HAVE_DBUS` is set only if `pkg-config dbus-1` succeeds) and reports `disabled`.

## Architecture

Data flows one direction: transport → `mesh_app` → UI store → controller → backend.

- `src/core/event_loop.c` — epoll loop with a fixed table of 32 fd sources. Everything (D-Bus
  watches, timerfd discovery refresh, UI store eventfd, minui-list child stdout) registers here.
  No threads anywhere; do not add them.
- `src/transport/transport_registry.c` — `struct mesh_transport_ops {start, stop, status, tick}`.
  Only BLE is registered today; Serial/HTTP are planned to plug in here.
- `src/transport/ble/bluez_client.c` — raw libdbus wrapper for `org.bluez` (adapter discovery,
  `GetManagedObjects`, GATT Connect/StartNotify/Write). Has a compile-time-independent mock
  (`mesh_bluez_client_mock_enable`) that tests use to script results and capture writes; there is
  no real BlueZ in CI.
- `src/transport/ble/ble_transport.c` — state machine (`disabled` → `waiting-for-bluez` →
  `waiting-for-adapter` → `running`), Meshtastic service UUID filtering, the `want_config_id`
  handshake, an outbound ToRadio packet queue, FromNum-notify → FromRadio-read drain loop, and a
  node-summary cache decoded from `FromRadio` (256 entries; every inbound `MeshPacket` also
  refreshes its sender's `last_heard`/SNR/hops, adding the sender by id if the sync never
  delivered its NodeInfo), plus the radio's channel table (`FromRadio.channel`,
  by slot, role DISABLED kept so indices stay meaningful). BLE is **not** Nordic UART and has no length
  framing: one bare protobuf per GATT write/read. `src/proto/framing.c` is for serial/TCP only.
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
- `src/core/message.c` — transport-agnostic messaging: builds `TEXT_MESSAGE_APP` packets into a
  `ToRadio`, folds inbound `MeshPacket`s into a fixed ring (`mesh_message_log`), and correlates
  `ROUTING_APP` replies with the outbound message they ack. Message text is untrusted radio
  input: `mesh_message_ingest` sanitises control bytes so backends can draw it directly.
- `src/core/app.c` — `mesh_app_publish_ui_state()` copies BLE discovery/handshake state into the
  UI store every loop iteration, persists the handshake cache and preferences under `$HOME`
  (`~/.meshclient/ui_prefs`, `ui_prefs.handshake`), and picks the UI backend.
  `mesh_app_autoconnect()` runs every foreground turn: preferred node if in range, else the
  strongest advertiser after 30 s, exponential backoff on failure; `MESHCLIENT_AUTOCONNECT=0`
  turns it off. The `--status`/`--list-devices` paths in `main.c` do their own connect.
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
  The nav's `target_node`/`target_channel` pair is both the Compose destination and the
  conversation the Messages tab shows (`inbox` shows everything); `mesh_ui_nav_filter_messages`
  is the one place that filter lives, so the Messages cursor indexes the filtered list. The
  on-screen keyboard is `keyboard_open` plus `kb_row/kb_col/kb_layer` and `draft`, all in the
  nav; while it is open every key goes to the keyboard handler and tabs do not switch. The
  `picker_open` overlay (Compose To:) works the same way; its rows come from
  `mesh_ui_nav_picker_row` (channels, then nodes). `app.c` sorts nodes by `last_heard` before
  publishing, so the UI's 64-node budget holds the recently active ones.
  Button positions: the Brick's A is `BTN_EAST` (305) and B is `BTN_SOUTH` (304), the reverse
  of the Linux `BTN_A`/`BTN_B` aliases. Verified from the device log; do not "fix" it back.
- `src/minui_helpers/` — tiny native fallbacks for `minui-list` / `minui-presenter` used when the
  NextUI cross toolchain isn't available; they honor `MESHCLIENT_MINUI_SELECTION`.

The fb backend draws into page 0 of the Brick's 1024x16384 framebuffer, then `FBIOPAN_DISPLAY`s
to it and mirrors the frame into page 1, because the Allwinner display engine keeps showing the
page NextUI's SDL last flipped to (page 1 in practice). The layer blends with per-pixel alpha,
so `compose_color` always writes an opaque alpha byte. Drop any of these and the screen is black.

Backend selection (`MESHCLIENT_UI_BACKEND=auto|minui|fb|cli|stub`, in `app.c`): `auto` prefers
minui if helpers are on PATH, then fb (`/dev/fb0`), then cli. `launch.sh` forces `fb` on device.

An SDL2 backend was tried in 1.1.11 and replaced by `fb` in 1.1.12; it is gone from the tree
(recoverable from commit 62fcb09 if ever wanted).

## Protobufs

`CMakeLists.txt` has a hardcoded `MESH_PROTO_NAMES` list (mesh, portnums, interdevice, config,
module_config, telemetry, channel, device_ui, xmodem, atak). Adding a new upstream `.proto` means adding
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
CTest labels need a matching `add_test` in `tests/CMakeLists.txt`. Verified state as of 2026-09-04: 40 unit tests, all passing in
the dev container with zero compiler warnings. `message_encode_text_golden` pins the
`TEXT_MESSAGE_APP` wire format against a hand-derived byte vector (not against our own encoder),
so a protobuf regeneration that changes field numbers or wire types fails loudly.

`make format` rewrites every tracked `.c`/`.h` using `.clang-format`, and the tree is kept
normalised against it - on a clean tree the command is a no-op. It is normalised with
**clang-format 18** (what `ubuntu:24.04` ships, so the dev container and CI agree); a
different major version reflows things and produces spurious churn, so run `make format`
inside `make docker-shell` if your host's clang-format is a different major. Nothing gates on
formatting in CI, precisely because host versions vary.
