# Transport Notes

This document tracks the state of the pluggable transport layer for the TrimUI Mesh Client. Updates
highlight what is shipped today, what is partially complete, and the next milestones across BLE,
Serial, and HTTP transports.

## Bluetooth Low Energy (BLE)

### Shipped

- D-Bus client wrapper that connects to the system bus and verifies the `org.bluez` service.
- BLE transport startup gracefully downgrades when BlueZ is missing or D-Bus support is disabled.
- Status reporting distinguishes between `disabled`, `waiting-for-bluez`, `waiting-for-adapter`, and `running` states.
- Adapter discovery via `GetManagedObjects`, plus automatic `StartDiscovery` / `StopDiscovery` orchestration for the first available adapter.
- Meshtastic node discovery: iterates `org.bluez.Device1` entries, filters on the Meshtastic service UUID `6ba1b218-15a8-461f-9fa8-5dcae273eafd` (Meshtastic does **not** use the Nordic UART Service), and caches address/name/RSSI for downstream UI use (mockable for tests).
- Periodic refresh loop (timerfd) keeps the discovery cache up to date while the app is running.
- CLI support: `meshclient --list-devices` prints the cached nodes and exits—useful for diagnostics.
- nanopb runtime linked against Meshtastic upstream schemas (tracked via git submodule). The varint framing helpers in `src/proto/framing.c` are for the serial/TCP stream transports; BLE carries bare protobufs. `make proto` regenerates `mesh`, `portnums`, `interdevice`, `config`, `module_config`, `telemetry`, `channel`, `device_ui`, `xmodem`, and `atak` (the `MESH_PROTO_NAMES` list in `CMakeLists.txt`).
- GATT data path per the Meshtastic client API: `Connect`, look up ToRadio (`f75c76d2-…`), FromRadio (`2c55e69e-…`) and FromNum (`ed9da18c-…`), `StartNotify` on FromNum, and on each notification `ReadValue` FromRadio until it returns empty. One protobuf per write/read, no length framing. Firmware in FIXED/RANDOM PIN mode requires the node to be paired with BlueZ first (`bluetoothctl pair`); the app does not yet register a pairing agent.
- Initial config handshake: queues `want_config_id`, tracks `MyNodeInfo` / `NodeInfo` summaries, and marks completion via `config_complete_id`.
- Outbound write queue of whole ToRadio packets (up to 512 bytes each); BlueZ handles ATT long writes, so no client-side chunking.
- CLI `--status` surface handshake data (text/JSON) and `--status-output` writes a JSON cache for MinUI scripts.
- MinUI backend consumes the discovery snapshot via JSON, renders it with `minui-list`, and feeds selections back into the BLE transport to trigger connects without blocking the loop.

### In Progress

- Persist richer UI preferences (channel selection, display options) alongside the discovery cache so reconnect flows remain sticky across sessions.

### Next

- Expand MinUI flows beyond device selection (node list, message compose, status toasts) using the same JSON contract.
- Promote cached config/node data to UI and drive follow-on ToRadio commands (message send, settings writes).
- Harden reconnect logic (automatic retries, failure toasts) now that the UI can initiate connects.

## Serial Transport

- Not yet implemented. Next step is to expose a `serial` transport stub mirroring the BLE
  registration pattern and mapping to the Meshtastic PROTO UART mode.
- Investigate using `termios`-backed polling with the existing event loop.

## HTTP Transport

- Not yet implemented. HTTP proxy client will reuse the same protobuf framing, ideally via a small
  reusable packet encoder/decoder module shared with BLE/Serial.
- Consider shipping a websocket forwarding proxy to allow simultaneous consumers of `/fromradio`.

## Testing Strategy

- Unit tests cover config defaults, registry behaviour, event loop scaffolding, and BLE state
  reporting without requiring a live BlueZ stack.
- Future work includes adding integration tests that exercise BlueZ interactions via a mock D-Bus
  daemon and golden protobuf frame fixtures once nanopb is integrated.
