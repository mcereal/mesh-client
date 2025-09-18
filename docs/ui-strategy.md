# UI Strategy for Mesh Client

## Current Snapshot

- The project currently exposes functionality through the CLI entrypoint in `src/main.c`.
- Core scaffolding under `src/ui/` exposes `mesh_ui_store`/`mesh_ui_controller`; a stub backend is still the default.
- BLE discovery and handshake state are now synced into the UI store from the app loop, ready for future backends.
- A lightweight CLI backend renders discovery/handshake summaries on development hosts (`MESHCLIENT_UI_BACKEND=cli`).

## Goals

1. Deliver a responsive UI on low-power TrimUI hardware without bundling large UI frameworks.
2. Keep the UI layer transport-agnostic so BLE, Serial, and HTTP data surface through the same flows.
3. Reuse the existing single-threaded event loop; avoid extra threads where possible.
4. Allow alternative front-ends (TrimUI MinUI helpers, desktop CLI/TUI, automated tests) to plug in with minimal changes.

## Implementation Status (Mar 2025)

- [done] Core scaffolding (`mesh_ui_store`, `mesh_ui_controller`, and the stub backend) now lives under `src/ui/` and is wired into the app lifecycle.
- [done] BLE discovery and handshake data feed directly into the UI store; store/controller unit coverage lives in `tests/test_main.c`.
- [done] CLI backend implemented for host development; backend selection driven by `MESHCLIENT_UI_BACKEND` (defaults to CLI unless MinUI helpers are present).
- [done] Preferences persist the last connected device under `~/.meshclient/ui_prefs` (auto-created via the launch `HOME`).
- [in-progress] Extract the TrimUI MinUI helper binaries from `third_party/nextui/` so the device backend can render real screens (see `docs/ui-nextui-integration.md`).
- [in-progress] Persist additional UI preferences (channel selection, display options) alongside the stored device.

## Architectural Principles

### Separate Core State from Presentation

- Create a `mesh_ui_store` module that owns the user-facing state (connection status, discovery list, node table, message drafts/acks).
- Transport modules push updates into the store via lightweight events (`mesh_ui_store_on_device_discovered`, `mesh_ui_store_on_handshake_update`, etc.).
- The store emits change notifications through a lock-free queue or eventfd that integrates with the existing event loop.

### Define a Backend-Agnostic UI Facade

- Introduce an interface in `include/mesh/ui/backend.h` exposing callbacks for screen transitions and input events.
- The UI controller (`mesh_ui_controller`) drives the store and delegates rendering to a backend implementation selected at runtime.
- Backends are kept in separate translation units under `src/ui/backends/` (e.g., `nextui_minui.c`, `cli_debug.c`).

### Embrace Small, Composable Helpers for TrimUI

- For the TrimUI/NextUI target, shell out to bundled MinUI helpers (`minui-list`, `minui-presenter`, `minui-keyboard`) via a thin C shim.
- Limit each screen to short-lived helper invocations; cache expensive data (node list, status) in the store to avoid recompute on every redraw.
- Provide a non-blocking command runner that feeds helper output back into the controller without stalling the BLE writer.

### Keep Layout and Input Logic Data-Driven

- Represent screens declaratively (e.g., structs describing menus, actions, and validation) so different backends can render the same model appropriately.
- Avoid embedding TrimUI keycodes directly in business logic; use symbolic actions (`UI_ACTION_CONFIRM`, `UI_ACTION_NEXT`) mapped by each backend.

## Recommended Implementation Plan

1. **Scaffold UI modules** *(done)*
   - `include/mesh/ui/{backend.h,controller.h,store.h}` created with implementations under `src/ui/`.
   - `mesh_app` initialises the UI store/controller with the stub backend and registers it with the event loop.
2. **State propagation** *(in progress)*
   - [done] BLE discovery and handshake updates publish into the store from `mesh_app`.
   - [todo] Provide serialization helpers so the store can persist minimal UI preferences (`preferred_device`, last channel) alongside existing config.
3. **NextUI backend**
   - Vendor MinUI helper binaries under `bin/shared/` (built from `third_party/nextui/`; see `docs/ui-nextui-integration.md`) and ship a wrapper that spawns them with well-defined JSON contracts.
   - Flesh out screen flows: home/status, device picker, node list, compose message, settings dialog.
   - Replace the placeholder presenter callouts with real MinUI invocations once helpers land.
4. **Testing** *(ongoing)*
   - Extend unit tests with transport-driven scenarios and backend contract checks as new pieces land.

## Backend Selection

- Use `MESHCLIENT_UI_BACKEND=cli|minui|stub|auto` to force a specific renderer (defaults to auto).
- `auto` prefers the MinUI backend when helpers (`minui-presenter`, `minui-list`, `minui-keyboard`) are on `PATH`, otherwise falls back to the CLI view.
- TrimUI builds should package MinUI helpers and set `MESHCLIENT_UI_BACKEND=minui` in the launch script once the backend is fully wired.
- Override helper binaries via `MESHCLIENT_MINUI_PRESENTER_CMD` / `MESHCLIENT_MINUI_LIST_CMD` to point at bundled scripts when packaging.
- Placeholder shell scripts live under `Tools/tg5040/MeshClient.pak/bin/shared/` for host development; device builds receive compiled helpers under `bin/tg5040/` from `scripts/build_minui_helpers.sh`.
- `make package` orchestrates helper builds via `scripts/build_minui_helpers.sh`; set `CROSS_COMPILE`/`PLATFORM` as needed in CI before calling it.

## Performance Considerations

- Reuse the existing epoll loop by registering an `eventfd` from the store; UI updates run on the main thread without busy-waiting.
- Batch transport events when possible (e.g., coalesce discovery updates) before notifying the UI to reduce redraw churn.
- Limit MinUI subprocess lifetimes and prefer incremental updates (e.g., update list entries via stdin) to avoid constant process respawns.

## Platform Agnosticism

- Backends implement `struct mesh_ui_backend` with function pointers for `show_screen`, `prompt_text`, `show_toast`, and `handle_input`.
- TrimUI builds select the MinUI backend at runtime using an environment flag or build flag; desktop builds default to the CLI backend.
- New platforms (e.g., SDL mock, web UI) only implement the backend interface, leaving controller and store untouched.

## Documentation & Packaging

- Document backend expectations in `docs/poc-architecture.md` and bundle MinUI helper versions in `bin/shared/` with licenses under `third_party/`.
- Update `scripts/package.sh` to include backend assets and verify executable permissions inside the pak.

Following this strategy keeps the UI layer lightweight, testable, and replaceable while fitting within the constraints of the TrimUI Brick and future transports.
