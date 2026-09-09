#ifndef MESH_UI_HELP_H
#define MESH_UI_HELP_H

/*
 * What the client can explain about where the user is standing.
 *
 * The Settings tab offers something like two hundred rows and, until this existed, explained
 * none of them. This is the half of that gap which is not drawing: given a snapshot, what should
 * the help screen say - as a title and a list of paragraphs, each named by a catalog id.
 *
 * It is `src/ui/status.c`'s shape and it is here for status.c's reason. `nav.c` walks this list
 * to scroll it, `src/ui/actions.c` asks whether there is one at all to decide whether to name
 * the press, and the backend draws it; three opinions about one list is how the keycap under the
 * screen and the screen above it come to disagree.
 *
 * Nothing here holds text. A topic is ids the whole way down, which is what keeps the help
 * screen translatable and what lets a test hold every note to a length without rendering one.
 *
 * The notes themselves are not stored here either: a section's is a table beside its icon in
 * settings.c and a field's is a member of its own row in k_fields, because "what does this
 * setting do" is a property of the setting in the same way its label and its presets are. This
 * module assembles; it does not describe. See docs/help.md.
 */

#include "mesh/i18n/strings.h"
#include "mesh/ui/settings.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_nav;
struct mesh_ui_settings;
struct mesh_ui_handshake_state;

/*
 * The most a topic carries: every row a section can have, plus the section's own note.
 *
 * Sized so it cannot overflow rather than sized for what the notes are expected to be. A cap
 * chosen for "how many rows will really need explaining" would be a cap that silently drops the
 * last paragraph the first time a section outgrew the guess - and worse, mesh_ui_help_entry_for_
 * row() would then clamp a row onto somebody else's explanation, which is a screen confidently
 * answering the wrong question. A section cannot have more explained rows than it has rows, so
 * this is the number that makes the failure unreachable. It costs a couple of hundred bytes of a
 * structure the callers already build on the stack.
 */
#define MESH_UI_HELP_ENTRIES_MAX (MESH_UI_SETTINGS_ITEMS_MAX + 1U)

/*
 * One paragraph and what it is about.
 *
 * `label` is the row's own name, so a reader scrolling can see which setting each paragraph
 * belongs to; MESH_STR_NONE marks the topic's own opening note, which is about the whole screen
 * and belongs to no row.
 */
struct mesh_ui_help_entry {
    enum mesh_str_id label;
    enum mesh_str_id body;
};

struct mesh_ui_help_topic {
    enum mesh_str_id title;
    struct mesh_ui_help_entry entries[MESH_UI_HELP_ENTRIES_MAX];
    uint32_t count;
};

/*
 * The topic for where this nav is, or false when there is nothing to explain.
 *
 * False is the load-bearing answer: it is what stops the action bar naming a press that would
 * open an empty screen, and it is the same call the press itself makes, so the two cannot
 * disagree. `out` is zeroed either way.
 *
 * It takes the radio's configuration and the handshake rather than a whole snapshot, which is
 * the argument list every other settings call already has - and the reason is not only
 * consistency: the two callers are the action bar, which builds a bar for every frame, and the
 * key handler, which has a store rather than a snapshot. A snapshot parameter would have made
 * one of them copy tens of kilobytes onto a fixed stack per press. `handshake` may be NULL.
 */
bool mesh_ui_help_topic(const struct mesh_ui_settings *settings,
                        const struct mesh_ui_handshake_state *handshake,
                        const struct mesh_ui_nav *nav, struct mesh_ui_help_topic *out);

/*
 * Which entry explains row `row` of the section this nav has open.
 *
 * What makes the help screen open where the user was looking rather than at the top: a press on
 * `Hop limit` lands on the hop-limit paragraph, and scrolling up from there reaches the
 * section's own note. A row with no paragraph of its own lands on the nearest one above it,
 * which is 0 - the section's overview - when there is none.
 */
uint32_t mesh_ui_help_entry_for_row(const struct mesh_ui_settings *settings,
                                    const struct mesh_ui_handshake_state *handshake,
                                    const struct mesh_ui_nav *nav, uint32_t row);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_HELP_H */
