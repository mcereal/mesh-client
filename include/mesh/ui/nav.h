#pragma once

#include "mesh/map/viewport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_store;
struct mesh_ui_message_list;

/*
 * Logical buttons. Backends and the input layer translate from whatever the hardware reports
 * (evdev codes on the Brick, keyboard keys on a host) so the navigation model never sees a
 * keycode. The names follow the Brick's face buttons: A confirms, B backs out.
 */
enum mesh_ui_key {
    MESH_UI_KEY_NONE = 0,
    MESH_UI_KEY_UP,
    MESH_UI_KEY_DOWN,
    MESH_UI_KEY_LEFT,
    MESH_UI_KEY_RIGHT,
    MESH_UI_KEY_A,
    MESH_UI_KEY_B,
    MESH_UI_KEY_X,
    MESH_UI_KEY_Y,
    MESH_UI_KEY_L1,
    MESH_UI_KEY_R1,
    MESH_UI_KEY_START,
    MESH_UI_KEY_SELECT,
};

/* Tabs, in the order LEFT/RIGHT (and L1/R1) walk them. Compose is not one: it is an overlay
   over the open conversation, so it can never be reached with a stale destination. */
enum mesh_ui_screen {
    MESH_UI_SCREEN_MESSAGES = 0,
    MESH_UI_SCREEN_NODES,
    /* The places, next to the nodes: a waypoint is a point on the mesh that does not move, and
       it belongs beside the list of points that do rather than buried inside one of them. */
    MESH_UI_SCREEN_WAYPOINTS,
    MESH_UI_SCREEN_DEVICES,
    MESH_UI_SCREEN_STATUS,
    MESH_UI_SCREEN_SETTINGS,
    MESH_UI_SCREEN_COUNT,
};

/*
 * The Nodes list's first row, which is not a node.
 *
 * It opens the map. A row rather than a keycap because the Nodes tab has already spent A, X and
 * Y on things a node row does, and because a way into a screen that only a button nobody
 * mentions can reach is a screen nobody finds - the argument the conversation list's "New
 * message" row and the Waypoints tab's "New waypoint here" row both make. It is the *first* row
 * rather than the last for the one reason those two are last: this list can be a hundred and
 * twenty-eight rows long, and a button at the bottom of that is a button that is not there.
 */
#define MESH_UI_NODES_MAP_ROW 0U

#define MESH_UI_NAV_TARGET_NAME_MAX 40U
/* nav.settings_section when the Settings tab shows the section list rather than a section. */
#define MESH_UI_SETTINGS_NO_SECTION 0xFFU
/* nav.settings_channel when the Channels section shows its list rather than one channel. */
#define MESH_UI_SETTINGS_NO_CHANNEL 0xFFU
#define MESH_UI_NAV_TOAST_MAX 64U
/*
 * Notices waiting behind the one on screen.
 *
 * There is one snackbar and it stands for four seconds, so two things happening at once used to
 * mean the second overwrote the first and the user saw one of them - which was survivable while
 * almost nothing raised a notice, and stopped being so once an arriving message could. Three is
 * twelve seconds of backlog at the far end: long enough that nothing in a burst is simply lost,
 * short enough that a notice is still about something that just happened. A fourth would be
 * telling the user about something sixteen seconds old.
 */
#define MESH_UI_NAV_TOAST_QUEUE 3U
#define MESH_UI_CANNED_MAX 16U
#define MESH_UI_CANNED_TEXT_MAX 64U
/* Upstream Data.payload caps at 233 bytes; the draft and action text hold that plus a NUL. */
#define MESH_UI_DRAFT_MAX 234U
/* Pending Settings edits held until Save. A section with fifteen editable fields has to be
   able to carry fifteen edits: below that, mesh_ui_nav_edit_set() returns false and the press
   silently does nothing. */
#define MESH_UI_SETTINGS_EDITS_MAX 16U

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
 * Which press writes an edit. The Position section is the one with two: Y writes
 * PositionConfig with a set_config, and "Set fixed position" writes the coordinate rows with
 * set_fixed_position, because the firmware only takes them that way. Every other field belongs
 * to its section's own save. An edit has to say which press owns it so that neither one clears
 * the other's pending work off the screen when it fires.
 * mesh_ui_settings_field_consumer() (settings.h) answers it for a field.
 */
enum mesh_ui_setting_consumer {
    MESH_UI_SETTING_CONSUMER_SECTION = 0,
    MESH_UI_SETTING_CONSUMER_FIXED_POSITION,
};

/* One edited setting. `field` is an enum mesh_ui_setting_field (settings.h); NONE marks an
   empty slot. Toggles and enums use `number`, numbers use `number`, text uses `text`. */
struct mesh_ui_setting_edit {
    uint8_t field;
    uint32_t number;
    char text[MESH_UI_SETTING_TEXT_MAX];
};

/* On-screen keyboard geometry: four rows of ten characters and a row of five actions. */
#define MESH_UI_KB_COLS 10U
#define MESH_UI_KB_CHAR_ROWS 4U
#define MESH_UI_KB_ROWS (MESH_UI_KB_CHAR_ROWS + 1U)
#define MESH_UI_KB_ACTIONS 5U

enum mesh_ui_kb_layer {
    MESH_UI_KB_LOWER = 0,
    MESH_UI_KB_UPPER,
    MESH_UI_KB_SYMBOLS,
    MESH_UI_KB_LAYER_COUNT,
};

enum mesh_ui_kb_action {
    MESH_UI_KB_ACTION_LAYER = 0, /* cycle lower/upper/symbols */
    MESH_UI_KB_ACTION_SPACE,
    MESH_UI_KB_ACTION_DELETE,
    MESH_UI_KB_ACTION_SEND,
    MESH_UI_KB_ACTION_CANCEL,
};

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
    /* One-line transient notice ("Sent to ABCD", "Connecting..."); empty when none. */
    char toast[MESH_UI_NAV_TOAST_MAX];
    uint64_t toast_until_ms;
    /*
     * What is waiting to be said, oldest first. Undated by construction: a queued notice has
     * not started standing yet, and it takes its deadline from the tick that promotes it - so
     * the queue needs no clock and mesh_ui_nav_raise_toast()'s undated path costs nothing here.
     *
     * Full means the *oldest waiting* one goes, never the newest. A backlog is only worth
     * keeping while it is still news, and a burst whose tail was dropped would show the user
     * the three oldest things that happened and silently withhold what happened last.
     */
    char toast_queue[MESH_UI_NAV_TOAST_QUEUE][MESH_UI_NAV_TOAST_MAX];
    uint8_t toast_queued;
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
    /* The tapback picker over the open thread: one emoji per row, sent as a reaction about
       `reply_to`. Its own overlay rather than a row in the compose sheet because it is not a
       message - it never opens the keyboard and it never takes the draft. */
    bool reaction_open;
    uint32_t reaction_cursor;
    /* "Send to" picker: every enabled channel, then every node. Picking opens that
       conversation's thread, and `picker_follow` says what opens over it. */
    bool picker_open;
    uint32_t picker_cursor;
    uint8_t picker_follow; /* enum mesh_ui_picker_follow */
    /* Free-text entry. */
    bool keyboard_open;
    uint8_t kb_row;
    uint8_t kb_col;
    uint8_t kb_layer; /* enum mesh_ui_kb_layer */
    char draft[MESH_UI_DRAFT_MAX];
    /* Nodes tab: a node's detail is open (cursor[NODES] indexes its rows) rather than the node
       list, whose position is parked in node_list_cursor meanwhile. The same two-level shape
       as Settings and Messages. Opening a detail does *not* move the compose target; only the
       detail's "Message this node" row does. */
    bool node_detail_open;
    uint32_t node_detail_node; /* the open node's id: the list is re-ranked under us */
    uint32_t node_list_cursor;
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
     * A level of the Nodes tab rather than a tab of its own, which is what docs/maps-roadmap.md
     * asked for and what the shape of the thing wants: a map is a second way of reading the
     * roster, not a seventh place to be. A node's detail can be opened *over* it - the map is
     * then one level deeper than the list and the detail is one deeper again - so backing out of
     * a node opened from the map lands on the map rather than on the list it was never on.
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
     * Status tab: which verb the cursor is on.
     *
     * **This is the Status cursor, and cursor[MESH_UI_SCREEN_STATUS] is not.** Status is the one
     * screen with no rows: its cards offer verbs, Up and Down walk those, and what the cursor
     * holds is `enum mesh_ui_status_verb` rather than a position in the list of them. The
     * difference is what a verb appearing or disappearing does. As an index it moved the
     * cursor's meaning without moving the cursor, so the list had to be append-only and every
     * verb had to restate the conditions of the verbs before it; as a verb the list is free to
     * be written in the order the cards draw, and a link that drops and comes back leaves the
     * reader on the button they were standing on.
     *
     * MESH_UI_STATUS_VERB_COUNT is "no verb", which is what a screen offering none holds. The
     * value is otherwise only ever one mesh_ui_status_verb_resolve() answered with, so nothing
     * reads it without asking the list on offer whether it is still there - see
     * include/mesh/ui/status.h.
     */
    uint8_t status_verb;
    /*
     * Status tab: the airtime chart is open over the cards.
     *
     * The one level this tab has, and it carries no cursor of its own - a chart is a picture and
     * there is nothing on it to choose between, so `status_verb` stays where it was and is still
     * naming the trend verb when B lands back on the cards. That is why the flag exists at all
     * rather than the screen being a fourth card: the cards are a list of verbs the cursor
     * walks, and a picture is not a verb.
     *
     * It outlives a change of tab, as map_open does and for the same reason - every tab keeps
     * its own place - which means it says *where the Status tab is standing* rather than *what
     * is on the panel*. Anything reading it has to check `screen` as well, or a press meant for
     * the Nodes list closes a chart nobody can see.
     */
    bool trend_open;
    /*
     * How far back both charts look: `enum mesh_ui_trend_span`, stepped by Left and Right.
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
     * MESH_UI_TREND_SPAN_ALL rather than zero at rest - mesh_ui_nav_init() says so - because ALL
     * is what these screens did before there was a picker, and a reader who never touches Left
     * or Right should see what the screen has always shown them.
     */
    uint8_t trend_span;
    /*
     * Waypoints tab: a place's detail is open (cursor[WAYPOINTS] indexes its rows) rather than
     * the list, whose position is parked in waypoint_list_cursor meanwhile. The same two-level
     * shape as Nodes, Messages and Settings.
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
       section. Row 0 goes ahead, row 1 cancels. `confirm_action` says which of the two it is
       standing in front of: MESH_UI_SETTINGS_ACTION_NONE is the section save, anything else is
       that radio action. */
    bool confirm_open;
    uint8_t confirm_cursor;
    uint8_t confirm_action; /* enum mesh_ui_settings_action */
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
    /* When the keyboard edits a setting rather than the Compose draft: the field it is for
       (NONE for Compose) and the Compose draft parked while it is open. */
    uint8_t keyboard_field;
    char draft_saved[MESH_UI_DRAFT_MAX];
    /* BLE pairing prompt. The keyboard is retargeted for it the same way a setting's text
       retargets it, except that this one is opened by the app rather than by a key press:
       BlueZ asks for the PIN somewhere in the middle of a connect and blocks until it is
       answered. `pairing_confirm` marks the numeric-comparison case, where the digits are
       pre-filled and Send means "yes, that is what the node is showing". */
    bool keyboard_passkey;
    bool pairing_confirm;
    char pairing_label[MESH_UI_NAV_TARGET_NAME_MAX];
    /* The keyboard the prompt displaced, restored when it closes. The prompt can land on top
       of an open keyboard, and the text being typed is parked in `draft_saved` like any other.
       `keyboard_displaced` is what says one was open at all: `keyboard_field_displaced` cannot,
       because NONE is what a message keyboard reads as. (A settings keyboard that had itself
       parked a compose draft loses that one: there is a single parking slot, and the text in
       front of the user is the one worth keeping.) */
    bool keyboard_displaced;
    uint8_t keyboard_field_displaced;
    /*
     * The help screen over whatever is under it: what this screen is for, and what its rows
     * mean (src/ui/help.c, docs/help.md).
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
    /* Devices tab: Y is armed by one press and forgets the node on the second, because a
       bond dropped by accident costs the user a re-pair with the PIN. */
    bool devices_forget_armed;
    uint32_t devices_forget_row;
};

enum mesh_ui_action_type {
    MESH_UI_ACTION_NONE = 0,
    MESH_UI_ACTION_CONNECT, /* identifier = BLE address */
    /* dest/channel/text, plus reply_id/is_reaction: a new message, a threaded reply, or a
       tapback. One type rather than three because the destination, the text and the failure
       path are the same three things in all of them. */
    MESH_UI_ACTION_SEND_TEXT,
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
       RADIO_ACTION: the enum mesh_ui_settings_action that was confirmed. */
    uint32_t number;
    char text[MESH_UI_DRAFT_MAX];
    /* SEND_TEXT: the message this one answers (0 for a new one), and whether `text` is an
       emoji about it rather than a line of its own. A reaction always names a target; the app
       refuses one that does not. */
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

/* Applies one button press. Returns true when the visible state changed. When the press
   asks the app to do something, *out_action is filled in (may be NULL to discard). The store
   is read for list sizes and to resolve node names; it is not modified. */
bool mesh_ui_nav_handle_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum mesh_ui_key key, struct mesh_ui_action *out_action);

/* Keeps cursors inside their lists after the data changed. Returns true if anything moved. */
bool mesh_ui_nav_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);

/* Rows on a screen for the current data (the Compose tab counts its To: and draft rows; the
   Messages tab counts only the current conversation). */
uint32_t mesh_ui_nav_row_count(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                               enum mesh_ui_screen screen);

/* Indices into `messages` that belong to the conversation the nav is showing, oldest first.
   Returns how many were written (at most `capacity`). */
uint32_t mesh_ui_nav_filter_messages(const struct mesh_ui_nav *nav,
                                     const struct mesh_ui_message_list *messages,
                                     uint32_t *out_indices, uint32_t capacity);

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
bool mesh_ui_nav_conversation_at(const struct mesh_ui_store *store, uint32_t index,
                                 struct mesh_ui_conversation *out);
/* True when X has been pressed once on this conversation and the next one deletes it. What a
   backend asks so the armed row can say so rather than the screen saying it in the abstract. */
bool mesh_ui_nav_conversation_is_armed(const struct mesh_ui_nav *nav,
                                       const struct mesh_ui_conversation *conversation);

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

/* Tapback picker rows: one per emoji in the fixed set (include/mesh/ui/reactions.h). */
uint32_t mesh_ui_nav_reaction_row_count(void);

/* Keyboard legend for the backends. Character rows return the glyph at that cell (a NUL for an
   unused cell); the action row is described by mesh_ui_kb_action_label(). */
char mesh_ui_kb_char(enum mesh_ui_kb_layer layer, unsigned row, unsigned col);
const char *mesh_ui_kb_action_label(const struct mesh_ui_nav *nav, enum mesh_ui_kb_action action);

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
/* Dates an undated notice. A no-op on one that is already dated, or on no notice at all. */
void mesh_ui_nav_date_toast(struct mesh_ui_nav *nav, uint64_t now_ms);
/* Clears an expired toast; returns true if it did. */
bool mesh_ui_nav_tick(struct mesh_ui_nav *nav, uint64_t now_ms);

const char *mesh_ui_screen_name(enum mesh_ui_screen screen);

/* Canned replies shown on the Compose tab. Defaults are built in; a file with one message per
   line (blank lines and '#' comments skipped) replaces them. */
size_t mesh_ui_canned_count(void);
const char *mesh_ui_canned_text(size_t index);
int mesh_ui_canned_load(const char *path);
void mesh_ui_canned_reset(void);

#ifdef __cplusplus
}
#endif
