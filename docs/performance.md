# Client performance

Two things are measured here: a CPU workload on a host, and how long a press takes to reach the
panel on a real Brick. Neither is a device frame rate or a battery-life measurement.

## The host benchmark

```sh
./scripts/docker.sh make release
./scripts/docker.sh build/linux/release/devtools/meshclient_perf
```

It draws 18 rows of repeated text at 1024×768, compares the rasterizer with its glyph cache cold
and warm, and checks the pixels match. One ARM64 Docker run measured **1.972 ms/frame uncached
and 0.776 ms/frame cached (2.54×)**. Navigating a 64-message transcript was 1.11× over a
reference with transcript caching and partial drawing disabled; animating a snackbar over it was
1.81×.

Use a separate build root if the release tree holds a cross-compiler cache:

```sh
./scripts/docker.sh make release BUILD_ROOT=build/perf-native
```

The caches are held to byte-for-byte equality with a full render: 784 frames across 27 capture
scenes matched with a fixed wall clock. `meshclient_uicap --reference` disables the transcript
cache and partial redraws for that comparison — **pin the wall clock for both runs**, or message
times change even when the renderer is identical.

## A press to the panel, on the device

`--trace-latency` (or `MESHCLIENT_LATENCY_TRACE=1`) switches on the probe in
[`src/ui/latency.c`](../src/ui/latency.c), off otherwise. Six readings, printed as percentiles on
exit: `press` (the kernel's evdev timestamp to the end of the `present()` that answered it),
`frame`, `draw`, `flip`, `read` (a tile off the card) and `decode`.

Three things about it are decisions rather than details:

- **It starts at the kernel, not at the read.** A loop busy decoding a tile does not wake for the
  event at all, and that wait is the whole of what an integrated number adds to a standalone one.
  The reader asks evdev for `CLOCK_MONOTONIC` stamps (`EVIOCSCLOCKID`); a device that refuses is
  not counted rather than counted wrongly.
- **A key repeat is not a press.** `src/ui/input.c` generates repeat off its own timerfd, so a
  held direction has no evdev event behind it. Counted from "now" it would report zero queueing
  delay on exactly the presses a held pan is made of.
- **It keeps histograms, not samples.** A fill frame is twenty times as common as a press, so a
  ring sized for one is a window on the other. The price is resolution — 50 µs below 12.8 ms,
  1 ms above — and the report says so rather than printing a bucket edge as a measurement.

`devtools/input_inject` is what makes a run repeatable: a uinput pad the client cannot tell from
the plastic one, plus a script of presses.

```sh
./scripts/docker.sh --cross devtools/input_inject/build.sh
devtools/input_inject/run-device.sh --every 600 --cold --tag map r1 a wait:6 x:3 right:20 down:10
```

## What a deep transcript costs

An open conversation is drawn from `struct mesh_ui_thread` — `MESH_UI_MAX_THREAD_MESSAGES` (256)
× `sizeof(struct mesh_ui_message)` (296 bytes), so about 76 KB — and that record is held three
times over: in the store, in the snapshot the backend is handed, and on the stack of the publish
that fills it. The framebuffer's transcript cache holds two formatted variants of each row and
scales with the same number, which puts it around 250 KB on the heap. Roughly half a megabyte in
total on a device with 1 GB and no swap.

Two things keep that off the hot path:

- **The window is only refilled when it has to be.** `mesh_app_publish_thread()` returns early
  unless the reader has moved to a different conversation or a message has changed. An ordinary
  publish — a node reporting, a position arriving, a tick — does nothing at all.
- **The transcript cache is keyed on the filtered run**, not on the whole message log. A message
  arriving on a channel nobody is looking at no longer invalidates the open thread, which it did
  when the key was `struct mesh_ui_message_list`.

The card is read once per conversation opened, never per frame: `mesh_ui_archive_load_thread()`
is one forward pass over a capped file, and everything after it folds the live log into what is
already in RAM.

## What the client does about it

The shape rather than the list: publication compares sources exactly and skips unchanged roster
ranking, message formatting and settings conversion; roster, message and read-marker saves share
a fixed two-second window; the transcript keeps a bounded cache of formatted rows and measured
heights; glyphs are cached as scaled coverage maps and tinted at draw time, so a colour change
does not regenerate them; and the device renders into ordinary RAM, compares against the previous
frame, and copies only changed row spans to page 0 and its page 1 mirror. Every one of those has
a fallback to the uncached path on allocation failure.

Still to check on hardware: a large NodeDB sync while navigating, a disconnect/reconnect, and
both framebuffer pages on a Brick.
