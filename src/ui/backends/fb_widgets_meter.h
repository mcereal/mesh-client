#ifndef MESH_UI_BACKENDS_FB_WIDGETS_METER_H
#define MESH_UI_BACKENDS_FB_WIDGETS_METER_H

/*
 * A quantity drawn as a length, and a reading drawn over time: the meter and its editable twin
 * the slider, the signal staircase, the sparkline, the proportion bar, and the chart.
 *
 * One group because they share a vocabulary - a scale, a band, and the tone that band picks -
 * and because a row or a card that carries one of them could carry any of them.
 */

/*
 * Not public API. include/mesh/ui/backends/fb.h is; fb_widgets.h is the umbrella over this file
 * and its siblings, and nothing outside src/ui/backends/ should include either.
 */

#include "fb_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/theme.h"
#include "mesh/ui/trend.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- the meter --------------------------------------------------------------------------- *
 *
 * A quantity as a length: how much of the air the mesh is using, how much of a download has
 * arrived.
 *
 * The two are the same widget on purpose, and this file predicted it before either existed - a
 * meter and a progress bar want the same table. What separates them is not the drawing but
 * *what the number means*: a meter reports a level that will go up and down on its own, a
 * progress bar reports a job that only goes forwards and then stops. Both are a track with a
 * fill in it, both take their fill from a tone, and both animate through the same keyed slot,
 * so there is one of them.
 *
 * Why a bar and not the number it sits next to. Channel utilization was a coloured percentage,
 * and a percentage has to be read and then compared against a threshold nobody carries around;
 * a length is compared against the track it is in, which is right there. The number is still
 * drawn - it is what says *how* busy - but the bar is what says *busy*, and that is the part
 * that should not need reading.
 *
 * What it is not: a spinner with a percentage bolted on. When the extent of the work is
 * unknown the widget says so with FB_METER_INDETERMINATE and moves without claiming a
 * position, rather than inventing a fraction. A bar that sat at 30% because somebody had to
 * pick a number is worse than no bar.
 */

enum fb_meter_kind {
    /* A known fraction of a known whole: `value` is where the fill ends. */
    FB_METER_DETERMINATE = 0,
    /*
     * Something is happening and how much of it is left cannot be known - a request out on the
     * network, a hash being taken. A pill travels the track instead of a fill growing, which is
     * the one shape that says "working" without also saying "this far along".
     *
     * It costs a repaint timer for as long as it is on screen, which is the reason it is a
     * separate kind rather than the default: a screen asks for motion deliberately.
     */
    FB_METER_INDETERMINATE,
};

/*
 * A meter's band is `struct mesh_ui_band` (include/mesh/ui/theme.h), and it lives there rather
 * than here because it is not this backend's idea: it is what a number means, in the same
 * vocabulary a tone is, and the node detail's row model states one without knowing a
 * framebuffer exists. mesh_ui_band_tone() is what reads it.
 *
 * The bar with no marks on it was the gap this closes. "Is 31% a lot?" is the question the
 * meter exists to answer without arithmetic, and a bare track answers it only for somebody who
 * already carries the threshold around: the fill turned amber at a quarter, but nothing on
 * screen said where a quarter *was*, so the colour reported a boundary that could not be
 * located. A notch cut into the track at each boundary is that boundary, drawn where it is.
 */

struct fb_meter {
    struct fb_rect rect; /* the track; fb_meter_thickness() is the height one wants */
    /*
     * Identity for the animation, 0 for none - the same contract the switch has.
     *
     * It matters more here than it does there. A determinate meter *eases towards* each value
     * it is given, which is what lets a reading sampled once a second look like a bar moving
     * rather than a bar jumping; without an id it draws each sample exactly and stutters.
     */
    uint32_t id;
    enum fb_meter_kind kind;
    /* DETERMINATE: the reading, in whatever units `scale` is stated in. Clamped to the ends. */
    int32_t value;
    /* The domain `value` and `band` are on. A zeroed scale means permille, which is what every
       meter that already holds a fraction wants and why it costs those callers nothing. */
    struct mesh_ui_scale scale;
    /*
     * Where the reading changes meaning, or NULL for a plain bar.
     *
     * A banded meter takes its fill's tone from where the reading falls and draws the
     * boundaries on its track, so the colour and the marks are two readings of one statement
     * rather than two statements. `tone` is then what it rests in - see fb_band_tone().
     */
    const struct mesh_ui_band *band;
    /* The fill. ACCENT, GOOD or BAD - the three mesh_ui_theme_validate() holds against
       MESH_UI_COLOR_METER_TRACK - and anything else is drawn in the accent. */
    enum mesh_ui_tone tone;
    bool selected; /* the row under it carries the cursor fill */
    /*
     * What the control is standing on when the row under it is not the cursor's: the panel, or
     * the surface of the card its list drew the group on.
     *
     * It matters because this control lays its own ground under a cursor fill - the colour pairs
     * it is contracted against are contracted against what it sits on, and on two of the four
     * themes the cursor fill *is* one of them. That patch has to be the row's ground and not the
     * panel's, or a control on a card gets a hole punched round it.
     *
     * MESH_UI_COLOR_BG is 0, so a caller drawing onto the panel leaves it zeroed and says
     * nothing. A control in a list never sets it at all: fb_list_item() writes it from the
     * model, the same way it already writes `selected`.
     */
    enum mesh_ui_color ground;
};

/* The height a meter wants at `scale`, in pixels. From the theme's metrics, so a bar keeps its
   proportion to the text beside it when a theme changes the glyph scale. */
int fb_meter_thickness(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws it, advancing the fill towards its target - or the pill along its loop. Needs the
   mutable state for the same reason the switch does. */
void fb_draw_meter(struct mesh_ui_backend_fb_state *state, const struct fb_meter *meter);

/* ---- the slider ------------------------------------------------------------------------------
 *
 * A quantity the reader is *choosing*, where the meter is a quantity they are being told.
 *
 * That is the whole of what separates the two components, and it is why this is not a flag on
 * the meter. A meter reports and eases towards each reading it is handed; a slider says where a
 * value sits among the values that could have been picked instead, marks those choices on its
 * own track, and shows which one the cursor is on. The first is a picture, the second is a
 * control, and a control has a state the picture has no word for.
 *
 * What it replaced: a NUMBER setting was Left/Right over a preset list with the chosen value in
 * the value column - "5m", and nothing at all about whether 5m was near the short end of what
 * this field offers or near the long one. The figure is still drawn, because the figure is what
 * says *how long*; the track is what says *how far along*, and that is the half a row of
 * durations could not answer without the reader already carrying the list around.
 *
 * The stops are evenly spaced and the reading between them is interpolated - see
 * mesh_ui_settings_number_track(), which is where that arithmetic lives so a test can reach it. Two
 * consequences the drawing depends on: a preset list that climbs geometrically still gives an
 * aimable track, and a value the list does not contain lands between two stops rather than being
 * refused. The segmented button had to fall back to words for an unknown value because a set of
 * alternatives has no room between its members; an axis has room, so this one does not need the
 * fallback.
 */

struct fb_slider {
    struct fb_rect rect; /* the track's box; fb_slider_height() is the height one wants */
    /* Identity for the animation, 0 for none - the meter's contract, and it matters here for
       the same reason it matters on the switch: a press should move the handle, and a screen
       opening on a value should not animate up to it from zero. */
    uint32_t id;
    /* Where the value sits, in permille of the track. The caller's, not derived here: which
       values a field offers is the settings model's business and the arithmetic that places one
       among them is unit-tested there. */
    int32_t position;
    /* How many choices to mark on the track, 0 for an unmarked one. The component decides
       whether they are drawn - see fb_draw_slider() - because a mark the eye cannot separate
       from its neighbour is worse than no mark, and the width that decides it is not known
       until the row has laid the track out. */
    uint32_t stops;
    /* The active track and the handle, from one tone. ACCENT, GOOD or BAD - the meter's three,
       validated against MESH_UI_COLOR_METER_TRACK - and anything else falls back to the
       accent. */
    enum mesh_ui_tone tone;
    /*
     * The value the row is showing is not one this track has room for, so the control draws its
     * stops and nothing else - no fill, and no handle anywhere.
     *
     * §10's rule arriving on an axis: *a control that shows a set has to be able to say "not one
     * of these"*. Two things reach it. Almost every settings scale here opens with a value that
     * is a word rather than a quantity - a "default" the firmware picks, LoRa's "max" - and
     * neither belongs at the bottom of a bar; and two lists start above zero because the thing
     * receiving the setting refuses anything below that, while an unconfigured radio still
     * reports 0. An empty track is not ambiguous with a value at the minimum, because a value at
     * the minimum has a handle sitting on it.
     */
    bool unplaced;
    /* The cursor is on this row: the handle stands up to its full height, and the track gets
       its own ground under the cursor fill. A slider is the one control on a settings row that
       the reader is about to change, so it says so rather than looking the same everywhere. */
    bool selected;
};

/* The height the whole control wants at `scale` - the handle's, which is taller than its track.
   Reserved whether or not the cursor is on the row, so a handle standing up under the cursor
   does not make the row it is on grow and shift every row below it. */
int fb_slider_height(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws the track, its stops, and the handle at `position`, easing the handle towards it. Needs
   the mutable state for the reason the meter and the switch do. */
void fb_draw_slider(struct mesh_ui_backend_fb_state *state, const struct fb_slider *slider);

/* ---- the signal staircase -------------------------------------------------------------------
 *
 * Its own component rather than a variant of the meter, because it is answering a different
 * question. A meter reports a level on a continuum and eases between samples; a staircase
 * reports a *bucket*, and easing between buckets would be inventing the intermediate values
 * that quantising them was meant to refuse. Nothing here animates, and that is the design.
 *
 * The rungs it does not light are drawn rather than left out, in the quiet ink the slot's other
 * furniture takes: an indicator that shortened as the signal fell would be a length, and a
 * length is a claim about proportion that four buckets cannot support. What the eye counts is
 * lit rungs against a constant total.
 */

/* Cells a staircase occupies, its trailing gap excluded. Stated rather than measured, for the
   reason the inline meter's width is: rungs have no natural width, and two cells is where four
   of them are still individually countable at the smallest glyph scale a theme may pick. */
#define FB_SIGNAL_CELLS 2U

/*
 * Draws `level` of MESH_UI_SIGNAL_STEPS rungs inside `box`, rising left to right.
 *
 * `ink` and `unlit` are handed in rather than looked up, so that a staircase takes its pair
 * from the row it is on - which is what the trailing slot already does for every other thing it
 * draws, and what keeps this from being a fifth colour every theme has to be validated for.
 */
void fb_draw_signal(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                    uint8_t level, struct mesh_ui_rgb ink, struct mesh_ui_rgb unlit);

/* ---- the sparkline --------------------------------------------------------------------------
 *
 * The same reading over time, in the room a row has: which *way* it is going.
 *
 * The third quantitative component and the first that is not about now. A meter says how much,
 * a staircase says how well, and both are read against a track that is right there - but the
 * two questions this client is actually opened for are "is the mesh getting worse" and "is this
 * battery going to last the night", and neither is answerable from a level. A reader who
 * happened to look an hour ago can answer them; nobody else can, and the client is the thing
 * that was looking.
 *
 * So it draws a memory, and the memory is `struct mesh_ui_series` (include/mesh/ui/layout.h) -
 * where the whole cost of this component is, and where the three rules that keep the picture
 * honest are stated. What is left here is the drawing, which is small.
 *
 * Four decisions in it, and each is the staircase's rules arriving on a second axis:
 *
 *   - **The vertical is the reading's own domain**, the same `struct mesh_ui_scale` the bar
 *     beside it fills against - never the range these particular samples happened to span. A
 *     line that rescaled itself to its data would draw a battery that fell two percent overnight
 *     as a cliff, which is what a spreadsheet does and what "a picture cannot be wrong quietly"
 *     forbids. It also means the line and the bar can be read against each other, because they
 *     are measured on one scale.
 *   - **Nothing animates.** A meter eases towards each reading because the value it is drawing
 *     replaces the last one; a trend *keeps* them, so there is nothing to move between. Easing
 *     the newest point into place would show a shape that was never a reading.
 *   - **The pen lifts at a gap.** A break in the line is a period nothing was reported, and
 *     drawing a slope across it claims readings nobody took - see `gap` on struct mesh_ui_point.
 *   - **It takes the row's pair, not a colour.** The line is a tone, and the floor under it is
 *     the meter's track - which is the pairing every theme is already validated for, and the
 *     reason a trend costs no new contract.
 *
 * Fewer than two points draws nothing at all, including no floor: one reading is a level, there
 * is a component for that, and an empty track in a list row is furniture reporting that nothing
 * has happened yet.
 */

/* Cells a trailing sparkline occupies, its gap to the words excluded. Stated rather than
   measured, for the reason the staircase's width is: a line has no natural width. Six is where
   two dozen samples are still individually placeable at the smallest glyph scale, and it is the
   inline meter's eight less the two the gap and the figure want back. */
#define FB_SPARK_CELLS 6U

struct fb_sparkline {
    struct fb_rect rect; /* the box the line is drawn in; fb_sparkline_height() is the height */
    /*
     * The samples, already normalised - x across the box, y up it, both in permille, oldest
     * first. mesh_ui_series_project() is what produces one, so the arithmetic that decides
     * where a reading lands is a unit test's to reach rather than a renderer's to hold.
     *
     * Borrowed for the call. Nothing here keeps it.
     */
    const struct mesh_ui_polyline *points;
    /* The line. A family tone, on the meter's terms: the neutral three fall back to the accent,
       because a stroke nobody validated against the track is a line that vanishes on a theme
       somebody has not opened yet. */
    enum mesh_ui_tone tone;
    bool selected; /* the row under it carries the cursor fill */
};

/*
 * The height the line wants at `scale`, in pixels: the glyph body's, which is taller than the
 * icon box the staircase beside it takes.
 *
 * Not an inconsistency between two things in one column. An icon has to stand as tall as the
 * capitals it is read among and no taller, and four rungs are counted rather than measured; a
 * line's whole reading is in its height, so every pixel of it is a pixel of the answer. This is
 * as tall as a row's own text, which is as tall as a slot can be without the row growing.
 */
int fb_sparkline_height(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws the floor and the line. Const state, unlike the meter and the slider: there is no
   animation to step - see above. */
void fb_draw_sparkline(const struct mesh_ui_backend_fb_state *state,
                       const struct fb_sparkline *spark);

/* ---- the proportion bar ----------------------------------------------------------------------
 *
 * A whole and the parts it is made of: the fourth quantitative component, and the one that says
 * what a reading is *composed of* rather than how large it is, how well it is doing or which way
 * it is going.
 *
 * The Status card is why it exists. The radio reports what it heard as three counters - packets
 * that were new, packets that were duplicates of something already relayed to us, and packets
 * that were malformed - and printed as three numbers those answer no question anybody has. The
 * question is a *ratio*: upstream's own comment on the duplicate counter says "if this number is
 * high, there are nodes in the mesh relaying packets when it's unnecessary", and high is not a
 * property of 4,812. It is a property of 4,812 out of 6,140, which is a length beside two other
 * lengths and is read without arithmetic - the meter's argument, on a whole with more than one
 * part in it.
 *
 * Four things about it are decisions rather than details:
 *
 *   - **It starts at three parts, because two parts is a meter.** A whole split in two is a
 *     fraction, a fraction is what a meter draws, and a meter can carry a band and a domain that
 *     this cannot. Heap free against heap total is a meter for that reason and not a composition,
 *     and nothing is gained by making it one.
 *   - **The parts must be disjoint, and that is the caller's promise.** It is the one way this
 *     component can be wrong quietly and it is not checkable from here: three counters that
 *     overlap still add up to something, and the bar drawn from them is a confident picture of a
 *     whole that does not exist. The radio's own transmit counters are exactly that trap -
 *     `num_tx_relay` is a subset of `num_packets_tx` rather than a sibling of it - so "tx, rx,
 *     relayed" is three numbers and not three parts, and there is no bar under that row.
 *   - **The colours are the theme's series palette, not tones.** A part means nothing except
 *     which part it is; a tone means good, bad or caution. Filling three slices from three
 *     families would report a judgement the data never made - and on the high-contrast theme,
 *     where the three non-status families are one yellow, it would report nothing at all. See
 *     MESH_UI_SERIES_COLORS.
 *   - **Nothing animates.** The meter eases because each reading it is handed replaces the last;
 *     these are counters that only ever climb, so between two frames a boundary moves by a
 *     fraction of a pixel. Easing it would spend an animation slot per boundary on motion nobody
 *     could see, and the slots are keyed per control.
 *
 * The slices are laid out by mesh_ui_proportion_split() (include/mesh/ui/layout.h), which is
 * where the two rules that keep the picture honest live: they sum to the bar exactly, and a part
 * that is there is never rounded away to nothing.
 */

struct fb_proportion {
    struct fb_rect rect; /* fb_proportion_thickness() is the height one wants */
    /* The parts, in the order they are drawn - which is left to right, and which is also the
       order the row above names them in. That correspondence is the whole legend: the bar has no
       words of its own, and a label per slice would not fit in a row's height on this panel even
       if it did. Nothing enforces it, for the same reason nothing enforces disjointness. */
    uint32_t values[MESH_UI_PROPORTION_PARTS];
    uint32_t count;
    /*
     * What is behind the bar, which the gaps between the parts are cut in.
     *
     * Stated by the caller rather than assumed, and it is the one field here with no sensible
     * default: a composition is drawn *inside* something - a card whose fill depends on its
     * variant - and a gap painted in the body's ground on a card is not a gap, it is a stripe
     * of a colour from somewhere else. The meter can get away with assuming, because a band
     * notch is a mark whose job is to divide a bar it is already inside; these gaps have to
     * *disappear*, which is a claim about what surrounds them.
     *
     * A zeroed struct is black rather than unset, so there is nothing to detect here: a caller
     * that forgets this draws black gaps, which is visible immediately.
     */
    struct mesh_ui_rgb ground;
    /* The row under it carries the cursor fill, so the bar lays a ground of its own - the
       meter's move, and needed here for the same reason: the series palette is validated against
       the body and against a card, and on two themes the cursor fill is neither. The gaps follow
       it: what is behind the bar on a selected row is the pad, not `ground`. */
    bool selected;
};

/* The height a composition wants at `scale` - the meter's, deliberately. A card that carried a
   bar of one weight and a bar of another would be reporting a difference between them that is
   not there: both are a reading drawn as a length. */
int fb_proportion_thickness(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws it. Const state, like the sparkline and unlike the meter: there is no animation to step
   - see above. Fewer than two parts, or parts that sum to nothing, draws nothing at all. */
void fb_draw_proportion(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_proportion *bar);

/* ---- the chart ------------------------------------------------------------------------------
 *
 * The same readings the sparkline draws in a row, in a whole body: what a trend *was*, with its
 * axes named and more than one line on it.
 *
 * The fifth quantitative component and the first that is a screen rather than a slot. Everything
 * before it fits in a row and pays for that by having no numbers on it - a sparkline is a shape,
 * and the reader has to already know what it is a shape of. That is the right trade in a list,
 * where the row above says which reading it is and the bar beside it says how far along. It stops
 * being the right trade the moment somebody stops to look, which is the press this exists for, and
 * it was the thing an axis frame turned out to cost: not a component, a *route*.
 *
 * What the room buys, in the order it matters:
 *
 *   - **The vertical says what it is measuring.** Its two ends are labelled, in the reading's own
 *     units, so "high" is a number rather than a feeling. The domain is still the reading's own
 *     `struct mesh_ui_scale` and never the range these samples happened to span - the sparkline's
 *     first rule, and it is *more* load-bearing here, not less: an axis with numbers on it is
 *     believed, so an axis that rescaled itself would be a labelled lie rather than a misleading
 *     shape.
 *   - **The horizontal says how long.** A shape with no time under it cannot distinguish a
 *     battery that fell ten percent in an hour from one that fell ten percent in a week.
 *   - **The thresholds are drawn.** The band the meter cuts notches into becomes a rule across
 *     the plot, so where a reading stops being comfortable is a line the trend can be seen
 *     crossing rather than a colour that changed at a moment nobody can locate. This is the one
 *     thing a chart says that no row-height component can.
 *   - **More than one line fits.** Which is what the legend is for, and why two lines could not
 *     be drawn in a row: the words naming them have to be somewhere, and a row has no somewhere.
 *
 * Three things it deliberately does not do:
 *
 *   - **No end mark.** A sparkline marks its newest reading because a stroke does not say which
 *     end is now, and a line falling left to right and one rising are the same picture read
 *     backwards. A chart has the answer written under it: the axis is labelled with the span it
 *     covers, so "now" is the right-hand edge by construction.
 *   - **No grid.** Two threshold rules and an axis are the marks that mean something; a lattice
 *     of evenly spaced lines is furniture that makes a picture look measured without measuring
 *     anything, and on a panel with no anti-aliasing it competes with the data for pixels.
 *   - **Nothing animates.** The sparkline's reason, unchanged: a trend keeps its readings rather
 *     than replacing them, so there is nothing to move between.
 */

/* Lines one chart may carry - the palette's count, because a line takes a series colour and two
   lines sharing one is a chart that cannot be read. The compile-time assertion beside
   fb_draw_proportion() holds the two halves of that seam equal. */
#define FB_CHART_LINES MESH_UI_SERIES_COLORS

struct fb_chart_line {
    /*
     * The samples, already normalised, exactly as the sparkline takes them - but projected by
     * mesh_ui_series_project_over() rather than by mesh_ui_series_project(), and that difference
     * is the whole of what makes two lines comparable. A series projected on its own span fills
     * whatever box it is given, so a series that stopped reporting half an hour ago would be
     * drawn as though it were still arriving. See mesh_ui_series_window().
     *
     * Borrowed for the call. Nothing here keeps it.
     */
    const struct mesh_ui_polyline *points;
    /*
     * Or the readings binned across the plot (mesh_ui_trend_airtime()), in the chart's `scale`
     * units, which takes the place of `points` when set. Bins divide the plot into equal slots,
     * oldest on the left.
     *
     * `columns` draws each present bin as a filled column up from the axis - the series colour is
     * a fill's colour, so this is the mark it was validated for - and otherwise a line through the
     * bins' centres, lifted wherever a bin does not join. Columns are drawn before every line, so
     * a line always reads on top of them.
     */
    const struct mesh_ui_trend_bins *bins;
    bool columns;
    /* What the legend calls it. MESH_STR_NONE draws the line and names it nowhere, which is
       honest only when there is exactly one line - with two it is the picture asking the reader
       to guess. */
    enum mesh_str_id label;
    /*
     * Where this line has got to, already formatted in the reading's own units, or NULL.
     *
     * The one number a chart could not say. Everything else on the screen is a shape and two
     * ends: the reader can see that the air got busier and that the ceiling is five percent, and
     * still not know whether it is at four or at one - which is the figure the card they came
     * from was showing them, and the figure they would go back for.
     *
     * In the legend rather than at the end of the line, because a label pinned to the last point
     * moves with the data and collides with the other line's the moment two readings converge.
     * It is also what gives the legend something to draw on a chart with one unnamed line: a
     * swatch and a reading, which is the whole of what there is to say about it.
     */
    const char *value;
};

/*
 * One reading, as the chart's other face draws it.
 *
 * `when` is how long before the newest reading in the window it was taken, and `value` is what it
 * said - both already words, because this component does not know what either is measured in. It
 * is deliberately not a timestamp: the series are stamped with a monotonic clock that counts from
 * boot and the device has no RTC, so a column of wall times would be the one thing on this screen
 * with nothing behind it. See struct mesh_ui_trend_reading.
 */
struct fb_chart_reading {
    const char *when;
    const char *value;
};

struct fb_chart {
    /* Everything: the plot, the words down its side and the two lines of chrome under it. The
       caller hands over a body and this divides it, which is the one place in this component set
       where that is the right way round - a chart is the whole screen, so there is nothing else
       laying claim to the room. */
    struct fb_rect rect;
    struct fb_chart_line lines[FB_CHART_LINES];
    uint32_t count;
    /*
     * The two ends of the vertical, already formatted in the reading's own units - a percentage,
     * a temperature, a count. Formatted by the caller because the units are the caller's: this
     * knows where the top of the domain is and has no idea what it is the top *of*.
     */
    const char *top;
    const char *bottom;
    /* What the horizontal covers, as words ("45m", "3h 20m"), or NULL when the readings share
       one clock tick and there is no span to name. NULL draws no label rather than a zero: an
       axis whose span is unknown says nothing about it, and "0m" is a claim. */
    const char *span;
    /*
     * Where the reading stops being comfortable, drawn as rules across the plot, and the domain
     * they are stated in. Optional: a NULL band draws no rules.
     *
     * The scale is the same one the lines were projected against, and the component cannot check
     * that - a band placed on one domain over lines placed on another is this component's way of
     * being wrong quietly, and it is the caller's promise in the way disjointness is
     * fb_draw_proportion()'s.
     */
    const struct mesh_ui_band *band;
    struct mesh_ui_scale scale;
    /*
     * How far back the picture goes, as the set of spans the reader may pick between, drawn as a
     * segmented button above the plot. NULL draws none and gives the room back to the plot.
     *
     * A `struct fb_segmented` rather than anything that knows what a span is, which is the rule
     * every slot in this component set follows: the strip draws four labels and lights one, and
     * what those labels *mean* is include/mesh/ui/trend.h's business. It is inside the chart
     * rather than beside it because the two are one statement - a picture and the words saying
     * how much of the record is on it - and a caller placing the strip itself would be a caller
     * computing coordinates, which is the thing a screen renderer does not do.
     *
     * It is drawn selected, always. There is no cursor on this screen to move onto it: it is the
     * only control here, Left and Right always reach it, and a control that drew unfocused while
     * being the only thing the d-pad can touch would be the frame disagreeing with the keys.
     */
    const struct fb_segmented *spans;
    /*
     * What to say in the middle of the plot when no line could be drawn in it, or MESH_STR_NONE
     * to leave it empty.
     *
     * A picture with nothing in it is the one state this component could not distinguish from a
     * bug, and the span picker is what made it reachable: narrow the span past the last two
     * readings and the frame, the axis labels and the legend are all still true and there is
     * nothing between them. So the empty state is the component's rather than a screen's - it is
     * the only thing that knows whether a line came out - and it is a string id, because it is a
     * sentence about the picture and this file does not hold sentences.
     */
    enum mesh_str_id empty;
    /*
     * The same window as a list of figures rather than as a plot, newest first - the chart's
     * other face, and what Y turns it into on a node's chart.
     *
     * A plot answers "which way is this going" and cannot answer "what exactly did it say": two
     * dozen readings across a plot 900 cells wide can be read to about a percent, which is fine
     * for a direction and useless for a number somebody is writing down. So the same window is
     * offered both ways, and it is one component rather than two screens because everything
     * around it is shared - the app bar, the span strip, the room they leave, and the fact that
     * both are *the readings in this span*. A second renderer would be a second opinion about
     * which readings those are.
     *
     * Both columns arrive worded. This knows where a row goes and has no idea what a decibel is,
     * which is the same division `top` and `bottom` are on one field up.
     *
     * NULL, or a count of zero, draws the plot. There is no mode flag: a chart handed rows draws
     * rows, which is one thing to get right instead of two that must agree.
     */
    const struct fb_chart_reading *readings;
    uint32_t reading_count;
    /*
     * The line under them, saying what the left-hand column is measured from - the caption the
     * plot spends on its span. A list of durations with nothing saying what they are durations
     * *of* is the one way this face can be read as a clock, which it is not: a Brick has no RTC.
     */
    const char *readings_note;
};

/*
 * The least room a chart is worth drawing in, in pixels of height.
 *
 * Below it the plot is shorter than the two lines of chrome under it, which is a caption with a
 * smear above it rather than a picture. A caller with less room draws something else; there is
 * no clipped chart, for the reason there is no clipped card.
 */
int fb_chart_min_height(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout);

/*
 * How many reading rows would fit, for a screen that has to cut its window before it can hand
 * the rows over.
 *
 * The measure-then-draw split the note list is on (fb_list_note_steps()), and it exists for the
 * same reason: only this file knows what the span strip and the caption leave, and only the
 * screen knows which readings there are. It is also what the nav is told as `page_rows`, so the
 * window the reader scrolls and the window drawn are one number.
 *
 * `rect` and `spans` are the chart's own, so the two calls measure the same room. Zero when
 * there is not enough of it - the same answer fb_draw_chart() gives by drawing nothing.
 */
uint32_t fb_chart_reading_rows(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_layout *layout, const struct fb_rect *rect,
                               const struct fb_segmented *spans);

/* Draws the frame, the threshold rules, the lines and the legend. Const state, like the
   sparkline and the composition: there is nothing here to animate. */
void fb_draw_chart(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_chart *chart);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_METER_H */
