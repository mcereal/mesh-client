# Testing

The whole suite is **one binary with a name filter**, not per-test CTest entries. Every case
lives in `tests/test_main.c` and is registered in the `k_test_cases` table with a category tag.

As of 2026-09-05: **94 unit tests, all passing**, zero compiler warnings.

## Categories

| Category | Scope |
|---|---|
| `unit` | Everything today: transports, event-loop helpers, session and message handling, UI store/controller/nav, preferences, MinUI JSON glue |
| `integration` | Reserved for cross-module tests (BlueZ-on-device, MinUI end-to-end) |
| `hardware` | Reserved for tests needing a real Brick; tagged so CI can skip them (`ctest -E HARDWARE`) |

## Running

```bash
make test                                                    # debug build + ctest
ctest -L unit                                                # the same suite directly

./build/debug/tests/meshclient_core_tests --list             # names and categories
./build/debug/tests/meshclient_core_tests --filter ble_transport
./build/debug/tests/meshclient_core_tests --category unit
```

The driver prints a `[RUN]` line per case and a pass/fail summary; a non-zero failure count is a
non-zero exit code.

## Adding a test

1. Implement it in `tests/test_main.c` and register it in `k_test_cases` with the right category
   tag.
2. Use the `record_failure` / `record_success` helpers so failures reach the summary.
3. If it is not part of the default unit suite, add an `add_test` stanza in
   `tests/CMakeLists.txt` and label it.

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
