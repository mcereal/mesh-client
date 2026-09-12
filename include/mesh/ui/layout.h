#ifndef MESH_UI_LAYOUT_H
#define MESH_UI_LAYOUT_H

/*
 * The layout primitives every list-and-rows UI needs, with no backend in them.
 *
 * They exist because the same mistakes kept being made by hand in the screen renderers:
 *
 *   - Laying a line out in *bytes*. A node named with one emoji is four bytes and one column,
 *     so "%-4s" pads it to nothing and a right-aligned figure computed from strlen() walks off
 *     the edge of the panel. `struct mesh_ui_line` only ever measures in drawn cells, so the
 *     mistake is no longer expressible: there is no byte-counting entry point.
 *   - Re-deriving the scroll window. Every screen wrote the same clamp-the-cursor,
 *     find-the-first-visible-row, loop-while-it-fits three-liner. `struct mesh_ui_list` is
 *     that arithmetic once.
 *   - Wrapping text twice. A chat bubble measures itself and then draws itself, and the two
 *     walks have to agree to the row or bubbles overlap. `struct mesh_ui_wrap` is the one walk
 *     both passes make, and `mesh_ui_transcript_window` is the bottom-anchored scroll window
 *     that variable-height items need.
 *
 * Neither touches a framebuffer, a snapshot or a font, so both are unit tested directly
 * (tests/suites/ui_layout.c) and both are as useful to the CLI backend as to the fb one.
 *
 * A cell is what `mesh_ui_text_cell_next` says it is - one character, or one emoji however
 * many codepoints it is spelled with. See include/mesh/ui/emoji.h.
 */

#include "mesh/i18n/strings.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Long enough for the widest thing drawn: a 233-byte message plus a peer name and a tag. */
#define MESH_UI_LINE_MAX 400U

/*
 * A line under construction.
 *
 * Every call appends, and every call is safe against a full buffer - it truncates on a cell
 * boundary rather than splitting a UTF-8 sequence, so a line that overflows still draws (and
 * still logs, and still serialises) as valid text. Build with mesh_ui_line_reset(), read with
 * mesh_ui_line_text().
 */
struct mesh_ui_line {
    char text[MESH_UI_LINE_MAX];
    size_t len; /* bytes used, excluding the terminator */
};

void mesh_ui_line_reset(struct mesh_ui_line *line);

/* Append formatted text. */
void mesh_ui_line_printf(struct mesh_ui_line *line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void mesh_ui_line_vprintf(struct mesh_ui_line *line, const char *fmt, va_list args);

/*
 * Append a catalog entry, formatted.
 *
 * The counterpart to mesh_ui_line_printf() for text the user reads: `id` names the string and
 * mesh_str_format() supplies it, so a renderer never holds an English sentence. Plain entries
 * with no %-specifier are better appended as mesh_ui_line_printf(line, "%s", mesh_str(id)) -
 * that keeps the compiler's format check where there is something to check.
 */
void mesh_ui_line_str(struct mesh_ui_line *line, enum mesh_str_id id, ...);

/*
 * Append `text` occupying exactly `cols` cells: clipped if it is wider, space-padded if it is
 * narrower. This is "%-*.*s" done in cells, and it is what makes a label column line up when
 * the labels are not all ASCII.
 */
void mesh_ui_line_column(struct mesh_ui_line *line, const char *text, size_t cols);

/* Pad with spaces until the line occupies `cols` cells. A no-op once it is that wide. */
void mesh_ui_line_pad_to(struct mesh_ui_line *line, size_t cols);

/*
 * Right-align `right` at column `cols`, clipping what is already in the line to make room.
 *
 * This is the shape a list row with a metric on the end has - "Andy      -7.5dB 3m" - and
 * doing it by hand is where the byte/cell confusion did the most damage. `right` may be empty,
 * in which case the line is simply clipped to `cols`.
 */
void mesh_ui_line_right(struct mesh_ui_line *line, size_t cols, const char *right);

/* Clip to `cols` cells. */
void mesh_ui_line_fit(struct mesh_ui_line *line, size_t cols);

/* Cells the line occupies once drawn. */
size_t mesh_ui_line_width(const struct mesh_ui_line *line);

const char *mesh_ui_line_text(const struct mesh_ui_line *line);

/*
 * Word wrapping, measured in cells and cut on cell boundaries.
 *
 * The point of the iterator shape is that measuring and drawing are the *same walk*: a chat
 * bubble has to know how many rows it will take before it is placed, and a bubble that
 * measured five rows and drew six would overwrite the message under it. Both passes call
 * mesh_ui_wrap_next(), so they cannot disagree.
 *
 * Breaks at the last space inside the window when there is one, at a hard newline always, and
 * mid-cell never. Leading spaces never open a line and trailing spaces never close one.
 */
struct mesh_ui_wrap {
    const char *rest;            /* what has not been emitted yet */
    size_t cols;                 /* window width in cells; 0 is read as 1 */
    char line[MESH_UI_LINE_MAX]; /* the line the last _next() produced */
};

void mesh_ui_wrap_begin(struct mesh_ui_wrap *wrap, const char *text, size_t cols);

/* Fills `wrap->line` with the next line; false once the text is spent. */
bool mesh_ui_wrap_next(struct mesh_ui_wrap *wrap);

/* Rows `text` needs at this width. Empty text needs none. */
uint32_t mesh_ui_wrap_lines(const char *text, size_t cols);

/* Cells the widest of those rows occupies - what a bubble sizes itself to, so a two-word
   message does not draw a full-width box. */
size_t mesh_ui_wrap_widest(const char *text, size_t cols);

/*
 * A bottom-anchored window onto items of differing heights: a transcript.
 *
 * struct mesh_ui_list cannot do this - it assumes every item is one row (or a fixed number of
 * them), and a wrapped message is however many rows its text needs. The two rules that make it
 * read like a messenger rather than like a list are here rather than in a backend:
 *
 *   - the newest item sits on the *bottom* row, with the slack above it, so a thread with two
 *     messages in it opens where a thread with forty does;
 *   - scrolling up puts the cursor at the top of the window and fills downward, so the item
 *     being read is never the one half off the edge.
 *
 * `heights` is one row count per item, oldest first. Heights of 0 are legal (nothing draws).
 */
struct mesh_ui_transcript {
    uint32_t first; /* first item drawn */
    uint32_t count; /* items drawn, starting at `first` */
    uint32_t pad;   /* blank rows above `first`, so the newest lands on the last row */
};

struct mesh_ui_transcript mesh_ui_transcript_window(const uint8_t *heights, uint32_t count,
                                                    uint32_t cursor, uint32_t rows);

/*
 * A window onto `count` items with the cursor kept on screen.
 *
 * Stateless between frames on purpose: the window is derived from the cursor every time rather
 * than remembered, so there is no scroll position to get out of step with a list that changed
 * underneath it. Iterate with mesh_ui_list_next().
 *
 * ---- steps ----
 *
 * A list measures itself in *steps* rather than in items, and an item is however many steps
 * tall it says it is. A step is one body row; nothing here knows that, which is the point -
 * `capacity` is steps the window holds and a backend multiplies by whatever a row costs it.
 *
 * Every item being one step is the common case and is what mesh_ui_list_begin() means, so
 * `first`, `visible` and the scroll thumb all still read as item arithmetic there. What the
 * step count buys is the case where they are not: a row carrying a chart wants two where its
 * neighbours want one, and a window that counted items would put the cursor, the highlight and
 * the scroll thumb in three different places the moment one row was taller than the rest.
 *
 * The heights are handed in as an array rather than measured through a callback because the
 * caller has already walked its items to build them - see mesh_ui_transcript_window(), which
 * takes the same shape for the same reason - and a second walk through a function pointer buys
 * generality no screen on this panel needs. Nothing here has to know the height of row four
 * hundred.
 */
struct mesh_ui_list {
    uint32_t count;   /* items in the list */
    uint32_t cursor;  /* clamped into [0, count) - count == 0 leaves it 0 */
    uint32_t first;   /* index of the first item on screen */
    uint32_t visible; /* items that fit */
    uint32_t next;    /* iterator position */

    /* One height per item, in steps, or NULL when every item is `step` steps tall. Borrowed:
       the caller owns the array and it has to outlive the list, which on every caller here
       means it is a local in the same function. A height of 0 is read as 1 - an item that
       occupied nothing could never be scrolled to. */
    const uint8_t *heights;
    uint8_t step;        /* the uniform height, when `heights` is NULL. 0 is read as 1 */
    uint32_t capacity;   /* steps the window holds */
    uint32_t total;      /* steps the whole list occupies */
    uint32_t first_step; /* steps above `first` */
    uint32_t used;       /* steps the visible items occupy */
    /* Steps above the window that ends on the last item: how far `first_step` can travel, and
       so where a scroll thumb reaches the end of its rail. Derived with the window because that
       is the walk already bounded by the window rather than by the list. */
    uint32_t last_first_step;
};

/*
 * `cursor` is taken raw from the nav state and clamped here, which is the check every screen
 * used to write out. `visible` of 0 yields a list that draws nothing rather than one that
 * divides by zero.
 */
struct mesh_ui_list mesh_ui_list_begin(uint32_t count, uint32_t cursor, uint32_t visible);

/*
 * The same window, over items `step` steps tall each. `capacity` is in steps, so a body of
 * fifteen rows holding two-row items is (15, 2) rather than a division the caller does.
 *
 * Doing the division here rather than at the call site is what keeps the scroll thumb honest:
 * a caller that divided first would hand over a window whose remainder - most of a row, at the
 * scales this ships with - had already been rounded away.
 */
struct mesh_ui_list mesh_ui_list_begin_step(uint32_t count, uint32_t cursor, uint32_t capacity,
                                            uint8_t step);

/*
 * The same window, over items of differing heights. `heights` is one step count per item and
 * is borrowed for the life of the list; NULL is every item one step.
 *
 * The window still puts the cursor on the last line that fits and fills upward from it, which
 * is what mesh_ui_list_begin() does and is why scrolling down a list of mixed heights does not
 * feel like a different list.
 */
struct mesh_ui_list mesh_ui_list_begin_heights(uint32_t count, uint32_t cursor, uint32_t capacity,
                                               const uint8_t *heights);

/* Steps item `index` occupies. Out of range is 0, which is what an iterator past the end of a
   list wants and what a caller advancing a y cursor should add. */
uint8_t mesh_ui_list_item_height(const struct mesh_ui_list *list, uint32_t index);

/* Hands back each visible index in turn, false when the window is exhausted. */
bool mesh_ui_list_next(struct mesh_ui_list *list, uint32_t *index);

bool mesh_ui_list_is_cursor(const struct mesh_ui_list *list, uint32_t index);

/* Where the window starts so that `cursor` is inside it, for a list of one-step items. Answered
   by mesh_ui_list_begin() rather than derived beside it - one answer for where a list starts. */
uint32_t mesh_ui_list_first_visible(uint32_t cursor, uint32_t count, uint32_t visible);

/*
 * Where a scroll indicator's thumb sits, and how long it is.
 *
 * Here rather than in the framebuffer backend for the reason the rest of this header is: it is
 * proportion arithmetic over `count`, `first` and `visible`, it touches no pixels, and a second
 * backend that grew a scroll indicator would want the same answer rather than a second
 * derivation of it. `track` is however long the indicator is - pixels in the fb backend, and it
 * could as well be rows - and `minimum` is the shortest a thumb may be drawn.
 *
 * `length` of 0 means *draw nothing*: the list fits, so there is nothing off screen to report
 * and an indicator would be furniture.
 *
 * The offset is measured against the *travel* - the track less the thumb - rather than against
 * the track, which is the difference between a thumb that reaches the end exactly when the last
 * item is on screen and one that stops short and reports that there is more below.
 */
struct mesh_ui_scroll {
    int offset; /* from the start of the track */
    int length;
};

struct mesh_ui_scroll mesh_ui_list_scroll(const struct mesh_ui_list *list, int track, int minimum);

/* ---- readings ------------------------------------------------------------------------------
 *
 * Turning a number into a length, which is the one piece of arithmetic every quantitative
 * widget needs and none of them should own.
 *
 * It is here for the reason mesh_ui_list_scroll() is: it is proportion arithmetic with no
 * pixels in it, it is unit tested directly (tests/suites/ui_layout.c), and a second backend
 * that grew a bar would want the same answer rather than a second derivation of it.
 */

/*
 * The domain a reading is measured on.
 *
 * A bar is a fraction of its track, so every reading that is not already a fraction has to be
 * turned into one - and doing that at each call site is how a picture and the words beside it
 * come to disagree. The airtime figure is a percentage, a battery is 0..100, and a LoRa
 * signal-to-noise ratio runs from about -20 dB to +10 and is the case that makes the point: a
 * bar that can only fill from zero cannot express it at all, and a caller normalising by hand
 * is free to pick ends that the thresholds it colours the number by know nothing about.
 *
 * So a reading travels as itself, with the ends it is measured between, and the widget asks
 * once.
 *
 * A scale with min == max - which a zeroed struct is - means the reading is *already* permille.
 * That is the identity domain rather than a special case smuggled in, and it is what lets a
 * caller that genuinely holds a fraction say nothing at all.
 */
struct mesh_ui_scale {
    int32_t min;
    int32_t max;
};

/*
 * Where `value` sits on `scale`, in permille - the unit a meter's fill and the animation table
 * are both already on (MESH_UI_ANIM_ONE). Clamped to the ends, because a reading off the air
 * carries no promise of being inside them.
 *
 * A descending scale (min > max) is legal and reads backwards, which is what a figure that is
 * better when it is smaller wants.
 */
int32_t mesh_ui_scale_permille(struct mesh_ui_scale scale, int32_t value);

/*
 * A percentage the radio reports as a float, as the permille the rest of this speaks.
 *
 * The mesh's own figures - channel utilization, transmit airtime - arrive as floats with a
 * tenth of a percent of real precision, and nothing upstream promises they are in range. This
 * is where that becomes an integer exactly once, so the two screens that draw those readings
 * cannot round them differently and then disagree by a pixel that looks like a bug.
 *
 * The guard is written as `!(percent > 0)` rather than `percent <= 0` so that a NaN - which
 * compares false against everything - is caught by it. Written the other way round a reading
 * the radio never made would sail through and be cast to whatever the platform felt like.
 */
int32_t mesh_ui_percent_permille(float percent);

/*
 * A temperature the radio reports as a float, as the tenths of a degree the rest of this speaks.
 *
 * The same job mesh_ui_percent_permille() does one line up and the same reason for existing -
 * one place where a reading off the air becomes an integer, so two screens drawing it cannot
 * round it differently - but it cannot share that function's guard. A percentage's domain starts
 * at zero, so "not above zero" stands in for "no reading"; a temperature's runs through zero, and
 * a client that read -8 degrees as a sensor saying nothing would erase every winter night on the
 * mesh. See the bounds test in src/ui/layout.c for how a NaN is caught instead.
 */
int32_t mesh_ui_temperature_decidegrees(float celsius);

/*
 * Where a LoRa link's signal-to-noise ratio changes meaning, in dB.
 *
 * Whole numbers rather than floats so that a threshold band can be stated in the same terms,
 * and named here rather than in a renderer because two things now read them: the staircase on
 * a node row and the bar on its detail screen.
 *
 * The values are the ones the firmware's own decode margin implies. LongFast demodulates down
 * to about -17.5 dB, so -15 is the last rung that is still a working link and below it is a
 * node that is about to stop arriving; 0 dB is where the signal has climbed above the noise
 * rather than merely out of it.
 */
#define MESH_UI_SNR_EXCELLENT 5
#define MESH_UI_SNR_GOOD 0
#define MESH_UI_SNR_FAIR (-7)
#define MESH_UI_SNR_POOR (-15)

/* The two ends a signal-to-noise ratio is drawn between: the demodulator's floor at the widest
   spreading factor, and a link so strong that more of it would not mean anything. */
#define MESH_UI_SNR_FLOOR (-20)
#define MESH_UI_SNR_CEILING 10

/* Rungs a signal indicator has. Four is what a handset shows, and it is about the most a
   staircase two cells wide can still be counted at a glance. */
#define MESH_UI_SIGNAL_STEPS 4U

/*
 * How many of the MESH_UI_SIGNAL_STEPS rungs `snr` lights: 0 at the floor, 4 for a link
 * that could not be better.
 *
 * Quantised rather than continuous, and deliberately: an SNR is measured off *one* packet, so
 * its error bars are wide enough that a smooth bar would be claiming a precision the number
 * does not have. Four rungs is a claim it can support.
 *
 * Signal-to-noise rather than RSSI, which is the reading a cellular indicator would use. LoRa
 * decodes *below* the noise floor, so received strength on its own says nothing about whether
 * a packet arrives - a loud band with a loud noise floor is a good RSSI and a dead link. SNR
 * is the figure that decides it, so it is the figure the rungs count.
 *
 * 0 is a real rung and means "heard, at the floor", not "unknown". Whether there is a reading
 * at all is the caller's question: a node reached over several hops or over MQTT has an SNR
 * for the last leg rather than for itself, and that is not this function's to guess at.
 */
uint8_t mesh_ui_signal_level(float snr);

/*
 * Where the mesh's two airtime figures stop being healthy, in permille of the air.
 *
 * Not look-and-feel numbers. Above roughly a quarter of channel utilization, LoRa's
 * listen-before-talk backs everything off and multi-hop delivery starts failing outright, and
 * by half the mesh is effectively a single-hop one. Transmit airtime is a different limit and a
 * much lower one: the ISM duty-cycle rules a radio has to keep to are around a tenth, so five
 * percent is a radio approaching its own ceiling rather than a busy band.
 *
 * Here, with the signal-to-noise rungs, because two screens read each of them - the Status
 * card's heading, figure, bar and the marks on that bar all take the first pair, and a node's
 * own detail screen takes both - and a threshold stated twice is a screen that can mark one
 * boundary while colouring another.
 */
#define MESH_UI_AIRTIME_BUSY_WARN 250
#define MESH_UI_AIRTIME_BUSY_BAD 500
#define MESH_UI_AIRTIME_TX_WARN 50
#define MESH_UI_AIRTIME_TX_BAD 100

/*
 * Where a battery stops being comfortable, in percent - a descending band, because this is the
 * one reading on the mesh that is worse when it is smaller.
 *
 * Percent rather than permille: it arrives from the radio as a whole percent, so a finer unit
 * would be a precision the wire does not carry.
 */
#define MESH_UI_BATTERY_LOW 30
#define MESH_UI_BATTERY_CRITICAL 15

/*
 * Where a node's own enclosure stops being comfortable, in tenths of a degree Celsius.
 *
 * This is a *node health* threshold rather than a weather one, which is the whole of why it can
 * be stated at all: nothing here knows whether 35 degrees of air is pleasant, and a client that
 * coloured the weather would be inventing an opinion. What it does know is that the radio
 * reporting the reading is sitting in it - LoRa modules derate above about 60 and the lithium
 * cells behind them stop taking a charge around there - so the band is asking "is this box
 * cooking?", which is exactly the question a solar repeater in a field cannot be asked any
 * other way.
 *
 * Tenths rather than whole degrees because that is the precision the sensors on the wire
 * actually carry, and the same reason mesh_ui_percent_permille() exists one reading over: a
 * whole degree would put a staircase under a reading that was holding still.
 */
#define MESH_UI_TEMPERATURE_WARM 500
#define MESH_UI_TEMPERATURE_HOT 600

/*
 * The two ends a temperature is drawn between, in the same tenths.
 *
 * Wide enough that a real reading is never against the stop - the coldest inhabited places sit
 * near -40 and a dark enclosure in summer sun clears 70 - and narrow enough that an ordinary
 * day is not a flat line through the middle of an empty plot. They are the ends of what a node
 * on this mesh could plausibly report, not the ends of what a thermometer can measure.
 */
#define MESH_UI_TEMPERATURE_FLOOR (-400)
#define MESH_UI_TEMPERATURE_CEILING 800

/*
 * Where humidity starts being a problem for the node reporting it, in permille.
 *
 * The same rule as the temperature above, and the same reason it is sayable: this is not a
 * comment on the weather. Condensation forms on a board whose enclosure is at the dew point,
 * and the margin before that shrinks fast above eighty percent - so the band says "this node is
 * getting wet", which is the failure a sealed box in a field actually has.
 *
 * Permille because mesh_ui_percent_permille() is where every percentage off the air becomes an
 * integer, and a second unit for this one would be a second rounding.
 */
#define MESH_UI_HUMIDITY_DAMP 800
#define MESH_UI_HUMIDITY_WET 900

/* ---- a composition ---------------------------------------------------------------------------
 *
 * A whole, and the parts it is made of - the third question a reading can be asked, after how
 * much (a meter) and which way it is going (a series).
 *
 * The arithmetic is here rather than in the widget that draws it for the reason every other
 * reading's is: there are no pixels in it, a test can reach it, and a second backend that grew
 * a composition would otherwise derive it a second time and round it differently.
 *
 * Two things about it are not obvious, and both are the same rule the sparkline's y axis is:
 * *a picture cannot be wrong quietly.*
 *
 *   - **The parts sum to the extent exactly.** Rounding each part on its own leaves a gap at
 *     the end of the bar on most inputs, and a gap in a bar that says "this is all of it" is
 *     the bar reporting a part nobody named. The leftover units go to the parts that lost most
 *     to rounding - the largest-remainder method every seat-allocation uses, and for the same
 *     reason: it is the split that is off by the least everywhere at once.
 *   - **A part that is there is never rounded away to nothing.** Three bad packets in fifty
 *     thousand is a quarter of a pixel, and drawn honestly it is a bar that says nothing is
 *     wrong. So a non-zero part takes a unit off the longest one instead. It costs the picture
 *     a pixel of accuracy at the one end where accuracy is worth least, and it buys the
 *     difference between "none" and "some", which is the difference the reader came for. A part
 *     that really is zero still gets nothing: it is not there, and inventing a sliver for it
 *     would be the same lie the other way round.
 */

/* Parts one composition may have. It is MESH_UI_SERIES_COLORS (include/mesh/ui/theme.h) counted
   from the other side of the seam this file keeps with the theme - what a part *is* is layout's
   half, and what colour it takes is the theme's - so the two are stated separately and held
   equal where they meet, by the _Static_assert in fb_widgets.c. Four is the number of
   categorical fills the panel's lightness range can hold apart; see that file. */
#define MESH_UI_PROPORTION_PARTS 4U

/*
 * Splits `extent` among `count` parts in proportion to `values`, writing `count` lengths to
 * `out`.
 *
 * `extent` is in whatever unit the caller draws in - pixels for a bar on a panel, permille for
 * a caller that has not laid anything out yet. The unit is the caller's because the rounding
 * has to happen in the unit that is finally drawn: permille rounded to pixels afterwards is two
 * roundings, and the gap the first one closed is reopened by the second.
 *
 * Answers with the number of lengths written, which is `count` - or 0 when there is nothing to
 * draw at all: no parts, no extent, more parts than MESH_UI_PROPORTION_PARTS, or a whole that
 * sums to zero. 0 is not an empty bar; it is the caller's cue to draw no bar, the way a series
 * of fewer than two points draws no line. A track with nothing in it says the mesh is quiet,
 * which is a different claim from having heard nothing yet.
 */
uint32_t mesh_ui_proportion_split(const uint32_t *values, uint32_t count, int32_t extent,
                                  int32_t *out);

/* ---- a series ------------------------------------------------------------------------------
 *
 * The same reading, kept over time, so that something can say which *way* it is going.
 *
 * A meter and a staircase both report a level: how busy the air is now, how well we hear a node
 * now. Neither can answer "is it climbing", and that is the question behind both of the ones a
 * client like this is actually opened for - is the mesh getting worse, is this battery going to
 * last the night. A level answers them only for a reader who happened to look an hour ago and
 * remembers what it said.
 *
 * So the client remembers instead, and a series is that memory: a bounded ring of stamped
 * readings in the reading's own units. It is here with the rest of the readings because what a
 * widget needs from it is arithmetic with no pixels in it - where each sample sits across a box
 * and up it - and because the fb backend must not be the only thing that can ask.
 *
 * Three rules the shape encodes, each of them a way a trend line can be wrong quietly:
 *
 *   - **The x axis is time, not the sample number.** Telemetry arrives on the radio's schedule
 *     and a reconnect resumes it whenever it resumes; spacing samples evenly would draw four
 *     readings taken over ten minutes and four taken over four hours as the same picture.
 *   - **A gap is a break, not a slope.** A line drawn straight across the hour the radio was
 *     away claims readings nobody took, and it is exactly the hour a reader would most want to
 *     see was missing. `gap_ms` is how long a silence has to be before the pen lifts.
 *   - **The y axis is the reading's own domain**, the same `struct mesh_ui_scale` the bar beside
 *     it uses - never the range the samples happen to span. Auto-scaling is what a spreadsheet
 *     does, and on a battery that fell two percent overnight it draws a cliff. A chart may
 *     contract that domain's *ceiling* down a fixed ladder and says on the axis which rung it
 *     landed on (mesh_ui_trend_domain()); the floor stays where the reading's domain put it, and
 *     a sparkline never contracts anything, because it shares its domain with the bar beside it
 *     and has nowhere to write down that it has moved.
 *
 * The clock is the *client's* monotonic one rather than the radio's stamp, because the client
 * is the thing that has been watching: a Brick with no wall clock still knows how long ago it
 * was told something, and a wall clock arriving mid-session would otherwise jump every reading
 * taken before it into the far past. mesh_ui_series_push() drops the whole series if the clock
 * goes backwards, which is what a wrap or a step change leaves behind - the samples are still
 * true and *when* they were taken is no longer known, and that is not a series.
 */

/* Readings held per series. Two dozen is what a line the width of a list row can still be read
   as a shape rather than as a scribble, and at the intervals telemetry actually arrives on -
   minutes for the radio's own report, half an hour for a node's - it is a session's worth. */
#define MESH_UI_SERIES_MAX 24U

struct mesh_ui_sample {
    uint32_t time; /* the client's monotonic clock, in milliseconds */
    int32_t value; /* the reading, in whatever units the series is stated in */
    /* This reading does not continue the one before it, for a reason the clock cannot show -
       see mesh_ui_series_break(). The elapsed-time test is the other way a point becomes the
       start of a segment, and the two are independent. */
    bool gap;
};

struct mesh_ui_series {
    struct mesh_ui_sample items[MESH_UI_SERIES_MAX];
    uint32_t first; /* ring head: where the oldest sample sits */
    uint32_t count;
    /* Longer than this between two readings and the line breaks rather than sloping across it.
       Stated by whoever fills the series, because how long a silence is remarkable is a fact
       about the source - the radio's own report is minutes apart and a node's is half an hour -
       and travels with the data so nothing downstream has to be told twice. 0 never breaks. */
    uint32_t gap_ms;
    /* mesh_ui_series_break() has been called and no reading has arrived since. */
    bool pending_break;
};

/* Empties it and states how long a silence counts as a break. */
void mesh_ui_series_reset(struct mesh_ui_series *series, uint32_t gap_ms);

/*
 * Appends a reading, evicting the oldest once the ring is full.
 *
 * `time` going backwards empties the series first: see above. A repeated `time` is kept - two
 * readings the clock could not separate are still two readings, and mesh_ui_series_project()
 * says what it does with them.
 */
void mesh_ui_series_push(struct mesh_ui_series *series, uint32_t time, int32_t value);

/*
 * Whatever arrives next does not continue what came before.
 *
 * A silence is not the only discontinuity a reading can have, and the elapsed-time test cannot
 * see the other kind: a reading that was *refused* rather than missing. A node that spends an
 * hour on external power is reporting throughout - punctually, inside any gap window - and
 * reporting something that is not a level, so the two samples either side of it have an hour of
 * unknown battery between them. Sloping across that is the same false claim as sloping across a
 * silence, and it arrives past the same guard.
 *
 * So the source says so, because the source is the only thing that knows it happened. Calling
 * it on an empty series does nothing a first sample was not already going to do.
 */
void mesh_ui_series_break(struct mesh_ui_series *series);

/* The `index`th oldest, or NULL past the end. */
const struct mesh_ui_sample *mesh_ui_series_at(const struct mesh_ui_series *series, uint32_t index);

/* The newest, or NULL for an empty series - the reading a caller would draw a figure from. */
const struct mesh_ui_sample *mesh_ui_series_newest(const struct mesh_ui_series *series);

/*
 * One projected sample: where it sits across the box and up it, both in permille.
 *
 * `gap` is the pen: true means "this sample does not continue the one before it", which is the
 * first sample of the line, every sample that follows a silence longer than the series'
 * `gap_ms`, and every sample the source itself broke before (mesh_ui_series_break()).
 */
struct mesh_ui_point {
    int16_t x;
    int16_t y;
    bool gap;
};

struct mesh_ui_polyline {
    struct mesh_ui_point items[MESH_UI_SERIES_MAX];
    uint32_t count;
};

/*
 * The series as a normalised polyline, oldest first: x across the series' own time span, y on
 * `scale` exactly as a meter's fill would place it.
 *
 * Fewer than two points is not a trend, and a widget handed one draws nothing - a single
 * reading is a level, and there is already a component for that.
 *
 * A span of zero - every sample stamped alike, which only a clock too coarse to separate two
 * pushes produces - falls back to even spacing, because their order is then all that is known
 * about them. That is the one case where the sample number is the axis, and it is stated here
 * rather than discovered in a renderer.
 */
void mesh_ui_series_project(const struct mesh_ui_series *series, struct mesh_ui_scale scale,
                            struct mesh_ui_polyline *out);

/*
 * Whether any two adjacent readings are one line.
 *
 * Not the same question as "are there two readings", and the difference is a picture with
 * nothing in it. Every sample that follows a silence longer than the series' `gap_ms`, or that
 * the source itself broke before, starts a segment rather than continuing one - so two readings
 * either side of a link that was down for a quarter of an hour are two samples the ring holds
 * and no line at all. A count says yes; a chart drawn from them has axes, a legend and nothing
 * between them.
 *
 * So anything that offers a picture asks this rather than counting: what a trend promises is a
 * shape, and the honest answer when there is none is to offer nothing. It is deliberately the
 * same break test mesh_ui_series_project_over() applies, because the alternative is a predicate
 * that can say a line exists and a projection that declines to draw one.
 */
bool mesh_ui_series_has_segment(const struct mesh_ui_series *series);

/*
 * The clock window a set of series covers: the oldest stamp on any of them, and the newest.
 *
 * One window for several series rather than one each, because that is the whole of what makes
 * two lines on one picture comparable. A series projected on its own span is stretched to fill
 * whatever box it is handed, so two of them - one an hour long, one that stopped reporting
 * twenty minutes ago - come out the same width, and the second is drawn as though it were still
 * arriving. Read together they say the two readings tracked each other, which is a claim neither
 * series made. Only the sparkline can get away without this, and only because it draws one line.
 *
 * False when there is nothing to frame: no series, all of them empty, or every stamp alike. A
 * window needs two ends, and the caller that cannot have one draws nothing rather than a frame
 * around an axis of zero width.
 */
bool mesh_ui_series_window(const struct mesh_ui_series *const *series, uint32_t count,
                           uint32_t *out_from, uint32_t *out_to);

/*
 * The same projection, over a window stated by the caller rather than over the series' own.
 *
 * mesh_ui_series_project() is this with the series' own two ends, which is the right window for
 * a line drawn by itself. What a chart wants instead is one window across everything on it - see
 * mesh_ui_series_window() - so that a series which stopped reporting ends where it stopped
 * rather than at the right-hand edge.
 *
 * A sample outside the window is held at the edge it fell off: a window is a frame, and a
 * reading that lands outside one is a reading the picture does not have room to be honest about.
 * `to` at or before `from` falls back to even spacing, which is mesh_ui_series_project()'s own
 * answer for a span of zero and is here for the same reason - the order of the samples is then
 * all that is known about them.
 */
void mesh_ui_series_project_over(const struct mesh_ui_series *series, struct mesh_ui_scale scale,
                                 uint32_t from, uint32_t to, struct mesh_ui_polyline *out);

/*
 * The same window, with a sample outside it dropped rather than held at the edge.
 *
 * The two differ in one case and it is the case a *reader* creates. A window taken from the
 * series themselves (mesh_ui_series_window()) contains every sample by construction, so nothing
 * ever falls outside one and clamping is a rule that never fires. A window the reader narrowed -
 * "show me the last quarter of an hour" - contains only part of the ring, and clamping there
 * stacks every older reading on the left-hand edge as a column of points at one x: a vertical
 * stroke up the side of the plot, drawn in the data's own colour, that is not a reading of
 * anything.
 *
 * So a chart with a span picker asks for this one. The line then starts at the first reading
 * inside the window - which is a pen lifted at the frame's edge rather than a claim about what
 * happened before it - and a series with nothing inside the window comes back empty, which the
 * components already treat as "no line".
 *
 * Neither of these interpolates a crossing at the edge, and that is deliberate: a point on the
 * frame's boundary is a reading nobody took, and this client draws the readings it was given.
 */
void mesh_ui_series_project_within(const struct mesh_ui_series *series, struct mesh_ui_scale scale,
                                   uint32_t from, uint32_t to, struct mesh_ui_polyline *out);

#endif /* MESH_UI_LAYOUT_H */
