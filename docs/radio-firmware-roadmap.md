# Updating the radio's firmware from the Brick

**This is about the *radio's* firmware, not the client's.** MeshClient already updates itself —
that is [`src/core/updater.c`](../src/core/updater.c), and everything below borrows from it.
This document is about the other binary in the room: the Meshtastic firmware running on the
node the Brick is talking to, which today can only be changed with a computer and a cable.

Status: **phase 1 has shipped**, 2026-09-09; everything from phase 2 on is still proposed. The
client now identifies the board, reports the newest release and says which bus - if any - could
carry an install; it writes nothing anywhere. See
[§Phases](#phases) for what that covers and [§What phase 1 measured](#what-phase-1-measured) for
the three upstream facts it corrected on the way. The rest of the upstream facts were read out
of `meshtastic/firmware` at `81b3ce8`, `meshtastic/esp32-unified-ota` at its head,
`meshtastic/firmware-ota` (the old loader), `adafruit/Adafruit_nRF52_Bootloader` (the UF2 side)
and `meshtastic/Meshtastic-Android`'s `feature/firmware` module, and the release layout was
measured against
`v2.7.26.54e0d8d` by range-reading the published zips. Everything about the *Brick* is a
proposal and is marked as such; the measurements it needs are in
[§Before any of this is written](#before-any-of-this-is-written).

It supersedes the one-line answer in
[`settings-roadmap.md`](settings-roadmap.md) — "large, hardware-specific, can brick the radio,
deferred indefinitely". That answer was right about the risk and wrong about the size: for
*one* family of radios the whole thing is a text protocol over a pair of GATT characteristics —
one written, one notified — and the client already owns every piece except the streaming.

## The short version

There are **two** paths, one per bus, and between them they cover 123 of the 129 build targets
upstream ships:

| | Radios | How |
|---|---|---|
| **Over USB** | nRF52840 (44 targets — T114, RAK4631, T-Echo, Tracker T1000-E …) and RP2040/RP2350 (6) | `enter_dfu_mode_request` over the serial link, then write the release's `.uf2` blocks to the bootloader's mass-storage endpoint |
| **Over BLE** | ESP32 (15) and ESP32-S3 (58) | `ota_request` with a SHA-256, a reboot into the loader in `app1`, then a text protocol over two GATT characteristics |
| **Neither** | ESP32-C3/C6 (4), STM32WL (2) | C3/C6 ship the old loader and current firmware refuses to boot into it; STM32WL drops into a ROM UART bootloader that is its own project |

The USB path is the smaller and safer of the two and it should ship first — see
[§Part four](#part-four-the-other-way-in--usb-and-uf2). What stays out is not "nRF52": it is
**Nordic DFU over BLE**, which is a genuinely large feature (two protocols, bond-key
subtleties, SoftDevice variants, ~2 500 lines in the phone app) and which the USB path makes
unnecessary for anyone with a cable. Wi-Fi OTA stays out too: the Brick's Wi-Fi and Bluetooth
are one part behind one antenna.

Whatever we can or cannot install, every radio gets a row saying which firmware it is running,
whether a newer one exists, and — when we cannot install it — one sentence about why and what to
use instead. A row that cannot be pressed is still the answer to "why is my radio behaving like
that".

The one fact that shapes the BLE half: **the ESP32 OTA loader has no way back.** Once the radio
reboots into it, its boot partition points at the loader and stays there. There is no timer, no
reboot counter, no fallback to the old firmware. The radio is off the mesh until an update
completes — which it can do over BLE, with no cable, but only if something finishes the job. So
that press is not "download and install"; it is "take the radio off the mesh until this is
done", and the client has to be built to finish, or to be able to come back and finish. **The
USB path has no equivalent hazard**, which is most of why it goes first: the UF2 bootloader is
permanent, an interrupted write leaves the radio sitting in it, and the retry is to write the
blocks again.

## What the phone app actually does

Worth reading before designing ours, because the phone app is what users will compare us to,
and its shape is dictated by the same upstream protocols.

`DefaultFirmwareUpdateManager` routes on two axes — how you are connected, and what the radio
is built on:

| Connection | ESP32 family | nRF52 family |
|---|---|---|
| BLE | `Esp32OtaUpdateHandler` — admin `ota_request`, reboot into the loader, stream the image | `SecureDfuHandler` — buttonless trigger, then Nordic Secure **or** Legacy DFU |
| Serial/USB | refused outright | `UsbUpdateHandler` — a `.uf2` onto the bootloader's mass-storage drive, which on Android means handing the *user* a file to save onto the mounted volume |
| TCP | Wi-Fi OTA, same loader, TCP instead of GATT | refused outright |

So "the Meshtastic app can do firmware updates" is four features. Two of them are small — and
the phone app makes one of those two (USB) smaller still by ending at a file picker, which is a
concession to Android's storage model rather than to the protocol. A Linux handheld with a host
port and usbfs can finish the job itself.

## Part one: which image

The radio tells us `DeviceMetadata.hw_model` (a `HardwareModel` enum value) and
`firmware_version`. It does **not** tell us its build target, and the build target is what
names the file. Resolving one to the other takes two documents, both published, both small:

1. `https://api.meshtastic.org/resource/deviceHardware` — 39 KB, 116 entries, one per board:
   `hwModel`, `hwModelSlug`, `platformioTarget`, `architecture`, `activelySupported`,
   `requiresDfu`. This is the same list the web flasher uses.
2. `https://api.meshtastic.org/github/firmware/list` — the release index, `stable` and `alpha`,
   newest first, each with a tag and a link to that release's manifest
   (`firmware-<version>.json`, 10 KB, 129 targets, each with its `platform`). **The index itself
   is 155 KB**, not small: almost all of it is release notes, and there is no way to ask for
   less. See [§What phase 1 measured](#what-phase-1-measured).

**`hw_model` is not unique, and this is the one place we should be better than the phone app.**
Nine models map to more than one build target: `TLORA_T3_S3` is both `tlora-t3s3-v1` and
`tlora-t3s3-epaper`; `HELTEC_WIRELESS_TRACKER` is four targets; `DIY_V1` and `HYDRA` share
model 39. The phone app takes the first match. Flashing the wrong variant of the right board
is exactly the failure this feature must not have, so when the lookup is ambiguous the client
**asks** — a list of the candidate boards by `displayName`, chosen once and remembered per
radio — and never guesses. When it is unambiguous there is nothing to ask and no row appears.

Use the release manifest's `platform` for the zip name, not `deviceHardware.architecture`:
the former says `esp32s3`, the latter says `esp32-s3`, and only one of them is in the URL.

## Part two: getting the bytes

Upstream publishes **one zip per platform**, not one file per board. `firmware-esp32-<ver>.zip`
is 40.7 MB; `firmware-nrf52840-<ver>.zip` is 46.3 MB. The image inside it for one board is
about 2.2 MB, and its DEFLATE'd form in the zip is about 1.4 MB.

Downloading 40 MB to use 2 MB of it, on a handheld, over a shared antenna, is not the plan.
GitHub's release CDN honours HTTP range requests (**measured**, `Content-Range` on both zips),
and a zip is designed to be read from the back, so:

1. Range-read the last ~64 KB → the end-of-central-directory record and the central directory
   (16 KB for the 139-entry nRF zip; less for ESP32's 65).
2. Find `firmware-<target>-<ver>.mt.json`, range-read it (~1 KB, one deflate block). It carries
   the board's `hwModel`, and an md5 and byte count for every file the board publishes —
   including which one goes in which partition (`part_name: app0` is the one we want).
3. Range-read that member's local header, then its compressed bytes (~1.4 MB).

Total transfer: about 1.5 MB instead of 40 MB, for the same 2.2 MB image.

**Inflating it without adding a dependency.** The members are DEFLATE, and the client links
libdbus and libm and nothing else. Three options, in the order I would take them:

- **Wrap the raw deflate stream in a gzip envelope and pipe it through the device's `gzip -dc`.**
  Ten bytes of header in front, the CRC32 and the uncompressed size (both already in the central
  directory) behind, and `gzip` inflates it *and checks the CRC for us*. **Verified here**
  against a real member of the real release zip. It costs no new library, and it reuses the
  fork-a-child-and-read-its-fd shape the updater already has. Take the CRC from the *central*
  directory entry, not the local header — a local header may carry zeroes and defer the CRC to
  a trailing data descriptor.
- **Link zlib.** Boring, portable, ~90 KB, and one more thing in the cross container and in the
  "static musl plus libdbus" sentence that is currently true.
- **Download the whole zip and shell out to `unzip -p`.** Simplest code, 40 MB of somebody's
  data allowance, and the one option that gets worse every release.

**The download must happen with the radio link down.** This is not new: the Brick's Wi-Fi and
its Bluetooth are one Xradio part behind one antenna, and `mesh_updater_holds_the_radio()`
exists because a couple of megabytes of curl was measured to break a live BLE link 36 ms in and
then spend four minutes reconnecting. A firmware download is the same size and the same
problem, so it takes the same hold — and the ordering falls out of it for free:

> fetch the metadata and the image with the link down → bring the link up → provision the hash
> and send the radio into the loader → talk to the loader (BLE only, no Wi-Fi) → reconnect.

Where it lands: a staging file beside the client's own `.update` staging area, named for the
release and the target, deleted after a successful install and after a failure that will not be
retried. 2.2 MB against an SD card is nothing; keeping it lets a retry skip the download.

## Part three: handing them over (the BLE half)

This is the ESP32 and ESP32-S3 path. It is small for what it does, and it is the one with the
hazard: everything below exists to get a radio into a loader that has no way back out and then
finish the job. The nRF52 and RP2040 boards do not come this way at all —
[§Part four](#part-four-the-other-way-in--usb-and-uf2) is theirs.

### Getting the radio into the loader

One admin message on `ADMIN_APP`, addressed to our own node, exactly as every other write in
[`radio_settings.c`](../src/core/radio_settings.c):

```
AdminMessage.ota_request = OTAEvent { reboot_ota_mode: OTA_BLE, ota_hash: <32 bytes> }
```

`ota_hash` is the SHA-256 of the image we are about to send, and it must be exactly 32 bytes or
the firmware refuses. The firmware stores it in NVS, sets the boot partition to `app1`, and
reboots one second later. The loader will later compare what it received against that stored
hash, which is what makes the handover safe: the client cannot talk a radio into flashing a
different image than the one it announced. (Bring your own trust for the *image*; see
[§What each hash actually proves](#what-each-hash-actually-proves).)

Before it reboots, the firmware answers with a `ClientNotification` — and **the client already
receives and surfaces those** (`mesh_session_handle_client_notification`). `"Rebooting to BLE
OTA"` is the go-ahead; anything else is a refusal with its reason already written in English by
the firmware:

| What the firmware says | What it means |
|---|---|
| `Cannot start OTA: Invalid ota_hash provided` | our bug, not the user's |
| `Cannot start OTA: Cannot find OTA Loader partition` | this board has no `app1` |
| `Cannot start OTA: Device does have a valid OTA Loader` | `app1` is empty or unreadable |
| `OTA Loader does not support BLE` | the loader in `app1` is the old one, or the Wi-Fi-only build |

That last row is the compatibility cliff, and it is worth understanding because it is invisible
from the version number. The firmware checks the loader's ESP-IDF project name and accepts only
`MeshtasticOTA`, `MeshtasticOTA-BLE` or `MeshtasticOTA-WiFi`. The loader is written by a *full
USB flash*, never by an OTA — so a radio flashed before the unified loader existed still has
the old `bleota.bin` in `app1`, will run the newest firmware happily, and will still refuse to
OTA. ESP32-C3 and C6 ship `bleota-c3.bin` to this day, which is why they are out of scope: it
is not that we cannot speak the old protocol, it is that current firmware will not boot into it.
The handler is also compiled out where the build excludes Wi-Fi.

There is no fallback verb. `reboot_ota_seconds` is deprecated upstream and its handler is
already gone from `AdminModule`.

### Finding the loader

The loader is a different BLE peripheral than the radio was a moment ago:

- **Address:** the radio's, with the last byte incremented. The phone app derives exactly this
  (`calculateMacPlusOne`, shared by its ESP32 and nRF paths), so on our side the D-Bus object
  path is predictable rather than searched for.
- **Name:** `Meshtastic_XXYY`, the last two bytes of the Wi-Fi STA MAC — *not* the node's short
  name.
- **Service:** `4FAFC201-1FB5-459E-8FCC-C5C9C331914B`, with
  `62ec0272-3ec5-11eb-b378-0242ac130005` to write (write / write-no-response) and
  `...0003` to notify. Scan on the service UUID; treat the name and the address as
  corroboration, not as the filter.
- **No pairing.** The loader registers no security on either characteristic. Whether BlueZ tries
  to encrypt anyway because it holds a bond for that address is one of the things
  [§Before any of this is written](#before-any-of-this-is-written) asks us to measure.

### The protocol

Text commands terminated with `\n`, then a raw binary stream, then a text answer. It takes
**both** characteristics: everything the client says goes to `...0005`, and every `ERASING`,
`ACK`, `OK` and `ERR` comes back as a notification on `...0003`. A client that subscribes to the
one it writes to waits for an acknowledgement that was never going to arrive there. In full:

| Client writes | Loader notifies |
|---|---|
| `VERSION\n` | `OK <hw> <fw> <reboot_count> <git_hash>\n` |
| `OTA <size> <sha256-hex>\n` | `ERASING\n`, then (seconds later) `OK\n`, or `ERR <reason>\n` |
| ≤ one MTU of firmware | `ACK\n`, per chunk |
| … last chunk | `OK\n` — flashed, boot partition switched, reboots in 2 s |

`VERSION` is also how we tell the two loaders apart without guessing: the unified one answers,
the old one is sitting in its `OTA_SIZE:` state machine and says nothing at all.

Three details that are load-bearing, and one of them was a bug in the phone app first:

- **One chunk is one GATT write.** The loader ACKs per *write*, not per declared chunk, so a
  512-byte chunk that BlueZ splits into 509 + 3 earns two ACKs and desynchronises the cadence
  from then on. Chunk size is `min(512, negotiated write payload)`, and the write is a single
  call.
- **One chunk outstanding at a time.** Write, wait for `ACK`, write. The loader's receive buffer
  is 4 KB and it drops what does not fit, silently as far as the wire is concerned.
- **`ERASING` needs a minute, not a second.** The phone app allows 60 s there against 10 s for
  an ACK.
- The loader requests MTU 517 and a 15 ms connection interval, so the ceiling is around 30 KB/s
  and the floor — if BlueZ gives us 23-byte MTU and a round trip per write — is far below that.
  A 2.2 MB image is therefore somewhere between one minute and unusable, which is precisely why
  the first thing to do is measure it (below).

On success the loader clears the reboot counters, sets `app0` as the boot partition and
restarts; the radio comes back on the mesh running the new firmware. On a hash mismatch it
**deliberately corrupts the first sector of the partition** so the bootloader cannot run what it
just wrote, answers `ERR Hash Mismatch`, and stays in the loader.

### And then the awkward part

If anything interrupts the stream — the Brick sleeps, the client is quit, the battery dies, the
user walks out of range — the radio stays in the loader, advertising, off the mesh, waiting. It
is not bricked and it needs no cable, but nothing recovers it except finishing an OTA.

So the client owes it two things:

1. **A persistent banner.** `src/ui/chrome.c` already owns "what the frame says about the client
   rather than about a screen", and the rule there is that a banner must be able to resolve.
   "Radio is in update mode" resolves by finishing the update, which is exactly the banner we
   want and exactly the press it should offer.
2. **Recovery on startup.** If the last thing we did was send a radio into the loader, scan for
   the loader before scanning for radios, and offer to resume with the image still on disk.
   The phone app has the same door (`recoverDfuDevice`); ours is cheaper because we already know
   the address is *that* address plus one.

## Part four: the other way in — USB and UF2

This is the path for the nRF52840 boards, and it is the one to build first. It reaches a Heltec
Mesh Node T114, a RAK4631, a T-Echo or a Tracker T1000-E — none of which the BLE half touches —
and it is a shorter piece of work than the BLE half by some margin.

### What the bootloader actually is

Every nRF52840 board Meshtastic ships runs the Adafruit nRF52 bootloader, and it never gets
overwritten by a firmware update. `enter_dfu_mode_request` — an ordinary `AdminMessage`, which
the client can send down the serial link it is already connected on — makes the firmware call
`enterUf2Dfu()`: a magic value into `GPREGRET` and a reset. The board comes back as a USB
composite device presenting **mass storage plus CDC**, with a drive whose name depends on the
board (`T114BOOT` and friends). A double-tap of the reset button does the same thing by hand,
which is the recovery path when the radio is too broken to be asked politely.

RP2040 and RP2350 take the same verb to the same place: `enterDfuMode()` there is
`reset_usb_boot()`, and BOOTSEL mass storage is UF2 too, with a different family id in the
file. That is why this path is worth building as "write a UF2 to a bootloader" rather than as
"update an nRF52".

Two caveats, neither fatal and both worth knowing before writing the detection. One variant
(`wio-sdk-wm1110`) defines `NRF_USE_SERIAL_DFU` and goes to Nordic *serial* DFU instead of UF2,
so the bootloader that appears is a CDC port and not a drive — the client should look at what
enumerated rather than assume, and say so plainly when the answer is a board it has no path for.
And boards do ship with older bootloaders: the phone app's device list carries a
`requiresBootloaderUpgradeForOta` flag for exactly that, which is a signal we can read from the
same document Part one already fetches.

### The write is the protocol

The thing that makes this small is a detail of how UF2 works, and it is worth stating plainly
because the obvious reading is wrong. **The bootloader does not care about the filesystem.**
Its `tud_msc_write10_cb` hands every 512-byte block of every SCSI `WRITE(10)` to `write_block()`,
which checks the block for the UF2 magic and — if it is there — flashes its payload at the
address the block itself names. A block without the magic is discarded and reported as written;
the FAT the drive appears to have is a fiction maintained for the benefit of hosts that insist
on one. The `lba` is passed through and never used to decide anything.

Completion is likewise carried in the file: each UF2 block states its own `blockNo` and the
`numBlocks` of the whole image, the bootloader keeps a bitmask of what it has seen so a host
that rewrites a block does not double-count, and when `numWritten >= numBlocks` it flushes,
completes the DFU and resets into the new firmware. Nothing has to tell it the transfer is over.

So the whole operation is: **write the `.uf2`'s 512-byte blocks to the bootloader's mass-storage
endpoint, in any order, and wait for the device to reboot.** No mount, no FAT, no filesystem
driver, no `enter`/`finish` handshake, no hash to provision, no state on the radio that we have
to be careful to leave in a good place.

### Two ways to do the writing

- **Through the kernel.** If the Brick's kernel carries `usb-storage` and `sd_mod`, the
  bootloader shows up as `/dev/sdX` and the client writes blocks to it with `write()`. Twenty
  lines. Whether that kernel does carry them is question 1 below and nobody has looked yet — the
  same kernel has `CONFIG_USB_ACM` off, so it is not a safe assumption.
- **Through usbfs, with no kernel driver at all.** `src/transport/serial/serial_usb.c` already
  opens `/dev/bus/usb/BBB/DDD`, claims an interface with `USBDEVFS_CLAIMINTERFACE` and issues a
  control transfer with `USBDEVFS_CONTROL` — that is the existing Brick workaround for a
  native-USB node with no CDC-ACM driver. Bulk-Only Transport is the same shape one ioctl over:
  a 31-byte Command Block Wrapper carrying a SCSI `WRITE(10)`, the data through `USBDEVFS_BULK`,
  a 13-byte Command Status Wrapper back. Perhaps 350 lines, and it makes the feature independent
  of what the kernel happens to have been built with.

The second is the one to write. It is more code than the first, and it is the only one of the
two that cannot be defeated by a kernel config we do not control — the same argument that
produced the usbfs workaround already in the tree.

### Why this is the safer half

Every hazard the BLE path has to design around is absent here:

| | ESP32 over BLE | nRF52 over USB |
|---|---|---|
| If the transfer is interrupted | radio sits in a loader with no way back, off the mesh, until an OTA completes | radio sits in its bootloader; write the blocks again, or double-tap reset |
| Getting back in without the client | nothing; it only speaks the OTA protocol | any computer, any OS: the drive mounts and a `.uf2` is a file copy |
| Integrity | a SHA-256 provisioned over the air beforehand, enforced by the loader | the block's own structure; we check the download against the zip's CRC32 before a byte is written |
| Recovery from a bad image | reflash over BLE | reflash over USB, or the bootloader's own double-tap |
| Cost of getting it wrong in code | a radio that needs a working client to come back | a rewrite of the same blocks |

The trade is that it is not over the air: the radio has to be plugged into the Brick's USB-C
port, which [`device.md`](device.md) confirms is a host port that enumerates and powers a
native-USB node. For a handheld that is a real constraint and a fair one — it is exactly the
situation the user is already in when they connect over serial.

### What it costs us

The image is the `.uf2` member of the same platform zip Part two already knows how to read —
1,467,392 bytes for the T114 at 2.7.26, 517,956 of them on the wire. `hw_model` 69 maps to
exactly one target (`heltec-mesh-node-t114`), so the ambiguity problem does not even arise for
this board.

The states are fewer than the BLE path's: `resolving → downloading → verifying → waiting for
the bootloader → writing 43% → done`. "Waiting for the bootloader" needs the care, since
the radio disappears from `/dev` and comes back as a different USB device — the client should
watch for the new VID/PID rather than counting seconds, and should say what it is waiting for so
that a user whose board needs a double-tap knows to give it one.

## What the client already has

More than half of this, which is the argument for doing it now rather than in the abstract.

| Piece | Where | What it gives us |
|---|---|---|
| Fork-a-fetcher, read it through the loop | `src/core/updater.c` | curl/wget probing, the pak's CA bundle (the Brick has no system CA store), per-step timeouts, one child at a time, no threads |
| Download progress with an opaque fetcher | `updater.c` (`downloaded`) | `stat()` on the staged file over the expected size. Same trick works here, against the **compressed** size from the central directory — what is landing is the zip member, and dividing by the `.mt.json`'s uncompressed size would stop the bar at 63% and call it done |
| SHA-256 | `src/utils/sha256.c` | the 32 bytes `ota_request` wants, and the hex the `OTA` command wants |
| Admin queue with passkeys and read-back | `src/core/radio_settings.c` | `ota_request` is one more `enum mesh_admin_request_kind` and one more encode arm |
| `ClientNotification` | `src/core/session.c` | the preflight answer, already parsed and already published to the UI |
| Generic GATT client | `src/transport/ble/bluez_client.c` | discovery, connect, pair, subscribe, read, write, notification dispatch — all by UUID, none of it structurally Meshtastic-specific |
| "Hold the radio down while I use the antenna" | `mesh_updater_holds_the_radio()` | the exact hold a firmware download needs, and auto-connect already reads it |
| A settings action that confirms before it acts | `src/ui/settings*.c`, `MESH_UI_SETTINGS_ACTION_*` | the confirm sheet, the action bar verb, the toast |
| A BlueZ mock | `mesh_bluez_client_mock_enable` | a fake loader can live entirely in the test suite |
| usbfs, already doing awkward things | `src/transport/serial/serial_usb.c` | opening `/dev/bus/usb/BBB/DDD`, `USBDEVFS_CLAIMINTERFACE`, `USBDEVFS_CONTROL`, and the libc-disagreement macro the ioctl codes need. Bulk-Only Transport is one more ioctl in the same file's idiom |
| A serial link to the radio | `src/transport/serial/` | which is what carries `enter_dfu_mode_request` on the USB path — the admin queue does not care which transport it is queued on |
| A usbfs mock | `mesh_serial_usb_mock_enable` | so a fake bootloader, like the fake loader, never touches real hardware in tests |

## What has to be built

| New | Roughly | Notes |
|---|---|---|
| `src/core/firmware_catalog.c` | 400 lines | the two API documents, the zip central directory, the `.mt.json`, and the "which target is this radio" answer including the ambiguous case. Parsers are the fiddly part; they are also pure functions over captured bytes, so they are the easiest thing here to test |
| `src/core/firmware_update.c` | 500 lines | the state machine: resolve → download → inflate → verify → *hand over* → confirm. One state per thing the screen can name, exactly as `enum mesh_update_state` does. The handover is the only part that differs per bus |
| `src/transport/serial/usb_msc.c` | 350 lines | Bulk-Only Transport over usbfs: CBW, SCSI `WRITE(10)`, CSW, and the `/dev/sdX` shortcut where the kernel offers one. The UF2 path's whole transport |
| `src/core/uf2.c` | 150 lines | reading a `.uf2`: magic, `blockNo`/`numBlocks`, family id, and the check that the file we are about to write is for the board we are about to write it to |
| `src/transport/ble/ble_ota.c` | 300 lines | the loader conversation. Chunk, write, await `ACK`, count. Sits on the bluez client, not on `mesh_session` |
| `mesh_bluez_client_find_characteristics()` | small | generalise `find_meshtastic_characteristics` to any service/characteristic pair. It is already generic underneath |
| Write-without-response | small | one `{"type": "command"}` entry in the options dict that is currently always empty |
| UI: a Firmware section, a progress screen, a banner, a confirm sheet | medium | see below |
| Strings, help notes, icons | small | catalog lines, one section note plus per-row notes, no new icons expected |

Neither handover goes through `mesh_session`: a loader and a bootloader are not Meshtastic
nodes, speak no protobuf, and have no node number. Routing either through the session would
mean teaching the session about a peer that has none of the things a session is about. The USB
path is the cheaper of the two to build and the cheaper to be wrong about, which is the whole
argument for its order.

A UF2 file names the board it is for, in the family id every block carries — so `uf2.c` refusing
a file whose family does not match the connected board is the same guard the ambiguous-`hw_model`
question needs, one layer lower and much harder to get past by accident.

## The UI

Where it lives: **Settings → About radio**, under the firmware version row that is already
there, because that is where somebody who wants to know what their radio is running already
went. The Radio actions section is verbs against a working radio; this is a verb that stops it
being one for a while.

The states are the same shape as the About screen's self-update, and for the same reason — a
row that names one of them is a row the user can read. The two paths share their first half and
diverge only at the handover:

```
shared   idle → resolving → downloading → verifying → ready
USB          → resetting into the bootloader → waiting for it to enumerate
             → writing 43% → done (the board reboots itself)
BLE          → arming (admin sent, waiting for the notification)
             → waiting for the loader (scanning) → sending 43% → finishing
             → reconnecting → done
either       → failed ("why", in one line)
BLE only     → interrupted (the banner state: the radio is in the loader and we are not
               talking to it)
```

There is no `interrupted` banner on the USB path, and that absence is the feature: an
interrupted write leaves a bootloader that any computer can talk to, so there is nothing the
client has to promise to come back and finish.

The house rules that already answer most of the design questions:

- The **screen progress bar** costs no body row and the **banner** costs rows. Sending is a
  request already in flight, so it goes in the bar; "your radio is in update mode" is content
  about the client's world, so it is a banner and it takes its rows honestly.
- The confirm sheet is not optional and its two rows are not "OK/Cancel". It has to say the
  thing that is actually true, which is not the same sentence on both paths: over BLE, *this
  radio leaves the mesh now and only comes back when this finishes*; over USB, *this radio
  restarts into its bootloader and needs this cable until the write is done*.
- No sentence is spelled out in a renderer. Every state above is a string id; every failure
  reason is a string id plus, where the firmware supplied one, the firmware's own words —
  which stay untranslated, like log lines and region codes.
- The Firmware rows get a section note in `src/ui/help.c` (what an OTA is, what it costs, what
  cannot be undone), and the "your board could not be identified" row gets its own.
- A `make ui-capture` scene per state, because the whole point of the capture harness is that a
  screen nobody can reach without a radio in a bootloader is still reviewable as a picture.

Refusals are rows, not silence — the client says *which* of these is true:

- this board can be updated, but not over the bus you are connected on: an nRF52 on BLE says
  *connect it by USB*, an ESP32 on serial says *connect it over Bluetooth*. This is the most
  common refusal and the most useful one, because it names a thing the user can go and do;
- this board has no path from here at all (ESP32-C3/C6, STM32WL);
- this radio's loader partition is empty or old — BLE only, and only discoverable by asking, so
  the row says "try it and the radio will tell us" rather than pretending to know;
- the radio is on battery below some floor, or the Brick is;
- we could not tell which of several boards this is, and nobody has picked yet.

## What each hash actually proves

Worth writing down plainly, because there are three hashes here, one of the two paths has only
two of them, and none of them is a signature.

- **The zip's CRC32** proves the bytes arrived intact. That is all a CRC does.
- **The md5 in `.mt.json`** proves the file matches what the release build recorded — but the
  manifest comes down the same connection as the payload, so it is a transfer check, not an
  authenticity check.
- **The SHA-256 in `ota_request`** proves the *loader flashed what the client announced*. It
  closes the gap between our client and the radio, not the gap between GitHub and our client.
  **The UF2 path has no equivalent** — a UF2 block carries no checksum, and the bootloader
  flashes what it is given. What stands in for it is that the bootloader survives a bad write:
  the failure mode there is "write it again", not "the radio is gone". It does mean the CRC
  check before the first block goes out is the only check there is, so it is not optional.

There is no code signing anywhere in the Meshtastic firmware release pipeline. The trust anchor
is TLS to GitHub's release CDN with the CA bundle the pak ships — the same anchor the client's
own self-update rests on, and worth saying out loud in the help note rather than implying more.

The one thing we must not do is hash the file we downloaded, hand *that* hash to the radio, and
call it verification: a corrupt download would be consistently corrupt and would sail through.
The publisher's digest is checked first, and the hash sent to the radio is only ever the hash of
bytes that already matched it.

## Testing

Everything except the last hop is testable without a radio, which is the other reason this is
worth doing in this order.

- **Catalog parsing**, against captured bytes committed as fixtures: the two API documents, a
  central directory, a `.mt.json`. Including the ambiguous-`hw_model` case, an unknown model, and
  a release that does not carry our platform.
- **A fake bootloader** on `mesh_serial_usb_mock_enable`: accepts Bulk-Only Transport,
  reassembles the UF2 blocks it is handed, and can be told to stall a CSW, to fail a
  `WRITE(10)`, or to vanish mid-write. The assertions are the interesting part — every block
  arrived, each one carried the right family id, and `numBlocks` was satisfied exactly once.
- **A fake loader** built on `mesh_bluez_client_mock_enable`: answers `VERSION`, `ERASING`/`OK`,
  ACKs per write, and can be told to stop ACKing, to answer `ERR Hash Mismatch`, to split a
  write, or to drop the link mid-stream. Every one of those is a state the screen has to name.
- **A UF2 golden**: the first and last block of a real release `.uf2`, committed, with the
  parser's answers pinned. It is a fixed 512-byte record and it should stay one.
- **A golden transcript** for the `ota_request` encode, in the same spirit as
  `message_encode_text_golden`: a protobuf regeneration that moves the field number should fail
  loudly rather than quietly send the radio nowhere.
- **Fuzz** the zip and manifest parsers. They read attacker-shaped input off the network and
  they are exactly the kind of length-prefix arithmetic `make fuzz` exists for.
- **Fuzz** the UF2 reader too — it is fixed-size records with a self-declared payload length,
  which is the shape that goes wrong.
- **On hardware**, and not skippable: a T114 or another nRF52840 over USB, including one write
  interrupted halfway and then repeated; then a real ESP32 and a real S3, one interrupted stream
  resumed, one wrong-hash refusal, and one radio whose `app1` holds the old loader.

## What phase 1 measured

Three things this document said turned out to be wrong or incomplete once the documents were
actually fetched and parsed, on 2026-09-09. None of them changes the plan; all three change a
number somebody would otherwise have designed against.

- **The release index is 155 KB, not 10.** Almost all of it is release notes, written by whoever
  merged the pull request, and there is no way to ask for less: no per-board endpoint, no way to
  opt out of the notes. Phase 1 reads it whole under a cap, which is a few seconds and half a
  megabyte held transiently. It also means the parser cannot be a scanner - see below.

- **The index's `zip_url` is usually a `.json`.** For a current release it is that release's own
  manifest (`firmware-<version>.json`); older entries really do point at a per-platform zip, and
  the newest `alpha` entry at the time of writing carries **no `zip_url` at all** - a release can
  appear in the index before its assets do. Phase 2 gets the manifest URL from here, so it has to
  survive all three shapes rather than assume the first.

- **The notes will eventually contain the keys the parser is looking for.** A `"zip_url":` or an
  `"id":` quoted inside a release note is exactly the kind of thing a maintainer writes when
  reverting a change, and a scanner that hunts for `"key":` at any depth - which is what
  `updater.c` does to GitHub's much smaller release JSON - would find it. Hence
  [`src/utils/json.c`](../src/utils/json.c), which walks structure instead; the fixture in
  `tests/data/firmware_list.json` carries the trap on purpose.

And one thing it got right that was worth confirming: **nine `hwModel` values in the served
document map to more than one board**, and the widest (48, `HELTEC_WIRELESS_TRACKER`) is four.
The lookup returns all of them and a count.

## Phases

Each phase is worth shipping alone, which is the test of whether the order is right.

**Phase 0 — measure.** No feature. Answer the questions below on a Brick with a radio attached.
Two of them can move a whole phase: whether the kernel gives us a block device for a bootloader
in mass-storage mode (question 1), and what BLE write throughput through D-Bus actually costs
(question 4). Both are cheaper to know now than to design around later.

**Phase 1 — tell the truth. Shipped.** Settings → About radio learns the release index: what
the radio runs, what the newest stable is, whether this board can be updated from here at all.
No downloads, no writes. This is most of the value for a user who owns a computer, and it is the
row that makes the rest legible.

It also carries a **channel**, stable or alpha, which the index has two lists for. Its own
setting rather than a follower of the client's own update channel: they are two projects, and a
stable client with alpha firmware on a spare node is a reasonable pair. The names are upstream's
own and stay untranslated, and there is no "automatic" third option - that one exists next door
to follow the running *build*, and this client's build says nothing about what a radio should
run.

What it turned into: [`src/core/firmware_catalog.c`](../src/core/firmware_catalog.c) for the two
documents (pure, and tested against captured bytes in `tests/data/`),
[`src/core/firmware.c`](../src/core/firmware.c) for the check, and
[`src/core/fetch.c`](../src/core/fetch.c) - the forked-fetcher machinery lifted out of
`updater.c`, which is the piece phases 2 and 3 will fetch through. The rows are the last three
under Settings → About radio, and `make ui-capture ARGS="devtools/ui_capture/scenes/radio-firmware.scene -o fw.gif"`
draws them without a radio.

**Phase 2 — get the image.** Resolve, range-download, inflate, verify, keep. Two sizes travel
together from here on and they are not interchangeable: the compressed member size, which the
download is measured against, and the uncompressed image size, which is what `OTA <size>` later
tells the loader to expect. Still nothing written anywhere. Ends with "1.4 MB fetched, it
matches the digest, and it inflates to the image the manifest describes", which is a real thing
to have proven. Serves both paths — a `.uf2` and an app image are two members of the same zip.

**Phase 3 — the USB handover.** `enter_dfu_mode_request` down the serial link, wait for the
bootloader to enumerate, write the `.uf2`'s blocks over Bulk-Only Transport, watch the board
reboot. This is the first phase that changes a radio, and it is deliberately the one whose
worst outcome is "write the blocks again". It also covers the boards the BLE path never will:
every nRF52840 target, plus RP2040 and RP2350 for the price of a different family id.

**Phase 4 — the BLE handover.** `ota_request`, the loader conversation, the banner, recovery.
The phase that can leave somebody's radio needing this client to come back, arriving after the
download half has been proven by phase 3 and after phase 0 has said what the throughput is.

**Phase 5 — the edges.** Resume a partial stream, a battery floor, the variant picker, a "the
loader answered but this is the old one" path, a bootloader that needs a double-tap because the
admin verb never reached it, and the help notes that explain what any of it means.

**Later, maybe never.** nRF52 over *Nordic DFU*, i.e. over BLE. Note what this is and is not:
phase 3 already updates those boards, with a cable. What this would add is doing it without
one, and the price is two protocol implementations, a fallback coordinator, per-attempt retry
budgets, stale-session cleanup, a bond that must be *kept* because the Adafruit bootloader
whitelists the bonded peer, and a SoftDevice-variant lookup whose failure mode is a corrupted
SoftDevice. That is a feature, not a phase, and it buys the least of anything here.

Also not planned: Wi-Fi OTA (one antenna), ESP32 over serial (the esptool protocol and a stub
loader — the phone app refuses it outright, and phase 4 reaches the same boards without it),
installing a firmware file the user brought themselves (a nice power-user feature and an
excellent way to flash a T-Deck image onto a T-Beam), and anything that flashes the `littlefs`
or `factory` images.

## Before any of this is written

Eight questions, all answerable in an afternoon with `make deploy-*`, all capable of changing
the design. The first three are the USB path's and come first because that phase does:

1. **Does a bootloader show up as a block device?** Put a T114 (or any nRF52840) into DFU with
   a double-tap, plug it into the Brick, and look: `lsusb`-equivalent under `/sys/bus/usb`, a
   `/dev/sd*`, `dmesg`. If `usb-storage` is there, phase 3 has a twenty-line fast path. If it is
   not — the same kernel has `CONFIG_USB_ACM` off — the usbfs BOT implementation is not optional
   and the phase gets its 350 lines.
2. **Does the port carry it?** The USB-C port is a host port that already enumerates a native-USB
   node, but a bootloader drawing more current is a different question, as is a board that
   expects to be bus-powered while it writes flash.
3. **How long does 1.4 MB of `WRITE(10)` take** through whichever of the two the answer to (1)
   turns out to be, and does the write survive the Brick's own sleep behaviour?
4. **BLE throughput.** What does a 512-byte `WriteValue` cost through BlueZ on this hardware, and
   what MTU does the Brick negotiate? At 20-byte writes and a round trip each, 2.2 MB is not a
   feature. If it is bad, does `AcquireWrite` (a socket fd, which the event loop would be happy
   with) fix it?
5. **The bond.** The radio is bonded; the loader at MAC+1 is a different address with no
   security. Does BlueZ connect cleanly, or does it try to encrypt with a key nobody has?
6. **Cached services.** After the radio reboots into the loader and back out again, does BlueZ
   re-discover its GATT database, or does it serve a stale one? The phone app has a
   cache-refresh reconnect delay for a reason.
7. **`gzip`.** Does the Brick's busybox have `gzip -dc`? If not, is `zcat` there? If neither, the
   answer is zlib and it should be decided now, not in phase 2.
8. **Power, and a radio that refuses.** What does the Brick's battery do over three minutes of
   continuous writing on either bus, and what floor should refuse the press? And take a node
   flashed with an older full install, send it `ota_request`, and confirm the refusal arrives as
   a `ClientNotification` the way the source says it does — the one upstream claim in this
   document that nothing here has watched happen.

## Sources

- `meshtastic/firmware` @ `81b3ce8` — `src/modules/AdminModule.cpp` (the `ota_request` and
  `enter_dfu_mode_request` handlers), `src/platform/esp32/MeshtasticOTA.cpp` (partition and
  loader checks, NVS hash), `src/platform/nrf52/main-nrf52.cpp` (`enterDfuMode` is UF2, not BLE),
  `bin/platformio-custom.py` and `.github/workflows/build_firmware.yml` (what goes in a release).
- `meshtastic/esp32-unified-ota` — the loader: `README.md` is a written protocol spec,
  `src/ble_ota.cpp` and `src/ota_processor.cpp` are what it actually does.
- `meshtastic/firmware-ota` — the old loader, same UUIDs, `OTA_SIZE:<n>` and a raw stream with
  no acknowledgement and no hash.
- `adafruit/Adafruit_nRF52_Bootloader` — `src/usb/msc_uf2.c` and `src/usb/uf2/ghostfat.c`: the
  `WRITE(10)` callback, `write_block()` ignoring the FAT and the LBA, and completion driven by
  the UF2 blocks' own `blockNo`/`numBlocks`. `src/main.c` for which transports each DFU magic
  brings up.
- `meshtastic/Meshtastic-Android` — `feature/firmware/`: routing, timeouts, the MAC+1 rule, the
  chunk-splitting bug and its fix, the asset names per method, and a USB path that ends at a
  file picker.
- Measured against release `v2.7.26.54e0d8d` on 2026-09-09: range support, zip sizes, central
  directory layout, `.mt.json` contents, and the gzip-envelope inflate.
