# Things that look like bugs and are not

The long form of the rule list. `CLAUDE.md` carries the handful an agent trips over in the first
hour and points here for the rest; this is the file to read before changing session, settings,
map, UI-layout or updater behaviour. **Each of these has cost a debugging round already. Do not
"fix" them back.**

A bare `name` at the end of an entry is a **test filter**: run it with
`./build/debug/tests/meshclient_core_tests --filter <name>`, and a partial name names a group.
**Where an entry cites a test, the test holds the line and the prose is only the reason** — so
those are written short on purpose. An entry with **no** test cited is one that nothing but the
paragraph holds (a hardware fact, a build flag, a measurement), which is why those run longer.
Within every section the tested rules come first, then a blank line, then the rest.

## Adding to this file is the exception, not the habit

Most of this file arrived in the same commit as the code it describes — an author explaining a
design choice, not a record of anything going wrong. The bar is **evidence, not reasoning**:
somebody has to have actually tried to undo the rule. A `feat` commit should not add an entry; if
you are writing the code now, nothing has been got wrong yet, and the reasoning belongs in the
test name or a comment at the seam. Prefer a test to a paragraph. Delete an entry whose rule no
longer holds.

## Contents

[The loop and the wire](#the-loop-and-the-wire) ·
[Devices, session, roster](#devices-the-session-and-the-roster) ·
[Input](#input) ·
[The Nodes tab](#the-nodes-tab) ·
[The map](#the-map) ·
[Messages](#messages) ·
[Store & Forward](#store--forward) ·
[Waypoints](#waypoints) ·
[Position and geography](#position-and-geography) ·
[Lists, cards and chrome](#lists-cards-and-chrome) ·
[Charts and history](#charts-and-history) ·
[Settings](#settings) ·
[Transitions and latency](#transitions-and-latency) ·
[Crash reports](#crash-reports) ·
[Tables, strings and the build](#tables-strings-and-the-build)

## Two premises

These two are not entries so much as the ground the sections stand on - several rules below are
only a consequence of one of them, and neither is a thing a test could pin.

- **No threads.** Everything is the one epoll loop, and every rule below that begins "because that
  would block" is this one.
- **BLE is not Nordic UART** and carries no length framing: one bare protobuf per GATT
  write/read. Framing is a *stream* concern - serial and TCP, which are one wire format - and it
  is `src/proto/stream_framing.c`.

## The loop and the wire

- **A hostname is resolved in a forked child, never on the loop.** `getaddrinfo()` blocks, POSIX
  has no non-blocking resolver and `getaddrinfo_a` starts threads, so calling it here directly is
  seconds of frozen UI — which is why `src/core/resolve.c` exists and why the TCP link refused a
  name until it did. A literal is still answered by `inet_pton` with no child at all.
  `tcp_transport_connects_by_name`, `tcp_transport_refuses_what_it_cannot_resolve`,
  `tcp_target_split_shapes`. See [`docs/transport.md`](transport.md#a-name-costs-a-fork).
- **The stream link never resets itself.** `pump()` and `flush()` report a fatal error and stop;
  only the transport knows whether `-ENOTCONN` is "port closed" or "the radio hung up", which is
  the rule that keeps a renderer naming an id rather than a sentence. `serial_transport_link_drop`,
  `tcp_transport_link_drop`.
- **The heartbeat is the TCP link's, not the session's tick.** A BLE link needs none of it - the
  GATT connection is its own liveness - so a transport asks on its own schedule.
  `tcp_heartbeat_golden_frame`.
- **The BLE device list is not a list of nodes in range, and `rssi` is not a range test.** A
  bond outlives the radio being in the room; what an absent radio lacks is the `RSSI` property.
  Reading that absence as a number is worse than useless - 0 is a *high* RSSI, so an out-of-range
  bond beat every node that answered. `app_autoconnect_ignores_a_node_out_of_range`.

- **The connecting socket is deliberately not the stream link's, and `struct mesh_stream_link`
  must not grow a connecting state.** A link is an *established* stream: it watches for
  readability and reads. A non-blocking connect is the opposite - it reports by becoming
  **writable**, and reading it would be meaningless - so the TCP transport holds that descriptor
  itself and hands it over at the moment the connect completes. That is what keeps the link free
  of a state in which its own `pump()` must not be called.
- **The network arm of auto-connect has a retry stamp of its own, and sharing the general one
  would break it.** A configured host is the one candidate that can be absent without being
  *gone*: a cable is plugged in or it is not and a node is advertising or it is not, but an
  address somebody wrote down stays written down with the WiFi off. Without
  `autoconnect_tcp_retry_at_ms` that arm runs first on every turn, fails five seconds later on
  its own connect deadline, and Bluetooth is never reached at all.

## Devices, the session and the roster

- **The Devices tab's last row is not a device, and it is present exactly when the list holds no
  network row of its own.** A network cannot be scanned, which left the whole TCP transport
  reachable only by editing `launch.sh`. **An emptied field forgets the host**, the address is its
  own preference (`network_host`) rather than a `known_devices` entry - that list ranks a *scan*
  - and it is written when the connect is **asked for** rather than when it succeeds.
  `devices_network_row`, `tcp_refused_target_is_remembered_by_nobody`.
- **A network link's Devices row is synthesised, and its `in_range` is false while it is
  connected.** It must state `MESH_UI_DEVICE_TCP` because the slot is `memset` to zero and
  `MESH_UI_DEVICE_BLE` is `0`; `in_range` is false because that field means *earshot* and a host
  answers from anywhere. `tcp_link_is_published_as_a_network_device`.
- **The node roster deliberately outlives the connection**, and a **NodeDB reset does not clear
  it either** - which is why Status can say 2 nodes while Nodes says 81. The radio's NodeDB holds
  80 and evicts, so mirroring it loses nodes for good. `session_roster`, `session_forget`,
  `session_keeps_the_radio_across_a_drop`.
- **A node with no `User` is named after its node number**, as the phone apps do; an empty `User`
  must not blank a name we have. `session_node_default_identity`.
- **A radio reboot after a settings write is expected.** The link drops and auto-connect
  reconnects.

- **The power button is deliberately not a quit key.** It was one until the Brick was measured:
  the PMIC (`axp2202-pek`, its own input device) really does emit `KEY_POWER`, so a tap of the
  button - this hardware's sleep gesture - tore the client down instead of suspending it. Sleep
  is the launcher's business. `MESHCLIENT_QUIT_KEYS` still overrides the set.

## Input

- **The Brick's face buttons do not report by position.** A is `BTN_EAST` (305), B is
  `BTN_SOUTH` (304), the button printed **Y (left)** is `BTN_NORTH` (307), X (top) is `BTN_WEST`
  (308). That is the `brick` row of `src/ui/input_profile.c`, **not a default the rest of the
  client may assume**: the `xbox` row is the ordinary convention, where A is the code the Brick
  calls B. The two disagree about exactly the buttons that confirm and go back, which is why one
  row holds the codes *and* the keycaps - correcting one without the other is invisible, because
  the binding still works and the action bar goes on promising the first.
  `input_brick_face_buttons`, `input_profile`. The measured table is in
  [`docs/device.md`](device.md#the-buttons).
- **Key repeat is generated in `input.c`, not by the kernel.** The d-pad is an absolute axis and
  never repeats however long it is held, so a timerfd is what scrolls a long list - and it drops
  the kernel's own `value == 2` for a direction. `ui_input_key_repeat`.

## The Nodes tab

- **The filter and the sort are stepped by Left and Right, and the shoulders still walk the tabs.**
  The d-pad does mean two things on this screen - edit on the top two rows, next tab on every row
  under them - and that is the Settings tab's arrangement rather than a new one: a field row edits,
  a row with no field walks the tabs, and `MESH_UI_ICON_EDIT` in the gutter is what tells them
  apart before the press. A still steps forward, for the reason it does on a Settings enum.
  It was A alone, on a chip strip wearing no marker, and the cost was a screen whose two controls
  were only discoverable by pressing every button on the case.
  `ui_nav_nodes_filter_steps_and_renumbers_the_rows`, `ui_nav_nodes_controls_take_the_d_pad`.
- **A fixture that wants a tab presses the shoulder, not Right.** `mesh_test_open_tab()` walked the
  ring with `MESH_UI_KEY_RIGHT` and hung on the first screen whose cursor was resting on a control
  - which, once the Nodes list took the axis, was the Nodes list every time. `ui_fixture.c`.
- **A filter matches what the list *draws*, not one of the conditions behind it.** Both extra
  halves are corrections: a node dropped from the radio's database still passes `signal_heard()`,
  and a radio can carry a stale `is_favorite` no press here can clear. `ui_nav_nodes_filter`,
  `node_detail_signal_heard`.
- **The list has `MESH_UI_NODES_LEAD_ROWS` rows before its first node, and nothing may subtract a
  literal.** A row becomes a node through `mesh_ui_node_view_at()` rather than by indexing the
  roster: under a filter or a sort an off-by-one is a *plausible* node, so X pins somebody else's
  radio and nothing on the frame looks wrong. `ui_nav_nodes_filter_steps_and_renumbers_the_rows`,
  `ui_nav_nodes_sort_steps_and_renumbers_the_rows`.
- **A sort permutes the rows the filter kept; it never selects among them.** `mesh_ui_nav_row_count()`
  asks the filter alone, so a sort that dropped or duplicated a node would be a cursor walking off
  the end of a list the screen says is longer.
  `ui_nav_nodes_sort_permutes_but_never_selects`.
- **A filter that keeps nothing keeps the lead rows above it.** The chip that emptied the list is
  on the first row, so falling through to `fb_draw_empty()` would take away the control that puts
  it back. `ui_nav_nodes_filter_that_keeps_nothing_keeps_its_own_rows`.
- **`map_open` outlives a change of tab, and the key handler must still check `nav->screen`.** The
  flag says *where the Nodes tab is standing*, not *what the reader is looking at*; read as "a map
  is open somewhere", the arrows pan a map nobody can see.
  `map_keys_belong_to_the_screen_the_map_is_on`, `map_hands_the_keys_over_when_a_place_opens`.

- **The Nodes title counts what is drawn, not what is held.** `held` is the published roster and
  `count` is whatever the filter kept; the heading's "n of m" already means "there is more than
  this", so a filter needs no sentence of its own. Conflating the two read "Nodes 42" over three
  pinned rows.

## The map

- **The d-pad does not move a cursor, and the shoulders do move tabs.** `nav_map.c` takes the four
  directions ahead of `nav.c` because Left on a map means "look west"; the shoulders are
  deliberately *not* taken, which is what pays for it. `map_the_dpad`,
  `map_keys_belong_to_the_screen_the_map_is_on`.
- **A direction is one step, and landing on a marker changes where that step ends rather than how
  long it is.** A fixed pixel pan leaves the crosshair on a lattice, so five markers in six could
  not be aimed at *at all*. **The reach is one pan step, per axis** - a reach of the whole panel
  left the ground *between* two nodes unreachable. `map_a_direction`, `map_the_dpad`,
  `map_the_crosshair_selects_by_panning`.
- **The map has no selection field on the nav, and must not grow one.** What A opens is derived
  every frame from pixels; a second opinion would let the ring a backend draws and the node a
  press opens name two different nodes. It is measured *in the projection*, the only reading that
  gets the poles right - a fix beyond the display limit is drawn at the limit, so a geodesic
  distance refuses a marker under the crosshair. `map_selection_names_a_thing_not_a_row`,
  `map_selects_a_marker_the_projection_had_to_clamp`.
- **The fit is computed against a declared box, not a measured one** - the nav cannot learn a
  backend's body size. `MESH_UI_MAP_FIT_WIDTH` is deliberately *smaller* than any body drawn into,
  because a fit for a small box leaves extra air where the opposite clips a marker off the edge.
  `map_viewport_fit`.
- **The map clips its artwork to its own body, and `visible` is not enough on its own.** A
  placement speaks for a marker's *centre*, so one centred a pixel inside the top edge paints most
  of itself over the app bar. The clip *intersects* the partial-redraw path's rather than
  replacing it. `ui_capture_map_keeps_its_ink_off_the_chrome`.
- **The map draws a different roster from the Nodes list, and it is not a subset.** The list
  publishes the best 128 because a rank says how likely you are to talk to a node; a marker is on
  the panel or it is not, so the map gets every *positioned* node. So **map-only nodes exist**,
  and A on one deliberately does nothing (`marker->openable`).
  `map_roster_agrees_across_the_seam`, `map_draws_nodes_the_list_never_published`,
  `map_falls_back_to_the_published_rows`, `map_press_refuses_a_node_it_cannot_open`.
- **The tile pack is a format of the client's own, and that is a measurement rather than a
  preference.** On the Brick's FAT32 card a sorted single file reached a cold tile in 0.80 ms
  where MBTiles and a `z/x/y` tree both took 4.6 ms, and SQLite would have cost 718 KB of a
  2.88 MB binary. Its **zoom range and coverage are derived from the index, never read out of the
  header**, and everything a read would trust is checked **once, at open**, sort order included -
  a `bsearch` over an unsorted index does not fail, it misses. `map_pack`.
- **The basemap is drawn *over* the graticule, and a tile still coming looks exactly like a tile
  that is not there.** Telling MISS from ABSENT *in ink* is worse than either: a placeholder
  covers the markers for the two thirds of a second a view takes to fill. The two states differ in
  what the client **does**. `ui_capture_map_draws_a_basemap_one_tile_at_a_time`.
- **One tile is read per frame, and a tile the pack does not hold costs no read at all.** A cold
  tile is 2-5 ms and a view stands on twenty, so a frame that filled the panel would be a tenth of
  a second with the BLE link unread. `fb_basemap_pending()` is what asks for the next frame.
  `ui_capture_map_draws_a_basemap_one_tile_at_a_time`,
  `ui_capture_map_stops_asking_once_it_is_left`.
- **The tile cache remembers which tiles are *not* in the pack, and that table is not an
  optimisation.** Without it, one hole spends the single read per turn forever and a view with any
  sea in it never finishes filling. The holes have a table of their own because a hole costs
  twelve bytes and a tile 256 KiB, and **a key is in at most one of the two** - which is also why
  a lookup answers with a *state* rather than a pointer that may be NULL. `map_tile_cache`.
- **Opening a different tile pack must clear the cache.** A key is three numbers about the world,
  not about a file, so two packs of the same city hold different pictures at the same key. Carried
  across a swap, every pixel is a real tile in the right place - so nothing looks wrong.
  `map_tile_cache_clear_drops_tiles_and_holes`, `ui_capture_map_forgets_the_pack_it_swapped_out`.
- **The tile decoder's memory is static, constant, and sized for a colour type no pack contains.**
  A decode on an event loop must not pause to find memory. Both blocks are **asked for and checked
  rather than known** - Wuffs' struct size is not stable across versions, and the scratch scales
  with the *file's* colour type - so it is sized for 8-bit RGBA (262,400 bytes) rather than the
  palette a builder usually emits (65,792), because a palette-sized buffer refuses every 24-bit
  tile with the constant looking perfectly reasonable. `tile_image`.

- **A marker's name is drawn five times.** A glyph carries coverage, not a mask, so text is
  blended against a colour the caller says it has just filled - which is a guess the moment a name
  stands on a street. The four extra runs are the halo in the ground colour, and they are drawn
  only where a tile is; the scale bar and the pack's attribution take a chip instead, because they
  are chrome pinned to a corner rather than things on the map.
- **A cached tile is in the decoder's pixel format, not the panel's, and `src/map/` may not
  learn which the panel is.** `struct fb_state` reads `bytes_per_pixel` off the kernel and it is
  not always 4, so a cache holding panel-format pixels would be the map layer including the
  framebuffer - and would make a cached tile the property of *one* backend, since the capture
  harness renders at four bytes a pixel whatever the device is doing. The blit converts per
  drawn pixel, joining the switch `fb_fill_packed()` already makes.

## Messages

- **A reply is aimed by the press that opened the sheet, not by the cursor when it sends.** The
  transcript keeps moving underneath, so reading the cursor at send time would re-aim the reply at
  whatever slid under it. Y clears it: a new message is not an answer to the last thing said.
  `ui_nav_reply_and_react_name_their_target`.
- **START on the conversation list is not A, and it is the second screen to spend it.** Here it
  mutes the row under the cursor, for the map's reason - there is no other key left. The action
  bar names the press for the *row*, and says nothing at all on the two rows that are not
  conversations. `ui_nav_mute_conversation`, `actions_compose_names_the_row_under_the_cursor`.
- **A mute rides `struct mesh_ui_read_mark`, and the eviction there prefers an unmuted slot.** The
  two share a key and a lifetime but not what losing one costs: a read mark is rebuilt by being
  read again, a mute is a choice. The loader takes four fields or five, because a conversation
  muted before it was read has no packet id to be saved under. `ui_nav_mute_survives_the_cache`.
- **"Muted" has two inputs and one predicate.** `mesh_ui_store_conversation_muted()` reads our own
  flag *and* upstream's per-node `is_muted`; only the local half is what START toggles, and only a
  direct conversation has the other. One predicate, so the badge, the snackbar and the row's bell
  cannot disagree. `ui_nav_mute_conversation`, `app_unmute_reports_the_radios_mute`.
- **Unmuting is reported from what it achieved, not from what it set.** Both halves can be on at
  once, so the toast asks again *after* the write. "Unmuted" on a row still muted is worse than a
  press that does nothing: it is a press that lies. `app_unmute_reports_the_radios_mute`.
- **An unmute that empties a mark takes the mark with it.** A conversation muted before it was
  opened has `packet_id` 0, so clearing the mute leaves a record holding neither - freshly
  stamped, which under the eviction above is the *last* unmuted mark to go.
  `ui_nav_unmuting_drops_a_mark_with_nothing_in_it`.
- **A muted conversation still counts its own unread, and is missing only from the total.** Muting
  is asking not to be interrupted, not asking to be kept in the dark; what it takes away is
  `mesh_ui_nav_unread_total()`, which is what keeps the badge worth looking at. `ui_nav_unread`,
  `ui_capture_nav_bar_badges_unread_messages`.
- **Only the Messages tab carries a badge, and that is a rule rather than a start.** A badge must
  be **clearable by going there**, exactly as a banner must be able to resolve; a count of nodes
  would never go down however often it was looked at.
  `ui_capture_nav_bar_badges_unread_messages`.
- **The transcript's unread line is captured by the press that opened the thread, not derived from
  the read mark.** By the time a frame is built the mark says "all of it".
  `nav.thread_unread_from` is the one copy of where the reader *was*, and does not move while they
  are in there. `ui_nav_unread_divider_marks_where_the_reader_was`.
- **A read mark never lands on a reaction.** The unread walk ignores reactions and looks for the
  marked packet to know where "read" stops, so a mark on a tapback is one that walk can never
  meet. `ui_nav_read_mark_skips_a_reaction`, `ui_store_read_revision_moves_only_when_the_mark_does`.
- **A notification cursor that has gone missing re-places itself in silence.** Where we had got to
  is unknowable. The reachable way in is a *delete*: read as "everything since is new", deleting a
  conversation announced whatever inbound message happened to be last.
  `app_direct_message_notice`, `app_delete_conversation`.
- **A press replaces the snackbar and an arrival queues behind it, and that is two functions on
  purpose.** `set_toast()` supersedes because it answers the button just pressed; `post_toast()`
  is news nobody asked for, and overwriting the answer to a press with it is how a button comes to
  look as though it did nothing. `ui_nav_toasts_queue_rather_than_overwrite`.
- **A direct message announces itself and a broadcast never does** - a channel is a room full of
  people talking. Every unseen direct message is announced rather than only the newest, and a
  launch announces none of it, the priming taken *before* the empty-log guard.
  `app_direct_message_notice`.
- **A reaction deliberately goes out without want_ack.** It has no bubble, so a delivery mark
  would be one nothing on the frame could draw, bought with a retransmit round on a shared band. A
  reaction with no `reply_id` is `-EINVAL`. `ui_nav_reactions_are_not_messages`,
  `message_encode_reply_and_reaction`.
- **A bubble's trailing run is typed slots, not a string, and it is dropped rather than
  truncated.** Right-aligned *as a string* inside a box the bubble had clamped, a failure reason
  came out of the left edge. It drops parts off the *front*, so a reaction chip is lost and the
  mark saying the message failed survives. `ui_capture_bubble_contains_its_own_ink`.

- **The unread line takes the separator slot from a date when both want it.** A bubble has one
  row above it. The date is recoverable from the clock in the bubble's own trailing run, and
  "this is where you stopped" is sayable in one place only.

## Store & Forward

- **A replayed message has no date, and that is what makes the de-duplication work.** `rx_time`
  "is _never_ sent on the radio link itself", so the stamp on a replay is our own radio marking
  when it landed; copying it would date the whole window at the minute it was fetched and defeat
  `mesh_message_log_holds_replay()`. The SNR, hop count and padlock are left off because they
  measure the router's link, not the sender's. `store_forward_replay`.
- **A replayed message takes its id from `original_id`, never from the packet carrying it.** The
  router wraps the message in a packet of its own, so *that* id names the delivery and two routers
  replaying one message would give it two of them. `StoreAndForward.original_id` names the message
  instead, which is the number the live copy already has, so it is kept and the de-duplication uses
  it. Firmware that leaves it 0 falls back to matching on content, which is what stops one press
  putting a second copy of the last four hours under the first.
  `store_forward_replay_skips_what_we_already_had`,
  `store_forward_replay_tells_two_identical_messages_apart`.
- **A replay counts twice, and the two numbers are different facts.** A router hands back its
  whole window, mostly traffic heard live - so `received` is the router's work and `stored` is the
  user's gain. `store_forward_rows_say_what_the_request_did`.
- **The history request looks for a router before it asks one, and never broadcasts itself.**
  With none known the press broadcasts a `CLIENT_PING` and the real request follows the pong; a
  broadcast `CLIENT_HISTORY` would have every router on the mesh replay at once.
  `store_forward_finds_a_router_then_asks_it`,
  `store_forward_refuses_to_broadcast_a_history_request`.
- **The history cursor belongs to one router, and hearing another drops it.** It indexes *that
  server's* packet history, so sending A's `last_request` to B asks B to skip into a table it does
  not have - B silently returns fewer messages, this feature failing in the one direction no
  screen could show. `store_forward_a_second_router_is_a_clean_slate`,
  `store_forward_only_the_router_we_asked_may_answer`.

- **The follow-up request goes out from the tick, not from the ingest that armed it.** The pong
  arrives on the link's read path, and writing back down the link on the same turn is what the
  admin queue's queue-here-drain-there split exists to avoid.

## Waypoints

- **Deleting a waypoint is broadcasting it again with an expiry in the past.** There is no
  "unshare" packet, so `mesh_session_forget_waypoint()` sending what looks like another copy is
  the protocol - and it only sends when `locked_to` says this client may. The local entry goes
  either way. `waypoint_expiry_and_withdrawal`, `waypoint_send_keeps_whose_place_it_is`.
- **An expiry is only honoured once we know what time it is, and a tombstone always is.** With no
  RTC battery `time(NULL)` is a small positive number, so "has this expired?" is usually
  unanswerable - but "was this expiry a moment in 1970?" needs no clock, and that is what a
  withdrawal is. Hence `mesh_time_wall_credible_s()`, which a *deadline* needs and an *age* does
  not. `waypoint_dated_expiry_is_honoured`, `time_wall_clock_credibility`.
- **The waypoint book is a table keyed by id, not a ring.** Upstream *edits* by re-broadcasting
  the same id. The message log next door does the opposite on purpose: two packets are two things
  that happened, and a waypoint arriving twice is one place that moved.
  `waypoint_same_id_is_an_edit`.
- **A place this client made is kept even when the send fails, and `-ENOTCONN` still stores it.**
  It is something the user made, theirs with or without a radio.
  `waypoint_book_keeps_our_own_places`.
- **A radio swap takes the waypoints with the roster, and a reconnect does not.** The channel
  index is why: a message's channel labels something that already happened, a waypoint's indexes
  the channel table the swap just discarded - so a place carried across would re-share on whatever
  slot that number names on the *new* radio. `waypoint_book_follows_the_radio`.
- **`MESH_WAYPOINT_NAME_MAX` is declared twice and that is the seam working.** `store.h` is
  nanopb-free by construction; including `mesh/core/waypoint.h` to reach three numbers would drag
  the generated protobuf headers into every backend and every test that draws a screen.
  `waypoint_limits_agree_across_the_seam`.

- **Waypoints are deliberately not persisted with the roster.** The roster is what we *know* and
  is worth keeping because a node the radio evicted is gone for good; a waypoint lives on the
  mesh and its sharer can withdraw it. A cache would put back places the mesh had already agreed
  were gone, because the withdrawal that arrived while the client was off is a packet nobody
  replays.
- **The Waypoints list is never empty, and the row that makes a place is the last one.** Every
  other tab can fall through to a picture when it has nothing; this one always has something to
  do. The row says why it cannot be pressed - "no position yet" - rather than disappearing, which
  is the ordinary case on a Brick, whose radio often has no fix.

## Position and geography

- **A fix carries two clocks and the row says which one it is answering with.** Neither
  `timestamp` (when the GPS solved) nor `time` (the sender's clock, usually 0) is when the packet
  reached us, hence `received`; the detail draws **Fix** against the node's dating and **Fix
  heard** against ours, and the label change is the point. `last_heard` advances on any packet.
  `session_position_clocks_and_range`, `ui_node_detail_position_honesty`.
- **A `precision_bits` of 0 on a received fix means "the node did not say", not "off".** The same
  formatter renders 0 as *off* for the channel's own `position_precision`, where it is a setting
  with an off state; off the air it is an absent field, so the detail guards on `> 0`. One table
  for both screens is what stops a rounded location being described two ways.
  `ui_node_detail_position_honesty`, `ui_settings_coords`.
- **Every length the client shows follows the radio's `DisplayConfig.units`, and there is no
  second preference.** A reader who set their radio to miles is not asked to set the Brick to
  miles as well, so the byte off the wire is the only say - `mesh_ui_units_imperial()` is the one
  place that decodes it and `src/ui/units.c` the only place that words a length. A new row that
  formats metres directly is the way this comes undone, which is why the guard is a sweep over
  every section rather than an assertion per row.
  `ui_units_no_setting_reads_in_metres_under_imperial`,
  `ui_units_node_detail_lengths_follow_the_setting`.
- **The fixed-position altitude row stays in metres under either setting**, and says so in its
  label. It is typed rather than read: the value is written to the radio as whole metres, and an
  integer round trip through feet moves a fixed position a metre each time the row is opened and
  backed out of. `ui_units_no_setting_reads_in_feet_under_metric`.
- **`(0, 0)` is a valid coordinate** - where a half-initialised GPS most often claims to be, and
  a real point in the Gulf of Guinea. `mesh_geo_coords_valid()` is a *range* check and nothing
  more; rejecting Null Island there is a guess about the sender's firmware in a range check's
  clothes. `geo_coords_range`, `waypoint_refuses_a_coordinate_off_earth`.

## Lists, cards and chrome

- **fb layout is measured in cells, not bytes.** A `strlen` or `%-Ns` there is a bug.
  `layout_wrap_measures_in_cells`, `ui_text_cells`.
- **A card in a scrolling list is wider than the rows standing in it, and its ends are square
  wherever the window cut it.** The highlight is measured from the same row gutter, so a card
  whose edge sat inside it loses its sides on precisely the row being read; and a rounded corner
  halfway down a scroll is a card claiming to *end* where the panel merely stopped.
  `ui_capture_node_detail_cards_survive_the_cursor`.
- **A list's rows, its cards and its scroll rail are one rectangle asked for once, and the rail's
  gutter is reserved whether or not the rail draws.** Derived three times, the opinions agreed
  right up until cards began spending their hairline outward - the rail then read as part of the
  card rather than a control beside it. Reserved only when a list outgrows its window, the layout
  would reflow on one more reading. `ui_capture_node_detail_cards_survive_the_cursor`.
- **A group's heading stands between two cards rather than inside either, and a card in a list is
  padded at the bottom only.** Both are where the column's nine pixels of air come from. A label
  on the bottom of a heading names the card that ended rather than the one it opens, and a row's
  box carries its leading at the *top*. The grouping costs no rows, which is what keeps the nav
  and every count in the `ui_nav_nodes` suite out of it. `node_detail_groups_are_unbroken_runs`.
- **A group heading is a row of the list and is not a row the cursor may stand on.** The cursor
  used to land on one: a full-width highlight under a dimmed word, A doing nothing, the action bar
  still promising "select". Both helpers **ask the built rows** rather than reasoning about where
  a heading falls - so a press is no longer the same number as a row.
  `ui_nav_node_detail_walks_its_groups`, `node_detail_groups_are_unbroken_runs`.
- **The Status cursor is a verb, not a position, and `cursor[MESH_UI_SCREEN_STATUS]` is unused.**
  Status has no rows: its cards offer verbs and Up/Down walk those. As an index it forced the list
  to be append-only and every verb to restate the conditions of the ones before it.
  `ui_status_cursor`, `ui_status_a_new_verb_does_not_move_the_cursor`.
- **The Status verb list is written in the order the cards draw**, so Down walks down the screen.
  Ordered by when each verb was added, the trend sat after the Radio card's refresh while its card
  is the middle one. `ui_status_verbs_stay_in_card_order`.
- **A column of cards reserves room for its last card, and the reservation yields rather than
  erasing the card making it.** `fb_draw_card()` pays by refusing a card outright, which is worse
  than losing rows because a card carries *verbs* - so the cursor walked onto a button that was
  not on the frame. **How much to reserve is a reading rather than a constant**: one helper
  promises the card exists, the other that it can say everything.
  `ui_capture_status_keeps_the_last_card_when_the_one_above_overflows`.
- **The screen progress bar costs no body row and the banner costs rows.** A request already sent
  must not reflow the list it went out from, so the bar takes a `const` layout; a banner is
  content about the client. Which states raise either is `src/ui/chrome.c`, never a renderer.
  `ui_capture_progress_costs_no_row_and_the_banner_costs_rows`.
- **A banner says only what nothing else on the frame says, and must be able to resolve.** Hence
  no "radio disconnected" banner - the status line under the keycaps already says it - and no
  dismissal, because dismissal is a nav change and refusing it is what keeps the table to states
  that go away on their own. `ui_chrome_banner`.
- **A font's cell height is not its cap height.** Anything sized to stand beside the text uses
  `mesh_ui_font_cap()`. They are equal for `5x7` and not for a face with real ascenders, where the
  cell makes every icon a seventh too big. `ui_theme_fonts_cap_height`.

- **A card's verbs are on its heading line, not in a row under its content.** Every phone puts
  card actions at the bottom, and that is how it was first written. It cost a row of content per
  card carrying a verb, and the screen it cost them on is the one that can outgrow its panel - the
  Status tab lost the TX queue and the reboot count off the end of the Radio card. A heading is
  three or four cells of a line that is otherwise empty.
- **A card's rows are drawn against the card, and a control on one takes the row's *resting*
  ground rather than its current one.** The first is a glyph carrying coverage rather than a mask:
  text told the wrong ground keeps its shape and gains a halo. The second looks like an
  inconsistency and is not. A switch's ring and a meter's track bed are laid *to escape* the
  cursor fill, and on two of the four themes that fill is the resting track's own colour - so
  handing them the current ground makes the control vanish on the row being pointed at.
  `fb_draw_trailing()` derives the first from the second so the two cannot be passed the wrong way
  round.
- **A card's focus ring is painted inward and is not part of its layout.** The card's edge is in
  the content inset and in the box height, so a ring that widened it would make a card grow when
  the cursor arrived and shift every card below it.
- **A card that can end up with no rows must not be given a verb.** A card with no rows is not
  drawn, and a verb on an undrawn card leaves the action bar naming a press whose button is not
  on the frame. That is why the Radio card says "no report yet" rather than disappearing.
- **Two rows of the Link card stand down while a radio is attached, and it is not a missing
  else.** `fb_link_summary()` builds the line under the keycaps on every frame of every screen, so
  with a link up it reads "running: Home Base" and the card's Transport and Radio rows were the
  same two expressions a dozen rows further up. With no radio that line says the quit hint
  instead, so the rows come back. It is the banner's rule on a card row.

## Charts and history

- **A trend's axes are not its data, and the one exception is stated on the axis.** A sparkline's
  y is the reading's own scale, never the range the samples span; what a *chart* may do is
  contract that domain's **ceiling** to a rung of a fixed ladder, so a mesh at 1.1% busy is a
  shape rather than a flat line. Three things make that not auto-scaling: the **floor never
  moves**, the rungs are fractions of the *domain* so two visits are comparable, and the ceiling
  is the axis's own top label. `trend_contracts_the_ceiling_to_a_quiet_mesh`,
  `trend_rungs_are_fixed_rather_than_fitted`.
- **A line is broken rather than sloped across a silence, and a *refused* reading breaks it too.**
  A node on external power reports punctually and reports something that is not a level, which no
  clock can see - so `mesh_ui_series_break()` is how the source says so. `series_breaks`.
- **The radio's airtime pair is persisted and a node's trends are not, and the saved sample is an
  age rather than a stamp.** Starting empty at every launch left the Mesh card's chart unoffered
  for the first half hour of *every session*, and "a trend is what we watched" is answered by the
  break lifting the pen over the seam. A time here is `CLOCK_MONOTONIC`, so restoring the numbers
  hands the series a reading older than its oldest - which it reads as the clock going backwards
  and answers by emptying itself. `history_airtime_survives_a_restart_onto_a_new_clock`,
  `series_drops_history_when_the_clock_goes_back`.
- **A chart carries one vertical, so a node's temperature and its humidity are two screens.** One
  `scale` is why the airtime chart can draw two lines - both permille of the same air - where
  degrees and relative humidity are not, and an axis with numbers on it gets believed.
  `ui_route_a_node_chart_names_its_reading`, `history_keeps_temperature_and_humidity_apart`.
- **A node chart takes its whole statement off the row it was opened from, and must not switch on
  the reading itself.** The series, the domain, the band and the words agree by being one row; a
  renderer with a switch of its own would be the node detail's opinion about what a temperature is
  measured between and the chart's. `ui_nav_node_trend_keeps_the_row_it_was_opened_from`.
- **The span picker only narrows, and it is anchored at the newest reading rather than at the
  clock.** Anchored at "now", a link that dropped twenty minutes ago answers every span but All
  with an empty plot. The window is cut **before** the ceiling is picked, which is why
  `mesh_ui_trend_frame()` is one function. `trend_span_narrows_the_window_and_never_pads_it`,
  `trend_ceiling_follows_the_window_rather_than_the_ring`.
- **A narrowed window drops the readings outside it rather than clamping them.** Holding an
  outside sample at the edge is right for a window taken from the series and wrong for one the
  *reader* narrowed - it draws every older reading at one x, a vertical stroke in the data's own
  colour that is not a reading of anything. `trend_projection_drops_readings_outside_the_window`.
- **Two lines on one chart are projected over a window neither of them owns.** Stretched across
  its own span, a series that stopped reporting is drawn as though it were still arriving;
  `mesh_ui_series_window()` is the union of the clocks. `series_share_one_window_across_a_chart`.
- **`nav->trend_span` is one field for both charts, and that is a claim about what it is.** Every
  other level flag says where a tab is standing; a span is not a place, it is how the reader likes
  their charts read. `mesh_ui_nav_init()` sets `ALL` rather than leaving the zero, the narrowest.
  `trend_span_is_one_choice_across_both_charts`.
- **The chart swallows the d-pad and A, and does not take the shoulders** - the map's split in
  reverse. Up and Down because there is nothing on the plot to move and a fall-through reaches the
  Status cards; Left and Right because the span picker *is* the only control on the screen.
  `trend_left_and_right_walk_the_span_not_the_tabs`, `ui_status_trend_opens_swallows_and_closes`.
- **A node's air is pushed into the history on its own test, not on the battery's.** The two
  Telemetry variants arrive on two schedules, so keyed on the battery's struct it is a temperature
  series on the wrong clock. `store_records_node_environment_on_its_own_schedule`,
  `history_keeps_temperature_and_humidity_apart`.
- **A history sample is stamped with the client's clock, and a new reading is detected by the
  report having changed.** The radio's own `time` fields are 0 on the device, so a series keyed on
  either question holds exactly one sample forever. `store_records_airtime_as_the_radio_reports_it`,
  `history_draws_a_line_at_the_radios_own_cadence`.
- **A series colour is not a tone, and the avatar tints are not a series palette.** A tint is
  picked by a hash so it only owes variety; a series colour is picked by position, so every theme
  states all four and the contract is luminance - 1.4:1 against the grounds *and against each
  other*, which is why the colour-blind theme spends four of Okabe-Ito's eight rather than any
  four. `ui_theme_series_palette`, `ui_theme_validate_holds_the_series_palette`.

- **The Status card's counters are split by direction, and only the received side gets a bar.**
  `num_packets_rx` is everything received, so new/dupe/bad are a *partition* of it and a divided
  bar is true. `num_tx_relay` is a **subset** of `num_packets_tx`, so the Sent row's three numbers
  add up to a whole that does not exist - and overlapping parts still sum to something, which is
  `fb_draw_proportion()`'s one way of being wrong quietly. A negative remainder skips the row.
- **Both chart screens are one renderer, and a third caller adds a description rather than a
  function.** `fb_render_chart()` takes a `struct fb_chart_screen` - the series, their labels, the
  domain, the band, and what unit the axis is *worded* in - and does everything else. The airtime
  chart and a node's were forty duplicated lines apart, which is forty lines in which two screens
  meant to be one picture can quietly stop being one.
- **The temperature and humidity bands are about the node, not about the weather.** Nothing here
  knows whether 35 degrees of air is pleasant, and a band coloured for *that* is an opinion the
  client has no business having. The thresholds are where a sealed box on a pole starts derating
  and where condensation starts forming on the board inside it - the only question it can answer,
  and the one a solar repeater in a field is opened for.
- **A chart's line is drawn thicker than a sparkline's on purpose.** A series colour promises
  1.4:1 and that was measured on the width of a bar - the palette is a fill's contract, never an
  ink's - so a hairline in one of those colours is a line the reader has to hunt for.
- **Both counter rows take their tone from a share, never from a count.** These are lifetime
  totals since the radio booted, so a colour read off an absolute lights once and stays lit -
  twelve malformed packets in six thousand is what the row this replaced spent its warning on. The
  live half is elsewhere on purpose: the Radio card's TX queue row goes to the error family the
  moment the radio is refusing sends *now*.

## Settings

- **A slider's stops are evenly spaced, and its zero may not be on it at all.** A `NUMBER` setting
  steps a preset list that climbs geometrically, so value space crowds eight of screen-on's ten
  choices into the first sixth of the track. Most lists open with a 0 the field reads as a word,
  which is not a quantity, so `SCALE_PRESETS_AFTER_ZERO()` stands it outside the track - drawn the
  other way, `max` reported itself at the empty end of its own bar. Which lists are a scale at all
  is stated per field: `{0, 1, ... 7}` is a hop limit under one and a GPIO pin under the next.
  `ui_settings_number`, `ui_capture_slider`.
- **The settings edit buffer's width is a `sizeof`, not a number.** It is the size of a union of
  every TEXT and KEY field, so a wider field widens it by being listed. It was a constant raised
  twice, and the failure was silent: the commit cuts what does not fit, so a radio that would have
  taken the whole string was sent part of one. `settings_text_fields_fit_the_edit_buffer`.
- **Three settings rows are shown and cannot be pressed, and that is the point.** The screen and
  settings locks (no verb turns them off, and the PIN is not on the wire), the **ringtone** (RTTTL
  is 231 bytes against a text max of 80), and the radio's **language** - the only enum whose wire
  values are not `0..n-1`, while every enum row steps by `(value + 1) % count`. A setting that
  cannot be pressed is still the answer to why the radio is behaving as it is.
  `ui_settings_radio_ui_and_canned`.
- **The canned message slots are six of 32 characters because the wire is one 200-byte string.**
  Six plus five separators is 197 and seven would not fit. A radio holding more than six keeps
  them - the save copies the tail across and closes the gaps. `ui_nav_canned_separator`,
  `ui_canned_load`.
- **The radio's firmware rows say short values, never sentences.** A settings row has about two
  dozen cells of value and no supporting line, so `"%s available (radio has %s)"` came out as
  `2.7.26.54e0d8d available (`. The row's *name* carries the difference.
  `ui_settings_radio_firmware_install_row`,
  `ui_settings_up_to_date_radio_says_nothing_about_installing`.

- **`DeviceUIConfig` is kept whole and written back whole.** It carries a touchscreen
  `calibration_data` blob and a map home point the client has no rows for, so a `store_ui_config`
  built from the rows alone would erase a screen's calibration - invisibly, until somebody touched
  their radio.

## Transitions and latency

- **A screen transition is derived from the nav, not declared by it.** `mesh_ui_route_of()` says
  which *place* the nav is showing, the backend remembers the last one, and the difference is the
  direction. A field on the nav is what this deliberately is not: eleven call sites open a level
  and a forgotten one animates backwards, which fails no build and is invisible in a screenshot.
  The cursor and the draft are excluded, or the slide restarts on every press of Down.
  `ui_route_in_and_out_are_opposite`, `ui_route_ignores_the_cursor_and_the_draft`.
- **Only the arriving screen is drawn, and it travels a quarter of the panel, not all of it.**
  There is no alpha and nothing can read back the panel, so a full-panel travel leaves the body
  *empty* on the frame the press lands. A quarter is also Material's shared-axis displacement.
  `ui_capture_slides_a_screen_in_and_settles`.
- **The navigation bar, the action bar, the progress bar and the banner do not slide with the
  body.** A frame-wide offset would say the whole application had been replaced when one level of
  one tab did. `ui_geometry_every_screen_keeps_its_chrome`,
  `fb_animation_clip_matches_full_composition`.
- **Half of a press is the panel, and the map's drawing is free because of it.** Measured
  2026-09-13: a press reaches the panel in 25-27 ms with a basemap and twenty tiles under it and
  the same on a list with neither, because `FBIOPAN_DISPLAY` waits for vblank. So a tile is
  **1.5 ms of a 22 ms frame**, and **partial redraw would buy no latency at all**, only CPU. The
  tail is not the map's either - the control run's own worst press varied from 33 to 529 ms across
  five identical runs with the median unmoved.
  `latency_splits_a_frame_into_the_draw_and_the_flip`,
  `latency_counts_a_frame_with_no_tile_in_it`.
- **A key repeat is deliberately not counted by the latency probe, and neither is a press that
  changed nothing.** A held direction reaches the store with no evdev event behind it, so timing
  it from when the probe sees it reports the fill loop's worst case as its best; and a press that
  publishes no snapshot draws no frame, so left pending it would be answered by whatever drew
  next. `latency_does_not_count_a_repeat_as_a_press`,
  `latency_does_not_charge_an_inert_press_to_the_next_frame`.

- **The capture harness cannot act on a `mesh_ui_action`.** START in the keyboard raises
  `SEND_TEXT` and the store stops there; sending is `mesh_app`'s job and there is no app behind
  the harness. A scene stands in for the echo with `message out ...`.

## Crash reports

- **The handler builds no strings, and walks the stack through a pipe.** A signal handler may not
  call `printf` or `malloc` - a fault inside the allocator leaves its lock held - so every heading
  is built at *install* time, and the frame walk probes each address through a pipe made then, so
  an unreadable page is `EFAULT` rather than a second SIGSEGV. The ordering is a safety property
  too: the log tail goes down before the registers are touched. **Re-raising at the end is not
  tidiness** - a handler that returned would report a clean exit for a process that faulted.
  `crash_handler_writes_a_report_from_a_real_fault`, `crash_report_carries_its_notes`.
- **The crash report does not promise to be free of private data, and must not start.** Its
  header claimed to carry no message text, no names and no coordinates; the log tail it carries is
  the *ordinary* log, which says `Sent "%s" to %s`. A user attaching the file *because it said it
  was safe* would publish exactly what it promised was absent. `crash_report_carries_its_notes`.
- **The handler runs on an alternate signal stack, and SA_ONSTACK is not belt-and-braces.** When
  the fault *is* the stack running out, the kernel has nowhere to build the signal frame - so the
  crash with the most interesting backtrace leaves no file.
  `crash_handler_survives_an_exhausted_stack`.
- **The frame walk requires pointer alignment, not 16-byte alignment.** AAPCS64 keeps the stack
  16-aligned throughout, which makes the tighter test look safer; x86-64 only promises it at a
  call boundary, and asking for 16 ended every walk on every host build after a single frame -
  which looks exactly like a shallow stack. `crash_handler_walks_more_than_one_frame`.
- **Whether a crash report is waiting is read once, at install, and must not become a `stat`.**
  Asked on demand, the flag flips the moment *this* run writes its own report.
  `crash_report_waiting_is_read_once_at_install`.
- **`mesh_ui_screen_id()` is not `mesh_ui_screen_name()`.** The first is the untranslated
  identifier a capture scene names a tab with and a crash report names a place with; the second is
  the catalog's label. Using the name means a scene that only runs under one locale, and a bug
  report in a language the maintainer may not read. `ui_route_describes_a_place_without_a_locale`.

## Tables, strings and the build

- **Neither `include/mesh/i18n/catalog.def` nor `include/mesh/ui/icons.def` is a header, and
  `make format` does not touch either.** Each is included several times with the macros defined
  differently, which keeps the enum, the table and the translation template from drifting apart.
  `i18n_catalog_is_complete`, `icon_table_covers_every_id`.

- **The framebuffer needs all three steps** - draw page 0, `FBIOPAN_DISPLAY`, mirror into page 1 -
  or the screen is black.
- **`deploy-start` kills NextUI's launcher with `SIGKILL`, and `TERM` there powers the Brick
  off.** SDL turns `TERM`/`INT` into a quit event, `nextui.elf` answers it with `PWR_powerOff()`,
  and `PLAT_powerOff()` deletes `/tmp/nextui_exec` and touches `/tmp/poweroff` - so the launch
  loop runs the pending pak and shuts the device down when it exits. Measured twice, by accident.
  Do not "gentle" the kill, and never `kill $(pidof nextui.elf)` in a device shell.
- **The pak is built without `--gc-sections`, so a third-party module ships whole.**
  `scripts/cross-build.sh` uses plain `-Os` with no `-ffunction-sections`, so any code compiled
  into an object file is code that ships. It is why `src/map/wuffs_png.h` names Wuffs' BASE
  **sub-modules** rather than BASE, and why the decoder costs 335 KB where the spike's probe -
  which did collect sections - predicted 106 KB. Read a size measurement's build flags before
  believing it about this binary.
- **Only the release build is a release.** Do not stamp a local build to test the updater; lift
  the guard (`MESHCLIENT_UPDATE_ALLOW_DEV=1`, or Settings → About → Dev updates).
- **`main` is deliberately missing from the release workflow's `push` trigger.** It reads as a
  workflow that forgot its own branch, and adding it back is how every merged pull request
  became a release again - sixteen on one day, a Pak Store nagging on each, and a five-entry
  store changelog covering eight hours. A release is pressed (`workflow_dispatch`, with a weekly
  cron behind it) because a merge and a release are two decisions. `beta` and `rc` keep their
  push trigger, because a prerelease reaches only a client that asked for one.
- **Do not edit `project(meshclient VERSION x.y.z ...)`** in `CMakeLists.txt` or bump it by hand;
  the release workflow rewrites that line with `sed`.
- **`launch.sh` and the pak's CA bundle do not ship through self-update.** Only the bare binary
  does. Changing either forces a pak reinstall, so treat them as a compatibility boundary.
- **`scripts/gen-emoji.py` is not part of the build.** Run it by hand and commit the result.
  The same goes for `scripts/gen-icons.py`, which rasterises the icon set out of Material
  Symbols, for `scripts/gen-font.py`, which rasterises the `ui` face out of JetBrains Mono, and
  for `scripts/gen-locale.py`, which turns the string catalog into a translation template or a
  locale skeleton.
- **`devtools/` is not `Tools/`.** `Tools/` holds the device-facing pak assets, and macOS
  filesystems are case-insensitive by default, so a `tools/` directory would collide with it.
