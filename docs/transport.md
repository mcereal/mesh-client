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
- nanopb runtime linked against Meshtastic upstream schemas (tracked via git submodule). BLE carries bare protobufs; the serial stream framing lives in `src/proto/stream_framing.c` (the varint helpers in `src/proto/framing.c` are a homegrown scheme that nothing on the wire uses). `make proto` regenerates `mesh`, `portnums`, `interdevice`, `config`, `module_config`, `telemetry`, `channel`, `device_ui`, `xmodem`, and `atak` (the `MESH_PROTO_NAMES` list in `CMakeLists.txt`).
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

### Shipped

- Meshtastic's stream framing (`src/proto/stream_framing.c`): `0x94 0xC3 len_hi len_lo` plus one
  raw protobuf, payload capped at 512 bytes. This is **not** what `src/proto/framing.c` does -
  that is a homegrown varint prefix, used by nothing on the wire - and BLE carries no framing at
  all. The parser is incremental and resync-tolerant, because the firmware interleaves its text
  log with the frames on the same port: anything that is not a well-formed header is handed to a
  text callback (logged at debug as `radio: ...`) and skipped, and a frame split across reads is
  held until the rest arrives.
- USB port discovery (`src/transport/serial/serial_usb.c`): scans `/sys/bus/usb/devices` for
  interfaces already bound to a usb-serial driver (`cp210x`, `ch341`, `ftdi_sio`, `generic`,
  `cdc_acm`) plus unbound CDC-Data interfaces that could be bound, and records the vendor/product
  ids, the CDC control interface number, and whether a tty exists yet.
- The Brick workaround, done by the transport at connect time (see the findings below):
  `mesh_serial_usb_bind()` writes `VID PID` to `/sys/bus/usb-serial/drivers/generic/new_id` and
  waits for the tty, then `mesh_serial_usb_set_line_state()` sends one CDC
  `SET_CONTROL_LINE_STATE` through usbfs. A UART-bridge board skips both and takes a normal
  `TIOCMBIS`.
- The link itself (`src/transport/serial/serial_transport.c`): opens the tty raw at 115200,
  registers the fd with the epoll loop, sends the 32-byte `0xC3` resync burst the Meshtastic
  clients send, waits 100 ms, then attaches `struct mesh_session` and starts the same
  `want_config_id` handshake BLE uses. Outbound `ToRadio` packets are framed into a queue with a
  cursor for partial writes; `EPOLLOUT` is armed only while that queue has a remainder. EOF or a
  fatal read/write error resets the link, marks queued messages FAILED and leaves the message log
  intact, exactly as the BLE link does.
- CLI: `--list-devices` lists USB ports beside BLE advertisers, and `--serial[=ID]` points
  `--status` and `--send-text` at a port instead of a radio over BLE. `--disable-serial` and
  `MESHCLIENT_DISABLE_SERIAL=1` turn the transport off; `MESHCLIENT_PREFERRED_SERIAL_DEVICE`
  picks a port.

### Device findings (Brick, TinaLinux 4.9.191, 2026-09-04)

- The USB-C port is a host port (`sunxi-ohci`) and enumerates a Heltec nRF52840 node
  (`239a:4405`) as CDC-ACM (interface 0 class 02/02, interface 1 class 0a). The kernel has
  `CONFIG_USB_ACM` off with no module, so no `/dev/ttyACM*` ever appears. Only `cp210x`,
  `ch341`, `ftdi_sio` and the generic `usbserial` drivers exist, which cover UART-bridge boards
  (ESP32 dev kits) but not native-USB nodes.
- Writing the vendor/product id to `/sys/bus/usb-serial/drivers/generic/new_id` makes the generic
  driver reject the control interface ("no bulk out") and attach the data interface as
  `/dev/ttyUSB0`.
- The node stays silent until DTR is asserted - TinyUSB discards output while the host has not
  set the line state - and the generic driver cannot set it. One CDC `SET_CONTROL_LINE_STATE`
  (`bmRequestType 0x21, bRequest 0x22, wValue 0x0003, wIndex 0`) against the unbound control
  interface through `/dev/bus/usb/BBB/DDD` (`USBDEVFS_CLAIMINTERFACE` on interface 0, then
  `USBDEVFS_CONTROL`) fixes that. With both in place a `want_config_id` over the tty was answered
  with the full config stream (125 frames in 10 s).
- Neither the binding nor the line state survives a reboot, which is why the transport redoes
  both every time it connects.

### Next

- The app and the UI still reach for `mesh_ble_transport()` directly in about 25 places, so the
  Devices tab lists BLE advertisers only and auto-connect is BLE-only. Lifting that to an
  "active link" the app holds is what makes a USB node selectable on the device.
- Nothing picks a serial port automatically yet; `--serial` and `MESHCLIENT_PREFERRED_SERIAL_DEVICE`
  are the only ways in.

## HTTP Transport

- Not yet implemented. HTTP proxy client will reuse the same protobuf framing, ideally via a small
  reusable packet encoder/decoder module shared with BLE/Serial.
- Consider shipping a websocket forwarding proxy to allow simultaneous consumers of `/fromradio`.

## Testing Strategy

- Unit tests cover config defaults, registry behaviour, event loop scaffolding, and BLE state
  reporting without requiring a live BlueZ stack.
- Future work includes adding integration tests that exercise BlueZ interactions via a mock D-Bus
  daemon and golden protobuf frame fixtures once nanopb is integrated.
