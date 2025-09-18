# Transport Notes

This document tracks the state of the pluggable transport layer for the TrimUI Mesh Client. Updates
highlight what is shipped today, what is partially complete, and the next milestones across BLE,
Serial, and HTTP transports.

## Bluetooth Low Energy (BLE)

### Shipped

- D-Bus client wrapper that connects to the system bus and verifies the `org.bluez` service.
- BLE transport startup gracefully downgrades when BlueZ is missing or D-Bus support is disabled.
- Status reporting distinguishes between `disabled`, `waiting-for-bluez`, and `running` states.

### In Progress

- Adapter discovery, scanning, and Meshtastic GATT service discovery.
- Protobuf framing via nanopb with MTU-aware chunking.
- Event-loop integration for watch descriptors and notifications.

### Next

- Persist preferred device selection in pak userdata.
- User-visible UI surfaces (scan list, connection feedback, retry loop).

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
