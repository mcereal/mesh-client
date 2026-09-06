#ifndef MESH_I18N_STRINGS_H
#define MESH_I18N_STRINGS_H

/*
 * Every word the user reads, in one place.
 *
 * A screen never spells out a sentence. It names a *string id* - MESH_STR_TAB_NODES,
 * MESH_STR_TOAST_NOT_CONNECTED - and this module answers with the text for the locale in
 * force, exactly the way src/ui/theme.c answers a colour role. That is what makes a language
 * switch total rather than a hunt: the renderers hold no opinion about English, so there is
 * nowhere for an untranslated sentence to hide.
 *
 * The catalog is include/mesh/i18n/catalog.def, one line per string. Adding a string is adding
 * a line there; the enum, the English table and the translation template all come off the same
 * list, so they cannot drift apart.
 *
 * A locale is a table of the same length with NULL where it has nothing to say, and NULL falls
 * back to English. A half-finished translation therefore ships and reads as a mixture rather
 * than as blanks, which is the state every translation is in for a while.
 *
 * What is deliberately *not* here:
 *
 *   - Log lines. mesh_log() output is for whoever is reading `deploy-logs`, and a bug report
 *     in a language the maintainer cannot read is worse than no bug report.
 *   - Names shared with the rest of Meshtastic: region codes ("EU 868"), hardware models
 *     ("Heltec V3"), modem presets ("Long Range - Fast"), device roles ("Router"). A setting
 *     read off the Brick has to be recognisable in the phone app and back, so those stay in
 *     src/core/radio_settings.c untranslated, for the same reason a channel key is shown as
 *     base64.
 *   - Protocol, path, environment and config text. Nobody reads it as prose.
 *   - src/main.c's --help and the cli/stub backends, which are the headless developer
 *     surfaces and never reach the device screen.
 *
 * See docs/i18n.md.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The ids, generated from the catalog.
 *
 * A plural entry occupies two consecutive ids - _ONE and _OTHER - because the id *is* the
 * table index, and mesh_str_plural() picks between them by the locale's rule rather than by
 * `n == 1`, which is an English rule and not even that in every sentence.
 */
enum mesh_str_id {
#define MESH_STR_ENTRY(id, text) MESH_STR_##id,
#define MESH_STR_PLURAL_ENTRY(id, one, other) MESH_STR_##id##_ONE, MESH_STR_##id##_OTHER,
#include "mesh/i18n/catalog.def"
#undef MESH_STR_ENTRY
#undef MESH_STR_PLURAL_ENTRY
    MESH_STR_COUNT
};

/* How many forms a plural entry has. Two covers English and the languages that count like it;
   Polish, Russian and Arabic need three to six, and widening them is this constant, the
   MESH_STR_PLURAL_ENTRY macro above, and nothing else. See docs/i18n.md. */
#define MESH_STR_PLURAL_FORMS 2

/*
 * One language.
 *
 * `table` is MESH_STR_COUNT entries; a NULL entry means "not translated yet" and resolves to
 * English. English itself has a NULL table, because it *is* the fallback.
 */
struct mesh_i18n_locale {
    const char *id;   /* what MESHCLIENT_LANG and the Settings row match, e.g. "en" */
    const char *name; /* what the Settings row shows, in that language */
    const char *const *table;
    /* Which plural form (0 .. MESH_STR_PLURAL_FORMS-1) `n` takes. NULL means the English rule,
       which is "one for 1, other for everything else including 0". */
    uint8_t (*plural_form)(uint32_t n);
};

/* The text for `id` in the current locale, never NULL: an untranslated entry falls back to
   English and an out-of-range id to the empty string. The pointer is to static storage and
   stays valid until the locale changes. */
const char *mesh_str(enum mesh_str_id id);

/* The text for `id` in `locale` specifically, with the same guarantees. */
const char *mesh_str_in(const struct mesh_i18n_locale *locale, enum mesh_str_id id);

/* The form of a plural entry that `count` takes. `one_form` is the _ONE id; the locale's rule
   picks the offset from it. */
const char *mesh_str_plural(enum mesh_str_id one_form, uint32_t count);

/*
 * snprintf() with a catalog entry as the format.
 *
 * Catalog entries carry their own %-specifiers - "Sent to %s" is one string, not "Sent to "
 * plus a name, because a language that puts the verb last cannot translate the halves. That
 * makes the format non-literal, which -Wformat=2 rightly objects to at every call site, so the
 * objection is answered once here instead of everywhere: this is the only place in the client
 * that formats a string it did not write, and mesh_i18n_validate() is what checks a
 * translation did not change the specifiers out from under a caller.
 *
 * Returns what snprintf() would, and always NUL-terminates when out_len > 0.
 */
int mesh_str_format(char *out, size_t out_len, enum mesh_str_id id, ...);
int mesh_str_vformat(char *out, size_t out_len, enum mesh_str_id id, va_list args);

/* The same, choosing the plural form for `count` first. `count` is not passed on to the
   format; pass it again in the arguments if the sentence shows the number. */
int mesh_str_format_plural(char *out, size_t out_len, enum mesh_str_id one_form, uint32_t count,
                           ...);

/* ---- locales ------------------------------------------------------------------------------ */

size_t mesh_i18n_locale_count(void);
const struct mesh_i18n_locale *mesh_i18n_locale_at(size_t index);
const struct mesh_i18n_locale *mesh_i18n_locale_by_id(const char *id); /* NULL when unknown */
const struct mesh_i18n_locale *mesh_i18n_locale_english(void);

/* The locale in force, never NULL. */
const struct mesh_i18n_locale *mesh_i18n_locale(void);

/* Switch languages. False, and no change, when `id` names no locale. */
bool mesh_i18n_set_locale(const char *id);

/*
 * Pick the starting locale from the environment: MESHCLIENT_LANG first, then LC_ALL, LC_MESSAGES
 * and LANG, each matched on the language part alone so "fr_CA.UTF-8" finds "fr". English when
 * nothing matches. Safe to call more than once.
 */
void mesh_i18n_init(void);

/*
 * Whether `locale` is fit to ship: the ids are in range, and every entry it does translate
 * carries the same %-specifiers, in the same order, as the English it replaces. A translation
 * that turns "%u of %u" into "%s of %u" is a crash, not a typo, which is why this runs over
 * every locale in the tests.
 *
 * `reason` is filled with a one-line explanation on failure when it is non-NULL.
 */
bool mesh_i18n_validate(const struct mesh_i18n_locale *locale, char *reason, size_t reason_len);

/* The catalog id's spelling, for the translation template and for test failures:
   "TAB_NODES", not the text. NULL when `id` is out of range. */
const char *mesh_str_id_name(enum mesh_str_id id);

#ifdef __cplusplus
}
#endif

#endif /* MESH_I18N_STRINGS_H */
