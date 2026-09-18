# Working with the TrimUI Brick

From "in a drawer" to "receives builds over WiFi", and the day-to-day loop. Everything assumes
NextUI (platform key `tg5040`).

## One-time setup

The SD card comes out exactly once, for step 2. Steps 3–5 are only for the WiFi route; the USB
path needs no on-device setup.

1. **Charge it.** A Brick that sat unused will not boot from a laptop's USB-C port. Use a wall
   charger; the LED is red charging, green when full. Give it 30 minutes.
2. **Install NextUI** if the card does not have it
   (<https://nextui.loveretro.games/getting-started/installation/>), and drop `dist/MeshClient.pak/`
   into `Tools/tg5040/` — that folder, not `dist/MeshClient.pak.zip`, whose contents unpack
   *inside* a `MeshClient.pak/` you make yourself.
3. **Join WiFi** from the NextUI Settings pak; a DHCP reservation saves grief later.
4. **Install the SSH Server pak** from Tools > Pak Store. It wraps dropbear on port 22; logins are
   `root:tina` and `trimui:trimui`. Launch it once from Tools.
5. **Tell the repo where the Brick is:**

   ```bash
   cp .brick.env.example .brick.env && $EDITOR .brick.env   # set BRICK_HOST
   make deploy-key       # installs ~/.ssh/id_ed25519.pub, asks for the password once
   make deploy-check     # confirms SSH works and reports what the device has
   ```

## The loop

```bash
make brick            # docker-pak (static aarch64) + push to Tools/tg5040/MeshClient.pak
make deploy           # push only, if dist/ is already current
make deploy-logs      # tail /.userdata/tg5040/logs/MeshClient.txt
make deploy-start     # launch it: the same hand-off the Tools menu does
make deploy-stop      # stop every MeshClient and give the device back to NextUI
make deploy-run       # start + follow the log; Ctrl-C or the client exiting stops it
make deploy-shell     # a shell on the device
```

These run over WiFi/SSH or USB, whichever is available. The pak lives at
`/mnt/SDCARD/Tools/tg5040/MeshClient.pak`, its `$HOME` at
`/mnt/SDCARD/.userdata/tg5040/MeshClient/`.

`launch.sh` runs the client at `info`. It used to hardcode `debug`, where the BLE scan alone wrote
a line per visible radio per second into a file nothing trimmed. `MESHCLIENT_LOG_LEVEL` in the
client's environment raises it again - the launcher reads it the same way it reads
`MESHCLIENT_UI_BACKEND`. NextUI execs the pak itself, so nothing on the host side passes it
through: set it in the launcher on the card, or export it in a `make deploy-shell` session and run
the pak from there. The client trims the log itself at startup once it has passed 512 KB - see
[`cli.md`](cli.md) for what it keeps and why it rewrites the file rather than rotating it.

A run that only prints keeps its output and exit status on the host:

```bash
make deploy-run ARGS="--list-devices"
make deploy-run ARGS="--status --json"
```

The map reads `$HOME/.meshclient/map.mctp`, outside the pak that self-update replaces, and
`make deploy` does not touch it. Check a pack on the board that will draw it with
`make deploy-run ARGS="--map-pack <path>"` — see [`cli.md`](cli.md#looking-inside-a-tile-pack).
On-device controls are in [`cli.md`](cli.md#on-device-controls).

### Starting it: `deploy-start`, never `launch.sh`

`make deploy-start` is the only way the client gets the device to itself. NextUI's launch loop
runs `nextui.elf` and, when it exits, runs whatever command it left in `/tmp/next`, then starts
the launcher again. A client started straight from a shell runs *beside* the launcher: both draw
to `/dev/fb0` and, since nothing grabs the pad, both act on every button. `deploy-start` writes
`/tmp/next` in the launcher's own format and takes the launcher off the screen; `deploy-stop`
brings it back. There is never more than one client — each start stops whatever is running,
because two clients fight over one radio link.

**The launcher is taken off the screen with `SIGKILL`, and must never be sent `TERM` or `INT`.**
SDL turns either into a quit event, which `nextui.elf` handles by powering off: `PLAT_powerOff()`
deletes `/tmp/nextui_exec` and touches `/tmp/poweroff`, so the loop runs the pak once more and
**shuts the Brick down the moment it exits**. `KILL` cannot be caught. A
`kill $(pidof nextui.elf)` typed into a device shell is a power-off.

## Updating without a laptop

Every release publishes `MeshClient.pak.zip` (+ `.sha256`) for a fresh install and the bare
`meshclient-tg5040-aarch64` (+ `.sha256`), which is what the client downloads when it updates
itself. **Settings > About MeshClient** shows the running version and offers `Check for updates`;
**A** on `Download and install` fetches the binary, checks the published checksum and renames it
over the running one. That is atomic and safe while the client runs — nothing changes until you
quit and relaunch.

What it needs:

- **WiFi**, and either `curl` or `wget`. NextUI ships a downloader for its Pak Store, so this is
  normally there.
- **The CA bundle in the pak** (`certs/certificates.crt`), for curl. The Brick has no system CA
  store. It ships in the pak rather than through self-update, so a client installed before this
  existed needs one pak reinstall. `SSL_CERT_FILE` or `CURL_CA_BUNDLE` override it. MQTT over TLS
  does not use it: the binary carries its own copy of the same roots.
- **A release build.** Anything you build yourself reports `<version>-dev` and is never offered an
  update, which is what stops a `make brick` deploy being replaced by whatever is on GitHub.

A `beta` or `rc` build tracks that channel; a stable build is only offered stable releases.
Self-update replaces the binary and the `version` line in `pak.json` only — **`launch.sh` and the
`Tools/` helpers ship in the zip**, so a release that changes either needs the zip unpacked into
`Tools/tg5040/MeshClient.pak/` by hand (the zip holds the pak's *contents*). Installing through
the Pak Store does the whole pak either way.

## Screenshots and clips

NextUI's own screenshot shortcut is part of `minarch` and captures that process's GL surface, so
it cannot see a pak that draws straight to `/dev/fb0`. `make deploy-shot` reads the framebuffer
over SSH instead. Output is a 1024×768 PNG converted by `scripts/frames.py` with nothing but the
Python standard library — no Pillow, no ffmpeg.

```bash
make deploy-shot                                   # grab now -> shot-<timestamp>.png
make deploy-shot ARGS="-d 10 -o nodes.png"         # 10 s to navigate there first
make deploy-shot ARGS="-n 5 -d 3 -o tour.png"      # five, 3 s apart
make deploy-shot ARGS="-P 1 -o launcher.png"       # the other page
make deploy-clip ARGS="-d 10 -n 40 -o open.gif"    # film it instead
```

Every clip frame is a 3 MB page and the device has nothing to shrink it with, so the whole clip
comes down one SSH connection at a few frames a second. **It is not real time** — `-r MS` sets
playback speed, not capture speed. `-s N` downscales, defaulting to 2.

Three things to know when a shot looks wrong:

- **`fb0` is 1024×16384**, a stack of 768-row pages the display engine flips between. The backend
  draws page 0 and mirrors into page 1, so page 0 is MeshClient and `-P 1` catches the launcher.
- **Colours are little-endian XRGB8888** (`B,G,R,X` in memory). If red and blue come out swapped,
  that assumption is what to change — three lines in `read_raw()` in `scripts/frames.py`, and the
  off-screen renderer writes the same order so both paths move together.
- **A clip of static means a short read.** `dd` is asked for one row per block rather than one
  page per block precisely so that cannot happen.

To see a UI change with no Brick at all, `make ui-capture` renders the same screens off-screen —
see [`ui.md`](ui.md#looking-at-a-ui-change).

## The buttons

Measured on-device, not assumed. `make deploy-input-map` records every `/dev/input/event*` while
you press buttons and prints both the capability bitmaps and the capture. Both halves are needed:
the bitmaps say what a device *can* emit (an absent code and an unpressed button look identical
on the wire), and the capture says which physical button emits what — the Brick's pad **declares
a `KEY_F1`, a `KEY_F2` and volume keys it never sends**. Press one at a time, about a second
apart. Nothing is grabbed, so recording is invisible to whatever is on screen.

The pad (`event3`, "TRIMUI Player1") impersonates an Xbox 360 controller, which is why **nothing
reports by position**:

| Printed on the case | evdev | Code | Logical key |
|---|---|---|---|
| D-pad up/down, left/right | `ABS_HAT0Y`, `ABS_HAT0X` | 17, 16 | `UP`/`DOWN`, `LEFT`/`RIGHT` |
| **A** (right) | `BTN_EAST` | 305 | `A` |
| **B** (bottom) | `BTN_SOUTH` | 304 | `B` |
| **X** (top) | `BTN_WEST` | 308 | `X` |
| **Y** (left) | `BTN_NORTH` | 307 | `Y` |
| **L1** / **R1** | `BTN_TL` / `BTN_TR` | 310 / 311 | `L1` / `R1` |
| **L2** / **R2** | `ABS_Z` / `ABS_RZ` | 2 / 5 | `L2` / `R2` (shift, on the keyboard) |
| **MENU** | `BTN_MODE` | 316 | quits |
| **SELECT** / **START** | `BTN_SELECT` / `BTN_START` | 314 / 315 | `SELECT` (help) / `START` |
| **F1** / **F2** | `BTN_THUMBL` / `BTN_THUMBR` | 317 / 318 | *unmapped* |

**L2 and R2 are triggers, not buttons** — there is no `BTN_TL2` in the key bitmap at all. They are
digital in practice (255 on press, 0 on release), which is what lets the client treat them as
ordinary logical keys: `mesh_ui_input_map_trigger()` reads the two axes, and a latch turns the
squeeze into one press rather than one per value an analogue pad would report on the way up. **F1 and F2 are the stick clicks**: a 360 pad
has two sticks and the Brick has none, so TrimUI spent those codes on the middle buttons.

The other nodes: `event0` (`sunxi-keyboard`) declares volume keys and emits nothing, `event1`
(`axp2202-pek`) emits `KEY_POWER` on a tap, `event2` is the audio jack. **The power button is
deliberately not a quit key** — on this hardware a tap is the sleep gesture, and sleep is the
launcher's business. **Only `event3` is watched**: the client opens every node, asks what it can
report, and drops the ones that report nothing it reads.

### Another pad, another profile

Everything above except the four face buttons is a convention. The Brick puts A on the right and
reports `BTN_EAST`; a Steam Deck or Xbox pad puts A at the bottom and reports `BTN_SOUTH`, so the
same code means *confirm* on one and *back* on the other.

**The compass names are a trap for X and Y.** `BTN_X` *is* `BTN_NORTH` (307) and `BTN_Y` *is*
`BTN_WEST` (308) — the directional aliases were added later over the older numbering and for
these two describe no real diamond. An Xbox pad's X is on the **left** and reports 307. Write
either profile by reading the compass name as a position and you get A and B right and silently
swap X and Y. `tests/suites/ui_input.c` asserts both by number.

That is why the codes and the keycaps are **one table**: `src/ui/input/input_profile.c` holds a row per
device, and `MESHCLIENT_INPUT_PROFILE` (`brick` or `xbox`) picks one for both the key mapping and
the action bar's caps. Correcting one without the other is invisible — the binding still works,
it just does the other thing, and the bar goes on promising the first.

There is no auto-detection, deliberately: a pad's evdev name is not a promise about its silkscreen,
and a nearly-right guess is the one nobody questions. Measure a new device and add the row.

## What `make deploy-check` tells you

| Line | Expected | If not |
|---|---|---|
| `sdcard` | mounted at `/mnt/SDCARD` | Nothing else will work; reseat the card |
| `bluetoothd` | running | BLE reports `waiting-for-bluez` and retries every 2 s, so it picks BlueZ up by itself — no relaunch needed |
| `dbus_socket` | `/var/run/dbus/system_bus_socket` | `launch.sh` hardcodes this in `DBUS_SYSTEM_BUS_ADDRESS` |
| `hci_adapters` | `hci0` | No adapter means `waiting-for-adapter`; toggle Bluetooth in NextUI Settings |
| `fb0` | present, `virtual_size` 1024x16384, bpp | The layer composites with per-pixel alpha, so every pixel needs an opaque alpha byte — a `0x00RRGGBB` pixel is invisible. `cat /sys/class/disp/disp/attr/sys` shows the live layer `crop` |
| `pak_sha256` | matches what `make deploy` printed | Confirms the binary on the card is the one you built |

## Troubleshooting

- **`REMOTE HOST IDENTIFICATION HAS CHANGED`** after reflashing: `ssh-keygen -R <ip>`.
- **Key not accepted after reboot:** the root filesystem may be read-only, so
  `~/.ssh/authorized_keys` might not survive. Fall back to the password, or check the SSH Server
  pak's README for its persistent key location.
- **`meshclient binary not found in PATH`:** the push did not finish. Re-run `make deploy`; it
  stages into `.MeshClient.pak.new` and swaps, so a partial copy never lands under the real name.
- **Screen stays black while the log shows the HUD active:** the display engine is showing a
  different page than the one being drawn. `cat /sys/class/disp/disp/attr/sys` prints the layer's
  `crop[x, y, w, h]`; a `y` of 768 means page 1.
- **The screen flickers to NextUI on a button press:** the client was started beside the launcher.
  `make deploy-stop`, then `make deploy-start`.
- **The Brick powered off when the client exited:** something sent `nextui.elf` a `TERM` or `INT`.
- **Buttons do nothing / the client will not exit:** press MENU (139 or 316), POWER or ESC. With
  `--log-level debug` every press is logged as `(input): key code N pressed`, so read the code out
  of the log and set `MESHCLIENT_QUIT_KEYS="139,316"` in `launch.sh`. If the log says
  `No readable /dev/input devices`, a power cycle is the only way out — report that.
- **Transfers stall:** the pak is under 5 MB, so a push takes seconds. NextUI's deep sleep turns
  the radio off, so keep the device awake. If it pings and downloads fine but SSH hangs, use USB.

## USB instead of WiFi

The same `make deploy*` targets run over USB — the path to reach for when a captive or mesh router
handles client-to-client traffic badly. NextUI runs `adbd` by default, so there is nothing to
install on the device.

- **The port.** The Brick has two USB-C ports and only one is wired to the SoC's USB gadget: the
  **port that also charges**. If `adb devices` is empty, try the other port.
- **The host.** `brew install --cask android-platform-tools`, or your distro's `android-tools`.

`BRICK_TRANSPORT` (`auto` by default) uses USB when a device is attached and falls back to SSH.
Force it with `BRICK_TRANSPORT=adb`/`ssh` in `.brick.env`, or `--transport`; with more than one
device attached set `BRICK_ADB_SERIAL`. Everything works over either transport except
`make deploy-key`, which is SSH-only.

The Brick's `adbd` is old — no `exec-out`, no no-pty shell, and it does not report remote exit
codes — so the USB path moves every byte with `adb push`/`pull` and verifies the pak by checksum
rather than by exit status. This is device data access, not USB mass storage.
