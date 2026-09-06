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
 *   scale N                glyph multiplier, 2..6                 (setup, default 4)
 *   delay MS               per-frame delay written to the manifest (setup, default 140)
 *   tab NAME               walk Left/Right to messages|nodes|devices|status|settings
 *   key NAME [COUNT]       up down left right a b x y l1 r1 start select
 *   hold MS                add MS to the delay of the frame just emitted
 *   frame                  emit the current screen again
 *   toast TEXT             raise the transient notice backends draw in the footer
 *   message in|out NAME TEXT   append a message to the log, as if the radio had just said so
 *   status TEXT            set the transport status line
 *
 * Every command but the setup three emits one frame (`key ... 3` emits three), and the screen
 * the script starts on is emitted before any of them.
 */

#include "mesh/core/message.h"
#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

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

    static const struct uicap_node_seed seeds[] = {
        {0x43A1C0DEU, "HOME", "Home Base", 30U, 11.5F, 0U, true, false},
        {0x8F21B004U, "ALFA", "Alfa Ridge", 95U, 8.25F, 1U, true, false},
        {0x8F21B005U, "BRVO", "Bravo Creek", 640U, -3.5F, 2U, true, false},
        {0x8F21B006U, "CHRL", "Charlie Lookout", 2400U, 4.0F, 1U, true, false},
        {0x8F21B007U, "DLTA", "Delta Camp", 5400U, 0.0F, 0U, false, true},
        {0x8F21B008U, "ECHO", "Echo Repeater", 9000U, 6.75F, 3U, true, false},
        {0x8F21B009U, "FXTR", "Foxtrot Mobile", 21600U, -8.0F, 0U, false, true},
        {0x8F21B00AU, "GOLF", "Golf Cabin", 76000U, 2.5F, 2U, true, false},
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
    }
    mesh_ui_store_set_messages(&cap->store, &messages);
    mesh_ui_store_set_transport_status(&cap->store, "running");
}

/* Nothing connected: what the HUD looks like before a radio is found. */
static void uicap_scene_empty(struct uicap *cap) {
    mesh_ui_store_set_transport_status(&cap->store, "scanning");
}

/* ---- frames ------------------------------------------------------------------------------ */

static void uicap_emit(struct uicap *cap) {
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
    cap->delays[cap->frame_count - 1U] = cap->delay_ms;
    if (!cap->quiet) {
        printf("  frame %u  %s-%04u.ppm\n", cap->frame_count, cap->prefix, cap->frame_count);
    }
}

static void uicap_start(struct uicap *cap) {
    if (cap->started) {
        return;
    }
    cap->started = true;
    mesh_ui_capture_set_scale(cap->capture, cap->scale);
    if (strcmp(cap->scene, "demo") == 0) {
        uicap_scene_demo(cap);
    } else if (strcmp(cap->scene, "empty") == 0) {
        uicap_scene_empty(cap);
    } else {
        die("scene: expected demo or empty");
    }
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

static void uicap_append_message(struct uicap *cap, bool outbound, const char *name,
                                 const char *text) {
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
        return;
    }

    if (strcmp(command, "status") == 0) {
        uicap_start(cap);
        mesh_ui_store_set_transport_status(&cap->store, uicap_tail(rest));
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
        uicap_append_message(cap, strcmp(direction, "out") == 0, name, uicap_tail(rest));
        return;
    }

    fprintf(stderr, "uicap: line %u: unknown command '%s'\n", line_number, command);
    exit(1);
}

static void uicap_usage(void) {
    fputs("usage: meshclient_uicap [--script FILE] [--out DIR] [--scale N] [--delay MS]\n"
          "                        [--prefix NAME] [--quiet]\n\n"
          "Reads a scene script (stdin by default), writes DIR/NAME-NNNN.ppm and\n"
          "DIR/frames.txt. See devtools/ui_capture/scenes/ for examples.\n",
          stderr);
}

int main(int argc, char **argv) {
    struct uicap cap;
    memset(&cap, 0, sizeof cap);
    cap.out_dir = "capture";
    cap.prefix = "frame";
    cap.delay_ms = UICAP_DEFAULT_DELAY_MS;
    cap.scene = "demo";
    cap.now_ms = 1000U;
    cap.next_packet_id = 0x5A0001U;

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
