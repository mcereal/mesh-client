#define _POSIX_C_SOURCE 200809L

/*
 * The string catalog, and the promises a translation has to keep.
 *
 * Three kinds of case live here.
 *
 * The first are about the catalog itself: it is complete, the ids and the text line up, and
 * nothing has an empty entry it should not have. These catch the one failure mode an X-macro
 * table has - the enum and the table drifting apart - which the compiler cannot see because
 * both are generated from the same list only as long as nobody hand-edits one of them.
 *
 * The second are about a *translation*, built here rather than shipped, to exercise fallback
 * independently of the shipped languages: a partial table falls back to English entry by entry, and
 * a table whose %-specifiers no longer match the English is rejected by mesh_i18n_validate() rather
 * than reaching a vsnprintf that would read the wrong argument off the stack.
 *
 * The third are about the renderers: with every string coming from one place, the way to catch
 * a screen that kept an English word of its own is to draw it in another language and check
 * the frame changed. That is the same argument ui_theme.c makes for colours.
 */

#include "framework/mesh_test.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/font5x7.h"
#include "mesh/ui/nav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the catalog ---------------------------------------------------------------------- */

MESH_TEST_CASE(i18n_catalog_is_complete, unit) {
    MESH_TEST_FAIL_IF(MESH_STR_COUNT <= 0, "the catalog is empty");

    /* Id 0 is the deliberate empty entry; every other id has text and a name. */
    MESH_TEST_FAIL_IF(mesh_str_in(NULL, MESH_STR_NONE)[0] != '\0',
                      "MESH_STR_NONE is not the empty string");
    for (int id = 0; id < (int)MESH_STR_COUNT; ++id) {
        const enum mesh_str_id which = (enum mesh_str_id)id;
        const char *text = mesh_str_in(mesh_i18n_locale_english(), which);
        const char *name = mesh_str_id_name(which);
        MESH_TEST_FAIL_IF(text == NULL, "an id has no English text");
        MESH_TEST_FAIL_IF(name == NULL || name[0] == '\0', "an id has no name");
        if (which != MESH_STR_NONE && text[0] == '\0') {
            char reason[128];
            snprintf(reason, sizeof reason, "%s is empty and is not MESH_STR_NONE", name);
            record_failure(test_name, reason);
            return;
        }
    }

    /* Out of range in either direction is the empty string, never a read past the table. */
    MESH_TEST_FAIL_IF(mesh_str((enum mesh_str_id)MESH_STR_COUNT)[0] != '\0',
                      "an id past the end returned text");
    MESH_TEST_FAIL_IF(mesh_str_id_name((enum mesh_str_id)MESH_STR_COUNT) != NULL,
                      "an id past the end has a name");
    record_success(test_name);
}

/* Every locale in the build is fit to ship. This is the case that fails when a translation
   changes a "%u" to a "%s"; with one language it holds English to its own contract. */
MESH_TEST_CASE(i18n_shipped_locales_validate, unit) {
    MESH_TEST_FAIL_IF(mesh_i18n_locale_count() == 0U, "no locales are registered");
    MESH_TEST_FAIL_IF(mesh_i18n_locale_at(0U) != mesh_i18n_locale_english(),
                      "English is not the first locale");
    MESH_TEST_FAIL_IF(mesh_i18n_locale_english()->table != NULL,
                      "English has a table of its own instead of being the fallback");

    for (size_t i = 0; i < mesh_i18n_locale_count(); ++i) {
        const struct mesh_i18n_locale *locale = mesh_i18n_locale_at(i);
        MESH_TEST_FAIL_IF(locale == NULL, "a registered locale is NULL");
        MESH_TEST_FAIL_IF(mesh_i18n_locale_by_id(locale->id) != locale,
                          "a locale does not come back under its own id");
        for (size_t j = 0; j < i; ++j) {
            MESH_TEST_FAIL_IF(strcmp(mesh_i18n_locale_at(j)->id, locale->id) == 0,
                              "two locales share an id");
        }
        char reason[192];
        if (!mesh_i18n_validate(locale, reason, sizeof reason)) {
            record_failure(test_name, reason);
            return;
        }
    }

    MESH_TEST_FAIL_IF(mesh_i18n_locale_at(mesh_i18n_locale_count()) != NULL,
                      "an index past the end returned a locale");
    MESH_TEST_FAIL_IF(mesh_i18n_locale_by_id("no-such-language") != NULL,
                      "an unknown id returned a locale");
    MESH_TEST_FAIL_IF(mesh_i18n_locale_by_id(NULL) != NULL, "a NULL id returned a locale");
    record_success(test_name);
}

/* ---- a translation ---------------------------------------------------------------------- */

/*
 * A locale table built here, translating three entries and leaving the rest NULL. That is the
 * state every translation is in for most of its life, and the case is that it *ships* in that
 * state: a NULL entry reads as English rather than as a blank row.
 */
static const char *k_partial[MESH_STR_COUNT];

static uint8_t plural_always_other(uint32_t n) {
    (void)n;
    return 1U;
}

MESH_TEST_CASE(i18n_partial_translation_falls_back, unit) {
    memset((void *)k_partial, 0, sizeof k_partial);
    k_partial[MESH_STR_TAB_NODES] = "Knoten";
    k_partial[MESH_STR_TAB_STATUS] = "Status";

    const struct mesh_i18n_locale locale = {
        .id = "test",
        .name = "Test",
        .table = k_partial,
        .plural_form = NULL,
    };

    MESH_TEST_FAIL_IF(strcmp(mesh_str_in(&locale, MESH_STR_TAB_NODES), "Knoten") != 0,
                      "a translated entry did not come back translated");
    MESH_TEST_FAIL_IF(strcmp(mesh_str_in(&locale, MESH_STR_TAB_MESSAGES),
                             mesh_str_in(mesh_i18n_locale_english(), MESH_STR_TAB_MESSAGES)) != 0,
                      "an untranslated entry did not fall back to English");

    char reason[192];
    MESH_TEST_FAIL_IF(!mesh_i18n_validate(&locale, reason, sizeof reason),
                      "a partial translation with matching formats was rejected");
    record_success(test_name);
}

/* The case mesh_i18n_validate() exists for: a translation that drops or retypes an argument
   is a crash waiting for the row that uses it, not a typo. */
MESH_TEST_CASE(i18n_validate_rejects_bad_formats, unit) {
    struct mesh_i18n_locale locale = {
        .id = "test",
        .name = "Test",
        .table = k_partial,
        .plural_form = NULL,
    };
    char reason[192];

    memset((void *)k_partial, 0, sizeof k_partial);
    /* "%s !%08x" with the hex dropped: one argument short of what the caller passes. */
    k_partial[MESH_STR_STATUS_MY_NODE] = "%s";
    MESH_TEST_FAIL_IF(mesh_i18n_validate(&locale, reason, sizeof reason),
                      "a translation that dropped an argument was accepted");
    MESH_TEST_FAIL_IF(reason[0] == '\0', "the rejection gave no reason");
    MESH_TEST_FAIL_IF(strstr(reason, "STATUS_MY_NODE") == NULL,
                      "the rejection did not name the entry");

    /* Right count, wrong type: %u read as %s is a pointer dereference of an integer. */
    memset((void *)k_partial, 0, sizeof k_partial);
    k_partial[MESH_STR_STATUS_NODEDB_ONLINE] = "%s nodes, %u online";
    MESH_TEST_FAIL_IF(mesh_i18n_validate(&locale, reason, sizeof reason),
                      "a translation that retyped an argument was accepted");

    /* Reordering the *words* is exactly what a translation is for, and is allowed as long as
       the specifiers stay in the order the caller passes them. */
    memset((void *)k_partial, 0, sizeof k_partial);
    k_partial[MESH_STR_STATUS_NODEDB_ONLINE] = "online: %u von %u Knoten";
    MESH_TEST_FAIL_IF(!mesh_i18n_validate(&locale, reason, sizeof reason),
                      "a reworded translation with the same arguments was rejected");

    /* A locale with no id is not a locale. */
    locale.id = "";
    MESH_TEST_FAIL_IF(mesh_i18n_validate(&locale, reason, sizeof reason),
                      "a locale with no id was accepted");
    MESH_TEST_FAIL_IF(mesh_i18n_validate(NULL, reason, sizeof reason), "NULL was accepted");
    record_success(test_name);
}

/* ---- formatting and plurals -------------------------------------------------------------- */

MESH_TEST_CASE(i18n_format_and_plural, unit) {
    char out[96];

    mesh_str_format(out, sizeof out, MESH_STR_STATUS_NODEDB_ONLINE, 12U, 3U);
    MESH_TEST_FAIL_IF(strcmp(out, "12 nodes, 3 online") != 0, "a formatted entry came out wrong");

    /* A truncating buffer still terminates, which is what every toast in the client relies on. */
    char tiny[8];
    mesh_str_format(tiny, sizeof tiny, MESH_STR_STATUS_NODEDB_ONLINE, 12U, 3U);
    MESH_TEST_FAIL_IF(tiny[sizeof tiny - 1U] != '\0', "a truncated format was not terminated");
    MESH_TEST_FAIL_IF(mesh_str_format(NULL, 0U, MESH_STR_TAB_NODES) != 0,
                      "formatting into no buffer did not return 0");

    /* English: one for 1, other for everything else - including 0. */
    MESH_TEST_FAIL_IF(strcmp(mesh_str_plural(MESH_STR_TOAST_FORGOT_NODES_ONE, 1U),
                             "Forgot %d node; pins kept") != 0,
                      "the singular form was not chosen for 1");
    MESH_TEST_FAIL_IF(strcmp(mesh_str_plural(MESH_STR_TOAST_FORGOT_NODES_ONE, 0U),
                             "Forgot %d nodes; pins kept") != 0,
                      "the plural form was not chosen for 0");
    MESH_TEST_FAIL_IF(strcmp(mesh_str_plural(MESH_STR_TOAST_FORGOT_NODES_ONE, 7U),
                             "Forgot %d nodes; pins kept") != 0,
                      "the plural form was not chosen for 7");

    mesh_str_format_plural(out, sizeof out, MESH_STR_TOAST_FORGOT_NODES_ONE, 1U, 1);
    MESH_TEST_FAIL_IF(strcmp(out, "Forgot 1 node; pins kept") != 0,
                      "a formatted singular came out wrong");
    mesh_str_format_plural(out, sizeof out, MESH_STR_TOAST_FORGOT_NODES_ONE, 4U, 4);
    MESH_TEST_FAIL_IF(strcmp(out, "Forgot 4 nodes; pins kept") != 0,
                      "a formatted plural came out wrong");
    record_success(test_name);
}

/* A locale that counts differently gets a different form, which is the whole reason the rule
   is the locale's rather than `n == 1` at the call site. */
MESH_TEST_CASE(i18n_plural_rule_is_the_locales, unit) {
    const struct mesh_i18n_locale english = {
        .id = "en", .name = "English", .table = NULL, .plural_form = NULL};
    const struct mesh_i18n_locale other = {
        .id = "test", .name = "Test", .table = NULL, .plural_form = plural_always_other};

    MESH_TEST_FAIL_IF(english.plural_form != NULL, "the fixture stopped using the default rule");
    MESH_TEST_FAIL_IF(other.plural_form(1U) != 1U, "the fixture's rule is not the one we set");
    /* MESH_STR_PLURAL_FORMS bounds every rule, so a rule that returns more than the entry has
       cannot walk off the end of the table. */
    MESH_TEST_FAIL_IF(MESH_STR_PLURAL_FORMS < 2, "there is no room for a plural");
    record_success(test_name);
}

/*
 * English on demand, which is what keeps a log line readable however the handheld is set.
 *
 * mesh_str_in(english, id) is the idiom for it, and the case that matters is the one where the
 * current locale is *not* English: a diagnostic that changes language with the UI is a
 * diagnostic nobody can search for.
 */
MESH_TEST_CASE(i18n_english_is_reachable_from_any_locale, unit) {
    memset((void *)k_partial, 0, sizeof k_partial);
    k_partial[MESH_STR_ACK_NO_ROUTE] = "keine Route zu diesem Knoten";
    const struct mesh_i18n_locale locale = {
        .id = "test", .name = "Test", .table = k_partial, .plural_form = NULL};

    MESH_TEST_FAIL_IF(
        strcmp(mesh_str_in(&locale, MESH_STR_ACK_NO_ROUTE), "keine Route zu diesem Knoten") != 0,
        "the translation did not come back for the translated locale");
    MESH_TEST_FAIL_IF(strcmp(mesh_str_in(mesh_i18n_locale_english(), MESH_STR_ACK_NO_ROUTE),
                             "no route to that node") != 0,
                      "English was not reachable while another locale had the entry");
    record_success(test_name);
}

/*
 * Every locale names itself, in its own language.
 *
 * This is why the name is a field on the locale and not a catalog entry: a catalog entry is
 * optional by design, and a locale whose name fell back to English would have Settings > About
 * reporting the wrong language while that language was on screen.
 */
MESH_TEST_CASE(i18n_locales_name_themselves, unit) {
    for (size_t i = 0; i < mesh_i18n_locale_count(); ++i) {
        const struct mesh_i18n_locale *locale = mesh_i18n_locale_at(i);
        MESH_TEST_FAIL_IF(locale->name == NULL || locale->name[0] == '\0',
                          "a locale does not name itself");
    }
    MESH_TEST_FAIL_IF(strcmp(mesh_i18n_locale_english()->name, "English") != 0,
                      "English stopped calling itself English");
    record_success(test_name);
}

/* ---- choosing a language ------------------------------------------------------------------ */

MESH_TEST_CASE(i18n_locale_selection, unit) {
    const struct mesh_i18n_locale *before = mesh_i18n_locale();
    MESH_TEST_FAIL_IF(before == NULL, "there is no current locale");

    MESH_TEST_FAIL_IF(!mesh_i18n_set_locale("en"), "English could not be selected");
    MESH_TEST_FAIL_IF(mesh_i18n_locale() != mesh_i18n_locale_english(),
                      "selecting English did not take");
    MESH_TEST_FAIL_IF(mesh_i18n_set_locale("no-such-language"), "an unknown language was accepted");
    MESH_TEST_FAIL_IF(mesh_i18n_locale() != mesh_i18n_locale_english(),
                      "a rejected selection changed the locale anyway");
    MESH_TEST_FAIL_IF(mesh_i18n_set_locale(NULL), "NULL was accepted as a language");

    /*
     * The environment. A territory and an encoding are matched on the language part alone, an
     * unknown language leaves English in force rather than falling through to a lower-priority
     * variable, and "C" is the absence of a language rather than a request for one.
     */
    (void)setenv("MESHCLIENT_LANG", "en_GB.UTF-8", 1);
    mesh_i18n_init();
    MESH_TEST_FAIL_IF(mesh_i18n_locale() != mesh_i18n_locale_english(),
                      "en_GB.UTF-8 did not resolve to English");

    (void)setenv("MESHCLIENT_LANG", "xx", 1);
    mesh_i18n_init();
    MESH_TEST_FAIL_IF(mesh_i18n_locale() != mesh_i18n_locale_english(),
                      "an unknown language did not fall back to English");

    (void)unsetenv("MESHCLIENT_LANG");
    (void)setenv("LANG", "C", 1);
    mesh_i18n_init();
    MESH_TEST_FAIL_IF(mesh_i18n_locale() != mesh_i18n_locale_english(),
                      "LANG=C did not resolve to English");
    (void)unsetenv("LANG");
    mesh_i18n_init();
    record_success(test_name);
}

/* A complete Spanish catalog, including the characters the framebuffer must draw. */
MESH_TEST_CASE(i18n_spanish_catalog, unit) {
    const struct mesh_i18n_locale *spanish = mesh_i18n_locale_by_id("es");
    MESH_TEST_FAIL_IF(spanish == NULL || spanish->table == NULL, "Spanish is not registered");
    for (int id = 0; id < (int)MESH_STR_COUNT; ++id) {
        /*
         * Help notes are the one class of string a locale may leave out, and this is where that
         * policy is enforced rather than merely written down (docs/help.md).
         *
         * They are paragraphs rather than labels: a couple of hundred of them are coming, they
         * are technical prose about radio behaviour, and mesh_str_in() already falls back to
         * English per id - so an untranslated note reads in English while every label around it
         * stays Spanish. Holding them to this bar would mean either blocking a note nobody can
         * translate yet, or filling the table with guesses, and a confidently wrong sentence
         * about transmit power is worse than a visibly English one.
         *
         * Everything else on the screen still has to be translated, which is what this loop is
         * for. The exemption is keyed on the id's own name so it cannot quietly widen: a string
         * gets out of the check by being called SETTINGS_NOTE_*, and nothing else is.
         */
        const char *name = mesh_str_id_name((enum mesh_str_id)id);
        if (name != NULL && strncmp(name, "SETTINGS_NOTE_", 14) == 0) {
            continue;
        }
        if (spanish->table[id] == NULL || (id != MESH_STR_NONE && spanish->table[id][0] == '\0')) {
            /* Named, because "Spanish has an untranslated entry" over eleven hundred ids is a
               bisect rather than a failure message. */
            char reason[160];
            snprintf(reason, sizeof reason, "%s has no Spanish translation",
                     name != NULL ? name : "an id");
            record_failure(test_name, reason);
            return;
        }
    }
    const uint32_t letters[] = {0x00e1, 0x00e9, 0x00ed, 0x00f3, 0x00fa,
                                0x00fc, 0x00f1, 0x00bf, 0x00a1};
    for (size_t i = 0; i < sizeof letters / sizeof letters[0]; ++i) {
        MESH_TEST_FAIL_IF(!mesh_font5x7_has_glyph(letters[i]), "a Spanish glyph is missing");
    }
    const char *before = mesh_i18n_locale()->id;
    mesh_ui_canned_reset();
    (void)mesh_i18n_set_locale("es");
    char one[96], zero[96], many[96];
    mesh_str_format_plural(one, sizeof one, MESH_STR_ACTION_FORGET_COUNT_ONE, 1U, 1U);
    mesh_str_format_plural(zero, sizeof zero, MESH_STR_ACTION_FORGET_COUNT_ONE, 0U, 0U);
    mesh_str_format_plural(many, sizeof many, MESH_STR_ACTION_FORGET_COUNT_ONE, 3U, 3U);
    const bool translated = strcmp(mesh_str(MESH_STR_TAB_SETTINGS), "Ajustes") == 0 &&
                            strcmp(mesh_ui_canned_text(4U), "¿Dónde estás?") == 0 &&
                            strcmp(mesh_str(MESH_STR_DATE_WED), "mié") == 0;
    (void)mesh_i18n_set_locale(before);
    mesh_ui_canned_reset();
    MESH_TEST_FAIL_IF(!translated, "Spanish UI text, dates or quick replies did not switch");
    MESH_TEST_FAIL_IF(strcmp(one, "1 nodo") != 0 || strcmp(zero, "0 nodos") != 0 ||
                          strcmp(many, "3 nodos") != 0,
                      "Spanish plurals are incorrect");
    record_success(test_name);
}

MESH_TEST_CASE(i18n_spanish_preference_and_environment, unit) {
    const char *before = mesh_i18n_locale()->id;
    const char *names[] = {"MESHCLIENT_LANG", "LC_ALL", "LC_MESSAGES", "LANG"};
    char *saved[4] = {0};
    for (size_t i = 0; i < 4U; ++i) {
        const char *value = getenv(names[i]);
        saved[i] = value != NULL ? strdup(value) : NULL;
        unsetenv(names[i]);
    }
    const char *failure = NULL;
    const char *tags[] = {"es", "es_ES.UTF-8", "es_PR.UTF-8", "es-MX", "es@euro"};
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; ++i) {
        setenv("MESHCLIENT_LANG", tags[i], 1);
        mesh_i18n_init_with_preference("en");
        if (strcmp(mesh_i18n_locale()->id, "es") != 0 || !mesh_i18n_is_overridden()) {
            failure = "Spanish environment tags must override the saved language";
        }
    }
    unsetenv("MESHCLIENT_LANG");
    setenv("LANG", "es_ES.UTF-8", 1);
    mesh_i18n_init_with_preference("en");
    if (strcmp(mesh_i18n_locale()->id, "en") != 0 || mesh_i18n_is_overridden()) {
        failure = "saved English must override system Spanish without locking the picker";
    }
    mesh_i18n_init_with_preference("unknown");
    if (strcmp(mesh_i18n_locale()->id, "es") != 0) {
        failure = "unknown saved language must retain the system fallback";
    }
    setenv("LC_MESSAGES", "en_US.UTF-8", 1);
    mesh_i18n_init_with_preference("");
    if (strcmp(mesh_i18n_locale()->id, "en") != 0) {
        failure = "LC_MESSAGES must take precedence over LANG";
    }
    setenv("LC_ALL", "es_PR.UTF-8", 1);
    mesh_i18n_init_with_preference("");
    if (strcmp(mesh_i18n_locale()->id, "es") != 0) {
        failure = "LC_ALL must take precedence over LC_MESSAGES";
    }
    setenv("MESHCLIENT_LANG", "xx", 1);
    mesh_i18n_init_with_preference("es");
    if (strcmp(mesh_i18n_locale()->id, "en") != 0) {
        failure = "an unknown explicit override must fall back to English";
    }
    setenv("MESHCLIENT_LANG", "C", 1);
    mesh_i18n_init_with_preference("es");
    if (strcmp(mesh_i18n_locale()->id, "es") != 0 || mesh_i18n_is_overridden()) {
        failure = "C must allow a saved language";
    }
    for (size_t i = 0; i < 4U; ++i) {
        if (saved[i] != NULL) {
            setenv(names[i], saved[i], 1);
        } else {
            unsetenv(names[i]);
        }
        free(saved[i]);
    }
    (void)mesh_i18n_set_locale(before);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}
