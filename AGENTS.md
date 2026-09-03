# Repository Guidelines

## Project Structure & Module Organization
Keep platform-agnostic client code in `src/` (subfolders such as `transport/ble`, `transport/serial`, `ui/`) with shared headers under `include/`. Device-facing assets live in `Tools/tg5040/MeshClient.pak/`; use `bin/shared/` for utilities bundled across platforms and `bin/tg5040/` for Brick-specific binaries. Place reusable scripts (`build.sh`, `package.sh`, future `format.sh`) under `scripts/`, and capture protocol or UX references in `docs/`. Mirror the `src/` layout in `tests/` so each module has a matching test target.

## Build, Test, and Development Commands
- `make debug` – configure + build a debug tree (defaults to `build/debug`; Linux only).
- `make docker-test` / `make docker-pak` – run the build+tests or the static aarch64 pak build inside the containers from `docker/Dockerfile` (use these on macOS).
- `make release` – build the optimized pak binary set; pass `CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` to toggle sanitizers.
- `cmake --build build/debug --target meshclient` – rebuild a single target after edits.
- `make test` – run unit tests locally.
- `make package` – assemble `MeshClient.pak` for sideloading; inspect the output zip before publishing.
- `make proto` – regenerate nanopb sources after editing files under `proto/meshtastic/meshtastic/` (requires `protoc` and the `nanopb_generator` script on PATH).
- Always sync submodules (`git submodule update --init --recursive`) after pulling to pick up upstream protobuf changes.

## Coding Style & Naming Conventions
Run `clang-format -i $(rg --files -g"*.[ch]")` before pushing; the repo ships `.clang-format` to keep 4-space indents, LLVM brace style, and 100-character lines. Use `snake_case` for functions and locals, `PascalCase` for structs/enums, and `kCamelCase` for file-scope constants. Keep platform conditionals isolated in per-transport files, and favor small static helpers over macros.

## Testing Guidelines
Today every test lives in `tests/test_main.c`, registered in the `k_test_cases` table with a category tag; add new cases there (or split into `tests/<module>_test.c` files once the harness supports it) and keep the shared CTest registration. Prefer deterministic fixtures over live radio calls; integration exercises that require hardware should be tagged `HARDWARE` so they can be skipped in CI (`ctest -E HARDWARE`). Aim to cover error paths—especially BLE reconnection and protobuf parsing—before promoting a feature. If adding a transport, include a golden protobuf frame test to validate encoding/decoding.

## Commit & Pull Request Guidelines
Write commit subjects in imperative mood with an optional scope prefix (`ble: retry mtu negotiation`). Keep bodies wrapped at 72 characters and mention relevant Meshtastic issue IDs when applicable. PRs must include: a concise summary, validation notes (commands run or hardware tested), screenshots for UI-facing work, and any follow-up TODOs. Link to design docs in `docs/` when behavior changes materially, and request review from both protocol and UI owners for cross-cutting updates.

## Packaging & Deployment Tips
Check that `launch.sh` remains POSIX-compliant and logs to `.userdata/tg5040/logs/`. After `scripts/package.sh`, sideload the pak into `Tools/tg5040/` (`make deploy` over SSH, see `docs/device.md`) and verify it starts from NextUI Launcher before merging. Avoid committing user-specific paths or logs—only ship reproducible assets inside the pak tree.
