# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C17 Meshtastic client for the TrimUI Brick (NextUI/MinUI, platform key `tg5040`), shipped as a
`MeshClient.pak`. Single-threaded epoll event loop, BlueZ-over-D-Bus BLE transport, nanopb for
Meshtastic protobufs. `AGENTS.md` is the contributor guide (style, tests, PR expectations) and
still applies; this file adds what's not obvious from it.

## Commands

**The core is Linux-only** (`epoll`, `timerfd`, `eventfd`). This repo is developed on macOS, so
build and test through the containers. `make docker-*` wraps `scripts/docker.sh`, which builds the
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
  node-summary cache decoded from `FromRadio`. BLE is **not** Nordic UART and has no length
  framing: one bare protobuf per GATT write/read. `src/proto/framing.c` is for serial/TCP only.
  Nodes in PIN mode must be paired with BlueZ out of band (`bluetoothctl pair`) before connect.
- `src/core/app.c` — `mesh_app_publish_ui_state()` copies BLE discovery/handshake state into the
  UI store every loop iteration, persists the handshake cache and preferences under `$HOME`
  (`~/.meshclient/ui_prefs`, `ui_prefs.handshake`), and picks the UI backend.
- `src/ui/store.c` + `controller.c` — store owns `mesh_ui_snapshot` and signals via eventfd;
  controller drains it and calls `backend->present(snapshot)`. Backends implement the three-function
  `struct mesh_ui_backend` in `include/mesh/ui/backend.h` and live in `src/ui/backends/`.
- `src/minui_helpers/` — tiny native fallbacks for `minui-list` / `minui-presenter` used when the
  NextUI cross toolchain isn't available; they honor `MESHCLIENT_MINUI_SELECTION`.

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
  unused.
- Release builds cross-compile with the Bootlin `aarch64--musl` toolchain and statically link a
  from-source libdbus built with meson and `message_bus=false` (library only, no daemon, no expat);
  see `.github/workflows/semantic-release.yml`. CI (`ci.yml`) is host-only gcc/clang on
  ubuntu-24.04 and does not cross-compile. Version pins for the toolchain and dbus live in both
  that workflow and `docker/setup-cross.sh`; bump them together. `make docker-pak` reproduces the
  release build locally via `scripts/cross-build.sh`; on an arm64 host the `cross` image uses
  native `musl-gcc` (exposed as `aarch64-linux-musl-gcc`) instead of downloading Bootlin.
- `scripts/package.sh` copies `$BUILD_ROOT/release/meshclient` into `dist/MeshClient.pak/bin/shared/`
  plus everything under `Tools/tg5040/MeshClient.pak/bin/{shared,tg5040}/` and `launch.sh`.
  `launch.sh` must stay POSIX sh; it sets `HOME` to the pak userdata dir, points
  `DBUS_SYSTEM_BUS_ADDRESS` at the system socket, and tees output to
  `/.userdata/tg5040/logs/MeshClient.txt`.

## Tests

One harness, `tests/test_main.c`, with a `k_test_cases` table tagged by category (`unit` today;
`integration`/`hardware` reserved). Register new cases in that table; use
`record_failure`/`record_success`. Tests must not touch real BlueZ — use the bluez mock. New
CTest labels need a matching `add_test` in `tests/CMakeLists.txt`. Verified state as of 2026-09-03: 13 unit tests, all passing in
the dev container with zero compiler warnings.
