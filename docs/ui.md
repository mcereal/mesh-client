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

**Key repeat is ours, not the kernel's.** Autorepeat is an EV_KEY/EV_REP feature, and the d-pad
arrives as the absolute axes `ABS_HAT0X/Y`: an axis sends one event out of centre and one back,
however long it is held, so a 60-node roster used to cost 60 separate presses. `input.c` runs its
own repeat off a timerfd on the same event loop — hold for `MESHCLIENT_KEY_REPEAT_DELAY_MS`
(350 ms), then a row every `MESHCLIENT_KEY_REPEAT_MS` (90 ms), halving after eight rows so a long
list is walked rather than crawled. Only the four directions repeat; a held A that confirmed
forty times would be a trap.

A direction is driven by our timer or by nothing at all — a kernel `value == 2` for one is always
dropped, so a USB keyboard cannot take two rows per step, and `MESHCLIENT_KEY_REPEAT_DELAY_MS=0`
means off on every device rather than only on the Brick. Face buttons keep whatever the kernel
does with them. A hold also ends when any other key is pressed, and when the device it started on
goes away: an unplugged keyboard hangs up its fd instead of sending the key up, and a repeat with
no release would scroll forever. Repeats reach the store one per event-loop turn and the store
coalesces its repaints, so the framebuffer draws once per row at most.

## Tabs

Five tabs: Messages, Nodes, Devices, Status, Settings.

The strip is drawn on a bar of its own — `SURFACE_LOW`, the theme's recessed tier — with the
current tab as a tonal pill. Both halves of that are saying "this is chrome, not the first row
of content", which is the job every phone's navigation bar does with the same two devices. The
pill is `FB_BUTTON_TONAL`: an accent *container* rather than the accent itself, because a block
of saturated colour the size of a tab stops being an indicator and starts being the thing you
read instead of the label on it.

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

The conversation list is the two-row shape a phone messenger has, drawn as one component
(`struct fb_conversation` / `fb_draw_conversation`, `fb_widgets.h`): a tinted **avatar disc**
carrying the correspondent's initials, then the name with the age of the last traffic against the
right edge, then what was last said with the unread count as a **pill**. The cell owns all of it
and the screen renderer only says which strings go in it.

Underneath it is a [`fb_list_item`](#struct-fb_list_item--one-row-with-slots) with every slot
filled — a leading avatar, a trailing age, a supporting line with a trailing badge — so what is
left in `fb_draw_conversation()` is the *translation*: which of a conversation's facts goes in
which slot, and which of them change what the row says rather than how it looks.

Three things make the list skimmable rather than a wall of text, and all three are in that
component:

- **The avatar.** Two cells and a colour is what the eye finds a thread by, long before it has
  read a name. The initials come from the nav (`mesh_ui_nav_conversation_at` fills `initials` and
  `tint`): the first letter of each of the first two words, or the first two letters of a
  one-word name — which is what a four-character Meshtastic short name is, so `BRVO` reads `BR`.
  A channel shows `#` and the all-traffic row a `*`. The `tint` seeds the colour and is the
  conversation's *identity* (a node number, a channel slot) rather than its name, so a node that
  renames itself keeps the colour the user has learned to look for.
- **Tighter leading than the gap.** The two rows are one item, so the preview sits a scale above
  where a second list row would put it, and the space that frees becomes the gap between cells —
  with an inset hairline in it. Without that the cell fills every pixel of its two rows and a
  list of them reads as one block with no way in.
- **Two tiers of text under the cursor.** `MESH_UI_COLOR_TEXT_ON_SEL_DIM` exists for this: the
  age and a read preview are secondary on a selected row too, and `TEXT_DIM` is chosen against
  the *ground* and says nothing about a fill over it.

The avatar tints are a per-theme palette (`theme->avatars`, read through
`mesh_ui_theme_avatar()`), not a role — the point of an avatar colour is that two of them
differ. The initials are drawn in `MESH_UI_COLOR_BG`, so every tint owes the ground the
body-text contrast and `mesh_ui_theme_validate()` holds it to that; the high-contrast theme
states two tints rather than six, because a palette of hues is what that theme exists to do
without.

**X deletes a conversation**, armed by one press and carried out by the second — the same idiom
as Y on the Devices tab and the node detail's remove row. The arming names the *conversation*
rather than the row, because direct peers are ordered by recency and one message from somebody
else re-ranks them under the cursor. The armed cell says so itself
(`mesh_ui_nav_conversation_is_armed`), in place of the preview it is about to take away.

The delete itself is the app's (`MESH_UI_ACTION_DELETE_CONVERSATION`), because a message lives in
three places at once — the transport's ring, the history read back from the cache at startup, and
the store — and one that reached only the store would survive about a second, until the next
publish rebuilt the store from the other two. Neither "All traffic" nor "New message" answers to
X: one is a view over the others and the other is a button. An emptied channel keeps its row, because
the row is the radio's channel table rather than the log; an emptied direct peer loses it, which
is what "delete conversation" means everywhere else.

**Only opening a thread moves the target.** The Nodes tab opens the node's *detail*, and only
that detail's message row opens a conversation, rather than retargeting what Messages was
showing. Compose is an overlay (`compose_open`) over the open thread rather than a tab, so it can
never be reached with a stale destination.

**A and Y in a thread are two different answers.** A opens the compose overlay - the canned
replies, cursor already on the first one, so the common reply is two presses on a d-pad. Y skips
it and opens the keyboard directly, which is why `keyboard_open` can be set with `compose_open`
clear: closing a keyboard Y opened lands back on the conversation rather than on a list the user
never asked for. Every other "Y write" (the Nodes list, a node's detail, the conversation list)
opens the thread and then the keyboard the same way; A on the "New message" row is the one that
still lands on the quick replies, and `picker_follow` is how the picker remembers which was
asked for.

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
list marks it with a star sprite before the name. Our own node is marked differently — its
avatar disc takes the accent as a stated fill instead of a tint — because being *us* is an
identity and being pinned is a preference, and the disc is the slot that carries identity.
**Our own node never shows the star**, whatever its `is_favorite` says: the flag can arrive
stale and neither `nav.c` nor `node_detail.c` will pin us, so a star there would advertise a
preference no press can clear.

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

### Status — cards

The Status tab is the one screen with nothing to select: it is a readout, and it used to be
eighteen label/value lines in one column on the bare ground, with a half-line of extra space
every so often standing in for a grouping. It is now three **cards** (`struct fb_card`, below),
which is the same information with the grouping said out loud:

| Card | What it holds | What its heading colour says |
|---|---|---|
| **Link** | transport, radio, sync, our node, the primary channel, devices in range | good when a radio is attached, bad when none is |
| **Mesh** | NodeDB and roster counts, airtime, packets, what the ring is holding | the airtime tone — accent past 25% channel utilization, bad past 50% |
| **Radio** | battery and uptime, what the firmware last said, reboots, the TX queue, free heap | the worst thing on it: bad for a flat battery, a refused packet or an `ERROR` notice |

The heading colour is the point. Every row on the Radio card exists only when something is
wrong, so on a healthy link that card is small and accent-coloured and there is nothing to read;
when it turns red, the screen has answered "is anything wrong" before a number has been.

Two consequences worth knowing:

- **The Radio card is ordered most-read first** — battery, then the radio's own words, then
  reboots, then the queue, then the heap. It is the one card that can outgrow the panel, because
  its worst case is every conditional row at once, and `fb_draw_card()` drops from the end. The
  free-heap figure is the row a user would have scrolled past anyway.
- **There is no screen title and no quit hint in the body.** The tab strip already says Status
  and each card names itself, so a title would be the third time; the quit hint moved to the
  footer, where every other screen says what the buttons do. Those two rows are what the cards
  spend on their headings. The footer only says it while a radio is attached — the line under it
  already ends in the quit hint when there is not.

Status does not scroll. On a radio reporting everything at once the last row or two of the Radio
card are dropped rather than drawn over the footer, which is the card's own contract; a scrolling
Status is the obvious next step and is a nav change, not a rendering one.

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
| `fb_widgets.c` | components | cards, buttons, chips, list items, rules, bubbles (`fb_widgets.h`) |
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

#### `struct fb_list_item` — one row with slots

A row that is more than a line of text is a `fb_list_item`: something optional at the **leading**
edge, one or two lines of content, something optional at the **trailing** edge. The shape every
phone and desktop platform settled on, and for the reason they did — it is the smallest
vocabulary that covers every row a list wants.

| Slot | Kinds |
|---|---|
| leading | `FB_LEADING_NONE`, `FB_LEADING_AVATAR` (a tinted disc with one or two cells in it) |
| headline | plain `text`, or a `label` column of `label_cols` cells then `marker` and `value` |
| supporting | a second line; non-NULL is what makes the item two rows tall |
| trailing | `FB_TRAILING_NONE` / `_TEXT` (right-aligned and quiet) / `_BADGE` (a filled capsule) / `_SWITCH` |

```c
const struct fb_list_item row = {
    .label = item->label, .label_cols = label_cols,
    .marker = "> ", .value = item->value, .tone = MESH_UI_TONE_NORMAL,
    .trailing = {.kind = FB_TRAILING_SWITCH, .sw = &sw},
};
fb_list_item(state, &list, i, &row);
```

There were four of these functions and they were the same row four times — a settings row, a
toggle row, a badge row, a conversation cell. Each carried its own copy of the **two things that
are actually hard**, and each got them slightly differently:

- **clipping the line to leave the trailing slot its room** rather than drawing under it, in
  cells and not bytes;
- **picking the ink for a row the cursor is on**, which is a *different pair* of colours rather
  than the same one dimmed — `TEXT_ON_SEL` and `TEXT_ON_SEL_DIM`, both validated against the
  cursor fill.

`supporting_quiet` is the one piece of that worth knowing: it says whether a secondary line
stays secondary *under the cursor*. A message preview does; a delete warning does not.

An avatar draws its cells at the largest glyph multiplier **that fits the disc**, which is not
always the body's. A two-row item gives the disc two lines to be round in and the body scale
fits; a one-row item — a node, a picker row — gives it one, and two cells at the body scale
overhang a circle barely taller than one glyph. `fb_draw_avatar()` measures rather than assumes,
because which scale fits is a fact about the component's geometry and a screen that worked it
out would be computing a glyph size.

`fb_list_item()` takes the state **mutably**, unlike `fb_list_row_line()`. A trailing switch
steps an animation kept on the backend and keyed by the control's identity, and a meter or a
progress bar will want the same table — so the item API carries it rather than growing a second
entry point per animated slot. `fb_draw_conversation()` is a thin translation on top: which of a
conversation's facts goes in which slot.

The Nodes, Devices, Picker and Compose lists are items too. They were plain `fb_list_row_line()`
rows for as long as the conversation list was the only thing with slots, and the result was one
app that looked like two: a node was a monospace table line with its signal right-aligned by
hand, next to a Messages tab with discs and dividers. The discs are the part that carries: a node
is the **same two cells and the same colour** in Messages, in Nodes and in the picker, because
all three resolve them through the node's own name — `mesh_ui_nav_initials()` and
`mesh_ui_nav_picker_avatar()` in the nav layer, never from the string a screen happens to be
showing. (The picker's row *reads* "BRVO  Bravo Creek", whose first two words both begin with B;
initials taken from that would be "BB" and the same radio would wear two different discs one
screen apart.)

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

`struct fb_button` is the smallest of them and carries a **variant** rather than a fill colour:
`FB_BUTTON_TEXT` shows nothing until the cursor arrives (the keyboard's character keys),
`FB_BUTTON_FILLED` is always visibly a control (its action row), and `FB_BUTTON_TONAL` is the
accent held back far enough to sit behind a label (the selected tab, and what a filter chip
would be). Each variant is a *pair* of theme colours at rest and under the cursor, resolved in
one table in `fb_button_paint()` — they travel together because every pair is one
`mesh_ui_theme_validate()` holds to 4.5:1, and splitting them across branches is how a label
ends up on a fill nothing checked it against.

`struct fb_card` is the container the others sit in: a titled panel that groups rows belonging to
one subject, which is what the [Status tab](#status--cards) is now made of. It is the odd one out
here because it is **declared and then drawn**, and the framebuffer forces that — a card's fill
has to go down before its text or it paints over it, and its height is not known until the last
row is in:

```c
struct fb_card card;
fb_card_begin(&card, MESH_STR_STATUS_CARD_LINK, connected ? MESH_UI_TONE_GOOD : MESH_UI_TONE_BAD);
fb_card_row_text(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_TRANSPORT, status);
if (handshake_valid) {
    fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_MY_NODE,
                MESH_STR_STATUS_MY_NODE, short_name, node_num);   /* formatted from the catalog */
}
fb_card_note(&card, notice_tone, notice->text);                   /* a wrapped paragraph */
(void)fb_draw_card(state, layout, &y, &card);
```

Declaring first buys three things beyond the drawing it saves:

- **A conditional row is an `if` around one call**, rather than a branch that has to remember to
  advance a y cursor by the right amount.
- **The label column is measured from the labels the card is actually holding**, so every value
  starts as far left as it can. A fixed column — which is what a row-at-a-time API has to use —
  must be wide enough for the longest label any screen might want, and leaves a card of one-word
  labels with a gutter of nothing down the middle of it. It is still capped at half the card.
- **The card owns the footer.** Rows that would fall past the body's bottom are dropped and the
  card shrinks to what is left; `fb_draw_card()` returns false when not even the heading and one
  row fit, and that is the answer for every card after it too, so a screen can stop. This was
  the "does another row fit" test the dense screens used to write out per row — and Status wrote
  two of them, differently.

The fill is `MESH_UI_COLOR_SURFACE` and the edge is `MESH_UI_COLOR_RULE`, and the edge is not
decoration: on every theme that ships, the surface is deliberately close to the ground — a
surface far from it is one body text is no longer validated against — so in daylight the
hairline is the whole of what says a card is there. `mesh_ui_theme_validate()` holds `RULE`
against both the ground and the surface for that reason, and holds the four tones a card row can
take against the surface as well. The inset is the `card_pad` metric in glyph-scale steps and
the corners are `MESH_UI_SHAPE_MD` off the [shape scale](#shape-is-a-scale), so a card grows with
the text; the vertical inset is deliberately half the horizontal one, because a row is a line
*advance* tall and the leading it already carries is counted twice in a stack of rows and once
at either end. Its edge is `OUTLINE`, not `RULE` — see [Surfaces are tiered](#surfaces-are-tiered).

`struct fb_button` is one component covering the on-screen keyboard's character keys, its action
row, and (sized to its own label, via `fb_draw_chip`) the tab strip: a cell with a label centred
**in cells**, so an emoji label sits where it looks centred. It takes a `variant` and a `shape`
rather than a fill and a radius — the keys are `FB_BUTTON_TEXT` at `MESH_UI_SHAPE_SM`, the tabs
are `FB_BUTTON_TONAL` at `MESH_UI_SHAPE_FULL`.

`struct fb_switch` is the boolean: a pill track with a knob that **slides** to the end it is
now at. Settings draws one on every `MESH_UI_SETTING_TOGGLE` row in place of the words "on" and
"off" — the words are still what `item.value` holds and still what the CLI backend prints, so
this is the fb backend choosing how to say the same thing on a screen. A caller passes identity
and state, never a position:

```c
struct fb_switch sw = {.id = 0x01000000U | (uint32_t)item.field, .on = item.number != 0U};
fb_list_field_row_switch(state, &list, i, item.label, label_cols, marker, tone, &sw);
```

Two things about it are worth knowing before reusing it:

- **`id` is the animation's key**, and it has to be stable while the control is on screen and
  unique in the frame. A settings field id is exactly such a key; the channel is mixed in
  because the Channels section repeats the same fields per channel. An `id` of 0 means "nothing
  to key on" — the switch draws correctly and never animates, which is the right answer for a
  control the frame cannot name.
- **On a selected row it lays its own ground first.** Its two colour pairs — `ACCENT` /
  `ON_ACCENT` and `SURFACE_SEL` / `TEXT_ON_SEL` — are the ones `mesh_ui_theme_validate()`
  already holds to 4.5:1, and both are contracted against the ground rather than against the
  cursor fill. On the dark and colorblind themes the cursor fill *is* `SURFACE_SEL`, so a
  switch drawn straight onto it would vanish on precisely the row being pointed at.

The track flips colour as the knob passes the midpoint rather than crossfading with it. A fade
was tried first and is wrong twice over: mid-fade the track is not a colour any knob colour was
validated against, and on the dark theme the two ends are yellow and blue, so everything between
them is mud.

### Animation

A frame is a function of a snapshot, and a snapshot has no notion of *was*: it says a switch is
on, never that it has just become on. Two pieces supply the difference.

**`src/ui/anim.c` (`include/mesh/ui/anim.h`)** is the arithmetic — a start value, a target, a
start time, a duration and an easing curve, in fixed point over 0..1000. It knows nothing about
pixels, which is what lets a slide be unit-tested frame by frame with no display anywhere near
it (`tests/suites/ui_anim.c`). `struct mesh_ui_anim_table` keys one animation per control id,
adopting the value on first sight so **nothing slides on the frame a screen opens** and
animating only a change after that. Aiming at the target it is already heading for is a no-op,
so a widget calls it every frame with the state it can see.

**The animation state lives in the backend, not in the store.** Where a knob has got to is
presentation: the application neither knows nor should be asked, it dies with the frame buffer,
and a second backend is entitled to animate differently or not at all. The table sits on
`struct mesh_ui_backend_fb_state` beside the clock the frame is drawn against, and the caller
passes identity in — the immediate-mode trick, the same shape as a `useState` keyed by
component identity.

**The repaint clock** is a `timerfd` in `src/ui/controller.c`. The store publishes on change,
which is all a screen made of text ever needs; a control that animates needs several frames from
one change. Rather than have the store invent updates nobody asked for, a backend that animates
answers `mesh_ui_backend.animating` and the controller keeps waking it every
`MESH_UI_FRAME_INTERVAL_MS` (33 ms, 30 fps) until it settles. **The timer is armed only while
something is moving** — a HUD sitting still costs exactly the wake-ups it did before any of this
existed, which on a handheld running off a battery is the only version worth shipping. A backend
that leaves `animating` NULL never ticks, which is why the CLI and stub backends are untouched.

The clock is set once per frame by whoever is driving — `mesh_time_monotonic_ms()` on the
device, a number the scene script names in a capture — and never read inside the drawing code. A
widget that read a clock of its own would draw two halves of one frame at two different times,
and a capture could not pin either of them.

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

#### Surfaces are tiered

There are three of them, and the tier says how far a thing is from the ground:

| Role | What sits on it |
|---|---|
| `SURFACE_LOW` | recessed chrome — the bar behind the tab strip |
| `SURFACE` | a panel on the ground — a card |
| `SURFACE_HIGH` | raised over the body — the keyboard's draft box |

Tonal, not shadowed, and that is forced rather than chosen: the Brick's display engine
composites `fb0` against *its own* background layer, not against what we have already drawn (see
`fb_draw.c`'s `compose_color()`), so there is no alpha to shade with and no shadow to cast. The
fill alone has to carry the distance — which is what Material's tonal elevation does, and it has
the useful property of surviving a light palette, where a dark shadow would have to invert and a
tonal step just changes direction. Lower is nearer the ground, so a dark theme's tiers get
lighter as they rise and a light theme's get darker; nothing that draws knows which way its
palette went.

`ACCENT_CONTAINER`/`ON_ACCENT_CONTAINER` is the accent's quiet half. A full accent fill is right
for a badge — small, and it has to be found across the panel — and wrong for anything the size
of a tab, where a block of saturated colour under a label becomes the loudest thing on screen
and the label stops being read. The container is the same hue held back far enough to sit behind
text. The `contrast` theme declines and states the full accent for both, the same way it states
two avatar tints instead of six: holding a colour back is the one thing that theme exists not to
do.

`OUTLINE` is the edge of a container, which is not the job `RULE` does. A rule divides content
that is already on one surface and may fade politely into it; an outline is what says a surface
is *there*, and has to be found against the ground outside it and the fill inside it at once.
They were one role while the card was the only thing with an edge and parted company when the
draft box grew one.

#### Shape is a scale

`enum mesh_ui_shape` is the geometry equivalent of a tone: a renderer names what kind of
container it is drawing and the theme answers with a radius.

| Shape | Steps | What takes it |
|---|---|---|
| `NONE` | 0 | a rule, a bar, anything meeting an edge |
| `SM` | 2 | a list row's cursor, a keycap, the conversation cell |
| `MD` | 3 | a panel — a card, the draft box, a chat bubble |
| `LG` | 4 | a surface over the body — a dialog, a sheet |
| `FULL` | — | a capsule or a circle — a chip, an unread badge, an avatar |

Steps are **glyph-scale multiples**, not pixels, for the same reason `card_pad` is: a theme
asking for bigger text gets proportionally rounder corners instead of the corners staying put
while everything round them grows. `FULL` is not a step count — "half of whatever this turns out
to be" is not a length a theme can state in advance — so it has no entry in the table and
`mesh_ui_theme_radius()` answers with a number `fb_fill_round_rect()` will clamp to half the
shorter side. An entirely square theme is `shape` all zeroes.

The scale starts at two steps rather than one because of how big things are here. A step is four
pixels at the device's scale, and four pixels off the corner of a row highlight a thousand
pixels wide and forty tall is not a rounded rectangle, it is a rectangle somebody sanded.

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

A pair belongs in `k_required` **when something is actually drawn that way**, and that cuts both
ways: a pair missing from the table is a pair nothing checks, which is how dim text on a
selected outbound bubble stayed at 1.9:1 for as long as it did. When a renderer starts drawing a
new combination it comes with a row — and when it stops, the row goes.

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

A press that starts an [animation](#animation) emits more than one: the harness keeps stepping
its clock and drawing until the renderer says nothing is moving any more, exactly as the event
loop's frame timer does on the device. Those extra frames carry the animation's own 33 ms
interval rather than the scene's delay, so a transition plays at the speed a hand holding the
device would see it, and no scene script has to know an animation exists.

| Command | What it does |
|---|---|
| `scene demo\|empty` | which invented radio to start from: a mesh with eight nodes and a message log, or nothing connected. Setup only, and the default is `demo` |
| `scale N` | glyph multiplier, 2..6. Setup only; the default is the theme's own |
| `theme NAME` | `dark\|light\|contrast\|colorblind`. Before the first frame it picks the look and emits nothing; after it, it switches and emits a frame |
| `delay MS` | default per-frame delay. Setup only |
| `tab NAME` | walk Left/Right to `messages`, `nodes`, `devices`, `status` or `settings` |
| `key NAME [COUNT]` | `up down left right a b x y l1 r1 start select` |
| `hold MS` | lengthen the frame just emitted, rather than emitting a duplicate. It also moves the clock on, so an animation that was mid-flight has advanced by the next line |
| `config` | a radio that has answered the config handshake, so the Settings sections have rows instead of "not loaded". Plausible values; what is on show is the rows |
| `frame` | emit the current screen again |
| `toast TEXT` | raise the transient notice the footer draws |
| `message in\|out NAME TEXT` | append a message, as if the radio had just said so |
| `react NAME EMOJI` | react to the newest message, as another node would. The transcript draws it on that message rather than as a bubble of its own |
| `alert NAME TEXT` | a critical alert (`ALERT_APP`) on the channel |
| `detection NAME TEXT` | a detection sensor announcing itself (`DETECTION_SENSOR_APP`) |
| `status TEXT` | set the transport status line |
| `notice info\|warn\|error TEXT` | what the radio last said about itself, on the Status tab |
| `queue FREE MAXLEN [refused]` | the radio's outgoing packet queue, on the Status tab. The row only appears once the queue is under pressure or has refused a send |
| `reboots N` | times the radio has restarted under us, on the Status tab |
| `offradio NAME\|all` | mark that node (or every node but ours) as one the radio's NodeDB no longer carries - what a NodeDB reset leaves behind. Its own verb because no press can reach it: the reset goes out over the air and the answer arrives on the next sync, and the harness has neither |

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
