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
        if (name == NULL || strncmp(name, "SETTINGS_NOTE_", 14) != 0) {
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

/* A field's note is optional, and the ones that have one are reachable through the accessor
   rather than only through the table. Phase 1 ships five, all on LoRa. */
MESH_TEST_CASE(help_field_notes_are_optional, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_settings_field_note(MESH_UI_FIELD_LORA_HOPS) == MESH_STR_NONE,
                      "the hop limit has no note");
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

/*
 * And where there is nothing to explain, neither the bar nor the press invents anything.
 *
 * The settings root is the case that matters: its rows are the sections, each of which explains
 * itself once opened, so a help screen over the list would be either empty or a table of
 * contents nobody asked for.
 */
MESH_TEST_CASE(help_is_not_offered_where_there_is_nothing_to_say, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    static const enum mesh_ui_screen k_screens[] = {
        MESH_UI_SCREEN_MESSAGES, MESH_UI_SCREEN_NODES,  MESH_UI_SCREEN_WAYPOINTS,
        MESH_UI_SCREEN_DEVICES,  MESH_UI_SCREEN_STATUS, MESH_UI_SCREEN_SETTINGS,
    };
    for (size_t i = 0; i < sizeof k_screens / sizeof k_screens[0]; ++i) {
        MESH_TEST_FAIL_IF(!mesh_test_open_tab(&store, k_screens[i]), "a tab did not open");
        MESH_TEST_FAIL_IF(bar_offers_help(&store), "a tab's own list offered help");
        press(&store, MESH_UI_KEY_SELECT);
        MESH_TEST_FAIL_IF(store.nav.help_open, "SELECT opened help on a tab's own list");
    }
    record_success(test_name);
}
