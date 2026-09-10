# Updating the radio's firmware from the Brick

**This is about the *radio's* firmware, not the client's.** MeshClient already updates itself —
that is [`src/core/updater.c`](../src/core/updater.c), and everything below borrows from it.
This document is about the other binary in the room: the Meshtastic firmware running on the
node the Brick is talking to, which today can only be changed with a computer and a cable.

Status: **phase 1 has shipped**, 2026-09-09, and **phase 0's USB half has been answered on
hardware**, 2026-09-10 — on a Brick with a Heltec Mesh Node T114 on the USB-C port, including
one complete `.uf2` written to the bootloader and the board rebooting into it. Phase 2 and
phase 3 are no longer proposals resting on assumptions; the numbers are in
[§What phase 0 measured](#what-phase-0-measured), and two of them move work off the plan
rather than onto it. The client itself still writes nothing anywhere: it identifies the board,
reports the newest release and says which bus - if any - could carry an install. See
[§Phases](#phases) for what that covers and [§What phase 1 measured](#what-phase-1-measured) for
the three upstream facts it corrected on the way. The rest of the upstream facts were read out
of `meshtastic/firmware` at `81b3ce8`, `meshtastic/esp32-unified-ota` at its head,
`meshtastic/firmware-ota` (the old loader), `adafruit/Adafruit_nRF52_Bootloader` (the UF2 side)
and `meshtastic/Meshtastic-Android`'s `feature/firmware` module, and the release layout was
measured against
`v2.7.26.54e0d8d` and `v2.8.0.47db0e3` by range-reading the published zips. Everything about
the *BLE* half is still a proposal and is marked as such; the measurements it still needs are in
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
**Measured**: `firmware-esp32-s3-2.7.26.54e0d8d.zip` is a `404` and
`firmware-esp32s3-2.7.26.54e0d8d.zip` is 170 MB of zip, so this is a hard failure rather than a
tidiness point.

The trap is worth stating once more precisely, because there are **three** spellings and they
appear in three documents:

| Where | Field | T114 | Heltec V3 |
|---|---|---|---|
| `deviceHardware` | `architecture` | `nrf52840` | `esp32-s3` |
| release manifest | `platform` | `nrf52840` | `esp32s3` |
| the board's own `.mt.json` | `mcu` | `nrf52840` | `esp32s3` |

They agree for every nRF52 and RP2040 board and disagree for exactly the ESP32-S3 family, which
is the worst possible distribution: code written and tested against an nRF52 board is *correct*,
and breaks the first time somebody points it at an S3. `architecture` is the path lookup's key —
it is what `k_paths[]` in [`firmware_catalog.c`](../src/core/firmware_catalog.c) already keys on,
correctly — and `platform` is the URL's. `mcu` is the same string as `platform` but lives inside
the zip the name was needed to open, so it is a cross-check and never a source.

## Part two: getting the bytes

Upstream publishes **one zip per platform**, not one file per board, and they grow: at 2.7.26
`firmware-esp32-<ver>.zip` is 40.7 MB and `firmware-nrf52840-<ver>.zip` is 46.3 MB; by 2.8.0 the
nRF one is 58.6 MB. The image inside it for one board is 1.4–2.2 MB, and its DEFLATE'd form in
the zip is a third to a half of that.

Downloading 40 MB to use 2 MB of it, on a handheld, over a shared antenna, is not the plan.
GitHub's release CDN honours HTTP range requests (**measured**, `Content-Range` on both zips),
and a zip is designed to be read from the back, so:

0. Learn the zip's length, from a `HEAD` or from the `Content-Range` on a one-byte probe. This
   step is not decoration: **the CDN refuses a suffix range** — `Range: bytes=-65536` comes back
   `501 Unsupported client range` from the Varnish cache in front of it — so the tail window has
   to be asked for as an explicit `bytes=<start>-<end>`, which means knowing where the end is.
1. Range-read the last ~64 KB → the end-of-central-directory record and the central directory
   (16 KB for the 139-entry nRF zip at 2.7.26, 21 KB for the 171-entry one at 2.8.0; less for
   ESP32's 65). Both fit the window with room to spare, but the check that the directory
   actually *starts* inside it is the parser's, not the reader's.
2. Find the member by **basename** — `firmware-<target>-<ver>.mt.json` — because the path in
   front of it is not stable across releases: flat at 2.7.26, under `nrf52840/` at 2.8.0. Range-
   read it (~0.5–0.7 KB compressed, 1.2–2.2 KB out, one deflate block). It carries the board's
   `hwModel`, and an md5 and byte count for every file the board publishes. **Which of those
   files is the image is asked differently per path** — see below; it is not `part_name: app0`
   on both.
3. Range-read that member's local header — for its `nlen` and `elen`, which differ from the
   central directory's and are what place the data — then its compressed bytes (~0.5 MB for the
   T114's `.uf2`). **Two fields and no more.** This document used to say the local header's CRC
   was the field to avoid at 2.8.0; measured, **all three** of its CRC, compressed size and
   uncompressed size are 0 there, deferred to a data descriptor written after the payload. A
   reader trusting them asks the CDN for a zero-length range and then checks what comes back
   against a CRC of zero, which passes.

Total transfer: **0.6 MB instead of 58** for the T114's 1.4 MB `.uf2`, and **1.4 MB instead of
170** for the V3's 2.1 MB `.bin` — the ratio gets better as the zips grow, which is the argument
holding up over time rather than eroding. **Measured on the Brick**: the tail window and the
local header are a round trip each, the T114's member lands in 1.8–3.0 s over Wi-Fi, and the
whole thing inflates in 0.03 s.

One correction to that arithmetic, from building it. Steps 0–3 run **twice**, because the
`.mt.json` is itself a member of the zip: once to fetch it and once to fetch the image it names.
That is a second HEAD and a second 64 KB tail window — about 65 KB on top of the 0.6 MB, against
the 46 MB not being downloaded — and it buys never having invented a file name from a pattern.
Whole run on the device, end to end including both API documents: **8.1 seconds**.

**What the `.mt.json` actually holds, and why the two paths read it differently.** Both were
pulled at `2.7.26.54e0d8d` on 2026-09-10 — the T114's and the Heltec V3's — and they are not the
same shape:

| | T114 (`nrf52840`) | Heltec V3 (`esp32s3`) |
|---|---|---|
| compressed / out | 486 → 1,157 B | 671 → 2,205 B |
| `mcu` | `nrf52840` | `esp32s3` |
| `architecture` | `nrf52840` | `esp32-s3` |
| `part` table | **absent** | 6 partitions, with offsets and sizes |
| `part_name` on files | **absent on every file** | `app0`, `spiffs`, `app1` |
| the image | `….uf2`, 1,467,392 B | `….bin`, 2,109,248 B, `part_name: app0` |
| also published | `.elf`, `.hex`, `-ota.zip` (the Nordic DFU package) | `.factory.bin`, `littlefs-….bin`, **`mt-esp32s3-ota.bin`** |
| `requiresDfu` | present | absent |

Three things follow, and the first is a correction to what this document said:

- **`part_name: app0` is the ESP32 selector and only the ESP32 selector.** No file in the T114's
  manifest carries a `part_name` at all, because an nRF52 has no partition table to name — so a
  parser that picks the image by `part_name == "app0"` finds nothing for every board the USB path
  serves, which is every board phase 3 is for. The nRF52/RP2040 selector is the `.uf2`, and the
  honest way to write it is one question per path rather than one clever predicate.
- **The OTA loader ships in the release zip**, as `mt-esp32s3-ota.bin`, 636,544 bytes, declared
  `part_name: app1`. That does not change [§Part three](#part-three-handing-them-over-the-ble-half)'s
  point that the loader only ever gets there by a full USB flash — nothing in the client is going
  to write `app1` — but it does mean the loader is versioned with the firmware, and it settles
  what a freshly flashed board has in it. A V3 web-flashed at 2.7.26 has 2.7.26's unified loader
  in `app1` and will therefore *accept* `ota_request`, which is exactly why one must not be sent
  at it before something exists that can finish the job.
- **The per-file hash is an md5**, and the client has `src/utils/sha256.c` and no md5. So the
  `.mt.json`'s hashes are a check we cannot currently make; see
  [§What each hash actually proves](#what-each-hash-actually-proves) for whether that matters.

**Inflating it without adding a dependency.** The members are DEFLATE, and the client links
libdbus and libm and nothing else. Three options, in the order I would take them:

- **Wrap the raw deflate stream in a gzip envelope and pipe it through the device's `gzip -dc`.**
  Ten bytes of header in front, the CRC32 and the uncompressed size (both already in the central
  directory) behind, and `gzip` inflates it *and checks the CRC for us*. **Measured on the
  Brick**, against a real member of the real release zip: busybox 1.27.2's `gzip -dc` inflated
  the T114's 1.4 MB in 0.03 s, answered `gzip: crc error` to a truncated member and
  `gzip: incorrect length` to a wrong `ISIZE`. So the envelope is not merely a way to avoid
  linking something — it is where the download's integrity check comes from, and both of its
  failure modes are known to fire. It costs no new library and reuses the
  fork-a-child-and-read-its-fd shape the updater already has. Take the CRC from the *central*
  directory entry, not the local header: at 2.7.26 the two agree and at 2.8.0 the local one is
  **0**, deferred to a trailing data descriptor, so code tested against the older release
  verifies the newer one against nothing.
- **Link zlib.** Boring, portable, ~90 KB, and one more thing in the cross container and in the
  "static musl plus libdbus" sentence that is currently true.
- **Download the whole zip and shell out to `unzip -p`.** Simplest code, 58 MB of somebody's
  data allowance, and the one option that gets worse every release. It is also **not available**:
  the Brick's busybox ships `gzip`, `gunzip` and `zcat`, and no `unzip` at all.

**The download must happen with the radio link down.** This is not new: the Brick's Wi-Fi and
its Bluetooth are one Xradio part behind one antenna, and `mesh_updater_holds_the_radio()`
exists because a couple of megabytes of curl was measured to break a live BLE link 36 ms in and
then spend four minutes reconnecting. A firmware download is the same size and the same
problem, so it takes the same hold — and the ordering falls out of it for free:

> fetch the metadata and the image with the link down → bring the link up → provision the hash
> and send the radio into the loader → talk to the loader (BLE only, no Wi-Fi) → reconnect.

Where it lands: a staging file named for the release and the target, deleted after a successful
install and after a failure that will not be retried. 2.2 MB against an SD card is nothing;
keeping it lets a retry skip the download.

**Not, however, beside the client's own `.update` staging area**, which is where this said to
put it and which is on `/mnt/SDCARD`. On the USB path that card is mounted over by the
bootloader's own ghost drive the moment the radio reboots — see
[§What phase 0 measured](#what-phase-0-measured) — so the image would vanish from its path
between being staged and being written. `/mnt/UDISK` is the internal 5.9 GB and has no such
problem. And on either bus the image should be **read into memory before the handover begins**:
1.4–2.2 MB against the ~700 MB free is nothing, it removes the whole class of "the file moved
under us while the radio was in a bootloader", and the streaming both paths do wants it in a
buffer regardless.

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

### Knowing a bootloader when we see one

Two halves of one question, and the client currently gets both of them wrong in the same place.
Phase 3 needs the answer to *find* the bootloader; the client needs it today to stop mistaking
one for a radio.

**Getting the radio there.** `enter_dfu_mode_request` is an ordinary `AdminMessage` addressed to
our own node, so it is one more `enum mesh_admin_request_kind` and one more encode arm in
[`radio_settings.c`](../src/core/radio_settings.c), queued on whichever transport is up — the
admin queue does not care, and on this path it is the serial link the radio is already on. It is
also the one admin verb whose *reply* is the link dropping, so nothing waits for an ack. Two
fallbacks are worth writing down because they are what a user reaches for when it does not land:
a **double-tap of reset** does the same thing by hand and is the recovery path when the radio is
too broken to be asked politely, and the **1200-baud touch** the Adafruit core implements is
technically a third — but not from here, because `usbserial_generic` never sends
`SET_LINE_CODING`, so it would have to go out as a usbfs control transfer on the CDC control
interface. That is the same ioctl `serial_usb.c` already issues for DTR, so it is cheap, but it
is a phase 5 nicety rather than a path.

**Not mistaking it for a radio.** `mesh_serial_usb_scan()` matches any interface of class
`0a` (CDC data), or anything a serial driver has already claimed. A UF2 bootloader is exactly
that plus a drive, so it matches — and the client goes further than passively accepting it: it
*binds* `usbserial_generic` to the bootloader's CDC itself, asserts DTR, and hands the result to
auto-connect, which requests a config sync that nothing will ever answer. The frame then says
connected, with the progress bar turning, forever. That is measured, not predicted; the log is in
[§What phase 0 measured](#what-phase-0-measured).

The fix is a reading rather than a table, and the distinction matters. A VID/PID list of known
bootloaders would be a second opinion about the hardware that goes stale every time a vendor
ships a board — and it would still be wrong for `wio-sdk-wm1110`, whose Nordic *serial* DFU
bootloader is a CDC port with **no drive at all**.

**The scan already matches two different kinds of thing, and only one of them can ever be a
bootloader.** That is the reading the rule should be built on, and it was not obvious until a
Heltec V3 was put on the same port as the T114 on 2026-09-10:

- **A bridge chip.** The V3 enumerates as `10c4:ea60`, "CP2102 USB to UART Bridge Controller",
  one vendor-class interface (`ff/00/00`) that the in-kernel `cp210x` driver claims. The USB
  device here is *the adapter*, and the radio is on the far side of a UART where USB cannot see
  it. `mesh_serial_usb_scan()` matches this through its second arm, `is_serial_driver()`.
- **A native-USB node.** The T114 enumerates as its own MCU — `239a:4405`, a CDC pair, no driver
  until the client binds one, which is why it needs the usbfs DTR workaround and the V3 does
  not. This is the first arm of the scan, `iface_class == CDC_DATA`.

**A bridge can never be a bootloader**, because a UF2 bootloader *is* the MCU's own USB
peripheral and a CP2102 is a separate chip that knows nothing about what it is wired to. So the
question only arises for the second kind, and for that kind the device says the answer
structurally:

- a **sibling interface at `08/06/50`** — mass storage, SCSI, Bulk-Only — alongside the CDC pair
  is a UF2 bootloader, and the drive it exposes is where a `.uf2` goes;
- a CDC pair with **no** such sibling, on a device whose `idProduct` is not the app's, is a
  serial-DFU bootloader or something else entirely: a board this client has no path for, which
  is a row that says so rather than a link;
- and the app firmware is the case where the device carries the CDC pair and the client has
  already spoken protobuf to it.

There is a corollary worth stating because it reaches the other path: **an ESP32 behind a bridge
chip has no USB-side bootloader signal at all.** When an ESP32 drops into its ROM download mode
the CP2102 does not change — same `10c4:ea60`, same tty, same everything — so there is nothing
for a scan to notice. That is an independent reason ESP32-over-serial is not a path here, on top
of the esptool one, and it means the "has it come back yet" question is answered by the USB tree
on the nRF52 path and must be answered by BLE discovery on the ESP32 one.

So `struct mesh_serial_device_info` grows a role — radio, bootloader, or bridge — derived from
what enumerated next to the interface, `mesh_serial_usb_scan()` fills it in from the sysfs walk
it is already doing, and the transport refuses to auto-connect to a bootloader. Phase 3 then
reads exactly the same field in the affirmative to answer "has it come back yet", which is why
this is one piece of work and not two: the detection a bug fix needs and the detection the
feature needs are the same function, and writing them separately is how they come to disagree.

**The same seam carries a smaller wart worth fixing while it is open.** A bridge's `product`
string is the adapter's, so the V3 comes through as `CP2102 USB to UART Bridge Controller` —
38 cells of chip name — and the client says `Auto-connecting to CP2102 USB to UART Bridge
Controller over USB`, puts that in the Devices tab and puts it under the keycaps through
`fb_link_summary()`. The T114 reads as `HT-n5262` only because a native-USB node's descriptor
happens to be the board. Neither is the node's name, and after the handshake the client knows
the real one; the label should prefer the owner's long name once it has it and keep the USB
string only for a device it has not yet spoken to.

The refusal is worth shipping on its own, ahead of phase 3. It is a live bug for anyone who
double-taps a node with the client running, its blast radius is one `if`, and a screen that says
*this node is in its bootloader* is already most of what phase 3's first state has to say.

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
  lines. **It does** — that is question 1, measured on 2026-09-10, and the write below went out
  this way.
- **Through usbfs, with no kernel driver at all.** `src/transport/serial/serial_usb.c` already
  opens `/dev/bus/usb/BBB/DDD`, claims an interface with `USBDEVFS_CLAIMINTERFACE` and issues a
  control transfer with `USBDEVFS_CONTROL` — that is the existing Brick workaround for a
  native-USB node with no CDC-ACM driver. Bulk-Only Transport is the same shape one ioctl over:
  a 31-byte Command Block Wrapper carrying a SCSI `WRITE(10)`, the data through `USBDEVFS_BULK`,
  a 13-byte Command Status Wrapper back. Perhaps 350 lines, and it makes the feature independent
  of what the kernel happens to have been built with.

**The order these were written in is the wrong one, and phase 0 is what says so.** The argument
for going straight to usbfs was that a kernel config we do not control could defeat the first —
the same argument that produced the workaround already in the tree. That argument was made
about a kernel nobody had looked at, and it was wrong twice over: `CONFIG_USB_STORAGE`,
`CONFIG_BLK_DEV_SD` and `CONFIG_SCSI` are all built in, and `usb-storage` binds the bootloader
without being asked. `CONFIG_USB_ACM` being off predicted nothing about the storage stack.

So **write the twenty lines first**, and keep the 350 as the fallback they are. Three reasons,
and only the first is about effort: the fast path is what this hardware actually does, so it is
the path that can be tested; a `write()` to a block device is a shape the event loop already
knows, where `USBDEVFS_BULK` is a new one; and a fallback written *after* a working path has
something to be checked against, where a fallback written first is the only implementation and
is therefore also the specification. The one thing the fast path must not do is assume it is
there — a kernel without it is a refusal with a reason, and that reason is what phase 5 turns
into the usbfs implementation.

Two details the fast path does not get for free, both from
[§What phase 0 measured](#what-phase-0-measured): the drive has to be **unmounted first**,
because this platform automounts it — over `/mnt/SDCARD`, no less — and the image has to be
**in memory before the radio is sent into DFU**, for the same reason.

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

That file is 2,866 blocks of a fixed 512 bytes, every one of them carrying `numBlocks` and its
own `blockNo`, a `familyID` of `0xADA52840` and a 256-byte payload — so the whole image is
733,696 bytes of flash, laid out contiguously from `0x26000` to `0xD9100` in 256-byte steps.
Every one of those is a thing `uf2.c` should check and pin in a golden, and **`payloadSize` is
256 rather than the 476 a UF2 block can carry**, which is worth knowing before writing an
arithmetic that assumes the maximum.

The states are fewer than the BLE path's: `resolving → downloading → verifying → waiting for
the bootloader → writing 43% → done`. "Waiting for the bootloader" needs the care, since
the radio disappears from `/dev` and comes back as a different USB device — the client should
watch for what enumerated rather than counting seconds (see
[§Knowing a bootloader when we see one](#knowing-a-bootloader-when-we-see-one)), and should say
what it is waiting for so that a user whose board needs a double-tap knows to give it one.

**How long the user is looking at that screen is now a measured number rather than a worry.**
The T114's whole write took **13 s**, the board reset itself into the new firmware **1 s** after
the last block, and the client had a handshake **3 s** after that — call it twenty seconds from
the confirm sheet to a radio back on the mesh, with the download's two or three seconds in front
of it. The progress bar has to be honest, but it does not have to be a companion for a long
wait, and nothing here needs a resume.

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
| `src/core/firmware_catalog.c` | 400 lines | **shipped.** Phase 1 for the two API documents; phase 2 added the board's `.mt.json` and the release's own manifest. The zip central directory went to `src/utils/zip.c` instead — it is a container reader with no firmware in it, it is reusable (the Nordic DFU package is a zip too), and it wanted a fuzz target of its own |
| `src/core/firmware_download.c`, `_fetch.c` | 500 lines | **shipped in phase 2**, as two: the four range reads and the gzip envelope in one, and "which zip, which member" in the other. What is still to come is the *handover* half — one state per thing the screen can name, exactly as `enum mesh_update_state` does, and the only part that differs per bus |
| Bootloader recognition in `serial_usb.c` | small | **shipped in phase 2.5.** A role on `struct mesh_serial_device_info`, read off the sibling interfaces the sysfs walk already visits. Fixed a live bug and is phase 3's "has it come back yet" — see [§Knowing a bootloader when we see one](#knowing-a-bootloader-when-we-see-one) |
| `src/transport/serial/usb_msc.c` | 20 lines, then 350 | the block-device write is twenty lines and is what this hardware does; the usbfs Bulk-Only Transport behind it — CBW, SCSI `WRITE(10)`, CSW — is a **fallback for a kernel without `usb-storage`**, and phase 0 says this kernel is not one. Unmounting the drive before writing belongs here |
| `src/core/uf2.c` | 150 lines | **shipped in phase 2.** Reading a `.uf2`: magic, `blockNo`/`numBlocks`, family id, `payloadSize`, and the check that the file we are about to write is for the board we are about to write it to |
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
  authenticity check. **It is also a check this client cannot currently make**: the manifest's
  per-file hashes are md5 (confirmed on both boards' manifests at 2.7.26), and `src/utils/`
  has a SHA-256 and no md5.

  That is a decision rather than a gap, and the argument is that it should stay a gap. The md5
  and the CRC32 answer the *same* question — did these bytes arrive as published — over the same
  TLS connection from the same host, so the second one adds a few hundred lines of md5 to
  re-confirm what `gzip` already refused to hand us. It would earn its place only if the two
  numbers could disagree, and the one way they can is a zip whose CRC was computed over different
  bytes than the manifest describes, which is a release-pipeline bug rather than a transfer
  failure. Worth revisiting if upstream ever publishes a sha256 there; not worth an md5.
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
  a release that does not carry our platform. **Done in phase 2**, and the fixtures went further
  than this asked: `tests/data/` now also holds a real member — the T114's `.mt.json`, its local
  header and all 486 of its deflated bytes, as the CDN serves them — so
  `tests/suites/firmware_download.c` runs the *whole* chain rather than the parsers. A fake CDN
  on PATH serves the tail window and that member at the offsets the 46 MB zip really keeps them
  at, answering a HEAD with both redirect hops' headers; what comes out the far end is handed to
  the manifest reader. One byte of the payload flipped produces exactly the `gzip: crc error`
  the Brick was measured producing.

  **Commit two central directories, not one**, and make them `v2.7.26.54e0d8d` and
  `v2.8.0.47db0e3`. Those two releases differ in both of the ways a zip can quietly break this
  parser — a flat member path against a `nrf52840/` prefixed one, and a local header that agrees
  with the central directory against one whose CRC is 0 — and a suite that holds only one of them
  passes while the code is wrong. The T114's entry in each is the anchor:

  | | 2.7.26.54e0d8d | 2.8.0.47db0e3 |
  |---|---|---|
  | zip size | 46,259,773 | 58,592,416 |
  | entries / central directory | 139 / 16,316 B | 171 / 21,358 B |
  | member | `firmware-…-t114-<ver>.uf2` | `nrf52840/firmware-…-t114-<ver>.uf2` |
  | general purpose flags | `0x0002` | `0x0008` (data descriptor) |
  | local header CRC | 3175933648 (agrees) | **0** |
  | central CRC | 3175933648 | 2320343302 |
  | compressed / uncompressed | 517,956 / 1,467,392 | 561,092 / 1,491,968 |
- **A fake bootloader** on `mesh_serial_usb_mock_enable`: accepts Bulk-Only Transport,
  reassembles the UF2 blocks it is handed, and can be told to stall a CSW, to fail a
  `WRITE(10)`, or to vanish mid-write. The assertions are the interesting part — every block
  arrived, each one carried the right family id, and `numBlocks` was satisfied exactly once.
- **A fake loader** built on `mesh_bluez_client_mock_enable`: answers `VERSION`, `ERASING`/`OK`,
  ACKs per write, and can be told to stop ACKing, to answer `ERR Hash Mismatch`, to split a
  write, or to drop the link mid-stream. Every one of those is a state the screen has to name.
- **A UF2 golden**: the first and last block of a real release `.uf2`, committed, with the
  parser's answers pinned. It is a fixed 512-byte record and it should stay one. The T114's
  2.7.26 image is the one to pin, and the answers are known: 2,866 blocks of 512 bytes, `flags`
  `0x2000` (family id present), `familyID` `0xADA52840`, `payloadSize` **256** on every block,
  `blockNo` sequential from 0, and target addresses running contiguously from `0x26000` to
  `0xD9100` in 256-byte steps. Pinning `payloadSize` matters more than it looks: 256 is not the
  476 a UF2 block can carry, so an arithmetic written against the maximum is wrong by 46% and
  still produces a plausible-looking number.
- **A golden transcript** for the `ota_request` encode, in the same spirit as
  `message_encode_text_golden`: a protobuf regeneration that moves the field number should fail
  loudly rather than quietly send the radio nowhere.
- **Fuzz** the zip and manifest parsers. They read attacker-shaped input off the network and
  they are exactly the kind of length-prefix arithmetic `make fuzz` exists for.
- **Fuzz** the UF2 reader too — it is fixed-size records with a self-declared payload length,
  which is the shape that goes wrong.

  Both are in, as `devtools/fuzz/fuzz_zip.c` and `fuzz_uf2.c`, and both check contracts on top
  of memory safety because either can be memory-safe and still wrong in a way that matters: a
  slice that came back has to lie inside the window it was cut from, a member's data cannot be
  placed before the header that placed it, and a file accepted for one family must never be
  accepted for another. 85 million cases, no findings. The one arithmetic that had to be fixed
  to make the last contract hold is a block whose `targetAddr + payloadSize` overflows 32 bits:
  left in, the span a caller computes comes back enormous, which is a progress bar that never
  moves and a size check that passes.
- **On hardware**, and not skippable: a T114 or another nRF52840 over USB, including one write
  interrupted halfway and then repeated; then a real ESP32 and a real S3, one interrupted stream
  resumed, one wrong-hash refusal, and one radio whose `app1` holds the old loader.

  The **uninterrupted** half of that has been done by hand — a full 2.7.26 `.uf2` written to a
  T114 on 2026-09-10, board rebooted, client reconnected — so what phase 3 owes on hardware is
  the *interrupted* one, and a way to cause it. Pulling the cable mid-write is the honest test:
  the expected outcome is a board still sitting in its bootloader with `/dev/sda` back on the
  next plug, and a second write that succeeds. There is a cheap rehearsal for everything above
  that, too, and it is worth knowing: **a block with no UF2 magic is discarded by the
  bootloader and reported as written**, so the transport can be exercised at full size, at full
  speed, against a real board, without touching its flash at all.

## What phase 2 measured

Four things, on 2026-09-10, while building the parsers against the served bytes rather than
against this document. None of them changes the plan; three change a number somebody would
otherwise have designed against, and one is a bug this feature introduced and then found.

- **At 2.8.0 the local header's sizes are 0 as well as its CRC.** This document said the CRC;
  measured, the compressed size and uncompressed size are zero too, because the general purpose
  flag is `0x0008` and all three are deferred to a data descriptor after the payload. Reading
  the CRC from the central directory and the sizes from the local header — which is the obvious
  half-fix — asks the CDN for `bytes=<start>-<start-1>` and then verifies whatever comes back
  against a CRC that also came from the wrong place.

- **The UF2 family ids, off real images rather than off a table.** `nrf52840` is `0xADA52840`,
  `rp2040` is `0xE48BFF56`, and `rp2350` is **`0xE48BFF59`** — the ARM secure family. The RP2350
  publishes several (ARM secure, ARM non-secure, RISC-V) and upstream builds that one, which is
  not a thing to guess when the failure is a board holding an image its bootloader will not
  start. Confirmed by walking every block of `firmware-rp2040-lora-…uf2` (3,851 blocks) and
  `firmware-pico2w-…uf2` (5,577), both of which also carry `payloadSize` 256 and flags
  `0x2000` throughout, exactly as the T114's does.

- **The 2.8.0 zip opens with a directory marker**, `nrf52840/` — stored, zero length, no flags —
  and it is the first entry a basename walk reaches. Its basename is empty, so a matcher that
  did not refuse an empty basename would hand the download a member whose bytes are a directory.
  It is also the only stored entry in any release zip measured; every real member is deflated.

- **`kill(0, …)` is the process group.** `struct mesh_firmware_download` is zeroed and then
  started, which is the ordinary way to use it — and a zeroed struct holds `0` where a child pid
  goes. An app that ticks all its modules ticks this one before anything has been started, and
  the deadline check then fired on a pid of 0, sending SIGKILL to the client and everything it
  had spawned. What it looked like was the test runner being killed with no output. The guard is
  `<= 0` rather than `< 0`, and it is worth stating here because phase 3 adds a second child
  (the write) with exactly the same shape.

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

## What phase 0 measured

Answered on 2026-09-10 against the real pair: a TrimUI Brick (`tg5040`, TinaLinux, Linux
4.9.191) with a Heltec Mesh Node T114 (`239a:4405`, `hw_model` 69, running
`2.7.26.54e0d8d`) on the USB-C port. Questions 1, 2, 3 and 7 are the USB path's and are now
closed; 4, 5, 6 and the battery half of 8 are the BLE path's and still are not. The headline is
that **the whole of part two and part four ran end to end on the device** — a range-read of the
release zip, a gzip-envelope inflate, a UF2 validated block by block, 1,467,392 bytes written to
the bootloader, and the board back on the mesh — which makes phase 3 an implementation rather
than an investigation.

### The four answers

**Q1 — a bootloader *is* a block device here, and that removes a file from the plan.**
`CONFIG_USB_STORAGE=y`, `CONFIG_BLK_DEV_SD=y` and `CONFIG_SCSI=y` are all built into this
kernel, along with `CONFIG_USB_UAS=y` and `vfat`. The fear that produced
`src/transport/serial/usb_msc.c` was that `CONFIG_USB_ACM is not set` predicted the storage
stack would be missing too; it does not, and the two facts turn out to be unrelated. A
double-tap of reset re-enumerates the board as **`239a:0071`** with three interfaces — the CDC
pair, plus one at `08/06/50`, mass storage / SCSI / **Bulk-Only Transport** — `usb-storage`
binds it unprompted, and `/dev/sda` appears about a second later:

```
BLOCK sda size=65801 sectors vendor='Adafruit' model='nRF UF2         ' rev='1.0 '
ghost FAT16, OEM 'UF2 UF2', volume label 'HT-n5262'
```

So the twenty-line fast path is the one that exists, and the 350 lines of Bulk-Only Transport
over usbfs are a **fallback for a kernel that is not this one**, not the first thing to write.
That is a phase's worth of work deferred, and it should be deferred: usbfs BOT is still the
right answer for a Brick whose kernel differs, but nothing can be tested against such a Brick
today.

**Q2 — the port carries it.** The bootloader declares `MxPwr=100mA`, exactly what the app
firmware declares, and it enumerated, held up through a full write and rebooted on bus power
alone. There is no current question here.

**Q3 — thirteen seconds, and the flash costs nothing on top.** Two writes of the same size, one
of them real:

| | Bytes | Time | Rate |
|---|---|---|---|
| 1,491,968 B of **non-UF2** blocks (discarded by the bootloader — transport only) | 2,914 blocks | 13.2 s | 113 KB/s |
| 1,467,392 B of the **real** `heltec-mesh-node-t114-2.7.26.54e0d8d.uf2` | 2,866 blocks | 13 s | 113 KB/s |

The two being the same number is the finding. The bootloader programs each 256-byte payload as
it arrives and the nRF52840's USB is full-speed, so **the 12 Mbit/s link is the bottleneck and
the flash erase/program hides entirely behind it**. The board reset itself into the new firmware
**1 s** after the last block, and the client had a handshake and an admin session 3 s after
that. A minute is a generous budget for the whole handover; three minutes of continuous writing,
which question 8 was framed around, does not happen on this bus.

The write also does not care how it is chunked. `bs=512`, `bs=4096` and `bs=61440` all took
13.2 s, because the block layer merges everything into commands of `max_sectors_kb` (120 KB on
this device) regardless. The client can write in whatever size is convenient for progress
reporting.

**Q7 — `gzip -dc` is there, and it checks the CRC.** busybox 1.27.2 supplies `gzip`, `gunzip`
and `zcat`; `unzip` is **not** present, which closes the third of part two's three inflate
options on its own. The envelope works exactly as hoped, and it was checked against both ways of
being wrong: a truncated member gives `gzip: crc error` and a wrong `ISIZE` gives
`gzip: incorrect length`. Inflating 1.4 MB takes **0.03 s**. Neither zlib nor a 40 MB download
is needed.

### And five things that were not on the list

**Suffix ranges are refused.** This document said "range-read the last ~64 KB", which is
`Range: bytes=-65536` — and the Varnish cache in front of GitHub's release CDN answers
**`501 Unsupported client range`**. Only an explicit `bytes=<start>-<end>` gets a 206. So the
length has to be learned first, from a `HEAD` (which does answer, with `accept-ranges: bytes`
and a `Content-Length`) or from the `Content-Range` on a one-byte probe. It is one extra round
trip and it is not optional.

**The member's name is not stable across releases, so match on the basename.** In
`v2.7.26.54e0d8d` the zip's 139 entries are flat — `firmware-heltec-mesh-node-t114-<ver>.uf2`.
In `v2.8.0.47db0e3` there are 171 and they sit under a platform directory —
`nrf52840/firmware-heltec-mesh-node-t114-<ver>.uf2`. A parser written against either release
alone fails on the other, and it fails as "this board is not in this release", which reads like
a board problem rather than a parser problem.

**The local header is honest in one release and empty in the next.** This is the same pair of
releases and it is the trap this document already warned about, now with a demonstration on
each side: 2.7.26's members carry `flags = 0x0002` and a local header whose CRC equals the
central directory's, while 2.8.0's carry `flags = 0x0008` — bit 3, a trailing data descriptor —
and a local header CRC of **0**. Code that reads the local header and happens to be tested
against 2.7.26 passes every test and then verifies every 2.8.0 download against a CRC of zero.
Take the CRC and both sizes from the **central** directory, always. The local header is still
read, but only for its own `nlen`/`elen`, which differ from the central directory's `elen` (57
and 32 against the central entry's) and are what place the compressed bytes.

**Plugging a bootloader into a Brick shadows `/mnt/SDCARD`.** This one is a platform bug and it
lands squarely on this feature. `/etc/hotplug.d/block/10-mount` first lets OpenWrt's
`/sbin/block hotplug` mount `/dev/sda` at `/mnt/exUDISK` from `/etc/config/fstab`, which is
fine; then, if that returns non-zero, a TrimUI patch probes for NTFS and on "not NTFS" runs a
hardcoded

```sh
mount /dev/${DEVNAME} /mnt/SDCARD -o rw,iocharset=utf8
```

— so the bootloader's 32 MB ghost FAT gets mounted **over the SD card**, and with it go the pak,
the client's own binary, the CA bundle, the log and anywhere the roadmap proposed staging the
image. It is not theoretical: the running client logged `Failed to persist handshake cache: -2`
every two seconds for the ten minutes the shadow was up. Three consequences, all of them phase 3
requirements:

- **the image must be in memory, or outside `/mnt/SDCARD`, before the radio is sent into DFU.**
  1.5 MB against ~700 MB free is nothing; `/mnt/UDISK` (5.9 GB, internal) is the alternative if
  it must be a file. Staging it "beside the client's own `.update` area", as part two proposed,
  is the one place it must not go;
- **the client must unmount the drive before it writes to it**, from both mount points, or the
  VFAT driver's own writeback interleaves FAT sectors with our UF2 blocks. They are discarded
  for want of the magic, so the radio is safe either way — but a write racing a filesystem
  driver over the same device is not a thing to leave in;
- **and it should put `/mnt/SDCARD` back**, because a user who quits to the launcher during an
  update otherwise finds an empty card.

**The client already binds the bootloader's CDC and tries to sync with it.** A UF2 bootloader
presents a CDC pair next to its drive, and `mesh_serial_usb_scan()` matches *any* CDC-data
interface — there is no VID/PID filter and no test of what else the device carries. So the
moment the T114 re-enumerated, the client bound `239a:0071` to `usbserial_generic` itself,
asserted DTR, called it `HT-n5262`, auto-connected and sent a config sync that nothing will ever
answer:

```
[INFO] (serial): Bound 239a:0071 to the generic usbserial driver
[INFO] (serial): Opened /dev/ttyUSB0 (HT-n5262); waking the radio
[INFO] (app): Auto-connecting to HT-n5262 over USB (2-1:1.1)
[INFO] (session): Requested config sync (request_id=1900718444)
```

On the frame that reads as a connected radio with the progress bar turning forever, which is the
one state a link is not allowed to have. See
[§Knowing a bootloader when we see one](#knowing-a-bootloader-when-we-see-one) — it is a bug
today, for anyone who double-taps a node, and it is phase 3's detection written the other way up.

### What a second board added

A Heltec V3 — freshly full-flashed at `2.7.26.54e0d8d`, never paired over Bluetooth, region set
and nothing else — went on the same port on 2026-09-10. It is the first ESP32-S3 anything here
has been checked against, and it answers none of the four open BLE questions, because all four
need a radio that is actually *in* the OTA loader and there is still nothing that could get one
back out. What it did settle is cheaper and was not on the list:

- **Phase 1's wrong-bus refusal works, and this is the first time it has been seen.** The check
  answered `hw_model 43 is heltec-v3 (esp32-s3, supported)`, found the newest alpha, and the
  Installing row said **"connect by Bluetooth"** — `MESH_FIRMWARE_BLOCKER_WRONG_BUS` resolving
  through `MESH_STR_FW_BLOCK_CONNECT_BLE`. Everything phase 1 does is now exercised on real
  hardware across both architectures and both outcomes: an nRF52 on the bus its path wants,
  reporting an update it could take, and an ESP32-S3 on a bus its path does not want, naming the
  bus that would work. That is [§The UI](#the-ui)'s "most common refusal and the most useful
  one" — the row that names a thing the user can go and do — and it needed a second board to
  produce at all.
- **It is a bridge chip, not a native-USB node** — `10c4:ea60`, CP2102, one vendor-class
  interface claimed by the in-kernel `cp210x`. No CDC, no usbfs DTR workaround needed, and
  nothing about the radio visible in the USB tree. That is what produced the two-kinds reading in
  [§Knowing a bootloader when we see one](#knowing-a-bootloader-when-we-see-one), and the
  corollary that the ESP32 path can never answer "has it come back yet" from USB.
- **`hw_model` 43 is unambiguous** (`heltec-v3` alone), so the V3 never exercises the variant
  picker. Something from the `HELTEC_WIRELESS_TRACKER` family — four targets on model 48 — is
  what that needs, and nothing on the desk is one.
- **The three spellings are real and only bite on S3.** See [§Part one](#part-one-which-image):
  `esp32-s3` in `deviceHardware`, `esp32s3` in the release manifest and in the board's own
  `.mt.json`, and the wrong one is a 404 rather than a slower path.
- **The two `.mt.json` shapes**, which is the correction in
  [§Part two](#part-two-getting-the-bytes): `part_name: app0` selects the image on ESP32 and does
  not exist at all on nRF52.
- **Its `app1` holds 2.7.26's unified loader**, because a full USB flash is what writes `app1`
  and the release ships `mt-esp32s3-ota.bin` for exactly that slot. So this board *will* accept
  `ota_request` and strand itself — which makes it the right phase 4 test target and the wrong
  thing to experiment on today. When phase 4 does run against it, have `esptool` staged.

The one BLE question it could help with early is **question 4**, and it turns out not to be the
blocker there: MTU and `WriteValue` cost are a property of the Brick's BlueZ (5.78 here, so
`AcquireWrite` is available and its reply carries the MTU) talking to *any* peripheral, and the
T114 has BLE too. Measuring both is still worth it — an ESP32 BLE stack negotiates differently
from an nRF52 one — but nothing has to wait for it.

## Phases

Each phase is worth shipping alone, which is the test of whether the order is right.

**Phase 0 — measure. The USB half is done**, 2026-09-10. No feature. Answer the questions below
on a Brick with a radio attached. Two of them can move a whole phase: whether the kernel gives
us a block device for a bootloader in mass-storage mode (question 1), and what BLE write
throughput through D-Bus actually costs (question 4). Both are cheaper to know now than to
design around later — and question 1 duly moved one, in the direction of less work. Questions 1,
2, 3 and 7 are answered in [§What phase 0 measured](#what-phase-0-measured); 4, 5, 6 and the
battery half of 8 are the BLE path's and are still open, which is one more reason phase 4 comes
after phase 3.

**Phase 1 — tell the truth. Shipped, and confirmed on hardware** on 2026-09-10 against both
architectures. Settings → About radio learns the release index: what the radio runs, what the
newest stable is, whether this board can be updated from here at all. No downloads, no writes.
This is most of the value for a user who owns a computer, and it is the row that makes the rest
legible. A T114 on USB reports an update it could take; a Heltec V3 on the same port says
"connect by Bluetooth", which is the refusal row doing its job — see
[§What a second board added](#what-a-second-board-added).

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

**Phase 2 — get the image. Shipped, and confirmed on hardware** on 2026-09-10. Resolve,
range-download, inflate, verify, keep. Two sizes travel together from here on and they are not
interchangeable: the compressed member size, which the download is measured against, and the
uncompressed image size, which is what `OTA <size>` later tells the loader to expect. Still
nothing written anywhere. It ends with "0.5 MB fetched, it matches the CRC, and it inflates to
the image the manifest describes", and on a Brick that took **8.1 seconds** — 0.6 MB moved
instead of 46.3, the CRC checked by the device's own busybox `gzip`, and 2,866 UF2 blocks read
and found to be for an nRF52840. Serves both paths: a `.uf2` and an app image are two members of
the same zip.

What it turned into: [`src/utils/zip.c`](../src/utils/zip.c) for the container — three passes
over three windows, one range request each, because that is the shape the download is — the
`.mt.json` and the release manifest in
[`firmware_catalog.c`](../src/core/firmware_catalog.c), [`src/core/uf2.c`](../src/core/uf2.c)
for the file a bootloader will be handed,
[`firmware_download.c`](../src/core/firmware_download.c) for the four steps and the gzip
envelope, and [`firmware_fetch.c`](../src/core/firmware_fetch.c) for the piece between
"there is newer firmware" and "here are the bytes". `--fetch-firmware <target>` runs the whole
of it from the device and writes nothing to a radio, because the parts of this that can only be
wrong on a Brick — the refused suffix range, the pak's CA bundle, busybox's `gzip` — are not
parts a suite can reach.

The four ways it can go wrong all have a case: the refused suffix range (hence the HEAD), the
moving member path (hence matching on the basename, and two committed release tails rather than
one), the empty local header (hence every size and the CRC coming from the central directory)
and a central directory that does not fit the window (hence
`mesh_zip_central_slice()` answering NULL rather than a pointer, and a fifth range read behind
it that no zip measured needs yet).

**Phase 2.5 — stop calling a bootloader a radio. Shipped.** Small enough that it is barely a
phase, and it was listed as one because it was a **bug** rather than groundwork: double-tap any nRF52
node with the client running and the frame says connected with the progress bar turning forever,
because `mesh_serial_usb_scan()` matches the bootloader's CDC and auto-connect syncs against it.
The fix is a role on the device info, read off the sibling interfaces —
[§Knowing a bootloader when we see one](#knowing-a-bootloader-when-we-see-one) — and it is
phase 3's "has it come back yet" written first and in the negative. Shipping it before phase 3
means the detection is already proven when the phase that depends on it arrives.

What it turned into: `enum mesh_serial_device_role` on `struct mesh_serial_device_info`, filled
in by the same sysfs walk that already found the CDC control interface;
`mesh_serial_transport_connect()` refusing a bootloader with `-ENOTSUP` before it binds
anything; `mesh_app_autoconnect()` skipping one when it picks a port; an `in bootloader` badge
in the Devices tab; and `MESHCLIENT_SYSFS_USB`, a test seam that lets the reading be checked
against a fixture tree laid out as the Brick's sysfs was measured — the mock replaces the scan
whole, so it can prove what the transport does with a role and nothing about how the role is
decided. It also closed a rule the client was breaking either side of the new case: `A connect`
and `Y forget` were unconditional on the Devices bar while the nav declined them on three kinds
of row, so both now ask `mesh_ui_device_connectable()` / `mesh_ui_device_forgettable()` — one
function for the bar and the press, as `mesh_ui_help_offered()` already is.

**Phase 3 — the USB handover.** `enter_dfu_mode_request` down the serial link, wait for the
bootloader to enumerate, unmount the ghost drive the platform will have mounted over
`/mnt/SDCARD`, write the `.uf2`'s blocks to the block device, watch the board reboot. This is
the first phase that changes a radio, and it is deliberately the one whose worst outcome is
"write the blocks again". It also covers the boards the BLE path never will: every nRF52840
target, plus RP2040 and RP2350 for the price of a different family id.

The whole of that sequence has now been done by hand on a T114 — twenty seconds from the first
range request to a radio back on the mesh — so what this phase owes is the code, not the
question of whether it works. The two things the manual run had to be careful about are the two
this phase must not forget: the drive is mounted by the time we want to write to it, and
`/mnt/SDCARD` is not where the image can live while that is true.

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
the design. The first three were the USB path's and came first because that phase does; they and
question 7 were answered on 2026-09-10, and the answers are in
[§What phase 0 measured](#what-phase-0-measured). What is left is the BLE path's, plus a battery
reading that only the BLE path still needs a number for.

1. ~~**Does a bootloader show up as a block device?**~~ **Answered: yes.** `CONFIG_USB_STORAGE`,
   `CONFIG_BLK_DEV_SD` and `CONFIG_SCSI` are all built in, `usb-storage` binds the bootloader
   unprompted, and `/dev/sda` appears about a second after the double-tap. Phase 3 gets its
   twenty-line fast path and usbfs BOT becomes the fallback. `CONFIG_USB_ACM` being off — which
   is what made this look like a coin toss — predicted nothing about the storage stack.
2. ~~**Does the port carry it?**~~ **Answered: yes.** The bootloader declares the same `100mA`
   the app firmware does, and enumerated, held up through a full write and rebooted on bus power.
3. ~~**How long does 1.4 MB of `WRITE(10)` take?**~~ **Answered: 13 s**, on the block-device
   path, for a real 1,467,392-byte `.uf2` — and the same 13 s for the same number of blocks the
   bootloader *discards*, so the flash costs nothing on top of a full-speed USB link. Sleep never
   came into it at twenty seconds end to end; if it ever does, the answer is the hold the
   updater already takes.
4. **BLE throughput.** What does a 512-byte `WriteValue` cost through BlueZ on this hardware, and
   what MTU does the Brick negotiate? At 20-byte writes and a round trip each, 2.2 MB is not a
   feature. If it is bad, does `AcquireWrite` (a socket fd, which the event loop would be happy
   with) fix it?
5. **The bond.** The radio is bonded; the loader at MAC+1 is a different address with no
   security. Does BlueZ connect cleanly, or does it try to encrypt with a key nobody has?
6. **Cached services.** After the radio reboots into the loader and back out again, does BlueZ
   re-discover its GATT database, or does it serve a stale one? The phone app has a
   cache-refresh reconnect delay for a reason.
7. ~~**`gzip`.**~~ **Answered: yes**, busybox 1.27.2's, along with `zcat` and `gunzip` — and it
   enforces both the CRC32 and the `ISIZE`, so the envelope is the download's integrity check and
   not just an inflate. There is no `unzip`, which closes the third option on its own. zlib is
   not needed.
8. **Power, and a radio that refuses.** What does the Brick's battery do over three minutes of
   continuous writing on either bus, and what floor should refuse the press? The USB path no
   longer asks this — thirteen seconds is not a power question — so it is the BLE path's alone.
   And take a node flashed with an older full install, send it `ota_request`, and confirm the
   refusal arrives as a `ClientNotification` the way the source says it does — the one upstream
   claim in this document that nothing here has watched happen.

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
