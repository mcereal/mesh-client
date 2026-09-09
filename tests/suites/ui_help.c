#define _POSIX_C_SOURCE 200809L

/*
 * What the client can explain, and whether the keycap and the press agree about it.
 *
 * Two kinds of case live here.
 *
 * The first are about the notes as *content*: every section has one, and none of them is longer
 * than the panel can show. Neither is checkable by reading a screenshot and both are the kind of
 * thing that rots silently - a section added in phase 12 with no note is a help key that opens
 * a screen with one paragraph on it, which fails no build.
 *
 * The second are about the press. The bar names SELECT exactly where SELECT does something, and
 * that is not a property either half can have on its own: it is an agreement between
 * src/ui/actions.c and src/ui/nav.c, both of which ask src/ui/help.c. The way to check an
 * agreement is to walk the real key handler and compare the two answers on every screen, which
 * is what help_keycap_and_press_agree does.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/actions.h"
#include "mesh/ui/help.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdio.h>
#include <string.h>

/* The longest a note may be, from docs/help.md. Not a buffer size - a note is a `const char *`
   straight out of the catalog and nothing copies it - but the width of a 3.2" panel, past which
   a paragraph stops being read. */
#define HELP_NOTE_MAX 200U

/*
 * A store on the Settings tab with the radio's LoRa configuration in it.
 *
 * The configuration matters for one of the cases and not the rest: an unloaded section still has
 * an overview to show - that is the point of the topic leading with the section's own note - but
 * a section with no rows has no *field* notes, and the LoRa cases are about both halves.
 */
static bool help_store_open(struct mesh_ui_store *store, enum mesh_ui_settings_section section) {
    if (mesh_ui_store_init(store) != 0) {
        return false;
    }
    mesh_test_nav_populate(store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.use_preset = true;
    /* The owner fragment as well as the modem one, because a section is only built once the
       radio has sent what it is made of - and User is the section with a TEXT row on it, which
       is the only way to raise a keyboard over a section. */
    settings.has_owner = true;
    mesh_ui_store_set_settings(store, &settings);
    return mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS) &&
           mesh_test_settings_open(store, section);
}

static void press(struct mesh_ui_store *store, enum mesh_ui_key key) {
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(store, key, &action);
}

/* The three things the bar and the help model read, copied out of the store. Deliberately not
   the whole store: naming what a snapshot has to carry for these two to work is the point. */
static void help_snapshot(const struct mesh_ui_store *store, struct mesh_ui_snapshot *out) {
    memset(out, 0, sizeof *out);
    out->nav = store->nav;
    out->settings = store->settings;
    out->handshake = store->handshake;
    out->handshake_valid = store->handshake_valid;
}

/* Whether the action bar for this store's state names the help press. */
static bool bar_offers_help(const struct mesh_ui_store *store) {
    struct mesh_ui_snapshot snapshot;
    help_snapshot(store, &snapshot);
    struct mesh_ui_action_bar bar;
    mesh_ui_actions_for(&snapshot, &bar);
    for (size_t i = 0; i < bar.count; ++i) {
        if (bar.items[i].button == MESH_UI_BUTTON_SELECT) {
            return true;
        }
    }
    return false;
}

static bool topic_for(const struct mesh_ui_store *store, struct mesh_ui_help_topic *out) {
    return mesh_ui_help_topic(&store->settings, store->handshake_valid ? &store->handshake : NULL,
                              &store->nav, out);
}

/* ---- the notes as content ------------------------------------------------------------------ */

/*
 * Every section explains itself.
 *
 * This is what makes the press worth offering at all: help is per section rather than per row
 * precisely so the key always does something, and that only holds while every section has an
 * overview. A section added without one would silently be a section with no help key.
 */
MESH_TEST_CASE(help_every_section_has_a_note, unit) {
    for (int i = 0; i < (int)MESH_UI_SETTINGS_SECTION_COUNT; ++i) {
        const enum mesh_ui_settings_section section = (enum mesh_ui_settings_section)i;
        const enum mesh_str_id note = mesh_ui_settings_section_note(section);
        if (note == MESH_STR_NONE) {
            char reason[128];
            snprintf(reason, sizeof reason, "section %s has no note",
                     mesh_ui_settings_section_name(section));
            record_failure(test_name, reason);
            return;
        }
        MESH_TEST_FAIL_IF(mesh_str_in(mesh_i18n_locale_english(), note)[0] == '\0',
                          "a section's note is empty");
    }
    /* Out of range answers "nothing to say" rather than reading past the table. */
    MESH_TEST_FAIL_IF(mesh_ui_settings_section_note(
                          (enum mesh_ui_settings_section)MESH_UI_SETTINGS_SECTION_COUNT) !=
                          MESH_STR_NONE,
                      "a section past the end has a note");
    record_success(test_name);
}

/*
 * No note outruns the panel.
 *
 * The help screen wraps rather than elides, so a note that is too long does not get cut - it
 * pushes the paragraphs under it off the bottom of a screen the reader has no reason to think
 * is still scrolling. The cap is a convention rather than a compiler error, which is exactly
 * the kind of rule that needs a test rather than a comment.
 */
MESH_TEST_CASE(help_notes_fit_the_panel, unit) {
    for (int i = 0; i < (int)MESH_STR_COUNT; ++i) {
        const enum mesh_str_id id = (enum mesh_str_id)i;
        const char *name = mesh_str_id_name(id);
        if (name == NULL ||
            (strncmp(name, "SETTINGS_NOTE_", 14) != 0 && strncmp(name, "HELP_NOTE_", 10) != 0)) {
            continue;
        }
        const size_t len = strlen(mesh_str_in(mesh_i18n_locale_english(), id));
        if (len > HELP_NOTE_MAX) {
            char reason[160];
            snprintf(reason, sizeof reason, "%s is %u characters, over the %u cap", name,
                     (unsigned)len, (unsigned)HELP_NOTE_MAX);
            record_failure(test_name, reason);
            return;
        }
    }
    record_success(test_name);
}

/*
 * A field's note is optional, and the ones that have one are reachable through the accessor
 * rather than only through the table.
 *
 * The listed fields are the rows docs/help.md names as worth explaining - the ones where the
 * label does not tell you what goes wrong. They are checked by name rather than by counting
 * how many rows have a note, because a count is a number that goes stale on the first row
 * anybody adds and says nothing about *which* row went missing.
 */
MESH_TEST_CASE(help_field_notes_are_optional, unit) {
    static const struct {
        enum mesh_ui_setting_field field;
        const char *name;
    } k_explained[] = {
        {MESH_UI_FIELD_LORA_REGION, "LoRa region"},
        {MESH_UI_FIELD_LORA_SPREAD, "spread factor"},
        {MESH_UI_FIELD_LORA_CODING, "coding rate"},
        {MESH_UI_FIELD_LORA_HOPS, "hop limit"},
        {MESH_UI_FIELD_LORA_TX_POWER, "transmit power"},
        {MESH_UI_FIELD_LORA_BANDWIDTH, "bandwidth"},
        {MESH_UI_FIELD_DEVICE_ROLE, "device role"},
        {MESH_UI_FIELD_DEVICE_REBROADCAST, "rebroadcast mode"},
        {MESH_UI_FIELD_POSITION_SMART, "smart broadcast"},
        {MESH_UI_FIELD_POSITION_SMART_DISTANCE, "smart distance"},
        {MESH_UI_FIELD_POSITION_SMART_INTERVAL, "smart interval"},
        {MESH_UI_FIELD_CHANNEL_KEY, "the channel key"},
        {MESH_UI_FIELD_CHANNEL_UPLINK, "MQTT uplink"},
        {MESH_UI_FIELD_CHANNEL_DOWNLINK, "MQTT downlink"},
        {MESH_UI_FIELD_CHANNEL_POSITION, "position precision"},
        {MESH_UI_FIELD_SECURITY_ADMIN_KEY_0, "the admin keys"},
        {MESH_UI_FIELD_SF_HISTORY_WINDOW, "the history window"},
        {MESH_UI_FIELD_NEIGHBOR_INTERVAL, "the neighbor info interval"},
    };
    for (size_t i = 0; i < sizeof k_explained / sizeof k_explained[0]; ++i) {
        if (mesh_ui_settings_field_note(k_explained[i].field) == MESH_STR_NONE) {
            char reason[128];
            snprintf(reason, sizeof reason, "%s has no note", k_explained[i].name);
            record_failure(test_name, reason);
            return;
        }
    }
    MESH_TEST_FAIL_IF(mesh_ui_settings_field_note(MESH_UI_FIELD_USER_LONG_NAME) != MESH_STR_NONE,
                      "a self-explanatory field acquired a note");
    MESH_TEST_FAIL_IF(mesh_ui_settings_field_note(MESH_UI_FIELD_NONE) != MESH_STR_NONE,
                      "the no-field row has a note");
    MESH_TEST_FAIL_IF(mesh_ui_settings_field_note(
                          (enum mesh_ui_setting_field)MESH_UI_FIELD_COUNT) != MESH_STR_NONE,
                      "a field past the end has a note");
    record_success(test_name);
}

/* ---- the screen ---------------------------------------------------------------------------- */

/* A topic is the section's overview followed by the rows that have something to add, and it is
   ids the whole way down - which is what lets this read one with no locale in force. */
MESH_TEST_CASE(help_topic_leads_with_the_section_overview, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_open(&store, MESH_UI_SETTINGS_LORA), "LoRa did not open");

    struct mesh_ui_help_topic topic;
    MESH_TEST_FAIL_IF(!topic_for(&store, &topic), "LoRa has no topic");
    MESH_TEST_FAIL_IF(topic.count < 2U, "LoRa's topic has no field notes in it");
    MESH_TEST_FAIL_IF(topic.entries[0].label != MESH_STR_NONE,
                      "the opening paragraph is attributed to a row");
    MESH_TEST_FAIL_IF(topic.entries[0].body != mesh_ui_settings_section_note(MESH_UI_SETTINGS_LORA),
                      "the opening paragraph is not the section's note");
    for (uint32_t i = 1; i < topic.count; ++i) {
        MESH_TEST_FAIL_IF(topic.entries[i].label == MESH_STR_NONE, "a field note names no row");
        MESH_TEST_FAIL_IF(topic.entries[i].body == MESH_STR_NONE, "an entry has no paragraph");
    }
    record_success(test_name);
}

/*
 * Help opens on the paragraph that explains the row the question was asked about.
 *
 * The whole reason the screen is per section rather than per row is that a per-row key would do
 * nothing on most rows; the whole reason that is not a downgrade is this. Checked for every row
 * of the section rather than for one, because the arithmetic is a running count of the notes
 * above a row and an off-by-one in it would be right at one end and wrong at the other.
 *
 * The expected answer is recomputed here from the section's rows rather than taken from the
 * model, so this is a second opinion rather than an echo.
 */
MESH_TEST_CASE(help_opens_where_the_cursor_was, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_open(&store, MESH_UI_SETTINGS_LORA), "LoRa did not open");

    struct mesh_ui_help_topic topic;
    MESH_TEST_FAIL_IF(!topic_for(&store, &topic), "LoRa has no topic");
    MESH_TEST_FAIL_IF(topic.count < 2U, "LoRa's topic has no field notes in it");

    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t rows =
        mesh_ui_settings_items(&store.settings, &store.handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, items, MESH_UI_SETTINGS_ITEMS_MAX);
    MESH_TEST_FAIL_IF(rows == 0U, "LoRa has no rows");

    bool saw_a_field_note = false;
    uint32_t expected = 0U; /* the entry the row before this one resolves to */
    for (uint32_t row = 0; row < rows; ++row) {
        const enum mesh_str_id note = items[row].field != MESH_UI_FIELD_NONE
                                          ? mesh_ui_settings_field_note(items[row].field)
                                          : MESH_STR_NONE;
        if (note != MESH_STR_NONE) {
            expected++;
            saw_a_field_note = true;
        }

        /* Put the cursor on `row` from a known position, then ask. */
        while (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] > 0U) {
            press(&store, MESH_UI_KEY_UP);
        }
        for (uint32_t i = 0; i < row; ++i) {
            press(&store, MESH_UI_KEY_DOWN);
        }
        press(&store, MESH_UI_KEY_SELECT);
        MESH_TEST_FAIL_IF(!store.nav.help_open, "SELECT did not open help");

        if (store.nav.help_cursor != expected) {
            char reason[160];
            snprintf(reason, sizeof reason, "row %u opened paragraph %u, expected %u",
                     (unsigned)row, (unsigned)store.nav.help_cursor, (unsigned)expected);
            record_failure(test_name, reason);
            return;
        }
        /* A row that has a note lands on its *own* paragraph, not merely on some paragraph. */
        if (note != MESH_STR_NONE && topic.entries[store.nav.help_cursor].body != note) {
            record_failure(test_name, "a row did not open its own paragraph");
            return;
        }
        press(&store, MESH_UI_KEY_B);
    }
    MESH_TEST_FAIL_IF(!saw_a_field_note, "LoRa had no explained row, so nothing was proved");
    record_success(test_name);
}

MESH_TEST_CASE(help_closes_onto_the_row_it_was_opened_from, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_open(&store, MESH_UI_SETTINGS_LORA), "LoRa did not open");
    press(&store, MESH_UI_KEY_DOWN);
    press(&store, MESH_UI_KEY_DOWN);
    const uint32_t was = store.nav.cursor[MESH_UI_SCREEN_SETTINGS];

    press(&store, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(!store.nav.help_open, "SELECT did not open help");
    press(&store, MESH_UI_KEY_B);
    MESH_TEST_FAIL_IF(store.nav.help_open, "B did not leave help");
    MESH_TEST_FAIL_IF(store.nav.settings_section != (uint8_t)MESH_UI_SETTINGS_LORA,
                      "B left the section as well as the help screen");
    MESH_TEST_FAIL_IF(store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != was,
                      "the section lost its place while help was up");

    /* SELECT is a toggle: the key that opens the screen is a key the user still has under their
       thumb when they want it gone. */
    press(&store, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(!store.nav.help_open, "SELECT did not re-open help");
    press(&store, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(store.nav.help_open, "SELECT did not close help");
    record_success(test_name);
}

/* The cursor scrolls between paragraphs and stops at both ends rather than wrapping - it is a
   reading position, and a paragraph list that wrapped would look like it had reloaded. */
MESH_TEST_CASE(help_cursor_stops_at_both_ends, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_open(&store, MESH_UI_SETTINGS_LORA), "LoRa did not open");
    press(&store, MESH_UI_KEY_SELECT);

    struct mesh_ui_help_topic topic;
    MESH_TEST_FAIL_IF(!topic_for(&store, &topic), "LoRa has no topic");
    MESH_TEST_FAIL_IF(topic.count < 2U, "not enough paragraphs to scroll");

    press(&store, MESH_UI_KEY_UP);
    MESH_TEST_FAIL_IF(store.nav.help_cursor != 0U, "Up wrapped off the top");
    for (uint32_t i = 0; i < topic.count + 3U; ++i) {
        press(&store, MESH_UI_KEY_DOWN);
    }
    MESH_TEST_FAIL_IF(store.nav.help_cursor != topic.count - 1U,
                      "Down ran past the last paragraph");
    record_success(test_name);
}

/* Help is a level, so the slide, the back arrow and the B keycap all follow without being told.
   The case route.h argues for: nothing records that a press went in. */
MESH_TEST_CASE(help_is_a_route_level, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_open(&store, MESH_UI_SETTINGS_LORA), "LoRa did not open");

    struct mesh_ui_route before;
    mesh_ui_route_of(&store.nav, &before);
    press(&store, MESH_UI_KEY_SELECT);
    struct mesh_ui_route after;
    mesh_ui_route_of(&store.nav, &after);

    MESH_TEST_FAIL_IF(after.level != MESH_UI_ROUTE_HELP, "help is not its own level");
    MESH_TEST_FAIL_IF(after.depth != before.depth + 1U, "help did not go a level deeper");
    MESH_TEST_FAIL_IF(mesh_ui_route_move(&before, &after) != MESH_UI_TRANSITION_FORWARD,
                      "opening help did not read as a move inwards");
    MESH_TEST_FAIL_IF(mesh_ui_route_move(&after, &before) != MESH_UI_TRANSITION_BACK,
                      "leaving help did not read as a move outwards");
    record_success(test_name);
}

/* ---- the agreement ------------------------------------------------------------------------- */

/*
 * The keycap and the press say the same thing, in every state a section can be in.
 *
 * This is the case the whole design is arranged around, and the first version of it checked only
 * pristine sections - which is exactly the hole two real bugs went through. `actions_settings()`
 * returns early for a section with pending edits and for one with the discard question armed,
 * and neither early return named the help press; the handler, meanwhile, was unconditional. So
 * SELECT opened help from the first keystroke of an edit with no keycap saying it could, and
 * with the question armed it stood the question down *and* opened a screen off one press.
 *
 * Both failures are invisible in a screenshot and neither is a crash. What catches them is
 * walking the states rather than the screens: a section is pristine, then edited, then armed,
 * and the bar and the press are compared at each.
 */
MESH_TEST_CASE(help_keycap_and_press_agree, unit) {
    static const enum mesh_ui_settings_section k_sections[] = {
        MESH_UI_SETTINGS_LORA,   MESH_UI_SETTINGS_ABOUT,  MESH_UI_SETTINGS_MODULES,
        MESH_UI_SETTINGS_DEVICE, MESH_UI_SETTINGS_CANNED, MESH_UI_SETTINGS_ACTIONS,
    };
    /* Pristine, one edit in hand, and the discard question armed over that edit. */
    static const char *const k_states[] = {"pristine", "edited", "discard armed"};

    for (size_t i = 0; i < sizeof k_sections / sizeof k_sections[0]; ++i) {
        for (int state = 0; state < 3; ++state) {
            struct mesh_ui_store store;
            MESH_TEST_FAIL_IF(!help_store_open(&store, k_sections[i]), "a section did not open");

            if (state > 0) {
                /* Step a value to make an edit. Not every section has an editable row - About
                   and Radio actions have none - so a state that could not be reached is skipped
                   rather than asserted into existence. */
                for (uint32_t row = 0;
                     row < mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) &&
                     store.nav.settings_edit_count == 0U;
                     ++row) {
                    press(&store, MESH_UI_KEY_RIGHT);
                    if (store.nav.settings_edit_count == 0U) {
                        press(&store, MESH_UI_KEY_DOWN);
                    }
                }
                if (store.nav.settings_edit_count == 0U) {
                    continue;
                }
            }
            if (state == 2) {
                press(&store, MESH_UI_KEY_B); /* arms the discard question */
                if (!store.nav.settings_discard_armed) {
                    continue;
                }
            }

            const bool offered = bar_offers_help(&store);
            press(&store, MESH_UI_KEY_SELECT);
            if (offered != store.nav.help_open) {
                char reason[192];
                snprintf(reason, sizeof reason, "%s (%s): the bar says %s and the press says %s",
                         mesh_ui_settings_section_name(k_sections[i]), k_states[state],
                         offered ? "help" : "nothing", store.nav.help_open ? "help" : "nothing");
                record_failure(test_name, reason);
                return;
            }
        }
    }
    record_success(test_name);
}

/*
 * A press that would answer a question the screen is holding open does not also open a screen.
 *
 * SELECT with the discard question armed stands the question down, exactly as any other press
 * that is not B does, and stops there. It opened help as well before the two callers shared a
 * predicate - one press doing two things, one of which the user could not see coming.
 */
MESH_TEST_CASE(help_stands_down_an_armed_question_without_opening, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_open(&store, MESH_UI_SETTINGS_LORA), "LoRa did not open");

    press(&store, MESH_UI_KEY_RIGHT); /* an edit on the first row */
    MESH_TEST_FAIL_IF(store.nav.settings_edit_count == 0U, "the row did not take an edit");
    /* With the edit in hand and nothing armed, help is offered and works. */
    MESH_TEST_FAIL_IF(!bar_offers_help(&store), "an edited section stopped offering help");

    press(&store, MESH_UI_KEY_B);
    MESH_TEST_FAIL_IF(!store.nav.settings_discard_armed, "B did not arm the discard question");
    MESH_TEST_FAIL_IF(bar_offers_help(&store), "an armed question still offered the help press");

    press(&store, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(store.nav.help_open, "SELECT opened help over an armed question");
    MESH_TEST_FAIL_IF(store.nav.settings_discard_armed,
                      "SELECT did not stand the question down the way any other press does");
    MESH_TEST_FAIL_IF(store.nav.settings_edit_count == 0U,
                      "standing the question down discarded the edit it was asking about");

    /* And now that the question is gone, the same key opens help. */
    MESH_TEST_FAIL_IF(!bar_offers_help(&store), "help was not offered once the question was down");
    press(&store, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(!store.nav.help_open, "SELECT did not open help after the question went");
    record_success(test_name);
}

/* ---- the features -------------------------------------------------------------------------- */

/*
 * A store on `screen`, with the fixture's devices, nodes and messages behind it.
 *
 * The settings fixture above opens a section; this one stops at the tab, which is what a feature
 * topic is keyed on.
 */
static bool help_store_tab(struct mesh_ui_store *store, enum mesh_ui_screen screen) {
    if (mesh_ui_store_init(store) != 0) {
        return false;
    }
    mesh_test_nav_populate(store);
    return mesh_test_open_tab(store, screen);
}

/*
 * Every tab explains itself, and says which tab it is explaining.
 *
 * The phase 1 client offered SELECT on a settings section and nowhere else, which made the one
 * key on the case that is not printed with a verb mean something on one tab in six. The subject
 * is checked with the topic because it is what the frame draws on the trail: a topic with
 * paragraphs and no subject is a help screen headed "Help" over nothing, which looks like a
 * missing string rather than like an answer.
 */
MESH_TEST_CASE(help_every_tab_explains_itself, unit) {
    static const enum mesh_ui_screen k_screens[] = {
        MESH_UI_SCREEN_MESSAGES, MESH_UI_SCREEN_NODES,  MESH_UI_SCREEN_WAYPOINTS,
        MESH_UI_SCREEN_DEVICES,  MESH_UI_SCREEN_STATUS,
    };
    for (size_t i = 0; i < sizeof k_screens / sizeof k_screens[0]; ++i) {
        struct mesh_ui_store store;
        MESH_TEST_FAIL_IF(!help_store_tab(&store, k_screens[i]), "a tab did not open");
        MESH_TEST_FAIL_IF(!bar_offers_help(&store), "a tab did not offer the help press");

        struct mesh_ui_help_topic topic;
        MESH_TEST_FAIL_IF(!topic_for(&store, &topic), "a tab has no topic");
        MESH_TEST_FAIL_IF(topic.count < 2U, "a tab's topic is only an overview");
        MESH_TEST_FAIL_IF(topic.subject == MESH_STR_NONE, "a topic does not name its screen");
        MESH_TEST_FAIL_IF(topic.entries[0].label != MESH_STR_NONE,
                          "the opening paragraph is attributed to a row");
        for (uint32_t e = 1; e < topic.count; ++e) {
            MESH_TEST_FAIL_IF(topic.entries[e].label == MESH_STR_NONE,
                              "a paragraph has no heading");
            MESH_TEST_FAIL_IF(topic.entries[e].body == MESH_STR_NONE, "a heading has no paragraph");
        }

        press(&store, MESH_UI_KEY_SELECT);
        MESH_TEST_FAIL_IF(!store.nav.help_open, "SELECT did not open a tab's help");
        /* A feature opens at the top: its paragraphs are about the screen, not about the row
           the cursor happened to be on. */
        MESH_TEST_FAIL_IF(store.nav.help_cursor != 0U, "a feature topic did not open at the top");
        press(&store, MESH_UI_KEY_B);
        MESH_TEST_FAIL_IF(store.nav.help_open, "B did not leave a tab's help");
        MESH_TEST_FAIL_IF(store.nav.screen != k_screens[i], "leaving help left the tab as well");
    }
    record_success(test_name);
}

/*
 * Opening a level changes the topic, without help being told that it has.
 *
 * This is the whole of what keying on the route buys, so it is checked rather than described: a
 * conversation is not the conversation list and a node is not the roster, and neither of the two
 * inner screens has a flag anywhere saying which paragraphs belong to it.
 */
MESH_TEST_CASE(help_follows_the_route_into_a_level, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_tab(&store, MESH_UI_SCREEN_MESSAGES), "Messages did not open");

    struct mesh_ui_help_topic list;
    MESH_TEST_FAIL_IF(!topic_for(&store, &list), "the conversation list has no topic");

    press(&store, MESH_UI_KEY_A); /* the all-traffic row, which is the first one */
    MESH_TEST_FAIL_IF(!store.nav.thread_open, "A did not open a transcript");

    struct mesh_ui_help_topic thread;
    MESH_TEST_FAIL_IF(!topic_for(&store, &thread), "the transcript has no topic");
    MESH_TEST_FAIL_IF(thread.subject == list.subject,
                      "the transcript is explained as the list it was opened from");
    MESH_TEST_FAIL_IF(!bar_offers_help(&store), "the transcript did not offer the help press");

    /* And the same one level in on another tab, so this is the route rather than one screen. */
    struct mesh_ui_store nodes;
    MESH_TEST_FAIL_IF(!help_store_tab(&nodes, MESH_UI_SCREEN_NODES), "Nodes did not open");
    struct mesh_ui_help_topic roster;
    MESH_TEST_FAIL_IF(!topic_for(&nodes, &roster), "the roster has no topic");
    press(&nodes, MESH_UI_KEY_A);
    MESH_TEST_FAIL_IF(!nodes.nav.node_detail_open, "A did not open a node");
    struct mesh_ui_help_topic detail;
    MESH_TEST_FAIL_IF(!topic_for(&nodes, &detail), "a node detail has no topic");
    MESH_TEST_FAIL_IF(detail.subject == roster.subject,
                      "a node is explained as the roster it was opened from");
    record_success(test_name);
}

/*
 * The tapback picker is explained, which is the case the press had to move for.
 *
 * SELECT was handled after the overlay dispatch, so every overlay swallowed it and the one
 * screen in the client made entirely of glyphs was the one screen that could not say what its
 * glyphs did. The bar and the press are compared here as everywhere else - the picker acquired
 * a keycap by acquiring a paragraph, and if it ever loses the paragraph it must lose the keycap.
 */
MESH_TEST_CASE(help_explains_the_tapback_picker, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_tab(&store, MESH_UI_SCREEN_MESSAGES), "Messages did not open");
    press(&store, MESH_UI_KEY_DOWN); /* off all-traffic, onto a conversation */
    press(&store, MESH_UI_KEY_A);
    MESH_TEST_FAIL_IF(!store.nav.thread_open || store.nav.inbox, "no conversation opened");
    press(&store, MESH_UI_KEY_X);
    MESH_TEST_FAIL_IF(!store.nav.reaction_open, "X did not raise the tapback picker");

    MESH_TEST_FAIL_IF(!bar_offers_help(&store), "the tapback picker offered no help press");
    struct mesh_ui_help_topic topic;
    MESH_TEST_FAIL_IF(!topic_for(&store, &topic), "the tapback picker has no topic");
    press(&store, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(!store.nav.help_open, "SELECT did not explain the tapback picker");
    /* And B goes back to the picker rather than out of it: help is a level over the overlay,
       not a screen that replaced it. */
    press(&store, MESH_UI_KEY_B);
    MESH_TEST_FAIL_IF(store.nav.help_open, "B did not leave help");
    MESH_TEST_FAIL_IF(!store.nav.reaction_open, "leaving help closed the picker underneath it");
    record_success(test_name);
}

/*
 * A screen holding a destructive question open does not offer help, on any tab.
 *
 * The settings section had this rule from the first version, for a reason that was never about
 * settings: an armed question has spent both keycaps on a yes and a no, and a third press that
 * opened a screen would stand the question down where the user could not see it happen. Moving
 * SELECT ahead of the overlay dispatch made the other four arming flags reachable by the same
 * press, so all of them are walked rather than the one that was found first.
 */
MESH_TEST_CASE(help_is_refused_while_a_question_is_armed, unit) {
    /* The Devices tab's forget (Y), and the conversation list's delete (X). Both are one press
       to arm and any other press to stand down. */
    struct mesh_ui_store devices;
    MESH_TEST_FAIL_IF(!help_store_tab(&devices, MESH_UI_SCREEN_DEVICES), "Devices did not open");
    MESH_TEST_FAIL_IF(!bar_offers_help(&devices), "Devices did not offer help to begin with");
    press(&devices, MESH_UI_KEY_Y);
    MESH_TEST_FAIL_IF(!devices.nav.devices_forget_armed, "Y did not arm the forget question");
    MESH_TEST_FAIL_IF(bar_offers_help(&devices), "an armed forget still offered the help press");
    press(&devices, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(devices.nav.help_open, "SELECT opened help over an armed forget");
    MESH_TEST_FAIL_IF(devices.nav.devices_forget_armed,
                      "SELECT did not stand the question down the way any other press does");
    MESH_TEST_FAIL_IF(!bar_offers_help(&devices), "help stayed away once the question was gone");

    struct mesh_ui_store messages;
    MESH_TEST_FAIL_IF(!help_store_tab(&messages, MESH_UI_SCREEN_MESSAGES), "Messages did not open");
    press(&messages, MESH_UI_KEY_DOWN);
    press(&messages, MESH_UI_KEY_X);
    MESH_TEST_FAIL_IF(!messages.nav.messages_delete_armed, "X did not arm the delete question");
    MESH_TEST_FAIL_IF(bar_offers_help(&messages), "an armed delete still offered the help press");
    press(&messages, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(messages.nav.help_open, "SELECT opened help over an armed delete");
    MESH_TEST_FAIL_IF(messages.nav.messages_delete_armed, "SELECT left the question armed");
    record_success(test_name);
}

/*
 * An overlay over a settings section is not the settings section.
 *
 * The regression the hoisted press made possible, and the one the feature table gets right for
 * free by not listing those routes. help_section_open() used to ask only which section the nav
 * had open, which stays true while a keyboard is up over one of its rows - so with SELECT moved
 * ahead of the overlay dispatch, the press opened the section's help over a half-typed field
 * while the keyboard's own bar, which names five keys and not that one, said nothing about it.
 *
 * Walked as states rather than asserted once, for help_keycap_and_press_agree's reason: the bar
 * and the press have to agree in each of them, and it is the agreement rather than either
 * answer that is the property worth checking.
 */
MESH_TEST_CASE(help_is_not_offered_over_an_overlay_on_a_section, unit) {
    /* A text field's keyboard, over the User section. */
    struct mesh_ui_store keyboard;
    MESH_TEST_FAIL_IF(!help_store_open(&keyboard, MESH_UI_SETTINGS_USER), "User did not open");
    MESH_TEST_FAIL_IF(!bar_offers_help(&keyboard), "the section did not offer help to begin with");
    press(&keyboard, MESH_UI_KEY_A); /* the long name row opens the keyboard */
    MESH_TEST_FAIL_IF(!keyboard.nav.keyboard_open, "A did not raise the keyboard");
    MESH_TEST_FAIL_IF(bar_offers_help(&keyboard), "the keyboard offered the help press");
    press(&keyboard, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(keyboard.nav.help_open, "SELECT opened help over the keyboard");
    MESH_TEST_FAIL_IF(!keyboard.nav.keyboard_open, "SELECT closed the keyboard instead");

    /* And the confirm dialog, over Radio actions. Same shape, different overlay: the question is
       waiting for A or B, and a third press that drew a screen over it would be answering
       something nobody asked. */
    struct mesh_ui_store confirm;
    MESH_TEST_FAIL_IF(!help_store_open(&confirm, MESH_UI_SETTINGS_ACTIONS),
                      "Radio actions did not open");
    MESH_TEST_FAIL_IF(!bar_offers_help(&confirm), "the section did not offer help to begin with");
    /* Walked rather than aimed at row 0: which rows are actions depends on what the radio has
       told us, and a test that pressed a fixed row would be asserting the section's order. */
    for (uint32_t row = 0;
         row < mesh_ui_nav_row_count(&confirm.nav, &confirm, MESH_UI_SCREEN_SETTINGS) &&
         !confirm.nav.confirm_open;
         ++row) {
        press(&confirm, MESH_UI_KEY_A);
        if (!confirm.nav.confirm_open) {
            press(&confirm, MESH_UI_KEY_DOWN);
        }
    }
    MESH_TEST_FAIL_IF(!confirm.nav.confirm_open, "no row raised the confirm dialog");
    MESH_TEST_FAIL_IF(bar_offers_help(&confirm), "the confirm dialog offered the help press");
    press(&confirm, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(confirm.nav.help_open, "SELECT opened help over the confirm dialog");
    MESH_TEST_FAIL_IF(!confirm.nav.confirm_open, "SELECT answered the question instead");
    record_success(test_name);
}

/*
 * And where there is nothing to explain, neither the bar nor the press invents anything.
 *
 * Two states rather than the six tabs this used to walk, because five of the six now have
 * something to say. What is left is the pair the design refuses on purpose: the Settings tab's
 * own list of sections, where each row explains itself once opened so a topic over the list
 * would be a table of contents for a table of contents; and an overlay that is asking the user a
 * question, where the way out is to answer it rather than to stack a second screen on it.
 */
MESH_TEST_CASE(help_is_not_offered_where_there_is_nothing_to_say, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!help_store_tab(&store, MESH_UI_SCREEN_SETTINGS), "Settings did not open");
    MESH_TEST_FAIL_IF(bar_offers_help(&store), "the list of sections offered help");
    press(&store, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(store.nav.help_open, "SELECT opened help on the list of sections");

    /* The send-to picker, raised by Y on the conversation list: a question with a list of
       answers, and the screen underneath it is the one with the explanation. */
    struct mesh_ui_store picker;
    MESH_TEST_FAIL_IF(!help_store_tab(&picker, MESH_UI_SCREEN_MESSAGES), "Messages did not open");
    press(&picker, MESH_UI_KEY_Y);
    MESH_TEST_FAIL_IF(!picker.nav.picker_open, "Y did not raise the picker");
    MESH_TEST_FAIL_IF(bar_offers_help(&picker), "the picker offered help");
    press(&picker, MESH_UI_KEY_SELECT);
    MESH_TEST_FAIL_IF(picker.nav.help_open, "SELECT opened help over the picker");
    MESH_TEST_FAIL_IF(!picker.nav.picker_open, "SELECT closed the picker instead");
    record_success(test_name);
}
