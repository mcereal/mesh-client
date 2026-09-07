# Client performance

The client keeps UI input responsive during a NodeDB sync and avoids repeating work when the
radio data or rendered pixels have not changed.

- **BLE reads:** `mesh_bluez_client_read()` returns `-EAGAIN` while a `ReadValue` request is
  pending. A reply or the three-second timer wakes the drain through its eventfd. Only one
  read is outstanding; packets retain FIFO order, failures retain exponential backoff, and
  disconnect cancels the read. Late replies cannot complete a newer request. Other BlueZ
  operations, including writes and property queries, still use synchronous calls.
- **Publication:** exact source comparisons skip unchanged roster ranking, message formatting
  and merging, and radio-settings conversion. Message changes, renamed nodes, preferences,
  locale, restored history and roster ownership invalidate the relevant cached view. Dynamic
  status, notices, timers and client settings are still published. This uses comparisons rather
  than revision counters so existing in-place mutation paths remain valid.
- **Animation:** the controller retains its latest snapshot. Timer frames consume any pending
  real update, then reuse that snapshot without requesting a synthetic full-store refresh.
- **Text:** a four-way cache retains up to 256 scaled glyph coverage maps. Its key is the
  immutable font descriptor, codepoint and scale; coverage is tinted at draw time, so changing
  colors does not require regenerating it. Oversized glyphs and allocation failure use the
  uncached renderer. Storage is approximately 518 KiB per renderer.
- **Display writes:** the device renders into ordinary RAM, compares against the previous RAM
  frame, and copies changed row spans to page 0 and its page 1 mirror. The first frame fills
  both pages. This preserves the launcher workaround without reading display memory. Two
  1024×768×4 buffers cost 6 MiB; allocation failure keeps the original direct-render path.
  Screens still render fully in RAM; widget-level damage tracking is a possible later step.

## Measurement

Run the text workload in an optimized Linux build (use Docker on macOS):

```sh
./scripts/docker.sh make release
./scripts/docker.sh build/linux/release/devtools/meshclient_perf
```

The benchmark draws 18 rows of repeated text at 1024×768, compares the same rasterizer with
its glyph cache disabled and warm, and checks that the resulting pixels match. One ARM64
Docker run measured **1.972 ms/frame uncached and 0.776 ms/frame cached (2.54×)**. This is a
CPU workload measurement, not a device frame-rate or battery-life measurement.

All **638 frames across 24 existing capture scenes** matched the pre-change renderer at
`b71c09f` byte-for-byte when `time()` was fixed for both builds. Fixing the wall clock matters:
message times otherwise change even when the renderer is identical. The 34-frame toggle
scene had a median of 33,704 changed bytes per subsequent frame across both display pages,
calculated from its pixel differences. A full two-page transfer is 6,291,456 bytes.

![Settings switch animation](assets/performance-toggle.gif)

Regenerate the clip with:

```sh
make docker-ui-capture ARGS="devtools/ui_capture/scenes/toggle.scene -o docs/assets/performance-toggle.gif"
```

## Validation and remaining device checks

`make docker-test` exercises the store, cached publication, glyph rendering, row-span copies,
transport retry paths and controller frame timers. When `dbus-run-session` is installed, it
also launches an isolated fake GATT service to verify real D-Bus marshalling, queued writes,
input handling while a reply is withheld, malformed replies, timeout and late-reply handling.
It never contacts real BlueZ. The dev image and CI install `dbus-daemon` for this test.

The cache and async-read tests also run under AddressSanitizer and UndefinedBehaviorSanitizer.
Hardware validation still needs a large NodeDB sync while navigating, a disconnect/reconnect,
and checking both framebuffer pages on a Brick. Full screen rendering and synchronous BlueZ
operations are the next places to measure if device traces show remaining latency.
