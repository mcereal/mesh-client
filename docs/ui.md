# UI layer

The UI is deliberately thin and data-driven: one store holds the state, one navigation model
turns key presses into actions, and backends only draw. Nothing in `src/ui/nav*.c` touches an fd
or a device, so it is testable directly.

## Shape

`src/ui/` is filed by group - `store/`, `nav/`, `settings/`, `tables/`, `theme/`, `views/`,
`input/`, `backends/`, `generated/` - with `layout.c` and `anim.c` at the top because every group
uses them. The group is where a source lives and never part of an include path: the header stays
at `mesh/ui/<name>.h` whichever group its source sits in. See the group map in `CLAUDE.md`.

```
mesh_app -> mesh_ui_store (snapshot + eventfd) -> mesh_ui_controller -> backend->present()
evdev -> inkcell_input -> mesh_ui_controller_handle_key -> mesh_ui_store_handle_key
      -> nav.c -> mesh_ui_action -> mesh_app_on_ui_action
```

- **`src/ui/store/store.c`** owns `mesh_ui_snapshot` and signals the loop via an eventfd.
- **`src/ui/nav/controller.c`** is what a press means: keys, commands and clicks, resolved
  against the last snapshot. Presenting is inkstand's frame scheduler
  (`inkstand/nav/frame_scheduler.h`), which drains the store, calls `backend->present(snapshot)`
  and keeps a frame timer armed only while the backend reports it is still animating.
- **Backends** implement the three-function `struct inkcell_backend` (`init`, `shutdown`,
  `present`): `fb.c` (the device UI), `cli.c` (a terminal fallback), `stub.c` (tests).
  **Backends are stateless** — they draw the cursor from `snapshot->nav`. A new platform
  implements the backend interface and leaves the store and controller untouched.
- **inkcell's `src/layout.c`** holds the backend-agnostic primitives: `struct inkcell_line` (a string
  builder that only ever measures in drawn cells), `struct inkcell_list` (the
  cursor-clamp-and-scroll-window arithmetic, measured in **steps** rather than items),
  `struct inkcell_wrap` (cell-measured word wrap) and `inkcell_transcript_window` (the
  bottom-anchored window variable-height items need). None touches a framebuffer, a font or a
  snapshot, so all are unit tested directly in `tests/suites/ui_layout.c`.
- **inkcell's `src/theme/theme.c`** and **`src/theme/font.c`** hold what the UI *looks* like. Nothing that draws
  holds a colour or a margin of its own — see [Themes](#themes).
- **`src/ui/nav/`** owns the tab/cursor/compose-target model (`struct mesh_ui_nav`, carried in
  every snapshot and clamped against the lists on each consume) and return a `mesh_ui_action`.
  `nav.c` is the router; `nav_canned.c`, `nav_keyboard.c`, `nav_conversations.c` and
  `nav_settings.c` are the subjects it dispatches into, over `nav_internal.h`.

The UI-side structs (`mesh_ui_node_summary`, `mesh_ui_settings`, `mesh_ui_client_info`) are
nanopb-free twins of the core records, filled field by field in `app.c`. Nothing else keeps the
two declarations in step, so **adding a field means touching both**.

They are declared by subject rather than all in `store.h`: `store_device.h` a discovered radio,
`store_node.h` a node, `store_channel.h` a channel slot, `store_handshake.h` the roster,
`store_message.h` the transcript and waypoint book, `store_mqtt.h` the broker connection held on
a radio's behalf, `store_settings.h` what Settings reads. `store.h` is the store itself and
includes all seven.

**Naming the narrow header decouples a reader, not a writer.** `mesh_ui_snapshot` embeds every
record by value, so anything holding a snapshot rebuilds when any one changes. What the split
buys is the other kind of reader — `trust.h` wants a node, `devices.h` wants a row — and a
1,700-line file no longer being where six unrelated subjects are edited.

## What the client remembers

There are three files on the card and they answer different questions.

| | `…prefs.handshake` | `…prefs.messages/` | `…prefs.trends/` |
|---|---|---|---|
| What | the roster, channels, read marks, airtime trend, measured routes, and the newest 64 messages | one append-only log per conversation | one append-only log per node |
| Shape | one file, rewritten whole every save through a temporary and renamed over | a file per conversation, appended to | a file per node, appended to |
| Keys | `include/mesh/ui/store_keys.def` | the same message records, over `store_internal.h` | one `trend` record, off the same table |
| Read | at launch, all of it | when a conversation is opened, one file | when a node's detail is opened, one file |
| Code | `src/ui/store/store_file.c` | `src/ui/store/store_archive.c` | `src/ui/store/store_trends.c` |

The cache is what makes a Brick with no radio in range open on a roster. All three are written
through a temporary and renamed into place, which for the cache matters more than its "rebuilt
on the next publish" shape suggests: **the roster is the one section nothing can rebuild.** It
is the record of the nodes the *radio* no longer carries - a NodeDB that evicted them, or one a
factory reset emptied - and `mesh_app_seed_nodes_from_cache()` hands it back to the session at
launch. A save cut off midway used to leave an empty file, which reads back as no roster at all.

The archive is what makes a conversation go back further than the radio does, and the two
numbers behind that are worth stating plainly:

- **`MESH_UI_MAX_MESSAGES` (64) is the transport ring**, shared by every conversation at once.
  The conversation list and the all-traffic screen are derived from that one flat list, and it
  is sized for what the radio is still holding.
- **`MESH_UI_ARCHIVE_MAX_MESSAGES` (512) is per conversation.** A channel that fills its own log
  has taken nothing from anybody else's, which is the whole complaint the archive answers: on a
  busy mesh a single chatty channel used to spend all 64 slots inside an hour and evict the
  direct exchange the reader actually cared about.

An open thread is drawn from `struct mesh_ui_thread` — `MESH_UI_MAX_THREAD_MESSAGES` (256) read
off the card when the conversation is opened, with the live log folded in on every publish after
that. Every screen asks `mesh_ui_store_message_view()` (or `mesh_ui_snapshot_message_view()`)
rather than reaching for either list: it answers with the window only when the window is over
the conversation the nav has open, and with the flat list for the conversation list, the
all-traffic view, and the one frame between a thread opening and the window being filled.

Three rules the archive turns on, each stated at length in `include/mesh/ui/store_archive.h`:

- **The row index groups a record, it does not number one.** A record is five lines sharing an
  `[i]`, and `i` restarts at zero every run — knowing the file's real record count would mean
  reading the whole file before the first append. The reader's rule is "a `msg[]` line always
  begins a record".
- **Only traffic from the transport ring is appended.** A message restored from the handshake
  cache was archived by the run that heard it; appending it again would grow every file by the
  whole of itself once per launch. `mesh_ui_archive_seed()` is the exception and runs once per
  conversation, for a card upgrading from a build that had no archive.
- **A file is capped by rewriting it.** Compaction fires on append, off one `stat()`, and the
  threshold is above what `MESH_UI_ARCHIVE_MAX_MESSAGES` records can possibly occupy — a cap the
  worst case could exceed would rewrite the transcript on every message, on a card mounted
  `sync`. A *delete* is the other way round: it streams the file through a temporary, holding
  only the one line whose verdict is unsettled, because a file ordinarily holds thousands of
  records and rebuilding it from a bounded read would throw away everything that did not fit.

**A packet id is not an identity.** `MeshPacket.id` only has to be unique per sender for a few
minutes (`mesh_session_next_packet_id`), and a channel's file holds every sender on it — so two
nodes can land on the same id. Anything that asks "is this the same message" keys on
`(packet_id, peer, direction)`: the archive's dedup, the reader's fold, and
`mesh_ui_thread_merge()`. Anything that asks "is this the message the user pressed on" also
takes the conversation, which is why `mesh_ui_store_forget_message()` and
`mesh_message_log_forget_message()` do.

**What changes about a message is its delivery state**, and that is the one thing the archive
re-writes: an outbound message goes to the card pending and is appended again when its ack
arrives, with the reader folding the later record onto the earlier one. Without that, a message
that had failed would read as still in flight after a restart — and the transcript offers a
resend on nothing but a `FAILED` one. (The Routing error *behind* a failure is not persisted by
either format; a restored failure reads with the generic word, as it always has.)

### The trend log

`struct mesh_ui_history` is what the client has *watched* — the sparkline on a node detail row
and the chart behind it — and until the log existed the whole of it died at exit except the
radio's own airtime pair, which rides the cache because that file is rewritten whole anyway and
six hours of one-minute readings fits in it. Twelve nodes' worth of half-hourly readings does
not, and rewriting them every time a read mark moved would be the wrong shape twice over.

So the log is the archive's shape applied to a different record, and everything interesting
about it follows from one fact: **a reading's time is not a time.** Every stamp in the history
is the client's own monotonic clock, which counts from boot, and a Brick has no RTC to write
down instead. The cache gets away with it by writing *ages* relative to its newest sample — it
can, because it rewrites the file. An append cannot, so a record carries the time since the
record *above* it and the reader adds them up:

- **A run's first record carries the seam rather than a measurement.** How long the client was
  not running is the one thing nothing on the device can measure, so it writes
  `MESH_UI_HISTORY_NODE_GAP_MS` — the shortest silence that is already a break — and marks the
  record as continuing nothing. That is `mesh_ui_history_resume()`'s rule for the radio's pair,
  written into the format instead of into a call.
- **Every reading of one node shares one chain**, so a battery and the temperature beside it come
  back on one timeline. The SNR and RSSI rows need that: they are two measurements of one packet
  and are drawn against one axis.
- **The reader bounds the span**, because deltas accumulate and a `uint32` of milliseconds is
  seven weeks. It keeps the newest `MESH_UI_TRENDS_MAX_RECORDS` and drops whatever is more than
  `MESH_UI_TRENDS_SPAN_MAX_MS` behind the last of them — which costs nothing real, since a series
  holds two dozen samples and a week of them is a reading every seven hours.

The read **replaces** what the history holds for that node rather than folding into it, and that
is what `mesh_ui_trends_append()` running on every publish makes safe: the log already holds this
session's readings, so there is nothing in the slot the card does not have. It is also why the
history's clock starts at `MESH_UI_HISTORY_EPOCH_MS` rather than at zero — a restore places saved
readings *behind* the live stamp, and a live stamp thirty seconds into a run has nowhere to put a
day of them.

A different radio takes the whole directory with it. Node numbers are the mesh's rather than the
radio's, so a trend carried across a swap would draw one node's battery as another's — the same
reason `mesh_ui_history_forget()` exists, noticed here off the roster owner and seeded from the
cache at startup so that a *relaunch* against a different radio is the same event as a swap.

Deleting has to reach every copy or the next publish undoes it: the transport's ring, the
history the app restored at startup, the store (both lists), and the card. `on_delete_message()`
and `on_delete_conversation()` in `src/app/app_actions.c` are where that is spelled out, which
is why both are app actions rather than something the store does on a key press.

### The traceroute log

A trace is a press somebody made deliberately and then waited up to a minute for, and until the
log existed the client kept exactly one: tracing a second node erased the first one's path, and
a restart erased both. `struct mesh_ui_traceroute_log` holds the last route measured to each of
`MESH_UI_TRACEROUTE_LOG_MAX` (8) nodes, and every screen asks
`mesh_ui_store_traceroute_view()` for the node it is drawing rather than reaching for the store's
own slot — which is the same rule the transcript's window follows one section up, for the same
reason: the one slot answers a different question, and reading it directly is how a second node's
detail came to describe itself with the first node's trace.

- **The slot is the trace, the log is the route.** Only a finished trace is recorded, so an entry
  is always a measured path; the slot is what says *running* or *timed out*, and the view hands
  that back while the trace is this node's. Re-tracing a node replaces that node's entry — there
  is only ever one current route to a node — and the cap evicts the least recently traced.
- **It rides the cache rather than a file of its own**, and the arithmetic is why: eight routes of
  at most ten stops is a few hundred lines in the worst case and a dozen in the ordinary one,
  against a roster of 128 nodes the same file already rewrites on every save. The keys are
  `trace[i]`, `trace_hop[i.n]` and `trace_name[i.n]`; a hop's name is written rather than
  re-resolved on load, for the reason `msg_relay[]` is.
- **A route's stamp is the radio's wall clock**, not ours, so unlike the trend log there is
  nothing to convert on the way out — the number still means what it meant, and the group closes
  with how old it is. A route is true for about as long as the mesh holds still, which is an
  argument for saying when it was measured rather than for throwing it away: the same terms a
  position fix is kept on.
- **Every path starts at us**, so a radio swap drops the log and the slot with it, exactly as it
  drops the trends above.

## Input

inkcell's `src/input/input.c` reads every `/dev/input/event*` and maps evdev codes to `enum inkcell_key`.
Quit keys stop the loop before mapping.

**The Brick's face buttons do not report by position.** A is `BTN_EAST` (305), B is `BTN_SOUTH`
(304), and the button printed **Y, on the left, is `BTN_NORTH` (307)**, so X on the top is
`BTN_WEST` (308). Reading them positionally leaves Y unreachable and silently fires X in its
place — which cost a round of "the save does nothing" debugging, since Y saves a settings section
and X refreshes it. `input_brick_face_buttons` pins all four. **Do not "fix" any of it back.**
See [`device.md`](device.md#the-buttons).

**L2 and R2 are absolute axes, not buttons.** The pad declares no `BTN_TL2`, so the two triggers
on the case went unread until the keyboard wanted a shift key. `inkcell_input_map_trigger()` reads
`ABS_Z`/`ABS_RZ` and a latch in `inkcell_input_handle_device_event()` turns a squeeze into one
press — on this hardware a trigger is digital (255 down, 0 up), but the axis is an axis, and a pad
that reported the way up as a ramp would otherwise be a press per value.
`input_triggers_are_axes_and_press_on_the_edge`.

**Key repeat is ours, not the kernel's.** Autorepeat is an EV_KEY/EV_REP feature and the d-pad
arrives as the absolute axes `ABS_HAT0X/Y`, which send one event out of centre and one back
however long they are held — so a 60-node roster used to cost 60 presses. `input.c` runs its own
repeat off a timerfd: hold for 350 ms, then a row every 90 ms, halving after eight rows. Only the
four directions repeat; a held A that confirmed forty times would be a trap.

A direction is driven by our timer or by nothing at all — a kernel `value == 2` is always
dropped, so a USB keyboard cannot take two rows per step. A hold ends when any other key is
pressed and when the device it started on goes away: an unplugged keyboard hangs up its fd
instead of sending the key up, and a repeat with no release would scroll forever. Repeats reach
the store one per event-loop turn and the store coalesces its repaints.

### The on-screen keyboard

**The grid is inkcell's and the jobs are this client's**, and that is the whole of how to read
this section. `inkcell/ui/keyboard.h` is the model - where the cursor is, which panel is showing,
what a press does to a buffer - and `inkcell_fb_draw_keyboard()` is the drawing. None of it was
ever about Meshtastic, and the second client built on the toolkit started by copying it.

What stayed here is `src/ui/nav/nav_keyboard.c`: the seven unrelated jobs one grid is opened for
(a message, a settings field, a waypoint's name, a network address, a channel link, a contact
link, and the PIN and security-number prompts that can arrive on top of any of them), what the
text is worth when it is finished, and `mesh_ui_nav_keyboard_close()`, which is where "give the
user back what they were doing" lives.

The seam is two functions. `mesh_ui_nav_kb_layout()` builds the `struct
inkcell_keyboard_layout` for whichever job is open - this client's emoji pages, the word on the
submit key, and the cap `mesh_ui_nav_draft_cap()` holds the append to - and
`mesh_ui_nav_kb_submit_finishes()` is the one predicate behind that word and the symbol over it.
One predicate rather than two copies of a list, because the two copies disagreed: the word said
"done" on the key-verification prompt and the symbol was a send arrow, over six digits the whole
ceremony depends on never reaching the mesh.

**The pad is used the way a console keyboard uses it.** A types, **X** is the backspace, **B**
leaves, **Y** is a space, START sends or finishes. That is not a preference: B goes back on every
other screen in the client, and a keyboard whose backspace is that button is one people stumble
over on every draft rather than once. B keeps the draft - the grid's own ✕ key is what discards -
and on the two prompts a radio raised it stands the ceremony down, because leaving a question
somebody is waiting on *is* answering it. `inkcell_keyboard_key()` tells those two presses apart
as `DISMISS` and `CANCEL` so that this client does not have to work out which one it was.

**The layers are a ring of panels, not a layer with pages inside it.** `abc`, `ABC`, symbols,
then one page of forty emoji at a time. The grid's bottom-left key and `L1`/`R1` walk it and
nothing else does; `L2`/`R2` are the shift, which is one capital and then back. The ring, the
tables behind it and the invariants that hold them - every cell carries a key, every printable
ASCII character is reachable on some layer - are inkcell's now, and so are the tests that say so
(`tests/suites/ui_keyboard.c` in that tree).

The emoji pages are not: a set chosen for a radio on a hillside is the wrong set for a music
player, so the toolkit carries none and each application hands over its own. This client's are
`k_kb_emoji` in `nav_keyboard.c`, written as `\U` escapes so a patch tool cannot mangle them, and
every cell is asserted to be a single glyph this build has a sprite for. That check is
`inkcell_keyboard_layout_drawable()` - the glyph tables are inkcell's, so walking them from here
was a test reaching down a layer to read data it does not own - and `kb_emoji_cells_are_drawable`
is this client asking it of these pages.

**A keycap that is one emoji is drawn at the key's size, not at the text scale.** It went through
`inkcell_fb_draw_text()` at first, which sizes an emoji like the letter beside it - correct in a node's
name and wrong on a key five times that across, where forty 20 px thumbnails a panel could not be
told apart. The button's `emoji_face` is the rule: a label that is a single sprite and nothing
else becomes the key's face, centred and sized to the box. It is ignored over a letter, a word or
an icon, so the four layers are still one grid described once (`fb_emoji_keycap_fills_its_key`).

**The grid takes the body it is given.** Five rows at one text line each left a third of the panel
blank under keys a twentieth of it tall; they grow to fill the room between the draft and the
footer, capped at their own width - past square a keycap reads as a bar - and floored at what the
row cost before, so a small panel or a large glyph scale lays out exactly as it always did. The
keycap's text grows with the key, never below the body scale and never past twice it. The slack
the cap leaves over goes *above* the grid: a keyboard sits at the bottom of what it is given. All
of that arithmetic is `inkcell_fb_draw_keyboard()`, which is what this screen hands a row and a
cursor and gets a grid back for.

## The framebuffer backend

`src/ui/backends/` stacks, and calls only ever go downward:

| File | Layer | What belongs there |
|---|---|---|
| inkcell's `src/fb/fb_draw.c` | ink | pixels, glyphs, theme lookups, cell metrics |
| inkcell's `src/fb/widgets_*.c` | components | every component below (`inkcell/ui/widgets.h` is the umbrella header) |
| `fb_screens_*.c` | screens | one renderer per screen, one file each (see below) |
| inkcell's `src/fb/fb.c` | device | `/dev/fb0`, the page flip, the backend vtable |

The top and the bottom of that stack are inkcell's - a panel and a button were never about
Meshtastic. What is in `src/ui/backends/` is the screens layer, plus `fb_app.c`, the frame
inkcell calls up into, and `fb_map.c`.

### ...and the window behind it

Only the bottom row of that table is the framebuffer's. Everything above it draws into a
`struct inkcell_surface` - a pointer, a stride and a channel layout - and cannot tell what is
presenting the result, which is what lets `MESHCLIENT_UI_BACKEND=sdl` put the same frames in an
SDL window on a development host:

```bash
MESHCLIENT_UI_BACKEND=sdl ./build/debug/meshclient -f
```

It is the same rasteriser, the same components and the same screen renderers; what differs is
that changed rows become texture uploads instead of `memcpy`s into `/dev/fb0`. Drive it with the
arrows, Enter (A), Backspace (B), Space (Y), `x` (X), F1 (START), F2 (SELECT) and Escape to
quit - the same table a USB keyboard on the device goes through, so a keycap means here what it
means there. No pad: SDL's controller mapping and inkcell's device profile would be two answers
to the same question.

The primary desktop modifier is Command on macOS and Control on Linux/Windows. With it, N asks
for New, S for Save, and R for Refresh. Each shortcut runs only when the current screen offers
that command; for example, Save on a message list does nothing. Modified keys never also act as
Brick face buttons, so Control+X cannot accidentally invoke X's current-screen verb.

When the on-screen keyboard is visible, the window also accepts native text entry and paste
(Command/Control+V). SDL's committed UTF-8 goes into the same draft with the same byte limit as
the grid; a paste that does not fit is refused whole. Backspace deletes, Enter submits, and
Escape leaves with the draft intact. The grid remains available for pointer and controller use,
and typing is ignored while another sheet is on top of it.

The mouse works too, and adds no second model of the screen: it clicks the boxes the frame
already registers for the d-pad (`mesh/ui/focus.h`). A tab switches to it, a row is the cursor
on it and A, and a dialog's answer is answered - `src/ui/nav/nav_click.c`, which turns each
click into the presses it stands for so every guard a key meets, a click meets. Two exceptions:
a bubble in a thread is only selected, since A there writes a reply, and a click under a sheet
goes nowhere. The wheel is Up and Down, a hint in the action bar is its key, and the mouse's
back button is B (inkcell's `inkcell/ui/pointer.h`). `tests/suites/ui_click.c` clicks the
real frame.

A window puts the screen's verbs in its heading rather than as keycaps at the foot (inkcell's
`inkcell_draw_state.pointer`), and draws no foot at all. The verbs are the same command set the
keycaps are projected from, projected a second time into the app bar's actions
(`mesh_ui_actions_heading()` in `src/ui/tables/actions.c`): each is a symbol, the first verb that
makes, sends or keeps something is the tonal pill with its word beside it, and help is last. A
command with no symbol in that table stays off the heading, which is how whatever the pointer
already has somewhere better is left out - a row's own A (the row is clicked), the tabs, the
arrows (the wheel), quit (the close box), and Back (the heading's arrow). A verb is its command
(`MESH_UI_FOCUS_BAR + command`) and reaches `mesh_ui_controller_handle_click()`, which runs it
only if the screen offers it right now. Two places keep the bar for a pointer: Status, whose
cards are its heading, and any layer the heading does not speak for - a dialog, a sheet, help -
where it is the toolbar it always was. `pointer` in a capture scene draws the same frame.

On the device the foot is one row of keycaps. They are the only place a d-pad reader learns what
the buttons do, so they stay; the line that used to sit under them saying which radio was attached
is the heading's link mark now - the radio's name in the success tone, or what the transport is
doing, dimmed - and the body has the row back. A screen draws its heading through
`fb_draw_app_bar()`, which hands the first heading on a frame the mark and the verbs; Status,
which draws none, gets the mark at the end of its keycap row.

A right-click (or a control-click) on a row of the screen's list selects it and opens that
row's menu at the pointer: its row commands, read from the same command set the action bar is
projected from, so the menu offers exactly what the active context can do. A verb is its semantic
command (`MESH_UI_FOCUS_MENU + command`) and reaches
`mesh_ui_controller_handle_command()` without becoming a Brick button first; a key, or a click
anywhere off the menu, puts it down and does nothing else. `context row N` in a capture scene
opens one. Command identity does not reorder the rows: they retain the action bar's established
A, X, Y, Start sequence.

Two things to know before reaching for it:

- **It is a presenter, not a GPU renderer.** The glyphs and the anti-aliasing are still the
  CPU's work. What moves to the GPU is the blit and the scale.
- **The pak does not carry it.** The cross container has no aarch64 SDL2, so a device build
  reports the backend unavailable and falls back to `fb` - which is the right default there
  regardless, and is what has actually been measured. See [`performance.md`](performance.md).

On a Mac it runs natively: `make setup && make debug`, then the command above. The loop is kqueue
there, and there is no framebuffer and no evdev, so the window is the only way the UI is seen and
the keyboard and mouse the only way it is driven. A radio is reached over TCP (Settings, or
`MESHCLIENT_TCP_HOST`); BLE needs BlueZ and a USB radio needs Linux's sysfs to be found, so
neither is there. The updater finds no binary to replace, on purpose - every release asset is a
Linux binary.

The Mac window has no title bar of its own: the frame runs up under it, the close, minimise and
zoom buttons sit in the tab strip, and the strip drags the window everywhere but on a tab (`unified_titlebar` in
`src/app/app.c`, the rest in inkcell's `src/sdl/sdl_cocoa.m`). That costs the first tab a shift
to clear the buttons - `top_leading_inset`, which is 0 on the device and in a capture - so the
window is the Brick's layout everywhere but there. The window holds the panel's 4:3 as it is
resized, which is what keeps the strip at its top edge.

The frame is placed by inkcell's scaffold (`fb_render_snapshot()` in
`src/ui/backends/fb_screens_frame.c`), so a window wide enough to leave the compact width class
moves the tabs into a rail down the leading edge and gives the body the rest. On the Brick, and on
any window that is still compact, it is the tab strip across the top - kept there on purpose,
because L1 and R1 are on the top edge of the case - and the frame is the one it always was.

`make ui-capture` is still the way to *review* a UI change, because a picture in a pull request
is reviewable and a window on somebody's desk is not.

**A screen renderer should read as a description of its content** — what the list holds, what
each row says, which rows are actions. If it is computing a pixel coordinate, a scroll offset or
a padding width, that belongs in a component instead.

### One file per screen

`fb_screens.c` was 4891 lines, so a screen is a file:

| File | What is on it |
|---|---|
| `fb_screens_frame.c` | the chrome around every screen, and the branch that picks one |
| `fb_screens_messages.c` | the conversations, and one of them open as a transcript |
| `fb_screens_compose.c` | the four sheets that put a message together: reactions, compose, the picker, the keyboard |
| `fb_screens_nodes.c` | the roster, one node's detail, and that node's verbs over it |
| `fb_screens_waypoints.c` | the places, and one of them open |
| `fb_screens_devices.c` | every radio this client can see, and the row for typing an address |
| `fb_screens_status.c` | the link, the radio and the mesh, as three cards |
| `fb_screens_settings.c` | the sections, and one section's rows |
| `fb_screens_overlays.c` | help, a confirm, the key-verification sheet |
| `fb_screens_code.c` | the two QR sheets: this radio's channels, this radio's contact |
| `fb_screens_chart.c` | one chart, opened from the Status cards and from a node's detail |

`fb_map.c` is the twelfth and stands apart for a reason of its own — it is the one screen that
places things at coordinates rather than describing rows, so it is not held to the rule above.

The renderers the frame calls are declared in `fb_screens_internal.h`, along with the five places
one screen reaches another: what a device is called, the one-line quote of a message, the airtime
thresholds, a node's chart and the detail under it. Everything else in these files is `static`,
and the header is not part of `inkcell/ui/widgets.h` — nothing outside `fb_screens_*.c` includes it.

The list is the component that earns the most. Every screen is the same shape:

```c
struct inkcell_fb_list list = inkcell_fb_list_begin(layout, count, nav->cursor[MESH_UI_SCREEN_NODES]);
uint32_t i;
while (inkcell_fb_list_next(&list, &i)) {
    inkcell_line_reset(&line);
    inkcell_line_printf(&line, "%s", node_name(i));
    inkcell_line_right(&line, layout->cols, metrics);   /* right-aligned, in cells */
    inkcell_fb_list_row_line(state, &list, i, &line, FB_TONE_NORMAL);
}
```

`inkcell_fb_list_begin_rows()` is the variant for an item that spends more than one row;
`inkcell_fb_list_begin_visible()` for a screen that reserves body rows for something else.

### A row is however many steps the model says

`inkcell_fb_list_begin_heights()` takes one row count per item and is what a list of **mixed** heights
opens with. The screen measures and hands over the array; the window, the highlight and the
scroll thumb are three sums of the same heights.

`struct inkcell_list` used to count items and multiply — the first row on screen, the scroll
thumb and the highlight rect were all `index * line` — so a row that quietly grew a second tier
put those three in three different places. It now counts **steps**, and the arithmetic is in
`layout.c`, unit tested there (`layout_list_window_counts_steps`,
`layout_list_scroll_counts_steps`), because a second backend with a taller row would want the
same answer rather than a second derivation of it.

The cursor used to sit on the **bottom line** of the window, so a list that had outgrown its
panel slid by a row on every press of Down and the reader could never see what the next press
would land on. The window now ends a **look-ahead** past the cursor —
`MESH_UI_LIST_LOOKAHEAD`, three steps, trimmed to a third of the window.

### The components

One file per group, all taking a family/tone/shape rather than a colour. The headers are the
reference; what follows is the map.

| Group | What is in it |
|---|---|
| `inkcell/ui/widgets/button.h` | the button, the chip, a strip of chips, the badge — the shapes sized to their own label |
| `inkcell/ui/widgets/chrome.h` | the app bar and its trail, the navigation bar, the action bar, the banner, the progress bar, the empty state, the hairline |
| `inkcell/ui/widgets/list.h` | the list window, the card surfaces and rail under it, the subheader, the note row, the disc |
| `inkcell/ui/widgets/item.h` | one row and its slots, and the conversation cell |
| `inkcell/ui/widgets/bubble.h` | the transcript: a message, and the separator between two of them |
| `inkcell/ui/widgets/card.h` | a card, built row by row and then drawn |
| `inkcell/ui/widgets/control.h` | the switch, the checkbox and radio, the segmented button, the text field |
| `inkcell/ui/widgets/meter.h` | the meter, the slider, the signal staircase, the sparkline, the proportion bar, the chart |
| `inkcell/ui/widgets/overlay.h` | the dialog, the snackbar, the QR code |
| `inkcell/ui/widgets/scaffold.h` | where the chrome goes at each width class: the tab strip on the Brick, a rail on a wider window |

Calls between them run one way — `button` is the leaf everything else reaches for — so a group's
header names only the groups above it. The two exceptions to one-header-per-group are
`inkcell/ui/widgets.h`, which includes all of them for a caller that wants the lot, and inkcell's
own `list_internal.h`, the six answers the list window and the row it draws both need.


| Component | What it is |
|---|---|
| `fb_list_*` | the list model above: rows, cards, notes, mixed heights |
| `struct inkcell_fb_list_item` | one row with slots — marker gutter, leading avatar or tonal disc, label, trailing value, supporting line |
| `struct inkcell_fb_bubble` | the transcript's one component: wrapped body, quote line, reactions, the relay chip, the delivery mark |
| `struct inkcell_fb_selection` | the checkbox and the radio |
| `struct inkcell_fb_segmented` | a small set of alternatives, all on screen at once |
| `struct inkcell_fb_meter`, the slider | a quantity as a length; the slider is the editable one |
| `inkcell_fb_draw_signal()` | signal as rungs |
| `inkcell_fb_draw_sparkline()`, `inkcell_fb_draw_chart()` | a reading over time; the axis frame under it, or the readings listed instead |
| `inkcell_fb_draw_proportion()` | a whole and its parts |
| `struct inkcell_fb_text_field`, `struct inkcell_fb_dialog` | the draft box and the confirm sheet |
| `struct inkcell_fb_snackbar` | the transient notice |
| `inkcell_fb_draw_badge()`, `inkcell_fb_draw_state_chip()` | a capsule of text: a count that shouts, a state that is read |
| `struct inkcell_fb_qr` | a QR code — the one component drawn for a camera rather than for a reader |
| `inkcell_fb_draw_app_bar()` | the heading, with slots |
| `inkcell_fb_scaffold_begin()`, `inkcell_fb_scaffold_end()` | the chrome, placed: the tabs, the hairline, the banner, the body and the keycaps, from the width class |
| `inkcell_fb_draw_nav_bar()`, `inkcell_fb_draw_action_bar()` | the bars the scaffold draws |
| `inkcell_fb_draw_progress()`, `inkcell_fb_draw_banner()` | what the *client* says, as opposed to the radio |

Four authoring rules hold across all of them, and breaking one compiles and looks fine:

- **Nothing is spelled out in a renderer.** A screen names an *id* and something else answers: a
  string (`MESH_STR_*`), an icon (`INKCELL_ICON_*`), a tone/family/role/shape. No English prose,
  colour, margin, glyph size or corner radius belongs in `src/ui/backends/`.
  `scripts/check-strings.py` fails the build on prose.
- **Available operations are semantic commands** (`mesh/ui/commands.h`). Each currently carries
  the Brick button that invokes it and the string id that names it. The state tables in
  `src/ui/tables/actions.c` declare command ids directly; `mesh_ui_actions_for()` projects them
  into the legacy action bar. A desktop button or future overflow menu invokes
  `mesh_ui_controller_handle_command()` with the command id and, for paired controls, previous or
  next. The controller accepts only commands offered by the last presented frame and drops input
  while that frame is stale. Internally it still resolves the command to the Brick key path so
  the existing navigation guards have one implementation. SDL action-hint clicks take the same
  route through `mesh_ui_controller_handle_action_key()`; physical keyboard, gamepad and wheel
  input remain ordinary navigation keys. A keycap is untranslated — it is what is printed on the
  case. A keycap that does nothing is a bug.
- **A heading is `struct inkcell_fb_app_bar`**, with slots; the back arrow is *derived* from the action
  table, never declared.
- **fb layout is measured in cells, not bytes.** A `strlen` or `%-Ns` there is a bug.

The tables a screen reads instead of deciding for itself: `actions.c` (button verbs), `status.c`
(card verbs), `help.c`, `devices.c`, `nodes.c`, `delivery.c`, `trust.c`, `chrome.c`, `trend.c`,
`duration.c`, `units.c`.

`units.c` is the last of those and the one with a preference behind it. Metric or imperial comes
from the radio's own `DisplayConfig.units` — the client keeps no second setting, so a reader who
set their radio to miles is not asked to set the Brick to miles as well — and
`mesh_ui_units_imperial()` is the single place that decodes the byte. Every length is worded
through one of three formatters, each taking that answer as an argument rather than reading a
global: `mesh_ui_format_distance()` for a range between two places, `mesh_ui_format_altitude()`
for a height (which stays in metres or feet however large it gets, so a node on a mountain does
not read as being kilometres away), and `mesh_ui_format_length()` for a setting whose value is
metres on the wire. Position precision is the fourth and lives with its bit-count table in
`settings.c`, with a column per system. A row that formats metres itself is the way this comes
undone; `ui_units_no_setting_reads_in_metres_under_imperial` sweeps every section for one.

The exception is the fixed-position **Altitude (m)** row, which is typed rather than read and
states its unit in its label — see `docs/non-bugs.md`.

### Colour on a row is one statement, drawn in more than one place

A row says what it means once, with its `tone`, and three of its slots are renderings of that one
answer rather than three fields a caller has to keep in step:

- **`INKCELL_FB_LEADING_TONAL`** fills the leading disc from the tone's family (the primary where
  the tone names none), which is what a verb's colour is *for* — the eye finds "Remove" by its
  red long before it reads the word. Its width is the avatar's, so a list may mix the two.
- **`value_chip`** draws the value column as a capsule instead of as words, filled from the same
  family, and outlined in the theme's `OUTLINE` where the tone names none. A state, not a reading:
  "verified", "over MQTT", "plugged in". A card where every row is a bubble is a column of colour
  reporting nothing, which is the bar `inkcell_fb_draw_badge()` already states.
- **`accent_edge`** is the bar down a selected row, in the same family.

The node detail is what this was written for. Eleven verbs in the accent is not eleven emphases,
it is a card with none — so the words went back to the ordinary ink, the colour went into the
discs, and the handful of facts that are *states* became shapes as well as inks. A state said in
ink alone is a state said to whoever can tell those two inks apart. The verbs have since moved
off that screen (below), and the discs went with them.

### The node detail opens on the node, not on a menu

A node's detail used to lead with its verbs: thirteen action rows under an "Actions" heading, which
is a full panel of them before the first fact about the node. "Remove from radio" was on screen
above its battery level, Identity began below the fold, and a reader who pressed A on a node to
find out *what it was* met a menu.

So the verbs are a screen — `mesh_ui_node_actions_build()`, drawn by `fb_render_node_actions()`,
raised by the one `MESH_UI_NODE_ACTION_OPEN_ACTIONS` row the detail keeps at its top and left by B.
It is the shape the share sheet one tab over is already on: a level of the tab, with a list and a
cursor of its own (`node_actions_open`, `node_actions_cursor`), raised by a row rather than by a
question the radio asked. The detail's cursor is parked meanwhile, so B lands back on the row that
opened it.

Three things fall out of the split and are worth knowing:

- **The two builders emit the same `struct mesh_ui_node_item`**, and the renderer draws an action
  row through one function (`fb_node_action_row`) on both screens. A sheet with its own idea of
  what a destructive row looks like would be the drift the row's `tone` and `icon` fields exist to
  prevent one layer down.
- **The press dispatch is one switch** (`mesh_ui_nav_node_action_run()`), over the *item* rather
  than over a row index, because the two callers reach it with different cursors.
- **`X pin` and `Y write` stay on the detail.** They are the two verbs worth a keycap, which is the
  same division a phone makes between the actions in a top app bar and the ones in its overflow.
  Neither is named on the sheet, where both are rows.

The detail's own row is a card of one above the first heading, and that is what made
`fb_list_card_of()`'s top hairline a bug rather than a limitation: a card whose first row is the
body's first row had nowhere to spend its top edge and lost it under the cursor on the row it opens
with. The ceiling in inkcell's `widgets_list.c` is now that row's top *less the hairline*, which is room
the app bar already leaves.

### A stated fact and a control are two tiers, not one

A row that is a label and a value is one of two things, and which one decides where the emphasis
goes:

- **A stated fact** — "Firmware", "Node number", "Latitude", "Sync". The label is the *question*
  and repeats down a column the reader is scanning for the **answers**, so the label takes the
  quiet tier (`INKCELL_TONE_DIM`) and the value keeps the row's own ink.
- **A control** — a setting, a verb, a row that opens a list. The label is what the reader is
  *choosing* and the value is merely where it currently stands, so the label leads and the row
  draws in one ink.

`inkcell_fb_list_item()` takes `label_quiet` per row because a settings section mixes the two; the card
component states it, because nothing a card draws is a control (a card's verbs are buttons beside
its heading, not rows) and a flag would be a question with one answer.

Which of the two a settings row is, is `mesh_ui_settings_item_is_fact()`'s answer: not a verb,
not a cycle, no field behind it, and not one of the `ACTION` rows that open a list — that last
clause is what keeps a channel slot and a module row out of it, since neither is a verb and
quietening them would recede the one tier that is the name of the thing being opened.
`ui_settings_a_fact_is_a_row_nothing_changes` walks every section and checks the five shapes
separately, because an expression there would be the test restating the predicate.

This started on the node detail, which is a hundred and twenty facts and read as a block of text
with no way into it until the two tiers were separated — and then stayed there, so the same
reading drew two ways depending on the screen it was on. About radio's fourteen readings were at
full strength beside a node's; the Status tab's cards pasted label and value into one string and
drew the result in one colour. Both answer the question now.

### The marker gutter says how a row is changed

One cell between the label column and the value, and `mesh_ui_settings_item_marker()` is the only
thing that answers it. Two questions share the slot, and the state always wins: a `conflict` takes
it as a warning, a `dirty` edit as a dot, and only under those does the row get to say how it is
worked.

| The row | The gutter | Because |
|---|---|---|
| `TEXT`, `KEY` | the pencil | a press opens the keyboard — the value is typed |
| `ENUM`, `NUMBER` drawn as a word | the stepper `‹›` | Left and Right walk a set, where the row stands |
| `TOGGLE`, `FLAG` | nothing | the switch and the checkbox *are* the offer |
| a segmented enum, a number on a slider | nothing | same — the backend drew a control, so it drops the stepper |
| a fact, a heading, a meter, a verb | nothing | none of them is changed in place |

The bottom three rows of that table are one rule said three ways: **a control in the value column
is its own affordance, and a mark beside it captions something already legible.** Which rows get a
control is the fb backend's own decision and depends on what will fit, so the model answers the
first question and `control_marker` in `fb_screens_settings.c` is the seam where the backend
withdraws the stepper it no longer needs. The two state marks pass straight through: an unsaved
slider is still unsaved.

The rule this replaced was one symbol for all of it — the pencil on anything with a field behind
it. That put a promise of a keyboard on roughly eighty switches and checkboxes, which is most of
the tab, and left the handful of rows that really do open one saying nothing the others did not.
`ui_settings_a_marker_says_how_the_row_is_changed` walks every section and holds the table above;
`ui_settings_a_state_mark_outranks_the_offer` holds the ranking.

The Nodes list's filter and sort rows answer the same table, which is why they are drawn as
settings rows at all: the filter is a segmented button and says nothing, the sort is five orders
and one word and takes the stepper.

### A settings row that is a verb

`MESH_UI_SETTING_ACTION` is a row that *does* something, and it is drawn as one rather than as a
setting whose value happens to be an instruction. Three tables answer for it and the renderer
reads all three:

| Question | Answer |
|---|---|
| what is it about | `mesh_ui_settings_action_icon()` — the symbol in the leading disc |
| what does it cost | `mesh_ui_settings_action_tone()` — `NORMAL`, `WARNING` or `ERROR` |
| is it a verb at all | `mesh_ui_settings_item_is_verb()` |
| does the press just step its own value | `mesh_ui_settings_action_is_cycle()` |

The last one is what keeps the first honest. Five presses change nothing but the row they stand
on — the language, the theme, this client's update channel and its dev-updates switch, and the
radio's firmware channel — and a row whose value column *is* the setting is a setting, whatever
key steps it. So they are built with `cycle` set and `verb` clear: no disc, and the swap rune in
the marker gutter where a field has the stepper. The nav is untouched — it reads the kind and the
action in `number`, never `verb` — which is the point of the flag saying what the row *is* rather
than what the press does.

The third exists because the kind is doing two jobs. A Modules row and a channel slot are
`ACTION` too — the nav answers all three with A, which is what the kind is for there — so the
row carries a `verb` flag that only the action builders set. Splitting the kind is a change to
the nav (`mesh_ui_settings_channel_at_row()` tells a slot from a share row by reading `number`
against the radio's table), and worth doing on its own rather than on the way past.

**The value column is empty on a verb.** Every one of these used to say "press A", which is the
action bar's job and is said once per screen there rather than once per row — eleven rows of one
instruction with the labels, the only part that differs, read past it. What is left is the rows
with a real value: the count a forget would remove, the language a press would cycle to. Never as
a badge; a filled capsule is a count that *shouts*, which is right for unread messages and wrong
for "English".

**A chevron is a promise the nav keeps.** The mark means "this row opens something", and it used
to be spent on any verb whose value column was empty — true of every verb that opens something,
and also of several that do not: "Check for firmware" sends a request, Language and Theme cycle,
the fixed-position pair goes straight to the radio. Which rows raise a sheet, a screen or the
keyboard is `mesh_ui_settings_action_opens()`, asked of the model because the answer is the
nav's, and held against the nav's own behaviour by
`ui_nav_a_chevron_is_a_promise_the_nav_keeps`. It is the action bar's rule — a keycap that does
nothing is a bug — one column further to the right.

**Which column the value goes in is the section's answer, not the row's.** A section is settings with
presses among them or it is a list of presses, and the two want the value in different places.
Radio actions is the second — eleven verbs, no field, nothing to line a column up with — and
there the value rides the trailing edge where a trailing age goes: "21 nodes" is the size of what
the press costs, and it belongs beside the eye. About and About radio are the first, mostly-read
screens whose values sit in the column every settings row puts one in, with a verb or two among
them (Language, Theme, the firmware channel) whose value is a value in exactly the same sense.
Sent to the trailing edge those came out alone against the right-hand margin, so a four-row
screen read its values in two columns with nothing to say which row belonged to which. **A row
joins the column its neighbours are in.** A withdrawn verb is the exception and keeps the
trailing edge either way: "not supported" is a reason rather than a value, and it is drawn
quietly where the chevron it replaces was.

**The red is spent where there is no way back**, not on everything that asks first. Radio actions
is a list of things done *to* a radio, so "this costs something" is the baseline and marking
every row marks none — the section is ordered least to most destructive and the three weights
draw that gradient. `settings_verbs_that_cannot_be_undone_are_red` holds the half that bites: an
`ERROR` row always has the confirm sheet in front of it, because a red row A fires straight off
is a trap.

**A verb that cannot be pressed keeps its shape.** `MESH_UI_SETTING_ACTION_OFF` is the same row
dimmed, with the reason where the chevron was — "not connected", "not supported", "nothing to
drop". It is a kind rather than a flag so the nav refuses it by construction, and it exists at
all because a section whose length changes when the radio drops moves the cursor out from under
whoever was reading it (`settings_withdrawn_verbs_keep_the_section_shape`).

### A settings section is a column of cards

`fb_render_settings()` derives `cards[]` the way the node detail does: a heading opens a card,
everything under it belongs to that card, and the heading itself stands on no card — in the break,
where it becomes the card's label and pays for both cards' insets without costing a row. The
rows ahead of the first heading are an unnamed group and get a card of their own.

**A card has to be a grouping, not a border.** Cards are drawn at *two* groups, never one: one
surface wrapping a whole page says nothing, and a lone heading over the only group is a section
with a title rather than a section with parts. Modules is fourteen ungrouped rows and was drawn
inside a single surface, which is a box round the screen rather than a statement about any of it.
Held by `ui_capture_an_ungrouped_section_draws_no_card` with
`ui_capture_a_grouped_section_draws_cards` as its other half — a gate that silenced every card
would pass the first and lose the grouping everywhere.

Three things ask that question — the renderer, to draw cards; the navigation, to let L2/R2 cross
one; the help screen, to say the pair exists — so it is **one predicate**,
`mesh_ui_settings_section_groups()`, and not three. They disagreed once: the help asked only
whether a heading was present, so the Radio section while administering a remote node (its one
conditional heading, over a single unnamed run) drew no cards and refused R2 while the help still
promised the jump. `help_does_not_offer_a_key_that_does_nothing` checks the equivalence against
the behaviour rather than against the predicate, in that state as well as the ordinary one.

Groups and cards are the same number rather than merely close, which is what lets the model
answer for the renderer: **a group is one card whatever is in it**, verbs included, so every
group holding a row leaves exactly one card behind.

It had a second shape once, and the leading slot is what forced it: a card of verbs indented
every row past a disc and a card of settings started at the card's own padding, so a card holding
both began its words in two columns. A group that was not *all* verbs therefore carded its fields
and stood its verbs on the bare panel underneath — which meant one verb drew two ways depending
on what else happened to be in its group. LoRa's "Switch to ham mode" and About radio's "Check
for firmware" were unboxed rows between two cards, while Radio actions, the one section that is
nothing but verbs, drew the same widget as a card row.

The premise stopped being true when the slot was made unconditional (below): every row of every
open section reserves it now, and `fb_item_measure()` gives `INKCELL_FB_LEADING_TONAL` and
`INKCELL_FB_LEADING_TONAL_SLOT` one gutter deliberately, so a list can mix the two. A verb's disc lands
in the gutter its neighbours were already holding open and the labels line up down the card. It
reads better as well as simpler: a verb under a group of fields is the thing that *applies* them,
and a button belongs on the form it commits rather than adrift below it.

A *symbol* is per row on those terms — "every verb has one and no setting has one"
(`ui_settings_row_icons_are_all_or_nothing`, `ui_settings_a_disc_marks_a_press_that_acts`), and
**a heading never has one**. A symbol on a heading is a card *header*, out at the card's own edge
where its rows begin two cells further in; it was optional per heading, so the tab divided into
the eight groups whose subject happened to own a rune — five of them Radio actions' — and the
thirty that had to say nothing. That made one section announce its groups in a shape no other
section used. A settings section is a list of fields and a list has one kind of subheader; the
node detail and Status are card screens and keep theirs. The **gutter** is neither per row nor
per card: it is one width for the whole tab.

A list that indents only the rows carrying something starts its text in two columns, and the
cards were hiding that rather than fixing it — About is four ungrouped rows, two of them verbs,
and drawing the verbs past a disc while the fields began at the panel's padding put the seam on
a card edge instead of removing it. That is why `INKCELL_FB_LEADING_TONAL_SLOT` exists: the gutter,
promised to a row that has nothing to put in it.

Reserving it *per section* fixed each screen and left the set of them wrong. The condition was
"does this section hold a verb", so Position reserved the width because of one press below the
fold and Radio UI — the same list of fields with no press in it — did not, and walking between
the two moved every word about two cells sideways for a reason nothing on either screen showed.
Device did it a third way by having no cards at all to indent inside. **So every open section
reserves the slot, whether or not anything fills it.** It costs the label column the disc's
width on the sections that have no verb, which is the price of the tab having one text column;
`ui_capture_every_section_starts_in_the_same_column` holds it, against the four shapes that used
to disagree. The two lists of *subjects* — the section list and Modules — are outside it: both
fill their own narrower icon slot on every row, and neither has ever mixed the two.

**A card's edge lives outside both the boxes it separates.** A card in a list takes its bottom
padding out of the step below it, which is where the break between two cards comes from — and a
*heading* is the only step it is ever taken out of, because a heading is drawn small and centres
itself in whatever is left. A full row cannot give the room up: it is a line advance with a glyph
cell in it and, where it leads with a disc, a disc nearly as tall as the step, so a card padding
into one would land its hairline across the disc's crown. Since a group is one card whatever is
in it, the step below a card is a heading or it is the end of the list, and there is no second
sentinel to say which — `INKCELL_FB_LIST_NO_CARD` is the only one.

The hairline still has to go somewhere, and outside is the only place: an edge drawn inside a
row's box is an edge that row's highlight paints out, which is why `box_top` spends one upward.
The heading in the break gives that hairline back out of its own box at each end a card is
adjacent, so neither highlight can reach an edge, and what is drawn in the heading's slot is
sized to what is left and centred in it — the room comes off the mark and never off the column.
The clearance either end is one pixel and is written as one: it is the least that can be seen.

`ui_capture_a_verbs_disc_clears_its_cards_edges` holds that on the frame rather than on the
arithmetic, and it walks the cursor because two of the three readings only appear under a
highlight. A verb is now the one card row whose slot is *filled*, so the first and last rows of a
card are the case to watch. Nothing tonal may stand on a scanline a card's edge owns or the one
either side of it (*purity*), and the number of edges may not depend on where the cursor is
(*presence*) — a highlight laid over an edge does not corrupt that scanline, it removes it, so
only the second assertion can see it.

**The gutter is one width, and a heading owes it too.** Two things used to make a section's own
rows disagree about where its words begin, and both are the same rule broken from a different
side. The slot was measured off the row's *fill height* rather than off the list's step, so a
row two steps tall — a setting whose value is a scale, and its track with it — reserved a wider
gutter than its neighbours and started a step to their right; Position is four such rows out of
six and LoRa two. And a heading with no symbol stood at the panel's own margin over rows indented
past a disc, which is the card's label sitting outside the column it names. Both are held by
`ui_capture_a_section_starts_every_row_in_one_column`, which reads the section's column off the
rows rather than deriving it and asks only that nothing begin to the right of it — left of it is
the gutter, and a row carrying a symbol is entitled to it.

**A control that will not fit falls back to a value, and a value goes in the value column.** The
segmented button is the one slot with a second form: a set of two to four choices is drawn as the
set, and anything wider is drawn as the chosen word. That word used to take the trailing slot,
because the segments had — which is the control's reasoning rather than the row's, and put
Display's "Layout / Default" against the right-hand edge two rows under "Panel type / Auto" in
the column.

### Crossing the cards

The d-pad walks rows, Left/Right belong to the editor inside a section and L1/R1 are the tabs —
so the cards were a grouping the eye was given and the thumb was not. **L2/R2 move a whole group
at a time** (`mesh_ui_nav_cursor_group()`), on both screens that draw groups as cards: a settings
section and the node detail.

Forward lands on the first row of the next group. Back lands on the first row of *this* group and
only crosses into the previous one when the cursor is already there — the asymmetry every
document reader has, and what makes the pair usable with one thumb. A screen with no headings has
no boundaries and refuses both, which is the same answer it gives by drawing no cards. Held by
`ui_nav_settings_shoulders_walk_the_cards`; the help screen names the pair on the sections that
have it, since the action bar is five hints wide there already.

### The one colour pair that is not a theme choice

`INKCELL_COLOR_CODE` and `INKCELL_COLOR_CODE_GROUND` are black on white on every palette, and
that is deliberate rather than an omission. A QR code on this panel is not read by a person — it
is read by a phone camera held by somebody standing next to the Brick — and several scanners,
the one built into iOS among them, will not read an inverted code at all. The choice still lives
in `theme.c` rather than in the renderer, which is the rule doing its job: a screen names a role
and the theme answers, and the answer for these two happens not to vary.

Two other things about drawing a code are worth knowing before changing `inkcell_fb_draw_qr()`. Its
module size is a whole number of pixels, because a code scaled to fill the room available puts
module boundaries between pixels and a reader thresholding a photograph of that finds edges the
code has none of; so a code may not quite fill its box. And the quiet zone is part of the code —
four modules of clear margin, drawn in the code's own ground rather than left to whatever is
behind it, because a reader that cannot find the margin does not lock on.

### Animation and transitions

`MESH_UI_ROUTE_*` levels in `mesh/ui/route.h` describe *where the nav is* as a stack, and
`mesh_ui_route_of()` derives it rather than each screen declaring one. A screen transition
(`inkcell_fb_shift_begin()`), the back arrow and the `B` keycap all follow from that without being told,
which is the whole of what deriving a route buys — a new way of reaching a screen arrives with
the right behaviour already attached.

Animation frames reuse the latest store snapshot rather than requesting a synthetic refresh, and
clip drawing to the union of the moving bounds. See [`performance.md`](performance.md).

## Text and glyphs

**Text is measured in cells, not bytes.** `inkcell_font_advance()` / `inkcell_font_line()` are
where every column count, button width, bubble height and scroll window comes from, never a
constant — which is what made a second font a table entry rather than a refactor.

**A glyph is coverage**, not a bitmap: scaled coverage maps are cached (four-way, 256 entries)
and tinted at draw time, so changing a colour does not regenerate them.

| File | What it is |
|---|---|
| inkcell's `src/theme/font_ui.c` | `"ui"`, JetBrains Mono, the default face — generated |
| inkcell's `src/theme/font5x7.c` | `"5x7"`, the pixel face. ASCII plus composed accented Latin |
| inkcell's `src/theme/emoji.c` + `src/generated/emoji_glyphs.c` | emoji, generated |
| inkcell's `src/theme/icon.c` + `src/generated/icon_glyphs.c` | the icon set; `icons.def` is the table |

`scripts/gen-{emoji,icons,font,locale}.py` are **not part of the build** — run them by hand and
commit the result.

## Themes

Everything that makes the UI look like something — palette, margin, glyph multiplier, font — is
one table in inkcell's `src/theme/theme.c`. `MESHCLIENT_THEME` picks one (`dark`, `light`, `contrast`,
`colorblind`).

**A scale is not a pixel count.** It counts quarters of a glyph step (`INKCELL_SCALE_UNIT`), so
that a type role can sit half a step above the body rather than a whole one — which is what lets
the type scale grow past three roles. `INKCELL_SCALE(4)` is the body size the Brick draws at, and
is what a literal `4` used to mean. Anything turning a scale into pixels goes through
`inkcell_scale_px(steps, scale)`, and the very common "one step" case — a fill's hairline inset, a
baseline's lift over a highlight — through `inkcell_step_px(scale)`. A bare scale in a pixel
expression compiles and draws four times too large, so it is worth looking for when a renderer
here comes out wrong by a factor. The knobs are unchanged: `MESHCLIENT_FB_SCALE` and uicap's
`scale N` still take 2–6.

Four vocabularies, most abstract to least:

| Layer | What it is | Who speaks it |
|---|---|---|
| **Tone** (`inkcell_tone`) | what a piece of *text* means | screens, and every widget taking text |
| **Family** (`inkcell_family`) | what a *fill* means | widgets that fill something |
| **Role** (`inkcell_color`) | what a colour *does* | widgets, for the neutral spine |
| **RGB** (`inkcell_rgb`) | an actual colour | `theme.c` and `inkcell_fb_fill_packed()`, nothing between |

`inkcell_fb_color()`, `inkcell_fb_tone_color()` and `inkcell_fb_paint()` are the only path, which
is what makes a theme switch total: a renderer cannot keep a colour back, because it has nowhere
to put one.

### The six families

| Family | What it means |
|---|---|
| `PRIMARY` | the brand colour, and "the one you are looking at" |
| `SECONDARY` | the other side of a pair, without being better or worse |
| `TERTIARY` | in flight — started, not finished |
| `SUCCESS` | connected, healthy, delivered |
| `WARNING` | not wrong yet |
| `ERROR` | disconnected, failed, armed to destroy something |

Four slots each — `BASE`, `ON_BASE`, `CONTAINER`, `ON_CONTAINER` — because a colour used as ink
and the same colour used as a fill are not the same colour, and a fill the size of a badge and
one the size of a tab are not either. `BASE` is the saturated value; `CONTAINER` is it held back
until text can sit on it.

**The pair is the unit.** `inkcell_theme_paint(theme, family, slot, state)` returns the fill and
the ink together, and every widget that fills something goes through it. A widget taking its fill
from one slot and its label colour from another would be drawing a combination no theme was ever
measured against. Adding a family means every theme answers for all four slots, and
`inkcell_theme_validate()` loops over `INKCELL_FAMILY_COUNT` rather than hand-written rows.

### State is a layer, not a second colour

`enum inkcell_state` — `REST`, `HOVERED`, `FOCUSED`, `PRESSED` — is a *modifier*: the resting
fill with its own ink mixed in (8%, 12%, 20%). Mixing the **ink** in rather than white or black is
what makes one rule work on both a dark ground and a light one.

Only the *transient* states are layers. **Focused** is where the d-pad will act; **selected** is
the application's own state — the current tab, a checked choice — and is a resting paint (a tonal
pill, a marker, a check), never a layer, because it has to survive the cursor leaving. Disabled
takes no layer and fades its ink instead. `struct inkcell_interaction` holds all five as separate
facts; a widget field that means "the cursor is on it" is spelled `focused`.

A focused list row is a **lift**, not a bar: the row's ground with the focused layer over it, in
its own inks, and the frame draws inkcell's focus ring around it. The ring's target is not named
by any screen — the list, dialog or key that drew itself focused marks its box in the focus map,
the last mark wins, and `fb_render_snapshot()` hands that to `inkcell_fb_draw_focus_ring()`. It
travels between rows on a press, is cleared while a screen slides in, and while it travels the
frame is drawn whole rather than under the partial-redraw band. The high-contrast theme sets
`focus_fill` and keeps its inverse-video bar, with the ring around it.

It applies to a **container and never to a base**, which is a definition rather than a special
case: a container is the colour held back so text can sit on it, and that room is the room a
layer has to move in; a base is already the full-strength end. Mixing its ink in anyway takes a
saturated pair under 4.5:1, which it did in three of the four themes before the rule was written.

### Two palettes that mean nothing

**Avatar tints** are picked by a *hash* of a conversation's identity, so what they owe is
variety; a theme may state fewer than the maximum (the high-contrast one offers two).

**Series colours** are picked by *position*, which changes every term: slice 0 is the same colour
on every frame and every theme, and a theme cannot state fewer, because a chart cannot draw fewer
parts than it has. They exist because a chart's parts need colours whose only meaning is *which
part* — on the high-contrast theme the primary, secondary and tertiary are one yellow, so a
three-part bar drawn from them is an undivided block claiming the mesh is made of one thing.
`inkcell_theme_validate()` holds them to 1.4:1 in **both directions** and over every pair, not
just neighbours: a part measuring zero is not drawn, so which two share an edge is a property of
the data.

### Shape is a scale

A renderer names what kind of container it is drawing and the theme answers with a radius.

| Shape | Steps | What takes it |
|---|---|---|
| `NONE` | 0 | a rule, a bar, anything meeting an edge |
| `SM` | 2 | a list row's cursor, a keycap, the conversation cell |
| `MD` | 3 | a panel — a card, the draft box, a chat bubble |
| `LG` | 4 | a surface over the body — a dialog, a sheet |
| `FULL` | — | a capsule or circle — a chip, an unread badge, an avatar |

Steps are **glyph-scale multiples, not pixels**, so a theme asking for bigger text gets
proportionally rounder corners. `FULL` has no entry — "half of whatever this turns out to be" is
not a length a theme can state in advance — and `inkcell_theme_radius()` answers with a number
`inkcell_fb_fill_round_rect()` clamps to half the shorter side. An entirely square theme is `shape` all
zeroes. The scale starts at two because four pixels off the corner of a row a thousand pixels
wide is not a rounded rectangle, it is a rectangle somebody sanded.

`struct inkcell_metrics` holds the rest of the geometry — margin, body scale, how many steps
smaller chrome text is, how much of the body a bubble may fill, the width below which the label
column gives way. A "large text" theme is that struct with a different `scale`.

### Adding a theme

Add an entry to `k_themes` in inkcell's `src/theme/theme.c` — id, name, font id, a colour per role, metrics.
That is the whole change. `inkcell_theme_validate()` then holds it to a readability contract the
suite runs over every registered theme: body text on its ground **4.5:1** (WCAG AA), secondary
text **3:1**, a hairline only has to be visible.

A pair belongs in `k_required` **when something is actually drawn that way**, and that cuts both
ways: a pair missing from the table is a pair nothing checks, which is how dim text on a selected
outbound bubble stayed at 1.9:1 for as long as it did.

`inkcell_theme_contrast()` undoes the display's gamma per channel, weights the three by
luminance and compares. It is a 256-entry table rather than a `pow()`, because `pow()` would be
the only thing in this tree pulling libm into the static aarch64 link.

The `colorblind` theme is why roles are named for meaning: it swaps green/red for the Okabe–Ito
blue and orange and moves the primary to reddish purple, and it is a palette change only because
no renderer ever said "green".

## Backend selection

`fb` unless there is no `/dev/fb0`; `MESHCLIENT_UI_BACKEND` forces `fb`, `sdl`, `headless`, `cli`
or `stub`. `headless` is inkcell's: the frame `fb` would draw, into memory, against the real clock,
for a host with no panel and no display.

## Looking at a UI change

A UI change wants a picture, and most want a moving one: the interesting part is usually the
*transition*. `scripts/ui-capture.sh` drives the HUD through a scripted sequence of presses and
renders each frame into memory. **Nothing about it is a mock** — `mesh_ui_store_handle_key()` and
`fb_render_snapshot()` are the ones that ship, drawing into a malloc'd page instead of an mmap of
`/dev/fb0` (inkcell's `src/fb/fb_capture.c`). Only the radio is invented, so it works from a
container, a CI runner or a cloud session.

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/messages.scene -o messages.gif"
make ui-capture ARGS="devtools/ui_capture/scenes/node-states.scene -o states.gif"
make docker-ui-capture ARGS="..."          # on macOS
make screenshots                           # re-render the listing stills
printf 'scene demo\ntab nodes\nkey down 2\nkey a\n' | ./scripts/ui-capture.sh -o node.gif
```

A `.png` output captures one frame; anything else is an animated GIF. Output is halved by default
(`-d 1` keeps the panel's 1024x768), `-s N` sets the glyph scale, `-t NAME` the theme, and
`-g WxH` renders into another panel — nothing is scaled to fit, the frame is *measured* into
whatever geometry it is given, so what moves between two of these is the layout. Geometry is a
flag rather than a scene command because a GIF has one canvas.

A press that starts an animation emits more than one frame: the harness steps its clock and draws
until the renderer says nothing is moving, at the animation's own 33 ms interval, so no scene
script has to know an animation exists.

Scene scripts are one command per line, `#` comments, and every command but the setup ones emits
a frame (`key ... 3` emits three). Worked examples are in `devtools/ui_capture/scenes/`.

| Command | What it does |
|---|---|
| `scene demo\|empty` | which invented radio to start from. Setup only |
| `scale N`, `theme NAME`, `delay MS` | glyph multiplier 2–6, palette, per-frame delay |
| `clock YYYY-MM-DD HH:MM` | pin the wall clock, as local time. Setup only |
| `tab NAME`, `key NAME [COUNT]` | walk Left/Right to a tab; press a key (`a`…`y`, `l1`/`r1`, `l2`/`r2`, `start`, `select`, directions) |
| `hold MS` | lengthen the frame just emitted, and move the clock on |
| `frame` | emit the current screen again |
| `config` | a radio that has answered the config handshake |
| `stats` | the radio's own LocalStats report |
| `message in\|out NAME TEXT` | append a message |
| `reply in\|out NAME TEXT` | the same, threaded onto the newest bubble |
| `react NAME EMOJI` | react to the newest message |
| `ack sending\|delivered\|failed [ERROR]` | what the mesh said about the newest message we sent |
| `alert NAME TEXT`, `detection NAME TEXT` | `ALERT_APP` / `DETECTION_SENSOR_APP` |
| `toast TEXT` | raise the snackbar; it times out on the scene's own clock |
| `status TEXT`, `notice info\|warn\|error TEXT` | the transport line; what the radio last said |
| `queue FREE MAXLEN [refused]`, `reboots N` | Status tab rows |
| `airtime BUSY [TX]`, `airtime history MINUTES` | the airtime row and meter; a chart's worth of it |
| `update check\|download [PCT]\|available\|ready` | the self-updater's state |
| `firmware ... `, `firmware-channel stable\|alpha` | what the client knows about the *radio's* firmware |
| `syncing on\|off` | put the config handshake back in flight |
| `link up\|down` | attach or drop the radio, leaving the roster and config alone |
| `offradio NAME\|all` | mark nodes the radio's NodeDB no longer carries |
| `battery NAME PCT`, `environment NAME C [HUM]` | one telemetry report each, one reading at a time |
| `signal NAME SNR [RSSI]` | one packet heard straight off the air: dB, dBm, and the clock moved half-way to now |
| `nofix` | take our own radio's fix away |
| `verified NAME`, `verify waiting\|show\|enter\|compare\|off NAME` | the key-verification bit, and the sheet |
| `pin NAME` | pin that node |

Most of those verbs exist because **no press can reach the state**: a Routing reply arrives after
the message, a NodeDB reset is answered on the next sync, a verification stage is raised by a
ClientNotification. `tab` walks the tabs with the buttons rather than assigning `nav.screen`, so a
scene can only reach a screen the device can reach. The one thing the harness cannot do is act on
a `mesh_ui_action` — pressing START in the keyboard raises `SEND_TEXT` and the store stops there,
so `message out ...` is how a scene stands in for the echo.

A scene that walks the **Nodes** list counts rows, not roster entries: the filter row, the sort
row and the Map are in front of the first node (`MESH_UI_NODES_LEAD_ROWS`), so a node's row is its
index plus that, and a node detail's row numbers move with what that node reported. Both mistakes
render a perfectly good picture of the wrong screen, which is the one failure a capture cannot
report — so check the frames rather than the count.

The listing stills in `.github/resources/screenshots` are scenes too, in
`devtools/ui_capture/scenes/shots/`, each named for the file it writes. Each pins its clock,
which is what makes them files rather than pictures of when they were taken: `pak.json` lists the
same paths, and `scripts/screenshots.sh` refreshes both.

On a real Brick, `make deploy-shot` and `make deploy-clip` read the framebuffer directly and
catch whatever is actually on the panel — see [`device.md`](device.md#screenshots-and-clips).

## Driving the running client

`ui-capture` replays a scene against an invented radio. When the question is what the *real*
client does - its transports, its caches, the radio on the desk - drive that instead:
`--ui-control PATH` (or `MESHCLIENT_UI_CONTROL`) opens a Unix socket that takes a key by name, a
wait, and a `shot` that writes the frame as a PPM once nothing on the panel is moving. The
protocol is in `include/mesh/app/control.h`; `meshclient --ui-send 'key r1; shot /tmp/x.ppm'` is
its other end, which is what lets a Brick be driven with only the binary it already has.

`scripts/ui-drive.sh` puts the three places it runs behind one command, and brings every shot
back as a PNG on this machine:

```bash
make ui-drive ARGS="start"                          # a window on a Mac or a desktop; headless without a display
make ui-drive ARGS="send 'key r1 r1; shot nodes.png; screen'"
make ui-drive ARGS="stop"
make ui-drive ARGS="--brick start"                  # deploy-start with the socket open
make ui-drive ARGS="--brick send 'key a; shot a.png'"
make ui-drive ARGS="--brick stop"                   # every on-device session still ends here
```

A key goes through `mesh_ui_controller_handle_key()`, which is where evdev's presses land too - so
this drives the UI, not the button wiring (`make deploy-input-map` is for that). And a shot is the
frame the client drew, from the backend's `frame` hook, not the display: in a window it has no
title-bar buttons over it, and on the Brick it is the draw buffer rather than `fb0`, so a fault in
the page flip is `make deploy-shot`'s to find.
