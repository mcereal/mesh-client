# Meshtastic for TrimUI Brick (NextUI Pak) — POC & Architecture

A native NextUI/MinUI Pak that turns the TrimUI Brick into a lightweight Meshtastic client. Phase 1 focuses on Bluetooth LE (BLE) connection, device settings, node discovery, and basic messaging—scaffolded so Serial and HTTP transports can be added later.

## Current Status

**Completed (Mar 2025)**

- Repository bootstrapped with C17 core, epoll event loop, and logging utilities.
- BLE transport stub registered; honours config flags and ready for BlueZ wiring.
- Build/test harness in place (`cmake`, `CTest`, Makefile wrappers, packaging scripts, TrimUI `launch.sh`).
- BlueZ presence detection via D-Bus with graceful fallback and adapter discovery / discovery start.
- BlueZ GATT data path with event-loop integration, notification buffering, and basic frame stats.
- Initial config handshake via `want_config_id`, with cached `MyNodeInfo`/`NodeInfo` summaries and completion tracking.
- Outbound BLE write queue with MTU-aware chunking ready for future ToRadio messaging.
- MinUI backend produces JSON device/status menus, launches `minui-list` asynchronously, and funnels the selected row back to the BLE transport for non-blocking connects.
- UI store snapshot (discovery + handshake roster) now persists to disk so framebuffer/CLI backends can show cached status before BLE reconnects; CLI JSON includes `cached` metadata for automation. *(Apr 2025)*
- UI store and CLI backend now surface BLE handshake metadata (MyNode details and node summaries) for downstream UI flows, keeping cached discovery state in sync with BLE updates. *(Apr 2025)*
- Text messaging end to end: `FromRadio.packet` is decoded, `TEXT_MESSAGE_APP` payloads land in a fixed-size message ring, and `Routing` replies settle the delivery state of outbound messages. Sending goes out as a single unframed `ToRadio` GATT write via `--send-text`. Messages reach the UI store (peer names resolved from the NodeDB), render in the framebuffer and CLI backends, and persist alongside the handshake cache so the inbox survives a restart. *(Sep 2026)*

**In Progress / Next Up**

- On-device input: `mesh_ui_input` maps the Brick's buttons and d-pad to logical keys and `src/ui/nav.c` turns them into tab/cursor state and actions (connect, send a canned reply). Compose has a d-pad keyboard, the Messages tab shows one conversation at a time (inbox, a channel, or a node's direct messages), and broadcasts pick their channel from the radio's channel table. Still missing: unread markers, message deletion, and a MinUI-native rendering of the same model.
- Package a `minui-keyboard` helper so text can be entered on the device.
- Add the HTTP transport and end-to-end protocol validation tests. Serial framing now lives in `src/proto/stream_framing.c`; `src/proto/framing.c` is a homegrown varint length prefix that nothing on the wire uses.

## Table of Contents

- [Current Status](#current-status)
- [1. Goals](#1-goals)
- [2. Target Device & OS Assumptions](#2-target-device--os-assumptions)
- [3. Transport Strategy](#3-transport-strategy-pluggable)
- [4. Architecture](#4-architecture)
- [5. UI/UX](#5-uiux-fits-the-brick--nextui)
- [6. Logging & Diagnostics](#6-logging--diagnostics)
- [7. Packaging](#7-packaging-pak-layout--install)
- [8. Build & Toolchain](#8-build--toolchain)
- [9. Meshtastic Protocol Notes](#9-meshtastic-protocol-notes-tldr)
- [10. Security & Pairing](#10-security--pairing)
- [11. Performance & Footprint](#11-performance--footprint)
- [12. Risks & Mitigations](#12-risks--mitigations)
- [13. POC Milestones](#13-poc-milestones-handy-for-codex)
- [14. Repository Deliverables](#14-repository-deliverables)
- [15. References](#15-references)
- [16. License & Attribution](#16-license--attribution)
- [Appendix A — launch.sh (starter)](#appendix-a--launchsh-starter)

## 1) Goals

### POC scope (Phase 1)

- Scan and connect to a nearby Meshtastic node over BLE; authenticate/pair if needed.
- Fetch and display radio info and node list; send a text message to a chosen destination.
- Minimal UI that works with NextUI input model (d‑pad/face buttons) and on‑screen keyboard.
- Ship as a NextUI Pak that installs from Pak Store (or by copying a `.pak` folder).
- No extra runtimes required on‑device; everything bundled in the Pak.

### Phase 2+ (scaffold now)

- Add Serial (USB/UART) and HTTP transports via the same client core.
- Message inbox, delivery/ack status, per‑channel controls, GPS viewing.
- Background service for periodic node sync and notifications.

## 2) Target Device & OS Assumptions

TrimUI Brick (Linux; NextUI/MinUI family). Brick hardware includes Wi‑Fi and Bluetooth 2.1/4.2, Allwinner A133P CPU, and 1 GB RAM—still constrained vs. phones, so we must keep the binary small and avoid heavy deps.

### Pak format (NextUI/MinUI)

- Apps are folders like `/Tools/tg5040/<Name>.pak/` with a `launch.sh` entrypoint.
- Logs go under `/.userdata/$PLATFORM/logs/<pak>.txt`.
- Per‑pak persistent state lives under `/.userdata/$PLATFORM/<pak>/`.
- You can optionally register an on‑boot script via `auto.sh` to run background services.
- Pak Store installs by placing a `.pak` folder under `Tools/tg5040`.
- NextUI/MinUI community paks follow the same patterns and paths (e.g., PortMaster, Terminal).
- Platform key: TrimUI Brick uses `tg5040` as the platform folder name.

## 3) Transport Strategy (pluggable)

Meshtastic exposes the same protobuf device API over BLE, Serial, and HTTP. We’ll implement a single client core that speaks `ToRadio` / `FromRadio` protobuf streams, and snap different transports into it.

### 3.1 BLE (Phase 1)

Meshtastic devices expose a stream via Nordic UART Service (NUS):

- Service UUID: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- TX characteristic (peripheral → central): `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` (NOTIFY/READ)
- RX characteristic (central → peripheral): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` (WRITE)

On connect, send `want_config_id` to receive NodeDB and config on the `fromradio` stream, then continue full‑duplex protobuf exchange.

Implementation:

- Tiny C client using BlueZ D‑Bus GATT (no GLib UI deps) + nanopb for protobuf (small footprint C).
- Non‑blocking I/O, single‑threaded event loop (`epoll`) to keep CPU usage low.
- MTU handling and message framing (Meshtastic frames protobuf messages on the stream).
- Reference semantics also appear in Meshtastic’s Python BLE interface and JS/Deno protobuf docs (useful for NOTIFY consumption behavior).

### 3.2 Serial (Phase 2)

Meshtastic Serial module in PROTO mode exposes the same Client API over UART—drop‑in transport.

### 3.3 HTTP (Phase 2)

The Meshtastic HTTP API serves protobufs under `/api/v1`—great when the node hosts AP/HTTP or via a phone bridge; note that `/fromradio` allows one consumer (work around with a proxy if needed).

## 4) Architecture

```
+-----------------------------------------------+
|                 NextUI Launcher               |
|            (calls Tools/tg5040/*.pak)         |
+-------------------------+---------------------+
                          |
                          v
               /Tools/tg5040/MeshClient.pak
               ├─ launch.sh                (Pak entrypoint; sets env, logging, exec)
               ├─ bin/
               │   ├─ shared/
               │   │   ├─ meshclient       (C binary: core + BLE transport)
               │   │   ├─ minui-list       (UI helper; external dep packaged)
               │   │   ├─ minui-keyboard   (on-screen keyboard; packaged)
               │   │   └─ minui-presenter  (messages/toasts; packaged)
               │   └─ arm64/               (room for arch-specific builds)
               ├─ assets/                   (icons, strings)
               └─ README.txt
```

### Core modules (inside `meshclient`)

- `transport_ble.c` — NUS scan/connect, GATT subscribe, read/write framing.
- `proto.{c,h}` — nanopb‑generated types for `ToRadio`, `FromRadio`, `MeshPacket`, etc. (vendored from Meshtastic protobufs).
- `client.c` — handshake (`want_config_id`), node DB cache, message send/recv, simple mailbox.
- `ui_bridge.c` — small shim to invoke `minui-*(list|presenter|keyboard)` binaries for menus and input.
- `log.c` — rotating logs to `/.userdata/$PLATFORM/logs/meshclient.txt`.
- `config.c` — per‑pak config in `/.userdata/$PLATFORM/MeshClient/` (e.g., last device addr, preferred transport, UI prefs).

### Process model

- Foreground TUI driven by launcher (blocking).
- Optional background service (`bin/shared/on-boot`) later for periodic sync/notifications (user‑controllable via `launch.sh` toggle).

## 5) UI/UX (fits the Brick + NextUI)

- Home: “Connect via Bluetooth” → scan list (device name / RSSI) → connect → status panel (battery, role, channel, time).
- Nodes: list all known nodes (name/short ID/last seen), select to Message.
- Message: opens on‑screen keyboard, sends text → toast for send/ack (use `minui-presenter`).
- Settings: transport (BLE/Serial/HTTP), autoconnect toggle, clear NodeDB, logging level.

See [`docs/ui-strategy.md`](ui-strategy.md) for the detailed UI plan covering the shared state store, backend interface, and TrimUI/desktop front-ends. The scaffolding in `src/ui/` (`mesh_ui_store` + `mesh_ui_controller`) is live: the app loop now pushes BLE discovery/handshake snapshots into the store, the CLI backend renders updates on host builds, and the MinUI backend now streams JSON snapshots to the packaged helpers and reacts to user selections in real time. UI preferences (last connected device) persist under the pak `HOME` via `~/.meshclient/ui_prefs` for autoconnect support.

## 6) Logging & Diagnostics

- All stdout/stderr redirected by `launch.sh` to `/.userdata/$PLATFORM/logs/MeshClient.txt` (rotated to 512 KB / 5 files).
- `--debug` flag in `launch.sh` enables `set -x` and verbose client logs (frame dumps truncated).
- In‑UI “Export logs” copies to `/Saves/MeshClient/logs/` for easy off‑device retrieval.
- Using the standard NextUI logging pattern makes triage from the device straightforward.

## 7) Packaging (Pak layout & install)

- Path: `/Tools/tg5040/MeshClient.pak/`
- Entrypoint: `/Tools/tg5040/MeshClient.pak/launch.sh` with:
  - env scaffolding, `$HOME` → `/.userdata/$PLATFORM/MeshClient`
  - add `bin/shared` and `bin/$arch` to `PATH`
  - run `meshclient` (or present error via `minui-presenter` if missing BLE)
- Distribution: zip the folder as `MeshClient.pak.zip` or publish a `.pakz` for NextUI’s auto‑install.

## 8) Build & Toolchain

- Language: C (C17), nanopb for protobufs → smallest and fastest on A133P with minimal RAM usage.
- BLE: link against BlueZ D‑Bus GATT (no X/GUI). Vendor a tiny D‑Bus client (or use a single‑file wrapper) to keep deps minimal.
- Cross‑compile: `aarch64-linux-gnu-gcc` with static or mostly‑static linking where permissible.

Repo layout:

```
/cmd/meshclient/        # main()
/pkg/transport/ble/
/pkg/meshtastic/proto/  # nanopb-generated from Meshtastic .proto
/pkg/ui/                # shell glue to minui-*
/scripts/               # build.sh, package.sh (create .pak)
/Tools/tg5040/MeshClient.pak/ (output)
```

Protobufs: generate from upstream `meshtastic/protobufs` (pin a commit for stability).

## 9) Meshtastic Protocol Notes (TL;DR)

Device speaks protobuf frames over BLE/Serial/HTTP.

Typical flow:

1. Connect (BLE) → discover NUS characteristics → subscribe to `FromRadio` (NOTIFY).
2. Send `ToRadio` with `want_config_id`.
3. Device streams NodeDB + config via `FromRadio`; app caches.
4. For messaging: craft `MeshPacket` (text), wrap in `ToRadio`, write to RX char.

NodeDB push behavior and single‑consumer caveats on HTTP are documented in upstream resources.

## 10) Security & Pairing

- Use BLE pairing/bonding when supported; cache the device addr and attempt autoconnect on launch.
- Add a “forget device” action that clears the saved addr/bond and restarts scan.
- Some Meshtastic roles (e.g., `ROUTER`) can be sleepy; if BLE updates are flaky, temporarily switch roles via an admin channel when maintaining over BLE.

## 11) Performance & Footprint

- Why C + nanopb? Lowest memory and CPU overhead; avoids bundling interpreters/runtimes.
- Zero‑copy buffers where possible; cap message sizes and truncate large frame dumps in debug.
- No SDL GUI—shell‑driven UI helpers keep binary tiny and battery‑friendly.

## 12) Risks & Mitigations

### BLE stack availability on NextUI builds

- Mitigate by probing D‑Bus and GATT at startup; display a clear error if BlueZ services are missing.
- Provide a Serial fallback early (Phase 2) which tends to be reliable on Brick via USB.

### HTTP single‑consumer for `/fromradio`

- If/when we add HTTP, consider shipping a lightweight WS proxy so multiple clients can observe the stream.

## 13) POC Milestones (handy for Codex)

### Repo bootstrap

- [x] CMake-based build system with `scripts/build.sh` and Makefile wrappers for common tasks.
- [x] Ship `.clang-format` for consistent formatting; upstream nanopb and Meshtastic protobufs vendored as submodules with nanopb sources generated at build time.

### Pak scaffold

- [x] Create `/Tools/tg5040/MeshClient.pak/launch.sh` with logging + env.
- [ ] Vendor `minui-list` / `minui-keyboard` / `minui-presenter` into `bin/shared/`.

### BLE scan/connect

- [x] Device discovery via BlueZ `GetManagedObjects` with Meshtastic UUID filtering, address/name/RSSI cache, and periodic refresh.
- [x] NUS discovery, subscribe to notify, MTU-aware chunked writes via BlueZ D-Bus GATT.
- [x] Basic status screen with device name/RSSI (framebuffer HUD on device, CLI backend on host).
- [x] Wire CLI/MinUI flows to reuse the discovery cache (`--list-devices`, `--status`, MinUI JSON menus).

### Protobuf handshake

- [x] Send `want_config_id`, parse NodeDB/config into cache (persisted to `~/.meshclient/ui_prefs.handshake`).

### Node list UI

- [x] Render nodes with `minui-list` (Nodes section from the handshake cache).
- [ ] Details panel via presenter.

### Send message

- [ ] Open keyboard, build `MeshPacket` text → `ToRadio` write; confirm via ack or timeout.

### Settings & persistence

- [x] Save last device addr and preferred channel in Pak userdata dir (`~/.meshclient/ui_prefs`).
- [ ] Autoconnect flag and log level.

### Packaging & test

- [x] `scripts/package.sh` → `MeshClient.pak.zip` (or `.pakz`).
- [ ] Install via Pak Store or manual copy; validate logs, memory, and battery.

## 14) Repository Deliverables

- `README.md` — contributor/user guide with installation, usage, and development workflow.
- `docs/poc-architecture.md` — this architecture & roadmap reference.
- `/docs/transport.md` — transport capability overview (shipped vs in-progress details).
- `/Tools/tg5040/MeshClient.pak/` — TrimUI pak scaffold (launch script + bins).
- `Makefile` plus `scripts/build.sh`, `scripts/package.sh` — build/package automation.
- `third_party/` — `nanopb` and `nextui` git submodules; MinUI helpers are built from the latter by `scripts/build_minui_helpers.sh`.
- `proto/meshtastic/` — Meshtastic protobuf submodule (currently v2.8.0); regenerate selected nanopb sources via `make proto` after syncing upstream.
- `LICENSE` (BSD‑3 or MIT).

## 15) References

- TrimUI Brick specs (Bluetooth, Wi‑Fi, A133P, 1 GB RAM).
- NextUI / MinUI (Pak‑based CFW) and community pak patterns.
- Pak anatomy, logging paths, on‑boot hooks (`auto.sh`).
- Meshtastic Client API (protobuf over BLE/Serial/HTTP) and HTTP single‑consumer caveat.
- Meshtastic Serial PROTO mode.
- Nordic UART Service (NUS) UUIDs.
- Python BLE interface reference semantics.
- Meshtastic protobufs (authoritative schema, e.g., via Buf).

## 16) License & Attribution

- Respect Meshtastic’s licenses across firmware, protobufs, and client libs.
- Ship third‑party tools (nanopb, minui utilities) with their licenses in `third_party/`.

## Appendix A — `launch.sh` (starter)

```sh
#!/bin/sh
# NextUI/MinUI Pak entrypoint for MeshClient
set -eu

PAK_DIR="$(dirname "$0")"
PAK_NAME="$(basename "$PAK_DIR")"; PAK_NAME="${PAK_NAME%.*}"
PLATFORM="${PLATFORM:-tg5040}"

# Paths provided by NextUI/MinUI conventions
LOGS_PATH="$SDCARD_PATH/.userdata/$PLATFORM/logs"
SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
HOME="$SHARED_USERDATA_PATH/$PAK_NAME"
mkdir -p "$LOGS_PATH" "$HOME"

# Logging
LOG="$LOGS_PATH/$PAK_NAME.txt"
rm -f "$LOG"; exec >>"$LOG" 2>&1
echo "[$(date)] Launching $PAK_NAME"

# PATH for our bundled helpers
export HOME
export PATH="$PAK_DIR/bin/$PLATFORM:$PAK_DIR/bin/shared:$PATH"

# Probe BLE stack (BlueZ D-Bus); show friendly error if missing
if ! busctl --user tree org.bluez >/dev/null 2>&1 && ! busctl tree org.bluez >/dev/null 2>&1; then
  minui-presenter "Bluetooth services not found.\nEnable BLE or install a build with BlueZ.\nYou can try Serial mode in Settings."
  exit 1
fi

# Run the client
exec meshclient "$@"
```

Note: The env variable names `SDCARD_PATH`, `PLATFORM`, etc., follow common NextUI pak patterns; refine to what the launcher exposes in practice.
