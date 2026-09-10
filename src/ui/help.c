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
#include "mesh/ui/store.h"

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
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_MESSAGES},
    {MESH_STR_HELP_LABEL_MESSAGES_NEW, MESH_STR_HELP_NOTE_MESSAGES_NEW},
    {MESH_STR_HELP_LABEL_MESSAGES_DROP, MESH_STR_HELP_NOTE_MESSAGES_DROP},
};

static const struct mesh_ui_help_entry k_help_thread[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_THREAD},
    {MESH_STR_HELP_LABEL_THREAD_MARKS, MESH_STR_HELP_NOTE_THREAD_MARKS},
    {MESH_STR_HELP_LABEL_THREAD_REPLY, MESH_STR_HELP_NOTE_THREAD_REPLY},
    {MESH_STR_HELP_LABEL_THREAD_ALL, MESH_STR_HELP_NOTE_THREAD_ALL},
};

static const struct mesh_ui_help_entry k_help_reaction[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_REACTION},
    {MESH_STR_HELP_LABEL_REACTION_SEND, MESH_STR_HELP_NOTE_REACTION_SEND},
};

static const struct mesh_ui_help_entry k_help_nodes[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_NODES},
    {MESH_STR_HELP_LABEL_NODES_PIN, MESH_STR_HELP_NOTE_NODES_PIN},
    {MESH_STR_HELP_LABEL_NODES_CACHED, MESH_STR_HELP_NOTE_NODES_CACHED},
};

static const struct mesh_ui_help_entry k_help_map[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_MAP},
    {MESH_STR_HELP_LABEL_MAP_MOVE, MESH_STR_HELP_NOTE_MAP_MOVE},
    {MESH_STR_HELP_LABEL_MAP_PICK, MESH_STR_HELP_NOTE_MAP_PICK},
    {MESH_STR_HELP_LABEL_MAP_TRUST, MESH_STR_HELP_NOTE_MAP_TRUST},
};

static const struct mesh_ui_help_entry k_help_node[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_NODE},
    {MESH_STR_HELP_LABEL_NODE_FIX, MESH_STR_HELP_NOTE_NODE_FIX},
    {MESH_STR_HELP_LABEL_NODE_TREND, MESH_STR_HELP_NOTE_NODE_TREND},
};

static const struct mesh_ui_help_entry k_help_waypoints[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_WAYPOINTS},
    {MESH_STR_HELP_LABEL_WAYPOINT_NEW, MESH_STR_HELP_NOTE_WAYPOINT_NEW},
    {MESH_STR_HELP_LABEL_WAYPOINT_SHARE, MESH_STR_HELP_NOTE_WAYPOINT_SHARE},
    {MESH_STR_HELP_LABEL_WAYPOINT_DROP, MESH_STR_HELP_NOTE_WAYPOINT_DROP},
};

static const struct mesh_ui_help_entry k_help_devices[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_DEVICES},
    {MESH_STR_HELP_LABEL_DEVICES_PAIR, MESH_STR_HELP_NOTE_DEVICES_PAIR},
    {MESH_STR_HELP_LABEL_DEVICES_FORGET, MESH_STR_HELP_NOTE_DEVICES_FORGET},
};

static const struct mesh_ui_help_entry k_help_status[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_STATUS},
    {MESH_STR_HELP_LABEL_STATUS_COUNTS, MESH_STR_HELP_NOTE_STATUS_COUNTS},
};

/* The airtime chart, whose two paragraphs are both about reading a picture rather than about
   working a screen - which is why it is a feature of its own rather than the Status tab's help
   one level in. What a reader arrives wanting to know here is what the axes mean, and the cards
   underneath have no axes. */
static const struct mesh_ui_help_entry k_help_trend[] = {
    {MESH_STR_NONE, MESH_STR_HELP_NOTE_TREND},
    {MESH_STR_HELP_LABEL_TREND_AXES, MESH_STR_HELP_NOTE_TREND_AXES},
    {MESH_STR_HELP_LABEL_TREND_MARKS, MESH_STR_HELP_NOTE_TREND_MARKS},
};

struct help_feature {
    uint8_t screen; /* enum mesh_ui_screen */
    uint8_t level;  /* enum mesh_ui_route_level */
    enum mesh_str_id subject;
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
    HELP_FEATURE(MESH_UI_SCREEN_WAYPOINTS, MESH_UI_ROUTE_LIST, MESH_STR_TAB_WAYPOINTS,
                 k_help_waypoints),
    /* One open place, answered by the list's paragraphs: it is the same feature one level in,
       and the four things worth knowing about a waypoint do not change with the depth. */
    HELP_FEATURE(MESH_UI_SCREEN_WAYPOINTS, MESH_UI_ROUTE_WAYPOINT, MESH_STR_HELP_SUBJECT_WAYPOINT,
                 k_help_waypoints),
    HELP_FEATURE(MESH_UI_SCREEN_DEVICES, MESH_UI_ROUTE_LIST, MESH_STR_TAB_DEVICES, k_help_devices),
    HELP_FEATURE(MESH_UI_SCREEN_STATUS, MESH_UI_ROUTE_LIST, MESH_STR_TAB_STATUS, k_help_status),
    HELP_FEATURE(MESH_UI_SCREEN_STATUS, MESH_UI_ROUTE_TREND, MESH_STR_HELP_SUBJECT_TREND,
                 k_help_trend),
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

/* The note for one row, or MESH_STR_NONE. A row that is not a field - a heading, a read-only
   fact, an action - has no field to ask about and so has no note; the section's own paragraph
   is what covers those. */
static enum mesh_str_id help_item_note(const struct mesh_ui_settings_item *item) {
    if (item->field == MESH_UI_FIELD_NONE) {
        return MESH_STR_NONE;
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
    const enum mesh_str_id overview = mesh_ui_settings_section_note(section);
    if (overview == MESH_STR_NONE) {
        /* Every section is supposed to have one, and a test says so - but a section that
           somehow does not is a screen with nothing to say, and offering the press for it would
           be worse than not offering it. */
        return false;
    }

    out->title = MESH_STR_HELP_TITLE;
    out->subject = mesh_ui_settings_section_label(section);
    out->entries[out->count].label = MESH_STR_NONE;
    out->entries[out->count].body = overview;
    out->count++;

    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t rows = help_section_items(settings, handshake, nav, section, items);
    for (uint32_t i = 0U; i < rows && out->count < MESH_UI_HELP_ENTRIES_MAX; ++i) {
        const enum mesh_str_id note = help_item_note(&items[i]);
        if (note == MESH_STR_NONE) {
            continue;
        }
        /* The row's own name, as an id rather than copied off the item: a topic is ids the
           whole way down, so a test can read one without a locale in force and a backend
           cannot end up holding English. */
        out->entries[out->count].label = mesh_ui_settings_field_label_id(items[i].field);
        out->entries[out->count].body = note;
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
     * omission: the entry count runs to one per row plus the overview, which is exactly what a
     * topic holds, so this cannot name an entry the topic does not have.
     */
    uint32_t entry = 1U;
    uint32_t landing = 0U;
    for (uint32_t i = 0U; i <= row; ++i) {
        if (items[i].kind == MESH_UI_SETTING_HEADING) {
            landing = 0U;
        } else if (help_item_note(&items[i]) != MESH_STR_NONE) {
            landing = entry;
            entry++;
        }
    }
    return landing;
}
