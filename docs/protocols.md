# Protocols

A **transport** is how bytes reach a radio (BLE, USB serial, TCP). A **protocol** is what those
bytes say. This client speaks three: Meshtastic (`struct mesh_session`), MeshCore
(`struct mesh_meshcore`, [below](#meshcore)) and Tern (`struct mesh_tern`, [below](#tern)). The line between a transport and a protocol is
`include/mesh/core/protocol.h`, and this page says what sits on each side of it and what is
still Meshtastic's everywhere else.

## The seam

A link does eight things with a protocol, and `struct mesh_protocol_ops` is those eight things:

| Op | When a link calls it | Meshtastic answers with |
|---|---|---|
| `attach` | the connection is up | `mesh_session_attach()` |
| `begin` | right after, to start the conversation | `mesh_session_begin_handshake()` (`want_config_id`) |
| `receive` | a whole frame arrived | `mesh_session_handle_from_radio()` |
| `frame_failed` | a queued frame was never delivered | `mesh_session_packet_failed()` |
| `tick` | every turn of the link's own tick | `mesh_session_tick()` |
| `silent` | after the tick: should the link drop the connection? | `mesh_session_link_silent()` |
| `keepalive` | TCP's idle timer (optional) | `mesh_session_send_heartbeat()` |
| `detach` | the connection went | `mesh_session_detach()` |

`mesh_session_protocol(session)` wraps a session in the table, and `src/app/app.c` hands the
result to every transport through `mesh_transport_registry_set_protocol()`. A transport run on
its own - tests, `--list-devices`, the CLI - still falls back to a Meshtastic session it embeds,
and `mesh_*_transport_session()` returns that one and only that one.

The protocol also says how its frames are wrapped on a byte stream: `stream_framing` points at a
`struct mesh_stream_framing` (`include/mesh/proto/stream_framing.h`), which the stream link
parses and encodes through. Meshtastic's is `mesh_stream_framing_meshtastic`. The framing is
the protocol's and not the port's, because one USB serial device speaks whichever protocol its
firmware does. A framing whose largest frame does not fit `MESH_STREAM_PARSER_CAPACITY` is
refused when it is bound, so a link cannot overrun its parser.

The same goes for BLE: `ble_profile` points at a `struct mesh_ble_profile`
(`include/mesh/proto/ble_profile.h`) - the service to scan for, the characteristic to write, the
one to subscribe to, and what a notification on it means:

| `inbound` | A notification is | Who uses it |
|---|---|---|
| `MESH_BLE_INBOUND_PULL` | a doorbell; the link reads `read_uuid` until a read comes back empty | Meshtastic (FromNum, then FromRadio) |
| `MESH_BLE_INBOUND_NOTIFY` | the frame itself; nothing is read | the Nordic UART shape, which is MeshCore's; Tern's |

The BLE link connects under the bound protocol's profile. Its scan looks for every profile in
`mesh_ble_known_profiles[]` (and the bound protocol's, if that is not one of them) and tags each
radio with the one it was found under; `mesh_ble_transport_device_profile()` is that tag, and is
what an app will choose a protocol by. A radio advertising two known profiles keeps the first in
that array. A profile whose `max_frame` exceeds `MESH_BLE_MAX_PACKET_SIZE` is refused on connect.

`tests/suites/protocol.c` drives the stream link with a fake protocol and a fake framing, and the
BLE link with a fake Nordic-UART-shaped profile. Those are the cases that fail if a link goes
back to calling Meshtastic by name.

## What is still Meshtastic's

The protocol seam covers the link. Above and beside it, these are still written against
Meshtastic, and each is where a second protocol has work to do:

| Where | What assumes Meshtastic |
|---|---|
| `src/transport/ble/ble_transport.c` | The words: a radio that does not expose the profile's characteristics is reported as "not Meshtastic", and the pairing prompt talks about a node's PIN |
| `src/transport/serial/` | The USB probe that tells a radio from a bootloader |
| `src/core/session/` | All of it. It is the Meshtastic conversation, and its structs carry nanopb types |
| `src/app/app_publish.c`, `app_actions.c`, `app_settings.c` | Translating between the session and the UI store, and between a UI action and a session call. This is where a second protocol's publish and dispatch would sit beside Meshtastic's |
| `include/mesh/ui/store_*.h` | A node is a 32-bit `node_id`. Channel roles, device roles, the settings sections and several enums are Meshtastic's ranges, carried as bytes so the UI does not include nanopb |
| `src/proto/channel_url.c`, `contact_url.c` | `meshtastic.org/e/#` and `/v/#` links |
| `src/app/app_mqtt.c`, `src/core/firmware/firmware_catalog.c` | Meshtastic's MQTT client proxy and its firmware release feed (MeshCore's feed is `firmware_meshcore.c`) |

The UI's renderers, the nav, inkcell, inkstand and inkwell do not know which protocol is on the
other end. They read the store.

## What the UI offers per protocol

`struct mesh_ui_settings` carries `protocol` and `protocol_lacks`: the `enum mesh_ui_feature`
bits (`include/mesh/ui/store_settings.h`) the protocol on the link has no counterpart for. A
screen that offers a Meshtastic-only verb asks `mesh_ui_settings_supports()` first, and the
publish fills both fields from `src/ui/tables/protocols.c`, one row per protocol by its ops
table's name. A protocol with no row lacks everything.

The bits say what *lacks*, like `excluded_modules`, so a zeroed record - a cold start, a test, a
cache written before the field - is full Meshtastic and nothing on screen changes for it.

| Feature | What disappears without it |
|---|---|
| `WAYPOINTS` | "Send a waypoint" on a node's sheet; the Map tab's L2/R2 to the places, which the bar stops naming and a press says why |
| `TRACEROUTE` | the traceroute verb (on MeshCore, a trace to a repeater it has a route to, else a path discovery) |
| `NODE_REQUESTS` | asking a node for its name or position |
| `NODE_TELEMETRY` | asking a node for its readings (on MeshCore, a contact) |
| `NODE_LOGIN` | logging in to a MeshCore repeater or room server - MeshCore's alone |
| `NODE_STATUS` | asking a MeshCore repeater or room server for its counters - MeshCore's alone |
| `NODE_NEIGHBORS` | asking a MeshCore repeater which nodes it hears - MeshCore's alone |
| `NODE_COMMANDS` | a MeshCore repeater's thread as its console: its commands where the quick replies would be - MeshCore's alone |
| `NODE_PATH` | a MeshCore contact's stored route on its screen, and the verb that forgets it - MeshCore's alone |
| `NODE_SHARE` | a MeshCore contact's advert sent again to the nodes in earshot - MeshCore's alone |
| `NODE_FLAGS` | mute, ignore - and pin on a node that is not a whole-key contact |
| `NODE_PIN` | pin, on the sheet and as X on the list and the detail |
| `NODE_REMOVE` | remove, on a node's sheet |
| `REMOTE_ADMIN` | configuring a node over the mesh |
| `KEY_VERIFICATION` | the verify-key ceremony |
| `CHANNEL_LINKS` | the channel share QR and import rows (on MeshCore, one channel to a link) |
| `CONTACT_LINKS` | the contact share and import rows (Meshtastic's `meshtastic.org/v/#` link, or the MeshCore app's `meshcore://contact/add`) |
| `NODE_ADD` | "put back on the radio" (Meshtastic) / "add as contact" (MeshCore), on a node the radio does not carry |
| `MODULES` | the Modules row in Settings |
| `REACTIONS` | React on X, and X itself inside a thread |
| `RADIO_FIRMWARE` | the radio firmware check and install rows |
| `FULL_CONFIG` | every Settings section but User, Position, LoRa and Channels, and inside those every row but the name, the coordinates, the frequency, bandwidth, spread factor, coding rate and power, a MeshCore channel's name and key, MeshCore's other parameters and its Bluetooth PIN under the name; the Radio details' capability, reboot-count and admin-session rows |
| `RADIO_MAINTENANCE` | shut down, NodeDB reset, backup, restore and factory reset - Reboot stays |

A LoRa save without `FULL_CONFIG` gets its own confirm sentence
(`mesh_ui_settings_confirm_for_protocol()`): nothing reboots, and there is no region or preset to
get wrong. `tests/suites/ui_protocol.c` is what fails when a gate is lost.

## MeshCore

MeshCore's companion protocol is the second conversation: `include/mesh/core/meshcore.h`, with the
codec in `src/core/meshcore/meshcore_codec.c` and the conversation in `meshcore.c`. Checked
against `examples/companion_radio/MyMesh.cpp` at companion-v1.17.1 (firmware version code 13).

- **BLE** is the Nordic UART Service (`6E400001-…`; the app writes RX `…0002` and is notified on
  TX `…0003`): `mesh_ble_profile_meshcore`, a `MESH_BLE_INBOUND_NOTIFY` profile, one frame per
  write or notification and never split. It is in `mesh_ble_known_profiles[]`, so the scan tags a
  companion radio with it, and `mesh_app_link_connect()` binds MeshCore for a radio so tagged.
- **Serial and TCP** frame as `'<'` (app to radio) or `'>'` (radio to app), a 16-bit
  *little*-endian length and the frame, at most 176 bytes: `mesh_stream_framing_meshcore`, with
  no `wake_byte`. Nothing on a port says which firmware is behind it, so the app asks
  (`src/app/app_probe.c`): a link opens in whatever it answered in last - Meshtastic for one never
  heard - and reopens in the other when no frame arrives within `MESH_APP_PROBE_WINDOW_MS`. Each
  protocol's parser ignores the other's opening, so a wrong guess costs the window and nothing
  else. A firmware build serves BLE *or* USB, never both, and a `_ble` build says nothing at all
  on its port - so a link that answers neither is dropped and auto-connect passes it over for
  `MESH_APP_PROBE_MUTE_MS`, which is what lets Bluetooth have its turn. A USB port is remembered
  by its sysfs id, not its tty, which it only gets once the driver binds.
  `MESHCLIENT_PROTOCOL=meshtastic|meshcore|tern` skips the question.
- **One command at a time.** Each `CMD_*` is answered by one `RESP_CODE_*` (the contact list by a
  start, a record each and an end), and the firmware's BLE queue holds four frames, so the
  conversation queues commands and writes the next only once the last is answered.
  `PUSH_CODE_*` (0x80 and up) arrive whenever they like. A command unanswered for ten seconds is
  given up on; two in a row is `silent()`.
- **Connecting** walks `DEVICE_QUERY` (asking for version 3, which puts an SNR on messages),
  `APP_START`, `SET_DEVICE_TIME` when the clock is credible, `GET_CONTACTS`, then `GET_CHANNEL`
  for each slot up to the lesser of the radio's count and `MESH_SESSION_MAX_CHANNELS` - 40,
  the most any companion build keeps. The tables are sized for MeshCore and Meshtastic fills
  the first eight: its admin requests, channel links and backups stop at
  `MESH_MESHTASTIC_CHANNELS`. Then the
  model's sync completes and `SYNC_NEXT_MESSAGE` drains the radio's queue - again on every
  `PUSH_CODE_MSG_WAITING`. A queue can also hold the pre-V3 shapes, so both decode.
- **The model is the session.** What MeshCore learns goes into `struct mesh_session` through
  `mesh_session_model_*()`, the same steps the FromRadio decoder takes, so every screen, the
  roster cache and the swap-of-radio rule work unchanged. While MeshCore holds the link the
  session's own send path is a refusal: a Meshtastic verb a screen forgot to gate fails rather
  than writing a protobuf at the radio.
- **A node is a 32-byte key.** Its roster number is the key's first four bytes
  (`mesh_meshcore_node_id()`), so a direct message - which names its sender by a six-byte
  prefix - resolves without a lookup, and `user_id` is the twelve hex digits MeshCore's apps
  print.
- **A channel message's only sender is `"Name: "` at the start of its text.** A name the roster
  holds becomes the sender and leaves the text; one it does not stays in it.
- **Settings are SELF_INFO.** `mesh_meshcore_store_settings()` projects it onto the session's
  `mesh_radio_settings` in Meshtastic's shape - the name as the owner, the radio numbers as a
  LoRa config with `use_preset` off, bandwidth in whole kHz as Meshtastic writes it (62.5 is 62) -
  so the Settings tab reads it unchanged. A save (`mesh_app_save_settings()`, routed by
  `meshcore_bound`) becomes `SET_ADVERT_NAME`, `SET_RADIO_PARAMS`, `SET_RADIO_TX_POWER` or
  `SET_ADVERT_LATLON` over what the radio reported, checked against the firmware's own bounds,
  then `APP_START` as the read-back. The answers settle into the record's `writes_acked` /
  `writes_failed`, which is what the save toast already watched. A row's 31 and 62 kHz go back
  out as 31.25 and 62.5, the reading Meshtastic's firmware gives them. The bandwidth, spread
  and power rows are MeshCore's own (`MESH_UI_FIELD_LORA_ANY_*`), stepping 7.8 kHz to 500, SF5
  to 12 and -9 dBm up in literal dBm (0 is 0, not "max"); the frequency row reads SELF_INFO's
  kHz rather than the record's float; a frequency finer than a kHz is refused, a seventh
  coordinate decimal is rounded, and a link lost mid-save is reported unanswered rather than as
  a restart.
- **The other parameters** - location in adverts, who may request telemetry, location and
  sensor readings (SELF_INFO's three 2-bit modes: no one, contacts given the permission,
  everyone), auto-add and multi-acks - are rows under the name in User, and one
  `SET_OTHER_PARAMS` with all four bytes over what SELF_INFO reported. "Add heard nodes" is the
  firmware's `manual_add_contacts` turned over.
- **Which heard nodes are added** is `GET_AUTOADD_CONFIG`, asked once the handshake is through
  and refused by firmware older than it, which leaves the rows unoffered. Under "Add heard
  nodes": the four kinds still added with it off (FLAG rows over the byte's
  `AUTO_ADD_CHAT`..`SENSOR` bits, dimmed while it is on - the firmware adds every kind then),
  "Replace oldest when full" (bit 0) and "Add from", the raw hop byte (0 any, 1 direct, N up to
  N - 1 hops). One `SET_AUTOADD_CONFIG` with both bytes, believed on its `OK`. Not in a backup.
- **A full contact list** is `PUSH_CONTACTS_FULL`, which the radio sends with every node it then
  hears and cannot add. It is kept as `contacts_full` and said once as a toast; a contact the
  user removes clears it, an overwrite of the oldest (`CONTACT_DELETED`) does not.
- **The Bluetooth PIN** is DEVICE_INFO's `ble_pin`, under its own heading below them: Pairing
  (random, 0; fixed) and the six digits, one `SET_DEVICE_PIN` u32 with no read-back - its OK
  moves the PIN. The firmware reads it at boot, so it takes effect on the next restart, and a
  USB build keeps it too: set it over the cable before moving the radio to Bluetooth. The handoff after
  Move to Bluetooth answers the pairing with it, once, rather than asking for it.
- **A channel slot is edited** as a name (31 bytes; `MESH_UI_FIELD_CHANNEL_ANY_NAME`) and a
  16-byte secret (`_ANY_KEY`) and nothing else, written whole with `SET_CHANNEL` and read back
  with `GET_CHANNEL` for that slot alone. The walk keeps each slot's secret in the settings
  record so a kept key is written back as it was; the record's name is Meshtastic's twelve bytes,
  so the editor and the save take the whole name from the roster's channel instead. The key's
  "default" is the Public channel's published secret, and "from the #name" is a hashtag
  channel's - SHA-256 of the name, `#` included, cut to 16 bytes - which is what lets anyone who
  knows the name join. No 256-bit key and no open channel: the firmware has neither. Clearing
  is `SET_CHANNEL` with an empty name and a zeroed secret, and is not offered on slot 0.
- **A contact is removed** with `REMOVE_CONTACT` and its whole key, and leaves the roster
  (`mesh_session_model_drop_node()`) when the radio answers OK - a refusal or a timeout leaves it
  listed. Only a contact is offered the row: a heard node the radio never added, or a sender
  known by its key's prefix, has nothing on the radio to remove. A radio that adds contacts by
  itself takes it back at its next advert.
- **A heard node is added** with `ADD_UPDATE_CONTACT` and the record its advert gave - key, kind,
  name, stamp, position - with no route, so the first message floods and the radio learns one. It
  becomes a contact (`in_nodedb`) on the radio's OK. That is the one way to reach a node heard
  while "Add heard nodes" is off.
- **A contact is pinned** by bit 0 of its flags, MeshCore's favourite, which an auto-add that
  finds the list full will not overwrite. An update replaces the whole record, route included,
  so `GET_CONTACT_BY_KEY` reads it first and `ADD_UPDATE_CONTACT` writes it back with that bit
  alone changed; the row follows the radio's OK.
- **A contact's readings** are `SEND_TELEMETRY_REQ` by its whole key, answered by
  `TELEMETRY_RESPONSE`'s Cayenne LPP (`mesh_meshcore_decode_lpp()`): channel 1's voltage is the
  battery, and a sensor's temperature, humidity, pressure, light, current and GPS fill the node's
  environment and position. A node answers only if its telemetry settings let this radio ask.
- **A login** to a repeater or room server contact is `SEND_LOGIN`: the whole key, then the
  password (at most 15 characters, blank for a guest), answered by `LOGIN_SUCCESS` or
  `LOGIN_FAIL`. The password is typed each time; nothing keeps it. A newer server's success
  carries our ACL permissions, whose low two bits are the role - guest, read-only, read-write,
  admin - and that is the answer; an older one's has only the byte before the key prefix, which
  is not "is admin": a room server sends 2 there for a visitor with no rights. The role is kept
  on the node (`mesh_node_summary.login`) with when it was said, and shown on its Identity group
  with that age: the server's access list outlives the login, and nothing tells the client when
  it drops us. `HAS_CONNECTION` and `LOGOUT` are not used - they track the radio's keep-alive
  connection, which current servers never ask for (their login answer's keep-alive is 0), so
  against them one always says no and the other does nothing.
- **A status** is `SEND_STATUS_REQ` by the whole key, answered by `STATUS_RESPONSE`: the node's
  stats struct as it lies in memory (`mesh_meshcore_decode_status()`). The first 48 bytes are
  shared; a repeater follows them with its receive airtime and errors, a room server with its
  post counts - the same length, so the contact's kind picks the reading. They land on the
  node's `relay` group, the battery on `metrics`. A node answers only a client on its access
  list, so silence is said as "log in to it first".
- **A message to a repeater is a command.** A repeater takes text only from its admin, and runs
  it: so a direct message to one goes as `SEND_TXT_MSG` with `TXT_TYPE_CLI_DATA`, which the radio
  sends with no ack, and the repeater's reply - a CLI-typed message through the radio's queue -
  is the answer that marks it delivered and the next message in the thread. The thread is the
  repeater's console, and its compose sheet lists commands where the quick replies would be
  (`src/ui/tables/repeater_commands.c`): only what a repeater answers over the mesh - its
  `stats-*` run from its serial console alone - and nothing that is a slip of the thumb away
  from `reboot` or a `set`. A command is never tried again, since it may have run and only its
  reply been lost - nor is it offered to START's resend - and one unanswered by the deadline
  fails, with a toast that says to log in as admin.
  A reply names no command, only the order they went in, so a failed one keeps its place for a
  minute: a late reply is its answer - it is delivered after all - not the next command's.
- **A repeater's neighbours** are `SEND_BINARY_REQ` with `REQ_GET_NEIGHBOURS` - the ten it heard
  most recently, each by the first four bytes of its key - answered by a `BINARY_RESPONSE`. That
  answer names no node, only the tag the request's `SENT` carried, so the tag is what ends it.
  The list lands on the node's `neighbors`, the record Meshtastic's NeighborInfo fills, so its
  Neighbours group and every other node's "Heard by" read it unchanged, and it is cached as a
  Meshtastic list is. Like a status, a repeater answers only a client logged in to it.
- **A channel link** is the MeshCore app's `meshcore://channel/add?name=...&secret=<32 hex>`:
  one channel, where Meshtastic's link is the whole set. So each channel in use has its own
  share row, drawn from that slot's name and secret, and a typed link joins one channel into the
  first slot the sync read as unused - a `SET_CHANNEL` save - leaving the others as they are. A
  slot already holding that name and secret is "already on it"; no free slot is refused.
- **A traceroute** to a repeater or room server the radio already has a route to is
  `SEND_TRACE_PATH`: sent direct along that route, to the node, and back along the same hops
  reversed, under a tag that `TRACE_DATA` echoes. Every hop appends the SNR it heard the trace
  at, so both ways get a reading per link. Only a node that forwards can be a stop - a companion
  cannot - and a trace names each hop by 1, 2, 4 or 8 bytes, so a route kept at three bytes a
  hop is traced by its first two. Anything else - a repeater with no route yet, or one whose
  route out and back is past the firmware's 64 hops - gets a path discovery. The tag is set
  when the trace is asked, so an answer that beats its `SENT` is still read.
- **A contact's stored route** is its record's `out_path`, kept on the roster entry
  (`mesh_node_summary.path_*`, the first eight hops) and read out on the node's Signal group a
  hop a row, named the way a path discovery's hops are. "Forget route" on its sheet is
  `CMD_RESET_PATH`, believed once the radio says `OK` - the same reset a message's last attempt
  sends - after which the next message floods and `PATH_UPDATED` brings the route it learns.
  Meshtastic lacks `NODE_PATH`: its next hop is the firmware's to learn and drop.
- **Share with nearby** on a contact's sheet is `SHARE_CONTACT` by key: the radio sends the
  last advert it kept for that contact again, zero-hop, so the nodes in earshot can add a node
  they never heard. A contact added from a link has no advert kept, and the radio's refusal is
  said as a toast naming it (`share_refusals`). Meshtastic lacks `NODE_SHARE`.
- **The radio's own counters** are `GET_STATS`, asked with `GET_BATT_AND_STORAGE` once the
  sync is through and every minute after (`MESH_MESHCORE_STATS_INTERVAL_MS`) - a MeshCore radio
  volunteers nothing like LocalStats. The three kinds land on the model's `stats` where
  LocalStats does, so the Radio tab reads them unchanged: uptime, noise floor, packets. The
  airtime counters are whole seconds since boot, so the shares are worked out over the last ten
  minutes of readings, and "busy" is only what the radio sent and heard. It keeps no relayed,
  dropped or duplicate counts; `has_routes` swaps those rows for its flood and direct split. The
  battery is a voltage on our own node, shown as volts - there is no percentage to trust on a
  board whose divider floats on USB - and the filesystem is a Storage row. A firmware older than
  companion v8 refuses `GET_STATS` once and is then asked for the battery alone.
- **A path discovery** is `SEND_PATH_DISCOVERY_REQ` by the whole key, flooded, answered by
  `PATH_DISCOVERY_RESPONSE`: the path our flood took out and the path the answer took back, each
  a length byte (hop count in the low six bits, bytes per hop less one in the top two) and the
  hops. A hop is the first one to three bytes of a repeater's key, so it is named from the roster
  only where exactly one node answers to it, and drawn as `!..ab` otherwise. It fills the same
  `mesh_traceroute` Meshtastic's does, with no SNR; silence is a trace that timed out.
- **One request to another node at a time.** The firmware keeps one pending and clears it for
  any new login, status, telemetry or binary request (`clearPendingReqs()`); a trace shares the lock, so a second is refused
  while the first is queued and until its answer or the deadline its `SENT` names. How each one
  ended is `mesh_meshcore.notice`, which the publish turns into a toast.
- **A contact link** is the MeshCore app's QR text, `meshcore://contact/add?name=…&public_key=<64
  hex>&type=N` (`src/proto/meshcore_url.c`). User in Settings shows this radio's, made from
  `SELF_INFO`, and adds a stranger's with `ADD_UPDATE_CONTACT` by key, kind and name - no route and
  no stamp, so the first message floods and the node's next advert is taken. A link for a node
  that is already a contact is refused rather than written over the route the radio has learned.
  The signed card the MeshCore app also shares, `meshcore://<hex>`, is the node's advert packet:
  it is read only for the sheet (name, key, kind) and handed to the radio whole with
  `IMPORT_CONTACT`, which checks its signature as it would an advert heard on the air and adds
  or refreshes the node through the advert push that follows.
- **An advert** is this radio announcing its name and key now: Radio details offers it to the
  nodes in earshot or flooded across the mesh. Meshtastic has no such verb, so the two rows are
  listed by `protocol`, not by a lacked feature.
- **A reboot is never answered.** Over a USB-serial bridge the port outlives the ESP32 behind it,
  so once the answer is overdue the conversation runs its handshake again by itself.
- **A direct message** is pending until `PUSH_CODE_SEND_CONFIRMED` carries the four bytes the
  `RESP_CODE_SENT` named. It is tried three times with the same timestamp - the last after
  `CMD_RESET_PATH`, so it floods - and then failed. A channel message gets `OK` and nothing more.

Not yet spoken: this radio's own signed card - its share code
is the plain `contact/add` link. The `meshcore` row in `src/ui/tables/protocols.c` hides the verbs
the protocol does not back.
`tests/suites/meshcore.c` holds the frames a Heltec V3 sent and drives the conversation end to
end.
- **Its firmware** is checked against MeshCore's own releases (`firmware_meshcore.c`): the web
  flasher's device list turns DEVICE_INFO's model string into a build, by name ignoring case or
  through the alias table there, then the newest `companion-v*` tag and that release's asset
  list name the file. The build is the one the radio runs now: `_usb` over serial, `_ble` over
  BLE. Both chips install over USB. An ESP32 companion is written through its ROM, as any ESP32
  is. An nRF52 has no admin verb to send it to its UF2 bootloader, so the link lets go and the
  port is reopened at 1200 baud with DTR dropped - the Adafruit core's "1200-baud touch"
  (`mesh_serial_transport_touch_bootloader()`) - and the drive it comes back as is written as a
  Meshtastic one's is. On the Brick a native-USB port is on the generic driver, whose tty
  reaches neither, so the rate and then DTR go through usbfs as CDC requests. Neither goes over BLE: Nordic DFU arms a radio by asking it. The T114 is
  one flasher entry with two builds, so it is the ambiguous refusal.
- **Switching a board between the two** is the other firmware's release for the same board,
  written as the whole flash over USB (`mesh_firmware_check_switch_to_meshcore()` and
  `_to_meshtastic()`). The board is named across firmwares only by the twin table in
  `firmware_meshcore.c` - the flasher's device name against Meshtastic's `platformioTarget`,
  never a name that looks alike - and a board with no row is not offered the switch. An ESP32
  gets the `-merged.bin` or `.factory.bin` at 0x0 through its ROM, then every data partition
  that image's own table declares past its end is erased, so the new firmware starts blank
  rather than reading the old one's filesystem. An nRF52 is not offered one yet: its UF2
  writes the application and leaves the internal filesystem, so switching back would find the
  old identity, keys and contacts - the opposite of what the confirm sheet says. Afterwards the port is asked in the new protocol first, with a 45 s window: a
  wiped MeshCore ESP32 formats and keys itself for about 20 s before its first frame.
- **A radio on USB that answers neither** - a MeshCore BLE build or repeater, an erased flash -
  has said nothing to check against. Once the probe has passed a bridge port over, Radio >
  details offers its board: the list is every ESP32 device in the flasher with one USB
  companion build (`mesh_firmware_list_blank()`), and the one picked by name gets that build as
  a whole flash (`mesh_firmware_check_blank()`), installed as a switch is. The list is not
  narrowed by chip - the flasher says only "esp32" - so a wrong pick is caught by the image's
  header against the ROM's answer, before anything is erased. The port stays passed over by
  auto-connect while its board is being chosen. It is offered beside a radio up over Bluetooth or
  TCP too, under that radio's own rows; choosing the board puts that link down, and auto-connect
  waits while the answer is held (`firmware.blank`), or a remembered node would take the link
  back before the install.
- **A MeshCore radio on its cable can move to its Bluetooth companion build**
  (`mesh_firmware_check_meshcore_bluetooth()`): the `companionBle` build's application alone,
  offered at any version, so the radio keeps its identity, contacts and channels and stops
  answering on USB. The answer carries `other_build`, which is what keeps it from being
  forgotten for not matching the bus the radio is on. Afterwards auto-connect reaches for
  `MeshCore-<node name>` over Bluetooth ahead of any other for five minutes, and pairs it
  attended: the BLE build shows a PIN on its screen, and the prompt is the user's to answer.

## Tern

Tern's companion protocol is the third conversation: `include/mesh/core/tern.h`, with the codec
in `src/core/tern/tern_codec.c`, the conversation in `tern.c` and the stream framing in
`src/proto/tern_framing.c`. Written from version 3 of `draft/companion.md` in
[ternmesh/spec](https://github.com/ternmesh/spec) alone - nothing of the Meshtastic or MeshCore
code here goes the other way - and checked against that repository's vectors, which
`tests/data/tern_companion.json` and `tern_routing.json` carry verbatim.

- **Frames** are a type, a sequence byte and big-endian fields in a fixed order, at most 180
  bytes. The codec is one table, a row per type, which reading and writing both walk. A reader
  ignores bytes past the fields it knows; a request the client cannot read is the node's
  problem, news of a type it does not know is ignored.
- **Versions.** HELLO carries the client's (`MESH_TERN_VERSION`, 3) and INFO the node's, and both
  ends speak the lesser. Every frame is read by that version - each row and field of the table
  carries the version that defines it - so a version 2 node's SYNCED is its two bytes, and news
  of a type the version does not define is unknown before its bytes are looked at, even cut
  short. A request the version lacks (END_SESSION before 1, a group request before 2) is refused
  with `-EOPNOTSUPP` and never sent.
- **Serial and TCP** frame as `0xF5 0x54`, a big-endian length, the frame and a CRC-16/IBM-3740
  over the length and the frame: `mesh_stream_framing_tern`. `0xF5` never occurs in UTF-8, so
  the node's console text, which shares the port, never starts a frame; a header with a bad
  length or CRC gives up its first byte as text. The parser keeps no clock, so the draft's
  `GAP` (a half frame idle 500 ms is text) is not applied on this side.
- **BLE** is Tern's own service, `7A280001-…`: write `…0002`, notified on `…0003`, one frame
  each and no stream wrapping (`mesh_ble_profile_tern`, in `mesh_ble_known_profiles[]`). The
  node refuses HELLO with ERROR 7 on an ATT MTU under 183, which the client takes as a failed
  connect; BlueZ and CoreBluetooth both negotiate more by themselves.
- **One request at a time.** HELLO, SET_TIME when the clock is credible, SYNC; then whatever the
  user asks, each written only once the last is answered. An answer carries its request's
  sequence byte, so one that arrives after the request was given up on is ignored. A request
  unanswered for `ANSWER_WAIT` (5 s; a sync's wait restarts with each news frame) is `silent()`,
  and the link is dropped and reopened. With nothing to ask, a PING goes `IDLE` (20 s) after the
  last answer - over USB that is the only way the node knows the client is still there, and
  ERROR 6 afterwards means it decided otherwise: the conversation says HELLO again.
- **News is counted.** The node numbers its news from 0 after HELLO. A gap, news of a known type
  that cannot be read, or a SYNCED whose count (version 3) is not the one expected is news
  missed: the sync under way forgets nothing on its account, and another is asked. It asks after
  the greatest id held when news was first missed, or one less than the least id that may still
  change (`mesh_tern.open`: a message or invite waiting or sent, a group message waiting,
  anything received and unread), whichever is lower; that mark holds until a sync finishes. A
  connection starting counts as news missed, and a sync at a later version than the last one
  asks from 0, once.
- **A node is its routing id** (`mesh_tern_routing_id()`, SHA-256 of `"tern routing id"` and
  the address). It is the one number a CONTACT and a MESSAGE (which carry a 32-byte address) and
  a NEIGHBOUR (which carries only the id) all lead to, so the three land on one roster entry.
  The address is the entry's `public_key`; `user_id` is its first six bytes in hex until the
  draft says what a short code is.
- **A sync is the whole of three lists.** A contact or neighbour it did not send is no longer the
  node's: it stays on the roster, which outlives the radio's lists, with `in_nodedb` off and a
  contact's name gone with it. Groups are the third, but only from version 2: an earlier sync
  sends none, which says nothing of whether they are gone.
- **Groups** (version 2) are kept in the conversation's own lists - `groups`, and the newest
  group messages and invites in `group_items` - and go no further: nothing in the model would
  show a group as anything but a channel or a direct message, so no screen shows them yet. The
  requests are there (`mesh_tern_make_group()` and the rest); ASKED, the node refusing first
  contact, is kept as `has_asked` for the screen that will offer to save the address.
- **A message's id is the node's.** Its packet id in the log is that id scoped to the node
  (`mesh_tern_packet_id()`: XORed with an odd multiple of the node's routing id, since every
  node counts from 1), so a record seen again - a sync after a reconnect, a received message
  marked read - lands on the entry it already has, the cache agrees with it across a restart,
  and two nodes on one run never share one. So a send logs nothing: the SEND's QUEUED names
  the id, the MESSAGE news that follows is the bubble, and `mesh_tern_take_queued()` hands the
  app the send's ticket and id once the message is in the log, for the delivery watch. A refused
  send has no bubble and is said as a toast. A message handed back by a sync is `replayed`, so a
  reconnect does not announce the history again.
- **Delivery** is the node's state: waiting is `MESH_MESSAGE_ACK_WAITING` (with the reason in
  `ack_error`), sent is `PENDING`, delivered and not delivered are `DELIVERED` and `FAILED`.
  Waiting is the one state no other protocol reports.
- **Airtime** is AIRTIME kept whole on `mesh_tern.airtime`, and the model's `air_util_tx` is the
  share of the region's period this node spent sending. POWER is our node's battery.

The `tern` row in `src/ui/tables/protocols.c` lacks every feature: the protocol has no
channels, no link format and no firmware feed, and nothing in the UI yet sends SAVE_CONTACT,
REMOVE_CONTACT, END_SESSION, READ, SET or a group request. Settings show nothing of SELF yet.
`tests/suites/tern_codec.c` runs every companion vector but `group_ids` (a client never holds a
group's secret); `tests/suites/tern.c` replays the spec's exchange and its older clients'
connections through the conversation and holds the rules around them.
