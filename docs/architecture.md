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
timerfd discovery refresh, the UI store eventfd, the serial tty, the updater's curl child stdout.

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

#### The node roster

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

**The roster is the client's, not the radio's.** The radio's NodeDB is small — 80 entries on the
hardware this targets — and evicts as soon as it fills, so mirroring it meant losing nodes that
by then existed nowhere else: a walk with the radio would fill its database with new neighbours,
and the walk home would replace them again. So:

- `mesh_session_reset_handshake` clears the connection's state and **keeps the nodes**. They are
  dropped only when `MyNodeInfo` reports a different radio, whose NodeDB is another view of the
  mesh.
- Each `want_config` bumps `session->sync_epoch`; a NodeInfo stamps the node with it *and sets
  `in_nodedb` on the spot* — the NodeInfo is the proof, so a list drawn mid-sync does not mark
  every node as forgotten — and `config_complete` settles the other direction for the nodes no
  NodeInfo mentioned. A node with `in_nodedb == false` is one we remember and the radio does
  not — still on the mesh, but with no stored key for a DM, which is why the Nodes tab dims the
  row and puts "off radio" where its signal would go, the Status screen counts them on their own
  `Cached here` line, and the detail screen spells it out.
- Full now means evict rather than refuse: the victim is a node the radio has already forgotten
  before one it still carries, oldest `last_heard` first, never ourselves and never a pinned node.
- Emptying it is the client's own verb, never a side effect of one sent over the air:
  `mesh_session_forget_nodes(only_off_nodedb)` drops either the nodes `in_nodedb == false`
  marks or the whole roster, always keeping our own record and every pinned node, and sends
  nothing - so it works with no link at all. Settings > Radio actions carries both as rows,
  under the NodeDB reset that is the usual reason to want them; `mesh_session_nodes_off_nodedb`
  is the count they show and the Nodes tab's "off radio" total.
- `mesh_session_seed_node` restores the roster the last run persisted (`mesh_app_seed_nodes_from_cache`,
  from the UI handshake cache) before any radio is attached, so a restart is not a reset either.
  The owning radio travels with it as its own `handshake_roster` line rather than as
  `my_info.node_num`, which is 0 while disconnected — which is exactly when the cache gets
  written, and without it the next radio would inherit a roster instead of replacing it.
  A cache written before `node_state` existed has its `has_user` inferred from the names: one
  nobody could have derived came from a `User`.

**A node with no `User` still has a name.** `mesh_session_default_identity` derives the same
placeholder the phone apps show — `!b2a7e54c` / `Meshtastic e54c` / `e54c`, all from the node
number — the moment a slot is created, and `has_user` stays false until a real `User` arrives.
Without it, every node heard before its NodeInfo drew as `----` with a blank line beside it.

#### `LocalStats` vs. `DeviceMetrics`

`struct mesh_radio_stats` is the one telemetry that is *not* about a node: `LocalStats` is the
connected radio describing itself and the air around it (packet counters, dupes, relays, online
node count, heap, noise floor). It lands on the session and is cleared with the handshake.

- The firmware sends it to the attached client alone, never over LoRa, so it is only taken from
  a packet whose `from` is our own node number.
- Its fields are plain proto3 scalars, so a zero heap size or a zero noise floor is "not
  reported", not a reading — which is why those two carry a flag and the counters do not.
- Battery is not in it at all. That arrives only as our own node's ordinary `DeviceMetrics`,
  which is why the Status screen reads it from the node roster.
- The airtime pair is in *both*, and LocalStats wins it: `DeviceMetrics` is what our node last
  broadcast about itself on the telemetry interval (half an hour by default), so preferring it
  leaves the row on a stale 0.0% while the radio is busy.

#### The other telemetry a node can send

Beyond `DeviceMetrics` and `EnvironmentMetrics`, a node can broadcast four more `Telemetry`
variants, and each is kept as its own group on the node record: `PowerMetrics` (up to three
monitored supplies), `AirQualityMetrics`, `HealthMetrics` and `HostMetrics` (a node that is a
computer — meshtasticd on a Pi — with a filesystem and a load average).

- They are **separate groups, not one struct**, because a node reports the ones its hardware has
  and nothing about the others. A screen that could not tell "no sensor" from "reading of zero"
  would show a solar repeater as having 0% humidity.
- They are **curated rather than complete**. `AirQualityMetrics` alone has twenty-six fields,
  most of them per-particle-size bin counts that mean nothing without a chart. The wire message
  is decoded whole either way, so adding one later is a field here and a row in the node detail.
- `HostMetrics` is the exception to the `has_*` rule: it has no optional fields at all, so the
  flags are derived from what a running host cannot plausibly report — no uptime and no free
  memory mean the sender did not fill them in. A load average of zero *is* a reading, so the
  trio is flagged together on any of them being non-zero.
- Each group gets its own key in the node cache, for the reason the position and metrics groups
  do: an older build reading a newer cache skips a line it does not recognise instead of
  dropping the node.

`TrafficManagementStats` is decoded and ignored: it is seven counters about an experimental
upstream module, and there is nothing on a handheld that would act on them.

#### What the radio says about itself

Three `FromRadio` variants are the radio talking to the attached client rather than carrying
mesh traffic, and each answers something no packet can.

- **`ClientNotification`** is the firmware explaining a decision to the *user*: a duty-cycle
  limit reached, a channel key that did not match, a public key seen on two nodes. A LogRecord
  goes to our log at its own level; this goes on the screen, because nothing else reports these
  and a send that quietly went nowhere looks identical to a slow one. Only the newest is kept,
  with a monotonic `seq` so two identical notifications read as two events.
- **`QueueStatus`** reports the outgoing queue after every `ToRadio`. A refusal (`res` non-zero)
  is the one failure that produces *no* Routing reply at all — the packet never went on the air,
  so nothing will ever answer for it and the message would sit `PENDING` until the ring evicted
  it. `res` is a `Routing_Error`, the scale `mesh_message_log_mark_ack` already speaks, so it
  lands on the message with the firmware's own reason.
- **`rebooted`** says every fact the config sync gave us describes a process that has died. The
  session re-runs the handshake rather than serving stale config. The reboot counter is bumped
  *after* `mesh_session_begin_handshake`, because that reset owns the counter — a detach or a
  radio swap has to put it back to zero.

All three are per-connection state and clear with the handshake, the way `stats` does. The app
announces each exactly once by watching for the counter to **differ** rather than to grow: both
restart at 1 on a reconnect, and a greater-than test would swallow the first notification of
every connection after a talkative one.

#### The mesh as a graph

`NEIGHBORINFO_APP` is the only thing on the wire that says which nodes hear which. Everything
else on a node record describes one link: `hops_away` is a count, `snr` is the reading of the
hop that reached *us*, and a traceroute is one path measured once. A neighbour list is a node's
own answer to "who do I hear", capped upstream at ten out-edges.

- **The list belongs to `NeighborInfo.node_id`, not to `packet->from`.** These are forwarded
  across the mesh and `last_sent_by_id` names the relayer, so filing the list under the sender
  would draw one node's neighbours on another node's screen — wrong in a way that looks
  entirely plausible.
- **An empty report is kept as an empty report.** A node that hears nobody is a real and
  interesting state — it is what a repeater that has fallen off the mesh looks like — and
  ignoring it would leave the last non-empty list standing as though it were still true. The
  node detail says "hears no one" rather than showing a heading with nothing under it.
- **The reverse edges are counted in full but drawn in part.** Upstream's ten-entry cap is on
  what one node reports about *its* neighbours; it says nothing about how many nodes may report
  hearing this one, which on a dense mesh is everyone in range. The rows stop at
  `MESH_UI_NODE_MAX_LISTENERS` for the row budget's sake and the screen then says how many it
  left out — stopping the count as well would make the one screen whose question is "how many
  can hear me" answer it wrongly and silently.
- **"Heard by" is derived, never stored.** No node reports who hears *it*; that edge exists only
  as every other node's list read backwards, which is why `mesh_ui_node_detail_build` takes the
  whole roster rather than one node. It is also the half a person holding the radio actually
  wants: "is anything hearing me" is not a question a hop count or an SNR reading can answer.
  It is walked per frame rather than cached because it is derived from data that moves under it
  — a node that stops hearing us simply drops out of its own next report, and a cached answer
  would go on claiming it still does.
- The list **is** persisted, unlike the traceroute: the neighbour module's broadcast interval is
  floored at four hours by the firmware, so a restart would otherwise wait that long for the
  picture to come back.

#### Three ports carry text

`TEXT_MESSAGE_APP` is not the only one. `ALERT_APP` (the firmware's critical alert) and
`DETECTION_SENSOR_APP` (a sensor announcing itself, "<name> detected") are both described
upstream as "same as Text Message": plain text addressed to a channel. The ingest accepted only
the first, so an alert or a detection reached this client and produced nothing whatsoever.

They belong in the conversation they were sent to, but not as ordinary messages, so
`struct mesh_message` carries an `enum mesh_message_kind`:

- The transcript **always** heads an alert or a detection, whatever it would otherwise have
  decided about naming and even in a direct conversation where the title already says who is
  talking. A run of identical-looking bubbles is precisely what a critical alert must not be.
- An alert's heading is drawn in the bad tone rather than the accent, which is why
  `{BAD, BUBBLE_IN}` and `{BAD, BUBBLE_IN_SEL}` joined the theme contrast contract: the one
  message a theme must not swallow is the one it would otherwise swallow.
- An alert also **toasts wherever the user is**, because the Messages tab may not be the one on
  screen and an alert is by definition the thing a client should interrupt for. A detection does
  not: a sensor announcing itself belongs in the conversation, not in front of whatever the user
  is doing. Only the newest unseen alert is announced — three arriving together are one
  situation — and it is tracked by packet id rather than by a count, because the log merges a
  cached history back in at startup and a counter would re-fire the lot at the next launch.

Nothing this client sends is ever an alert or a detection; there is no reason to originate one.

#### Reactions are not messages

`Data.emoji` marks a packet whose payload is an emoji reacting to `Data.reply_id`. It is kept in
the message log — it is traffic that happened — but `mesh_ui_nav_message_matches` filters it out
of every transcript and `mesh_ui_nav_conversation_summarise` out of every preview and unread
count, and the transcript draws it on the bubble it names instead, identical emoji counted
rather than repeated. Read as an ordinary message it was a bubble containing one emoji with no
indication of what it was about, which is three wrong answers from one dropped field.

`MeshPacket.pki_encrypted` rides along the same path and puts a padlock on a direct message. It
is worth saying: on a channel still using the default key every node on the mesh holds that key,
so a DM that did *not* go out PKI-encrypted was readable by all of them.

For **our own** sends the echo is the only source of it. `mesh_session_send_text` records the
message before the radio has done anything with it, and the radio decides per packet from
whether it holds the recipient's public key; it tells us by echoing the packet back. The dedup
branch in `mesh_message_ingest` therefore copies the encryption state as well as the timestamps.

All four of these fields — the kind, the padlock, `reply_id` and the reaction flag — are written
to the node cache on their own `msg_meta[]` key. Losing that line is not cosmetic: a reaction
reloaded without its flag is a bubble containing a bare emoji that also bumps the unread count,
which is the whole behaviour reading `Data.emoji` was meant to end, returning at every restart.

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

- **`mesh_session_request_position`** / **`mesh_session_request_telemetry`** send an *empty*
  payload on `POSITION_APP` / `TELEMETRY_APP` with `want_response`, and the answer lands on the
  node record the ordinary way. They exist because the alternative is waiting for the node's own
  broadcast - fifteen minutes for a position and half an hour for telemetry at the firmware's
  defaults - which made the node detail's readings a report on the past rather than an answer.
  Unlike the NodeInfo request neither needs our owner record and neither can erase anything: a
  `Position` and a `Telemetry` are merged field by field at the far end, so an empty one asserts
  nothing. Neither carries `want_ack` either — the reply *is* the acknowledgement, and asking
  for both would double what the question costs the mesh.

An earlier version of this document said a position request "duplicates what the NodeInfo
exchange already brings back". It does not: a `NODEINFO_APP` reply is a `User` record and
carries no position at all.

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

### `src/core/app*.c`

Four files around one `struct mesh_app`, with `src/core/app_internal.h` as the seam between
them: `app.c` owns the loop, the two links and the process lifecycle; `app_actions.c` is the
`mesh_ui_action` dispatch; `app_publish.c` copies session state into the UI store;
`app_settings.c` turns the UI's pending edits into admin writes.


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
stdout through the event loop, because the release build is static musl with libdbus as its
only dependency. One child at a time, states
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
  Serial does have framing, and it is `src/proto/stream_framing.c`.
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
