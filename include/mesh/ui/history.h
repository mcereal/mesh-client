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
 *   - **The radio's pair is persisted and a node's trends are not.** A trend is what we
 *     *watched*, so a resumed one must not put a line over a period nothing observed - but that
 *     is an argument for drawing the seam as a seam, not for throwing the readings away, and
 *     mesh_ui_history_resume() is what lifts the pen over it. The radio's pair earns the cache
 *     because of its cadence: LocalStats arrives every fifteen minutes and two readings make a
 *     line, so starting empty left the Status tab's chart unoffered for the first half hour of
 *     every session. A node's readings keep the old rule for now and have the same problem on a
 *     half-hour cadence.
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
 * How often each source's readings actually arrive, and how long a silence has to be before a
 * line breaks rather than sloping across it.
 *
 * The cadence is stated first and the gap is a multiple of it, because the gap is not a duration
 * anybody picked: it is "a few reports missed", and writing it as a bare number is how it comes
 * to be *one* report - which is a series that breaks at every sample and so never has a line in
 * it at all. The radio's was exactly that. LocalStats reaches the attached client on the
 * firmware's `sendStatsToPhoneIntervalMs`, which is fifteen minutes, and the gap was fifteen
 * minutes - and the two are never equal in practice, because that throttle is tested on a
 * once-a-minute thread tick and only on the turns the module is not sending telemetry to the
 * mesh instead. Consecutive reports therefore land a little *over* fifteen minutes apart, every
 * sample starts a segment of its own, and mesh_ui_history_has_airtime() answered false for the
 * life of every session: the Mesh card was offered no verb, its chart could not be opened, and
 * the Status cursor walked from the Link card past it to the Radio one. On hardware only - a
 * capture scene stamps its readings on the harness clock, a few hundred milliseconds apart, so
 * every still and every film of this screen showed a button the device never drew.
 *
 * Three reports rather than two, because a report is skipped as well as delayed: the module
 * sends its stats only on the turns it is not broadcasting telemetry to the mesh and only while
 * the phone's queue is empty, so a spacing of two cadences is ordinary and a gap of two would
 * break on it. A node's device metrics ride that same broadcast instead, which is half an hour
 * at the firmware's default and longer on anything running on a battery it cares about - four
 * of those, on the same reasoning, and the value it already had.
 */
#define MESH_UI_HISTORY_RADIO_REPORT_MS (15U * 60U * 1000U)
#define MESH_UI_HISTORY_NODE_REPORT_MS (30U * 60U * 1000U)

#define MESH_UI_HISTORY_RADIO_GAP_MS (3U * MESH_UI_HISTORY_RADIO_REPORT_MS)
#define MESH_UI_HISTORY_NODE_GAP_MS (4U * MESH_UI_HISTORY_NODE_REPORT_MS)

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
    MESH_UI_HISTORY_READING_COUNT
};

/*
 * One node's slot, and every trend kept for it.
 *
 * The three series share a slot rather than having tables of their own, and that is a statement
 * about what a slot *is*: it is a node somebody is watching, not a reading. A sensor node
 * reports its battery and its air in the same telemetry, so splitting them would be two
 * evictions racing over one node - and a node whose temperature survived while its battery was
 * evicted would draw half a screen of trend and half a screen of nothing, for no reason the
 * reader could see.
 *
 * `seen` is still one stamp for the slot, so any reading arriving keeps the whole node fresh.
 * That is the right way round: what the eviction is protecting is the node being looked at.
 */
struct mesh_ui_history_node {
    uint32_t node_id; /* 0 for a free slot */
    uint32_t seen;    /* the clock at the last push, so the least recent slot can be evicted */
    struct mesh_ui_series battery;
    struct mesh_ui_series temperature;
    struct mesh_ui_series humidity;
};

struct mesh_ui_history {
    /* The radio we are attached to, from its own LocalStats report. Both in permille of the
       air, which is the unit mesh_ui_percent_permille() puts the wire's floats on and the unit
       the Status card's meter already reads. */
    struct mesh_ui_series channel_utilization;
    struct mesh_ui_series air_util_tx;
    struct mesh_ui_history_node nodes[MESH_UI_HISTORY_NODES];
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
 * `now_ms` on the client's clock.
 *
 * One call for the pair rather than one per series, because they arrive in one LocalStats and
 * two series stamped a publish apart would draw two readings of one moment as two moments.
 */
void mesh_ui_history_note_airtime(struct mesh_ui_history *history, uint32_t now_ms,
                                  int32_t utilization_permille, int32_t tx_permille);

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
 * Whether the radio's airtime has been reported often enough to draw a line between.
 *
 * A drawable *segment* rather than two readings, which is not the same test and is the way this
 * was first written wrong: every sample that follows a silence the series calls a break starts a
 * line rather than continuing one, so two reports either side of a link that was down for a
 * quarter of an hour are two samples the ring holds and no stroke at all. Counted, the verb
 * offers a chart with axes, a legend and nothing between them.
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

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_HISTORY_H */
