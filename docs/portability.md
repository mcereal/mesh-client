# Other handhelds: what runs this today, what needs work, what cannot

The Brick is the target. This is what it would take to run the client somewhere else, device by
device — written because the question is now being asked faster than it can be answered one reply
at a time.

**Read the tiers as claims of different strength.** Where something is marked *measured*, it was
run. Where it is marked *inferred*, it follows from a published spec and the tree's own code, and
nobody has put it on hardware. The distinction matters here more than usual, because this tree
already refuses to ship a guessed input profile on the grounds that "a profile guessed from a spec
sheet is worse than no profile: the fallback is at least a known wrong answer, where a guess that
is nearly right is the one a user stops questioning"
([`src/ui/input_profile.c`](../src/ui/input_profile.c)). The same rule governs this document. A
device below is not supported because it appears here.

## The five questions

Every device answers the same five, and the answers are close to independent. A device that fails
question 5 has no use for a perfect score on the other four.

| # | Question | Where the client decides it |
|---|---|---|
| 1 | Is there a Linux userspace to drop a static binary into? | not Android's; a pak, a port, or a shell |
| 2 | Is it **aarch64**? | [`scripts/cross-build.sh`](../scripts/cross-build.sh) asserts both aarch64 and static and refuses anything else |
| 3 | Is `/dev/fb0` writable, with nothing else holding the display? | `mesh_ui_backend_fb_is_available()`, else the CLI backend |
| 4 | Does the pad speak evdev, and has somebody **measured** its four face buttons? | [`src/ui/input_profile.c`](../src/ui/input_profile.c) |
| 5 | Is there a way to reach a radio — BlueZ over D-Bus, a USB host port, or Wi-Fi? | [`src/transport/`](../src/transport/) |

Question 5 is the one that sorts the field, and it is worth being precise about the three answers,
because they are not equally available:

- **BLE** needs Bluetooth *hardware*, a kernel that drives it, **and** a running `bluetoothd`
  reachable on the system bus. The three come apart: no D-Bus headers at build time compiles the
  transport out and it reports `disabled`; headers but no reachable bus leaves it compiled in and
  sitting in `waiting-for-bluez`. A handheld whose CFW ships Bluetooth *audio* has usually cleared
  all three, since that is BlueZ too.
- **USB serial** needs the port to do host mode and the kernel to carry the right driver. There is
  **no VID/PID allowlist** — that trade is refused on purpose, since it would have to be
  maintained against every board Meshtastic supports — but there *is* a **driver** allowlist, and
  a port should check against it rather than against "does USB work". `mesh_serial_usb_scan()`
  offers an interface when its class is CDC-data, or when its bound driver is one of
  `k_serial_drivers[]` — `cp210x`, `ch341`, `ch341-uart`, `ftdi_sio`, `generic`, `cdc_acm`
  ([`src/transport/serial/serial_usb.c`](../src/transport/serial/serial_usb.c)). A vendor-specific
  interface bound by anything else is skipped, so a board behind a bridge outside that list is not
  merely unlisted by accident — it will never appear, and the kernel having *a* serial driver is
  not enough to conclude the transport is available.
- **TCP** needs only Wi-Fi and a Meshtastic node with its own network access on. This is the
  escape hatch for a device with no Bluetooth at all, and it is reached from the device: the last
  row of the Devices tab is the network radio, A on it types an address and the address is
  remembered. A launcher line still works and still wins for one launch:

  ```sh
  export MESHCLIENT_TCP_HOST=192.168.1.50
  ```

  **It must be a numeric address.** `getaddrinfo()` blocks and this client is one epoll loop with
  no threads, so a hostname is refused rather than silently freezing the UI — which is also what
  makes the on-device field a small one: it holds digits, dots and colons and nothing else. See
  [`docs/transport.md`](transport.md#typing-one-the-devices-tabs-last-row).

Two things that look like blockers and are not. **Panel size is not one** — the renderer measures
everything it draws in cells, so a smaller screen reflows rather than clipping; see
[below](#640x480-was-rendered-not-guessed). **Pixel depth is not one either**: `fb_store_span()` in
[`src/ui/backends/fb_draw.c`](../src/ui/backends/fb_draw.c) already packs 32-, 24- and 16-bit
(RGB565) framebuffers, and `bytes_per_pixel` is read off the kernel rather than assumed.

## The short answer, by device

| Device | SoC / arch | Verdict | What it needs |
|---|---|---|---|
| **TrimUI Brick** | A133P, aarch64 | **Ships today** | nothing — this is the target |
| **TrimUI Smart Pro** | A133P, aarch64 | **Very likely works as-is** *(inferred)* | same `tg5040` pak; confirm the buttons |
| **TrimUI Smart Pro S** | A523, aarch64 | **Likely a repackage** *(inferred)* | a `tg5050` pak dir + platform key |
| **Anbernic H700 family** (RG35XX Plus/H/SP, RG40XX, RG28XX, CubeXX) | H700, aarch64 | **Needs work, no new core code** | a pak/port for the CFW, a measured button row, BlueZ confirmed |
| **RK3566 family** (RG353x, RGB30, …) under ROCKNIX/ArkOS | RK3566, aarch64 | **Needs work, no new core code** | same as above |
| **Miyoo Mini Plus** | SSD202D, **armv7** | **Needs real work**, and BLE is off the table | a 32-bit toolchain, and TCP as the only transport |
| **Miyoo Mini** (original) | SSD202D, armv7 | **Effectively no** | as above, and no Wi-Fi either — no transport at all |
| **Steam Deck** | x86-64 | **Builds and talks to a radio; no panel UI** | an SDL backend — see [`docs/steamdeck.md`](steamdeck.md) |
| **Retroid Pocket, Odin, any Android handheld** | aarch64, Android | **Out of scope** | a different application |

## TrimUI Smart Pro — the one that is probably already done

**`tg5040` is not the Brick's platform key. It is TrimUI's, and the Smart Pro shares it.** NextUI's
own tree carries `workspace/tg5040` and `workspace/tg5050`, and NextUI lists the Brick, the Smart
Pro and the Smart Pro S as its supported devices. The pak this repo publishes installs to
`Tools/tg5040/` on either machine, and the binary inside it is the same static aarch64 ELF.

The Smart Pro also clears question 5 outright: Bluetooth 5.4 with BLE, where the Brick's own
BlueZ-over-D-Bus path already works.

The differences are two, and only one is real work:

- **The panel is 1280x720 rather than 1024x768.** Not a problem — measured, by rendering the real
  nav model into that geometry:

  ```bash
  make ui-capture ARGS="-g 1280x720 devtools/ui_capture/scenes/status-verbs.scene -o sp.gif"
  ```

  The layout simply has more room. Nothing clips, nothing needs a scale change.

- **The buttons are unverified.** The Brick's row exists because every code in it was read off the
  device by pressing that button, and the Brick is the cautionary tale for why: it prints Nintendo's
  letters over an Xbox 360 pad's codes, so A is `BTN_EAST` and the button printed **Y** is
  `BTN_NORTH`. A Smart Pro is the same manufacturer and very likely the same table, which is
  exactly the kind of "nearly right" this tree refuses to assume. Run `make deploy-input-map`, press
  the four buttons, and either reuse the `brick` row or add a `smartpro` one beside it.

**So the honest instruction to a Smart Pro owner today is: try it, and tell us what the buttons
do.** If confirm and back are swapped, set `MESHCLIENT_INPUT_PROFILE=xbox` in `launch.sh` and try
again. That single report is most of the porting work.

The Smart Pro S (`tg5050`, Allwinner A523) is still aarch64, so the binary should run; it wants a
`Tools/tg5050/` scaffold and a second entry in `pak.json`'s `platforms`. Neither is written,
because neither has been tested.

## Miyoo Mini Plus — the one everybody asked about

Short version: **the UI is nearly free, and the two things that are hard are the CPU and the
radio link.** It is not impossible. It is a genuine port, and it ends somewhere less capable than
a Brick.

### 640x480 was rendered, not guessed

The most common worry is the screen, and it is the least of the problems. The Mini Plus is 640x480
and `/dev/fb0` on a Miyoo Mini is 640x480 at 32bpp. Rendering the real navigation model and the
real `fb_render_snapshot()` into that geometry:

```bash
make ui-capture ARGS="-g 640x480 -s 3 devtools/ui_capture/scenes/status-verbs.scene -o mmp.gif"
```

**Pick a scene that does not pin its own scale, or `-s` does nothing.** A scene's `scale` line
wins over the flag, exactly as its `theme` and `delay` do, and most of the scenes in that
directory state `scale 4` because they were written to review the Brick. `status-verbs`,
`devices`, `waypoints`, `node-cards`, `cards` and `map` leave it unset, so they are the ones to
reach for when what you are testing *is* the scale. A scene of your own is three lines:

```bash
printf 'scene demo\ntab nodes\nhold 400\n' | ./scripts/ui-capture.sh -g 640x480 -s 3 -o nodes.gif
```

**Measured:** every tab composes correctly at 640x480. At the device default of scale 4 it is
legible but cramped — three node rows fit. At **scale 3** it is the right picture: five node rows,
the Status cards intact with their verbs, the settings list eleven rows deep, and the action bar
complete with its keycaps. Nothing clips and nothing overflows, because layout here is measured in
cells rather than written in pixels.

So the panel costs one line in `launch.sh`:

```sh
export MESHCLIENT_FB_SCALE=3
```

### The CPU is the real work

The SSD202D is a **dual-core Cortex-A7** — ARMv7, 32-bit — where every shipped binary here is
aarch64. [`scripts/cross-build.sh`](../scripts/cross-build.sh) asserts this on purpose:

```
case "$DESCRIPTION" in
    *"ARM aarch64"*) ;;
    *) echo "not an aarch64 binary: $DESCRIPTION" >&2; exit 1 ;;
```

That check is not the obstacle, it is the accounting of it. What is needed is a second cross
toolchain (armv7, static, with libdbus cross-built the way the aarch64 one is), a second cross
container, and a second release asset. The good news is that the tree is portable C17 and the
only architecture-specific code in it is the crash handler's register read
([`src/utils/crash.c`](../src/utils/crash.c)), which has a clean `#else return false` for anything
that is not aarch64 or x86-64 — so **armv7 costs the backtrace, not the build**. The report still
gets written; it just has no PC and no frame walk in it.

128 MB of RAM shared with the GPU is the other half of that question and is unmeasured. The static
binary and its fixed buffers are modest, but the map tile decoder alone holds 48 KiB plus 256 KiB
of scratch and the cache is byte-budgeted — a port should expect to tune `MESHCLIENT_MAP_*` down
or leave the basemap out.

### There is no Bluetooth, so there is no BLE

This is the part that changes what the client *is* on this device. Neither the stock OS nor OnionOS
offers Bluetooth on the Mini Plus, and the consistent community answer is that the hardware is not
there to drive. Marketing copy and several spec aggregators claim "Wi-Fi and Bluetooth"; the
firmware does not agree, and the firmware is what has to open the socket.

So BLE — the client's primary transport, and the one the Brick uses — is simply unavailable.

That leaves two:

- **USB serial over the USB-C port.** Unverified, and the honest answer is *probably not without
  kernel work*. The port is used in gadget mode (USB networking) rather than host mode, the
  firmware is closed, and the kernel ships with no `/proc/config.gz` — so establishing whether any
  of the drivers named above is even present, let alone building one out of tree against a config
  nobody published, is its own project. Do not promise this one.
- **TCP over Wi-Fi.** This works, and it is the answer. The Mini Plus has Wi-Fi; point it at a
  Meshtastic node that has its own network access enabled:

  ```sh
  export MESHCLIENT_TCP_HOST=192.168.1.50
  ```

  A numeric address, per the rule above. Everything downstream of the link — the roster, the
  message log, settings, waypoints, the charts — is transport-agnostic and behaves identically.

**So a Miyoo Mini Plus port is a Wi-Fi client for a network-connected node, not a pocket BLE
terminal for a radio on your belt.** That is a real and useful thing, and it is worth saying
plainly rather than letting somebody discover it after building a toolchain.

### And the launcher

OnionOS is not NextUI/MinUI: `launch.sh`'s `SDCARD_PATH`, its `.userdata/$PLATFORM/` layout and
the `Tools/<platform>/` pak convention are all NextUI's. An Onion port needs its own launcher
scaffold. That is shell, not C.

### Summing the Mini Plus

| Question | Answer |
|---|---|
| 1. Linux userspace | yes |
| 2. aarch64 | **no** — armv7, needs a second toolchain |
| 3. `/dev/fb0` | yes, 640x480; scale 3, *measured* |
| 4. evdev pad | almost certainly, needs a measured profile row |
| 5. transport | **TCP over Wi-Fi only** — no BLE, USB doubtful |

The original Miyoo Mini has no Wi-Fi, which removes the last transport. There is nothing to
connect to a radio with, so it is a no.

## The Anbernic H700 family, and RK3566

Both families are **aarch64 with real Bluetooth**, which is the combination that matters: they
clear questions 2 and 5, the two the Miyoo fails. The H700 devices (RG35XX Plus, RG35XX H, RG35XX
SP, RG40XX, RG28XX, CubeXX) are quad Cortex-A53 with 1 GB of RAM and Bluetooth 4.2 — more machine
than a Brick. The RK3566 devices are the same story a generation over.

What they need is not core code. It is:

1. **A pak or port for whichever CFW** — muOS, Knulli, ROCKNIX, ArkOS. `launch.sh` is NextUI's
   convention and each of these has its own.
2. **A confirmation that `bluetoothd` is actually running and reachable on the system bus.** A CFW
   that pairs Bluetooth controllers or plays Bluetooth audio is BlueZ, so this is promising —
   Knulli is Batocera-derived, which ships the whole stack — but it is a thing to check rather than
   assume, and it is exactly the `waiting-for-bluez` case if the bus is not there.
3. **A measured input profile row**, as always.
4. **A check that nothing else holds the display.** These CFWs are generally fbdev or plain KMS
   with the frontend in charge, so stopping the frontend should hand over `/dev/fb0` — unlike a
   Steam Deck, where a compositor holds DRM master and writes to `fb0` silently go nowhere.

None of that is speculative in the way the Miyoo is. It is porting work, and the binary that comes
out is the full client with BLE.

## Android handhelds are a different application

A Retroid Pocket, an Odin, an AYN — these are aarch64 Linux kernels, but there is no writable
`/dev/fb0`, no system D-Bus to reach BlueZ on, and no unprivileged evdev. The client's whole device
layer is the wrong shape. Anyone on Android is better served by the official Meshtastic Android
app, which is excellent and does not need porting.

## If you want to help

The highest-value contributions, in order, and the first one is not code:

1. **Own a TrimUI Smart Pro?** Install the existing pak into `Tools/tg5040/`, run it, and report
   what the four face buttons do. That may close the whole device.
2. **Own an H700 or RK3566 handheld?** Report whether `bluetoothctl` works on your CFW and whether
   `/dev/fb0` is writable once the frontend is stopped. Those two answers decide the family.
3. **Measure a pad.** `make deploy-input-map` equivalents, a row in
   [`src/ui/input_profile.c`](../src/ui/input_profile.c) with the codes *and* the printed letters
   together — the two live in one row precisely because correcting one without the other is
   invisible.
4. **The armv7 toolchain**, if the Miyoo matters to you. It is the one item here that is a
   genuine build-system project rather than a configuration.

Reviewing how any change looks on another panel needs none of this and no hardware:

```bash
make ui-capture ARGS="-g 640x480 -s 3 devtools/ui_capture/scenes/status-verbs.scene -o small.gif"
make ui-capture ARGS="-g 1280x720 devtools/ui_capture/scenes/status-verbs.scene -o wide.gif"
```
