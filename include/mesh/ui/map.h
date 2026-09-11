#pragma once

/*
 * What is on the map, as data.
 *
 * The same split every other screen in this client makes: this builds the markers, a backend
 * draws them, and the nav walks between them - so what the map *shows* is a unit test's
 * business and what it looks like is a capture's. A screen that read the roster itself would be
 * the third place in the tree that decides which nodes have a position worth drawing.
 *
 * Both kinds of place go on it. The nodes are the reason the roadmap wanted a map at all, and
 * the waypoints are the reason it is worth panning: a shared place is the thing you navigate
 * *to*, and seeing it beside the nodes that can hear it is the one question neither list can
 * answer on its own - the Waypoints tab says how far away a place is from you, and this says
 * where two places are in relation to each other.
 *
 * Everything here is derived from the snapshot the store already holds, so opening the map
 * costs no radio traffic and works on a cached, disconnected client - the Waypoints tab's rule,
 * for the same reason.
 */

#include "mesh/geo/mercator.h"
#include "mesh/map/viewport.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many markers the map will carry.
 *
 * The map's own roster plus the waypoint book, which is everything the client holds that has
 * somewhere to be drawn - so this cannot truncate. It used to be the *published* roster's 128,
 * because that was all a snapshot carried; docs/maps-roadmap.md's fourth pre-work item left
 * open whether the map should see the session's whole roster, and struct mesh_ui_map_node is
 * that decision taken. What the map still owes the reader is how many of what is known has no
 * position at all, which is what `mesh_ui_map_view.known` is for.
 */
#define MESH_UI_MAP_MARKERS_MAX (MESH_UI_MAX_MAP_NODES + MESH_UI_MAX_WAYPOINTS)

/*
 * The body a fit is computed against, in pixels.
 *
 * A fit has to choose a zoom, and a zoom is only meaningful against a box - but the nav, which
 * owns the map's state, is never told how big a backend's body is: a backend is handed a
 * `const` snapshot and has no way to answer back. So the fit is computed against a declared
 * box rather than a measured one, and the declaration is deliberately *smaller* than any body
 * this client is drawn into. That is what makes the approximation safe rather than merely
 * convenient: a fit computed for a small box and drawn into a larger one leaves extra air
 * around the outermost marker, which is invisible, where the opposite would clip one off the
 * edge, which is the bug.
 *
 * It is also honest about what this client is. The pak targets one device with one panel; a
 * number derived from that panel is a fact about the hardware rather than a guess about a
 * renderer, and the capture harness draws at the same geometry.
 */
#define MESH_UI_MAP_FIT_WIDTH 880
#define MESH_UI_MAP_FIT_HEIGHT 420

/* How much air a fit leaves around the outermost marker, so one at the edge of the set is not
   drawn half off the panel - and so its label has somewhere to go. */
#define MESH_UI_MAP_FIT_MARGIN 40

/*
 * The zoom "show on map" opens at when it is aimed at one node.
 *
 * Closer than a fit, because the press said which node it was about: a reader who pressed
 * "show on map" on a neighbour wants to see where that neighbour is, not to be shown the whole
 * mesh with it somewhere inside. START widens back out to everything, which is the other half
 * of the same idea.
 */
#define MESH_UI_MAP_ZOOM_FOCUS 15

/* MESH_UI_MAP_LABEL_MAX - the longest thing drawn beside a marker - was declared here and now
   lives in mesh/ui/store.h, which this includes: it is the width of a published field, and a
   limit a producer and a consumer both have to agree about belongs beside the record. */

/*
 * What a marker is about.
 *
 * Our own radio is its own kind rather than a flag on a node, because it is the one marker that
 * is never a destination: it is where the reader is standing, every range on the panel is
 * measured from it, and it is drawn differently for that reason rather than because it happens
 * to be favourite or near.
 */
enum mesh_ui_map_marker_kind {
    MESH_UI_MAP_MARKER_SELF = 0,
    MESH_UI_MAP_MARKER_NODE,
    MESH_UI_MAP_MARKER_WAYPOINT,
};

struct mesh_ui_map_marker {
    uint8_t kind; /* enum mesh_ui_map_marker_kind */
    /*
     * Which node or which waypoint. Read in `kind`'s terms: a node number and a waypoint id are
     * both uint32_t and are not comparable, which is why a selection is the pair and never the
     * number alone.
     */
    uint32_t id;
    int32_t latitude_i;
    int32_t longitude_i;
    /* Already projected, because the fit and the placement both want it and projecting twice
       per marker per frame is a logarithm the map does not have to pay for. */
    struct mesh_geo_point point;
    char label[MESH_UI_MAP_LABEL_MAX];
    /*
     * How much the fix should be believed.
     *
     * `precision_bits` is the sender's own statement that it rounded the position - see
     * src/ui/settings.c's table, which turns it into the metres the node detail already shows -
     * and a marker drawn as a hard pin over a fix rounded to 360 metres would be the map
     * claiming a precision nobody sent. 0 means the node never set the field, which is not
     * "exact": it is "did not say", and it is the ordinary case.
     */
    uint8_t precision_bits;
    /* When the fix was heard, by our clock. 0 when we witnessed no arrival - a fix replayed out
       of the radio's NodeDB has none - which is the same absence the node detail relabels
       around rather than guessing at. */
    uint32_t received;
    /* A node the radio's NodeDB no longer carries: still ours to remember, and worth drawing
       differently from one it can still hear. Never true of a waypoint or of ourselves. */
    bool stale;
    /*
     * Whether A can open this marker: a node the ranked roster published a row for, or any
     * waypoint - the waypoint book is not cut, so a place on the map is always a place in the
     * list. See struct mesh_ui_map_node's `has_row` for how a node comes to have neither.
     */
    bool openable;
};

/*
 * The markers, and what could not be drawn.
 *
 * `known` and `count` are two different facts and the app bar says both: a mesh of two hundred
 * nodes where six have ever reported a position is a map with six markers on it, and a title
 * that said "6" alone would read as a mesh of six nodes. `known` is what the client holds,
 * `count` is what has somewhere to be drawn.
 */
struct mesh_ui_map_view {
    struct mesh_ui_map_marker markers[MESH_UI_MAP_MARKERS_MAX];
    uint32_t count;
    /* Every node and place the client holds, positioned or not. */
    uint32_t known;
    /* Whether one of the markers is our own radio, and which one - so a screen wanting to draw
       "you are here" differently does not go looking for it by node number. */
    bool has_self;
    uint32_t self_index;
};

/*
 * Builds the marker set from a snapshot.
 *
 * Ordered by kind and then by the order the underlying lists are already in: our own radio
 * first, then the nodes as the roster ranked them, then the places as the waypoint book holds
 * them. The order is a drawing order rather than a ranking - later markers are drawn over
 * earlier ones, so our own radio ends up under everything else, which is where a marker that is
 * always in the middle of the panel belongs.
 *
 * A node or place with no valid coordinates contributes to `known` and not to `count`. That is
 * the whole of the "which nodes go on a map" question, answered here rather than in a renderer.
 */
void mesh_ui_map_build(const struct mesh_ui_store *store, struct mesh_ui_map_view *out);

/*
 * How near the crosshair a marker has to be to be the selected one, in pixels.
 *
 * A little over a marker's own drawn size, so a reader who has the crosshair visibly on a
 * marker has it selected and one who has visibly missed does not. It is a pixel count rather
 * than a distance on the ground because it is a statement about aiming, and aiming is done on
 * a panel: at the deepest zoom this is a few metres and at the shallowest it is a county, which
 * is right both times.
 */
#define MESH_UI_MAP_SELECT_RADIUS_PX 28

/*
 * The selected marker: the one nearest the middle of the view, or none.
 *
 * This is how the map has a selection without having a cursor. The crosshair is the middle of
 * the panel, the reader pans the world under it, and whatever is nearest it is what A opens - so
 * panning *is* selecting, and no key has to be spent on choosing between markers. A d-pad that
 * stepped between markers instead would be a map that cannot be panned, and panning is most of
 * what a map without a basemap is for.
 *
 * Measured in pixels from the middle of the view through mesh_map_viewport_offset(), which is
 * the same arithmetic a placement does with the panel left off the end - so the marker under the
 * crosshair is by construction the marker a renderer drew there. It needs no box, which is what
 * lets the nav and a backend give one answer: the nav decides what A opens and the backend draws
 * the ring, the two cannot ask each other how wide the body is, and a selection that depended on
 * the body's width would let them disagree about which node the reader is looking at.
 *
 * In the projection rather than across the ground, and that is not a shortcut. A fix beyond the
 * display limit is *drawn* at the limit, so a marker at 88 degrees north and a view framed on it
 * are the same point on the picture and three degrees apart on Earth - and a geodesic distance
 * would refuse a marker sitting dead centre under the crosshair.
 *
 * False when nothing is near enough, which is a real and common state - a reader panning between
 * two clusters is looking at empty grid, and a selection that stuck to the last marker would
 * have A opening something off the edge of the panel.
 */
bool mesh_ui_map_selected(const struct mesh_ui_map_view *view,
                          const struct mesh_map_viewport *viewport, uint32_t *out_index);

/*
 * How far one press of a direction moves the world, in pixels - and therefore how far ahead of
 * the crosshair that press will look for a marker to land on instead.
 *
 * One number for both halves, deliberately. A press is a *step*, and what stopping on a marker
 * changes is where the step ends, never how long it is: a marker inside the step is where the
 * step stops, and a marker beyond it is somewhere the next step can go. That is what keeps a
 * direction honest - the world moves about as far per press whether or not there is anything
 * that way - and it is what lets a reader cross open ground between two nodes instead of being
 * handed the next one every time they tap.
 *
 * A reach of the whole declared panel is what this replaced, and on a mesh with any density it
 * answered every press with a marker: nothing in between was reachable, because nothing in
 * between was ever where a press stopped. Too *short* a reach costs a press and no more - the
 * step falls back to a pan, the pan brings the marker inside the reach, and the press after
 * that lands on it exactly.
 *
 * A fraction of the declared body rather than a fixed count of pixels, so a press covers the
 * same *proportion* of the view at every zoom. Roughly a fifth: five presses cross the panel,
 * which is few enough to be quick and many enough to see where you are going. The nav owns the
 * map's state and is never told how big a backend's body is, which is why this is expressed
 * against the declared box (MESH_UI_MAP_FIT_WIDTH) rather than a measured one.
 */
#define MESH_UI_MAP_PAN_STEP_X (MESH_UI_MAP_FIT_WIDTH / 5)
#define MESH_UI_MAP_PAN_STEP_Y (MESH_UI_MAP_FIT_HEIGHT / 5)

/*
 * Which way a press is going, in compass terms rather than in the d-pad's.
 *
 * North-up is a property of the map, so a direction here is a direction on the ground: Left
 * means look west, and this is the enum that says so. It also keeps the step's arithmetic free
 * of the screen's y sign, which is the one place a map's directions are easy to write upside
 * down.
 */
enum mesh_ui_map_direction {
    MESH_UI_MAP_NORTH = 0,
    MESH_UI_MAP_SOUTH,
    MESH_UI_MAP_WEST,
    MESH_UI_MAP_EAST,
};

/*
 * Where a direction should take the view: the nearest marker that way, or none.
 *
 * This is what makes panning able to aim rather than merely to move. A pan of a fixed number of
 * pixels puts the crosshair on a lattice - a fifth of the body across and a fifth down, from
 * wherever the view happened to open - and a marker is selectable only within
 * MESH_UI_MAP_SELECT_RADIUS_PX of one of those points. The disc is 28 pixels and the cell is
 * 176 by 84, so about a sixth of the panel is reachable and five markers in six could not be
 * put under the crosshair *at all* at a given zoom, however long the reader panned. Zooming
 * re-phases the lattice, which is why the symptom reads as "sometimes it works": the reader was
 * not aiming, they were resampling.
 *
 * So a direction lands *on* something when there is something that way. The view centres
 * exactly on the marker's own coordinates, which is what makes the landing exact - the
 * selection is still derived from the centre by mesh_ui_map_selected(), there is still no
 * selection on the nav, and the marker the ring goes round is by construction the one the press
 * opens. Nothing was spent to get it: the same four keys, and a direction that still means what
 * it says.
 *
 * "That way" is the 45-degree quadrant around the direction pressed - |cross| <= |along| - and
 * the four quadrants tile the plane, so nothing on the panel is in a direction no press names.
 * Among the candidates the nearest wins, so a press walks outward through them rather than
 * jumping the furthest way; a tie goes to the marker built first, which is the order
 * mesh_ui_map_selected() already settles two markers at one place with.
 *
 * "Near enough to land on" is one step, measured per axis: a candidate is inside the box that
 * one press of the pan covers, MESH_UI_MAP_PAN_STEP_X across and MESH_UI_MAP_PAN_STEP_Y down.
 * So a press moves the view by about a step whether it lands on something or not, and a marker
 * further out is walked up to rather than teleported to - the step falls back to a pan, and the
 * press after it lands exactly. Without that bound this aimed *too* well: with a reach of the
 * whole declared panel every press on a mesh of any density had a marker to answer with, so the
 * ground between two nodes was not merely hard to stop on, it was unreachable, and a reader
 * tapping a direction cycled through the roster instead of exploring the map. Bounding the
 * cross axis matters as much as the along one: a marker far off to the side is a sideways lurch
 * from a press that said "north", which is a press that did not do what it said.
 *
 * The marker under the crosshair is not a candidate - it is what the reader is already aimed at,
 * not something ahead of the press - and it is excluded by *identity*: mesh_ui_map_selected()
 * is asked, rather than "near enough" being re-derived here. That matters because being under
 * the crosshair is a disc rather than a point, so a marker can be selected while sitting a few
 * pixels ahead of centre; treated as a candidate it would win every time, and the press would
 * spend itself nudging the view onto something already selected while the line under the map
 * said nothing new. Only that one marker is skipped, so two markers a few pixels apart are
 * still how the reader chooses between them - the press steps from one to the other, which is
 * the disambiguation a cluster needs and which no amount of panning could do before.
 *
 * False when the quadrant is empty, and the caller then pans by its own step: open grid still
 * pans, which is what a map under a basemap will need and what makes "look west" honest when
 * there is nothing west.
 */
bool mesh_ui_map_step(const struct mesh_ui_map_view *view, const struct mesh_map_viewport *viewport,
                      enum mesh_ui_map_direction direction, uint32_t *out_index);

/*
 * The markers' projected positions, for framing them.
 *
 * A plain copy out of the marker list, because mesh_map_viewport_fit() takes points and must not
 * take a store - the module boundary docs/maps-roadmap.md drew, kept by giving the caller the
 * one line it needs to cross it rather than by widening the viewport's own idea of the world.
 * Returns how many were written, at most `capacity`.
 */
uint32_t mesh_ui_map_points(const struct mesh_ui_map_view *view, struct mesh_geo_point *out,
                            uint32_t capacity);

/* How many markers are on the panel as it is currently framed - the other half of what the app
   bar says, and the reason a reader panning away from everything is told so rather than left
   looking at an empty grid wondering whether the map is broken. */
uint32_t mesh_ui_map_visible(const struct mesh_ui_map_view *view,
                             const struct mesh_map_viewport *viewport);

/*
 * The marker for a node or a place, by id - never by index.
 *
 * The roster re-ranks as fixes arrive, so an index names a different node one frame later. This
 * is the same rule the node detail follows and the one docs/maps-roadmap.md's fifth pre-work
 * item pinned; a marker under a crosshair is another index into another ordering of the same
 * roster, which is exactly the case that item said a map would quietly break.
 */
bool mesh_ui_map_find(const struct mesh_ui_map_view *view, uint8_t kind, uint32_t id,
                      uint32_t *out_index);

/* Whether there is anything worth opening a map on: at least one node or place with a position.
   The Nodes tab's map row is unpressable when there is not, and says why rather than
   disappearing - the Waypoints tab's rule about a row that cannot be pressed. */
bool mesh_ui_map_has_markers(const struct mesh_ui_store *store);

#ifdef __cplusplus
}
#endif
