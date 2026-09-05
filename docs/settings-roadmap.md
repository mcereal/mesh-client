# Radio settings roadmap

Where the "configure the radio from the Brick" work is going, why it is ordered the way it
is, and what each phase has to prove before the next one starts. Field names below are the
ones in the Meshtastic protobufs we vendor (`proto/meshtastic`, v2.8.0); check them against
the submodule before relying on them after a bump.

## The one mechanism

Everything in this roadmap except a firmware *install* is a field of `Config`,
`ModuleConfig`, `Channel` or `User`, read and written with an `AdminMessage` on the
`ADMIN_APP` port, addressed to our own node number. The flow is always:

1. `get_*_request` → the radio answers with `get_*_response` (correlated by
   `Data.request_id`), and every `get_*` reply carries a `session_passkey`.
2. Edit locally.
3. A `set_*` carrying the passkey and the **whole** section (the firmware assigns
   `config.display = received`, it does not merge; `set_owner` is the exception and merges
   names/flags, ignoring empty strings). The radio answers a `set_*` with a `ROUTING_APP`
   packet quoting our packet id: `error_reason` NONE is the ack, `ADMIN_BAD_SESSION_KEY` or
   `BAD_REQUEST` a rejection. No AdminMessage comes back for a set.
4. We follow every set with the matching `get_*` so the tab shows what the radio actually
   kept.

Passkeys: the firmware accepts a key for 300 s from generation and rotates it when a `get_*`
arrives more than 150 s after the last rotation. A key we hold may therefore be stale, so
every write is preceded by a `get_owner_request`; the extra round trip is cheap.

Reboots (checked against firmware `master`, `AdminModule::saveChanges` defaults to
`shouldReboot = true`): `set_owner` reboots when anything changed; every `set_module_config`
reboots; `set_config` DISPLAY reboots only when `screen_on_secs`, `flip_screen`, `oled` or
`displaymode` change; LoRa, Bluetooth, Security, Device, Position, Power, Network all reboot.
The reboot fires 7 s after the set (after our read-back has been answered), BLE is disabled
first, the link drops, and auto-connect reconnects. `begin_edit_settings` /
`commit_edit_settings` only coalesce several sets into one reboot; since a save is one
section at a time we do not use them yet.

We already receive most of this without asking: the `want_config_id` handshake streams every
`Config` and `ModuleConfig` section, the channel table, `DeviceMetadata` and our own `User`
(inside our `NodeInfo`). Phase 1 keeps those instead of throwing away all but the last
fragment, and uses the admin path for refreshes and as proof that writes will work later.

## Where each requested feature lands

| Feature | Protobuf | Notes |
|---|---|---|
| Short / long name, licensed operator, unmessageable | `User.short_name` (4 bytes), `long_name` (39 on the wire, 24 kept by the firmware), `is_licensed`, `is_unmessagable` (optional, so `has_is_unmessagable` must be set); written with `set_owner` | The firmware ignores empty names and rejects all-whitespace ones with `BAD_REQUEST`. |
| Bluetooth PIN mode | `BluetoothConfig.mode` (`RANDOM_PIN`, `FIXED_PIN`, `NO_PIN`), `fixed_pin` | Changing it invalidates the BlueZ bond. The UI says so and points at Devices → Y (forget), then connect again to pair with the new PIN. |
| Device role | `DeviceConfig.role`: CLIENT, CLIENT_MUTE, ROUTER, ROUTER_CLIENT (deprecated), REPEATER (deprecated), TRACKER, SENSOR, TAK, CLIENT_HIDDEN, LOST_AND_FOUND, TAK_TRACKER, ROUTER_LATE, CLIENT_BASE | Shipped in phase 5. The deprecated two are *marked*, not hidden: they are still in the protobuf and still what an older radio reports, so a hidden value would leave the row unable to show the setting the node already has. |
| Time zone | `DeviceConfig.tzdef`, a POSIX TZ string (65 bytes) | Shipped as free text on the keyboard; a preset list would still be kinder. |
| Compass, 12 h clock, units, screen on, carousel | `DisplayConfig.compass_orientation`, `use_12h_clock`, `units`, `screen_on_secs`, `auto_screen_carousel_secs` | Plain enums, toggles and number steppers. |
| Store and Forward | `ModuleConfig.store_forward.enabled` (+ `heartbeat`, `is_server`) | |
| Telemetry device metrics | `ModuleConfig.telemetry.device_telemetry_enabled`, `device_update_interval` | The radio also reports environment/power/air quality toggles; show them, edit later. |
| LoRa | `LoRaConfig.region`, `use_preset`, `modem_preset`, `hop_limit`, `tx_enabled`, `bandwidth`/`spread_factor`/`coding_rate`, `ignore_mqtt`, `config_ok_to_mqtt` | Reboots on commit. A wrong region or preset silently drops you off the mesh. Confirm screen required. |
| Channels | `set_channel` with `Channel.index`, `role`, `settings.name`, `psk`, `uplink_enabled`, `downlink_enabled`, `module_settings.position_precision` | Key size is just the PSK length: 1 byte = default key index, 16 = AES-128, 32 = AES-256. Keys are typed as hex or generated; never shown in full on screen by default. |
| Security | `SecurityConfig.public_key`, `private_key`, `admin_key[3]`, `is_managed`, `admin_channel_enabled`, `packet_signature_policy` | Regenerating the private key breaks every peer's key for you. Backup = show/export the key; do it before regenerate is offered. |
| Firmware, show | `DeviceMetadata.firmware_version`, `hw_model`, plus `LoRaConfig.region` | Arrives in the handshake; `get_device_metadata_request` refreshes it. |
| Firmware, install | ESP32: `reboot_ota_mode` then a separate BLE OTA protocol. nRF52: `enter_dfu_mode_request` then Nordic DFU over BLE. | Large, hardware-specific, can brick the radio. Deferred indefinitely; we will show the version and point at the web flasher instead. |

## UI model

Twelve hand-drawn screens is the wrong shape. The Settings tab is one generic form renderer
over a static description of each section:

- A **section list** (Radio, User, Device, Display, LoRa, Bluetooth, Channels, Security,
  Modules). A opens a section, B returns.
- Each section is a list of **items**: label, current value, and a kind. Kinds are `info`
  (read-only), `toggle`, `enum`, `text`, `number`, `key`, `action`. An editable item names
  its `field`; a table in `src/ui/settings.c` describes every field (kind, enum names, number
  presets, text cap) so the nav edits blind: Left/Right/A flip toggles, cycle enums and step
  numbers through presets; A on text opens the on-screen keyboard retargeted at the field.
  Keys will show a fingerprint with reveal/regenerate behind a confirm, actions run a command.
- Edits accumulate in the nav (pending values per field) until **Y** saves the section as
  one `set_*`. Dirty rows carry a marker and the title says `(unsaved)`; B on a dirty section
  asks once and discards on the second press.
- Sections that reboot the radio get a **confirm** screen (a new overlay, two rows) that
  states what will happen, and after commit the HUD shows "radio rebooting, reconnecting"
  until the link poller and auto-connect bring it back.

New primitives this needed beyond what existed: confirm overlay, number presets, hex key
entry through the keyboard, and per-field pending values in the nav (all in place as of
phase 3).

## Phases

Each phase ships on its own and is tested on the Brick before the next starts.

### Phase 1 - read everything, prove the admin path (this branch)

- Compile `admin.proto` (and `connection_status.proto`, which it imports).
- New core module `src/core/radio_settings.c`: keeps every `Config`/`ModuleConfig`
  section, `DeviceMetadata` and our `User` as they stream in; encodes `AdminMessage`
  requests into `ToRadio` packets; decodes `ADMIN_APP` replies, stores the session passkey,
  correlates by `request_id`; runs a one-at-a-time fetch queue with a timeout.
- BLE transport feeds it and, once the handshake completes, sends one admin probe
  (`get_device_metadata_request` and `get_owner_request`) so the log shows the passkey
  round trip on real hardware. X on the Settings tab refreshes every section through the
  admin path.
- Settings tab with the section list and read-only items for every section above,
  including channels (name, role, PSK size, uplink/downlink) and firmware/hardware.
- Exit criteria: on the Brick, the log shows `Admin reply ... session passkey held`, every
  section shows real values, and X refreshes them without disturbing the message log.

### Phase 2 - first writes (this branch)

User (names, licensed, unmessageable), Display, Telemetry, Store and Forward. Introduces the
field table, pending edits, Y to save, toggles, enums, number presets, text via the keyboard,
the passkey-refresh / set / read-back sequence, Routing-ack correlation, and the save-outcome
toasts ("saved; radio may restart", "rejected: session expired", "restarting to apply;
reconnecting"). The original premise that these sections do not reboot the radio was wrong
(see "The one mechanism"); the reboot is handled by the existing link poller and auto-connect
rather than avoided.

- Exit criteria: on the Brick, flipping a Display toggle and pressing Y shows the saving toast,
  the log shows `Admin request N acknowledged`, the row shows the new value after the
  read-back, the radio reboots and the client reconnects with the value still set; renaming
  the node via the keyboard shows the new name in the phone app.

### Phase 3 - Channels and Bluetooth (this branch)

The core keeps the radio's full channel table (`FromRadio.channel` during the handshake,
`get_channel_response` on refresh; `get_channel_request` is one-based on the wire) and writes
a slot with `set_channel` carrying the whole `Channel`, id included. In the Channels section every slot
is listed, empty ones included, and A opens it (adding a channel is setting up an empty slot;
removing one is setting its role to Disabled): name (11 bytes), role (Disabled/Secondary; the primary slot's role is shown
read-only so a mesh cannot be left with two primaries or none by accident), key, MQTT uplink
and downlink, position precision (presets 0, 10..19, 32 labelled by distance). The key row is
a new kind: Left/Right walk keep / default key / new random AES-128 / new random AES-256 / no
encryption, and A opens the keyboard on the current key as hex (the one place it is revealed)
to type or copy one; random keys are drawn with `getrandom()` when the write is built.
Bluetooth: enabled, pairing mode, fixed PIN (six digits, validated before the write).

Both sections sit behind a **confirm overlay**: Y opens "Save channel N?" / "Save Bluetooth?"
with the consequence spelled out (reboot; forget and re-pair from Devices; a new key or name moves
the radio to another channel), cursor on Cancel, A on "Save to radio" emits the write.

- Exit criteria: on the Brick, a secondary channel renamed and re-keyed from the Brick shows
  the same name and key fingerprint in the phone app after the reboot; switching the pairing
  mode to No PIN, forgetting the node in Devices and connecting again to re-pair, works.

### Phase 4 - LoRa and Security (this branch)

LoRa: region (all 38 codes), use preset, preset, bandwidth / spread factor / coding rate
(always listed so the row count is stable while the preset toggle is edited), hop limit,
transmit, TX power (0 = the radio's maximum), ignore MQTT, OK to MQTT. The firmware does not
reject a LoRa write; it only coerces an out-of-range spread factor or coding rate back to the
default, and any RF-relevant change reboots.

Security: the public key is shown as a fingerprint. The private key is a KEY row with keep /
new random key, and A reveals it as base64 for backup or lets a backed-up key be typed back
in. A new key is 32 random bytes clamped the Curve25519 way (the firmware does not clamp a
client-supplied key) and is sent with the public key cleared: the firmware derives the public
key itself (`regeneratePublicKey`) and copies it into the owner record. Three admin key rows
(keep / none / typed) with the repeated field compacted on save; managed mode, admin channel,
serial console, debug log API toggles; packet signature policy. The firmware refuses managed
mode without an admin key, and managed mode locks out any client whose key is not listed,
this one included. Key and admin-key changes do not reboot; the serial and debug-log toggles
do.

Keys everywhere are shown and typed as base64, what the phone apps use, so a key read off the
Brick can be entered in the app and vice versa; hex is accepted when typing too.

- Exit criteria: on the Brick, a LoRa hop-limit change survives the reboot and shows in the
  phone app; the private key revealed on the Brick matches the app's; an admin key typed from
  the app's public key lets that phone administer the node.

### Phase 5 - Device, Position and Power (this branch)

The three sections that were listed read-only become editable through the same field table,
with no new UI primitives: `Device` gains role (all thirteen values, the two deprecated ones
labelled "(retired)" rather than hidden - a radio already set to one has to be able to show
it), rebroadcast mode, NodeInfo broadcast interval, the LED heartbeat and the double-tap
toggle; `Position` gains GPS mode, broadcast interval, smart broadcast with its distance and
interval thresholds, fixed position and the GPS update interval; `Power` gains power saving,
light sleep, minimum wake, the Bluetooth wait and the shutdown-on-battery timer.

`LED heartbeat` is the one row shown inverted: the protobuf field is
`led_heartbeat_disabled`, and a row reading "LED heartbeat off = on" would be nonsense, so
`app.c` negates it on the way back. The smart-broadcast thresholds stay listed whether or not
smart broadcast is on, the same rule the LoRa trio follows, so the row count does not move
under the cursor mid-edit.

`Power` joins the sections behind the **confirm overlay**. It is the only one of the three
that can cut this client off: power saving and a short light-sleep or minimum-wake leave the
radio's Bluetooth off for most of every cycle, and auto-connect cannot fix a radio that is
asleep. Device and Position only reboot, which the link poller and auto-connect already
handle.

- Exit criteria: on the Brick, a role change survives the reboot and shows in the phone app;
  turning smart broadcast on with a 250 m threshold reads back with the threshold intact and
  the position fields we do not show untouched.

### Phase 6 - MQTT (this branch)

The last read-only section. Enabled, server, username, password, root topic, encryption, TLS
and map reporting all become editable through the field table; `json_enabled` is skipped
because the firmware removed JSON support and ignores the field.

Two rows are not what the protobuf suggests. `map_reporting_enabled` is labelled **Report to
public map**, because that is what it does and "map reporting" is not what a person stepping
through toggles would read it as. And `proxy_to_client_enabled` stays **read-only**: with it
on the radio stops talking to the broker itself and hands every message to the attached
client as a `MqttClientProxyMessage` (`FromRadio` tag 14) to relay, and this client ignores
that variant entirely - so offering the toggle would let the Brick silently take the radio's
MQTT off the air. It is still shown, because it is the answer when a phone left it on and
MQTT stopped working. It becomes editable if we ever speak the proxy protocol.

MQTT is not behind the confirm overlay. It reboots the radio, which auto-connect handles, and
it cannot cut this client off or take the radio off the mesh - the rule the overlay follows.

- Exit criteria: on the Brick, a username and root topic typed on the keyboard read back
  after the reboot with the server and the proxy flag untouched.

### Phase 7 - Radio actions (this branch)

The first section that writes nothing. Reboot, shutdown, reset the node database, factory
reset the config, factory reset the device: five `AdminMessage` verbs (`reboot_seconds`,
`shutdown_seconds`, `nodedb_reset`, `factory_reset_config`, `factory_reset_device`) that make
the radio *do* something instead of keeping something.

Three things follow from that and are the whole of the design:

- **Nothing is read back.** A rebooting radio has no state to re-read and a factory-reset one
  has none we would recognise, so the queue shape is the clock push's: a `get_owner_request`
  for a passkey the firmware will still accept, then the verb. The firmware checks the session
  passkey on these exactly as it does on a `set_*`.
- **They are not writes.** `mesh_admin_request_is_action` marks them and
  `mesh_admin_request_is_write` continues not to, for a sharper version of the reason the
  clock push and the favorites are excluded: the radio stops answering *in the middle of doing
  what it was asked*, so a Routing ack that never lands is the action working. Counting it as
  a rejected save would make the Settings tab announce a failure every time a reboot succeeded.
- **The reboot and the shutdown carry a delay** (`MESH_RADIO_ACTION_DELAY_SECONDS`, 5).
  Zero is refused: the firmware answers before it acts, and the ack has to get out of the door
  while the radio is still listening.

Nothing about the UI is new either, except that the **confirm overlay had to stop being
hard-wired to a section save**. It now carries `nav.confirm_action`: NONE is the save it always
was, anything else is the radio action A on the row put there. The title, the body and the verb
on the accept row all come from `mesh_ui_settings_confirm_*` in `src/ui/settings.c` rather than
from the backend, since the overlay now says five things it did not before. Every row in the
section goes through it - A on the row opens the question, and only the answer emits anything.

The section is last in the list so a cursor that overshoots lands on nothing worse than the row
above it, its rows run least to most destructive, and it needs no config fragment to render:
the one thing it waits on is our own node number, which is what an `AdminMessage` cannot be
addressed without. `Shutdown` reads "not supported" when `DeviceMetadata.can_shutdown` says the
board cannot cut its own power, rather than offering a press the firmware would drop.

Reconnection is the existing machinery: a reboot drops the link and auto-connect brings it
back. A shutdown does not, and the toast says so - there is nothing to reconnect to until
somebody presses the radio's own button. A `factory_reset_device` clears the BLE bond, so the
toast points at Devices → Y.

- Exit criteria: on the Brick, Reboot restarts the radio and auto-connect reconnects with
  everything intact; Shutdown powers it off and the toast does not promise a reconnect; the
  node database reset empties the radio's list with favorites still in it, and the Brick's own
  cached list survives.

### Later, maybe never

Firmware install. Also `tzdef` presets beyond typing the POSIX string by hand, `Network`
(WiFi credentials on a device with no WiFi of its own is a poor fit), MQTT client proxying,
and closing channel gaps the way the phone apps do when a middle slot is removed.

The admin verbs deliberately left out of phase 7: `enter_dfu_mode_request` and `ota_request`
(firmware install, above), `exit_simulator`, and `reboot_ota_seconds` (deprecated upstream in
favour of `reboot_ota_mode`). `remove_by_nodenum` and `toggle_muted_node` belong on the Nodes
tab beside the favorite and ignore rows rather than in a settings section, so they are not
here either.

### Done outside the phases

The **clock**. A node with no GPS that has never had a phone attached sits at 00:00, and every
packet it hands us then carries `rx_time` 0, so the UI can say nothing about when anything
arrived. `mesh_session_sync_clock` pushes the Brick's own time at the radio once per
connection as `AdminMessage.set_time_only`, behind a `get_owner` for a fresh passkey and with
no read-back (there is no `get_time`). It is gated on `MESH_RADIO_CLOCK_MIN_EPOCH` so a Brick
that has lost its RTC pushes nothing, and it is deliberately excluded from
`mesh_admin_request_is_write` so it never toasts "saved" or claims the ", saving" marker that
belongs to the user's own save. `set_time_only` is UTC: the node shows local time only once
`Device` → Time zone (`DeviceConfig.tzdef`, a POSIX TZ string such as `AST4` or
`EST5EDT,M3.2.0,M11.1.0`) is set, which is the one editable row in the Device section.
