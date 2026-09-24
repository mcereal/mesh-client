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

#include "mesh/i18n/strings.h"
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
/* Opens the keyboard on the open thread with no overlay behind it, so cancelling lands
   back on the conversation rather than on the canned list. */
void mesh_ui_nav_open_keyboard(struct mesh_ui_nav *nav);

/* Whether row `row` of the screen the nav is on is a group title the cursor may not stand on. */
bool mesh_ui_nav_row_is_heading(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                uint32_t row);

/* ---- nav_conversations.c ----------------------------------------------------------------- */

/* Channel `index`'s display name, falling back to "Channel N". */
void mesh_ui_nav_channel_name(const struct mesh_ui_store *store, uint8_t index, char *out,
                              size_t out_len);
/* A node number as a human-readable name, falling back to the "!hex" id. */
void mesh_ui_nav_node_name(const struct mesh_ui_store *store, uint32_t node_id, char *out,
                           size_t out_len);
/* Opens the conversation at `index` in the Messages list. False when the index is past the end. */
bool mesh_ui_nav_open_conversation(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   uint32_t index);
/* X on a conversation row: arms the delete, or emits it when that row is already armed. */
bool mesh_ui_nav_delete_conversation(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                     uint32_t index, struct mesh_ui_action *action);
/* Opens the send-to picker over the current screen. `follow` is what opens over the thread
   once a row is picked. */
void mesh_ui_nav_picker_open(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                             enum mesh_ui_picker_follow follow);
/* One key while the picker is up. False when the key was not the picker's to take. */
bool mesh_ui_nav_picker_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum inkcell_key key);

/* ---- nav_keyboard.c ----------------------------------------------------------------------- */

/*
 * Raises the keyboard to type the network radio's address, preloaded with `host` (NULL or ""
 * starts empty). What the Devices tab's last row does.
 *
 * Lower case rather than the waypoint keyboard's upper: every layer's first row is the digits,
 * and an address is digits, dots and colons - so the cursor lands on '1' and the whole of an
 * IPv4 literal is typed without changing layer once.
 */
void mesh_ui_nav_open_network_keyboard(struct mesh_ui_nav *nav, const char *host);
/* Puts the Radio tab on its device list, which is where every keyboard raised from that list
   lands again - the address it typed, the PIN a pairing asked for. */
void mesh_ui_nav_land_on_devices(struct mesh_ui_nav *nav);
/* The verbs the Status cards offer, from the store: nav.c walks them and nav_click.c answers a
   click on one of their buttons. */
struct mesh_ui_status_actions;
void mesh_ui_nav_status_actions(const struct mesh_ui_store *store,
                                struct mesh_ui_status_actions *out);
/*
 * Done on that keyboard.
 *
 * A draft with something in it asks the app to connect to it - which is also what stores it,
 * because the address a link was brought up on is the one worth remembering. An *empty* draft
 * forgets the configured host instead: deleting the address is the only way the user can say
 * "stop reaching for that", and refusing an empty draft the way the waypoint keyboard does
 * would leave them with a host they cannot clear.
 */
bool mesh_ui_nav_commit_network_host(struct mesh_ui_nav *nav, struct mesh_ui_action *action);

/* The keyboard that collects a Meshtastic channel link, from the Channels list's import row,
   and the Done that checks it. The commit raises the confirm sheet rather than an action: what
   reaches the app is the answer to that sheet. */
void mesh_ui_nav_open_channel_url_keyboard(struct mesh_ui_nav *nav);
bool mesh_ui_nav_commit_channel_url(struct mesh_ui_nav *nav);

/* The same pair for a Meshtastic contact link, from the User list's add row. Separate from the
   channel pair rather than one taking a kind: the parsers differ, the sheets differ, and the
   only thing they share is the keyboard they open. */
void mesh_ui_nav_open_contact_url_keyboard(struct mesh_ui_nav *nav);
bool mesh_ui_nav_commit_contact_url(struct mesh_ui_nav *nav);

/* B on the share sheet. Its own handler rather than a branch in the tab's, the way the help
   overlay has one: nothing on this screen moves, so every key but the one that leaves it is a
   key that does nothing. */
bool mesh_ui_nav_share_key(struct mesh_ui_nav *nav, enum inkcell_key key);

/* B on the contact code sheet, the share sheet's counterpart and for its reason. */
bool mesh_ui_nav_contact_key(struct mesh_ui_nav *nav, enum inkcell_key key);

/* ---- nav_waypoints.c --------------------------------------------------------------------- */

/* Opens the place with this id over the list, parking the list's cursor. */
void mesh_ui_nav_open_waypoint(struct mesh_ui_nav *nav, uint32_t id);
/* The places list over the Nodes roster, and back out of it to the roster's own row. */
void mesh_ui_nav_open_waypoints(struct mesh_ui_nav *nav);
bool mesh_ui_nav_close_waypoints(struct mesh_ui_nav *nav);
/* B out of an open place. False when the list is already showing. */
bool mesh_ui_nav_close_waypoint(struct mesh_ui_nav *nav);
/* Raises the keyboard to name a new place at `source_node`'s fix - 0 for our own radio. */
void mesh_ui_nav_open_waypoint_keyboard(struct mesh_ui_nav *nav, uint32_t source_node);
/* Send on that keyboard: emits the share. False when the draft is empty, which leaves the
   keyboard up rather than broadcasting a place with no name. */
bool mesh_ui_nav_commit_waypoint(struct mesh_ui_nav *nav, struct mesh_ui_action *action);
/* Rows on whichever of the tab's two levels is showing. */
uint32_t mesh_ui_nav_waypoint_row_count(const struct mesh_ui_nav *nav,
                                        const struct mesh_ui_store *store);
/* A on either level: opens a place, starts a new one, or runs an action row. */
bool mesh_ui_nav_waypoint_confirm(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  uint32_t cursor, struct mesh_ui_action *action);
/* Closes a detail whose place has gone, and clamps the parked list position. */
bool mesh_ui_nav_waypoint_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);

/* ---- nav_map.c --------------------------------------------------------------------------- */

/* Opens the map over the node list. `focus_node` aims it at one node - 0 frames everything,
   which is also what START goes back to. */
void mesh_ui_nav_open_map(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                          uint32_t focus_node);
/* B out of the map. False when it was not open. */
bool mesh_ui_nav_close_map(struct mesh_ui_nav *nav);
/* Closes a map with nothing left to draw on it. */
bool mesh_ui_nav_map_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);
/*
 * One key while the map is up, taken *before* the routing that turns Left and Right into tabs -
 * the map is the one screen where the d-pad moves the world rather than a cursor.
 *
 * `handled` says whether the press was the map's, which is the pattern the settings section
 * handler uses: the return value is whether the frame changed, and a press that moved nothing
 * still has to stop here rather than falling through and switching tabs.
 */
bool mesh_ui_nav_map_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                         enum inkcell_key key, bool *handled);

/* ---- nav_keyboard.c ---------------------------------------------------------------------- */

/* Tears the keyboard down and parks the cursor, restoring whatever it was opened over. */
void mesh_ui_nav_keyboard_close(struct mesh_ui_nav *nav);
/* One key while the keyboard is up. False when the key was not the keyboard's to take. */
bool mesh_ui_nav_keyboard_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              enum inkcell_key key, struct mesh_ui_action *action);

/* ---- nav.c ------------------------------------------------------------------------------- */

/*
 * Puts `screen`'s cursor on the first row it may stand on, which is row 0 on every list whose
 * first row is not a group title.
 *
 * Declared here rather than kept static because opening a level is not one file's job: the node
 * detail is opened from the Nodes list in nav.c *and* from a marker in nav_map.c, and the second
 * of those wrote a bare 0 - which was the same answer until the detail's first row became the
 * actions group's heading. Two opinions about where a level opens is exactly the split this
 * group of files keeps collapsing.
 */
/* Moves the cursor one whole group - one card - rather than one row: L2 and R2. Returns false
   where there is no group that way, or where the screen draws no groups at all. */
bool mesh_ui_nav_cursor_group(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              int delta);

void mesh_ui_nav_cursor_to_first_row(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                     enum mesh_ui_screen screen);

/* ---- nav_settings.c ---------------------------------------------------------------------- */

/* The store's handshake if it holds one, else NULL - what the settings model takes as its
   "what the radio actually said" argument. */
const struct mesh_ui_handshake_state *mesh_ui_nav_handshake(const struct mesh_ui_store *store);
/* The settings row under the cursor, with or without the pending edits applied. */
bool mesh_ui_nav_settings_current(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  bool with_edits, struct mesh_ui_settings_item *out);
/* Left/right on a settings row: records or drops an edit. */
bool mesh_ui_nav_settings_edit_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   enum inkcell_key key);
/* Commits the keyboard's draft into the field it was opened for. */
bool mesh_ui_nav_settings_commit_text(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);
/* B out of an open section, or off the section list. False when there is nowhere to go. */
bool mesh_ui_nav_settings_back(struct mesh_ui_nav *nav);
/* The open section's rows as built, on whichever tab shows one (mesh_ui_nav_open_section());
   0 when none is. Pending edits are applied only on the Settings tab, which is whose they are. */
uint32_t mesh_ui_nav_section_items(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   bool with_edits, struct mesh_ui_settings_item *items,
                                   uint32_t max);
/* A Radio tab page over the cards, opened on its first row with the device list's row parked;
   closing puts the row back. Close answers false when no page was open. */
void mesh_ui_nav_open_radio_page(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                 enum mesh_ui_radio_page page);
bool mesh_ui_nav_close_radio_page(struct mesh_ui_nav *nav);
/* A on the settings section list: opens a section or a channel slot. */
bool mesh_ui_nav_settings_section_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                      enum inkcell_key key, struct mesh_ui_action *action,
                                      bool *handled);
/* One key while a confirm sheet is up. False when the key was not the sheet's to take. */
/*
 * Which of a dialog's two answers a press lands on, from where the cursor is now.
 *
 * Both dialogs used to toggle on every direction, which was right only because neither had any
 * way to be wrong: the two answers sit side by side on one line when the words are short enough
 * and are *stacked* when they are not, and which of those happened is a fact about the frame
 * that was drawn. Left-right on a stacked pair is not the press that moves between them.
 *
 * So the press is resolved against the boxes the frame registered (`focus` on struct
 * mesh_ui_store), and the toggle is what is left when there are none - a backend that draws no
 * boxes, a test with no panel behind it, the frame before the first one. That fallback is the
 * old behaviour exactly, which is what makes this safe to adopt one screen at a time.
 *
 * Returns the answer to put the cursor on; `cursor` back again is a press that goes nowhere.
 */
uint8_t mesh_ui_nav_dialog_answer(const struct mesh_ui_store *store, enum inkcell_key key,
                                  uint8_t cursor);

bool mesh_ui_nav_confirm_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                             enum inkcell_key key, struct mesh_ui_action *action);
/* One key while the key-verification sheet is up. Reads the stage out of the store, because
   which answer each button gives depends on what the radio is asking. */
bool mesh_ui_nav_verify_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum inkcell_key key, struct mesh_ui_action *action);
/* Fills `action` with what an ACTION row - or the confirm sheet standing in front of one -
   is asking for. Most are radio actions; the two forget rows are the client's own. */
void mesh_ui_nav_fill_settings_action(const struct mesh_ui_nav *nav,
                                      enum mesh_ui_settings_action which,
                                      struct mesh_ui_action *action);

#endif /* MESH_UI_NAV_INTERNAL_H */
