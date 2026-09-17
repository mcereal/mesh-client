#ifndef MESH_UI_HISTORY_H
#define MESH_UI_HISTORY_H

/*
 * What the client has been told, and when - as opposed to the snapshot, which is what is true
 * now.
 *
 * Every other structure the store publishes is the present tense: this many nodes, this much of
 * the air in use, this battery at this percent. That is the whole of what the radio sends, and
 * it is why nothing on this panel could answer a question about a *direction* - the reader had
 * to have looked before and remembered. The sparkline needed somewhere for the client to
 * remember instead, and this is it: a small, bounded set of `struct mesh_ui_series`
 * (layout.h) filled as the readings arrive.
 *
 * Three decisions, because each of them is a way this could have been bigger and worse:
 *
 *   - **It keeps only what a screen draws.** Two series about the radio we are attached to, and
 *     the handful of readings a node's own detail screen puts a line under, for a bounded
 *     number of nodes. Not every reading, not every node: session.c decodes six telemetry
 *     variants and a general store of everything the mesh ever said is a database, so what
 *     earns a slot here is a reading some screen has somewhere to draw it.
 *   - **Everything here is persisted, and the two halves are kept in different files.** A trend
 *     is what we *watched*, so a resumed one must not put a line over a period nothing observed -
 *     but that is an argument for drawing the seam as a seam, not for throwing the readings away,
 *     and mesh_ui_history_resume() is what lifts the pen over it. The radio's pair rides the
 *     handshake cache, because six hours of it is what the chart is for and the cache is
 *     rewritten whole on every save. A node's readings are a per-node append-only log
 *     (include/mesh/ui/store_trends.h), read back into the slot below when that node's detail
 *     screen is opened - twelve nodes' worth of half-hourly readings is not something a file
 *     rewritten every save should be carrying, and only one node's is ever drawn.
 *   - **Nothing here knows what a node is.** The store walks the roster and hands over
 *     readings; this holds series and decides which ones are worth a slot. That is what lets
 *     store.h include this rather than the other way round, and it is the same seam
 *     mesh_ui_signal_level() sits on - what a number means is one layer, what it is a number
 *     *about* is another.
 */

#include "mesh/ui/layout.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nodes we keep trends for.
 *
 * A screen budget, not a mesh limit, exactly as MESH_UI_MAX_HANDSHAKE_NODES is: only the node
 * whose detail screen is open is ever drawn, and a slot exists so that the trend is already
 * there when it is opened rather than starting from nothing. Twelve is comfortably more than
 * the nodes anybody keeps an eye on, and the least recently heard from is what a thirteenth
 * costs.
 */
#define MESH_UI_HISTORY_NODES 12U

/*
 * Series kept at once, across every node and every reading - the pool a node's trends are
 * drawn from.
 *
 * It exists because the readings worth watching stopped being three. Held as named fields on a
 * node's slot, every reading cost every node whether that node reports it or not: a mesh of
 * twelve where three have thermometers still carried nine unused temperature series and nine
 * unused humidity ones, and each of those is a quarter of a kilobyte inside a snapshot that is
 * copied whole on every publish. Five readings that way would have been sixty series, most of
 * them zeroes.
 *
 * So a slot names the series it has and the rest are not spent. Forty-eight is more than the
 * readings a real mesh produces - every node's battery, a few sensors' air, and the signal of
 * the nodes actually heard over the air - and it is deliberately below twelve nodes times every
 * reading, because a budget nothing can exhaust is not a budget, it is the fixed table again
 * with extra arithmetic.
 *
 * What runs out first is still the node table, and that matters: a series is only ever taken
 * *by* a node, so the pool emptying is handled the way a full node table is - the least recently
 * heard node goes, and every series it held comes back at once. See mesh_ui_history_series().
 */
#define MESH_UI_HISTORY_SERIES 48U

/* A slot's reading that has no series: nothing has been kept for it, and nothing is spent on
   it. Not 0, which is a real pool index. */
#define MESH_UI_HISTORY_NO_SERIES 0xFFU

/*
 * How often each source's readings actually arrive, and how long a silence has to be before a
 * line breaks rather than sloping across it.
 *
 * The cadence is stated first and the gap is a multiple of it, because the gap is not a duration
 * anybody picked: it is "a few reports missed", and writing it as a bare number is how it comes
 * to be *one* report - which is a series that breaks at every sample and so never has a line in
 * it at all.
 *
 * The radio's cadence is a minute, and that is the DeviceMetrics the firmware hands its attached
 * client for our own node rather than LocalStats. Both carry `channel_utilization` and
 * `air_util_tx`, but DeviceTelemetryModule sends the first to the phone on every one-minute tick
 * it is not broadcasting to the mesh - and cc's the phone on the broadcast when it is - while
 * LocalStats rides the fifteen-minute `sendStatsToPhoneIntervalMs` and is skipped whenever the
 * phone's queue is busy. Sampled off LocalStats alone, three hours on a Heltec V4 left six
 * readings about 45 minutes apart: a chart of dots with no line between any of them. LocalStats
 * is still taken when DeviceMetrics is not arriving; see mesh_ui_history_note_airtime().
 *
 * Three reports rather than two, because a report is skipped as well as delayed. A node's device
 * metrics ride the mesh broadcast instead, which is half an hour at the firmware's default and
 * longer on anything running on a battery it cares about - four of those, on the same reasoning.
 */
#define MESH_UI_HISTORY_RADIO_REPORT_MS (60U * 1000U)
#define MESH_UI_HISTORY_NODE_REPORT_MS (30U * 60U * 1000U)

#define MESH_UI_HISTORY_RADIO_GAP_MS (3U * MESH_UI_HISTORY_RADIO_REPORT_MS)
#define MESH_UI_HISTORY_NODE_GAP_MS (4U * MESH_UI_HISTORY_NODE_REPORT_MS)

/*
 * Where this history's clock starts, so that a reading restored from the card has somewhere to
 * go *before* the first live one.
 *
 * The timeline below is the client's own and is only ever read as differences, so its zero is
 * free to be anywhere - and it cannot be at zero. A restore places saved readings behind the
 * live clock (mesh_ui_history_stamp_now()), and the live clock a few seconds into a run is a
 * few seconds: a day of saved readings placed behind it would wrap under, and a time that
 * wrapped under reads as a time far in the *future*, which is a series drawn backwards.
 *
 * Eight days, against the seven a node's log is allowed to span (MESH_UI_TRENDS_SPAN_MAX_MS),
 * so the widest restore still lands above zero with the better part of a day to spare. What it
 * costs is at the other end: a uint32 of milliseconds is 49 days, so the client now has 41 of
 * them of continuous running before the clock wraps rather than 49. A wrap empties a series
 * either way - see mesh_ui_series_push() - and a Brick is a handheld.
 */
#define MESH_UI_HISTORY_EPOCH_MS (8U * 24U * 60U * 60U * 1000U)

/*
 * Readings of the radio's airtime kept: six hours at the one-minute cadence, which is the widest
 * fixed span the chart offers (MESH_UI_TREND_SPAN_6H).
 *
 * Its own ring rather than a `struct mesh_ui_series`, whose two dozen is sized for a sparkline
 * the width of a list row. The chart bins these into columns (mesh_ui_trend_airtime()), so the
 * count here is a memory budget rather than a count of marks: about 4 KB, carried in every
 * snapshot.
 */
#define MESH_UI_HISTORY_AIRTIME_MAX 360U

/*
 * Which reading a series is of.
 *
 * It exists because three readings now have one, and because what the client does with a series
 * has stopped being "draw it here": the nav holds one of these to say which chart is open, the
 * action bar names the press from it, and a renderer resolves it back to a series. Written as a
 * switch in each of those it would be three opinions about which readings are kept, and the day
 * a fourth arrives two of them would go on saying there are three.
 *
 * It is deliberately *not* a list of everything a node can report. session.c decodes six
 * telemetry variants and a node detail draws forty-odd rows off them; this names the handful
 * the client has been watching over time, which is the first of the three decisions at the top
 * of this file and the one that keeps it from becoming a database.
 */
enum mesh_ui_history_reading {
    MESH_UI_HISTORY_NONE = 0,    /* no reading: a row with nothing watched, a closed chart */
    MESH_UI_HISTORY_BATTERY,     /* whole percent, as the wire carries it */
    MESH_UI_HISTORY_TEMPERATURE, /* tenths of a degree Celsius */
    MESH_UI_HISTORY_HUMIDITY,    /* permille, as mesh_ui_percent_permille() leaves a percentage */
    /*
     * The link itself: whole decibels as mesh_ui_snr_db() leaves the wire's float, and whole dBm
     * as the wire already carries it.
     *
     * Whole units rather than the tenths a temperature is kept in, and that is not a shortcut -
     * it is the rule that a series is measured in the units of the row it hangs on. The bar
     * under the SNR row is banded in whole decibels (MESH_UI_SNR_FAIR and its neighbours) and
     * the chart is drawn on that row's own scale, so a series in tenths would be a line placed
     * against a domain ten times too small. An SNR is measured off one packet anyway, which is
     * error bars wide enough that a tenth of a decibel is precision the reading does not have.
     */
    MESH_UI_HISTORY_SNR,
    MESH_UI_HISTORY_RSSI,
    MESH_UI_HISTORY_READING_COUNT
};

/*
 * One node's slot: which pool entry holds each of its trends.
 *
 * The series are still the *node's* rather than a table of their own, and that is a statement
 * about what a slot is: it is a node somebody is watching, not a reading. A sensor node reports
 * its battery and its air in the same telemetry, so splitting them would be two evictions racing
 * over one node - and a node whose temperature survived while its battery was evicted would draw
 * half a screen of trend and half a screen of nothing, for no reason the reader could see. They
 * are named by index rather than held inline so that a reading this node does not report costs
 * it nothing; the eviction rule is unchanged, and a slot going takes every series with it.
 *
 * Indexed by `enum mesh_ui_history_reading`, NONE's own entry included and never used - the
 * wasted byte buys an array a caller can subscript with the value it is already holding, rather
 * than a mapping that would be a second opinion about which readings exist.
 *
 * `seen` is still one stamp for the slot, so any reading arriving keeps the whole node fresh.
 * That is the right way round: what the eviction is protecting is the node being looked at.
 */
struct mesh_ui_history_node {
    uint32_t node_id; /* 0 for a free slot */
    uint32_t seen;    /* the clock at the last push, so the least recent slot can be evicted */
    uint8_t series[MESH_UI_HISTORY_READING_COUNT]; /* pool index, or MESH_UI_HISTORY_NO_SERIES */
};

/*
 * One report of the radio's airtime: how much of the channel was busy and how much of that was
 * us, both in permille of the air - the unit mesh_ui_percent_permille() puts the wire's floats on
 * and the unit the Status card's meter reads. Permille fits sixteen bits, and at 360 of these the
 * difference from two int32 is the difference between 4 KB and 6 KB per snapshot.
 *
 * The two are one record rather than two series because they arrive in one packet. They are not
 * the same kind of number, though: the firmware's `channel_utilization` covers roughly the last
 * minute and `air_util_tx` is a rolling hour, so the first is bursty and the second is smooth.
 */
struct mesh_ui_airtime_sample {
    uint32_t time; /* the history's own timeline, as a series sample's is */
    int16_t utilization;
    int16_t tx;
    /* Does not continue the reading before it - a restart seam, see mesh_ui_history_resume(). */
    bool gap;
};

struct mesh_ui_airtime {
    struct mesh_ui_airtime_sample items[MESH_UI_HISTORY_AIRTIME_MAX];
    uint32_t first; /* ring head: where the oldest sample sits */
    uint32_t count;
    bool pending_break;
};

struct mesh_ui_history {
    /* The radio we are attached to. */
    struct mesh_ui_airtime airtime;
    /* The history clock at the last DeviceMetrics airtime, so a LocalStats arriving beside that
       stream is not a second sample of the same minute. Meaningful only while
       `has_metrics_airtime`. */
    uint32_t metrics_airtime_at;
    bool has_metrics_airtime;
    struct mesh_ui_history_node nodes[MESH_UI_HISTORY_NODES];
    /*
     * The series themselves, handed out to the slots above.
     *
     * Which entries are in use is deliberately not recorded here: it is read off the slots, which
     * are the only things that can hold one. A `used` flag beside this would be the same fact
     * written twice, and the run where the two disagreed would be a node drawing another node's
     * temperature - the failure this whole module is arranged to make impossible. Taking an entry
     * happens once per node per reading, so what it costs is a scan nobody is waiting on.
     */
    struct mesh_ui_series pool[MESH_UI_HISTORY_SERIES];
    /*
     * What the caller's clock has to be shifted by to land on this history's own timeline, and
     * the shift a resumed history has not worked out yet.
     *
     * A sample is stamped with CLOCK_MONOTONIC, which counts from *boot* - so a series restored
     * from the cache carries times from a clock that no longer exists, and the first live push
     * after it would be a reading from before the oldest one we hold. mesh_ui_series_push()
     * reads that as the clock having gone backwards and empties the series, which is right for
     * what it can see and would silently undo the whole restore.
     *
     * So the restored samples define the timeline and the live clock is fitted to *them*:
     * mesh_ui_history_resume() says where the next reading goes, the first note_* after it
     * works out the difference, and everything after that rides the same offset. Modular
     * arithmetic on purpose - the offset is a uint32 difference, so it is correct wrapped and
     * the sum lands exactly on the target.
     */
    uint32_t clock_offset_ms;
    uint32_t resume_target_ms;
    bool resume_pending;
};

/* Empties everything and states the per-source gaps. Call before the first note. */
void mesh_ui_history_reset(struct mesh_ui_history *history);

/*
 * The radio's own report of how much of the air is in use and how much of that is ours, at
 * `now_ms` on the client's clock, from its LocalStats.
 *
 * Not taken while DeviceMetrics for our own node are arriving
 * (mesh_ui_history_note_metrics_airtime()): the two carry the same figures, and the minute stream
 * is the one the chart is shaped for. LocalStats is the fallback for a radio that does not send
 * it.
 */
void mesh_ui_history_note_airtime(struct mesh_ui_history *history, uint32_t now_ms,
                                  int32_t utilization_permille, int32_t tx_permille);

/* The same pair, from our own node's DeviceMetrics - the once-a-minute source. */
void mesh_ui_history_note_metrics_airtime(struct mesh_ui_history *history, uint32_t now_ms,
                                          int32_t utilization_permille, int32_t tx_permille);

/* How many airtime readings are held, the `index`th oldest (NULL past the end), and the newest
   (NULL when there are none). */
uint32_t mesh_ui_history_airtime_count(const struct mesh_ui_history *history);
const struct mesh_ui_airtime_sample *
mesh_ui_history_airtime_at(const struct mesh_ui_history *history, uint32_t index);
const struct mesh_ui_airtime_sample *
mesh_ui_history_airtime_newest(const struct mesh_ui_history *history);

/*
 * A node's battery level, in whole percent as the radio reports it (101 is "plugged in", which
 * is not a level and is refused here rather than drawn as a reading above full).
 *
 * A node with no slot takes the least recently heard from, which is the one whose trend has
 * least left to say. Our own node is a node like any other here: it reports its battery through
 * the same telemetry every other node does.
 */
void mesh_ui_history_note_battery(struct mesh_ui_history *history, uint32_t now_ms,
                                  uint32_t node_id, uint8_t battery_level);

/*
 * A node's air, from the EnvironmentMetrics half of its telemetry: temperature in tenths of a
 * degree Celsius and humidity in permille, each with a flag saying whether the node reported it.
 *
 * One call for the pair rather than one per reading, for mesh_ui_history_note_airtime()'s
 * reason: they arrive in one Telemetry, and two series stamped a publish apart would draw one
 * moment as two. The flags are what keeps that honest when only half of it is there - a node
 * with a thermometer and no hygrometer is the ordinary case, and pushing a zero for the reading
 * it does not have would draw a flat line at freezing rather than no line at all.
 *
 * A node with neither takes no slot: claiming one to record that a node reports nothing is how
 * the node somebody is actually watching gets evicted.
 */
void mesh_ui_history_note_environment(struct mesh_ui_history *history, uint32_t now_ms,
                                      uint32_t node_id, bool has_temperature,
                                      int32_t temperature_decidegrees, bool has_humidity,
                                      int32_t humidity_permille);

/*
 * How this node's last packet reached us, for the two readings that are about the link rather
 * than about the node: its signal-to-noise ratio in whole decibels, and how loud it was in dBm.
 *
 * One call for the pair, for mesh_ui_history_note_environment()'s reason - they are two
 * measurements of one packet, and two series stamped a publish apart would draw one arrival as
 * two. `has_rssi` is the flag that keeps that honest when only half of it is there: not every
 * radio reports a received strength, and pushing a zero for the one it did not measure would
 * draw a line at the loudest reading the scale has.
 *
 * **Both readings have to be about this node's own link, and that is the caller's to establish**
 * - mesh_ui_node_signal_heard() is the one answer to it. A packet that reached us through a
 * relay carries the *relay's* SNR, and one that came over somebody's MQTT bridge crossed no air
 * at all; either is a true number about something else, and a number about something else is
 * exactly what a trend makes look like evidence. The node detail draws the bar under these two
 * rows on the same condition, so the picture and the line it opens into are one claim.
 *
 * The cadence is the one thing here that is not a schedule. A node's telemetry arrives on a
 * timer; its packets arrive when it has something to say, which on a quiet mesh is a NodeInfo
 * every few minutes and on a busy one is constant. The gap the series breaks at is still the
 * node gap, which is the right length for the question it is answering - four missed telemetry
 * reports and a node that has gone quiet for two hours are the same silence to look at.
 */
void mesh_ui_history_note_signal(struct mesh_ui_history *history, uint32_t now_ms, uint32_t node_id,
                                 int32_t snr_db, bool has_rssi, int32_t rssi_dbm);

/*
 * Whether the radio's airtime has been reported enough to chart: two readings on different ticks
 * of the clock, which is a window with a width to lay columns across.
 *
 * Two readings rather than a drawable line segment, which is what this asked when the chart was
 * lines. A column is a mark on its own, so two reports either side of a silence are two columns
 * with the silence visibly between them - a picture, where two line ends were not.
 *
 * It is a question of the history rather than of a series because three places ask it - the verb
 * table that offers the chart, the action bar that names the press, and the clamp that closes
 * the screen when it empties - and three of them working it out by hand is three chances to
 * disagree about whether a screen exists.
 */
bool mesh_ui_history_has_airtime(const struct mesh_ui_history *history);

/*
 * That node's trend of that reading, or NULL when nothing has been kept for it. Borrowed: it
 * lives as long as the history does, which for a snapshot is the frame being drawn from it.
 *
 * Keyed on the reading rather than three functions named after them, because by the time this
 * is asked the reading is usually a value somebody is holding - the row the cursor is on, the
 * chart the nav has open - and a caller that had to switch on it to pick a function would be
 * deciding which readings exist for the second time.
 *
 * "Kept" means a sample, not a drawable line. Whether the readings can be *stroked* is the
 * further question mesh_ui_history_has_airtime() answers for the radio's own pair, and it is
 * asked here where the drawing is decided - see rows_trend() in src/ui/node_detail.c, which is
 * the one place a reading becomes a picture and a press.
 */
const struct mesh_ui_series *mesh_ui_history_series(const struct mesh_ui_history *history,
                                                    uint32_t node_id,
                                                    enum mesh_ui_history_reading reading);

/*
 * Drops everything about the radio and its mesh.
 *
 * A different radio is a different mesh with different nodes behind the same node numbers, so
 * carrying a trend across a swap would draw one node's battery as another's. Called for the
 * same event mesh_session_forget_nodes() is, and for the same reason.
 */
void mesh_ui_history_forget(struct mesh_ui_history *history);

/*
 * Puts one saved airtime reading back, at `time_ms` on the history's *own* timeline.
 *
 * Separate from note_airtime() because the two answer different questions about the clock. A
 * note is a reading arriving now, so it is stamped with the caller's clock and shifted onto
 * this timeline; a restore *is* the timeline, so it is placed exactly where it is told. Call
 * these oldest first, then mesh_ui_history_resume() once.
 */
void mesh_ui_history_restore_airtime(struct mesh_ui_history *history, uint32_t time_ms,
                                     int32_t utilization_permille, int32_t tx_permille, bool gap);

/*
 * Closes a restore: the next reading lands `seam_ms` after the newest sample held, and starts a
 * segment of its own.
 *
 * Both halves are deliberate and either alone would do it. The break is armed explicitly
 * because how long the client was *not running* is unknowable on a Brick - it has no RTC, so
 * there is no clock that can measure the seam - and a line sloping across it would be the one
 * claim the history has no evidence for. The seam is then the shortest silence that is already
 * a break (MESH_UI_HISTORY_RADIO_GAP_MS at the call site), so the gap drawn on the axis and the
 * pen lifted over it say the same thing whichever rule a reader believes.
 */
void mesh_ui_history_resume(struct mesh_ui_history *history, uint32_t seam_ms);

/*
 * Where a reading arriving at `now_ms` would land on this history's own timeline.
 *
 * The const half of mesh_ui_history_stamp(): it answers the same question without resolving a
 * pending resume, because the caller asking is not a reading arriving. A node restore asks it
 * to find out how far behind the live clock it may place what it read off the card, which is
 * the one thing about the timeline that nothing outside this file could work out - the offset
 * is private and the resume may not have been spent yet.
 */
uint32_t mesh_ui_history_stamp_now(const struct mesh_ui_history *history, uint32_t now_ms);

/*
 * The newest moment anything has been recorded about this node, across every reading it holds.
 *
 * False when the node has no slot or nothing in it. The readings are one node's, so they are
 * one timeline, which is what makes a single answer meaningful - a restore uses it to lay the
 * card's copy of this node's trend down so that its last record sits exactly where this run
 * already has one, rather than drawing a seam where nothing was interrupted.
 */
bool mesh_ui_history_node_newest(const struct mesh_ui_history *history, uint32_t node_id,
                                 uint32_t *out_time);

/*
 * The three calls a node restore is made of: what this node holds is dropped, saved readings go
 * back one at a time at times on this history's *own* timeline, and the seam is armed.
 *
 * The same shape as mesh_ui_history_restore_airtime() and for the same reasons, with one
 * difference that is the whole of why the first call exists. The airtime is restored into an
 * empty history at launch; a node's trend is restored into a running one, when its detail screen
 * is opened, and by then the slot may already hold readings this run took. A restore is
 * therefore a replacement rather than an addition: the log on the card holds everything this run
 * has pushed as well (store_trends.c appends on every publish), so keeping both would draw every
 * reading of this session twice.
 *
 * Call reset once, then restore oldest first, then resume once - and resume only when the
 * restored readings do *not* run up to what this run already had, because that is the case
 * where how long ago they were taken is unknowable. See mesh_ui_trends_restore().
 */
void mesh_ui_history_restore_node_reset(struct mesh_ui_history *history, uint32_t node_id);
void mesh_ui_history_restore_node(struct mesh_ui_history *history, uint32_t node_id,
                                  enum mesh_ui_history_reading reading, uint32_t time_ms,
                                  int32_t value, bool gap);
void mesh_ui_history_resume_node(struct mesh_ui_history *history, uint32_t node_id);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_HISTORY_H */
