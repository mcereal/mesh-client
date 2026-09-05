# Transports

Meshtastic exposes the same protobuf device API over BLE, Serial and HTTP. This client speaks
`ToRadio`/`FromRadio` once, in `struct mesh_session` (see [`architecture.md`](architecture.md)),
and snaps transports underneath it.

A transport implements `struct mesh_transport_ops {start, stop, status, tick}` plus the optional
`set_session` and `take_error`, and registers with `src/transport/transport_registry.c`. **The
session never sees GATT, ttys or framing; a link never decodes a protobuf.**

| Transport | State |
|---|---|
| BLE (BlueZ over D-Bus) | shipped, the default |
| Serial (USB) | shipped, CLI-selectable |
| HTTP | not implemented |

## Bluetooth LE

### The GATT data path

Meshtastic does **not** use the Nordic UART Service. The service UUID is
`6ba1b218-15a8-461f-9fa8-5dcae273eafd`, with three characteristics: ToRadio (`f75c76d2-…`),
FromRadio (`2c55e69e-…`) and FromNum (`ed9da18c-…`).

Per the [Meshtastic client API](https://meshtastic.org/docs/development/device/client-api/):
`Connect`, wait for `Device1.ServicesResolved`, look up the three characteristics, `StartNotify`
on FromNum, and on each notification `ReadValue` FromRadio until it returns empty.

**One bare protobuf per write/read — there is no length framing on BLE.** BlueZ handles ATT long
writes, so there is no client-side chunking either; packets are capped at 512 bytes. Framing is a
serial-only concern; see `src/proto/stream_framing.c` below.

### `src/transport/ble/bluez_client.c`

A raw libdbus wrapper for `org.bluez`: adapter discovery, `GetManagedObjects`, GATT
Connect/StartNotify/Write. It has a compile-time-independent mock
(`mesh_bluez_client_mock_enable`) that tests use to script results and capture writes — **there
is no real BlueZ in CI, and tests must not touch one.**

Without D-Bus headers at build time the transport compiles out entirely (`MESH_HAVE_DBUS` is set
only if `pkg-config dbus-1` succeeds) and reports `disabled`.

### `src/transport/ble/ble_transport.c`

The link itself: a state machine (`disabled` -> `waiting-for-bluez` -> `waiting-for-adapter` ->
`running`), Meshtastic service UUID filtering, an outbound ToRadio queue (the session's send
path), and the FromNum-notify -> FromRadio-read drain loop. It owns one session
(`mesh_ble_transport_session`); the `mesh_ble_transport_*` send/settings/messages/handshake
functions are thin wrappers over it. `StartDiscovery`/`StopDiscovery` are driven
automatically for the first available adapter, and a timerfd refreshes the discovery cache
(address/name/RSSI) while the app runs.

Connect is **non-blocking**: `mesh_ble_transport_connect` sends `Device1.Connect` and returns 0
with the link in `connecting` (the reply is matched by serial in `bluez_client.c`, 30 s cap), so
a slow or unanswered connect never stalls the UI. `tick()` then waits for `ServicesResolved`
(250 ms polls, 20 s cap) before wiring the characteristics — the GATT database is not on the bus
yet when nothing is cached. `connected_address()` is NULL until then; `status()` reads
`connecting`/`connected` meanwhile. Everything else on the bus (reads, writes, `StartNotify`) is
still a blocking call.

**BlueZ never tells us about a dropped link** (only characteristic properties are watched), so
`tick()` reads `Device1.Connected` every 2 s while CONNECTED (`mesh_ble_transport_check_link`),
and a failed GATT write also resets the link. Queued messages are marked FAILED either way; the
message log survives the reset, and auto-connect reconnects.

### Pairing

Firmware in FIXED/RANDOM PIN mode has to be bonded before `StartNotify` will answer, so the app
does the pairing itself rather than deferring to a system agent.

`bluez_client.c` registers an `org.bluez.Agent1` at `/org/meshclient/agent` with
**KeyboardDisplay** capability. That is what makes a PIN-mode node (its own IO capability is
DisplayOnly) choose passkey entry and ask *us* for the six digits shown on its screen. The
agent's reply is **deferred** — the D-Bus call message is held in `agent_pending_message` until
the user has typed them — which is the whole reason a bond can span several event-loop turns
without blocking. `Device1.Pair` is sent the same non-blocking way `Connect` is, and `Trusted` is
set afterwards so the next connect needs neither the agent nor the PIN.

Two rules keep it predictable:

- **Only a connect the user asked for bonds** (`mesh_ble_transport_connect_and_pair`, from the
  Devices tab). Auto-connect's plain `mesh_ble_transport_connect` raising a PIN prompt over
  whatever the user was doing would be worse than the connect failing.
- **The pairing timeout stops while the agent is holding a question**, or the prompt would cancel
  itself out from under the person reading the node's screen.

`mesh_ble_transport_forget` is `Adapter1.RemoveDevice` — the fix when a node's PIN has changed
under a bond BlueZ still believes in.

## Serial (USB)

### `src/proto/stream_framing.c`

Meshtastic's serial/TCP framing: `0x94 0xC3 len_hi len_lo` plus one raw protobuf, 512-byte cap.

The parser is **incremental and resync-tolerant** because the firmware interleaves its text log
with the frames on the same port: anything that is not a well-formed header goes to a text
callback (logged as `radio: ...` at debug) and is skipped, and a frame split across reads is held
until the rest lands.

### `src/transport/serial/serial_transport.c`

Scans sysfs every 3 s while idle (never while a port is held), binds and asserts DTR, opens the
tty raw at 115200, registers the fd with the epoll loop, sends the 32-byte `0xC3` resync burst
the Meshtastic clients send, waits 100 ms, then attaches the session and runs the same
`want_config_id` handshake BLE uses.

Outbound packets are framed into a queue with a partial-write cursor; `EPOLLOUT` is armed only
while that queue has a remainder. EOF or a fatal read/write error resets the link and marks
queued messages FAILED, leaving the message log intact, exactly as the BLE link does.

### `src/transport/serial/serial_usb.c` — and the Brick workaround

Port discovery scans `/sys/bus/usb/devices` for interfaces already bound to a usb-serial driver
(`cp210x`, `ch341`, `ftdi_sio`, `generic`, `cdc_acm`) plus unbound CDC-Data interfaces that could
be bound, recording vendor/product ids, the CDC control interface number, and whether a tty
exists yet.

**The Brick's kernel has `CONFIG_USB_ACM` off**, so a native-USB node gets no `/dev/ttyACM*`. The
transport works around it at connect time:

1. `mesh_serial_usb_bind()` writes `VID PID` to
   `/sys/bus/usb-serial/drivers/generic/new_id` — the generic driver refuses the control
   interface ("no bulk out") and takes the data one as `/dev/ttyUSB0` — then waits for the tty.
2. `mesh_serial_usb_set_line_state()` sends one CDC `SET_CONTROL_LINE_STATE` through usbfs,
   because the node discards output until DTR is asserted and the generic driver cannot assert
   it.

**Neither survives a reboot, so both happen on every connect.** UART-bridge boards
(cp210x/ch341/ftdi_sio) skip both and take a normal `TIOCMBIS`.

It is mockable (`mesh_serial_usb_mock_enable`) so tests never touch sysfs or usbfs; the mock's
`open_fd` lets a test hand the link one end of a socketpair.

Note the `MESH_IOCTL_REQUEST` shim: glibc's `ioctl` takes `unsigned long`, musl's takes `int`,
and the USBDEVFS codes have the high bit set. The release build is musl and the dev container
glibc, so both need narrowing.

#### Device findings (Brick, TinaLinux 4.9.191, 2026-09-04)

- The USB-C port is a host port (`sunxi-ohci`) and enumerates a Heltec nRF52840 node
  (`239a:4405`) as CDC-ACM (interface 0 class 02/02, interface 1 class 0a). `CONFIG_USB_ACM` is
  off with no module. Only `cp210x`, `ch341`, `ftdi_sio` and the generic `usbserial` drivers
  exist, which cover UART-bridge boards (ESP32 dev kits) but not native-USB nodes.
- The control transfer is `bmRequestType 0x21, bRequest 0x22, wValue 0x0003, wIndex 0` against
  the unbound control interface through `/dev/bus/usb/BBB/DDD`
  (`USBDEVFS_CLAIMINTERFACE` on interface 0, then `USBDEVFS_CONTROL`).
- With both in place, a `want_config_id` over the tty was answered with the full config stream:
  125 frames in 10 s.

A USB node is fully selectable on the device: `mesh_app_publish_ui_state()` prepends serial
ports to the Devices list (`paired = true` — a cable has nothing to bond) so they are the default
cursor row, `MESH_UI_ACTION_CONNECT` routes those rows to `mesh_serial_transport_connect()`, and
`mesh_app_autoconnect()` tries a plugged-in port before any BLE candidate. Forget
(`MESH_UI_ACTION_FORGET`) is the one action still BLE-only, which is correct — there is no bond
to remove.

### Known gaps

- The app and UI still reach for `mesh_ble_transport()` directly in about 30 places. Lifting that
  to an "active link" the app holds would remove a good deal of per-kind branching.

## HTTP

Not implemented. The Meshtastic HTTP API serves the same protobufs under `/api/v1`, so it would
register with the same `mesh_transport_ops` and reuse `mesh_session` unchanged.

One caveat to design around: `/fromradio` allows a single consumer, so sharing a node with
another client would want a small websocket forwarding proxy.
