#ifndef MESH_I18N_STRINGS_H
#define MESH_I18N_STRINGS_H

/*
 * Every word this client shows, continuing inkcell's.
 *
 * The mechanism moved to inkcell - the catalog macros, the locale registry, the plural rules,
 * the format-string validation. What is here is this client's half of the *content*: the ids
 * start where inkcell's fifteen leave off, and mesh_i18n_register() hands both halves over as
 * one table.
 *
 * A screen still never spells out a sentence. It names a string id - MESH_STR_TAB_NODES,
 * MESH_STR_TOAST_NOT_CONNECTED - and inkcell_str() answers with the text for the locale in
 * force. The catalog is include/mesh/i18n/catalog.def, one line per string, and adding a string
 * is still adding a line there.
 *
 * What is deliberately *not* here:
 *
 *   - Log lines. inkcell_log() output is for whoever is reading `deploy-logs`, and a bug report in
 *     a language the maintainer cannot read is worse than no bug report.
 *   - Names shared with the rest of Meshtastic: region codes ("EU 868"), hardware models
 *     ("Heltec V3"), modem presets ("Long Range - Fast"), device roles ("Router"). A setting
 *     read off the Brick has to be recognisable in the phone app and back, so those stay in
 *     src/core/session/radio_settings.c untranslated, for the same reason a channel key is
 *     shown as base64.
 *   - Protocol, path, environment and config text. Nobody reads it as prose.
 *   - src/main.c's --help and the cli/stub backends, which are the headless developer surfaces
 *     and never reach the device screen.
 *   - The fifteen words inkcell itself prints - "now", "15m", the elapsed-time forms, the list
 *     count. Those are the toolkit's, and a screen that wants one names it INKCELL_STR_*.
 *
 * See docs/i18n.md.
 */

#include "inkcell/i18n/strings.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This client's ids, continuing inkcell's.
 *
 * **Anonymous, and `enum inkcell_str_id` is inkcell's own type.** There is one index space here -
 * an id *is* a table index into the one array mesh_i18n_register() hands over - and a second
 * enum type over it makes every one of the ~900 call sites an implicit conversion between two
 * enum types, which is 214 warnings under clang and a legitimate complaint: nothing says the
 * two agree. Declaring the ids as constants of inkcell's type says that they do.
 *
 * The base is an enumerator rather than an `= INKCELL_STR_COUNT` on the first entry, because
 * the first entry is whatever happens to be at the top of the catalog and a renumbering should
 * not depend on which line that is. An enumerator with no entry of its own takes the value
 * before the first real one, so MESH_STR__BASE is one less than where the catalog starts.
 *
 * A plural entry occupies two consecutive ids - _ONE and _OTHER - because the id is the index,
 * and inkcell_str_plural() picks between them by the locale's rule rather than by `n == 1`,
 * which is an English rule and not even that in every sentence.
 */
enum {
    MESH_STR__BASE = INKCELL_STR_COUNT - 1,
#define MESH_STR_ENTRY(id, text) MESH_STR_##id,
#define MESH_STR_PLURAL_ENTRY(id, one, other) MESH_STR_##id##_ONE, MESH_STR_##id##_OTHER,
#include "mesh/i18n/catalog.def"
#undef MESH_STR_ENTRY
#undef MESH_STR_PLURAL_ENTRY
    MESH_STR_COUNT
};

/*
 * Hands this client's catalog to inkcell. Call once, before inkcell_i18n_init().
 *
 * Idempotent, so a test that re-registers is not a leak or a double free: the tables are static
 * and what this installs is a pointer to one structure.
 */
void mesh_i18n_register(void);

/* inkcell_i18n_locale_count() and inkcell_i18n_locale_at() answer for both halves: they are
   inkcell's, reading the catalog registered above. */

#ifdef __cplusplus
}
#endif

#endif /* MESH_I18N_STRINGS_H */
