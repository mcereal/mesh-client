# Working with the TrimUI Brick

How to get a Brick from "in a drawer" to "receives builds over WiFi", and the day-to-day loop
once it is there. Everything below assumes NextUI (platform key `tg5040`).

## One-time setup

The SD card comes out of the device exactly once, for step 2. After that everything is SSH.

1. **Charge it.** A Brick that sat unused for months has a drained cell and will not boot from
   the USB-C port on a laptop. Use a USB-A to USB-C cable into a plain 5 V wall charger. The
   power LED is red while charging and green when full; give it 30 minutes before long-pressing
   POWER. If the screen flashes on and off, keep charging.
2. **Install NextUI** if the card does not already have it: <https://nextui.loveretro.games/getting-started/installation/>.
   While the card is in the Mac, drop `dist/MeshClient.pak/` into `Tools/tg5040/` so the first
   boot already has a build to test. (That folder, not `dist/MeshClient.pak.zip`, whose
   contents unpack *inside* a `MeshClient.pak/` you make yourself.)
3. **Join WiFi** from the NextUI Settings pak. Note the IP it shows; the Brick will keep it on
   most home routers, but a DHCP reservation saves grief later.
4. **Install the SSH Server pak** from Tools > Pak Store (preinstalled with NextUI). It wraps
   dropbear and listens on port 22. Default logins are `root:tina` and `trimui:trimui`. Launch
   it once from Tools after installing; it shows the IP on screen.
5. **Tell the repo where the Brick is:**

   ```bash
   cp .brick.env.example .brick.env
   $EDITOR .brick.env            # set BRICK_HOST
   make deploy-key               # installs ~/.ssh/id_ed25519.pub, asks for the password once
   make deploy-check             # confirms SSH works and reports what the device has
   ```

## Updating without a laptop

Every release publishes four assets: `MeshClient.pak.zip` (plus a `.sha256`) for a fresh
install, and the bare `meshclient-tg5040-aarch64` binary (plus a `.sha256`), which is what the
client downloads when it updates itself.

On the device, **Settings > About MeshClient** shows the running version and offers
`Check for updates`. If GitHub has a newer release a `Download and install` row appears; **A**
on it downloads the binary, checks it against the checksum the release published, and renames
it over the running one. That last step is atomic and safe to do while the client is running -
Linux keeps the running image alive - so nothing changes until you quit and launch it again.

What it needs:

- **WiFi**, and either `curl` or `wget` on the device. Without one the About section says so
  instead of offering the rows; NextUI ships a downloader for its own Pak Store, so this is
  normally already there.
- **The CA bundle in the pak** (`certs/certificates.crt`). The Brick has no system CA store, so
  curl cannot verify github.com without it and every check fails; the About screen says "No CA
  certificates; reinstall the pak" when it is missing. It ships in the pak rather than through
  self-update, so a client installed before this existed needs one pak reinstall (from the Pak
  Store, or `make brick`) before updates start working. Set `SSL_CERT_FILE` or `CURL_CA_BUNDLE`
  to override it.
- A **release build**. Anything you build yourself reports `<version>-dev` (e.g. `1.12.0-dev`)
  and is never offered an update, which is what stops a `make brick` deploy from being replaced
  by whatever is on GitHub. Only the release workflow stamps a build as a release.

A client built from the `beta` or `rc` channel tracks that channel: it is offered the newest
release of any kind, so `1.13.0-beta.1` will be offered `1.13.0-beta.2`. A stable build is only
ever offered stable releases.

It only replaces the `meshclient` binary (and the `version` line in the pak's own `pak.json`,
so the Pak Store stops offering an update the device already has). `launch.sh` and the
`Tools/` helper binaries ship in the pak zip, so a release that changes either still needs the
zip unpacked into `Tools/tg5040/MeshClient.pak/` by hand - the zip holds the pak's contents,
not the folder, so unzip *into* the pak directory rather than next to it. The release notes
say when that is the case. Installing through the Pak Store does the whole pak either way.

`meshclient --version` prints the same number from a shell, e.g. over SSH:

```bash
make deploy-run ARGS="--version"
```

## The loop

```bash
make brick            # docker-pak (static aarch64 build) + push to Tools/tg5040/MeshClient.pak
make deploy           # push only, if dist/ is already current
make deploy-logs      # tail /.userdata/tg5040/logs/MeshClient.txt while you launch from the Tools menu
```

Launch the pak from **Tools > MeshClient** on the device for anything involving the screen. It
scans, connects on its own (the last node it talked to, or the strongest one in range if there is
no saved preference), runs the config handshake and shows the result on the HUD. **MENU** or
**POWER** quits back to NextUI; both are in the default quit-key set (the Brick's gamepad reports
MENU as `BTN_MODE` 316 and the power key as `KEY_POWER` 116).

The HUD is five tabs (Messages, Nodes, Devices, Status, Settings). **Left/Right** (or
**L1/R1**) switch tabs, **Up/Down** move the cursor, **A** acts on the highlighted row, **B**
backs out. Compose is not a tab: it opens over the conversation you are in, so it can never be
reached with a stale destination. The Brick's A is the right-hand face button
(`BTN_EAST` 305) and B the bottom one (`BTN_SOUTH` 304).

| Tab | Shows | Buttons |
|-----|-------|---------|
| Messages | Two levels. The **conversation list**: **All traffic**, then each enabled channel, then each node you have direct messages with, then **New message**; every row carries its newest message, and rows with unread traffic are marked `*` with an `N new` count instead of the usual total and age. Opening one shows that **conversation** - its messages, the highlighted one in full below the list. | On the list: **A** open the row, **Y** new message (opens the **Send to** picker). In a conversation: **A** reply (opens Compose over it), **Y** write, **B** back to the list. In **All traffic** (every line tagged `#n` or `dm`) **A** opens the conversation that line belongs to instead; it is a view rather than a conversation, so looking at it marks nothing read and its own badge is whatever the rows below still owe. |
| Nodes | Two levels. The **node list** is the mesh as the radio sent it: short name, long name, hops or SNR, time since last heard; `*` is this radio. Opening a node shows what is known about it - its actions first, then Identity, Signal, Device metrics, Position and Environment, each only as far as that node has actually reported. | On the list: **A** open the node, **X** pin or unpin it, **Y** write to it. In a node: **A** on a row acts, **B** back. The action rows are `Message this node`, `Pinned to top`, `Trace route` (asks the mesh which way it reaches the node and shows both paths with the SNR of every link), `Ask for its name` (for a node that joined after the NodeDB replay and is still a bare id), `Mute this node` (the radio stops announcing it; its messages still arrive), `Ignore this node` (the radio drops its packets before they reach us) and `Remove from radio` (drops it from the radio's node list - **A** arms it and **A** again does it, and the node returns on its own the next time it transmits). None of these rows appears on your own node. |
| Devices | Meshtastic radios in BLE range, `*` connected | **A** connect to that radio and make it the preferred one |
| Status | Transport state, radio, sync, my node, channel, counts | none |
| Settings | A section list. **About MeshClient** is first and is about the client rather than the radio - version, UI backend, where its data lives, and the self-update rows; it is the only section that works with nothing connected. The rest is the radio's configuration, read from the radio (Radio, User, Device, Display, LoRa, Bluetooth, Channels, Security, Position, Power, MQTT, Store & Forward, Telemetry), each a list of label/value rows, and **Radio actions** last - the one section that keeps nothing and writes nothing (reboot, shutdown, and the three resets). Under **Position**, `Fixed position` is shown rather than offered: the radio sets that flag itself when you use the `Set fixed position` row, which takes the `Latitude`, `Longitude` and `Altitude (m)` rows above it (pre-filled with where the radio says it is, so pinning down a GPS fix is one press). `Clear fixed position` appears only when there is one to clear. Neither waits for **Y** and neither asks first - setting a position is undone by setting another one. Rows marked `>` can be edited (User, Display, Store & Forward, Telemetry, Bluetooth, LoRa, Security, and each channel under Channels; Radio, Device, Position, Power and MQTT are read-only); an edited row shows `*` and the title says `(unsaved)`. | On the section list: **A** open, **X** re-read every section from the radio (the Radio section's `Admin session` row shows the replies). In **About MeshClient**: **A** on `Check for updates` asks GitHub for the newest release, and if there is one a `Download and install` row appears - **A** on that downloads it, checks it against the release's published checksum and swaps it in; quit and relaunch to run it. Nothing there is editable, so Left/Right and Y do nothing. In a section: **Left/Right** step a value (toggles flip, enums cycle, times step through presets), **A** flips a toggle or opens the keyboard for a name (START or `done` keeps it, `cancel` drops it), **Y** save the section to the radio (Channels, Bluetooth, LoRa and Security first show a confirm screen that spells out the consequence: Up/Down to `Save to radio`, **A**; **B** cancels), **B** back (with unsaved edits it asks once; **B** again discards). Under Channels every slot the radio has is listed, empty ones as `N (empty)`: open an empty slot and set its role to Secondary (plus a name and key) to add a channel, set an existing one to Disabled to remove it. **A** on a channel opens it; its `Key` row cycles keep / default key / new random AES-128 / AES-256 / none with Left/Right, and **A** opens the keyboard on the current key as base64 (what the phone app shows) to copy it down or type one in; hex is accepted too. Under Security the `Private key` row works the same way (keep / new random key, **A** to reveal or restore a backup) and the three `Admin key` rows take a phone's public key (keep / none / typed). **L1/R1** still switch tabs. After a save the footer reports the ack or rejection; most sections make the radio reboot a few seconds later, the link drops and auto-connect brings it back. In **Radio actions** nothing is edited and Y does nothing: **A** on a row opens the same confirm screen, and only answering it sends anything. `Reboot` drops the link and auto-connect brings it back; `Shutdown` does not, and the radio has to be switched on by its own button (the row reads `not supported` on a board that cannot cut its own power); `Reset node database` empties the radio's node list, favorites excepted, and leaves the Brick's own cached list alone; `Factory reset config` returns every setting to its default but keeps the Bluetooth bond; `Factory reset device` clears the bond too, so the node has to be forgotten in Devices (**Y**) and paired again. |

The keyboard is a ten-column grid with lower-case, upper-case and symbol layers plus an action
row (layer, space, del, send, cancel). **D-pad** moves (wrapping), **A** types, **B** deletes
(and closes the keyboard once the draft is empty), **X** shifts for one character, **Y** is
space, **START** sends. L1 and R1 double as delete and space. Drafts survive leaving the
keyboard: the Compose overlay's draft row shows what is pending.

Nodes are ranked: this radio, then every node you have exchanged messages with, then nodes
heard directly over RF (most recent first), then nodes that only arrive via MQTT. Every packet
a node sends refreshes its place. The HUD carries 128 of them; the radio's full NodeDB count is
shown in the Nodes title.

If the radio drops the BLE link (it happens after a few minutes idle on some firmware), the
footer flips from green `connected: <name>` to `running`, a toast says the link was lost, and
auto-connect brings it back within a few seconds. A message sent in that window is tagged `!!`
rather than pretending it went out; send it again once the footer is green.

Quick replies default to OK / Yes / No / On my way / Where are you? / I'm here / Call me / Need
help / Heading back / Ping. Put your own, one per line, in
`/mnt/SDCARD/.userdata/tg5040/MeshClient/.meshclient/canned.txt` (`#` comments allowed, 16 max).
Sends to a node ask for an ack; the Messages tab tags them `..` while pending, `ok` when
delivered, `!!` when routing failed. Broadcasts go out on the channel shown in `To:`; the
channel table comes from the radio during the config sync. Text is drawn at four times the 5x7
font; set `MESHCLIENT_FB_SCALE=3` in `launch.sh` for more rows or `5` for bigger type.

The framebuffer backend and the NextUI launcher share `/dev/fb0`, so a run started over SSH
while the launcher is on screen may get painted over. For headless checks SSH is fine:

```bash
make deploy-run ARGS="--list-devices"
make deploy-run ARGS="--status --json"
```

To watch a launch from the Mac, run `make deploy-logs` first, then start the pak from the Tools
menu; the log shows discovery, `Auto-connecting to ...`, the handshake, and every button press.

`make deploy-shell` drops you into a shell on the device. The pak lives at
`/mnt/SDCARD/Tools/tg5040/MeshClient.pak`, its `$HOME` (prefs, handshake cache) at
`/mnt/SDCARD/.userdata/tg5040/MeshClient/`.

## Screenshots

NextUI's own screenshot shortcut is part of `minarch`, the emulator runtime, and captures that
process's GL surface - so it cannot see a pak like ours, which draws straight to `/dev/fb0`.
`make deploy-shot` reads the framebuffer over SSH instead, which catches whatever is actually
on the panel: our HUD, the launcher, a crash. Nothing extra is needed on the device.

```bash
make deploy-shot                                   # grab now -> shot-<timestamp>.png
make deploy-shot ARGS="-d 10 -o nodes.png"         # 10 s to navigate there first
make deploy-shot ARGS="-n 5 -d 3 -o tour.png"      # five, 3 s apart -> tour-1.png ...
make deploy-shot ARGS="-P 1 -o launcher.png"       # the other page (see below)
```

Output is a 1024x768 PNG written where you ran the command, converted on the host with nothing
but the Python standard library - no Pillow, no ffmpeg. This is how the screenshots for the Pak
Store listing get made.

**Launch MeshClient from the Tools menu, not over SSH.** Started with `nohup ./launch.sh` from
an SSH session, the client does draw - but NextUI's launcher is still the foreground app and
keeps repainting `fb0` over it, so every shot comes back as the launcher's menu on both pages.
Opening the pak from Tools is what suspends that repaint. (The client itself runs fine either
way; this only affects what is on the panel.)

Two things to know when a shot looks wrong:

- **`fb0` is 1024x16384**, a stack of 768-row pages the display engine flips between. The fb
  backend draws page 0 and mirrors into page 1, so page 0 is MeshClient. NextUI's SDL usually
  leaves the panel on page 1, so `-P 1` is what catches the launcher.
- **Colours are read as little-endian XRGB8888** (`B,G,R,X` in memory), which is what the Brick
  reports. If red and blue ever come out swapped, that assumption is what to change - the
  channel assignment is four lines in `cmd_shot`.

## What `make deploy-check` tells you

It runs a busybox-only script on the device and prints one line per fact. The ones that matter
for MeshClient:

| Line | Expected | If not |
|---|---|---|
| `sdcard` | mounted at `/mnt/SDCARD` | Nothing else will work; reseat the card. |
| `bluetoothd` | running | The BLE transport will sit in `waiting-for-bluez`. NextUI starts BlueZ for Bluetooth audio, so this should be up when Bluetooth is enabled in Settings. |
| `dbus_socket` | `/var/run/dbus/system_bus_socket` | `launch.sh` hardcodes this path in `DBUS_SYSTEM_BUS_ADDRESS`. If the socket is elsewhere, fix `launch.sh`. |
| `hci_adapters` | `hci0` | No adapter means `waiting-for-adapter`. Toggle Bluetooth in NextUI Settings. |
| `fb0` | present, with `virtual_size` and `bpp` | The fb backend needs these; the Brick panel is 1024x768. `virtual_size` is 1024x16384: a stack of 768-row pages that NextUI's SDL flips between, and the display keeps showing whichever page SDL last presented (page 1, rows 768..1535, in practice). The backend pans back to page 0 after every frame and mirrors into page 1 as a fallback. The layer also composites with per-pixel alpha, so every pixel is written with an opaque alpha byte; a `0x00RRGGBB` pixel is invisible. `cat /sys/class/disp/disp/attr/sys` shows the live layer `crop`. |
| `pak_sha256` | matches the value `make deploy` printed | Confirms the binary on the card is the one you built. |

## Troubleshooting

- **`REMOTE HOST IDENTIFICATION HAS CHANGED`** after reflashing the card: `ssh-keygen -R <ip>`.
- **Key not accepted after reboot:** the root filesystem on the Brick is small and may be
  read-only, so `~/.ssh/authorized_keys` might not survive. Fall back to the password, or check
  the SSH Server pak's README for its persistent key location.
- **`meshclient binary not found in PATH`** in the log: the push did not finish, or `launch.sh`
  was run from the wrong directory. Re-run `make deploy`; it stages into `MeshClient.pak.new`
  and swaps, so a partial copy never lands under the real name.
- **Screen stays black while the log shows the HUD backend active:** the display engine is
  showing a different framebuffer page than the one being drawn. `cat
  /sys/class/disp/disp/attr/sys` on the device prints the layer's `crop[x, y, w, h]`; a `y` of
  768 means page 1. Builds before the pan fix never handled this. A quick check that the panel
  really is fb0: `head -c 3145728 /dev/zero | tr '\000' '\377' | dd of=/dev/fb0 bs=4096
  seek=768 conv=notrunc` paints page 1 white until the next redraw.
- **Nothing on screen but the log shows discovery working:** the fb backend lost the
  framebuffer to the launcher. Exit to the Tools menu and launch the pak from there.
- **Buttons do nothing / the client will not exit:** press MENU. The client watches every
  `/dev/input/event*` node and quits on MENU (`KEY_MENU` 139 or `BTN_MODE` 316), POWER or ESC;
  SELECT and START are navigation keys, not quit keys. If none of those work, the Brick reports
  different codes: with `--log-level debug` (what `launch.sh` passes) every press is logged to
  `MeshClient.txt` as `(input): key code N pressed`, so run the pak, press the button you want,
  read the code out of the log, and set it in `launch.sh`:

  ```sh
  export MESHCLIENT_QUIT_KEYS="139,316"   # comma-separated evdev codes; replaces the defaults
  ```

  If the log says `No readable /dev/input devices`, nothing can quit the client from the
  device and a power cycle is the only way out - report that, it means the pak is not seeing
  the Brick's input nodes at all.
- **`make deploy-run` output never reaches `MeshClient.txt`:** the pak's `launch.sh` predates
  the absolute `PAK_DIR` fix and wrote to `logs/.txt` with `$HOME` at `.userdata/tg5040/`
  when started as `./launch.sh`. Redeploy; move anything useful out of
  `.userdata/tg5040/.meshclient/` into `.userdata/tg5040/MeshClient/.meshclient/`.
- **Transfers are slow:** the pak is small (well under 5 MB), so a push should take a few
  seconds. If it stalls, the Brick has dropped WiFi; NextUI's deep sleep turns the radio off, so
  keep the device awake while pushing.

## USB instead of WiFi?

Not worth it today. NextUI does not expose USB mass storage by default, the community
mass-storage pak warns about SD corruption, and ADB is not confirmed to be enabled out of the
box. WiFi plus the SSH Server pak is the supported path.
