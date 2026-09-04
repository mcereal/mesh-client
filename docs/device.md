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
   boot already has a build to test.
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

The HUD is five tabs. **Left/Right** (or **L1/R1**) switch tabs, **Up/Down** move the cursor,
**A** acts on the highlighted row, **B** backs out. The Brick's A is the right-hand face button
(`BTN_EAST` 305) and B the bottom one (`BTN_SOUTH` 304).

| Tab | Shows | Buttons |
|-----|-------|---------|
| Messages | One conversation at a time: the **Inbox** (everything, each line tagged `#n` or `dm`), a channel's broadcasts, or the direct messages with one node. The highlighted message is shown in full below the list. | **A** reply to the highlighted message. **X** next conversation (Inbox, each channel, each node you have direct messages with). **Y** compose to the current conversation. |
| Nodes | The mesh as the radio sent it: short name, long name, hops or SNR, time since last heard; `*` is this radio | **A** open Compose to that node. **Y** compose to the current target. |
| Compose | `To:` row, a draft row, then the quick replies | **A** on `To:` opens the **Send to** picker (every enabled channel, then every node; Up/Down move, Left/Right jump ten rows, A picks, B cancels); on the draft row opens the keyboard; on a reply sends it. **B** back to the conversation. |
| Devices | Meshtastic radios in BLE range, `*` connected | **A** connect to that radio and make it the preferred one |
| Status | Transport state, radio, sync, my node, channel, counts | none |
| Settings | The radio's configuration, read from the radio: a section list (Radio, User, Device, Display, LoRa, Bluetooth, Channels, Security, Position, Power, MQTT, Store & Forward, Telemetry), each a list of label/value rows. Rows marked `>` can be edited (User, Display, Store & Forward, Telemetry, Bluetooth, and each channel under Channels; the rest follow `docs/settings-roadmap.md`); an edited row shows `*` and the title says `(unsaved)`. | On the section list: **A** open, **X** re-read every section from the radio (the Radio section's `Admin session` row shows the replies). In a section: **Left/Right** step a value (toggles flip, enums cycle, times step through presets), **A** flips a toggle or opens the keyboard for a name (START or `done` keeps it, `cancel` drops it), **Y** save the section to the radio (Channels and Bluetooth first show a confirm screen that spells out the consequence: Up/Down to `Save to radio`, **A**; **B** cancels), **B** back (with unsaved edits it asks once; **B** again discards). Under Channels every slot the radio has is listed, empty ones as `N (empty)`: open an empty slot and set its role to Secondary (plus a name and key) to add a channel, set an existing one to Disabled to remove it. **A** on a channel opens it; its `Key` row cycles keep / default key / new random AES-128 / AES-256 / none with Left/Right, and **A** opens the keyboard on the current key as hex to copy or type one (32 or 64 hex digits). **L1/R1** still switch tabs. After a save the footer reports the ack or rejection; most sections make the radio reboot a few seconds later, the link drops and auto-connect brings it back. |

The keyboard is a ten-column grid with lower-case, upper-case and symbol layers plus an action
row (layer, space, del, send, cancel). **D-pad** moves (wrapping), **A** types, **B** deletes
(and closes the keyboard once the draft is empty), **X** shifts for one character, **Y** is
space, **START** sends. L1 and R1 double as delete and space. Drafts survive leaving the
keyboard: the Compose draft row shows what is pending.

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
