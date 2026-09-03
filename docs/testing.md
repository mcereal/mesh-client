# Testing Strategy

The project currently groups tests into categories so we can tell at a glance
what is covered and run focused suites when iterating locally or on CI.

## Test Categories

| Category     | Scope                                                                           |
|--------------|----------------------------------------------------------------------------------|
| `unit`       | Pure C units compiled into `meshclient_core_tests` that exercise transports,
|              | event-loop helpers, UI store/controller, preferences, and MinUI JSON glue.      |
| `integration`| Reserved for cross-module/system tests (e.g., BlueZ-on-device, MinUI E2E).       |
| `hardware`   | Placeholder for tests that require TrimUI hardware; these will be tagged so CI   |
|              | can skip them via `ctest -E HARDWARE`.                                           |

All currently implemented scenarios land in the `unit` bucket. As we grow the
suite we can add new categories without changing the existing harness.

## Running Tests

```bash
# Default: run the unit suite (same as `make test`)
ctest -L unit

# List available test cases with their categories
./build/debug/tests/meshclient_core_tests --list

# Run a single category (e.g., future integration tests)
./build/debug/tests/meshclient_core_tests --category integration

# Filter by test name substring
./build/debug/tests/meshclient_core_tests --filter ble_transport
```

The test driver now prints a concise `[RUN]` line for each case, followed by a
summary table with pass/fail counts. Non-zero failures cause a non-zero exit
code for CI compatibility.

## Adding New Tests

1. Implement the test in `tests/test_main.c` (or a new file) and register it in
   the `k_test_cases` table with the correct category tag.
2. Use `record_failure` / `record_success` helpers so failures surface in the
   summary output.
3. If the test is not part of the default unit suite, add a new `add_test`
   stanza in `tests/CMakeLists.txt` and label it appropriately.

## Coverage & Next Steps

- **Code coverage:** we plan to add a `make coverage` helper that wraps `gcovr`
  once more modules land; the new test harness already supports filtering so we
  can scope coverage reports by category.
- **Integration hooks:** as BLE/MinUI end-to-end flows come online, mark them as
  `integration` or `hardware` to keep CI fast while still documenting coverage.
- **CI reporting:** use `ctest --output-junit` in automation to feed results into
  dashboards; the category labels map cleanly to CI job names.

Refer back to this document when adding transports or UI flows so we keep the
suite balanced across layers.
