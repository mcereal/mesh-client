# UI layer

The UI is deliberately thin and data-driven: one store holds the state, one navigation model
turns key presses into actions, and backends only draw. Nothing in `src/ui/nav*.c` touches an fd
or a device, so it is testable directly.

## Shape

```
mesh_app -> mesh_ui_store (snapshot + eventfd) -> mesh_ui_controller -> backend->present()
evdev -> mesh_ui_input -> mesh_ui_controller_handle_key -> mesh_ui_store_handle_key
      -> nav.c -> mesh_ui_action -> mesh_app_on_ui_action
```

- **`src/ui/store.c`** owns `mesh_ui_snapshot` and signals the loop via an eventfd.
- **`src/ui/controller.c`** drains the store and calls `backend->present(snapshot)`.
- **Backends** implement the three-function `struct mesh_ui_backend` (`init`, `shutdown`,
  `present`): `fb.c` (the device UI), `cli.c` (a terminal fallback), `stub.c` (tests).
  **Backends are stateless** — they draw the cursor from `snapshot->nav`. A new platform
  implements the backend interface and leaves the store and controller untouched.
- **`src/ui/layout.c`** holds the backend-agnostic primitives: `struct mesh_ui_line` (a string
  builder that only ever measures in drawn cells), `struct mesh_ui_list` (the
  cursor-clamp-and-scroll-window arithmetic, measured in **steps** rather than items),
  `struct mesh_ui_wrap` (cell-measured word wrap) and `mesh_ui_transcript_window` (the
  bottom-anchored window variable-height items need). None touches a framebuffer, a font or a
  snapshot, so all are unit tested directly in `tests/suites/ui_layout.c`.
- **`src/ui/theme.c`** and **`src/ui/font.c`** hold what the UI *looks* like. Nothing that draws
  holds a colour or a margin of its own — see [Themes](#themes).
- **`src/ui/nav*.c`** own the tab/cursor/compose-target model (`struct mesh_ui_nav`, carried in
  every snapshot and clamped against the lists on each consume) and return a `mesh_ui_action`.
  `nav.c` is the router; `nav_canned.c`, `nav_keyboard.c`, `nav_conversations.c` and
  `nav_settings.c` are the subjects it dispatches into, over `nav_internal.h`.

The UI-side structs (`mesh_ui_node_summary`, `mesh_ui_settings`, `mesh_ui_client_info`) are
nanopb-free twins of the core records, filled field by field in `app.c`. Nothing else keeps the
two declarations in step, so **adding a field means touching both**.

They are declared by subject rather than all in `store.h`: `store_device.h` a discovered radio,
`store_node.h` a node, `store_channel.h` a channel slot, `store_handshake.h` the roster,
`store_message.h` the transcript and waypoint book, `store_settings.h` what Settings reads.
`store.h` is the store itself and includes all six.

**Naming the narrow header decouples a reader, not a writer.** `mesh_ui_snapshot` embeds every
record by value, so anything holding a snapshot rebuilds when any one changes. What the split
buys is the other kind of reader — `trust.h` wants a node, `devices.h` wants a row — and a
1,700-line file no longer being where six unrelated subjects are edited.

## Input

`src/ui/input.c` reads every `/dev/input/event*` and maps evdev codes to `enum mesh_ui_key`.
Quit keys stop the loop before mapping.

**The Brick's face buttons do not report by position.** A is `BTN_EAST` (305), B is `BTN_SOUTH`
(304), and the button printed **Y, on the left, is `BTN_NORTH` (307)**, so X on the top is
`BTN_WEST` (308). Reading them positionally leaves Y unreachable and silently fires X in its
place — which cost a round of "the save does nothing" debugging, since Y saves a settings section
and X refreshes it. `input_brick_face_buttons` pins all four. **Do not "fix" any of it back.**
See [`device.md`](device.md#the-buttons).

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

## The framebuffer backend

`src/ui/backends/` stacks, and calls only ever go downward:

| File | Layer | What belongs there |
|---|---|---|
| `fb_draw.c` | ink | pixels, glyphs, theme lookups, cell metrics |
| `fb_widgets.c` | components | every component below (`fb_widgets.h`) |
| `fb_screens.c` | screens | one renderer per screen, and nothing else |
| `fb.c` | device | `/dev/fb0`, the page flip, the backend vtable |

**A screen renderer should read as a description of its content** — what the list holds, what
each row says, which rows are actions. If it is computing a pixel coordinate, a scroll offset or
a padding width, that belongs in a component instead.

The list is the component that earns the most. Every screen is the same shape:

```c
struct fb_list list = fb_list_begin(layout, count, nav->cursor[MESH_UI_SCREEN_NODES]);
uint32_t i;
while (fb_list_next(&list, &i)) {
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", node_name(i));
    mesh_ui_line_right(&line, layout->cols, metrics);   /* right-aligned, in cells */
    fb_list_row_line(state, &list, i, &line, FB_TONE_NORMAL);
}
```

`fb_list_begin_rows()` is the variant for an item that spends more than one row;
`fb_list_begin_visible()` for a screen that reserves body rows for something else.

### A row is however many steps the model says

`fb_list_begin_heights()` takes one row count per item and is what a list of **mixed** heights
opens with. The screen measures and hands over the array; the window, the highlight and the
scroll thumb are three sums of the same heights.

`struct mesh_ui_list` used to count items and multiply — the first row on screen, the scroll
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

All in `fb_widgets.c`, all taking a family/tone/shape rather than a colour. The header is the
reference; what follows is the map.

| Component | What it is |
|---|---|
| `fb_list_*` | the list model above: rows, cards, chips, notes, mixed heights |
| `struct fb_list_item` | one row with slots — marker gutter, leading avatar or tonal disc, label, trailing value, supporting line |
| `struct fb_bubble` | the transcript's one component: wrapped body, quote line, reactions, the relay chip, the delivery mark |
| `struct fb_selection` | the checkbox and the radio |
| `struct fb_segmented` | a small set of alternatives, all on screen at once |
| `struct fb_meter`, the slider | a quantity as a length; the slider is the editable one |
| `fb_draw_signal()` | signal as rungs |
| `fb_draw_sparkline()`, `fb_draw_chart()` | a reading over time; the axis frame under it |
| `fb_draw_proportion()` | a whole and its parts |
| `struct fb_text_field`, `struct fb_dialog` | the draft box and the confirm sheet |
| `struct fb_snackbar` | the transient notice |
| `fb_draw_badge()`, `fb_draw_state_chip()` | a capsule of text: a count that shouts, a state that is read |
| `struct fb_qr` | a QR code — the one component drawn for a camera rather than for a reader |
| `fb_draw_app_bar()` | the heading, with slots |
| `fb_draw_nav_bar()`, `fb_draw_action_bar()` | the chrome |
| `fb_draw_progress()`, `fb_draw_banner()` | what the *client* says, as opposed to the radio |

Four authoring rules hold across all of them, and breaking one compiles and looks fine:

- **Nothing is spelled out in a renderer.** A screen names an *id* and something else answers: a
  string (`MESH_STR_*`), an icon (`MESH_UI_ICON_*`), a tone/family/role/shape. No English prose,
  colour, margin, glyph size or corner radius belongs in `src/ui/backends/`.
  `scripts/check-strings.py` fails the build on prose.
- **Button hints are (button, string id) pairs** in `src/ui/actions.c`, never a sentence. A
  keycap is untranslated — it is what is printed on the case. A keycap that does nothing is a bug.
- **A heading is `struct fb_app_bar`**, with slots; the back arrow is *derived* from the action
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

- **`FB_LEADING_TONAL`** fills the leading disc from the tone's family (the primary where the tone
  names none), which is what a verb's colour is *for* — the eye finds "Remove" by its red long
  before it reads the word. Its width is the avatar's, so a list may mix the two.
- **`value_chip`** draws the value column as a capsule instead of as words, filled from the same
  family, and outlined in the theme's `OUTLINE` where the tone names none. A state, not a reading:
  "verified", "over MQTT", "plugged in". A card where every row is a bubble is a column of colour
  reporting nothing, which is the bar `fb_draw_badge()` already states.
- **`accent_edge`** is the bar down a selected row, in the same family.

The node detail is what this was written for. Eleven verbs in the accent is not eleven emphases,
it is a card with none — so the words went back to the ordinary ink, the colour went into the
discs, and the handful of facts that are *states* became shapes as well as inks. A state said in
ink alone is a state said to whoever can tell those two inks apart.

### The one colour pair that is not a theme choice

`MESH_UI_COLOR_CODE` and `MESH_UI_COLOR_CODE_GROUND` are black on white on every palette, and
that is deliberate rather than an omission. A QR code on this panel is not read by a person — it
is read by a phone camera held by somebody standing next to the Brick — and several scanners,
the one built into iOS among them, will not read an inverted code at all. The choice still lives
in `theme.c` rather than in the renderer, which is the rule doing its job: a screen names a role
and the theme answers, and the answer for these two happens not to vary.

Two other things about drawing a code are worth knowing before changing `fb_draw_qr()`. Its
module size is a whole number of pixels, because a code scaled to fill the room available puts
module boundaries between pixels and a reader thresholding a photograph of that finds edges the
code has none of; so a code may not quite fill its box. And the quiet zone is part of the code —
four modules of clear margin, drawn in the code's own ground rather than left to whatever is
behind it, because a reader that cannot find the margin does not lock on.

### Animation and transitions

`MESH_UI_ROUTE_*` levels in `mesh/ui/route.h` describe *where the nav is* as a stack, and
`mesh_ui_route_of()` derives it rather than each screen declaring one. A screen transition
(`fb_shift_begin()`), the back arrow and the `B` keycap all follow from that without being told,
which is the whole of what deriving a route buys — a new way of reaching a screen arrives with
the right behaviour already attached.

Animation frames reuse the latest store snapshot rather than requesting a synthetic refresh, and
clip drawing to the union of the moving bounds. See [`performance.md`](performance.md).

## Text and glyphs

**Text is measured in cells, not bytes.** `mesh_ui_font_advance()` / `mesh_ui_font_line()` are
where every column count, button width, bubble height and scroll window comes from, never a
constant — which is what made a second font a table entry rather than a refactor.

**A glyph is coverage**, not a bitmap: scaled coverage maps are cached (four-way, 256 entries)
and tinted at draw time, so changing a colour does not regenerate them.

| File | What it is |
|---|---|
| `src/ui/font_ui.c` | `"ui"`, JetBrains Mono, the default face — generated |
| `src/ui/font5x7.c` | `"5x7"`, the pixel face. ASCII plus composed accented Latin |
| `src/ui/emoji.c` + `emoji_glyphs.c` | emoji, generated |
| `src/ui/icon.c` + `icon_glyphs.c` | the icon set; `icons.def` is the table |

`scripts/gen-{emoji,icons,font,locale}.py` are **not part of the build** — run them by hand and
commit the result.

## Themes

Everything that makes the UI look like something — palette, margin, glyph multiplier, font — is
one table in `src/ui/theme.c`. `MESHCLIENT_THEME` picks one (`dark`, `light`, `contrast`,
`colorblind`).

Four vocabularies, most abstract to least:

| Layer | What it is | Who speaks it |
|---|---|---|
| **Tone** (`mesh_ui_tone`) | what a piece of *text* means | screens, and every widget taking text |
| **Family** (`mesh_ui_family`) | what a *fill* means | widgets that fill something |
| **Role** (`mesh_ui_color`) | what a colour *does* | widgets, for the neutral spine |
| **RGB** (`mesh_ui_rgb`) | an actual colour | `theme.c` and `fb_fill_packed()`, nothing between |

`fb_color()`, `fb_tone_color()` and `fb_paint()` are the only path, which is what makes a theme
switch total: a renderer cannot keep a colour back, because it has nowhere to put one.

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

**The pair is the unit.** `mesh_ui_theme_paint(theme, family, slot, state)` returns the fill and
the ink together, and every widget that fills something goes through it. A widget taking its fill
from one slot and its label colour from another would be drawing a combination no theme was ever
measured against. Adding a family means every theme answers for all four slots, and
`mesh_ui_theme_validate()` loops over `MESH_UI_FAMILY_COUNT` rather than hand-written rows.

### State is a layer, not a second colour

`enum mesh_ui_state` — `REST`, `SELECTED`, `ACTIVE` — is a *modifier*: the resting fill with its
own ink mixed in (12% and 20%). Mixing the **ink** in rather than white or black is what makes
one rule work on both a dark ground and a light one.

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
`mesh_ui_theme_validate()` holds them to 1.4:1 in **both directions** and over every pair, not
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
not a length a theme can state in advance — and `mesh_ui_theme_radius()` answers with a number
`fb_fill_round_rect()` clamps to half the shorter side. An entirely square theme is `shape` all
zeroes. The scale starts at two because four pixels off the corner of a row a thousand pixels
wide is not a rounded rectangle, it is a rectangle somebody sanded.

`struct mesh_ui_metrics` holds the rest of the geometry — margin, body scale, how many steps
smaller chrome text is, how much of the body a bubble may fill, the width below which the label
column gives way. A "large text" theme is that struct with a different `scale`.

### Adding a theme

Add an entry to `k_themes` in `src/ui/theme.c` — id, name, font id, a colour per role, metrics.
That is the whole change. `mesh_ui_theme_validate()` then holds it to a readability contract the
suite runs over every registered theme: body text on its ground **4.5:1** (WCAG AA), secondary
text **3:1**, a hairline only has to be visible.

A pair belongs in `k_required` **when something is actually drawn that way**, and that cuts both
ways: a pair missing from the table is a pair nothing checks, which is how dim text on a selected
outbound bubble stayed at 1.9:1 for as long as it did.

`mesh_ui_theme_contrast()` undoes the display's gamma per channel, weights the three by
luminance and compares. It is a 256-entry table rather than a `pow()`, because `pow()` would be
the only thing in this tree pulling libm into the static aarch64 link.

The `colorblind` theme is why roles are named for meaning: it swaps green/red for the Okabe–Ito
blue and orange and moves the primary to reddish purple, and it is a palette change only because
no renderer ever said "green".

## Backend selection

`fb` unless there is no `/dev/fb0`; `MESHCLIENT_UI_BACKEND` forces `fb`, `cli` or `stub`.

## Looking at a UI change

A UI change wants a picture, and most want a moving one: the interesting part is usually the
*transition*. `scripts/ui-capture.sh` drives the HUD through a scripted sequence of presses and
renders each frame into memory. **Nothing about it is a mock** — `mesh_ui_store_handle_key()` and
`fb_render_snapshot()` are the ones that ship, drawing into a malloc'd page instead of an mmap of
`/dev/fb0` (`src/ui/backends/fb_capture.c`). Only the radio is invented, so it works from a
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
| `tab NAME`, `key NAME [COUNT]` | walk Left/Right to a tab; press a key |
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
| `offradio NAME\|all` | mark nodes the radio's NodeDB no longer carries |
| `battery NAME PCT`, `environment NAME C [HUM]` | one telemetry report each, one reading at a time |
| `nofix` | take our own radio's fix away |
| `verified NAME`, `verify waiting\|show\|enter\|compare\|off NAME` | the key-verification bit, and the sheet |
| `pin NAME` | pin that node |

Most of those verbs exist because **no press can reach the state**: a Routing reply arrives after
the message, a NodeDB reset is answered on the next sync, a verification stage is raised by a
ClientNotification. `tab` walks the tabs with the buttons rather than assigning `nav.screen`, so a
scene can only reach a screen the device can reach. The one thing the harness cannot do is act on
a `mesh_ui_action` — pressing START in the keyboard raises `SEND_TEXT` and the store stops there,
so `message out ...` is how a scene stands in for the echo.

A scene that walks the **Nodes** list counts rows, not roster entries: the filter chips, the sort
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
