#define _POSIX_C_SOURCE 200809L

/*
 * meshclient_uicap - drive the HUD through a scripted sequence and render every frame.
 *
 * The point is transitions. A screenshot off a Brick (`make deploy-shot`) says what one screen
 * looks like; a UI change is usually about what happens *between* two of them - a thread
 * opening, the keyboard coming up, a toast arriving - and neither a still nor a diff shows
 * that. This walks the real navigation model (mesh_ui_store_handle_key, the same function the
 * device's evdev loop calls) and hands each resulting snapshot to the off-screen renderer, so
 * a scene script turns into a strip of frames that scripts/frames.py encodes as a GIF.
 *
 * Nothing here fakes the UI. The store, the nav model and fb_render_snapshot() are the ones
 * that ship; only the radio at the other end is invented, and only far enough to give the
 * screens something to draw.
 *
 * Scene script (one command per line, '#' starts a comment):
 *
 *   scene demo|empty       which invented radio to start from     (setup, default demo)
 *   scale N                glyph multiplier, 2..6                 (setup, default the theme's)
 *   delay MS               per-frame delay written to the manifest (setup, default 140)
 *   theme NAME             dark|light|contrast|colorblind - before the first frame it picks
 *                          the look, after it switches and emits one, so a single script can
 *                          show the same screen in every theme
 *   tab NAME               walk Left/Right to messages|nodes|devices|status|settings
 *   config                 a radio that has answered the config handshake
 *   key NAME [COUNT]       up down left right a b x y l1 r1 start select
 *   hold MS                add MS to the delay of the frame just emitted
 *   frame                  emit the current screen again
 *   toast TEXT             raise the transient notice - the snackbar. It times out on the
 *                          scene's own clock, so a `hold` past four seconds and a `frame`
 *                          film it sliding back out again
 *   message in|out NAME TEXT   append a message to the log, as if the radio had just said so
 *   react NAME EMOJI       react to the newest message, as another node would
 *   alert NAME TEXT        a critical alert (ALERT_APP) on the channel
 *   detection NAME TEXT    a detection sensor announcing itself (DETECTION_SENSOR_APP)
 *   status TEXT            set the transport status line
 *   notice info|warn|error TEXT   what the radio last said about itself (Status tab)
 *   queue FREE MAXLEN [refused]   the radio's outgoing packet queue (Status tab)
 *   reboots N              times the radio has restarted under us (Status tab)
 *   offradio NAME|all      mark that node (or every node but ours) as one the radio's NodeDB
 *                          no longer carries - what a NodeDB reset leaves behind
 *   pin NAME               pin that node, which is what X on the Nodes tab does on a device -
 *                          the star in a row's marker gutter
 *
 * Every command but the setup three emits one frame (`key ... 3` emits three), and the screen
 * the script starts on is emitted before any of them.
 */

#include "mesh/core/message.h"
#include "mesh/core/updater.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define UICAP_DEFAULT_DELAY_MS 140U
#define UICAP_LINE_MAX 512U

struct uicap {
    struct mesh_ui_store store;
    struct mesh_ui_capture *capture;
    struct mesh_ui_snapshot snapshot;
    const char *out_dir;
    const char *prefix;
    unsigned delay_ms;
    /* One delay per frame, so `hold` can lengthen the frame already written. The manifest is
       assembled from this at the end rather than appended to as we go. */
    unsigned *delays;
    unsigned frame_count;
    unsigned delay_capacity;
    int scale;
    const char *theme_id;
    bool started;
    bool quiet;
    const char *scene;
    /* A monotonic-ish clock for toasts. Nothing ticks it forward on its own: a captured toast
       should still be on screen in the frame after the one that raised it. */
    uint64_t now_ms;
    uint32_t next_packet_id;
};

static void die(const char *message) {
    fprintf(stderr, "uicap: %s\n", message);
    exit(1);
}

/* ---- the invented radio ------------------------------------------------------------------ */

struct uicap_node_seed {
    uint32_t node_id;
    const char *short_name;
    const char *long_name;
    uint32_t heard_ago_s;
    float snr;
    uint8_t hops;
    bool has_hops;
    bool via_mqtt;
};

struct uicap_message_seed {
    uint32_t peer;
    const char *peer_name;
    const char *text;
    uint32_t sent_ago_s;
    uint8_t channel;
    bool broadcast;
    bool outbound;
    uint8_t ack;
};

/*
 * A mesh with enough in it that the screens are not mostly empty: eight nodes at a spread of
 * ages and signal, and a message log that crosses a day boundary and a long silence so the
 * transcript's separators have something to separate.
 */
static void uicap_scene_demo(struct uicap *cap) {
    const struct mesh_ui_device devices[3] = {
        {.identifier = "F4:12:FA:00:0A:11",
         .name = "Trailhead",
         .rssi = -48,
         .connected = true,
         .paired = true},
        {.identifier = "F4:12:FA:00:0A:22", .name = "Summit Relay", .rssi = -71, .paired = true},
        {.identifier = "F4:12:FA:00:0A:33", .name = "Meshtastic 4c2a", .rssi = -88},
    };
    mesh_ui_store_set_discovery(&cap->store, devices, 3U);

    /*
     * Four of these are zero hops away and not over MQTT, which is what makes them *heard* -
     * and their SNR figures are spread across the four rungs of a signal staircase on purpose,
     * because a roster where nothing was heard directly could not show one at all. That was the
     * roster before: every node reached over a relay or over MQTT, which is a mesh with no
     * neighbours in range and not one anybody has.
     */
    static const struct uicap_node_seed seeds[] = {
        {0x43A1C0DEU, "HOME", "Home Base", 30U, 11.5F, 0U, true, false},
        {0x8F21B004U, "ALFA", "Alfa Ridge", 95U, 8.25F, 0U, true, false},
        {0x8F21B005U, "BRVO", "Bravo Creek", 640U, -3.5F, 0U, true, false},
        {0x8F21B006U, "CHRL", "Charlie Lookout", 2400U, 4.0F, 0U, true, false},
        {0x8F21B007U, "DLTA", "Delta Camp", 5400U, 0.0F, 0U, false, true},
        {0x8F21B008U, "ECHO", "Echo Repeater", 9000U, 6.75F, 3U, true, false},
        {0x8F21B009U, "FXTR", "Foxtrot Mobile", 21600U, -8.0F, 0U, true, false},
        {0x8F21B00AU, "GOLF", "Golf Cabin", 76000U, 2.5F, 2U, true, false},
        /* Past here the list is longer than any body this renders into, which is the point:
           the scroll rail only draws when there is something off screen, so a demo roster that
           fit the panel could not show one. NATO order so a reviewer can tell at a glance
           which way a list has scrolled. */
        {0x8F21B00BU, "HOTL", "Hotel Saddle", 88000U, -1.5F, 3U, true, false},
        {0x8F21B00CU, "INDA", "India Point", 91000U, 5.0F, 1U, true, false},
        {0x8F21B00DU, "JULT", "Juliet Fire Road", 99000U, -6.25F, 0U, false, true},
        {0x8F21B00EU, "KILO", "Kilo Meadow", 105000U, 3.75F, 2U, true, false},
        {0x8F21B00FU, "LIMA", "Lima Crossing", 112000U, 9.0F, 1U, true, false},
        {0x8F21B010U, "MIKE", "Mike Hollow", 120000U, -4.0F, 4U, true, false},
        {0x8F21B011U, "NOVM", "November Bend", 133000U, 1.25F, 0U, false, true},
        {0x8F21B012U, "OSCR", "Oscar Summit", 140000U, 7.5F, 2U, true, false},
        {0x8F21B013U, "PAPA", "Papa Landing", 158000U, -2.75F, 3U, true, false},
        {0x8F21B014U, "QBEC", "Quebec Draw", 166000U, 4.5F, 1U, true, false},
        {0x8F21B015U, "ROMO", "Romeo Spur", 174000U, 0.5F, 2U, true, false},
        {0x8F21B016U, "SIER", "Sierra Notch", 188000U, -9.5F, 0U, false, true},
        {0x8F21B017U, "TNGO", "Tango Basin", 195000U, 6.0F, 1U, true, false},
        {0x8F21B018U, "UNIF", "Uniform Gap", 210000U, 2.0F, 3U, true, false},
    };

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.config_complete = true;
    handshake.has_my_info = true;
    handshake.has_config = true;
    handshake.my_info.node_num = seeds[0].node_id;
    handshake.my_info.nodedb_entries = 42U;
    handshake.roster_owner = seeds[0].node_id;
    snprintf(handshake.primary_channel, sizeof handshake.primary_channel, "%s", "LongFast");
    snprintf(handshake.my_short_name, sizeof handshake.my_short_name, "%s", "HOME");

    const uint32_t now = (uint32_t)time(NULL);
    handshake.node_count = (uint32_t)(sizeof seeds / sizeof seeds[0]);
    for (uint32_t i = 0U; i < handshake.node_count; ++i) {
        struct mesh_ui_node_summary *node = &handshake.nodes[i];
        node->node_id = seeds[i].node_id;
        snprintf(node->short_name, sizeof node->short_name, "%s", seeds[i].short_name);
        snprintf(node->long_name, sizeof node->long_name, "%s", seeds[i].long_name);
        node->last_heard = now - seeds[i].heard_ago_s;
        node->snr = seeds[i].snr;
        node->has_hops_away = seeds[i].has_hops;
        node->hops_away = seeds[i].hops;
        node->via_mqtt = seeds[i].via_mqtt;
        node->has_user = true;
        node->in_nodedb = true;
    }

    /*
     * Sensors on four of them, so the node detail's reading groups have something to draw.
     * Each node gets the group its role would plausibly report - a solar repeater has a
     * current monitor, a cabin has an indoor air sensor, a base station running meshtasticd
     * has a filesystem - because a demo where every node reports everything shows the layout
     * but not the point of splitting the groups up.
     */
    struct mesh_ui_node_summary *echo = &handshake.nodes[5]; /* Echo Repeater, solar */
    echo->power.valid = true;
    echo->power.time = now - 300U;
    echo->power.channel[0].has_voltage = true;
    echo->power.channel[0].voltage = 13.42F;
    echo->power.channel[0].has_current = true;
    echo->power.channel[0].current = -180.0F; /* charging */
    echo->power.channel[1].has_voltage = true;
    echo->power.channel[1].voltage = 18.9F;
    echo->power.channel[1].has_current = true;
    echo->power.channel[1].current = 410.0F;
    echo->environment.valid = true;
    echo->environment.time = now - 300U;
    echo->environment.has_temperature = true;
    echo->environment.temperature = 6.5F;

    struct mesh_ui_node_summary *golf = &handshake.nodes[7]; /* Golf Cabin, indoor sensor */
    golf->air_quality.valid = true;
    golf->air_quality.time = now - 900U;
    golf->air_quality.has_pm25 = true;
    golf->air_quality.pm25_standard = 8U;
    golf->air_quality.has_pm10 = true;
    golf->air_quality.pm10_standard = 3U;
    golf->air_quality.has_pm100 = true;
    golf->air_quality.pm100_standard = 11U;
    golf->air_quality.has_co2 = true;
    golf->air_quality.co2 = 812U;
    golf->air_quality.has_voc_index = true;
    golf->air_quality.voc_index = 103.0F;

    struct mesh_ui_node_summary *home = &handshake.nodes[0]; /* Home Base, meshtasticd on a Pi */
    home->host.valid = true;
    home->host.time = now - 60U;
    home->host.has_uptime = true;
    home->host.uptime_seconds = 806400U;
    home->host.has_freemem = true;
    home->host.freemem_kib = 512U * 1024U;
    home->host.has_diskfree = true;
    home->host.diskfree_mib = 21504U;
    home->host.has_load = true;
    home->host.load1 = 42U;
    home->host.load5 = 137U;
    home->host.load15 = 8U;

    /*
     * Device metrics on two of them - battery, voltage and the airtime pair.
     *
     * Almost every node on a real mesh reports these; none of the demo's did, which left the
     * node detail's largest group with nothing to draw and no way to look at it. The two are
     * deliberately either side of the battery band: Alfa is a repeater on mains with room in the
     * air, Foxtrot is a handheld running down, so one screen shows a reading resting and the
     * other shows one that has crossed a boundary.
     */
    struct mesh_ui_node_summary *alfa_metrics = &handshake.nodes[1]; /* Alfa Ridge, mains */
    alfa_metrics->metrics.valid = true;
    alfa_metrics->metrics.time = now - 200U;
    alfa_metrics->metrics.has_battery = true;
    alfa_metrics->metrics.battery_level = 87U;
    alfa_metrics->metrics.has_voltage = true;
    alfa_metrics->metrics.voltage = 4.02F;
    alfa_metrics->metrics.has_channel_utilization = true;
    alfa_metrics->metrics.channel_utilization = 12.5F;
    alfa_metrics->metrics.has_air_util_tx = true;
    alfa_metrics->metrics.air_util_tx = 1.8F;
    alfa_metrics->metrics.has_uptime = true;
    alfa_metrics->metrics.uptime_seconds = 259200U;

    struct mesh_ui_node_summary *foxtrot = &handshake.nodes[6]; /* Foxtrot Mobile, wearable */
    foxtrot->metrics.valid = true;
    foxtrot->metrics.time = now - 400U;
    foxtrot->metrics.has_battery = true;
    foxtrot->metrics.battery_level = 11U;
    foxtrot->metrics.has_voltage = true;
    foxtrot->metrics.voltage = 3.41F;
    foxtrot->metrics.has_channel_utilization = true;
    foxtrot->metrics.channel_utilization = 38.0F;
    foxtrot->metrics.has_air_util_tx = true;
    foxtrot->metrics.air_util_tx = 7.2F;
    foxtrot->health.valid = true;
    foxtrot->health.time = now - 120U;
    foxtrot->health.has_heart_bpm = true;
    foxtrot->health.heart_bpm = 62U;
    foxtrot->health.has_spo2 = true;
    foxtrot->health.spo2 = 98U;

    /*
     * Neighbour lists on four of them, arranged so the two groups on the node detail say
     * different things: Echo hears Alfa, Charlie and Golf; Alfa and Charlie both hear Echo, so
     * Echo's "Heard by" is not simply its own list read back. Delta hears nobody, which is what
     * a node that has dropped off the mesh looks like and is a real answer rather than a gap.
     */
    echo->neighbors.valid = true;
    echo->neighbors.time = now - 1800U;
    echo->neighbors.broadcast_interval_secs = 14400U;
    echo->neighbors.count = 3U;
    echo->neighbors.entries[0].node_id = seeds[1].node_id; /* Alfa Ridge */
    echo->neighbors.entries[0].snr = 8.25F;
    echo->neighbors.entries[1].node_id = seeds[3].node_id; /* Charlie Lookout */
    echo->neighbors.entries[1].snr = 4.0F;
    echo->neighbors.entries[2].node_id = seeds[7].node_id; /* Golf Cabin */
    echo->neighbors.entries[2].snr = -6.75F;

    struct mesh_ui_node_summary *alfa = &handshake.nodes[1];
    alfa->neighbors.valid = true;
    alfa->neighbors.time = now - 2400U;
    alfa->neighbors.count = 2U;
    alfa->neighbors.entries[0].node_id = seeds[5].node_id; /* Echo Repeater */
    alfa->neighbors.entries[0].snr = 7.5F;
    alfa->neighbors.entries[1].node_id = seeds[0].node_id; /* Home Base */
    alfa->neighbors.entries[1].snr = 11.0F;

    struct mesh_ui_node_summary *charlie = &handshake.nodes[3];
    charlie->neighbors.valid = true;
    charlie->neighbors.time = now - 3000U;
    charlie->neighbors.count = 1U;
    charlie->neighbors.entries[0].node_id = seeds[5].node_id; /* Echo Repeater */
    charlie->neighbors.entries[0].snr = 3.25F;

    struct mesh_ui_node_summary *delta = &handshake.nodes[4];
    delta->neighbors.valid = true;
    delta->neighbors.time = now - 7200U;
    delta->neighbors.count = 0U;

    handshake.channel_count = 2U;
    handshake.channels[0].index = 0U;
    handshake.channels[0].role = 1U;
    snprintf(handshake.channels[0].name, sizeof handshake.channels[0].name, "%s", "LongFast");
    handshake.channels[0].psk_len = 1U;
    handshake.channels[1].index = 1U;
    handshake.channels[1].role = 2U;
    snprintf(handshake.channels[1].name, sizeof handshake.channels[1].name, "%s", "Trail");
    handshake.channels[1].psk_len = 16U;
    mesh_ui_store_set_handshake(&cap->store, &handshake);

    static const struct uicap_message_seed log[] = {
        {0x8F21B004U, "ALFA", "Heading up the ridge, back before dark", 93000U, 0U, true, false,
         MESH_MESSAGE_ACK_NONE},
        {0x8F21B004U, "ALFA", "Copy that, we'll keep the repeater warm", 92400U, 0U, true, true,
         MESH_MESSAGE_ACK_DELIVERED},
        {0x8F21B006U, "CHRL", "Lookout is clear, no smoke", 88000U, 0U, true, false,
         MESH_MESSAGE_ACK_NONE},
        {0x8F21B005U, "BRVO", "Are you still at the creek?", 5400U, 0U, false, true,
         MESH_MESSAGE_ACK_DELIVERED},
        {0x8F21B005U, "BRVO", "Yes, water is high but crossable", 5100U, 0U, false, false,
         MESH_MESSAGE_ACK_NONE},
        {0x8F21B005U, "BRVO", "Bring the long rope if you have it", 5040U, 0U, false, false,
         MESH_MESSAGE_ACK_NONE},
        {0x8F21B008U, "ECHO", "Repeater battery at 71%", 3000U, 1U, true, false,
         MESH_MESSAGE_ACK_NONE},
        {0x8F21B004U, "ALFA", "Anyone got eyes on the north trail?", 900U, 0U, true, false,
         MESH_MESSAGE_ACK_NONE},
        {0x8F21B006U, "CHRL", "Rolling that way now", 300U, 0U, true, false, MESH_MESSAGE_ACK_NONE},
        {0x8F21B009U, "FXTR", "Radio check", 120U, 0U, false, false, MESH_MESSAGE_ACK_NONE},
    };

    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof messages);
    messages.count = (uint32_t)(sizeof log / sizeof log[0]);
    for (uint32_t i = 0U; i < messages.count; ++i) {
        struct mesh_ui_message *entry = &messages.entries[i];
        entry->packet_id = cap->next_packet_id++;
        entry->peer = log[i].peer;
        entry->rx_time = now - log[i].sent_ago_s;
        snprintf(entry->peer_name, sizeof entry->peer_name, "%s", log[i].peer_name);
        snprintf(entry->text, sizeof entry->text, "%s", log[i].text);
        entry->channel = log[i].channel;
        entry->direction =
            log[i].outbound ? (uint8_t)MESH_MESSAGE_OUTBOUND : (uint8_t)MESH_MESSAGE_INBOUND;
        entry->ack = log[i].ack;
        entry->broadcast = log[i].broadcast;
        /* Direct messages in the demo went out end-to-end encrypted, which is what the modern
           firmware does and what the padlock on the bubble reports. */
        entry->pki_encrypted = !log[i].broadcast;
    }
    mesh_ui_store_set_messages(&cap->store, &messages);
    mesh_ui_store_set_transport_status(&cap->store, "running");
}

/* Nothing connected: what the HUD looks like before a radio is found. */
static void uicap_scene_empty(struct uicap *cap) {
    mesh_ui_store_set_transport_status(&cap->store, "scanning");
}

/* ---- frames ------------------------------------------------------------------------------ */

/*
 * Steps the clock every frame is drawn against.
 *
 * The scene's own clock and the renderer's are the same clock: a `hold` is the frame sitting on
 * screen for that long, so a knob that was mid-slide when it started has moved by the time the
 * next line runs. Anything else would film the HUD with a stopped watch.
 */
static void uicap_advance(struct uicap *cap, unsigned ms) {
    cap->now_ms += ms;
    mesh_ui_capture_advance(cap->capture, ms);
    /* The housekeeping the event loop does on every turn, which for the store is one thing: a
       transient notice expiring. Without it the scene's clock ran but nothing timed out, so a
       `toast` stayed up for the rest of the script and a notice sliding *away* - the half of
       that transition a still cannot show - was not filmable at all. */
    mesh_ui_store_tick(&cap->store, cap->now_ms);
}

/*
 * The interval a frame carries while something on it is still moving: 30 fps, the rate the
 * event loop's frame timer wakes the backend at on the device.
 */
#define UICAP_FRAME_MS 33U

static void uicap_emit_delay(struct uicap *cap, unsigned delay_ms) {
    if (!mesh_ui_store_consume_updates(&cap->store, &cap->snapshot)) {
        /* Nothing changed - a press the screen ignores, say. Draw it anyway: a clip that
           silently drops the frames where nothing happened is a clip that lies about what the
           button did. */
        mesh_ui_store_request_refresh(&cap->store);
        (void)mesh_ui_store_consume_updates(&cap->store, &cap->snapshot);
    }
    mesh_ui_capture_render(cap->capture, &cap->snapshot);

    char path[1024];
    cap->frame_count++;
    snprintf(path, sizeof path, "%s/%s-%04u.ppm", cap->out_dir, cap->prefix, cap->frame_count);
    const int status = mesh_ui_capture_write_ppm(cap->capture, path);
    if (status != 0) {
        fprintf(stderr, "uicap: cannot write %s: %s\n", path, strerror(-status));
        exit(1);
    }

    if (cap->frame_count > cap->delay_capacity) {
        const unsigned grown = cap->delay_capacity == 0U ? 64U : cap->delay_capacity * 2U;
        unsigned *delays = realloc(cap->delays, (size_t)grown * sizeof *delays);
        if (delays == NULL) {
            die("out of memory");
        }
        cap->delays = delays;
        cap->delay_capacity = grown;
    }
    /*
     * A frame that leaves something mid-transition carries the animation's interval, whatever
     * the scene asked for. The scene's delay is for a frame somebody is meant to read, and the
     * frame a press emits is not one: the knob has not moved yet, so holding it for the scene's
     * delay froze the old state for a third of a second before every slide - a pause the device
     * does not have and the clip should not invent. The frame the transition lands on is not
     * animating any more, so it keeps the scene's delay and a `hold` after it still works.
     */
    if (mesh_ui_capture_animating(cap->capture) && delay_ms > UICAP_FRAME_MS) {
        delay_ms = UICAP_FRAME_MS;
    }
    cap->delays[cap->frame_count - 1U] = delay_ms;
    if (!cap->quiet) {
        printf("  frame %u  %s-%04u.ppm\n", cap->frame_count, cap->prefix, cap->frame_count);
    }
}

static void uicap_emit(struct uicap *cap) { uicap_emit_delay(cap, cap->delay_ms); }

/*
 * Plays out whatever the last frame left moving.
 *
 * A press that flips a switch does not finish on the frame that handled it - the knob is a few
 * pixels into a slide. The renderer says so (mesh_ui_capture_animating), so the harness keeps
 * stepping the clock and drawing until it stops, exactly as the event loop's frame timer does
 * on the device. That is what makes an animation reviewable in a GIF without a single scene
 * script having to know an animation exists.
 *
 * Each frame's delay is uicap_emit_delay()'s decision, not this loop's: a frame that is still
 * moving takes the animation's interval and the one it lands on takes the scene's.
 *
 * The cap was a guard against a widget that never settles - a bug, but not one that should hang
 * a capture - and it is now also the length of one legitimate case: an *indeterminate* meter
 * loops for as long as it is on screen and has no landing frame to reach, so it films until the
 * cap and stops. That is the right amount of it to put in a clip, and a scene that wants more
 * asks for it with another `frame`.
 */
#define UICAP_MAX_ANIM_FRAMES 40U

static void uicap_settle(struct uicap *cap) {
    for (unsigned i = 0U; i < UICAP_MAX_ANIM_FRAMES; ++i) {
        if (!mesh_ui_capture_animating(cap->capture)) {
            return;
        }
        uicap_advance(cap, UICAP_FRAME_MS);
        uicap_emit(cap);
    }
}

/*
 * Picks the theme by name, and re-applies the scale because a theme carries one of its own.
 * `scale` of 0 means "whatever this theme asks for", which is what makes `theme light` alone do
 * the right thing and `scale 5` still win when a script says both.
 */
/*
 * Publishes the capture's theme as the client info a real app would.
 *
 * Without it the Settings > About screen in a capture has no Theme row, because the row is
 * drawn from what the app says it is drawing with - and there is no app here. This is the one
 * client fact the harness genuinely owns, so it fills that one and leaves the rest alone.
 */
static void uicap_publish_theme(struct uicap *cap) {
    const struct mesh_ui_theme *theme = mesh_ui_capture_theme(cap->capture);
    if (theme == NULL) {
        return;
    }
    struct mesh_ui_settings settings = cap->store.settings;
    snprintf(settings.client.theme, sizeof settings.client.theme, "%s", theme->id);
    snprintf(settings.client.theme_name, sizeof settings.client.theme_name, "%s", theme->name);
    /* Only when the capture really is drawing what MESHCLIENT_THEME named: a scene that picked
       its own theme is not being held by the environment, whatever the environment says. */
    settings.client.theme_from_env = (mesh_ui_theme_env() == theme);
    /* The About row that names the language. mesh_app_publish_ui_state() fills this in on the
       device; the harness has no app behind it, so a capture of About would otherwise be one
       row short of what a Brick draws. */
    settings.client.language_from_env = mesh_i18n_is_overridden();
    snprintf(settings.client.language_name, sizeof settings.client.language_name, "%s",
             mesh_i18n_locale()->name);
    mesh_ui_store_set_settings(&cap->store, &settings);
}

static void uicap_apply_theme(struct uicap *cap, const char *name, unsigned line_number) {
    const struct mesh_ui_theme *theme = mesh_ui_theme_by_id(name);
    if (theme == NULL) {
        fprintf(stderr, "uicap: line %u: no theme called '%s'. Try:", line_number, name);
        for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
            fprintf(stderr, " %s", mesh_ui_theme_at(i)->id);
        }
        fputc('\n', stderr);
        exit(1);
    }
    mesh_ui_capture_set_theme(cap->capture, theme);
    mesh_ui_capture_set_scale(cap->capture, cap->scale);
    uicap_publish_theme(cap);
}

static void uicap_start(struct uicap *cap) {
    if (cap->started) {
        return;
    }
    cap->started = true;
    if (cap->theme_id != NULL) {
        uicap_apply_theme(cap, cap->theme_id, 0U);
    }
    mesh_ui_capture_set_scale(cap->capture, cap->scale);
    if (strcmp(cap->scene, "demo") == 0) {
        uicap_scene_demo(cap);
    } else if (strcmp(cap->scene, "empty") == 0) {
        uicap_scene_empty(cap);
    } else {
        die("scene: expected demo or empty");
    }
    /* After the scene, which is free to publish settings of its own. */
    uicap_publish_theme(cap);
    mesh_ui_store_request_refresh(&cap->store);
    uicap_emit(cap);
}

/* Lengthens the frame just emitted rather than emitting a duplicate: a still moment in a clip
   is one frame that lingers, not twenty identical ones. */
static void uicap_hold(struct uicap *cap, unsigned extra_ms) {
    if (cap->frame_count == 0U) {
        die("hold: nothing has been captured yet");
    }
    unsigned *delay = &cap->delays[cap->frame_count - 1U];
    /* GIF carries the delay in centiseconds in a 16-bit field, so 655350 ms is the ceiling
       anything downstream can express. */
    *delay = *delay + extra_ms > 600000U ? 600000U : *delay + extra_ms;
    uicap_advance(cap, extra_ms);
}

/* ---- the script -------------------------------------------------------------------------- */

struct uicap_key_name {
    const char *name;
    enum mesh_ui_key key;
};

static enum mesh_ui_key uicap_key_from_name(const char *name) {
    static const struct uicap_key_name names[] = {
        {"up", MESH_UI_KEY_UP},       {"down", MESH_UI_KEY_DOWN},   {"left", MESH_UI_KEY_LEFT},
        {"right", MESH_UI_KEY_RIGHT}, {"a", MESH_UI_KEY_A},         {"b", MESH_UI_KEY_B},
        {"x", MESH_UI_KEY_X},         {"y", MESH_UI_KEY_Y},         {"l1", MESH_UI_KEY_L1},
        {"r1", MESH_UI_KEY_R1},       {"start", MESH_UI_KEY_START}, {"select", MESH_UI_KEY_SELECT},
    };
    for (size_t i = 0U; i < sizeof names / sizeof names[0]; ++i) {
        if (strcmp(names[i].name, name) == 0) {
            return names[i].key;
        }
    }
    return MESH_UI_KEY_NONE;
}

static int uicap_screen_from_name(const char *name) {
    static const char *const names[MESH_UI_SCREEN_COUNT] = {"messages", "nodes", "devices",
                                                            "status", "settings"};
    for (int i = 0; i < (int)MESH_UI_SCREEN_COUNT; ++i) {
        if (strcmp(names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void uicap_press(struct uicap *cap, enum mesh_ui_key key) {
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&cap->store, key, &action);
    uicap_emit(cap);
    uicap_settle(cap);
}

/* Walks the tabs with the buttons rather than assigning nav.screen, so a scene can only ever
   reach a screen the device can reach. */
static void uicap_tab(struct uicap *cap, int screen) {
    for (int guard = 0; guard < (int)MESH_UI_SCREEN_COUNT; ++guard) {
        const int current = (int)cap->store.nav.screen;
        if (current == screen) {
            return;
        }
        uicap_press(cap, current < screen ? MESH_UI_KEY_RIGHT : MESH_UI_KEY_LEFT);
    }
    die("tab: could not reach that tab (an overlay is open)");
}

static void uicap_append_message(struct uicap *cap, bool outbound, enum mesh_message_kind kind,
                                 const char *name, const char *text) {
    struct mesh_ui_message_list messages = cap->store.messages;
    if (messages.count >= MESH_UI_MAX_MESSAGES) {
        memmove(&messages.entries[0], &messages.entries[1],
                (MESH_UI_MAX_MESSAGES - 1U) * sizeof messages.entries[0]);
        messages.count = MESH_UI_MAX_MESSAGES - 1U;
        messages.dropped++;
    }

    /* Match the named node when the roster has it, so the message lands in that node's
       conversation rather than opening one against a made-up id. */
    uint32_t peer = 0U;
    for (uint32_t i = 0U; i < cap->store.handshake.node_count; ++i) {
        if (strcmp(cap->store.handshake.nodes[i].short_name, name) == 0) {
            peer = cap->store.handshake.nodes[i].node_id;
            break;
        }
    }
    if (peer == 0U) {
        die("message: no node in the scene has that short name");
    }

    struct mesh_ui_message *entry = &messages.entries[messages.count++];
    memset(entry, 0, sizeof *entry);
    entry->packet_id = cap->next_packet_id++;
    entry->peer = peer;
    entry->rx_time = (uint32_t)time(NULL);
    snprintf(entry->peer_name, sizeof entry->peer_name, "%s", name);
    snprintf(entry->text, sizeof entry->text, "%s", text);
    entry->direction = outbound ? (uint8_t)MESH_MESSAGE_OUTBOUND : (uint8_t)MESH_MESSAGE_INBOUND;
    entry->ack = outbound ? (uint8_t)MESH_MESSAGE_ACK_PENDING : (uint8_t)MESH_MESSAGE_ACK_NONE;
    entry->kind = (uint8_t)kind;
    /* An alert and a detection are broadcasts on a channel, which is what the firmware sends
       and what puts them in a conversation rather than in a private exchange. */
    entry->broadcast = (kind != MESH_MESSAGE_KIND_TEXT);
    mesh_ui_store_set_messages(&cap->store, &messages);
    uicap_emit(cap);
}

/*
 * A reaction to the newest message in the log, which is what a person reacting to what was
 * just said produces. It is appended like any other message and flagged; the transcript
 * filters it out of the bubbles and draws it on the one it names.
 */
static void uicap_append_reaction(struct uicap *cap, const char *name, const char *emoji) {
    struct mesh_ui_message_list messages = cap->store.messages;
    if (messages.count == 0U || messages.count >= MESH_UI_MAX_MESSAGES) {
        die("react: needs a message to react to, and room for it");
    }

    uint32_t peer = 0U;
    for (uint32_t i = 0U; i < cap->store.handshake.node_count; ++i) {
        if (strcmp(cap->store.handshake.nodes[i].short_name, name) == 0) {
            peer = cap->store.handshake.nodes[i].node_id;
            break;
        }
    }
    if (peer == 0U) {
        die("react: no node in the scene has that short name");
    }

    /* The newest message that is not itself a reaction: reacting to a reaction is not a thing
       the firmware produces, and it would attach the chip to nothing on screen. */
    const struct mesh_ui_message *target = NULL;
    for (uint32_t i = messages.count; i > 0U; --i) {
        if (!messages.entries[i - 1U].is_reaction) {
            target = &messages.entries[i - 1U];
            break;
        }
    }
    if (target == NULL) {
        die("react: nothing in the log to react to");
    }

    struct mesh_ui_message *entry = &messages.entries[messages.count++];
    const uint32_t reply_id = target->packet_id;
    const uint8_t channel = target->channel;
    const bool broadcast = target->broadcast;
    memset(entry, 0, sizeof *entry);
    entry->packet_id = cap->next_packet_id++;
    entry->peer = peer;
    entry->rx_time = (uint32_t)time(NULL);
    entry->channel = channel;
    entry->broadcast = broadcast;
    snprintf(entry->peer_name, sizeof entry->peer_name, "%s", name);
    snprintf(entry->text, sizeof entry->text, "%s", emoji);
    entry->direction = (uint8_t)MESH_MESSAGE_INBOUND;
    entry->is_reaction = true;
    entry->reply_id = reply_id;
    mesh_ui_store_set_messages(&cap->store, &messages);
    uicap_emit(cap);
}

/* Splits off the next whitespace-delimited word, leaving *rest on what follows. */
static char *uicap_word(char **rest) {
    char *cursor = *rest;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor == '\0') {
        *rest = cursor;
        return NULL;
    }
    char *start = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        cursor++;
    }
    if (*cursor != '\0') {
        *cursor++ = '\0';
    }
    *rest = cursor;
    return start;
}

static char *uicap_tail(char *rest) {
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    return rest;
}

static unsigned uicap_number(const char *text, const char *what) {
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 600000UL) {
        fprintf(stderr, "uicap: %s: '%s' is not a number I can use\n", what, text);
        exit(1);
    }
    return (unsigned)value;
}

static void uicap_run_line(struct uicap *cap, char *line, unsigned line_number) {
    char *rest = line;
    char *command = uicap_word(&rest);
    if (command == NULL || command[0] == '#') {
        return;
    }

    if (strcmp(command, "scene") == 0 || strcmp(command, "scale") == 0 ||
        strcmp(command, "delay") == 0) {
        if (cap->started) {
            fprintf(stderr, "uicap: line %u: '%s' has to come before the first frame\n",
                    line_number, command);
            exit(1);
        }
        char *value = uicap_word(&rest);
        if (value == NULL) {
            fprintf(stderr, "uicap: line %u: '%s' needs a value\n", line_number, command);
            exit(1);
        }
        if (strcmp(command, "scene") == 0) {
            cap->scene = strdup(value);
        } else if (strcmp(command, "scale") == 0) {
            cap->scale = (int)uicap_number(value, "scale");
        } else {
            cap->delay_ms = uicap_number(value, "delay");
        }
        return;
    }

    /* A theme before the first frame chooses the look; after it, switching is itself the thing
       worth filming, so it emits a frame like every other command. */
    if (strcmp(command, "theme") == 0) {
        char *value = uicap_word(&rest);
        if (value == NULL) {
            fprintf(stderr, "uicap: line %u: 'theme' needs a name\n", line_number);
            exit(1);
        }
        if (!cap->started) {
            cap->theme_id = strdup(value);
            return;
        }
        uicap_apply_theme(cap, value, line_number);
        uicap_emit(cap);
        return;
    }

    if (strcmp(command, "key") == 0) {
        char *name = uicap_word(&rest);
        char *count_text = uicap_word(&rest);
        if (name == NULL) {
            fprintf(stderr, "uicap: line %u: 'key' needs a button\n", line_number);
            exit(1);
        }
        const enum mesh_ui_key key = uicap_key_from_name(name);
        if (key == MESH_UI_KEY_NONE) {
            fprintf(stderr, "uicap: line %u: no button called '%s'\n", line_number, name);
            exit(1);
        }
        const unsigned count = count_text != NULL ? uicap_number(count_text, "key count") : 1U;
        uicap_start(cap);
        for (unsigned i = 0U; i < count; ++i) {
            uicap_press(cap, key);
        }
        return;
    }

    if (strcmp(command, "tab") == 0) {
        char *name = uicap_word(&rest);
        if (name == NULL) {
            fprintf(stderr, "uicap: line %u: 'tab' needs a name\n", line_number);
            exit(1);
        }
        const int screen = uicap_screen_from_name(name);
        if (screen < 0) {
            fprintf(stderr, "uicap: line %u: no tab called '%s'\n", line_number, name);
            exit(1);
        }
        uicap_start(cap);
        uicap_tab(cap, screen);
        return;
    }

    if (strcmp(command, "frame") == 0) {
        uicap_start(cap);
        uicap_emit(cap);
        /* Whatever the clock has started since the last frame - a notice that timed out during
           a `hold` is the case - plays out here, the same way a press's does. */
        uicap_settle(cap);
        return;
    }

    if (strcmp(command, "hold") == 0) {
        char *value = uicap_word(&rest);
        if (value == NULL) {
            fprintf(stderr, "uicap: line %u: 'hold' needs a duration in ms\n", line_number);
            exit(1);
        }
        uicap_start(cap);
        uicap_hold(cap, uicap_number(value, "hold"));
        return;
    }

    if (strcmp(command, "toast") == 0) {
        uicap_start(cap);
        mesh_ui_store_set_toast(&cap->store, cap->now_ms, uicap_tail(rest));
        uicap_emit(cap);
        uicap_settle(cap);
        return;
    }

    if (strcmp(command, "status") == 0) {
        uicap_start(cap);
        mesh_ui_store_set_transport_status(&cap->store, uicap_tail(rest));
        uicap_emit(cap);
        return;
    }

    /*
     * The three Status rows that describe the radio rather than the traffic. Each is a
     * read-modify-write of the settings view, because the store replaces it wholesale and the
     * demo scene has already put a radio behind it.
     */
    if (strcmp(command, "react") == 0) {
        char *name = uicap_word(&rest);
        if (name == NULL) {
            fprintf(stderr, "uicap: line %u: 'react' needs a short name and an emoji\n",
                    line_number);
            exit(1);
        }
        uicap_start(cap);
        uicap_append_reaction(cap, name, uicap_tail(rest));
        return;
    }

    /* A pinned node. Its own verb for the same reason `offradio` is: X on the Nodes tab raises
       a mesh_ui_action and the store stops there, so the press this harness can make does not
       reach the flag. Without it the marker gutter's star has nothing to draw and the one row
       shape that carries it cannot be looked at. */
    if (strcmp(command, "pin") == 0) {
        char *name = uicap_word(&rest);
        if (name == NULL) {
            fprintf(stderr, "uicap: line %u: 'pin' needs a short name\n", line_number);
            exit(1);
        }
        uicap_start(cap);
        struct mesh_ui_handshake_state handshake = cap->store.handshake;
        bool matched = false;
        for (uint32_t i = 0; i < handshake.node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
            struct mesh_ui_node_summary *node = &handshake.nodes[i];
            /* Our own node is never pinned - nav.c and node_detail.c both refuse it - so the
               harness refuses it too rather than drawing a star no press could clear. */
            if (node->node_id == handshake.my_info.node_num) {
                continue;
            }
            if (strcmp(node->short_name, name) == 0) {
                node->is_favorite = true;
                matched = true;
            }
        }
        if (!matched) {
            fprintf(stderr, "uicap: line %u: no node in the scene called '%s'\n", line_number,
                    name);
            exit(1);
        }
        /* A pinned node survives a forget, so it leaves the counts the Settings rows offer -
           the same arithmetic `offradio` below makes for the same reason. */
        handshake.nodes_forgettable_off_radio = 0U;
        handshake.nodes_forgettable_all = 0U;
        for (uint32_t i = 0; i < handshake.node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
            const struct mesh_ui_node_summary *node = &handshake.nodes[i];
            if (node->is_favorite || node->node_id == handshake.my_info.node_num) {
                continue;
            }
            ++handshake.nodes_forgettable_all;
            if (!node->in_nodedb) {
                ++handshake.nodes_forgettable_off_radio;
            }
        }
        mesh_ui_store_set_handshake(&cap->store, &handshake);
        uicap_emit(cap);
        return;
    }

    /* The state a NodeDB reset leaves the roster in: the nodes are still ours, and the radio
       has stopped carrying them. Its own verb because no key press can reach it - the reset
       goes out over the air and the answer comes back on the next sync, neither of which
       exists behind the harness. */
    if (strcmp(command, "offradio") == 0) {
        char *name = uicap_word(&rest);
        if (name == NULL) {
            fprintf(stderr, "uicap: line %u: 'offradio' needs a short name or 'all'\n",
                    line_number);
            exit(1);
        }
        uicap_start(cap);
        struct mesh_ui_handshake_state handshake = cap->store.handshake;
        const bool all = strcmp(name, "all") == 0;
        bool matched = false;
        for (uint32_t i = 0; i < handshake.node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
            struct mesh_ui_node_summary *node = &handshake.nodes[i];
            if (node->node_id == handshake.my_info.node_num) {
                continue; /* our own radio is never a node its own database has forgotten */
            }
            if (all || strcmp(node->short_name, name) == 0) {
                node->in_nodedb = false;
                matched = true;
            }
        }
        if (!matched) {
            fprintf(stderr, "uicap: line %u: no node in the scene called '%s'\n", line_number,
                    name);
            exit(1);
        }
        /* What the Settings rows offer to drop. Pinned nodes and our own record survive a
           forget, so they are not in either count - the same arithmetic the app publishes. */
        const uint32_t off_radio = mesh_ui_handshake_off_radio(&handshake);
        handshake.nodes_forgettable_off_radio = 0U;
        handshake.nodes_forgettable_all = 0U;
        for (uint32_t i = 0; i < handshake.node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
            const struct mesh_ui_node_summary *node = &handshake.nodes[i];
            if (node->is_favorite || node->node_id == handshake.my_info.node_num) {
                continue;
            }
            ++handshake.nodes_forgettable_all;
            if (!node->in_nodedb) {
                ++handshake.nodes_forgettable_off_radio;
            }
        }
        /* The radio's own count goes with them: after a reset its database holds what it has
           re-heard, which is what makes the Status screen and the Nodes tab disagree. */
        handshake.my_info.nodedb_entries = handshake.node_count - off_radio;
        mesh_ui_store_set_handshake(&cap->store, &handshake);
        uicap_emit(cap);
        return;
    }

    if (strcmp(command, "notice") == 0) {
        char *level = uicap_word(&rest);
        if (level == NULL) {
            fprintf(stderr, "uicap: line %u: 'notice' needs info|warn|error and text\n",
                    line_number);
            exit(1);
        }
        uicap_start(cap);
        struct mesh_ui_settings settings = cap->store.settings;
        /* python logging's scale, which is what LogRecord.Level is. */
        if (strcmp(level, "error") == 0) {
            settings.notice.level = 40U;
        } else if (strcmp(level, "warn") == 0) {
            settings.notice.level = 30U;
        } else if (strcmp(level, "info") == 0) {
            settings.notice.level = 20U;
        } else {
            fprintf(stderr, "uicap: line %u: 'notice' level is info, warn or error\n", line_number);
            exit(1);
        }
        settings.notice.seq++;
        settings.notice.received = (uint32_t)time(NULL);
        snprintf(settings.notice.text, sizeof settings.notice.text, "%s", uicap_tail(rest));
        mesh_ui_store_set_settings(&cap->store, &settings);
        uicap_emit(cap);
        return;
    }

    /*
     * A radio that has answered the config handshake.
     *
     * Without it every Settings section says "not loaded", because the demo scene has no radio
     * behind it - so the one tab whose rows are all controls was the one tab a capture could
     * not show. The values are plausible rather than meaningful: what is on show is the rows.
     */
    if (strcmp(command, "config") == 0) {
        uicap_start(cap);
        struct mesh_ui_settings settings = cap->store.settings;
        settings.loaded = true;
        settings.admin_ok = true;
        settings.has_owner = true;
        snprintf(settings.long_name, sizeof settings.long_name, "%s", "Brick");
        snprintf(settings.short_name, sizeof settings.short_name, "%s", "BRK");
        settings.has_device = true;
        settings.node_info_broadcast_secs = 10800U;
        settings.led_heartbeat_disabled = false;
        settings.double_tap_as_button_press = true;
        settings.has_display = true;
        settings.screen_on_secs = 600U;
        settings.carousel_secs = 0U;
        settings.use_12h_clock = true;
        settings.flip_screen = false;
        settings.has_lora = true;
        settings.use_preset = true;
        settings.tx_enabled = true;
        settings.hop_limit = 3U;
        settings.has_bluetooth = true;
        settings.bluetooth_enabled = true;
        mesh_ui_store_set_settings(&cap->store, &settings);
        uicap_emit(cap);
        return;
    }

    if (strcmp(command, "queue") == 0) {
        char *free_slots = uicap_word(&rest);
        char *maxlen = uicap_word(&rest);
        if (free_slots == NULL || maxlen == NULL) {
            fprintf(stderr, "uicap: line %u: 'queue' needs FREE and MAXLEN\n", line_number);
            exit(1);
        }
        const char *refused = uicap_word(&rest);
        uicap_start(cap);
        struct mesh_ui_settings settings = cap->store.settings;
        settings.queue.valid = true;
        settings.queue.free = (uint8_t)uicap_number(free_slots, "queue");
        settings.queue.maxlen = (uint8_t)uicap_number(maxlen, "queue");
        /* Any Routing_Error will do: the row says "refused", not which error it was. */
        settings.queue.res = (refused != NULL && strcmp(refused, "refused") == 0) ? 1 : 0;
        mesh_ui_store_set_settings(&cap->store, &settings);
        uicap_emit(cap);
        return;
    }

    /*
     * The radio's own airtime report: how much of the channel is busy, and how much of that is
     * ours. Two figures because they are the pair the Status card draws - the row's words and
     * the meter under them are the same number, and a scene that could only set one of them
     * could not show them agreeing.
     */
    if (strcmp(command, "airtime") == 0) {
        char *busy = uicap_word(&rest);
        if (busy == NULL) {
            fprintf(stderr, "uicap: line %u: 'airtime' needs a busy percentage\n", line_number);
            exit(1);
        }
        const char *tx = uicap_word(&rest);
        uicap_start(cap);
        struct mesh_ui_settings settings = cap->store.settings;
        settings.stats.valid = true;
        settings.stats.channel_utilization = (float)uicap_number(busy, "airtime");
        settings.stats.air_util_tx = tx != NULL ? (float)uicap_number(tx, "airtime") : 0.0f;
        mesh_ui_store_set_settings(&cap->store, &settings);
        uicap_emit(cap);
        /* The bar eases to the new reading rather than jumping to it, so the frames between the
           two figures are the point - the same reason a press settles. */
        uicap_settle(cap);
        return;
    }

    /*
     * A self-update in flight, which the About screen draws as a meter.
     *
     * `update check` is the step with no length - the request is out and its reply has no size
     * until it lands - and `update download PERCENT` is the one that has a fraction, because
     * the release metadata said how big the asset would be. There is no updater behind the
     * harness (it forks curl and reaches the network, which a capture must not), so this sets
     * what mesh_app_publish_ui_state() would have published: the state, and the progress read
     * off the staged file.
     */
    if (strcmp(command, "update") == 0) {
        char *step = uicap_word(&rest);
        if (step == NULL) {
            fprintf(stderr, "uicap: line %u: 'update' needs check, download, available or ready\n",
                    line_number);
            exit(1);
        }
        const bool downloading = strcmp(step, "download") == 0;
        const bool checking = strcmp(step, "check") == 0;
        /* The two settled states, which is what a banner is for: a check that has finished and
           found something, and an install that has finished and is waiting for a restart.
           Neither is busy - the bar and the banner are the moving half and the settled half of
           the same story, and a scene has to be able to show them apart. */
        const bool available = strcmp(step, "available") == 0;
        const bool ready = strcmp(step, "ready") == 0;
        if (!downloading && !checking && !available && !ready) {
            fprintf(stderr, "uicap: line %u: 'update' takes check, download, available or ready\n",
                    line_number);
            exit(1);
        }
        const char *percent = uicap_word(&rest);
        uicap_start(cap);
        struct mesh_ui_settings settings = cap->store.settings;
        settings.client.update_supported = true;
        settings.client.update_busy = downloading || checking;
        /* A build that may install what it finds. mesh_ui_chrome_banner() gates the "available"
           banner on it, because an update a build cannot install is a notice nothing clears. */
        settings.client.update_can_install = true;
        settings.client.update_state = (uint8_t)(downloading ? MESH_UPDATE_DOWNLOADING
                                                 : checking  ? MESH_UPDATE_CHECKING
                                                 : available ? MESH_UPDATE_AVAILABLE
                                                             : MESH_UPDATE_READY);
        settings.client.update_progress_known = downloading && percent != NULL;
        settings.client.update_progress =
            (uint16_t)(settings.client.update_progress_known ? uicap_number(percent, "update") * 10U
                                                             : 0U);
        snprintf(settings.client.update_latest, sizeof settings.client.update_latest, "%s",
                 "999.0.0");
        mesh_ui_store_set_settings(&cap->store, &settings);
        uicap_emit(cap);
        uicap_settle(cap);
        return;
    }

    /*
     * A radio that has not finished answering, which is what the screen progress bar reports.
     *
     * The demo scene starts with the handshake complete, because every screen in it needs a
     * roster - so the one state the bar exists for is the one state a capture could not reach.
     * `syncing on` puts the handshake back in flight without touching the nodes it has already
     * published, exactly as a reconnect does.
     */
    if (strcmp(command, "syncing") == 0) {
        char *value = uicap_word(&rest);
        if (value == NULL || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0)) {
            fprintf(stderr, "uicap: line %u: 'syncing' takes on or off\n", line_number);
            exit(1);
        }
        uicap_start(cap);
        struct mesh_ui_handshake_state handshake = cap->store.handshake;
        const bool on = strcmp(value, "on") == 0;
        handshake.request_in_flight = on;
        handshake.config_complete = !on;
        mesh_ui_store_set_handshake(&cap->store, &handshake);
        uicap_emit(cap);
        uicap_settle(cap);
        return;
    }

    if (strcmp(command, "reboots") == 0) {
        char *count_text = uicap_word(&rest);
        if (count_text == NULL) {
            fprintf(stderr, "uicap: line %u: 'reboots' needs a count\n", line_number);
            exit(1);
        }
        uicap_start(cap);
        struct mesh_ui_settings settings = cap->store.settings;
        settings.reboot_notices = uicap_number(count_text, "reboots");
        mesh_ui_store_set_settings(&cap->store, &settings);
        uicap_emit(cap);
        return;
    }

    if (strcmp(command, "message") == 0) {
        char *direction = uicap_word(&rest);
        char *name = uicap_word(&rest);
        if (direction == NULL || name == NULL) {
            fprintf(stderr, "uicap: line %u: 'message' needs in|out, a short name and text\n",
                    line_number);
            exit(1);
        }
        if (strcmp(direction, "in") != 0 && strcmp(direction, "out") != 0) {
            fprintf(stderr, "uicap: line %u: 'message' direction is in or out\n", line_number);
            exit(1);
        }
        uicap_start(cap);
        uicap_append_message(cap, strcmp(direction, "out") == 0, MESH_MESSAGE_KIND_TEXT, name,
                             uicap_tail(rest));
        return;
    }

    /* The two ports that are "same as Text Message" upstream and were never accepted here. */
    if (strcmp(command, "alert") == 0 || strcmp(command, "detection") == 0) {
        char *name = uicap_word(&rest);
        if (name == NULL) {
            fprintf(stderr, "uicap: line %u: '%s' needs a short name and text\n", line_number,
                    command);
            exit(1);
        }
        uicap_start(cap);
        uicap_append_message(cap, false,
                             strcmp(command, "alert") == 0 ? MESH_MESSAGE_KIND_ALERT
                                                           : MESH_MESSAGE_KIND_DETECTION,
                             name, uicap_tail(rest));
        return;
    }

    fprintf(stderr, "uicap: line %u: unknown command '%s'\n", line_number, command);
    exit(1);
}

static void uicap_usage(void) {
    fputs("usage: meshclient_uicap [--script FILE] [--out DIR] [--scale N] [--delay MS]\n"
          "                        [--theme NAME] [--prefix NAME] [--quiet] [--reference]\n\n"
          "Reads a scene script (stdin by default), writes DIR/NAME-NNNN.ppm and\n"
          "DIR/frames.txt. See devtools/ui_capture/scenes/ for examples.\n",
          stderr);
}

int main(int argc, char **argv) {
    mesh_i18n_init();
    struct uicap cap;
    memset(&cap, 0, sizeof cap);
    cap.out_dir = "capture";
    cap.prefix = "frame";
    cap.delay_ms = UICAP_DEFAULT_DELAY_MS;
    cap.scene = "demo";
    cap.now_ms = 1000U;
    cap.next_packet_id = 0x5A0001U;

    bool reference = false;
    const char *script_path = NULL;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(arg, "--script") == 0 && value != NULL) {
            script_path = argv[++i];
        } else if (strcmp(arg, "--out") == 0 && value != NULL) {
            cap.out_dir = argv[++i];
        } else if (strcmp(arg, "--prefix") == 0 && value != NULL) {
            cap.prefix = argv[++i];
        } else if (strcmp(arg, "--scale") == 0 && value != NULL) {
            cap.scale = (int)uicap_number(argv[++i], "--scale");
        } else if (strcmp(arg, "--delay") == 0 && value != NULL) {
            cap.delay_ms = uicap_number(argv[++i], "--delay");
        } else if (strcmp(arg, "--theme") == 0 && value != NULL) {
            cap.theme_id = argv[++i];
        } else if (strcmp(arg, "--reference") == 0) {
            reference = true;
        } else if (strcmp(arg, "--quiet") == 0) {
            cap.quiet = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            uicap_usage();
            return 0;
        } else {
            fprintf(stderr, "uicap: unexpected argument '%s'\n", arg);
            uicap_usage();
            return 2;
        }
    }

    if (mkdir(cap.out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "uicap: cannot create %s: %s\n", cap.out_dir, strerror(errno));
        return 1;
    }

    if (mesh_ui_store_init(&cap.store) != 0) {
        die("cannot initialise the UI store");
    }
    if (mesh_ui_capture_open(&cap.capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT,
                             cap.scale) != 0) {
        die("cannot allocate the off-screen page");
    }

    mesh_ui_capture_set_reference(cap.capture, reference);

    FILE *script = stdin;
    if (script_path != NULL && strcmp(script_path, "-") != 0) {
        script = fopen(script_path, "r");
        if (script == NULL) {
            fprintf(stderr, "uicap: cannot read %s: %s\n", script_path, strerror(errno));
            return 1;
        }
    }

    char line[UICAP_LINE_MAX];
    unsigned line_number = 0U;
    while (fgets(line, (int)sizeof line, script) != NULL) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        uicap_run_line(&cap, line, line_number);
    }
    if (script != stdin) {
        fclose(script);
    }

    /* A script that only set up the scene still owes one frame. */
    uicap_start(&cap);

    char manifest_path[1024];
    snprintf(manifest_path, sizeof manifest_path, "%s/frames.txt", cap.out_dir);
    FILE *manifest = fopen(manifest_path, "w");
    if (manifest == NULL) {
        fprintf(stderr, "uicap: cannot write %s: %s\n", manifest_path, strerror(errno));
        return 1;
    }
    for (unsigned i = 0U; i < cap.frame_count; ++i) {
        fprintf(manifest, "%s-%04u.ppm\t%u\n", cap.prefix, i + 1U, cap.delays[i]);
    }
    fclose(manifest);

    free(cap.delays);
    mesh_ui_capture_close(cap.capture);
    mesh_ui_store_shutdown(&cap.store);

    if (!cap.quiet) {
        printf("uicap: %u frame%s in %s\n", cap.frame_count, cap.frame_count == 1U ? "" : "s",
               cap.out_dir);
    }
    return 0;
}
