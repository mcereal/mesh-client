# Protocols

A **transport** is how bytes reach a radio (BLE, USB serial, TCP). A **protocol** is what those
bytes say. Today there is one protocol, Meshtastic, and one conversation, `struct mesh_session`.
The line between the two is `include/mesh/core/protocol.h`, and this page says what sits on
each side of it and what is still Meshtastic's everywhere else.

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

## MeshCore

For reference when a second protocol arrives. Checked against MeshCore's
`examples/companion_radio/` source; re-check it before building on any of it.

- **BLE** is the Nordic UART Service (`6E400001-…`, RX `…0002`, TX `…0003`). One frame per
  write or notify, no length prefix, and inbound frames are notified directly rather than read:
  a `MESH_BLE_INBOUND_NOTIFY` profile, which `tests/suites/protocol.c` already drives.
- **Serial and TCP** frame as `'<'` (app to radio) or `'>'` (radio to app), then a 16-bit
  little-endian length, then the frame. As a `struct mesh_stream_framing`, it has no
  `wake_byte`.
- **A frame** is a one-byte code and packed little-endian fields. Commands are `CMD_*` (`1`
  `APP_START`, `2` `SEND_TXT_MSG`, `4` `GET_CONTACTS`, `10` `SYNC_NEXT_MESSAGE`, `22`
  `DEVICE_QUERY`, `31` `GET_CHANNEL`, …). Replies are `RESP_CODE_*`, and unsolicited events are
  `PUSH_CODE_*` at `0x80` and up.
- **The conversation is pulled.** `begin` would be `APP_START` then `DEVICE_QUERY`, and the rest
  is requested: contacts, then each channel slot. `PUSH_CODE_MSG_WAITING` means "call
  `SYNC_NEXT_MESSAGE` until `NO_MORE_MESSAGES`".
- **A node is a 32-byte public key**, addressed by a prefix of it. The store already keeps a
  node's 32-byte key beside its `node_id`, so one option is to keep `node_id` as a local handle
  derived from the key and treat the key as the identity.
