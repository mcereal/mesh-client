# Updating the radio's firmware from the Brick

**This is about the *radio's* firmware, not the client's.** MeshClient already updates itself —
that is [`src/core/updater.c`](../src/core/updater.c), and everything below borrows from it.
This document is about the other binary in the room: the Meshtastic firmware running on the
node the Brick is talking to, which today can only be changed with a computer and a cable.

Status: **proposed**, 2026-09-09. Nothing here has shipped. The upstream facts were read out
of `meshtastic/firmware` at `81b3ce8`, `meshtastic/esp32-unified-ota` at its head,
`meshtastic/firmware-ota` (the old loader) and `meshtastic/Meshtastic-Android`'s
`feature/firmware` module, and the release layout was measured against
`v2.7.26.54e0d8d` by range-reading the published zips. Everything about the *Brick* is a
proposal and is marked as such; the measurements it needs are in
[§Before any of this is written](#before-any-of-this-is-written).

It supersedes the one-line answer in
[`settings-roadmap.md`](settings-roadmap.md) — "large, hardware-specific, can brick the radio,
deferred indefinitely". That answer was right about the risk and wrong about the size: for
*one* family of radios the whole thing is a text protocol over a pair of GATT characteristics —
one written, one notified — and the client already owns every piece except the streaming.

## The short version

| | |
|---|---|
| **Do** | ESP32 and ESP32-S3 radios, over BLE, using the admin verb `ota_request` and the unified OTA loader. That is roughly 73 of the 129 build targets upstream ships. |
| **Don't** | nRF52 (Nordic DFU: two protocols, bond-key subtleties, SoftDevice variants, ~2 500 lines in the phone app), ESP32-C3/C6 (their loader is the old one and current firmware refuses to boot into it), Wi-Fi OTA (the Brick's Wi-Fi and Bluetooth are one part behind one antenna), and USB/UF2 (needs a kernel that mounts the bootloader's mass-storage drive). |
| **Say so anyway** | Every radio gets a row telling it which firmware it is running, whether a newer one exists, and — when we cannot install it — the one sentence about why and what to use instead. A row that cannot be pressed is still the answer to "why is my radio behaving like that". |

The one fact that shapes the whole feature: **the ESP32 OTA loader has no way back.** Once the
radio reboots into it, its boot partition points at the loader and stays there. There is no
timer, no reboot counter, no fallback to the old firmware. The radio is off the mesh until an
update completes — which it can do over BLE, with no cable, but only if something finishes the
job. So the press that starts this is not "download and install"; it is "take the radio off the
mesh until this is done", and the client has to be built to finish, or to be able to come back
and finish.

## What the phone app actually does

Worth reading before designing ours, because the phone app is what users will compare us to,
and its shape is dictated by the same upstream protocols.

`DefaultFirmwareUpdateManager` routes on two axes — how you are connected, and what the radio
is built on:

| Connection | ESP32 family | nRF52 family |
|---|---|---|
| BLE | `Esp32OtaUpdateHandler` — admin `ota_request`, reboot into the loader, stream the image | `SecureDfuHandler` — buttonless trigger, then Nordic Secure **or** Legacy DFU |
| Serial/USB | refused outright | `UsbUpdateHandler` — copy a `.uf2` onto the bootloader's mass-storage drive |
| TCP | Wi-Fi OTA, same loader, TCP instead of GATT | refused outright |

So "the Meshtastic app can do firmware updates" is four features, and only one of them is small.

## Part one: which image

The radio tells us `DeviceMetadata.hw_model` (a `HardwareModel` enum value) and
`firmware_version`. It does **not** tell us its build target, and the build target is what
names the file. Resolving one to the other takes two documents, both published, both small:

1. `https://api.meshtastic.org/resource/deviceHardware` — 39 KB, 116 entries, one per board:
   `hwModel`, `hwModelSlug`, `platformioTarget`, `architecture`, `activelySupported`,
   `requiresDfu`. This is the same list the web flasher uses.
2. `https://api.meshtastic.org/github/firmware/list` — the release index, `stable` and `alpha`,
   newest first, each with a tag and a link to that release's manifest
   (`firmware-<version>.json`, 10 KB, 129 targets, each with its `platform`).

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

## Part three: handing them over

This is the part that is genuinely small, and it is the reason to do the ESP32 family first.

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

## What has to be built

| New | Roughly | Notes |
|---|---|---|
| `src/core/firmware_catalog.c` | 400 lines | the two API documents, the zip central directory, the `.mt.json`, and the "which target is this radio" answer including the ambiguous case. Parsers are the fiddly part; they are also pure functions over captured bytes, so they are the easiest thing here to test |
| `src/core/firmware_update.c` | 500 lines | the state machine: resolve → download → inflate → hash → admin → wait for the loader → stream → verify → reconnect. One state per thing the screen can name, exactly as `enum mesh_update_state` does |
| `src/transport/ble/ble_ota.c` | 300 lines | the loader conversation. Chunk, write, await `ACK`, count. Sits on the bluez client, not on `mesh_session` |
| `mesh_bluez_client_find_characteristics()` | small | generalise `find_meshtastic_characteristics` to any service/characteristic pair. It is already generic underneath |
| Write-without-response | small | one `{"type": "command"}` entry in the options dict that is currently always empty |
| UI: a Firmware section, a progress screen, a banner, a confirm sheet | medium | see below |
| Strings, help notes, icons | small | catalog lines, one section note plus per-row notes, no new icons expected |

The BLE OTA transport deliberately does not go through `mesh_session`: the loader is not a
Meshtastic node, speaks no protobuf, and has no node number. Routing it through the session
would mean teaching the session about a peer that has none of the things a session is about.

## The UI

Where it lives: **Settings → About radio**, under the firmware version row that is already
there, because that is where somebody who wants to know what their radio is running already
went. The Radio actions section is verbs against a working radio; this is a verb that stops it
being one for a while.

The states are the same shape as the About screen's self-update, and for the same reason — a
row that names one of them is a row the user can read:

```
idle → resolving → downloading → verifying → ready
     → arming (admin sent, waiting for the notification)
     → waiting for the loader (scanning)
     → sending 43% → finishing → reconnecting → done
     → failed ("why", in one line)
     → interrupted (the banner state: the radio is in the loader and we are not talking to it)
```

The house rules that already answer most of the design questions:

- The **screen progress bar** costs no body row and the **banner** costs rows. Sending is a
  request already in flight, so it goes in the bar; "your radio is in update mode" is content
  about the client's world, so it is a banner and it takes its rows honestly.
- The confirm sheet is not optional and its two rows are not "OK/Cancel". It has to say the
  thing that is actually true: *this radio leaves the mesh now and only comes back when this
  finishes*.
- No sentence is spelled out in a renderer. Every state above is a string id; every failure
  reason is a string id plus, where the firmware supplied one, the firmware's own words —
  which stay untranslated, like log lines and region codes.
- The Firmware rows get a section note in `src/ui/help.c` (what an OTA is, what it costs, what
  cannot be undone), and the "your board could not be identified" row gets its own.
- A `make ui-capture` scene per state, because the whole point of the capture harness is that a
  screen nobody can reach without a radio in the loader is still reviewable as a picture.

Refusals are rows, not silence — the client says *which* of these is true:

- this board's architecture has no over-the-air path from here (nRF52, C3/C6, RP2040, STM32);
- this radio's loader partition is empty or old (only discoverable by asking, so the row says
  "try it and the radio will tell us" rather than pretending to know);
- the radio is on battery below some floor, or the Brick is;
- we could not tell which of several boards this is, and nobody has picked yet.

## What each hash actually proves

Worth writing down plainly, because there are three hashes here and none of them is a signature.

- **The zip's CRC32** proves the bytes arrived intact. That is all a CRC does.
- **The md5 in `.mt.json`** proves the file matches what the release build recorded — but the
  manifest comes down the same connection as the payload, so it is a transfer check, not an
  authenticity check.
- **The SHA-256 in `ota_request`** proves the *loader flashed what the client announced*. It
  closes the gap between our client and the radio, not the gap between GitHub and our client.

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
- **A fake loader** built on `mesh_bluez_client_mock_enable`: answers `VERSION`, `ERASING`/`OK`,
  ACKs per write, and can be told to stop ACKing, to answer `ERR Hash Mismatch`, to split a
  write, or to drop the link mid-stream. Every one of those is a state the screen has to name.
- **A golden transcript** for the `ota_request` encode, in the same spirit as
  `message_encode_text_golden`: a protobuf regeneration that moves the field number should fail
  loudly rather than quietly send the radio nowhere.
- **Fuzz** the zip and manifest parsers. They read attacker-shaped input off the network and
  they are exactly the kind of length-prefix arithmetic `make fuzz` exists for.
- **On hardware**, and not skippable: a real ESP32 and a real S3, one interrupted stream
  resumed, one wrong-hash refusal, and one radio whose `app1` holds the old loader.

## Phases

Each phase is worth shipping alone, which is the test of whether the order is right.

**Phase 0 — measure.** No feature. Answer the questions below on a Brick with a radio attached.
If BLE write throughput through D-Bus is bad enough, the whole plan changes shape (to
`AcquireWrite`, which is a socket rather than a method call) and it is much cheaper to know that
now.

**Phase 1 — tell the truth.** Settings → About radio learns the release index: what the radio
runs, what the newest stable is, whether this board can be updated from here at all. No
downloads, no writes. This is most of the value for a user who owns a computer, and it is the
row that makes the rest legible.

**Phase 2 — get the image.** Resolve, range-download, inflate, verify, keep. Two sizes travel
together from here on and they are not interchangeable: the compressed member size, which the
download is measured against, and the uncompressed image size, which is what `OTA <size>` later
tells the loader to expect. Still nothing sent to the radio. Ends with "1.4 MB fetched, it
matches the digest, and it inflates to the image the manifest describes", which is a real thing
to have proven.

**Phase 3 — the handover.** `ota_request`, the loader conversation, the banner, recovery. The
phase that can break somebody's radio, arriving after everything it depends on is already known
to work.

**Phase 4 — the edges.** Resume a partial stream, a battery floor, the variant picker, a "the
loader answered but this is the old one" path, and the help notes that explain what any of it
means.

**Later, maybe never.** nRF52 over Nordic DFU. It is not that it is impossible — the buttonless
trigger is a one-byte write and the bootloader is at the same address or one past it — it is
that the phone app needs two protocol implementations, a fallback coordinator, per-attempt retry
budgets, stale-session cleanup, a bond that must be *kept* because the Adafruit bootloader
whitelists the bonded peer, and a SoftDevice-variant lookup whose failure mode is a corrupted
SoftDevice. That is a feature, not a phase. If it ever happens it should follow ESP32 by a long
way, on real hardware, with a device on the desk that somebody is willing to lose.

Also not planned: Wi-Fi OTA (one antenna), USB/UF2 (needs the Brick's kernel to mount the
bootloader's mass storage, and it does not even have CDC-ACM), installing a firmware file the
user brought themselves (a nice power-user feature and an excellent way to flash a T-Deck image
onto a T-Beam), and anything that flashes the `littlefs` or `factory` images.

## Before any of this is written

Six questions, all answerable in an afternoon with `make deploy-*`, all capable of changing the
design:

1. **Throughput.** What does a 512-byte `WriteValue` cost through BlueZ on this hardware, and
   what MTU does the Brick negotiate? At 20-byte writes and a round trip each, 2.2 MB is not a
   feature. If it is bad, does `AcquireWrite` (a socket fd, which the event loop would be happy
   with) fix it?
2. **The bond.** The radio is bonded; the loader at MAC+1 is a different address with no
   security. Does BlueZ connect cleanly, or does it try to encrypt with a key nobody has?
3. **Cached services.** After the radio reboots into the loader and back out again, does BlueZ
   re-discover its GATT database, or does it serve a stale one? The phone app has a
   cache-refresh reconnect delay for a reason.
4. **`gzip`.** Does the Brick's busybox have `gzip -dc`? If not, is `zcat` there? If neither, the
   answer is zlib and it should be decided now, not in phase 2.
5. **Power.** What does the Brick's battery do over three minutes of continuous BLE writing, and
   what floor should refuse the press?
6. **A radio that refuses.** Take a node flashed with an older full install, send it
   `ota_request`, and confirm the refusal arrives as a `ClientNotification` the way the source
   says it does. This is the one upstream claim in this document that nothing here has watched
   happen.

## Sources

- `meshtastic/firmware` @ `81b3ce8` — `src/modules/AdminModule.cpp` (the `ota_request` and
  `enter_dfu_mode_request` handlers), `src/platform/esp32/MeshtasticOTA.cpp` (partition and
  loader checks, NVS hash), `src/platform/nrf52/main-nrf52.cpp` (`enterDfuMode` is UF2, not BLE),
  `bin/platformio-custom.py` and `.github/workflows/build_firmware.yml` (what goes in a release).
- `meshtastic/esp32-unified-ota` — the loader: `README.md` is a written protocol spec,
  `src/ble_ota.cpp` and `src/ota_processor.cpp` are what it actually does.
- `meshtastic/firmware-ota` — the old loader, same UUIDs, `OTA_SIZE:<n>` and a raw stream with
  no acknowledgement and no hash.
- `meshtastic/Meshtastic-Android` — `feature/firmware/`: routing, timeouts, the MAC+1 rule, the
  chunk-splitting bug and its fix, and the asset names per method.
- Measured against release `v2.7.26.54e0d8d` on 2026-09-09: range support, zip sizes, central
  directory layout, `.mt.json` contents, and the gzip-envelope inflate.
