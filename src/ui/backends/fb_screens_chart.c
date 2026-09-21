#define _POSIX_C_SOURCE 200809L

/*
 * The two chart screens: the radio's airtime over the Status cards, and one of a node's readings
 * over its detail.
 *
 * One picture opened from two places, and one renderer. See the note over enum fb_chart_axis
 * below for what the two screens actually differ in and why everything else about a chart is
 * answered in include/mesh/ui/trend.h rather than here.
 */

#include "inkcell/ui/layout.h"
#include "inkcell/ui/widgets.h"
#include "inkwell/base/text.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/duration.h"
#include "mesh/ui/history.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/trend.h"

#include <string.h>

/* ---- the two chart screens ------------------------------------------------------------------
 *
 * One picture, opened from two places: the radio's airtime over the Status cards, and one of a
 * node's readings over its detail. They were two renderers with the same forty lines in them -
 * take a window, project against a domain, word the two ends of the vertical, word the span,
 * divide the body, fill a struct inkcell_fb_chart - and the forty lines were where the two screens
 * could quietly stop agreeing about what a chart is.
 *
 * So the frame is one function and what differs is a description handed to it. What differs is
 * genuinely small: which series, what the legend calls them, what domain they are measured on,
 * which thresholds are ruled across them, and what unit the axis is worded in. Everything else -
 * the span picker, the window it cuts, the ceiling it picks, the projection, the caption, the
 * empty state - is the same question with the same answer, and it is answered in
 * include/mesh/ui/trend.h and here.
 */

/*
 * How the two ends of the vertical are put into words.
 *
 * The one thing about a chart that cannot be derived from its domain once the ceiling has been
 * contracted: inkcell_trend_domain() turns the identity domain into real permille ends, so
 * "already a fraction" stops being visible in the numbers. The caller states it, because the
 * caller is what chose the unit its readings travel in.
 */
enum fb_chart_axis {
    FB_CHART_AXIS_PERMILLE, /* a share of something, kept in permille, read as whole percent */
    FB_CHART_AXIS_PERCENT,  /* already whole percent, which is all the wire carries for a battery */
    FB_CHART_AXIS_CELSIUS,  /* tenths of a degree, read as whole ones */
    /*
     * The two signal readings, already in whole units and signed throughout.
     *
     * Their own kinds rather than the percent one, because that one ends `(unsigned)(value > 0 ?
     * value : 0)` - a clamp that is right for a share of something and would draw every decibel
     * a LoRa link has ever been measured at as a zero.
     */
    FB_CHART_AXIS_DECIBEL, /* dB: a signal-to-noise ratio */
    FB_CHART_AXIS_DBM,     /* dBm: how loud the packet was */
};

/*
 * Where a line has got to, in the reading's own units, for the legend under the plot.
 *
 * The axis ends are whole numbers because they are read rather than measured; this one is the
 * *reading*, so it carries the precision the row the reader came from was showing - a mesh at
 * 1.1% busy and one at 1.9% are the same whole percent and are not the same mesh.
 */
static void fb_chart_reading(uint8_t axis, int32_t value, char *out, size_t len) {
    switch ((enum fb_chart_axis)axis) {
    case FB_CHART_AXIS_CELSIUS:
        inkcell_str_format(out, len, MESH_STR_TREND_VALUE_CELSIUS, (double)value / 10.0);
        return;
    /* The same words as the axis ends, because these two are kept in the units they are read in:
       there is no tenth to spend on the legend that the axis was not already showing. */
    case FB_CHART_AXIS_DECIBEL:
        inkcell_str_format(out, len, MESH_STR_NODE_TREND_AXIS_DB, value);
        return;
    case FB_CHART_AXIS_DBM:
        inkcell_str_format(out, len, MESH_STR_NODE_TREND_AXIS_DBM, value);
        return;
    case FB_CHART_AXIS_PERMILLE:
        /* The Status card's own format, so the chart and the card round one figure one way. */
        inkcell_str_format(out, len, MESH_STR_STATUS_PERCENT, (double)value / 10.0);
        return;
    case FB_CHART_AXIS_PERCENT:
    default:
        break;
    }
    inkcell_str_format(out, len, MESH_STR_TREND_AXIS_PERCENT, (unsigned)(value > 0 ? value : 0));
}

/* One end of the vertical, in the reading's own units. Whole numbers throughout: an axis end is
   read rather than measured, and a tenth of a percent there is precision nobody is asking it
   for. */
static void fb_chart_axis_end(uint8_t axis, int32_t value, char *out, size_t len) {
    switch ((enum fb_chart_axis)axis) {
    case FB_CHART_AXIS_CELSIUS:
        inkcell_str_format(out, len, MESH_STR_NODE_TREND_AXIS_CELSIUS, value / 10);
        return;
    case FB_CHART_AXIS_DECIBEL:
        inkcell_str_format(out, len, MESH_STR_NODE_TREND_AXIS_DB, value);
        return;
    case FB_CHART_AXIS_DBM:
        inkcell_str_format(out, len, MESH_STR_NODE_TREND_AXIS_DBM, value);
        return;
    case FB_CHART_AXIS_PERMILLE:
        /* Rounded rather than truncated: the ladder's rungs are whole percents of the domain, so
           this is exact on every rung, and a domain that is not on one is better read up than
           cut down - an axis end under the readings it is labelling is the one error a reader
           cannot see. */
        value = (value + 5) / 10;
        break;
    case FB_CHART_AXIS_PERCENT:
    default:
        break;
    }
    inkcell_str_format(out, len, MESH_STR_TREND_AXIS_PERCENT, (unsigned)(value > 0 ? value : 0));
}

/*
 * What one chart screen is: the description, and nothing about how it is drawn.
 *
 * The app bar travels whole rather than as a title and a trail, because a bar is already a
 * component with slots and taking it apart here would be this struct restating them.
 */
struct fb_chart_screen {
    struct inkcell_fb_app_bar bar;
    /* Borrowed for the call, as every pointer in this file's descriptions is. */
    const struct inkcell_series *series[INKCELL_FB_CHART_LINES];
    enum inkcell_str_id labels[INKCELL_FB_CHART_LINES];
    uint32_t count;
    /* The domain the readings are measured on, before the ceiling is contracted to fit them. */
    struct inkcell_scale domain;
    /* Ruled across the plot, or NULL for a reading with no thresholds. */
    const struct inkcell_band *band;
    uint8_t axis; /* enum fb_chart_axis */
    /* The radio's airtime, binned (mesh_ui_trend_airtime()) - which replaces `series` and
       `domain`: line 0 is the channel as columns, line 1 our share as a line over them. */
    const struct mesh_ui_history *airtime;
};

/*
 * The frame, drawn from that description.
 *
 * The order here is the whole of what makes the picture honest, and it is stated in
 * inkcell_trend_frame(): the reader's span cuts the window first, and the ceiling is then picked
 * from the readings *left inside it*. Done the other way round, narrowing the span to the last
 * quarter hour would leave the axis held open by a busy spell that is no longer on the panel.
 *
 * The projection is inkcell_series_project_within() rather than _over(): a window the reader
 * narrowed contains only part of the ring, and the older readings have to be left out rather than
 * stacked on the left-hand edge. See layout.h.
 */
static void fb_render_chart(struct inkcell_draw_state *state,
                            const struct mesh_ui_snapshot *snapshot,
                            struct inkcell_fb_layout *layout,
                            const struct fb_chart_screen *screen) {
    inkcell_fb_draw_app_bar(state, layout, &screen->bar);

    const uint8_t span_choice = snapshot->nav.trend_span;
    const int margin = inkcell_fb_margin(state);
    const int body_w = inkcell_fb_panel_width(state) - margin * 2;
    struct inkcell_trend frame;
    memset(&frame, 0, sizeof frame);
    struct mesh_ui_trend_airtime binned;
    memset(&binned, 0, sizeof binned);
    bool framed = false;
    if (screen->airtime != NULL) {
        /* One bin per cell of the chart's own text across the body: as fine as the panel can
           show a column, and measured in cells rather than pixels. */
        const int adv = inkcell_fb_char_adv(state, layout->small);
        const uint32_t max_bins = adv > 0 ? (uint32_t)(body_w / adv) : INKCELL_TREND_BINS_MAX;
        framed = mesh_ui_trend_airtime(screen->airtime, span_choice, max_bins, &binned);
        frame = binned.frame;
    } else {
        framed =
            inkcell_trend_frame(screen->series, screen->count, screen->domain, span_choice, &frame);
    }
    const struct inkcell_scale scale = framed ? frame.scale : screen->domain;

    struct inkcell_polyline points[INKCELL_FB_CHART_LINES];
    memset(points, 0, sizeof points);
    for (uint32_t i = 0U;
         framed && screen->airtime == NULL && i < screen->count && i < INKCELL_FB_CHART_LINES;
         ++i) {
        inkcell_series_project_within(screen->series[i], scale, frame.from, frame.to, &points[i]);
    }

    /* The ends in the reading's own units, off the domain the lines were actually placed on - so
       a contracted ceiling is a number the reader can see, which is what separates this from the
       auto-scaling it would otherwise be. */
    char top[16];
    char bottom[16];
    fb_chart_axis_end(screen->axis, scale.min == scale.max ? INKCELL_ANIM_ONE : scale.max, top,
                      sizeof top);
    fb_chart_axis_end(screen->axis, scale.min, bottom, sizeof bottom);

    /*
     * How far back the picture goes - which is what there turned out to be, not what was asked
     * for. The strip above the plot says the choice; this says the measurement, and on a span
     * wider than the readings the two differ on purpose.
     */
    char span[48];
    span[0] = '\0';
    if (framed) {
        char words[24];
        mesh_ui_format_duration((frame.to - frame.from) / 1000U, words, sizeof words);
        inkcell_str_format(span, sizeof span, MESH_STR_TREND_SPAN, words);
    }

    /* The picker itself: four words and which one is lit. What they mean is trend.h's. */
    struct inkcell_fb_segmented spans;
    memset(&spans, 0, sizeof spans);
    spans.count = (size_t)INKCELL_TREND_SPAN_COUNT;
    for (size_t i = 0U; i < spans.count && i < INKCELL_FB_SEGMENTED_MAX; ++i) {
        spans.labels[i] = inkcell_str(inkcell_trend_span_label((uint8_t)i));
    }
    spans.active = (size_t)span_choice < spans.count ? (size_t)span_choice : spans.count - 1U;
    spans.value = spans.labels[spans.active];

    const struct inkcell_fb_rect plot_rect = {.x = margin,
                                              .y = layout->body_y,
                                              .w = body_w,
                                              .h = layout->footer_y - inkcell_fb_gutter(state) -
                                                   layout->body_y};
    struct inkcell_fb_chart chart = {
        .rect = plot_rect,
        .count = screen->count,
        .top = top,
        .bottom = bottom,
        .span = span[0] != '\0' ? span : NULL,
        .band = screen->band,
        .scale = scale,
        .spans = &spans,
        /* Reachable now that the span can be narrowed past the last two readings, and the one
           thing a plot with nothing in it must not look like is a frame that failed to draw. */
        .empty = MESH_STR_TREND_EMPTY,
    };
    /*
     * And where each line has got to, which is the one number the picture could not say.
     *
     * From the series' newest sample rather than from anything the screen was handed, because
     * this is a caption on the *line*: a figure taken from the store while the line came from the
     * history would be two readings of one thing, and the day they disagreed the picture would be
     * the one that was right.
     *
     * A line with no readings in the window still names none: the reading it would state is
     * outside the picture, and a legend entry saying otherwise is the frame contradicting the
     * plot.
     */
    char readings[INKCELL_FB_CHART_LINES][24];
    memset(readings, 0, sizeof readings);
    for (uint32_t i = 0U; i < screen->count && i < INKCELL_FB_CHART_LINES; ++i) {
        chart.lines[i].label = screen->labels[i];
        if (screen->airtime != NULL) {
            const struct mesh_ui_airtime_sample *newest =
                mesh_ui_history_airtime_newest(screen->airtime);
            if (framed && newest != NULL) {
                chart.lines[i].bins = i == 0U ? &binned.utilization : &binned.tx;
                chart.lines[i].columns = i == 0U;
                fb_chart_reading(screen->axis, i == 0U ? newest->utilization : newest->tx,
                                 readings[i], sizeof readings[i]);
                chart.lines[i].value = readings[i];
            }
            continue;
        }
        chart.lines[i].points = &points[i];
        const struct inkcell_sample *newest = inkcell_series_newest(screen->series[i]);
        if (newest != NULL && points[i].count > 0U) {
            fb_chart_reading(screen->axis, newest->value, readings[i], sizeof readings[i]);
            chart.lines[i].value = readings[i];
        }
    }
    /*
     * The other face, when the reader has asked for the figures rather than the direction.
     *
     * Built here rather than in inkcell_fb_draw_chart() because only this side knows what the
     * readings *are*: which series, which span, how far the list has been scrolled and what unit
     * the values are worded in. The component is told how many rows it has room for and handed that
     * many - the measure-then-draw split inkcell_fb_chart_reading_rows() exists for, and the same
     * one the note list is on.
     *
     * Only a node's chart has one. The airtime ring is six hours at a reading a minute and is
     * binned into columns precisely because reading by reading is the wrong grain for it - see
     * the readings section of include/mesh/ui/trend.h - so `screen->airtime` never gets here.
     */
    char whens[INKCELL_SERIES_MAX][24];
    char figures[INKCELL_SERIES_MAX][24];
    struct inkcell_fb_chart_reading rows[INKCELL_SERIES_MAX];
    if (snapshot->nav.trend_table && screen->airtime == NULL && screen->count == 1U &&
        screen->series[0] != NULL) {
        const uint32_t room = inkcell_fb_chart_reading_rows(state, layout, &plot_rect,
                                                            chart.spans != NULL ? &spans : NULL);
        /* What the nav pages this list by on the next press, and what the action bar asks before
           it names one. Told even when it is zero, which is a panel with no room for a row. */
        state->page_rows = room;
        const uint32_t total = inkcell_trend_readings(screen->series[0], span_choice);
        uint32_t first = snapshot->nav.trend_scroll;
        /* The clamp has the same arithmetic and runs on the next publish; this is the frame in
           between, and it must not draw past the end of the window it was given. */
        if (total > room && first > total - room) {
            first = total - room;
        } else if (total <= room) {
            first = 0U;
        }
        uint32_t drawn = 0U;
        for (uint32_t i = 0U; i < room && drawn < INKCELL_SERIES_MAX; ++i) {
            struct inkcell_trend_reading reading;
            if (!inkcell_trend_reading_at(screen->series[0], span_choice, first + i, &reading)) {
                break;
            }
            if (first + i == 0U) {
                inkwell_str_copy(whens[drawn], sizeof whens[drawn],
                                 inkcell_str(MESH_STR_TREND_READINGS_NEWEST));
            } else {
                mesh_ui_format_duration(reading.before_ms / 1000U, whens[drawn],
                                        sizeof whens[drawn]);
            }
            fb_chart_reading(screen->axis, reading.value, figures[drawn], sizeof figures[drawn]);
            rows[drawn].when = whens[drawn];
            rows[drawn].value = figures[drawn];
            ++drawn;
        }
        if (drawn > 0U) {
            chart.readings = rows;
            chart.reading_count = drawn;
            chart.readings_note = inkcell_str(MESH_STR_TREND_READINGS_FROM);
        }
    }
    inkcell_fb_draw_chart(state, layout, &chart);
}

/*
 * The airtime trend, over the Status cards that offered it.
 *
 * Binned rather than drawn reading by reading - see mesh_ui_trend_airtime() for why. The channel
 * is columns and our own share a line over them, on one domain and one set of bins: our share is
 * inside the channel's total, so a column and the line above it are two readings of one stretch
 * of air and can be compared by looking.
 */
void fb_render_trend(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                     struct inkcell_fb_layout *layout) {
    const struct fb_chart_screen screen = {
        /* No trail. The navigation bar above is already saying Status, and an overline says only
           what nothing else on the frame says. */
        .bar = {.title = inkcell_str(MESH_STR_TREND_TITLE)},
        .labels = {MESH_STR_TREND_SERIES_CHANNEL, MESH_STR_TREND_SERIES_TX},
        .count = 2U,
        .domain = {0, 0},
        /* The same thresholds the card's bar cuts notches at, so the amber the reader saw there
           is a line here they can watch the trend cross - on the rungs of the ladder that still
           have room for it. A quiet mesh drawn on a twentieth of the domain has both of them off
           the top, and the chart says so by drawing neither. */
        .band = &fb_air_band,
        .axis = FB_CHART_AXIS_PERMILLE,
        .airtime = &snapshot->history,
    };
    fb_render_chart(state, snapshot, layout, &screen);
}

/*
 * One of a node's readings over time, drawn from the row the press was made on.
 *
 * The whole of what this screen knows comes from rebuilding the detail's rows and finding the
 * one whose trend is the reading the nav is holding - and that is the point rather than a
 * shortcut. A row already carries the four things a chart has to get right together: the series,
 * the domain it is measured on, the band ruled across it and the words naming it. Taking them
 * from the row means the chart and the bar the reader opened it from are one statement, drawn
 * twice at two sizes. Taking them from a switch here would be a second opinion about what a
 * temperature is measured between, and the first thing it would get wrong is the day one of them
 * changed.
 *
 * The unit the axis is *worded* in is the one thing that is not on the row, and it is a switch on
 * the reading rather than a fifth column: what a row states is where a reading sits between two
 * ends, and whether those ends are read as degrees or as percent is a question about the words.
 *
 * A reading whose row has gone - the node stopped reporting it, the history was forgotten under
 * us - draws the detail instead. mesh_ui_nav_clamp() closes the chart on the same condition a
 * publish later, so this is the frame in between rather than a state the client sits in.
 */
void fb_render_node_trend(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                          struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, nav->node_detail_node);
    if (node == NULL) {
        fb_render_node_detail(state, snapshot, layout);
        return;
    }
    const bool is_self = hs->has_my_info && node->node_id == hs->my_info.node_num;

    struct mesh_ui_node_item found;
    if (!mesh_ui_node_detail_trend_row(
            node, is_self, mesh_ui_snapshot_traceroute_view(snapshot, node->node_id), hs,
            &snapshot->history, (enum mesh_ui_history_reading)nav->node_trend, &found)) {
        fb_render_node_detail(state, snapshot, layout);
        return;
    }
    const struct mesh_ui_node_item *row = &found;

    /* The reading names the screen; the node is on the trail, because the app bar's overline
       says only what nothing else on the frame says and the navigation bar is already saying
       Nodes. */
    enum inkcell_str_id title = MESH_STR_NODE_TREND_BATTERY;
    uint8_t axis =
        row->scale.min == row->scale.max ? FB_CHART_AXIS_PERMILLE : FB_CHART_AXIS_PERCENT;
    switch ((enum mesh_ui_history_reading)nav->node_trend) {
    case MESH_UI_HISTORY_TEMPERATURE:
        title = MESH_STR_NODE_TREND_TEMPERATURE;
        axis = FB_CHART_AXIS_CELSIUS;
        break;
    case MESH_UI_HISTORY_HUMIDITY:
        title = MESH_STR_NODE_TREND_HUMIDITY;
        break;
    case MESH_UI_HISTORY_SNR:
        title = MESH_STR_NODE_TREND_SNR;
        axis = FB_CHART_AXIS_DECIBEL;
        break;
    case MESH_UI_HISTORY_RSSI:
        title = MESH_STR_NODE_TREND_RSSI;
        axis = FB_CHART_AXIS_DBM;
        break;
    case MESH_UI_HISTORY_BATTERY:
    case MESH_UI_HISTORY_NONE:
    case MESH_UI_HISTORY_READING_COUNT:
    default:
        break;
    }
    char trail[48];
    const char *name = node->long_name[0] != '\0'    ? node->long_name
                       : node->short_name[0] != '\0' ? node->short_name
                                                     : NULL;
    if (name != NULL) {
        inkwell_str_copy(trail, sizeof trail, name);
    } else {
        inkcell_str_format(trail, sizeof trail, MESH_STR_NODE_VAL_USER_ID_HEX, node->node_id);
    }

    const struct fb_chart_screen screen = {
        /* The node *is* the trail here, and it is the one screen where that does not repeat the
           frame: the title is the reading, and without this nothing on the panel would say which
           node's temperature is being drawn. The detail underneath spends its title line on the
           same name for the opposite reason - there, nothing else was competing for it. */
        .bar = {.trail = {trail}, .trail_count = 1U, .title = inkcell_str(title)},
        /*
         * One line, and so no legend to name it: the title says which reading this is, and a
         * legend repeating it would be the frame saying one thing twice. That is also the whole
         * reason the two readings the user asked for are two screens - a chart carries one
         * domain, and a temperature in degrees and a humidity in percent do not share one.
         */
        .series = {row->trend},
        .labels = {INKCELL_STR_NONE},
        .count = 1U,
        .domain = row->scale,
        /* The row's own band, so the amber the reader saw under the figure is a rule here they
           can watch the trend cross - and a row with no band rules none rather than inventing
           thresholds the bar did not have. */
        .band = row->banded ? &row->band : NULL,
        .axis = axis,
    };
    fb_render_chart(state, snapshot, layout, &screen);
}
