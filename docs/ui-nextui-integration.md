# Leveraging NextUI for MinUI Helpers

The repository now vendors the upstream NextUI sources as a submodule under
`third_party/nextui/`. This tree gives us access to the actual MinUI codebase and
support binaries that ship on TrimUI/NextUI devices.

## Directory Overview

- `third_party/nextui/workspace/all/` – shared engine code (`common/`,
  `settings/`, `minarch/`, etc.). The files we care about for the Mesh Client UI
  are:
  - `settings/keyboardprompt.cpp` – on-screen keyboard implementation we can
    reuse for composing messages.
  - `settings/menu.cpp` / `menu.hpp` – generic list rendering that powers the
    MinUI menus.
  - `common/` – utilities, API bindings, scaler helpers used by both settings
    and launcher components.
- `third_party/nextui/workspace/tg5040/` – platform-specific glue for the TrimUI
  Brick (`platform/`, `libmsettings/`, `show/`). In particular:
  - `platform/platform.c` contains input, framebuffer, and audio bindings we can
    reuse when embedding MinUI widgets inside Mesh Client.
  - `show/show.c` builds the simple viewer used by NextUI to splash PNG assets;
    we can adapt/extend this as a lightweight presenter fallback.

## Planned Integration Steps

1. **Isolate helper targets**: create focused CMake targets that build the
   keyboard and list widgets from the NextUI sources (rather than compiling
   their entire launcher). We plan to expose two tiny binaries:
   - `minui-presenter` – wraps the existing `keyboardprompt`/`menu` rendering for
     simple status and toast output.
   - `minui-list` – repackages `MenuList` (from `settings/menu.cpp`) to present
     device lists and respond to d-pad selection.
2. **Cross-compile for tg5040**: invoke `scripts/build_minui_helpers.sh` (run
   automatically by `make package`) to bootstrap the NextUI toolchain
   (`make PLATFORM=tg5040`) and drop helper binaries under
   `Tools/tg5040/MeshClient.pak/bin/tg5040/`. When the cross toolchain is not
   available the script still builds lightweight native fallbacks from
   `src/minui_helpers/` so packaging succeeds during host development.
3. **Package assets**: update `scripts/package.sh` so the release pak includes
   the helper binaries and any required shared objects (`libmsettings`, fonts,
   etc.) from the NextUI tree.
4. **Embed keyboard/list flows**: once the helpers exist, wire the MinUI backend
   (`src/ui/backends/minui.c`) to invoke them instead of the current placeholder
   warnings.

## Current Status

- Submodule is present and ready: run `git submodule update --init
  third_party/nextui` after checking out the repo.
- CLI backend remains the default during development until the helper binaries
  are compiled and packaged.
- `scripts/build_minui_helpers.sh` scaffolds the NextUI build and copies the
  resulting helpers into the pak tree. The Makefile’s `package` target invokes
  this script automatically so CI releases include the helpers, while host
  builds fall back to the native helpers in `src/minui_helpers/` when no cross
  compiler is present.
- MinUI backend now serialises discovery/handshake state to JSON, launches the
  helpers asynchronously, and consumes the selected row to trigger BLE
  connections without blocking the event loop. The placeholder helpers in
  `src/minui_helpers/` now emit a compatible selection payload—defaulting to the
  first entry and honouring `MESHCLIENT_MINUI_SELECTION`—so the CLI fallback can
  still request connections when the real MinUI binaries are unavailable.
- The `minui-list` menu now shows a third “Nodes” section populated from the
  cached handshake summaries (node id, names, SNR) so users can inspect the mesh
  roster before deeper UI flows arrive.
- Handshake cache now persists between runs (`~/.meshclient/ui_prefs.handshake`),
  allowing TrimUI builds to render stale-but-useful status immediately after launch.

## Next Actions

- Implement the focused helper build (Step 1) so we can link against the NextUI
  `common/` code without dragging the full launcher.
- Verify the helper binaries on actual hardware and document their runtime
  dependencies under `Tools/tg5040/MeshClient.pak/bin/shared/`.
