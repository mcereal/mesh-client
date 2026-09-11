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

`--tcp-host ADDR[:PORT]` points them at a node on the network instead — an ESP32 with its
network module enabled, or `meshtasticd` on any Linux box. The port defaults to 4403. **It takes
a numeric address, not a name**: resolving one would block the client's single event loop, so
`meshtastic.local` is refused in words rather than paid for in a frozen UI. See
[`transport.md`](transport.md#an-address-not-a-name).

```sh
meshclient --tcp-host 192.168.1.50 --status
meshclient --tcp-host '[fd00::1]:4403' --send-text "hello"
```

`--serial` outranks `--tcp-host` when both are given: it is the more explicit of the two.

`--disable-ble` / `--disable-serial` / `--disable-tcp` turn a transport off entirely.

## Fetching radio firmware

```sh
meshclient --fetch-firmware heltec-mesh-node-t114 --staging /mnt/UDISK
```

Downloads the newest **stable** firmware image for a build target, verifies it, and leaves it
staged. **No radio is touched** — no transport is even started — because the download and the
install are separate phases and this is the one without a radio in it. See
[`docs/radio-firmware-roadmap.md`](radio-firmware-roadmap.md).

The board is named by its build target rather than resolved from a connected radio, which is
what makes it runnable with nothing plugged in. Four documents get read: the release index for
the newest version, that release's own manifest for the platform whose zip holds the target,
the board's `.mt.json` from inside that zip for the name of the image, and then the image. Only
the last is large, and none of them is the 46 MB zip — the whole run moves about 0.6 MB and
takes some eight seconds on a Brick over Wi-Fi.

What it prints is the point: the image's name and length, where it was staged, and — on the USB
path, where the image is a UF2 — its block count, family id and address range, read out of the
file rather than assumed.

`--staging DIR` is where it lands, `/tmp` by default. On a Brick use `/mnt/UDISK` and **not**
`/mnt/SDCARD`: the nRF52 bootloader's mass-storage drive gets mounted over that card the moment
a radio reboots into DFU, and a file staged there disappears from its own path.

## Installing radio firmware

```sh
meshclient --install-firmware heltec-mesh-node-t114 --staging /mnt/UDISK
```

Everything the fetch does, and then **the radio is changed**: an `enter_dfu_mode_request` goes
down the serial link, the client watches the USB tree until the board comes back as a
bootloader, takes the drive off whatever the platform mounted it at, writes the `.uf2`'s blocks
to it, and waits for the board to reset itself into the new firmware. About eight seconds of
download and thirteen of write on a T114, with a second for the reboot.

An nRF52 or RP2040 radio has to be on **USB**. That is the feature's constraint rather than the
command's — an nRF52 has no over-the-air path from here at all, which is what the About screen's
"connect it by USB" row already says. An ESP32 target goes over Bluetooth instead; see
[below](#over-bluetooth-esp32).

**With no radio connected it still runs.** It says so, and then waits for a bootloader instead of
asking for one — which is the recovery path: a double-tap of the reset button does by hand
exactly what the admin verb does, and it is what a board too broken to be asked politely needs.
When a radio *is* connected the bootloader is only accepted on the same USB port it was on, so a
write cannot follow a board that moved.

Three things about it are worth knowing before running it on a Brick:

- **Stage outside `/mnt/SDCARD`.** The platform mounts the bootloader's 32 MB ghost FAT over the
  SD card, taking the pak, the binary, the CA bundle and the log with it. The client reads the
  image into memory before sending the radio anywhere, so the install survives that — but a
  `--staging /mnt/SDCARD` puts the *download* somewhere that vanishes underneath it.
- **The drive is unmounted, and that is also the repair.** The shadow is a stacked mount, so
  taking it off is what puts `/mnt/SDCARD` back. A mountpoint that will not come off is a
  refusal rather than a lazy unmount: a write racing the VFAT driver over one device is not a
  thing to leave in.
- **An interrupted write is not a broken radio.** The board sits in its bootloader, which any
  computer on any OS can talk to, and the recovery is to run the same command again. That is the
  whole reason the USB half was built before the Bluetooth one.

### Over Bluetooth (ESP32)

```sh
meshclient --install-firmware heltec-v3 --staging /mnt/UDISK -p 9C:13:9E:9D:0A:D9
```

The same command for an ESP32 or ESP32-S3 target, and a different handover: the fetch runs with
no transport up (the Brick's Wi-Fi and Bluetooth are one part), then the client connects to the
radio over BLE, checks that the radio's `hw_model` is the one the image is for, and sends an
`ota_request` holding it to the image's SHA-256. The radio reboots into its OTA loader - a
different BLE peripheral, at the radio's address plus one - and the client finds it, asks for a
7.5 ms connection interval, streams the image, and waits for the radio to advertise again. Then
it connects once more and prints the firmware and reboot count the radio reports, because that
line is the evidence the install happened.

Name the radio with `-p ADDRESS`. Without it the loudest radio in range is picked, and the
`hw_model` check will refuse an nRF52 that happened to be louder rather than send it anything.
With it there is **no fallback**: `--status` settles for the loudest node when the named one is
not in range, and an install does not - a second radio of the same model would pass the
`hw_model` check and be flashed in its place. A named radio that does not answer is taken to be
the one already in its loader, which is what the resume path below looks for.

**This is the half with a hazard, and the command is built around it.** Once the radio is in its
loader it stays there - off the mesh, advertising the loader's service - until something sends it
the image it was promised. A transfer that breaks is retried from the start, up to three times,
because that is what the loader does with a link that comes back. If the client itself is
stopped, **run the same command again**: with no radio answering it looks for one already in its
loader (at `-p`'s address plus one) and finishes the job. The staged image is kept, and the loader
only accepts an image with the hash it was given, so it has to be the same release.

## Looking inside a tile pack

```sh
meshclient --map-pack /mnt/SDCARD/Tools/tg5040/MeshClient.pak/maps/region.mctp
```

Prints what a raster tile pack holds — its name, its attribution, the date it was cut, how many
tiles at which zooms, the area they cover — and reads one tile out of the middle of that area to
prove the index's offsets point at bytes the card really has. **No radio and no network are
touched**, and nothing is decoded: this is a file, a header and one `pread`.

It exists for the reason `--fetch-firmware` existed before there was a firmware screen. A pack
is read on the device, off a FAT32 card, and "does this file open, and can a tile come out of
it" is not a question the host test suite can answer about the card in somebody's Brick. It is
also what the sideload path has to check with — otherwise a reader copies a file across and
finds out at the map.

The coverage it prints is **derived from the tiles the pack actually holds**, not read out of a
header field, so it is the number a mis-built pack disagrees with. A "middle tile is not in the
pack" line is not a fault: a pack is a rectangle of the world with holes in it, and the middle
of a coastal region is often water nobody cut a tile for.

Packs are built on a host with
[`devtools/map_pack/map_pack.py`](../devtools/map_pack/map_pack.py), which converts an MBTiles
file or a `z/x/y` tree and refuses anything the device could not draw. The format and why the
client carries one of its own are in
[`docs/maps-roadmap.md`](maps-roadmap.md#the-pack-format).

## Auto-connect

In foreground mode the app connects by itself, and **a plugged-in node wins over anything on the
air** — it needs no pairing, has no range to lose, and is almost certainly why the cable is
there.

1. **USB first.** If any port is discovered, it takes the preferred one
   (`MESHCLIENT_PREFERRED_SERIAL_DEVICE`), else the port used most recently, else the first
   found — with no grace period. Only if that connect fails outright does it fall through.
2. **Then a configured network host**, if `MESHCLIENT_TCP_HOST` / `--tcp-host` named one. It
   needs no pairing and has no range to lose, and unlike anything on a scan it is a place
   somebody deliberately wrote down. It is also the one candidate that can be absent without
   being *gone* — an address stays written down with the WiFi off — so this arm gets at most one
   attempt every 30 s, which is what leaves room for the other two. With no host configured, the
   default, this step does not exist.
3. **Then BLE**, over the nodes that answered the last scan and only those. In order:
   1. the preferred node (`--preferred-device`, `MESHCLIENT_PREFERRED_BLE_DEVICE`, or the last
      node connected to), if it is in range;
   2. otherwise the radio of *yours* that is in range and was used most recently — the client
      keeps the last eight it has connected to over either link;
   3. otherwise the strongest Meshtastic advertiser.

**Only a node that answered the scan is a candidate.** BlueZ lists every device it holds a bond
for, so a radio switched off in another building is in that list all day with the right address,
the right name and `Paired` set — and with no RSSI at all, which as a raw number reads as a
stronger signal than anything real. Taking either at face value is how leaving the house with the
second of two radios used to spend the whole session timing out against the one still at home.

The grace period is how long the preferred node gets to show up before another is used: **5 s**
when a radio of yours is already advertising — long enough for a node that is merely slow, short
enough that being wrong costs seconds rather than half a minute — and **30 s** when the only
alternative is a node you have never connected to.

Failed attempts back off from 2 s to 60 s; only an established link clears the backoff. The two
preferences are kept apart, so unplugging a USB node does not erase which radio to look for over
the air. Forgetting a node's pairing in the Devices tab also drops it from the list, so the radio
you used before it takes its place.

`--status`, `--list-devices` and `--send-text` do not auto-connect. They apply the same range
test, though: the one-shot link picks among nodes that answered the scan, and `--list-devices`
prints `[not in range]` for a bond with nothing behind it rather than an RSSI of 0. Set
`MESHCLIENT_AUTOCONNECT=0` to stop auto-connect.

## Environment variables

| Variable | Effect |
|---|---|
| `MESHCLIENT_RUN_MODE` | `foreground` / single-poll, same as `--foreground` |
| `MESHCLIENT_IDLE_TIMEOUT_MS` | poll timeout, same as `--timeout` |
| `MESHCLIENT_DISABLE_BLE`, `MESHCLIENT_DISABLE_SERIAL`, `MESHCLIENT_DISABLE_TCP` | turn a transport off |
| `MESHCLIENT_PREFERRED_BLE_DEVICE`, `MESHCLIENT_PREFERRED_SERIAL_DEVICE` | preferred node / port |
| `MESHCLIENT_TCP_HOST` | the node to reach over the network, `ADDR` or `ADDR:PORT`, same as `--tcp-host`. Not named `PREFERRED_` like the two above because there is nothing to prefer it over: the other two pick one of several things the client found, and this one *is* the link |
| `MESHCLIENT_AUTOCONNECT` | `0` stops the foreground loop connecting on its own |
| `MESHCLIENT_SCAN_RESUME_GRACE_MS` | how long a teardown keeps the BLE scan down, 0–60000; default 3000. It exists so the scan is not started for the one second between a drop and the auto-connect that follows it, only to be stopped again microseconds before `Connect`. `0` restores the old always-scan-when-idle behaviour |
| `MESHCLIENT_UI_BACKEND` | `fb\|cli\|stub`; `fb` unless there is no `/dev/fb0` |
| `MESHCLIENT_FB_SCALE` | framebuffer font multiplier, 2–6; default is whatever the theme asks for (4) |
| `MESHCLIENT_THEME` | `dark\|light\|contrast\|colorblind`. Outranks the theme picked in Settings → About, which then shows as a fact rather than a switch; unset, the saved choice applies, and an unknown name warns and falls back rather than leaving a handheld with no UI |
| `MESHCLIENT_LANG` | which language the UI is drawn in, e.g. `en`. Matched on the language part alone, so `fr_CA.UTF-8` finds `fr`; it outranks `LC_ALL`, `LC_MESSAGES` and `LANG`, and a language this build does not have leaves English in force. Settings → About says which one resolved. See [`docs/i18n.md`](i18n.md) |
| `MESHCLIENT_INPUT_PROFILE` | which pad this is: `brick` (the default) or `xbox`. It decides which evdev code each *printed* face button reports, and the keycaps the action bar draws beside its verbs — one table, because a port that corrected the codes and not the words would leave the bar naming a key that does something else. `xbox` is the ordinary Linux convention (A is `BTN_SOUTH`), which the Brick reverses. An unknown name warns and falls back; the client is perfectly usable with the wrong profile, just confusing. See [`docs/device.md`](device.md#another-pad-another-profile) |
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
