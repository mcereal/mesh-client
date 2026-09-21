# CLI and runtime reference

`meshclient --help` is authoritative; this page adds the behaviour behind the flags.

## Getting the binary

On a handheld it is already there — the pak is this binary plus a `launch.sh`. On an x86-64
desktop or server, each release carries `meshclient-linux-x86_64` (+ `.sha256`): static against
musl, so it needs no libdbus, no Python and no matching glibc on the target, only a Linux
kernel. On ARM, build it there with `make linux-cli` (needs `musl-tools`) — a release publishes
no general-purpose ARM CLI, and `meshclient-tg5040-aarch64` is the handheld's build rather than
one. See [`scripts/linux-cli-build.sh`](../scripts/linux-cli-build.sh).

Two things a desktop gets that the device does not. **BLE still goes through BlueZ over D-Bus**,
so `--list-devices` finds nothing without a running `dbus-daemon` and a `bluetoothd`; the binary
looks for the system bus at `/run/dbus/system_bus_socket` and honours `DBUS_SYSTEM_BUS_ADDRESS`
if it is somewhere else. HTTPS needs nothing from the host: the binary verifies against the CA
roots compiled into it, and `SSL_CERT_FILE` names a bundle to use instead.

## Modes

Without `--foreground` the client does a single poll and exits — that is what `--status`,
`--list-devices` and `--send-text` use. `--foreground` runs the event loop until stopped, which
is what the pak does.

```bash
meshclient --list-devices                 # BLE advertisers and USB ports, then exit
meshclient --status --json                # connect, print the handshake summary
meshclient --status --status-output PATH  # write that JSON to a file
meshclient --foreground --log-level debug # the on-device mode, verbose
```

`--status --json` emits a `cached` flag (and `cached_handshake` / `cached_messages` when offline)
so automation can tell a live snapshot from a stale one.

```bash
meshclient --send-text "hello mesh"                          # broadcast on channel 0
meshclient --send-text "on my way" --dest '!433d1a2c' --ack  # direct, wait for the ack
meshclient --send-text "net in 5" --channel 2
```

`--dest` takes `!hex`, `0xhex`, decimal, or `all`. **The mesh never acks broadcasts**, so `--ack`
is ignored for them.

## Picking a transport

BLE is the default. `--serial[=ID]` takes a sysfs interface id (`1-1:1.1`) or a device node, and
without one uses the first port found. `--tcp-host ADDR[:PORT]` reaches an ESP32 with its network
module enabled, or `meshtasticd` on any Linux box; the port defaults to 4403. It takes an address
or a name — a name is resolved in a forked child rather than on the loop, so it costs a fork and
a round trip rather than a frozen UI ([`transport.md`](transport.md#a-name-costs-a-fork)).

`--serial` outranks `--tcp-host`. `--disable-ble` / `--disable-serial` / `--disable-tcp` turn a
transport off entirely.

On the device the address is typed instead: the last row of the Devices tab, saved as
`network_host` in `~/.meshclient/ui_prefs`. That saved address seeds `--tcp-host` only when
neither the flag nor `MESHCLIENT_TCP_HOST` named one.

## Radio firmware

```sh
meshclient --fetch-firmware heltec-mesh-node-t114 --staging /mnt/UDISK    # download only
meshclient --install-firmware heltec-mesh-node-t114 --staging /mnt/UDISK  # and write it
meshclient --install-firmware heltec-v3 --staging /mnt/UDISK -p 9C:13:9E:9D:0A:D9   # ESP32, BLE
```

The fetch touches no radio and starts no transport: the board is named by its build target, so it
runs with nothing plugged in. It reads the release index, the release manifest, the board's
`.mt.json` and then the image — about 0.6 MB in total, not the 46 MB zip.

The install adds the radio: an `enter_dfu_mode_request` down the serial link, then the client
watches the USB tree for the bootloader, unmounts the drive from wherever the platform put it,
writes the `.uf2` blocks, and waits for the reset. nRF52 and RP2040 have **no over-the-air path**
and must be on USB; an ESP32 target goes over BLE instead, where the radio reboots into its OTA
loader (a second peripheral at its address plus one) and the client streams the image to it.

- **Stage outside `/mnt/SDCARD`.** The nRF52 bootloader's ghost FAT gets mounted over the card,
  taking the pak, the binary and the log with it. Use `/mnt/UDISK`. The image is read into memory
  before the radio is sent anywhere, so the install survives that — the *download* would not.
- **An interrupted write is not a broken radio.** The board sits in its bootloader; run the same
  command again. On the BLE path a named radio that does not answer is taken to be one already in
  its loader, which is the resume path. The loader only accepts the hash it was promised, so it
  has to be the same release.
- **With no radio connected the install still runs**, waiting for a bootloader rather than asking
  for one — the recovery path, since a double-tap of reset does by hand what the admin verb does.
  With a radio connected the bootloader is only accepted on the same USB port.
- **`-p` means no fallback.** `--status` settles for the loudest node when the named one is out of
  range; an install does not, because a second radio of the same model would pass the `hw_model`
  check and be flashed in its place.

## Looking inside a tile pack

```sh
meshclient --map-pack /mnt/SDCARD/Tools/tg5040/MeshClient.pak/maps/region.mctp
```

Prints a raster tile pack's name, attribution, cut date, tile counts by zoom and the area they
cover, then reads one tile out of the middle to prove the index points at bytes the card really
has. No radio, no network, nothing decoded. Coverage is derived from the tiles the pack holds
rather than read out of a header, so it is the number a mis-built pack disagrees with; a "middle
tile is not in the pack" line is not a fault, since a pack is a rectangle with holes in it.

The map draws **`$HOME/.meshclient/map.mctp`** — beside the node cache, deliberately not inside
the pak, because the pak is what self-update replaces and a converted region is the user's file.
`MESHCLIENT_MAP_PACK` names another path. One pack only; choosing between several is a screen.

Packs are built on a host with
[`devtools/map_pack/map_pack.py`](../devtools/map_pack/map_pack.py) from an MBTiles file or a
`z/x/y` tree. `map_pack.py synth` (also `make demo-pack`) draws a pack of a place that does not
exist, which is how the map can be looked at without settling whose tiles to ship.

## Auto-connect

In foreground mode the app connects by itself, and **a plugged-in node wins over anything on the
air** — it needs no pairing, has no range to lose, and is almost certainly why the cable is there.

1. **USB first**, with no grace period: the preferred port, else the most recent, else the first
   found. Only an outright connect failure falls through.
2. **Then a configured network host**, if one was named. At most one attempt every 30 s — it is
   the one candidate that can be absent without being *gone*, since an address stays written down
   with the WiFi off. With no host configured this step does not exist.
3. **Then BLE**, over the nodes that answered the last scan and only those: the preferred node if
   in range, else the radio of *yours* used most recently (the last eight over either link), else
   the strongest Meshtastic advertiser.

**Only a node that answered the scan is a candidate.** BlueZ lists every device it holds a bond
for, so a radio switched off in another building is in that list all day with the right address,
the right name, `Paired` set and no RSSI at all — which as a raw number reads as a stronger
signal than anything real.

The grace period for the preferred node is **5 s** when a radio of yours is already advertising,
**30 s** when the only alternative is one you have never connected to. Failed attempts back off
from 2 s to 60 s; only an established link clears it. The USB and BLE preferences are kept apart.
`--status`, `--list-devices` and `--send-text` do not auto-connect but apply the same range test.
`MESHCLIENT_AUTOCONNECT=0` turns it off.

## Environment variables

| Variable | Effect |
|---|---|
| `MESHCLIENT_RUN_MODE` | `foreground` / single-poll, same as `--foreground` |
| `MESHCLIENT_IDLE_TIMEOUT_MS` | poll timeout, same as `--timeout` |
| `MESHCLIENT_DISABLE_BLE`, `_SERIAL`, `_TCP` | turn a transport off |
| `MESHCLIENT_PREFERRED_BLE_DEVICE`, `_SERIAL_DEVICE` | preferred node / port |
| `MESHCLIENT_TCP_HOST` | the node to reach over the network. Not named `PREFERRED_`: the other two pick one of several things the client found, and this one *is* the link |
| `MESHCLIENT_AUTOCONNECT` | `0` stops the foreground loop connecting on its own |
| `MESHCLIENT_MQTT_PROXY` | `0` stops the client holding a broker connection for a radio that asks for one. The whole arrangement is otherwise the *radio's* decision — see [`mqtt.md`](mqtt.md) — so this is the only say the Brick has in it |
| `MESHCLIENT_SCAN_RESUME_GRACE_MS` | how long a teardown keeps the BLE scan down, 0–60000, default 3000, so it is not restarted for the second between a drop and the reconnect |
| `MESHCLIENT_UI_BACKEND` | `fb\|sdl\|cli\|stub`; `fb` unless there is no `/dev/fb0`, and then `cli`. `sdl` is never reached by falling back — a window is a thing you ask for |
| `MESHCLIENT_FB_SCALE` | font multiplier, 2–6; default is the theme's (4). Applies to whichever backend opened the panel, `sdl` included |
| `MESHCLIENT_SDL_SIZE` | the window's geometry as `WxH`; default `1024x768`, which is the Brick's panel — so what comes up on a desktop is the geometry the device will draw |
| `MESHCLIENT_SDL_VSYNC` | `1` waits for the scan-out before returning from a present. Off by default: this client has one thread, and waiting there is up to a frame in which no transport is serviced |
| `MESHCLIENT_SDL_POLL_MS` | how often SDL's event queue is drained, 1–200, default 8. It is drained on a timer because SDL exposes no descriptor to wait on |
| `MESHCLIENT_MAP_PACK` | the tile pack the map draws. Read once at startup |
| `MESHCLIENT_THEME` | `dark\|light\|contrast\|colorblind`. Outranks Settings → About, which then shows it as a fact rather than a switch |
| `MESHCLIENT_LANG` | which language the UI is drawn in. Outranks `LC_ALL`, `LC_MESSAGES`, `LANG`. See [`i18n.md`](i18n.md) |
| `MESHCLIENT_INPUT_PROFILE` | `brick` (default) or `xbox`. Decides which evdev code each *printed* face button reports **and** the keycaps the bar draws — one table, since correcting the codes and not the words would name a key that does something else. See [`device.md`](device.md#the-buttons) |
| `MESHCLIENT_QUIT_KEYS` | override the evdev codes that quit, e.g. `"139,316"` |
| `MESHCLIENT_KEY_REPEAT_DELAY_MS` | hold-before-repeat, 0–5000, default 350; `0` turns hold-to-scroll off |
| `MESHCLIENT_KEY_REPEAT_MS` | gap between repeats, 10–2000, default 90, halving after eight rows |
| `MESHCLIENT_UPDATE_REPO`, `_ASSET` | where the self-updater looks |
| `MESHCLIENT_UPDATE_ALLOW_DEV` | let a `-dev` build install what it finds |
| `MESHCLIENT_LOG_LEVEL` | the level `launch.sh` starts the client at; `info` unless set. The pak used to hardcode `debug`, which is what made the log on the card grow the way it did |
| `MESHCLIENT_LOG_FILE` | the log the client cuts back at startup, when it is not the one derived from `HOME` |
| `MESHCLIENT_LATENCY_TRACE` | same as `--trace-latency`; see [`performance.md`](performance.md) |

Boolean knobs read `1`/`true`/`yes`/`on` and `0`/`false`/`no`/`off`, case-insensitively. Anything
else warns and leaves the default. Build-time only: `MESHCLIENT_RELEASE_BUILD`,
`MESHCLIENT_VERSION`, `MESHCLIENT_VERSION_OVERRIDE`.

Logs stream to `stderr`, and on device to `/.userdata/tg5040/logs/MeshClient.txt` via `launch.sh`.
State lives under `$HOME/.meshclient/` (`ui_prefs`, `ui_prefs.handshake`, `canned.txt`).

**The log on the card is bounded, and the client is what bounds it.** `launch.sh` appends to that
file on every run and nothing ever cut it back, so it grew for the life of the install. At startup
the client now trims it to its newest 128 KB once it has passed 512 KB - in place, keeping the
file's inode, because `tee` is holding it open and appending to it by the time `main()` runs. The
path is worked out from `HOME` rather than passed in, which is what makes an install that only ever
self-updates the binary get the same treatment: `launch.sh` does not ship through self-update. That
derivation only fires when `HOME` has the launcher's `.userdata/<platform>/<pak>` shape - off device
it would otherwise name some unrelated `<parent>/logs/<name>.txt` and trim a file the client does
not own.
A single very long session is still free to grow past the cap; what the cap bounds is the
accumulation across runs. `MESHCLIENT_LOG_FILE` names the file when it is somewhere else.

## On-device controls

Five tabs: **Messages, Nodes, Devices, Status, Settings.**

| Key | Action |
|---|---|
| Left/Right, L1/R1 | switch tab |
| L2/R2 | move a whole group at a time, where a screen draws its groups as cards |
| Up/Down | move the cursor — hold to keep scrolling, which speeds up after a few rows |
| A | act on the row |
| B | back out |
| Y | write a message (Messages/Nodes), save a section (Settings) |
| X | delete a conversation (Messages), refresh (Settings), pin a node (Nodes), disconnect (Devices) |
| SELECT | help for what is on screen ([`help.md`](help.md)) |
| MENU | quit |

**Messages** is two levels: a conversation list (all traffic, each channel, each node with direct
messages, a *New message* row) and one conversation. X arms a delete on the first press and
throws the messages away on the second — from the log, the cache and the file on disk. Nothing is
asked of the radio, which keeps no per-client history. Deleting a channel empties it but keeps
its row (that row is the radio's channel table); deleting a direct conversation takes the row.
Inside a conversation A answers the bubble under the cursor, X puts an emoji on it, Y writes to
the conversation, and START sends it again when it is one of ours the mesh came back on — the
failed bubble stays where it is and the retry goes out as a new message.

**Compose** is an overlay over the conversation rather than a tab, so it always knows where the
message is going: a d-pad keyboard and quick replies from `$HOME/.meshclient/canned.txt`. The
keyboard uses the pad the way a console keyboard does — **A** types the key under the cursor,
**X** is the backspace, **B** leaves (keeping what was typed; the grid's own ✕ discards),
**Y** is a space, **START** sends. **L2/R2** shift for one capital, and **L1/R1** step the panel
the grid is showing: `abc`, `ABC`, symbols, then three pages of forty emoji. The bottom-left key
of the grid steps the same ring, so everything is reachable without the shoulders.

**Nodes** is the contact list; A opens a node's detail, where rows appear only for what the node
has actually reported. **Devices** lists USB ports first (no pairing, so they sort to the top),
then BLE advertisers; A connects and bonds, X disconnects and holds auto-connect off, Y twice
forgets a bond. **Settings** reads the radio's configuration over the admin protocol and edits it
in place; the radio usually reboots to apply a change and the client reconnects on its own.
**About** is the one section that works with nothing connected.
