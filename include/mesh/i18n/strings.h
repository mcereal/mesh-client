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
 *   - Log lines. mesh_log() output is for whoever is reading `deploy-logs`, and a bug report in
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
 *     count. Those are the toolkit's, and their old MESH_STR_ names are bridged below.
 *
 * See docs/i18n.md.
 */

#include "inkcell/i18n/strings.h"

#include "mesh/inkcell_compat.h"

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
 * **Anonymous, and `enum mesh_str_id` is inkcell's own type.** There is one index space here -
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

/* The type an id is held in. One enum for one index space - see the note above. */
#define mesh_str_id inkcell_str_id

/*
 * The twenty-three ids that moved to inkcell, under the names this tree already used.
 *
 * Bridged rather than rewritten for the reason everything in inkcell_compat.h is, and these are
 * the ones that could not be generated with the rest: MESH_STR_* is still a live prefix - it is
 * what the 880 ids above are called - so a blanket rule would have rewritten this client's own
 * catalog along with the toolkit's.
 */
#define MESH_STR_NONE INKCELL_STR_NONE
#define MESH_STR_COMMON_UNKNOWN_SHORT INKCELL_STR_COMMON_UNKNOWN_SHORT
#define MESH_STR_TIME_NOW INKCELL_STR_TIME_NOW
#define MESH_STR_TIME_SECONDS_SHORT INKCELL_STR_TIME_SECONDS_SHORT
#define MESH_STR_TIME_MINUTES_SHORT INKCELL_STR_TIME_MINUTES_SHORT
#define MESH_STR_TIME_HOURS_SHORT INKCELL_STR_TIME_HOURS_SHORT
#define MESH_STR_TIME_DAYS_SHORT INKCELL_STR_TIME_DAYS_SHORT
#define MESH_STR_LIST_TITLE_COUNT INKCELL_STR_LIST_TITLE_COUNT
#define MESH_STR_LIST_TITLE_COUNT_OLDER INKCELL_STR_LIST_TITLE_COUNT_OLDER
#define MESH_STR_TREND_SPAN_15M INKCELL_STR_TREND_SPAN_15M
#define MESH_STR_TREND_SPAN_1H INKCELL_STR_TREND_SPAN_1H
#define MESH_STR_TREND_SPAN_6H INKCELL_STR_TREND_SPAN_6H
#define MESH_STR_TREND_SPAN_ALL INKCELL_STR_TREND_SPAN_ALL
#define MESH_STR_HINT_QUIT_MENU INKCELL_STR_HINT_QUIT_MENU
#define MESH_STR_HINT_QUIT_KEY_CODE INKCELL_STR_HINT_QUIT_KEY_CODE
/* The keyboard's own keycaps, which went with the grid. The submit key did not - see
   KEY_DONE and KEY_SEND, which are still this client's. */
#define MESH_STR_KEY_LAYER_UPPER INKCELL_STR_KEY_LAYER_UPPER
#define MESH_STR_KEY_LAYER_SYMBOLS INKCELL_STR_KEY_LAYER_SYMBOLS
#define MESH_STR_KEY_LAYER_LOWER INKCELL_STR_KEY_LAYER_LOWER
#define MESH_STR_KEY_LAYER_EMOJI INKCELL_STR_KEY_LAYER_EMOJI
#define MESH_STR_KEY_LAYER_EMOJI_MORE INKCELL_STR_KEY_LAYER_EMOJI_MORE
#define MESH_STR_KEY_SPACE INKCELL_STR_KEY_SPACE
#define MESH_STR_KEY_DELETE INKCELL_STR_KEY_DELETE
#define MESH_STR_KEY_CANCEL INKCELL_STR_KEY_CANCEL

/* The plural machinery is inkcell's; the count is about the mechanism, not about either
   catalog, so it keeps its old name here. */
#define MESH_STR_PLURAL_FORMS INKCELL_STR_PLURAL_FORMS

/*
 * Hands this client's catalog to inkcell. Call once, before mesh_i18n_init().
 *
 * Idempotent, so a test that re-registers is not a leak or a double free: the tables are static
 * and what this installs is a pointer to one structure.
 */
void mesh_i18n_register(void);

/* mesh_i18n_locale_count() and mesh_i18n_locale_at() still answer, through inkcell_compat.h:
   they are inkcell's, reading the catalog registered above. */

#ifdef __cplusplus
}
#endif

#endif /* MESH_I18N_STRINGS_H */
