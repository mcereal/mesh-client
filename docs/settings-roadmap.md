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
   `Data.request_id`), and every admin reply carries a `session_passkey`.
2. Edit locally.
3. `begin_edit_settings`, one or more `set_*` carrying the passkey, `commit_edit_settings`.
   The radio reboots on commit for LoRa, Bluetooth, Security and a few other sections.

Firmware 2.5+ rejects a `set_*` without a fresh passkey (they live five minutes), which is
why every phase keeps the passkey dance live rather than bolting it on at the end.

We already receive most of this without asking: the `want_config_id` handshake streams every
`Config` and `ModuleConfig` section, the channel table, `DeviceMetadata` and our own `User`
(inside our `NodeInfo`). Phase 1 keeps those instead of throwing away all but the last
fragment, and uses the admin path for refreshes and as proof that writes will work later.

## Where each requested feature lands

| Feature | Protobuf | Notes |
|---|---|---|
| Short / long name, licensed operator | `User.short_name` (4 chars), `long_name` (39), `is_licensed`; written with `set_owner` | `is_unmessagable` is only on the device-side `UserLite` in v2.8.0, not on the wire `User`; revisit after a protobuf bump. |
| Bluetooth PIN mode | `BluetoothConfig.mode` (`RANDOM_PIN`, `FIXED_PIN`, `NO_PIN`), `fixed_pin` | Changing it invalidates the BlueZ pairing. The UI must say so and point at `bluetoothctl pair`. |
| Device role | `DeviceConfig.role`: CLIENT, CLIENT_MUTE, ROUTER, ROUTER_CLIENT (deprecated), REPEATER (deprecated), TRACKER, SENSOR, TAK, CLIENT_HIDDEN, LOST_AND_FOUND, TAK_TRACKER, ROUTER_LATE, CLIENT_BASE | Hide the deprecated two. |
| Time zone | `DeviceConfig.tzdef`, a POSIX TZ string (65 bytes) | Ship a preset list (US zones, UTC, common EU/APAC); no free text. |
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
  (read-only), `toggle`, `enum`, `text`, `number`, `key`, `action`. Phase 1 renders every
  kind read-only; later phases wire up A per kind: toggles flip, enums open the existing
  picker overlay, text reuses the on-screen keyboard, numbers get a stepper, keys show a
  fingerprint with reveal/regenerate behind a confirm, actions run a command.
- Edits accumulate in the nav (pending values per item) until **Save**, which sends
  `begin_edit_settings` / `set_*` / `commit_edit_settings`. A dirty section shows a marker;
  B on a dirty section asks discard/keep.
- Sections that reboot the radio get a **confirm** screen (a new overlay, two rows) that
  states what will happen, and after commit the HUD shows "radio rebooting, reconnecting"
  until the link poller and auto-connect bring it back.

New primitives this needs beyond what exists: confirm overlay, number stepper, hex key
entry (the keyboard's symbol layer covers it), and per-item pending values in the nav.

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

### Phase 2 - low-risk writes

User (names, licensed), Display, Telemetry, Store and Forward. Introduces pending edits,
Save, toggles, enums, number stepper, text via keyboard, and the
`begin_edit_settings`/`commit_edit_settings` bracket. None of these reboot the radio.

### Phase 3 - Channels and Bluetooth

Channel name/role/PSK/uplink/downlink/position precision with generated keys; Bluetooth PIN
mode with the re-pair warning. Bluetooth changes reboot the radio; this phase adds the
confirm overlay and the "rebooting, reconnecting" state.

### Phase 4 - LoRa and Security

Region, preset, hops, MQTT flags, TX enable, coding rate; public/private key view, key
backup, admin keys, regenerate key. All behind confirm screens with explicit consequences.

### Later, maybe never

Firmware install. Also `tzdef` presets beyond a short list, `Position` and `Power`
sections (shown read-only from phase 1, not edited), `Network` (WiFi credentials on a device
with no WiFi of its own is a poor fit).
