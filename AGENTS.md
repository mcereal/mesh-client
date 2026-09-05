# Repository Guidelines

## Project Structure & Module Organization

Keep platform-agnostic client code in `src/` (subfolders such as `core/`, `transport/ble`,
`transport/serial`, `proto/`, `ui/`, `utils/`) with shared headers under `include/`.
Device-facing assets live in `Tools/tg5040/MeshClient.pak/`: `bin/shared/` for utilities bundled
across platforms, `bin/tg5040/` for committed aarch64 binaries. Reusable scripts go under
`scripts/`, and protocol or UX references under `docs/`. Tests all live in `tests/test_main.c`
today; see [`docs/testing.md`](docs/testing.md).

## Build, Test, and Development Commands

- `make setup` — install prerequisites on a Linux host (safe to re-run; `--check` reports only).
- `make debug` — configure and build a debug tree (`build/debug`; Linux only).
- `make test` — build and run the unit suite.
- `make docker-test` / `make docker-pak` — the same, or the static aarch64 pak build, inside the
  containers from `docker/Dockerfile`. **Use these on macOS.**
- `make release` / `make package` — optimized binary, then `dist/MeshClient.pak.zip`. Inspect the
  zip before publishing.
- `make proto` — regenerate nanopb sources after editing `proto/meshtastic/meshtastic/`.
- `cmake --build build/debug --target meshclient` — rebuild a single target after edits.
- `make format` — clang-format the tree. See the note below.
- Sanitizers: `make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or `UBSAN`).

Always sync submodules (`git submodule update --init --recursive`) after pulling; CMake
FATAL_ERRORs without them.

## Coding Style & Naming Conventions

Run `make format` before pushing — not `clang-format` by hand. The repo ships `.clang-format`
(4-space indents, LLVM brace style, 100-character lines) and the tree is normalised with
**clang-format 18**, so `make format` refuses to run under a different major version; from a host
with another one use `./scripts/docker.sh make format`.

Use `snake_case` for functions and locals, `PascalCase` for structs/enums, and `kCamelCase` for
file-scope constants. Keep platform conditionals isolated in per-transport files, and favour
small static helpers over macros.

## Testing Guidelines

Register new cases in the `k_test_cases` table in `tests/test_main.c` with a category tag, and
use `record_failure` / `record_success` so failures reach the summary. Tests must never touch
real BlueZ — use `mesh_bluez_client_mock_enable`. Prefer deterministic fixtures over live radio
calls; tag anything needing hardware `HARDWARE` so CI can skip it. Cover the error paths,
especially BLE reconnection and protobuf parsing, and give any new transport a golden protobuf
frame test. Full guidance in [`docs/testing.md`](docs/testing.md).

## Commit & Pull Request Guidelines

Commits follow [Conventional Commits](https://www.conventionalcommits.org/) — the type decides
the release bump, so it is not cosmetic. Write subjects in imperative mood with an optional scope
(`fix(ble): retry mtu negotiation`), keep bodies wrapped at 72 characters, and mention relevant
Meshtastic issue IDs where they apply. See [`docs/semantic-release.md`](docs/semantic-release.md).

PRs should include a concise summary, validation notes (commands run or hardware tested),
screenshots for UI-facing work, and any follow-up TODOs. Link to the docs in `docs/` when
behaviour changes materially, and request review from both protocol and UI owners for
cross-cutting updates.

## Packaging & Deployment Tips

Check that `launch.sh` stays POSIX-compliant and keeps logging to `.userdata/tg5040/logs/`;
it and the `Tools/` helpers do not ship through self-update, so changes there force a pak
reinstall. After `scripts/package.sh`, sideload into `Tools/tg5040/` (`make deploy` over SSH, see
[`docs/device.md`](docs/device.md)) and verify it starts from the NextUI Launcher before merging.
Never commit user-specific paths or logs — only reproducible assets inside the pak tree.
