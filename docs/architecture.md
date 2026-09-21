# Architecture

A C17 Meshtastic client for the TrimUI Brick (NextUI/MinUI, `tg5040`), shipped as a
`MeshClient.pak`. Single-threaded epoll loop, pluggable transports, nanopb for the protobufs.

The transports have their own page ([`transport.md`](transport.md)), as does the UI layer
([`ui.md`](ui.md)). The rules that look like bugs and are not are in
[`non-bugs.md`](non-bugs.md).

```
link (transport) -> mesh_session -> mesh_app -> UI store -> controller -> backend
evdev -> inkcell_input -> controller -> nav.c -> mesh_ui_action -> mesh_app_on_ui_action
```

## Layers

The directory under `src/` is a layer, and the arrows above are its rules. Everything compiles
into one library (`meshclient_core`), so the linker has no opinion about direction — nothing but
a convention stops `src/geo` from reaching into `src/ui`. `scripts/check-layers.py` is that
convention made into a test: it reads every `#include "mesh/<area>/..."` and checks it against a
table of allowed edges, and `make test` runs it.

```
app          the composition root: one of everything, wired together
 |
 +-- ui      store, nav, backends            -- reads core's types to draw them
 +-- core    session, admin queue, messaging -- answers to a radio, never to a screen
 +-- transport  BLE, serial, TCP             -- carries frames for a session
 +-- map     tiles and the viewport
 +-- proto   wire formats
 +-- geo     projection and geometry          (leaf)
 +-- i18n    the string catalog               (leaf)
 +-- utils   text, time, log, json, sha256    (leaf)
```

**`core` does not include `ui`.** That is the edge the check exists for. The session, the message
log and the admin queue answer to a radio, and the moment one of them reads a store record the
client can no longer be driven headless — which is what the CLI backend and most of the test
suite depend on. Publishing is a one-way copy in the other direction, and `src/app/app_publish.c`
is the only thing that does it.

`app` is the exception and is meant to be: it embeds a `struct mesh_ui_store`, a
`struct mesh_session` and a transport registry *by value* in one `struct mesh_app`, so it needs
the complete type of each. That is why it is its own directory rather than part of `core` — a
composition root that sees everything is not a layering violation, but a composition root hiding
inside a layer is.

Adding a *top-level* directory under `src/` means adding a row to `ALLOWED` in that script, with
the areas it may include from and why. An edge with no reason written next to it is one to delete.

A **group** inside an area is not a layer and needs no row: a file's area is its first directory,
so `src/ui/store/store.c` is `ui` and `src/transport/ble/bluez_client.c` is `transport`. Groups
are how `src/ui/` (51 sources) and `src/core/` (26) are kept navigable; they carry no rule of
their own, and a source moving between two of them changes nothing a caller can see, because
`include/mesh/<area>/` is flat.

Allwinner A133P, 1 GB RAM, WiFi and Bluetooth. Constrained enough that the binary stays small
and brings no runtime with it — C plus nanopb, no SDL, no interpreter. Pak conventions the code
depends on: a pak is `/Tools/tg5040/<Name>.pak/` with a `launch.sh`; logs go to
`/.userdata/$PLATFORM/logs/<pak>.txt`; state lives under `/.userdata/$PLATFORM/<pak>/`, which
`launch.sh` sets as `$HOME`.

## `src/core/runtime/event_loop.c`

An epoll loop over a fixed table of 32 fd sources: D-Bus watches, the timerfd discovery refresh,
the UI store eventfd, the serial tty, a fetch's socket. **No threads anywhere. Do
not add them.**

### Blocking work, and the two ways out of it

Nothing may block the loop, and two things a client has to do are blocking by nature.

**A name lookup forks.** `getaddrinfo()` blocks, POSIX offers no non-blocking form, and
`getaddrinfo_a()` starts threads. `src/core/net/resolve.c` forks a child that blocks in it and writes
one fixed-size record back through a pipe the loop owns. The child does not exec, so it is no
program the device has to have. An address literal costs no child at all. See
[`docs/transport.md`](transport.md#a-name-costs-a-fork).

**TLS does not.** `src/core/net/tls_client.c` drives Mbed TLS through BIO callbacks over a
non-blocking socket, reporting `-EAGAIN` back out to the loop rather than waiting. Two things sit
on it: the MQTT proxy's broker connection, and `src/core/net/fetch.c`, which is every HTTPS request
the client makes - HTTP/1.1 from the codec in `src/proto/http.c` over that session. See
[`docs/mqtt.md`](mqtt.md#tls) and the updater section below.

## `src/core/session/session.c` — the Meshtastic conversation

`struct mesh_session` is the conversation, independent of how the bytes travel: the
`want_config_id` handshake, the node-summary cache, the channel table, the message log, the
radio settings and admin queue, and packet ids. A link calls `mesh_session_attach(send_fn)`,
hands every FromRadio to `mesh_session_handle_from_radio`, calls `mesh_session_tick` each turn,
and detaches when the link drops (handshake and settings reset, messages survive).

### The node roster

`struct mesh_node_summary` is the whole node record: identity from `NodeInfo.user`, the NodeDB
flags, and `position`/`metrics`/`environment` sub-structs. 256 entries; every inbound
`MeshPacket` also refreshes its sender's `last_heard`/SNR/hops, adding the sender by id if the
sync never delivered its NodeInfo.

The firmware **replays its NodeDB exactly once per connection**, which shapes the rest:
`NODEINFO_APP`, `POSITION_APP` and `TELEMETRY_APP` packets are the only reason a node joining
mid-session ever gets a name, and the only source of environment telemetry at all. A resync
therefore overwrites only what the NodeInfo actually carries rather than rebuilding the record.

**The roster is the client's, not the radio's.** The radio's NodeDB holds 80 entries on this
hardware and evicts as soon as it fills, so mirroring it lost nodes that by then existed nowhere
else.

- `mesh_session_reset_handshake` **keeps the nodes**. They are dropped only when `MyNodeInfo`
  reports a different radio.
- Each `want_config` bumps `sync_epoch`; a NodeInfo stamps the node with it *and sets
  `in_nodedb` on the spot* — the NodeInfo is the proof, so a list drawn mid-sync does not mark
  every node as forgotten — and `config_complete` settles the other direction. A node with
  `in_nodedb == false` is one we remember and the radio does not: still on the mesh, but with no
  stored key for a DM, which is why the Nodes tab dims it and says "off radio".
- Full means **evict, not refuse**: a node the radio has already forgotten goes before one it
  still carries, oldest `last_heard` first, never ourselves and never a pinned node.
- Emptying it is the client's own verb, never a side effect of one sent over the air.
  `mesh_session_forget_nodes(only_off_nodedb)` sends nothing, so it works with no link. Each
  row shows `mesh_session_forgettable_nodes()` — the count *that press removes*, through the
  same predicate the forget uses — which is a different number from the Nodes tab's "off radio"
  total (the UI carries 128 nodes and the roster holds 256).
- `mesh_session_seed_node` restores the roster the last run persisted before any radio is
  attached, so a restart is not a reset either. The owning radio travels with it as its own
  `handshake_roster` line rather than as `my_info.node_num`, which is 0 while disconnected —
  exactly when the cache gets written.

**A node with no `User` still has a name.** `mesh_session_default_identity` derives the
placeholder the phone apps show (`!b2a7e54c` / `Meshtastic e54c` / `e54c`) from the node number
the moment a slot is created; `has_user` stays false until a real `User` arrives.

### Telemetry

`struct mesh_radio_stats` is the one telemetry *not* about a node: `LocalStats` is the connected
radio describing itself and the air around it.

- The firmware sends it to the attached client alone, so it is only taken from a packet whose
  `from` is our own node number.
- Its fields are plain proto3 scalars, so a zero heap or noise floor is "not reported", not a
  reading — which is why those two carry a flag and the counters do not.
- Battery is not in it at all; that arrives as our own node's ordinary `DeviceMetrics`.
- The airtime pair is in **both**, and LocalStats wins: `DeviceMetrics` is what our node last
  broadcast on the telemetry interval (half an hour), so preferring it leaves the row stale.

Beyond `DeviceMetrics` and `EnvironmentMetrics` a node can send `PowerMetrics`,
`AirQualityMetrics`, `HealthMetrics` and `HostMetrics`. They are **separate groups, not one
struct**, because a node reports the ones its hardware has: a screen that could not tell "no
sensor" from "reading of zero" would show a solar repeater as having 0% humidity. They are
**curated rather than complete** — `AirQualityMetrics` alone has twenty-six fields — and the wire
message is decoded whole either way. `HostMetrics` is the exception to the `has_*` rule: it has
no optional fields, so the flags are derived from what a running host cannot plausibly report.
Each group gets its own key in the node cache, so an older build reading a newer cache skips a
line instead of dropping the node. `TrafficManagementStats` is decoded and ignored.

### What the radio says about itself

Three `FromRadio` variants are the radio talking to the attached client:

- **`ClientNotification`** is the firmware explaining a decision to the *user* — a duty-cycle
  limit, a key that did not match. It goes on the screen, because a send that quietly went
  nowhere looks identical to a slow one. Only the newest is kept, with a monotonic `seq`.
- **`QueueStatus`** reports the outgoing queue after every `ToRadio`. A refusal is the one
  failure that produces *no* Routing reply at all, so the message would sit `PENDING` until the
  ring evicted it. `res` is a `Routing_Error`, the scale `mesh_message_log_mark_ack` speaks.
- **`rebooted`** says every fact the config sync gave us describes a process that has died. The
  reboot counter is bumped *after* `mesh_session_begin_handshake`, because that reset owns it.

All three are per-connection state. The app announces each exactly once by watching for the
counter to **differ** rather than to grow: both restart at 1 on a reconnect, and a greater-than
test would swallow the first notification of every connection after a talkative one.

### The mesh as a graph

`NEIGHBORINFO_APP` is the only thing on the wire that says which nodes hear which — everything
else describes one link.

- **The list belongs to `NeighborInfo.node_id`, not to `packet->from`.** These are forwarded and
  `last_sent_by_id` names the relayer, so filing under the sender draws one node's neighbours on
  another node's screen — wrong in a way that looks entirely plausible.
- **An empty report is kept as an empty report.** A node that hears nobody is what a repeater
  that has fallen off the mesh looks like.
- **Reverse edges are counted in full but drawn in part.** Upstream's ten-entry cap is on what
  one node reports about *its* neighbours, not on how many may report hearing this one. Rows stop
  at `MESH_UI_NODE_MAX_LISTENERS` and the screen says how many it left out.
- **"Heard by" is derived, never stored**, which is why `mesh_ui_node_detail_build` takes the
  whole roster. It is walked per frame rather than cached because a node that stops hearing us
  drops out of its own next report, and a cached answer would go on claiming it still does.
- The list **is** persisted, and so is the traceroute beside it: the broadcast interval is
  floored at four hours by the firmware, and a measured route is worth the same keeping.

### Relay attribution is a byte, not a node

`MeshPacket.relay_node` and `MeshPacket.next_hop` are the routing half of the header: who handed
this packet to our radio, and who that packet asked to carry it onward. Both are the **last byte**
of a node number, because that is all the header has room for, and everything awkward about them
follows from that.

- **A byte names one node number in 256.** Resolving one means scanning the roster, and a mesh of
  a hundred nodes collides by arithmetic rather than by bad luck. The rule both resolvers follow —
  `mesh_app_format_relay_name` for a message, `node_rows_relay_name` for the detail screen — is
  that a name is drawn only when there is **exactly one** candidate; no candidate and several
  both fall back to the `!..a3` partial id. "Relayed by ALICE" against a node that did not relay it is the client
  inventing a path through the mesh, and nothing on the screen would say so.
- **Zero means two different things.** `NO_RELAY_NODE` is "the firmware did not say";
  `NO_NEXT_HOP_PREFERENCE` is "flooded rather than routed", which is a real reading and is how
  every packet looked before firmware 2.5. So the node record carries a `has_route` flag: without
  it a node nothing has been heard from is indistinguishable from one whose traffic is flooding.
- **A relay byte equal to the sender's is usually not a relay.** A node stamps itself into
  `relay_node` as it transmits, so a packet heard straight from its sender names that sender —
  the common case on a small mesh, and a chip on every bubble if it were not filtered out. The
  message store writes `""` there and the detail screen says *direct*. The exception is a packet
  that came at least one hop: that one *was* carried, so the match is a collision with some other
  node ending in the same byte, and both fall back to the partial id. The sender is struck off
  the candidates there too — a node cannot have relayed what it sent, so naming it would be the
  relay row contradicting the hop row above it.
- **Ambiguity is settled over the whole session roster, never the published one.** The session
  holds `MESH_SESSION_MAX_NODES` and the UI is published the ranked `MESH_UI_MAX_HANDSHAKE_NODES`
  of them, so a byte can have one claimant among the nodes a screen was handed and another that
  was ranked away. The message resolver runs in core and sees all of it; the detail screen cannot,
  so it is handed `relay_ambiguous` / `next_hop_ambiguous` — computed at publish — and renders the
  partial id whenever either is set, rather than naming the one survivor it can see.
- **The node's pair is the last packet's and is not persisted**, and the reason is not the one
  the traceroute is kept on: a trace is a measurement with a stamp, where this pair is a side
  effect of whichever packet happened to arrive last and carries no time of its own - so a
  restored one could not say how old it is. A **message's** relay is
  persisted, because the route one packet took does not change after it arrives — it rides its own
  `msg_relay[]` key, resolved to a name at publish rather than re-resolved on load against
  whatever roster the next run happens to have.

The pair is not a route and the screen does not draw it as one: the traceroute above is what
answers that. It answers "did this come straight to me, and is the mesh routing this node's
traffic or flooding it", which a hop count does not say and which is what changes first when a
repeater goes down.

### Three ports carry text

`ALERT_APP` and `DETECTION_SENSOR_APP` are both "same as Text Message" upstream, so
`struct mesh_message` carries an `enum mesh_message_kind`.

- The transcript **always** heads an alert or a detection, even in a direct conversation. A run
  of identical-looking bubbles is precisely what a critical alert must not be.
- An alert's heading is drawn in the error tone, which is why `{BAD, BUBBLE_IN}` joined the theme
  contrast contract: the one message a theme must not swallow.
- An alert **toasts wherever the user is**; a detection does not. Only the newest unseen alert is
  announced, tracked by packet id rather than by a count, because the log merges a cached history
  back in at startup and a counter would re-fire the lot at the next launch.

A **direct message** toasts on the same machinery with three conditions: the conversation is not
muted, the user is not already looking at it, and it is not the first pass after a launch — the
log is seeded from the cache before the first publish. A broadcast raises nothing at all: a
channel is a room full of people talking.

Notices **queue** rather than overwrite — three at most, and a full queue drops its *oldest
waiting* entry, because a burst whose tail was dropped would withhold what happened last. A
repeat of what is already showing is dropped: two identical notices in a row are one notice
standing for eight seconds.

### Reactions are not messages

`Data.emoji` marks a packet whose payload is an emoji reacting to `Data.reply_id`. It is kept in
the log — it is traffic that happened — but filtered out of every transcript, preview and unread
count, and drawn on the bubble it names instead. Read as an ordinary message it was a bubble
containing one emoji with no indication what it was about: three wrong answers from one dropped
field.

`MeshPacket.pki_encrypted` puts a padlock on a direct message. What it does **not** say is whose
key that was: a public key arrives in a `NodeInfo` from whoever transmitted it, so "the radio
held a key for that name" and "the radio held *their* key" are different claims.
`NodeInfo.is_key_manually_verified` is the difference, and `src/core/session/key_verification.c` is the
out-of-band ceremony that sets it — two radios show a six-digit number and a short code, and the
users read both to each other by voice. The mark is a padlock for a key that merely arrived and a
shield for one somebody proved, which is `src/ui/tables/trust.c`'s answer rather than the transcript's,
since three screens draw trust and they have to agree. `AdminMessage.add_contact` is the other
half: the radio's NodeDB evicts and this roster does not, so handing a record back — public key
and verified bit included — is what makes a DM to it encryptable again.

A `SharedContact` reaches that verb two ways, and they are not equally trusted. From a node's own
row it is a record this client already holds, so `manually_verified` rides across intact — the
ceremony happened, and a round trip through the radio is not a reason to undo it. From a
`meshtastic.org/v/#` link (`src/proto/contact_url.c`) it is a stranger's bytes with nothing
authenticating them, so `src/core/session/contact_share.c` clears both `manually_verified` and
`should_ignore` before queueing: those two are instructions to the *reader's* radio rather than
statements about the sender's node, and a link that could set the first would launder a shield
out of a picture on a screen. What the link is for is the other direction — a key for a node that
has never transmitted, which no `NodeInfo` and therefore no roster record can supply.

For **our own** sends the echo is the only source of it, so the dedup branch in
`mesh_message_ingest` copies the encryption state as well as the timestamps. All four fields —
kind, padlock, `reply_id`, reaction flag — are written to the node cache on their own `msg_meta[]`
key; a reaction reloaded without its flag is a bare emoji bubble that also bumps the unread count.
The `Routing_Error` behind a failed delivery rides the same key as a fifth field, which the loader
takes or does without: an archive file holds records from every build that ever ran on the card,
and one written before the field existed reads back with no reason — which is what the bubble
already draws as the bare word.

### Traceroute

`mesh_session_send_traceroute` puts an empty `RouteDiscovery` on `TRACEROUTE_APP` with
`want_response`; every forwarding node appends itself and the target answers with both directions
and the SNR of every link.

- Matched on `Data.request_id` against our packet id, because a trace between two *other* nodes
  crosses our radio wearing the same portnum. A RouteDiscovery never reaches the message log.
- One trace at a time (`-EBUSY`), which is also why a finished result is kept rather than re-run:
  a screen that traced on every repaint would be refused and would flood the mesh.
- Nothing reports a trace dropped, so `mesh_session_tick` times it out at 60 s.
- `mesh_app_flatten_traceroute` turns the protobuf's shape into the two paths the UI draws, hop
  `i` taking `snr[i - 1]`. That array is normally one longer than the route — one reading per
  *link* — but every pairing is bounds checked. `INT8_MIN` is "not measured" and draws as "no
  reading", not a -32 dB link.
- **The result is kept per node and written to the card.** The session's one slot is the trace in
  *flight*; `struct mesh_ui_traceroute_log` beside it holds the last route measured to each of
  eight nodes, which is what a screen asks through `mesh_ui_store_traceroute_view()`. See
  [`ui.md`](ui.md#what-the-client-remembers) for the format and for what a swap drops.

### Asking the radio about a node

- **`mesh_session_request_node_info`** sends our own `User` with `want_response` — the only way
  to name a node that joined after the replay. It returns `-EAGAIN` until our owner record has
  arrived, and there is deliberately **no placeholder**: a NodeInfo is applied by overwriting the
  record whole, so a `User` carrying only an id would erase *this* node's identity on every peer.
- **`set_node_favorite` / `set_node_ignored`** flip the cached flag themselves: there is no
  `get_favorite`, and the radio only returns the flag with that node's next NodeInfo. Ignoring
  the radio we are connected through is refused — it would drop our own traffic.
- **`mesh_session_sync_clock`** pushes the Brick's `time(NULL)` once per connection so a node
  with no GPS stops sitting at 00:00.
- **`request_position` / `request_telemetry`** send an *empty* payload with `want_response`,
  because the alternative is waiting fifteen or thirty minutes for the node's own broadcast.
  Neither can erase anything (both are merged field by field at the far end) and neither carries
  `want_ack` — the reply *is* the acknowledgement.

## `src/core/session/radio_settings.c` — the admin protocol

A transport-agnostic view of the radio's configuration plus the `AdminMessage` plumbing: requests
addressed to our own node with `want_response`, replies correlated by `Data.request_id`, the
`session_passkey` every reply carries (firmware 2.5+ rejects a `set_*` without it), and a
one-at-a-time queue with a 5 s timeout. Admin replies never reach the message log.

A write always goes out as **three** requests:

1. `get_owner` — for a fresh passkey; the firmware rotates it after 150 s.
2. the `set_*`, carrying the **whole** section (the firmware replaces, it does not merge).
3. the matching `get_*`, so the tab shows what the radio actually kept.

A `set_*` is answered by a `ROUTING_APP` packet quoting our id: `error_reason` NONE is the ack,
`ADMIN_BAD_SESSION_KEY`/`BAD_REQUEST` a rejection. The full channel table (keys included, never
persisted) is kept for `set_channel`, which must carry the whole `Channel`; `get_channel_request`
is index+1.

`MESH_ADMIN_SET_TIME` is the odd one out: no read-back, because there is no `get_time`. It is
gated on `MESH_RADIO_CLOCK_MIN_EPOCH` and left out of `mesh_admin_request_is_write`, so it never
counts as a save or toasts over one.

**Most sections reboot the radio 7 s after a set**, so the link drops and auto-connect
reconnects. That is expected, not a bug.

### Administering another node's radio

`mesh_radio_settings_set_admin_dest()` points the Settings tab at a node in the roster instead
of the radio on the end of the link. The same queue, the same verbs, the same rows; four things
change.

- **The packet is addressed to that node and sealed to it.** `MeshPacket.to` is the node,
  `pki_encrypted` is set and `public_key` carries the node's own key, taken from its roster
  record — the division of labour every other packet here has, where the client says what it
  wants and the firmware does the cryptography. A destination we hold no key for is refused
  rather than sent in the clear; there is no unencrypted remote admin, because the legacy way of
  doing it is a shared channel named `admin` that every node holding the key is an administrator
  of. It deliberately does **not** carry `want_ack`: the firmware reports a delivery ack for a
  packet we originated as a ROUTING_APP packet quoting the same id the answer will quote, and
  `ingest_routing()` cannot tell the two apart — it would arrive first, release the queue before
  the AdminMessage landed, and record a write as saved by its delivery rather than by the
  firmware's verdict. The reply is the acknowledgement, which is `request_position`'s rule with
  a second reason behind it.
- **A reply is allowed a minute**, not five seconds — the same `MESH_TRACEROUTE_TIMEOUT_MS`
  allows, and for the same reason. The deadline is a property of the request that went out
  (`pending_dest`), not of where the tab is pointed now, because the two interleave. Three
  unanswered remote requests in a row drop the rest of the queue: a refresh is nearly thirty
  requests, and half an hour of a tab that looks busy and will never fill in says less than
  stopping does.
- **Everything the struct held is dropped**, because a config section is one radio's, and the
  tab showing this radio's LoRa settings beside that radio's owner reads as a working screen and
  is not one. What survives is what is not a section: the write tallies the app announces
  outcomes from, the region preset map (which no admin verb can ask a remote node for), and
  `link_metadata`.

That last one is the seam between two questions that used to share an answer. "What board is
this and what is it running" is the Settings tab's and follows the target;
`mesh_radio_settings_link_metadata()` answers "which firmware image may be written down this
cable", which is the *link's* and does not move — reading the tab's would offer a Heltec image
for a RAK in your hand, and the model comparison that exists to refuse exactly that would be
comparing the wrong two radios. The firmware group is not drawn in About radio while a target is
set: there is nothing to put in its place, because an image crosses a cable or a BLE link and
never a mesh.
- **Only the Settings tab's own requests follow it.** `queue_probe`, `queue_all`, `queue_write`,
  `queue_action` and `queue_ham_mode` go to the target; the clock push, the NodeDB verbs behind
  the Nodes tab, `add_contact`, the key-verification ceremony and the OTA request are always the
  connected radio's. The one action refused remotely is `ENTER_DFU_MODE` — a UF2 bootloader
  needs somebody at the USB port, so over the air it is a verb that takes a node off the mesh
  and puts the only way back at the far end of a walk.

Every queued request carries the `dest` it was stamped with, and the deduplication that folds
two `get_owner`s together keys on it. That is what makes one `session_passkey` slot safe with
two radios in the queue: each set sits directly behind its own passkey refresh, the queue is
strictly one at a time, and a refresh for our own radio is never folded into one for the target.
A reply that answers a request sent elsewhere contributes its passkey and nothing else.

**What authorises the request is not something this client can check.** Our own public key has
to be in the far radio's `SecurityConfig.admin_key` list, which only that radio knows. So the
gate is the half this side can answer — we hold a key to seal the request to — and a node that
has not been told to trust us answers nothing, which looks exactly like a node out of range.
Both end at the give-up count.

The banner (`mesh/ui/chrome.h`) outranks every other entry while a target is set, and stands
down only inside Settings > About radio, which names the node in a row and carries the press
that comes back.

## `src/core/session/store_forward.c`

A Store & Forward router keeps the last few hours of text traffic and hands it back on request —
the half that matters on a handheld, since a Brick spends most of its life switched off. One
portnum, `STORE_FORWARD_APP`, carrying a `StoreAndForward` whose `rr` says which half of the
conversation it is.

- **The client finds a router before it asks one.** A router announces itself on a fifteen-minute
  timer, so with none known `mesh_session_request_history()` broadcasts a `CLIENT_PING` and sends
  the real request to whichever router answers. That is the only thing this client ever
  broadcasts on the port: a broadcast `CLIENT_HISTORY` would have every router replay its whole
  window at once, and `mesh_store_forward_encode()` refuses one.
- **A replayed message is the sender's, not the router's — but it has no date.** `rx_time` is
  "never sent on the radio link itself", so the stamp on a replay packet is *our own* radio
  marking when the replay landed. Copying it would date the whole window at the minute it was
  fetched. The packet id, SNR, hop count and padlock go the same way: all measure how *this*
  packet reached us, one hop from a node that is not the sender. The roster is not touched either.
- **A replay is mostly things we already have.** `mesh_message_log_holds_replay()` matches on
  what was said — sender, channel, text, and the stamp when both copies have one, which for a
  replay is never — and the session counts `received` and `stored` separately. The cost is that a
  sender who said the same short thing twice on one channel gets one bubble back.
- **What we know is only ever true of one router.** The history cursor is an index into *that
  router's* packet history, so hearing a different router drops it: handing A's index to B would
  ask B to skip to a position in a table it does not have.
- **The follow-up request is sent from the tick, not from the ingest** — writing back down the
  link on the same turn is what the admin queue's queue-here-drain-there split exists to avoid.

The state machine is ten values rather than a bool because each is a different thing to tell the
user: "no router answered" and "the router never replied" are the same silence from two different
places, and only one is worth pressing again. Nothing about it is persisted.

## `src/app/*.c`

Four files around one `struct mesh_app`, with `app_internal.h` as the seam: `app.c` owns the
loop, the links and the process lifecycle; `app_actions.c` is the `mesh_ui_action` dispatch;
`app_publish.c` copies session state into the UI store; `app_settings.c` turns pending edits into
admin writes.

The dispatch is a table — `k_app_actions`, one row per verb — with a `_Static_assert` against
`MESH_UI_ACTION_COUNT`, because a verb the table does not answer for is a press that arrives and
silently does nothing.

`mesh_app_autoconnect()` runs every foreground turn over the nodes that answered the last scan
and only those. **The range test is `in_range`, not the RSSI**: BlueZ lists every device it holds
a bond for, and one it has not heard has no RSSI property at all — so the 0 that leaves behind
used to outrank every node that answered, and a radio left at home used to win outright. The MRU
of the user's own radios is `known_devices` (eight, over either link), whose head *is*
`preferred_device`; `mesh_app_note_connected_device()` is the only writer of either.

A BLE connect returns 0 several seconds before it is a connection, so neither the backoff nor the
UI can key off that return value. `mesh_app_report_link_errors()` runs between `tick()` and
`autoconnect()`, pops a transport's `take_error()` line, toasts it when the user asked for the
connect, and counts the attempt against the backoff.

## `src/core/update/updater.c` + `version.c` — the client updating itself

`mesh_version_compare()` is SemVer precedence including prerelease ordering, so a `dev` build
never offers to "update" itself to a release.

The updater fetches in-process: `src/core/net/fetch.c` resolves, connects, does the TLS
handshake and speaks HTTP/1.1 on the event loop, one request at a time, states strictly
sequential. It needs no program on the device. The firmware check and the firmware download each
have their own fetcher on the same code.

What the fetcher decides, and why:

- **https only, verified against the compiled-in roots** (`include/mesh/core/ca_roots.h`), or
  against `SSL_CERT_FILE` when it is set. The Brick has no system CA store, and a bundle in the pak
  would not ship through self-update; in the binary, the roots are as new as the release.
- **Redirects are followed, up to `MESH_FETCH_REDIRECTS_MAX`, and never off https.** A release
  asset is a 302 from github.com to its CDN. Every header goes to every hop, a `Range` included.
- **A range request must be answered `206`.** A server that ignores the range sends the whole
  file, and a caller that asked for 64 KB of a 46 MB zip is owed a failure rather than the zip.
- **A body framed by the close is whole only after a TLS close_notify.** Without one, a cut
  connection looks exactly like the end of the body. Content-Length and chunked bodies carry
  their own end and do not need it.
- **One deadline for the whole request**, lookups and hops included, and a read budget per loop
  turn so megabytes arriving on a fast link cannot hold the loop the UI draws on.
- **A download streams to its file only once a 2xx has arrived**, so a 404's error page never
  lands where the binary was expected.

**Byte progress comes from the file, not from the fetcher.** The download is going to
`staged_path`, a file this process named, and the metadata already said how large it will be —
so the fraction is `stat()` over `asset_size`, which needs nothing from the transport.
`mesh_updater_progress()` returns **false** for a step with no length at all, which the About
screen draws as an indeterminate bar rather than a zero.

**`--insecure` is not an alternative and must not be added.** What makes downloading an
executable safe is not the transport but the digest:

- Metadata comes from `api.github.com` — `releases/latest` on Stable, `releases?per_page=1` on
  Prerelease. `latest` skips prereleases, so a beta client polling it would never see the next
  beta; the `per_page=1` cap also keeps the reply a single release object, so the scanner cannot
  pair one release's tag with another's asset.
- The asset URL is refused unless it sits under *this* repository's `releases/download/` path.
- The bytes must hash (`src/utils/sha256.c`, self-contained) to the `digest` that metadata
  carried. A release with no digest is refused rather than installed unverified.
- The install is `rename()` within one directory, so it is atomic, and Linux keeps the running
  image alive off its inode — which is why the last state is READY rather than a self-restart.

The channel is an About-screen setting persisted as `update_channel=`, not inferred from the
running build. Two channels and not one per release branch: telling beta from rc would mean
parsing an array instead of the single object `per_page=1` guarantees.
`mesh_updater_set_channel` forgets whatever the last check found.

## `src/utils/`

`text.c` holds the UTF-8 helpers everything that touches radio text shares. `inkcell_text_sanitise`
folds C0 controls, replaces malformed bytes with `?` and never splits a sequence at the buffer
boundary. **Names are radio input exactly like message text is**, and `User.short_name` is
`char[5]` — sized for one four-byte emoji and its NUL — so multi-byte names are the norm.

`crash.c` catches SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT, writes
`$HOME/.meshclient/crash.txt` (signal and fault address, uptime, load base, version/route/transport
notes, the last 32 log lines, the PC and a frame walk), then re-raises so the process still dies
of the signal it was given.

**Nothing leaves the device.** This is deliberately not a crash-reporting service: the memory of
this process holds node names, coordinates, the message log and the channel keys. What lands on
disk is a page of text the user can read in full before attaching it to an issue.

**The file does not promise to be free of private data, and must not start.** Its header once
claimed to carry no message text or names, and three quarters of that was false — the log tail is
the client's *ordinary* log, which says `Sent "%s" to %s` and prints a hand-entered fixed
position. A user who attached the file *because the file told them it was safe* would have
published exactly what it promised was absent. Channel keys really are never logged.

Four rules rather than implementation details:

- **The handler builds no strings.** Neither `printf` nor `malloc` is async-signal-safe — a fault
  inside `malloc` leaves the allocator's lock held. The path, the load base and every fixed
  heading are built by `mesh_crash_install()` in ordinary context.
- **The stack walk is probed, not dereferenced.** `mesh_crash_install()` makes a pipe it never
  sends anything through: writing an address to a descriptor turns an unreadable page into
  `EFAULT` — a return value — where `*(uint64_t *)fp` would be a second SIGSEGV inside the
  handler for the first.
- **The risky half goes last.** Headings, notes and the log tail are written before the registers
  are touched, so a handler that dies part way through has already put the useful half on disk.
- **The handler runs on an alternate signal stack.** When the fault *is* the stack running out,
  the kernel has nowhere to build the signal frame, so without `sigaltstack()` and `SA_ONSTACK`
  the crash with the most interesting backtrace leaves no file.
  `crash_handler_survives_an_exhausted_stack` is the only case that notices when the flag goes.

Whether a report is waiting is read once, at install — answered on demand, the flag would flip
the moment *this* run wrote its own report. One path, so a second crash replaces the first: the
reader has just watched the client die, and a file from last week is the wrong half to keep. An
address resolves with `addr2line -fpe meshclient <address - load base>`.

## Protobufs

`MESH_PROTO_NAMES` in `CMakeLists.txt` is a hardcoded list. **Adding a new upstream `.proto`
means adding it there.** Headers are included as `meshtastic/<name>.pb.h`. The generator is
`nanopb_generator` from PATH, falling back to `third_party/nanopb/generator/nanopb_generator.py`
(needs `pip install protobuf grpcio-tools`). `make proto` regenerates after a submodule bump.
