# Architecture

A C17 Meshtastic client for the TrimUI Brick (NextUI/MinUI, platform key `tg5040`), shipped as a
`MeshClient.pak`. Single-threaded epoll event loop, pluggable transports, nanopb for the
Meshtastic protobufs.

This document is the "why it is like this" reference. The transports have their own page
([`transport.md`](transport.md)), as does the UI layer ([`ui.md`](ui.md)); the radio-settings
admin protocol is in [`settings-roadmap.md`](settings-roadmap.md) and releasing in
[`semantic-release.md`](semantic-release.md).

## Data flow

Data flows one direction:

```
link (transport) -> mesh_session -> mesh_app -> UI store -> controller -> backend
```

Input goes the other way: evdev -> `mesh_ui_input` -> controller -> `nav.c` -> a
`mesh_ui_action` -> `mesh_app_on_ui_action`.

## Target device

TrimUI Brick running NextUI (platform key `tg5040`): Allwinner A133P, 1 GB RAM, WiFi and
Bluetooth. Constrained enough that the binary stays small and brings no runtime with it — C plus
nanopb, no SDL, no interpreter.

Pak conventions the code depends on:

- A pak is a folder `/Tools/tg5040/<Name>.pak/` with a `launch.sh` entrypoint.
- Logs go to `/.userdata/$PLATFORM/logs/<pak>.txt`.
- Per-pak state lives under `/.userdata/$PLATFORM/<pak>/`, which `launch.sh` sets as `$HOME`.

## Core

### `src/core/event_loop.c`

An epoll loop over a fixed table of 32 fd sources. Everything registers here: D-Bus watches, the
timerfd discovery refresh, the UI store eventfd, the serial tty, `minui-list` child stdout.

**No threads anywhere. Do not add them.**

### `src/transport/transport_registry.c`

`struct mesh_transport_ops {start, stop, status, tick}` plus the optional `set_session` and
`take_error`. BLE and serial are registered today; HTTP is meant to plug in here. Both links own
a `struct mesh_session`, so everything past the connect is shared.

### `src/core/session.c` — the Meshtastic conversation

`struct mesh_session` is the conversation, independent of how the bytes travel. It owns the
`want_config_id` handshake (tracking `MyNodeInfo` and `NodeInfo` summaries, completion marked by
`config_complete_id`), the node-summary cache, the radio's channel table, the message log,
the radio settings and admin queue pump, and packet ids.

A link calls `mesh_session_attach(send_fn)` when its connection is usable, hands every FromRadio
protobuf to `mesh_session_handle_from_radio`, calls `mesh_session_tick` each turn, and
`mesh_session_detach`es when the link drops (handshake and settings reset, messages survive).
**The session never sees GATT, ttys or framing; a link never decodes a protobuf.**

#### The node cache

`struct mesh_node_summary` is the whole node record, not just a name: identity from
`NodeInfo.user` (id, hw model, role, public key, licensed/unmessagable), the NodeDB flags, and
`position`/`metrics`/`environment` sub-structs. 256 entries, decoded from `FromRadio`; every
inbound `MeshPacket` also refreshes its sender's `last_heard`/SNR/hops, adding the sender by id
if the sync never delivered its NodeInfo (`mesh_session_apply_packet_details`).

The firmware **replays its NodeDB exactly once per connection**. That single fact shapes the
rest:

- `NODEINFO_APP`, `POSITION_APP` and `TELEMETRY_APP` packets are the only reason a node that
  joins mid-session ever gets a name, and the only source of environment telemetry at all (the
  NodeDB carries none).
- A resync therefore overwrites only what the NodeInfo actually carries rather than rebuilding
  the record — otherwise every resync would empty the detail screen.

#### `LocalStats` vs. `DeviceMetrics`

`struct mesh_radio_stats` is the one telemetry that is *not* about a node: `LocalStats` is the
connected radio describing itself and the air around it (packet counters, dupes, relays, online
node count, heap, noise floor). It lands on the session and is cleared with the handshake.

- The firmware sends it to the attached client alone, never over LoRa, so it is only taken from
  a packet whose `from` is our own node number.
- Its fields are plain proto3 scalars, so a zero heap size or a zero noise floor is "not
  reported", not a reading — which is why those two carry a flag and the counters do not.
- Battery is not in it at all. That arrives only as our own node's ordinary `DeviceMetrics`,
  which is why the Status screen reads it from the node cache.
- The airtime pair is in *both*, and LocalStats wins it: `DeviceMetrics` is what our node last
  broadcast about itself on the telemetry interval (half an hour by default), so preferring it
  leaves the row on a stale 0.0% while the radio is busy.

#### Traceroute

`hops_away` is a count, not a route, and never says which nodes are carrying you.
`mesh_session_send_traceroute` puts an empty `RouteDiscovery` on `TRACEROUTE_APP` with
`want_response`; every node that forwards it appends itself, and the target answers with both
directions and the SNR of every link.

- The reply is matched on `Data.request_id` against our packet id, because a trace between two
  *other* nodes crosses our radio wearing the same portnum. A RouteDiscovery never reaches the
  message log either way.
- One trace at a time (`-EBUSY`) — the client's half of the firmware's own rate limit, and the
  reason a finished result is kept rather than re-run: a screen that traced on every repaint
  would be refused and would flood the mesh.
- Nothing reports a trace dropped on the way out or back, so `mesh_session_tick` times it out at
  60 s, ahead of the link guards.
- `mesh_app_flatten_traceroute` turns the protobuf's shape (intermediate nodes plus a parallel
  array of link SNRs) into the two paths the UI draws, every stop carrying the reading of the
  link that reached it: the ends are stitched on (us going out, the target coming back), each hop
  resolves to a name, and hop `i` takes `snr[i - 1]`. That array is normally one longer than the
  route — one reading per *link* rather than per node — but every pairing is bounds checked
  rather than assumed. `INT8_MIN` is the firmware's "not measured" and draws as "no reading", not
  a -32 dB link.

#### Asking the radio about a node

- **`mesh_session_request_node_info`** sends our own `User` on `NODEINFO_APP` with
  `want_response`. This is the only way to name a node that joined after the NodeDB replay. It
  returns `-EAGAIN` until our own owner record has arrived, and there is deliberately **no
  placeholder**: a NodeInfo is applied by overwriting the record whole
  (`mesh_session_apply_user` blanks the names and drops the public key when the incoming `User`
  lacks them, and the firmware's NodeDB does the same), so a `User` carrying only an id would
  erase *this* node's identity on every peer that received it.
- **`mesh_session_set_node_favorite`** / **`mesh_session_set_node_ignored`** queue
  `set_favorite_node`/`remove_favorite_node` and `set_ignored_node`/`remove_ignored_node`, and
  flip the cached flag themselves: there is no `get_favorite` or `get_ignored`, and the radio
  only returns the flag with that node's next NodeInfo. Ignoring the radio we are connected
  through is refused — it would drop our own traffic.
- **`mesh_session_sync_clock`** pushes the Brick's own `time(NULL)` at the radio once per
  connection so a node with no GPS stops sitting at 00:00 and its packets carry a real `rx_time`.

Three neighbouring admin verbs are deliberately *not* exposed: `toggle_muted_node` means "no
notifications" and we have none; `remove_by_nodenum` is undone by the node's next packet, so it
would often look broken; and a position request duplicates what the NodeInfo exchange already
brings back.

### `src/core/radio_settings.c` — the admin protocol

A transport-agnostic view of the connected radio's configuration (`struct mesh_radio_settings`:
every `Config`/`ModuleConfig` section, owner `User`, `DeviceMetadata`) plus the `AdminMessage`
plumbing. It encodes `ADMIN_APP` requests addressed to our own node with `want_response`, decodes
replies (correlated by `Data.request_id`), keeps the `session_passkey` every reply carries
(firmware 2.5+ rejects a `set_*` without it), and runs a one-at-a-time request queue with a 5 s
timeout. The session feeds it handshake fragments and admin packets, sends a metadata+owner
probe once `config_complete_id` arrives, and pumps the queue from `mesh_session_tick()`. Admin
replies never reach the message log.

Writes (`mesh_radio_settings_queue_write`) always go out as three requests:

1. `get_owner` — for a fresh passkey; the firmware rotates it after 150 s.
2. the `set_*`, carrying the **whole** section (the firmware replaces, it does not merge).
3. the matching `get_*`, so the tab shows what the radio actually kept.

A `set_*` is answered by a `ROUTING_APP` packet quoting our id: `error_reason` NONE is the ack,
`ADMIN_BAD_SESSION_KEY`/`BAD_REQUEST` a rejection. `ingest` claims those and counts them in
`writes_acked`/`writes_failed`. The full channel table (`has_channel[]`/`channels[]`, keys
included, never persisted) is kept for `set_channel`, which must carry the whole `Channel`;
`get_channel_request` is index+1.

`MESH_ADMIN_SET_TIME` is the odd one out: no read-back, because there is no `get_time`. It is
gated on `MESH_RADIO_CLOCK_MIN_EPOCH` and left out of `mesh_admin_request_is_write` on purpose,
so it never counts as a save or toasts over one. `set_time_only` is UTC; the node shows local
time only once `DeviceConfig.tzdef` is set, which is the Device section's one editable row.

Most sections reboot the radio 7 s after a set (owner, module configs, display when
`screen_on_secs`/`flip_screen` change), so the link drops and auto-connect reconnects. **That is
expected, not a bug.** Phase status is in [`settings-roadmap.md`](settings-roadmap.md).

### `src/core/message.c`

Transport-agnostic messaging: builds `TEXT_MESSAGE_APP` packets into a `ToRadio`, folds inbound
`MeshPacket`s into a fixed ring (`mesh_message_log`), and correlates `ROUTING_APP` replies with
the outbound message they ack. Message text is untrusted radio input, so `mesh_message_ingest`
sanitises control bytes and backends can draw it directly.

### `src/core/app.c`

`mesh_app_publish_ui_state()` copies discovery/handshake state into the UI store every loop
iteration and persists the handshake cache and preferences under `$HOME`
(`~/.meshclient/ui_prefs`, `ui_prefs.handshake`).

`mesh_app_autoconnect()` runs every foreground turn: preferred node if in range, else the
strongest advertiser after 30 s, exponential backoff (2 s to 60 s) on failure.
`MESHCLIENT_AUTOCONNECT=0` turns it off.

A BLE connect returns 0 several seconds before it is a connection, so neither the backoff nor the
UI can key off that return value. `mesh_app_report_link_errors()` therefore runs between `tick()`
and `mesh_app_autoconnect()` (a retry restarts the link and clears the reason the last attempt
failed), pops a transport's `take_error()` line, toasts it when the user asked for the connect,
and counts the attempt against the backoff. Only an established link clears the backoff.

The `--status`/`--list-devices` paths in `main.c` do their own connect.

The post-stop publish in `mesh_app_run` only touches the transport line, because the shutdown
save would otherwise persist an empty handshake and unresolved peer names.

### `src/core/updater.c` + `src/core/version.c` — the client updating itself

Everything else here is about the radio; this is about the client.

`mesh_version_string()` returns the `MESHCLIENT_VERSION` compile definition CMake feeds from
`project(... VERSION ...)`, or `"dev"` for a build without one. `mesh_version_compare()` is
SemVer precedence including prerelease ordering, so a `dev` build never offers to "update" itself
to a release — which is what keeps a working tree from replacing its own binary.

The updater has **no TLS of its own**: it forks the device's `curl` (then `wget`) and reads its
stdout through the event loop, the same shape `minui.c` uses for `minui-list`, because the
release build is static musl with libdbus as its only dependency. One child at a time, states
strictly sequential, `tick()` enforcing the timeout.

**It has to bring its own CA bundle.** The Brick has no system CA store at all — no `/etc/ssl` —
so a bare `curl` fails every HTTPS request with exit 60, and busybox `wget` there has no HTTPS
support whatever. The pak ships Mozilla's roots at `certs/certificates.crt` (the same thing Pak
Store does) and `updater_resolve_ca_bundle()` picks one: `SSL_CERT_FILE` or `CURL_CA_BUNDLE`
first, then our bundle via `updater_pak_file()`, then the usual distro paths so a desktop build
keeps using the system's. Because the bundle ships in the pak and not through self-update, a
device installed before it has to reinstall the pak once; curl's exit 60 is mapped to
"No CA certificates; reinstall the pak" so the About screen says so.

**`--insecure` is not an alternative and must not be added.** The release metadata is what
carries the digest every download is checked against, so trusting it unauthenticated would defeat
the verification rather than route around a missing file.

What makes downloading an executable safe is not the transport but the digest:

- The release metadata comes from `api.github.com` — `releases/latest` on the Stable channel,
  `releases?per_page=1` on Prerelease. `latest` deliberately skips prereleases, so a beta client
  polling it would never see the next beta; the `per_page=1` cap also keeps the reply a single
  release object, so the scanner cannot pair one release's tag with another's asset.
- The asset URL is refused unless it sits under *this* repository's `releases/download/` path.
- The bytes must hash (`src/utils/sha256.c`, self-contained so the one check that matters does
  not depend on busybox) to the `digest` that metadata carried. A release with no digest is
  refused rather than installed unverified.
- The install is `rename()` within one directory, so it is atomic, and Linux keeps the running
  image alive off its inode — which is why the last state is READY ("relaunch to run it") rather
  than a restart the app performs on itself.

Which question to ask is `enum mesh_update_channel`, an About-screen setting persisted as
`update_channel=` in `ui_prefs` rather than inferred from the running build: a stable install had
no way to opt into beta and a beta install no way back out. DEFAULT keeps the old inference (a
prerelease build follows prereleases), so a prefs file written before the setting reads as "no
change". There are two channels and not one per release branch on purpose — telling beta from rc
would mean parsing an array of releases instead of the single object `per_page=1` guarantees, and
the SemVer ordering already offers a beta user the stable that supersedes their beta.
`mesh_updater_set_channel` forgets whatever the last check found, so an asset fetched on one
channel can never be installed after switching to the other.

### `src/utils/text.c`

The UTF-8 helpers everything that touches radio text shares. `mesh_text_sanitise` (folds C0
controls, replaces malformed bytes with `?`, never splits a sequence at the buffer boundary) is
what `message.c` runs on message bodies and `session.c` runs on `long_name`/`short_name`;
`mesh_text_utf8_length`/`_offset`/`_truncate` are what the fb backend measures lines with.

Names are radio input exactly like message text is — whoever owns the node picks the bytes — and
`User.short_name` is `char[5]`, sized for one four-byte emoji and its NUL, so multi-byte names
are the norm, not an edge case.

## Protobufs

`CMakeLists.txt` has a hardcoded `MESH_PROTO_NAMES` list (mesh, portnums, interdevice, config,
module_config, telemetry, channel, device_ui, xmodem, atak, admin, connection_status). **Adding a
new upstream `.proto` means adding it there.** Generated headers are included as
`meshtastic/<name>.pb.h` and land in `build/<type>/generated/nanopb/`.

The generator is `nanopb_generator` from PATH, falling back to
`third_party/nanopb/generator/nanopb_generator.py` via Python3 (needs
`pip install protobuf grpcio-tools`). `proto/meshtastic` and `third_party/nanopb` are git
submodules; `make proto` regenerates after a submodule bump.

## Invariants

Things that look like bugs, are not, and have each cost a debugging round already:

- **No threads.** Everything is the one epoll loop.
- **BLE is not Nordic UART** and has no length framing: one bare protobuf per GATT write/read.
  `src/proto/framing.c` is a homegrown varint prefix that nothing on the wire uses; serial
  framing is `src/proto/stream_framing.c`.
- **The Brick's face buttons do not report by position.** A is `BTN_EAST` (305), B is `BTN_SOUTH`
  (304), the button printed **Y (on the left) is `BTN_NORTH` (307)**, so X on the top is
  `BTN_WEST` (308). All four verified from the device log; `input_brick_face_buttons` pins them.
  Do not "fix" any of it back.
- **A radio reboot after a settings write is expected**, not a dropped link to chase.
- **Text is measured in cells, not bytes.** A `strlen` in fb layout code is a bug; so is `%-Ns`.
  See [`ui.md`](ui.md).
- **Only the release build is a release.** Do not stamp a local build to test the updater; lift
  the guard instead. See [`semantic-release.md`](semantic-release.md).
- **`project(meshclient VERSION x.y.z ...)` in `CMakeLists.txt` is rewritten by the release
  workflow.** Do not change that line's shape and do not bump it by hand.
- **`launch.sh` and the `Tools/` helpers do not ship through self-update.** Changing either means
  the user reinstalls the pak, so treat those two as a compatibility boundary.

## History

An SDL2 backend was tried in 1.1.11 and replaced by the framebuffer backend in 1.1.12; it is gone
from the tree, recoverable from commit 62fcb09 if ever wanted.
