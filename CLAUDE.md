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

Verified 2026-09-05: 106 unit tests, all passing, zero compiler warnings.
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
| Admin protocol | `src/core/radio_settings.c` | `AdminMessage` get/set queue, passkeys, radio actions, NodeDB verbs |
| Messaging | `src/core/message.c` | text packets, message ring, ack correlation |
| App glue | `src/core/app*.c` | `app` lifecycle/link, `_actions` UI actions, `_publish` to store, `_settings` writes |
| Self-update | `src/core/updater.c`, `version.c` | forks curl, SemVer, digest-verified install |
| UI | `src/ui/` | store/controller + `nav*.c` + `settings*.c` + `backends/{fb*,cli,stub}.c`; **`fb` is the device UI** |
| Text | `src/utils/text.c`, `src/ui/{font5x7,emoji}.c` | UTF-8 sanitising, cell-based measurement |
| Shared utils | `src/utils/` | `text` (UTF-8 + `mesh_str_copy`), `time` (`mesh_time_monotonic_ms`), `env` (`mesh_env_bool`/`_int`), `log`, `sha256`, `array` |

`include/mesh/` mirrors `src/` one-for-one — `core/`, `transport/`, `ui/`, `proto/`, `utils/` —
so a header always sits in the directory named after the source file that defines it.

Four subsystems are split across several files sharing one `*_internal.h` next to them
(`src/core/app_internal.h`, `src/ui/nav_internal.h`, `src/ui/settings_internal.h`,
`src/ui/backends/fb_internal.h`). Those headers are **not** public API — they declare only what
would still be `static` if the group were one file, and nothing outside the group should include
one. A symbol added to an internal header is a seam widened; prefer keeping the call inside the
file that owns the state.

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
  on a radio swap, and `in_nodedb` marks what the radio no longer carries.
- **A node with no `User` is named after its node number**, exactly as the phone apps do
  (`mesh_session_default_identity`). An empty `User` in a NodeInfo must not blank a name we have.
- **A radio reboot after a settings write is expected.** The link drops and auto-connect
  reconnects.
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
| [`docs/cli.md`](docs/cli.md) | flags, environment variables, on-device controls |
| [`docs/device.md`](docs/device.md) | Brick setup, deploy loop, screenshots, troubleshooting |
| [`docs/settings-roadmap.md`](docs/settings-roadmap.md) | radio settings phases and admin verbs |
| [`docs/semantic-release.md`](docs/semantic-release.md) | versioning, packaging, release assets |
| [`docs/testing.md`](docs/testing.md) | test categories and how to run them |
