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
- Meshtastic node discovery: iterates `org.bluez.Device1` entries, filters on the NUS UUID, and caches address/name/RSSI for downstream UI use (mockable for tests).
- Periodic refresh loop (timerfd) keeps the discovery cache up to date while the app is running.
- CLI support: `meshclient --list-devices` prints the cached nodes and exits—useful for diagnostics.
- Varint-based framing helpers for BLE packets, plus nanopb runtime linked against Meshtastic upstream schemas (tracked via git submodule).
- GATT data path: device `Connect`/`Disconnect`, `StartNotify`, and notification handling wired into the event loop with frame buffering and basic stats.
- Initial config handshake: queues `want_config_id`, tracks `MyNodeInfo` / `NodeInfo` summaries, and marks completion via `config_complete_id`.
- Outbound write queue with MTU-aware chunking ensures large protobuf frames are split across BLE packets.

### In Progress

- UI integration for the discovery cache (planned via MinUI resources).
- Automate protobuf regeneration (`make proto`) after updating the Meshtastic protobuf submodule; currently we build `mesh.proto`, `portnums.proto`, `interdevice.proto`, `module_config.proto`, `telemetry.proto`.

### Next

- Persist preferred device selection in pak userdata.
- User-visible UI surfaces (scan list, connection feedback, retry loop).
- Promote cached config/node data to UI and drive follow-on ToRadio commands (message send, settings writes).

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
