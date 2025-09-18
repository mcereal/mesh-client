# Mesh Client for TrimUI Brick

A lightweight Meshtastic client targeting the TrimUI Brick and other NextUI/MinUI devices. The project focuses on a small C core with pluggable transports, starting with Bluetooth LE, and ships as a TrimUI pak for sideloading.

> Looking for the full architecture notes and roadmap? See [`docs/poc-architecture.md`](docs/poc-architecture.md).

## Quick Start

```bash
# Debug build (requires CMake ≥3.18 and a C17 toolchain)
make debug

# Run the client locally (or use `make run`)
./build/debug/meshclient --foreground --log-level debug

# Execute unit tests
make test
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
- **CLI options:** `meshclient --help` lists foreground mode, BLE toggles, preferred device, timeout, and log-level flags.
- **Runtime env vars:** `MESHCLIENT_RUN_MODE`, `MESHCLIENT_IDLE_TIMEOUT_MS`, `MESHCLIENT_DISABLE_BLE`, `MESHCLIENT_PREFERRED_BLE_DEVICE`.

## Repository Layout

- `src/` — application core, event loop, transports, utilities.
- `include/` — public headers consumed by transports and tests.
- `scripts/` — build/package automation.
- `tests/` — lightweight unit tests (run via CTest).
- `Tools/tg5040/MeshClient.pak/` — TrimUI pak scaffold including `launch.sh` and platform bins.
- `AGENTS.md` — contributor guide with coding and review expectations.

## Contributing

Follow the guidelines in [`AGENTS.md`](AGENTS.md) for code style, testing, and pull request expectations.

## License

This project is released under the terms of the license in [`LICENSE`](LICENSE).
