# Testing

The suite is **one binary with a name filter**, not per-test CTest entries. Cases live in
`tests/suites/<area>.c` and **register themselves** — there is no central table to keep in sync.

| Path | What lives there |
|---|---|
| `tests/framework/` | `MESH_TEST_CASE`, the guard macros, the registry and `main` |
| `tests/support/` | fixtures shared by more than one suite, prefixed `mesh_test_` |
| `tests/suites/` | the cases themselves, one file per area |

Categories are `unit` (everything today), `integration` and `hardware` (both reserved; CI skips
hardware with `ctest -E HARDWARE`).

## Running

```bash
make test                                                    # debug build + ctest
./build/debug/tests/meshclient_core_tests --list             # names, categories, suites
./build/debug/tests/meshclient_core_tests --filter ble_transport   # substring match
./build/debug/tests/meshclient_core_tests --suite ui_nav
```

Four CTest entries are not in the suite binary: `meshclient_hardcoded_strings` runs
`scripts/check-strings.py` (see [`i18n.md`](i18n.md)), `meshclient_vendored_files` re-checks
vendored code against the digest its own README states, `meshclient_frames_codec` round-trips the
GIF encoder in `scripts/frames.py` through an independent decoder, and `meshclient_bluez_bus`
starts a private bus with a fake GATT service when `dbus-run-session` is installed. None of them
contacts real BlueZ.

## Adding a test

1. Open the `tests/suites/` file for the area, or add one and list it in
   `MESHCLIENT_TEST_SUITES` in `tests/CMakeLists.txt`.
2. Write the case with `MESH_TEST_CASE(name, category)` — that is the whole registration, and
   `test_name` is already in scope.
3. End every path with `record_success(test_name)` or a failure. `MESH_TEST_FAIL_IF` is the short
   form; `MESH_TEST_FAIL_IF_CLEANUP` is for a case holding a loop, a mock or an fd.

```c
MESH_TEST_CASE(config_defaults, unit) {
    struct mesh_app_config config = mesh_app_config_default();
    MESH_TEST_FAIL_IF(config.idle_timeout_ms != 1000, "idle timeout should default to 1000 ms");
    record_success(test_name);
}
```

Suite files are compiled straight into the executable, not through a static library: cases are
found through constructors and a linker may drop unreferenced library members.

## Rules

- **Never touch real BlueZ.** Use `mesh_bluez_client_mock_enable`; `mesh_serial_usb_mock_enable`
  does the same for sysfs and usbfs, and its `open_fd` hands the link one end of a socketpair.
- **A helper used by one suite stays `static` in it**, and moves to `support/` when a second
  suite needs it.
- **A new transport needs a golden protobuf frame test.** `message_encode_text_golden` pins the
  `TEXT_MESSAGE_APP` wire format against a hand-derived byte vector rather than against our own
  encoder, so a protobuf regeneration that changes field numbers fails loudly.
- **Cover the error paths** — BLE reconnection and protobuf parsing especially.

`tests/suites/ui_geometry.c` draws every screen at four panel sizes and holds invariants (the
nav bar is drawn, the action bar is drawn, nothing is written into the first or last column)
rather than golden images, which would fail for the wrong reason on every legitimate UI change.
`tests/suites/ui_capture.c` covers the fb renderer through `mesh_ui_capture_*`, the only way its
output is exercised anywhere but on a Brick.

## What CI runs

Four jobs on every pull request ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)):

| Job | What it proves |
|---|---|
| `Build and test` | `make test` on ubuntu-24.04 under gcc *and* clang, then `make release && make package` |
| `Cross build (tg5040)` | the pak builds for the device, aarch64 and statically linked. Uploads the zip, so a pull request can be sideloaded without building it. It is GCC 14.3 (musl) at `-Os`, which derives bounds the host build does not — `-Wformat-truncation` fires here and nowhere else |
| `Sanitizers` | the same suite under ASan and UBSan, clang, with `-fno-sanitize-recover=undefined` so a diagnostic fails the run |
| `Fuzz` | the deterministic pass below |

Nothing gates on formatting: the tree is normalised with clang-format 18 and host versions vary.

## Fuzzing

Two harnesses in `devtools/fuzz/`, over the two places bytes we did not write enter the client:
`stream_framing` (`mesh_stream_parser_push()` — the serial link, where the radio interleaves its
text log with framed protobufs on one port) and `session` (`mesh_session_handle_from_radio()`).

```bash
make fuzz                       # what CI runs: seeds, then 20k fixed-seed runs
make fuzz ARGS="--time 600"     # an actual hunt, ten minutes per target
make docker-fuzz                # the same on macOS
```

**Memory safety is not the oracle for the session.** The counted arrays a decoded packet lands
in — the 256-node roster, the 8-slot channel table, the message ring — live *inside*
`struct mesh_session`, so a write past `nodes[255]` lands on the next field of a struct the
allocator handed out whole: nothing faults and nothing is poisoned. The harness checks those
counts by hand. The framing harness checks two identities: every byte pushed is accounted for
once, and every byte handed to the text callback is counted as dropped.

The seed corpus is **generated, not committed** — `meshclient_fuzz_seeds` encodes one real
message per FromRadio variant with the same nanopb encoders the client decodes with, so a
protobuf regeneration changes the seeds with it.

Needs clang and Ubuntu's `libclang-rt-18-dev`, which the `clang` package does not pull in.
