/*
 * What the client can explain about where the user is standing.
 *
 * One job: read the nav, and answer with the paragraphs that belong to the screen behind it -
 * as catalog ids, never as text. The notes themselves live on the things they describe (a
 * section's beside its icon in settings.c, a field's in its own k_fields row), because what a
 * setting does is a property of the setting. This file only assembles.
 *
 * Two things can be standing behind the help screen and they are answered differently.
 *
 * A *settings section* is a list of fields, so its topic is built by walking the rows: the
 * section's own note first, then whichever rows carry one. Nothing about that list is written
 * down here, which is the point - a field acquires help by naming an id in the row somebody was
 * already editing.
 *
 * A *feature* has no rows to read a note off. What the Waypoints tab is for is not a property of
 * any one of the places on it, so those topics are the table at the top of this file, keyed on
 * the route underneath the help screen. Keyed on the route rather than on the nav's flags for
 * mesh/ui/route.h's reason: a new way of reaching a screen then arrives with the right help
 * already attached, instead of with a condition somebody has to remember to add here.
 *
 * See docs/help.md.
 */

#include "mesh/ui/help.h"

#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store_handshake.h"
#include "mesh/ui/store_settings.h"

#include <string.h>

/* ---- the features ---------------------------------------------------------------------------- */

/*
 * What each screen that is not a list of settings has to say for itself.
 *
 * A table of tables: one array of paragraphs per feature, and one row per *route* pointing at
 * the array that answers for it. Two rows may name one array, which is how the Waypoints list
 * and one open place share a topic - they are one feature seen at two depths, and splitting the
 * paragraphs would mean writing "what a waypoint is" twice and watching the two drift.
 *
 * Ordered as the tabs are, which is the order a reader looking for the entry to edit will scan.
 */

static const struct mesh_ui_help_entry k_help_messages[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_MESSAGES},
    {MESH_STR_HELP_LABEL_MESSAGES_NEW, MESH_STR_HELP_NOTE_MESSAGES_NEW},
    {MESH_STR_HELP_LABEL_MESSAGES_DROP, MESH_STR_HELP_NOTE_MESSAGES_DROP},
    {MESH_STR_HELP_LABEL_MESSAGES_MUTE, MESH_STR_HELP_NOTE_MESSAGES_MUTE},
};

static const struct mesh_ui_help_entry k_help_thread[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_THREAD},
    {MESH_STR_HELP_LABEL_THREAD_HISTORY, MESH_STR_HELP_NOTE_THREAD_HISTORY},
    {MESH_STR_HELP_LABEL_THREAD_MARKS, MESH_STR_HELP_NOTE_THREAD_MARKS},
    {MESH_STR_HELP_LABEL_THREAD_REPLY, MESH_STR_HELP_NOTE_THREAD_REPLY},
    {MESH_STR_HELP_LABEL_THREAD_RESEND, MESH_STR_HELP_NOTE_THREAD_RESEND},
    {MESH_STR_HELP_LABEL_THREAD_ALL, MESH_STR_HELP_NOTE_THREAD_ALL},
};

static const struct mesh_ui_help_entry k_help_reaction[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_REACTION},
    {MESH_STR_HELP_LABEL_REACTION_SEND, MESH_STR_HELP_NOTE_REACTION_SEND},
    {MESH_STR_HELP_LABEL_REACTION_DELETE, MESH_STR_HELP_NOTE_REACTION_DELETE},
};

static const struct mesh_ui_help_entry k_help_nodes[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_NODES},
    {MESH_STR_HELP_LABEL_NODES_FILTER, MESH_STR_HELP_NOTE_NODES_FILTER},
    {MESH_STR_HELP_LABEL_NODES_SORT, MESH_STR_HELP_NOTE_NODES_SORT},
    {MESH_STR_HELP_LABEL_NODES_PIN, MESH_STR_HELP_NOTE_NODES_PIN},
    {MESH_STR_HELP_LABEL_NODES_CACHED, MESH_STR_HELP_NOTE_NODES_CACHED},
};

static const struct mesh_ui_help_entry k_help_map[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_MAP},
    {MESH_STR_HELP_LABEL_MAP_MOVE, MESH_STR_HELP_NOTE_MAP_MOVE},
    {MESH_STR_HELP_LABEL_MAP_PICK, MESH_STR_HELP_NOTE_MAP_PICK},
    {MESH_STR_HELP_LABEL_MAP_TRUST, MESH_STR_HELP_NOTE_MAP_TRUST},
};

/*
 * One node, which is the longest screen in the client and had the shortest explanation of it.
 *
 * Three paragraphs said what the screen was and then explained two of its hundred and twenty
 * rows, which is a help screen that answers the questions a reader did not arrive with. The four
 * added here are the ones the screen cannot answer itself, and each is a different kind of
 * unanswerable:
 *
 *   - the presses, because Left and Right stopped being the tab switch here and nothing on the
 *     frame but the action bar's two-cell verb says so;
 *   - the three verbs that change what the *radio* does, because "mute", "ignore" and "remove"
 *     are three words for what reads as one thing and only one of them is recoverable by
 *     pressing it again;
 *   - the signal readings, because a decibel figure means nothing to anyone who has not
 *     memorised the demodulator's floor, and because the row goes on printing a number that is
 *     about the last relay rather than about this node (mesh_ui_node_signal_heard());
 *   - the two neighbour groups, because they are the same subject read in both directions and
 *     one of them is assembled from every other node's report rather than reported at all.
 *
 * In the order a reader meets them: what the screen is, how to move around it, what the top of
 * it does, and then the four kinds of reading down it.
 */
static const struct mesh_ui_help_entry k_help_node[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_NODE},
    {MESH_STR_HELP_LABEL_NODE_GROUPS, MESH_STR_HELP_NOTE_NODE_GROUPS},
    /* Where the verbs went, and the two that did not have to go anywhere. It is here rather than
       among the readings because it is the only press on this screen that leads somewhere the
       reader cannot see from it; the verbs' own paragraphs are on that screen, with them. */
    {MESH_STR_HELP_LABEL_NODE_ACTIONS, MESH_STR_HELP_NOTE_NODE_ACTIONS},
    {MESH_STR_HELP_LABEL_NODE_SIGNAL, MESH_STR_HELP_NOTE_NODE_SIGNAL},
    {MESH_STR_HELP_LABEL_NODE_FIX, MESH_STR_HELP_NOTE_NODE_FIX},
    {MESH_STR_HELP_LABEL_NODE_TREND, MESH_STR_HELP_NOTE_NODE_TREND},
    {MESH_STR_HELP_LABEL_NODE_NEIGHBOURS, MESH_STR_HELP_NOTE_NODE_NEIGHBOURS},
};

/*
 * The node's verbs, which used to be the top of its detail and are now a screen.
 *
 * The paragraphs are the detail's own, moved rather than rewritten: they explain three verbs
 * that read as one thing, a key most readers have never had to think about, and the one row that
 * changes what every other screen means. What decides which of them are here is where the row
 * they are about is drawn, which is the rule the whole of this table is keyed on.
 */
static const struct mesh_ui_help_entry k_help_node_actions[] = {
    {MESH_STR_HELP_LABEL_NODE_VERBS, MESH_STR_HELP_NOTE_NODE_VERBS},
    /* The padlock paragraph is the one entry here about something drawn on a *different* screen:
       the mark in the transcript. It follows the row that changes it, which is on this one. The
       verification sheet itself has no topic, for the reason the confirm dialog has none - a
       panel asking the user a question is not a place to open an explanation over. */
    {MESH_STR_HELP_LABEL_NODE_KEY, MESH_STR_HELP_NOTE_NODE_KEY},
    {MESH_STR_HELP_LABEL_NODE_VERIFY, MESH_STR_HELP_NOTE_NODE_VERIFY},
    /* Beside the key paragraphs rather than with the verbs above them, because the key is what
       the row runs on: an admin request to a remote node is sealed to it, which is why the row
       is not offered for a node we hold none for. */
    {MESH_STR_HELP_LABEL_NODE_ADMIN, MESH_STR_HELP_NOTE_NODE_ADMIN},
};

static const struct mesh_ui_help_entry k_help_waypoints[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_WAYPOINTS},
    {MESH_STR_HELP_LABEL_WAYPOINT_NEW, MESH_STR_HELP_NOTE_WAYPOINT_NEW},
    {MESH_STR_HELP_LABEL_WAYPOINT_SHARE, MESH_STR_HELP_NOTE_WAYPOINT_SHARE},
    {MESH_STR_HELP_LABEL_WAYPOINT_DROP, MESH_STR_HELP_NOTE_WAYPOINT_DROP},
};

static const struct mesh_ui_help_entry k_help_devices[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_DEVICES},
    {MESH_STR_HELP_LABEL_DEVICES_PAIR, MESH_STR_HELP_NOTE_DEVICES_PAIR},
    {MESH_STR_HELP_LABEL_DEVICES_FORGET, MESH_STR_HELP_NOTE_DEVICES_FORGET},
    /* The last row of the list, and the one thing on this screen a reader cannot work out by
       pressing: why the address has to be numbers, and where the port went. */
    {MESH_STR_HELP_LABEL_DEVICES_NETWORK, MESH_STR_HELP_NOTE_DEVICES_NETWORK},
    /* The two things a row says that the client decided rather than measured: which radio it
       reaches for, and why the others' readings stop moving while a link is up. */
    {MESH_STR_HELP_LABEL_DEVICES_AUTO, MESH_STR_HELP_NOTE_DEVICES_AUTO},
    {MESH_STR_HELP_LABEL_DEVICES_SIGNAL, MESH_STR_HELP_NOTE_DEVICES_SIGNAL},
};

/*
 * The Status tab, whose cards are almost entirely numbers - which is what makes its topic
 * different in kind from the tabs either side of it.
 *
 * The other tabs' paragraphs are about *presses*: what X does to a node, why a place cannot be
 * deleted. Three of these four are about *readings*, because that is what a reader standing in a
 * field with a radio is actually stuck on. "1.1% busy, 0.1% tx, -120 dBm floor" is three figures
 * in three units with no room on the card to say what any of them is, and the card cannot grow
 * the room: this screen is the one column in the client that runs out of it (see
 * inkcell_fb_draw_card_reserving()). A note is where the sentence goes when the row cannot hold
 * one, which is the same rule a settings field's note follows one screen over.
 *
 * In the order the cards draw, as every other list about this screen is - the counts and the
 * airtime are the Mesh card, and the traffic totals are the Radio card under it.
 */
static const struct mesh_ui_help_entry k_help_status[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_STATUS},
    {MESH_STR_HELP_LABEL_STATUS_COUNTS, MESH_STR_HELP_NOTE_STATUS_COUNTS},
    {MESH_STR_HELP_LABEL_STATUS_AIRTIME, MESH_STR_HELP_NOTE_STATUS_AIRTIME},
    {MESH_STR_HELP_LABEL_STATUS_FLOOR, MESH_STR_HELP_NOTE_STATUS_FLOOR},
    {MESH_STR_HELP_LABEL_STATUS_TRAFFIC, MESH_STR_HELP_NOTE_STATUS_TRAFFIC},
    /* Last, because it is the one entry about a card most readers will never see: it is drawn
       only for a radio that asked to be proxied for. A reader who has one has come here on
       purpose and will read to the bottom; everybody else would have had a paragraph about MQTT
       ahead of the airtime figures they actually opened this for. */
    {MESH_STR_HELP_LABEL_STATUS_BROKER, MESH_STR_HELP_NOTE_STATUS_BROKER},
};

/*
 * The share screen.
 *
 * Its first paragraph is not about the screen at all: a QR code needs no explaining, and what
 * somebody standing here does need told is that the thing on the panel is a *secret*. The
 * client cannot enforce that - a code on a screen is readable by whoever is looking at it - so
 * the only place it can be said is here, second, where somebody who opened help will read it.
 */
static const struct mesh_ui_help_entry k_help_share[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_SHARE},
    {MESH_STR_HELP_LABEL_SHARE_KEYS, MESH_STR_HELP_NOTE_SHARE_KEYS},
    {MESH_STR_HELP_LABEL_SHARE_WHAT, MESH_STR_HELP_NOTE_SHARE_WHAT},
    {MESH_STR_HELP_LABEL_SHARE_IMPORT, MESH_STR_HELP_NOTE_SHARE_IMPORT},
};

/*
 * The contact code screen, whose help exists mostly to separate it from the one above.
 *
 * Two squares on two screens, and the reader has just been told that one of them is a secret to
 * be shown only to the people joining. This one is a public key and is safe to show to anybody,
 * which is the second paragraph - and the third is the other half of that, because "safe to
 * show" is not "proves who sent it", and the padlock in this client means the second thing.
 */
static const struct mesh_ui_help_entry k_help_contact[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_CONTACT},
    {MESH_STR_HELP_LABEL_CONTACT_SAFE, MESH_STR_HELP_NOTE_CONTACT_SAFE},
    {MESH_STR_HELP_LABEL_CONTACT_TRUST, MESH_STR_HELP_NOTE_CONTACT_TRUST},
    {MESH_STR_HELP_LABEL_CONTACT_IMPORT, MESH_STR_HELP_NOTE_CONTACT_IMPORT},
};

/* The airtime chart, whose two paragraphs are both about reading a picture rather than about
   working a screen - which is why it is a feature of its own rather than the Status tab's help
   one level in. What a reader arrives wanting to know here is what the axes mean, and the cards
   underneath have no axes. */
static const struct mesh_ui_help_entry k_help_trend[] = {
    {INKCELL_STR_NONE, MESH_STR_HELP_NOTE_TREND},
    {MESH_STR_HELP_LABEL_TREND_AXES, MESH_STR_HELP_NOTE_TREND_AXES},
    {MESH_STR_HELP_LABEL_TREND_MARKS, MESH_STR_HELP_NOTE_TREND_MARKS},
    /* And the one press the screen has. It is last rather than first because a reader arrives
       here wanting to know what the picture means; the control is what they want next. */
    {MESH_STR_HELP_LABEL_TREND_SPAN, MESH_STR_HELP_NOTE_TREND_SPAN},
};

/*
 * A node's own chart, which is the airtime one's twin and deliberately not its entry.
 *
 * Both are pictures with axes, so the shape of what has to be said is the same - but every
 * sentence in it differs. This one is a single reading rather than two lines, its rules are about
 * the node reporting the reading rather than about the band being busy, and the reading it is of
 * was chosen by the row the reader pressed, which the airtime chart has no equivalent of. A
 * shared entry would have had to say all of that in the general, which is how a help screen ends
 * up telling the reader nothing they could not see.
 */
static const struct mesh_ui_help_entry k_help_node_chart[] = {
    {MESH_STR_HELP_LABEL_NODE_CHART, MESH_STR_HELP_NOTE_NODE_CHART},
    {MESH_STR_HELP_LABEL_NODE_CHART_GAPS, MESH_STR_HELP_NOTE_NODE_CHART_GAPS},
    /* The same press with its own sentence, which is this table's whole argument: a node reports
       every half hour, so what the span picker does *here* is not what it does over a radio
       reporting every few minutes, and the general form of that would have said neither. */
    {MESH_STR_HELP_LABEL_NODE_CHART_SPAN, MESH_STR_HELP_NOTE_NODE_CHART_SPAN},
    /* And the press that turns the picture into the readings behind it, which is the one thing on
       this screen a reader cannot discover by looking: the bar names the keycap, and what the
       left-hand column of that list is measured from is not a thing a column of durations can
       say for itself. */
    {MESH_STR_HELP_LABEL_NODE_CHART_READINGS, MESH_STR_HELP_NOTE_NODE_CHART_READINGS},
};

struct help_feature {
    uint8_t screen; /* enum mesh_ui_screen */
    uint8_t level;  /* enum mesh_ui_route_level */
    inkcell_str_id subject;
    const struct mesh_ui_help_entry *entries;
    uint32_t count;
};

#define HELP_FEATURE(screen_, level_, subject_, entries_)                                          \
    {                                                                                              \
        (uint8_t)(screen_), (uint8_t)(level_), (subject_), (entries_),                             \
            (uint32_t)(sizeof(entries_) / sizeof((entries_)[0]))                                   \
    }

static const struct help_feature k_help_features[] = {
    HELP_FEATURE(MESH_UI_SCREEN_MESSAGES, MESH_UI_ROUTE_LIST, MESH_STR_TAB_MESSAGES,
                 k_help_messages),
    HELP_FEATURE(MESH_UI_SCREEN_MESSAGES, MESH_UI_ROUTE_THREAD, MESH_STR_HELP_SUBJECT_THREAD,
                 k_help_thread),
    HELP_FEATURE(MESH_UI_SCREEN_MESSAGES, MESH_UI_ROUTE_REACTION, MESH_STR_HELP_SUBJECT_REACTION,
                 k_help_reaction),
    HELP_FEATURE(MESH_UI_SCREEN_NODES, MESH_UI_ROUTE_LIST, MESH_STR_TAB_NODES, k_help_nodes),
    /* The map is its own feature rather than the Nodes tab's help one level in, because it is
       not the same screen with more of it: the d-pad does something else here, and that is the
       first thing its help has to say. */
    HELP_FEATURE(MESH_UI_SCREEN_NODES, MESH_UI_ROUTE_MAP, MESH_STR_HELP_SUBJECT_MAP, k_help_map),
    HELP_FEATURE(MESH_UI_SCREEN_NODES, MESH_UI_ROUTE_NODE, MESH_STR_HELP_SUBJECT_NODE, k_help_node),
    /* The verbs over that detail. Its own feature rather than the detail's help one level in,
       for the map's reason two rows up: it is not the same screen with more of it. Every row is
       a press here and none of them is on the screen underneath. */
    HELP_FEATURE(MESH_UI_SCREEN_NODES, MESH_UI_ROUTE_NODE_ACTIONS,
                 MESH_STR_HELP_SUBJECT_NODE_ACTIONS, k_help_node_actions),
    /* And the chart one of that detail's readings opens. Keyed on the route like every other
       entry here, which is what got it the right help without the press that opens it having to
       say anything: MESH_UI_ROUTE_TREND under the Nodes tab is this, and under the Radio tab is
       the airtime one two rows down. */
    HELP_FEATURE(MESH_UI_SCREEN_NODES, MESH_UI_ROUTE_TREND, MESH_STR_HELP_SUBJECT_NODE_CHART,
                 k_help_node_chart),
    /* The places, a level of this tab since they stopped being a tab of their own, and keeping
       the subject they had then. */
    HELP_FEATURE(MESH_UI_SCREEN_NODES, MESH_UI_ROUTE_WAYPOINTS, MESH_STR_TAB_WAYPOINTS,
                 k_help_waypoints),
    /* One open place, answered by the list's paragraphs: it is the same feature one level in,
       and the four things worth knowing about a waypoint do not change with the depth. */
    HELP_FEATURE(MESH_UI_SCREEN_NODES, MESH_UI_ROUTE_WAYPOINT, MESH_STR_HELP_SUBJECT_WAYPOINT,
                 k_help_waypoints),
    /* The Radio tab's three places: the cards it opens on, and the two levels those open. Each
       keeps the subject it had while it was a tab of its own, because each is still answering
       the question it was then. */
    HELP_FEATURE(MESH_UI_SCREEN_RADIO, MESH_UI_ROUTE_LIST, MESH_STR_TAB_STATUS, k_help_status),
    HELP_FEATURE(MESH_UI_SCREEN_RADIO, MESH_UI_ROUTE_DEVICES, MESH_STR_TAB_DEVICES, k_help_devices),
    HELP_FEATURE(MESH_UI_SCREEN_RADIO, MESH_UI_ROUTE_TREND, MESH_STR_HELP_SUBJECT_TREND,
                 k_help_trend),
    /* The Settings tab's two features. Every other screen under that tab is a settings section,
       whose paragraphs are the fields' own; these two have no fields and are not lists. */
    HELP_FEATURE(MESH_UI_SCREEN_SETTINGS, MESH_UI_ROUTE_SHARE, MESH_STR_HELP_SUBJECT_SHARE,
                 k_help_share),
    HELP_FEATURE(MESH_UI_SCREEN_SETTINGS, MESH_UI_ROUTE_CONTACT, MESH_STR_HELP_SUBJECT_CONTACT,
                 k_help_contact),
};

#undef HELP_FEATURE

/*
 * The place the help screen is being asked about.
 *
 * Read *under* the help screen, because when this is asked from an open help screen the topmost
 * level is help itself - and a help screen that asked what it was drawing rather than what it
 * was explaining would answer about itself and empty on the first repaint.
 *
 * One derivation for both halves of this file, which is what stops the two disagreeing about
 * which screen is in front of the user. It cost a bug to learn: the settings half asked the nav
 * instead ("which section is open?"), and that stays true while a keyboard is up over one of
 * that section's rows - so with SELECT moved ahead of the overlay dispatch, the press opened a
 * section's help over a half-typed field while the keyboard's own bar said nothing about it. The
 * feature half never had that failure, because a table keyed on the route cannot answer for a
 * route nobody put in it.
 */
static void help_place(const struct mesh_ui_nav *nav, struct mesh_ui_route *out) {
    mesh_ui_route_under_help(nav, out);
}

/*
 * The feature at this place, or NULL.
 *
 * The compose sheet, the keyboard, the picker and the confirm dialog are absent from the table
 * on purpose rather than by omission. Each is a question being asked of the user, and the way
 * out of a question is to answer it: help over one would be a second screen stacked on a press
 * that is waiting, and the screen underneath - which does have a topic - is where the
 * explanation belongs.
 */
static const struct help_feature *help_feature_for(const struct mesh_ui_route *route) {
    for (size_t i = 0; i < sizeof k_help_features / sizeof k_help_features[0]; ++i) {
        if (k_help_features[i].screen == route->screen &&
            k_help_features[i].level == route->level) {
            return &k_help_features[i];
        }
    }
    return NULL;
}

/* ---- settings sections ----------------------------------------------------------------------- */

/*
 * Whether a settings section is the screen in front of the user.
 *
 * The *route* decides that a section is showing and the nav says which one, and the split
 * matters: `settings_section` stays set under every overlay a section can raise, so asking the
 * nav alone answers "a section is open somewhere below" when the question is "a section is what
 * you are looking at". The two levels that count are a section and one channel slot - the two
 * places whose rows are settings. The two list-shaped sections (Modules, and Channels before a
 * slot is picked) are MESH_UI_ROUTE_SECTION as well: they have a note of their own, and their
 * rows are sections rather than fields, so a topic there is the overview and nothing else.
 */
static bool help_section_open(const struct mesh_ui_route *route, const struct mesh_ui_nav *nav,
                              enum mesh_ui_settings_section *out) {
    if (nav == NULL) {
        return false;
    }
    if (route->level != MESH_UI_ROUTE_SECTION && route->level != MESH_UI_ROUTE_CHANNEL) {
        return false;
    }
    if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    *out = (enum mesh_ui_settings_section)nav->settings_section;
    return *out < MESH_UI_SETTINGS_SECTION_COUNT;
}

/*
 * The open section's rows, with no pending edits applied.
 *
 * Deliberately without them: help says what a setting *is*, never what it is set to, so an edit
 * waiting to be saved changes nothing about which paragraphs this screen carries. Passing the
 * edits would also make the help screen's contents depend on how far through an edit the user
 * is, which is a way for the row a press was aimed at to move out from under it.
 */
static uint32_t help_section_items(const struct mesh_ui_settings *settings,
                                   const struct mesh_ui_handshake_state *handshake,
                                   const struct mesh_ui_nav *nav,
                                   enum mesh_ui_settings_section section,
                                   struct mesh_ui_settings_item *items) {
    return mesh_ui_settings_items(settings, handshake, NULL, 0U, section, nav->settings_channel,
                                  items, MESH_UI_SETTINGS_ITEMS_MAX);
}

/* The note for one row, or INKCELL_STR_NONE. A row that is not a field - a heading, a read-only
   fact, an action - has no field to ask about and so has no note; the section's own paragraph
   is what covers those. */
static inkcell_str_id help_item_note(const struct mesh_ui_settings_item *item) {
    if (item->field == MESH_UI_FIELD_NONE) {
        return INKCELL_STR_NONE;
    }
    return mesh_ui_settings_field_note(item->field);
}

/*
 * A feature's topic: the table's paragraphs, copied out in order.
 *
 * Copied rather than pointed at because a topic is a value the callers build on their own
 * stacks - the action bar makes one per frame - and because the settings half has to build its
 * entries anyway. One shape out of this module, whichever half answered.
 */
static bool help_feature_topic(const struct help_feature *feature, struct mesh_ui_help_topic *out) {
    out->title = MESH_STR_HELP_TITLE;
    out->subject = feature->subject;
    for (uint32_t i = 0; i < feature->count && out->count < MESH_UI_HELP_ENTRIES_MAX; ++i) {
        out->entries[out->count++] = feature->entries[i];
    }
    return out->count > 0U;
}

bool mesh_ui_help_topic(const struct mesh_ui_settings *settings,
                        const struct mesh_ui_handshake_state *handshake,
                        const struct mesh_ui_nav *nav, struct mesh_ui_help_topic *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    if (nav == NULL) {
        return false;
    }
    struct mesh_ui_route place;
    help_place(nav, &place);

    enum mesh_ui_settings_section section;
    if (settings == NULL || !help_section_open(&place, nav, &section)) {
        /*
         * Not a settings section, so ask the feature table. This is also the answer for the
         * Settings tab's own list of sections, which deliberately has no entry in it: each
         * section explains itself once opened, so a topic over the list would be a table of
         * contents for a table of contents.
         */
        const struct help_feature *feature = help_feature_for(&place);
        return feature != NULL && help_feature_topic(feature, out);
    }
    const inkcell_str_id overview = mesh_ui_settings_section_note(section);
    if (overview == INKCELL_STR_NONE) {
        /* Every section is supposed to have one, and a test says so - but a section that
           somehow does not is a screen with nothing to say, and offering the press for it would
           be worse than not offering it. */
        return false;
    }

    out->title = MESH_STR_HELP_TITLE;
    out->subject = mesh_ui_settings_section_label(section);
    out->entries[out->count].label = INKCELL_STR_NONE;
    out->entries[out->count].body = overview;
    out->count++;

    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t rows = help_section_items(settings, handshake, nav, section, items);
    for (uint32_t i = 0U; i < rows && out->count < MESH_UI_HELP_ENTRIES_MAX; ++i) {
        const inkcell_str_id note = help_item_note(&items[i]);
        if (note == INKCELL_STR_NONE) {
            continue;
        }
        /* The row's own name, as an id rather than copied off the item: a topic is ids the
           whole way down, so a test can read one without a locale in force and a backend
           cannot end up holding English. */
        out->entries[out->count].label = mesh_ui_settings_field_label_id(items[i].field);
        out->entries[out->count].body = note;
        out->count++;
    }

    /*
     * How to cross the groups, on the sections that have any.
     *
     * Conditional rather than a line on every section, and asked of the same predicate the
     * renderer and the navigation ask: two groups, because one group is nothing to cross. A
     * heading alone is not enough and that was the bug - a section whose single heading opened
     * its only group drew no cards and refused R2, while this still promised the jump. Offering
     * a note for a key that does nothing is the help screen doing what the action bar keeps a
     * single table to avoid.
     *
     * **Last, and that is load-bearing rather than a preference.** The overview is entry 0 and
     * the explained rows run 1..n in row order - mesh_ui_help_entry_for_row() walks the rows and
     * counts, rather than searching, so a paragraph inserted anywhere among them shifts every
     * row's landing out from under it. Written second, this opened Ham mode's note on the row
     * above it all the way down LoRa (`help_opens_where_the_cursor_was`). After the rows it
     * names no row, so nothing maps onto it and the correspondence is untouched.
     */
    if (out->count < MESH_UI_HELP_ENTRIES_MAX &&
        mesh_ui_settings_section_groups(items, rows) >= 2U) {
        out->entries[out->count].label = MESH_STR_HELP_LABEL_SETTINGS_GROUPS;
        out->entries[out->count].body = MESH_STR_HELP_NOTE_SETTINGS_GROUPS;
        out->count++;
    }
    return true;
}

/*
 * Whether the screen is holding a destructive question open.
 *
 * All five are the same shape: one press arms it, the two keycaps become the answer, and any
 * other press stands it back down. A screen in that state has spent its bar on a yes and a no,
 * so the help press is not on it - and a press that opened help would stand the question down
 * *and* open a screen, which is one keystroke doing two things the user can only see one of.
 *
 * Listed rather than derived, and that is the cost of the arming being five flags rather than
 * one. It is also why this is a function with a name instead of a condition in the predicate
 * below: the settings one was the only entry while SELECT was handled after the overlays, and
 * the day the press moved ahead of them the other four became reachable in exactly the way that
 * fails no build and shows in no screenshot.
 */
static bool help_question_armed(const struct mesh_ui_nav *nav) {
    return nav->settings_discard_armed || nav->devices_forget_armed || nav->node_remove_armed ||
           nav->waypoint_delete_armed || nav->messages_delete_armed;
}

bool mesh_ui_help_offered(const struct mesh_ui_settings *settings,
                          const struct mesh_ui_handshake_state *handshake,
                          const struct mesh_ui_nav *nav) {
    if (nav == NULL || help_question_armed(nav)) {
        return false;
    }
    struct mesh_ui_help_topic topic;
    return mesh_ui_help_topic(settings, handshake, nav, &topic);
}

uint32_t mesh_ui_help_entry_for_row(const struct mesh_ui_settings *settings,
                                    const struct mesh_ui_handshake_state *handshake,
                                    const struct mesh_ui_nav *nav, uint32_t row) {
    if (nav == NULL) {
        return 0U;
    }
    struct mesh_ui_route place;
    help_place(nav, &place);

    enum mesh_ui_settings_section section;
    if (settings == NULL || !help_section_open(&place, nav, &section)) {
        /*
         * A feature opens at its first paragraph, and that is not the settings answer failing
         * to apply - it is the difference between the two kinds of topic. A section's rows and
         * its paragraphs correspond one for one, so there is a row to open on; a feature's
         * paragraphs are about the screen rather than about the rows of it, and the third one
         * has no more claim to a place in the list than the first.
         */
        return 0U;
    }

    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t rows = help_section_items(settings, handshake, nav, section, items);
    if (row >= rows) {
        return 0U;
    }

    /*
     * Walk down to the row, counting the paragraphs that stand above it.
     *
     * `entry` is the index the next explained row would take, the section's own overview being
     * entry 0. `landing` is where this row opens: its own paragraph when it has one, and
     * otherwise the nearest one above it. The fallback is the useful answer rather than merely
     * the safe one - the paragraphs above a row are the ones about the setting it sits with,
     * and opening at the top every time would put the screen at the overview for two rows out
     * of three.
     *
     * A subheading is where "sits with" ends, so it resets the landing to the overview. That is
     * the correction Telemetry asked for: its rows are five groups and only one of them has an
     * explained row, so without the reset every row of Air quality, Power and Health opened on
     * a paragraph about Fahrenheit - the nearest note above, and about a different reading than
     * the row the question was asked from. The overview is the honest answer there, and it is
     * the paragraph that names all five readings.
     *
     * No clamp on the way out, and that is MESH_UI_HELP_ENTRIES_MAX's doing rather than an
     * omission: the entry count runs to one per row plus the overview, and a topic holds that
     * with room to spare, so this cannot name an entry the topic does not have. The spare is the
     * group note, which is appended *after* the rows precisely so that it is not one of the
     * indices counted here.
     */
    uint32_t entry = 1U;
    uint32_t landing = 0U;
    for (uint32_t i = 0U; i <= row; ++i) {
        if (items[i].kind == MESH_UI_SETTING_HEADING) {
            landing = 0U;
        } else if (help_item_note(&items[i]) != INKCELL_STR_NONE) {
            landing = entry;
            entry++;
        }
    }
    return landing;
}
