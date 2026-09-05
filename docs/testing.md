# Testing

The whole suite is **one binary with a name filter**, not per-test CTest entries. Cases live in
`tests/suites/<area>.c`, one file per subject area, and **register themselves** — there is no
central table to keep in sync.

As of 2026-09-05: **98 unit tests, all passing**, zero compiler warnings.

## Layout

| Path | What lives there |
|---|---|
| `tests/framework/` | `MESH_TEST_CASE`, the guard macros, the registry and `main` |
| `tests/support/` | fixtures shared by more than one suite, prefixed `mesh_test_` |
| `tests/suites/` | the cases themselves, one file per area |

A helper used by a single suite stays `static` in that suite. It only moves to `support/` once a
second suite needs it — that is the whole rule.

## Categories

| Category | Scope |
|---|---|
| `unit` | Everything today: transports, event-loop helpers, session and message handling, UI store/controller/nav, preferences, the updater |
| `integration` | Reserved for cross-module tests (BlueZ-on-device, end-to-end) |
| `hardware` | Reserved for tests needing a real Brick; tagged so CI can skip them (`ctest -E HARDWARE`) |

## Running

```bash
make test                                                    # debug build + ctest
ctest -L unit                                                # the same suite directly

./build/debug/tests/meshclient_core_tests --list             # names, categories and suites
./build/debug/tests/meshclient_core_tests --filter ble_transport
./build/debug/tests/meshclient_core_tests --category unit
./build/debug/tests/meshclient_core_tests --suite ui_nav      # everything from ui_nav_*.c
```

The driver prints a `[RUN]` line per case and a pass/fail summary; a non-zero failure count is a
non-zero exit code.

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
