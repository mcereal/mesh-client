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
  `struct mesh_ui_list`, the cursor-clamp-and-scroll-window arithmetic — measured in *steps*
  rather than in items, so one row can be taller than its neighbours; `struct mesh_ui_wrap`,
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

Six tabs: Messages, Nodes, Waypoints, Devices, Status, Settings.

The strip is drawn on a bar of its own — `SURFACE_LOW`, the theme's recessed tier — with the
current tab as a tonal pill. Both halves of that are saying "this is chrome, not the first row
of content", which is the job every phone's navigation bar does with the same two devices. The
pill is `FB_BUTTON_TONAL`: a primary *container* rather than the primary itself, because a block
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
and a marker bar down its outer edge — a fill one step lighter is not, by itself, findable on a
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

**And the difference between them is on the wire.** A is *reply* on the action bar and now means
it: it records the packet id of the bubble under the cursor in `nav.reply_to`, whatever is
written or picked over it carries that id, and `mesh_session_send_reply()` puts it in
`Data.reply_id` - the field `mesh_message_ingest()` has always read on the way in. Y clears it,
because the bar calls Y *write* and a new message to a conversation is not an answer to the last
thing said in it. The target is taken by **the press that opened the overlay**, not read off the
cursor when the send happens: the transcript keeps moving under an open sheet, and a message
arriving while the user picks a canned line would otherwise re-aim the reply at whatever the
cursor had slid onto.

**X in a thread is the tapback.** It opens the reaction picker (`reaction_open`) over the same
bubble - the fixed emoji set in `src/ui/reactions.c`, one per row with the glyph in the leading
slot and what it means beside it, because eight faces in a column at this glyph scale are not
eight distinguishable things. A sends one as a reaction (`Data.emoji` set, the emoji as the
payload) and B is the way out, which is the compose sheet's two presses exactly. A reaction asks
for no ack: it has no bubble of its own - the transcript filters it out and draws it as a chip on
the message it names - so a delivery mark it earned would be one nothing on the frame could draw.
Not offered in all-traffic, for the reason A is not: a reaction goes out on the conversation its
target belongs to, and that view is several of them at once.

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
avatar disc takes the primary as a stated fill instead of a tint — because being *us* is an
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

### Waypoints — `src/ui/waypoints.c`, `src/ui/nav_waypoints.c`

A waypoint is a named place somebody broadcast to a channel: an id, a coordinate, a name, a
description and an expiry, on `WAYPOINT_APP`. Every phone app can make one and share one, and
this client used to discard them.

Two levels, the Nodes tab's shape. What is different is the trailing column, and it is the whole
reason the tab is worth having before a map exists: **how far away a place is and which way it
lies**, from our own radio's fix. `mesh_geo_vector_between()` answers both — haversine on the
IUGG mean radius, plus the great circle's initial bearing — and `mesh_ui_waypoint_format_range()`
turns that into `1.2 km NE` in whichever units the radio's own display is set to, so the client
and the radio never disagree about a distance.

**The list is ordered nearest first**, which is the order the tab exists to give. Two groups: the
places we can measure, by distance; then everything we cannot — a place with no coordinates, or
every place when our own radio has no fix — by how recently we heard it, which is the only other
thing telling one unplaceable name from another. The sort is stable, so two places at the same
distance do not swap under the cursor between frames, and the open detail is remembered **by
waypoint id** rather than by row for the same reason the node detail is: a fix arriving re-ranks
the list under whoever is reading it.

**The last row makes a place**, and the list is therefore never empty. It takes our own radio's
fix and asks for a name on the keyboard; with no fix of our own it stays on screen and says why
on its supporting line — *this radio has no position yet* — rather than disappearing, because a
row that vanishes explains nothing, and a Brick has no GPS, so this is the ordinary state rather
than the odd one. **The reason goes on the supporting line and not into the range column**: a
range is a measurement between two points and the trailing column is where a reader looks for
one, so a sentence sitting there is the marker-gutter mistake again, and it left the row's own
supporting line talking about the empty list instead. It outranks that sentence too — "no places
have been shared yet" and "this radio has no position yet" are both true on a fresh Brick, and
only the second answers the press.

**And the refused press says the same thing out loud.** A on that row raises
`MESH_STR_TOAST_WAYPOINT_NO_FIX` and opens nothing: the keyboard staying shut is deliberate —
naming a place with nowhere to put it throws the typing away at the end — but a press that did
*nothing at all* is the client looking broken, and the one reader guaranteed not to have read the
row is whoever has just pressed A. It is the same refusal `app_actions.c` raises when a name
arrives with no fix behind it, one step earlier, where the nav can see it coming.

The other way in is the node detail's **Save this place**, which takes *that* node's reported
fix: on a handheld with no GPS of its own, a node that has just broadcast a position is the
other place a real coordinate can come from.

Neither path carries the coordinate through the nav. `MESH_UI_ACTION_SHARE_WAYPOINT` names the
*node* whose fix to use (0 for our own), and `app_actions.c` reads it out of the session roster
when it acts — which is both more current and twice the size of the published one, so a node
that moves while its name is being typed is saved where it ends up.

Places expire. Upstream's `expire` is a real date, so the client honours it wherever it can read
one — a place that had already expired when it arrived is dropped rather than stored, and
`mesh_session_tick()` retires one whose deadline passes under us, because nothing on the mesh
re-announces an expiry. All three readings ask `mesh_time_wall_credible_s()` rather than
`mesh_time_wall_s()`: a Brick with no network boots into 1970, and a deadline measured against
that clock reads as tens of thousands of days rather than as the unanswerable question it is.

A place's detail says what it is (range, coordinates), what its sharer said about it (the note,
wrapped across the row's full width rather than squeezed into a value column), who shared it and
on what channel, and offers two verbs: share it again, and delete. **The delete row says which
of the two deletes it is before it is pressed** — `Delete from the mesh` when `locked_to` lets
this client withdraw it, `Forget it here` when it does not, because broadcasting a withdrawal we
are not entitled to make would ask every other client to forget a place its owner still holds.
It arms on the first press and acts on the second, the node detail's rule.

### The map — `src/map/viewport.c`, `src/ui/map.c`, `src/ui/nav_map.c`, `src/ui/backends/fb_map.c`

A picture of the nodes and the shared places, opened from the first row of the Nodes list or
from a node detail's **Show on map**. There is no basemap under it — see
[`docs/maps-roadmap.md`](maps-roadmap.md) — so what it draws is a graticule, the markers, a
crosshair and a scale bar to read the distances against.

It is the one screen in this client that is not a list, and three things follow from that.

**The d-pad moves the world, not a cursor.** `mesh_ui_nav_map_key()` takes the four directions
*ahead* of the routing in `nav.c` that turns Left and Right into a change of tab, because Left on
a map means "look west". It deliberately does not take the shoulders, and that is what pays for
it: L1/R1 and Left/Right are the same press on every other screen, and splitting them here is
what lets the map have the d-pad without the tab strip above the body going dead. X and Y are the
zoom in whole levels, START frames everything again, B leaves, and SELECT is help — the roadmap
suggested SELECT for recentring, before there was a help screen; a keycap that means one thing
everywhere it means anything is worth more than that suggestion.

**A direction stops on the next marker that way**, and pans by a fifth of the body only when
there is nothing that way. That is `mesh_ui_map_step()`, and it is a correction rather than a
flourish: a pan of a fixed number of pixels only ever leaves the crosshair on a lattice 176
across and 84 down, the crosshair captures a disc of 28, and π·28² over 176·84 is a sixth — so
*five markers in six could not be put under the crosshair at all* at a given zoom, however long
the reader panned. Zooming re-phased the lattice, which made the symptom read as "sometimes it
works" and sent readers zooming in and out to shake a node loose. "That way" is the 45-degree
quadrant around the press, and the four of them tile the plane, so everything on the panel is
one press away in the direction it looks like it is in; among the candidates the nearest wins,
so a press walks outward rather than jumping the furthest way. The view centres on the marker's
own coordinates, so the landing is exact and the selection below is still derived rather than
recorded — and because what is already under the crosshair is behind the press rather than ahead
of it, a direction always moves, which is how a reader steps between two markers drawn on top of
each other.

**Panning is aiming, so there is no selection to store.** The crosshair is the middle of the
panel and the selected marker is whatever is nearest it, derived on every frame by
`mesh_ui_map_selected()`. A field on the nav is the thing this deliberately is not — the app
bar's back arrow and `mesh_ui_route_of()` make the same argument at length, and here a second
opinion would let the ring a renderer draws and the node a press opens name two different nodes.

What makes one answer possible is that the distance is measured **in pixels from the middle of
the view**, through `mesh_map_viewport_offset()` — the same arithmetic a placement does with the
panel left off the end, which is why `mesh_map_viewport_place()` is itself written in terms of
it. It needs no box, and that matters because the store owns the nav and a backend is handed a
`const` snapshot: the two genuinely cannot ask each other how wide the body is, so anything
box-dependent there would be two answers by construction.

Measuring it in the projection rather than across the ground is the second half, and it is the
only reading that gets the poles right. A fix beyond the display limit is *drawn* at the limit,
so a marker at 88 degrees north and a view framed on it are the same point on the picture and
three degrees apart on Earth — a geodesic distance refuses a marker sitting dead centre under the
crosshair. The same constraint is why a
*fit* is computed against a declared box (`MESH_UI_MAP_FIT_WIDTH`) rather than a measured one,
and why that box is deliberately smaller than any real body — too small leaves extra air around
the outermost marker, where too large would clip one off the edge.

**The map clips its artwork to its own body.** `visible` can only speak for a marker's *centre*,
and a marker is not a point once it is drawn — a rounded-position footprint is the widest thing
the map places, so a marker centred a pixel inside the top edge would paint most of itself over
the app bar. The clip goes on the fb state rather than on each call site, because
`fb_fill_packed()` is the one function every fill, glyph and icon span in this backend goes
through: one rectangle covers the discs, the pins and the names alike, which per-call-site
bounding would not, since two of those are drawn by components that take no box. It intersects
the partial-redraw path's own clip rather than replacing it.

**A marker says how much of itself to believe.** `precision_bits` is the sender's own statement
that it rounded its position, and a hard dot drawn over a fix rounded to 360 metres would be the
client claiming a precision nobody sent — so the footprint is drawn as a filled disc under the
marker, sized from `mesh_ui_settings_precision_metres()`, the same table the node detail and the
channel's own `position_precision` row read. A filled disc rather than a ring because there is no
alpha on this panel: a ring would have to be a fill and a second fill in the ground colour, and
the second would erase the grid inside it, which is the thing the distance is judged against.

`fb_map.c` is its own file rather than a renderer in `fb_screens.c`, and that is not a size
decision. Everything in `fb_screens.c` describes rows and hands them to a component; this places
things at coordinates. Keeping it apart is what stops "a screen renderer never computes a pixel"
from becoming a rule with an exception buried inside it.

```
make ui-capture ARGS="devtools/ui_capture/scenes/map.scene -o map.gif"
```

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

A reading with ends the reader does not carry around gets a banded bar on a second step
(`MESH_UI_NODE_ROW_METER`) — battery, SNR, the two airtime figures — and the battery row also
gets a trend in its trailing slot, because a percentage is nearly always a proxy for the question
about the direction. Both are measured on the same scale, so the line and the bar are one reading
drawn twice rather than two. What the reading is *now* gets the wider picture, because that is
what this screen is for.

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

The Status tab used to be eighteen label/value lines in one column on the bare ground, with a
half-line of extra space every so often standing in for a grouping. It is now three **cards**
(`struct fb_card`, below), which is the same information with the grouping said out loud:

| Card | What it holds | Variant | Heading colour | Verb |
|---|---|---|---|---|
| **Link** | transport, radio, sync, our node, the primary channel, devices in range | elevated, always | good when a radio is attached, bad when none is | *disconnect*, while one is |
| **Mesh** | NodeDB and roster counts, airtime with a banded bar, the two counter rows and the proportion under the received one, what the ring is holding | filled | the airtime tone — warning past 25% channel utilization, error past 50% | *trend*, once there is a line to draw |
| **Radio** | battery and uptime, what the firmware last said, reboots, the TX queue, free heap | outlined while quiet, elevated when not | the worst thing on it: bad for a flat battery, a refused packet or an `ERROR` notice | *refresh*, once it has synced |

The heading colour is the point. Every row on the Radio card exists only when something is
wrong, so on a healthy link that card is small and primary-coloured and there is nothing to read;
when it turns red, the screen has answered "is anything wrong" before a number has been. Its
**variant is the same reading** — a card with nothing to report recedes into the ground rather
than spending a panel of fill saying nothing, and lifts to the raised tier when the tone does.
Link is elevated always, because it answers the screen's first question; a column of three equal
weights had nothing to say which one to look at.

**It is no longer inert.** Up and Down walk the *verbs* the cards carry — a flat list, so a card
with none is stepped over and a card is focused because the cursor is on one of its buttons — and
A runs the one it lands on. The table is
[`include/mesh/ui/status.h`](../include/mesh/ui/status.h), read by the three places that have to
agree about it: `nav.c` moves the cursor and raises the action, `actions.c` names the press in
the action bar, and `fb_screens.c` hangs the buttons on the cards. It is flat rather than
per-card because Left and Right are the tab switch here as everywhere, so there was no second
axis to spend on a cursor inside a card.

**The cursor is a verb, not a position** (`nav->status_verb`), and `cursor[MESH_UI_SCREEN_STATUS]`
is unused. It was an index into that list, which made the list *append-only*: a verb appearing
ahead of the cursor changed what the next A press did without the cursor moving, so every verb
had to be gated on the verbs before it, and a verb went where it was added rather than where its
card is. Two things came out of naming the verb instead. The list is written in the **order the
cards draw** — Link, Mesh, Radio — so Down walks down the screen, where it used to reach the
bottom card and then come back up to the middle one. And each verb states **its own** condition:
the two that are requests over the air need a link, and the *trend* does not, because what it
opens is a picture of readings this client already holds and the history outlives the radio
going away. `mesh_ui_status_verb_resolve()` is where a cursor whose verb has gone lands — the
nearest verb above it, and the reader's own place kept untouched while the screen offers none, so
a link that drops and comes back puts them back on the button they were on.

The first two verbs are presses that already existed elsewhere — X on Devices and X on Settings —
which is deliberate: the step gave a card somewhere to put a verb, and a verb invented for it
would have been arguing two things at once. What is *not* there is a destructive one: the
confirmation dialog is still keyed on `nav->settings_section`, so "reboot the radio" from this
screen means decoupling the dialog from the settings model first.

Two consequences worth knowing:

- **The Radio card is ordered most-read first** — battery, then the radio's own words, then
  reboots, then the queue, then the heap. It is the one card that can outgrow the panel, because
  its worst case is every conditional row at once, and `fb_draw_card()` drops from the end. The
  free-heap figure is the row a user would have scrolled past anyway.
- **There is no screen title and no quit hint in the body.** The tab strip already says Status
  and each card names itself, so a title would be the third time; the quit hint moved to the
  action bar, where every other screen says what the buttons do — as a keycap and the verb
  *quit*, like every other press. Those two rows are what the cards spend on their headings. The
  bar only offers it while a radio is attached — the line under it already ends in the quit hint
  when there is not.
- **The Radio card says "no report yet" rather than disappearing.** It used to vanish on a radio
  that had told us nothing about itself, because every row on it is conditional and a card with
  no rows is not drawn. That was fine while it was a readout and is not now that it carries a
  verb: the action bar would be naming a press whose button is not on the frame, and the cursor
  would step onto nothing. The Mesh card already had the same row for the same reason.

Status does not scroll. On a radio reporting everything at once the last row or two of the Radio
card are dropped rather than drawn over the action bar, which is the card's own contract —
softened by `fb_draw_card_reserving()`, which lets the Mesh card give up its last rows so the
card below it is drawn at all. Where that goes next is
[`docs/components-roadmap.md`](components-roadmap.md) §2.20: a level under each card, now that
the cursor names a verb and three more verbs can be added without re-ordering the ones already
there.

`make ui-capture ARGS="devtools/ui_capture/scenes/card-actions.scene -o cards.gif"` walks the
three weights, the ring moving between cards, and the Radio card lifting as the radio gets into
trouble, in all four themes. `status-verbs.scene` is the cursor itself: the ring walking down
the column in the cards' own order, and the trend verb standing on a card with no radio behind
it.

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
arrived" is most of what the screen is for. It is also the only thing on this tab with an
overline: `nav.settings_parent` is what the [top app bar](#fb_draw_app_bar--the-top-app-bar)
puts above a module's title, and the two-level Channels list is the same shape.

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
| `fb_widgets.c` | components | cards (three variants, with verbs), buttons, chips, badges, list items, switches, meters, sliders, sparklines, proportion bars, signal staircases, rules, bubbles, the top app bar, the navigation bar, the screen progress bar, the banner, the action bar, the snackbar (`fb_widgets.h`) |
| `fb_screens.c` | screens | one renderer per screen, and nothing else |
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

#### Rows that are not all the same height

`fb_list_begin_heights()` takes one row count per item and is what a list of *mixed* heights
opens with. Two screens call it. The node detail's readings get a bar with a row to themselves
and everything else on the screen gets one row; a Settings section gives a second step to the
number rows that draw a [slider](#the-slider), and one row to everything else. Neither list is
described by a single number.

The settings screen's arrival is why `mesh_ui_settings_items()` exists: measuring means holding
every row before placing the first one, which is the shape `mesh_ui_node_detail_build()` already
had. It is also the cheaper of the two accessors by a whole order — `mesh_ui_settings_item()`
rebuilds the section from the radio's config for each row it answers, so a renderer asking row
by row built it once per visible row.

The whole of why this needs the model rather than the widget is the **cursor**. `struct
mesh_ui_list` used to count items and multiply — the first row on screen, the scroll thumb and
the highlight rect were all `index * line` — so a row that quietly grew a second tier put those
three in three different places. It now counts *steps*, and every one of them is a sum of
heights instead. That arithmetic is in `layout.c` and unit tested there
(`layout_list_window_counts_steps`, `layout_list_scroll_counts_steps`), because a second backend
that grew a taller row would want the same answer rather than a second derivation of it.

Two rules come out of it:

- **The measure is the caller's; the authority is the list's.** A screen knows whether a row
  carries a bar and has already walked its items to find out, so it builds the heights — the
  same shape `mesh_ui_transcript_window()` takes. But once the model has been told, every entry
  point advances by *its* answer (`fb_list_row_height()`), never by what the item it was handed
  looks like. A row whose height a screen forgot to declare therefore draws short rather than
  over the row beneath it.
- **A step is a body row, and that is the floor.** There is no half-step, so a section heading
  drawn a type smaller does not get *cheaper* — it gets the air. `fb_list_subheader()` draws at
  `MESH_UI_TYPE_LABEL` and sits on the bottom of its step, so the space the smaller glyphs free
  becomes the gap above the heading, which is where a section break wants it. That closes the
  half of the type scale that could not be done while the list counted rows: a group title is no
  longer distinguished from the rows it heads by colour alone.

#### `struct fb_list_item` — one row with slots

A row that is more than a line of text is a `fb_list_item`: something optional at the **leading**
edge, one or two lines of content, something optional at the **trailing** edge. The shape every
phone and desktop platform settled on, and for the reason they did — it is the smallest
vocabulary that covers every row a list wants.

| Slot | Kinds |
|---|---|
| leading | `FB_LEADING_NONE`, `FB_LEADING_AVATAR` (a tinted disc with initials or an icon in it), `FB_LEADING_ICON` |
| headline | plain `text` — with `marker_slot` for a gutter before it — or a `label` column of `label_cols` cells then the `marker_icon` gutter and `value` |
| supporting | a second line, with its own `supporting_icon`; non-NULL is what makes the item two rows tall |
| trailing | `FB_TRAILING_NONE` / `_TEXT` (right-aligned and quiet) / `_BADGE` (a filled capsule, drawn by [`fb_draw_badge()`](#fb_draw_app_bar--the-top-app-bar)) / `_SWITCH` / `_ICON` / `_METER` / `_SIGNAL` / `_CHECKBOX` / `_RADIO` / `_SEGMENTED` |
| bar | `meter` or [`slider`](#the-slider): a track across the width the words had, on a second step, rather than against the trailing edge. Drawn only when the list gave the row that step |

```c
const struct fb_list_item row = {
    .label = item->label, .label_cols = label_cols,
    .marker_icon = item->dirty ? MESH_UI_ICON_UNSAVED : MESH_UI_ICON_EDIT,
    .value = item->value, .tone = MESH_UI_TONE_NORMAL,
    .trailing = {.kind = FB_TRAILING_SWITCH, .sw = &sw},
};
fb_list_item(state, &list, i, &row);
```

Three of those four slots hold an [icon](#srcuiiconc--the-generated-srcuiicon_glyphsc) rather than a character, which is what the row
markers used to be. `FB_LEADING_ICON` **reserves its cell whether or not the row filled it** —
`MESH_UI_ICON_NONE` included — because a list that indents only the rows with something to say
is a list the eye cannot run down.

The **marker gutter** works the same way and reaches it differently depending on the row's
shape. A label/value row measures the gutter out of `label_cols`, so it is there for free. A
plain row — `text` the whole line — has nothing to measure it against, so the list declares it
with `marker_slot` on **every** row and fills `marker_icon` on the ones with something to say.
The Nodes tab is the caller: a pinned node's star sits between the avatar and the name, which is
where a fact about the row belongs when the leading slot is already carrying the row's identity.

The settings root and the Modules list under it are the leading slot's own callers, and they are
what it was added for: twenty-five rows of prose in two lists where every other list here gives
the eye a disc or a rune. `mesh_ui_settings_section_icon()` answers what a section is about,
beside `mesh_ui_settings_section_name()` because it is the same kind of fact, and
`mesh_ui_settings_section_icons_rows()` is what lets a renderer declare the slot once for the
list rather than test a row — a section gives every row an icon or gives none, and
`ui_settings_row_icons_are_all_or_nothing` holds it to that.

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

The Devices row is where the trailing **badge** slot earns itself outside the conversation cell.
A device's state - connected, working, needs pairing - used to be a word on the supporting line
in the dim ink every supporting line takes, which meant a list of radios read as three identical
rows until each had been read; it is a capsule in the family that says what the state *is*, and
the signal figure it displaced drops to the supporting line, because how a radio is attached is
a detail and whether it is the one we are on is not.

`paired` is deliberately **not** a capsule. It is the resting state of a bonded radio, so a pill
there is on every row at once - a column of colour reporting nothing - and on two of the four
themes it was worse than nothing: the contrast palette has one yellow and the colourblind
palette one blue, so a resting capsule came out the same colour as the warning beside it on the
first and as `connected` on the second. The rule that came out of it is the one to apply to the
next badge: **anything a badge does not shout is a badge that should not be there**, and a quiet
word in the same right-aligned slot says it without spending a colour. A row armed to be
forgotten is the exception that proves it - the capsule comes back, in the error family, because
the row is being asked a destructive question and every part of it should say so.

#### `struct fb_text_field` and `struct fb_dialog`

The last two things a screen renderer was drawing by hand.

**The text field** is the keyboard's draft box. The outline, the fill inside it, the corner
radius, the caret, which tail of an overlong draft to show and where the counter sits were all
spelled out in `fb_render_keyboard()`, which is exactly the pixel arithmetic `fb_screens.c` is
not supposed to contain. There is one field on screen at a time and it is always the thing being
edited, so it has no unfocused state and takes no cursor: a text field here is a *focused* text
field. It owns the two things the screen got wrong when it owned them — the draft scrolls to show
its **tail**, because the end is where the caret is, measured in cells so a draft of emoji moves
a glyph at a time; and the counter sits **outside** the box, because text inside the fill that is
not the value reads as the value.

**The dialog** is the confirmation screen. It was a title, four lines of wrapped text on the bare
ground, and the two answers as ordinary list rows — which is to say it looked like every other
list in the app, at the one moment the app is asking rather than showing. It fills the body
rather than floating over it, and that is deliberate: a dialog elsewhere dims what is behind it
with a scrim, and a scrim is alpha, which this framebuffer has none of (see **Surfaces are
tiered**). What stands in for it is that `fb_render_confirm()` is a screen rather than an
overlay, so there is nothing left to dim. Only the supporting paragraph gives way when the panel
is short: the buttons, the headline and the icon are reserved first, because a dialog that
dropped a button to fit its explanation would be unanswerable.

> **Exactly one button carries a fill, and it is always the focused one.** The obvious design
> gives the accept a standing tonal fill so it reads as the proposed answer, and lets the cursor
> promote it to the full accent. That works on three themes and fails on the fourth: the
> high-contrast palette deliberately collapses `ACCENT`, `ACCENT_CONTAINER` and `SURFACE_ACTIVE`
> onto one yellow, because a theme built for legibility has no held-back version of its one
> accent. The accept then renders identically whether or not it is selected. So the fill means
> *focus* and nothing else, and what marks the affirmative is its check and its family-coloured
> label — ink on the panel, which survives every palette. `ui_capture_dialog_marks_the_selected_answer`
> pins it, and fails on the design that looked right.

The action row **stacks when the two answers will not fit side by side**, and each label is
fitted to the panel first. Both halves are needed: "Reset the node database" at
`MESH_UI_SCALE_MAX` wants more than the whole panel on its own, and beside Cancel it put the
cancel button off the left-hand edge of the *screen* — on a destructive confirmation, where the
answer that vanished was the safe one. Stacked, the acting answer goes on top and the dismissive
one stays nearest the thumb. `ui_capture_dialog_actions_stay_inside_the_panel` renders every
confirmable action at the largest scale and fails if anything lands outside the panel.

#### `struct fb_bubble` — the transcript's one component

`struct fb_bubble` is the other component that earns its keep, and it is the one place the
thread's geometry lives. A bubble sizes itself to its own text (never past three quarters of the
body), sits against the edge its direction names, and reports its height with
`fb_bubble_rows()` before it is placed. **Its measure and its draw share one `mesh_ui_wrap`
walk**, which is not an optimisation: a bubble that reserved five rows and painted six would
paint over the message below it, and the transcript places the next bubble from the count this
one reported. A second way of measuring the same text — a `strlen`, a second wrapper — is how
that happens.

The bubble's **trailing run** (`struct fb_bubble_meta`) is four *typed slots* rather than one
string, and that distinction is the whole of it. The reactions, the padlock, the clock and the
delivery mark used to be concatenated by the screen and handed over as `meta`; the bubble
measured that string, clamped its own box to three quarters of the body when the string was
wider than that, and then right-aligned the string inside the box it had clamped. The
difference came out of the left edge — a whole line painted on bare background, outside the
bubble it belonged to. It took a failure reason ("no public key for that node" is 27 cells
against a bubble that holds 25 at the largest scale) or a fourth reaction chip to reach, which
is why it survived so long.

Typed parts cannot do that. `fb_bubble_run()` assembles them, measures them once for the
measure *and* the draw, and drops them off the **front** until they fit the same budget the box
gets — so what is lost first is a reaction chip and what survives longest is the mark saying the
message failed. The bubble is then widened around what is left, which makes "the run cannot
reach past the left padding" an arithmetic fact rather than a thing to be careful about.
`ui_capture_bubble_contains_its_own_ink` finds the bubble by its own fill at every scale, in
every theme, in all four delivery states and both cursor positions, and fails on any drawn pixel
outside it.

A **quote** (`quote`) is the one other optional block, and it is the visible half of a threaded
reply: one dim line above the text with a bar down its left edge, which is the quote block every
messenger draws. One line and *elided* rather than wrapped - it is a reminder of something
already further up the transcript, not a second message, and a quote that could grow would let
one bubble be mostly somebody else's words. The bar rather than a glyph because there is no
corner arrow in either face here (`U+2190`..`U+2193` is the whole of what the fonts carry), and
because a rule is already what the eye reads as "this is being cited" - the same job a list
row's accent edge does. The screen resolves it in `fb_thread_quote()` against the **whole**
message list rather than the filtered transcript, and a target the ring has since evicted simply
leaves the bubble without one.

The slots, in the order they are drawn:

| Slot | What it says |
|---|---|
| `reactions` | the chip run, counted rather than repeated (`\U0001F44D3 \U0001F602`) |
| `lock` | this direct message was decrypted with our key pair rather than with a channel PSK. The one mark on a bubble that no word on screen repeats: on a channel still using the default key every node on the mesh holds that key, so a DM that did *not* go out PKI-encrypted was readable by all of them and nothing else distinguishes the two |
| `clock` | when it arrived; empty when the radio has no clock set |
| `state` | what became of one of ours — the mark `src/ui/delivery.c` answers with |

**Delivery is a mark, not a word.** `"ok"`, `".."` and `"!!"` are gone from the string catalog:
each was two cells of punctuation standing for a state, which is the marker-gutter mistake one
level in — unreadable until learnt, and a translator's problem the moment it is a word. A clock
(`schedule`), a double tick (`done_all`) and an alert circle (`error`) are read without being
learnt, which is why every messenger draws exactly these three. Which state gets which is
[`src/ui/delivery.c`](../src/ui/delivery.c), never a renderer — the same rule the Status cards'
verbs and the chrome's banners follow, and it is what lets a backend with no sprites say the
word (`MESH_STR_DELIVERY_*`) for the same state the transcript draws a picture of.

The mark takes the run's own quiet ink rather than a tone of its own, on purpose. A failed
message is already drawn in the error family — its bubble *is* the error container — so a red
tick would be the fill said twice; and "gone out" against "acknowledged" is a difference of one
tick, which is the difference everybody already reads. It also keeps the mark inside a pairing
[the theme is already validated on](#themes) instead of asking every palette for another one.

**A failure reason is not a corner mark.** It goes under the message, as `note` — a wrapped
supporting line inside the bubble, which the trailing run then tucks onto the end of. A reason
is a sentence, so a run carrying one could never be a corner mark, and it was what pushed the
run past the bubble in the first place. A failure is worth the row; nothing else on a bubble is.

`make ui-capture ARGS="devtools/ui_capture/scenes/delivery.scene -o delivery.gif"` puts all
three states on one frame, with the cursor moving over them — a selected bubble is a different
fill with an accent bar laid outside it, and the marks have to stay legible against both.

Screens name a **tone** (`MESH_UI_TONE_WARNING`, `MESH_UI_TONE_ERROR`, …) rather than a colour;
components that fill something take a **family** (`MESH_UI_FAMILY_ERROR`) and ask the theme for
the fill and ink together; components that only draw neutral furniture take a **role**
(`MESH_UI_COLOR_SURFACE_SEL`). None of them takes an RGB — see [Themes](#themes) below. Same
idea as a stylesheet with a token called `danger` instead of a hex value.

`struct fb_button` is the smallest of them and carries a **variant** rather than a fill colour:
`FB_BUTTON_TEXT` shows nothing until the cursor arrives (the keyboard's character keys),
`FB_BUTTON_FILLED` is always visibly a control (its action row), and `FB_BUTTON_TONAL` is a
family held back far enough to sit behind a label (the selected tab, and what a filter chip
would be). Each variant resolves to a *pair* of theme colours at rest and under the cursor in
one place, `fb_button_paint()` — they travel together because every pair is one
`mesh_ui_theme_validate()` holds to 4.5:1, and splitting them across branches is how a label
ends up on a fill nothing checked it against.

A tonal button also carries a **family**, which defaults to the primary. That is what lets one
component be the selected tab *and* the destructive confirm in a delete dialog: the dialog names
`MESH_UI_FAMILY_ERROR` once and its icon, its headline and its accept button's fill all change
together, instead of a red word being painted onto the ordinary pill. `struct fb_switch` and the
badge slot of `struct fb_trailing` take one for the same reason.

`struct fb_card` is the container the others sit in: a titled panel that groups rows belonging to
one subject, which is what the [Status tab](#status--cards) is now made of. It is the odd one out
here because it is **declared and then drawn**, and the framebuffer forces that — a card's fill
has to go down before its text or it paints over it, and its height is not known until the last
row is in:

```c
struct fb_card card;
fb_card_begin(&card, FB_CARD_ELEVATED, MESH_UI_ICON_LINK, MESH_STR_STATUS_CARD_LINK,
              connected ? MESH_UI_TONE_SUCCESS : MESH_UI_TONE_ERROR);
fb_card_row_text(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_TRANSPORT, status);
if (handshake_valid) {
    fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_MY_NODE,
                MESH_STR_STATUS_MY_NODE, short_name, node_num);   /* formatted from the catalog */
}
fb_card_note(&card, notice_tone, notice->text);                   /* a wrapped paragraph */
fb_card_action(&card, MESH_STR_ACTION_DISCONNECT, focused);       /* a verb on the heading line */
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

A card states one of three **variants**, which is how a column of them gets a shape. There is no
alpha on this panel and nothing to cast a shadow into, so the three are three [surface
tiers](#surfaces-are-tiered) rather than three elevations — Material's tonal elevation, which is
also why they survive a light palette as well as a dark one:

| Variant | Fill | What it says |
|---|---|---|
| `FB_CARD_FILLED` | `SURFACE` | the ordinary weight, and the zero value |
| `FB_CARD_ELEVATED` | `SURFACE_HIGH` | the card to read first |
| `FB_CARD_OUTLINED` | `BG` | on screen because the set would be incomplete without it |

All three keep the hairline, including the outlined one whose fill *is* the ground.

A card can also carry up to three **verbs**, as text buttons against the far edge of its heading's
line — `fb_card_action()`, drawn at the chrome scale. They are on the heading's line rather than
in a row under the content because a row of their own costs a row of content, and on the one
screen dense enough to notice, the rows it cost were the ones that card exists to show. A card
holding the *selected* verb draws its edge in the primary and draws it thicker: that is the focus
ring, and it is derived from the buttons rather than declared, so the card and its verb cannot
disagree about which one the next press acts on.

The fill is the variant's and the edge is `MESH_UI_COLOR_OUTLINE`, and the edge is not
decoration: on every theme that ships, the surface is deliberately close to the ground — a
surface far from it is one body text is no longer validated against — so in daylight the
hairline is the whole of what says a card is there. `mesh_ui_theme_validate()` holds `OUTLINE`
against the ground and both surfaces for that reason, and holds the tones a card row can take
against every ground a variant can put them on — including `SURFACE_HIGH`, which the elevated
variant added. The inset is the `card_pad` metric in glyph-scale steps and
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

#### `struct fb_selection` — the checkbox and the radio

The other two answers to *which of these*, and one component: a circle is "one of these", a
square is "any of these", and the shape is the only part of either control a reader takes in
before they have counted the column.

```c
struct fb_selection sel = {.id = 0x04000000U | i, .on = current};
const struct fb_list_item row = {..., .trailing = {.kind = FB_TRAILING_RADIO, .sel = &sel}};
```

The kind names the shape, and `fb_draw_trailing()` writes it into the struct — a caller cannot
name a radio and be handed a checkbox. Everything else is the switch's: `id` keys the same
animation table, the geometry is the switch's height squared so the two stand the same distance
off a row's edges, and a control on the cursor's row lays its own ground first for exactly the
reason the switch does.

What separates it from the switch is what it *means*, not how it looks. A switch is a boolean
that acts — flick it and the thing it names is on. A radio is one of a set of alternatives, so
it says as much about the rows it is not on as about the row it is on; one radio alone is a
switch that has forgotten how to say off. That is why both live in a list's trailing slot and
the switch is the only one of the three that also makes sense on its own.

The "send to" picker is the first caller, and it is a correction as much as an addition. That
list marked the current target by giving its avatar a *stated accent fill* — which works for one
choice, does not generalise, and costs the row the very thing the disc is there for: a node is
the same two letters and the same colour everywhere in this client, and marking the target
overwrote the second of those on precisely the row the eye was hunting for. **Identity is the
leading slot's job and selection is the trailing slot's**, and a row that said both in one disc
was a row where turning the second on turned the first off.

#### `struct fb_segmented` — a small set of alternatives, all on screen

`fb_draw_chip()`'s own comment has said since it was written that a tab strip, a filter row and a
segmented control are one shape. This is the third: the same buttons, sized to a *share* of the
room rather than to their own words, inside one outlined container that says they are
alternatives rather than a row of separate offers.

```c
struct fb_segmented segmented = {.count = choices, .active = item.number, .value = item.value};
for (uint32_t c = 0U; c < choices; ++c) {
    segmented.labels[c] = mesh_ui_settings_enum_name(item.field, c);
}
const struct fb_list_item row = {..., .trailing = {.kind = FB_TRAILING_SEGMENTED,
                                                   .segmented = &segmented}};
```

Four things it decided that the next component of this shape will meet again:

- **It takes no input of its own.** Left and Right already step an `ENUM` field, the marker
  gutter already carries the pencil that says so, and a control that grew its own cursor would be
  a second opinion about a row the list is already highlighting. What the control adds is that
  the *set* is visible: `Metric` alone gives no sign that there is an `Imperial` behind it.
- **Equal shares, not chips.** A strip whose segments were each sized to their own words is a
  chip strip. What separates the two components is that these are alternatives — the eye has to
  compare them, and three boxes of three different widths read as three different kinds of
  thing. Each segment is `fb_button_width()` of the *widest* label, because that is the function
  that draws them.
- **Labels at `MESH_UI_TYPE_LABEL`, height at the row's scale.** The type role is Material's
  answer and is also what makes the component reach the settings it exists for — a segmented
  button spends its width `count` times over, and `Random PIN / Fixed PIN / No PIN` at body
  scale does not fit a value column at any scale that ships. The *height* stays the switch's at
  the row's own scale: two controls in one column have to stand the same distance off their
  rows, and it is the labels that are chrome-sized, not the control.
- **It is the one trailing slot with a second form.** Every other kind either fits or is
  dropped; a set of choices always has the chosen one in words to fall back on, so
  `struct fb_segmented` carries `value` — the same string the row would have drawn — and one
  function (`fb_segmented_cols()`) decides between the two for the measure and the draw alike.
  Two to four choices, because five equal shares of a value column are five clipped words; the
  thirty-eight regions and seventeen presets are still stepped one at a time, which is the
  honest answer for a set nobody can take in at a glance.
- **An `active` outside the set is drawn as the words too, and is never clamped.** A radio can
  report an enum value this build does not know — a newer firmware's, or a corrupt one — and the
  settings item keeps it and formats it as `Unknown`. Clamping it into range would have the panel
  state a configuration nobody reported; lighting no segment at all would say *none of these*.
  Both are claims. The words are what the row knows.

It is also what put `reserved` into `fb_trailing_cols()`. A headline is clipped from its *tail*,
so a slot fitted against the whole line eats the value first and then the label — and a label
column is the one thing on a settings row that may not move. Every slot is now fitted against
what is free rather than against the line; the narrow ones never reached it, and this one
reaches it at every scale.

#### `struct fb_meter` — a quantity as a length

`struct fb_meter` draws a track with a fill in it, and it is one component for two jobs that
`fb_widgets.h` predicted before either existed — *a meter and a progress bar want the same
table*. A meter reports a level that moves on its own; a progress bar reports a job that only
goes forwards and then stops. Nothing about the drawing differs, so there is one of them.

```c
struct fb_meter meter = {.id = 0x03000000U | i, .kind = FB_METER_DETERMINATE,
                         .value = permille, .tone = MESH_UI_TONE_PRIMARY};
const struct fb_list_item row = {..., .trailing = {.kind = FB_TRAILING_METER, .meter = &meter}};
```

A reading that is not already a fraction states the ends it is measured between, and the widget
normalises it — a battery on `{0, 100}`, an SNR on `{-20, +10}`. A zeroed scale is the identity
domain and means "already permille", so the call above costs nothing.

```c
struct fb_meter snr = {.value = (int32_t)node->snr, .scale = {MESH_UI_SNR_FLOOR, MESH_UI_SNR_CEILING},
                       .band = &node_snr_band, .tone = MESH_UI_TONE_SUCCESS};
```

It appears in two slots, and which one to use is a sentence about what the bar is for:

- **`FB_TRAILING_METER`**, a short bar against a list row's trailing edge, where a switch would
  go. Eight cells, which is enough to be read as a length and no more — a figure the eye passes
  on its way down a list. `MESH_UI_SETTING_METER` rows get one; the About screen's update
  progress is the first.
- **`struct fb_list_item.meter`**, a full-width bar on the row's *second step*, under the words.
  What the trailing slot cannot be: threshold marks land on top of each other in eight cells,
  and a domain with a negative end — a signal-to-noise ratio — has its whole interesting half
  inside two of them. This is the slot for a reading the screen wants judged rather than
  glanced at, and the node detail's four gauges are it. It costs the row a step, so the list has
  to have been told (`fb_list_begin_heights()`); a row that was not told simply draws no bar,
  because the alternative is painting over the row beneath it.
- **`FB_CARD_ROW_METER`**, a row inside a card. With a label it lines up with the field rows
  above it; with `MESH_STR_NONE` it takes the card's whole content width, which is the shape for
  a bar that is *about the row above it* — the Status card's airtime pair, where the words say
  how busy the air is and the bar under them says busy.

Four things are worth knowing before reusing it:

- **`id` is the animation's key**, on exactly the switch's terms. It matters more here: a
  determinate meter *eases towards* each value it is handed, which is what turns readings
  sampled a second apart into a bar that moves rather than one that jumps. Without an id it
  draws each sample exactly and stutters.
- **Indeterminate is a kind, not a zero.** When the extent of the work cannot be known — a
  request out on the network, a hash being taken — `FB_METER_INDETERMINATE` sends a pill
  travelling the track instead of inventing a fraction. It is driven by `mesh_ui_anim_loop()`
  and costs a repaint timer for as long as it is on screen, which is why a screen asks for it
  deliberately.
- **The track is its own role.** `MESH_UI_COLOR_METER_TRACK` exists because it is the one colour
  with a contract in both directions: findable on the two grounds a bar is drawn on (the body
  and a card) *and* distinguishable from every fill. `SURFACE_SEL` fails the first half on the
  light theme, where the cursor fill is within 1.2:1 of a card; `OUTLINE` fails the second half
  on the contrast theme, where it is the same near-white as the success colour. `mesh_ui_theme_validate()`
  holds both halves.
- **The fill takes any family tone**, and a tone that names no family is drawn in the primary.
  Every family is held against the track by `mesh_ui_theme_validate()`, so the widget's rule is
  a question about the *kind* of tone rather than a list to keep in step with the validator —
  which is what it used to be, and the list went stale the moment a fourth fill existed.
  `mesh_ui_tone_for_load()` answers with success, warning and error, so the figure's colour, the
  card heading's and the bar's fill are one sentence about one number rather than three
  thresholds that can drift apart.
- **A band is drawn, not just obeyed.** `struct mesh_ui_band` (in `theme.h`, beside the tones it
  answers with) says where a reading changes meaning, in the reading's own units. A meter given
  one takes its fill from `mesh_ui_band_tone()` *and cuts a notch into its track at each
  boundary* — which is the half that was missing. A fill turning amber at a quarter reports a
  threshold that cannot be located; a notch is that threshold, drawn where it is, so "is 31% a
  lot" becomes "past the first mark". The notch is the ground colour rather than an ink of its
  own, so it reads the same over the track and over the fill and needs no new contract.
  Order is meaning: `bad` above `warn` is worse as it climbs (airtime), `bad` below `warn` is
  worse as it falls (a battery), and there is no third case.

#### The slider

`struct fb_slider` is a quantity the reader is **choosing**, where the meter is a quantity they
are being told. That is the whole of what separates the two, and it is why this is not a flag on
the meter: a meter reports and eases towards each reading handed to it, while a slider says where
a value sits among the values that could have been picked instead, marks those choices on its own
track, and shows which one the cursor is on. The first is a picture; the second is a control, and
a control has states a picture has no word for.

What it replaced: a `MESH_UI_SETTING_NUMBER` row was Left/Right over a preset list with the
chosen value in the value column — `5m`, and nothing about whether five minutes is near the short
end of what the field offers or the long one. The figure still says *how long*; the track says
*how far along*.

```c
struct mesh_ui_settings_track track;
if (mesh_ui_settings_number_track(item.field, item.number, &track)) {
    struct fb_slider slider = {.id = 0x05000000U | (uint32_t)item.field,
                               .position = track.position, .stops = track.stops,
                               .unplaced = track.unplaced, .tone = MESH_UI_TONE_PRIMARY};
    const struct fb_list_item row = {..., .value = item.value, .slider = &slider};
}
```

It goes in the row's **bar slot** — the second step, where the node detail's banded meters go —
and not in the trailing slot, by the rule that slot was written with: *inline is for a figure the
eye passes, a step is for one it stops on*. A settings control is by definition the second kind,
and eight cells cannot hold a dozen stops any more than they could hold threshold marks.

Four things decide whether it is honest, and each of them is a way it was wrong first:

- **Only a field whose numbers measure something gets one.** `{0, 1, … 7}` is a hop limit under
  one field and a GPIO pin under the next, so nothing derives this: a field states it in its own
  table entry through `SCALE_PRESETS()` or `NAMED_PRESETS()` (`src/ui/settings.c`), and a
  spreading factor, a bandwidth, a coding rate, a pin and a count of coordinate bits all say
  *named*. Drawing a length across one of those is a claim about magnitude the number does not
  make.
- **The stops are evenly spaced, and a value between two of them is interpolated.** The preset
  lists climb geometrically — screen-on runs 15s, 30s, a minute, two, five, ten, fifteen, half an
  hour, an hour — so a handle at `value/3600` would crowd eight of the ten choices into the first
  sixth of the track. What is being chosen between is the *choices*. Interpolating between them is
  what an axis can do that a set of alternatives cannot: the segmented button had to fall back to
  words for a value outside its set, and a slider does not need to, because a radio reporting 42
  seconds lands where 42 seconds is.
- **Anything below the bottom stop is off the track, not at the bottom of it.** Two things reach
  that. Most of these lists open with a 0 the field reads as "whatever the firmware picks", and
  LoRa's transmit power reads it as "as much as this radio has" — neither is a quantity, and
  `SCALE_PRESETS_AFTER_ZERO()` stands it outside the scale. And two lists simply start above zero
  because what receives the setting refuses less (the public map drops a report under an hour, the
  firmware floors neighbour info at four), while a radio nobody has configured still reports 0.
  Either way the value comes back `unplaced` and the control draws its stops and no handle
  anywhere. This is §2.11's rule on an axis. Drawn the other way, `max` had its handle hard left,
  at the empty end of its own bar.
- **The step is reserved from the field, never from the value.** `unplaced` is the only value in
  the client whose row would otherwise be a different height, and a height that moved with the
  value would reflow the whole section under the cursor the moment somebody pressed Right off
  `default`. `ui_capture_slider_refuses_a_word` pins it from both ends: the row must differ
  between the two values and every row below it must be pixel-identical.

The drawing reuses what the meter already argued for. The active track and the handle are one
ink from one tone, `MESH_UI_COLOR_METER_TRACK` is the rest, the stops are notches cut out in the
ground colour — the same mark a band's boundary is, for the same reason, and drawn **after** both
halves of the track for the same reason too: a gap painted before the fill is a gap the fill
closes, which quietly ate every stop behind the handle — and the handle is
separated from the fill it ends by a gap in that ground, without which the two are one shape and
there is no position to read. The handle is narrower than its track is thick because it marks a
*position*, and a wide one is a range; it is drawn taller under the cursor, inside a box that
always reserves the taller size.

#### `fb_draw_signal()` — signal as rungs

A four-rung staircase, in the list row's trailing slot (`FB_TRAILING_SIGNAL`), for the one
reading a list is actually scanned for. The Nodes tab's trailing column was `4.2dB 3m`: a
figure whose scale nobody carries around, forty-two times down a screen. Rungs are compared
against the rungs above and below without being read at all.

Three things it does deliberately differently from the meter:

- **It quantises**, and does not ease between buckets. An SNR is measured off *one* packet, so a
  smooth bar would claim a precision the number does not have. `mesh_ui_signal_level()` in
  `layout.c` is where the ladder lives, so the buckets are arithmetic a test can reach rather
  than a chain of `if`s in a renderer.
- **It reads SNR, not RSSI** — the reading a cellular indicator would use. LoRa decodes *below*
  the noise floor, so received strength alone says nothing about whether a packet arrives: a
  loud band with a loud noise floor is a good RSSI and a dead link.
- **It is drawn only where the reading is this node's.** A node reached over relays has the last
  relay's SNR and one over MQTT has nothing that was on the air, so those rows keep their `Nhop`
  and `mqtt` text. A number can be wrong quietly; a picture cannot.

Unlit rungs are `MESH_UI_COLOR_METER_TRACK` — the role already contracted as "the empty part of
an indicator" — except under the cursor, where the track is the cursor fill on two of the four
themes and the row's own dim pairing is used instead.

#### `fb_draw_sparkline()` — a reading over time

The third quantitative component, and the first that is not about now. A meter says how much of
the air is in use; a staircase says how well we hear a node. Neither can answer "is it climbing",
and that is the question behind both of the ones this client is actually opened for — *is the
mesh getting worse*, and *is that battery going to last the night*. A level answers them only for
a reader who happened to look an hour ago and remembers what it said.

Most of the work is not the drawing. `struct mesh_ui_snapshot` is the present tense throughout —
this many nodes, this much air, this battery at this percent — so the client had nowhere to
remember anything. It has one now: `struct mesh_ui_series` in
[`layout.h`](../include/mesh/ui/layout.h) is a bounded ring of stamped readings, and
`struct mesh_ui_history` in [`history.h`](../include/mesh/ui/history.h) is which series the
client keeps. The store fills them as publishes arrive and hands a copy to the backends in the
snapshot; nothing is persisted, because the gap where the client was not running is not a silence
it can honestly draw.

Four rules, and each is a way a trend line can be wrong quietly:

- **The x axis is time, not the sample number.** Telemetry arrives on the radio's schedule and a
  reconnect resumes whenever it resumes. Four readings over ten minutes and four over four hours
  would otherwise be the same picture.
- **A gap is a break, not a slope.** A line drawn straight across the hour the radio was away
  claims readings nobody took, and it is exactly the hour a reader would want to see was missing.
  Each series carries its own `gap_ms`, because how long a silence is remarkable is a fact about
  the source: minutes for the radio's own report, hours for a node's telemetry broadcast. A
  silence is not the only discontinuity, though — a reading can be *refused* rather than missing,
  and the clock cannot see that one. A node on external power reports punctually and reports
  something that is not a level, so the source says so with `mesh_ui_series_break()` and the next
  reading starts a segment of its own.
- **The y axis is the reading's own domain** — the same `struct mesh_ui_scale` the bar beside it
  fills against, never the range these particular samples span. Auto-scaling is what a
  spreadsheet does, and on a battery that fell two percent overnight it draws a cliff. It is also
  what lets the line and the bar be read against each other.
- **Fewer than two readings is not a trend.** One is a level, and there is a component for that.
  A row with one reading draws no line and no floor: an empty box says the radio has gone quiet,
  which is a different claim from having nothing to say yet.

Nothing animates, unlike the meter and the slider. A meter eases towards each reading because the
new value *replaces* the last one; a series keeps them, so there is nothing to move between —
easing the newest point into place would show a shape that was never a reading.

It draws in one place — `FB_TRAILING_SPARK`, six cells against a row's trailing edge, for a
reading the eye is passing: the node detail's battery, beside the figure.

It shipped in two, and losing the second is worth recording because of *what* took it. There was
a `FB_CARD_ROW_SPARK` as well — the card's content width, two body rows, for a reading somebody
has stopped to look at — and its one caller was the Status tab's airtime, under the bar that
reads it. Then [`fb_draw_chart()`](#fb_draw_chart--the-axis-frame) arrived, and a whole
body with its axes labelled, its span named, its thresholds ruled across it and a second series
beside the first is what *"a reading somebody has stopped to look at"* actually wants. The middle
size was squeezed out from above rather than trimmed for room, and what is left is the honest
pair: **a row carries a glance, a screen carries a study.**

The reason a card row cost two where a meter costs none is still the rule, and it is why a card
draws levels and a shape gets a body: a meter's whole reading is a *length*, so it can be as thin
as the theme likes; a sparkline's reading is a *shape*, and a shape squeezed into the height of a
hairline is a hairline.

Two smaller decisions from doing it. The stroke is drawn a pixel column at a time rather than
with Bresenham's, because consecutive columns are then joined by construction — on an axis
measured in time, two readings a minute apart on a line spanning an hour land within a few
columns of each other, and a line rasteriser leaves a ladder of separated pixels there. And the
newest reading carries a square: a line has two ends and nothing about a stroke says which of
them is now, which on a trend is the whole reading.

The line takes a tone and the floor under it takes `MESH_UI_COLOR_METER_TRACK` — the meter's
pairing, so a trend costs no theme a new contract — and swaps the floor for the row's quiet ink
under the cursor, exactly as an unlit rung does and for the same reason.

Rendered by `make ui-capture ARGS="devtools/ui_capture/scenes/trend.scene"`.

#### A column of cards, and the one at the bottom

Cards are drawn top down and each takes the room it wants, so the **last** card pays for
everything above it — and `fb_draw_card()` pays by refusing a card it cannot fit rather than by
clipping it. Losing a card is worse than losing rows, and not only because it is more content: a
card carries **verbs**, and which verbs a screen offers is a table
([`src/ui/status.c`](../src/ui/status.c)) with no idea how tall anything came out. So the cursor
keeps walking onto a button that is not on the frame — which is exactly the failure *a card that
can end up with no rows must not be given a verb* names, reached from the layout side instead.

It was not hypothetical. The Status screen's Radio card carries `refresh`, and the airtime block
above it once cost four body rows — the figure, the bar, and a trend line that took two more and
appeared on the radio's *second* LocalStats report, a few minutes after connecting. The card was
already gone in ordinary use.

`fb_draw_card_reserving()` turns it around: the card that can afford to drop a row drops one, and
the card that would have vanished survives. Three things about it are decisions:

- **How much to reserve is a reading, not a constant.** `fb_card_min_height()` is the heading and
  one row — *this card exists and its verb is reachable* — and `fb_card_height()` is every row it
  holds. A card handed more than it needs will spend it, so the minimum is right for a card with
  nothing urgent to say. It is wrong for one whose rows only appear when something is broken:
  the Status tab's Radio card is a heading over a battery figure while the radio is well and five
  rows of explanation when it is not, so `fb_render_status()` reserves by `radio_tone` — the same
  reading that already picks that card's variant. Under a fixed minimum the Mesh card kept its
  message ring and the radio's own account of why nothing worked was clipped off the bottom.
- **The gap belongs to the reserver.** `*y` advances past a card's box *and* its gap, so the card
  being reserved for starts a gap lower than this one ends — `fb_draw_card_reserving()` adds its
  own `gap` to the reservation, and neither height function carries it. Counted once it is right;
  counted twice it costs a row of content, and left out entirely the card still does not fit.
  (`fb_card_height()` used to carry it and had no callers; a pair of reservation heights that
  disagreed about a gap is a trap rather than a distinction.)
- **A reservation that cannot be afforded is dropped.** It is a promise about the card below, and
  a promise that can only be kept by deleting the card above is not worth keeping.

It is also why `fb_render_status()` is the one screen where declaration order and drawing order
come apart: the Radio card is built into a local of its own so the Mesh card can be drawn knowing
what it has to leave behind. The rows the Mesh card gives up are the ones declared last, which is
the right order — a screen declares its least important rows last.

Pinned by `ui_capture_status_keeps_the_last_card_when_the_one_above_overflows`, which counts card
*edges* rather than rows: three cards is six bands, and a card refused for want of room is four.

#### `fb_draw_proportion()` — a whole and its parts

The fourth quantitative component, and the first that is not one number. A meter says how much, a
staircase says how well, a sparkline says which way; all three read a single figure. A
composition reads several that are parts of one, and until this existed a screen holding some
printed them as a list — `5428 new, 431 dupe, 12 bad`, three numbers with no sense of proportion
between them.

Which is the whole point of them. Upstream's own comment on the duplicate counter is *"if this
number is high, there are nodes in the mesh relaying packets when it's unnecessary"*, and high is
not a property of 431 — it is a property of 431 against 5,871. That is the meter's argument
(*"is 31% a lot?"*) on a whole with more than one part in it: a length is compared against the
lengths beside it, which are right there.

Four things about it are decisions rather than details:

- **It starts at three parts, because two parts is a meter.** A whole split in two is a fraction,
  a fraction is what `fb_draw_meter()` draws, and a meter can carry a domain and a band that this
  cannot. Heap free against heap total is a meter for that reason.
- **The parts must be disjoint, and that is the caller's promise.** Nothing here can check it:
  three counters that overlap still add up to something, and the bar drawn from them is a
  confident picture of a whole that does not exist. The radio's transmit counters are exactly
  that trap — `num_tx_relay` is a *subset* of `num_packets_tx` — which is why there is a bar
  under the Status card's Heard row and none under its Sent row.
- **The colours are the theme's series palette, not tones.** See [Themes](#themes): a part means
  nothing except which part it is, where a tone means good, bad or caution.
- **Nothing animates.** These are counters that only climb, so between two frames a boundary
  moves by a fraction of a pixel — the sparkline's reasoning arriving from the other direction.

The slices are laid out by `mesh_ui_proportion_split()` ([`layout.c`](../src/ui/layout.c)), which
is where the two rules that keep the picture honest live, and both are *a picture cannot be wrong
quietly*. They sum to the bar **exactly**, because a gap at the end of a bar that claims to be
everything is a part nobody named. And a part that is there is **never rounded away to
nothing**: three bad packets in fifty thousand is a quarter of a pixel, and drawn honestly that
bar reports a mesh with nothing wrong with it — so the part takes a unit off the longest one
instead. A part that really is zero still draws nothing, which is the same lie the other way
round.

It is drawn as nested pills, widest first, so the two ends of the bar are the meter's ends and
the only new edges on it are the boundaries between parts — and each boundary is a gap cut in
the ground, which is the band notch doing the same job one level along. The palette promises two
slices are 1.4:1 apart, and that is a difference the eye finds reliably when there is an edge to
find it at.

The bar carries no words: what names the slices is the order the row above names its numbers in.
That correspondence is the only legend a bar in a row's height has room for, and it is why
`catalog.def` and `locale_es.c` both carry a note saying the order is not a translator's to
change.

Rendered by `make ui-capture ARGS="devtools/ui_capture/scenes/shots/status.scene"`.

#### `fb_draw_chart()` — the axis frame

The fifth quantitative component and the first that is a **screen** rather than a slot. The four
before it fit in a row and pay for that by having no numbers on them: a sparkline is a shape, and
the reader has to already know what it is a shape of. That is the right trade in a list, where
the row above names the reading and the bar beside it says how far along. It stops being the
right trade the moment somebody stops to look — which is the press this exists for, and which is
why the roadmap's entry for it said its cost would be a *route* rather than a component.

The Status screen's Mesh card carries the verb (`MESH_UI_STATUS_VERB_TREND`), `nav->trend_open`
is the level, `MESH_UI_ROUTE_TREND` is the place, and
[`fb_render_trend()`](../src/ui/backends/fb_screens.c) is a dozen lines because a chart has no
rows to measure and no cursor to place.

What the room buys, in the order it matters:

- **The vertical says what it is measuring.** Its two ends are labelled, in the reading's own
  units, so *high* is a number rather than a feeling. The domain is still the reading's own
  `struct mesh_ui_scale` and never the range these samples happened to span — the sparkline's
  first rule, and it is *more* load-bearing here: an axis with numbers on it gets believed, so an
  axis that rescaled itself would be a labelled lie rather than a misleading shape.
- **The horizontal says how long.** A shape with no time under it cannot distinguish a battery
  that fell ten percent in an hour from one that fell ten percent in a week.
- **The thresholds are drawn.** The band `fb_draw_meter()` cuts notches into becomes a broken
  rule across the plot, so where a reading stops being comfortable is a line the trend can be
  *seen* crossing rather than a colour that changed at a moment nobody can locate. This is the
  one thing a chart says that no row-height component can.
- **More than one line fits**, which is what the legend is for — and why two lines could not be
  drawn in a row: the words naming them have to be somewhere.

Three things it deliberately does not do. **No end mark**: a sparkline marks its newest reading
because a stroke does not say which end is now, and a chart has the answer written under it.
**No grid**: two threshold rules and two axes are the marks that mean something, and a lattice of
evenly spaced lines makes a picture look measured without measuring anything. **Nothing
animates**, for the sparkline's reason.

Two rules came out of building it, and both are about what a *fill* is:

- **Two lines on one picture share one window.** `mesh_ui_series_project()` stretches a series
  across its own span, which is right for a line drawn alone and wrong the moment there is a
  second one — a series that stopped reporting half an hour ago would be drawn as though it were
  still arriving, and on this screen that is our transmit share climbing to meet the channel's
  total. `mesh_ui_series_window()` takes the union of the series' clocks and
  `mesh_ui_series_project_over()` places every line on it; the first is now written in terms of
  the second.
- **A chart's pen is thicker than a sparkline's, and that is the palette's contract rather than
  a preference.** A series colour promises 1.4:1 against the grounds and against its neighbours,
  and that was measured on a *bar* — it is why the palette may never be an ink. A hairline in one
  of those colours is a line the reader has to hunt for, so `fb_chart_stroke()` is at least twice
  `fb_spark_stroke()`: the room a chart has is spent making the mark wide enough to be the fill
  the palette was validated for.

The legend is a swatch and a word per line, the swatch in the series colour and the word in the
row's own ink — the same rule, from the other side. Rendered by
`make ui-capture ARGS="devtools/ui_capture/scenes/chart.scene"`.

#### `struct fb_snackbar` — the transient notice

`mesh_ui_store_set_toast()` raises a one-line notice: *Sent to BRVO*, *Not connected*,
*Rebooting*. It used to be drawn as primary-coloured text on the footer's **second line**, which is also
where the link summary lives — so for the four seconds after every action the frame stopped
saying whether there was a radio attached. Two unrelated facts were taking turns on one row
because the notice had nowhere else to be.

It has somewhere now: a container of its own, drawn **last of everything on the frame**, that
slides up from below the panel and slides back down when the nav drops it. The shape is the
point. A notice that appears in place has to be noticed before it can be read, and on a handheld
the eye is usually somewhere else at the moment it appears; movement is what brings it back.

```c
const struct fb_snackbar snackbar = {
    .text = snapshot->nav.toast,
    .until_ms = snapshot->nav.toast_until_ms,
};
fb_draw_snackbar(state, &layout, &snackbar);
```

Three things are worth knowing:

- **The backend keeps the words, not just the position.** A snackbar leaves by sliding out, and
  by then the store has already forgotten the text — `mesh_ui_nav_tick()` clearing an expired
  toast is the very thing that causes the frame where it starts leaving. So
  `mesh_ui_backend_fb_state` carries a copy alongside the animation table, for the same reason
  the table is there at all: it is presentation, and it dies with the frame buffer.
- **`until_ms` is identity, not a deadline.** Expiry is the nav's business. Two notices can read
  the same — pressing send twice with no radio raises *Not connected* twice — and the second has
  to arrive rather than sit there looking like the first never left. The deadline moves every
  time one is raised, so it tells them apart when the words cannot.
- **A press cannot date one.** `mesh_ui_store_set_toast()` takes the clock from its caller — the
  app, which has `mesh_time_monotonic_ms()` — but a notice raised *inside* a key press
  (`mesh_ui_nav_raise_toast()`, which is what a refused press uses) has no clock to hand, and
  reading the real one there would be wrong in a capture: the harness ticks the store with a
  synthetic clock that starts at 1000 and moves only when a scene says `hold`, so a real-clock
  deadline is four seconds on the device and longer than any scene on a host that has been up an
  hour. `mesh_ui_store_handle_key()` dates it instead, from the clock the store was last ticked
  with, inside the same call — before anything is drawn, because a frame carrying an undated
  notice would read as a *different* notice a frame later and restart the entrance below.

- **Entering has to be forced.** The animation table adopts its target on first sight and treats
  re-aiming at the current target as a no-op, which is exactly what stops a switch sliding on
  the frame a screen opens. A snackbar wants the opposite, so on a new notice it is put back to
  zero with a zero duration before being aimed at the resting place.

It is sized to its own words rather than to the panel, up to two lines, and **centred** — a bar
the full width of the screen is a region of the chrome, and one pinned to the leading margin
reads as the start of a row that ran out of things to say, which is exactly what the footer line
it replaced was. Centred, it is one object placed over the screen, and it stays put as the
wording changes length instead of growing rightwards out of a fixed corner.

`SURFACE_INVERSE` carries it with no outline: a card needs a hairline because its fill is one
step off the ground, and this one is the furthest from the ground the theme has.

#### `fb_draw_app_bar()` — the top app bar

A screen's heading used to be a `const char *`, so everything a heading had to carry got glued
into that string. Settings built `"Settings > %s%s%s"` out of two catalog entries, with the
unsaved marker arriving as the third `%s`.

That is a whole-sentence string id doing structural work — the same mistake the button hints
were, and worse here on three counts. The `>` separators handed a translator the breadcrumb's
*grammar* along with its words. A count glued in with `%s` cannot be a badge. And at the title's
glyph scale `Settings > Modules > Telemetry` is thirty of the thirty-four cells on the line, so
the part that was elided when it overran was the leaf — the one word naming *this* screen.

The bar has four slots and a screen fills the ones it needs:

```c
struct fb_app_bar bar = {.title = mesh_ui_settings_section_name(section)};
if (nav->settings_parent != MESH_UI_SETTINGS_NO_SECTION) {
    bar.trail[bar.trail_count++] = mesh_ui_settings_section_name(nav->settings_parent);
}
if (nav->settings_edit_count > 0U) {
    mesh_str_format(unsaved, sizeof unsaved, MESH_STR_SETTINGS_UNSAVED, edits);
    bar.badge = unsaved;
    bar.badge_family = MESH_UI_FAMILY_WARNING;
}
fb_draw_app_bar(state, layout, &bar);
```

| Slot | What it holds |
|---|---|
| leading | the back affordance — **not a field**; see below |
| overline | `trail[]`, one entry per level, separated by a drawn `MESH_UI_ICON_CHEVRON` |
| title | what this screen is, one line at `MESH_UI_TYPE_TITLE` |
| trailing | `badge` + `badge_family`, a capsule saying one thing about the whole screen |

Three things doing it settled, and each is a rule the next screen meets again.

- **The trail names only the levels between the tab and here.** The navigation bar is already
  saying `Settings`, selected, three rows above; a trail that repeated it would spend a body row
  on a word the frame already carries. So a top-level section has no overline at all, the node
  detail has none (`Nodes > Bravo Creek` became an arrow and a name), and what is left is the one
  level nothing else says: `Modules` over a module's own section, `Channels` over one channel.
  The general form of the rule: **an overline says only what nothing else on the frame says.**
  It is why the thread keeps `channel` in its title rather than above it — a channel's name
  already begins with a `#`, and every bubble under it is tagged.
- **The arrow is derived, not declared.** `layout->back` comes from
  `mesh_ui_action_bar_goes_back()`, which looks for `MESH_STR_ACTION_BACK` in the bar
  [`src/ui/actions.c`](../src/ui/actions.c) has already built — so the arrow at the top of the
  panel and the `B` keycap at the bottom read one table and cannot disagree. That also makes it
  exactly as conditional as the press is, which a flag on a screen would not be: a settings
  section holding edits offers `B` as *discard*, and the arrow correctly goes away.
- **A badge is a component now.** `FB_TRAILING_BADGE` used to fill its own capsule inside the
  trailing slot; it and the app bar both call `fb_draw_badge()`, because two places drawing their
  own round rect are two capsules that drift. The caller supplies the box — a list row's slot is
  its cursor fill, which is one shape on a one-line row and another on a two-line one, while the
  app bar's is centred on the title's glyph body.

`make ui-capture ARGS="devtools/ui_capture/scenes/app-bar.scene -o app-bar.gif"` walks all four.

#### `fb_draw_nav_bar()` and `fb_draw_action_bar()` — the chrome

The tab strip and the two lines under the body were the last two screen-level renderers laying
out their own pixels, and they were also the two pieces of chrome that most made the UI read as
a terminal rather than as a handheld OS. Both are components now, and `fb_screens.c` is left
holding only the *content*: which tabs there are, which one is up, and what the bottom line has
to say about the radio.

The **navigation bar** is a recessed surface, a chip strip, and the rule that closes it off. The
strip underneath it is the reusable half — `fb_draw_chip_strip()` takes an array of
`struct fb_chip`, an active index and the room it has, and owns the label elision described
under [icons](#srcuiiconc--the-generated-srcuiicon_glyphsc). It was private to `fb_screens.c`
before, which meant a filter row on Nodes (*All / Direct / Favourites*) would have had to
re-derive the measuring loop — the exact duplication `fb_chip_width()` was added to prevent.

The **action bar** is the same surface on the other edge, holding a keycap and a verb per
action, over the line that says what the transport is doing.

```c
struct mesh_ui_action_bar actions;
mesh_ui_actions_for(snapshot, &actions);
const struct fb_action_bar bar = {
    .items = actions.items,
    .count = actions.count,
    .status = mesh_ui_line_text(&summary),
    .status_tone = summary_tone,
};
fb_draw_action_bar(state, &layout, &bar);
```

**The catalog change was the work, not the drawing.** The hints were whole localised sentences —
`HINT_NODES` was `"A open node  X pin  Y write  L/R tabs"`, one entry — and a sentence is the
right shape for exactly one renderer: a single line of text. The moment the buttons are drawn as
keycaps the renderer needs the letters and the verbs *apart*, and the only place they were apart
was inside a translation. Going looking for them there is the one thing `src/i18n` exists to
prevent.

So the answer is the one the colours, the icons and the shapes all got: a screen names a token
and a table answers. The token is a `(button, verb)` pair, and the table is
[`src/ui/actions.c`](../src/ui/actions.c) — which lives in the UI layer rather than beside this
backend because *which buttons mean something in a given state* is a fact about the nav, not
about drawing. Three things follow from that:

- **A cap is not translated.** `mesh_ui_button_cap()` answers with what is printed on the case
  — `A`, `START`, `L/R` — for the same reason a region code stays as it is. The two directional
  pairs are drawn from the font's arrows rather than spelled `Up/Down`, and `MESH_UI_BUTTON_QUIT`
  is the one cap that is not a constant: `mesh_ui_input_quit_cap()` says `MENU`, or a bare key
  code when `MESHCLIENT_QUIT_KEYS` has moved it somewhere with no printed name.
- **The order is priority.** A bar that does not fit drops from the *end*, so each table is
  written with the press the screen is for at the front and `L/R tabs` — true everywhere, and so
  the least worth the room — at the back. Nothing is clipped: half a verb is a button whose
  meaning has to be guessed.
- **It is testable now.** `fb_render_snapshot()` used to decide which buttons meant something
  in a branch alongside the one picking a renderer — the same conditions written out twice, so a
  screen that grew a press had two places to remember. `tests/suites/ui_actions.c` walks every
  combination of overlay and screen and holds the tables to the things a screenshot would not
  catch: that an armed destructive action says so, that no table overruns
  `MESH_UI_ACTIONS_MAX`, and that every verb named is one the catalog has.

#### `fb_draw_progress()` and `fb_draw_banner()` — what the *client* says

Everything else on the panel belongs to something. A row describes a node, a card describes the
link, the app bar names the screen it heads. Two statements do not: *a newer release is out
there* and *something is in flight right now* are true of the client, on whichever tab you
happen to be looking at — and until these two components existed the first was visible only
inside Settings > About and the second was not visible anywhere.

So they are chrome, drawn once by `fb_render_snapshot()` around whichever screen is up. What
they *say* is decided in [`src/ui/chrome.c`](../src/ui/chrome.c), for the reason the action
bar's verbs are decided in `src/ui/actions.c`: which states are worth a notice is a fact about
the client, not about a framebuffer, so a second backend gets the same two answers and a unit
test can ask the questions without a panel.

```c
fb_draw_progress(state, &layout, mesh_ui_chrome_busy(snapshot));   /* costs no row */

struct mesh_ui_banner banner;
if (mesh_ui_chrome_banner(snapshot, &banner)) {
    fb_draw_banner(state, &layout, &(const struct fb_banner){
        .icon = banner.icon,
        .text = mesh_str(banner.text),
        .supporting = mesh_str(banner.supporting),
        .detail = banner.detail,                                   /* a version, untranslated */
        .family = banner.family,
    });                                                            /* costs the rows it takes */
}
```

**The split between them is the rule that keeps either from being noise.** The bar is for what
is *moving*: it costs no row, it says nothing about what, and it goes away on its own when the
work lands. The banner is for what has *settled*: it stays until something resolves it, so it
has to be worth the rows it takes. A state that is one of them is never the other — which is
why the three updater states in flight raise the bar and the two settled ones raise the banner,
and why the roadmap's *radio disconnected* is neither.

**The bar never moves the body.** The navigation bar already leaves a gap between its rule and
the first body row, and the bar hangs in it — so `layout` is `const` in that call, a list gets
the same rows whether or not anything is in flight, and a save going out does not reflow the
screen it was saved from. It is the same rule the card's focus ring is painted by: an indicator
that changes the layout is an indicator that moves what it is pointing at.
`tests/suites/ui_capture.c` pins both ends of it — the busy frame and the quiet frame must stop
differing inside the top eighth of the panel, and the banner frame must differ all the way down.

It is `fb_draw_meter()` at `FB_METER_INDETERMINATE`, full bleed and one hairline tall, rather
than a drawing of its own. There is exactly one "a thing is working" motion in this UI and a
second implementation of a travelling pill is a second one to keep in step with the theme's
timings. Full bleed because it is the navigation bar's rule saying something: inset by the
margin it would read as the first row of the body, which is the mistake the tab strip made
before it was given a surface of its own.

**The banner sits above the screen's own app bar**, which is not where Material puts one, and
the reason is what each piece of chrome belongs to: the navigation bar is this client's
app-level chrome and the top app bar is the *screen's* heading, so a statement about the client
goes with the first. The practical half of the same answer — drawn below the app bar it would
have to be called by every screen renderer, and by each of the four overlays, which is the
duplication `fb_render_snapshot()`'s single tail exists to prevent.

Nothing about it animates. The container consumes body rows, so a height that eased open would
reflow the list underneath it for the length of the animation — and unlike the snackbar, which
arrives *over* the UI and has to be noticed to be read, a banner is read whenever the eye next
reaches the top of the panel.

Three rules decide what the table in `chrome.c` may raise, and they are why it is shorter than
the audit expected:

- **A banner says only what nothing else on the frame says.** This is the app bar overline's
  rule arriving somewhere else. It refuses *radio disconnected*, which the status line under the
  keycaps reports on every frame anyway, and it is why the update banner stands down inside
  Settings > About: the section it points at states the same thing in more detail, so a banner
  over it is the client telling you something you are already reading.
- **A banner must resolve.** There is no dismissal, because dismissal needs somewhere to
  remember what was dismissed, a press to spend on it, and a rule about when it comes back —
  a nav change with a component on the end of it. So nothing is raised that cannot go away on
  its own terms. That is what refuses the radio's own `ERROR` notice, which is kept until the
  link cycles, and it is what makes `update_can_install` part of the gate rather than a detail:
  an update a build is not allowed to install is a banner nothing the user does can clear.
- **A modal owns the body.** Nothing is raised over the confirm dialog, the picker, the keyboard
  or the compose sheet. Those four take the body for a question, and a container that shortened
  the body while one was up would move the question as it was being answered. The bar keeps
  running under all four, because it has nothing to move.

`make ui-capture ARGS="devtools/ui_capture/scenes/banner.scene -o banner.gif"` shows both: the
bar running with the list unmoved under it, the banner following you across the tabs, and it
standing down on the one screen that already says the same thing.

The banner's slots are the app bar's lesson applied again: a headline string id, a supporting
string id, and a `detail` that is a runtime string rather than an id — a version number, in the
same category as a region code (see [i18n](i18n.md)). Keeping the version out of the words is
what lets the headline be one short translatable phrase instead of a format string with a number
glued into it. The supporting line is the half that goes when the body cannot spare a row for
it: the headline says what is true, the hint says where to go about it, and a hint over an empty
screen is worse than no hint.


### Animation

A frame is a function of a snapshot, and a snapshot has no notion of *was*: it says a switch is
on, never that it has just become on. Two pieces supply the difference.

**`mesh_ui_anim_loop()`** answers a different question from the rest of the module. Everything
else says "this value has changed, where is it on the way?"; a loop says nothing has changed,
nothing is going to, and the widget still has to move — which is what an indeterminate progress
bar needs. It is a sawtooth derived from the clock modulo a period, so a missed frame costs
nothing, and because it never finishes, `mesh_ui_anim_table_active()` reports it as running for
`MESH_UI_ANIM_LOOP_STALE_MS` after the last frame that drew it. That is what stops a slot left
behind by a widget that scrolled off screen from pinning the repaint timer on for the rest of
the run.

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

#### Screen transitions — `mesh/ui/route.h` and `fb_shift_begin()`

Every animation above is a *control* moving: a switch's knob, a meter's fill, the snackbar's
rise. Moving between two screens is the one animation whose subject is the whole body, and it is
the only one that needs something no frame contains — **which way the user went**. Opening a node
and backing out of one are the same two frames in the opposite order.

**`include/mesh/ui/route.h` answers it by deriving, not by recording.** A *route* is where the
nav is: a `depth` (0 is a tab's own list, and every level opened over one adds one), the `screen`
that orders two places equally deep, and enough of the level's own subject to tell two of them
apart. `mesh_ui_route_move()` compares two and says forward, back, or neither. **A change of tab decides
first**, by the shorter way round the strip: L/R work from a nested screen, so Right off an open
node detail is one tab rightwards *and* a level shallower at once, and the strip is on the panel
above the body already saying which way that went. The strip is a ring too — `switch_screen()`
wraps — so measuring both ways round is what keeps Right off the last tab a step rightwards
rather than the largest leftwards move there is. **Within one tab the hierarchy decides**, which
is what the four transitions worth animating actually are.

The audit ([`components-roadmap.md` §2.16](components-roadmap.md)) expected a *field* on `struct
mesh_ui_nav` instead, written by every call site that opens or closes a level. There are eleven
of those and nine that close one, and a new one that forgot to set it would animate the wrong way
round — which is not a crash, fails no build, and is invisible in a screenshot. This is the top
app bar's back arrow one level up: **a second opinion about the nav is a second opinion that can
be wrong.** Nothing in the store or the nav records how it got here.

A route deliberately excludes the cursor, the draft and every armed press. Those change
constantly and change no *place*; a route that included them would restart the slide under the
user's thumb on every press of Down.

**The "was" lives in the backend**, on `struct mesh_ui_backend_fb_state` beside the animation
table and for the same reasons — it is presentation, it dies with the frame buffer, and a second
backend may animate differently or not at all. `fb_transition_offset()` compares the route this
frame is drawing against the one the last frame drew, and on a change starts a
`MESH_UI_MOTION_MEDIUM` ease-out. First sight adopts, so nothing slides on the frame the client
comes up.

**Only the arriving screen is drawn, and it travels a quarter of the panel.** There is no alpha
here and nothing can read back what is already on the panel, so a cross-fade is out and so is
carrying the outgoing screen along beside the incoming one. A full-panel travel was tried first
and was wrong for a reason a still cannot show: with only one screen drawn, the body is *empty*
on the frame the press lands — one blank frame, every time. A quarter is also what Material's
shared-axis transition displaces, arrived at from the other end: there the slide only says which
way because the cross-fade carries the change of identity, and here the short travel keeps the
content legible for the whole of the move.

**`fb_shift_begin()` is the one transform in the drawing layer**, applied inside
`fb_fill_packed()` — which every pixel this backend writes goes through, so it covers glyphs,
icons, emoji, fills and rounded corners at once, and covers anything added later without being
told to. A screen renderer and a widget know nothing about it; threading an offset through them
would be putting a pixel coordinate back into the layer that exists not to have one.

**What slides is what changed.** The transform wraps the screen renderer only, so the navigation
bar still names the tab it named and the keycaps change their verbs without travelling. The
screen progress bar and the banner sit inside the band and are drawn *before* the transform, for
the same reason: both are about the client rather than about the screen that is arriving.
`fb_animation_damage()` is handed the whole band, because everything in there is a function of
the clock while a move runs and the partial-redraw path assumes the opposite of anything it has
not been told about.

Two consequences worth knowing. **A theme that asks for no motion gets none** — a zero
`MESH_UI_MOTION_MEDIUM` puts the value straight on its target, so the screen lands in place on
the frame it arrives. And the ease is **derived from the clock, not accumulated over frames**, so
a panel that cannot hold 30 fps through a full-body redraw gets a coarser slide rather than a
longer one.

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

Everything above therefore packs once and then describes rectangles. `fb_draw_glyph` resamples
the glyph's coverage into its cell and fills one span per run of equal coverage; `fb_draw_emoji`
packs the 255-entry sprite palette once per pixel format, precomputes the nearest-neighbour
column map so the scaling division runs per column rather than per pixel, and coalesces
equal-index neighbours into spans. **A per-pixel drawing helper is a regression** — it was one,
and reintroducing it costs about 5x on every text frame.

The coverage ramp is the same trick one level up: `fb_blend_table()` quantises ink-over-ground
into `FB_BLEND_STEPS` packed colours *once*, so blending is one multiply per channel per step
rather than per pixel, and `fb_draw_text` builds it once for a whole run rather than per
character. Text and icons share it, because they are the same operation.

### A glyph is coverage

`struct mesh_ui_glyph` holds one value per pixel, `0` to `MESH_UI_GLYPH_MAX_ALPHA`, exactly as an
icon sprite does — not one bit per pixel. That is not a refinement, it is the difference between
a UI that can look modern and one that cannot: at the body scale a source pixel is a 4×4 block,
and no amount of Material chrome survives text made of visible squares.

A font declares two sizes. Its **cell** (`width`/`height`, in scale steps) is what every
measurement above derives from. Its **master** (`master_w`/`master_h`) is the resolution the
coverage is stored at, and `fb_draw_glyph` resamples one into the other:

| | `5x7` | `ui` |
|---|---|---|
| cell at scale 1 | 5×7, gaps 1 and 2 | 5×8, gaps 0 and 1 |
| master | 5×8 | 20×36 |
| sampling | `MESH_UI_FONT_PIXEL` (nearest) | `MESH_UI_FONT_SMOOTH` (bilinear) |
| at the body scale | 42 columns × 21 rows | 51 columns × 21 rows |

The sampling is declared rather than inferred, and it is not a preference: pixel art resampled
bilinearly reads as a smudge, and an outline resampled with nearest neighbour drops every fourth
row. On the integer ratio a pixel font is drawn at, nearest is exact block replication — which is
why moving 5x7 onto this path left every frame pixel-identical.

`master_top` is the **overhang**: master rows drawn *above* the cell, where a diacritic goes when
the cell has no room for it. Both fonts need it — 5x7's capitals fill all seven of its rows, and a
face rasterised to fill its cell puts an acute above the cap height by definition — so it is one
mechanism rather than the single hard-coded accent row it replaced. A font sizes the overhang by
its own `line_gap`.

`cap_rows` is how tall the capitals actually stand. Anything sized to match the text — an icon in
a row slot above all — uses `mesh_ui_font_cap()` rather than the cell height. For 5x7 the two are
the same, which is why the cell height stood in for it until a face with real ascenders and
descenders arrived and every icon came out a seventh too big.

Text is drawn in `ink` **over `ground`**, and `ground` is a parameter for the reason
`fb_draw_icon`'s is: what is already on the panel is not readable from the drawing code, and a
widget that has just filled a row is the only thing that knows what colour it filled it with.
Getting it wrong does not lose the text — it puts a faint halo of the wrong colour around it.

### `src/ui/font_ui.c` — the generated face

`scripts/gen-font.py` rasterises JetBrains Mono (SIL OFL 1.1, `licenses/`) into
`src/ui/font_ui_glyphs.c`, which is **committed**; the build rasterises nothing and never reaches
the network, exactly as the icon set works. It is not part of the build — run it by hand and
commit the result.

The face is rasterised at the size the device draws it (a 20×36 master for a 20×32 cell at the
body scale), so on the Brick a glyph is blitted 1:1 and resampled only when a theme asks for
another scale. Each glyph is stored as its ink box, packed four bits a pixel: 50 KB, against 95 KB
for the run-length encoding the icons use — a letter at this size is a small dense patch of
varying coverage rather than the long flat runs a filled symbol is.

The coverage set is **whatever `src/ui/font5x7.c` can draw**, parsed out of it by the generator
rather than listed twice; `ui_theme_fonts_agree_on_coverage` holds the two to that, because a
face that covers less turns a node name into a row of boxes only for the people whose names need
the letters it dropped. `EM` and `BASELINE` were found by search, not arithmetic — the binding
constraint is a different character at each end (an accented capital above, C-cedilla below), and
neither is the one the face's own ascent and descent metrics describe.

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

### `src/ui/font5x7.c` — the pixel face

The original framebuffer font, still registered and still selectable by a theme, reached through
the `struct mesh_ui_font` descriptor it publishes (`mesh_ui_font5x7()`) and keyed by **codepoint**
rather than by byte: ASCII plus Latin-1 Supplement and Latin Extended-A. Accented letters are
**composed** from a base letter and a mark (`k_composed`) rather than drawn, so adding one is a
line. Lowercase leaves rows 0 and 1 of the cell free and the mark goes there; capitals and
ascenders fill all seven rows, so their mark collapses to a one-row silhouette, which the font
publishes as row 0 of its master — the overhang every font now has.

Its tables are still 1-bit, and the descriptor widens them to coverage on the way out (0 or
`MESH_UI_GLYPH_MAX_ALPHA`, nothing between). That is what keeps it a font you can edit five hex
bytes at a time, and it is why it comes out of the resampler as exactly the spans it always drew.

Consequences of a seven-row cell, all deliberate and all this font's alone: circumflex, caron,
macron and ring are indistinguishable over a capital, and marks that sit *under* a letter have
nowhere to go, so `Ç` draws as `C`. The `ui` face has room for both. Anything with no glyph gets
the replacement box — except what the emoji table covers.

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

### `src/ui/icon.c` + the generated `src/ui/icon_glyphs.c`

Monochrome Material Symbols, in the slots that used to hold `>`, `*`, `#` and `+`.

The set is one line per icon in `include/mesh/ui/icons.def`, included twice to build
`enum mesh_ui_icon` and the name table, and read a third time by `scripts/gen-icons.py` to decide
what to rasterise — the same trick `catalog.def` plays for strings, and for the same reason:
the enum, the sprites and the generator cannot drift apart. Adding an icon is a line there plus
a regeneration.

Two things separate icons from [emoji](#srcuiemojic--the-generated-srcuiemoji_glyphsc), and they
are why there is a second sprite table rather than a bigger first one:

- **An icon carries coverage, not colour.** It is drawn in the row's ink — the tone on the
  ground, `TEXT_ON_SEL` under the cursor, the quiet pairing in a trailing slot — because it is
  doing the job the character it replaced was doing. An emoji carries its own palette, which is
  most of what makes one recognisable at 20 px. There is deliberately no way to ask a slot for
  an icon in some other colour: a screen that wants one to shout gives the *row* a tone.
- **An icon is named by the UI, an emoji is looked up by codepoint.** `MESH_UI_ICON_CHEVRON`
  is chosen by a renderer; an emoji arrives inside a name somebody typed.

Coverage is 4 bits per pixel over a 32x32 sprite, run-length encoded — about 15 KB for the whole
set, most of which is the icon per settings section the leading slot spends. Sprites are larger than the cell they usually land in (28 px at the body scale, 21 in the
chrome) so that the one place that draws an icon *big* — the symbol on an empty screen — is not
resampling a thumbnail. `fb_draw_icon()` samples them **bilinearly** and blends between an ink
and a ground the caller passes in, which is the one place this differs from the emoji path's
nearest neighbour: dropping or duplicating source rows turns a 2 px chevron stroke into a
staircase. The ground has to be passed because nothing
can read what is already on the panel — the Brick's display engine composites `fb0` against its
own layer, and a row is drawn on the ground on one line and on the cursor fill on the next. A
widget that has just filled a row knows the colour it filled it with; nothing else does.

An icon occupies **exactly one text cell** so the layout above it keeps counting in columns, and
is *drawn* so that its **symbol** stands at the glyph body's height, centred on that cell — a
symbol the width of a cell advance comes out visibly smaller than the capitals it is labelling,
and overhanging the gaps either side by a pixel or two costs nothing. The symbol rather than the
sprite, because a sprite is a window a little wider than Material's grid
(`MESH_UI_ICON_WINDOW`) with the shape inside the central `MESH_UI_ICON_BODY` of it and air
around the outside: `fb_icon_drawn()` scales the height it is matching by that ratio, so what
lands on the capitals' height is the shape and what overhangs is the air.

The chip strip is the one place that measures before it draws: five labelled tabs fit at the
scale the device ships with and do not at the two above it, so `fb_chip_strip_fit()` asks
`fb_chip_width()` for the total and drops to labelling only the selected chip, then to no labels
at all, rather than pushing a tab off the edge of the panel. That is Material's "selected" label
mode, arrived at by measurement.

`scripts/gen-icons.py` rasterises Material Symbols Rounded (Apache 2.0, `licenses/`) and is
**not part of the build** — run it by hand and commit the result, exactly like the emoji
generator. It renders filled at weight 500, because an outlined symbol is a 1 px stroke by the
time it is 16 px across and thins to nothing where it curves.

Two things about the crop are worth knowing, because getting either wrong cuts every symbol in
the set and the miss reads as "that is just what the icon looks like":

- **It is measured off the baseline.** Material sits its 24 grid on the baseline, one em tall,
  while PIL's default text anchor is the *ascender* — and this face's ascender is a tenth of an
  em above the em box. Treating the two as the same put the window a descender too high and
  shaved the bottom off everything the UI drew.
- **The window is the whole grid plus a little air**, not the central 20 the symbol usually
  occupies. Usually is not always: at this fill and weight the full-bleed symbols (`hub`, the
  antenna, the warning triangle) reach the grid's edge and a hair past it. The air is free,
  because `fb_icon_drawn()` scales it back out at draw time.

The generator fails the run if a glyph's ink reaches outside the window, and
`icon_sprites_are_not_cropped` in `tests/suites/ui_icon.c` holds the committed data to the same
contract — a cut leaves a saturated edge on the sprite, which is a thing a test can see and a
person reviewing a 32x32 hex table cannot.

## Themes

Everything that makes the UI *look* like something — the palette, the margin, the glyph
multiplier, the font — is one table in `src/ui/theme.c`, and nothing that draws holds an opinion
of its own. `MESHCLIENT_THEME` picks one (`dark`, `light`, `contrast`, `colorblind`); `dark` is
the palette the device has always drawn and is unchanged.

Three vocabularies, from most abstract to least:

| Layer | What it is | Who speaks it |
|---|---|---|
| **Tone** (`enum mesh_ui_tone`) | what a piece of *text* means: normal, dim, strong, and one per family | screens, and every widget that takes text |
| **Family** (`enum mesh_ui_family`) | what a *fill* means: primary, secondary, tertiary, success, warning, error | widgets that fill something — a button, a switch, a badge, a bubble |
| **Role** (`enum mesh_ui_color`) | what a colour *does*: the ground, the fill under the cursor, the ink on an error container | widgets, for the neutral spine |
| **RGB** (`struct mesh_ui_rgb`) | an actual colour | `src/ui/theme.c` and `fb_fill_packed()`, nothing between |

A tone resolves to a role and a role resolves to an RGB, both through the theme. `fb_color()`,
`fb_tone_color()` and `fb_paint()` on the backend state are the only path, which is what makes a
switch total: a renderer cannot keep a colour back, because it has nowhere to put one.

#### The six families

Most of the palette is six colours that carry meaning, four roles each:

| Family | What it means | Where it shows |
|---|---|---|
| `PRIMARY` | the brand colour, and "the one you are looking at" | titles, the compose target, the active tab, an unread badge |
| `SECONDARY` | the other side of a pair, without being better or worse | our own chat bubbles |
| `TERTIARY` | in flight — started, not finished | an unsent draft, a send queue under pressure |
| `SUCCESS` | connected, healthy, delivered | the link card, a meter below its warn threshold |
| `WARNING` | not wrong yet | packet loss, a radio low on heap, a `WARNING`-level notice |
| `ERROR` | disconnected, failed, armed to destroy something | a failed bubble, a destructive dialog, an armed row |

The four slots are `BASE`, `ON_BASE`, `CONTAINER`, `ON_CONTAINER` — Material's shape, and for
Material's reason. A colour used as ink and the same colour used as a fill are not the same
colour, and a fill the size of a badge and one the size of a tab are not either. `BASE` is the
saturated value: right as ink on the ground, and as a fill only where the fill is a mark rather
than a field. `CONTAINER` is it held back until text can sit on it — a tab, a chip, a chat
bubble. Each comes with the ink checked against it.

**The pair is the unit.** `mesh_ui_theme_paint(theme, family, slot, state)` returns the fill and
the ink together, and every widget that fills something goes through it. A widget that took its
fill from one slot and its label colour from another would be drawing a combination no theme was
ever measured against, which is exactly how the chat bubbles, the switch and the dialog each
ended up with a colour decision of their own.

Adding a family means every theme answers for all four of its slots and
`mesh_ui_theme_validate()` checks all six of its contracts — there is no list to forget to add
it to, because the validator loops over `MESH_UI_FAMILY_COUNT` rather than over hand-written
rows. That is the difference from the palette this replaced, where a missing row was a pair
nothing checked.

#### The series palette — colour that means nothing

Two palettes in `theme.c` are not roles, and they are not the same kind of thing as each other.

The **avatar tints** (`theme->avatars`, `mesh_ui_theme_avatar()`) are picked by a *hash* of a
conversation's identity, so what they owe is variety: two threads landing on one colour costs the
eye a moment. A theme may state fewer than the maximum — the high-contrast one offers two,
because a palette of six hues is what that theme exists to do without.

The **series colours** (`theme->series`, `mesh_ui_theme_series()`) are picked by *position*, and
that changes every term of the contract. Slice 0 is the same colour on every frame, on every
theme; a theme cannot state fewer, because a chart cannot draw fewer parts than it has; and there
is no `series_count` to match `avatar_count`. They exist because a chart's parts need colours
whose only meaning is **which part** — the families all mean something, three of them mean a
status outright, and the three that do not are not reliably distinct: on the high-contrast theme
the primary, the secondary and the tertiary are one yellow, so a three-part bar drawn from them
there is an undivided block claiming the mesh is made of one thing.

`mesh_ui_theme_validate()` holds them to the meter's 1.4:1 in **both directions**, and the
inward half is the one no palette here had needed before. Every other check asks whether a colour
can be found against a *ground*; two slices of a bar are never against a ground, they are against
each other. It is every pair rather than the neighbouring ones, because a part measuring zero is
not drawn — which two end up sharing an edge is a property of the data.

The measure is **luminance**, not hue, and that is what picks the palettes rather than describing
them. Hue would let far more through and is the wrong cue twice over: it is what goes first in
sunlight, and the colour-blind theme is here because some readers do not have it. That theme is
the best evidence — Okabe-Ito is built to stay separable by hue under dichromacy, and its sky blue
and its orange are within **1.02:1** in lightness, so as adjacent slices they are one slice for
everybody. Four of the eight ladder; the palette is those four. A theme may still separate two
slices by hue — it just has to move them in lightness as well, so the difference survives both.

A series colour is only ever a **fill**, never an ink, which is what keeps the contract at 1.4:1
instead of text's 4.5:1.

#### State is a layer, not a second colour

`enum mesh_ui_state` — `REST`, `SELECTED`, `ACTIVE` — is a *modifier* on a colour. Material calls
it a state layer: the resting fill with its own ink mixed in a little (12% and 20% here), so
"the cursor is on this" is one operation applied to whatever the thing is already painted in.

Mixing the **ink** in rather than white or black is what makes one rule work on both a dark
ground and a light one: the layer always moves a fill towards the thing written on it, so it
lightens on dark and darkens on light without either being spelled out.

It applies to a **container and never to a base**, which is a definition rather than a special
case. A container is the colour held back so text can sit on it, and the room it was held back
by is the room a layer has to move in; a base is already the full-strength end — it is what a
pressed tonal button commits to — so there is nowhere further for it to go. Mixing its ink in
anyway takes a saturated pair under 4.5:1, which it did in three of the four themes before the
rule was written down.

This is what removed the four bubble roles: a selected chat bubble used to be a stated colour
matched by eye against the resting one in every theme. `mesh_ui_theme_validate()` computes the
layer and checks what is written on the result, so derived is not the same as unchecked.

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
| `SURFACE_HIGH` | raised over the body — the keyboard's draft box, an inbound bubble |
| `SURFACE_INVERSE` | over the whole UI — the snackbar |
| `METER_TRACK` | the empty part of a meter — see [`struct fb_meter`](#struct-fb_meter--a-quantity-as-a-length) |

Tonal, not shadowed, and that is forced rather than chosen: the Brick's display engine
composites `fb0` against *its own* background layer, not against what we have already drawn (see
`fb_draw.c`'s `compose_color()`), so there is no alpha to shade with and no shadow to cast. The
fill alone has to carry the distance — which is what Material's tonal elevation does, and it has
the useful property of surviving a light palette, where a dark shadow would have to invert and a
tonal step just changes direction. Lower is nearer the ground, so a dark theme's tiers get
lighter as they rise and a light theme's get darker; nothing that draws knows which way its
palette went.

`SURFACE_INVERSE`/`TEXT_ON_INVERSE` is the odd one out and deliberately so. The three tiers say
how far a thing is from the ground, which works for anything belonging to the screen it is on. A
transient notice does not belong to it — it was not there a second ago and will not be there in
four — and with no shadow and no alpha there is no tier that can say so. Inverting the ground
can: a light fill on a dark theme, a dark one on a light theme, found before it is read. It is
Material's `inverse-surface` pair, and the snackbar is its one user.

The `contrast` theme declines to hold the primary back and states the full colour for both its
`BASE` and its `CONTAINER`, the same way it states two avatar tints instead of six: holding a
colour back is the one thing that theme exists not to do. It also collapses `SECONDARY` and
`TERTIARY` onto that one yellow, because its cursor fill *is* white and a marker bar laid under
it in white is a marker bar nobody can see — which is a rule the validator now holds rather than
a thing to remember.

Two contracts exist only because the palette has families, and nothing else would catch either.
**The three verdicts must be three colours**: success, warning and error each pass every
contrast rule while being identical to one another, and a device where "connected", "busy" and
"failed" read the same is worse than one that is merely hard to read. Contrast cannot measure
that — two hues of one lightness sit at 1.0:1 however different they look — so it is a plain
channel-sum distance, a crude guard rather than a perceptual measure. **A family's base must be
tellable from the cursor fill**, because the marker bar down a selected row is drawn in it.

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
gaps, a cap height and a glyph lookup, and a theme names one by id. Every measurement in the UI —
columns per line, button widths, bubble heights, the scroll window — comes from
`mesh_ui_font_advance()`/`mesh_ui_font_line()` rather than from a constant, which is what made a
second font a table entry rather than a refactor. `MESH_UI_GLYPH_MAX_WIDTH`/`_HEIGHT` bound the
cell; `MESH_UI_GLYPH_MASTER_MAX_WIDTH`/`_HEIGHT` bound the coverage a glyph is decoded into.

Two ship: `src/ui/font_ui.c` (`"ui"`, JetBrains Mono, the default) and `src/ui/font5x7.c`
(`"5x7"`, the pixel one). See [A glyph is coverage](#a-glyph-is-coverage) for what separates
them.

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
Okabe–Ito blue and orange and moves the primary to reddish purple, and it is a palette change
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
| `clock YYYY-MM-DD HH:MM` | pin the wall clock, as local time. The scene seeds its message log and its last-heard times against it, and the renderer draws its clocks, its ages and its day separators from the same value — so the scene renders the same frames on any host at any hour, which is what a checked-in screenshot needs. Setup only; the default is the machine's own clock |
| `tab NAME` | walk Left/Right to `messages`, `nodes`, `devices`, `status` or `settings` |
| `key NAME [COUNT]` | `up down left right a b x y l1 r1 start select` |
| `hold MS` | lengthen the frame just emitted, rather than emitting a duplicate. It also moves the clock on, so an animation that was mid-flight has advanced by the next line |
| `config` | a radio that has answered the config handshake, so the Settings sections, the module table and About radio have rows instead of "not loaded". Plausible values; what is on show is the rows |
| `stats` | the radio's own LocalStats report: the packet counters, the online count, the noise floor and the airtime pair. Its own verb rather than part of `scene demo` because it costs the Status tab four steps — the airtime row, the meter under it and the two counter rows — and those come off the end of the last card, which is where the queue and the radio's own words live. Calling it before `airtime` is also what stops that verb from raising a counter row of zeroes |
| `frame` | emit the current screen again |
| `toast TEXT` | raise the transient notice — the snackbar. It times out on the scene's own
clock, so a `hold` past four seconds followed by a `frame` films it sliding back out |
| `message in\|out NAME TEXT` | append a message, as if the radio had just said so |
| `reply in\|out NAME TEXT` | the same, threaded onto the newest bubble — what A on a message sends. Its own verb rather than a flag on `message` because the quote line inside the bubble is the thing being filmed |
| `react NAME EMOJI` | react to the newest message, as another node would. The transcript draws it on that message rather than as a bubble of its own |
| `ack sending\|delivered\|failed [ERROR]` | what the mesh said about the newest message we sent — the mark in the bubble's corner, and the reason under it once it failed. `ERROR` is a `Routing_Error` number, named by number because that is what the radio would have sent. Its own verb because a Routing reply is the one thing about a message that arrives *after* it, and no press this harness can make produces one: `message out` leaves a bubble pending, which is one of the three marks and the only one a scene could otherwise reach |
| `alert NAME TEXT` | a critical alert (`ALERT_APP`) on the channel |
| `detection NAME TEXT` | a detection sensor announcing itself (`DETECTION_SENSOR_APP`) |
| `status TEXT` | set the transport status line |
| `notice info\|warn\|error TEXT` | what the radio last said about itself, on the Status tab |
| `queue FREE MAXLEN [refused]` | the radio's outgoing packet queue, on the Status tab. The row only appears once the queue is under pressure or has refused a send |
| `reboots N` | times the radio has restarted under us, on the Status tab |
| `airtime BUSY [TX]` | the radio's airtime report as whole percentages: how much of the channel is busy, and how much of that is ours. What the Status card's Airtime row and the meter under it both read |
| `update check\|download [PERCENT]\|available\|ready` | the self-updater's state. `check` and `download` are in flight - `check` is the step with no length and draws the indeterminate bar, `download` with a percentage draws the fraction - and both raise the screen progress bar. `available` and `ready` are settled, and each raises a banner. There is no updater behind the harness - it forks curl and reaches the network - so this sets what the app would have published |
| `firmware checking\|behind\|current\|failed [usb\|ble\|ambiguous\|nopath\|unknown]` | what the client knows about the *radio's* firmware, on Settings > About radio. Its own verb rather than a flag on `update` because these are two binaries on two computers, and because this one has a second axis: what the check found and why it cannot be acted on are separate rows, and the optional word is the second. There is no firmware module behind the harness - it forks curl and reaches the network - so this sets what the app would have published |
| `firmware-channel stable\|alpha` | which of upstream's two release lists the firmware rows say they are reading. Its own verb rather than an argument to `firmware`, because the channel is a setting that outlives any one check - the same split the screen itself draws |
| `syncing on\|off` | put the config handshake back in flight, or finish it. What the screen progress bar reports, and unreachable any other way in a scene: `scene demo` starts with the handshake already complete because every screen in it needs a roster |
| `offradio NAME\|all` | mark that node (or every node but ours) as one the radio's NodeDB no longer carries - what a NodeDB reset leaves behind. Its own verb because no press can reach it: the reset goes out over the air and the answer arrives on the next sync, and the harness has neither |
| `battery NAME PERCENT` | one telemetry report from that node: a battery level, and the uptime that moves with it. Several of these lines are what makes a trend, and the command takes one reading at a time on purpose - a verb that took a whole series would let a scene declare a shape the client could not have been told |
| `nofix` | take our own radio's fix away. `scene demo` gives it one because every range on the Waypoints tab is measured from it, and this is the other state: a Brick has no GPS, so a radio with no fix and no fixed position set is the ordinary case, and it is what the "New waypoint here" row's supporting line and its refused press are about. Its own verb because no press removes a fix - a position arrives off the air, and there is no air here |
| `pin NAME` | pin that node — the star in a row's marker gutter. Its own verb for the same reason: X on the Nodes tab raises a `mesh_ui_action` and the store stops there, so the press the harness can make never reaches the flag |

`tab` walks the tabs with the buttons rather than assigning `nav.screen`, so a scene can only
ever reach a screen the device can reach.

The one thing the harness cannot do is act on a `struct mesh_ui_action`. Pressing START in the
keyboard raises `SEND_TEXT` and the store stops there — it is `mesh_app` that sends and echoes it
back. `message out ...` is how a scene stands in for that.

#### The listing screenshots

The four stills the README and the Pak Store listing carry are scenes too, one per shot in
[`devtools/ui_capture/scenes/shots/`](../devtools/ui_capture/scenes/shots), each named for the
file it writes:

```bash
make screenshots                       # all four, into .github/resources/screenshots
make screenshots ARGS="status"         # just one
make docker-screenshots                # on macOS
```

A screenshot goes stale the way nothing else in the tree does — the UI moves on and nothing
fails — so the fix is that regenerating them is a command rather than an afternoon with a Brick.
They are rendered at the panel's own 1024x768 by the renderer that ships, which is what makes
them the frames the device would draw rather than an approximation: `pak.json` lists the same
four paths, and `scripts/screenshots.sh` is what refreshes both.

Each of those scenes pins its clock, which is what makes them files rather than pictures of when
they were taken. A frame drawn from the real clock is a function of the minute it was drawn in —
the clock column moves, and either side of midnight the day separators move with it — so
regenerating an unchanged UI still rewrote all four. `clock` and the seam under it
(`mesh_time_wall_s()`, beside the monotonic clock in `src/utils/time.c`) make the rendering
reproducible: the same scene draws the same bytes on any host at any hour. Nothing on the device
pins it, so there the clock is the clock.

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

### Rendering cost

Animation frames reuse the latest store snapshot, and scaled glyph coverage is cached. The
device renders in RAM and copies only changed row spans to both framebuffer pages. See
[performance.md](performance.md) for memory costs, benchmarks and visual-equivalence checks.
