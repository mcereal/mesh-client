# CLI and runtime reference

`meshclient --help` is authoritative; this page adds the behaviour behind the flags, the
environment variables, and the on-device controls.

## Modes

Without `--foreground` the client does a single poll and exits — that is what `--status`,
`--list-devices` and `--send-text` use. `--foreground` runs the event loop until stopped, which
is what the pak does.

```bash
meshclient --list-devices                 # BLE advertisers and USB ports, then exit
meshclient --status                       # connect, print the handshake summary
meshclient --status --json                # machine-readable
meshclient --status --status-output PATH  # write that JSON to a file (implies --status --json)
meshclient --foreground --log-level debug # the on-device mode, verbose
```

`--status --json` emits a `cached` flag (and `cached_handshake` / `cached_messages` when offline)
so automation can tell a live snapshot from a stale one.

## Messaging from the shell

```bash
meshclient --send-text "hello mesh"                        # broadcast on channel 0
meshclient --send-text "on my way" --dest '!433d1a2c' --ack  # direct, wait for the ack
meshclient --send-text "net in 5" --channel 2
```

`--dest` takes `!hex`, `0xhex`, decimal, or `all` (the default broadcast). `--ack` requests
delivery confirmation and waits for the `Routing` reply; **the mesh never acks broadcasts**, so
`--ack` is ignored for them.

Received messages appear in `--status`, in the on-device HUD, and in the persisted cache, so the
last conversation is readable with the radio out of range.

## Picking a transport

BLE is the default. `--serial[=ID]` points `--status` and `--send-text` at a USB port instead;
`ID` is a sysfs interface id (`1-1:1.1`) or a device node (`/dev/ttyUSB0`), and without one the
first port found is used.

`--disable-ble` / `--disable-serial` turn a transport off entirely.

## Auto-connect

In foreground mode the app connects by itself, and **a plugged-in node wins over anything on the
air** — it needs no pairing, has no range to lose, and is almost certainly why the cable is
there.

1. **USB first.** If any port is discovered, it takes the preferred one
   (`MESHCLIENT_PREFERRED_SERIAL_DEVICE`) or the first found, with no grace period. Only if that
   connect fails outright does it fall through to Bluetooth.
2. **Then BLE.** The preferred node (`--preferred-device`, `MESHCLIENT_PREFERRED_BLE_DEVICE`, or
   the last node it connected to) when it is in range, otherwise the strongest Meshtastic
   advertiser after a 30 s grace period.

Failed attempts back off from 2 s to 60 s; only an established link clears the backoff. The two
preferences are kept apart, so unplugging a USB node does not erase which radio to look for over
the air.

`--status`, `--list-devices` and `--send-text` are unaffected. Set `MESHCLIENT_AUTOCONNECT=0` to
stop it.

## Environment variables

| Variable | Effect |
|---|---|
| `MESHCLIENT_RUN_MODE` | `foreground` / single-poll, same as `--foreground` |
| `MESHCLIENT_IDLE_TIMEOUT_MS` | poll timeout, same as `--timeout` |
| `MESHCLIENT_DISABLE_BLE`, `MESHCLIENT_DISABLE_SERIAL` | turn a transport off |
| `MESHCLIENT_PREFERRED_BLE_DEVICE`, `MESHCLIENT_PREFERRED_SERIAL_DEVICE` | preferred node / port |
| `MESHCLIENT_AUTOCONNECT` | `0` stops the foreground loop connecting on its own |
| `MESHCLIENT_UI_BACKEND` | `fb\|cli\|stub`; `fb` unless there is no `/dev/fb0` |
| `MESHCLIENT_FB_SCALE` | framebuffer font multiplier, 2–6; default is whatever the theme asks for (4) |
| `MESHCLIENT_THEME` | `dark\|light\|contrast\|colorblind`. Outranks the theme picked in Settings → About, which then shows as a fact rather than a switch; unset, the saved choice applies, and an unknown name warns and falls back rather than leaving a handheld with no UI |
| `MESHCLIENT_LANG` | which language the UI is drawn in, e.g. `en`. Matched on the language part alone, so `fr_CA.UTF-8` finds `fr`; it outranks `LC_ALL`, `LC_MESSAGES` and `LANG`, and a language this build does not have leaves English in force. Settings → About says which one resolved. See [`docs/i18n.md`](i18n.md) |
| `MESHCLIENT_QUIT_KEYS` | override the evdev codes that quit, e.g. `"139,316"` — tunable on-device from the log without a rebuild |
| `MESHCLIENT_KEY_REPEAT_DELAY_MS` | how long a direction is held before it starts repeating, 0–5000; default 350, and `0` turns hold-to-scroll off on every device, a USB keyboard included |
| `MESHCLIENT_KEY_REPEAT_MS` | the gap between repeats once it starts, 10–2000; default 90, halving after eight rows |
| `MESHCLIENT_UPDATE_REPO`, `MESHCLIENT_UPDATE_ASSET` | where the self-updater looks |
| `MESHCLIENT_UPDATE_ALLOW_DEV` | let a `-dev` build install what it finds; same switch as Settings → About → Dev updates |

The boolean knobs above (`MESHCLIENT_DISABLE_*`, `MESHCLIENT_AUTOCONNECT`,
`MESHCLIENT_UPDATE_ALLOW_DEV`) all read the same vocabulary, case-insensitively: `1`/`true`/
`yes`/`on` and `0`/`false`/`no`/`off`. Anything else logs a warning and leaves the default in
place, rather than being read as one or the other.

Build-time only: `MESHCLIENT_RELEASE_BUILD`, `MESHCLIENT_VERSION` and
`MESHCLIENT_VERSION_OVERRIDE` (see [`semantic-release.md`](semantic-release.md)).

Logs stream to `stderr` locally and, on device, to the pak log `launch.sh` tees into
`/.userdata/tg5040/logs/MeshClient.txt`. Verbosity is `--log-level trace|debug|info|warn|error`.

State lives under `$HOME/.meshclient/` (`ui_prefs`, `ui_prefs.handshake`, and `canned.txt` if you
write one). `launch.sh` sets `$HOME` to the pak's userdata dir on device.

## On-device controls

The framebuffer HUD is five tabs: **Messages, Nodes, Devices, Status, Settings.**

| Key | Action |
|---|---|
| Left/Right, L1/R1 | switch tab |
| Up/Down | move the cursor — hold to keep scrolling, which speeds up after a few rows |
| A | act on the row |
| B | back out |
| Y | write a message (Messages/Nodes), save a section (Settings) |
| X | delete a conversation (Messages), refresh (Settings), pin a node (Nodes), disconnect (Devices) |
| MENU | quit |

**Messages** is two levels, the way a phone messenger is: a conversation list (all traffic, each
channel, each node you have direct messages with, and a *New message* row) and, inside one, that
conversation. Each row is a coloured disc with the correspondent's initials, the name and how long
ago it last spoke, then the last thing said with the unread count as a pill; opening a conversation
clears the count, and the marks persist across restarts alongside the cached history.

**X deletes the conversation under the cursor.** The first press arms it and the row says so, the
second throws the messages away — from the running log, from the cached history and from the file
on disk, so they do not come back on the next start. Nothing is asked of the radio: it keeps no
per-client history to delete, and it will happily deliver the same conversation again. Deleting a
channel empties it but keeps its row, because that row is the radio's channel table rather than
your message log; deleting a direct conversation takes the row with it. *All traffic* and *New
message* are not conversations and X does nothing on either.

**Compose** is an overlay over the conversation you are in rather than a tab, so it always knows
where the message is going. It has a d-pad keyboard for free text (A types, B deletes, X shifts,
Y space, START sends) and quick replies from `$HOME/.meshclient/canned.txt` (one per line) or a
built-in list. *New message* opens a picker of every enabled channel and every node.

**Nodes** is the contact list. A opens a node's detail (identity, signal, device metrics,
position, environment — rows appear only for what the node has actually reported), and from there
you can message it, trace the route to it, ask for its name, pin it to the top of the list, or
ignore it.

**Devices** lists USB ports first — they need no pairing, so they sort to the top and are the
default cursor row — then BLE advertisers. A connects either kind; on a BLE row it also bonds,
prompting for the six digits a PIN-mode node shows on its own screen, while a cable has nothing
to bond. X disconnects the current radio and holds auto-connect off so it stays disconnected. Y,
pressed twice, forgets a bond, and applies to BLE rows only.

**Settings** shows the radio's configuration read over the Meshtastic admin protocol and edits it
in place: Left/Right or A change a row, the keyboard handles names and hex keys, Y saves, B
discards. Channels, Bluetooth, LoRa, Security and Power ask for confirmation first. The radio
usually reboots to apply a change and the client reconnects on its own. **About** is the one
section that works with nothing connected: version, UI backend, data dir, and the self-updater.

See [`device.md`](device.md) for getting builds onto the Brick, and
[`settings-roadmap.md`](settings-roadmap.md) for what each Settings section can do.
