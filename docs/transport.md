# Transports

Meshtastic exposes the same protobuf device API over BLE, Serial, TCP and HTTP. This client
speaks `ToRadio`/`FromRadio` once, in `struct mesh_session`, and snaps transports underneath it.

A transport implements `struct mesh_transport_ops {start, stop, status, tick}` plus the optional
`set_protocol` and `take_error`, and registers with `src/transport/transport_registry.c`. **The
session never sees GATT, ttys or framing; a link never decodes a protobuf.** A link talks to the
session only through `struct mesh_protocol` (`mesh/core/protocol.h`) - see
[`protocols.md`](protocols.md) for that seam and what a second protocol would plug into it.

| Transport | State |
|---|---|
| BLE (BlueZ over D-Bus) | shipped, the default |
| Serial (USB) | shipped, CLI-selectable |
| TCP (network) | shipped, configuration-selectable; nothing discovers one |
| HTTP | not implemented |

Serial and TCP are one wire format — `0x94 0xC3`-framed protobufs over a byte stream — and differ
only in how the descriptor is obtained, so the identical half is `src/transport/stream_link.c`.

Most of *that* is inkwell's now. `struct inkwell_stream` owns the descriptor: the bounded read,
the outbound queue with its partial-write cursor, and keeping `EPOLLOUT` armed exactly while
that queue has a remainder — none of which knows what a radio is. What `stream_link.c` still
owns is the three things that do:

| | |
|---|---|
| the frame parser | through the protocol's `struct mesh_stream_framing`; `0x94 0xC3` is Meshtastic's and is spelled only in `src/proto/stream_framing.c` |
| the protocol | frames go to `mesh_protocol_receive()`; a packet dropped unsent comes back as `mesh_protocol_frame_failed()` |
| the queue's two numbers | eight slots of one whole frame each — this client's budget, passed to inkwell as storage rather than declared by it |

The seam reads as a pair: going out, `mesh_stream_link_send()` frames the packet and hands
inkwell the bytes; going in, inkwell hands back whatever arrived and this pushes it at the
parser. Neither direction lets the framing down or the descriptor up.

## How a failure reaches the user

`take_error()` hands back one sentence, once, and the screen that asked shows it. What changed
is where the sentence is *made*.

The network transport records a failure as `struct inkwell_net_failure` — inkwell's
`enum inkwell_net_reason` and a number — and builds no text at all at the moment it fails.
`take_error()` turns the pair into words, through the table in
[`src/i18n/net_reason.c`](../src/i18n/net_reason.c). That table is the whole of this client's
half of the boundary: inkwell reports *why*, and this client decides what to say about it, in
whatever language is in force.

The reason it is worth the indirection is that every link that reaches a host fails in the same
ways. "No such host" used to be in `catalog.def` twice — once under `LINK_TCP_` and once under
`LINK_MQTT_` — and translated twice in every language, because a string id belongs to one caller
and a reason belongs to nobody. Four entries collapsed to one set the day the transports had a
reason to report instead.

The MQTT proxy reports the same way, through its own table in `src/ui/tables/mqtt.c`; what is
left over there is the broker's answer rather than the network's. See
[`docs/mqtt.md`](mqtt.md#how-a-failure-reaches-the-user).

Two things the table settles rather than inkwell:

- **Collapsing.** inkwell keeps a lookup that failed apart from a lookup that timed out, because
  it can tell them apart. This client says one sentence about both, because there is one thing
  for a reader to do about it. That is a decision about words and it belongs with the words.
- **Arity.** `LINK_UNREACHABLE` is `"%.24s: %.20s"` — a subject and the C library's own word for
  the errno. A reason mapped to the wrong entry still produces a non-empty string, so
  `tcp_error_text_matches_the_reason` in `tests/suites/transport_tcp.c` checks the text rather
  than its presence.

The three failures that are **not** any link's — network links switched off, a target only this
link knows the shape of, and a radio that did not answer the handshake once the socket was up —
stay as string ids in the transport. That split is the extraction seam drawn in advance: the
socket half is what eventually moves down to inkwell, and it only ever records a reason.

## Bluetooth LE

Meshtastic does **not** use the Nordic UART Service. The service is
`6ba1b218-15a8-461f-9fa8-5dcae273eafd`, with ToRadio, FromRadio and FromNum characteristics.
Connect, wait for `Device1.ServicesResolved`, `StartNotify` on FromNum, and on each notification
`ReadValue` FromRadio until it returns empty.

**One bare protobuf per write/read — there is no length framing on BLE.** BlueZ handles ATT long
writes, so there is no client-side chunking either; packets cap at 512 bytes. Framing is a
*stream* concern only.

The Bluetooth stack is inkwell's: `inkwell/ble/central.h` is a BLE central that names a node by
its address and a characteristic by an opaque handle, with BlueZ-over-libdbus as its Linux
backend and a mock (`inkwell_ble_mock_enable`) — **there is no real BlueZ in CI and tests must
not touch one.** Which service and characteristics a link uses is the protocol's
`struct mesh_ble_profile` (`src/proto/ble_profile.c`, Meshtastic's is
`mesh_ble_profile_meshtastic`), and `src/transport/ble/ble_gatt.c` looks one up on a radio - see
[`protocols.md`](protocols.md#the-seam). Built without D-Bus headers, inkwell
links a backend that refuses every call and the transport reports `disabled`.

On a Mac the same transport stands on inkwell's CoreBluetooth backend, and three things read
differently there. A node's address is the UUID macOS gives it - stable on that Mac, meaningless
on any other, and never the radio's MAC. Every node lists as `paired`, because macOS bonds on
demand with a dialog of its own and there is no pair step for the client to take. And macOS asks
the user about Bluetooth on behalf of the app that launched the binary - Terminal, from a shell -
with the sentence in the `Info.plist` linked into it (`src/app/Info.plist.in`); until that is
allowed the transport waits in "Bluetooth is starting".

`ble_transport.c` is the link: a state machine (`disabled` → `waiting-for-bluez` →
`waiting-for-adapter` → `running`), service-UUID filtering, an outbound queue, and the
FromNum-notify → FromRadio-read drain (or, for a profile whose notification is the frame, no
drain at all).

- **Bring-up is retried, not assumed.** `mesh_ble_bring_up()` runs from `start()` and then from
  `tick()` every 2 s while not `running`, because `bluetoothd` is not always on the bus when we
  are — the first launch after the Brick wakes routinely beats it there. The same poll watches
  the other direction: a `running` transport whose `org.bluez` name has gone (Bluetooth toggled
  off in NextUI) drops the link and the device list. A bond in flight and the agent registration
  go with it — both belong to the daemon that left, and a stale one answers every later `Pair`
  with `-EBUSY`.
- **Connect is non-blocking.** `Device1.Connect` returns 0 with the link `connecting` and the
  reply matched by serial (30 s cap), so a slow connect never stalls the UI. `tick()` waits for
  `ServicesResolved` (250 ms polls, 20 s cap) before wiring the characteristics; the GATT
  database is not on the bus yet when nothing is cached. FromRadio reads are async too: `-EAGAIN`
  means pending, and a reply or a three-second timeout wakes the drain through its eventfd.
  Writes, property queries and FromNum's `StartNotify` are the same shape - `-EAGAIN` until the
  reply, each with its own deadline - and the device list is read from inkwell's copy of BlueZ's
  object tree, so a refresh costs no round trip to `bluetoothd`.
- **BlueZ never tells us about a dropped link**, so `tick()` reads `Device1.Connected` every 2 s
  while connected, and a failed GATT write also resets. Queued messages are marked FAILED; the
  message log survives and auto-connect reconnects.

### Pairing

Firmware in FIXED/RANDOM PIN mode must be bonded before `StartNotify` will answer, so the app
pairs itself rather than deferring to a system agent. inkwell's BlueZ backend registers an
`org.bluez.Agent1` with **KeyboardDisplay** capability, which is what makes a PIN-mode node (its
own capability is DisplayOnly) choose passkey entry and ask *us* for the six digits on its screen.
The agent's reply is **deferred** — the D-Bus message is held by the backend until the user types
them — which is how a bond spans several event-loop turns without blocking.

- **Only a connect the user asked for bonds** (`mesh_ble_transport_connect_and_pair`).
  Auto-connect raising a PIN prompt over whatever the user was doing is worse than a failed
  connect.
- **The pairing timeout stops while the agent holds a question**, or the prompt cancels itself out
  from under the person reading the node's screen.

`mesh_ble_transport_forget` is `Adapter1.RemoveDevice` — the fix when a node's PIN changed under a
bond BlueZ still believes in.

### Firmware over the air: two different bootloaders

An install over Bluetooth talks to one of two things, chosen by the board's architecture
(`mesh_firmware_architecture_uses_nordic_dfu()`), and they share nothing but the bus:

- **ESP32**: the unified OTA loader (`ble_ota.c`, `firmware_ota.c`). The radio is armed with an
  `ota_request` admin verb, and the loader is a *second peripheral* at the radio's address plus
  one.
- **nRF52**: the Adafruit bootloader's Nordic **Legacy** DFU (`ble_dfu.c`, `firmware_dfu.c`),
  fed the `-ota.zip` beside the UF2. It is armed by a GATT write to the running firmware's own
  `BLEDfu` control point (`1531`), not by an admin verb. That write needs the authenticated bond,
  so a radio paired without its PIN refuses it. The radio then hands its bond to the bootloader,
  which comes up **at the radio's own address** and advertises only to us. So it is connected to
  directly rather than scanned for. An unbonded bootloader moves to plus one, and is looked for
  there too.

What will bite on the nRF52 path:

- **Data writes are whole words.** The bootloader answers a packet whose length is not a multiple
  of four with NOT_SUPPORTED. `mesh_ble_dfu_packet_for_mtu()` floors `mtu - 3` to four and caps it
  at 244.
- **The packet characteristic is write-without-response only.** BlueZ's `WriteValue` with no
  `type` option sends a Write Command for such a characteristic, which is what makes this work on
  the Brick. inkwell's CoreBluetooth and WinRT backends always write with response, so on a Mac
  or a PC `mesh_firmware_nordic_dfu_available()` is false and an nRF52 is offered USB only - the
  first packet would fail after START had already erased the application.
- **The application is erased before the first byte.** From START on, the radio has no firmware
  to go back to. While the bootloader stays up it waits in BLE DFU and the install retries from
  START. A stock Adafruit bootloader that *resets* with no application comes back in USB DFU
  only, and then it takes a cable.
- **Image packets are paced, not sent on BlueZ's answer.** BlueZ answers a Write Command once it
  has queued it, so writing the next packet from that answer is a burst, and a stock bootloader
  answers ten of them with OPERATION_FAILED. A timer on the loop releases one packet every
  `MESHCLIENT_DFU_PACKET_GAP_MS` (10 ms by default). With the receipt wait after every ten, a
  T1000-E took about 7 minutes at ~1.2 KB/s.
- **START answered INVALID_STATE is a stale session** from a broken transfer, not a bad image.
  Nothing clears it over BLE: RESET does, but on a stock bootloader with no application that
  brings it back in USB DFU only. So the install stops and names USB instead.
- **BlueZ keeps the bootloader's services on the radio's record.** The bootloader shares the
  radio's address and bond, so its GATT discovery replaces the Meshtastic service on that one
  record. `mesh_ble_list_meshtastic()` also lists a *bonded* device that shows only `1530`, or
  auto-connect never reaches for the radio again.

## Serial (USB)

`src/proto/stream_framing.c` parses `0x94 0xC3 len_hi len_lo` plus one raw protobuf, 512-byte cap.
The parser is **incremental and resync-tolerant** because the firmware interleaves its text log
with frames on the same port: anything that is not a well-formed header goes to a text callback
and is skipped, and a frame split across reads is held.

`serial_transport.c` scans for ports every 3 s while idle (never while a port is held), binds and
asserts DTR, opens the tty raw at 115200, sends the 32-byte `0xC3` resync burst, waits 100 ms,
then runs the same `want_config_id` handshake BLE uses.

**The ports are inkwell's** (`inkwell/io/serial.h`): the scan, the bind, the usbfs line state,
termios and DTR, and the mock. What stays here is which of those ports is a radio
(`serial_usb.c`) and the rate Meshtastic talks at (`MESH_SERIAL_BAUD`).

On a Mac the scan reads the I/O Registry instead of sysfs, and the id is the callout device (`/dev/cu.usbserial-0001`). None of the Brick workaround below applies there:
macOS ships drivers for CDC-ACM and the common bridge chips, so every port is already bound and
DTR is a plain `TIOCMBIS`.

### The Brick workaround

**The Brick's kernel has `CONFIG_USB_ACM` off**, so a native-USB node gets no `/dev/ttyACM*`.
At connect time:

1. `inkwell_serial_bind()` writes `VID PID` to
   `/sys/bus/usb-serial/drivers/generic/new_id` — the generic driver refuses the control
   interface ("no bulk out") and takes the data one as `/dev/ttyUSB0`. The bind answers
   `-EAGAIN` until the tty is there, so the link sits in `BINDING` and the tick asks again for up
   to a second rather than the connect sleeping on the loop.
2. `inkwell_serial_set_line_state()` sends one CDC `SET_CONTROL_LINE_STATE` through usbfs,
   because the node discards output until DTR is asserted and the generic driver cannot assert it.

**Neither survives a reboot, so both happen on every connect.** UART-bridge boards
(cp210x/ch341/ftdi_sio) skip both and take a normal `TIOCMBIS`.

Note inkwell's `inkwell_ioctl_request_of()`: glibc's `ioctl` takes `unsigned long`, musl's takes `int`, and
the USBDEVFS codes have the high bit set. The release build is musl and the dev container glibc.

### What is on the other end: a radio or a bootloader

The scan reports two structurally different things as `enum inkwell_serial_kind`:

- **a UART bridge** (the V3's CP2102, `10c4:ea60`) — the USB device is *the adapter*, the radio is
  behind a UART where USB cannot see it, so it says nothing about what is wired to it and can
  never itself be a bootloader;
- **a native-USB node** (the T114, `239a:4405`) — its own MCU, which is what the workaround exists
  for, and where the question is real.

For the second kind an Adafruit UF2 bootloader answers structurally: it presents a **mass-storage
Bulk-Only interface (`08/06/50`) beside its CDC pair**, which inkwell reports as `mass_storage`
on a native port. `mesh_serial_device_is_bootloader()` is that one reading.

This is a bug fix before it is groundwork: a bootloader presents the same CDC pair the firmware
did, so every step of a connect succeeds and the handshake is then asked of something that speaks
no protobuf — which draws as a connected radio with the progress bar turning forever.
`mesh_serial_device_is_radio()` is the predicate; the connect refuses a bootloader with `-ENOTSUP`
before binding anything, auto-connect skips one, and the device list still lists it with an
`in bootloader` badge and no `A connect` — a refusal is a row, not silence.

A board behind a bridge chip has **no USB-side bootloader signal at all**: an ESP32 in ROM
download mode leaves the CP2102 unchanged, so that question can only be answered over BLE.

`inkwell_serial_set_sysfs_root()` points the scan at a fixture tree — nothing in the client calls
it outside a test. inkwell's `io_serial` suite reads the kinds off one laid out as the Brick's
was measured, and `firmware_install.c` walks a radio into its bootloader through one. On a Mac it
also switches the scan from the I/O Registry back to sysfs, so those cases run on every host.

## `stream_link.c` — the half both stream links share

`struct mesh_stream_link` owns everything about an *established* byte stream: the descriptor, the
parser, the framed outbound queue with its partial-write cursor, and the `EPOLLOUT` arming that
keeps the loop awake exactly while that queue has a remainder.

It is a **component, not a seam**, and holds no policy: `pump()` and `flush()` report a fatal
error and stop, and the transport that owns the link decides what it means — "port closed" and
"the radio closed the connection" are the same `-ENOTCONN` and two different sentences.

That split is also what keeps the *connecting* socket out of the link: a non-blocking connect
reports by becoming writable, so the TCP transport holds that descriptor itself and hands it over
when the connect completes. A link is never in a state where `pump()` must not be called.

## TCP (network)

An ESP32 with its network module enabled, or `meshtasticd` on anything running Linux. Both listen
on **4403** and speak the framing already parsed, so the transport is a socket and a connect state
machine. The connect is non-blocking, with no wake burst and no settle window unlike a tty — a
completed three-way handshake has a far end that is listening by definition.

Three things it does that the serial link does not:

- **A connect deadline of its own**, five seconds. The kernel's answer to a dead address is a
  couple of minutes of SYN retries, which on a handheld reads as a press that did nothing.
- **A heartbeat**, `ToRadio.heartbeat` every 30 s. The radio drops a client that has gone silent
  and a quiet mesh is the ordinary case, so without it a link that is working perfectly is
  dropped for having nothing to say. One 6-byte frame.
- **`TCP_NODELAY` and keepalive.** The traffic is small request/reply pairs, exactly what Nagle
  delays; keepalive notices the network going away without anybody closing anything.

### A name stays off the loop

**`getaddrinfo()` blocks, and this client is one epoll loop with no threads.** There is no
non-blocking resolver in POSIX (`getaddrinfo_a` starts threads), so the call cannot be made on the
loop at all — for a long time that meant a target had to be a numeric literal and a name was
refused in words.

inkwell's resolver (`inkwell/net/resolve.h`) is the way out. On POSIX, `src/net/resolve.c` forks a
child, lets *it* block in `getaddrinfo()`, and reads one fixed-size record back through the loop.
The child does not exec—there is nothing worth exec'ing, since `getent` is not on the Brick and
busybox's `nslookup` prints something different every version—so it inherits everything this
process has open and must touch none of it before `_exit`. On Windows,
`src/net/resolve_windows.c` starts overlapped `GetAddrInfoExW` and registers its event with the
same loop; cancellation and the five-second deadline remain the resolver's rather than the UI's.

A literal still costs nothing: `inkwell_resolve_literal()` answers `192.168.1.50:4403` and
`[fd00::1]:4403` with the platform parser and no asynchronous request, which is both faster and
what keeps an address behaving exactly as it did. So the link gained a state — `RESOLVING`,
before `CONNECTING`, with no socket yet and nothing for epoll to watch — and a name now fails
later and more usefully:
"no such host" when it resolved to nothing, "could not look up the name" when the lookup itself
did not work. The two are different sentences because they ask the reader for different things.

`AI_ADDRCONFIG`, and the first answer only. A Brick on WiFi usually has no routable IPv6, and an
AAAA record there is an address whose connect can only time out; walking a second A record is not
what fixes a failed connect, and a retry goes back through the whole attempt anyway — which is
also how it picks up a lease that moved. `tcp_transport_connects_by_name`,
`tcp_transport_takes_a_name`, `resolve_*`.

### Typing one: the device list's last row

**A network cannot be scanned**, so there is one row at the end of the list that *is* the network
radio: the configured address, or `Set an address`. **A** connects when there is one and opens the
keyboard when there is not; **Y** opens the keyboard preloaded; **clearing the field and pressing
Done forgets the host**, which is the only way to say "stop trying that address" and why an empty
draft is accepted here where the waypoint keyboard refuses one.

The row is present exactly when the device list holds no network row of its own —
[`src/ui/tables/devices.c`](../src/ui/tables/devices.c) is the single authority, read by the nav for the row
count, by `actions.c` for the keycaps and by the renderer for the row.

**The address is remembered in `network_host`**, not in the `known_devices` list beside it: that
list is what auto-connect ranks a *scan* with, and a host is in no scan. It is written when the
connect is *asked for* rather than when it succeeds — the radio may be off and the WiFi elsewhere,
and it is still the address the user wrote down. What is saved is the address the transport
**adopted**, asked rather than inferred from a return code: the link takes a target only once the
attempt is actually under way — a socket for an address, a forked lookup for a name — and
enumerating the refusal codes missed three of them
(`tcp_refused_target_is_remembered_by_nobody`).

The link that is *already up* gets a row synthesised by `mesh_app_publish_ui_state()` for
"connected, but in nobody's list". For BLE and USB the real row replaces it a moment later; for a
network link there is no discovery to catch up, so it is the steady state. It carries
`MESH_UI_DEVICE_TCP` and that kind is load-bearing: the slot is `memset` to zero and
`MESH_UI_DEVICE_BLE` is `0`, so a row that does not state its kind *is* a Bluetooth radio — 
unstated, the network link drew a Bluetooth disc, reported `0dBm` (an absent RSSI read as the
strongest signal on screen) and offered to forget a bond that was never made.
`tcp_link_is_published_as_a_network_device` pins it.

### Testing against real firmware

`meshtasticd` runs on ordinary Linux, so the handshake, NodeDB sync, admin queue and Store &
Forward can be exercised against real firmware:

```sh
meshtasticd &
./build/debug/meshclient --tcp-host 127.0.0.1 --status
```

`tests/suites/transport_tcp.c` needs none of that — it stands a listener on loopback.

## HTTP

Not implemented. The HTTP API serves the same protobufs under `/api/v1`, so it would register with
the same ops and reuse `mesh_session` unchanged. One caveat: `/fromradio` allows a single
consumer, so sharing a node with another client would want a small websocket proxy.

## Known gap

The app and UI still reach for `mesh_ble_transport()` directly in about 30 places. Lifting that to
an "active link" the app holds would remove a good deal of per-kind branching.
