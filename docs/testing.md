# Testing

The whole suite is **one binary with a name filter**, not per-test CTest entries. Cases live in
`tests/suites/<area>.c`, one file per subject area, and **register themselves** — there is no
central table to keep in sync.

As of 2026-09-06: **123 unit tests, all passing**, zero compiler warnings.

## Layout

| Path | What lives there |
|---|---|
| `tests/framework/` | `MESH_TEST_CASE`, the guard macros, the registry and `main` |
| `tests/support/` | fixtures shared by more than one suite, prefixed `mesh_test_` |
| `tests/suites/` | the cases themselves, one file per area |

Two CTest entries are not part of the suite binary at all: `meshclient_frames_codec` round-trips the GIF encoder, and `meshclient_hardcoded_strings` runs `scripts/check-strings.py`, which fails when a renderer spells out an English sentence instead of naming a catalog id ([`docs/i18n.md`](i18n.md)).

A helper used by a single suite stays `static` in that suite. It only moves to `support/` once a
second suite needs it — that is the whole rule.

`tests/suites/ui_geometry.c` is the one suite that is about a *panel* rather than about a
subject. Everything else that renders draws at the Brick's 1024x768, which cannot separate the
layout's measured behaviour from the fact that 1024x768 is roomy; these cases draw every screen
and both overlays at four geometries and hold three invariants that must be true of any frame on
any panel — the navigation bar is drawn, the action bar is drawn, and nothing is written into the
first or last column. Invariants rather than golden images on purpose: four pinned frames would
be four times the maintenance for every legitimate UI change and would fail for the wrong reason
each time. Render a scene at another size with `./scripts/ui-capture.sh -g WxH`.

## Categories

| Category | Scope |
|---|---|
| `unit` | Everything today: transports, event-loop helpers, session and message handling, UI store/controller/nav, preferences, the updater |
| `integration` | Reserved for cross-module tests (BlueZ-on-device, end-to-end) |
| `hardware` | Reserved for tests needing a real Brick; tagged so CI can skip them (`ctest -E HARDWARE`) |

## Running

```bash
make test                                                    # debug build + ctest
ctest -L unit                                                # both suites directly

./build/debug/tests/meshclient_core_tests --list             # names, categories and suites
./build/debug/tests/meshclient_core_tests --filter ble_transport
./build/debug/tests/meshclient_core_tests --category unit
./build/debug/tests/meshclient_core_tests --suite ui_nav      # everything from ui_nav_*.c
```

The driver prints a `[RUN]` line per case and a pass/fail summary; a non-zero failure count is a
non-zero exit code.

CTest runs one more thing beside it. `scripts/frames.py` hand-rolls GIF's variable-width LZW for
`make ui-capture` and `make deploy-clip`, and the part that is easy to get subtly wrong — when
the code width grows — produces a file that still opens and shows garbage, so
`meshclient_frames_codec` round-trips the compressor through an independently written decoder
(`scripts/frames.py selftest`).

## What CI runs

Three jobs on every pull request ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)):

| Job | What it proves |
|---|---|
| `Build and test` | `make test` on ubuntu-24.04 under gcc *and* clang, then `make release && make package` |
| `Cross build (tg5040, static aarch64)` | that the pak builds for the device. The same `docker/setup-cross.sh` toolchain `make docker-pak` uses, through `scripts/cross-build.sh`, with the binary asserted to be aarch64 and statically linked before it is packaged. The zip and the bare binary are uploaded, so a pull request can be sideloaded onto a Brick without building it |
| `Sanitizers (ASan + UBSan)` | the same suite under both sanitizers, built with clang. `-fno-sanitize-recover=undefined` is on, so a UBSan diagnostic fails the run rather than printing into a log that passes |
| `Fuzz (libFuzzer, seeded regression)` | the two decoders that read bytes off the air, over their seed corpus plus a fixed number of mutations from a fixed seed. Deterministic on purpose - see below |

The cross job is the one that was missing longest. The device build lived only in
`semantic-release.yml`, which fires on a push to `main` - so a musl/aarch64 break was green in
review and red at release time, on a protected branch. Adding it found such a break immediately:
`docker/setup-cross.sh` exposed the Bootlin tools as symlinks under a shorter name, and those
tools exec `<the name they were invoked by>.br_real`, so on an x86-64 host the toolchain had
never worked at all. It goes unnoticed on an Apple Silicon Mac because Docker runs the arm64
branch of that script, which builds against the system musl instead.

Nothing gates on formatting, deliberately: the tree is normalised with clang-format 18 and host
versions vary.

**The cross job compiles the same code with a different compiler**, and that turns out to be a
second thing it checks. The device build is GCC 14.3 (musl) at `-Os`, where inlining gives the
optimiser bounds the host build never derives, so it reports `-Wformat-truncation` on calls the
host toolchain passes without comment. "Zero compiler warnings" therefore means *both* builds,
and the four sites the job found the day it landed are recorded in the commit that fixed them:
three `snprintf` calls whose destination the compiler could not prove was large enough, and one
whose *source* it could not prove was NUL-terminated.

The framebuffer renderer is covered too, in `tests/suites/ui_capture.c`. There is no `/dev/fb0`
in CI or in the dev container, and `mesh_ui_capture_*` (`src/ui/backends/fb_capture.c`) is the
only way the fb backend's output is exercised anywhere but on a Brick. Those cases check the
contract the encoders rely on — geometry, pixel order, that two screens do not render
identically — rather than pinning pixels, which would fail on every legitimate UI change.

## Fuzzing

Two harnesses in `devtools/fuzz/`, over the two places bytes we did not write enter the client:

| Target | Entry point | What it is looking at |
|---|---|---|
| `stream_framing` | `mesh_stream_parser_push()` | the serial link's parser, where the radio interleaves its own text log with framed protobufs on one port, so resyncing past junk is most of the job |
| `session` | `mesh_session_handle_from_radio()` | one FromRadio, however it travelled. nanopb guards its own bounds; what is worth fuzzing is what the session does with the fields afterwards |

```bash
make fuzz                       # the deterministic pass CI runs: seeds, then 20k fixed-seed runs
make fuzz ARGS="--time 600"     # an actual hunt, ten minutes per target
make docker-fuzz                # the same on macOS
./build/fuzz/debug/devtools/meshclient_fuzz_session build/fuzz/findings/crash-<hash>
```

**Memory safety is not the only oracle, and for the session it is not even the main one.** The
counted arrays a decoded packet lands in — the 256-node roster, the 8-slot channel table, the
message ring, a traceroute's hops — live *inside* `struct mesh_session`, which is precisely the
case a sanitizer cannot see: a write one slot past `nodes[255]` lands on the next field of a
struct the allocator handed out whole, so nothing faults and nothing is poisoned. The harness
therefore checks those counts by hand after every input. The framing harness checks two exact
identities instead: every byte pushed is accounted for once (delivered, in a frame header,
dropped, or still buffered), and every byte handed to the text callback is a byte counted as
dropped. A parser can be perfectly memory-safe and still lose a message.

**The seed corpus is generated, not committed.** `meshclient_fuzz_seeds` writes one real message
per FromRadio variant the client acts on, encoded with the same nanopb encoders the client
decodes with, plus framing seeds built from those — a whole frame, the same frame arriving a
byte at a time, a log line then a frame, a false start inside a log line, a header whose payload
never arrives. Generating them means a protobuf regeneration that changes a field number changes
the seeds with it; a corpus of blobs in git would quietly stop decoding and quietly stop seeding
anything. It also means findings are the only binary artifacts, and those are reproducers.

**CI runs the deterministic half only.** Every seed once, then a fixed number of mutations from
a fixed seed: red there is a bug in the diff rather than a fuzzer that happened to get lucky on
somebody's pull request, which is the failure mode that teaches a team to ignore a job. The
open-ended hunt is `--time`, run by hand when the parser or the decode paths change.

The harnesses need clang (libFuzzer is a clang runtime) and Ubuntu's `libclang-rt-18-dev`, which
the `clang` package does not pull in. `scripts/fuzz.sh` builds with ASan and UBSan alongside,
because a fuzzer without a sanitizer only reports the crashes bad enough to fault on their own.

## Adding a test

1. Open the `tests/suites/` file for the area, or add a new one and list it in
   `MESHCLIENT_TEST_SUITES` in `tests/CMakeLists.txt`.
2. Write the case with `MESH_TEST_CASE(name, category)`. That is the whole registration — the
   macro defines the function and hooks it into the runner, and `test_name` is already in scope.
3. End every path with `record_success(test_name)` or a failure, so the case reaches the summary.
   `MESH_TEST_FAIL_IF` is the short form; `MESH_TEST_FAIL_IF_CLEANUP` is for a case holding a
   loop, a mock or an fd it has to release first.
4. If it is not part of the default unit suite, add an `add_test` stanza in
   `tests/CMakeLists.txt` and label it.

```c
MESH_TEST_CASE(config_defaults, unit) {
    struct mesh_app_config config = mesh_app_config_default();
    MESH_TEST_FAIL_IF(config.idle_timeout_ms != 1000, "idle timeout should default to 1000 ms");
    record_success(test_name);
}
```

Cases are found through constructors, which is why the suite files are compiled straight into the
executable rather than through a static library — a linker may drop library members nothing
references, and every case inside them would go quiet. `tests/CMakeLists.txt` says so too.

## Rules

- **Never touch real BlueZ.** There is none in CI. Use `mesh_bluez_client_mock_enable` to script
  results and capture writes; `mesh_serial_usb_mock_enable` does the same for sysfs and usbfs,
  and its `open_fd` lets a test hand the link one end of a socketpair.
- **Prefer deterministic fixtures over live radio calls.**
- **Cover the error paths** — BLE reconnection and protobuf parsing especially — before calling a
  feature done.
- **A new transport needs a golden protobuf frame test.** `message_encode_text_golden` is the
  model: it pins the `TEXT_MESSAGE_APP` wire format against a hand-derived byte vector rather
  than against our own encoder, so a protobuf regeneration that changes field numbers or wire
  types fails loudly instead of silently agreeing with itself.

## Isolated D-Bus reads

When `dbus-run-session` is available, `meshclient_bluez_bus` starts a private bus and fake
GATT service. It verifies actual `ReadValue` marshalling, socket watches, UI-event dispatch
while a reply is withheld, malformed responses, and timeout/late-reply behavior. It uses the
mock configuration's explicit test-bus address and never contacts the system bus or real
BlueZ. Install `dbus-daemon` to run it; the dev container and CI include it.

See [performance.md](performance.md) for the renderer benchmark and cache validation.
