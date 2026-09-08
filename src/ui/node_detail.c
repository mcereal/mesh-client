#include "mesh/ui/node_detail.h"

#include "mesh/i18n/strings.h"
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
    /* This node's battery trend, resolved once by the build rather than looked up per row.
       NULL when nothing has been watched, which is every caller that passes no history and
       every node the client has not heard a second reading from. */
    const struct mesh_ui_series *battery_trend;
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

static void rows_heading(struct node_rows *rows, enum mesh_str_id label) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    snprintf(item->label, sizeof item->label, "%s", mesh_str(label));
    item->kind = MESH_UI_NODE_ROW_HEADING;
}

static void rows_action(struct node_rows *rows, enum mesh_str_id label, const char *value,
                        enum mesh_ui_node_action action) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    snprintf(item->label, sizeof item->label, "%s", mesh_str(label));
    if (value != NULL) {
        snprintf(item->value, sizeof item->value, "%s", value);
    }
    item->kind = MESH_UI_NODE_ROW_ACTION;
    item->action = (uint8_t)action;
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
static void rows_trend(struct node_rows *rows, const struct mesh_ui_series *series) {
    if (series == NULL || rows->items == NULL || rows->count == 0U ||
        rows->count > rows->capacity) {
        return;
    }
    struct mesh_ui_node_item *item = &rows->items[rows->count - 1U];
    if (item->kind != MESH_UI_NODE_ROW_METER) {
        return;
    }
    item->trend = series;
}

/* "4m", "3h", "2d" - the same shorthand the Nodes list uses, so the two agree. An unset or
   future stamp reads as "?" rather than a wrapped enormous age. */
static void format_age(uint32_t stamp, uint32_t now, char *out, size_t out_len) {
    if (stamp == 0U || now == 0U || stamp > now) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        return;
    }
    const uint32_t seconds = now - stamp;
    if (seconds < 60U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_SECONDS, seconds);
    } else if (seconds < 3600U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_MINUTES, seconds / 60U);
    } else if (seconds < 86400U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_HOURS, seconds / 3600U);
    } else {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_DAYS, seconds / 86400U);
    }
}

static void format_uptime(uint32_t seconds, char *out, size_t out_len) {
    if (seconds >= 86400U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_DAYS_HOURS, seconds / 86400U,
                        (seconds % 86400U) / 3600U);
    } else if (seconds >= 3600U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_HOURS_MINUTES, seconds / 3600U,
                        (seconds % 3600U) / 60U);
    } else {
        mesh_str_format(out, out_len, MESH_STR_TIME_MINUTES_SHORT, seconds / 60U);
    }
}

static void node_rows_identity(struct node_rows *rows, const struct mesh_ui_node_summary *node) {
    rows_heading(rows, MESH_STR_NODE_HEAD_IDENTITY);

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
    rows_heading(rows, MESH_STR_NODE_HEAD_SIGNAL);

    char age[24];
    format_age(node->last_heard, now, age, sizeof age);
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
                format_age(node->rssi_time, now, measured, sizeof measured);
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
    rows_heading(rows, MESH_STR_NODE_HEAD_METRICS);

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
            rows_trend(rows, rows->battery_trend);
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
        format_uptime(metrics->uptime_seconds, uptime, sizeof uptime);
        rows_text(rows, MESH_STR_NODE_UPTIME, uptime);
    }
    char age[24];
    format_age(metrics->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_position(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                               uint32_t now) {
    const struct mesh_ui_node_position *position = &node->position;
    if (!position->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_POSITION);

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
        format_age(position->time, now, age, sizeof age);
        rows_text(rows, MESH_STR_NODE_FIX, age);
    } else {
        format_age(position->received, now, age, sizeof age);
        rows_text(rows, MESH_STR_NODE_FIX_RECEIVED, age);
    }
}

static void node_rows_environment(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                  uint32_t now) {
    const struct mesh_ui_node_environment *env = &node->environment;
    if (!env->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_ENVIRONMENT);

    if (env->has_temperature) {
        rows_info(rows, MESH_STR_NODE_TEMPERATURE, MESH_STR_NODE_VAL_TEMPERATURE,
                  (double)env->temperature, (double)env->temperature * 9.0 / 5.0 + 32.0);
    }
    if (env->has_humidity) {
        rows_info(rows, MESH_STR_NODE_HUMIDITY, MESH_STR_NODE_VAL_PERCENT_FINE,
                  (double)env->relative_humidity);
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
    format_age(env->time, now, age, sizeof age);
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
    rows_heading(rows, MESH_STR_NODE_HEAD_POWER);
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
    format_age(power->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_air_quality(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                  uint32_t now) {
    const struct mesh_ui_node_air_quality *air = &node->air_quality;
    if (!air->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_AIR_QUALITY);
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
    format_age(air->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_health(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                             uint32_t now) {
    const struct mesh_ui_node_health *health = &node->health;
    if (!health->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_HEALTH);
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
    format_age(health->time, now, age, sizeof age);
    rows_text(rows, MESH_STR_NODE_REPORTED, age);
}

static void node_rows_host(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                           uint32_t now) {
    const struct mesh_ui_node_host *host = &node->host;
    if (!host->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_HOST);
    if (host->has_uptime) {
        char uptime[32];
        format_uptime(host->uptime_seconds, uptime, sizeof uptime);
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
    format_age(host->time, now, age, sizeof age);
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
        rows_heading(rows, MESH_STR_NODE_HEAD_NEIGHBOURS);
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
        format_age(heard->time, now, age, sizeof age);
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
                rows_heading(rows, MESH_STR_NODE_HEAD_HEARD_BY);
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
 * The traced route, if the one trace slot is holding this node's. Two paths of stops, each
 * row a node and the SNR of the link that reached it - the first stop of a path is the sender
 * and has no incoming link, so it carries no reading rather than a zero.
 *
 * The action row is emitted whatever the state, because it is also how a trace is started and
 * re-run; the path rows only when there is a path. A trace of some *other* node shows nothing
 * here beyond a plain "press A", so opening a second node never appears to describe it with
 * the first one's route.
 */
static void node_rows_route(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                            const struct mesh_ui_traceroute *trace, uint32_t now) {
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

    if (!ours || trace->state != MESH_TRACEROUTE_DONE) {
        return;
    }

    for (unsigned direction = 0; direction < 2U; ++direction) {
        const struct mesh_ui_traceroute_hop *path = direction == 0U ? trace->forward : trace->back;
        const uint8_t count = direction == 0U ? trace->forward_count : trace->back_count;
        if (count == 0U) {
            continue;
        }
        rows_heading(rows, direction == 0U ? MESH_STR_NODE_HEAD_ROUTE_OUT
                                           : MESH_STR_NODE_HEAD_ROUTE_BACK);
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
    format_age(trace->completed, now, age, sizeof age);
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
        .battery_trend = mesh_ui_history_battery(history, node->node_id),
    };

    /* The actions lead: opening a node from the Nodes tab used to go straight to its
       conversation, so the first thing under the cursor still gets you there. */
    if (!is_self) {
        rows_action(&rows, MESH_STR_NODE_ACT_MESSAGE, NULL, MESH_UI_NODE_ACTION_MESSAGE);
        /* Pinning our own node would be meaningless - it already ranks above everything. */
        rows_action(&rows, MESH_STR_NODE_ACT_PIN,
                    mesh_str(node->is_favorite ? MESH_STR_COMMON_YES : MESH_STR_COMMON_NO),
                    MESH_UI_NODE_ACTION_FAVORITE);
        /* Tracing the route to ourselves is a question with no links in it. */
        node_rows_route(&rows, node, trace, now);
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
        rows_action(&rows, MESH_STR_NODE_ACT_MUTE,
                    mesh_str(node->is_muted ? MESH_STR_COMMON_YES : MESH_STR_COMMON_NO),
                    MESH_UI_NODE_ACTION_MUTE);
        /* Then, stated as what the radio will do rather than as a preference: an ignored
           node's packets are dropped before they reach us. */
        rows_action(&rows, MESH_STR_NODE_ACT_IGNORE,
                    mesh_str(node->is_ignored ? MESH_STR_COMMON_YES : MESH_STR_COMMON_NO),
                    MESH_UI_NODE_ACTION_IGNORE);
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
        rows_action(&rows, MESH_STR_NODE_ACT_WAYPOINT, mesh_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_WAYPOINT);
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
