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
