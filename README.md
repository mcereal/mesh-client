# Mesh Client for TrimUI Brick

A lightweight Meshtastic client targeting the TrimUI Brick and other NextUI/MinUI devices. The project focuses on a small C core with pluggable transports, starting with Bluetooth LE, and ships as a TrimUI pak for sideloading.

> Looking for the full architecture notes and roadmap? See [`docs/poc-architecture.md`](docs/poc-architecture.md), the transport breakdown in [`docs/transport.md`](docs/transport.md), and the UI strategy in [`docs/ui-strategy.md`](docs/ui-strategy.md).

## Quick Start

```bash
# Ensure submodules (Meshtastic protobufs, nanopb) are available
git submodule update --init --recursive
```

```bash
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
```

If CMake is not installed, install it with your package manager first (e.g. `sudo apt install cmake`).

## TrimUI Packaging & Deployment

```bash
# Build a release binary and emit MeshClient.pak.zip under dist/
make release
make package
```

Copy the resulting `dist/MeshClient.pak.zip` (or the extracted `MeshClient.pak/` folder) to `Tools/tg5040/` on the TrimUI SD card. From NextUI Launcher, the app will appear under Tools. Logs are written to `/.userdata/tg5040/logs/MeshClient.txt` on the device.

## Development Environment

- **Toolchain:** GCC or Clang with C17 support, `cmake`, `ninja` or Make (CMake will pick the default generator).
- **Make targets:** `make debug`, `make release`, `make test`, `make package`, and `make run` cover the common workflows (see `make help`).
- **Sanitizers:** Enable with `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or swap for `UBSAN`).
- **Logging:** Adjust verbosity using the `--log-level` flag. Logs stream to `stderr` locally and to the pak log on device.
- **CLI options:** `meshclient --help` lists foreground mode, BLE toggles, preferred device, timeout, and log-level flags. `--status --json` now emits a `cached` flag (and `cached_handshake` object when offline) so automation can detect stale snapshots.
- **Runtime env vars:** `MESHCLIENT_RUN_MODE`, `MESHCLIENT_IDLE_TIMEOUT_MS`, `MESHCLIENT_DISABLE_BLE`, `MESHCLIENT_PREFERRED_BLE_DEVICE`.
- **Testing strategy:** see [`docs/testing.md`](docs/testing.md) for the test categories, filtering options, and future coverage plan (`ctest -L unit` mirrors `make test`).
- **UI backend:** Set `MESHCLIENT_UI_BACKEND=auto|minui|fb|cli|stub` to pick the renderer. The TrimUI pak defaults to the `fb` backend which paints a simple framebuffer HUD (with controller exit handling) and falls back to CLI when the framebuffer is unavailable. The MinUI backend still emits JSON menus for `minui-list` and handles selections to request BLE connects without blocking. On desktop the placeholder helpers select the first device automatically; export `MESHCLIENT_MINUI_SELECTION=<index>` to pick a different row.
- **UI prefs:** Last-connected device is cached under `$HOME/.meshclient/ui_prefs` to seed future sessions.
- **MinUI helpers:** `make package` automatically runs `scripts/build_minui_helpers.sh` (see `MESHCLIENT_MINUI_*` variables) to compile and stage the NextUI-based list/keyboard helpers when preparing device builds.
- **BlueZ:** At runtime the BLE transport connects to system D-Bus, locates the first adapter, and begins discovery. If BlueZ or an adapter is missing, the transport reports `waiting-for-bluez` / `waiting-for-adapter` and stays idle without failing the app.
- **Protobuf scaffolding:** `third_party/nanopb` currently ships a stub; replace with upstream nanopb before shipping and add generated Meshtastic protobufs.
- **Protogen:** Use `make proto` to regenerate nanopb sources from files in `proto/meshtastic/meshtastic/` (requires `protoc` plus the `nanopb_generator` script; install via `pip install nanopb` or ensure `nanopb_generator` is on PATH).
- **Discovery cache:** BLE transport keeps a mockable in-memory list of nearby Meshtastic nodes (address/name/RSSI) filtered on the Nordic UART UUID for downstream UI components.
- **Semantic Release:** This project uses [semantic-release](https://github.com/semantic-release/semantic-release) to automate versioning and releases based on [Conventional Commits](https://www.conventionalcommits.org/). See [`docs/semantic-release.md`](docs/semantic-release.md) for usage details and commit message conventions.

## Repository Layout

- `src/` — application core, event loop, transports, UI scaffolding, utilities.
- `include/` — public headers consumed by transports and tests.
- `scripts/` — build/package automation.
- `tests/` — lightweight unit tests (run via CTest).
- `Tools/tg5040/MeshClient.pak/` — TrimUI pak scaffold including `launch.sh` and platform bins.
- `docs/` — architecture roadmap and transport-specific progress notes.
- `AGENTS.md` — contributor guide with coding and review expectations.
- `proto/meshtastic/` — upstream Meshtastic protobuf definitions (git submodule).

## Contributing

Follow the guidelines in [`AGENTS.md`](AGENTS.md) for code style, testing, and pull request expectations.

## License

This project is released under the terms of the license in [`LICENSE`](LICENSE).
