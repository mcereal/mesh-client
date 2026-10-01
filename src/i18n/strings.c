/*
 * This client's half of the catalog, handed to inkcell as one table.
 *
 * Every table here comes off two .def files read in order - inkcell's fifteen ids, then this
 * client's eight hundred and something - so the ids, the English text and the id names cannot
 * drift apart: they are three expansions of one list.
 *
 * Both halves in one array rather than a second lookup for the app's range, because an id *is*
 * a table index: indexed straight through keeps inkcell_str() a bounds check and a load. The
 * _Static_assert below is what holds the two .def files together - if this client's catalog and
 * its enum ever disagree about how many ids there are, the build stops here rather than reading
 * off the end of a table at runtime.
 *
 * A locale is a table of the same length with NULL where it has nothing to say, and NULL falls
 * back to English. A half-finished translation therefore ships and reads as a mixture rather
 * than as blanks, which is the state every translation is in for a while. Because the tables
 * are written with designated initializers, a language only has to name the ids it translates -
 * inkcell's included, and it costs nothing to leave them out.
 */

#include "mesh/i18n/strings.h"

#include <stddef.h>

/* ---- the catalog --------------------------------------------------------------------------- */

static const char *const k_english[MESH_STR_COUNT] = {
#define INKCELL_STR_ENTRY(id, text) text,
#define INKCELL_STR_PLURAL_ENTRY(id, one, other) one, other,
#include "inkcell/i18n/catalog.def"
#undef INKCELL_STR_ENTRY
#undef INKCELL_STR_PLURAL_ENTRY
#define MESH_STR_ENTRY(id, text) text,
#define MESH_STR_PLURAL_ENTRY(id, one, other) one, other,
#include "mesh/i18n/catalog.def"
#undef MESH_STR_ENTRY
#undef MESH_STR_PLURAL_ENTRY
};

/* The ids as text, for the translation template and for a test failure that has to name the
   entry it is complaining about. */
static const char *const k_id_names[MESH_STR_COUNT] = {
#define INKCELL_STR_ENTRY(id, text) #id,
#define INKCELL_STR_PLURAL_ENTRY(id, one, other) #id "_ONE", #id "_OTHER",
#include "inkcell/i18n/catalog.def"
#undef INKCELL_STR_ENTRY
#undef INKCELL_STR_PLURAL_ENTRY
#define MESH_STR_ENTRY(id, text) #id,
#define MESH_STR_PLURAL_ENTRY(id, one, other) #id "_ONE", #id "_OTHER",
#include "mesh/i18n/catalog.def"
#undef MESH_STR_ENTRY
#undef MESH_STR_PLURAL_ENTRY
};

/*
 * The two .def files and the enum agree about the length.
 *
 * This is the one way the split goes wrong: the enum in strings.h reads only this client's
 * catalog and offsets it, while the tables above read both, so an id added to inkcell's .def
 * without rebuilding against the new INKCELL_STR_COUNT would leave the array one longer than
 * the enum says. Every lookup would still compile and every one past the seam would be off by
 * one - a whole language shifted by a word, which is exactly the kind of bug that looks like a
 * bad translation.
 */
_Static_assert(sizeof k_english / sizeof k_english[0] == (size_t)MESH_STR_COUNT,
               "the English table and the catalog disagree about how many ids there are");
_Static_assert(sizeof k_id_names / sizeof k_id_names[0] == (size_t)MESH_STR_COUNT,
               "the id-name table and the catalog disagree about how many ids there are");

/* ---- locales ------------------------------------------------------------------------------- */

/*
 * "one for exactly 1, other for everything else", which is English, German, Dutch, the
 * Scandinavian languages and most of the rest of Germanic Europe. French counts 0 as one;
 * Polish, Russian and Arabic need more forms than INKCELL_STR_PLURAL_FORMS has - see
 * docs/i18n.md before adding one of those.
 */
static uint8_t plural_english(uint32_t n) { return (uint8_t)(n == 1U ? 0 : 1); }

/*
 * Every language in the build.
 *
 * English has no table: it *is* the fallback, and giving it one would mean two copies of the
 * same 900 strings to keep in step. A new language is a src/i18n/locale_<id>.c declaring
 * `const char *const mesh_i18n_table_<id>[MESH_STR_COUNT]`, a row here, and the file in
 * CMakeLists.txt - nothing else. docs/i18n.md walks through it.
 */
extern const char *const mesh_i18n_table_es[MESH_STR_COUNT];

static const struct inkcell_i18n_locale k_locales[] = {
    {
        .id = "en",
        .name = "English",
        .table = NULL,
        .plural_form = plural_english,
    },
    {.id = "es", .name = "Español", .table = mesh_i18n_table_es, .plural_form = plural_english},
};

#define MESH_I18N_LOCALE_COUNT (sizeof k_locales / sizeof k_locales[0])

static const struct inkcell_i18n_catalog k_catalog = {
    .count = (size_t)MESH_STR_COUNT,
    .english = k_english,
    .id_names = k_id_names,
    .locales = k_locales,
    .locale_count = MESH_I18N_LOCALE_COUNT,
};

/* ---- desktop wording ----------------------------------------------------------------------- */

/*
 * The ids that read differently in a window, each beside the entry it reads as there.
 *
 * A pair rather than a naming convention looked up at run time, so a variant whose base was
 * renamed is a compile error here and not a sentence that silently stops being swapped. A plural
 * is two rows, one per form, because each form is an id of its own.
 */
#define MESH_DESKTOP(id)                                                                           \
    { MESH_STR_##id, MESH_STR_##id##_DESKTOP }

static const struct {
    inkcell_str_id brick;
    inkcell_str_id desktop;
} k_desktop[] = {
    MESH_DESKTOP(COMMON_PRESS_A),
    MESH_DESKTOP(THREAD_EMPTY_INBOX),
    MESH_DESKTOP(THREAD_EMPTY),
    MESH_DESKTOP(WAYPOINT_ACT_DELETE_ARMED),
    MESH_DESKTOP(NODE_TRACE_TIMEOUT),
    MESH_DESKTOP(NODE_ACT_REMOVE_ARMED),
    MESH_DESKTOP(SETTINGS_EMPTY_SECTION),
    MESH_DESKTOP(LINK_NEEDS_PAIRING),
    MESH_DESKTOP(LINK_DROPPED),
    {MESH_STR_TOAST_REFRESH_EDITS_ONE, MESH_STR_TOAST_REFRESH_EDITS_DESKTOP_ONE},
    {MESH_STR_TOAST_REFRESH_EDITS_OTHER, MESH_STR_TOAST_REFRESH_EDITS_DESKTOP_OTHER},
    MESH_DESKTOP(TOAST_SECTION_NOT_LOADED),
    MESH_DESKTOP(TOAST_SAVE_NO_REPLY),
    MESH_DESKTOP(TOAST_NODEDB_RESET),
    MESH_DESKTOP(CONFIRM_TEXT_RESET_DB),
    MESH_DESKTOP(CONFIRM_TEXT_FORGET_OFF),
    MESH_DESKTOP(CONFIRM_TEXT_FORGET_ALL),
    MESH_DESKTOP(CONFIRM_TEXT_FACTORY_DEV),
    MESH_DESKTOP(CONFIRM_TEXT_BLUETOOTH),
    MESH_DESKTOP(CONFIRM_TEXT_FW_BLE),
    MESH_DESKTOP(SETTINGS_NOTE_DISPLAY),
    MESH_DESKTOP(SETTINGS_NOTE_DEVICE_TZDEF),
    MESH_DESKTOP(SETTINGS_NOTE_DISPLAY_UNITS),
    MESH_DESKTOP(HELP_NOTE_SETTINGS_GROUPS),
    MESH_DESKTOP(HELP_NOTE_MESSAGES_NEW),
    MESH_DESKTOP(HELP_NOTE_MESSAGES_DROP),
    MESH_DESKTOP(HELP_NOTE_MESSAGES_MUTE),
    MESH_DESKTOP(HELP_NOTE_THREAD_JUMP),
    MESH_DESKTOP(HELP_NOTE_THREAD_REPLY),
    MESH_DESKTOP(HELP_NOTE_THREAD_RESEND),
    MESH_DESKTOP(HELP_NOTE_NODES_FILTER),
    MESH_DESKTOP(HELP_NOTE_NODES_SORT),
    MESH_DESKTOP(HELP_NOTE_NODES_PIN),
    MESH_DESKTOP(HELP_NOTE_NODE_GROUPS),
    MESH_DESKTOP(HELP_NOTE_NODE_ACTIONS),
    MESH_DESKTOP(HELP_NOTE_MAP_MOVE),
    MESH_DESKTOP(HELP_NOTE_MAP_PICK),
    MESH_DESKTOP(HELP_NOTE_DEVICES),
    MESH_DESKTOP(HELP_NOTE_DEVICES_FORGET),
    MESH_DESKTOP(HELP_NOTE_DEVICES_NETWORK),
    MESH_DESKTOP(HELP_NOTE_TREND_SPAN),
    MESH_DESKTOP(HELP_NOTE_NODE_CHART_SPAN),
    MESH_DESKTOP(HELP_NOTE_NODE_CHART_READINGS),
};

#undef MESH_DESKTOP

#define MESH_DESKTOP_COUNT (sizeof k_desktop / sizeof k_desktop[0])

/*
 * The catalog a window registers: every table above copied, with each pair's desktop text
 * written over its Brick text. Built once, the first time it is asked for - the tables are
 * static, so it is a few kilobytes of pointers and no allocation.
 *
 * A locale's desktop entry may be NULL (an untranslated help note). The copy then holds NULL in
 * the Brick id's slot too, so the lookup falls back to the *desktop* English rather than to a
 * translated sentence about a button the window does not have.
 */
static const char *s_desktop_english[MESH_STR_COUNT];
static const char *s_desktop_tables[MESH_I18N_LOCALE_COUNT][MESH_STR_COUNT];
static struct inkcell_i18n_locale s_desktop_locales[MESH_I18N_LOCALE_COUNT];
static struct inkcell_i18n_catalog s_desktop_catalog;
static bool s_desktop_built;
static bool s_desktop;

static void desktop_build(void) {
    if (s_desktop_built) {
        return;
    }
    for (size_t id = 0; id < (size_t)MESH_STR_COUNT; ++id) {
        s_desktop_english[id] = k_english[id];
    }
    for (size_t i = 0; i < MESH_DESKTOP_COUNT; ++i) {
        s_desktop_english[k_desktop[i].brick] = k_english[k_desktop[i].desktop];
    }
    for (size_t l = 0; l < MESH_I18N_LOCALE_COUNT; ++l) {
        s_desktop_locales[l] = k_locales[l];
        const char *const *table = k_locales[l].table;
        if (table == NULL) {
            continue;
        }
        for (size_t id = 0; id < (size_t)MESH_STR_COUNT; ++id) {
            s_desktop_tables[l][id] = table[id];
        }
        for (size_t i = 0; i < MESH_DESKTOP_COUNT; ++i) {
            s_desktop_tables[l][k_desktop[i].brick] = table[k_desktop[i].desktop];
        }
        s_desktop_locales[l].table = s_desktop_tables[l];
    }
    s_desktop_catalog = k_catalog;
    s_desktop_catalog.english = s_desktop_english;
    s_desktop_catalog.locales = s_desktop_locales;
    s_desktop_built = true;
}

void mesh_i18n_register(void) {
    if (s_desktop) {
        desktop_build();
    }
    inkcell_i18n_set_catalog(s_desktop ? &s_desktop_catalog : &k_catalog);
}

void mesh_i18n_set_desktop(bool desktop) {
    if (desktop == s_desktop) {
        return;
    }
    /* A new catalog drops inkcell back to its first locale, so the reader's language is
       carried across by id. */
    const struct inkcell_i18n_locale *current = inkcell_i18n_locale();
    const char *language = current != NULL ? current->id : NULL;
    s_desktop = desktop;
    mesh_i18n_register();
    if (language != NULL) {
        (void)inkcell_i18n_set_locale(language);
    }
}

bool mesh_i18n_desktop(void) { return s_desktop; }

/* inkcell_i18n_locale_count() and inkcell_i18n_locale_at() are not defined here: they are
   inkcell_i18n_locale_count() and inkcell_i18n_locale_at() under their old names, and once the
   catalog above is registered those answer out of k_locales. Defining them here as well is a
   duplicate symbol, which is what the linker said the first time. */
