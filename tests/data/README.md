# Captured bytes

Documents fetched from a real service and committed so the parsers that read them can be tested
without a network, a radio, or a mock of either. A test reads these through
`mesh_test_data_path()`, which resolves them against `MESH_TEST_DATA_DIR` — the source
directory, not the build one, so nothing has to be copied at configure time.

Each file says where it came from and what, if anything, was cut. **Nothing here is
hand-authored**: a fixture that was written to suit a parser tests the parser against itself.
Refresh one by fetching it again, not by editing it.

| File | Source | Captured | Trimmed |
|---|---|---|---|
| `device_hardware.json` | `https://api.meshtastic.org/resource/deviceHardware` | 2026-09-09 | nothing — all 116 boards, verbatim |
| `firmware_list.json` | `https://api.meshtastic.org/github/firmware/list` | 2026-09-09 | see below |
| `zip_tail_nrf52840_2.7.26.bin` | the last 64 KB of `firmware-nrf52840-2.7.26.54e0d8d.zip` | 2026-09-10 | nothing — it *is* the range the client asks for |
| `zip_tail_nrf52840_2.8.0.bin` | the same window of `firmware-nrf52840-2.8.0.47db0e3.zip` | 2026-09-10 | nothing |
| `t114_2.7.26.mt.json` | that member of the 2.7.26 nrf52840 zip, inflated | 2026-09-10 | nothing — all 1,157 bytes |
| `heltec_v3_2.7.26.mt.json` | the same member of the 2.7.26 **esp32s3** zip | 2026-09-10 | nothing — all 2,205 bytes |
| `t114_2.7.26.uf2` | the T114's image out of the 2.7.26 nrf52840 zip | 2026-09-10 | four of its 2,866 blocks — see below |

`firmware_list.json` is the one that is not whole, because the served document is 155 KB and
almost all of it is release notes. Kept: the first four entries of each channel, every key each
one carries, and `pullRequests` cut to one. Each `release_notes` keeps its real first line and
then carries text that looks like the document's own structure — a `"zip_url"`, an `"id"`, an
escaped quote and a brace — because that is the trap this index sets. Release notes are written
by whoever merged the pull request, so sooner or later one of them contains the keys the parser
is looking for, and a reader that scanned for `"zip_url":` instead of walking the structure
would find the wrong one. The fixture makes that failure a test rather than a surprise.

Two other things in it are load-bearing and were not staged: the newest `alpha` entry has **no
`zip_url` at all** (a release can appear in the index before its assets do), and the second
`stable` entry's URL is a per-platform `.zip` rather than a per-release `.json` manifest. Both
are real, both are shapes the parser has to survive, and neither is the newest of its channel by
accident.

## The phase 2 fixtures

**Two zip tails and not one.** `v2.7.26.54e0d8d` and `v2.8.0.47db0e3` differ in both of the ways
a zip can quietly break the reader in [`src/utils/zip.c`](../../src/utils/zip.c): the member
path is flat in the older release and under `nrf52840/` in the newer, and the older release's
local file header agrees with the central directory while the newer one's carries a CRC and
both sizes of **0**, deferred to a data descriptor after the payload. Each of those is silent —
a full-path match reports "this release does not build for your board", and a reader trusting
the local header range-reads zero bytes and checks them against a CRC of zero, which passes. A
suite holding only the older tail is green while the code is wrong, which is why there are two.

They are 64 KB because that is the window the client requests: the largest tail a conforming
zip can need, and — happily rather than by guarantee — enough to hold both of these central
directories whole, which is what makes the download two round trips instead of three.

**Two manifests and not one, for the same reason.** The T114's has no partition table and no
`part_name` on any of its four files, because an nRF52 has none to name; the Heltec V3's has six
partitions and names three. The image is selected by a different question on each path, and a
parser written against either fixture alone finds nothing for half the boards this feature
serves. The pair also carries the three-spellings trap: `mcu` is `esp32s3` and `architecture` is
`esp32-s3` on the V3, and both are `nrf52840` on the T114.

**`t114_2.7.26.uf2` is four blocks of 2,866** — the first two and the last two of the real
image, cut and otherwise untouched, so 2 KB instead of 1.4 MB. Keeping both ends rather than a
prefix pins where the image starts (`0x26000`) and where it ends (`0xD9100`), and hands the
suite both download failures for free: the first half is a truncated file, and the whole of it
is a file missing its middle, which the third block announces by being numbered 2,864 where 2
was due.
