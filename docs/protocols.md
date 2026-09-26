# Protocols

A **transport** is how bytes reach a radio (BLE, USB serial, TCP). A **protocol** is what those
bytes say. This client speaks two: Meshtastic (`struct mesh_session`) and MeshCore
(`struct mesh_meshcore`, [below](#meshcore)). The line between a transport and a protocol is
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
| `MESH_BLE_INBOUND_NOTIFY` | the frame itself; nothing is read | the Nordic UART shape, which is MeshCore's |

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
| `src/app/app_mqtt.c`, `src/core/firmware/firmware_catalog.c` | Meshtastic's MQTT client proxy and its firmware release feed |

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
| `WAYPOINTS` | "Send a waypoint" on a node's sheet; the Nodes list's Waypoints row dims and a press says why, since every row under it is counted from it (`MESH_UI_NODES_LEAD_ROWS`) |
| `TRACEROUTE` | the traceroute verb |
| `NODE_REQUESTS` | asking a node for its name, position or telemetry |
| `NODE_FLAGS` | pin, mute, ignore |
| `NODE_REMOVE` | remove, on a node's sheet |
| `REMOTE_ADMIN` | configuring a node over the mesh |
| `KEY_VERIFICATION` | the verify-key ceremony |
| `CHANNEL_LINKS` | the channel share QR and import rows |
| `CONTACT_LINKS` | the contact share and import rows, and "put back on the radio" |
| `MODULES` | the Modules row in Settings |
| `REACTIONS` | React on X, and X itself inside a thread |
| `RADIO_FIRMWARE` | the radio firmware check and install rows |
| `FULL_CONFIG` | every Settings section but User, Position, LoRa and Channels, and inside those every row but the name, the coordinates, the frequency, bandwidth, spread factor, coding rate and power, and a MeshCore channel's name and key; the Radio details' capability, reboot-count and admin-session rows |
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
  `MESHCLIENT_PROTOCOL=meshtastic|meshcore` skips the question.
- **One command at a time.** Each `CMD_*` is answered by one `RESP_CODE_*` (the contact list by a
  start, a record each and an end), and the firmware's BLE queue holds four frames, so the
  conversation queues commands and writes the next only once the last is answered.
  `PUSH_CODE_*` (0x80 and up) arrive whenever they like. A command unanswered for ten seconds is
  given up on; two in a row is `silent()`.
- **Connecting** walks `DEVICE_QUERY` (asking for version 3, which puts an SNR on messages),
  `APP_START`, `SET_DEVICE_TIME` when the clock is credible, `GET_CONTACTS`, then `GET_CHANNEL`
  for each slot up to the lesser of the radio's count and `MESH_SESSION_MAX_CHANNELS`. Then the
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
- **An advert** is this radio announcing its name and key now: Radio details offers it to the
  nodes in earshot or flooded across the mesh. Meshtastic has no such verb, so the two rows are
  listed by `protocol`, not by a lacked feature.
- **A reboot is never answered.** Over a USB-serial bridge the port outlives the ESP32 behind it,
  so once the answer is overdue the conversation runs its handshake again by itself.
- **A direct message** is pending until `PUSH_CODE_SEND_CONFIRMED` carries the four bytes the
  `RESP_CODE_SENT` named. It is tried three times with the same timestamp - the last after
  `CMD_RESET_PATH`, so it floods - and then failed. A channel message gets `OK` and nothing more.

Not yet spoken: repeater and room-server login, telemetry and status requests, trace paths,
adding a heard node as a contact and a contact's favourite flag, contact sharing, and the settings SELF_INFO
carries but the tab does not show (advert location policy, auto-add, multi-acks, telemetry
modes). The `meshcore` row in `src/ui/tables/protocols.c` hides the verbs those would back.
`tests/suites/meshcore.c` holds the frames a Heltec V3 sent and drives the conversation end to
end.
