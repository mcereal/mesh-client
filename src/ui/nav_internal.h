#ifndef MESH_UI_NAV_INTERNAL_H
#define MESH_UI_NAV_INTERNAL_H

/*
 * The seams between nav.c and the four files split out of it.
 *
 * The navigation model is one struct and one key handler, but the behaviour behind that handler
 * had grown to 2000 lines covering five unrelated subjects: the canned-message list, the
 * on-screen keyboard, the conversation and send-to lists, the settings editor, and the screen
 * routing that ties them together. Each is now a file; nav.c keeps the routing.
 *
 * Nothing here is public - that is include/mesh/ui/nav.h. These are the calls that would still
 * be `static` if this were one translation unit, so keep the list short: a symbol added here is
 * a seam widened, and the direction that stays clean is nav.c calling outward.
 */

#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

/* ---- nav.c: screen routing --------------------------------------------------------------- */

/* Open a thread against `target`/`kind`, resolving the name from the store. */
void mesh_ui_nav_open_thread(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                             uint32_t node_id, uint8_t channel, const char *name_hint);
/* The unfiltered firehose: every message, whoever it was for. */
void mesh_ui_nav_open_all_traffic(struct mesh_ui_nav *nav);
/* The compose overlay over the current thread. */
void mesh_ui_nav_open_compose(struct mesh_ui_nav *nav);

/* ---- nav_conversations.c ----------------------------------------------------------------- */

/* Channel `index`'s display name, falling back to "Channel N". */
void mesh_ui_nav_channel_name(const struct mesh_ui_store *store, uint8_t index, char *out,
                              size_t out_len);
/* A node number as a human-readable name, falling back to the "!hex" id. */
void mesh_ui_nav_node_name(const struct mesh_ui_store *store, uint32_t node_id, char *out,
                           size_t out_len);
/* Opens the conversation at `index` in the Messages list. False when the index is past the end. */
bool mesh_ui_nav_open_conversation(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   uint32_t index, bool then_compose);
/* Opens the send-to picker over the current screen. */
void mesh_ui_nav_picker_open(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                             bool then_compose);
/* One key while the picker is up. False when the key was not the picker's to take. */
bool mesh_ui_nav_picker_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum mesh_ui_key key);

/* ---- nav_keyboard.c ---------------------------------------------------------------------- */

/* Tears the keyboard down and parks the cursor, restoring whatever it was opened over. */
void mesh_ui_nav_keyboard_close(struct mesh_ui_nav *nav);
/* One key while the keyboard is up. False when the key was not the keyboard's to take. */
bool mesh_ui_nav_keyboard_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              enum mesh_ui_key key, struct mesh_ui_action *action);

/* ---- nav_settings.c ---------------------------------------------------------------------- */

/* The store's handshake if it holds one, else NULL - what the settings model takes as its
   "what the radio actually said" argument. */
const struct mesh_ui_handshake_state *mesh_ui_nav_handshake(const struct mesh_ui_store *store);
/* The settings row under the cursor, with or without the pending edits applied. */
bool mesh_ui_nav_settings_current(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  bool with_edits, struct mesh_ui_settings_item *out);
/* Left/right on a settings row: records or drops an edit. */
bool mesh_ui_nav_settings_edit_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   enum mesh_ui_key key);
/* Commits the keyboard's draft into the field it was opened for. */
bool mesh_ui_nav_settings_commit_text(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);
/* B out of an open section, or off the section list. False when there is nowhere to go. */
bool mesh_ui_nav_settings_back(struct mesh_ui_nav *nav);
/* A on the settings section list: opens a section or a channel slot. */
bool mesh_ui_nav_settings_section_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                      enum mesh_ui_key key, struct mesh_ui_action *action,
                                      bool *handled);
/* One key while a confirm sheet is up. False when the key was not the sheet's to take. */
bool mesh_ui_nav_confirm_key(struct mesh_ui_nav *nav, enum mesh_ui_key key,
                             struct mesh_ui_action *action);
/* Fills `action` with the radio action the open confirm sheet is asking about. */
void mesh_ui_nav_fill_radio_action(const struct mesh_ui_nav *nav,
                                   enum mesh_ui_settings_action which,
                                   struct mesh_ui_action *action);

#endif /* MESH_UI_NAV_INTERNAL_H */
