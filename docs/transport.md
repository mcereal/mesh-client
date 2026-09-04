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
- GATT data path per the Meshtastic client API: `Connect` (sent asynchronously; the reply is matched by serial so a slow or unanswered connect never stalls the UI), wait for `Device1.ServicesResolved` (polled every 250 ms, 20 s cap; the link reports `connecting` meanwhile), look up ToRadio (`f75c76d2-…`), FromRadio (`2c55e69e-…`) and FromNum (`ed9da18c-…`), `StartNotify` on FromNum, and on each notification `ReadValue` FromRadio until it returns empty. One protobuf per write/read, no length framing. Firmware in FIXED/RANDOM PIN mode requires the node to be paired with BlueZ first (`bluetoothctl pair`); the app does not yet register a pairing agent.
- Initial config handshake: queues `want_config_id`, tracks `MyNodeInfo` / `NodeInfo` summaries, and marks completion via `config_complete_id`.
- Outbound write queue of whole ToRadio packets (up to 512 bytes each); BlueZ handles ATT long writes, so no client-side chunking.
- CLI `--status` surface handshake data (text/JSON) and `--status-output` writes a JSON cache for MinUI scripts.
- MinUI backend consumes the discovery snapshot via JSON, renders it with `minui-list`, and feeds selections back into the BLE transport to trigger connects without blocking the loop.

### In Progress

- Persist richer UI preferences (channel selection, display options) alongside the discovery cache so reconnect flows remain sticky across sessions.

### Next

- Expand MinUI flows beyond device selection (node list, message compose, status toasts) using the same JSON contract.
- Promote cached config/node data to UI and drive follow-on ToRadio commands (message send, settings writes).
- Failure toasts and a reconnect indicator in the HUD; the foreground loop already auto-connects (preferred node, else strongest, with exponential backoff - `mesh_app_autoconnect()`).

## Serial Transport

- Not yet implemented. The protocol side is ready: `struct mesh_session` (`src/core/session.c`)
  is link-agnostic, and `src/proto/framing.c` has the `0x94 0xC3 len_hi len_lo` stream framing.
  The firmware interleaves its text log with the frames on the same port, so the parser must
  resync on junk between frames.
- Findings from the Brick (TinaLinux 4.9.191, 2026-09-04): the USB-C port is a host port
  (`sunxi-ohci`) and enumerates a Heltec nRF52840 node (`239a:4405`) as CDC-ACM (interface 0
  class 02/02, interface 1 class 0a). The kernel has `CONFIG_USB_ACM` off with no module, so no
  `/dev/ttyACM*` ever appears. Only `cp210x`, `ch341`, `ftdi_sio` and the generic `usbserial`
  drivers exist, which cover UART-bridge boards (ESP32 dev kits) but not native-USB nodes.
- Workaround that was verified end to end: write the vendor/product id to
  `/sys/bus/usb-serial/drivers/generic/new_id`; the generic driver rejects the control
  interface ("no bulk out") and attaches the data interface as `/dev/ttyUSB0`. The node stays
  silent until DTR is asserted (TinyUSB discards output while the host has not set the line
  state), and the generic driver cannot do that, so send one CDC `SET_CONTROL_LINE_STATE`
  (`bmRequestType 0x21, bRequest 0x22, wValue 0x0003, wIndex 0`) to the unbound control interface
  through `/dev/bus/usb/BBB/DDD` (`USBDEVFS_CLAIMINTERFACE` on interface 0, then
  `USBDEVFS_CONTROL`). With that, a `want_config_id` over the tty was answered with the full
  config stream (125 frames in 10 s). The binding and line state are lost at reboot, so the
  transport does both at start.
- Plan: a `serial` link mirroring the BLE registration pattern - sysfs scan for CDC devices
  without a driver, `new_id` + DTR, open the tty raw (baud is meaningless over USB CDC; 115200
  for UART bridges), register the fd with the epoll loop, frame parser in, framed writes out,
  the same `mesh_session` attach/detach as BLE. The Devices tab then lists USB nodes beside BLE
  nodes and picking one disconnects the other.

## HTTP Transport

- Not yet implemented. HTTP proxy client will reuse the same protobuf framing, ideally via a small
  reusable packet encoder/decoder module shared with BLE/Serial.
- Consider shipping a websocket forwarding proxy to allow simultaneous consumers of `/fromradio`.

## Testing Strategy

- Unit tests cover config defaults, registry behaviour, event loop scaffolding, and BLE state
  reporting without requiring a live BlueZ stack.
- Future work includes adding integration tests that exercise BlueZ interactions via a mock D-Bus
  daemon and golden protobuf frame fixtures once nanopb is integrated.
