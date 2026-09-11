# Map support assessment

Status: **steps 1 and 2 of the delivery sequence have shipped** - the geography contract is
complete and there is a marker map on the device - with a basemap under it as of step 3, below -
and it now draws
every positioned node the session holds rather than the 128 the node list publishes. See
§"What steps 1 and 2 became" for what landed and what it deliberately did not, and
§"The roster decision, taken" for the one open decision that has since been closed. Steps 3 to 6
- the offline raster spike, the offline release, downloading a pack on the device and the
optional extensions - are still proposed, based on the repository inspected on 2026-09-07, and
no device benchmarks accompany them. The **pre-work in §"Pre-work that matters" shipped first**,
on its own and ahead of any map.

**Step 3's measurement has run on a Brick (2026-09-11)** and answers the question this document
said nothing should be built before: raster is viable, one tile at a time on the event loop, with
no helper process - from a **single-file pack decoded by Wuffs**. The `z/x/y` tree and MBTiles
both lost on this hardware, and the reasons are specific to it. See §"What the Brick measured".

**Step 3's first half is now in the client (2026-09-11):** the pack the measurement chose is a
format with a reader (`src/map/source_pack.c`), a host-side builder (`devtools/map_pack`) and
a `--map-pack` flag that opens one on the device, and the viewport answers which tiles a box is
standing on. See §"The pack format".

**And the decoder with it (2026-09-11):** Wuffs is vendored and a tile's bytes become pixels
through one function, `mesh_map_tile_decode()`. One thing the integration measured differently
from the spike, and it is about the pak's build rather than about Wuffs: see §"What the decoder
actually cost".

**And the cache the pixels land in (2026-09-11):** byte-budgeted, LRU, and holding what a tile
is *not* as well as what it is - see §"What the cache turned out to be".

**Step 3 is complete (2026-09-11): there is a basemap under the markers.** The fill loop and the
blit are in `fb_map.c`, one tile is read and decoded per frame, and a pack can be drawn on a host
rather than downloaded - which is what lets a UI capture of the map have streets under it with
nobody's licence involved. See §"What the fill loop and the blit turned out to be". What is left
of step 3's *entry* is its last line: the integrated input-latency number, which needs a Brick.

§"How a pack gets onto the device" (2026-09-10) corrects a premise that ran through the original
assessment - that the Brick has no network - and re-sequences the delivery steps around the
correction. It is the section to read before acting on any statement about "offline" below.

## Recommended first release

Build an offline, north-up raster map showing the client's known node locations. Support
button-driven pan, integer zoom, centering on the connected radio or a selected node, and
opening node details. Keep markers useful when no basemap covers the viewport. Prepare and
sideload a small regional map pack on a host computer before adding downloads on the Brick.

"Offline" here means the map must be *usable* with no network, which is nearly all of the time
it is read - not that the Brick has none. It has WiFi, and this client already downloads over
it. Sideload-first is a sequencing choice and a consequence of not hosting a tile service, not
a property of the hardware; see §"How a pack gets onto the device".

This fits the existing C17, software-framebuffer architecture. Vector maps would add geometry
decoding, styling, label placement and font concerns; defer that work until a concrete need
justifies it. Routing, address search, terrain, track history and waypoint sharing are separate
features, not prerequisites for viewing nodes on a map.

## Waypoints landed first, and without a map

Waypoints were listed under "optional extensions" below, on the assumption that a place is
something you look at on a map. They shipped ahead of the map because that assumption is wrong
on this hardware: a handheld with a d-pad and no touchscreen can answer *how far away is it and
which way* without drawing anything, and that answer is most of what a shared place is for.

What it took from this document was the geography, and only the part a range needs:
`mesh_geo_vector_between()` alongside the bounds test, both in `src/geo/`, with no projection
and no tiles. The map inherits them rather than writing a second copy - which is the whole
argument for the module boundary proposed below, now with a caller to check it against.

What the map still adds is the picture: markers, panning, and seeing where two places are in
relation to *each other* rather than each one's relation to you. The Waypoints tab's list is the
thing that makes those worth having, because it is where the places come from.

## What is already present

| Existing component | Reuse and limitation |
| --- | --- |
| `src/core/session.c`, `include/mesh/core/session.h` | NodeInfo and POSITION_APP populate fixed-point coordinates, altitude, time and precision. The session owns up to 256 nodes. No transport-specific map work is required. |
| `src/core/app_publish.c` | Publishes positions into nanopb-free UI records and restores cached records into the session. It ranks and truncates the UI roster to 128 nodes. |
| `src/ui/store.c` | Persists positions with the displayed roster. This does not persist all 256 session nodes. |
| `src/ui/node_detail.c` | Already presents coordinates, age and precision; node actions can request a position. Reuse the existing action path. |
| `src/ui/nav.c`, `include/mesh/ui/nav.h` | Pure navigation model and logical buttons. Left/right and shoulders currently switch tabs, so map input needs deliberate routing. |
| `src/ui/backends/fb_draw.c`, `fb_widgets.c` | Software drawing, clipping, text, icons and themed components. No general raster-tile decoder or image-blit interface was found. |
| `src/core/event_loop.c`, `updater.c` | Single-threaded epoll and an existing subprocess/fd pattern. Map work must preserve event-loop responsiveness. |
| `devtools/ui_capture`, `devtools/perf` | Real renderer capture and performance infrastructure; extend with map scenes and workloads. |

The current build does not link SQLite or a runtime PNG/JPEG decoder. Those are new packaging
decisions, not dependencies already paid for by the application.

## Pre-work that matters

> **Landed, ahead of any map.** All five are below as they were written; what each one turned
> into is recorded under it. They shipped on their own because none of them is really about
> maps: each was the client already answering a question about a location less honestly than
> it could, and the node detail is where that showed. The map layer inherits the answers
> rather than having to invent them.

1. **Define location semantics.** Inbound application currently checks that both coordinates
   are present but does not range-check them. Share range validation across ingestion, cache
   restoration and user-entered coordinates. Preserve explicit `(0, 0)` as valid. A packet
   with missing coordinates must not erase a previous fix.

   > **Done.** `mesh_geo_coords_valid()` in [`src/geo/coords.c`](../src/geo/coords.c) - the
   > first piece of the `geo` module proposed below, deliberately holding nothing but the
   > bounds test until the map needs projection. Three ingresses ask it: `POSITION_APP` and
   > `NodeInfo` decoding in `mesh_session_apply_position()`, the cache loader in
   > `src/ui/store.c`, and `mesh_session_set_fixed_position()`, which had grown its own copy
   > of the constants. An out-of-range fix leaves the previous one standing, which is the same
   > answer already given to a packet carrying no coordinates. `(0, 0)` is valid: rejecting
   > Null Island would be a guess about the sender's firmware wearing a range check's clothes.

2. **Separate fix age from node activity.** The current record copies `Position.time`; the
   upstream message also has a GPS-solution `timestamp`. Decide and test their precedence,
   retain a position-received timestamp where needed, and label unknown ages honestly.
   `last_heard` can advance on unrelated packets and cannot establish location freshness.

   > **Done.** The precedence is `timestamp` (the GPS solution) over `time` (the sender's own
   > clock), and `struct mesh_node_position` gained `received` - ours, from the packet's
   > `rx_time` - because upstream says `time` is "usually not sent over the mesh (to save
   > space)", so on a real mesh the node's own dating is usually absent. The detail row is
   > *relabelled* rather than silently falling back: **Fix** against the node's clock, **Fix
   > heard** against ours, and the old behaviour - "Fix: unknown" - threw away the one honest
   > answer available. `last_heard` is not offered here at all, for the reason given above.
   > `received` is persisted as an eighth field on the cache's `node_pos` line, and the loader
   > takes seven or eight so a roster written by an older build still loads.

3. **Preserve precision honestly.** Carry `precision_bits` through the map model. A rounded
   location is approximate, not an exact pin. Derive a quantization footprint only after
   verifying encoding semantics; it is not a GPS accuracy estimate.

   > **Done, and the footprint was already verified.** `precision_bits` was reaching the
   > screen, but as `"16 bits"` - carried, and unreadable. The bits-to-distance table the
   > channel's own `position_precision` row uses had already been derived and shipped, so the
   > honest fix was to make the node detail ask the same function rather than to derive a
   > second answer: `mesh_ui_settings_format_precision()` is now public and the row reads
   > `~360 m`. A rounded location described two ways on two screens is how a client comes to
   > disagree with itself about how much it knows. `precision_bits` of 0 means the node never
   > set the field, not "off", so the row is absent rather than claiming a footprint.

4. **Choose roster coverage explicitly.** Prefer a compact map projection of all session nodes
   rather than doubling every large UI detail record. Publish it from the same authoritative
   roster and conversion helpers. Decide whether complete restart persistence is in scope:
   the existing UI cache only preserves its 128-node subset. Report displayed/known counts.

   > **Done.** The reporting half landed first: the truncation was *silent*, because the
   > session holds 256 and the UI publishes its best 128, and the Nodes tab could only compare
   > itself against the radio's `nodedb_entries`, which is a different set and, after a NodeDB
   > reset, the smaller one. `handshake.nodes_known` carries the roster's own total and the
   > title takes whichever of the two is larger, so "128 of 200" is sayable.
   >
   > The coverage half is now taken too, and it is the compact projection this item asked for
   > rather than the doubling it warned against - see §"The roster decision, taken". The
   > persistence question is still open and still deliberately so: the cache holds the
   > published 128, and widening it is a decision for whoever needs all 256 back after a
   > restart.

5. **Keep selection stable.** Store selected node ID, not a roster index, since publication
   reorders nodes. A map-only node may be outside the detail roster: resolve its detail by ID
   through the app/store seam before opening it. Handle eviction, forgetting and radio swaps.

   > **Already true, now pinned.** `nav->node_detail_node` was an id from the start, resolved
   > through `mesh_ui_node_detail_find()` on every frame, and `mesh_ui_nav_clamp()` closes the
   > detail when the node leaves the roster. Nothing tested it, which is the state a map
   > selection would have quietly broken - a marker under a cursor is another index into
   > another ordering of the same roster. `ui_nav_node_detail_follows_the_node` now holds it:
   > re-rank the roster under an open detail and it still shows the node it was opened on;
   > drop that node and the screen closes as the frame is built. The map-only case remains
   > open by construction - there are no map-only nodes until there is a map.

## What steps 1 and 2 became

> **Landed.** The delivery sequence below is as it was written; this records what the first two
> steps turned into, and the three places where doing them changed the answer.

**Step 1, the geography contract, is closed.** `mesh_geo_mercator_forward()` and its inverse are
in [`src/geo/mercator.c`](../src/geo/mercator.c), answering in the unit square so that nothing
in `geo` knows what a zoom is. The two limits this document asked to be kept apart are two
constants and always will be: `MESH_GEO_LATITUDE_I_MAX` is a fact about Earth and
`MESH_GEO_MERCATOR_LATITUDE_I_MAX` is a fact about a picture, and a fix beyond the display limit
is *clamped rather than refused* - 88 degrees north is somewhere, and the top edge is the honest
place to draw it. Longitude wrap is `mesh_geo_longitude_wrap_i()`, asked by everything that
crosses the seam. `geo` is still the only directory in the tree that includes `<math.h>`.

**Step 2, the map without a basemap, is on the device.** [`src/map/viewport.c`](../src/map/viewport.c)
owns the centre, the integer zoom, the pan and the bounds fit; [`src/ui/map.c`](../src/ui/map.c)
builds the markers; [`src/ui/nav_map.c`](../src/ui/nav_map.c) handles the presses;
[`src/ui/backends/fb_map.c`](../src/ui/backends/fb_map.c) draws. It is reached from a row at the
top of the Nodes list and from a node detail's "Show on map", and
`devtools/ui_capture/scenes/map.scene` renders the whole of it without a device.

Three things came out differently from what is written below, and each is worth stating because
the reasoning generalises to the steps that are still open:

- **The controls are not the ones suggested.** SELECT was proposed for recentring; it is help,
  on every screen in this client that has anything to explain, and one screen where a keycap
  meant something else would be the exception nobody could know about. The d-pad pans - which
  makes the map the only screen here where it is not a cursor - the shoulders keep the tabs, X
  and Y are the zoom, and START frames everything again. Marker cycling was dropped entirely:
  the crosshair is the middle of the panel and the selection is whatever is nearest it, so
  panning *is* aiming and no key has to be spent choosing between markers.

  > **Corrected on the device (2026-09-10).** Panning was aiming in principle and could not aim
  > in practice. A pan of a fixed number of pixels only ever leaves the crosshair on a lattice -
  > 176 across and 84 down, from wherever the view opened - and the crosshair captures a disc of
  > 28 pixels, so about a sixth of the plane was selectable and five markers in six could not be
  > put under it at all at a given zoom. Zooming re-phases the lattice, so the reader's remedy
  > was to zoom in and out until a node happened to land, which is what it felt like.
  >
  > What shipped keeps the decision above and fixes the arithmetic under it: a direction goes to
  > the *nearest marker in the 45-degree quadrant around it* and centres the view on that
  > marker's own coordinates, falling back to the old step when the quadrant is empty
  > (`mesh_ui_map_step()`). It is not the marker cycling this rejected - no key was spent, the
  > directions still mean what they say, and open grid still pans - and the selection is still
  > derived from the centre of the view, so the nav still holds no selection. The four quadrants
  > tile the plane, which is the property that makes nothing on the panel lie in a direction no
  > press names.

  > **Corrected again, from the same device (2026-09-11).** The first correction fixed the aim
  > and took the exploring with it. Its reach was the whole declared panel, so on a mesh with a
  > few dozen positioned nodes *every* tap of a direction had a marker somewhere in its quadrant
  > to answer with: the view hopped from node to node, and the ground between two of them was
  > not merely awkward to stop on - it was unreachable, which is the lattice complaint again
  > from the other side. The reader's report was "with every dpad tap it just cycles to the
  > next... it will snap to nodes that are several squares over instead of letting you move
  > around in between".
  >
  > What bounds it is the step itself. A press is one pan - `MESH_UI_MAP_PAN_STEP_X/Y`, declared
  > once in `map.h` and used by both halves so they cannot drift apart - and landing on a marker
  > changes where that step *ends*, never how long it is. A candidate has to sit inside the box
  > one press covers, on both axes: the along bound is what leaves open ground reachable, and
  > the cross bound is what stops a press that said "north" from answering with a sideways lurch
  > onto something mostly east. A marker further out is walked up to instead - the step falls
  > back to a pan, the pan brings it inside the reach, and the press after that lands on it
  > exactly, which costs a press and gives back the whole plane between the markers.
- **The selection is measured in metres, not pixels.** The store owns the nav and a backend is
  handed a `const` snapshot, so the nav genuinely cannot learn how wide a backend's body is.
  Anything box-dependent would therefore be two answers - the ring a renderer draws and the node
  a press opens. `mesh_map_viewport_metres_per_pixel()` depends only on zoom and latitude, and
  that is what makes one answer possible. The same constraint is why the *fit* is computed
  against a declared box (`MESH_UI_MAP_FIT_WIDTH`) that is deliberately smaller than any real
  body: too small only ever leaves extra air, where too large would clip a marker off the edge.
- **The roster's published 128 was what went on the map, not the session's 256.** Step 2 asks
  for "all live session markers with explicit restart coverage". Widening the published roster
  was the decision the fourth pre-work item left open on purpose, and a map was not the place to
  settle it quietly - so the app bar said how many of what is known has a position instead
  ("4 of 24"), which is the reporting half that item did land. That decision **has since been
  taken**; see §"The roster decision, taken" below for the shape it took and the one thing it
  left behind.

What step 2 asked for and did not get, beyond the above: tile addressing on the viewport, which
waits for a tile to fetch, exactly as `geo` held nothing but a bounds test until a range needed a
vector. There is nothing to attribute yet either, so there is no attribution in the layout; that
arrives with the first basemap and not before.

## The roster decision, taken

> **Landed.** The fourth pre-work item asked for a "compact map projection of all session
> nodes" and warned against "doubling every large UI detail record" to get one. Step 2 shipped
> without it and named it the largest single thing still between this map and the one described
> here. This is what it became.

**The map now draws every positioned node the session holds.** `struct mesh_ui_map_node` is the
compact projection: a node number, two coordinates, when the fix was heard, the rounding its
sender declared, whether the radio still carries it, and the label to write beside it. Nothing
else. `mesh_app_publish_ui_state()` fills one per positioned node over the *whole* ranked roster
rather than over the 128 rows it then copies, and `mesh_ui_map_build()` reads it.

The measurement is why it is a second array rather than a wider first one. A
`struct mesh_ui_node_summary` is **532 bytes** - seven telemetry tables, two names, a public key
- so widening `nodes[128]` to the session's 256 would have added **68 KB** to a snapshot that
was 105 KB and is copied whole every frame, to reach two coordinates. The compact record is
**36 bytes**; 256 of them cost **9 KB**, and the snapshot went to 114 KB. That is the difference
between a 65 percent snapshot and a 9 percent one for the same markers.

Three things came out of doing it:

- **Only positioned nodes are carried, so the cap can never truncate.** An unpositioned node
  contributes nothing a marker needs, and how many the client knows is a different question that
  `nodes_known` already answers - which is the reporting half the same pre-work item landed
  first. A roster in which all 256 nodes have a fix still fits, so there is no second cut to
  explain and no second "N of M" to get wrong. `MESH_UI_MAX_MAP_NODES` is pinned against
  `MESH_SESSION_MAX_NODES` in the map suite, the way the waypoint limits are pinned against the
  book's, and for the same reason: `store.h` is nanopb-free by construction.
- **A handshake nobody published falls back to its rows.** A roster loaded from the cache before
  the first publish, a hand-built fixture, the capture harness: none of them fills a second
  array, and a producer that forgot to would leave a map that is silently empty - which no build
  and no screenshot catches. The fallback is provably dead after a real publish (publish scans a
  superset of the rows it copies, so a published row with a position always has a map entry
  beside it), so it is a default rather than a second opinion.
- **Map-only nodes now exist, and the fifth pre-work item's open case is open for real.** That
  item said "a map-only node may be outside the detail roster: resolve its detail by ID through
  the app/store seam before opening it", and closed as "open by construction - there are no
  map-only nodes until there is a map". There are now: a node ranked 200th has a marker and no
  row, and a node detail resolves by id against the rows. Left alone, A on such a marker opens a
  detail that cannot be filled; `mesh_ui_nav_clamp()` runs on every snapshot and closes it
  before the next frame is drawn, so nothing is *seen* - but the press has already reset the
  node list's cursor on its way past and left `nav.node_detail_node` naming a node nothing is
  showing.

  What ships is the guard, not the seam: `struct mesh_ui_map_node` carries `has_row` - free,
  because both arrays are cut from the same ranking - and A on a marker without one does
  nothing, exactly as A on empty grid already does. The action bar still names A unconditionally
  here, deliberately: a keycap that appeared and vanished as the reader panned would be the bar
  flickering rather than informing. The line under the map still names the node and its range,
  which is most of what a detail would have said about a node that far away.

  **The seam itself is the next piece of this**, and it is small: publish would have to promise
  that the node `nav.node_detail_node` names survives the ranking cut, and the clamp would have
  to stop closing a detail for a node the map can select. The ordering is what makes it more
  than a one-liner - a key press and the repaint it triggers happen inside one
  `mesh_event_loop_run()`, with no publish between them - so it is a change to the loop's
  contract rather than to the ranking, and it is not a tile problem.

What this decision does **not** settle is persistence, which the same pre-work item also left
open and which is still open: the cache holds the published 128, so a restart brings back 128
nodes and the map's roster is rebuilt from those. Widening the cache is a decision for whoever
needs all 256 back after a restart, and it is independent of everything above.

## What the Brick measured

> **Measured 2026-09-11** on a TrimUI Brick (4x Cortex-A53, `schedutil` 408 MHz-2 GHz, 1 GB)
> with [`devtools/tile_bench`](../devtools/README.md#tile_bench--what-a-map-tile-costs-on-the-device),
> which reads, decodes and blits one tile the way a client would and runs on the device over adb.
> This is the cold-read-plus-decode number step 3 asked for, and the decisions below rest on it.

What was compared: three layouts - a `z/x/y` file tree, MBTiles (SQLite 3.53, statically
linked), and a single file with a sorted in-memory index shaped like PMTiles - and two PNG
decoders, stb_image 2.30 and Wuffs 0.4. The page cache was either **warm**, **dropped once**
before 200 tiles, or **dropped before every tile** (the worst case: a tile nobody has read, in a
directory nobody has read). Tiles are synthetic - see the caveats - and are 256x256 palette PNGs,
9.4 KiB median, over a 3,069-tile z12-16 pyramid; the 24-bit set is 25 KiB median. Both decoders
produced **identical pixels on all 800 tiles checked**. All numbers are milliseconds.

**One tile, from the SD card, cache dropped before every tile:**

| Layout | Decoder | Fetch p50 | Fetch p99 | Decode p50 | Tile p50 | Tile p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| single-file pack | Wuffs | 0.80 | 2.9 | 1.24 | **2.3** | **4.4** |
| single-file pack | stb_image | 0.83 | 4.5 | 2.41 | 3.6 | 8.2 |
| MBTiles | Wuffs | 4.6 | 20.3 | 1.28 | 6.2 | 21.9 |
| `z/x/y` tree | Wuffs | 4.6 | 40.9 | 1.24 | 6.1 | 42.5 |
| single-file pack, 24-bit tiles | Wuffs | 1.1 | 1.4 | 2.09 | 3.4 | 6.7 |

Warm, every layout's fetch is under half a millisecond and a tile is decode plus a 0.2-0.4 ms blit.

**A whole 1024x768 map from cold** (5x4 tiles at an arbitrary offset, which is the most one can
touch), then from that same view **one pan across** (a new column: four tiles, twenty blitted
from decoded ones) and **one pan down** (a new row: five tiles), 20 rounds each. The pan-down
column comes from a second idle run, in which every other column reproduced the first to within a
millisecond. The rest was run first with the client stopped and the launcher's `schedutil`
governor, then with
the client running and connected over BLE to a Heltec V3, which NextUI runs under `performance`
(a fixed 2 GHz; see the last point below). The client's 147-node NodeDB sync overlapped only the
first ten seconds of that run - the first row's rounds - and the rest ran beside a connected,
mostly quiet client:

| Layout | Decoder | View p50 / max | Pan across p50 / max | Pan down p50 / max | View p50 / max, client running |
| --- | --- | ---: | ---: | ---: | ---: |
| single-file pack | Wuffs | **43 / 47** | **12.5 / 14.4** | **12.4 / 15.1** | **41 / 46** |
| single-file pack | stb_image | 66 / 78 | 16.8 / 18.9 | 17.2 / 20.7 | 64 / 79 |
| MBTiles | Wuffs | 58 / 68 | 15.1 / 19.0 | 15.5 / 17.8 | 54 / 69 |
| `z/x/y` tree | Wuffs | 90 / 141 | 19.2 / 26.8 | 16.9 / 20.2 | 85 / 253 |
| `z/x/y` tree | stb_image | 111 / 246 | 22.2 / 26.4 | 22.4 / 24.2 | 119 / **625** |

What this settles:

- **Raster is viable here, on the loop, without a helper process.** A cold tile is 2-5 ms, so the
  roadmap's "bounded count of tiles per turn" is literally one tile per turn: a full view fills
  in twenty turns of ~2 ms each rather than one 45 ms stall, and input is serviced between them.
  The helper-process protocol the resource strategy held in reserve is not needed, and should not
  be built. The one thing a turn cannot promise is the SD card's own rare stall: one tile in two
  hundred took 32-38 ms in the cold-per-tile runs, on every layout.
- **The pack is a single file, not a directory tree, and it is not SQLite.** The card is FAT32
  with **32 KiB clusters** and mounted `sync`. A tree of 3,069 tiles holding 28.4 MiB occupies
  **99 MiB** on it (every tile rounds up to a cluster), took **55 s** to push over adb against 16 s
  for the same tiles as one file, and a cold lookup walks FAT directories: 4.6 ms median, 40 ms at
  p99, and the worst view - during the client's NodeDB sync - was **625 ms**. The client's own
  saves go to the same sync-mounted card, and the tree got *worse* with the client running despite
  the faster clock. MBTiles avoids the directories and pays in B-tree pages from cold (4.6 ms
  median fetch), plus **+718 KB** of SQLite on a 2.88 MB binary. The PMTiles-shaped pack is best on
  every column, and its view was unchanged with the client running (41/46 against 43/47) - which,
  with the clock faster on that side, says the client's load cost it no more than the difference
  between the governors, not that it cost nothing.
- **The decoder is Wuffs.** Twice as fast as stb_image on palette tiles (1.2 vs 2.4 ms) and 1.8x
  on 24-bit, memory-safe by construction, and it allocates nothing: its whole footprint was a
  **44.6 KB** decoder plus a **64-192 KB** caller-owned work buffer, which is the bounded
  allocation this document asked a decoder to prove. It costs **+106 KB** of code against stb's
  +52 KB, which is the price of the checking.
- **A pan costs the same in both directions, so plain `(z, x, y)` order is enough.** The worry was
  that the pack's order favours one direction: a new column is one contiguous run, while a new row
  is five tiles spread through the file. It did not show - a pan down read one more tile than a
  pan across and took the same 12.4 ms - because each tile of a new row sits just after a tile the
  view has already read, and readahead has it. That adjacency holds at any pack size, so tiles do
  not need a space-filling-curve order for panning. Zooming, which reads a different level of the
  pack, was not measured.
- **Packs should be palette PNGs.** The 24-bit set decodes 1.7x slower and is 2.7x larger for no
  gain on a 1024x768 panel; quantising is the pack builder's job, on the host, once.
- **The SD card is fast enough; the internal ext4 is not needed.** The same pack on `/mnt/UDISK`
  fetched in 0.58 ms rather than 0.80 - not worth leaving per-pak state on the card for. Writing is
  the slow direction (28 MiB in 16 s to the card, 7 s to ext4, both over adb), which is step 5's
  concern rather than rendering's.
- **The clock depends on who started the process, and the client gets the fast one.** NextUI's
  launch loop (`.system/tg5040/paks/MinUI.pak/launch.sh`) runs the launcher under `schedutil`,
  408-1800 MHz, and switches to `performance`, a fixed 2 GHz, before it runs any pak - "they can
  change it themselves after launch if they want". So the client as launched decodes at the warm
  numbers: with it up, a cold pack-and-Wuffs tile was **2.2 ms at p50 and 2.8 ms at p99**. Under
  `schedutil` and after a one-second pause, decode goes from 1.2 to **4.7 ms** (tile p99 9.7 ms,
  with the cache dropped before the pause so that the timed tile is the first work after it),
  which is what the first press after a spell of reading would cost if the client ever dropped to
  `schedutil` to save battery. That is a trade worth making knowingly, and the map is where it
  would show.

What it did **not** measure, and each is a reason to keep going rather than a reason to doubt the
above:

- **Input latency inside the real client.** This is a separate process on an idle launcher and a
  running client; the contention it saw is the client's CPU and SD traffic, not the client's own
  loop doing the decoding. The integrated number comes with the first tile blit in `fb_map.c`.
- **A like-for-like contention comparison.** The idle and client runs differ in governor as well
  as in load. Pinning it on both sides (`sh /mnt/SDCARD/.system/tg5040/bin/governor.sh
  performance`, then `auto` to hand the launcher back) separates the two.
- **Real tiles.** The synthetic set is drawn to be Carto-shaped and calibrated to size, but dense
  urban Carto z16 tiles run ~15-25 KiB, above the palette set's 9.4 KiB median - which is why the
  24-bit set exists to bracket them. Decode is dominated by pixel count, so the conclusion is not
  expected to move; the absolute numbers might by a millisecond.
- **A regional-size pack.** This one is 28 MiB with its whole index in RAM (24 B a tile). A pack of
  a million tiles would need 24 MB of index, which is why PMTiles has leaf directories - and a
  single file on FAT32 **cannot exceed 4 GiB**, so a large region is split into files or the
  format's directories are read lazily. Both are pack-format questions, not rendering ones.

## The pack format

> **Landed 2026-09-11.** The reader is [`src/map/source_pack.c`](../src/map/source_pack.c), the
> seam it sits behind is [`include/mesh/map/source.h`](../include/mesh/map/source.h), and packs
> are built on a host with [`devtools/map_pack`](../devtools/map_pack/map_pack.py).

The measurement above chose a single indexed file over a `z/x/y` tree and over MBTiles, so the
client carries a format of its own rather than reading one of the two the rest of the world
publishes. That is a cost worth naming: every pack has to be converted before it can be used,
and a reader cannot point the client at an MBTiles file they already have. What it buys is the
0.80 ms cold tile, no SQLite in the binary, and a transfer to the card that takes 16 seconds
rather than 55 - all three of which are properties of *this* hardware rather than of raster
maps, which is why the conversion is a host step and not a device one.

It is shaped like PMTiles and it is not PMTiles. What the two share is the only property that
mattered: a tile is at a known offset in one file, so reading one is a seek and a read. PMTiles'
own directory encoding - varints, compressed leaf directories, a hilbert-ordered id space - is
what makes a *planet* file readable over HTTP range requests, and none of it is needed by a
regional pack whose whole index fits in RAM. If step 5 ever fetches tiles over range requests,
that is the moment to read the real thing; until then this is the same idea with the parts a
Brick does not need left out.

Little-endian throughout, read byte by byte:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | `"MCTPACK2"` |
| 8 | 2 | tile size in pixels |
| 10 | 1 | tile format (1 = PNG) |
| 11 | 1 | reserved, zero |
| 12 | 4 | index offset from the start of the file |
| 16 | 4 | tile count |
| 20 | 4 | reserved, zero |
| 24 | 8 | generated: seconds since the epoch, 0 when the builder did not say |
| 32 | 64 | attribution, UTF-8, NUL padded |
| 96 | 32 | name, UTF-8, NUL padded |
| 128 | 24 x count | the index, sorted ascending by (z, x, y) |

An index entry is `u8 zoom`, three reserved bytes, `u32 x`, `u32 y`, `u32 length`, `u64 offset`.
The tile bytes follow the index, in index order. "MCTPACK2" rather than "MCTPACK1" because
there was a 1: the headerless layout `devtools/tile_bench` measured with, which was never a
format anything shipped and carried no way to say what was in it. The entry is deliberately
unchanged at 24 bytes, so the numbers the benchmark produced are the numbers this reader gets.

Four decisions in it are worth stating, because each one is a bug this cannot have:

- **The zoom range and the coverage are derived from the index, not declared in the header.**
  It is the rule the app bar's back arrow and the map's selection already follow. A header field
  saying where a pack covers is a field that can be wrong, and the way it goes wrong is a builder
  that names a city and packs a suburb - believed by every screen that draws it. The union is
  taken across zooms on the unit square, so a coarse tile that reaches further than the deep ones
  sets the edge.
- **Everything a read would have to trust is checked once, at open.** Zooms the client can
  address, tiles inside the world at their own zoom, extents inside the file, and the index's
  own sort order - because a binary search over an index that is not sorted does not fail, it
  *misses*, which on the device is a blank square nobody can explain. Afterwards a tile is a
  `bsearch` and one `pread` and cannot be wrong about anything, which is what keeps the per-frame
  cost off the event loop.
- **The tile dimensions and the format are restricted rather than believed**, which this
  document asks for by name. A file claiming 512-pixel tiles is a screen full of quarter-tiles
  found out at the blit; a file holding vector tiles is the failure MBTiles is famous for, where
  a raster-only reader opens one happily and draws nothing.
- **A missing tile is an answer, not an error.** Every pack is a rectangle of the world with
  holes in it - a sparse pack is the ordinary case for a coastline - so a read that finds nothing
  returns 0 and the map draws the hole, which is a state it needs anyway while a tile is still on
  its way.

What is deliberately *not* in it: a compression flag (the tiles are PNGs, which are already
deflated), a per-tile checksum (the PNG's own CRCs cover the bytes, and the decoder checks them),
and leaf directories. A pack of a million tiles would hold 24 MB of index in RAM, which is where
the format would have to grow them - and a single file on FAT32 cannot exceed 4 GiB, so a region
that large is split into files either way. Neither is a rendering question.

## What the decoder actually cost

> **Measured 2026-09-11**, integrating the decoder the spike chose. The spike's figure was
> +106 KB of code; the client's is **335 KB**, and the difference is not Wuffs.

`devtools/tile_bench/build.sh` compiled its size probes with `-ffunction-sections
-fdata-sections` and linked them with `-Wl,--gc-sections`, so a function nothing called was
dropped before it was measured. [`scripts/cross-build.sh`](../scripts/cross-build.sh) builds the
pak with plain `-Os` and none of those. With no per-function sections there is nothing for a
linker to drop *inside* an object file, so the whole compiled subset ships whether or not the
client reaches it - and the subset is large because Wuffs' BASE module carries a general pixel
swizzler that can convert any format to any other, plus a 16 KiB CRC32 table.

Measured on the host at `-Os`, which is the pak's optimisation level if not its target:

| | text | data | bss |
| --- | ---: | ---: | ---: |
| `wuffs_png.c.o`, BASE named whole | 366,371 | 1,032 | — |
| `wuffs_png.c.o`, BASE's three needed sub-modules | **335,107** | 488 | — |
| the client's own decoder state (`tile_image.c`) | — | — | 311,552 |

Two things follow, one taken and one proposed.

**Taken: BASE is named by sub-module.** PNG reaches CORE, INTERFACES and PIXCONV; FLOATCONV,
INTCONV, MAGIC and UTF8 are what a JSON decoder wants and are now never compiled. It is worth
31 KB under the pak's build and 128 bytes under a build that collects sections, which is a neat
demonstration of where the cost actually lives.

**Proposed, and deliberately not done here: give the cross build `-ffunction-sections
-fdata-sections -Wl,--gc-sections`.** Measured on the host, it takes **136 KB** off the whole
binary - most of it not the decoder's - and the spike already built and ran a static aarch64
binary that way on the device, so the configuration is not speculative. It is a change to how
the pak is produced rather than to what is in it, which is why it is a separate decision: it
touches every release asset and the self-update path, where the binary is what gets downloaded.

The bss line is the client's own and is a deliberate constant: a 48 KiB decoder (Wuffs' struct
is opaque in C and its size is explicitly not stable across versions, so the block is sized with
headroom and checked at every decode) and a 256 KiB scratch buffer. The scratch is sized for
8-bit RGBA - `width * bytes_per_pixel * height + width`, which is 262,400 at four bytes a pixel -
because a style with transparency is an ordinary thing to publish. Sixteen bits a channel needs
twice that and is refused with a stated `-ENOTSUP` rather than paid for: no tile server emits it
and no panel here could show it. None of it is allocated per tile, which is the property that
matters on an event loop - a decode cannot pause to find memory and cannot fail for want of it.

## What the cache turned out to be

> **Landed 2026-09-11.** [`src/map/tile_cache.c`](../src/map/tile_cache.c), 15 cases in
> `tests/suites/tile_cache.c`, and no pack or decoder linked into any of them.

Three decisions, of which only the first was the one step 3's entry expected to be making.

**A cached tile is the decoder's pixels, not the panel's.** The entry above asked for "the
decoded representation chosen to suit the blit rather than the decoder", and that came out the
other way round for a reason that is about the boundary rather than about bytes. `struct
fb_state` reads its `bytes_per_pixel` off the kernel at runtime; storing panel-format pixels
means `src/map/` learning that number, which is the one thing §"Proposed module boundaries" says
the map layer may not do. It would also make a cached tile the property of one backend - the
capture harness renders at four bytes a pixel whatever the device is doing, so one cache would be
holding the wrong format for one of the two. The cost is stated rather than hidden: on a panel
narrower than four bytes the cache holds more bytes than that panel needs, and the blit converts
per *drawn* pixel, joining the switch `fb_fill_packed()` already makes rather than adding one.

**A missing tile has to be remembered, and it is the reason the cache has two tables.** This was
not in the plan at all, and it is what step 3's one-decode-per-turn budget runs into: every pack
is a rectangle of the world with holes in it, and without a record of what has already been
asked for and refused, a single hole on screen spends that one read every turn forever - so a
view with any sea in it never finishes filling the tiles around it. The holes are kept in a
table of their own because a hole costs twelve bytes and a tile costs 256 KiB, and sharing the
slots would let a sparse view evict the picture to remember the sea. The invariant that makes
the pair safe is that **a key is in at most one of the two tables**, or the cache answers READY
or ABSENT for one tile depending on which it consults first.

That is also why what a lookup returns is a *state* rather than a pointer that may be NULL.
`MISS` and `ABSENT` are drawn differently - one is a tile still on its way and the other is the
final picture of open water - and a renderer that could not tell them apart would either show a
loading state forever over the sea or stop drawing one at all.

**The cache counts its own answers, because nothing else can ask for the repaint.** A frame in
this client is a function of the snapshot, and no radio packet arrives to say that a tile
landed. `mesh_map_tile_cache_revision()` changes whenever the cache's answer about some key
changes - a commit, an eviction, an absence learned or dropped, a clear - and not on a plain
hit, which changes only what eviction takes next. Where the two readings differ it errs towards
reporting a change: a repaint nobody needed costs a frame, and one that did not happen is a map
that stays blank until the user presses something. This is the "publish small resource
revisions and loading state" §"Proposed module boundaries" asks for, in the one place that
needs it.

Two smaller things worth keeping. Eviction is a linear scan over at most 64 slots rather than an
LRU list, because a frame asks about twenty tiles and the whole of that bookkeeping is less
arithmetic than placing one marker - where a list that comes apart leaks a slot silently and
shows up as a map that reads the card more often as it runs. And **opening a different source
must clear the cache**: a key is three numbers about the world rather than about a file, so two
packs of the same city hold different pictures at the same key, and a cache carried across a
swap draws the old pack's streets under the new pack's attribution with nothing on the frame
looking wrong.

## What the fill loop and the blit turned out to be

> **Landed 2026-09-11.** [`src/ui/backends/fb_map.c`](../src/ui/backends/fb_map.c) (the fill
> loop, `fb_basemap_*`), [`src/ui/backends/fb_draw.c`](../src/ui/backends/fb_draw.c)
> (`fb_blit_bgra()`), and three cases in `tests/suites/ui_capture.c` that draw a fixture pack
> through the real renderer.

The entry this closes said "one decode per turn, and the clipped blit". Both are there and
neither was the interesting part. Five things are worth recording.

**A hole costs no read at all, and that is what makes one read a frame enough.** The plan
budgeted one *read* per turn and expected absences to be learned by spending one. They are not:
`mesh_map_source_has()` answers out of the index already in RAM, so a tile the pack does not hold
is recorded as absent for the price of a `bsearch`, and the frame's one read is always spent on a
tile that will actually arrive. Without it, a view with any sea in it spends its whole budget
discovering the sea again - which the cache's absence table already prevented across frames, and
this prevents *within* one.

**The tile that gets read is the one nearest the middle of the view.** A view fills outwards from
the crosshair, which is where the reader is aiming and where the selection is derived from. Filling
from the top-left corner is the obvious loop and is wrong for the same reason the selection is
measured in pixels from the centre.

**The graticule stayed, and the tiles are drawn over it.** The grid was written as "the whole of
the basemap for now" and the obvious change was to replace it. Drawing it underneath instead
means it survives in exactly the places there is no tile - the edge of a regional pack, the holes
in it, the second before a tile arrives - so nothing has to decide whether to draw a grid, and a
pack's edge is a thing a reader can see rather than a blank they have to interpret.

**MISS and ABSENT are drawn the same, against what §"What the cache turned out to be" says.**
That section argued they are two different pictures and a renderer that could not tell them apart
would show a loading state forever over open sea. Drawing them apart turned out to be worse than
either: a placeholder square over each missing tile covers the markers for the two thirds of a
second a view takes to fill, which is the whole of the time anybody is looking at it. So the
difference between them stayed exactly where it was useful - the cache still answers two ways,
and what differs is whether the client asks for another frame - and none of it reaches the ink.
The distinction earns its keep in the loop rather than on the panel.

**Text over a picture needed something the rest of this client has never needed.** A glyph here
carries coverage rather than a mask, so every run is blended against a colour the caller says it
has just filled - true everywhere in a client that fills its own rows, and a guess the moment a
name stands on a street. A marker's name is drawn a pixel out in each direction in the ground
colour and then over itself; the scale bar and the pack's attribution get a chip instead, because
they are chrome pinned to a corner rather than things on the map. Both only where a tile is: over
the bare graticule they would rub out the lines a distance is judged against.

Doing that surfaced a bug that was already there, which is the usual way. `fb_draw_text()` places
a run by the top of its *cell*, and the map's labels were reserving a collision box a line above
where they landed - so a name was tested against a rectangle nothing was drawn in, and the
overlap rules the label pass exists for had been quietly inert. The halo made it visible because
a halo in the wrong place is a halo you can see.

### What a pack costs to look at, and why one is drawn rather than shipped

`map_pack.py synth` draws a pack of a place that does not exist: flat landcover, water, a road
grid with casings and building blocks, all as a function of position in a fixed reference space,
so the same street is in the same place at every zoom. It exists because the two questions left
in step 3 - what a filled panel looks like and what a tile costs to read - are not questions
about *whose* map it is, and a synthetic pack answers both without the source decision this
document keeps for step 5.

It is also what keeps the repository free of tiles. A pack of the demo roster's own coordinates
is 244 tiles and about a megabyte; `make demo-pack` writes it in four seconds from a stdlib-only
script, deterministically, which is what a scene compared against a reference frame needs.

### What is still open

- **The integrated latency number**, on a Brick, with BLE being serviced. Everything about the
  fill loop's shape is derived from the standalone benchmark in §"What the Brick measured"; what
  has not been measured is a frame of the real client with a decode in it.
- **The blit's cost on a 16-bit panel.** The fast path is a copy with the alpha forced opaque,
  taken when the mapping holds exactly the word the decoder produces - which the Brick's 32 bpp
  fb0 does. Anything else packs per run of one colour, which is cheap on the palette tiles a pack
  is built from and has not been measured on anything else.
- **Partial redraw.** A map frame redraws the whole body, tiles included, whenever anything
  changes. The frame is compared against the previous one before it reaches the panel, so nothing
  *transfers* twice - but the blit runs regardless, and a pan that moves the view by a few pixels
  redraws twenty tiles to move twenty tiles.
- **Choosing between packs**, which is a settings screen and belongs with step 4's import step.
  Today it is one path and one pack.

## Proposed module boundaries

Names below are proposals, not APIs that already exist.

| Module | Owns | Reused by |
| --- | --- | --- |
| `include/mesh/geo/`, `src/geo/` | Coordinate validation, conversion, distance/bearing, Web Mercator projection and inverse | Node details, maps, waypoints |
| — *exists:* [`include/mesh/geo/coords.h`](../include/mesh/geo/coords.h) | The bounds test alone, asked by every ingress | Session ingestion, the cache loader, fixed position |
| — *exists:* [`include/mesh/geo/vector.h`](../include/mesh/geo/vector.h) | Distance and initial bearing between two points, and the eight-point compass. Written because the Waypoints tab needed a range, not speculatively - projection still waits for a map. The one `<math.h>` in the tree | The Waypoints list and one place's detail |
| — *exists:* [`include/mesh/geo/mercator.h`](../include/mesh/geo/mercator.h) | Web Mercator and its inverse, the display limit, the longitude wrap, and the projection’s own scale factor. The second `<math.h>` in the tree | The viewport, and anything that needs a coordinate turned into a position |
| — *exists:* [`include/mesh/map/viewport.h`](../include/mesh/map/viewport.h) | Centre/zoom, world-to-screen and back, pan, bounds fitting, metres per pixel. **No tile keys**: those arrive with a tile to fetch | The map screen, and a future location preview |
| — *exists:* [`include/mesh/map/tile.h`](../include/mesh/map/tile.h) | The pyramid's own vocabulary - tile size, the zoom range, a key, and the block of them a box covers. Its own header because the viewport and a source both need it and must not include each other | The viewport and every source |
| — *exists:* `mesh_map_viewport_tiles()` in [`src/map/viewport.c`](../src/map/viewport.c) | Which tiles the box is standing on, and where the first one's corner lands. Measured against the box the viewport *has*, which is why a renderer resizes its own copy first | The map screen, and a "download what is on screen" press later |
| — *exists:* [`include/mesh/map/source.h`](../include/mesh/map/source.h), [`src/map/source_pack.c`](../src/map/source_pack.c) | Map metadata and tile-byte lookup behind a small source interface. One implementation, the single-file pack the device measurement chose; an HTTP source would arrive here and change nothing above | The offline pack, `--map-pack`, and step 5's optional HTTP source |
| — *exists:* [`include/mesh/map/tile_image.h`](../include/mesh/map/tile_image.h), [`src/map/tile_image.c`](../src/map/tile_image.c) | A tile's bytes to pixels, in one stated order, with a bounded and constant footprint. The only file in the client that includes Wuffs | The tile cache, and `--map-pack` |
| — *exists:* [`include/mesh/map/tile_cache.h`](../include/mesh/map/tile_cache.h), [`src/map/tile_cache.c`](../src/map/tile_cache.c) | Byte-budgeted decoded tile cache, request de-duplication and eviction - plus the holes, which are what stop a sparse pack spending the fill budget forever. A store, not a loader: it opens nothing and decodes nothing | Any map viewport |
| — *exists:* `struct mesh_ui_map_node` in [`include/mesh/ui/store.h`](../include/mesh/ui/store.h) | The compact projection: every positioned node the *session* holds, not the ranked 128 the list publishes. Filled by `src/core/app_publish.c` | The markers, and anything else that wants a position without a summary |
| — *exists:* [`src/ui/map.c`](../src/ui/map.c) | Markers from the map's roster and the waypoint book; the selection, measured from the view’s centre in metres | Framebuffer and test consumers |
| — *exists:* [`src/ui/nav_map.c`](../src/ui/nav_map.c) | Button handling and map navigation state. Takes the d-pad *ahead* of the tab routing, and deliberately leaves the shoulders to it | Existing store/controller path |
| — *exists:* [`src/ui/backends/fb_map.c`](../src/ui/backends/fb_map.c) | Graticule, markers, labels, crosshair and scale bar. Tiles and attribution when there are any | Device and off-screen capture |
| — *exists:* [`devtools/map_pack/`](../devtools/map_pack/map_pack.py) | Validate and prepare regional packs from MBTiles or a `z/x/y` tree, and show coverage and size. Where the TMS row order is turned the right way up, so the client sees one convention | Host workflow and fixtures |

Geography and viewport code should not include protobuf, UI store, framebuffer or filesystem
headers. Keep tile addressing in the map layer; normalize storage-specific row conventions
inside the source adapter. Backend code consumes ready assets and never opens files or starts
downloads in `present()`.

Preserve the existing core-to-UI boundary. A small common coordinate value type is reasonable;
merging the entire session and UI node records to remove field copying is not necessary.
Do not make a generic GIS engine or a new widget framework. Extract drawing helpers when the
map actually needs them, and keep framebuffer-only components inside the backend directory.

Tile bytes belong in a resource cache, not in every copied `mesh_ui_snapshot`. Publish small
resource identifiers/revisions and loading state; define asset ownership and lifetime through
presentation. Completion must invalidate the map even when no new radio packet has arrived.
Capture tests should inject the same resource interface with deterministic fixture tiles.

## Basemap and resource strategy

> **Answered by measurement (2026-09-11)** - see §"What the Brick measured". The comparison below
> was run, and it came out the other way from the preference stated in it: MBTiles lost to a
> single-file indexed pack on FAT32, on fetch latency, under load and on binary size, and Wuffs
> was chosen as the decoder. The paragraph is kept as the question that was asked.

Prototype with synthetic tiles, then compare a directory of XYZ PNG tiles against read-only
raster MBTiles on the device. Prefer MBTiles for a user-facing regional pack if its SQLite
and decoder costs are acceptable: one file is convenient to validate, transfer and replace.
Keep the source seam small and implement only the chosen production format initially.

[MBTiles 1.3](https://github.com/mapbox/mbtiles-spec/blob/master/1.3/spec.md) uses SQLite and
spherical Mercator tiles; its tile rows follow TMS ordering. An XYZ-facing adapter must convert
`tile_row = (1 << z) - 1 - y`. Explicitly restrict accepted format, tile dimensions and zoom
range; a file named `.mbtiles` can contain vector data that a raster-only reader cannot draw.

Use a maintained decoder behind one image interface; verify licensing, static aarch64 builds,
bounded allocation and malformed-input handling before choosing a library. Account for math
library linkage in the projection implementation. Avoid committing regional tile archives.
Store user map packs in per-pak state, independently of binary updates, with source,
attribution, coverage, supported zooms and generation date available in metadata.

For scale: a decoded 256×256 RGBA tile occupies 256 KiB. At a full 1024×768 viewport, arbitrary
alignment can intersect 5×4 tiles (5 MiB decoded); the actual map body is smaller. A proposed
32-tile cache is 8 MiB, excluding decoder scratch space and the existing framebuffer buffers.
These are arithmetic budgets, not device measurements. Compressed pack size depends on area,
style and zoom; adding one zoom level roughly quadruples tile count for the same area.

Budget disk reads and decoding per event-loop turn. A bounded count of tiles is not proof of
bounded latency: measure a cold read plus decode on the Brick's storage. If it stalls input or
BLE servicing, use a bounded helper-process protocol integrated through epoll, or prepare a
cheaper on-device representation on the host. The architecture forbids adding threads.

Start offline. OSM's standard public raster server explicitly prohibits bulk downloads and
offline packs; use self-rendered tiles or a source permitting the intended offline distribution.
Keep visible attribution in the map layout. See the
[OSMF tile policy](https://operations.osmfoundation.org/policies/tiles/), checked 2026-09-07.
Provider choice and permitted regional coverage remain open decisions - see §"How a pack gets
onto the device", which revises what "start offline" was resting on.

If online tiles follow, add bounded requests, cancellation, timeout/backoff, cache validation,
disk quotas and atomic writes. Reuse/extract the updater's process and certificate mechanics
where appropriate, not its release-download state machine. Tile HTTP is a map source, not a
Meshtastic radio transport. Disconnection must leave cached maps and markers usable.

## How a pack gets onto the device

> **Revised 2026-09-10**, and the paragraph above is what it revises. "Start offline" was
> written as though the Brick had no network, which is wrong: the correction and what it does
> and does not change are below.

**The Brick has WiFi, and this client already uses it.** [`docs/device.md`](device.md) opens on
getting a Brick to "receives builds over WiFi"; [`src/core/updater.c`](../src/core/updater.c)
forks curl and ships its own CA bundle, because the Brick has no system CA store - no
`/etc/ssl` at all. Every install from the Pak Store is a download. So nothing about this
hardware forces a pack to arrive on an SD card.

**What is true is that the Brick is offline nearly all the time it is being used**, which is a
different and more useful statement. It is on a network at moments the reader chooses - at
home, at the hotel, before setting out - and off it for the whole of the time the map matters,
because a mesh is what you carry where there is no other network. That asymmetry says exactly
when a download may happen: **never while the map is being read**, and always at a moment the
reader picked. It is a weaker constraint than "no network ever" and a sharper one, because it
rules out on-demand tile fetching as a *strategy* while leaving downloading as a *feature*.

### Acquisition and rendering are separable, and the spike is about rendering

Worth stating plainly, because this document ran the two together and so did the reasoning that
put "online sources" under optional extensions.

- **Rendering** is whether the Brick can read a tile off its storage and decode it fast enough
  not to stall the event loop while BLE is being serviced. That is the whole of what step 3
  asks, and **how the pack arrived does not change the answer** - a cold read plus decode costs
  what it costs whether the bytes were copied over USB or fetched over WiFi.
- **Acquisition** is how the pack gets there. It is a product question, it is where the
  licensing lives, and it can be answered after step 3 without step 3 waiting on it.

This also settles what "choose a first region" means for the spike, which the closing section
overstates: for a *measurement* it is a test fixture, not a commitment. Any few square
kilometres will do, including synthetic tiles. The region question only becomes a product
decision at step 4.

### Two acquisition shapes, and the target wants both

Not alternatives. They are what a reader does at two different moments, and neither covers the
other:

1. **A region chosen ahead of time.** The Merlin Bird ID model: at the hotel, on WiFi, before
   the trip, you fetch the pack for where you are going. This is the common case, because a
   reader generally knows where they will be, and it is the one that can fetch a *large* area
   while there is bandwidth and time to spare.
2. **Download what is on screen.** The map already pans and zooms, so the viewport can be asked
   to *describe* what it is showing: `mesh_map_viewport_at()` is the pixel-to-coordinate
   inverse, and run on the box's corners it gives a bounding box, which with the zoom is a
   request. "Get me this" then needs no region list, no place-name search and no keyboard. It is
   the answer when the plan changed and there is still a network.

   Two things to get right, neither of which is free. `mesh_map_viewport_fit()` is **not** the
   operation and cannot be run backwards - it takes points and moves the centre and zoom, and
   hands back no bounds. And the corners have to come from the box **actually drawn into**: the
   nav's own viewport carries the declared `MESH_UI_MAP_FIT_WIDTH`, deliberately smaller than
   any real body, which is the safe direction for a fit (too small only leaves air) and the
   wrong one for a download (it would fetch less than the reader can see). Only the backend
   knows the real body - `fb_render_map()` resizes a copy of the nav's viewport to it on every
   frame - so this press needs the measured box to reach it, which is the same seam the fit
   declined to open.

The first needs a way to *name* a region without a map of the world to point at, which is a real
UI problem on a d-pad - a list of pre-cut packs is the cheap answer and a coarse world map you
pan is the better one. The second has no such problem and is nearly free given the map that
exists. Neither is a reason to drop the other.

### The constraint that gates this: no self-hosted tile service

**As of 2026-09-10 the project is not committing to running a map download service.** This is a
stated project constraint rather than a technical finding, and it is revisable - but until it
changes, it decides what an in-app downloader can be.

What it does **not** block: the step 3 spike (no source needed - synthetic or self-rendered
tiles answer the measurement), and the step 4 sideload path (the reader builds or fetches a pack
on a computer, so the project hosts nothing).

What it does block is the in-app downloader, which has to download *from somewhere*. The options
and what each costs:

| Source | What it needs | Why it is awkward here |
| --- | --- | --- |
| OSM's public raster server | Nothing | Explicitly prohibited for this use; not an option |
| A commercial provider (API key) | A key shipped in the pak | The pak is open source. A key in it is a key anyone can extract and spend, and the quota is the project's |
| A provider, reader-supplied key | A settings field | A URL template plus a key runs right at `MESH_UI_SETTING_TEXT_MAX`, which is 80, so it may not even fit - and a d-pad keyboard is not a way to enter one either. A reader who must drop a config file on the SD card could have dropped the pack there instead |
| Self-hosted | A service, and a bill | Ruled out for now |

The middle rows are why the sideload path stays the honest first release: **an in-app downloader
is only clearly better than sideloading when it needs no secret from the reader.** That is the
condition to test a source against, and it is a sharper filter than "does the licence permit
offline use".

One format is worth evaluating specifically because it narrows the hosting question rather than
answering it: a **single-file tile archive read over HTTP range requests**
([PMTiles](https://docs.protomaps.com/pmtiles/) is the current one; MBTiles is its SQLite-shaped
predecessor and is not designed to be read that way). It turns "run a tile server" into "put a file
on static storage", which is a much smaller commitment than a service - and the *same file* is
what a reader would sideload, so the two acquisition paths share a format instead of each having
one. It is still a file somebody hosts, so it does not make the constraint above go away.

Attribution is required on the map layout whichever path a pack arrives by, and the metadata a
pack carries - source, attribution, coverage, supported zooms, generation date - is what makes
that drawable.

## Delivery sequence and acceptance

1. **Geography and location contract.** Pure helpers and tests for coordinate limits, missing
   fields, zero coordinates, unknown/stale times, precision, longitude wrap and Mercator polar
   clamping. Keep full geographic latitude separate from the projection's display limit.
2. **Map without a basemap.** Nodes-screen entry plus node-detail "show on map", grid, markers,
   pan, integer zoom, recenter and return navigation. Publish all live session markers with
   explicit restart coverage. Reuse existing node details and actions. Add capture fixtures
   for no fixes, one fix, overlaps, 256 nodes, radio swap and antimeridian neighbors.
3. **Offline raster spike.** Small licensed regional pack, decoder, clipped image blit and
   resource lifecycle. Measure cold/warm pan, memory, executable growth and input latency on
   the Brick while receiving mesh traffic. Select the production source based on those results.
   *Done, bar the measurement on hardware: the source is selected and its reader, its format,
   its host-side builder, the decoder, the cache, the fill loop and the blit have all landed -
   see §"The pack format", §"What the decoder actually cost", §"What the cache turned out to be"
   and §"What the fill loop and the blit turned out to be". A pack can also now be *drawn* rather
   than converted (`map_pack.py synth`), which is what took the licence question off step 3's
   path entirely.*
4. **Offline release.** Pack validation/import instructions, loading/missing/corrupt tile states,
   bounded cache, stale/approximate markers, label prioritization, scale and attribution.
   Include cache/source changes in repaint invalidation and deterministic capture tests.
   Sideload is the delivery path here, and remains so while §"How a pack gets onto the device"
   rules out hosting a service - not because the hardware requires it.
5. **Downloading a pack on the device.** Promoted out of "optional extensions", where it sat on
   the mistaken premise that the Brick had no network. Both shapes: a region chosen ahead of
   time, and the area currently on screen. Bounded requests, cancellation, timeout/backoff,
   disk quotas, atomic writes, and resumption - a regional pack is thousands of requests and a
   reader will walk away mid-download. **Gated on a source that needs no secret from the
   reader**, which is the filter, not the licence alone.
6. **Optional extensions.** Saved areas, trails and neighbor edges can follow independently.
   Neighbor links express reported connectivity, not radio range; neither traceroutes nor
   neighbor reports provide route navigation.

Suggested controls for the initial nested map view: D-pad pans, X/Y zoom, A opens the selected
marker, B returns, Select recenters, shoulders cycle visible markers. Route these before the
global tab handler while the map is open. Use shared button hints and localized string IDs;
confirm readability and control discoverability through captures and hardware use.

Run `make docker-test` on macOS, add deterministic source/cache and projection cases, and use
`make docker-ui-capture` for transitions. Test tile seams, negative/world-edge coordinates,
XYZ/TMS conversion, clipped blits, allocation failure, corrupted packs and selection after
roster reorder. Compare map frames with the full-render reference to catch damage-invalidation
bugs. Run format through the repository's prescribed command before pushing code.

A map release also needs the documented pak inspection, sideload and NextUI launch checks.
Changes to launcher scripts or bundled helpers require a pak reinstall; ordinary application
changes should not assume those files arrived through self-update.

## Effort and first decision

Planning estimate for one developer familiar with this tree: 2–4 focused days for the location
foundation, 3–5 for an interactive marker view, 5–10 for offline tile integration, and 3–5 for
pack workflow and device hardening: roughly 3–5 working weeks for a usable offline release.
These are estimates, not measured commitments; storage/decode latency and source preparation
are the largest unknowns. They do not cover step 5, which was an optional extension when they
were written. A marker-only milestone can land substantially earlier.

Steps 1 and 2 have shipped, and step 3's *measurement* has run - see §"What the Brick measured",
which chose a single-file pack, Wuffs and one tile per event-loop turn. The **pack reader behind
the source seam has landed** with it - see §"The pack format" - along with the viewport's tile
keys, a host-side builder and `--map-pack` for reading one back on the device.

**Recommended next implementation is the rest of step 3**, in this order, because each one is
useless without the one before it:

1. ~~**The decoder.**~~ **Done**, 2026-09-11. Wuffs at a pinned revision in
   `third_party/wuffs`, behind `mesh_map_tile_decode()`. What the spike predicted held except
   for the code size, which is a fact about the pak's build - see §"What the decoder actually
   cost". Two numbers the spike had not pinned down: the work buffer is a function of the
   file's *colour type* and not only of the tile size, so it is sized for 8-bit RGBA rather
   than for the palette tiles a pack is built from; and the decoder's struct is opaque in C, so
   its size can only be asked for and is checked at every decode.
2. ~~**The tile cache.**~~ **Done**, 2026-09-11. Byte-budgeted and LRU, keyed on
   `struct mesh_map_tile_key`, behind [`include/mesh/map/tile_cache.h`](../include/mesh/map/tile_cache.h).
   The open question in this entry - what a cache holds against what a panel wants - is
   answered in favour of the decoder, and two things came out of building it that were not in
   the entry. See §"What the cache turned out to be".
3. ~~**One decode per turn, and the clipped blit in `fb_map.c`.**~~ **Done**, 2026-09-11. A
   frame draws what the cache holds and then reads exactly one more tile, nearest the middle of
   the view; the blit goes through the same clip every fill in that backend goes through. Three
   things came out of building it that the entry did not anticipate, and one of them is a
   correction to a bug that was already there. See §"What the fill loop and the blit turned out
   to be".
4. **The integrated latency number**, which is the one thing here that needs a Brick: the
   input-latency measurement the standalone benchmark could not give, taken inside the real
   client with a pack on the card while BLE is being serviced. The *capture* half of this entry
   landed with step 3 - `map pack` in a scene script, `make demo-pack` to draw the pack it names,
   and three cases in `tests/suites/ui_capture.c` that render a fixture pack through the real
   renderer. A pack is drawn rather than committed: 244 tiles of synthetic streets is a megabyte
   of binary in a repository, against four seconds of a stdlib script that produces the same
   bytes every time.

The history of how step 3 was unblocked follows. **It was no longer blocked on anything.** This section
used to name two decisions it waited on - a first region and zoom range, and a tile source with
offline rights - and §"How a pack gets onto the device" retires both as blockers: for a
*measurement* a region is a test fixture rather than a commitment, and a source is an
*acquisition* question that step 3 does not touch. Synthetic tiles, or a self-rendered pack of a
few square kilometres, answer what step 3 actually asks: what a cold read plus decode costs on
the Brick’s storage while BLE is being serviced. That number decides whether raster is viable
here at all, and nothing else should be built before it is known.

The source decision does not disappear, it moves: it gates **step 5**, the on-device download,
and the filter to judge a candidate by is sharper than a licence - see the table in §"How a
pack gets onto the device".

The decision carried over from the fourth pre-work item - whether the map sees everything the
session holds - **has been taken**; see §"The roster decision, taken". Two smaller things came
out of it and both are independent of tiles: resolving a **map-only node's detail** through the
app/store seam, which the fifth pre-work item described and which is now a real case rather than
a hypothetical one, and **cache coverage**, which is still deliberately open.
