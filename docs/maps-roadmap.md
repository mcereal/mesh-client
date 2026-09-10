# Map support assessment

Status: **steps 1 and 2 of the delivery sequence have shipped** - the geography contract is
complete and there is a marker map on the device, with no basemap under it, and it now draws
every positioned node the session holds rather than the 128 the node list publishes. See
§"What steps 1 and 2 became" for what landed and what it deliberately did not, and
§"The roster decision, taken" for the one open decision that has since been closed. Steps 3 to 5 -
the offline raster spike, the offline release and the optional extensions - are still proposed,
based on the repository inspected on 2026-09-07, and no device benchmarks accompany them. The
**pre-work in §"Pre-work that matters" shipped first**, on its own and ahead of any map.

## Recommended first release

Build an offline, north-up raster map showing the client's known node locations. Support
button-driven pan, integer zoom, centering on the connected radio or a selected node, and
opening node details. Keep markers useful when no basemap covers the viewport. Prepare and
sideload a small regional map pack on a host computer before adding downloads on the Brick.

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

## Proposed module boundaries

Names below are proposals, not APIs that already exist.

| Module | Owns | Reused by |
| --- | --- | --- |
| `include/mesh/geo/`, `src/geo/` | Coordinate validation, conversion, distance/bearing, Web Mercator projection and inverse | Node details, maps, waypoints |
| — *exists:* [`include/mesh/geo/coords.h`](../include/mesh/geo/coords.h) | The bounds test alone, asked by every ingress | Session ingestion, the cache loader, fixed position |
| — *exists:* [`include/mesh/geo/vector.h`](../include/mesh/geo/vector.h) | Distance and initial bearing between two points, and the eight-point compass. Written because the Waypoints tab needed a range, not speculatively - projection still waits for a map. The one `<math.h>` in the tree | The Waypoints list and one place's detail |
| — *exists:* [`include/mesh/geo/mercator.h`](../include/mesh/geo/mercator.h) | Web Mercator and its inverse, the display limit, the longitude wrap, and the projection’s own scale factor. The second `<math.h>` in the tree | The viewport, and anything that needs a coordinate turned into a position |
| — *exists:* [`include/mesh/map/viewport.h`](../include/mesh/map/viewport.h) | Centre/zoom, world-to-screen and back, pan, bounds fitting, metres per pixel. **No tile keys**: those arrive with a tile to fetch | The map screen, and a future location preview |
| `include/mesh/map/viewport.h`, `src/map/viewport.c` | Visible tile keys, once there are tiles | Full map, future location preview |
| `include/mesh/map/source.h`, `src/map/source_*.c` | Map metadata and tile-byte lookup behind a small source interface | Offline packs; optional HTTP source later |
| `src/map/tile_cache.c` | Byte-budgeted decoded tile cache, request deduplication and eviction | Any map viewport |
| — *exists:* `struct mesh_ui_map_node` in [`include/mesh/ui/store.h`](../include/mesh/ui/store.h) | The compact projection: every positioned node the *session* holds, not the ranked 128 the list publishes. Filled by `src/core/app_publish.c` | The markers, and anything else that wants a position without a summary |
| — *exists:* [`src/ui/map.c`](../src/ui/map.c) | Markers from the map's roster and the waypoint book; the selection, measured from the view’s centre in metres | Framebuffer and test consumers |
| — *exists:* [`src/ui/nav_map.c`](../src/ui/nav_map.c) | Button handling and map navigation state. Takes the d-pad *ahead* of the tab routing, and deliberately leaves the shoulders to it | Existing store/controller path |
| — *exists:* [`src/ui/backends/fb_map.c`](../src/ui/backends/fb_map.c) | Graticule, markers, labels, crosshair and scale bar. Tiles and attribution when there are any | Device and off-screen capture |
| `devtools/map_pack/` | Validate/prepare regional packs, show coverage and size | Host workflow and fixtures |

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
Provider choice and permitted regional coverage remain open decisions.

If online tiles follow, add bounded requests, cancellation, timeout/backoff, cache validation,
disk quotas and atomic writes. Reuse/extract the updater's process and certificate mechanics
where appropriate, not its release-download state machine. Tile HTTP is a map source, not a
Meshtastic radio transport. Disconnection must leave cached maps and markers usable.

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
4. **Offline release.** Pack validation/import instructions, loading/missing/corrupt tile states,
   bounded cache, stale/approximate markers, label prioritization, scale and attribution.
   Include cache/source changes in repaint invalidation and deterministic capture tests.
5. **Optional extensions.** Online sources, saved areas, trails and neighbor edges
   can follow independently. Neighbor links express reported connectivity, not radio range;
   neither traceroutes nor neighbor reports provide route navigation.

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
are the largest unknowns. A marker-only milestone can land substantially earlier.

Steps 1 and 2 have shipped; see §"What steps 1 and 2 became". **Recommended next implementation
is the offline raster spike (step 3)**, and the two decisions it is blocked on are unchanged:
choose a first region and useful zoom range, and choose a tile source with suitable offline
rights. Neither needs a provider account to begin measuring - a self-rendered pack of a few
square kilometres is enough to answer the question step 3 actually asks, which is what a cold
read plus decode costs on the Brick’s storage while BLE is being serviced.

The decision carried over from the fourth pre-work item - whether the map sees everything the
session holds - **has been taken**; see §"The roster decision, taken". Two smaller things came
out of it and both are independent of tiles: resolving a **map-only node's detail** through the
app/store seam, which the fifth pre-work item described and which is now a real case rather than
a hypothetical one, and **cache coverage**, which is still deliberately open.
