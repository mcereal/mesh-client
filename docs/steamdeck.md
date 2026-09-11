# Running mesh-client on a Steam Deck

The Brick is the target; a Steam Deck is a useful second machine, because it is a Linux handheld
with a real Bluetooth adapter, a gamepad that speaks evdev, and enough room to build the tree
natively. This is what that costs, what works today, and what a UI on its panel would actually
take.

Everything below was measured on SteamOS `holo` (kernel 6.16.12-valve24.5, x86_64) on
2026-09-11, against a Heltec V3 running firmware 2.7.26.54e0d8d.

**Short version.** The client builds, passes its whole suite, and talks to a radio over BLE on a
Steam Deck today. What it does *not* do is draw on the panel: it falls back to the CLI backend,
and three separate things stand between it and `/dev/fb0`. The cheapest way through them is not to
fix the framebuffer at all — it is a ~150-line SDL backend over the seam
[`fb_capture.h`](../include/mesh/ui/backends/fb_capture.h) already exposes.

## Building: SteamOS has no toolchain, and must not grow one

There is no compiler on a Steam Deck. No `cc`, no `cmake`, no `make`, no `pkg-config` — the image
ships a runtime, not a build host. `make setup` cannot help: `scripts/setup-linux.sh` is apt-based
and SteamOS is Arch-derived, and even `pacman -S base-devel` is the wrong move, because the rootfs
is managed by an atomic A/B update that replaces it wholesale. Packages installed into the OS image
do not survive an update, and neither does anything else written to `/etc` or `/usr`.

So the toolchain goes in a container, and SteamOS ships the two things needed to run one rootless:
`podman` and `distrobox`. Use `ubuntu:24.04` — not for taste, but because
[`docker/Dockerfile`](../docker/Dockerfile)'s dev stage is Ubuntu 24.04 and the tree is normalised
with **clang-format 18**, which is what that release carries. A container built on anything else
gets `make format` refused, exactly as a host with the wrong major does.

```bash
distrobox create --name meshdev --image ubuntu:24.04 --yes \
    --additional-flags "--volume /run/dbus:/run/dbus"
distrobox enter --name meshdev -- bash -lc '
    sudo apt-get update -qq
    sudo apt-get install -y -qq --no-install-recommends \
        build-essential clang clang-format libclang-rt-18-dev cmake ninja-build pkg-config \
        libdbus-1-dev dbus-daemon protobuf-compiler python3 python3-pip git zip ca-certificates
    python3 -m pip install --no-cache-dir --break-system-packages -q protobuf grpcio-tools'
```

**The `/run/dbus` mount is the whole difference between this and `make docker-test`.** Inside the
dev container there is no BlueZ, so the BLE transport sits in `waiting-for-bluez` and the CLI
backend is selected. Binding the host's system bus socket into the container hands it the *host's*
BlueZ instead: `pkg-config --exists dbus-1` succeeds at build time so `MESH_HAVE_DBUS` is set, and
`org.bluez` answers at run time so the transport actually connects. Without it the BLE transport
compiles out and reports `disabled`, which on a machine with a working adapter three inches away
reads as a bug in the client.

Two details make the rest work by themselves. distrobox shares the home directory, so the repo is
at `/home/deck/mesh-client` inside the container and out, and the CMake cache's absolute paths
agree either way. And `BUILD_ROOT` is left at the default `build/` rather than the container
convention `build/linux`: that split exists so a container build cannot collide with a *host*
configure, and on this machine a host configure is impossible, so there is nothing to collide with.

From there everything in [`CLAUDE.md`](../CLAUDE.md) works unchanged:

```bash
distrobox enter --name meshdev -- bash -lc 'make test'
```

Measured: all five ctest targets pass, zero compiler warnings.

## Pairing: the PIN is on the radio's screen, and it is not 123456

A Heltec V3 has an OLED, so Meshtastic generates a **random** six-digit passkey per pairing attempt
and displays it there. The fixed `123456` that PIN-less boards use is rejected, and what comes back
is `org.bluez.Error.AuthenticationFailed` — which looks like a transport bug and is not one.

The client's own pairing agent registers correctly (`Pairing agent registered at
/org/meshclient/agent`) and reports `needs pairing; press A on it in Devices`. That instruction
assumes the fb UI, and on a machine where the fb UI does not come up (which, for now, is this one)
there is nowhere to type the digits. So bond out of band first, once:

```bash
bluetoothctl
  agent KeyboardDisplay
  default-agent
  pair 9C:13:9E:9D:0A:D9      # read the six digits off the radio's OLED, type them
  trust 9C:13:9E:9D:0A:D9
```

BlueZ keeps the bond afterwards, and `meshclient -p <mac>` connects with no further prompting.
`trust` is worth the extra line: without it the bond survives but auto-reconnect does not.

## What works today

With the radio bonded, a foreground run on a Steam Deck reaches the same state it reaches on a
Brick — auto-connect, GATT discovery, config sync, the admin session, the node roster:

```bash
distrobox enter --name meshdev -- bash -lc \
    './build/debug/meshclient -f --disable-serial --disable-tcp -p 9C:13:9E:9D:0A:D9'
```

Measured against the Heltec: link up in ~2 s, config sync complete in ~20 s, `MyNodeInfo` with 147
NodeDB entries, `hw_model 43`, channel 0 `<default>`, admin replies with the session passkey held,
and the client setting the radio's clock — which is a genuine advantage of this host over the
Brick, since a Deck has a battery-backed clock and a Brick does not.

`--disable-serial` is not optional here; see [below](#the-serial-transport-picks-up-the-decks-own-controller).

## The UI, and the three things between it and the panel

`mesh_app_select_backend()` asks `mesh_ui_backend_fb_is_available()`, which is
`access("/dev/fb0", R_OK | W_OK) == 0`, and falls back to the CLI backend when that fails. On a
Steam Deck it fails, and fixing the access is the *least* of what stands in the way.

**1. `/dev/fb0` is not openable, and the fix does not persist.** The node is `root:video` mode
`0660`, and the `deck` user is in `deck`, `steamos-log-submitter` and `wheel` — not `video`.
`sudo usermod -aG video deck` closes it, but `/etc/group` lives on the rootfs that the atomic
update replaces, so it is a change that silently reverts on a SteamOS update. A udev rule has the
same problem for the same reason.

**2. A compositor holds DRM master.** `amdgpudrmfb` is fbdev *emulation* over KMS. In desktop mode
`kwin_wayland` owns the display; in gaming mode `gamescope` does. Writes to `/dev/fb0` while
another process holds DRM master land in a buffer that is never scanned out — they do not fail,
they do nothing, which is the worse of the two outcomes to debug. Getting pixels onto the panel
means a VT with no compositor on it (`sudo openvt -s -- ...`), which is root again, and takes the
desktop down while the client runs.

**3. The panel is portrait and the fb backend does not rotate.** Measured: `/dev/fb0` is
**800x1280**, 32 bpp, stride 3328 (800 pixels padded to 832), single-paged — `yres_virtual` equals
`yres`, so the Brick's page-1 mirror in `fb_copy_damage()` never engages. eDP-1's only mode is
`800x1280`. The panel is physically mounted portrait and the compositor is what turns it the right
way up; a process writing raw to fbdev inherits none of that, so the UI would render **sideways**
on a Deck held normally.

Rotating in the backend is not the small change it sounds like.
[`fb.c`](../src/ui/backends/fb.c)'s present path is `fb_copy_damage()`, which walks rows, compares
each against the previous frame, narrows to the changed byte span, and `memcpy`s it — one
contiguous copy per changed row, which is what makes the partial redraw cheap. Under a 90° present
a row of the UI is a *column* of the panel, so every one of those copies becomes a strided
per-pixel write and the damage tracking stops paying for itself. The transform belongs at present
time rather than in layout — the tree's rule is that layout is measured in cells, and rotation is a
property of the panel, not of the screen being drawn — but the cost lands squarely on the one
optimisation that path exists for.

### What it would actually take: an SDL backend over the capture seam

The cheapest real UI on this machine does not touch the framebuffer at all.

[`fb_capture.h`](../include/mesh/ui/backends/fb_capture.h) is already a public, shipping seam that
does most of the work: it is, in its own words, "the fb backend with the device taken out of it" —
the same `fb_render_snapshot()`, the same palette, the same cell measurement, drawing into a
malloc'd page instead of an mmap of `/dev/fb0`. `mesh_ui_capture_pixels()` hands back that page as
32 bpp `B,G,R,X`, "byte-for-byte what `/dev/fb0` holds on the device". That is precisely an
`SDL_PIXELFORMAT_ARGB8888` texture.

And `struct mesh_ui_backend` is five fields — `name`, `init`, `shutdown`, `present`, `animating`.
So the backend is:

| vtable slot | body |
|---|---|
| `init` | `SDL_Init`, `SDL_CreateWindow`, `SDL_CreateRenderer`, `SDL_CreateTexture(ARGB8888, STREAMING)`, `mesh_ui_capture_open(w, h, scale)` |
| `present` | `mesh_ui_capture_render(capture, snapshot)` → `mesh_ui_capture_pixels()` → `SDL_UpdateTexture` → `SDL_RenderCopy` → `SDL_RenderPresent` |
| `animating` | `mesh_ui_capture_animating(capture)` |
| `shutdown` | the teardown |

Call it ~150 lines. It clears all three blockers at once and buys two more things: the compositor
handles the rotation *and* the scaling, so the client renders into a landscape geometry of its own
choosing and never learns the panel is portrait; and it runs in desktop mode **and** in gaming
mode, as a non-Steam shortcut, with no root and no VT switch.

Three things to get right:

- **The capture clock is scripted on purpose.** `mesh_ui_capture_advance()` exists because "a
  capture whose contents depend on the machine that took it is not a reviewable picture" — the
  harness names the time so a transition is reproducible frame for frame. A live backend wants the
  opposite, so it must drive `advance()` from `mesh_time_monotonic_ms()` each frame. It only ever
  moves forwards, which is exactly the contract a monotonic clock satisfies.
- **It must not reach the Brick's binary.** The pak is a static aarch64 link built without
  `--gc-sections`, so anything compiled in is shipped. This goes behind an opt-in
  `-DMESHCLIENT_ENABLE_SDL=ON` that `scripts/cross-build.sh` never sets.
- **The selector already has room.** `MESHCLIENT_UI_BACKEND` is documented as `fb|cli|stub`; `sdl`
  slots in beside them, and `mesh_app_select_backend()`'s existing fall-through does the rest.

SteamOS ships SDL2 (2.32.56) and SDL2_ttf system-wide, and `libsdl2-dev` is one `apt-get` away in
the container, so neither side needs anything vendored.

**The alternatives are worse.** Raw fbdev on a VT is no new code but costs root, a rotation
transform, the damage path, and the desktop session while it runs — it is a diagnostic, not a way
to use the client. A Wayland backend written directly against `libwayland-client` is strictly more
code than the SDL one for no benefit the client can use, since it wants a single ARGB surface and
nothing else.

## Input is very nearly free

This is the half that is already done, because the tree anticipated it.
[`docs/device.md`](device.md#another-pad-another-profile) says so outright: "a Steam Deck, an Xbox
pad and most USB controllers put A at the bottom and report it as `BTN_SOUTH`" — which is the
`xbox` row of [`src/ui/input_profile.c`](../src/ui/input_profile.c), already written.

Measured, with **no Steam process running**, `/dev/input/event10` is `Microsoft X-Box 360 pad 0`
and declares exactly:

```
BTN_SOUTH BTN_EAST BTN_NORTH BTN_WEST   BTN_TL BTN_TR   BTN_SELECT BTN_START BTN_MODE
BTN_THUMBL BTN_THUMBR
ABS_X ABS_Y ABS_RX ABS_RY   ABS_Z ABS_RZ   ABS_HAT0X ABS_HAT0Y
```

That is the same shape the Brick's pad reports, down to the two details `CLAUDE.md` calls out:
**L2/R2 are the analog axes `ABS_Z`/`ABS_RZ`** rather than buttons, and **the d-pad is
`ABS_HAT0X/Y`**, an absolute axis that never autorepeats — so `input.c`'s own key-repeat timerfd is
what makes holding a direction scroll, here exactly as there. The difference is only the four face
buttons, and that difference is what the `xbox` row already encodes.

The client picks the right devices unaided. It watches `event3` (the AT keyboard), `event7` and
`event10`, and skips the power buttons and the lid switch as "nothing this client reads". The two
`Valve Software Steam Deck Controller` nodes declare no `EV_KEY` or `EV_ABS` at all — they are the
trackpad/mouse surfaces. Input device ACLs come from logind, so the logged-in user can read the
pad with no group changes and no root; it is only the *framebuffer* that needs privilege.

```bash
MESHCLIENT_INPUT_PROFILE=xbox ./build/debug/meshclient -f ...
```

**One claim here is not yet measured on hardware.** The capability bitmap says which codes the pad
*can* send; it cannot say which silkscreened button sends which. The `xbox` mapping is the standard
Linux convention and the emulated pad is a 360 pad by construction, so it is very likely right —
but `docs/device.md` refuses auto-detection precisely because "a nearly-right guess is the one
nobody questions", and this is a guess until somebody presses the four buttons and reads the codes.
That is a `make deploy-input-map` equivalent run locally, and it should happen before the `xbox`
row is documented as the Deck's answer rather than inferred to be.

## The serial transport picks up the Deck's own controller

`mesh_serial_device_is_radio()` is `device->role != MESH_SERIAL_ROLE_BOOTLOADER` — there is no
VID/PID allowlist, by design, because a Meshtastic board can appear behind any of the UART bridges
in `k_serial_drivers[]`. On a Steam Deck that catches the machine's own controller:
`/dev/ttyACM0` is `28de:1205`, `Steam_Deck_Controller`, and it is offered as a candidate radio
twice — once as the bound tty, once as an unbound interface the transport would bind by writing
`new_id`.

As the `deck` user this is harmless: `/dev/ttyACM0` is `root:uucp`, `deck` is not in `uucp`, and
the open simply fails. **Run the client as root on a Steam Deck and it is not harmless** — it would
write Meshtastic frames at the controller's CDC endpoint and rebind its interface to the generic
`usbserial` driver. Since the framebuffer route in the section above is the route that needs root,
these two interact badly, and anything that runs the client privileged on this hardware should pass
`--disable-serial`.

This is a property of the host rather than a defect in the enumeration — a VID/PID allowlist would
have to be maintained against every board Meshtastic supports, which is the trade the current rule
deliberately refuses. But it is worth a line in the log a reader can act on.

## Reviewing UI changes here

Unchanged from anywhere else, and it needs none of the above:

```bash
distrobox enter --name meshdev -- bash -lc \
    'make ui-capture ARGS="-g 1280x800 devtools/ui_capture/scenes/messages.scene -o out.gif"'
```

`-g 800x1280` renders into the Deck panel's native portrait instead, which is a fair preview of
what a rotation-aware fb backend would have to place — and a quick way to see why the landscape
geometry is the one worth targeting.

## Tearing it down

The container is the only thing installed, and it is removable without trace:

```bash
distrobox rm --force meshdev
podman rmi docker.io/library/ubuntu:24.04
```

Nothing was written to `/etc`, `/usr`, or the Steam client. The BlueZ bond is the one piece of
state that outlives the container — `bluetoothctl remove <mac>` drops it if the radio should not
stay paired to the machine.
