#ifndef MESH_UI_TREND_H
#define MESH_UI_TREND_H

/*
 * What a chart is looking at, and the one chart whose source is a radio.
 *
 * The frame - how far back, how far up, the bin ladder, the projection - moved to inkcell
 * (inkcell/ui/trend.h): it is arithmetic about a window and a ceiling, and the fb backend must
 * not be the only thing that can ask. What is left here is the airtime chart, because how often
 * LocalStats arrives and what a gap in it means are facts about a radio rather than about a
 * picture.
 *
 * Nothing here has a pixel in it, for the reason inkcell_series_project() does not. Nothing here
 * holds state either - a span is on the nav, where every other thing a press moves is.
 */

#include "inkcell/ui/trend.h"

#include "inkcell/ui/layout.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/history.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The radio's airtime drawn as columns rather than as a line through every reading.
 *
 * Two series over one window - the air in use, and our own share of it - because the second is
 * only ever read against the first: a client transmitting a tenth of a busy channel and a tenth
 * of an idle one are two different situations and one number.
 */
struct mesh_ui_trend_airtime {
    struct inkcell_trend frame; /* the window, and the ceiling picked from the bins */
    uint32_t bin_ms;
    struct inkcell_trend_bins utilization;
    struct inkcell_trend_bins tx;
};

/*
 * The airtime history cut to `span` and binned, with the ceiling contracted to the tallest bin -
 * the bins are what is drawn, so they are what the axis has to fit.
 *
 * False when mesh_ui_history_has_airtime() would be: no window to lay bins across.
 */
bool mesh_ui_trend_airtime(const struct mesh_ui_history *history, uint8_t span, uint32_t max_bins,
                           struct mesh_ui_trend_airtime *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_TREND_H */
