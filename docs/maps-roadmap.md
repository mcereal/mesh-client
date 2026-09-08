# Map support assessment

Status: the map itself is proposed, based on the repository inspected on 2026-09-07; no
runtime changes or device benchmarks accompany that part of the assessment. The **pre-work in
§"Pre-work that matters" has since shipped**, on its own and ahead of any map - see the note
there for what each item became.

## Recommended first release

Build an offline, north-up raster map showing the client's known node locations. Support
button-driven pan, integer zoom, centering on the connected radio or a selected node, and
opening node details. Keep markers useful when no basemap covers the viewport. Prepare and
sideload a small regional map pack on a host computer before adding downloads on the Brick.

This fits the existing C17, software-framebuffer architecture. Vector maps would add geometry
decoding, styling, label placement and font concerns; defer that work until a concrete need
justifies it. Routing, address search, terrain, track history and waypoint sharing are separate
features, not prerequisites for viewing nodes on a map.

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

   > **Partly done - the reporting half.** The projection is map work and waits for a map.
   > What could not wait is that the truncation was *silent*: the session holds 256 and the UI
   > publishes its best 128, and the Nodes tab could only compare itself against the radio's
   > `nodedb_entries`, which is a different set and, after a NodeDB reset, the smaller one.
   > `handshake.nodes_known` now carries the roster's own total and the title takes whichever
   > of the two is larger, so "128 of 200" is sayable. The persistence question is still open
   > and still deliberately so: the cache holds the published 128, and widening it is a
   > decision for whoever needs all 256 back after a restart.

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

## Proposed module boundaries

Names below are proposals, not APIs that already exist.

| Module | Owns | Reused by |
| --- | --- | --- |
| `include/mesh/geo/`, `src/geo/` | Coordinate validation, conversion, distance/bearing, Web Mercator projection and inverse | Node details, maps, future waypoints |
| — *exists:* [`include/mesh/geo/coords.h`](../include/mesh/geo/coords.h) | The bounds test alone, asked by every ingress. Deliberately not more: conversion, distance and projection are written when a map needs them, not speculatively | Session ingestion, the cache loader, fixed position |
| `include/mesh/map/viewport.h`, `src/map/viewport.c` | Center/zoom, world-to-screen transforms, pan, bounds fitting, visible tile keys | Full map, future location preview |
| `include/mesh/map/source.h`, `src/map/source_*.c` | Map metadata and tile-byte lookup behind a small source interface | Offline packs; optional HTTP source later |
| `src/map/tile_cache.c` | Byte-budgeted decoded tile cache, request deduplication and eviction | Any map viewport |
| `src/ui/map.c` | Build marker/label geometry from published records; selection and collision policy | Framebuffer and test consumers |
| `src/ui/nav_map.c` | Button handling and map navigation state | Existing store/controller path |
| `src/ui/backends/fb_map.c` | Compose tiles, markers, scale and attribution using shared drawing primitives | Device and off-screen capture |
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
5. **Optional extensions.** Online sources, saved areas, waypoints, trails and neighbor edges
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

Recommended next implementation is steps 1 and 2, with a small offline raster spike soon after.
No provider account or regional pack is required to begin those steps. Before step 3, choose a
first region, useful zoom range and a tile source with suitable offline rights.
