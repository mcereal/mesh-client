#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The commands the compose sheet offers in a MeshCore repeater's conversation, where the canned
 * replies would be.
 *
 * A message to a repeater is a command it runs for its admin, and its reply is the next message
 * in the thread - so the thread is the repeater's console, and this list is what the Brick has
 * in place of a keyboard worth typing `get advert.interval` on. Each is a command's own
 * spelling and goes on the air unchanged, which is why the list is never translated: it is what
 * the repeater reads, not what a person does.
 *
 * Only what a repeater answers over the mesh, and only what is safe as one press. Its stats
 * commands are its serial console's alone - the firmware runs them only with no sender - and
 * `reboot`, `erase` and every `set` are left to the keyboard, where they cost a sentence to
 * send rather than a slip of the thumb.
 */
size_t mesh_ui_repeater_command_count(void);

/* The command itself, or "" when the index is past the end. */
const char *mesh_ui_repeater_command(size_t index);

#ifdef __cplusplus
}
#endif
