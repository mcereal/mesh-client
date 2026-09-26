#pragma once

#include "mesh/map/viewport.h"
/* For struct mesh_ui_message_view, which the transcript's filter takes by value: the nav has
   to see its definition, and the record header names nothing here, so this is not a cycle. */
#include "mesh/ui/store_message.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inkcell/ui/key.h"
#include "inkcell/ui/keyboard.h"
#include "inkstand/nav/dialog.h"
#include "inkstand/nav/toast.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_store;

/* Logical buttons moved to inkcell (inkcell/ui/key.h): which physical button reports which
   press is a fact about a piece of plastic, and the navigation model never sees a keycode
   either way. The names are INKCELL_KEY_*. */

/* Tabs, in the order LEFT/RIGHT (and L1/R1) walk them. Compose is not one: it is an overlay
   over the open conversation, so it can never be reached with a stale destination. */
enum mesh_ui_screen {
    MESH_UI_SCREEN_MESSAGES = 0,
    /*
     * The points on the mesh: the nodes, which move, and the places, which do not. Waypoints was
     * a tab of its own beside this one and is now a row of it (MESH_UI_NODES_WAYPOINTS_ROW),
     * beside the map row - the map already drew both, so the list of places belongs with the
     * picture of them rather than a tab away from it.
     */
    MESH_UI_SCREEN_NODES,
    /*
     * The radio we are attached to, and the radios we could be.
     *
     * This was two tabs - Devices and Status - and they were one subject split by a question of
     * layout: Status said how the link was doing and Devices said which link it was, and the
     * reader walked between them for every change of radio. What the tab opens on is the
     * Status cards, because a client is connected for almost all of its life and how that
     * connection is doing is the thing worth a glance. The device list is one level in, behind
     * the Link card's "devices" button (`devices_open`), which is the one verb that card
     * offers whether or not anything is attached - see include/mesh/ui/status.h.
     */
    MESH_UI_SCREEN_RADIO,
    MESH_UI_SCREEN_SETTINGS,
    MESH_UI_SCREEN_COUNT,
};

/*
 * The rows on the front of the Nodes list that are not nodes.
 *
 * The map row opens the map. A row rather than a keycap because the Nodes tab has already spent
 * A, X and Y on things a node row does, and because a way into a screen that only a button
 * nobody mentions can reach is a screen nobody finds - the argument the conversation list's
 * "New message" row and the Waypoints tab's "New waypoint here" row both make. Both of these
 * are at the *front* of the list for the one reason those two are at the back of theirs: this
 * list can be a hundred and twenty-eight rows long, and a control at the bottom of that is a
 * control that is not there.
 *
 * The filter row is the control that says which of the roster is below it
 * (include/mesh/ui/nodes.h), and it is above the map row rather than below it because a control
 * belongs above the thing it changes. It costs a row on every Nodes list, which is the honest
 * price of a control the reader can see rather than a keycap they have to be told about - and
 * the same trade the map row made first.
 *
 * Both are edited with Left and Right, which mesh_ui_nav_handle_key() takes before the tab
 * switch, and A steps either forward through mesh_ui_nav_confirm(). mesh_ui_nav_node_at_row() is
 * the one place that knows how many rows to subtract, so the four presses this list offers cannot
 * disagree about which node row 7 is about.
 */
#define MESH_UI_NODES_FILTER_ROW 0U
#define MESH_UI_NODES_SORT_ROW 1U
/* Find: a piece of a name, typed. A opens the keyboard on it and X clears it; Left and Right
   are the tabs here, since the row edits nothing in place - see `node_query`. */
#define MESH_UI_NODES_FIND_ROW 2U
/* The Find text's buffer, NUL included: longer than any short name and most long ones. */
#define MESH_UI_NODE_QUERY_MAX 24U
#define MESH_UI_NODES_MAP_ROW 3U
/* The places list, one level in (`waypoints_open`). Under the map row because it is the same
   argument - a way into a screen is a row somebody can see - and because the two are the two
   halves of "where things are". */
#define MESH_UI_NODES_WAYPOINTS_ROW 4U
/* Rows before the first node. Written once so a third one cannot be added to only some of the
   arithmetic - which is exactly how the map row's own arrival went wrong before it was, and
   what made the sort row's and the find row's arrivals a constant and a row id rather than an
   audit. */
#define MESH_UI_NODES_LEAD_ROWS 5U

#define MESH_UI_NAV_TARGET_NAME_MAX 40U
/* nav.settings_section when the Settings tab shows the section list rather than a section. */
#define MESH_UI_SETTINGS_NO_SECTION 0xFFU

/* nav.radio_page: which page, if any, the Radio tab has open over its cards. */
enum mesh_ui_radio_page {
    MESH_UI_RADIO_PAGE_NONE = 0,
    MESH_UI_RADIO_PAGE_DETAILS,    /* the Radio card's: MESH_UI_SETTINGS_RADIO_DETAILS */
    MESH_UI_RADIO_PAGE_NODE_LISTS, /* the Mesh card's: MESH_UI_SETTINGS_NODE_LISTS */
};
/* nav.settings_channel when the Channels section shows its list rather than one channel. */
#define MESH_UI_SETTINGS_NO_CHANNEL 0xFFU
/* The snackbar's limits are inkstand's nav/toast.h's; these names are what this client's callers
   size a notice buffer by. */
#define MESH_UI_NAV_TOAST_MAX INKSTAND_TOAST_TEXT_MAX
#define MESH_UI_NAV_TOAST_QUEUE INKSTAND_TOAST_QUEUE
#define MESH_UI_CANNED_MAX 16U
#define MESH_UI_CANNED_TEXT_MAX 64U
/* Upstream Data.payload caps at 233 bytes; the draft and action text hold that plus a NUL. */
#define MESH_UI_DRAFT_MAX 234U
/* Unsent drafts kept for conversations other than the open one. See `parked_drafts`. */
#define MESH_UI_PARKED_DRAFTS 4U
/*
 * Pending Settings edits held until Save. A section with nineteen editable rows has to be able
 * to carry nineteen edits: below that, mesh_ui_nav_edit_set() returns false and the press
 * silently does nothing, which is a mistake nothing on the frame reports.
 *
 * Position is the section that sets it - six settings, three coordinate rows and the ten bits
 * of what a position packet carries - and the number is not read off that section by hand:
 * settings_sections_fit_the_edit_list walks every section, counts what it offers, and fails
 * naming the one that outgrew this. Raise it there or not at all.
 */
#define MESH_UI_SETTINGS_EDITS_MAX 24U

/*
 * The longest TEXT or KEY value a field will take, plus its NUL.
 *
 * Measured from the field table rather than declared beside it. A union is as wide as its
 * widest member and no wider, and every member here is one field's bytes and its NUL, so this
 * is exactly the widest field in mesh/ui/settings_text.def - which is where a field's limit is
 * written, and which the field table's own rows read the same number out of.
 *
 * It was a constant, and the constant was raised twice (72 for DeviceConfig.tzdef, then 80 for
 * StatusMessageConfig.node_status) because a value too long for the buffer is truncated by
 * mesh_ui_nav_settings_commit_text() rather than refused: the radio would have taken the whole
 * string and is sent part of one, with nothing on the frame saying so. A number that has to be
 * raised by hand is a number that is raised one field too late, so the third field to outgrow
 * it - mesh beacon's 100-byte broadcast_message - widens the buffer by being listed instead.
 *
 * The member names are the field enumerators without their prefix. Nothing reads them; what is
 * wanted is the sizeof, and naming them after their fields is what makes a debugger's view of
 * the union say which field each width belongs to.
 */
union mesh_ui_setting_text_widest {
#define MESH_UI_TEXT_FIELD(name, bytes) char name[(bytes) + 1U];
#include "mesh/ui/settings_text.def"
#undef MESH_UI_TEXT_FIELD
};
#define MESH_UI_SETTING_TEXT_MAX (sizeof(union mesh_ui_setting_text_widest))

/*
 * Which press writes an edit. Position and LoRa are the two sections with more than one: Y writes
 * PositionConfig with a set_config, and "Set fixed position" writes the coordinate rows with
 * set_fixed_position, because the firmware only takes them that way. Every other field belongs
 * to its section's own save. An edit has to say which press owns it so that neither one clears
 * the other's pending work off the screen when it fires.
 * mesh_ui_settings_field_consumer() (settings.h) answers it for a field.
 */
enum mesh_ui_setting_consumer {
    MESH_UI_SETTING_CONSUMER_SECTION = 0,
    MESH_UI_SETTING_CONSUMER_FIXED_POSITION,
    /* LoRa's second press: "Switch to ham mode" reads the call sign, frequency and power rows
       and sends set_ham_mode, while Y beside them still writes LoRaConfig. */
    MESH_UI_SETTING_CONSUMER_HAM_MODE,
};

/* One edited setting. `field` is an enum mesh_ui_setting_field (settings.h); NONE marks an
   empty slot. Toggles and enums use `number`, numbers use `number`, text uses `text`. */
struct mesh_ui_setting_edit {
    uint16_t field;
    uint32_t number;
    char text[MESH_UI_SETTING_TEXT_MAX];
};

/*
 * The on-screen keyboard's grid, its panel ring and its edits moved to inkcell
 * (inkcell/ui/keyboard.h): none of it was ever about Meshtastic, and the second client built on
 * this toolkit started by copying it. What stayed here is the half with the radio in it - which
 * job the keyboard was opened for, what the text is worth when it is finished, and what it goes
 * back to. See mesh_ui_nav_kb_layout() below for the seam.
 */

/* What opens over the thread once the picker has named one. The picker itself does not care
   which; it is the key that opened it that decides - A wants the quick replies, Y wants to
   type. */
enum mesh_ui_picker_follow {
    MESH_UI_PICKER_FOLLOW_NONE = 0,
    MESH_UI_PICKER_FOLLOW_QUICK,    /* the compose overlay: canned replies */
    MESH_UI_PICKER_FOLLOW_KEYBOARD, /* straight into typing */
};

/*
 * Everything a backend needs to draw a cursor, the compose target and the keyboard. Lives in
 * the store and is copied into each snapshot, so backends stay stateless.
 *
 * The conversation model, in the shape the Settings tab already uses: the Messages tab is two
 * levels. With `thread_open` clear it lists conversations (all traffic, each enabled channel,
 * each node we have direct messages with, then "New message"); with it set it shows the one
 * conversation named by `target_node` (MESH_MESSAGE_BROADCAST_ADDR means the channel
 * `target_channel`) or, when `inbox` is set, every message. Compose is an overlay over the open
 * thread and always sends there.
 *
 * The invariant that keeps this predictable: *only* opening a thread moves the target. The
 * Nodes tab opens the node's thread rather than retargeting whatever Messages was showing.
 */
struct mesh_ui_nav {
    enum mesh_ui_screen screen;
    /* One row cursor per tab. The Status entry is unused and stays 0: that screen has no rows,
       and what its cursor holds is `status_verb` below. */
    uint32_t cursor[MESH_UI_SCREEN_COUNT];
    uint32_t target_node;
    uint8_t target_channel;
    /* Messages tab: a thread is open (cursor[MESSAGES] indexes its messages) rather than the
       conversation list, whose position is parked in conversation_list_cursor meanwhile. */
    bool thread_open;
    uint32_t conversation_list_cursor;
    /* X on the conversation list is armed by one press and deletes on the second, the same way
       Y on the Devices tab and the node detail's remove row are: a conversation is history the
       radio cannot give back, so a press that lands on it by accident should cost nothing.
       The arming names the *conversation* rather than the row it was made on, because the
       direct peers are ordered by recency and one message from somebody else re-ranks them
       under the cursor - a row index armed a moment ago can be a different conversation by the
       time the second press lands. */
    bool messages_delete_armed;
    uint8_t messages_delete_kind; /* enum mesh_ui_conversation_kind */
    uint8_t messages_delete_channel;
    uint32_t messages_delete_node;
    /*
     * Where the reader had got to when they opened this thread: the packet id their read mark
     * named, or 0 when there was nothing to remember. The transcript rules a line under it.
     *
     * Recorded by the press that opened the thread, for the reason a reply's target is - the
     * store marks the open conversation read on the very next publish, so by the time a frame
     * is drawn the mark itself says "all of it", and a divider derived from the live mark would
     * sit under the newest bubble every time. This is the one copy of where the user *was*, and
     * it deliberately does not move while they are in there: a message arriving into an open
     * thread lands below the line rather than moving it.
     */
    uint32_t thread_unread_from;
    /* The open thread is the all-traffic one; meaningless unless thread_open. */
    bool inbox;
    char target_name[MESH_UI_NAV_TARGET_NAME_MAX];
    /* The snackbar: the one-line transient notice ("Sent to ABCD", "Connecting..."), how long it
       stands, and what is waiting behind it. See inkstand's nav/toast.h. */
    struct inkstand_toast toast;
    /* Filtered message count at the last clamp, so a cursor parked on the newest message
       follows new traffic instead of being left behind. */
    uint32_t messages_seen;
    /* Compose overlay over the open thread: the draft row, then the canned replies. A opens
       it; Y skips it and goes straight to the keyboard, which is why `keyboard_open` can be
       set with this one clear. */
    bool compose_open;
    uint32_t compose_cursor;
    /*
     * The message whatever is being written answers, 0 for a new one - the packet id that ends
     * up in Data.reply_id.
     *
     * It is set by the press that opened the overlay rather than read off the cursor when the
     * send happens, and that is the whole point: A on a bubble is "reply to *this*", and the
     * transcript underneath keeps moving - a message arriving while the user is picking a
     * canned line would otherwise re-aim the reply at whatever the cursor had slid onto. Y is
     * the other half of the same decision and clears it: the bar says "write", and a new
     * message to the conversation is not an answer to the last thing said in it.
     */
    uint32_t reply_to;
    /*
     * The sheet over the open thread that holds the verbs about one bubble: the fixed emoji
     * set, each sent as a reaction about `reply_to`, and the delete on the end.
     *
     * Its own overlay rather than a row in the compose sheet because none of it is a message -
     * it never opens the keyboard and it never takes the draft. It is where the delete lives
     * because X in a thread already means "the bubble under the cursor", and the thread's four
     * face buttons are already spoken for; a fifth verb on a fourth key would have been a
     * keycap that does nothing on most rows.
     */
    bool reaction_open;
    uint32_t reaction_cursor;
    /* The delete row has been pressed once. A second press on it carries the delete out, and
       anything else stands it back down - the conversation list's X does exactly this, and a
       press that throws messages away should cost the same two presses wherever it is. */
    bool message_delete_armed;
    /*
     * A retry has been raised and no other press has happened since, so START is spent.
     *
     * This is what makes one press one send, and it has to live here because the nav is the
     * only thing that can. A button going *down* and the kernel's autorepeat arrive as the
     * same event - inkcell_input_handle_event() drops `value == 2` only for the four
     * directions, because those are repeated by our own timer instead, and a face button keeps
     * whatever the kernel does with it. Every other press in this client either changes what
     * is on screen or arms something, so a repeat lands somewhere different; a resend leaves
     * the cursor on the same failed bubble, and a held START would put the same words on the
     * air thirty times a second until the store caught up. That is airtime on a shared band,
     * and a DM asks for an ack, so each one costs the mesh retransmits too.
     *
     * Spent rather than a packet id, because a bubble restored from the card may have no id to
     * name and two of them would then be one latch. Any press that is not START stands it back
     * down, which is the conversation list's delete arming turned around: there a second press
     * of the *same* key carries the thing out, and here a second press of the same key is the
     * one thing that must not.
     */
    bool resend_spent;
    /* "Send to" picker: every enabled channel, then every node. Picking opens that
       conversation's thread, and `picker_follow` says what opens over it. */
    bool picker_open;
    uint32_t picker_cursor;
    uint8_t picker_follow; /* enum mesh_ui_picker_follow */
    /* Free-text entry. */
    bool keyboard_open;
    /* Where the cursor is in the grid and which panel is showing. Four bytes and no pointer, so
       it travels in the snapshot with everything else here. */
    struct inkcell_keyboard kb;
    char draft[MESH_UI_DRAFT_MAX];
    /*
     * Unsent drafts that belong to conversations other than the open one.
     *
     * The draft is the text of *a* conversation, not of the keyboard. It used to be one buffer
     * that nothing cleared on a change of thread, and compose opens on a non-empty draft - so
     * words started to one peer were sitting ready in the next thread opened, one press from
     * going to somebody they were not written for. Opening a thread parks what was being
     * written here, keyed by the conversation it was for, and brings back whatever was parked
     * for the one being opened. A handful of slots, oldest overwritten: a draft is a thing
     * somebody is in the middle of, and nobody is in the middle of five.
     */
    struct {
        uint32_t node;
        uint8_t channel;
        uint32_t age; /* higher is more recent; 0 is an empty slot */
        char text[MESH_UI_DRAFT_MAX];
    } parked_drafts[MESH_UI_PARKED_DRAFTS];
    uint32_t parked_draft_clock;
    /* Nodes tab: a node's detail is open (cursor[NODES] indexes its rows) rather than the node
       list, whose position is parked in node_list_cursor meanwhile. The same two-level shape
       as Settings and Messages. Opening a detail does *not* move the compose target; only the
       detail's "Message this node" row does. */
    bool node_detail_open;
    uint32_t node_detail_node; /* the open node's id: the list is re-ranked under us */
    uint32_t node_list_cursor;
    /*
     * Nodes tab: the node's verbs, over its detail.
     *
     * A level rather than an overlay, on the terms the share sheet one tab over is on: it is
     * raised by a row, it leaves by B, and what it draws is a list of its own with a cursor of
     * its own. The cursor is separate from cursor[NODES] for the reason node_list_cursor is
     * separate from it too - the detail's place has to survive going in and coming back, or a
     * reader who opened the verbs from halfway down a node returns to the top of it.
     *
     * Why the verbs are a screen at all is in include/mesh/ui/node_detail.h, on the action that
     * opens them: the detail used to lead with thirteen of them and a reader who pressed A on a
     * node to see what it was met a menu instead.
     *
     * It is a row index here and not the `enum mesh_ui_node_action` the reading and the node are
     * held as elsewhere, and that is safe for the reason those are not: this list is rebuilt from
     * one node's own state and nothing re-ranks it under the reader. What *can* change it is a
     * verb's own gate - a key arriving adds three rows - so the cursor is clamped on every frame
     * like every other list's, rather than trusted across a publish.
     */
    bool node_actions_open;
    uint32_t node_actions_cursor;
    /*
     * Nodes tab: which of the roster the list is showing - `enum mesh_ui_node_filter`, stepped
     * by A on the list's own first row.
     *
     * A filter rather than a sort, and the reasoning is in include/mesh/ui/nodes.h; the Find
     * row's text is `node_query`, beside it.
     * What belongs here is why it is on the nav at all: it decides how many rows the screen has,
     * so mesh_ui_nav_row_count() has to read it, and everything that turns a row into a node has
     * to read it too or the cursor and the list part company on the first press.
     *
     * It survives leaving the tab, like every other level flag here - a reader who narrowed to
     * Pinned and went to look at a message comes back to the list they left. It does *not*
     * survive a restart: it is a lens on a roster that has changed while the client was off, and
     * a client that opened on an empty Nodes tab because of a chip pressed last week would be a
     * setting silently undoing the screen. That is the opposite call from the theme and from the
     * chart's span, and it is the same distinction: a span is how the reader likes charts read,
     * a filter is where they are standing right now.
     */
    uint8_t node_filter; /* enum mesh_ui_node_filter */
    /*
     * Nodes tab: the Find row's text - a piece of a name, a short name or an id, narrowing what
     * the filter kept. "" is no query. On the nav for the filter's reason, since it decides how
     * many rows there are, and like the filter it lasts until the reader clears it but not
     * past a restart.
     */
    char node_query[MESH_UI_NODE_QUERY_MAX];
    /*
     * Nodes tab: what order the rows the filter kept are in - `enum mesh_ui_node_sort`, stepped
     * by A on the row under the filter's.
     *
     * Beside the filter rather than folded into it because they are different axes: "which of
     * them" and "in what order", and a reader looking for the nearest pinned node wants both at
     * once. include/mesh/ui/nodes.h has why a sort exists at all when that header argues a
     * filter cannot be replaced by one.
     *
     * The same lifetime as the filter, and for the filter's reason rather than by copying it: a
     * sort is where the reader is standing, not how they like lists read. It survives leaving
     * the tab, so coming back from a message lands on the list they left; it does not survive a
     * restart, because a client that opened on a distance sort made against a fix it no longer
     * has would be a setting quietly reordering a screen nobody asked it to. Unlike the filter
     * it can never *empty* the list, which is why this is the weaker of the two arguments - and
     * why the two still land in the same place.
     */
    uint8_t node_sort; /* enum mesh_ui_node_sort */
    /* "Remove from radio" is armed by one press and acts on the second, the same way Y on the
       Devices tab is: it is the one node row that takes its own row away, so a press that
       lands on it by accident should cost nothing. Any other press stands it down. */
    bool node_remove_armed;
    /*
     * Nodes tab: a chart of one of the open node's readings, over its detail.
     *
     * A reading rather than a flag, because this tab's chart is not the Status tab's. There is
     * one airtime trend and it is the radio we are attached to, so `trend_open` is a yes or no;
     * a node has three readings worth watching and the press that opened this one named which -
     * so what the nav holds is the reading, and MESH_UI_HISTORY_NONE is the closed state. That
     * is also what lets the route tell two of these apart: a temperature chart and a humidity
     * chart on one node are two places, not one repainted, and a slide between them would be a
     * lie either way round if the nav could not say which is up.
     *
     * It is deliberately *not* a row index, for the reason `node_detail_node` is not one: the
     * rows a node produces depend on what it has reported, so a reading arriving or lapsing
     * re-numbers them under the reader. The reading survives that; a row number quietly becomes
     * a different row.
     *
     * Like map_open and trend_open it outlives a change of tab, so anything reading it checks
     * `screen` and `node_detail_open` as well.
     */
    uint8_t node_trend; /* enum mesh_ui_history_reading */
    /*
     * Nodes tab: the map, opened over the node list.
     *
     * A level of the Nodes tab rather than a tab of its own, which is what the shape of the thing
     * wants: a map is a second way of reading the roster, not a seventh place to be. A node's
     * detail can be opened *over* it - the map is then one level deeper than the list and the
     * detail is one deeper again - so backing out of a node opened from the map lands on the map
     * rather than on the list it was never on.
     *
     * The viewport is the whole of the map's state and it is here rather than in a backend for
     * the reason every other cursor is: a press moves it, and presses arrive at the nav. Its
     * pixel box is the *declared* one (MESH_UI_MAP_FIT_WIDTH), not any panel's - a backend
     * resizes its own copy to the body it actually has. The two never need to agree, because
     * the only thing the nav's box decides is which zoom a fit picks, and a box smaller than
     * every real body can only ever leave extra air around the edge.
     *
     * There is deliberately no selection field. What A opens is the marker nearest the middle of
     * the view, derived on every frame by mesh_ui_map_selected() from the viewport alone - so the
     * ring a backend draws and the node a press opens are one decision made twice rather than
     * two that agree until they do not. It is the top app bar's back arrow again, and the
     * transition route again: a second opinion about the nav is a second opinion that can be
     * wrong.
     */
    bool map_open;
    struct mesh_map_viewport map_viewport;
    /*
     * Radio tab, the Status cards: which verb the cursor is on.
     *
     * **This is the Status cursor, and cursor[MESH_UI_SCREEN_RADIO] is not** - that one is the
     * device list's, one level in (`devices_open`). The cards are the one screen with no rows: its
     * cards offer verbs, Up and Down walk those, and what the cursor holds is `enum
     * mesh_ui_status_verb` rather than a position in the list of them. The difference is what a
     * verb appearing or disappearing does. As an index it moved the cursor's meaning without moving
     * the cursor, so the list had to be append-only and every verb had to restate the conditions of
     * the verbs before it; as a verb the list is free to be written in the order the cards draw,
     * and a link that drops and comes back leaves the reader on the button they were standing on.
     *
     * MESH_UI_STATUS_VERB_COUNT is "no verb", which is what a screen offering none holds. The
     * value is otherwise only ever one mesh_ui_status_verb_resolve() answered with, so nothing
     * reads it without asking the list on offer whether it is still there - see
     * include/mesh/ui/status.h.
     */
    uint8_t status_verb;
    /*
     * Radio tab: the airtime chart is open over the Status cards.
     *
     * The one level this tab has, and it carries no cursor of its own - a chart is a picture and
     * there is nothing on it to choose between, so `status_verb` stays where it was and is still
     * naming the trend verb when B lands back on the cards. That is why the flag exists at all
     * rather than the screen being a fourth card: the cards are a list of verbs the cursor
     * walks, and a picture is not a verb.
     *
     * It outlives a change of tab, as map_open does and for the same reason - every tab keeps
     * its own place - which means it says *where the Radio tab is standing* rather than *what
     * is on the panel*. Anything reading it has to check `screen` as well, or a press meant for
     * the Nodes list closes a chart nobody can see.
     */
    bool trend_open;
    /*
     * Radio tab: the device list is open over the Status cards.
     *
     * The tab's other level, opened by the Link card's "devices" verb and closed by B, and never
     * up at the same time as `trend_open` - both are opened from the cards and each is left
     * back to them. It carries its cursor in `cursor[MESH_UI_SCREEN_RADIO]`, which the cards
     * never use (theirs is `status_verb`), so the list keeps the reader's row across a visit to
     * the cards and back, as every other list does.
     *
     * Like `trend_open` it outlives a change of tab, so ask mesh_ui_nav_devices_showing() rather
     * than reading it: that is the flag and the screen together.
     */
    bool devices_open;
    /*
     * Radio tab: a page of rows open over the Status cards - the Radio card's details or the
     * Mesh card's node lists - as an enum mesh_ui_radio_page, NONE on the cards.
     *
     * Each page is built from a settings section (mesh_ui_nav_radio_page_section()) because that
     * is what the pages are: facts and verbs under headings, with the confirm sheet in front of
     * the costly ones, which is the settings row model exactly. So everything that answers for
     * "the open section" - the rows, the A press, the sheet, the bar, help - asks
     * mesh_ui_nav_open_section() and serves both tabs. The field is a page rather than the
     * section itself so that a zeroed nav stands on the cards, as it does for every other flag
     * here: section 0 is About, and a nav nobody had initialised would otherwise open it.
     *
     * The third of the tab's levels, and like the other two opened from the cards and left back
     * to them, never up with either. Its rows use `cursor[MESH_UI_SCREEN_RADIO]`, which the
     * device list does too; the two never show at once, so a page parks the list's row below
     * and puts it back on the way out.
     */
    uint8_t radio_page;
    /* The device list's row, parked while a page has `cursor[MESH_UI_SCREEN_RADIO]`. */
    uint32_t radio_devices_cursor;
    /*
     * How far back both charts look: `enum inkcell_trend_span`, stepped by Left and Right.
     *
     * One field for the two chart screens rather than one each, and that is a claim about what
     * this is. A span is not *where the reader is* - which is what every other field on this
     * struct records, one per tab, so that every tab keeps its own place - it is how they like
     * their charts read, the same way the theme is not a place. Two fields would mean opening a
     * node's temperature at the quarter hour and finding the airtime chart still on all of it,
     * which is the client having two opinions about one preference.
     *
     * It survives a chart being closed and reopened for the same reason, and it is deliberately
     * not persisted: the history it slices is not persisted either, so a span restored across a
     * restart would be a choice made about readings that no longer exist.
     *
     * INKCELL_TREND_SPAN_ALL rather than zero at rest - mesh_ui_nav_init() says so - because ALL
     * is what these screens did before there was a picker, and a reader who never touches Left
     * or Right should see what the screen has always shown them.
     */
    uint8_t trend_span;
    /*
     * A node's chart is showing its readings as a list rather than as a plot, and how far down
     * that list the window has been scrolled.
     *
     * Two faces of one screen rather than a screen of its own, which is what makes Y the press
     * and B still the way out: the reader is looking at the same reading over the same window,
     * and what changes is whether they are reading a direction or a figure. A level of its own
     * would have meant backing out of the list into the plot and out of the plot into the row,
     * which is two presses to leave one thing.
     *
     * `trend_table` sits beside `trend_span` and is the same kind of field: not *where the reader
     * is* - every other field on this struct is one per tab, so that each tab keeps its place -
     * but how they like a chart read. So it survives the chart being closed and reopened, and is
     * deliberately not persisted, for the reason the span is not: the readings it lists do not
     * survive a restart either.
     *
     * `trend_scroll` is a place, and is reset whenever the list is opened or the span changes -
     * a span is a different set of readings, and holding a row number across that would land the
     * reader somewhere they did not choose. It is the *top* row rather than a cursor, because
     * nothing in this list is pressable: there is no row to highlight, only a window to move.
     * Held in range by mesh_ui_nav_clamp() against the readings actually in the span and the body
     * rows the backend last reported - see `page_rows` on struct mesh_ui_store.
     *
     * **The airtime chart has neither.** Six hours at a reading a minute is 360 rows nobody will
     * scroll, and it is binned into columns precisely because reading by reading is the wrong
     * grain for it - see the readings section of mesh/ui/trend.h. The fields are on the nav
     * rather than per-tab all the same, because there is one chart screen and one set of things
     * a chart can be doing.
     */
    bool trend_table;
    uint32_t trend_scroll;
    /*
     * Nodes tab, the places: a place's detail is open (cursor[NODES] indexes its rows) rather
     * than whatever it was opened from - the places list, or the map - whose position is parked
     * in waypoint_list_cursor meanwhile. The same two-level shape as a node's detail.
     *
     * The open place is named by id rather than by row for the reason the node detail is: the
     * list is ordered by distance from our own fix, so a fix arriving re-ranks it under the
     * cursor. mesh_ui_nav_clamp() closes the detail when the place leaves the list, which is
     * what a withdrawal from the mesh looks like from here.
     */
    bool waypoint_detail_open;
    uint32_t waypoint_detail_id;
    uint32_t waypoint_list_cursor;
    /* The detail's delete row is armed by one press and acts on the second, the same way the
       node detail's remove row is: it takes the place off the mesh for everybody, and a press
       that lands on it by accident should cost nothing. */
    bool waypoint_delete_armed;
    /*
     * Nodes tab: the places list is open over the roster, opened from its row. cursor[NODES]
     * walks the places while it is up; the roster's own position is parked in
     * `waypoints_nodes_cursor` and put back by B, the node detail's arrangement one level
     * sideways. Like map_open it outlives a change of tab, so ask
     * mesh_ui_nav_waypoints_showing() rather than reading it.
     */
    bool waypoints_open;
    uint32_t waypoints_nodes_cursor;
    /* Settings tab: the open section (enum mesh_ui_settings_section) or NO_SECTION for the
       section list. cursor[SETTINGS] indexes whichever list is showing; the section list's
       position is parked here while a section is open. */
    uint8_t settings_section;
    uint32_t settings_list_cursor;
    /* Which list B goes back to from the open section: NO_SECTION for the top-level list,
       MESH_UI_SETTINGS_MODULES for a module opened from the Modules list. The Modules list's
       own position is parked here while one of its sections is open, the same way the channel
       list's is. */
    uint8_t settings_parent;
    uint32_t settings_module_list_cursor;
    /* Channels section: the open channel slot or NO_CHANNEL for the channel list, whose
       position is parked in settings_channel_list_cursor while a channel is open. */
    uint8_t settings_channel;
    uint32_t settings_channel_list_cursor;
    /* The confirm overlay: "Save <section>?" for sections whose write can cut this client off
       (Bluetooth, Channels, LoRa, Security, Power), and every row in the Radio actions
       section. See inkstand's nav/dialog.h. `confirm.subject` is an enum mesh_ui_settings_action
       and says which of the two it is standing in front of: MESH_UI_SETTINGS_ACTION_NONE is the
       section save, anything else is that radio action. */
    struct inkstand_dialog confirm;
    /* Edits made in the open section and not yet saved. Y sends them as one
       MESH_UI_ACTION_SAVE_SETTINGS; B asks once (discard_armed) and discards on the second
       press. The app clears them through mesh_ui_store_settings_edits_clear() once queued. */
    struct mesh_ui_setting_edit settings_edits[MESH_UI_SETTINGS_EDITS_MAX];
    uint8_t settings_edit_count;
    bool settings_discard_armed;
    /*
     * When the keyboard names a new waypoint rather than editing a setting or writing a
     * message. A flag of its own rather than a fourth value of `keyboard_field`, which is an
     * enum mesh_ui_setting_field and has no member that means "not a setting at all" other
     * than NONE - the value a message keyboard already carries.
     *
     * `waypoint_source_node` is whose fix the new place takes: 0 is our own radio, anything
     * else is the node a "Save this place" row was pressed on. The coordinate itself is
     * deliberately not held here - the nav has no business carrying one, and the app resolves
     * it from the session roster, which is more current and twice the size of the published one.
     */
    bool keyboard_waypoint;
    uint32_t waypoint_source_node;
    /*
     * When the keyboard is typing the network radio's address, from the Devices tab's last row.
     *
     * A fourth flavour rather than a settings field, because it is not one: `k_fields` is what
     * the radio holds, and this is what *this client* connects with - it is written to the
     * preferences file next to the theme, and a radio that has never been reached has no
     * setting to edit. The parking slot and the restore are shared with the other three; what
     * differs is where the keyboard came from and therefore where B lands.
     */
    bool keyboard_network;
    /*
     * When the keyboard is typing a Meshtastic channel link, from the Channels list's import
     * row.
     *
     * A fifth flavour beside the network address, and a flavour for the same reason that one
     * is: `k_fields` is what the radio holds, and this is not a field on it - it is a whole
     * channel set on its way in, which the app turns into as many writes as it takes. It is
     * also the one keyboard here whose text is *checked* before the keyboard closes: a link
     * that does not parse leaves the user on the keyboard with what they typed, because the
     * alternative is a screen full of base64 thrown away over one wrong character.
     */
    bool keyboard_channel_url;
    /*
     * When the keyboard is typing a Meshtastic *contact* link, from the User list's add row.
     *
     * A sixth flavour, beside the channel link and for its reasons: it is not a field the radio
     * holds, and it is checked before the keyboard closes rather than after, because a link
     * that does not parse must leave the user on the keyboard with what they typed. The two
     * link flavours stay separate flags rather than one with a kind beside it, because what
     * differs is not only the parser but where B lands and which sheet comes up.
     */
    bool keyboard_contact_url;
    /* When the keyboard is typing the Nodes list's Find text: a seventh flavour, and the one
       whose text never leaves the client - Done narrows the list, and that is all. */
    bool keyboard_node_query;
    /* When the keyboard edits a setting rather than the Compose draft: the field it is for
       (NONE for Compose) and the Compose draft parked while it is open. */
    uint16_t keyboard_field;
    char draft_saved[MESH_UI_DRAFT_MAX];
    /* BLE pairing prompt. The keyboard is retargeted for it the same way a setting's text
       retargets it, except that this one is opened by the app rather than by a key press:
       BlueZ asks for the PIN somewhere in the middle of a connect and blocks until it is
       answered. `pairing_confirm` marks the numeric-comparison case, where the digits are
       pre-filled and Send means "yes, that is what the node is showing". */
    bool keyboard_passkey;
    bool pairing_confirm;
    char pairing_label[MESH_UI_NAV_TARGET_NAME_MAX];
    /*
     * The keyboard collecting a key-verification security number: a fifth flavour beside the
     * pairing PIN, and there for the same reason that one is a flavour rather than a screen.
     * It is opened by the *radio* asking a question in the middle of whatever the user was
     * doing, so it takes over the keyboard and parks what was there, and the digits it collects
     * answer a nonce rather than a field.
     *
     * It carries no label of its own, unlike the pairing prompt. The name to put on it is in
     * the snapshot already - `verification.remote_name`, which is the name the far radio used
     * and the one the other person is looking at - and a copy here would be a second opinion
     * about who is being verified.
     */
    bool keyboard_verify;
    /* The keyboard the prompt displaced, restored when it closes. The prompt can land on top
       of an open keyboard, and the text being typed is parked in `draft_saved` like any other.
       `keyboard_displaced` is what says one was open at all: `keyboard_field_displaced` cannot,
       because NONE is what a message keyboard reads as. (A settings keyboard that had itself
       parked a compose draft loses that one: there is a single parking slot, and the text in
       front of the user is the one worth keeping.) */
    bool keyboard_displaced;
    uint16_t keyboard_field_displaced;
    /*
     * The help screen over whatever is under it: what this screen is for, and what its rows
     * mean (src/ui/tables/help.c, docs/help.md).
     *
     * A level rather than an overlay, and that is the whole of its interaction model: nothing
     * is stacked on top of the screen it explains, so nothing has to be restored when it
     * closes. `help_cursor` scrolls it and does nothing else - there is no row here to press,
     * because a paragraph is not a control.
     *
     * It has a cursor of its own instead of borrowing cursor[screen], so the section
     * underneath keeps its place: B out of help lands back on the row the question was asked
     * about, which is the only landing that makes the press worth making twice.
     */
    bool help_open;
    uint32_t help_cursor;
    /*
     * The key-verification sheet: the radio's half of the ceremony, put to the user.
     *
     * An overlay like the confirm dialog, and opened like the pairing prompt - by the app,
     * because the thing that raises it is a ClientNotification rather than a press. `verify_cursor`
     * is the dialog's own 0-is-accept, 1-is-cancel, the same one `confirm.cursor` carries.
     *
     * What it *says* is not here: the stage, the digits and the characters are in the snapshot
     * (struct mesh_ui_verification), because they are what the radio is doing rather than where
     * the user is. This flag is only whether the question is on screen - which matters on its
     * own, because "Later" closes the sheet without ending the exchange.
     */
    bool verify_open;
    uint8_t verify_cursor;
    /*
     * The share sheet: this radio's channel set as a QR code, over the Channels list.
     *
     * A screen rather than a dialog, and not one of the tabs' own levels: it is raised by a row
     * and leaves by B, exactly as help does, and what it draws is a picture rather than a list
     * anything can be chosen from. It carries no cursor for that reason - there is nothing on
     * it to move between.
     */
    bool share_open;
    /*
     * The contact code sheet: this radio's own identity as a QR code, over the User list.
     *
     * Its own flag beside `share_open` rather than a kind on one of them. They are raised from
     * different sections, they clamp on different fields of the store, and only one can be open
     * at a time by construction - a row of one list cannot be pressed while the other's screen
     * is up - so a shared flag would buy nothing and cost every reader a second question.
     */
    bool contact_open;
    /*
     * A channel link that has been typed and parsed, waiting on the sheet in front of it.
     *
     * Its own buffer rather than the draft, because the keyboard closes before the sheet opens
     * and closing it is what puts the parked Compose draft back. Bounded by the draft rather
     * than by the link format: what can be typed is what fits in the keyboard's own buffer, and
     * a link longer than that is one nobody was going to type anyway.
     */
    char channel_url[MESH_UI_DRAFT_MAX];
    /* A contact link that has been typed and parsed, waiting on the sheet in front of it.
       Its own buffer beside the channel one, for that buffer's reason: the keyboard closes
       before the sheet opens, and closing it is what puts the parked Compose draft back. */
    char contact_url[MESH_UI_DRAFT_MAX];
    /* Device list: Y is armed by one press and forgets the node on the second, because a
       bond dropped by accident costs the user a re-pair with the PIN. */
    bool devices_forget_armed;
    uint32_t devices_forget_row;
    /*
     * A window's right-click menu over the row under the cursor: that row's verbs, at the
     * pointer. The verbs are not held here - the frame reads them off the action table for the
     * row the cursor is on, the same answer the action bar gives - so what is kept is only that
     * it is up and where the click was, in the frame's pixels, for the menu to hang from.
     *
     * Any key puts it down and does nothing else, and so does a click anywhere but one of its
     * verbs: a menu is dismissed by looking away from it, not answered.
     */
    bool context_open;
    int32_t context_x;
    int32_t context_y;
};

enum mesh_ui_action_type {
    MESH_UI_ACTION_NONE = 0,
    MESH_UI_ACTION_CONNECT, /* identifier = BLE address */
    /* dest/channel/text, plus reply_id/is_reaction: a new message, a threaded reply, or a
       tapback. One type rather than three because the destination, the text and the failure
       path are the same three things in all of them. */
    MESH_UI_ACTION_SEND_TEXT,
    /*
     * One message that came back undelivered, sent again: `dest`, `channel`, `text` and
     * `reply_id` are copied off the failed bubble and `number` is the packet id that failed.
     *
     * Its own verb rather than a SEND_TEXT the nav happens to have pre-filled, and the
     * difference is not the wire - the packet is identical. It is that this one names a prior
     * attempt, which is the one thing nothing else in this list does: the app says "resent"
     * rather than "sent", and the log line can name the id that went unanswered. A press that
     * silently became an ordinary send would leave the retry indistinguishable, in the toast
     * and in the log, from the user typing the same words out a second time.
     *
     * The failed bubble is deliberately left where it is. The transcript is a record of what
     * happened on the air, and the attempt that failed is part of it - the same reason a
     * reaction stays in the log. What the user gets is a second bubble that goes out, not an
     * edit to the first.
     */
    MESH_UI_ACTION_RESEND,
    MESH_UI_ACTION_REFRESH_SETTINGS,  /* re-read the radio's configuration */
    MESH_UI_ACTION_SAVE_SETTINGS,     /* section + edits: write one section to the radio */
    MESH_UI_ACTION_TOGGLE_FAVORITE,   /* dest = node to pin/unpin; `number` is 1 to pin */
    MESH_UI_ACTION_TRACEROUTE,        /* dest = node to trace the route to */
    MESH_UI_ACTION_REQUEST_NODE_INFO, /* dest = node to ask for a NodeInfo */
    MESH_UI_ACTION_REQUEST_POSITION,  /* dest = node to ask for a fix now */
    MESH_UI_ACTION_REQUEST_TELEMETRY, /* dest = node to ask for a reading now */
    MESH_UI_ACTION_TOGGLE_IGNORE,     /* dest = node; `number` is 1 to start ignoring it */
    /* dest = node. Mute is a bare toggle rather than a wanted state, because the admin verb
       behind it (toggle_muted_node) offers nothing else. */
    MESH_UI_ACTION_TOGGLE_MUTE,
    MESH_UI_ACTION_REMOVE_NODE,
    /* Throws away one conversation's messages: `number` is the enum mesh_ui_conversation_kind,
       `dest` the peer for a direct one and `channel` the slot for a channel. The app owns it
       because the log lives in three places at once - the transport's ring, the history read
       back from the cache, and the store - and a delete that missed any of them would put the
       conversation back on the next publish. */
    MESH_UI_ACTION_DELETE_CONVERSATION,
    /*
     * Throws away one message: `number` is its packet id, `dest` and `channel` name the
     * conversation it sits in the way the delete above names one.
     *
     * The app owns it for the conversation delete's reason and one more of its own: a message
     * lives in four places once there is a transcript on the card, and the card's copy is the
     * one that outlives a restart. Purely local, like everything else here - Meshtastic has no
     * retraction, so this deletes our record of a message and nothing anybody else holds.
     *
     * A packet id rather than a row, because the four places store the log in four different
     * orders and an index into one of them names nothing in the others. A message with no id
     * therefore cannot be deleted - and cannot be reacted to either, so the sheet that offers
     * both never opens on one.
     */
    MESH_UI_ACTION_DELETE_MESSAGE,
    /*
     * Stops one conversation interrupting the user: `number` is the enum
     * mesh_ui_conversation_kind, `dest` the peer for a direct one and `channel` the slot for a
     * channel, exactly as the delete above names one.
     *
     * A bare toggle rather than a wanted state, for MESH_UI_ACTION_TOGGLE_MUTE's reason turned
     * around: there the wire verb offers nothing else, and here the *store* is the only thing
     * that knows the answer. The nav can see that a conversation is muted but not by which of
     * the two halves - its own flag, or the radio's per-node one - so a wanted state read here
     * would be the nav guessing at a question mesh_ui_store_conversation_muted() answers.
     */
    MESH_UI_ACTION_MUTE_CONVERSATION,
    /* About section: ask GitHub what the newest release is, and install the one a check
       found. Two actions rather than one because installing replaces the running binary. */
    MESH_UI_ACTION_CHECK_UPDATE,
    MESH_UI_ACTION_INSTALL_UPDATE,
    MESH_UI_ACTION_CYCLE_UPDATE_CHANNEL,
    MESH_UI_ACTION_TOGGLE_DEV_UPDATES,
    /* Steps the UI to the next theme and remembers it. The app owns the choice because the
       app owns the preferences file; the store finds out the same way every backend does,
       through the client info in the next snapshot. */
    MESH_UI_ACTION_CYCLE_THEME,
    MESH_UI_ACTION_CYCLE_LANGUAGE,
    /* Steps the text size and remembers it, on the theme's terms. */
    MESH_UI_ACTION_CYCLE_TEXT_SIZE,
    /* The compose sheet's draft, kept as a quick reply: `text`. The draft itself stays. */
    MESH_UI_ACTION_SAVE_QUICK_REPLY,
    /* Throw away the crash report a previous run left on the card. Purely local, like the theme
       and the language beside it - there is no radio behind About - and it is what the crash
       banner resolves by: a notice with nowhere to go is the one thing the banner table refuses
       to raise. */
    MESH_UI_ACTION_DISCARD_CRASH_REPORT,
    /* About radio: ask what firmware exists for the *radio*, which is a different binary on a
       different computer and a different pair of documents. Its own action rather than a flag
       on CHECK_UPDATE because the two can be in flight at once and fail separately, and
       because only one of them ever installs anything. */
    MESH_UI_ACTION_CHECK_RADIO_FIRMWARE,
    /* Steps the firmware channel and remembers it, as CYCLE_UPDATE_CHANNEL does for the
       client's own. Its own action for the same reason the check is: two projects. */
    MESH_UI_ACTION_CYCLE_FIRMWARE_CHANNEL,
    /*
     * Install what the check found. `number` is the bus - 0 for USB, 1 for Bluetooth - taken
     * from which of the two rows was confirmed rather than re-read from the snapshot, because
     * the sheet the user answered named a bus and a link that moved between the press and the
     * reply would otherwise have them agreeing to one thing and getting the other.
     *
     * Not a RADIO_ACTION: nothing here is an admin request with a read-back behind it. It is a
     * download, one verb, and then a conversation with a bootloader or a loader, neither of
     * which is a Meshtastic node.
     */
    MESH_UI_ACTION_INSTALL_RADIO_FIRMWARE,
    /* Radio actions section: `number` is the enum mesh_ui_settings_action the user confirmed.
       One action type rather than five because the nav has nothing to say about any of them
       beyond which row it was - the app owns what each one means. */
    MESH_UI_ACTION_RADIO_ACTION,
    /* The two rows in that section that ask *this client* to forget nodes rather than the
       radio: `number` is 1 to empty the roster, 0 to drop only what the radio's NodeDB no
       longer carries. Its own type rather than a sixth radio action because nothing goes over
       the air and it works with no link at all. */
    MESH_UI_ACTION_FORGET_NODES,
    /* Devices tab. DISCONNECT with an empty identifier means "whatever link is up": only one
       radio is ever connected, so the row the cursor happens to be on does not decide it. */
    MESH_UI_ACTION_DISCONNECT,
    MESH_UI_ACTION_FORGET, /* identifier = BLE address to unpair */
    /*
     * Broadcasts a place to the mesh. `text` is its name, `dest` is the node whose fix it takes
     * (0 means our own radio) and `number` is the id of an existing waypoint being re-shared,
     * or 0 for a new one.
     *
     * The coordinate is not here on purpose: the app resolves it from the session roster at the
     * moment it acts, so the place is where the node is *now* rather than where a published
     * snapshot said it was when the key was pressed.
     */
    MESH_UI_ACTION_SHARE_WAYPOINT,
    /* Withdraws one: `number` is the waypoint id. Whether that reaches the mesh or only this
       client is the session's decision, because it turns on `locked_to` and on our own node
       number - neither of which the nav holds. */
    MESH_UI_ACTION_FORGET_WAYPOINT,
    /* Answers the BlueZ pairing agent: `text` holds the digits typed into the prompt. */
    MESH_UI_ACTION_SUBMIT_PASSKEY,
    MESH_UI_ACTION_CANCEL_PAIRING,
    /*
     * Key trust (mesh/core/key_verification.h). Four verbs, and the split between them is the
     * one the ceremony itself makes: the first two are presses on a node, and the last two
     * answer a question the *radio* asked.
     */
    MESH_UI_ACTION_ADD_CONTACT, /* dest = the node to hand to the radio, key and all */
    MESH_UI_ACTION_VERIFY_KEY,  /* dest = the node to start a ceremony against */
    /* `text` holds the four digits the other person read out. No `dest`: the digits answer the
       nonce the radio is holding open, and naming a node here would be the nav having an
       opinion about which exchange is in front of the user. */
    MESH_UI_ACTION_VERIFY_NUMBER,
    /* The comparison, answered: `number` is 1 for "they match" and 0 for "they do not". */
    MESH_UI_ACTION_VERIFY_ANSWER,
    /*
     * Joins the channel set in a Meshtastic link: `text` is the link, exactly as it was typed
     * and after the sheet in front of it was answered.
     *
     * The link rather than a parsed set, and that is deliberate. A `ChannelSet` is eight
     * channels with their keys in it, which is far more than `struct mesh_ui_action` should
     * grow to carry, and the nav has no business holding protobuf: what it has is the
     * characters the user typed, which it has already checked parse. The app parses them again
     * against the radio's *current* table, which is the one that will be overwritten and which
     * may have moved since the press.
     */
    MESH_UI_ACTION_IMPORT_CHANNELS,
    /*
     * Adds the contact in a Meshtastic link to the radio's NodeDB: `text` is the link, exactly
     * as it was typed and after the sheet in front of it was answered.
     *
     * The link rather than a parsed contact, for MESH_UI_ACTION_IMPORT_CHANNELS's reason: a
     * `SharedContact` is a node number, a name and a 32-byte key, which the nav has no business
     * holding as protobuf. What it has is the characters the user typed, which it has already
     * checked parse; the app parses them again on the other side of the seam.
     *
     * Distinct from MESH_UI_ACTION_ADD_CONTACT above, which hands over a node this client
     * already holds and carries a `dest` rather than text. They queue the same admin verb and
     * arrive from opposite directions - one from a node's own row, one from a stranger's link -
     * and only this one can name a node that has never transmitted.
     */
    MESH_UI_ACTION_IMPORT_CONTACT,
    /*
     * Points the Settings tab at another node's radio, over the mesh: `dest` is the node, and 0
     * means come back to the one on the end of the link.
     *
     * One verb for both directions rather than a pair, because there is only one thing being
     * said - which radio the tab describes - and the two presses that say it arrive from
     * opposite ends of the client: a node's own row on the Nodes tab, and the row in About
     * radio that names whoever is currently being configured. A second verb would be the same
     * sentence written twice and a table row to keep in step.
     *
     * Not a SETTINGS action despite where half of it is pressed: it changes nothing on any
     * radio, sends no write, and what it moves is this client's idea of who it is talking to.
     */
    MESH_UI_ACTION_SET_ADMIN_TARGET,
    /* Not a verb: how many there are. It is what pins the dispatch table in
       src/app/app_actions.c to this list - a verb added above and not given a row there is a
       press that reaches the app and does nothing, with nothing to see at the seam. */
    MESH_UI_ACTION_COUNT,
};

struct mesh_ui_action {
    enum mesh_ui_action_type type;
    char identifier[64];
    /* CONNECT: which transport the picked row belongs to (enum mesh_ui_device_kind). The
       Devices tab lists BLE advertisers and USB ports together, so the app cannot infer it
       from the identifier. */
    uint8_t kind;
    uint32_t dest;
    uint8_t channel;
    /* TOGGLE_FAVORITE: 1 to pin, 0 to unpin. The nav reads the node's current flag and sends
       the state it wants, so a press that races a NodeInfo cannot end up as a no-op toggle.
       RADIO_ACTION: the enum mesh_ui_settings_action that was confirmed.
       RESEND: the packet id of the attempt that failed. */
    uint32_t number;
    char text[MESH_UI_DRAFT_MAX];
    /* SEND_TEXT: the message this one answers (0 for a new one), and whether `text` is an
       emoji about it rather than a line of its own. A reaction always names a target; the app
       refuses one that does not. RESEND: whatever the failed message answered, because a retry
       of a reply is still a reply to the same thing. */
    uint32_t reply_id;
    bool is_reaction;
    /* SAVE_SETTINGS: the section (enum mesh_ui_settings_section), the channel slot for the
       Channels section (in `channel`), and the pending edits. */
    uint8_t section;
    uint8_t edit_count;
    struct mesh_ui_setting_edit edits[MESH_UI_SETTINGS_EDITS_MAX];
};

void mesh_ui_nav_init(struct mesh_ui_nav *nav);

/* The PIN a passkey prompt accepts: BlueZ passkeys are 0-999999. */
#define MESH_UI_PASSKEY_DIGITS 6U

/* Opens (or closes) the PIN prompt over whatever the user was doing. Driven by the app from
   the pairing agent, not by a key press, so it lives outside mesh_ui_nav_handle_key(). For a
   numeric comparison (`confirm`) the digits are pre-filled and Send accepts them. Returns
   true when the frame needs repainting. */
bool mesh_ui_nav_open_passkey(struct mesh_ui_nav *nav, const char *label, uint32_t passkey,
                              bool confirm);
bool mesh_ui_nav_close_passkey(struct mesh_ui_nav *nav);

/* How many digits a security number has, restated here for the draft cap the way
   MESH_UI_PASSKEY_DIGITS is. Pinned against the core's in the nodes suite. */
#define MESH_UI_VERIFY_DIGITS_MAX 6U

/* Opens and closes the key-verification sheet, and the keyboard that collects the security
   number. Driven by the app from the ceremony's state rather than by a key press, the way the
   pairing prompt is: what raises them is the radio asking something. Each returns true when
   the frame needs repainting. */
bool mesh_ui_nav_open_verify(struct mesh_ui_nav *nav);
bool mesh_ui_nav_close_verify(struct mesh_ui_nav *nav);
bool mesh_ui_nav_open_verify_number(struct mesh_ui_nav *nav);
bool mesh_ui_nav_close_verify_number(struct mesh_ui_nav *nav);

/* Applies one button press. Returns true when the visible state changed. When the press
   asks the app to do something, *out_action is filled in (may be NULL to discard). The store
   is read for list sizes and to resolve node names; it is not modified. */
bool mesh_ui_nav_handle_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum inkcell_key key, struct mesh_ui_action *out_action);

/* Insert committed host text only while the keyboard is the visible route. The grid and host
   input share its field cap and draft; returns true only when text was appended. */
bool mesh_ui_nav_insert_text(struct mesh_ui_nav *nav, const char *text);

/*
 * Applies one click on `target`, an id from include/mesh/ui/focus.h that the last frame drew.
 * Returns and fills *out_action exactly as mesh_ui_nav_handle_key() does, because a click is
 * answered as the presses it stands for - see src/ui/nav/nav_click.c.
 */
bool mesh_ui_nav_handle_click(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              uint32_t target, struct mesh_ui_action *out_action);

/*
 * A secondary click on `target` at (`x`, `y`): on a row of the screen's own list, the cursor goes
 * to it and the row's menu opens there. Anywhere else it puts an open menu down. Returns true
 * when the visible state changed; it never raises an action, since opening a menu does nothing.
 */
bool mesh_ui_nav_handle_context(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                uint32_t target, int x, int y);

/* Keeps cursors inside their lists after the data changed. Returns true if anything moved. */
bool mesh_ui_nav_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);

/* Rows on a screen for the current data (the Compose tab counts its To: and draft rows; the
   Messages tab counts only the current conversation). */
uint32_t mesh_ui_nav_row_count(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                               enum mesh_ui_screen screen);

/*
 * Indices into `messages` that belong to the conversation the nav is showing, oldest first.
 * Returns how many were written (at most `capacity`).
 *
 * A view rather than a list, because the transcript is drawn from whichever of the two the
 * store says is the right one - the flat 64 the radio still has, or the deeper window the card
 * filled for the open conversation. Callers get the view from
 * mesh_ui_store_message_view()/mesh_ui_snapshot_message_view() and index back into
 * `view.entries` with what this writes, so `capacity` and the caller's array are sized for
 * MESH_UI_MAX_THREAD_MESSAGES rather than for the flat list.
 */
uint32_t mesh_ui_nav_filter_messages(const struct mesh_ui_nav *nav,
                                     struct mesh_ui_message_view messages, uint32_t *out_indices,
                                     uint32_t capacity);

/*
 * The bubble under the cursor in the open thread, or NULL when the cursor is not on one.
 *
 * The seam every per-bubble verb asks through - the resend below, and the delete that has to
 * name a packet id - so the press and the keycap that offers it read the same row.
 */
const struct mesh_ui_message *mesh_ui_nav_message_at_cursor(const struct mesh_ui_nav *nav,
                                                            struct mesh_ui_message_view messages);

/*
 * The bubble under the cursor in the open thread when it is one this client sent and the mesh
 * came back to say it did not arrive, or NULL for anything else.
 *
 * Asked by both the press that resends and the bar that names the keycap, which is the point:
 * a verb that appears on some rows and not others has to be one answer, or the frame ends up
 * offering a press the nav refuses. The same seam mesh_ui_actions_node_press() uses.
 *
 * Takes the message list rather than the whole store because the action bar has only a
 * snapshot, and building a store view to ask one question would be a snapshot-sized copy per
 * frame for a pointer comparison.
 */
const struct mesh_ui_message *mesh_ui_nav_resendable(const struct mesh_ui_nav *nav,
                                                     struct mesh_ui_message_view messages);

/* Human name for the open thread: "All traffic", "#LongFast", "BRVO", or "Messages" when the
   conversation list is showing. */
void mesh_ui_nav_conversation_name(const struct mesh_ui_nav *nav, char *out, size_t out_len);

/* What a row of the Messages tab's conversation list is. */
enum mesh_ui_conversation_kind {
    MESH_UI_CONVERSATION_ALL = 0, /* every message, whatever it belongs to */
    MESH_UI_CONVERSATION_CHANNEL,
    MESH_UI_CONVERSATION_DIRECT,
    MESH_UI_CONVERSATION_NEW, /* the "New message" row: opens the send-to picker */
};

#define MESH_UI_CONVERSATION_PREVIEW_MAX 96U
/* Two cells and a terminator: an avatar carries initials, not a name. Four bytes because one
   of those cells may be a multi-byte character. */
#define MESH_UI_CONVERSATION_INITIALS_MAX 9U

struct mesh_ui_conversation {
    uint8_t kind; /* enum mesh_ui_conversation_kind */
    /* Where a message to this conversation goes, in the same terms as nav.target_*. */
    uint32_t node;
    uint8_t channel;
    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    /* What a backend draws in the avatar, and the seed that picks its colour. The seed is the
       conversation's identity rather than its name, so a node that renames itself keeps the
       tint the user has learned to look for. */
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
    uint32_t tint;
    /* Newest message in the conversation, for the list's second line. Empty and zero when the
       conversation has no traffic yet. */
    char preview[MESH_UI_CONVERSATION_PREVIEW_MAX];
    uint32_t last_time;
    uint32_t message_count;
    bool preview_outbound;
    /* Inbound messages that arrived after this conversation was last read. The "All traffic"
       row carries the total across every other row rather than a mark of its own.
     *
     * Counted on a muted conversation exactly as on any other: the row still says how much has
     * piled up, because muting a channel is asking not to be interrupted by it and not asking
     * to be lied to about it. What a mute takes away is the *total* - see
     * mesh_ui_nav_unread_total(), which is what the tab badge and the all-traffic row read. */
    uint32_t unread;
    /* Whether this conversation may interrupt: mesh_ui_store_conversation_muted()'s answer,
       carried on the row so the list can mark it and the action bar can name the press. */
    bool muted;
};

/*
 * The conversation list: "All traffic", then every enabled channel in the radio's table order,
 * then every node we have direct messages with newest first, then "New message". Channels keep
 * the radio's order rather than sorting by recency so a row does not move out from under the
 * cursor while the user is reaching for it.
 */
uint32_t mesh_ui_nav_conversation_count(const struct mesh_ui_store *store);
/* The rows of that list that are a conversation somebody can be in - the channels and the
   direct threads - leaving out "All traffic", which is a view over them, and "New message",
   which is a button. It is what the tab's heading counts: counting all four kinds read
   "Messages (3)" over a client that had never received a word. */
uint32_t mesh_ui_nav_conversation_threads(const struct mesh_ui_store *store);
bool mesh_ui_nav_conversation_at(const struct mesh_ui_store *store, uint32_t index,
                                 struct mesh_ui_conversation *out);
/* True when X has been pressed once on this conversation and the next one deletes it. What a
   backend asks so the armed row can say so rather than the screen saying it in the abstract. */
bool mesh_ui_nav_conversation_is_armed(const struct mesh_ui_nav *nav,
                                       const struct mesh_ui_conversation *conversation);

/*
 * Which row of the conversation list the open thread is, found by what the thread is rather than
 * by where the list was: all traffic, a channel slot, or a peer. The row is the list's own order,
 * and a direct peer's place in it moves with every message that re-ranks the peers - so the index
 * parked when the thread opened can name somebody else by the time the list is drawn beside the
 * thread, and a thread opened from the Nodes tab or the picker was never at that index at all.
 *
 * Returns `nav->conversation_list_cursor` when no thread is open or the thread's conversation is
 * not listed yet - a peer written to for the first time, before anything has been said.
 */
uint32_t mesh_ui_nav_open_conversation_row(const struct mesh_ui_nav *nav,
                                           const struct mesh_ui_store *store);

/* Inbound messages across every channel and peer that have not been read. */
uint32_t mesh_ui_nav_unread_total(const struct mesh_ui_store *store);

/* START on a conversation row: ask the app to flip its mute. Neither "All traffic" nor "New
   message" is a conversation, so the press does nothing on either. Returns true when the frame
   changed - which it does not here, because what changes is the app's to publish. */
bool mesh_ui_nav_mute_conversation(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   uint32_t index, struct mesh_ui_action *action);

/*
 * The two cells an avatar shows for `name`, upper-cased.
 *
 * The rule every messenger uses: the first letter of each of the first two words, or the first
 * two letters when there is only one - which for a Meshtastic short name ("BRVO") is its first
 * half. Non-alphanumerics are skipped, so the "!a1b2c3d4" a node with no User falls back to
 * gives "A1" rather than "!A", and a channel's leading '#' does not eat one of the two cells.
 * Anything outside ASCII is taken as-is and counted as one cell.
 *
 * Public because it is the *same* rule on four screens, not just the conversation list: a node
 * row, a device row and a picker row all want the disc the eye has already learned. Two screens
 * deriving initials two ways is two nodes with different colours for the same radio.
 *
 * `out` wants MESH_UI_CONVERSATION_INITIALS_MAX bytes. An empty result is legal - a name with
 * no letters in it draws an empty disc rather than a wrong one.
 */
void mesh_ui_nav_initials(const char *name, char *out, size_t out_len);

/* The picker's rows: channels first (node_id = MESH_MESSAGE_BROADCAST_ADDR, channel set), then
   nodes other than ourselves. Returns the row count; mesh_ui_nav_picker_row() describes one. */
uint32_t mesh_ui_nav_picker_count(const struct mesh_ui_store *store);
bool mesh_ui_nav_picker_row(const struct mesh_ui_store *store, uint32_t index, uint32_t *out_node,
                            uint8_t *out_channel, char *out_name, size_t out_name_len);

/*
 * The disc worn by the channel or node that `node`/`channel` addresses - its two cells and the
 * seed that tints them - in the same terms as nav.target_*.
 *
 * It is here rather than in a backend because three lists draw the same discs for the same
 * radios, and a node whose disc is a different colour or a different two letters between them
 * is a node the user cannot follow from one to the next. Deriving it per screen is exactly how
 * they come to disagree, and both ways it happened are worth keeping in mind:
 *
 *   - from the string a screen is showing. A picker row *reads* "BRVO  Bravo Creek", whose
 *     first two words both begin with B, so its initials would be "BB" against Messages' "BR".
 *   - from one field rather than the resolved name. A node with no short name shows the "----"
 *     placeholder, which has no letters in it at all and yields an empty disc, while the other
 *     lists fall through to the long name and then to the "!hex" id.
 *
 * So every caller comes through here, and the fallback order is mesh_ui_nav_node_name()'s once
 * rather than each screen's own. A screen may still *show* the placeholder - what it displays
 * and what identifies it are different questions.
 *
 * `out_initials` wants MESH_UI_CONVERSATION_INITIALS_MAX bytes; `out_tint` may be NULL.
 */
void mesh_ui_nav_target_avatar(const struct mesh_ui_store *store, uint32_t node, uint8_t channel,
                               char *out_initials, size_t out_len, uint32_t *out_tint);

/* Compose overlay rows: 0 = the draft, then the canned replies. There is no To: row; the
   overlay only ever opens over a thread, and that thread is the destination. */
#define MESH_UI_COMPOSE_ROW_DRAFT 0U
#define MESH_UI_COMPOSE_FIRST_CANNED 1U
uint32_t mesh_ui_nav_compose_row_count(void);

/*
 * Rows on the bubble sheet: one per emoji in the fixed set (include/mesh/ui/reactions.h), plus
 * the delete on the end.
 */
uint32_t mesh_ui_nav_reaction_row_count(void);

/* Whether that row is the delete rather than one of the emoji. The renderer asks it to draw an
   icon instead of a glyph, and the action bar asks it to name A. */
bool mesh_ui_nav_reaction_row_is_delete(uint32_t index);

/*
 * The grid this client's keyboard is: the emoji pages, the word on the submit key, and the cap
 * the append is held to.
 *
 * Built per call rather than kept, because two of the three change with the job the keyboard
 * was opened for and a copy on the nav would be a second answer to go stale. It is a const
 * description over tables that outlive any nav, so the value returned may be handed straight to
 * inkcell_keyboard_key() or to the renderer.
 */
struct inkcell_keyboard_layout mesh_ui_nav_kb_layout(const struct mesh_ui_nav *nav);

/*
 * The submit key finishes rather than sends.
 *
 * One predicate for the word and the symbol, which is the point of it being here: "Send" only
 * when something goes to a person. A settings field is finished, a waypoint's name is finished
 * - the place is shared by the app afterwards, not by this key - and so is a link, which is
 * brought up rather than put on the air. A security number is the sharpest case: it goes to the
 * radio in the user's hand, and a send arrow over six digits the whole ceremony depends on
 * staying off the mesh teaches exactly the wrong thing. The word and the icon were two copies
 * of this list and the icon's copy was missing that line.
 */
bool mesh_ui_nav_kb_submit_finishes(const struct mesh_ui_nav *nav);

/*
 * The keyboard on screen is the Find keyboard: the one whose query is typed into the heading.
 *
 * Not `keyboard_node_query` read on its own. A pairing prompt or a security number opened over
 * the Find keyboard parks it rather than closing it, so that flag stays set underneath - and the
 * renderer that read it alone drew the search heading over six digits BlueZ was waiting for,
 * while every key went to the prompt. The prompt wins here as it wins in the key dispatch.
 */
bool mesh_ui_nav_kb_node_search(const struct mesh_ui_nav *nav);

/*
 * The most bytes the draft may hold, whichever job the keyboard is doing: the message limit, a
 * settings field's own cap, a waypoint's name, a passkey's six digits, or a network address.
 *
 * Public because a backend draws a counter under the field and was computing the number itself,
 * so the two disagreed on every flavour but the first two - a place's name is cut at 29 bytes by
 * the append under a counter promising 233, and that counter is the only thing on the frame
 * saying a limit exists at all. One function, for the reason the action bar and the press it
 * names ask one function.
 */
size_t mesh_ui_nav_draft_cap(const struct mesh_ui_nav *nav);

void mesh_ui_nav_set_toast(struct mesh_ui_nav *nav, uint64_t now_ms, const char *text);
/* The same, raised from inside a key press, which has no clock of its own. It is dated by
   mesh_ui_store_handle_key() from the clock the store was last ticked with - so the notice
   stands for four seconds of whichever clock is driving the frames, and a capture is the same
   on any host. Nothing else raises one: an undated notice never expires. */
void mesh_ui_nav_raise_toast(struct mesh_ui_nav *nav, const char *text);

/* The same snackbar, for a notice nothing the user did asked for - something arrived. It waits
   for whatever is showing rather than replacing it, and waits behind anything already waiting.
   See the definition for why an arrival yields and a press does not. */
void mesh_ui_nav_post_toast(struct mesh_ui_nav *nav, uint64_t now_ms, const char *text);
/* Takes down the notice showing, for a press. What is waiting stays queued until
   mesh_ui_nav_date_toast() runs after the press, so a notice the press raised itself goes up first.
   Returns true when anything was showing. */
bool mesh_ui_nav_dismiss_toast(struct mesh_ui_nav *nav);
/* After a press: dates a notice the press raised undated, or, when the press left nothing showing,
   puts up the next one waiting. A no-op on a dated notice. */
void mesh_ui_nav_date_toast(struct mesh_ui_nav *nav, uint64_t now_ms);
/* Clears an expired toast; returns true if it did. */
bool mesh_ui_nav_tick(struct mesh_ui_nav *nav, uint64_t now_ms);

const char *mesh_ui_screen_name(enum mesh_ui_screen screen);

/*
 * Which of the Radio tab's three places is on the panel.
 *
 * The flags that say where the tab is standing outlive a change of tab, so each of these is the
 * flag *and* the screen - the question every reader actually means. Asked here rather than
 * spelled out at each site because the Radio tab used to be two tabs, and forty places that
 * said `screen == DEVICES` are forty places that could each have forgotten half of it.
 */
bool mesh_ui_nav_devices_showing(const struct mesh_ui_nav *nav);
/* The Nodes tab showing a place - the places list, or one place's detail, from the list or from
   the map - rather than the roster, the map or a node. What the Waypoints tab was. */
bool mesh_ui_nav_waypoints_showing(const struct mesh_ui_nav *nav);
bool mesh_ui_nav_status_showing(const struct mesh_ui_nav *nav);

/*
 * The settings section the panel is showing, on whichever tab shows it: the Settings tab's open
 * section, the Radio tab's open page, or MESH_UI_SETTINGS_NO_SECTION on every other screen.
 *
 * The question every reader of a section's rows means. Before the Radio tab opened pages built
 * from sections, `screen == SETTINGS && settings_section != NO_SECTION` was the only way a
 * section could be on the panel; spelling that at each site now would be a dozen places that
 * could each forget the Radio tab. A channel and a module's parent are the Settings tab's alone,
 * so on the Radio tab mesh_ui_nav_open_channel() answers MESH_UI_SETTINGS_NO_CHANNEL.
 */
uint8_t mesh_ui_nav_open_section(const struct mesh_ui_nav *nav);
/* The section a Radio tab page is built from, or MESH_UI_SETTINGS_NO_SECTION for NONE. */
uint8_t mesh_ui_nav_radio_page_section(uint8_t page);
uint8_t mesh_ui_nav_open_channel(const struct mesh_ui_nav *nav);

/* Canned replies shown on the Compose tab. Defaults are built in; a file with one message per
   line (blank lines and '#' comments skipped) replaces them. */
size_t mesh_ui_canned_count(void);
const char *mesh_ui_canned_text(size_t index);
int mesh_ui_canned_load(const char *path);
void mesh_ui_canned_reset(void);
/*
 * Whether `text` could join the list as it stands: short enough for a slot, not already on it,
 * room left, and nothing mesh_ui_canned_load() would skip on the way back in - a control byte,
 * or a leading '#' that would read as a comment. The compose sheet offers the save only when
 * this holds, so the keycap is never a press that fails.
 */
bool mesh_ui_canned_accepts(const char *text);
/*
 * Adds `text` to the end of the list and writes the whole list to `path`, which is then the
 * list: the file replaces the built-in replies, so while they are what is showing they are
 * written out first rather than lost. Written beside and renamed over, so a card pulled halfway
 * leaves the old file. Returns the new count, or -EINVAL when the text is not one
 * mesh_ui_canned_accepts() takes, or the errno the write failed with.
 */
int mesh_ui_canned_add(const char *path, const char *text);

#ifdef __cplusplus
}
#endif
