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
  `inbox`). B backs out.
- `mesh_ui_nav_filter_messages` is the one place that filter lives, so the Messages cursor
  indexes the filtered list.

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
`node_key[i]` / `node_pos[i]` / `node_metrics[i]` / `node_env[i]` key lines, so it is both
browsable offline and compatible in either direction with a build that knows nothing about it.

### Settings — `src/ui/settings*.c`

`settings.c` is the `k_fields` table and everything derived from it, `settings_codec.c` converts
coordinates and channel keys between bytes and text, and `settings_rows.c` builds the rows a
screen draws.

The tab as data: sections -> items (label, formatted value, kind, and for editable rows a `field`
id). Backends draw the list; `nav.c` walks it (`settings_section` open or
`MESH_UI_SETTINGS_NO_SECTION`, X yields `MESH_UI_ACTION_REFRESH_SETTINGS`).

**About (`MESH_UI_SETTINGS_ABOUT`) is first and is the odd one out.** It describes *this
client* (version, UI backend, data dir, update state) rather than the radio, so `mesh_ui_settings_section_loaded()` always reports
it loaded and the fb backend lets it through the "connect to a radio" guard — it is the one
section that means anything with nothing connected. Its rows come from `mesh_ui_client_info` in
`store.h`, so neither the nav nor the backends ever see the updater. Its ACTION rows carry an
`enum mesh_ui_settings_action` in `number`, which is how `nav.c` turns A into
`MESH_UI_ACTION_CHECK_UPDATE`/`INSTALL_UPDATE`/`CYCLE_UPDATE_CHANNEL` without knowing what a
section means. Check and install are deliberately separate presses because install replaces the
running binary. The update channel is an ACTION and not an editable ENUM field because About has
no Y-save behind it — a pending edit there would sit unwritten forever — so A steps it and
`app.c` persists it immediately. An ACTION row's value column carries a verb (`press A`) or the
setting it holds, never a bare button letter: `Check for updates > A` read as a row whose value
was the letter A.

**Editing** is driven by the `k_fields` table (label, kind, enum names, number presets, text byte
cap per `enum mesh_ui_setting_field`). The nav keeps pending edits in `nav.settings_edits`
(Left/Right/A change the row, the keyboard is retargeted for text via `keyboard_field`, Y emits
`MESH_UI_ACTION_SAVE_SETTINGS`, B asks once then discards), the item builder renders them in
place marked `dirty`, and `mesh_app_build_settings_write` in `app.c` maps each field back onto
the nanopb section.

> Adding an editable field means four edits: the enum plus table row in `settings.c`, the flatten
> in `app.c`, the `mesh_app_apply_setting_edit` case, and the `item_field` call in the section
> builder.

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

`src/ui/backends/fb*.c` draw into **page 0** of the Brick's 1024x16384 framebuffer, then
`FBIOPAN_DISPLAY`s to it and mirrors the frame into page 1, because the Allwinner display engine
keeps showing the page NextUI's SDL last flipped to (page 1 in practice). The layer blends with
per-pixel alpha, so `compose_color` always writes an opaque alpha byte. **Drop any of these and
the screen is black.**

### Text is measured in cells, not bytes

`fb_draw_text` walks `mesh_ui_text_cell_next` and spends one cell per character or emoji.
`fb_fit`/`fb_width` (over `mesh_ui_text_cell_truncate`/`mesh_ui_text_cells`) are the only right
way to clip or right-align a line.

**A `strlen` in layout code is a bug.** It used to mean an emoji name counted four columns and
drew four question marks, and it still means padding computed from bytes pushes right-aligned
metrics off the edge. `%-Ns` has the same problem, and is why the Nodes tab pads its short-name
field by hand.

`fb_draw_emoji` draws a sprite across the full character advance rather than the glyph's five
columns, so an emoji stands as tall as the capitals next to it; the sprites' own transparent
margins keep neighbours apart.

### `src/ui/font5x7.c`

The framebuffer font, keyed by **codepoint** rather than by byte: ASCII plus Latin-1 Supplement
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

## Backend selection

`MESHCLIENT_UI_BACKEND=fb|cli|stub`, resolved in `mesh_app_select_backend()` (`src/core/app.c`).

`fb` is the default and the only one that draws the real UI. It needs `/dev/fb0`, so anywhere
there is no framebuffer — a container, a dev host over SSH — selection falls through to `cli`,
which prints snapshot diffs to the terminal. `cli` and `stub` are the two you ask for by name;
anything else, an unset variable included, means "the framebuffer, or the CLI if there isn't
one". `launch.sh` sets `fb` explicitly on device.

`stub` accepts snapshots and draws nothing. It is what the tests drive the controller against,
and what `mesh_app_init` falls back to if the chosen backend's `init` fails.
