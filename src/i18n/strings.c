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
               "the English table and enum inkcell_str_id disagree about how many ids there are");
_Static_assert(sizeof k_id_names / sizeof k_id_names[0] == (size_t)MESH_STR_COUNT,
               "the id-name table and enum inkcell_str_id disagree about how many ids there are");

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

void mesh_i18n_register(void) { inkcell_i18n_set_catalog(&k_catalog); }

/* inkcell_i18n_locale_count() and inkcell_i18n_locale_at() are not defined here: they are
   inkcell_i18n_locale_count() and inkcell_i18n_locale_at() under their old names, and once the
   catalog above is registered those answer out of k_locales. Defining them here as well is a
   duplicate symbol, which is what the linker said the first time. */
