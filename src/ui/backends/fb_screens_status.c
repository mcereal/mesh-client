#define _POSIX_C_SOURCE 200809L

/*
 * The Status tab: the link, the radio and the mesh, as three cards.
 *
 * Each card names its subject and reports on it in its own heading colour, so "is anything
 * wrong" is answered by the shape and the colour before a number has been read. The thresholds
 * that decide those colours are stated once at the top of this file, because a card that heads
 * itself "fine" over a row it has just drawn as a warning is the one failure a card cannot
 * survive - and one of them, fb_air_band, is ruled across the chart this screen opens.
 */

#include "inkcell/ui/layout.h"
#include "inkcell/ui/widgets.h"
#include "inkwell/base/text.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/delivery.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/history.h"
#include "mesh/ui/nodes.h"
#include "mesh/ui/reach.h"
#include "mesh/ui/status.h"
#include "mesh/ui/trust.h"

#include <stdio.h>
#include <string.h>

/* "3d 4h", "5h 12m", "40m" - a radio's uptime, which is a duration rather than an age. */
static void fb_format_uptime(uint32_t seconds, char *out, size_t out_len) {
    if (seconds >= 86400U) {
        inkcell_str_format(out, out_len, MESH_STR_TIME_DAYS_HOURS, seconds / 86400U,
                           (seconds % 86400U) / 3600U);
    } else if (seconds >= 3600U) {
        inkcell_str_format(out, out_len, MESH_STR_TIME_HOURS_MINUTES, seconds / 3600U,
                           (seconds % 3600U) / 60U);
    } else {
        inkcell_str_format(out, out_len, INKCELL_STR_TIME_MINUTES_SHORT, seconds / 60U);
    }
}

/* Our own node's record, which is where the connected radio's battery and airtime live: those
   arrive as ordinary DeviceMetrics telemetry, not in LocalStats. NULL before the sync. */
static const struct mesh_ui_node_summary *fb_self_node(const struct mesh_ui_snapshot *snapshot) {
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    if (!snapshot->handshake_valid || !hs->has_my_info) {
        return NULL;
    }
    for (uint32_t i = 0; i < hs->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        if (hs->nodes[i].node_id == hs->my_info.node_num) {
            return &hs->nodes[i];
        }
    }
    return NULL;
}

/*
 * Where a busy mesh stops being healthy, in permille of the air.
 *
 * Not a look-and-feel number: above roughly a quarter, LoRa's listen-before-talk backs
 * everything off and multi-hop delivery starts failing outright, and by half the mesh is
 * effectively a single-hop one.
 *
 * One band rather than two constants, and it is handed to the bar rather than consulted
 * alongside it. Four things on this card now read these two numbers - the figure's colour, the
 * card heading's, the bar's fill and the notches cut into the bar's track - and the last of
 * those is the reason the shape changed: a threshold that is drawn has to be the same threshold
 * that is compared, or the screen is marking one boundary and colouring another.
 *
 * The numbers themselves moved to layout.h once a node's own detail screen started reading
 * them too. This is the band they make; where the mesh's limits actually are is stated there.
 */
const struct inkcell_band fb_air_band = {.warn = INKCELL_AIRTIME_BUSY_WARN,
                                         .bad = INKCELL_AIRTIME_BUSY_BAD};

/*
 * Where a radio's free heap stops being comfortable, in bytes.
 *
 * Stated here for the reason the pair above is: the figure's colour and the card heading's are
 * two readings of one number, and a threshold written out at each of them is a card that can
 * head itself "fine" over a row it has just drawn as a warning.
 */
#define FB_HEAP_LOW_BYTES 20480U

/*
 * The airtime meter's key in the animation table.
 *
 * At the top of the range with the snackbar's, and for the same reason: every other id in here
 * is a row index or a field, handed in by a list that has many of them, and this is a control a
 * screen has exactly one of.
 */
#define FB_ANIM_ID_AIRTIME 0xFFFFFF02U

/*
 * The verbs one card offers, hung on the card that offers them.
 *
 * The flat list is walked rather than the card asked what it wants, so the order the buttons
 * draw in is the order the cursor walks them by construction - see include/mesh/ui/status.h.
 * `focus` is the verb the cursor is on; the button naming it is the one that draws filled, and
 * a card holding it draws its focus ring.
 *
 * A *verb* rather than a position, and that is what stops this screen drawing the highlight in
 * one place while A runs something else. A position is only true of the list it was read
 * against, and the list here is a function of the link: a snapshot taken across a radio going
 * away is a cursor counted on one list and drawn on another.
 */
static void fb_status_card_actions(struct inkcell_fb_card *card,
                                   const struct mesh_ui_status_actions *actions,
                                   enum mesh_ui_status_card which, uint8_t focus) {
    /*
     * The buttons are registered for a pointer, in the rows block at their place in the flat
     * list - the same index the d-pad walks, so a click and a press name one verb. They were
     * drawn and never registered while every verb here had a keycap somewhere else too; the
     * device list's is now the only way into that list, and a window has no d-pad.
     */
    uint32_t first = 0U;
    if (mesh_ui_status_card_actions(actions, which, &first) > 0U) {
        card->action_focus_id = (uint32_t)MESH_UI_FOCUS_ROWS + first;
    }
    for (uint32_t i = 0U; i < actions->count && i < MESH_UI_STATUS_ACTIONS_MAX; ++i) {
        if (actions->items[i].card != (uint8_t)which) {
            continue;
        }
        inkcell_fb_card_action(card, actions->items[i].label, actions->items[i].verb == focus);
    }
}

/*
 * The narrowest a column of cards may be, in columns of body text: the Mesh card's airtime line
 * beside its label, which is the widest row any card writes, with a little over.
 *
 * In columns rather than pixels for the width classes' reason: the reader's scale is already in
 * the unit, so a window with the text turned up keeps one column for exactly as long as two
 * would wrap. Below it the cards still draw - a row wraps - but two columns of wrapped rows are
 * taller than one column of whole ones, and the second column bought nothing.
 */
#define FB_STATUS_COLUMN_COLS 44U

/*
 * Whether the cards stand in two columns, and where.
 *
 * One column is a phone's answer, and on the Brick it is the only one: the panel is a measure
 * wide. On a window it was a ribbon of three cards down the middle of a desktop with most of the
 * glass either side of it empty, because a card is placed against the reading column and the
 * reading column is capped. The cards are not running text - they are label and value rows read
 * a line at a time - so the measure is the wrong cap for them, and what the room is for is the
 * second column a dashboard on any wider screen has.
 *
 * Two halves of the region, each inset by its own margins, which leaves the gap between them the
 * same two margins a card leaves between itself and the panel edge. Only above the compact class,
 * and only when both halves hold FB_STATUS_COLUMN_COLS; at the Brick's scale that is the window
 * the other tabs split at, so a window becomes two-pane everywhere at once.
 */
static bool fb_status_columns(struct inkcell_draw_state *state, struct inkcell_box columns[2]) {
    if (inkcell_fb_width_class(state) == INKCELL_WIDTH_COMPACT) {
        return false;
    }
    const struct inkcell_box region = inkcell_fb_region(state);
    const int half = region.w / 2;
    columns[0] = (struct inkcell_box){region.x, region.y, half, region.h};
    columns[1] = (struct inkcell_box){region.x + half, region.y, region.w - half, region.h};
    const struct inkcell_box was = inkcell_fb_set_region(state, columns[0]);
    const bool room = inkcell_fb_cols(state, state->scale) >= FB_STATUS_COLUMN_COLS;
    (void)inkcell_fb_set_region(state, was);
    return room;
}

/*
 * The cards the Status tab draws, and the handful of figures the board's tiles read beside them.
 * Built once a frame by fb_status_build() and laid out by whichever of the two layouts has room.
 */
struct fb_status_cards {
    struct inkcell_fb_card link;
    struct inkcell_fb_card broker;
    bool have_broker;
    struct inkcell_fb_card mesh;
    struct inkcell_fb_card radio;
    /* The Radio card's report on itself, which also decides how much room it claims. */
    enum inkcell_tone radio_tone;

    /* The channel: the same reading, tone and band the Mesh card's airtime row is drawn in. */
    bool have_util;
    int32_t util_permille;
    enum inkcell_tone air_tone;
    char util[16]; /* "18.0%", or empty */
    char tx[16];   /* our share, or empty */
    /* The database, and how much of it LocalStats calls online. */
    uint32_t known;
    bool have_online;
    uint32_t online;
    /* The Radio card's battery figure, and its uptime on its own. */
    char battery[64];
    bool low_battery;
    char uptime[32];
};

/*
 * The four cards, built and not drawn - and the figures the board's tiles read off the same
 * sources.
 *
 * Built apart from where they are drawn because there are now two places: the column (or two)
 * the handheld and a narrow window get, and the board a wide one does. Both draw *these* cards,
 * so a row, a tone and a verb are decided once - a board that built its own Mesh card would be a
 * second opinion about what the mesh is doing, and the first thing it would disagree about is a
 * threshold.
 */
static void fb_status_build(const struct mesh_ui_snapshot *snapshot, struct fb_status_cards *out) {
    memset(out, 0, sizeof *out);
    char buffer[64];
    char second[64];

    /* ---- the link: what we are talking to, and whether it has told us who it is ---- */

    const struct mesh_ui_device *connected = mesh_ui_snapshot_connected_device(snapshot);

    /*
     * The verbs on offer and which of them the cursor is on. Both come from the same table
     * nav.c walks and the action bar names, so the button that draws filled here is the one A
     * will run - see include/mesh/ui/status.h.
     */
    struct mesh_ui_status_actions actions;
    mesh_ui_status_actions(&actions, connected != NULL, snapshot->handshake_valid,
                           mesh_ui_history_has_airtime(&snapshot->history));
    /* Resolved rather than read straight off the nav: the nav is clamped against the store and
       this is drawn from a snapshot, so a verb that has gone since would leave no button
       highlighted at all. mesh_ui_status_verb_resolve() answers with the one the cursor stands
       on now, which is the same answer nav.c's own clamp reached. */
    const uint8_t focus = mesh_ui_status_verb_resolve(&actions, snapshot->nav.status_verb);

    /*
     * Elevated, always. It is the first question the screen answers - is there a radio - and
     * every number on the two cards below it is about a link this one says whether we have; a
     * column of equal weights was the audit's complaint about this screen.
     */
    inkcell_fb_card_begin(&out->link, INKCELL_FB_CARD_ELEVATED, INKCELL_ICON_LINK,
                          MESH_STR_STATUS_CARD_LINK,
                          connected != NULL ? INKCELL_TONE_SUCCESS : INKCELL_TONE_ERROR);
    /*
     * The transport and the radio it found - but only while nothing else on the frame is
     * saying them.
     *
     * This is the banner's rule arriving on a card row, and it is the same two expressions
     * rather than the same two facts: fb_link_summary() builds the line under the keycaps from
     * `transport_status` and fb_device_label() of the connected device, on every frame of every
     * screen. With a radio attached that line reads "running: Home Base" and these two rows say
     * it again a dozen rows further up, at the top of the one column on this client that runs
     * out of room - so they were being paid for twice and read once.
     *
     * With no radio the line says only the transport state, not a device, so the rows are back and
     * the card is where "not connected" is written. Which is the whole of the rule: a row says
     * only what nothing else on the frame says, and whether anything else is saying it is a
     * question about the state rather than about the row.
     */
    if (connected == NULL) {
        inkcell_fb_card_row_text(&out->link, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_TRANSPORT,
                                 snapshot->transport_status[0] != '\0'
                                     ? snapshot->transport_status
                                     : inkcell_str(MESH_STR_HEADER_TRANSPORT_STARTING));
        inkcell_fb_card_row_text(&out->link, INKCELL_TONE_ERROR, MESH_STR_STATUS_LABEL_RADIO,
                                 inkcell_str(MESH_STR_STATUS_NOT_CONNECTED));
    }
    if (snapshot->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
        /* A running sync says how far along it is rather than only that it is running: the
           replay is most of the wait, and on a link that keeps dropping mid-roster the count is
           the difference between "this is working" and "this has stalled again". It needs the
           radio's own total to count against, so a sync that has not reached MyNodeInfo yet
           still says just "in progress". */
        if (hs->request_in_flight && !hs->config_complete && hs->has_my_info &&
            hs->my_info.nodedb_entries > 0U) {
            inkcell_fb_card_row(&out->link, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_SYNC,
                                MESH_STR_STATUS_SYNC_PROGRESS, hs->sync_nodes,
                                hs->my_info.nodedb_entries);
        } else {
            inkcell_fb_card_row(&out->link, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_SYNC,
                                MESH_STR_STATUS_SYNC_VALUE,
                                inkcell_str(hs->config_complete ? MESH_STR_STATUS_SYNC_COMPLETE
                                            : hs->request_in_flight
                                                ? MESH_STR_STATUS_SYNC_IN_PROGRESS
                                                : MESH_STR_STATUS_SYNC_IDLE),
                                hs->cached ? inkcell_str(MESH_STR_STATUS_SYNC_CACHED) : "");
        }
        if (hs->has_my_info) {
            inkcell_fb_card_row(&out->link, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_MY_NODE,
                                MESH_STR_STATUS_MY_NODE, hs->my_short_name, hs->my_info.node_num);
        }
        if (hs->primary_channel[0] != '\0') {
            inkcell_fb_card_row(&out->link, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_CHANNEL,
                                MESH_STR_STATUS_CHANNEL_NAME, hs->primary_channel);
        }
    } else {
        inkcell_fb_card_row_text(&out->link, INKCELL_TONE_DIM, MESH_STR_STATUS_LABEL_SYNC,
                                 inkcell_str(MESH_STR_STATUS_SYNC_WAITING));
    }
    /* What else is within reach, which is the same subject as what we are attached to - and on
       a screen where the radio is gone it is the row that says whether anything is there. The
       row says "in range" and means it: the Devices tab also lists radios BlueZ is merely
       bonded to, and counting those here would report a node that is at home as reachable. */
    size_t devices_in_range = 0U;
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        if (snapshot->devices[i].in_range) {
            ++devices_in_range;
        }
    }
    inkcell_fb_card_row(&out->link, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_DEVICES,
                        MESH_STR_STATUS_DEVICES_IN_RANGE, devices_in_range);
    fb_status_card_actions(&out->link, &actions, MESH_UI_STATUS_CARD_LINK, focus);

    /*
     * ---- the broker: the second link, and only when a radio has asked for one ----
     *
     * Drawn on `wanted` alone, which is the radio's own configuration rather than anything about
     * this client's socket. The card is four rows of a column that runs out of room, and almost
     * no radio has client proxying turned on - so on almost every Brick this is nothing at all,
     * and on the ones where it matters it is the only place the answer exists. A card saying
     * "Off" on every other device would have been the worst of both.
     *
     * Everything here is about a connection the *radio* cannot see. The radio hands over a
     * message and is told nothing about what happened to it - `publishQueuedMessages()` runs
     * every 200 ms whether or not a broker is reachable - so a proxy that is failing looks, from
     * the radio and from every other screen on this client, exactly like one that is working.
     */
    if (snapshot->mqtt.wanted) {
        out->have_broker = true;
        const struct mesh_ui_mqtt_state *mqtt = &snapshot->mqtt;
        /*
         * The heading's tone is the whole card in one colour: green once the broker has accepted
         * us, red while an attempt has failed and another is scheduled, and neutral for the
         * handful of seconds a connection spends resolving, connecting and signing in. The
         * client declining outright is red too - it is not a transient state and it is not
         * working.
         */
        const enum inkcell_tone broker_tone = mqtt->connected ? INKCELL_TONE_SUCCESS
                                              : (mqtt->failing || mqtt->disabled)
                                                  ? INKCELL_TONE_ERROR
                                                  : INKCELL_TONE_NORMAL;
        inkcell_fb_card_begin(&out->broker, INKCELL_FB_CARD_FILLED, INKCELL_ICON_MQTT,
                              MESH_STR_STATUS_CARD_BROKER, broker_tone);
        /*
         * Where, before what. It is the row the radio's own Settings screen cannot draw: an
         * empty `MQTTConfig.address` means the public broker, and that substitution happens on
         * this client - so the Server field over in Settings is blank while this says
         * mqtt.meshtastic.org, and the blank one is the one somebody has already looked at.
         */
        inkcell_fb_card_row_text(&out->broker, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_BROKER,
                                 mqtt->host);
        /*
         * How many times we have signed in, folded into the status row rather than given one of
         * its own. A proxy that connects, drops and reconnects every minute reads as "Connected"
         * on any single frame, and the count is the only thing on the screen that would say
         * otherwise - but it is worth nothing at 1, which is what a healthy connection says
         * forever.
         */
        if (mqtt->connections > 1U) {
            inkcell_fb_card_row(&out->broker, broker_tone, MESH_STR_STATUS_LABEL_BROKER_STATE,
                                MESH_STR_STATUS_BROKER_RECONNECTS, mqtt->state, mqtt->connections);
        } else {
            inkcell_fb_card_row_text(&out->broker, broker_tone, MESH_STR_STATUS_LABEL_BROKER_STATE,
                                     mqtt->state);
        }
        /*
         * And the sentence, directly under the status it elaborates rather than at the foot of
         * the card.
         *
         * A note rather than a row, because `last_error` names a host and a reason and runs past
         * any value column. Its *position* is the part that was got wrong first: rows are
         * clipped from the end, and on the crowded screen this card exists for, a sentence
         * declared last is a sentence cut off in the middle of the only words that answer the
         * question. The counters under it are what can afford to go.
         *
         * It stays on screen after a reconnect succeeds, which is deliberate - a link that flaps
         * is diagnosed by the reason it last failed, and clearing it on every CONNACK would
         * leave the card green with nothing to say about why it keeps going red.
         */
        if (mqtt->disabled) {
            inkcell_fb_card_note(&out->broker, INKCELL_TONE_ERROR,
                                 inkcell_str(MESH_STR_STATUS_BROKER_DISABLED));
        } else if (mqtt->last_error[0] != '\0') {
            inkcell_fb_card_note(&out->broker, INKCELL_TONE_ERROR, mqtt->last_error);
        }
        /*
         * Everything below reports on a connection, so none of it is drawn when this client was
         * told not to hold one. They would all be zero, and a zero here is not the same sentence
         * as the zero a running proxy reports - "none subscribed" in particular would put a
         * warning on the radio's channels for something the Brick decided.
         */
        if (!mqtt->disabled) {
            /*
             * Subscriptions, and none said in words rather than as a 0.
             *
             * This is the row for the fault with no error behind it: a radio whose channels all
             * have downlink off publishes perfectly and receives nothing, and every other row on
             * this card is green while it happens. Nothing logs it, because as far as MQTT is
             * concerned everything worked.
             */
            if (mqtt->subscriptions > 0U) {
                inkcell_fb_card_row(&out->broker, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_TOPICS,
                                    MESH_STR_STATUS_BROKER_TOPICS, mqtt->subscriptions);
            } else {
                inkcell_fb_card_row_text(&out->broker, INKCELL_TONE_WARNING,
                                         MESH_STR_STATUS_LABEL_TOPICS,
                                         inkcell_str(MESH_STR_STATUS_BROKER_NO_TOPICS));
            }
            /* What has actually crossed. Drawn from the first message either way, because "0
               out, 0 in" against a broker that says Connected is itself an answer - it means the
               radio has not offered anything yet, which is a different problem from a broker
               refusing. */
            inkcell_fb_card_row(&out->broker, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_RELAYED,
                                MESH_STR_STATUS_BROKER_RELAYED, mqtt->published, mqtt->received);
            /*
             * What did not cross, in the same shape, and only once something has not.
             *
             * The two halves are counted at opposite ends and are two different faults. Outbound
             * is the proxy refusing a publish, which on a broker that is down is every position
             * report the radio makes; inbound is a broker message that reached the radio's
             * doorstep and no further. A single total would have averaged a link that is down
             * with a message that was too big.
             */
            if (mqtt->dropped > 0U || mqtt->undelivered > 0U) {
                inkcell_fb_card_row(&out->broker, INKCELL_TONE_WARNING,
                                    MESH_STR_STATUS_LABEL_DROPPED, MESH_STR_STATUS_BROKER_DROPPED,
                                    mqtt->dropped, mqtt->undelivered);
            }
        }
        /* Messages the radio offered with nowhere to put them - the client declining, or a
           connection that was never made. Its own row because it is counted on the far side of
           the link from everything above, and it is the number that moves when this client is
           the thing that is wrong. */
        if (mqtt->unhandled > 0U) {
            inkcell_fb_card_row(&out->broker, INKCELL_TONE_WARNING, MESH_STR_STATUS_LABEL_MESSAGES,
                                MESH_STR_STATUS_BROKER_UNHANDLED, mqtt->unhandled);
        }
    }

    /*
     * ---- the mesh: how many nodes, and how much of the air they are using ----
     *
     * Mesh health comes from the two sources that carry it. LocalStats is the radio's own live
     * view, sent to the attached client on its own schedule; DeviceMetrics is what our node
     * last *broadcast* about itself, on the telemetry interval, which is half an hour by
     * default. Both carry the airtime pair, so LocalStats wins it when it has arrived and
     * DeviceMetrics only fills the gap before the first report - reading the broadcast copy by
     * preference means the row can sit on a half-hour-old 0.0% while the radio is busy.
     */
    const struct mesh_ui_node_summary *self = fb_self_node(snapshot);
    const struct mesh_ui_node_metrics *metrics =
        (self != NULL && self->metrics.valid) ? &self->metrics : NULL;
    const struct mesh_ui_radio_stats *stats = &snapshot->settings.stats;

    /* LocalStats' airtime fields are plain scalars the firmware always fills, so `valid` is the
       whole test; DeviceMetrics' are optional and carry their own has_*. */
    const bool air_from_stats = stats->valid;
    const bool have_util = air_from_stats || (metrics != NULL && metrics->has_channel_utilization);
    const bool have_tx = air_from_stats || (metrics != NULL && metrics->has_air_util_tx);
    const float util_value = air_from_stats
                                 ? stats->channel_utilization
                                 : (metrics != NULL ? metrics->channel_utilization : 0.0f);
    /*
     * Above ~25% channel utilization the mesh is saturated and hop delivery collapses, so the
     * number is coloured rather than left as one more figure to interpret - and the card's
     * heading takes the same tone, which is what makes a saturated mesh visible from the shape
     * of the screen rather than from reading a percentage.
     *
     * The thresholds are stated once, in fb_air_band, and answer for the figure's colour, the
     * heading's, the meter's fill and the marks on its track alike - see inkcell_band_tone(). A
     * screen that worked them out separately for the words and for the bar would be drawing a
     * picture and a number that can disagree, and the picture is the one that gets believed.
     */
    const int32_t util_permille = inkcell_percent_permille(util_value);
    const enum inkcell_tone air_tone =
        have_util ? inkcell_band_tone(&fb_air_band, util_permille, INKCELL_TONE_SUCCESS)
                  : INKCELL_TONE_NORMAL;

    /* Filled: the ordinary weight, and the middle of the three. The mesh is the subject of the
       screen once there is a link, but it is never the thing to read first. */
    inkcell_fb_card_begin(&out->mesh, INKCELL_FB_CARD_FILLED, INKCELL_ICON_NODES,
                          MESH_STR_STATUS_CARD_MESH,
                          air_tone != INKCELL_TONE_NORMAL ? air_tone : INKCELL_TONE_PRIMARY);
    if (snapshot->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
        /* One line for the NodeDB, and LocalStats' online count when the radio has sent it:
           "132 nodes" alone says nothing about how much of that mesh is still alive. */
        if (hs->has_my_info && stats->valid && stats->num_online_nodes > 0U) {
            inkcell_fb_card_row(&out->mesh, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_NODEDB,
                                MESH_STR_STATUS_NODEDB_ONLINE, hs->my_info.nodedb_entries,
                                stats->num_online_nodes);
        } else if (hs->has_my_info) {
            inkcell_fb_card_row(&out->mesh, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_NODEDB,
                                MESH_STR_STATUS_NODEDB_REBOOTS, hs->my_info.nodedb_entries,
                                hs->my_info.reboot_count);
        }
        /* Ours, next to the radio's, and only while the two differ. The row above counts the
           radio's database; this one counts the roster, which outlives it on purpose - so after
           a NodeDB reset one says 2 and the other 81 with nothing to explain it. Both numbers
           here are the published rows, so the second can never exceed the first. */
        const uint32_t off_radio = mesh_ui_handshake_off_radio(hs);
        if (off_radio > 0U) {
            inkcell_fb_card_row(&out->mesh, INKCELL_TONE_DIM, MESH_STR_STATUS_LABEL_CACHED_HERE,
                                MESH_STR_STATUS_CACHED_OFF_RADIO, hs->node_count, off_radio);
        }
        /* And the count that is not a window. Both rows above are capped and evict, so on a
           big enough mesh they read as a ceiling - "256 nodes" looks like everything ever
           found. This one only goes up, and is drawn whenever there is anything in it. */
        if (hs->nodes_heard_ever > 0U) {
            inkcell_fb_card_row(&out->mesh, INKCELL_TONE_DIM, MESH_STR_STATUS_LABEL_HEARD_EVER,
                                hs->nodes_heard_ever_floor ? MESH_STR_STATUS_HEARD_EVER_FLOOR
                                                           : MESH_STR_STATUS_HEARD_EVER,
                                hs->nodes_heard_ever, hs->nodes_heard_ever_rf);
        }
    }

    if (have_util || have_tx) {
        const float tx_value =
            air_from_stats ? stats->air_util_tx : (metrics != NULL ? metrics->air_util_tx : 0.0f);
        char util[32];
        inkwell_str_copy(util, sizeof util, inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT));
        if (have_util) {
            inkcell_str_format(util, sizeof util, MESH_STR_STATUS_PERCENT, (double)util_value);
        }
        char tx[32];
        inkwell_str_copy(tx, sizeof tx, inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT));
        if (have_tx) {
            inkcell_str_format(tx, sizeof tx, MESH_STR_STATUS_PERCENT, (double)tx_value);
        }
        if (stats->valid && stats->has_noise_floor) {
            inkcell_fb_card_row(&out->mesh, air_tone, MESH_STR_STATUS_LABEL_AIRTIME,
                                MESH_STR_STATUS_AIRTIME_FLOOR, util, tx, stats->noise_floor);
        } else {
            inkcell_fb_card_row(&out->mesh, air_tone, MESH_STR_STATUS_LABEL_AIRTIME,
                                MESH_STR_STATUS_AIRTIME, util, tx);
        }
        /*
         * Two rows about this figure and no third, which is a change from what shipped here.
         *
         * There were three: the words, this bar, and a full-width trend line under it. The line
         * was right while it was the *whole* of what the client could say about a direction -
         * and it stopped being that when the chart arrived. `trend` on the heading above opens
         * the same readings with their axes labelled, their span named, their thresholds ruled
         * across the plot and our own transmit share beside them, which is every question the
         * line could answer and four it could not. So the shape moved to the screen built for
         * it, the verb on the heading is the entrance - and an entrance costs no row where the
         * line cost two, on the one card here that had none to spend.
         *
         * What is left is the pair, and the pair is the point: the words say how busy, and the
         * bar says whether that is a lot. Neither replaces the other. A percentage has to be
         * read and then held against a threshold nobody carries around - "is 31% a lot?" -
         * where a bar a third full is compared against the track it sits in, which is right
         * there. The band goes with it, so the track carries a notch at a quarter and one at a
         * half, and "a bar a third full" becomes "a bar past the first mark".
         *
         * Only when there is a real reading. A track drawn empty because the radio has not
         * reported yet says the mesh is quiet, which is a different claim from saying nothing.
         *
         * No label: the row above already names it twice over, and a label column here would
         * cost the track the third of its length that makes a fill readable as a proportion.
         *
         * A zeroed scale: this reading is already permille, so there is no domain to state.
         */
        if (have_util) {
            inkcell_fb_card_meter(&out->mesh, INKCELL_TONE_SUCCESS, INKCELL_STR_NONE, util_permille,
                                  (struct inkcell_scale){0, 0}, &fb_air_band, FB_ANIM_ID_AIRTIME);
        }
    }

    if (stats->valid) {
        /*
         * The counters, split by direction - which is a change from what shipped here, and the
         * change is that each number is now named once.
         *
         * There were three rows: Packets (tx, rx, relayed), Dropped (bad rx, dupe, tx dropped)
         * and Heard (new, dupe, bad). The middle one restated two of the third's three parts
         * three rows further up, in a different order, under a heading that read as a fault -
         * so the same duplicate count was amber on one row because something was going wrong
         * and neutral on the next because it was a share. Both answers were wanted and neither
         * needed the other's row.
         *
         * Sent is the transmit side whole, and Heard is the receive side whole. What made the
         * old split incoherent is that "dropped" was never one subject: a malformed packet is
         * something we *heard*, and a packet the radio could not send is something we did not.
         *
         * No bar under this one, and that is §2.17's rule rather than a gap: `num_tx_relay` is
         * documented as a subset of `num_packets_tx` rather than a sibling of it, so tx,
         * relayed and dropped add up to a whole that does not exist. A composition drawn from
         * overlapping parts is the one way that component is wrong quietly.
         *
         * Both rows take their tone from a *share*, and the thresholds differ because the two
         * things do: one in a hundred here, half below. These are lifetime counters since the
         * radio booted, so anything read off an absolute count lights once and then stays lit
         * for the rest of the connection - which is what the Dropped row this replaces did with
         * twelve malformed packets in six thousand. A ratio recovers as the radio runs well,
         * which is the behaviour a colour on a running total has to have to mean anything.
         *
         * The live half of this is already elsewhere and deliberately stays there: the Radio
         * card's TX queue row goes to the error family the moment the radio is refusing sends
         * *now*, which is the alarm. This is the tally, and a tally's job is proportion.
         */
        if (stats->has_routes) {
            /* A MeshCore radio's pair: totals and routing, with no partition to draw. Only the
               bad share is coloured, on the threshold the Heard row below uses. */
            inkcell_fb_card_row(&out->mesh, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_SENT,
                                MESH_STR_STATUS_SENT_ROUTES, stats->num_packets_tx,
                                stats->sent_flood, stats->sent_direct);
            inkcell_fb_card_row(&out->mesh,
                                (uint64_t)stats->num_packets_rx_bad * 2U > stats->num_packets_rx
                                    ? INKCELL_TONE_WARNING
                                    : INKCELL_TONE_NORMAL,
                                MESH_STR_STATUS_LABEL_HEARD, MESH_STR_STATUS_HEARD_ROUTES,
                                stats->num_packets_rx, stats->recv_flood, stats->recv_direct,
                                stats->num_packets_rx_bad);
        } else {
            inkcell_fb_card_row(&out->mesh,
                                (uint64_t)stats->num_tx_dropped * 100U > stats->num_packets_tx
                                    ? INKCELL_TONE_WARNING
                                    : INKCELL_TONE_NORMAL,
                                MESH_STR_STATUS_LABEL_SENT, MESH_STR_STATUS_SENT,
                                stats->num_packets_tx, stats->num_tx_relay, stats->num_tx_dropped);
            /*
             * And the receive side, which is a partition and so gets the picture.
             *
             * Upstream's own comment on the duplicate counter is "if this number is high, there are
             * nodes in the mesh relaying packets when it's unnecessary", and high is a property of
             * a *share*: 4,812 duplicates is a busy mesh or a broken one depending entirely on what
             * the other number is. Three lengths beside each other answer that without arithmetic,
             * which is the airtime bar's argument on a whole with more than one part in it.
             *
             * The received total is not a fourth number on the row. It is the sum of the three, and
             * it is the length of the bar underneath - so stating it as well would be the row
             * saying one thing twice, which is what the row this replaced was doing three rows up.
             *
             * The tone is read off the share, for the same reason the bar exists at all. It was an
             * absolute count on the Dropped row: twelve malformed packets in six thousand lit a
             * warning that then stayed lit for the life of the connection. A mesh where most of
             * what arrives is not new is the thing worth colouring, and that is a ratio. Half is
             * the threshold because it is the one a reader can check against the bar with no
             * arithmetic at all - the first slice is shorter than the rest of the track.
             *
             * And the partition is checked rather than assumed. Two counters off the air have no
             * promise of agreeing with a third: a firmware that counted duplicates outside its
             * received total, or a report that arrived across a counter reset, would leave the
             * remainder negative - and clamped to zero it would draw a bar claiming every packet
             * the radio heard was bad. The row and its bar are skipped instead, which leaves the
             * Sent row above saying what it always said.
             */
            /*
             * Both share tests are done 64 bits wide, and so is the sum feeding this one. These are
             * `uint32_t` off the air multiplied by a constant, so a share written at the counters'
             * own width wraps at a total the wire can perfectly well carry - `dropped * 100` at 43
             * million and `not_new * 2` at two billion - and a wrapped product does not fail
             * loudly. It compares small, so the row goes back to its resting colour at exactly the
             * totals that earned the warning. The widening is the cheapest thing on this screen and
             * it is the difference between a tone that is wrong and a tone that is quietly wrong.
             *
             * The sum is the same argument one step earlier: `rx_bad + rx_dupe` at 32 bits can wrap
             * to a *small* number, which then passes the partition check below and draws a bar with
             * a remainder computed from a total that never happened.
             */
            const uint32_t heard = stats->num_packets_rx;
            const uint64_t not_new = (uint64_t)stats->num_packets_rx_bad + stats->num_rx_dupe;
            if (heard > 0U && not_new <= heard) {
                const uint32_t parts[] = {heard - (uint32_t)not_new, stats->num_rx_dupe,
                                          stats->num_packets_rx_bad};
                inkcell_fb_card_row(
                    &out->mesh, not_new * 2U > heard ? INKCELL_TONE_WARNING : INKCELL_TONE_NORMAL,
                    MESH_STR_STATUS_LABEL_HEARD, MESH_STR_STATUS_HEARD, parts[0], parts[1],
                    parts[2]);
                /* No label: the row it sits under names all three parts, in this order, and that
                   correspondence is the only legend a bar in a row's height has room for. */
                inkcell_fb_card_proportion(&out->mesh, INKCELL_TONE_NORMAL, INKCELL_STR_NONE, parts,
                                           (uint32_t)(sizeof parts / sizeof parts[0]));
            }
        }
    } else if (snapshot->handshake_valid) {
        /* Standing in for the two rows above, so it carries their label rather than one naming
           the card it is already inside - "Mesh: no report yet" on a card headed Mesh says the
           word twice and the subject once. Short enough for the value gutter, too: the long
           form was cut mid-word, which reads as a bug rather than as a radio that has simply
           not reported yet. */
        inkcell_fb_card_row_text(&out->mesh, INKCELL_TONE_DIM, MESH_STR_STATUS_LABEL_PACKETS,
                                 inkcell_str(MESH_STR_STATUS_MESH_NO_REPORT));
    }
    /* How much of that traffic this client is still holding. It is the one row on the card that
       counts something of ours rather than the radio's, and it sits here because what the ring
       holds is mesh traffic - a card of its own for two client-side numbers is a heading and two
       insets spent on the least-read rows of the screen. */
    inkcell_fb_card_row(&out->mesh, INKCELL_TONE_NORMAL, MESH_STR_STATUS_LABEL_MESSAGES,
                        MESH_STR_STATUS_MESSAGES_KEPT, (unsigned)snapshot->messages.count,
                        (unsigned)snapshot->messages.dropped);
    /*
     * And the verb that opens the airtime readings as a chart, on the heading line above all of
     * them.
     *
     * The card this belongs to is the one that can lose rows to the reservation below, which is
     * exactly why it is safe: the rows a clipped card sheds are the ones declared last, and the
     * heading - with the verbs on it - is not a row at all. A card that could end up with *no*
     * rows may not carry a verb, and this one always has the message counts above.
     */
    fb_status_card_actions(&out->mesh, &actions, MESH_UI_STATUS_CARD_MESH, focus);
    /* ---- the radio itself: its battery, its queue, and what it last said about itself ---- */

    /* Uptime is in both sources, like the airtime pair above, so LocalStats wins it for the same
       reason - and without this the row vanishes entirely when LocalStats has arrived but our
       node has not broadcast DeviceMetrics yet. Battery really does have only the one source. */
    const bool have_battery = metrics != NULL && metrics->has_battery;
    /* A MeshCore radio gives a voltage and no percentage, and no curve to read one off: a board
       on USB with no cell reads whatever its divider floats at. So the volts, uncoloured. */
    const bool have_volts = !have_battery && metrics != NULL && metrics->has_voltage;
    const bool have_uptime = stats->valid || (metrics != NULL && metrics->has_uptime);
    const uint32_t uptime_value = stats->valid        ? stats->uptime_seconds
                                  : (metrics != NULL) ? metrics->uptime_seconds
                                                      : 0U;
    const bool low_battery = have_battery && metrics->battery_level <= 20U;

    /*
     * The last thing the radio said in its own words, and how many times it has restarted under
     * us. Both are the answers to "why is this not working" that no counter above can give: the
     * counters describe traffic, and a duty-cycle refusal or a key mismatch is not traffic.
     *
     * Levels are python logging's scale: 40 is ERROR, 30 WARNING. Anything below that is the
     * radio being informative rather than reporting a problem.
     */
    const struct mesh_ui_radio_notice *notice = &snapshot->settings.notice;
    const bool have_notice = notice->seq != 0U && notice->text[0] != '\0';
    const enum inkcell_tone notice_tone = notice->level >= 40U   ? INKCELL_TONE_ERROR
                                          : notice->level >= 30U ? INKCELL_TONE_WARNING
                                                                 : INKCELL_TONE_NORMAL;
    const struct mesh_ui_queue_status *queue = &snapshot->settings.queue;
    /* The radio's send queue is only worth a row once it is under pressure or has just refused
       something: on an idle link it reads "16 of 16 free" for ever, which is one more number to
       skip past. A refusal keeps the row up because it is the explanation for a message that
       was never transmitted at all. */
    const bool have_queue =
        queue->valid && queue->maxlen > 0U && (queue->res != 0 || queue->free < queue->maxlen / 2U);

    /* Two more rows that only appear when something is off. Derived here rather than at the
       rows themselves because the heading is a reading of the same two facts, and a predicate
       written out twice is the pair that drifts. */
    const bool rebooted = snapshot->settings.reboot_notices > 0U;
    const bool low_heap =
        stats->valid && stats->has_heap && stats->heap_free_bytes < FB_HEAP_LOW_BYTES;

    /* The card reports the worst thing it holds. A refused packet and an ERROR notice are both
       the radio saying no; a flat battery is the reason it is about to. Below that, anything
       drawn in the warning tone heads the card in it too - a card saying "fine" over a row it
       has just drawn as a warning is the summary being wrong about its own contents. */
    enum inkcell_tone radio_tone = INKCELL_TONE_PRIMARY;
    if (low_battery || (have_queue && queue->res != 0) || (have_notice && notice->level >= 40U)) {
        radio_tone = INKCELL_TONE_ERROR;
    } else if ((have_notice && notice->level >= 30U) || rebooted || low_heap) {
        radio_tone = INKCELL_TONE_WARNING;
    }

    /*
     * Ordered most-read first, which matters here and on no other card: this is the one that can
     * outgrow the panel - every row on it appears only when the radio is in some kind of
     * trouble, so the worst case is all of them at once - and inkcell_fb_draw_card() drops from the
     * end. So the battery and the radio's own words come first and the heap figure last, because a
     * free-heap number is the row a user would have scrolled past anyway.
     */
    /*
     * The one card whose weight is a reading rather than a decision.
     *
     * Every row on it appears only when the radio is in some kind of trouble, so on a healthy
     * link it is a heading over a battery figure and nothing else - and a quiet card should
     * recede rather than spend a panel's worth of fill saying nothing. It is outlined there,
     * and lifts to the raised tier the moment the tone above says it has something to report,
     * which is the fact the heading colour was already carrying alone.
     */
    inkcell_fb_card_begin(&out->radio,
                          radio_tone == INKCELL_TONE_PRIMARY ? INKCELL_FB_CARD_OUTLINED
                                                             : INKCELL_FB_CARD_ELEVATED,
                          INKCELL_ICON_RADIO, MESH_STR_STATUS_CARD_RADIO, radio_tone);
    if (have_battery || have_volts || have_uptime) {
        buffer[0] = '\0';
        if (have_volts) {
            inkcell_str_format(buffer, sizeof buffer, MESH_STR_STATUS_BATTERY_VOLTS,
                               (double)metrics->voltage);
        } else if (have_battery) {
            /* 101 is upstream's "running off USB", not a 101% battery. */
            if (metrics->battery_level > 100U) {
                inkwell_str_copy(buffer, sizeof buffer, inkcell_str(MESH_STR_STATUS_BATTERY_USB));
            } else {
                inkcell_str_format(buffer, sizeof buffer, MESH_STR_STATUS_BATTERY_PERCENT,
                                   (unsigned)metrics->battery_level);
            }
        } else {
            /*
             * The row draws for either half, so a missing battery is a word rather than a gap -
             * and the word has to go in the buffer rather than be substituted at the draw,
             * because the separator below is chosen from what is in it.
             *
             * Chosen from an empty buffer and then drawn with a fallback in it, the two
             * disagreed and the row read "unknownup 9d 8h". The state is the ordinary one on a
             * radio that has sent LocalStats and not yet sent DeviceMetrics: uptime is in both
             * reports and battery is only in the second.
             */
            inkwell_str_copy(buffer, sizeof buffer, inkcell_str(MESH_STR_STATUS_BATTERY_UNKNOWN));
        }
        second[0] = '\0';
        if (have_uptime) {
            char uptime[32];
            fb_format_uptime(uptime_value, uptime, sizeof uptime);
            inkcell_str_format(second, sizeof second, MESH_STR_STATUS_UPTIME_SUFFIX, uptime);
        }
        inkcell_fb_card_row(&out->radio, low_battery ? INKCELL_TONE_ERROR : INKCELL_TONE_NORMAL,
                            MESH_STR_STATUS_LABEL_BATTERY, MESH_STR_STATUS_SYNC_VALUE, buffer,
                            second);
    }
    if (have_notice) {
        /*
         * When and how often on the labelled row, the words themselves as a note underneath at
         * the card's full width. Every other row here is a label and a short value, but a
         * notification is a sentence the firmware wrote, and a sentence in the narrow value
         * gutter is three words and a cut - which loses exactly the part that explains anything.
         */
        char age[24];
        inkcell_fb_format_age(notice->received, age, sizeof age);
        if (notice->seq > 1U) {
            inkcell_fb_card_row(&out->radio, INKCELL_TONE_DIM, MESH_STR_STATUS_LABEL_RADIO_SAID,
                                MESH_STR_STATUS_RADIO_SAID_COUNT, age, notice->seq);
        } else {
            inkcell_fb_card_row_text(&out->radio, INKCELL_TONE_DIM,
                                     MESH_STR_STATUS_LABEL_RADIO_SAID, age);
        }
        inkcell_fb_card_note(&out->radio, notice_tone, notice->text);
    }
    if (have_queue) {
        /* A queue under pressure is work in flight, not a fault - the tertiary. A refusal is
           a fault, and takes the error family. */
        inkcell_fb_card_row(&out->radio,
                            queue->res != 0 ? INKCELL_TONE_ERROR : INKCELL_TONE_TERTIARY,
                            MESH_STR_STATUS_LABEL_TX_QUEUE, MESH_STR_STATUS_TX_QUEUE,
                            (unsigned)queue->free, (unsigned)queue->maxlen,
                            queue->res != 0 ? inkcell_str(MESH_STR_STATUS_TX_QUEUE_REFUSED) : "");
    }
    if (rebooted) {
        /* A radio that has restarted since we attached is not broken, but it is the first thing
           to know when something else looks wrong. */
        inkcell_fb_card_row(&out->radio, INKCELL_TONE_WARNING, MESH_STR_STATUS_LABEL_REBOOTS,
                            MESH_STR_STATUS_REBOOTS_SINCE, snapshot->settings.reboot_notices);
    }
    if (stats->valid && stats->has_heap) {
        inkcell_fb_card_row(&out->radio, low_heap ? INKCELL_TONE_WARNING : INKCELL_TONE_DIM,
                            MESH_STR_STATUS_LABEL_HEAP, MESH_STR_STATUS_HEAP,
                            stats->heap_free_bytes / 1024U, stats->heap_total_bytes / 1024U);
    }
    if (stats->has_storage && stats->storage_total_kb > 0U) {
        /* Coloured from nine tenths, while there is still room to clear. */
        inkcell_fb_card_row(&out->radio,
                            (uint64_t)stats->storage_used_kb * 10U >
                                    (uint64_t)stats->storage_total_kb * 9U
                                ? INKCELL_TONE_WARNING
                                : INKCELL_TONE_DIM,
                            MESH_STR_STATUS_LABEL_STORAGE, MESH_STR_STATUS_STORAGE,
                            stats->storage_used_kb, stats->storage_total_kb);
    }
    /*
     * A radio that has told us nothing about itself, said out loud.
     *
     * Every row above appears only when something is worth reporting, so a link that has just
     * come up and a radio with no battery sensor both leave this card with no rows at all - and
     * a card with no rows is not drawn. That was fine while the card was a readout. It is not
     * fine now that it carries a verb: the action bar would be naming a press whose button is
     * not on screen, and the cursor would step onto nothing. The Mesh card already had this row
     * for the same reason its counters can be missing; this is the same sentence for the same
     * situation, with its own id because it is read somewhere else.
     */
    if (inkcell_fb_card_is_empty(&out->radio) && snapshot->handshake_valid) {
        inkcell_fb_card_row_text(&out->radio, INKCELL_TONE_DIM, MESH_STR_STATUS_LABEL_RADIO_SELF,
                                 inkcell_str(MESH_STR_STATUS_RADIO_NO_REPORT));
    }
    fb_status_card_actions(&out->radio, &actions, MESH_UI_STATUS_CARD_RADIO, focus);
    out->radio_tone = radio_tone;

    /* ---- what the tiles read ---- */
    out->have_util = have_util;
    out->util_permille = util_permille;
    out->air_tone = air_tone;
    if (have_util) {
        inkcell_str_format(out->util, sizeof out->util, MESH_STR_STATUS_PERCENT,
                           (double)util_value);
    }
    if (have_tx) {
        const float tx_value =
            air_from_stats ? stats->air_util_tx : (metrics != NULL ? metrics->air_util_tx : 0.0f);
        inkcell_str_format(out->tx, sizeof out->tx, MESH_STR_STATUS_PERCENT, (double)tx_value);
    }
    if (snapshot->handshake_valid && snapshot->handshake.has_my_info) {
        out->known = snapshot->handshake.my_info.nodedb_entries;
        out->have_online = stats->valid;
        out->online = stats->num_online_nodes;
    }
    if (have_battery || have_volts) {
        inkwell_str_copy(out->battery, sizeof out->battery, buffer);
        out->low_battery = low_battery;
    }
    if (have_uptime) {
        fb_format_uptime(uptime_value, out->uptime, sizeof out->uptime);
    }
}

/*
 * The narrowest a board's track may be, in columns of body text: a card's label column and a
 * value beside it, which is what the narrowest card on the board - Link, Radio - has to hold.
 *
 * In columns for the width classes' reason. Below three tracks the board is not built at all and
 * the cards stand in their two columns, because a board of two tracks is those two columns with a
 * row of tiles over them that pushes the Radio card - and its verbs - off the bottom.
 */
#define FB_BOARD_TRACK_COLS 26U
#define FB_BOARD_TRACKS_MIN 3U
#define FB_BOARD_TRACKS_MAX 4U

/* The channel tile's meter in the animation table, beside the Mesh card's own, which is drawn on
   the same frame from the same reading. */
#define FB_ANIM_ID_AIRTIME_TILE 0xFFFFFF04U

/*
 * The Reach card: how far away the roster is, from the hop counts the radio keeps per node.
 *
 * Built only for the board. On the handheld the column has no room for a fifth card and this is
 * the Nodes tab's to answer; on a window it is the card that turns "42 nodes" into a shape.
 */
/* A neighbour as a row names it: the callsign when it has one, since the row is short, and
   otherwise the title every other screen falls back through - long name, then "!0a1b2c3d". */
static void fb_status_reach_name(const struct mesh_ui_node_summary *node, char *out,
                                 size_t out_len) {
    if (node->short_name[0] != '\0') {
        inkwell_str_copy(out, out_len, node->short_name);
        return;
    }
    mesh_ui_node_title(node, node->node_id, out, out_len);
}

static void fb_status_reach_card(const struct mesh_ui_snapshot *snapshot,
                                 struct inkcell_fb_card *card) {
    struct mesh_ui_reach reach;
    mesh_ui_reach_of(snapshot->handshake_valid ? &snapshot->handshake : NULL, &reach);
    inkcell_fb_card_begin(card, INKCELL_FB_CARD_FILLED, INKCELL_ICON_NEIGHBORS,
                          MESH_STR_BOARD_CARD_REACH, INKCELL_TONE_PRIMARY);
    if (reach.counted == 0U) {
        inkcell_fb_card_row_text(card, INKCELL_TONE_DIM, MESH_STR_BOARD_LABEL_HOPS,
                                 inkcell_str(MESH_STR_BOARD_REACH_NONE));
    } else {
        inkcell_fb_card_row(card, INKCELL_TONE_NORMAL, MESH_STR_BOARD_LABEL_HOPS,
                            MESH_STR_BOARD_HOPS, reach.hops[MESH_UI_REACH_DIRECT],
                            reach.hops[MESH_UI_REACH_ONE], reach.hops[MESH_UI_REACH_TWO],
                            reach.hops[MESH_UI_REACH_MORE]);
        /* Under the row that names its parts, in the same order - the only legend a bar in a
           row's height has room for, as on the Mesh card's Heard row. */
        inkcell_fb_card_proportion(card, INKCELL_TONE_NORMAL, INKCELL_STR_NONE, reach.hops,
                                   MESH_UI_REACH_BUCKETS);
    }
    const struct mesh_ui_node_summary *nodes = snapshot->handshake.nodes;
    char name[48];
    if (reach.strongest >= 0) {
        fb_status_reach_name(&nodes[reach.strongest], name, sizeof name);
        inkcell_fb_card_row(card, INKCELL_TONE_SUCCESS, MESH_STR_BOARD_LABEL_STRONGEST,
                            MESH_STR_BOARD_SNR, name, (double)nodes[reach.strongest].snr);
    }
    /* Only when it is a different node: with one neighbour the two rows are one fact. */
    if (reach.weakest >= 0 && reach.weakest != reach.strongest) {
        fb_status_reach_name(&nodes[reach.weakest], name, sizeof name);
        inkcell_fb_card_row(card, INKCELL_TONE_NORMAL, MESH_STR_BOARD_LABEL_WEAKEST,
                            MESH_STR_BOARD_SNR, name, (double)nodes[reach.weakest].snr);
    }
    if (reach.via_broker > 0U) {
        inkcell_fb_card_row(card, INKCELL_TONE_DIM, MESH_STR_BOARD_LABEL_VIA_BROKER,
                            MESH_STR_BOARD_VIA_BROKER, reach.via_broker);
    }
}

/*
 * The four figures the board opens on, in the order they are read: is the mesh there, is the air
 * full, will the radio last, and is what we send arriving. Returns how many were filled, which is
 * all four - a caller with fewer tracks takes the first of them.
 *
 * Each is a figure the cards below also state, on purpose: a tile is the glance and the card is
 * the reading, and the two are built off the same fields so they cannot disagree.
 */
static uint32_t fb_status_tiles(const struct mesh_ui_snapshot *snapshot,
                                const struct fb_status_cards *cards,
                                struct inkcell_fb_stat tiles[4], char words[4][2][48],
                                struct inkcell_polyline *battery_trend) {
    memset(tiles, 0, 4U * sizeof tiles[0]);
    const char *unknown = inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);

    /* Online, out of the database. */
    struct inkcell_fb_stat *online = &tiles[0];
    online->variant = INKCELL_FB_CARD_ELEVATED;
    online->icon = INKCELL_ICON_NODES;
    online->label = MESH_STR_BOARD_TILE_ONLINE;
    online->value = unknown;
    online->tone = INKCELL_TONE_DIM;
    if (cards->have_online) {
        snprintf(words[0][0], sizeof words[0][0], "%u", (unsigned)cards->online);
        online->value = words[0][0];
        online->tone = INKCELL_TONE_NORMAL;
    }
    if (cards->known > 0U) {
        inkcell_str_format(words[0][1], sizeof words[0][1], MESH_STR_BOARD_ONLINE_OF,
                           (unsigned)cards->known);
        online->caption = words[0][1];
    }

    /* The channel, on the band the Mesh card's bar and the chart's rules are drawn at. */
    struct inkcell_fb_stat *channel = &tiles[1];
    channel->variant = INKCELL_FB_CARD_FILLED;
    channel->icon = INKCELL_ICON_LORA;
    channel->label = MESH_STR_BOARD_TILE_CHANNEL;
    channel->value = cards->have_util ? cards->util : unknown;
    channel->tone = cards->have_util ? cards->air_tone : INKCELL_TONE_DIM;
    if (cards->tx[0] != '\0') {
        inkcell_str_format(words[1][1], sizeof words[1][1], MESH_STR_BOARD_CHANNEL_OURS, cards->tx);
        channel->caption = words[1][1];
    }
    if (cards->have_util) {
        channel->picture = INKCELL_FB_STAT_METER;
        channel->meter_value = cards->util_permille;
        channel->meter_band = &fb_air_band;
        channel->meter_id = FB_ANIM_ID_AIRTIME_TILE;
    }

    /* The battery, with its trend under it when this client has been watching our own node. */
    struct inkcell_fb_stat *battery = &tiles[2];
    battery->variant = INKCELL_FB_CARD_FILLED;
    battery->icon = INKCELL_ICON_POWER;
    battery->label = MESH_STR_BOARD_TILE_BATTERY;
    battery->value = cards->battery[0] != '\0' ? cards->battery : unknown;
    battery->tone = cards->battery[0] == '\0' ? INKCELL_TONE_DIM
                    : cards->low_battery      ? INKCELL_TONE_ERROR
                                              : INKCELL_TONE_NORMAL;
    if (cards->uptime[0] != '\0') {
        inkcell_str_format(words[2][1], sizeof words[2][1], MESH_STR_BOARD_UPTIME, cards->uptime);
        battery->caption = words[2][1];
    }
    if (snapshot->handshake_valid && snapshot->handshake.has_my_info) {
        const struct inkcell_series *series = mesh_ui_history_series(
            &snapshot->history, snapshot->handshake.my_info.node_num, MESH_UI_HISTORY_BATTERY);
        if (series != NULL) {
            inkcell_series_project(series, (struct inkcell_scale){0, 100}, battery_trend);
            if (battery_trend->count >= 2U) {
                battery->picture = INKCELL_FB_STAT_TREND;
                battery->trend = battery_trend;
            }
        }
    }

    /* What share of what we sent directly the mesh confirmed, over this client's whole life. */
    const struct mesh_ui_lifetime_stats *life = &snapshot->settings.client.lifetime;
    struct inkcell_fb_stat *delivered = &tiles[3];
    delivered->variant = INKCELL_FB_CARD_FILLED;
    delivered->icon = INKCELL_ICON_SEND;
    delivered->label = MESH_STR_BOARD_TILE_DELIVERED;
    delivered->value = inkcell_str(MESH_STR_STATS_NONE_YET);
    delivered->tone = INKCELL_TONE_DIM;
    const uint64_t settled = life->messages_delivered + life->messages_failed;
    unsigned percent = 0U;
    if (mesh_ui_delivery_rate(life->messages_delivered, life->messages_failed, &percent)) {
        /* Rounded down by the Stats page's own arithmetic, so one failure keeps it under 100. */
        snprintf(words[3][0], sizeof words[3][0], "%u%%", percent);
        delivered->value = words[3][0];
        delivered->tone = INKCELL_TONE_NORMAL;
        inkcell_str_format(words[3][1], sizeof words[3][1], MESH_STR_BOARD_DELIVERED_OF,
                           (unsigned long long)life->messages_delivered,
                           (unsigned long long)settled);
        delivered->caption = words[3][1];
    }
    return 4U;
}

/*
 * The Status tab as a board, for a window wide enough for one: false when it is not, and the
 * caller draws the columns instead.
 *
 *     [ Online ] [ Channel use ] [ Battery ] [ Delivered ]
 *     [ Mesh, with the airtime chart under its rows ][ Link  ]
 *     [                                             ][ Radio ]
 *     [ Reach                     ][ Broker, when there is one ]
 *
 * On the column the airtime is a bar and a verb that opens the chart, because the column has
 * room for a bar and no more. Here the chart *is* the Mesh card's body - the room under its rows
 * that inkcell_fb_draw_card_in() hands back - and the verb is still on the heading for the screen
 * where the span can be narrowed.
 *
 * Every card is the column's card, verbs included, so the d-pad walks the same flat list in the
 * same order and a click lands on the same verb; the board only decides where each card stands.
 * The tiles are read, not pressed (inkcell/ui/widgets/stat.h), so they add nothing to walk.
 *
 * The three cards carrying verbs - Link, Mesh, Radio - are in the middle row, which is sized
 * first and never below what they promised: the Radio card whole when its tone says it has
 * something wrong to say, its heading and a row otherwise, which is the column's reservation
 * rule. The bottom row holds the two cards with no verbs, and is what gives way.
 */
static bool fb_status_board(struct inkcell_draw_state *state,
                            const struct mesh_ui_snapshot *snapshot,
                            struct inkcell_fb_layout *layout, struct fb_status_cards *cards) {
    if (inkcell_fb_width_class(state) == INKCELL_WIDTH_COMPACT) {
        return false;
    }
    const struct inkcell_box body = inkcell_fb_full_box(state, layout);
    const int gap = inkcell_fb_space(state, INKCELL_SPACE_MD);
    const int adv = inkcell_fb_char_adv(state, state->scale);
    const uint32_t tracks =
        inkcell_dash_columns(body.w, (int)FB_BOARD_TRACK_COLS * adv, gap, FB_BOARD_TRACKS_MAX);
    if (tracks < FB_BOARD_TRACKS_MIN) {
        return false;
    }
    /* The measure is off for the rest of the frame, as it is for the two columns: a card on a
       board is as wide as its tile. fb_render_snapshot() puts it back once the bar is drawn. */
    (void)inkcell_fb_set_measured(state, false);
    const struct inkcell_box whole = inkcell_fb_region(state);

    struct inkcell_dash dash;
    inkcell_dash_begin(&dash, body, tracks, gap);
    const int track_w = dash.edges[1] - dash.edges[0] - gap;
    /* The side column is the last track, which carries no gap after it. */
    const int side_w = dash.edges[tracks] - dash.edges[tracks - 1U];

    /* The bottom row: the reach, and the broker beside it when the radio wants one. Two tracks
       for the broker, whose rows are a host name and a sentence; the reach takes the rest. */
    struct inkcell_fb_card reach;
    fb_status_reach_card(snapshot, &reach);
    const uint32_t broker_span = cards->have_broker ? 2U : 0U;
    const uint32_t reach_span = tracks - broker_span;

    /* What each card needs, measured at the width it is drawn at - a narrower card wraps a note
       further. A card's own measure reads the region, so the region is moved to each in turn. */
    const struct inkcell_box side = {dash.edges[tracks - 1U], body.y, side_w, body.h};
    (void)inkcell_fb_set_region(state, side);
    const int link_need = inkcell_fb_card_height(state, layout, &cards->link);
    const int link_least = inkcell_fb_card_min_height(state, layout, &cards->link);
    const int radio_least = cards->radio_tone != INKCELL_TONE_PRIMARY
                                ? inkcell_fb_card_height(state, layout, &cards->radio)
                                : inkcell_fb_card_min_height(state, layout, &cards->radio);
    (void)inkcell_fb_set_region(
        state, (struct inkcell_box){dash.edges[0], body.y,
                                    dash.edges[reach_span] - dash.edges[0] - gap, body.h});
    int bottom_need = inkcell_fb_card_height(state, layout, &reach);
    if (cards->have_broker) {
        (void)inkcell_fb_set_region(
            state, (struct inkcell_box){dash.edges[reach_span], body.y,
                                        dash.edges[tracks] - dash.edges[reach_span], body.h});
        const int broker_need = inkcell_fb_card_height(state, layout, &cards->broker);
        bottom_need = broker_need > bottom_need ? broker_need : bottom_need;
    }
    (void)inkcell_fb_set_region(state, whole);
    const int middle_least = link_least + gap + radio_least;

    /* The tiles, unless the window is too short to keep the middle row's promise under them. */
    struct inkcell_fb_stat tiles[4];
    char words[4][2][48];
    memset(words, 0, sizeof words);
    struct inkcell_polyline battery_trend;
    memset(&battery_trend, 0, sizeof battery_trend);
    const uint32_t tile_count = fb_status_tiles(snapshot, cards, tiles, words, &battery_trend);
    const uint32_t shown = tile_count < tracks ? tile_count : tracks;
    int tile_h = 0;
    for (uint32_t i = 0U; i < shown; ++i) {
        tiles[i].rect.w = track_w;
        const int h = inkcell_fb_stat_height(state, layout, &tiles[i]);
        tile_h = h > tile_h ? h : tile_h;
    }
    if (tile_h + gap + middle_least <= inkcell_dash_left(&dash) &&
        inkcell_dash_row(&dash, tile_h)) {
        for (uint32_t i = 0U; i < shown; ++i) {
            const struct inkcell_box cell = inkcell_dash_cell(&dash, 1U);
            tiles[i].rect = (struct inkcell_fb_rect){cell.x, cell.y, cell.w, cell.h};
            inkcell_fb_draw_stat(state, layout, &tiles[i]);
        }
    }

    /* The bottom row gets what its cards need, held under a third of what is left so the chart
       keeps a picture's worth of room - and gives up altogether before the middle row does. */
    const int left = inkcell_dash_left(&dash);
    int bottom_h = bottom_need < left / 3 ? bottom_need : left / 3;
    if (left - gap - bottom_h < middle_least) {
        bottom_h = left - gap - middle_least;
    }
    const int middle_h = bottom_h > 0 ? left - gap - bottom_h : left;

    if (inkcell_dash_row(&dash, middle_h)) {
        const struct inkcell_box mesh_box = inkcell_dash_cell(&dash, tracks - 1U);
        struct inkcell_box room = inkcell_fb_draw_card_in(state, layout, mesh_box, &cards->mesh);
        /* A step of air between the last row and the chart's top label, which otherwise reads as
           a value of the row above it. */
        const int air = inkcell_fb_space(state, INKCELL_SPACE_SM);
        room.y += air;
        room.h -= air;
        fb_render_airtime_in(state, snapshot, layout, room);

        /* Link over Radio in the side column: the link as tall as it needs while that leaves the
           radio its promise, and the radio the rest - which is all of it when it is in trouble. */
        const struct inkcell_box cell = inkcell_dash_cell(&dash, 1U);
        int link_h = link_need;
        if (link_h > cell.h - gap - radio_least) {
            link_h = cell.h - gap - radio_least;
        }
        if (link_h < link_least) {
            link_h = link_least;
        }
        (void)inkcell_fb_draw_card_in(
            state, layout, (struct inkcell_box){cell.x, cell.y, cell.w, link_h}, &cards->link);
        (void)inkcell_fb_draw_card_in(
            state, layout,
            (struct inkcell_box){cell.x, cell.y + link_h + gap, cell.w, cell.h - link_h - gap},
            &cards->radio);
    }
    if (bottom_h > 0 && inkcell_dash_row(&dash, bottom_h)) {
        (void)inkcell_fb_draw_card_in(state, layout, inkcell_dash_cell(&dash, reach_span), &reach);
        if (cards->have_broker) {
            (void)inkcell_fb_draw_card_in(state, layout, inkcell_dash_cell(&dash, broker_span),
                                          &cards->broker);
        }
    }
    return true;
}

/*
 * The Status tab, as three cards.
 *
 * It used to be eighteen label/value lines on the bare ground, in one column, and nothing in it
 * said that Transport, Radio and Sync are one subject and Packets and Dropped are another - the
 * only grouping was a half-line of extra space every so often, which is not a grouping so much
 * as a hope. Each card names its subject and reports on it in its own heading colour, so "is
 * anything wrong" is answered by the shape and the colour before a number has been read.
 *
 * There is no screen title: the tab strip already says Status and every card names itself, so a
 * title would be the third time. The rows it frees are the ones the cards spend on their
 * headings.
 *
 * Cards are declared and then drawn (see inkcell/ui/widgets.h), so a row that only exists when the
 * radio has reported something is an `if` around one call. Nothing here guards the footer
 * either: inkcell_fb_draw_card() drops what does not fit and refuses a card outright when nothing
 * does, which is the check this screen used to write out per row, and in two different ways.
 *
 * On a window with room the cards stand in two columns (fb_status_columns()): the link and the
 * broker - what this client is connected to - on the leading side, and the mesh and the radio -
 * what it has heard - on the other. On one with room for a board they are tiles on it instead
 * (fb_status_board()). The order they are read in is unchanged either way, and so is the order
 * the d-pad walks their verbs, which is the flat list's rather than the panel's.
 */
void fb_render_status(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                      struct inkcell_fb_layout *layout) {
    struct fb_status_cards cards;
    fb_status_build(snapshot, &cards);
    if (fb_status_board(state, snapshot, layout, &cards)) {
        return;
    }
    int y = layout->body_y;
    struct inkcell_fb_card *const radio = &cards.radio;
    const enum inkcell_tone radio_tone = cards.radio_tone;

    /*
     * Two columns or one. With two the measure is off for the rest of the frame - each column is
     * narrower than a measure and a card fills it, and the action bar under them spans both - and
     * fb_render_snapshot() puts it back once that bar is drawn.
     */
    struct inkcell_box columns[2];
    const bool two = fb_status_columns(state, columns);
    const struct inkcell_box whole = inkcell_fb_region(state);
    if (two) {
        (void)inkcell_fb_set_measured(state, false);
        (void)inkcell_fb_set_region(state, columns[0]);
    }
    (void)inkcell_fb_draw_card(state, layout, &y, &cards.link);

    /*
     * And now both, in the order they are read - the Mesh card first, told to leave room for
     * this one.
     *
     * This is the only place on the screen where the order things are *declared* and the order
     * they are *drawn* come apart, and it is what fixes a card disappearing. A column of cards
     * is drawn top down and each takes what it wants, so the last one pays for everything above
     * it by not being drawn at all - and this is the card carrying `refresh`, which
     * mesh_ui_status_actions() offers from the link state alone. The cursor therefore walked
     * onto a button that was not on the frame, which is "a card that can end up with no rows
     * must not be given a verb" reached from the layout side.
     *
     * The Mesh card is the one that grows: its airtime block is three rows when the radio has
     * reported twice, where the Link card above is a fixed six and is never the card that
     * squeezes this one out. So the reservation goes there, and the rows it costs are the rows
     * a screen declared last - which on that card is the message ring, the row a reader would
     * have skipped anyway.
     *
     * **How much** room is a reading rather than a constant, and that is the half of this the
     * first version got backwards.
     *
     * Reserving the minimum promises the card exists and its verb is reachable, which is the
     * right promise while the radio is well: the card is then a heading over a battery figure,
     * the Mesh card's counters are the screen's subject, and a card handed more room than it
     * needs will spend it. It is the wrong promise in exactly the state this card exists for.
     * Every row on it appears only when something is wrong, so the worst case is all of them at
     * once - a firmware notice, a refused send, a reboot count, a flat battery - and those are
     * the highest-value words on the frame. Under a fixed minimum the Mesh card kept its message
     * ring and the radio's own explanation of why nothing is working was clipped off the bottom.
     *
     * So the reservation follows the tone the card already computed for itself. `radio_tone` is
     * that card's report on its own contents and it is what decides its variant a few lines up;
     * it decides its claim on the column here, on the same reading and for the same reason. A
     * quiet card recedes; a card with something wrong to say takes the room to say it, and the
     * rows it takes are the ones the Mesh card declared last - the message ring and the received
     * composition, which are the rows a reader chasing a fault would have skipped.
     */
    /* Measured in the column it is drawn in, since a narrower card wraps into more rows. */
    if (two) {
        (void)inkcell_fb_set_region(state, columns[1]);
    }
    const int radio_reserve = radio_tone == INKCELL_TONE_PRIMARY
                                  ? inkcell_fb_card_min_height(state, layout, radio)
                                  : inkcell_fb_card_height(state, layout, radio);
    /*
     * The Broker card goes in above them, holding the same promise one level further up.
     *
     * It is the second card on the screen and the last to be drawn, which is the only way it can
     * reserve room for cards that are built after it reads. Without the reservation it simply
     * took what it wanted - and what it wants in the state it exists for is seven rows and a
     * wrapped sentence, which pushed the Radio card off the bottom of the screen and left
     * `refresh` a verb the cursor could walk onto and nobody could see.
     *
     * The minimum for the Mesh card and the Radio card's own claim, so the promise is "both of
     * them exist" rather than "both of them are whole". That is the right split here: the rows
     * the Mesh card gives up are its packet counters, and a reader who has come to this screen
     * because MQTT is not working is not reading packet counters.
     */
    if (two) {
        /*
         * Beside each other, the reservations shrink to what is still below a card in its own
         * column: the broker is under the link with nothing after it, and the mesh keeps the
         * radio's claim exactly as it does in one column.
         */
        int right_y = layout->body_y;
        (void)inkcell_fb_draw_card_reserving(state, layout, &right_y, &cards.mesh, radio_reserve);
        (void)inkcell_fb_draw_card(state, layout, &right_y, radio);
        if (cards.have_broker) {
            (void)inkcell_fb_set_region(state, columns[0]);
            (void)inkcell_fb_draw_card(state, layout, &y, &cards.broker);
        }
        (void)inkcell_fb_set_region(state, whole);
        return;
    }
    if (cards.have_broker) {
        (void)inkcell_fb_draw_card_reserving(
            state, layout, &y, &cards.broker,
            inkcell_fb_card_min_height(state, layout, &cards.mesh) + radio_reserve);
    }
    (void)inkcell_fb_draw_card_reserving(state, layout, &y, &cards.mesh, radio_reserve);
    (void)inkcell_fb_draw_card(state, layout, &y, radio);
}
