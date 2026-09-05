# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C17 Meshtastic client for the TrimUI Brick (NextUI/MinUI, platform key `tg5040`), shipped as a
`MeshClient.pak`. Single-threaded epoll event loop, BlueZ-over-D-Bus BLE transport, nanopb for
Meshtastic protobufs. `AGENTS.md` is the contributor guide (style, tests, PR expectations) and
still applies; this file adds what's not obvious from it.

## Commands

**The core is Linux-only** (`epoll`, `timerfd`, `eventfd`). This repo is developed on macOS, so
build and test through the containers. On a Linux host (including a Claude Code on the web
session) `make setup` provisions the same prerequisites natively and the plain `make debug` /
`make test` targets work directly - no Docker needed; the `.claude/hooks/session-start.sh`
SessionStart hook runs that setup automatically for remote sessions. `make docker-*` wraps `scripts/docker.sh`, which builds the
image from `docker/Dockerfile` on first use and bind-mounts the repo at `/src`. Container builds
use `BUILD_ROOT=build/linux` so outputs land in `build/linux/{debug,release}`.

```bash
git submodule update --init --recursive   # nanopb, meshtastic/protobufs, NextUI — CMake FATAL_ERRORs without them
make docker-test                          # Debug build + ctest in the dev container (the default verify step)
make docker-debug                         # Debug build only
make docker-shell                         # bash in the dev container; run cmake/ctest/binaries by hand
make docker-pak                           # cross container: static aarch64 build → dist/MeshClient.pak.zip
make docker-image / docker-cross-image    # force image rebuild after editing docker/
make format                               # clang-format all tracked .c/.h (runs fine on the host)
make brick                                # docker-pak + push to the Brick over SSH (needs .brick.env, see docs/device.md)
make deploy / deploy-logs / deploy-check  # push only / tail device log / report BlueZ, D-Bus, fb0 state on device
make deploy-shot ARGS="-d 10 -o x.png"    # screenshot the device's screen off /dev/fb0 (page 0; -P 1 is the launcher)
make deploy-run ARGS="--list-devices"     # run launch.sh on the device headless, streaming output
```

Device deploys go through `scripts/deploy-device.sh` over SSH (dropbear "SSH Server" pak on the
Brick, busybox only: transfers are `tar | ssh tar`, no rsync/scp). Host settings live in the
gitignored `.brick.env`. The script quotes for POSIX `sh`, not bash; keep it that way.

Inside the container (or on a Linux host) the plain targets apply: `make debug`, `make test`,
`make release && make package`, `make proto`, `make run`,
`cmake --build $BUILD_ROOT/debug --target meshclient`,
`make debug CMAKE_ARGS="-- -DMESHCLIENT_ENABLE_ASAN=ON"` (or UBSAN).

Single test: the whole suite is one binary with a name filter, not per-test CTest entries.

```bash
./scripts/docker.sh ./build/linux/debug/tests/meshclient_core_tests --list
./scripts/docker.sh ./build/linux/debug/tests/meshclient_core_tests --filter ble_transport
```

Manual checks: `meshclient --list-devices` (BLE advertisers and USB ports), `--status --json`,
`--status --status-output PATH`, and `--serial[=ID] --status` to drive a USB node instead of BLE.
Inside the container there is no BlueZ, so the BLE transport sits in `waiting-for-bluez` and the
CLI backend is selected; without D-Bus headers at build time it compiles out entirely
(`MESH_HAVE_DBUS` is set only if `pkg-config dbus-1` succeeds) and reports `disabled`.

## Architecture

Data flows one direction: link (transport) → `mesh_session` → `mesh_app` → UI store → controller → backend.

- `src/core/event_loop.c` — epoll loop with a fixed table of 32 fd sources. Everything (D-Bus
  watches, timerfd discovery refresh, UI store eventfd, minui-list child stdout) registers here.
  No threads anywhere; do not add them.
- `src/transport/transport_registry.c` — `struct mesh_transport_ops {start, stop, status, tick}`
  plus the optional `set_session` and `take_error`.
  BLE and serial are registered today; HTTP is planned to plug in here. Both links own a
  `struct mesh_session`, so everything past the connect is shared.
- `src/transport/ble/bluez_client.c` — raw libdbus wrapper for `org.bluez` (adapter discovery,
  `GetManagedObjects`, GATT Connect/StartNotify/Write). Has a compile-time-independent mock
  (`mesh_bluez_client_mock_enable`) that tests use to script results and capture writes; there is
  no real BlueZ in CI.
- `src/core/session.c` — the Meshtastic conversation, independent of how bytes travel
  (`struct mesh_session`): the `want_config_id` handshake, a node-summary cache decoded from
  `FromRadio` (256 entries; every inbound `MeshPacket` also refreshes its sender's
  `last_heard`/SNR/hops, adding the sender by id if the sync never delivered its NodeInfo), the
  radio's channel table (`FromRadio.channel` by slot, role DISABLED kept so indices stay
  meaningful), the message log, the radio settings and admin queue pump, and packet ids.
  `struct mesh_node_summary` is the whole node record, not just a name: identity from
  `NodeInfo.user` (id, hw model, role, public key, licensed/unmessagable), the NodeDB flags,
  and `position`/`metrics`/`environment` sub-structs. The NodeDB replay fills what it carries
  and `mesh_session_apply_packet_details` keeps it current from the air - the firmware replays
  its database exactly once per connection, so `NODEINFO_APP`, `POSITION_APP` and
  `TELEMETRY_APP` packets are the only reason a node that joins mid-session ever gets a name
  and the only source of environment telemetry at all (the NodeDB has none). A resync therefore
  overwrites only what the NodeInfo actually carries rather than rebuilding the record, or it
  would empty the detail screen every time. `struct mesh_radio_stats` is the one telemetry
  that is *not* about a node: `LocalStats` is the connected radio describing itself and the
  air around it (packet counters, dupes, relays, online node count, heap, noise floor), so it
  lands on the session and is cleared with the handshake. The firmware sends it to the
  attached client alone, never over LoRa, so it is only taken from a packet whose `from` is
  our own node number - and its fields are plain proto3 scalars, so a zero heap size or a
  zero noise floor is "not reported", not a reading, which is why those two carry a flag and
  the counters do not. Battery is not in it at all - that arrives only as our own node's
  ordinary `DeviceMetrics`, which is why the Status screen reads it from the node cache. The
  airtime pair is in *both*, and LocalStats wins it: DeviceMetrics is what our node last
  broadcast about itself on the telemetry interval (half an hour by default), so preferring
  it leaves the row on a stale 0.0% while the radio is busy. A link
  calls `mesh_session_attach(send_fn)` when its connection is usable, hands every FromRadio
  protobuf to `mesh_session_handle_from_radio`, calls `mesh_session_tick` each turn, and
  `mesh_session_detach`es when the link drops (handshake and settings reset, messages survive).
  The session never sees GATT, ttys or framing; a link never decodes a protobuf. Today the BLE
  transport owns one session (`mesh_ble_transport_session`); the `mesh_ble_transport_*`
  send/settings/messages/handshake functions are thin wrappers over it.
- `src/transport/ble/ble_transport.c` — the BLE link: state machine (`disabled` →
  `waiting-for-bluez` → `waiting-for-adapter` → `running`), Meshtastic service UUID filtering,
  an outbound ToRadio packet queue (the session's send path), and the FromNum-notify →
  FromRadio-read drain loop. BLE is **not** Nordic UART and has no length
  framing: one bare protobuf per GATT write/read. `src/proto/framing.c` is a homegrown varint
  prefix that nothing on the wire uses; serial framing is `src/proto/stream_framing.c`.
  Pairing happens in the app: `bluez_client.c` registers an `org.bluez.Agent1` at
  `/org/meshclient/agent` with **KeyboardDisplay** capability, which is what makes a PIN-mode
  node (its own IO capability is DisplayOnly) choose passkey entry and ask *us* for the six
  digits on its screen. The agent's reply is **deferred** - the D-Bus call message is held
  (`agent_pending_message`) until the user has typed them - which is the whole reason a bond
  can span several event-loop turns without blocking. `Device1.Pair` is sent the same
  non-blocking way `Connect` is, and `Trusted` is set afterwards so the next connect needs
  neither. Two rules keep it predictable: only a connect the user asked for bonds
  (`mesh_ble_transport_connect_and_pair`, the Devices tab), because auto-connect's plain
  `mesh_ble_transport_connect` raising a PIN prompt over whatever the user was doing would be
  worse than the connect failing; and the pairing timeout stops while the agent is holding a
  question, or the prompt would cancel itself out from under the person reading the node's
  screen. `mesh_ble_transport_forget` is `Adapter1.RemoveDevice` - the fix when a node's PIN
  has changed under a bond BlueZ still believes in.
  `mesh_ble_transport_connect` sends `Device1.Connect` without blocking (reply matched by
  serial in `bluez_client.c`, 30 s cap) and returns 0 with the link in `connecting`; `tick()`
  then waits for `ServicesResolved` (250 ms polls, 20 s cap) before wiring the characteristics.
  `connected_address()` is NULL until then; `status()` reads `connecting`/`connected` meanwhile.
  Everything else on the bus (reads, writes, StartNotify) is still a blocking call. BlueZ never
  tells us about a dropped link (only characteristic properties are watched), so `tick()` reads
  `Device1.Connected` every 2 s while CONNECTED (`mesh_ble_transport_check_link`) and a failed
  GATT write also resets the link; queued messages are marked FAILED either way, and the
  message log survives the reset. Auto-connect then reconnects.
- `src/proto/stream_framing.c` — Meshtastic's serial/TCP framing: `0x94 0xC3 len_hi len_lo` plus
  one raw protobuf, 512-byte cap. The parser is incremental and resync-tolerant because the
  firmware interleaves its text log with the frames on the same port; junk between frames goes to
  a text callback (logged as `radio: ...` at debug), a split frame is held until the rest lands.
- `src/transport/serial/serial_usb.c` — finding and opening a USB port, and the Brick-specific
  part. The Brick's kernel has `CONFIG_USB_ACM` off, so a native-USB node gets no `/dev/ttyACM*`;
  the transport writes `VID PID` to `/sys/bus/usb-serial/drivers/generic/new_id` (the generic
  driver refuses the control interface and takes the data one as `/dev/ttyUSB0`), then sends one
  CDC `SET_CONTROL_LINE_STATE` through usbfs because the node discards output until DTR is
  asserted and the generic driver cannot assert it. Neither survives a reboot, so both happen on
  every connect. UART-bridge boards (cp210x/ch341/ftdi_sio) skip both and take a normal
  `TIOCMBIS`. Mockable (`mesh_serial_usb_mock_enable`) so tests never touch sysfs or usbfs; the
  mock's `open_fd` lets a test hand the link one end of a socketpair. Note the `MESH_IOCTL_REQUEST`
  shim: glibc's `ioctl` takes `unsigned long`, musl's takes `int`, and the USBDEVFS codes have the
  high bit set - the release build is musl, the dev container glibc, so both need narrowing.
- `src/transport/serial/serial_transport.c` — the serial link: sysfs scan (rescanned every 3 s
  while idle, never while a port is held), bind + DTR, open the tty raw at 115200, register the fd
  with the epoll loop, send the 32-byte `0xC3` resync burst the Meshtastic clients send, wait
  100 ms, then attach the session and run the same `want_config_id` handshake as BLE. Outbound
  packets are framed into a queue with a partial-write cursor; `EPOLLOUT` is armed only while
  that queue has a remainder. EOF or a fatal read/write error resets the link and marks queued
  messages FAILED, leaving the message log intact.
- `src/core/radio_settings.c` — transport-agnostic view of the connected radio's configuration
  (`struct mesh_radio_settings`: every `Config`/`ModuleConfig` section, owner `User`,
  `DeviceMetadata`) and the `AdminMessage` plumbing: encodes `ADMIN_APP` requests addressed to
  our own node with `want_response`, decodes replies (correlated by `Data.request_id`), keeps the
  `session_passkey` every reply carries (firmware 2.5+ rejects a `set_*` without it), and runs
  a one-at-a-time request queue with a 5 s timeout. The session feeds it handshake fragments
  and admin packets, sends a metadata+owner probe once `config_complete_id` arrives, and pumps
  the queue from `mesh_session_tick()`. Admin replies never reach the message log. Writes
  (`mesh_radio_settings_queue_write`) always go out as `get_owner` (fresh passkey; the firmware
  rotates it after 150 s), the `set_*` carrying the **whole** section (the firmware replaces, it
  does not merge), then the matching `get_*`. A `set_*` is answered by a `ROUTING_APP` packet
  quoting our id: `error_reason` NONE is the ack, `ADMIN_BAD_SESSION_KEY`/`BAD_REQUEST` a
  rejection; `ingest` claims those too and counts them in `writes_acked`/`writes_failed`. The
  full channel table (`has_channel[]`/`channels[]`, keys included, never persisted) is kept
  for `set_channel`, which must carry the whole `Channel`; `get_channel_request` is index+1.
  `MESH_ADMIN_SET_TIME` and the two favorite kinds are the odd ones out: `mesh_session_sync_clock` pushes the Brick's own
  `time(NULL)` at the radio once per connection (`set_time_only`, behind a `get_owner` for the
  passkey, no read-back - there is no `get_time`), so a node with no GPS stops sitting at 00:00
  and its packets carry a real `rx_time`. It is gated on `MESH_RADIO_CLOCK_MIN_EPOCH` and left
  out of `mesh_admin_request_is_write` on purpose, so it never counts as a save or toasts over
  one. `set_time_only` is UTC; the node shows local time only once `DeviceConfig.tzdef` is set,
  which is the Device section's one editable row. Most
  sections reboot the radio 7 s after a set (owner, module configs, display when
  `screen_on_secs`/`flip_screen` change), so the link drops and auto-connect reconnects; that is
  expected, not a bug. Phase status is in `docs/settings-roadmap.md`.
- `src/core/message.c` — transport-agnostic messaging: builds `TEXT_MESSAGE_APP` packets into a
  `ToRadio`, folds inbound `MeshPacket`s into a fixed ring (`mesh_message_log`), and correlates
  `ROUTING_APP` replies with the outbound message they ack. Message text is untrusted radio
  input: `mesh_message_ingest` sanitises control bytes so backends can draw it directly.
- `src/core/app.c` — `mesh_app_publish_ui_state()` copies BLE discovery/handshake state into the
  UI store every loop iteration, persists the handshake cache and preferences under `$HOME`
  (`~/.meshclient/ui_prefs`, `ui_prefs.handshake`), and picks the UI backend.
  `mesh_app_autoconnect()` runs every foreground turn: preferred node if in range, else the
  strongest advertiser after 30 s, exponential backoff on failure; `MESHCLIENT_AUTOCONNECT=0`
  turns it off. A BLE connect returns 0 several seconds before it is a connection, so neither
  the backoff nor the UI can key off that return value: `mesh_app_report_link_errors()` runs
  between `tick()` and `mesh_app_autoconnect()` (a retry restarts the link and clears the
  reason the last attempt failed), pops a transport's `take_error()` line, toasts it when the
  user asked for the connect, and counts the attempt against the backoff. Only an established
  link clears the backoff. The `--status`/`--list-devices` paths in `main.c` do their own connect.
- `src/ui/node_detail.c` — the Nodes tab's second level, the same list-of-rows shape Settings
  uses: `mesh_ui_node_detail_build` emits the rows one node produces (an action, then Identity /
  Signal / Device metrics / Position / Environment groups) and a row simply is not emitted when
  the node has not reported it, so the count the nav walks and the list the backend draws can
  never disagree. A opens the detail, its first row ("Message this node") opens the
  conversation, B backs out, Y still writes from either level, and X pins the node to the top
  of the list (`MESH_UI_ACTION_TOGGLE_FAVORITE` -> `mesh_session_set_node_favorite`, which
  queues `set_favorite_node`/`remove_favorite_node` and flips the cached flag itself because
  there is no get_favorite and the radio only returns the flag with that node's next NodeInfo).
  `mesh_app_node_rank` puts a pinned node at rank 1, above even a node you are mid-conversation
  with, which is also what keeps a quiet pinned node inside the UI's 128-node budget; the list
  marks it with a star sprite in the same column our own node's `*` uses. A pin is **NodeDB
  state on the radio it was made on** - `is_favorite` is resolved per receiver - so it never
  follows the Brick from one of your radios to another. That cuts both ways and only one half
  needs handling: the node you connect to is rank 0 (`*`) whatever its stale flag says, and
  `node_detail.c`/`nav.c` both refuse to pin our own node, so a leftover flag is inert; but the
  radio you just unplugged arrives on the new one as an ordinary stranger. Rank 2 is that case
  - `mesh_ui_preferences_note_radio` records every `my_node_num` we connect to in a small MRU
  in `ui_prefs` (`known_radios=`), and `mesh_ui_preferences_knows_radio` lifts those above
  message peers. It is client-side on purpose: no admin write, and nothing that could disagree
  with what "favorite" means on the radio.
  A on the detail's **"Trace route"** row measures the mesh itself rather than repeating what
  a node said about itself - `hops_away` is a count, not a route, and never says which nodes
  are carrying you. `mesh_session_send_traceroute` puts an empty `RouteDiscovery` on
  `TRACEROUTE_APP` with `want_response`; every node that forwards it appends itself and the
  target answers with both directions and the SNR of every link. The reply is matched on
  `Data.request_id` against our packet id, because a trace between two *other* nodes crosses
  our radio wearing the same portnum, and a RouteDiscovery never reaches the message log
  either way. One trace at a time (`-EBUSY`) - the client's half of the firmware's own rate
  limit, and the reason a finished result is kept rather than re-run, since a screen that
  traced on every repaint would be refused and would flood the mesh. Nothing reports a trace
  dropped on the way out or back, so `mesh_session_tick` times it out at 60 s ahead of the
  link guards. `mesh_app_flatten_traceroute` turns the protobuf's shape - intermediate nodes
  plus a parallel array of link SNRs - into the two paths the UI draws, every stop carrying
  the reading of the link that reached it: the ends are stitched on (us going out, the target
  coming back), each hop resolves to a name, and hop `i` takes `snr[i - 1]`. That array is
  normally one longer than the route, one reading per *link* rather than per node, but every
  pairing is bounds checked rather than assumed. `INT8_MIN` is the firmware's "not measured"
  and draws as "no reading", not a -32 dB link. The open node is remembered by
  **id** (`nav.node_detail_node`), not by row: `app.c` re-ranks the node list on every publish,
  so an index would slide onto a different node while the user was reading one; `nav.c`'s clamp
  closes the detail when that id leaves the list. `mesh_ui_node_summary` in `store.h` is the
  nanopb-free twin of the session's record, copied by `mesh_app_copy_node_detail` in `app.c` -
  field by field on purpose, because nothing else keeps the two declarations in step. The whole
  detail rides in the handshake cache as its own `node_user[i]`/`node_ident[i]`/`node_key[i]`/
  `node_pos[i]`/`node_metrics[i]`/`node_env[i]` key lines, so it is both browsable offline and
  compatible in either direction with a build that knows nothing about it.
- `src/core/updater.c` + `src/core/version.c` — the client updating itself, as opposed to
  everything else here which is about the radio. `mesh_version_string()` returns the
  `MESHCLIENT_VERSION` compile definition CMake feeds from `project(... VERSION ...)`, or
  `"dev"` for a build without one, and `mesh_version_compare()` is SemVer precedence including
  prerelease ordering - a `dev` build never offers to "update" itself to a release, which is
  what keeps a working tree from replacing its own binary. The updater has no TLS: it forks the
  device's `curl` (then `wget`) and reads its stdout through the event loop, the same shape
  `minui.c` uses for `minui-list`, because the release build is static musl with libdbus as its
  only dependency. One child at a time, states strictly sequential, `tick()` enforcing the
  timeout. **It also has to bring its own CA bundle**: the Brick has no system CA store at all
  - no `/etc/ssl` - so a bare `curl` fails every HTTPS request with exit 60, and busybox `wget`
  there has no HTTPS support whatever. So the pak ships Mozilla's roots at `certs/certificates.crt`
  (the same thing Pak Store does) and `updater_resolve_ca_bundle()` picks one: `SSL_CERT_FILE`
  or `CURL_CA_BUNDLE` first, then our bundle via `updater_pak_file()`, then the usual distro
  paths so a desktop build keeps using the system's. `--insecure` is **not** an alternative and
  must not be added - the release metadata is what carries the digest every download is checked
  against, so trusting it unauthenticated would defeat the verification rather than route around
  a missing file. Because the bundle ships in the pak and not through self-update, a device
  installed before it has to reinstall the pak once; curl's exit 60 is mapped to
  "No CA certificates; reinstall the pak" so the About screen says so.
  What makes downloading an executable safe is not the transport but the digest: the
  release metadata comes from `api.github.com` - `releases/latest` on the Stable channel, but
  `releases?per_page=1` on Prerelease, because `latest` deliberately skips prereleases and a
  beta client polling it would never see the next beta (the `per_page=1` cap also keeps the
  reply a single release object, so the scanner cannot pair one release's tag with another's
  asset) - the asset URL is refused unless it sits under
  *this* repository's `releases/download/` path, and the bytes must hash (`src/utils/sha256.c`,
  self-contained so the one check that matters does not depend on busybox) to the `digest` that
  metadata carried. A release with no digest is refused rather than installed unverified. The
  install is `rename()` within one directory, so it is atomic, and Linux keeps the running
  image alive off its inode - which is why the last state is READY ("relaunch to run it")
  rather than a restart the app performs on itself.
  Which of those two questions to ask is `enum mesh_update_channel`, an About-screen setting
  persisted as `update_channel=` in `ui_prefs`, rather than something inferred from the running
  build: a stable install had no way to opt into beta and a beta install no way back out.
  DEFAULT keeps the old inference (a prerelease build follows prereleases), so a prefs file
  written before the setting reads as "no change". There are two channels and not one per
  release branch on purpose - telling beta from rc would mean parsing an array of releases
  instead of the single object `per_page=1` guarantees, and the SemVer ordering already offers
  a beta user the stable that supersedes their beta. `mesh_updater_set_channel` forgets
  whatever the last check found, so an asset fetched on one channel can never be installed
  after switching to the other.
- `src/ui/settings.c` — the Settings tab as data: sections → items (label, formatted value,
  kind, and for editable rows a `field` id). `MESH_UI_SETTINGS_ABOUT` is first and is the odd
  one out: it describes *this client* (version, UI backend, data dir, update state) rather
  than the radio, so `mesh_ui_settings_section_loaded()` always reports it loaded and the fb
  backend lets it through the "connect to a radio" guard - it is the one section that means
  anything with nothing connected. Its rows come from `mesh_ui_client_info` in `store.h`,
  filled by `mesh_app_flatten_client_info()` in `app.c` the same way the radio's settings
  are flattened, so neither the nav nor the backends ever see the updater. Its ACTION
  rows carry an `enum mesh_ui_settings_action` in `number`, which is how `nav.c` turns A
  into `MESH_UI_ACTION_CHECK_UPDATE`/`INSTALL_UPDATE`/`CYCLE_UPDATE_CHANNEL` without knowing
  what a section means;
  check and install are deliberately separate presses because install replaces the running
  binary. The update channel is an ACTION and not an editable ENUM field because About has no
  Y-save behind it - a pending edit there would sit unwritten forever - so A steps it and
  `app.c` persists it immediately. An ACTION row's value column carries a verb (`press A`) or
  the setting it holds, never a bare button letter: `Check for updates > A` read as a row
  whose value was the letter A. Backends draw the list; `nav.c` walks it
  (`settings_section` open or `MESH_UI_SETTINGS_NO_SECTION`, X yields
  `MESH_UI_ACTION_REFRESH_SETTINGS`). The UI's `struct mesh_ui_settings` in `store.h` is a
  flattened copy without nanopb types, filled by `mesh_app_flatten_settings` in `app.c`.
  Editing is driven by the `k_fields` table here (label, kind, enum names, number presets, text
  byte cap per `enum mesh_ui_setting_field`): the nav keeps pending edits in
  `nav.settings_edits` (Left/Right/A change the row, the keyboard is retargeted for text via
  `keyboard_field`, Y emits `MESH_UI_ACTION_SAVE_SETTINGS`, B asks once then discards), the
  item builder renders them in place marked `dirty`, and `mesh_app_build_settings_write` in
  `app.c` maps each field back onto the nanopb section. Adding an editable field means: the
  enum + table row here, the flatten in `app.c`, the `mesh_app_apply_setting_edit` case, and
  the `item_field` call in the section builder. Every radio section is editable now except the
  parts noted below. Device, Position, Power and MQTT became editable in phases 5 and 6; `LED heartbeat` is
  the one row shown inverted, because the protobuf field is `led_heartbeat_disabled` and
  `app.c` negates it on the way back. The Device role lists all thirteen values with the two
  deprecated ones labelled "(retired)" rather than hidden - a radio already set to one has to
  be able to show it, and the nav steps enums as `(value + 1) % count`, so a hole in the range
  would be unreachable rather than skipped.
  Channels are a two-level list (`nav.settings_channel` is the open slot or
  `MESH_UI_SETTINGS_NO_CHANNEL`; `mesh_ui_settings_channel_at_row` maps a list row to a slot)
  and the Key row is kind `KEY`: `number` is an `enum mesh_ui_psk_choice` (keep, default,
  random 128/256, none, typed hex in `text`), resolved to bytes in `app.c`. Sections named by
  `mesh_ui_settings_section_needs_confirm` (Bluetooth, Channels) get the `confirm_open`
  overlay between Y and the write; the action's `channel` carries the slot. LoRa, Security and
  Power are behind it too - Power because saving mode plus a short light-sleep or minimum-wake
  leaves the radio's Bluetooth off for most of every cycle, and auto-connect cannot reconnect
  to a radio that is asleep. Device and Position only reboot, which the link poller already
  handles, so they are not. MQTT is not behind it either, but one of its rows stays read-only:
  `proxy_to_client_enabled` makes the radio hand its MQTT traffic to the attached client as
  `MqttClientProxyMessage` (FromRadio tag 14) instead of reaching the broker itself, and this
  client ignores that variant, so a toggle there would silently take the radio's MQTT off the
  air. It is still *shown*, because it is the explanation when a phone left it on and MQTT
  stopped working. Keys are shown and typed as base64 (`mesh_ui_settings_key_text`/`_parse`,
  hex accepted); each KEY field has a choice mask (`mesh_ui_settings_key_choices`) and a length
  rule (`_key_len_ok`). A new private key is clamped in `app.c` and sent with the public key
  cleared, which the firmware fills in; admin keys are compacted before the write.
- `src/ui/store.c` + `controller.c` — store owns `mesh_ui_snapshot` and signals via eventfd;
  controller drains it and calls `backend->present(snapshot)`. Backends implement the three-function
  `struct mesh_ui_backend` in `include/mesh/ui/backend.h` and live in `src/ui/backends/`.
- Input goes the other way: `src/ui/input.c` reads every `/dev/input/event*`, maps evdev codes
  (BTN_SOUTH.., ABS_HAT0X/Y for the d-pad, arrow keys on a keyboard) to `enum mesh_ui_key`, and
  calls the handler the app installed; quit keys stop the loop before mapping. The handler feeds
  `mesh_ui_controller_handle_key` → `mesh_ui_store_handle_key` → `src/ui/nav.c`, which owns the
  tab/cursor/compose-target model (`struct mesh_ui_nav`, carried inside every snapshot, clamped
  against the lists on each consume) and returns a `mesh_ui_action` (connect, send text) that the
  controller hands to `mesh_app_on_ui_action` in `app.c`. Backends are stateless: they draw the
  cursor from `snapshot->nav`. Nav logic has no fd or device dependency, so test it directly.
  The Messages tab is two levels, the shape the Settings tab already uses: `thread_open` clear
  lists conversations (`mesh_ui_nav_conversation_count`/`_at`: all traffic, each enabled
  channel, each node with direct messages, then "New message"), set shows the one named by
  `target_node`/`target_channel` (or everything, when `inbox`), with the list's cursor parked in
  `conversation_list_cursor` and B backing out. `mesh_ui_nav_filter_messages` is the one place
  that filter lives, so the Messages cursor indexes the filtered list. Unread counts come from
  `struct mesh_ui_read_state` in the store (persisted with the message cache): one
  `packet_id` per conversation meaning "read up to here", chosen over a timestamp or an index
  because ids survive both the ring evicting older messages and the cache merging history back
  in - and a mark whose message has been evicted correctly reads as "everything in view is
  newer". `mesh_ui_store_mark_open_conversation_read` runs from `consume_updates`, so opening a
  thread clears its badge and a message landing in the thread you are sitting in never raises
  one; the all-traffic view marks nothing (it is a view, not a conversation) and its badge is
  the sum of the rest. **Only opening a thread
  moves the target** - the Nodes tab opens the node's *detail*, and only that detail's message
  row opens a conversation, rather than retargeting what Messages was showing, and Compose is
  an overlay (`compose_open`) over the open thread rather than a tab, so it can never be
  reached with a stale destination. The on-screen keyboard is
  `keyboard_open` plus `kb_row/kb_col/kb_layer` and `draft`, all in the nav; while it is open
  every key goes to the keyboard handler and tabs do not switch. The `picker_open` overlay
  ("New message") works the same way; its rows come from `mesh_ui_nav_picker_row` (channels,
  then nodes), and picking one opens that conversation. `app.c` ranks nodes before publishing
  (`mesh_app_node_rank`: us, pinned nodes, our other radios, message peers, RF nodes by
  `last_heard`, MQTT nodes) so the UI's
  128-node budget always holds whoever you are talking to; on an MQTT-fed mesh last_heard alone
  buries them. The post-stop publish in `mesh_app_run` only touches the transport line, because
  the shutdown save would otherwise persist an empty handshake and unresolved peer names.
  Button codes: the Brick's A is `BTN_EAST` (305) and B is `BTN_SOUTH` (304), the reverse of
  the Linux `BTN_A`/`BTN_B` aliases. X and Y do not report by position at all - the button
  printed **Y, on the left, is `BTN_NORTH` (307)**, so X on the top is `BTN_WEST` (308).
  Reading them positionally leaves Y unreachable and silently fires X in its place, which cost
  a round of "the save does nothing" debugging: Y saves a settings section, X refreshes it, so
  every save became a refresh and the edits stayed pending. All four verified from the device
  log by pressing the button; `input_brick_face_buttons` pins them. Do not "fix" any of it back.
- `src/utils/text.c` — the UTF-8 helpers everything that touches radio text shares.
  `mesh_text_sanitise` (folds C0 controls, replaces malformed bytes with `?`, never splits a
  sequence at the buffer boundary) is what `message.c` runs on message bodies and `session.c`
  runs on `long_name`/`short_name`; `mesh_text_utf8_length`/`_offset`/`_truncate` are what the
  fb backend measures lines with. Names are radio input exactly like message text is - whoever
  owns the node picks the bytes - and `User.short_name` is `char[5]`, sized for one four-byte
  emoji and its NUL, so multi-byte names are the norm, not an edge case.
- `src/ui/font5x7.c` — the framebuffer font, keyed by codepoint rather than by byte. ASCII plus
  Latin-1 Supplement and Latin Extended-A; accented letters are **composed** from a base letter
  and a mark (`k_composed`) rather than drawn, so adding one is a line. Lowercase leaves rows 0
  and 1 of the cell free and the mark goes there; capitals and ascenders fill all seven rows, so
  their mark collapses to a one-row silhouette in `glyph.above`, which `fb_draw_glyph` hangs in
  the gap `fb_line_adv` leaves between lines. Consequences of a seven-row cell, all deliberate:
  circumflex/caron/macron/ring are indistinguishable over a capital, and marks that sit *under*
  a letter have nowhere to go, so `Ç` draws as `C`. Anything with no glyph - emoji, CJK - gets
  the replacement box - but only for what `emoji.c` below does not cover.
- `src/ui/emoji.c` + the generated `src/ui/emoji_glyphs.c` — colour emoji sprites, and the
  **display-cell walker** the whole UI measures with. On a real mesh a good share of nodes are
  named entirely in emoji, so without these those rows are indistinguishable boxes.
  `mesh_ui_text_cell_next` is the one place that decides what one drawn column contains, and a
  cell is neither a byte nor always a codepoint: a flag is a regional-indicator pair, a family
  is a ZWJ sequence, and selectors/skin tones attach to what precedes them. Two rules earn
  their keep: matching happens with variation selectors **filtered out** (the font spells its
  keycap `0039 20E3`, people type `0039 FE0F 20E3`), and a single codepoint the text font can
  draw is drawn by the text font (the emoji font claims `#`, `*` and the ten digits because
  they lead keycaps - without this the 9 of "Dog Tracker K9" becomes a grey keycap tile).
  Sequences always win; that is what the extra codepoints mean.
  `scripts/gen-emoji.py` rasterises Noto Color Emoji into the committed table and is **not part
  of the build** - run it by hand, commit the result, the way `Tools/` holds committed aarch64
  helpers. 5626 sprites over 3963 unique 16x16 bitmaps (a third are duplicates), one shared
  255-colour palette, run-length encoded, ~920 KB. The pixels are emitted as one string literal
  on purpose: a braced initialiser of a million integers costs minutes of compile time, the
  literal costs about two seconds, and the file carries its own
  `#pragma GCC diagnostic ignored "-Woverlength-strings"` plus a `.clang-format-ignore` entry.
  Emoji ignore the row's text colour - carrying their own is the point of having them.
- `src/minui_helpers/` — tiny native fallbacks for `minui-list` / `minui-presenter` used when the
  NextUI cross toolchain isn't available; they honor `MESHCLIENT_MINUI_SELECTION`.

The fb backend draws into page 0 of the Brick's 1024x16384 framebuffer, then `FBIOPAN_DISPLAY`s
to it and mirrors the frame into page 1, because the Allwinner display engine keeps showing the
page NextUI's SDL last flipped to (page 1 in practice). The layer blends with per-pixel alpha,
so `compose_color` always writes an opaque alpha byte. Drop any of these and the screen is black.

Its text is measured in **cells, not bytes**: `fb_draw_text` walks `mesh_ui_text_cell_next` and
spends one cell per character or emoji, and `fb_fit`/`fb_width` (over
`mesh_ui_text_cell_truncate`/`mesh_ui_text_cells`) are the only right way to clip or
right-align a line. A `strlen` in layout code is a bug - it used to mean an emoji name counted
four columns and drew four question marks, and it still means padding computed from bytes
pushes right-aligned metrics off the edge. `%-Ns` has the same problem and is why the Nodes tab
pads its short-name field by hand. `fb_draw_emoji` draws a sprite across the full character
advance rather than the glyph's five columns, so an emoji stands as tall as the capitals next
to it; the sprites' own transparent margins keep neighbours apart.

Backend selection (`MESHCLIENT_UI_BACKEND=auto|minui|fb|cli|stub`, in `app.c`): `auto` prefers
minui if helpers are on PATH, then fb (`/dev/fb0`), then cli. `launch.sh` forces `fb` on device.

An SDL2 backend was tried in 1.1.11 and replaced by `fb` in 1.1.12; it is gone from the tree
(recoverable from commit 62fcb09 if ever wanted).

## Protobufs

`CMakeLists.txt` has a hardcoded `MESH_PROTO_NAMES` list (mesh, portnums, interdevice, config,
module_config, telemetry, channel, device_ui, xmodem, atak, admin, connection_status). Adding a new upstream `.proto` means adding
it there. Generated headers are included as `meshtastic/<name>.pb.h`. The generator is
`nanopb_generator` from PATH, falling back to `third_party/nanopb/generator/nanopb_generator.py`
via Python3 (needs `pip install protobuf grpcio-tools`).

## Release and packaging

- semantic-release on `main`/`beta`/`rc`. Conventional Commits decide the bump: `feat` minor,
  `fix`/`perf`/`refactor`/`revert` patch, `docs`/`chore`/`ci`/`test`/`build`/`style` no release.
  Recent history is almost entirely `fix:` commits, each producing a release.
- The release workflow rewrites `project(meshclient VERSION x.y.z ...)` in `CMakeLists.txt` with
  `sed`. Do not change that line's shape and do not bump it by hand. `package.json` version is
  unused. **That rewrite is why the build lives inside semantic-release's `prepareCmd` rather
  than in a workflow step**: the version becomes the `MESHCLIENT_VERSION` compile definition the
  client reports, so anything built before prepare carries the *previous* release's number.
  `prepareCmd` is just `scripts/release-build.sh ${nextRelease.version}`, and that script owns
  the `sed` as well as the build. Plugin order puts `exec` before `@semantic-release/git`, so a
  failed build aborts with nothing committed and nothing tagged.
- **`project(VERSION)` stays numeric; the full tag rides beside it.** CMake accepts only numeric
  components there and errors outright on `1.13.0-beta.1`, which is exactly what the `beta` and
  `rc` channels release. So `release-build.sh` seds in `${version%%-*}` and passes the whole tag
  as `-DMESHCLIENT_VERSION_OVERRIDE` (empty by default, so a build tree that already exists
  still picks up a version bump - seeding that cache entry with `PROJECT_VERSION` froze it at
  whatever the project was when the tree was first configured, and `make brick` stamped
  1.13.0-dev for three releases). Keeping the file numeric also keeps the rewrite idempotent - a
  suffix left behind would not match the pattern next time and the version would compound.
- **Only the release build is a release.** `-DMESHCLIENT_RELEASE_BUILD=ON` (set by
  `release-build.sh`, nothing else) is what defines `MESHCLIENT_RELEASE_BUILD` and makes
  `mesh_version_is_release()` true. Every other build - `make debug`, `make release`,
  `make brick` - reports `<version>-dev` and is never offered an update, so a binary you just
  deployed cannot be replaced by whatever is on GitHub; the About screen says
  "Dev build; updates disabled" and shows no install row rather than naming a release it will
  not install. Do not stamp a local build to "test the updater". To exercise the real
  download/verify/install path on hardware, lift the guard for that run instead
  (`mesh_updater_can_install()`): the **Dev updates** row in Settings > About, or
  `MESHCLIENT_UPDATE_ALLOW_DEV=1`. Both exist on purpose - the env var suits a run started
  from a shell, and the About row is what a handheld with no computer nearby actually has,
  which is the whole reason it is not env-only. The row is emitted only on a non-release
  build (there is no guard to lift on a release) and shows `on (environment)` as a plain fact
  when the env var is holding it, since a toggle that sprang back would read as broken; the
  choice persists as `update_allow_dev=` in `ui_prefs`, and the env var wins over the file for
  that run. A `-dev` build under it compares its own `<version>-dev` string, which SemVer
  sorts below the release of the same number, so it is offered exactly the release its working
  tree is based on. Point `MESHCLIENT_UPDATE_REPO` at a scratch repo to test against releases
  you control. Keep `conventional-changelog-conventionalcommits` on 9.x until semantic-release's
  notes generator ships `conventional-changelog-writer` 9; 10.x fails `generateNotes` with
  "Missing helper" (Dependabot is told to ignore it). Local dry runs need Node 24.10+, e.g.
  `docker run --rm -v "$PWD":/src -w /src -e GITHUB_TOKEN=$(gh auth token) node:24 bash -c
  'npm install && npx semantic-release --dry-run --branches <pushed-branch>'`.
- Release builds cross-compile with the Bootlin `aarch64--musl` toolchain and statically link a
  from-source libdbus built with meson and `message_bus=false` (library only, no daemon, no expat);
  see `.github/workflows/semantic-release.yml`. CI (`ci.yml`) is host-only gcc/clang on
  ubuntu-24.04 and does not cross-compile. Version pins for the toolchain and dbus live in both
  that workflow and `docker/setup-cross.sh`; bump them together. `make docker-pak` reproduces the
  release build locally via `scripts/cross-build.sh`; on an arm64 host the `cross` image uses
  native `musl-gcc` (exposed as `aarch64-linux-musl-gcc`) instead of downloading Bootlin. Both
  that script and `scripts/release-build.sh` emit the same two artifacts, so `make docker-pak`
  produces what a release publishes.
- Each release carries **four assets**: `MeshClient.pak.zip` (+ `.sha256`) for a fresh install,
  and the bare `meshclient-tg5040-aarch64` binary (+ `.sha256`) which is what the in-app
  updater downloads. One file the updater can hash and `rename()` into place beats a zip: no
  unzip on the device, and an interrupted download cannot leave a half-populated pak. The
  consequence is that `launch.sh` and the `Tools/` helpers do **not** ship through self-update;
  changing either means the user reinstalls the pak, so treat those two as a compatibility
  boundary rather than something to edit freely.
- **`pak.json` is the NextUI Pak Store listing**, and the store's rules shape two things here.
  Its `version` must match the release tag, so `release-build.sh` stamps it (`v${VERSION}`)
  beside the CMakeLists rewrite and `@semantic-release/git` commits it; prereleases are
  skipped, so the file only ever carries the last stable `vX.Y.Z` and a beta merging into
  `main` cannot put one in front of the store. And the zip holds the **contents** of the pak,
  not the `MeshClient.pak` folder - the store creates that folder and unpacks into it, so a
  nested zip would install as `MeshClient.pak/MeshClient.pak/launch.sh`. Installing by hand
  means unzipping *into* the pak directory. `pak.json` also ships inside the pak, because that
  copy is how the store knows the installed version - which is why `updater.c` rewrites its
  `version` line after a self-update (best effort; the binary is already installed by then).
  The store folder is named from the submitted display name, and `launch.sh` derives `$HOME`
  from the folder name, so the listing must stay **MeshClient** or every user's prefs and
  message cache move.
- `scripts/build_minui_helpers.sh` stages the helper binaries into the tracked
  `Tools/tg5040/MeshClient.pak/bin/tg5040/` tree, which holds committed **aarch64** artifacts.
  Without a cross toolchain it falls back to the host compiler, so every install is checked
  against the platform's expected ELF machine (`e_machine` 183 for tg5040) and skipped with a
  warning on a mismatch - a native `make package` on x86_64 leaves the committed binaries
  alone instead of replacing them with host-arch ones. `MESHCLIENT_ALLOW_HOST_HELPERS=1`
  overrides this. It warns rather than failing, so CI's host-only `make package` stays green.
- `scripts/package.sh` copies `$BUILD_ROOT/release/meshclient` into `dist/MeshClient.pak/bin/shared/`
  plus everything under `Tools/tg5040/MeshClient.pak/bin/{shared,tg5040}/` and `launch.sh`.
  `launch.sh` must stay POSIX sh; it sets `HOME` to the pak userdata dir, points
  `DBUS_SYSTEM_BUS_ADDRESS` at the system socket, and tees output to
  `/.userdata/tg5040/logs/MeshClient.txt`.

## Tests

One harness, `tests/test_main.c`, with a `k_test_cases` table tagged by category (`unit` today;
`integration`/`hardware` reserved). Register new cases in that table; use
`record_failure`/`record_success`. Tests must not touch real BlueZ — use the bluez mock. New
CTest labels need a matching `add_test` in `tests/CMakeLists.txt`. Verified state as of 2026-09-04: 71 unit tests, all passing in
the dev container with zero compiler warnings. `message_encode_text_golden` pins the
`TEXT_MESSAGE_APP` wire format against a hand-derived byte vector (not against our own encoder),
so a protobuf regeneration that changes field numbers or wire types fails loudly.

`make format` rewrites every tracked `.c`/`.h` using `.clang-format`, and the tree is kept
normalised against it - on a clean tree the command is a no-op. It is normalised with
**clang-format 18** (what `ubuntu:24.04` ships, so the dev container and CI agree); a
different major version reflows things and produces spurious churn. `make format` therefore
**refuses to run** unless `clang-format --version` reports that major - use
`./scripts/docker.sh make format` (or `make docker-shell`) from a host with a different one,
or set `CLANG_FORMAT_ANY_VERSION=1` if you mean it. That guard exists because the tree did
drift this way once: `src/ui/font5x7.c`, `src/ui/emoji.c`, `include/mesh/ui/emoji.h` and
`src/ui/node_detail.c` were committed from a host running clang-format 17. Nothing gates on
formatting in CI, precisely because host versions vary.
