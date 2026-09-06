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

- **`src/ui/store.c`** owns `mesh_ui_snapshot` and signals the loop via an eventfd, so UI updates
  run on the main thread without busy-waiting.
- **`src/ui/controller.c`** drains the store and calls `backend->present(snapshot)`.
- **Backends** implement the three-function `struct mesh_ui_backend` in
  `include/mesh/ui/backend.h` (`init`, `shutdown`, `present`) and live in `src/ui/backends/`:
  `fb.c` (the device UI), `cli.c` (a terminal fallback where there is no framebuffer), and
  `stub.c` (tests). **Backends are stateless** — they draw the cursor from
  `snapshot->nav`. A new platform implements the backend interface and leaves the store and
  controller untouched.
- **`src/ui/layout.c`** holds the backend-agnostic layout primitives every list-and-rows UI
  needs: `struct mesh_ui_line`, a string builder that only ever measures in drawn cells;
  `struct mesh_ui_list`, the cursor-clamp-and-scroll-window arithmetic; `struct mesh_ui_wrap`,
  the cell-measured word wrap that a chat bubble measures *and* draws itself with; and
  `mesh_ui_transcript_window`, the bottom-anchored scroll window that variable-height items
  need. None of them touches a framebuffer, a font or a snapshot, so all are unit tested
  directly (`tests/suites/ui_layout.c`) and all are as useful to a new backend as to the fb one.
- **`src/ui/theme.c`** and **`src/ui/font.c`** hold what the UI *looks* like: the palette by
  role, the metrics, and the font registry a theme names one from. Nothing that draws holds a
  colour or a margin of its own; see [Themes](#themes).
- **`src/ui/nav*.c`** own the tab/cursor/compose-target model (`struct mesh_ui_nav`, carried
  inside every snapshot and clamped against the lists on each consume) and return a
  `mesh_ui_action` the controller hands to `mesh_app_on_ui_action`. `nav.c` is the router;
  `nav_canned.c`, `nav_keyboard.c`, `nav_conversations.c` and `nav_settings.c` are the subjects
  it dispatches into, over the seams in `nav_internal.h`.

The UI-side structs in `store.h` (`mesh_ui_node_summary`, `mesh_ui_settings`,
`mesh_ui_client_info`) are nanopb-free twins of the core records, filled field by field in
`app.c` (`mesh_app_copy_node_detail`, `mesh_app_flatten_settings`,
`mesh_app_flatten_client_info`). Nothing else keeps the two declarations in step, so adding a
field means touching both.

## Input

`src/ui/input.c` reads every `/dev/input/event*` and maps evdev codes (`BTN_SOUTH`..,
`ABS_HAT0X/Y` for the d-pad, arrow keys on a keyboard) to `enum mesh_ui_key`, then calls the
handler the app installed. Quit keys stop the loop before mapping.

**The Brick's face buttons do not report by position.** A is `BTN_EAST` (305) and B is
`BTN_SOUTH` (304), the reverse of the Linux `BTN_A`/`BTN_B` aliases. X and Y do not report by
position at all: the button printed **Y, on the left, is `BTN_NORTH` (307)**, so X on the top is
`BTN_WEST` (308). Reading them positionally leaves Y unreachable and silently fires X in its
place — which cost a round of "the save does nothing" debugging, because Y saves a settings
section and X refreshes it, so every save became a refresh and the edits stayed pending. All four
were verified from the device log by pressing the button; `input_brick_face_buttons` pins them.
**Do not "fix" any of it back.**

## Tabs

Five tabs: Messages, Nodes, Devices, Status, Settings.

### Messages

Two levels, the shape a phone messenger has and the shape the Settings tab already used.

- `thread_open` clear lists conversations (`mesh_ui_nav_conversation_count`/`_at`: all traffic,
  each enabled channel, each node with direct messages, then "New message"), with the list's
  cursor parked in `conversation_list_cursor`.
- `thread_open` set shows the one named by `target_node`/`target_channel` (or everything, when
  `inbox`), drawn as a **transcript of bubbles**. B backs out.
- `mesh_ui_nav_filter_messages` is the one place that filter lives, so the Messages cursor
  indexes the filtered list.

**The thread is a transcript, not a list.** Theirs sits against the left edge, ours against the
right, the newest against the bottom, and each message is wrapped whole rather than clipped to a
row. It replaced a list of one-line rows with a three-line detail pane underneath, where the only
way to read a message in full was to select it — and reading the one before it meant losing the
one you had. Three rules make it read as a conversation rather than as a log, and all three live
in `fb_screens.c` because they are content decisions:

- A **separator** opens the transcript and marks each day boundary (`Today`, `Yesterday`,
  `Sat 6 Sep`) and each silence of 30 minutes or more, so "when was this" is answered by the
  shape of the screen rather than by reading timestamps.
- The **sender is named once per run**, not once per message: a run breaks on a separator, a
  change of direction or peer, or five minutes' gap. In a direct conversation no name is drawn at
  all — the title already says who it is — while a channel and all-traffic name every run, and
  all-traffic tags each bubble with the conversation it belongs to (`#2`, `dm`).
- The **clock and delivery state tuck onto the last line** when they fit there, and take their
  own line when they do not. That is what keeps a three-word message three words tall.

The cursor still selects a message (A drills into its conversation from all-traffic; `nav.c`'s
clamp keeps it pinned to the newest as traffic arrives), so a bubble carries both a lifted fill
and an accent bar down its outer edge — a fill one step lighter is not, by itself, findable on a
3.2" panel, and gives a colour-blind eye nothing at all.

The conversation list is the two-row shape a phone messenger has: name and age, then the last
message with the unread count as a **filled badge** flush right. Both rows highlight together,
because a conversation is one item rather than two adjacent rows.

**Only opening a thread moves the target.** The Nodes tab opens the node's *detail*, and only
that detail's message row opens a conversation, rather than retargeting what Messages was
showing. Compose is an overlay (`compose_open`) over the open thread rather than a tab, so it can
never be reached with a stale destination.

The on-screen keyboard is `keyboard_open` plus `kb_row/kb_col/kb_layer` and `draft`, all in the
nav; while it is open every key goes to the keyboard handler and tabs do not switch. The
`picker_open` overlay ("New message") works the same way; its rows come from
`mesh_ui_nav_picker_row` (channels, then nodes), and picking one opens that conversation.

**Unread counts** come from `struct mesh_ui_read_state` in the store (persisted with the message
cache): one `packet_id` per conversation meaning "read up to here". An id rather than a timestamp
or an index because ids survive both the ring evicting older messages and the cache merging
history back in — and a mark whose message has been evicted correctly reads as "everything in
view is newer". `mesh_ui_store_mark_open_conversation_read` runs from `consume_updates`, so
opening a thread clears its badge and a message landing in the thread you are sitting in never
raises one. The all-traffic view marks nothing (it is a view, not a conversation) and its badge
is the sum of the rest.

### Nodes

`app.c` ranks nodes before publishing (`mesh_app_node_rank`) so the UI's 128-node budget always
holds whoever you are talking to; on an MQTT-fed mesh `last_heard` alone buries them. The order
is: us, pinned nodes, our other radios, message peers, RF nodes by `last_heard`, MQTT nodes.

**Pinning** (X, `MESH_UI_ACTION_TOGGLE_FAVORITE`) puts a node at rank 1, above even a node you
are mid-conversation with, which is also what keeps a quiet pinned node inside the budget. The
list marks it with a star sprite in the same column our own node's `*` uses.

A pin is **NodeDB state on the radio it was made on** — `is_favorite` is resolved per receiver —
so it never follows the Brick from one of your radios to another. That cuts both ways and only
one half needs handling:

- The node you connect to is rank 0 (`*`) whatever its stale flag says, and `node_detail.c` and
  `nav.c` both refuse to pin our own node, so a leftover flag is inert.
- The radio you just unplugged arrives on the new one as an ordinary stranger. Rank 2 is that
  case: `mesh_ui_preferences_note_radio` records every `my_node_num` we connect to in a small MRU
  in `ui_prefs` (`known_radios=`), and `mesh_ui_preferences_knows_radio` lifts those above
  message peers. Client-side on purpose — no admin write, and nothing that could disagree with
  what "favorite" means on the radio.

### Node detail — `src/ui/node_detail.c`

The Nodes tab's second level, the same list-of-rows shape Settings uses.
`mesh_ui_node_detail_build` emits the rows one node produces (actions, then Identity / Signal /
Device metrics / Position / Environment groups), and **a row simply is not emitted when the node
has not reported it**, so the count the nav walks and the list the backend draws can never
disagree.

A opens the detail, its first row ("Message this node") opens the conversation, B backs out, Y
still writes from either level, X pins.

Five actions is already a lot to walk past with a d-pad before reaching the readings, which is
why the set is closed: message, trace route, ask for its name, pin, ignore. What each does and
why the neighbouring admin verbs are absent is in
[`architecture.md`](architecture.md#asking-the-radio-about-a-node).

The open node is remembered by **id** (`nav.node_detail_node`), not by row: `app.c` re-ranks the
node list on every publish, so an index would slide onto a different node while the user was
reading one. `nav.c`'s clamp closes the detail when that id leaves the list.

The whole detail rides in the handshake cache as its own `node_user[i]` / `node_ident[i]` /
`node_state[i]` / `node_key[i]` / `node_pos[i]` / `node_metrics[i]` / `node_env[i]` key lines, so
it is both browsable offline and compatible in either direction with a build that knows nothing
about it. That cache is also what seeds the session's roster at startup
([`architecture.md`](architecture.md#the-node-roster)), which is why it carries `node_state`: a
name is either the node's own or derived from its number, and the node is either one the radio
still has or one only we remember. The detail says so in two rows, because both change what a
message to that node will do.

### Settings — `src/ui/settings*.c`

`settings.c` is the `k_fields` table and everything derived from it, `settings_codec.c` converts
coordinates and channel keys between bytes and text, and `settings_rows.c` builds the rows a
screen draws.

The tab as data: sections -> items (label, formatted value, kind, and for editable rows a `field`
id). Backends draw the list; `nav.c` walks it (`settings_section` open or
`MESH_UI_SETTINGS_NO_SECTION`, X yields `MESH_UI_ACTION_REFRESH_SETTINGS`).

**The section list is a table, not an enum range.** `mesh_ui_settings_root_at()` says what row
*n* of the top level is and `mesh_ui_settings_module_at()` does the same for the Modules list, so
`enum mesh_ui_settings_section` stays in declaration order — which every switch in the client is
written against — while the rows are ordered for the person reading them. Nothing may reach a
section by counting Down presses; tests go through `mesh_test_settings_open()`.

**Modules (`MESH_UI_SETTINGS_MODULES`) is a folder.** `ModuleConfig` has seventeen variants, and
putting them on the top level would push LoRa and Channels under the fold, so they sit one level
down behind a single row — the same two-level shape Channels has. Its rows are ACTION rows
carrying their target section in `number`, `nav.c` intercepts A on them ahead of the ordinary
ACTION handling, and `nav.settings_parent` is what B reads to know it goes back to the Modules
list rather than to the top. It reports itself loaded with no radio attached, because a module
the radio has not sent is listed as `not loaded` rather than hidden — "which of these has not
arrived" is most of what the screen is for.

**Headings** (kind `MESH_UI_SETTING_HEADING`) group the rows in a section long enough to need it;
Telemetry is five groups of a toggle, an interval and sometimes a screen flag. They are dimmed,
carry no value, and A on one does nothing — the same row `node_detail.c` draws. A heading is
never emitted conditionally, and neither is a row under one: a row count that moves under the
cursor mid-edit moves the cursor, which is the rule the LoRa trio is always listed for.

**About (`MESH_UI_SETTINGS_ABOUT`) is first and is the odd one out.** It describes *this
client* (version, UI backend, data dir, update state) rather than the radio, so `mesh_ui_settings_section_loaded()` always reports
it loaded and the fb backend lets it through the "connect to a radio" guard — it is the one
section that means anything with nothing connected. Its rows come from `mesh_ui_client_info` in
`store.h`, so neither the nav nor the backends ever see the updater. Its ACTION rows carry an
`enum mesh_ui_settings_action` in `number`, which is how `nav.c` turns A into
`MESH_UI_ACTION_CHECK_UPDATE`/`INSTALL_UPDATE`/`CYCLE_UPDATE_CHANNEL`/`CYCLE_THEME` without
knowing what a section means. Check and install are deliberately separate presses because install replaces the
running binary. The update channel is an ACTION and not an editable ENUM field because About has
no Y-save behind it — a pending edit there would sit unwritten forever — so A steps it and
`app.c` persists it immediately. An ACTION row's value column carries a verb (`press A`) or the
setting it holds, never a bare button letter: `Check for updates > A` read as a row whose value
was the letter A.

The **theme row** sits above the update rows, and deliberately: those return early in three
places — no updater, a check in flight, an install ready — so a row after them would disappear
exactly when somebody standing in the sun wanted it. See [Switching a theme](#switching-a-theme).

**Editing** is driven by the `k_fields` table (label, kind, enum names, number presets, text byte
cap per `enum mesh_ui_setting_field`). The nav keeps pending edits in `nav.settings_edits`
(Left/Right/A change the row, the keyboard is retargeted for text via `keyboard_field`, Y emits
`MESH_UI_ACTION_SAVE_SETTINGS`, B asks once then discards), the item builder renders them in
place marked `dirty`, and `mesh_app_build_settings_write` in `app.c` maps each field back onto
the nanopb section.

> Adding an editable field means four edits: the enum plus table row in `settings.c`, the flatten
> in `app.c`, the `mesh_app_apply_setting_edit` case, and the `item_field` call in the section
> builder. Adding a whole *module* means those four plus its storage in
> `struct mesh_radio_settings`, one row in `k_modules` in `radio_settings.c`, one arm in
> `module_admin_type()` in `app_settings.c`, one row in `k_modules` in `settings.c` (the UI
> list order), and its `has_*` in `mesh_ui_settings_section_loaded`.
> See [`settings-roadmap.md`](settings-roadmap.md) phases 12-13.

**The module table** (`struct mesh_module_binding`, `radio_settings.c`) is what a module's
protobuf identity lives in: its admin `ModuleConfigType`, its `which_payload_variant` tag, and
where its section is kept, with the size taken from the member so a row cannot claim a length
its storage does not have. The apply, the loaded predicate, the refresh queue and the write
builder all read it, so the type and the tag are named once, together, rather than typed apart
in places that had to agree — a mismatch there wrote correct bytes into the *wrong module*,
which the radio accepts without an error. Copies are `memcpy` rather than assignment only
because the section is picked at runtime; every `payload_variant` member shares the union's
address, so no offset into the `ModuleConfig` is needed.

Every radio section is editable now, with these exceptions and quirks:

- **`LED heartbeat` is shown inverted**, because the protobuf field is `led_heartbeat_disabled`
  and `app.c` negates it on the way back.
- **The Device role lists all thirteen values** with the two deprecated ones labelled
  "(retired)" rather than hidden: a radio already set to one has to be able to show it, and the
  nav steps enums as `(value + 1) % count`, so a hole in the range would be unreachable rather
  than skipped.
- **`proxy_to_client_enabled` (MQTT) stays read-only.** It makes the radio hand its MQTT traffic
  to the attached client as `MqttClientProxyMessage` (FromRadio tag 14) instead of reaching the
  broker itself, and this client ignores that variant, so a toggle would silently take the
  radio's MQTT off the air. It is still *shown*, because it is the explanation when a phone left
  it on and MQTT stopped working.

**Channels** are a two-level list (`nav.settings_channel` is the open slot or
`MESH_UI_SETTINGS_NO_CHANNEL`; `mesh_ui_settings_channel_at_row` maps a list row to a slot). The
Key row is kind `KEY`: `number` is an `enum mesh_ui_psk_choice` (keep, default, random 128/256,
none, typed hex in `text`), resolved to bytes in `app.c`. Keys are shown and typed as base64
(`mesh_ui_settings_key_text`/`_parse`, hex accepted); each KEY field has a choice mask
(`mesh_ui_settings_key_choices`) and a length rule (`_key_len_ok`). A new private key is clamped
in `app.c` and sent with the public key cleared, which the firmware fills in; admin keys are
compacted before the write.

**Confirmation** (`mesh_ui_settings_section_needs_confirm`, the `confirm_open` overlay between Y
and the write; the action's `channel` carries the slot) guards Bluetooth, Channels, LoRa,
Security and Power. Power is behind it because saving mode plus a short light-sleep or
minimum-wake leaves the radio's Bluetooth off for most of every cycle, and auto-connect cannot
reconnect to a radio that is asleep. Device and Position only reboot, which the link poller
already handles, so they are not.

A TEXT row naming a credential (`field_is_secret`, the MQTT password today) draws a fixed-width
mask instead of its value — fixed so it does not leak the length — while the keyboard still opens
on the real text. That is the KEY rows' rule exactly: redacted where it is read over somebody's
shoulder, revealed in the one place you went to change it.

## The framebuffer backend

### The three layers

`src/ui/backends/` stacks, and calls only ever go downward:

| File | Layer | What belongs there |
|---|---|---|
| `fb_draw.c` | ink | pixels, glyphs, the theme lookups, cell metrics (`fb_internal.h`) |
| `fb_widgets.c` | components | buttons, chips, list rows, field rows, rules (`fb_widgets.h`) |
| `fb_screens.c` | screens | one renderer per screen, plus the tab strip and footer |
| `fb.c` | device | `/dev/fb0`, the page flip, the backend vtable |

**A screen renderer should read as a description of its content** — what the list holds, what
each row says, which rows are actions. If it is computing a pixel coordinate, a scroll offset or
a padding width, that belongs in a component instead. Those were the three things all nine
renderers used to re-derive, each subtly differently.

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

`fb_list_begin_rows()` is the variant for an item that spends more than one row (the
conversation list spends two, a name and a preview); `fb_list_begin_visible()` is for a screen
that reserves body rows for something else.

`struct fb_bubble` is the other component that earns its keep, and it is the one place the
thread's geometry lives. A bubble sizes itself to its own text (never past three quarters of the
body), sits against the edge its direction names, and reports its height with
`fb_bubble_rows()` before it is placed. **Its measure and its draw share one `mesh_ui_wrap`
walk**, which is not an optimisation: a bubble that reserved five rows and painted six would
paint over the message below it, and the transcript places the next bubble from the count this
one reported. A second way of measuring the same text — a `strlen`, a second wrapper — is how
that happens.

Screens name a **tone** (`MESH_UI_TONE_ACCENT`, `MESH_UI_TONE_BAD`, …) rather than a colour, and
components take a tone or a **role** (`MESH_UI_COLOR_SURFACE_SEL`) rather than an RGB — see
[Themes](#themes) below. Same idea as a stylesheet with a token called `danger` instead of a hex
value.

`struct fb_button` is one component covering the on-screen keyboard's character keys, its action
row, and (sized to its own label, via `fb_draw_chip`) the tab strip: a filled cell with a label
centred **in cells**, so an emoji label sits where it looks centred.

### Drawing

`src/ui/backends/fb*.c` draw into **page 0** of the Brick's 1024x16384 framebuffer, then
`FBIOPAN_DISPLAY`s to it and mirrors the frame into page 1, because the Allwinner display engine
keeps showing the page NextUI's SDL last flipped to (page 1 in practice). The layer blends with
per-pixel alpha, so `compose_color` always writes an opaque alpha byte. **Drop any of these and
the screen is black.**

`msync` after the flip covers page 0 and its mirror, not the whole mapping: fb0 is 64 MB of
virtual rows and a frame dirties 6 MB of it.

#### Colours are packed once, then spans are filled

`fb_fill_packed()` is the only function that writes the mapping, and it takes a colour that has
already been through `compose_color`. That is the rule worth keeping: `compose_color` reads the
bitfields and branches on the pixel format, and a full screen of text is around 200k scaled
sub-pixels, so running it per pixel was roughly a third of the frame.

Everything above therefore packs once and then describes rectangles. `fb_draw_glyph` transposes
the column-major font into horizontal runs of lit pixels and fills one span per run;
`fb_draw_emoji` packs the 255-entry sprite palette once per pixel format, precomputes the
nearest-neighbour column map so the scaling division runs per column rather than per pixel, and
coalesces equal-index neighbours into spans. **A per-pixel drawing helper is a regression** — it
was one, and reintroducing it costs about 5x on every text frame.

### Text is measured in cells, not bytes

`fb_draw_text` walks `mesh_ui_text_cell_next` and spends one cell per character or emoji.

**A `strlen` in layout code is a bug.** It used to mean an emoji name counted four columns and
drew four question marks, and it still means padding computed from bytes pushes right-aligned
metrics off the edge. `%-Ns` has exactly the same problem.

`struct mesh_ui_line` exists so the mistake is no longer expressible: it has no byte-counting
entry point, and its three layout calls are the ones the screens used to hand-roll —
`mesh_ui_line_column()` (a label column: `%-*.*s` done in cells), `mesh_ui_line_right()` (a
metric flush against the right edge) and `mesh_ui_line_pad_to()`. It also trims a partial UTF-8
sequence off an append that overflowed, so a line in hand is always valid UTF-8 and can be
drawn, measured, logged or serialised interchangeably. `fb_fit`/`fb_width` remain for the few
places still working on a bare `char[]`.

`fb_draw_emoji` draws a sprite across the full character advance rather than the glyph's five
columns, so an emoji stands as tall as the capitals next to it; the sprites' own transparent
margins keep neighbours apart.

### `src/ui/font5x7.c`

The framebuffer font, reached through the `struct mesh_ui_font` descriptor it publishes
(`mesh_ui_font5x7()`) and keyed by **codepoint** rather than by byte: ASCII plus Latin-1 Supplement
and Latin Extended-A. Accented letters are **composed** from a base letter and a mark
(`k_composed`) rather than drawn, so adding one is a line. Lowercase leaves rows 0 and 1 of the
cell free and the mark goes there; capitals and ascenders fill all seven rows, so their mark
collapses to a one-row silhouette in `glyph.above`, which `fb_draw_glyph` hangs in the gap
`fb_line_adv` leaves between lines.

Consequences of a seven-row cell, all deliberate: circumflex/caron/macron/ring are
indistinguishable over a capital, and marks that sit *under* a letter have nowhere to go, so `Ç`
draws as `C`. Anything with no glyph gets the replacement box — except what the emoji table
covers.

### `src/ui/emoji.c` + the generated `src/ui/emoji_glyphs.c`

Colour emoji sprites, and the **display-cell walker** the whole UI measures with. On a real mesh
a good share of nodes are named entirely in emoji, so without these those rows are
indistinguishable boxes.

`mesh_ui_text_cell_next` is the one place that decides what one drawn column contains, and a cell
is neither a byte nor always a codepoint: a flag is a regional-indicator pair, a family is a ZWJ
sequence, and selectors/skin tones attach to what precedes them. Two rules earn their keep:

- Matching happens with variation selectors **filtered out** (the font spells its keycap
  `0039 20E3`, people type `0039 FE0F 20E3`).
- A single codepoint the text font can draw is drawn by the text font. The emoji font claims `#`,
  `*` and the ten digits because they lead keycaps; without this the 9 of "Dog Tracker K9"
  becomes a grey keycap tile.

Sequences always win — that is what the extra codepoints mean. Emoji ignore the row's text
colour; carrying their own is the point of having them.

`scripts/gen-emoji.py` rasterises Noto Color Emoji into the committed table and is **not part of
the build** — run it by hand and commit the result. 5626 sprites over 3963 unique 16x16 bitmaps (a third are duplicates), one shared
255-colour palette, run-length encoded, ~920 KB. The pixels are emitted as one string literal on
purpose: a braced initialiser of a million integers costs minutes of compile time, the literal
costs about two seconds. The file carries its own
`#pragma GCC diagnostic ignored "-Woverlength-strings"` plus a `.clang-format-ignore` entry.

## Themes

Everything that makes the UI *look* like something — the palette, the margin, the glyph
multiplier, the font — is one table in `src/ui/theme.c`, and nothing that draws holds an opinion
of its own. `MESHCLIENT_THEME` picks one (`dark`, `light`, `contrast`, `colorblind`); `dark` is
the palette the device has always drawn and is unchanged.

Three vocabularies, from most abstract to least:

| Layer | What it is | Who speaks it |
|---|---|---|
| **Tone** (`enum mesh_ui_tone`) | what a piece of content *means*: normal, dim, strong, accent, good, bad, inbound, outbound | screens, and every widget that takes text |
| **Role** (`enum mesh_ui_color`) | what a colour *does*: the ground, the fill under the cursor, text on an accent fill, an inbound bubble | widgets, for the things that are not text |
| **RGB** (`struct mesh_ui_rgb`) | an actual colour | `src/ui/theme.c` and `fb_fill_packed()`, nothing between |

A tone resolves to a role and a role resolves to an RGB, both through the theme. `fb_color()`
and `fb_tone_color()` on the backend state are the only path, which is what makes a switch
total: a renderer cannot keep a colour back, because it has nowhere to put one.

Geometry works the same way. `struct mesh_ui_metrics` holds the margin, the body scale, how many
steps smaller chrome text is, how much of the body a bubble may fill, and the width below which
the label column gives way — all of which used to be literals in drawing functions. A
"large text" theme is that struct with a different `scale`; a roomier one is a different
`margin`. Neither needs a renderer touched.

The font is a seam too (`include/mesh/ui/font.h`). `struct mesh_ui_font` is a cell size, two
gaps and a glyph lookup; `src/ui/font5x7.c` provides the one that ships, and a theme names it by
id. Every measurement in the UI — columns per line, button widths, bubble heights, the scroll
window — comes from `mesh_ui_font_advance()`/`mesh_ui_font_line()` rather than from a constant,
so a second font is a table entry rather than a refactor. `MESH_UI_GLYPH_MAX_WIDTH`/`_HEIGHT`
bound the buffers a glyph is decoded into; raise them when a font needs it.

### Adding a theme

Add an entry to `k_themes` in `src/ui/theme.c` — an id, a name, a font id, a colour per role and
its metrics. That is the whole change. `mesh_ui_theme_validate()` then holds it to a readability
contract, which the test suite runs over every registered theme:

- body text on its ground **4.5:1**, the WCAG AA threshold;
- secondary text — dim rows, the clock on a bubble, a status colour — **3:1**;
- a hairline only has to be visible.

Contrast is `mesh_ui_theme_contrast()`: undo the display's gamma per channel, weight the three
by how much of our sense of brightness comes from each (green most, blue almost none), compare
the lighter against the darker. It is a 256-entry table rather than a `pow()` because `pow()`
would be the only thing in this tree pulling libm into the static aarch64 link.

The `colorblind` theme is why roles are named for meaning. Roughly one man in twelve cannot
separate the green of "connected" from the red of "failed"; that theme swaps the pair for the
Okabe–Ito blue and orange and moves the accent to reddish purple, and it is a palette change
only because no renderer ever said "green".

### Switching a theme

Settings → About → **Theme**, and A steps to the next one. Cycling rather than a submenu because
the screen is its own preview: the frame the press draws *is* the answer, and pressing A round
the loop comes back to `dark`, which is how somebody who has stepped into a theme they cannot
read gets home.

Nothing pushes the choice at a backend. It travels the way every other fact about this client
does:

```
About row (ACTION)  ->  nav.c raises MESH_UI_ACTION_CYCLE_THEME
                    ->  app_actions.c steps app->ui_theme, writes prefs.theme, marks it dirty
                    ->  app_publish.c puts the id and name in mesh_ui_client_info
                    ->  fb_state_follow_snapshot() adopts it at the top of the next frame
```

That last step is the whole trick, and it is why **backends stay a function of the snapshot**:
the renderer reads the theme out of the frame it was handed, exactly as it reads the cursor. A
backend that does not care (the CLI one) ignores the field and nothing else changes. The
adoption happens *before* anything is measured, because a theme carries the glyph scale and the
margin the frame is laid out against.

`MESHCLIENT_THEME` outranks the saved choice, the same way `MESHCLIENT_UPDATE_ALLOW_DEV`
outranks the dev-updates switch. When it names a theme the row becomes the fact `Theme (env)`
with the name beside it, rather than offering a press that the next frame would undo. (The note
goes in the label because the value column is about eighteen cells on the device, and
`Colour-blind safe (environment)` would clip to something that reads as a bug.) The saved preference is
`theme=` in `~/.meshclient/ui_prefs`, written by name so reordering the theme table cannot move
anybody onto a different look, and an id this build does not know resolves to the default
without being overwritten.

### Seeing a theme

`--theme` on `ui-capture.sh`, or a `theme NAME` line in a scene script. Before the first frame it
picks the look; after it, it switches and emits a frame, so one script renders the same screen in
every theme:

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/themes.scene -o themes.gif"
./scripts/ui-capture.sh -t light -o light.png -d 1 devtools/ui_capture/scenes/messages.scene
```

The harness publishes its own theme as client info, so the About screen in a capture shows the
real Theme row. It cannot *act* on the row — pressing A raises the action and there is no app
behind the harness to carry it out, the same limit `message out ...` exists for — so a scene
stands in for the app by following the press with its own `theme` line.

## Backend selection

`MESHCLIENT_UI_BACKEND=fb|cli|stub`, resolved in `mesh_app_select_backend()` (`src/core/app.c`).

`fb` is the default and the only one that draws the real UI. It needs `/dev/fb0`, so anywhere
there is no framebuffer — a container, a dev host over SSH — selection falls through to `cli`,
which prints snapshot diffs to the terminal. `cli` and `stub` are the two you ask for by name;
anything else, an unset variable included, means "the framebuffer, or the CLI if there isn't
one". `launch.sh` sets `fb` explicitly on device.

`stub` accepts snapshots and draws nothing. It is what the tests drive the controller against,
and what `mesh_app_init` falls back to if the chosen backend's `init` fails.

## Looking at a UI change

A UI change wants a picture, and most of them want a moving one: the interesting part is usually
the *transition* — a thread opening, the keyboard coming up, a toast arriving — and neither a
still nor a diff shows that.

There are two ways to get one, and they meet at the same encoder
(`scripts/frames.py`, Python standard library only, so a macOS host needs no Pillow and no
ffmpeg).

### Off-screen, with no device

`scripts/ui-capture.sh` drives the HUD through a scripted sequence of button presses and renders
each frame into memory. Nothing about it is a mock: `mesh_ui_store_handle_key()` and
`fb_render_snapshot()` are the ones that ship, drawing into a malloc'd page instead of an mmap of
`/dev/fb0` (`src/ui/backends/fb_capture.c`). Only the radio is invented. That makes it usable
from a container, a CI runner or a cloud session — anywhere a Brick is not.

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/messages.scene -o messages.gif"
make docker-ui-capture ARGS="..."          # on macOS, where the core does not build natively
./scripts/ui-capture.sh -o thread.png devtools/ui_capture/scenes/messages.scene
printf 'scene demo\ntab nodes\nkey down 2\nkey a\n' | ./scripts/ui-capture.sh -o node.gif
```

A `.png` output captures a single frame; anything else is an animated GIF. The output is halved
by default (`-d 1` keeps it at the panel's 1024x768), `-s N` sets the glyph scale the device
takes from `MESHCLIENT_FB_SCALE`, and `-t NAME` the theme it takes from `MESHCLIENT_THEME`.

**Scene scripts** are one command per line, `#` starts a comment, and every command but the
first three emits a frame — `key ... 3` emits three, and the screen the scene starts on is
emitted before any of them. Worked examples live in `devtools/ui_capture/scenes/`.

| Command | What it does |
|---|---|
| `scene demo\|empty` | which invented radio to start from: a mesh with eight nodes and a message log, or nothing connected. Setup only, and the default is `demo` |
| `scale N` | glyph multiplier, 2..6. Setup only; the default is the theme's own |
| `theme NAME` | `dark\|light\|contrast\|colorblind`. Before the first frame it picks the look and emits nothing; after it, it switches and emits a frame |
| `delay MS` | default per-frame delay. Setup only |
| `tab NAME` | walk Left/Right to `messages`, `nodes`, `devices`, `status` or `settings` |
| `key NAME [COUNT]` | `up down left right a b x y l1 r1 start select` |
| `hold MS` | lengthen the frame just emitted, rather than emitting a duplicate |
| `frame` | emit the current screen again |
| `toast TEXT` | raise the transient notice the footer draws |
| `message in\|out NAME TEXT` | append a message, as if the radio had just said so |
| `status TEXT` | set the transport status line |

`tab` walks the tabs with the buttons rather than assigning `nav.screen`, so a scene can only
ever reach a screen the device can reach.

The one thing the harness cannot do is act on a `struct mesh_ui_action`. Pressing START in the
keyboard raises `SEND_TEXT` and the store stops there — it is `mesh_app` that sends and echoes it
back. `message out ...` is how a scene stands in for that.

### Off the device

`scripts/deploy-device.sh` reads the Brick's framebuffer directly, so it catches whatever is
actually on the panel — our HUD, the launcher, a crash:

```bash
make deploy-shot ARGS="-d 10 -o nodes.png"        # one frame
make deploy-clip ARGS="-d 10 -n 30 -o open.gif"   # 30 frames as a GIF
```

A page is 3 MB and there is nothing on the device to shrink it, so a clip comes back at a handful
of frames a second over WiFi — it is not real time. `-r MS` sets how fast it plays back rather
than how fast it was shot. See [`docs/device.md`](device.md).
