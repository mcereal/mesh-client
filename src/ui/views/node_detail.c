#include "mesh/ui/node_detail.h"

#include "inkwell/base/text.h"

#include "mesh/geo/coords.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/duration.h"
#include "mesh/ui/nodes.h"

/* session.h for the traceroute state enum: the UI struct carries it as a byte so store.h
   stays plain, but this file already pulls nanopb in through radio_settings.h, so naming the
   real enum here beats keeping a second copy of it in step. */
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/trust.h"
#include "mesh/ui/units.h"

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
    /* The radio's display units, for the two rows that are lengths: a node's altitude and the
       footprint its precision_bits describe. Carried on the row builder rather than read from a
       store, because this file takes a node and a roster and never sees the settings. */
    bool imperial;
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
static void rows_heading(struct node_rows *rows, inkcell_str_id label, enum inkcell_icon icon) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    snprintf(item->label, sizeof item->label, "%s", inkcell_str(label));
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
static const enum inkcell_icon k_action_icons[] = {
    [MESH_UI_NODE_ACTION_NONE] = INKCELL_ICON_NONE,
    /* The group's own symbol, which is what the heading over these verbs used to carry: the row
       that opens them is the card they were on, so it wears what named them. */
    [MESH_UI_NODE_ACTION_OPEN_ACTIONS] = INKCELL_ICON_ACTIONS,
    [MESH_UI_NODE_ACTION_MESSAGE] = INKCELL_ICON_MESSAGES,
    [MESH_UI_NODE_ACTION_FAVORITE] = INKCELL_ICON_PINNED,
    /* A traced route is the chain of links that reaches the node, which is what LINK says on
       the Status card's transport row. */
    [MESH_UI_NODE_ACTION_TRACEROUTE] = INKCELL_ICON_LINK,
    [MESH_UI_NODE_ACTION_REQUEST_INFO] = INKCELL_ICON_USER,
    [MESH_UI_NODE_ACTION_REQUEST_POSITION] = INKCELL_ICON_POSITION,
    [MESH_UI_NODE_ACTION_REQUEST_TELEMETRY] = INKCELL_ICON_TELEMETRY,
    /* The bell with a stroke through it, which is the mark the Messages tab already puts on a
       muted conversation - one mute, one symbol, whichever screen turns it on. */
    [MESH_UI_NODE_ACTION_MUTE] = INKCELL_ICON_MUTED,
    /* Ignoring is the harder one and gets the harder rune: a mute still lets the traffic
       arrive, an ignore has the radio drop it before we ever see it. */
    [MESH_UI_NODE_ACTION_IGNORE] = INKCELL_ICON_CLOSE,
    [MESH_UI_NODE_ACTION_REMOVE] = INKCELL_ICON_DELETE,
    [MESH_UI_NODE_ACTION_WAYPOINT] = INKCELL_ICON_POSITION,
    [MESH_UI_NODE_ACTION_SHOW_ON_MAP] = INKCELL_ICON_MAP,
    /* The shield the verified state is drawn with, on the row that gets you there - so the
       verb and the state it produces are the same mark. */
    [MESH_UI_NODE_ACTION_VERIFY_KEY] = INKCELL_ICON_SECURITY,
    /* The radio, because that is what the row is about: our list already has this node and the
       radio's does not. */
    [MESH_UI_NODE_ACTION_ADD_CONTACT] = INKCELL_ICON_RADIO,
    /* The Settings tab's own mark, because that is where the press lands and what it changes is
       which radio that tab is about. The radio rune next door means "this node's entry in our
       radio's database", which is a different sentence. */
    [MESH_UI_NODE_ACTION_ADMIN] = INKCELL_ICON_SETTINGS,
};

/*
 * Two of these eleven rows cost something, and the tone is how the row says so.
 *
 * Every verb used to name the primary, which said the true thing the wrong way round: eleven
 * rows in the accent is not eleven emphases, it is a card with no emphasis in it at all, and
 * the one row that deletes something had to shout over ten rows already shouting. The accent
 * did not go away - it moved to the leading disc, where a colour marks *what the row is about*
 * without competing with the words (see INKCELL_FB_LEADING_TONAL) - and the ink went back to saying
 * only what it can say once: this row is not like the others.
 *
 * So an ordinary errand is the ordinary ink. Ignoring a node is the radio dropping its packets -
 * recoverable, and a surprise if it was not meant - so it takes the warning family; removing it
 * takes the node's own row away, which is the error family and the same ink the confirm dialog
 * uses. Both are now the only coloured words on the card, which is what a colour on a control
 * is for.
 *
 * Stated here rather than at each call site so the arming press, the row's ink, the disc at its
 * leading edge and the action bar's "confirm remove" cannot come from four different opinions
 * about which row is which.
 */
static const enum inkcell_tone k_action_tones[] = {
    [MESH_UI_NODE_ACTION_NONE] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_MESSAGE] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_FAVORITE] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_TRACEROUTE] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_REQUEST_INFO] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_REQUEST_POSITION] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_REQUEST_TELEMETRY] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_IGNORE] = INKCELL_TONE_WARNING,
    [MESH_UI_NODE_ACTION_MUTE] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_REMOVE] = INKCELL_TONE_ERROR,
    [MESH_UI_NODE_ACTION_WAYPOINT] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_SHOW_ON_MAP] = INKCELL_TONE_NORMAL,
    /* Ordinary verbs, both of them. Neither costs anything that cannot be done again, and a
       warning colour on the row that establishes trust would be saying the opposite of what
       the row is for. */
    [MESH_UI_NODE_ACTION_VERIFY_KEY] = INKCELL_TONE_NORMAL,
    [MESH_UI_NODE_ACTION_ADD_CONTACT] = INKCELL_TONE_NORMAL,
    /*
     * The warning family, and the only row on this card that earns one without taking anything
     * away.
     *
     * What it costs is not this node - it is every press on the Settings tab afterwards, which
     * stops meaning the radio in your hand. The two rows above establish trust and the two
     * coloured ones below spend it; this one moves where the client is pointed, and a reader
     * who presses it by accident would not find out from any row on this screen. The banner
     * says so from then on (mesh/ui/chrome.h); the colour is what says it first.
     */
    [MESH_UI_NODE_ACTION_ADMIN] = INKCELL_TONE_WARNING,
};

static enum inkcell_icon action_icon(enum mesh_ui_node_action action) {
    return (size_t)action < sizeof k_action_icons / sizeof k_action_icons[0]
               ? k_action_icons[action]
               : INKCELL_ICON_NONE;
}

static enum inkcell_tone action_tone(enum mesh_ui_node_action action) {
    return (size_t)action < sizeof k_action_tones / sizeof k_action_tones[0]
               ? k_action_tones[action]
               : INKCELL_TONE_NORMAL;
}

static struct mesh_ui_node_item *rows_action(struct node_rows *rows, inkcell_str_id label,
                                             const char *value, enum mesh_ui_node_action action) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return NULL;
    }
    snprintf(item->label, sizeof item->label, "%s", inkcell_str(label));
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
 * inkcell_fb_draw_switch() was added to stop on the settings rows. The words stay, for the backend
 * with no sprites; what is new is that the state is also a field, so a screen that can draw the
 * control draws it from the same flag this read.
 */
static void rows_toggle(struct node_rows *rows, inkcell_str_id label, bool on,
                        enum mesh_ui_node_action action) {
    struct mesh_ui_node_item *item = rows_action(
        rows, label, inkcell_str(on ? MESH_STR_COMMON_YES : MESH_STR_COMMON_NO), action);
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

static void rows_text(struct node_rows *rows, inkcell_str_id label, const char *value) {
    struct mesh_ui_node_item *item = rows_info_row(rows, inkcell_str(label));
    if (item != NULL) {
        snprintf(item->value, sizeof item->value, "%s", value != NULL ? value : "");
    }
}

static void rows_info(struct node_rows *rows, inkcell_str_id label, inkcell_str_id format, ...) {
    struct mesh_ui_node_item *item = rows_info_row(rows, inkcell_str(label));
    if (item == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)inkcell_str_vformat(item->value, sizeof item->value, format, args);
    va_end(args);
}

static void rows_named(struct node_rows *rows, const char *label, inkcell_str_id format, ...) {
    struct mesh_ui_node_item *item = rows_info_row(rows, label);
    if (item == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)inkcell_str_vformat(item->value, sizeof item->value, format, args);
    va_end(args);
}

/*
 * The fourth: a fact whose value is a *state* rather than a reading, a name or an identifier.
 *
 * rows_text() above states something the node reported - a name, a figure, an age - and the
 * reader takes it at face value. These are the rows the reader is *checking*: whether the key
 * is verified, whether the packets crossed the air or came over somebody's MQTT bridge, whether
 * the radio still carries this node at all. Each is one of a handful of answers, each answer
 * means something, and a screen that can draw a shape draws them as bubbles - see
 * mesh_ui_node_item.chip.
 *
 * The tone is the answer's meaning and the only thing the call site decides: the chip's colour
 * follows from it, so a row cannot end up green and "not verified". The neutral tone is the
 * honest answer for a state that is simply the usual one, and most of these take it.
 *
 * The bar for using this rather than rows_text() is deliberately high, and it is the one
 * inkcell_fb_draw_badge() states: a card where every row is a bubble is a column of colour
 * reporting nothing. A measurement is never a state - there is no "6.75 dB" to be in - and neither
 * is anything the node chose for itself, which is what its names and its hardware are.
 */
static void rows_state(struct node_rows *rows, inkcell_str_id label, const char *value,
                       enum inkcell_tone tone) {
    struct mesh_ui_node_item *item = rows_info_row(rows, inkcell_str(label));
    if (item == NULL) {
        return;
    }
    snprintf(item->value, sizeof item->value, "%s", value != NULL ? value : "");
    item->tone = (uint8_t)tone;
    item->chip = true;
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
static const struct inkcell_scale node_battery_scale = {0, 100};
static const struct inkcell_band node_battery_band = {.warn = INKCELL_BATTERY_LOW,
                                                      .bad = INKCELL_BATTERY_CRITICAL};
/* A zeroed scale is the identity domain: these readings are already permille. */
static const struct inkcell_scale node_permille_scale = {0, 0};
static const struct inkcell_band node_channel_util_band = {.warn = INKCELL_AIRTIME_BUSY_WARN,
                                                           .bad = INKCELL_AIRTIME_BUSY_BAD};
static const struct inkcell_band node_air_tx_band = {.warn = INKCELL_AIRTIME_TX_WARN,
                                                     .bad = INKCELL_AIRTIME_TX_BAD};
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
static const struct inkcell_scale node_temperature_scale = {INKCELL_TEMPERATURE_FLOOR,
                                                            INKCELL_TEMPERATURE_CEILING};
static const struct inkcell_band node_temperature_band = {.warn = INKCELL_TEMPERATURE_WARM,
                                                          .bad = INKCELL_TEMPERATURE_HOT};
static const struct inkcell_band node_humidity_band = {.warn = INKCELL_HUMIDITY_DAMP,
                                                       .bad = INKCELL_HUMIDITY_WET};
/*
 * The three air readings whose ends somebody else published - see the note beside them in
 * layout.h for why these get a band where a temperature's is about the node instead.
 */
static const struct inkcell_scale node_iaq_scale = {INKCELL_IAQ_FLOOR, INKCELL_IAQ_CEILING};
static const struct inkcell_band node_iaq_band = {.warn = INKCELL_IAQ_POLLUTED,
                                                  .bad = INKCELL_IAQ_HEAVY};
static const struct inkcell_scale node_co2_scale = {INKCELL_CO2_FLOOR, INKCELL_CO2_CEILING};
static const struct inkcell_band node_co2_band = {.warn = INKCELL_CO2_STUFFY,
                                                  .bad = INKCELL_CO2_BAD};
static const struct inkcell_scale node_pm25_scale = {INKCELL_PM25_FLOOR, INKCELL_PM25_CEILING};
static const struct inkcell_band node_pm25_band = {.warn = INKCELL_PM25_ELEVATED,
                                                   .bad = INKCELL_PM25_UNHEALTHY};
static const struct inkcell_scale node_snr_scale = {INKCELL_SNR_FLOOR, INKCELL_SNR_CEILING};
static const struct inkcell_band node_snr_band = {.warn = INKCELL_SNR_FAIR,
                                                  .bad = INKCELL_SNR_POOR};
/* Received strength's own ends and its own thresholds, which are deliberately not the ratio's
   scaled - see INKCELL_RSSI_FAIR for why the two readings are banded on different questions. */
static const struct inkcell_scale node_rssi_scale = {INKCELL_RSSI_FLOOR, INKCELL_RSSI_CEILING};
static const struct inkcell_band node_rssi_band = {.warn = INKCELL_RSSI_FAIR,
                                                   .bad = INKCELL_RSSI_POOR};

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
static void rows_gauge(struct node_rows *rows, int32_t value, struct inkcell_scale scale,
                       const struct inkcell_band *band) {
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
    const struct inkcell_series *series =
        mesh_ui_history_series(rows->history, rows->node_id, reading);
    if (series == NULL || !inkcell_series_has_segment(series)) {
        return;
    }
    struct mesh_ui_node_item *item = &rows->items[rows->count - 1U];
    if (item->kind != MESH_UI_NODE_ROW_METER) {
        return;
    }
    item->trend = series;
    item->trend_reading = (uint8_t)reading;
}

/*
 * A key's base64 cut to its two ends, the way a fingerprint is usually shown. Whole, a 32-byte
 * key is 44 characters, which runs off the value column at the Brick's scale and was clipped
 * mid-character with nothing to say it had been - a reader comparing it with a phone saw a key
 * that was simply different. Both ends are what a person compares by eye; the key itself is
 * never typed back from here, and the verification ceremony has a row of its own below.
 */
#define NODE_KEY_END_CHARS 8U

static void node_key_fingerprint(char *key) {
    static const char k_ellipsis[] = "\xE2\x80\xA6"; /* U+2026, one glyph */
    const size_t len = strlen(key);
    const size_t cut = sizeof k_ellipsis - 1U;
    if (len <= 2U * NODE_KEY_END_CHARS + cut) {
        return;
    }
    memcpy(key + NODE_KEY_END_CHARS, k_ellipsis, cut);
    memmove(key + NODE_KEY_END_CHARS + cut, key + len - NODE_KEY_END_CHARS,
            NODE_KEY_END_CHARS + 1U);
}

static void node_rows_identity(struct node_rows *rows, const struct mesh_ui_node_summary *node) {
    rows_heading(rows, MESH_STR_NODE_HEAD_IDENTITY, INKCELL_ICON_USER);

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
        rows_state(rows, MESH_STR_NODE_NAME, inkcell_str(MESH_STR_NODE_DERIVED_NAME),
                   INKCELL_TONE_NORMAL);
    }
    if (!node->in_nodedb) {
        /* The warning tone rather than the neutral one, because this row only exists when the
           answer is the bad one: a node the radio has evicted is one a direct message has no
           stored key to travel with, and the row is here to be noticed. */
        rows_state(rows, MESH_STR_NODE_NODEDB, inkcell_str(MESH_STR_NODE_NOT_IN_NODEDB),
                   INKCELL_TONE_WARNING);
    }

    if (node->role != 0U || node->hw_model != 0U) {
        /* What the node is *for* - client, router, repeater - which is a role out of a fixed
           set and the one thing on this card that changes how every other card should be read.
           The hardware beside it is a model name the node chose, so it stays words. */
        rows_state(rows, MESH_STR_NODE_ROLE, mesh_radio_role_name(node->role), INKCELL_TONE_NORMAL);
    }
    if (node->hw_model != 0U) {
        char fallback[MESH_UI_NODE_VALUE_MAX];
        rows_text(rows, MESH_STR_NODE_HARDWARE,
                  mesh_radio_hw_model_name(node->hw_model, fallback, sizeof fallback));
    }
    if (node->public_key_len > 0U) {
        char key[MESH_UI_NODE_VALUE_MAX];
        mesh_ui_settings_key_text(node->public_key, node->public_key_len, key, sizeof key);
        node_key_fingerprint(key);
        rows_text(rows, MESH_STR_NODE_PUBLIC_KEY, key);
    }
    /*
     * And what that key is worth, which the fingerprint above cannot say and which is the whole
     * of what the padlock in the transcript is claiming.
     *
     * Always listed, including for a node we hold no key for - that is the case it answers
     * best. "No key held" is why a direct message to this node goes out under the channel key
     * instead, and a row that vanished exactly when the answer was most useful would leave a
     * reader hunting for a setting that does not exist. The same rule the MQTT proxy row set:
     * a fact worth knowing is worth a row even when nothing about it can be pressed here.
     */
    const enum mesh_ui_key_trust trust = mesh_ui_key_trust_of(node);
    struct mesh_ui_node_item *const trust_row =
        rows_info_row(rows, inkcell_str(MESH_STR_NODE_KEY_TRUST));
    if (trust_row != NULL) {
        snprintf(trust_row->value, sizeof trust_row->value, "%s",
                 inkcell_str(mesh_ui_key_trust_label(trust)));
        /* The tone and not the mark. This card has no icon column - every fact in it is a
           label and a value - so a leading icon here would start one row's words in a column of
           their own. The mark belongs where there is no room for the words: the padlock on a
           bubble, and the row that opens the ceremony.
           What the tone does get is a shape around it. This is the row the whole card is
           qualified by, its three answers are a closed set, and until it was a bubble the
           difference between "verified" and "not verified" was one of two inks on two words of
           the same size - which is a claim made to whoever can tell those inks apart. */
        trust_row->tone = (uint8_t)mesh_ui_key_trust_tone(trust);
        trust_row->chip = true;
    }

    /* One row for the handful of booleans, so a plain node does not carry four "no" rows. */
    char flags[MESH_UI_NODE_VALUE_MAX];
    flags[0] = '\0';
    const char *set[3];
    size_t set_count = 0U;
    if (node->is_ignored) {
        set[set_count++] = inkcell_str(MESH_STR_NODE_FLAG_IGNORED);
    }
    if (node->is_licensed) {
        set[set_count++] = inkcell_str(MESH_STR_NODE_FLAG_LICENSED);
    }
    if (node->is_unmessagable) {
        set[set_count++] = inkcell_str(MESH_STR_NODE_FLAG_UNMESSAGEABLE);
    }
    for (size_t i = 0; i < set_count; ++i) {
        const size_t used = strlen(flags);
        snprintf(flags + used, sizeof flags - used, "%s%s",
                 i > 0U ? inkcell_str(MESH_STR_NODE_FLAG_SEPARATOR) : "", set[i]);
    }
    if (flags[0] != '\0') {
        /* Every flag in this set is something being withheld - the radio dropping the node's
           packets, a node that cannot be written to - so the row warns whenever it is there at
           all. Licensed alone is the exception and is merely a fact about the operator. */
        rows_state(rows, MESH_STR_NODE_FLAGS, flags,
                   (node->is_ignored || node->is_unmessagable) ? INKCELL_TONE_WARNING
                                                               : INKCELL_TONE_NORMAL);
    }
}

/*
 * A relay or next-hop byte, resolved against the roster the screen already has.
 *
 * The same job node_rows_neighbor_name() does for a whole node number, and it is a separate
 * function rather than a parameter on that one because the ambiguity is different in kind: a
 * node number identifies a node, and a last byte identifies 1 in 256 of them. So an exact match
 * has to be the *only* match to be a name at all - a mesh of a hundred nodes collides here by
 * arithmetic - and everything else falls back to the "!..a3" partial id, which is the honest
 * rendering of what the LoRa header actually had room to carry.
 *
 * Resolved here rather than joined in at publish, unlike a message's relay: this screen is
 * handed the roster anyway (it is what "Heard by" is read across), and resolving live means a
 * relay that was two hex digits at connect time becomes a name the moment its NodeInfo lands.
 *
 * `ambiguous` is the one part it cannot work out for itself, and it is not an optimisation.
 * This roster is the ranked MESH_UI_MAX_HANDSHAKE_NODES of a session that holds twice as many,
 * so on a big mesh a byte can have exactly one claimant *here* and another one that was ranked
 * away - and a scan of what was published would then name a node and sound certain about it.
 * The flag is settled at publish over the whole roster; see mesh_app_relay_byte_is_ambiguous().
 */
static void node_rows_relay_name(const struct mesh_ui_handshake_state *roster, uint8_t last_byte,
                                 bool ambiguous, uint32_t exclude, char *out, size_t out_len) {
    const struct mesh_ui_node_summary *match = NULL;
    if (roster != NULL && !ambiguous) {
        const uint32_t count = roster->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                                   ? MESH_UI_MAX_HANDSHAKE_NODES
                                   : roster->node_count;
        for (uint32_t i = 0; i < count; ++i) {
            if ((uint8_t)(roster->nodes[i].node_id & 0xFFU) != last_byte) {
                continue;
            }
            if (exclude != 0U && roster->nodes[i].node_id == exclude) {
                continue; /* it cannot have relayed a packet it is the far end of */
            }
            if (match != NULL) {
                match = NULL; /* a second candidate, so the byte names neither */
                break;
            }
            match = &roster->nodes[i];
        }
    }
    if (match != NULL) {
        const char *name = match->short_name[0] != '\0' ? match->short_name : match->long_name;
        if (name[0] != '\0') {
            inkwell_str_copy(out, out_len, name);
            return;
        }
    }
    inkcell_str_format(out, out_len, MESH_STR_NODE_VAL_RELAY_HEX, (unsigned)last_byte);
}

/*
 * The two routing rows, off the last packet this node's traffic arrived in.
 *
 * They are a pair and they point in opposite directions, which is the whole reason both are
 * worth a row. "Relayed by" is who handed that packet to *us* - the last stop on the way here,
 * and so the first stop of anything going back. "Next hop" is who that packet asked to carry
 * it onward, which is the sender's own routing table talking rather than ours.
 *
 * Neither is a route and neither should be read as one: a traced route is further up this
 * screen and is the thing that answers that. These answer "did this come straight to me, and
 * is the mesh routing it or flooding it", which a hop count does not say and which is what
 * changes first when a repeater goes down.
 */
static void node_rows_route(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                            const struct mesh_ui_handshake_state *roster) {
    if (!node->has_route) {
        /* Nothing has been heard from this node this run - a roster entry the NodeDB replayed,
           or one restored from the cache. Silence rather than a row of zeroes, which would read
           as a flooded packet that never arrived. */
        return;
    }

    if (node->relay_node != 0U) {
        /*
         * A byte matching the node's own is the firmware saying nothing carried this: a node
         * stamps itself into relay_node as it transmits, so a packet heard straight from it
         * names it.
         *
         * Except when the hop count says otherwise. A packet that came at least one hop *was*
         * relayed, so the match is a collision with some other node ending in the same byte,
         * and "direct" there would be the one row on the screen contradicting the row above it.
         * `has_hops_away` unset is the firmware declining to say, which is not zero - the same
         * distinction the hop row itself is careful about - so it leaves the shortcut standing.
         */
        const bool relayed = node->has_hops_away && node->hops_away > 0U;
        const bool direct_said = node->has_hops_away && node->hops_away == 0U;
        if ((uint8_t)(node->node_id & 0xFFU) == node->relay_node && !relayed) {
            /* A state rather than a name, because "direct" is a fact about the path and the
               reader is scanning this column for node names.
               Unless the hop row above already said it: two rows reading "direct" in a row is
               the same fact twice, and "Relayed by: direct" is the clumsier of the two. The row
               stays when the firmware gave no hop count, because then it is the only one. */
            if (!direct_said) {
                rows_state(rows, MESH_STR_NODE_RELAYED_BY, inkcell_str(MESH_STR_NODE_RELAY_DIRECT),
                           INKCELL_TONE_SUCCESS);
            }
        } else {
            char relay[MESH_UI_NODE_VALUE_MAX];
            /* Struck off when we know the packet travelled: whatever carried it, it was not
               the node it came from, so naming that node here would contradict the hop row. */
            node_rows_relay_name(roster, node->relay_node, node->relay_ambiguous,
                                 relayed ? node->node_id : 0U, relay, sizeof relay);
            rows_text(rows, MESH_STR_NODE_RELAYED_BY, relay);
        }
    }

    if (node->next_hop == 0U) {
        /* Upstream's NO_NEXT_HOP_PREFERENCE: the packet went out to whoever would carry it
           rather than to a chosen relay. Tertiary, not a warning - flooding is how the mesh
           works until it has learnt a route, and how all of it worked before firmware 2.5. */
        rows_state(rows, MESH_STR_NODE_NEXT_HOP, inkcell_str(MESH_STR_NODE_NEXT_HOP_FLOOD),
                   INKCELL_TONE_TERTIARY);
    } else {
        char hop[MESH_UI_NODE_VALUE_MAX];
        node_rows_relay_name(roster, node->next_hop, node->next_hop_ambiguous, 0U, hop, sizeof hop);
        rows_text(rows, MESH_STR_NODE_NEXT_HOP, hop);
    }
}

static void node_rows_signal(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                             const struct mesh_ui_handshake_state *roster, bool is_self,
                             uint32_t now) {
    rows_heading(rows, MESH_STR_NODE_HEAD_SIGNAL, INKCELL_ICON_LORA);

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
        const bool heard = mesh_ui_node_signal_heard(node);
        if (heard) {
            rows_gauge(rows, inkcell_snr_db(node->snr), node_snr_scale, &node_snr_band);
            rows_trend(rows, MESH_UI_HISTORY_SNR);
        }
        /* Beside it rather than instead of it: SNR is how far above the noise the packet was
           and RSSI is how loud it was, and a link can be good on one and poor on the other. */
        if (node->has_rssi) {
            /* Only this radio can measure an RSSI, so a node now reaching us over MQTT keeps
               the reading from the last packet we heard ourselves. Saying when that was is what
               stops the row reading as a description of the packet that just arrived. */
            const bool aged = node->rssi_time != 0U && node->last_heard > node->rssi_time;
            if (aged) {
                char measured[24];
                mesh_ui_format_age(node->rssi_time, now, measured, sizeof measured);
                rows_info(rows, MESH_STR_NODE_RSSI, MESH_STR_NODE_VAL_RSSI_AGED, (int)node->rx_rssi,
                          measured);
            } else {
                rows_info(rows, MESH_STR_NODE_RSSI, MESH_STR_NODE_VAL_RSSI, (int)node->rx_rssi);
            }
            /*
             * And where that sits between a link about to stop arriving and a node on the same
             * bench, on the SNR row's terms exactly: the figure is printed either way and it is
             * only *drawn* when it describes this node's own last packet. A reading the row has
             * already had to date-stamp is one the bar beside it would be claiming as current.
             *
             * dBm has ends nobody has memorised, which is the whole test for a meter - see
             * MESH_UI_NODE_ROW_METER. A reader who knows -95 is worse than -70 still does not
             * know how much of the link is left at either.
             */
            if (heard && !aged) {
                rows_gauge(rows, (int32_t)node->rx_rssi, node_rssi_scale, &node_rssi_band);
                rows_trend(rows, MESH_UI_HISTORY_RSSI);
            }
        }
        if (node->has_hops_away && node->hops_away == 0U) {
            /* "0" is a count of nothing; the reader's question is whether it came straight
               here, and this is the yes. Words rather than a capsule, like the counts it stands
               in for - see node_detail_states_are_chips. */
            rows_text(rows, MESH_STR_NODE_HOPS_AWAY, inkcell_str(MESH_STR_NODE_RELAY_DIRECT));
        } else if (node->has_hops_away) {
            rows_info(rows, MESH_STR_NODE_HOPS_AWAY, MESH_STR_NODE_VAL_NUMBER,
                      (unsigned)node->hops_away);
        } else {
            rows_text(rows, MESH_STR_NODE_HOPS_AWAY, inkcell_str(MESH_STR_COMMON_UNKNOWN));
        }
        /* Beside the hop count rather than under its own heading: how far away a node is and
           which node stands between us are one question asked twice, and a heading between
           them would put a card boundary through the middle of it. */
        node_rows_route(rows, node, roster);
    }
    /* The slot by the name the conversation list gives it. As a bare index it read "0", which
       is the radio's numbering and not a thing anybody calls a channel. */
    char channel[MESH_UI_NODE_VALUE_MAX];
    mesh_ui_channel_name(roster, node->channel, channel, sizeof channel);
    rows_text(rows, MESH_STR_NODE_CHANNEL, channel);
    /*
     * And the qualifier on everything above it: whether this node reached us across the air or
     * through somebody's MQTT bridge. Two answers, and the second one quietly invalidates the
     * SNR, the RSSI and the hop count three rows up - which is exactly the kind of fact a
     * bubble is for and exactly the kind that disappears when it is set as a word.
     *
     * The tertiary family for MQTT: neither good nor bad, and not the thing the reader is
     * looking for - a packet that came in over the internet is still a packet.
     */
    rows_state(rows, MESH_STR_NODE_HEARD_VIA,
               inkcell_str(node->via_mqtt ? MESH_STR_NODE_VIA_MQTT : MESH_STR_NODE_VIA_RF),
               node->via_mqtt ? INKCELL_TONE_TERTIARY : INKCELL_TONE_NORMAL);
}

static void node_rows_power(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                            uint32_t now) {
    const struct mesh_ui_node_metrics *metrics = &node->metrics;
    if (!metrics->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_METRICS, INKCELL_ICON_TELEMETRY);

    if (metrics->has_battery) {
        /* 101 is upstream's "running off USB", not a 101% battery. */
        if (metrics->battery_level > 100U) {
            /* Not a reading at all - upstream's way of saying there is nothing to measure -
               so it is a state where every other battery row is a percentage with a bar. */
            rows_state(rows, MESH_STR_NODE_BATTERY, inkcell_str(MESH_STR_STATUS_BATTERY_USB),
                       INKCELL_TONE_SUCCESS);
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
        rows_gauge(rows, inkcell_percent_permille(metrics->channel_utilization),
                   node_permille_scale, &node_channel_util_band);
    }
    if (metrics->has_air_util_tx) {
        rows_info(rows, MESH_STR_NODE_AIR_UTIL_TX, MESH_STR_NODE_VAL_PERCENT_FINE,
                  (double)metrics->air_util_tx);
        /* Its own band, an order of magnitude below the one above: this is the radio's own
           transmit duty cycle rather than how busy the band is. */
        rows_gauge(rows, inkcell_percent_permille(metrics->air_util_tx), node_permille_scale,
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

/*
 * How many decimals of a degree the sender's rounding leaves worth printing.
 *
 * Fixed-point 1e-7 degrees on the wire, and five decimals is about a metre, which is finer than
 * anything a LoRa node reports - so five is the most this ever says, and what it says when the
 * sender stated no rounding (0) or none at all (32). A node that rounded its fix to ~360 m printed
 * "47.62050" over a Precision row saying "~360 m", which is the coordinate claiming a metre in
 * the one place the reader cannot see the row that takes it back.
 *
 * The footprint is worked out from the bit count rather than read from the settings table,
 * because the table is the ten values the radio's own setting offers and a sender may use any:
 * keeping `bits` of the 32-bit coordinate leaves steps of 2^(32 - bits) units of 1e-7 degrees,
 * and half a step either side of a degree of latitude's ~111 km is the "~360 m" the table says
 * for 16. A decimal is then printed while its step is no coarser than that footprint: one digit
 * more than the rounding strictly supports rather than one fewer, because a digit too many is
 * noise and a digit too few moves the point.
 */
static int node_degree_decimals(uint8_t precision_bits) {
    if (precision_bits == 0U || precision_bits >= 32U) {
        return 5; /* not said, or not rounded: the wire's own figure */
    }
    const uint64_t metres = (111000ULL << (31U - precision_bits)) / 10000000ULL;
    uint64_t step = 111000U;
    int decimals = 0;
    while (decimals < 5 && step > metres) {
        step /= 10U;
        ++decimals;
    }
    return decimals;
}

static void node_rows_position(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                               uint32_t now) {
    const struct mesh_ui_node_position *position = &node->position;
    if (!position->valid) {
        return;
    }
    rows_heading(rows, MESH_STR_NODE_HEAD_POSITION, INKCELL_ICON_POSITION);

    const int decimals = node_degree_decimals(position->precision_bits);
    rows_info(rows, MESH_STR_NODE_LATITUDE, MESH_STR_NODE_VAL_DEGREES_ROUNDED, decimals,
              (double)position->latitude_i / 1e7);
    rows_info(rows, MESH_STR_NODE_LONGITUDE, MESH_STR_NODE_VAL_DEGREES_ROUNDED, decimals,
              (double)position->longitude_i / 1e7);
    if (position->has_altitude) {
        char altitude[24];
        mesh_ui_format_altitude((int32_t)position->altitude, rows->imperial, altitude,
                                sizeof altitude);
        rows_text(rows, MESH_STR_NODE_ALTITUDE, altitude);
    }
    if (position->sats_in_view > 0U) {
        rows_info(rows, MESH_STR_NODE_SATELLITES, MESH_STR_NODE_VAL_NUMBER,
                  (unsigned)position->sats_in_view);
    }
    /* A bit count is not a fact about the world. The sender rounded its coordinates off by
       this many bits, and the phone apps' distance for each step is the honest way to say how
       much - so the row reads "~360 m", and the coordinates above it stop at the decimal that
       footprint still supports (node_degree_decimals()). 0 here means the node never set the field,
       not "off": an unrounded fix and one whose precision we were not told apart are the same to
       us, and neither claims a footprint it cannot support. */
    if (position->precision_bits > 0U) {
        char precision[24];
        mesh_ui_settings_format_precision((uint32_t)position->precision_bits, rows->imperial,
                                          precision, sizeof precision);
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
    rows_heading(rows, MESH_STR_NODE_HEAD_ENVIRONMENT, INKCELL_ICON_DETECTION);

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
        rows_gauge(rows, inkcell_temperature_decidegrees(env->temperature), node_temperature_scale,
                   &node_temperature_band);
        rows_trend(rows, MESH_UI_HISTORY_TEMPERATURE);
    }
    if (env->has_humidity) {
        rows_info(rows, MESH_STR_NODE_HUMIDITY, MESH_STR_NODE_VAL_PERCENT_FINE,
                  (double)env->relative_humidity);
        rows_gauge(rows, inkcell_percent_permille(env->relative_humidity), node_permille_scale,
                   &node_humidity_band);
        rows_trend(rows, MESH_UI_HISTORY_HUMIDITY);
    }
    if (env->has_pressure) {
        rows_info(rows, MESH_STR_NODE_PRESSURE, MESH_STR_NODE_VAL_PRESSURE,
                  (double)env->barometric_pressure);
    }
    if (env->has_iaq) {
        rows_info(rows, MESH_STR_NODE_AIR_QUALITY, MESH_STR_NODE_VAL_IAQ, (unsigned)env->iaq);
        rows_gauge(rows, (int32_t)env->iaq, node_iaq_scale, &node_iaq_band);
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
    rows_heading(rows, MESH_STR_NODE_HEAD_POWER, INKCELL_ICON_POWER);
    for (size_t ch = 0; ch < sizeof power->channel / sizeof power->channel[0]; ++ch) {
        const struct mesh_ui_node_power_channel *channel = &power->channel[ch];
        if (!channel->has_voltage && !channel->has_current) {
            continue;
        }
        char label[MESH_UI_NODE_LABEL_MAX];
        inkcell_str_format(label, sizeof label, MESH_STR_NODE_POWER_CHANNEL, (unsigned)ch + 1U);
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
    rows_heading(rows, MESH_STR_NODE_HEAD_AIR_QUALITY, INKCELL_ICON_DETECTION);
    /* PM2.5 first and on its own row: it is the number air quality is judged by, and the one a
       person looks for. The coarser fractions share a row because they are read against it. */
    if (air->has_pm25) {
        rows_info(rows, MESH_STR_NODE_PM25, MESH_STR_NODE_VAL_PARTICULATES,
                  (unsigned)air->pm25_standard);
        rows_gauge(rows, (int32_t)air->pm25_standard, node_pm25_scale, &node_pm25_band);
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
        rows_gauge(rows, (int32_t)air->co2, node_co2_scale, &node_co2_band);
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
    rows_heading(rows, MESH_STR_NODE_HEAD_HEALTH, INKCELL_ICON_DETECTION);
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
    rows_heading(rows, MESH_STR_NODE_HEAD_HOST, INKCELL_ICON_STATUS);
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
    /* Our own radio by what it is to the reader rather than by its short name: "HOME 11 dB" in
       a list of strangers reads as one more of them, and whether this node can hear *us* is
       the row a reader opened the list to find. */
    if (roster != NULL && roster->has_my_info && roster->my_info.node_num != 0U &&
        node_id == roster->my_info.node_num) {
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_NODE_NEIGHBOUR_SELF));
        return;
    }
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
                inkwell_str_copy(out, out_len, name);
                return;
            }
            break;
        }
    }
    inkcell_str_format(out, out_len, MESH_STR_NODE_VAL_USER_ID_HEX, node_id);
}

static void node_rows_neighbors(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                const struct mesh_ui_handshake_state *roster, uint32_t now) {
    if (roster == NULL) {
        return;
    }

    const struct mesh_ui_node_neighbors *heard = &node->neighbors;
    if (heard->valid) {
        rows_heading(rows, MESH_STR_NODE_HEAD_NEIGHBOURS, INKCELL_ICON_NEIGHBORS);
        if (heard->count == 0U) {
            /* A node that hears nobody is a real state and an interesting one - it is how a
               repeater that has fallen off the mesh looks - so it says so rather than showing
               a heading with nothing under it. */
            rows_text(rows, MESH_STR_NODE_NEIGHBOURS_NONE, inkcell_str(MESH_STR_NODE_HEARS_NO_ONE));
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
                rows_heading(rows, MESH_STR_NODE_HEAD_HEARD_BY, INKCELL_ICON_NEIGHBORS);
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
 * The verb that starts a trace, and what this node's record currently says.
 *
 * Emitted whatever the state, because it is also how a trace is re-run. The record is the one
 * mesh_ui_store_traceroute_view() picked for this node - the trace in flight while it is this
 * node's, and the route last measured to it otherwise - so a screen never describes one node
 * with another node's trace, and NULL is a node nothing has been asked about.
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
    const char *value = inkcell_str(MESH_STR_COMMON_PRESS_A);
    if (ours) {
        switch ((enum mesh_traceroute_state)trace->state) {
        case MESH_TRACEROUTE_PENDING:
            value = inkcell_str(MESH_STR_NODE_TRACE_RUNNING);
            break;
        case MESH_TRACEROUTE_TIMEOUT:
            value = inkcell_str(MESH_STR_NODE_TRACE_TIMEOUT);
            break;
        default:
            break;
        }
    }
    rows_action(rows, MESH_STR_NODE_TRACE_ROUTE, value, MESH_UI_NODE_ACTION_TRACEROUTE);
}

/*
 * The traced route itself, when this node's record holds a finished trace. Two paths of stops,
 * each row a node and the SNR of the link that reached it - the first stop of a path is the
 * sender and has no incoming link, so it carries no reading rather than a zero.
 *
 * The record may have been measured in an earlier run of the client, which is what the stamp at
 * the foot of the group is for: a route is drawn with its age exactly as a position fix is.
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
                     INKCELL_ICON_LINK);
        for (uint8_t i = 0; i < count && i < MESH_UI_TRACEROUTE_MAX_HOPS; ++i) {
            const struct mesh_ui_traceroute_hop *hop = &path[i];
            char label[MESH_UI_NODE_LABEL_MAX];
            /* An arrow would be two bytes the framebuffer font has no glyph for. */
            snprintf(label, sizeof label, "%s%s",
                     i == 0U ? "" : inkcell_str(MESH_STR_NODE_HOP_ARROW), hop->name);
            /* INT8_MIN is the firmware's "this link was not measured", not a -32 dB link. */
            if (hop->has_snr && hop->snr_quarter_db != INT8_MIN) {
                rows_named(rows, label, MESH_STR_NODE_VAL_SNR, (double)hop->snr_quarter_db / 4.0);
            } else {
                struct mesh_ui_node_item *row = rows_info_row(rows, label);
                if (row != NULL) {
                    snprintf(row->value, sizeof row->value, "%s",
                             inkcell_str(i == 0U ? MESH_STR_NODE_HOP_START
                                                 : MESH_STR_NODE_HOP_NO_READING));
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

/*
 * Every verb this node offers, as its own screen.
 *
 * These thirteen rows used to open the node's detail - a full panel of them above the first
 * fact, so a reader who pressed A on a node to find out what it *was* met a menu, with "Remove
 * from radio" on screen before the battery level. They are the same rows in the same shape, one
 * press further in, and the detail keeps one row (MESH_UI_NODE_ACTION_OPEN_ACTIONS) that opens
 * them.
 *
 * What is *not* here is the traced route the traceroute verb produces. That is a report and it
 * belongs among the reports, which is the rule node_rows_route_path() already stated from the
 * other side: a group is one unbroken run, and a measured path between two verbs is a group
 * interrupting a group. The verb is here; what it measured is on the detail.
 */
/* Whether the protocol has `feature`, from the builder's `lacks`. */
static bool node_actions_offer(uint32_t lacks, enum mesh_ui_feature feature) {
    return (lacks & (uint32_t)feature) == 0U;
}

uint32_t mesh_ui_node_actions_build(const struct mesh_ui_node_summary *node, bool is_self,
                                    const struct mesh_ui_traceroute *trace, bool remove_armed,
                                    uint32_t lacks, struct mesh_ui_node_item *out,
                                    uint32_t capacity) {
    if (node == NULL) {
        return 0U;
    }
    const bool flags = node_actions_offer(lacks, MESH_UI_FEATURE_NODE_FLAGS);
    const bool requests = node_actions_offer(lacks, MESH_UI_FEATURE_NODE_REQUESTS);

    struct node_rows rows = {
        .items = out,
        .capacity = (out == NULL) ? MESH_UI_NODE_ACTIONS_MAX : capacity,
        .count = 0U,
        /* No history and no units: a verb is a press rather than a reading, so neither of the
           two things that word a *fact* on the detail has anything to say about one. */
        .history = NULL,
        .node_id = node->node_id,
        .imperial = false,
    };

    if (!is_self) {
        rows_action(&rows, MESH_STR_NODE_ACT_MESSAGE, NULL, MESH_UI_NODE_ACTION_MESSAGE);
        /* Pinning our own node would be meaningless - it already ranks above everything. */
        if (flags) {
            rows_toggle(&rows, MESH_STR_NODE_ACT_PIN, node->is_favorite,
                        MESH_UI_NODE_ACTION_FAVORITE);
        }
        /* Tracing the route to ourselves is a question with no links in it. */
        if (node_actions_offer(lacks, MESH_UI_FEATURE_TRACEROUTE)) {
            node_rows_route_action(&rows, node, trace);
        }
        /* The one row that answers "who is this?" for a node that joined after the NodeDB
           replay and has been sitting in the list as a bare id ever since. */
        if (requests) {
            rows_action(&rows, MESH_STR_NODE_ACT_REQUEST_INFO, inkcell_str(MESH_STR_COMMON_PRESS_A),
                        MESH_UI_NODE_ACTION_REQUEST_INFO);
        }
        /* The same shape, for the two readings that otherwise arrive on the node's own
           schedule. They sit next to "Ask for its name" because they are the same question -
           tell me what you have now - and because the answer to all three lands in the groups
           further down this screen rather than anywhere else. */
        if (requests) {
            rows_action(&rows, MESH_STR_NODE_ACT_REQUEST_POSITION,
                        inkcell_str(MESH_STR_COMMON_PRESS_A), MESH_UI_NODE_ACTION_REQUEST_POSITION);
            rows_action(&rows, MESH_STR_NODE_ACT_REQUEST_TELEM,
                        inkcell_str(MESH_STR_COMMON_PRESS_A),
                        MESH_UI_NODE_ACTION_REQUEST_TELEMETRY);
        }
        /* Muting is the gentle one of the three below: the node's traffic still arrives and
           still shows in its conversation, the radio just stops announcing it. The wire verb
           is a toggle rather than a set, so this row states the flag and flips it. */
        if (flags) {
            rows_toggle(&rows, MESH_STR_NODE_ACT_MUTE, node->is_muted, MESH_UI_NODE_ACTION_MUTE);
        }
        /* Then, stated as what the radio will do rather than as a preference: an ignored
           node's packets are dropped before they reach us. */
        if (flags) {
            rows_toggle(&rows, MESH_STR_NODE_ACT_IGNORE, node->is_ignored,
                        MESH_UI_NODE_ACTION_IGNORE);
        }
        /* Last, because it is the only row here that takes its own row away with it: the node
           leaves the list and there is nothing left to press to undo it. It comes back on its
           own when the node next transmits, which is why this is an arming press rather than
           the confirm overlay - the cost is a wait, not a loss. */
        if (node_actions_offer(lacks, MESH_UI_FEATURE_NODE_REMOVE)) {
            rows_action(&rows, MESH_STR_NODE_ACT_REMOVE,
                        inkcell_str(remove_armed ? MESH_STR_NODE_ACT_REMOVE_ARMED
                                                 : MESH_STR_COMMON_PRESS_A),
                        MESH_UI_NODE_ACTION_REMOVE);
        }
        /*
         * The two key rows, after everything above because they are the pair that acts on what
         * the Identity group states rather than on the node's traffic - and because one of them
         * opens a ceremony involving two people and a telephone, which is not something to land
         * on by overshooting a cursor.
         *
         * Both are gated on holding a key: there is nothing to verify without one and nothing
         * worth giving the radio, which would build an entry with no key for itself the moment
         * the node transmitted. "Put back on the radio" is gated again on the radio not already
         * having it - it is the answer to the "not on the radio" line four rows up, and on a
         * node the NodeDB still carries it would be a press with nothing to do.
         */
        const enum mesh_ui_key_trust key_trust = mesh_ui_key_trust_of(node);
        if (key_trust != MESH_UI_KEY_TRUST_NONE) {
            if (!node->in_nodedb && node_actions_offer(lacks, MESH_UI_FEATURE_CONTACT_LINKS)) {
                rows_action(&rows, MESH_STR_NODE_ACT_ADD_CONTACT,
                            inkcell_str(MESH_STR_COMMON_PRESS_A), MESH_UI_NODE_ACTION_ADD_CONTACT);
            }
            /* Already verified is not a reason to hide the row. A key that changed is exactly
               when somebody would want to do it again, and the label says which of the two
               presses this is so the row is not silently a no-op. */
            if (node_actions_offer(lacks, MESH_UI_FEATURE_KEY_VERIFICATION)) {
                rows_action(&rows,
                            key_trust == MESH_UI_KEY_TRUST_VERIFIED ? MESH_STR_NODE_ACT_VERIFY_AGAIN
                                                                    : MESH_STR_NODE_ACT_VERIFY_KEY,
                            inkcell_str(MESH_STR_COMMON_PRESS_A), MESH_UI_NODE_ACTION_VERIFY_KEY);
            }
            /*
             * And the last row on the card: open the Settings tab against this node's radio
             * instead of our own.
             *
             * Under the same gate as the two above, and for a harder reason than theirs - a
             * remote AdminMessage is sealed to the node's public key, so without one there is
             * nothing to address it to. Last of the verbs because it is the one that changes
             * what every *other* screen means, which is not something to land on by
             * overshooting a cursor.
             *
             * The row reads the same whether or not this node is already the one being
             * configured, and that is deliberate: pressing it then takes you to the tab, which
             * is what somebody pressing "configure this radio" wanted either way. The way back
             * is a row in About radio, which is where the banner sends them.
             */
            if (node_actions_offer(lacks, MESH_UI_FEATURE_REMOTE_ADMIN)) {
                rows_action(&rows, MESH_STR_NODE_ACT_ADMIN, inkcell_str(MESH_STR_COMMON_PRESS_A),
                            MESH_UI_NODE_ACTION_ADMIN);
            }
        }
    }
    /*
     * Outside the block above, because this is the one action our own node has a use for too:
     * a Brick has no GPS, so "where my radio says it is" and "where that node says it is" are
     * the same kind of answer and the only two a waypoint can be made from. The row appears
     * only when there is a fix to make one at - offering it against no coordinates would be
     * offering a place that is nowhere.
     */
    if (node->position.valid) {
        /* Looking at it, and keeping it: the two things a fix is good for, and both gated on
           there being one. A "show on map" row over a node with no position would open a map
           aimed at nowhere. */
        rows_action(&rows, MESH_STR_NODE_ACT_SHOW_ON_MAP, inkcell_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_SHOW_ON_MAP);
        if (node_actions_offer(lacks, MESH_UI_FEATURE_WAYPOINTS)) {
            rows_action(&rows, MESH_STR_NODE_ACT_WAYPOINT, inkcell_str(MESH_STR_COMMON_PRESS_A),
                        MESH_UI_NODE_ACTION_WAYPOINT);
        }
    }

    return rows.count;
}

uint32_t mesh_ui_node_actions_count(const struct mesh_ui_node_summary *node, bool is_self,
                                    const struct mesh_ui_traceroute *trace, uint32_t lacks) {
    /* Built to be counted, exactly as mesh_ui_node_detail_count() is and for its reason: which
       verbs exist depends on what the node has - a key, a fix, a place in the radio's list - so
       there is no arithmetic from a node to a number. `remove_armed` changes a row's value and
       never whether it is there, so this passes false and cannot disagree with the build. */
    return mesh_ui_node_actions_build(node, is_self, trace, false, lacks, NULL, 0U);
}

uint32_t mesh_ui_node_detail_build(const struct mesh_ui_node_summary *node, bool is_self,
                                   uint32_t now, const struct mesh_ui_traceroute *trace,
                                   const struct mesh_ui_handshake_state *roster,
                                   const struct mesh_ui_history *history, bool imperial,
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
        .imperial = imperial,
    };

    /*
     * The one verb of the detail itself: the row that opens every other one.
     *
     * Offered only when there is a sheet behind it. Our own node has no use for eleven of the
     * thirteen and none at all for the two that are left unless it has reported a fix, so the
     * question is asked of the builder rather than re-derived here - a row that opened an empty
     * screen would be this client's own version of the keycap-that-does-nothing that the action
     * bar table exists to prevent, and re-testing `is_self` and `position.valid` here is how the
     * two copies would come to disagree the first time a verb changed its gate.
     *
     * No heading over it. A heading names a group the reader can skip past and this is one row;
     * what the row is about is in the row, and the sheet it opens says "Actions" in its own bar.
     */
    /* With nothing lacked: message and show-on-map are never gated, so whether there is a sheet
       is the same answer whatever the protocol, and the detail has no settings to ask. */
    if (mesh_ui_node_actions_count(node, is_self, trace, 0U) > 0U) {
        rows_action(&rows, MESH_STR_NODE_HEAD_ACTIONS, inkcell_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_NODE_ACTION_OPEN_ACTIONS);
    }
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
     * Still gated on `is_self` with the verb that starts it, which now lives a screen away: the
     * trace is not offered against our own node, so one targeting it is a trace nothing here
     * could have started.
     */
    if (!is_self) {
        node_rows_route_path(&rows, node, trace, now);
    }
    node_rows_identity(&rows, node);
    node_rows_signal(&rows, node, roster, is_self, now);
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
    const uint32_t count = mesh_ui_node_detail_build(node, is_self, 0U, trace, roster, history,
                                                     false, items, MESH_UI_NODE_ITEMS_MAX);
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
    const uint32_t count = mesh_ui_node_detail_build(node, is_self, 0U, trace, roster, history,
                                                     false, items, MESH_UI_NODE_ITEMS_MAX);
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

enum mesh_ui_node_press mesh_ui_node_detail_press_at(const struct mesh_ui_node_summary *node,
                                                     bool is_self,
                                                     const struct mesh_ui_traceroute *trace,
                                                     const struct mesh_ui_handshake_state *roster,
                                                     const struct mesh_ui_history *history,
                                                     uint32_t row) {
    if (node == NULL || row >= MESH_UI_NODE_ITEMS_MAX) {
        return MESH_UI_NODE_PRESS_NONE;
    }
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(node, is_self, 0U, trace, roster, history,
                                                     false, items, MESH_UI_NODE_ITEMS_MAX);
    if (row >= count) {
        return MESH_UI_NODE_PRESS_NONE;
    }
    /*
     * The chart is tested first because it is the one press on this screen that is not an action
     * row - the same order mesh_ui_nav_confirm() takes the two in, and it has to be the same
     * order or the bar and the press name different verbs on one row.
     */
    if (items[row].kind == MESH_UI_NODE_ROW_METER &&
        items[row].trend_reading != (uint8_t)MESH_UI_HISTORY_NONE) {
        return MESH_UI_NODE_PRESS_TREND;
    }
    if (items[row].kind != MESH_UI_NODE_ROW_ACTION) {
        return MESH_UI_NODE_PRESS_NONE;
    }
    /* The Actions row runs nothing itself - it opens a screen, and its chevron already says so,
       which "select" under it did not. */
    return items[row].action == MESH_UI_NODE_ACTION_OPEN_ACTIONS ? MESH_UI_NODE_PRESS_OPEN
                                                                 : MESH_UI_NODE_PRESS_SELECT;
}

/* A reading with a bar takes a second line for it; every other row is one. */
uint8_t mesh_ui_node_item_steps(const struct mesh_ui_node_item *item) {
    return item != NULL && item->kind == MESH_UI_NODE_ROW_METER ? 2U : 1U;
}

/* Whether A does something on this row - mesh_ui_node_detail_press_at()'s answer, asked of a
   row already built. */
static bool item_is_press(const struct mesh_ui_node_item *item) {
    return item->kind == MESH_UI_NODE_ROW_ACTION ||
           (item->kind == MESH_UI_NODE_ROW_METER &&
            item->trend_reading != (uint8_t)MESH_UI_HISTORY_NONE);
}

/* One group: the heading that opens it (when the list has one there), its rows [top, end), and
   whether any of them is a press. */
struct node_group {
    uint32_t heading;
    uint32_t top;
    uint32_t end;
    bool has_heading;
    bool presses;
};

/* The group `row` stands in. A heading is read as the group it opens. */
static struct node_group group_of(const struct mesh_ui_node_item *items, uint32_t count,
                                  uint32_t row) {
    struct node_group group = {0};
    uint32_t at = row;
    while (at > 0U && items[at].kind != MESH_UI_NODE_ROW_HEADING) {
        at -= 1U;
    }
    group.has_heading = items[at].kind == MESH_UI_NODE_ROW_HEADING;
    group.heading = at;
    group.top = group.has_heading ? at + 1U : 0U;
    group.end = group.top;
    while (group.end < count && items[group.end].kind != MESH_UI_NODE_ROW_HEADING) {
        group.presses = group.presses || item_is_press(&items[group.end]);
        group.end += 1U;
    }
    return group;
}

/*
 * The page of facts `row` is on, in the run [top, end): its first row, and its last in `*last`.
 *
 * A run that fits the window with a heading is one page. One that does not is cut into as few
 * pages as fit, evened out rather than filled greedily - so a run a row too tall becomes two
 * halves, not a full page and a page holding one row. `rows` of 0 is no window: one page.
 */
static uint32_t page_of(const struct mesh_ui_node_item *items, uint32_t top, uint32_t end,
                        uint32_t rows, uint32_t row, uint32_t *last) {
    uint32_t total = 0U;
    for (uint32_t r = top; r < end; ++r) {
        total += mesh_ui_node_item_steps(&items[r]);
    }
    /* The heading is a step the first page spends, and a reading takes two - so a window under
       three cannot hold a page and pages nothing rather than a row at a time. */
    const uint32_t budget = rows > 2U ? rows - 1U : 0U;
    const uint32_t pages = budget > 0U && total > budget ? (total + budget - 1U) / budget : 1U;
    const uint32_t target = pages > 1U ? (total + pages - 1U) / pages : total;
    uint32_t start = top;
    uint32_t steps = 0U;
    for (uint32_t r = top; r < end; ++r) {
        const uint32_t h = mesh_ui_node_item_steps(&items[r]);
        if (steps > 0U && steps + h > target) {
            if (row < r) {
                *last = r - 1U;
                return start;
            }
            start = r;
            steps = 0U;
        }
        steps += h;
    }
    *last = end > top ? end - 1U : top;
    return start;
}

/* Whether the whole group, heading included, fits the window - or there is no window to fit. */
static bool group_fits(const struct mesh_ui_node_item *items, const struct node_group *group,
                       uint32_t rows) {
    if (rows == 0U) {
        return true;
    }
    uint32_t steps = group->has_heading ? 1U : 0U;
    for (uint32_t r = group->top; r < group->end; ++r) {
        steps += mesh_ui_node_item_steps(&items[r]);
    }
    return steps <= rows;
}

/*
 * The rows of facts `row` is paged within: the whole group for a card of facts, and for a group
 * holding a press, the unbroken run of facts around `row` - which is paged only when the group
 * is taller than the window, because a group that fits is in view from any of its presses.
 * False for a fact row of a group that fits, which is no page at all.
 */
static bool fact_run(const struct mesh_ui_node_item *items, const struct node_group *group,
                     uint32_t rows, uint32_t row, uint32_t *top, uint32_t *end) {
    if (!group->presses) {
        *top = group->top;
        *end = group->end;
        return true;
    }
    if (group_fits(items, group, rows)) {
        return false;
    }
    *top = row;
    while (*top > group->top && !item_is_press(&items[*top - 1U])) {
        *top -= 1U;
    }
    *end = row + 1U;
    while (*end < group->end && !item_is_press(&items[*end])) {
        *end += 1U;
    }
    return true;
}

static bool row_is_stop(const struct mesh_ui_node_item *items, uint32_t count, uint32_t rows,
                        uint32_t row) {
    if (row >= count || items[row].kind == MESH_UI_NODE_ROW_HEADING) {
        return false;
    }
    if (item_is_press(&items[row])) {
        return true;
    }
    const struct node_group group = group_of(items, count, row);
    uint32_t top;
    uint32_t end;
    if (!fact_run(items, &group, rows, row, &top, &end)) {
        return false;
    }
    uint32_t last;
    return page_of(items, top, end, rows, row, &last) == row;
}

/* A group's first stop, or `count` for a group with no rows. */
static uint32_t group_first_stop(const struct mesh_ui_node_item *items, uint32_t count,
                                 uint32_t rows, const struct node_group *group) {
    for (uint32_t r = group->top; r < group->end; ++r) {
        if (row_is_stop(items, count, rows, r)) {
            return r;
        }
    }
    return count;
}

uint32_t mesh_ui_node_detail_step(const struct mesh_ui_node_summary *node, bool is_self,
                                  const struct mesh_ui_traceroute *trace,
                                  const struct mesh_ui_handshake_state *roster,
                                  const struct mesh_ui_history *history, uint32_t rows,
                                  uint32_t row, int delta) {
    if (node == NULL || delta == 0) {
        return row;
    }
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(node, is_self, 0U, trace, roster, history,
                                                     false, items, MESH_UI_NODE_ITEMS_MAX);
    if (row >= count) {
        return row;
    }
    if (delta > 0) {
        for (uint32_t at = row + 1U; at < count; ++at) {
            if (row_is_stop(items, count, rows, at)) {
                return at;
            }
        }
        return row;
    }
    for (uint32_t at = row; at-- > 0U;) {
        if (row_is_stop(items, count, rows, at)) {
            return at;
        }
    }
    return row;
}

uint32_t mesh_ui_node_detail_group_step(const struct mesh_ui_node_summary *node, bool is_self,
                                        const struct mesh_ui_traceroute *trace,
                                        const struct mesh_ui_handshake_state *roster,
                                        const struct mesh_ui_history *history, uint32_t rows,
                                        uint32_t row, int delta) {
    if (node == NULL || delta == 0) {
        return row;
    }
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(node, is_self, 0U, trace, roster, history,
                                                     false, items, MESH_UI_NODE_ITEMS_MAX);
    if (row >= count) {
        return row;
    }
    struct node_group group = group_of(items, count, row);
    if (delta > 0) {
        /* The next group with somewhere to stand; a heading with no rows under it is skipped. */
        while (group.end < count) {
            group = group_of(items, count, group.end);
            const uint32_t stop = group_first_stop(items, count, rows, &group);
            if (stop < count) {
                return stop;
            }
        }
        return row;
    }
    /* Not yet at the top of this group, so that is where Left goes - the halfway house the
       header describes, and the reason Right needs none. */
    const uint32_t own = group_first_stop(items, count, rows, &group);
    if (own < row) {
        return own;
    }
    while (group.has_heading && group.heading > 0U) {
        group = group_of(items, count, group.heading - 1U);
        const uint32_t stop = group_first_stop(items, count, rows, &group);
        if (stop < count) {
            return stop;
        }
    }
    return row;
}

bool mesh_ui_node_detail_span(const struct mesh_ui_node_item *items, uint32_t count, uint32_t rows,
                              uint32_t row, struct mesh_ui_node_span *out) {
    if (items == NULL || out == NULL || row >= count) {
        return false;
    }
    const struct node_group group = group_of(items, count, row);
    const uint32_t first = group.has_heading ? group.heading : group.top;
    if (group.end <= group.top) {
        *out = (struct mesh_ui_node_span){.first = row, .last = row, .card = false};
        return true;
    }
    const bool press = item_is_press(&items[row]);
    uint32_t top;
    uint32_t end;
    if (press || !fact_run(items, &group, rows, row, &top, &end)) {
        /* A press, or a fact of a group that fits: the whole group in view around it. A fact row
           of such a group is only under the cursor when the group changed beneath it, and it is
           the card that is focused then, not a row A would do nothing on. */
        *out = (struct mesh_ui_node_span){.first = first, .last = group.end - 1U, .card = !press};
        return true;
    }
    uint32_t last;
    const uint32_t page = page_of(items, top, end, rows, row, &last);
    *out = (struct mesh_ui_node_span){
        .first = page == group.top ? first : page, .last = last, .card = true};
    return true;
}

uint32_t mesh_ui_node_detail_count(const struct mesh_ui_node_summary *node, bool is_self,
                                   const struct mesh_ui_traceroute *trace,
                                   const struct mesh_ui_handshake_state *roster) {
    return mesh_ui_node_detail_build(node, is_self, 0U, trace, roster, NULL, false, NULL, 0U);
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

bool mesh_ui_node_our_fix(const struct mesh_ui_handshake_state *handshake, int32_t *out_latitude_i,
                          int32_t *out_longitude_i) {
    if (handshake == NULL || !handshake->has_my_info || handshake->my_info.node_num == 0U) {
        return false;
    }
    const struct mesh_ui_node_summary *self =
        mesh_ui_node_detail_find(handshake, handshake->my_info.node_num);
    if (self == NULL || !self->position.valid) {
        return false;
    }
    /* Range-checked again on the way out rather than trusted because it is ours: the roster is
       restored from a cache written by an older build, and a coordinate is checked where it is
       used as one. */
    if (!mesh_geo_coords_valid(self->position.latitude_i, self->position.longitude_i)) {
        return false;
    }
    if (out_latitude_i != NULL) {
        *out_latitude_i = self->position.latitude_i;
    }
    if (out_longitude_i != NULL) {
        *out_longitude_i = self->position.longitude_i;
    }
    return true;
}
