# Transports

Meshtastic exposes the same protobuf device API over BLE, Serial, TCP and HTTP. This client speaks
`ToRadio`/`FromRadio` once, in `struct mesh_session` (see [`architecture.md`](architecture.md)),
and snaps transports underneath it.

A transport implements `struct mesh_transport_ops {start, stop, status, tick}` plus the optional
`set_session` and `take_error`, and registers with `src/transport/transport_registry.c`. **The
session never sees GATT, ttys or framing; a link never decodes a protobuf.**

| Transport | State |
|---|---|
| BLE (BlueZ over D-Bus) | shipped, the default |
| Serial (USB) | shipped, CLI-selectable |
| TCP (network) | shipped, configuration-selectable; no Devices row to *find* one yet |
| HTTP | not implemented |

Two of those four are one wire format. The serial and TCP APIs both carry
`0x94 0xC3`-framed protobufs over a byte stream, and the only thing that differs is how the file
descriptor is obtained - so the identical half lives in **`src/transport/stream_link.c`** and
each transport writes only its own way of getting connected. See
[`struct mesh_stream_link`](#srctransportstream_linkc--the-half-both-stream-links-share).

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

**Bring-up is retried, not assumed.** `mesh_ble_bring_up()` — readiness check, adapter,
pairing agent, `StartDiscovery` — runs from `start()` and then from `tick()` every 2 s for as
long as the transport is not `running`, because `bluetoothd` is not always on the bus when we
are: the first launch after the Brick wakes from sleep routinely beats it there, and before
this the transport sat in `waiting-for-bluez` until the user quit and relaunched. The same poll
watches the other direction — a `running` transport whose `org.bluez` name has gone (Bluetooth
toggled off in NextUI) drops the link, the adapter path and the device list and goes back to
`waiting-for-bluez`. A bond in flight is cancelled and the pairing agent registration is
dropped as part of that: both belong to the daemon that left, and a stale one would answer
every later `Pair` with `-EBUSY` or leave the new daemon with no agent at all. Device enumeration only happens while `running`, so the list never
outlives the BlueZ that produced it. The reason is logged when it changes rather than on every
retry.

Connect is **non-blocking**: `mesh_ble_transport_connect` sends `Device1.Connect` and returns 0
with the link in `connecting` (the reply is matched by serial in `bluez_client.c`, 30 s cap), so
a slow or unanswered connect never stalls the UI. `tick()` then waits for `ServicesResolved`
(250 ms polls, 20 s cap) before wiring the characteristics — the GATT database is not on the bus
yet when nothing is cached. `connected_address()` is NULL until then; `status()` reads
`connecting`/`connected` meanwhile. FromRadio reads also complete asynchronously: `-EAGAIN`
means pending, and a reply or three-second timeout wakes the drain through its eventfd. Reads
remain sequential; disconnect cancels the pending request and late replies are ignored.
Writes, property queries and `StartNotify` still block. See [performance.md](performance.md)
for the tradeoffs and isolated D-Bus test.

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

#### What is on the other end: `enum mesh_serial_device_role`

The scan matches two structurally different things, and only one of them can ever be a
bootloader:

- **a UART bridge** — the V3's CP2102 is `10c4:ea60`, one vendor-class interface (`ff/00/00`)
  that `cp210x` claims. The USB device is *the adapter*; the radio is behind a UART where USB
  cannot see it, so the bridge says nothing about what is wired to it and can never itself be a
  bootloader;
- **a native-USB node** — the T114 is its own MCU (`239a:4405`, a CDC pair, no driver until we
  bind one), which is what the workaround above exists for. Here the question is real.

For the second kind the device answers structurally: an Adafruit UF2 bootloader presents a
**mass-storage Bulk-Only interface (`08/06/50`) beside its CDC pair**, and that sibling is the
whole tell. `read_device_facts()` reads it in the same walk that finds the CDC control
interface — one reading of one device, because two walks are how two answers come to disagree
about which device they were reading.

**This is a bug fix before it is groundwork.** A bootloader presents the same CDC pair the
firmware did, so without the role every step of a connect succeeds — the bind takes, the tty
appears, DTR goes out — and the handshake is then asked of something that speaks no protobuf.
Measured on 2026-09-10: double-tapping a T114's reset with the client running had it bind
`239a:0071` itself, auto-connect, and request a config sync that nothing would ever answer,
which draws as a connected radio with the progress bar turning forever.

`mesh_serial_device_is_radio()` is the predicate; `mesh_serial_transport_connect()` refuses a
bootloader with `-ENOTSUP` before it binds anything, and `mesh_app_autoconnect()` skips one when
choosing a port so it does not attempt the same refusal on every retry. The Devices tab still
lists it, carrying an `in bootloader` badge — a refusal is a row, not silence — and the action
bar drops `A connect` there, because the bar and the press ask one function
(`mesh_ui_device_connectable()`). Phase 3 of
[`radio-firmware-roadmap.md`](radio-firmware-roadmap.md) reads the same role in the affirmative
to answer "has the board come back yet".

A note for the ESP32 half of that roadmap: a board behind a bridge chip has **no USB-side
bootloader signal at all** — an ESP32 in ROM download mode leaves the CP2102 unchanged — so that
question can only be answered over BLE.

`MESHCLIENT_SYSFS_USB` overrides the sysfs root. Nothing in the client sets it; it exists so the
role reading can be tested against a fixture tree laid out as the Brick's sysfs actually was,
which the mock cannot do because it replaces the scan whole.

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

## `src/transport/stream_link.c` — the half both stream links share

`struct mesh_stream_link` owns everything about an *established* byte stream: the descriptor, the
frame parser, the framed outbound queue with its partial-write cursor, and the `EPOLLOUT` arming
that keeps the loop awake exactly while that queue has a remainder. Both the serial and the TCP
transport own one.

It is a **component, not a seam**. It holds no policy: it decides nothing about when to connect,
and it never resets itself. `pump()` and `flush()` report a fatal error and stop; the transport
that owns the link decides what that means and says so in its own words — "port closed" and "the
radio closed the connection" are the same `-ENOTCONN` and two different sentences, and only the
transport knows which it is.

That split is also what keeps the *connecting* socket out of the link. A non-blocking connect
reports by becoming **writable**, so it is watched with `EPOLLOUT` and reading it would be
meaningless; the TCP transport therefore holds that descriptor itself and hands it over at the
moment the connect completes. A link is never in a state where `pump()` must not be called.

## TCP (network)

### `src/transport/tcp/tcp_transport.c`

A node reached over the network: an ESP32 with its network module enabled, or `meshtasticd` on
anything that runs Linux. Both listen on **4403**, and both speak the same framing
`src/proto/stream_framing.c` already parses — so the transport is a socket, a connect state
machine and nothing else.

The connect is non-blocking: `socket(… | SOCK_NONBLOCK)`, `connect()`, and either it completes
immediately (usual on loopback) or returns `EINPROGRESS` and the loop finishes it. There is no
wake burst and no settle window, unlike a tty — a socket that has completed a three-way handshake
has a far end that is listening by definition, and nothing was on the stream before us to leave a
parser mid-frame. The handshake goes out the moment the connect lands.

Three things it does that the serial link does not:

- **A connect deadline of its own.** The kernel's answer to a dead address is a couple of minutes
  of SYN retries, which on a handheld reads as a press that did nothing. Five seconds, then the
  attempt fails and says so.
- **A heartbeat.** `ToRadio.heartbeat` every 30 s while the link is up
  (`mesh_session_send_heartbeat()`). The radio drops a client that has gone silent and a quiet
  mesh is the ordinary case, so without it a link that is working perfectly is dropped for having
  nothing to say. It costs one 6-byte frame.
- **`TCP_NODELAY` and keepalive.** Meshtastic's traffic is small request/reply pairs, which is
  exactly what Nagle delays. Keepalive is the other half of the heartbeat's problem: a heartbeat
  proves the radio still wants us, and keepalive is what notices the network went away without
  anybody closing anything — a Brick carried out of WiFi range.

### An address, not a name

**`getaddrinfo()` blocks, and this client is one epoll loop with no threads in it.** A DNS lookup
that takes five seconds is five seconds of frozen UI, and there is no non-blocking resolver in
POSIX: `getaddrinfo_a` is a glibc extension that starts threads. So a target here is a numeric
literal — `192.168.1.50`, `192.168.1.50:4403`, `[fd00::1]:4403` — and a name is refused in words
(`use an IP address, not a name`) rather than paid for in a stall.

This is a deliberate limitation and not a permanent one. The shape that would lift it is the one
[`src/core/fetch.c`](../src/core/fetch.c) already uses for HTTPS: fork a child, let it block, read
the answer back through the loop. That is its own piece of work and it belongs with the on-device
way of *typing* an address, which does not exist yet either.

### What it does not have yet

**A way to *find* a node in the Devices tab.** A network cannot be scanned the way a USB bus or
a Bluetooth adapter can — there is no equivalent of a sysfs walk or an advertisement — so the
only thing this link could list is the address somebody already wrote down, and on a handheld
there is nowhere to write one. Until both halves exist, the link is reached from configuration:
`--tcp-host`, `MESHCLIENT_TCP_HOST`, or `preferred_tcp_host`. See [`cli.md`](cli.md).

The link that is *already up* does get a row, and it is worth knowing why, because it is not a
row anybody added. `mesh_app_publish_ui_state()` has always synthesised one for "connected, but
in nobody's list" — for BLE and USB that is a connect which beat its own discovery, and the real
row replaces it a moment later. For a network link there is no discovery to catch up, so the
synthesised row is the steady state and the only row a TCP link will ever have. It carries
`MESH_UI_DEVICE_TCP`, is named by its address, says `network` where a Bluetooth row says its
RSSI, and offers `X disconnect` and no `Y forget` — there is no bond behind it to forget.

That kind is load-bearing rather than decorative. The slot is `memset` to zero and
`MESH_UI_DEVICE_BLE` is `0`, so a row that does not state its kind *is* a Bluetooth radio, and a
renderer asks the field three separate questions. Unstated, the network link drew a Bluetooth
disc, reported `0dBm` — an absent RSSI read as a number, which is the strongest signal on the
screen and the one reading this client refuses everywhere else — and offered to forget a bond
that was never made. It also counted toward the Status card's "*n* in range", which means
earshot by that field. `tcp_link_is_published_as_a_network_device` pins all four.

Everything else in the app already knows about it: `mesh_app_active_transport()`,
`mesh_app_connected_identifier()` and `mesh_app_link_connecting()` all count a TCP link, so the
status line under the keycaps names it — by its address, since a network host has no
advertisement coming and so never earns the placeholder name a radio wears until one does — and
auto-connect will not talk over it.

### Where it sits in auto-connect

After a cable and before the air. A configured host needs no pairing and has no range to lose,
and unlike anything on a scan it is a place somebody deliberately wrote down — but it is also the
one candidate that can be absent without being *gone*, since an address stays written down with
the WiFi off. So the network arm has a retry stamp of its own
(`autoconnect_tcp_retry_at_ms`, 30 s): without it that arm runs first on every turn, fails five
seconds later on its connect deadline, and Bluetooth is never reached at all.

With no host configured — the default — none of this runs and auto-connect behaves exactly as it
did before the link existed.

### Testing it against real firmware

This is the transport's other reason to exist. `meshtasticd` runs on ordinary Linux, so the
handshake, the NodeDB sync, the admin queue and Store & Forward can be exercised against *real
firmware* rather than against a mock:

```sh
meshtasticd &                                   # or a container
./build/debug/meshclient --tcp-host 127.0.0.1 --status
```

`tests/suites/transport_tcp.c` needs none of that — it stands a listener on the loopback and
scripts a radio into it, so the suite has no external dependency.

## HTTP

Not implemented. The Meshtastic HTTP API serves the same protobufs under `/api/v1`, so it would
register with the same `mesh_transport_ops` and reuse `mesh_session` unchanged.

One caveat to design around: `/fromradio` allows a single consumer, so sharing a node with
another client would want a small websocket forwarding proxy.
