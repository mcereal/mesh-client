#include "mesh/ui/node_detail.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/duration.h"
#include "mesh/utils/text.h"

/* session.h for the traceroute state enum: the UI struct carries it as a byte so store.h
   stays plain, but this file already pulls nanopb in through radio_settings.h, so naming the
   real enum here beats keeping a second copy of it in step. */
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/ui/settings.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The builder appends through this, so a row that turns out to have nothing to say simply is
   not added and every count in this file stays honest by construction. */
struct node_rows {
    struct mesh_ui_node_item *items;
    uint32_t capacity;
    uint32_t count;
    /*
     * What the client has watched this node do, and which node that is.
     *
     * The pair rather than one pre-resolved series, because three rows now ask for one and the
     * lookup is keyed on the reading: resolving them all up front would be three fields that
     * only ever have one reader each, and a fourth reading would be a fourth. NULL history is
     * every caller that passes none, and answers NULL for every reading.
     */
    const struct mesh_ui_history *history;
    uint32_t node_id;
};

static struct mesh_ui_node_item *rows_next(struct node_rows *rows) {
    if (rows->count >= rows->capacity) {
        return NULL;
    }
    struct mesh_ui_node_item *item = NULL;
    if (rows->items != NULL) {
        item = &rows->items[rows->count];
        memset(item, 0, sizeof *item);
    }
    rows->count++;
    return item;
}

/*
 * A group's heading, and the symbol beside it.
 *
 * The icon is on the heading because a backend that draws these groups as cards draws it as the
 * *card's* icon - one cell that says what the card is about, which is what the eye finds when it
 * is looking for Signal rather than Identity on a screen a hundred and twenty rows long. A flat
 * list leaves the slot empty and nothing about this changes; which of the two is happening is
 * the renderer's business, and the group states its subject either way.
 *
 * Stated at the call site rather than in a table because a heading is emitted in exactly one
 * place each - unlike k_action_icons[] above, where the same verb is reached from several. What
 * would be a table with one reader per row is a parameter.
 *
 * Four ids are spent twice and that is honest rather than lazy, on the terms k_action_icons[]
 * already sets: Environment, Air quality and Health are three sensor reports and DETECTION is
 * what a sensor report is, the two neighbour groups are one subject read in both directions, and
 * the two halves of a traced route are the two halves of one trace. In every pair the labels are
 * what tells them apart, which is the thing a heading is for.
 */
static void rows_heading(struct node_rows *rows, enum mesh_str_id label, enum mesh_ui_icon icon) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    snprintf(item->label, sizeof item->label, "%s", mesh_str(label));
    item->kind = MESH_UI_NODE_ROW_HEADING;
    item->icon = (uint8_t)icon;
}

/*
 * What each verb on this screen is *about*, and how much of a statement pressing it makes.
 *
 * Two tables in the enum's own order rather than two switches, which is settings.c's
 * k_section_icons[] one tab over and for the same three reasons: a lookup with no cases in it
 * cannot fall through, an action added without an entry reads as "no icon" rather than failing
 * to compile somewhere unrelated, and - the one that matters here - the renderer never has to
 * hold a second opinion about which row is the dangerous one.
 *
 * The icons are all ids that already exist, because the meaning is already in the set: asking a
 * node for its name is the same subject the User settings section is, and saving its fix as a
 * waypoint is the same place-pin the Waypoints tab drops. Two rows share POSITION, and that is
 * honest rather than lazy - "Ask where it is" and "Save this place" are two verbs about one
 * subject, and their labels are what tells them apart.
 */
static const enum mesh_ui_icon k_action_icons[] = {
    [MESH_UI_NODE_ACTION_NONE] = MESH_UI_ICON_NONE,
    [MESH_UI_NODE_ACTION_MESSAGE] = MESH_UI_ICON_MESSAGES,
    [MESH_UI_NODE_ACTION_FAVORITE] = MESH_UI_ICON_PINNED,
    /* A traced route is the chain of links that reaches the node, which is what LINK says on
       the Status card's transport row. */
    [MESH_UI_NODE_ACTION_TRACEROUTE] = MESH_UI_ICON_LINK,
    [MESH_UI_NODE_ACTION_REQUEST_INFO] = MESH_UI_ICON_USER,
    [MESH_UI_NODE_ACTION_REQUEST_POSITION] = MESH_UI_ICON_POSITION,
    [MESH_UI_NODE_ACTION_REQUEST_TELEMETRY] = MESH_UI_ICON_TELEMETRY,
    /* The bell with a stroke through it, which is the mark the Messages tab already puts on a
       muted conversation - one mute, one symbol, whichever screen turns it on. */
    [MESH_UI_NODE_ACTION_MUTE] = MESH_UI_ICON_MUTED,
    /* Ignoring is the harder one and gets the harder rune: a mute still lets the traffic
       arrive, an ignore has the radio drop it before we ever see it. */
    [MESH_UI_NODE_ACTION_IGNORE] = MESH_UI_ICON_CLOSE,
    [MESH_UI_NODE_ACTION_REMOVE] = MESH_UI_ICON_DELETE,
    [MESH_UI_NODE_ACTION_WAYPOINT] = MESH_UI_ICON_POSITION,
    [MESH_UI_NODE_ACTION_SHOW_ON_MAP] = MESH_UI_ICON_MAP,
};

/*
 * Two of these eleven rows cost something, and until now nothing on the frame said so: "Message
 * this node" and "Remove from radio" were one colour and one weight, and the only thing between
 * them was reading the words. Ignoring a node is the radio dropping its packets - recoverable,
 * and a surprise if it was not meant - so it takes the warning family; removing it takes the
 * node's own row away, which is the error family and the same ink the confirm dialog uses.
 *
 * Stated here rather than at each call site so the arming press, the row's ink and the action
 * bar's "confirm remove" cannot come from three different opinions about which row is which.
 */
static const enum mesh_ui_tone k_action_tones[] = {
    [MESH_UI_NODE_ACTION_NONE] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_MESSAGE] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_FAVORITE] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_TRACEROUTE] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_REQUEST_INFO] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_REQUEST_POSITION] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_REQUEST_TELEMETRY] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_IGNORE] = MESH_UI_TONE_WARNING,
    [MESH_UI_NODE_ACTION_MUTE] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_REMOVE] = MESH_UI_TONE_ERROR,
    [MESH_UI_NODE_ACTION_WAYPOINT] = MESH_UI_TONE_PRIMARY,
    [MESH_UI_NODE_ACTION_SHOW_ON_MAP] = MESH_UI_TONE_PRIMARY,
};

static enum mesh_ui_icon action_icon(enum mesh_ui_node_action action) {
    return (size_t)action < sizeof k_action_icons / sizeof k_action_icons[0]
               ? k_action_icons[action]
               : MESH_UI_ICON_NONE;
}

static enum mesh_ui_tone action_tone(enum mesh_ui_node_action action) {
    return (size_t)action < sizeof k_action_tones / sizeof k_action_tones[0]
               ? k_action_tones[action]
               : MESH_UI_TONE_PRIMARY;
}

static struct mesh_ui_node_item *rows_action(struct node_rows *rows, enum mesh_str_id label,
                                             const char *value, enum mesh_ui_node_action action) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return NULL;
    }
    snprintf(item->label, sizeof item->label, "%s", mesh_str(label));
    if (value != NULL) {
        snprintf(item->value, sizeof item->value, "%s", value);
    }
    item->kind = MESH_UI_NODE_ROW_ACTION;
    item->action = (uint8_t)action;
    item->icon = (uint8_t)action_icon(action);
    item->tone = (uint8_t)action_tone(action);
    return item;
}

/*
 * The three action rows that are a boolean rather than an errand, said as one.
 *
 * Pinned, muted and ignored are each a flag the press flips, and each spelled its state into
 * the value column as "Yes" or "No" - a control written down as a word, which is the thing
 * fb_draw_switch() was added to stop on the settings rows. The words stay, for the backend with
 * no sprites; what is new is that the state is also a field, so a screen that can draw the
 * control draws it from the same flag this read.
 */
static void rows_toggle(struct node_rows *rows, enum mesh_str_id label, bool on,
                        enum mesh_ui_node_action action) {
    struct mesh_ui_node_item *item =
        rows_action(rows, label, mesh_str(on ? MESH_STR_COMMON_YES : MESH_STR_COMMON_NO), action);
    if (item == NULL) {
        return;
    }
    item->toggle = true;
    item->on = on;
}

/*
 * The three ways a fact gets onto this screen.
 *
 * rows_text() is for a value that is already text - a name off the wire, a formatted age;
 * rows_info() formats a catalog entry into it; rows_named() is the one case where the *label*
 * comes from the mesh rather than from the catalog, which is a neighbour's name or a numbered
 * power channel. Nothing here takes an English string.
 */
static struct mesh_ui_node_item *rows_info_row(struct node_rows *rows, const char *label) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return NULL;
    }
    snprintf(item->label, sizeof item->label, "%s", label);
    item->kind = MESH_UI_NODE_ROW_INFO;
    return item;
}

static void rows_text(struct node_rows *rows, enum mesh_str_id label, const char *value) {
    struct mesh_ui_node_item *item = rows_info_row(rows, mesh_str(label));
    if (item != NULL) {
        snprintf(item->value, sizeof item->value, "%s", value != NULL ? value : "");
    }
}

static void rows_info(struct node_rows *rows, enum mesh_str_id label, enum mesh_str_id format,
                      ...) {
    struct mesh_ui_node_item *item = rows_info_row(rows, mesh_str(label));
    if (item == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)mesh_str_vformat(item->value, sizeof item->value, format, args);
    va_end(args);
}

static void rows_named(struct node_rows *rows, const char *label, enum mesh_str_id format, ...) {
    struct mesh_ui_node_item *item = rows_info_row(rows, label);
    if (item == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)mesh_str_vformat(item->value, sizeof item->value, format, args);
    va_end(args);
}

/*
 * The ends and the boundaries the readings on this screen are drawn against.
 *
 * Where they come from is stated with them in layout.h, because the Status card reads the same
 * airtime limits. What is decided here is only which *unit* each reading travels in, and the
 * rule is that a scale, a value and a band are always three numbers in one unit: airtime in
 * permille because that is the precision the radio reports it at, battery in whole percent
 * because that is all the wire carries, signal in decibels because that is what it is.
 */
static const struct mesh_ui_scale node_battery_scale = {0, 100};
static const struct mesh_ui_band node_battery_band = {.warn = MESH_UI_BATTERY_LOW,
                                                      .bad = MESH_UI_BATTERY_CRITICAL};
/* A zeroed scale is the identity domain: these readings are already permille. */
static const struct mesh_ui_scale node_permille_scale = {0, 0};
static const struct mesh_ui_band node_channel_util_band = {.warn = MESH_UI_AIRTIME_BUSY_WARN,
                                                           .bad = MESH_UI_AIRTIME_BUSY_BAD};
static const struct mesh_ui_band node_air_tx_band = {.warn = MESH_UI_AIRTIME_TX_WARN,
                                                     .bad = MESH_UI_AIRTIME_TX_BAD};
/*
 * The node's own air, and the two bands that are about the *node* rather than about the weather.
 *
 * A meter's test is whether the figure has ends the reader does not know, and a temperature is
 * the one reading on this screen where that depends on what is being asked. Nobody needs a bar
 * to understand 22 degrees of afternoon; everybody needs one to know whether the box on the pole
 * is inside what its cells and its LoRa module will tolerate. The thresholds in layout.h are
 * stated for the second question, which is the only one this client can answer, and the scale
 * runs wide enough that an ordinary day is not pinned against either end.
 */
static const struct mesh_ui_scale node_temperature_scale = {MESH_UI_TEMPERATURE_FLOOR,
                                                            MESH_UI_TEMPERATURE_CEILING};
static const struct mesh_ui_band node_temperature_band = {.warn = MESH_UI_TEMPERATURE_WARM,
                                                          .bad = MESH_UI_TEMPERATURE_HOT};
static const struct mesh_ui_band node_humidity_band = {.warn = MESH_UI_HUMIDITY_DAMP,
                                                       .bad = MESH_UI_HUMIDITY_WET};
static const struct mesh_ui_scale node_snr_scale = {MESH_UI_SNR_FLOOR, MESH_UI_SNR_CEILING};
static const struct mesh_ui_band node_snr_band = {.warn = MESH_UI_SNR_FAIR,
                                                  .bad = MESH_UI_SNR_POOR};

/*
 * An SNR in whole decibels, rounded rather than truncated.
 *
 * A cast alone truncates toward zero, which on a negative reading always moves it *up* - so a
 * link at -7.6 dB would be banded as though it were at -7, and the one direction a signal bar
 * must not err in is optimism.
 */
static int32_t snr_db(float snr) { return (int32_t)(snr < 0.0f ? snr - 0.5f : snr + 0.5f); }

/*
 * The fourth way a fact gets onto this screen, and it is a modifier on the other three rather
 * than a way of its own: the row has already said what it says, and this adds the ends the
 * figure is measured between.
 *
 * Written that way round deliberately. A reading with a scale is still a reading, so it keeps
 * the same builder, the same label and the same formatted value - which is what lets a backend
 * with nothing to draw a bar with show exactly what it showed before. A separate rows_meter()
 * would have had to restate the formatting, and the two copies would have drifted the first
 * time a unit changed.
 */
static void rows_gauge(struct node_rows *rows, int32_t value, struct mesh_ui_scale scale,
                       const struct mesh_ui_band *band) {
    /* The row builder counts past the end so its totals stay honest, so "there is a row behind
       me" is not the same question as "a row was written". */
    if (rows->items == NULL || rows->count == 0U || rows->count > rows->capacity) {
        return;
    }
    struct mesh_ui_node_item *item = &rows->items[rows->count - 1U];
    item->kind = MESH_UI_NODE_ROW_METER;
    item->number = value;
    item->scale = scale;
    if (band != NULL) {
        item->band = *band;
        item->banded = true;
    }
}

/*
 * The fifth way, and a modifier on a modifier: what this reading has been doing, hung on the
 * row that already says what it is now.
 *
 * Only ever on a meter row - see `trend` on struct mesh_ui_node_item - which rows_gauge() has
 * just made, so the two are called as a pair and the second is refused if the first did not
 * happen. That is not defensiveness: a trend on an info row would be a line with no ends to be
 * drawn between, and the ends are the meter's.
 */
/*
 * Hangs this node's trend of `reading` on the meter row just emitted, and says which reading it
 * is so a press can name it.
 *
 * The series and the reading are set together and never apart, which is the whole reason the
 * lookup is in here rather than at the call site: they are one statement - "this row's bar has
 * been doing this" - and a row carrying a battery series labelled as a temperature is a chart
 * that draws the wrong line under the right axis. A reading with nothing watched leaves both
 * unset, so the row is a bar with no press rather than a press with nothing behind it.
 */
static void rows_trend(struct node_rows *rows, enum mesh_ui_history_reading reading) {
    if (rows->items == NULL || rows->count == 0U || rows->count > rows->capacity) {
        return;
    }
    /*
     * A drawable *segment*, not a sample - mesh_ui_history_has_airtime()'s test, asked here
     * because this is where a reading becomes both a picture and a press.
     *
     * Every sample following a silence the series calls a break starts a line rather than
     * continuing one, so a node heard once, or twice either side of a two-hour gap, is readings
     * the ring holds and no stroke at all. The sparkline was already honest about that by
     * drawing nothing; the press is not, because what A would open is an axis frame with its
     * ends labelled and nothing between them. Gating the attach rather than the press keeps the
     * two answers one answer: a row has a trend, or it has neither trend nor verb.
     */
    const struct mesh_ui_series *series =
        mesh_ui_history_series(rows->history, rows->node_id, reading);
    if (series == NULL || !mesh_ui_series_has_segment(series)) {
        return;
    }
    struct mesh_ui_node_item *item = &rows->items[rows->count - 1U];
    if (item->kind != MESH_UI_NODE_ROW_METER) {
        return;
    }
    item->trend = series;
    item->trend_reading = (uint8_t)reading;
}

static void node_rows_identity(struct node_rows *rows, const struct mesh_ui_node_summary *node) {
    rows_heading(rows, MESH_STR_NODE_HEAD_IDENTITY, MESH_UI_ICON_USER);

    if (node->long_name[0] != '\0') {
        rows_text(rows, MESH_STR_NODE_LONG_NAME, node->long_name);
    }
    if (node->short_name[0] != '\0') {
        rows_text(rows, MESH_STR_NODE_SHORT_NAME, node->short_name);
    }
    /* The radio gives the id as text; derive it when a node was added from a bare packet. */
    if (node->user_id[0] != '\0') {
        rows_text(rows, MESH_STR_NODE_USER_ID, node->user_id);
    } else {
        rows_info(rows, MESH_STR_NODE_USER_ID, MESH_STR_NODE_VAL_USER_ID_HEX, node->node_id);
    }
    rows_info(rows, MESH_STR_NODE_NUMBER, MESH_STR_NODE_VAL_NUMBER, (unsigned)node->node_id);
    /* Two things the row above cannot say on its own. A derived name is not a name the node
       chose, and a node the radio's NodeDB no longer carries is one this client remembers
       alone - it is still on the mesh, but a message to it has no stored key to travel with. */
    if (!node->has_user) {
        rows_text(rows, MESH_STR_NODE_NAME, mesh_str(MESH_STR_NODE_DERIVED_NAME));
    }
    if (!node->in_nodedb) {
        rows_text(rows, MESH_STR_NODE_NODEDB, mesh_str(MESH_STR_NODE_NOT_IN_NODEDB));
    }

    if (node->role != 0U || node->hw_model != 0U) {
        rows_text(rows, MESH_STR_NODE_ROLE, mesh_radio_role_name(node->role));
    }
    if (node->hw_model != 0U) {
        char fallback[MESH_UI_NODE_VALUE_MAX];
        rows_text(rows, MESH_STR_NODE_HARDWARE,
                  mesh_radio_hw_model_name(node->hw_model, fallback, sizeof fallback));
    }
    if (node->public_key_len > 0U) {
        char key[MESH_UI_NODE_VALUE_MAX];
        mesh_ui_settings_key_text(node->public_key, node->public_key_len, key, sizeof key);
        rows_text(rows, MESH_STR_NODE_PUBLIC_KEY, key);
    }

    /* One row for the handful of booleans, so a plain node does not carry four "no" rows. */
    char flags[MESH_UI_NODE_VALUE_MAX];
    flags[0] = '\0';
    const char *set[3];
    size_t set_count = 0U;
    if (node->is_ignored) {
        set[set_count++] = mesh_str(MESH_STR_NODE_FLAG_IGNORED);
    }
    if (node->is_licensed) {
        set[set_count++] = mesh_str(MESH_STR_NODE_FLAG_LICENSED);
    }
    if (node->is_unmessagable) {
        set[set_count++] = mesh_str(MESH_STR_NODE_FLAG_UNMESSAGEABLE);
    }
    for (size_t i = 0; i < set_count; ++i) {
        const size_t used = strlen(flags);
        snprintf(flags + used, sizeof flags - used, "%s%s",
                 i > 0U ? mesh_str(MESH_STR_NODE_FLAG_SEPARATOR) : "", set[i]);
    }
    if (flags[0] != '\0') {
        rows_text(rows, MESH_STR_NODE_FLAGS, flags);
    }
}

static void node_rows_signal(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                             bool is_self, uint32_t now) {
    rows_heading(rows, MESH_STR_NODE_HEAD_SIGNAL, MESH_UI_ICON_LORA);

    char age[24];
    mesh_ui_format_age(node->last_heard, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_LAST_HEARD, age);

    if (!is_self) {
        rows_info(rows, MESH_STR_NODE_SNR, MESH_STR_NODE_VAL_SNR, (double)node->snr);
        /*
         * And where that sits between the demodulator's floor and a link that could not be
         * better, which is the part decibels do not say to anyone who has not memorised them.
         *
         * Only when the reading is this node's own - see mesh_ui_node_signal_heard(). The
         * figure above stays either way: it is true, it is just not always about what the label
         * says, and that is the difference between printing it and drawing it.
         */
        if (mesh_ui_node_signal_heard(node)) {
            rows_gauge(rows, snr_db(node->snr), node_snr_scale, &node_snr_band);
        }
        /* Beside it rather than instead of it: SNR is how far above the noise the packet was
           and RSSI is how loud it was, and a link can be good on one and poor on the other. */
        if (node->has_rssi) {
            /* Only this radio can measure an RSSI, so a node now reaching us over MQTT keeps
               the reading from the last packet we heard ourselves. Saying when that was is what
               stops the row reading as a description of the packet that just arrived. */
            if (node->rssi_time != 0U && node->last_heard > node->rssi_time) {
                char measured[24];
                mesh_ui_format_age(node->rssi_time, now, measured, sizeof measured);
                rows_info(rows, MESH_STR_NODE_RSSI, MESH_STR_NODE_VAL_RSSI_AGED, (int)node->rx_rssi,
                          measured);
            } else {
                rows_info(rows, MESH_STR_NODE_RSSI, MESH_STR_NODE_VAL_RSSI, (int)node->rx_rssi);
            }
        }
        if (node->has_hops_away) {
            rows_info(rows, MESH_STR_NODE_HOPS_AWAY, MESH_STR_NODE_VAL_NUMBER,
                      (unsigned)node->hops_away);
        } else {
            rows_text(rows, MESH_STR_NODE_HOPS_AWAY, mesh_str(MESH_STR_COMMON_UNKNOWN));
        }
    }
    rows_info(rows, MESH_STR_NODE_CHANNEL, MESH_STR_NODE_VAL_NUMBER, (unsigned)node->channel);
    rows_text(rows, MESH_STR_NODE_HEARD_VIA,
              mesh_str(node->via_mqtt ? MESH_STR_NODE_VIA_MQTT : MESH_STR_NODE_VIA_RF));
}

static void node_rows_power(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                            uint32_t now) {
    const struct mesh_ui_node_metrics *metrics = &node->metrics;
    if (!metrics->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_METRICS, MESH_UI_ICON_TELEMETRY);

    if (metrics->has_battery) {
        /* 101 is upstream's "running off USB", not a 101% battery. */
        if (metrics->battery_level > 100U) {
            rows_text(rows, MESH_STR_NODE_BATTERY, mesh_str(MESH_STR_STATUS_BATTERY_USB));
        } else {
            rows_info(rows, MESH_STR_NODE_BATTERY, MESH_STR_NODE_VAL_PERCENT,
                      (unsigned)metrics->battery_level);
            rows_gauge(rows, (int32_t)metrics->battery_level, node_battery_scale,
                       &node_battery_band);
            /* And which way it has been going, which is the question a battery percentage is
               nearly always a proxy for. Drawn on the bar's own scale, so the line and the bar
               under it are one reading measured twice rather than two. */
            rows_trend(rows, MESH_UI_HISTORY_BATTERY);
        }
    }
    if (metrics->has_voltage) {
        rows_info(rows, MESH_STR_NODE_VOLTAGE, MESH_STR_NODE_VAL_VOLTS, (double)metrics->voltage);
    }
    if (metrics->has_channel_utilization) {
        rows_info(rows, MESH_STR_NODE_CHANNEL_UTIL, MESH_STR_NODE_VAL_PERCENT_FINE,
                  (double)metrics->channel_utilization);
        rows_gauge(rows, mesh_ui_percent_permille(metrics->channel_utilization),
                   node_permille_scale, &node_channel_util_band);
    }
    if (metrics->has_air_util_tx) {
        rows_info(rows, MESH_STR_NODE_AIR_UTIL_TX, MESH_STR_NODE_VAL_PERCENT_FINE,
                  (double)metrics->air_util_tx);
        /* Its own band, an order of magnitude below the one above: this is the radio's own
           transmit duty cycle rather than how busy the band is. */
        rows_gauge(rows, mesh_ui_percent_permille(metrics->air_util_tx), node_permille_scale,
                   &node_air_tx_band);
    }
    if (metrics->has_uptime) {
        char uptime[24];
        mesh_ui_format_duration(metrics->uptime_seconds, uptime, sizeof uptime);
        rows_text(rows, MESH_STR_NODE_UPTIME, uptime);
    }
    char age[24];
    mesh_ui_format_age(metrics->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_position(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                               uint32_t now) {
    const struct mesh_ui_node_position *position = &node->position;
    if (!position->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_POSITION, MESH_UI_ICON_POSITION);

    /* Fixed-point 1e-7 degrees on the wire; five decimals is about a metre, which is finer
       than anything a LoRa node reports. */
    rows_info(rows, MESH_STR_NODE_LATITUDE, MESH_STR_NODE_VAL_DEGREES,
              (double)position->latitude_i / 1e7);
    rows_info(rows, MESH_STR_NODE_LONGITUDE, MESH_STR_NODE_VAL_DEGREES,
              (double)position->longitude_i / 1e7);
    if (position->has_altitude) {
        rows_info(rows, MESH_STR_NODE_ALTITUDE, MESH_STR_NODE_VAL_METRES, (int)position->altitude);
    }
    if (position->sats_in_view > 0U) {
        rows_info(rows, MESH_STR_NODE_SATELLITES, MESH_STR_NODE_VAL_NUMBER,
                  (unsigned)position->sats_in_view);
    }
    /* A bit count is not a fact about the world. The sender rounded its coordinates off by
       this many bits, and the phone apps' distance for each step is the honest way to say how
       much - so the row reads "~360 m" and the five decimals above it are read as the rounded
       number they are. 0 here means the node never set the field, not "off": an unrounded fix
       and one whose precision we were not told apart are the same to us, and neither claims a
       footprint it cannot support. */
    if (position->precision_bits > 0U) {
        char precision[24];
        mesh_ui_settings_format_precision((uint32_t)position->precision_bits, precision,
                                          sizeof precision);
        rows_text(rows, MESH_STR_NODE_PRECISION, precision);
    }

    /*
     * Whose clock this is, said out loud. The node's own dating of the fix comes first
     * because it is the answer to the question the row asks; when the node dated nothing -
     * which is most packets, since upstream leaves `time` off the mesh to save space - the
     * row switches to when the fix reached us and changes its label to match. Falling back
     * silently would put our arrival time under a heading that reads as the node's, and
     * last_heard is not offered here at all: it advances on any packet, so a chatty node that
     * has not moved in a day would report a one-minute-old fix.
     */
    char age[24];
    if (position->time != 0U) {
        mesh_ui_format_age(position->time, now, age, sizeof age);
        rows_text(rows, MESH_STR_NODE_FIX, age);
    } else {
        mesh_ui_format_age(position->received, now, age, sizeof age);
        rows_text(rows, MESH_STR_NODE_FIX_RECEIVED, age);
    }
}

static void node_rows_environment(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                  uint32_t now) {
    const struct mesh_ui_node_environment *env = &node->environment;
    if (!env->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_ENVIRONMENT, MESH_UI_ICON_DETECTION);

    /*
     * The two readings the client keeps a trend of, so each is a figure, a bar and a line.
     *
     * Both rows keep the value text they had - the bar is a third thing said about the reading
     * rather than a replacement for it, which is the rule the battery row above follows and the
     * reason the CLI backend still shows a complete fact. The bar is drawn in the unit the series
     * is kept in so that the row and the chart it opens are one reading measured once: tenths of
     * a degree here, permille there, converted where every other reading off the air is.
     */
    if (env->has_temperature) {
        rows_info(rows, MESH_STR_NODE_TEMPERATURE, MESH_STR_NODE_VAL_TEMPERATURE,
                  (double)env->temperature, (double)env->temperature * 9.0 / 5.0 + 32.0);
        rows_gauge(rows, mesh_ui_temperature_decidegrees(env->temperature), node_temperature_scale,
                   &node_temperature_band);
        rows_trend(rows, MESH_UI_HISTORY_TEMPERATURE);
    }
    if (env->has_humidity) {
        rows_info(rows, MESH_STR_NODE_HUMIDITY, MESH_STR_NODE_VAL_PERCENT_FINE,
                  (double)env->relative_humidity);
        rows_gauge(rows, mesh_ui_percent_permille(env->relative_humidity), node_permille_scale,
                   &node_humidity_band);
        rows_trend(rows, MESH_UI_HISTORY_HUMIDITY);
    }
    if (env->has_pressure) {
        rows_info(rows, MESH_STR_NODE_PRESSURE, MESH_STR_NODE_VAL_PRESSURE,
                  (double)env->barometric_pressure);
    }
    if (env->has_iaq) {
        rows_info(rows, MESH_STR_NODE_AIR_QUALITY, MESH_STR_NODE_VAL_IAQ, (unsigned)env->iaq);
    }
    if (env->has_lux) {
        rows_info(rows, MESH_STR_NODE_LIGHT, MESH_STR_NODE_VAL_LUX, (double)env->lux);
    }
    if (env->has_voltage) {
        rows_info(rows, MESH_STR_NODE_VOLTAGE, MESH_STR_NODE_VAL_VOLTS, (double)env->voltage);
    }
    if (env->has_current) {
        rows_info(rows, MESH_STR_NODE_CURRENT, MESH_STR_NODE_VAL_MILLIAMPS_FINE,
                  (double)env->current);
    }
    char age[24];
    mesh_ui_format_age(env->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

/*
 * The four sensor groups beyond device metrics and environment. Each is emitted only when the
 * node has actually reported it, so a plain handheld shows none of them and a solar-powered
 * weather station shows two - which is the whole reason they are separate groups rather than
 * one "Telemetry" heading with empty rows under it.
 */
static void node_rows_power_metrics(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                    uint32_t now) {
    const struct mesh_ui_node_power *power = &node->power;
    if (!power->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_POWER, MESH_UI_ICON_POWER);
    for (size_t ch = 0; ch < sizeof power->channel / sizeof power->channel[0]; ++ch) {
        const struct mesh_ui_node_power_channel *channel = &power->channel[ch];
        if (!channel->has_voltage && !channel->has_current) {
            continue;
        }
        char label[MESH_UI_NODE_LABEL_MAX];
        mesh_str_format(label, sizeof label, MESH_STR_NODE_POWER_CHANNEL, (unsigned)ch + 1U);
        /* Both readings on one row: a supply is a voltage and a draw, and splitting them makes
           a three-channel board six rows that have to be read in pairs anyway. */
        if (channel->has_voltage && channel->has_current) {
            rows_named(rows, label, MESH_STR_NODE_VAL_VOLTS_MILLIAMPS, (double)channel->voltage,
                       (double)channel->current);
        } else if (channel->has_voltage) {
            rows_named(rows, label, MESH_STR_NODE_VAL_VOLTS, (double)channel->voltage);
        } else {
            rows_named(rows, label, MESH_STR_NODE_VAL_MILLIAMPS, (double)channel->current);
        }
    }
    char age[24];
    mesh_ui_format_age(power->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_air_quality(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                  uint32_t now) {
    const struct mesh_ui_node_air_quality *air = &node->air_quality;
    if (!air->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_AIR_QUALITY, MESH_UI_ICON_DETECTION);
    /* PM2.5 first and on its own row: it is the number air quality is judged by, and the one a
       person looks for. The coarser fractions share a row because they are read against it. */
    if (air->has_pm25) {
        rows_info(rows, MESH_STR_NODE_PM25, MESH_STR_NODE_VAL_PARTICULATES,
                  (unsigned)air->pm25_standard);
    }
    if (air->has_pm10 && air->has_pm100) {
        rows_info(rows, MESH_STR_NODE_PM1_PM10, MESH_STR_NODE_VAL_PARTICULATES_TWO,
                  (unsigned)air->pm10_standard, (unsigned)air->pm100_standard);
    } else if (air->has_pm10) {
        rows_info(rows, MESH_STR_NODE_PM1, MESH_STR_NODE_VAL_PARTICULATES,
                  (unsigned)air->pm10_standard);
    } else if (air->has_pm100) {
        rows_info(rows, MESH_STR_NODE_PM10, MESH_STR_NODE_VAL_PARTICULATES,
                  (unsigned)air->pm100_standard);
    }
    if (air->has_co2) {
        rows_info(rows, MESH_STR_NODE_CO2, MESH_STR_NODE_VAL_PPM, (unsigned)air->co2);
    }
    if (air->has_voc_index) {
        rows_info(rows, MESH_STR_NODE_VOC_INDEX, MESH_STR_NODE_VAL_INDEX, (double)air->voc_index);
    }
    if (air->has_nox_index) {
        rows_info(rows, MESH_STR_NODE_NOX_INDEX, MESH_STR_NODE_VAL_INDEX, (double)air->nox_index);
    }
    char age[24];
    mesh_ui_format_age(air->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_health(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                             uint32_t now) {
    const struct mesh_ui_node_health *health = &node->health;
    if (!health->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_HEALTH, MESH_UI_ICON_DETECTION);
    if (health->has_heart_bpm) {
        rows_info(rows, MESH_STR_NODE_HEART_RATE, MESH_STR_NODE_VAL_BPM,
                  (unsigned)health->heart_bpm);
    }
    if (health->has_spo2) {
        rows_info(rows, MESH_STR_NODE_SPO2, MESH_STR_NODE_VAL_PERCENT, (unsigned)health->spo2);
    }
    if (health->has_temperature) {
        rows_info(rows, MESH_STR_NODE_TEMPERATURE, MESH_STR_NODE_VAL_TEMPERATURE,
                  (double)health->temperature, (double)health->temperature * 1.8 + 32.0);
    }
    char age[24];
    mesh_ui_format_age(health->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_host(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                           uint32_t now) {
    const struct mesh_ui_node_host *host = &node->host;
    if (!host->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_HOST, MESH_UI_ICON_STATUS);
    if (host->has_uptime) {
        char uptime[32];
        mesh_ui_format_duration(host->uptime_seconds, uptime, sizeof uptime);
        rows_text(rows, MESH_STR_NODE_UPTIME, uptime);
    }
    if (host->has_freemem) {
        rows_info(rows, MESH_STR_NODE_FREE_MEMORY, MESH_STR_NODE_VAL_MEGABYTES,
                  host->freemem_kib / 1024U);
    }
    if (host->has_diskfree) {
        /* Below a gigabyte the megabyte figure is the one that matters; above it, it is noise. */
        if (host->diskfree_mib >= 1024U) {
            rows_info(rows, MESH_STR_NODE_FREE_DISK, MESH_STR_NODE_VAL_GIGABYTES,
                      (double)host->diskfree_mib / 1024.0);
        } else {
            rows_info(rows, MESH_STR_NODE_FREE_DISK, MESH_STR_NODE_VAL_MEGABYTES,
                      host->diskfree_mib);
        }
    }
    if (host->has_load) {
        /* The firmware sends the load average times 100. */
        rows_info(rows, MESH_STR_NODE_LOAD, MESH_STR_NODE_VAL_LOAD, (double)host->load1 / 100.0,
                  (double)host->load5 / 100.0, (double)host->load15 / 100.0);
    }
    char age[24];
    mesh_ui_format_age(host->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

/*
 * The mesh as a graph, which is the one thing a neighbour list gives that nothing else does.
 *
 * Two groups, and the second is the reason this screen needs the whole roster rather than one
 * node. "Neighbours" is what the node itself reported it can hear - an out-edge list, and the
 * only thing on the wire that says so. "Heard by" is the reverse, and no node reports it: it
 * exists only as every *other* node's list read backwards, and it is the half a person holding
 * the radio actually wants, because "is anything hearing me" is not a question a hop count or
 * an SNR reading can answer.
 *
 * A neighbour is a bare node number on the wire, so each is resolved against the roster and
 * falls back to the "!0a1b2c3d" form the apps show - the same fallback the identity group uses
 * for a node with no User.
 */
static void node_rows_neighbor_name(const struct mesh_ui_handshake_state *roster, uint32_t node_id,
                                    char *out, size_t out_len) {
    if (roster != NULL) {
        const uint32_t count = roster->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                                   ? MESH_UI_MAX_HANDSHAKE_NODES
                                   : roster->node_count;
        for (uint32_t i = 0; i < count; ++i) {
            if (roster->nodes[i].node_id != node_id) {
                continue;
            }
            const char *name = roster->nodes[i].short_name[0] != '\0' ? roster->nodes[i].short_name
                                                                      : roster->nodes[i].long_name;
            if (name[0] != '\0') {
                mesh_str_copy(out, out_len, name);
                return;
            }
            break;
        }
    }
    mesh_str_format(out, out_len, MESH_STR_NODE_VAL_USER_ID_HEX, node_id);
}

static void node_rows_neighbors(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                const struct mesh_ui_handshake_state *roster, uint32_t now) {
    if (roster == NULL) {
        return;
    }

    const struct mesh_ui_node_neighbors *heard = &node->neighbors;
    if (heard->valid) {
        rows_heading(rows, MESH_STR_NODE_HEAD_NEIGHBOURS, MESH_UI_ICON_NEIGHBORS);
        if (heard->count == 0U) {
            /* A node that hears nobody is a real state and an interesting one - it is how a
               repeater that has fallen off the mesh looks - so it says so rather than showing
               a heading with nothing under it. */
            rows_text(rows, MESH_STR_NODE_NEIGHBOURS_NONE, mesh_str(MESH_STR_NODE_HEARS_NO_ONE));
        }
        for (uint8_t i = 0; i < heard->count && i < MESH_UI_MAX_NEIGHBORS; ++i) {
            char name[MESH_UI_NODE_LABEL_MAX];
            node_rows_neighbor_name(roster, heard->entries[i].node_id, name, sizeof name);
            rows_named(rows, name, MESH_STR_NODE_VAL_SNR, (double)heard->entries[i].snr);
        }
        char age[24];
        mesh_ui_format_age(heard->time, now, age, sizeof age);
        rows_text(rows, MESH_STR_NODE_REPORTED, age);
    }

    /*
     * The reverse edges. Walked over the roster rather than stored, because it is derived from
     * data that changes under it: a node that stops hearing us drops out of its own next
     * report, and a cached answer would keep saying it still does.
     *
     * The ten-entry cap upstream puts on a neighbour list is a cap on what *one* node reports,
     * not on how many nodes may report hearing this one - on a dense mesh that is every node in
     * range. So the rows are capped for the row budget's sake but the count is not: stopping at
     * ten silently would make the one screen whose question is "how many can hear me" answer it
     * wrongly, and quietly.
     */
    uint32_t listeners = 0U;
    uint32_t shown = 0U;
    const uint32_t count = roster->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                               ? MESH_UI_MAX_HANDSHAKE_NODES
                               : roster->node_count;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_node_summary *other = &roster->nodes[i];
        if (other->node_id == node->node_id || !other->neighbors.valid) {
            continue;
        }
        for (uint8_t n = 0; n < other->neighbors.count && n < MESH_UI_MAX_NEIGHBORS; ++n) {
            if (other->neighbors.entries[n].node_id != node->node_id) {
                continue;
            }
            if (listeners == 0U) {
                rows_heading(rows, MESH_STR_NODE_HEAD_HEARD_BY, MESH_UI_ICON_NEIGHBORS);
            }
            listeners++;
            /* The roster is already ordered by mesh_app_node_rank, so the first ten are the
               ones a reader would have looked for anyway. */
            if (shown < MESH_UI_NODE_MAX_LISTENERS) {
                char name[MESH_UI_NODE_LABEL_MAX];
                node_rows_neighbor_name(roster, other->node_id, name, sizeof name);
                rows_named(rows, name, MESH_STR_NODE_VAL_SNR,
                           (double)other->neighbors.entries[n].snr);
                shown++;
            }
            break;
        }
    }
    if (listeners > shown) {
        rows_info(rows, MESH_STR_NODE_AND_MORE, MESH_STR_NODE_NOT_SHOWN, listeners - shown);
    }
}

/*
 * The verb that starts a trace, and what the one trace slot currently says about this node.
 *
 * Emitted whatever the state, because it is also how a trace is re-run. A trace of some *other*
 * node leaves it a plain "press A", so opening a second node never appears to describe it with
 * the first one's route.
 *
 * Split from the path rows below, and the split is load-bearing rather than tidying. This is an
 * action and it belongs among the actions; a measured route is a *report*, and a report between
 * two verbs is a group interrupting a group. A renderer that draws each group as a card has no
 * way back from that - what says a card ends is the next heading, and nothing says a run of rows
 * has rejoined the group it left - so the verbs after the route were drawn inside the "Route
 * back" card, under a heading that had nothing to do with them. Keeping every group a single
 * unbroken run is what the cards need and what the flat list wanted anyway.
 */
static void node_rows_route_action(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                   const struct mesh_ui_traceroute *trace) {
    const bool ours = trace != NULL && trace->target == node->node_id;
    const char *value = mesh_str(MESH_STR_COMMON_PRESS_A);
    if (ours) {
        switch ((enum mesh_traceroute_state)trace->state) {
        case MESH_TRACEROUTE_PENDING:
            value = mesh_str(MESH_STR_NODE_TRACE_RUNNING);
            break;
        case MESH_TRACEROUTE_TIMEOUT:
            value = mesh_str(MESH_STR_NODE_TRACE_TIMEOUT);
            break;
        default:
            break;
        }
    }
    rows_action(rows, MESH_STR_NODE_TRACE_ROUTE, value, MESH_UI_NODE_ACTION_TRACEROUTE);
}

/*
 * The traced route itself, when the one trace slot is holding a finished trace of this node.
 * Two paths of stops, each row a node and the SNR of the link that reached it - the first stop
 * of a path is the sender and has no incoming link, so it carries no reading rather than a zero.
 */
static void node_rows_route_path(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                 const struct mesh_ui_traceroute *trace, uint32_t now) {
    const bool ours = trace != NULL && trace->target == node->node_id;
    if (!ours || trace->state != MESH_TRACEROUTE_DONE) {
        return;
    }

    for (unsigned direction = 0; direction < 2U; ++direction) {
        const struct mesh_ui_traceroute_hop *path = direction == 0U ? trace->forward : trace->back;
        const uint8_t count = direction == 0U ? trace->forward_count : trace->back_count;
        if (count == 0U) {
            continue;
        }
        /* LINK is what k_action_icons[] gives the traceroute verb, which is the press these rows
           came out of - one trace, one symbol, whichever end of it is being read. */
        rows_heading(rows,
                     direction == 0U ? MESH_STR_NODE_HEAD_ROUTE_OUT : MESH_STR_NODE_HEAD_ROUTE_BACK,
                     MESH_UI_ICON_LINK);
        for (uint8_t i = 0; i < count && i < MESH_UI_TRACEROUTE_MAX_HOPS; ++i) {
            const struct mesh_ui_traceroute_hop *hop = &path[i];
            char label[MESH_UI_NODE_LABEL_MAX];
            /* An arrow would be two bytes the framebuffer font has no glyph for. */
            snprintf(label, sizeof label, "%s%s", i == 0U ? "" : mesh_str(MESH_STR_NODE_HOP_ARROW),
                     hop->name);
            /* INT8_MIN is the firmware's "this link was not measured", not a -32 dB link. */
            if (hop->has_snr && hop->snr_quarter_db != INT8_MIN) {
                rows_named(rows, label, MESH_STR_NODE_VAL_SNR, (double)hop->snr_quarter_db / 4.0);
            } else {
                struct mesh_ui_node_item *row = rows_info_row(rows, label);
                if (row != NULL) {
                    snprintf(
                        row->value, sizeof row->value, "%s",
                        mesh_str(i == 0U ? MESH_STR_NODE_HOP_START : MESH_STR_NODE_HOP_NO_READING));
                }
            }
        }
    }

    /* A route is only true for as long as the mesh holds still, so the section closes with
       when it was measured rather than presenting it as a standing fact - the same trailing
       stamp the metrics and position groups carry. */
    char age[24];
    mesh_ui_format_age(trace->completed, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_MEASURED, age);
}

uint32_t mesh_ui_node_detail_build(const struct mesh_ui_node_summary *node, bool is_self,
                                   uint32_t now, const struct mesh_ui_traceroute *trace,
                                   bool remove_armed, const struct mesh_ui_handshake_state *roster,
                                   const struct mesh_ui_history *history,
                                   struct mesh_ui_node_item *out, uint32_t capacity) {
    if (node == NULL) {
        return 0U;
    }

    struct node_rows rows = {
        .items = out,
        .capacity = (out == NULL) ? MESH_UI_NODE_ITEMS_MAX : capacity,
        .count = 0U,
        .history = history,
        .node_id = node->node_id,
    };

    /*
     * The actions lead: opening a node from the Nodes tab used to go straight to its
     * conversation, so the first thing under the cursor still gets you there.
     *
     * Under a heading of their own since, which is the smaller half of the same point. Every
     * other group on this screen names itself and this one did not, so eleven verbs simply
     * *began* the screen and the first thing the eye met was a wall of them with no word saying
     * what they had in common - and the "Identity" heading four rows down then read as the
     * first heading rather than the second. A heading costs one row and is what turns the block
     * into a group the reader can skip past.
     */
    const uint32_t actions_at = rows.count;
    if (!is_self) {
        rows_heading(&rows, MESH_STR_NODE_HEAD_ACTIONS, MESH_UI_ICON_ACTIONS);
        rows_action(&rows, MESH_STR_NODE_ACT_MESSAGE, NULL, MESH_UI_NODE_ACTION_MESSAGE);
        /* Pinning our own node would be meaningless - it already ranks above everything. */
        rows_toggle(&rows, MESH_STR_NODE_ACT_PIN, node->is_favorite, MESH_UI_NODE_ACTION_FAVORITE);
        /* Tracing the route to ourselves is a question with no links in it. */
        node_rows_route_action(&rows, node, trace);
        /* The one row that answers "who is this?" for a node that joined after the NodeDB
           replay and has been sitting in the list as a bare id ever since. */
        rows_action(&rows, MESH_STR_NODE_ACT_REQUEST_INFO, mesh_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_REQUEST_INFO);
        /* The same shape, for the two readings that otherwise arrive on the node's own
           schedule. They sit next to "Ask for its name" because they are the same question -
           tell me what you have now - and because the answer to all three lands in the groups
           further down this screen rather than anywhere else. */
        rows_action(&rows, MESH_STR_NODE_ACT_REQUEST_POSITION, mesh_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_REQUEST_POSITION);
        rows_action(&rows, MESH_STR_NODE_ACT_REQUEST_TELEM, mesh_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_REQUEST_TELEMETRY);
        /* Muting is the gentle one of the three below: the node's traffic still arrives and
           still shows in its conversation, the radio just stops announcing it. The wire verb
           is a toggle rather than a set, so this row states the flag and flips it. */
        rows_toggle(&rows, MESH_STR_NODE_ACT_MUTE, node->is_muted, MESH_UI_NODE_ACTION_MUTE);
        /* Then, stated as what the radio will do rather than as a preference: an ignored
           node's packets are dropped before they reach us. */
        rows_toggle(&rows, MESH_STR_NODE_ACT_IGNORE, node->is_ignored, MESH_UI_NODE_ACTION_IGNORE);
        /* Last, because it is the only row here that takes its own row away with it: the node
           leaves the list and there is nothing left to press to undo it. It comes back on its
           own when the node next transmits, which is why this is an arming press rather than
           the confirm overlay - the cost is a wait, not a loss. */
        rows_action(
            &rows, MESH_STR_NODE_ACT_REMOVE,
            mesh_str(remove_armed ? MESH_STR_NODE_ACT_REMOVE_ARMED : MESH_STR_COMMON_PRESS_A),
            MESH_UI_NODE_ACTION_REMOVE);
    }
    /*
     * Outside the block above, because this is the one action our own node has a use for too:
     * a Brick has no GPS, so "where my radio says it is" and "where that node says it is" are
     * the same kind of answer and the only two a waypoint can be made from. The row appears
     * only when there is a fix to make one at - offering it against no coordinates would be
     * offering a place that is nowhere.
     */
    if (node->position.valid) {
        /* Our own node reaches here having emitted none of the block above, so the group's
           heading has not been written yet and these two rows would open the screen ungrouped -
           which is the state the heading was added to remove. Asking where the group started
           rather than re-testing `is_self` keeps the two conditions from drifting: what decides
           is whether anything is under the heading, which is what a heading is about. */
        if (rows.count == actions_at) {
            rows_heading(&rows, MESH_STR_NODE_HEAD_ACTIONS, MESH_UI_ICON_ACTIONS);
        }
        /* Looking at it, and keeping it: the two things a fix is good for, and both gated on
           there being one. A "show on map" row over a node with no position would open a map
           aimed at nowhere. */
        rows_action(&rows, MESH_STR_NODE_ACT_SHOW_ON_MAP, mesh_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_SHOW_ON_MAP);
        rows_action(&rows, MESH_STR_NODE_ACT_WAYPOINT, mesh_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_WAYPOINT);
    }
    /*
     * The traced route, after every verb rather than beside the one that starts it.
     *
     * It reads as the first of the report groups, which is what it is - a measurement, like the
     * readings under it, rather than something to press. Beside its verb it was a group in the
     * middle of the action block, and the rows after it went on being actions under a "Route
     * back" heading: harmless-looking in a flat list and a card whose heading lies about its
     * contents once the groups are drawn as cards. Every group on this screen is now one
     * unbroken run.
     *
     * Still gated on `is_self` with the block above: the verb is not offered for our own node,
     * so a trace targeting it is a trace nothing here could have started.
     */
    /*
     * The traced route, after every verb rather than beside the one that starts it.
     *
     * It reads as the first of the report groups, which is what it is - a measurement, like the
     * readings under it, rather than something to press. Beside its verb it was a group in the
     * middle of the action block, and the rows after it went on being actions under a "Route
     * back" heading: harmless-looking in a flat list and a card whose heading lies about its
     * contents once the groups are drawn as cards. Every group on this screen is now one
     * unbroken run, which is what node_detail_groups_are_unbroken_runs pins.
     *
     * Still gated on `is_self` with the action block: the verb is not offered for our own node,
     * so a trace targeting it is a trace nothing here could have started.
     */
    if (!is_self) {
        node_rows_route_path(&rows, node, trace, now);
    }
    node_rows_identity(&rows, node);
    node_rows_signal(&rows, node, is_self, now);
    node_rows_power(&rows, node, now);
    node_rows_position(&rows, node, now);
    node_rows_environment(&rows, node, now);
    node_rows_power_metrics(&rows, node, now);
    node_rows_air_quality(&rows, node, now);
    node_rows_health(&rows, node, now);
    node_rows_host(&rows, node, now);
    node_rows_neighbors(&rows, node, roster, now);

    return rows.count;
}

enum mesh_ui_history_reading
mesh_ui_node_detail_trend_at(const struct mesh_ui_node_summary *node, bool is_self,
                             const struct mesh_ui_traceroute *trace,
                             const struct mesh_ui_handshake_state *roster,
                             const struct mesh_ui_history *history, uint32_t row) {
    if (node == NULL || history == NULL || row >= MESH_UI_NODE_ITEMS_MAX) {
        return MESH_UI_HISTORY_NONE;
    }
    /*
     * The whole list, to read one row of it.
     *
     * Which rows exist depends on what the node has reported, so there is no arithmetic that
     * gets from a row number to a reading without building - and the two callers that ask this
     * are already in a build's company: the renderer has just drawn the list, and the nav is
     * answering a press on it. Doing it again here costs a hundred and twenty rows of stack and
     * buys the guarantee that the row the cursor is on is the row this answers for.
     *
     * `remove_armed` is false because it changes one row's value text and no row's existence,
     * which is the only thing that could move a reading to a different index.
     */
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(node, is_self, 0U, trace, false, roster,
                                                     history, items, MESH_UI_NODE_ITEMS_MAX);
    if (row >= count) {
        return MESH_UI_HISTORY_NONE;
    }
    return (enum mesh_ui_history_reading)items[row].trend_reading;
}

bool mesh_ui_node_detail_trend_row(const struct mesh_ui_node_summary *node, bool is_self,
                                   const struct mesh_ui_traceroute *trace,
                                   const struct mesh_ui_handshake_state *roster,
                                   const struct mesh_ui_history *history,
                                   enum mesh_ui_history_reading reading,
                                   struct mesh_ui_node_item *out) {
    if (node == NULL || history == NULL || reading == MESH_UI_HISTORY_NONE) {
        return false;
    }
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(node, is_self, 0U, trace, false, roster,
                                                     history, items, MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].trend == NULL || items[i].trend_reading != (uint8_t)reading) {
            continue;
        }
        if (out != NULL) {
            *out = items[i];
        }
        return true;
    }
    return false;
}

uint32_t mesh_ui_node_detail_count(const struct mesh_ui_node_summary *node, bool is_self,
                                   const struct mesh_ui_traceroute *trace,
                                   const struct mesh_ui_handshake_state *roster) {
    return mesh_ui_node_detail_build(node, is_self, 0U, trace, false, roster, NULL, NULL, 0U);
}

static uint32_t node_list_count(const struct mesh_ui_handshake_state *handshake) {
    return handshake->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES
                                                               : handshake->node_count;
}

bool mesh_ui_node_signal_heard(const struct mesh_ui_node_summary *node) {
    if (node == NULL || node->via_mqtt) {
        return false;
    }
    /* Unknown is not zero: `hops_away` is only meaningful once the firmware has said so. */
    if (!node->has_hops_away || node->hops_away > 0U) {
        return false;
    }
    /* And the session layer's own test for a reading that exists at all. */
    return node->snr != 0.0f;
}

const struct mesh_ui_node_summary *
mesh_ui_node_detail_find(const struct mesh_ui_handshake_state *handshake, uint32_t node_id) {
    if (handshake == NULL || node_id == 0U) {
        return NULL;
    }
    const uint32_t count = node_list_count(handshake);
    for (uint32_t i = 0; i < count; ++i) {
        if (handshake->nodes[i].node_id == node_id) {
            return &handshake->nodes[i];
        }
    }
    return NULL;
}

const struct mesh_ui_node_summary *
mesh_ui_node_detail_at(const struct mesh_ui_handshake_state *handshake, uint32_t row) {
    if (handshake == NULL || row >= node_list_count(handshake)) {
        return NULL;
    }
    return &handshake->nodes[row];
}
