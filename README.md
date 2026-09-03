# Mesh Client for TrimUI Brick

A lightweight Meshtastic client targeting the TrimUI Brick and other NextUI/MinUI devices. The project focuses on a small C core with pluggable transports, starting with Bluetooth LE, and ships as a TrimUI pak for sideloading.

> Looking for the full architecture notes and roadmap? See [`docs/poc-architecture.md`](docs/poc-architecture.md), the transport breakdown in [`docs/transport.md`](docs/transport.md), and the UI strategy in [`docs/ui-strategy.md`](docs/ui-strategy.md).

## Quick Start

```bash
# Ensure submodules (Meshtastic protobufs, nanopb) are available
git submodule update --init --recursive
```

The core is Linux-only (`epoll`/`timerfd`/`eventfd`). On macOS, or any host with Docker, use the
container targets — they bind-mount the repo and build into `build/linux/`:

```bash
make docker-test     # Debug build + unit tests in the dev container (image built on first use)
make docker-shell    # bash inside the container for ad-hoc cmake/ctest work
make docker-pak      # static aarch64 build → dist/MeshClient.pak.zip for the TrimUI Brick
```

On a Linux host you can build natively — no Docker needed:

```bash
# One-time: git submodules, libdbus-1-dev, and the Python protobuf packages
# the nanopb generator needs. Safe to re-run; --check reports without installing.
make setup

# Debug build (requires CMake ≥3.18 and a C17 toolchain)
make debug

# Run the client locally (or use `make run`)
./build/debug/meshclient --foreground --log-level debug

# Execute unit tests
make test

# List nearby Meshtastic BLE nodes and exit
./build/debug/meshclient --list-devices

# Print cached handshake / node summary (use --json for machine-readable output)
./build/debug/meshclient --status --json

# Update a JSON cache for MinUI/automation
./build/debug/meshclient --status --status-output "$HOME/.userdata/meshclient/status.json"

# Send a text message (broadcast on the primary channel) and exit
./build/debug/meshclient --send-text "hello mesh"

# Direct message a node, waiting for delivery confirmation
./build/debug/meshclient --send-text "on my way" --dest '!433d1a2c' --ack
```

If CMake is not installed, install it with your package manager first (e.g. `sudo apt install cmake`).

## TrimUI Packaging & Deployment

```bash
# Build a release binary and emit MeshClient.pak.zip under dist/
make release
make package
```

From macOS, `make docker-pak` does the same via the cross container (static aarch64 binary, static libdbus) and also writes `dist/MeshClient.pak.zip.sha256`.

Copy the resulting `dist/MeshClient.pak.zip` (or the extracted `MeshClient.pak/` folder) to `Tools/tg5040/` on the TrimUI SD card. From NextUI Launcher, the app will appear under Tools. Logs are written to `/.userdata/tg5040/logs/MeshClient.txt` on the device.

Once the Brick is on WiFi with the SSH Server pak installed, skip the SD card: set `BRICK_HOST` in `.brick.env` (copy `.brick.env.example`) and use `make brick` (build + push), `make deploy`, `make deploy-logs`, `make deploy-run ARGS="--list-devices"`, and `make deploy-check`. See [`docs/device.md`](docs/device.md) for the one-time device setup and troubleshooting.

## Development Environment

- **Toolchain:** GCC or Clang with C17 support, `cmake`, `ninja` or Make (CMake will pick the default generator). The core is Linux-only (`epoll`, `timerfd`, `eventfd`); on macOS use the `make docker-*` targets, which wrap `scripts/docker.sh` and the images in `docker/Dockerfile` (`dev` mirrors `ci.yml`, `cross` mirrors the release workflow's static aarch64 musl build).
- **Build tree:** `BUILD_ROOT` (default `build`) selects the build tree root; container builds use `build/linux` so they never share a CMake cache with a host configure.
- **Make targets:** `make debug`, `make release`, `make test`, `make package`, `make run`, and `make format` cover the common workflows; the `docker-*` variants run them in the container (see `make help`).
- **Sanitizers:** Enable with `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or swap for `UBSAN`).
- **Logging:** Adjust verbosity using the `--log-level` flag. Logs stream to `stderr` locally and to the pak log on device.
- **CLI options:** `meshclient --help` lists foreground mode, BLE toggles, preferred device, timeout, and log-level flags. `--status --json` now emits a `cached` flag (and `cached_handshake` / `cached_messages` when offline) so automation can detect stale snapshots.
- **Messaging:** `--send-text TEXT` connects, sends a `TEXT_MESSAGE_APP` packet and exits. `--dest` takes `!hex`, `0xhex`, decimal, or `all` (the default broadcast); `--channel N` selects the channel index; `--ack` requests delivery confirmation and waits for the `Routing` reply. Broadcasts are never acked by the mesh, so `--ack` is ignored for them. Received messages appear in `--status`, in the on-device HUD, and in the persisted cache, so the last conversation is readable with the radio out of range.
- **Runtime env vars:** `MESHCLIENT_RUN_MODE`, `MESHCLIENT_IDLE_TIMEOUT_MS`, `MESHCLIENT_DISABLE_BLE`, `MESHCLIENT_PREFERRED_BLE_DEVICE`.
- **Testing strategy:** see [`docs/testing.md`](docs/testing.md) for the test categories, filtering options, and future coverage plan (`ctest -L unit` mirrors `make test`).
- **UI backend:** Set `MESHCLIENT_UI_BACKEND=auto|minui|fb|cli|stub` to pick the renderer. The TrimUI pak defaults to the `fb` backend which paints a simple framebuffer HUD (with controller exit handling) and falls back to CLI when the framebuffer is unavailable. The MinUI backend still emits JSON menus for `minui-list` and handles selections to request BLE connects without blocking. On desktop the placeholder helpers select the first device automatically; export `MESHCLIENT_MINUI_SELECTION=<index>` to pick a different row.
- **UI prefs:** Last-connected device is cached under `$HOME/.meshclient/ui_prefs` to seed future sessions.
- **MinUI helpers:** `make package` automatically runs `scripts/build_minui_helpers.sh` (see `MESHCLIENT_MINUI_*` variables) to compile and stage the NextUI-based list/keyboard helpers when preparing device builds.
- **BlueZ:** At runtime the BLE transport connects to system D-Bus, locates the first adapter, and begins discovery. If BlueZ or an adapter is missing, the transport reports `waiting-for-bluez` / `waiting-for-adapter` and stays idle without failing the app.
- **Protobufs:** `third_party/nanopb` (runtime) and `proto/meshtastic` (schemas) are git submodules. Generated `.pb.c/.pb.h` land in `build/<type>/generated/nanopb/` at build time; the set of compiled `.proto` files is the `MESH_PROTO_NAMES` list in `CMakeLists.txt`.
- **Protogen:** Use `make proto` to regenerate nanopb sources from files in `proto/meshtastic/meshtastic/` (requires `protoc` plus the `nanopb_generator` script; install via `pip install nanopb` or ensure `nanopb_generator` is on PATH).
- **Discovery cache:** BLE transport keeps a mockable in-memory list of nearby Meshtastic nodes (address/name/RSSI) filtered on the Meshtastic service UUID (`6ba1b218-…`) for downstream UI components. Data flows per the [Meshtastic client API](https://meshtastic.org/docs/development/device/client-api/): raw protobufs on ToRadio, FromNum notify then FromRadio reads. Nodes using a PIN must be paired with BlueZ once (`bluetoothctl pair <addr>`) before the app can connect.
- **Semantic Release:** This project uses [semantic-release](https://github.com/semantic-release/semantic-release) to automate versioning and releases based on [Conventional Commits](https://www.conventionalcommits.org/). See [`docs/semantic-release.md`](docs/semantic-release.md) for usage details and commit message conventions.

## Repository Layout

- `src/` — application core, event loop, transports, UI scaffolding, utilities.
- `include/` — public headers consumed by transports and tests.
- `scripts/` — build/package automation, plus `docker.sh` (container wrapper) and `cross-build.sh` (static aarch64 pak build).
- `docker/` — `Dockerfile` (`dev` and `cross` stages) and the cross toolchain bootstrap.
- `tests/` — lightweight unit tests (run via CTest).
- `Tools/tg5040/MeshClient.pak/` — TrimUI pak scaffold including `launch.sh` and platform bins.
- `docs/` — architecture roadmap and transport-specific progress notes.
- `AGENTS.md` — contributor guide with coding and review expectations.
- `proto/meshtastic/` — upstream Meshtastic protobuf definitions (git submodule).

## Contributing

Follow the guidelines in [`AGENTS.md`](AGENTS.md) for code style, testing, and pull request expectations.

## License

This project is released under the terms of the license in [`LICENSE`](LICENSE).
