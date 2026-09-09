# In-client help

The client offers something like two hundred radio settings and a dozen features, and until now
it explained none of them. A row says `Hop limit` and `3`, and the reader either already knows
what a hop limit is or leaves the screen no wiser than they arrived. That is a gap the Meshtastic
phone apps fill with a tap-and-a-tooltip and this one filled with nothing.

This document is what fills it, and the shape it takes.

## The rule this follows

Every "what is this?" question in this client is already answered the same way: **a screen names
an id, a table answers**. `theme.c` answers a colour role, `strings.c` answers a string id,
`status.c` answers which card carries which verb, `delivery.c` answers which mark an ack state
gets. Nothing is spelled out where it is drawn.

Help is that kind of question. *What does this setting do* is a property of the field, exactly as
its label, its kind and its presets are - so it belongs in the table that already describes the
field, and not in a renderer, a switch, or a module of its own that would have to be kept in step
with `k_fields` by hand.

So there is no help *system*. There is one more column on the tables that already exist, and one
screen that reads it.

## The model

### A note is a catalog id on the thing it is about

`struct field_spec` (`src/ui/settings_internal.h`) gains one member:

```c
struct field_spec {
    enum mesh_str_id label;
    enum mesh_str_id note;   /* what this setting does; MESH_STR_NONE for a row that explains itself */
    ...
};
```

`MESH_STR_NONE` is id 0 and the empty string, which is what a designated initialiser leaves
behind - so all ~150 existing rows are correct without being touched, and a field acquires help by
naming one id in the row somebody was already writing to add it. The rule in `settings.c`'s header
comment survives intact: adding a setting is a row in `k_fields` plus a case in `app_settings.c`,
and nothing else.

Sections get the same treatment as a table beside `k_section_icons[]`, for the reason the comment
there already gives: a lookup with no cases in it is a table, and a section added without a note
then reads as "no note" rather than failing to compile in a file that has nothing to do with help.

Two accessors, mirroring the ones next to them:

```c
enum mesh_str_id mesh_ui_settings_section_note(enum mesh_ui_settings_section section);
enum mesh_str_id mesh_ui_settings_field_note(enum mesh_ui_setting_field field);
```

They return the **id**, not the text. That is what lets a caller ask *is there help here* without
a `strlen`, and it is what stops the fb backend, the tests and any future backend from each
having their own opinion about what an absent note looks like.

### A topic is built, not drawn

`src/ui/help.c` owns one question: given where the nav is, what does the help screen say? It
answers with data - a title and a list of entries, each an id pair - and never with pixels:

```c
struct mesh_ui_help_entry {
    enum mesh_str_id label; /* the row this is about, MESH_STR_NONE for the topic's own note */
    enum mesh_str_id body;
};

struct mesh_ui_help_topic {
    enum mesh_str_id title;   /* "Help", on every one of them */
    enum mesh_str_id subject; /* what is being explained, for the trail above the title */
    struct mesh_ui_help_entry entries[MESH_UI_HELP_ENTRIES_MAX];
    uint32_t count;
};

bool mesh_ui_help_topic(const struct mesh_ui_settings *settings,
                        const struct mesh_ui_handshake_state *handshake,
                        const struct mesh_ui_nav *nav, struct mesh_ui_help_topic *out);
```

It takes the configuration and the handshake rather than a whole snapshot because the two callers
are the action bar, which builds one per frame, and the key handler, which has a store rather than
a snapshot - a snapshot parameter would have made one of them copy tens of kilobytes onto a fixed
stack per press.

This is `status.c`'s shape and it is here for `status.c`'s reason: `nav.c` walks the same list the
renderer draws and `actions.c` names the press that opens it, and three opinions about one list is
how a screen comes to disagree with itself.

`mesh_ui_help_topic()` returning **false** is how the client says there is nothing to explain
here - which is what decides whether the key is offered at all.

### A feature's topic is a table, keyed on the route

A settings section is a list of fields, so its topic is *built*: walk the rows, take the notes.
Nothing about that list is written down in `help.c`, which is the point - a field acquires help by
naming one id in the row somebody was already editing.

A feature has no rows to read a note off. What the Waypoints tab is for is not a property of any
one of the places on it, and the tapback picker is a row of glyphs with nothing behind them to
carry a note at all. So those topics are a table in `help.c`: one array of paragraphs per feature,
and one row per **route** pointing at the array that answers for it.

```c
static const struct help_feature k_help_features[] = {
    HELP_FEATURE(MESH_UI_SCREEN_WAYPOINTS, MESH_UI_ROUTE_LIST, MESH_STR_TAB_WAYPOINTS,
                 k_help_waypoints),
    HELP_FEATURE(MESH_UI_SCREEN_WAYPOINTS, MESH_UI_ROUTE_WAYPOINT, MESH_STR_HELP_SUBJECT_WAYPOINT,
                 k_help_waypoints),
    ...
};
```

Keyed on the route rather than on the nav's flags for the reason
[`route.h`](../include/mesh/ui/route.h) gives at length: a new way of reaching a screen then
arrives with the right help already attached, instead of with a condition somebody has to
remember to add here. Two rows may name one array, which is how the Waypoints list and one open
place share a topic - they are one feature seen at two depths.

The route it reads is the one **underneath** the help screen. `mesh_ui_route_of()` puts
`MESH_UI_ROUTE_HELP` on top of everything, so a help screen asking what is being *drawn* would
answer about itself and empty on the first repaint. `route.c` therefore stops its walk below help
and adds the level afterwards, and the two halves are two calls:

```c
void mesh_ui_route_of(const struct mesh_ui_nav *nav, struct mesh_ui_route *out);
void mesh_ui_route_under_help(const struct mesh_ui_nav *nav, struct mesh_ui_route *out);
```

The alternative was for `help.c` to copy the nav, clear `help_open` and ask again - a second
derivation of the same answer, which is what that header spends its opening paragraphs refusing.

Help's `slot` and `subject` are now left exactly as the place underneath filled them, rather than
overwritten with the settings section. That was right while help only explained sections and would
have been wrong the day a node detail acquired a topic: help on one node and help on another have
to be two places, or the slide plays when the cursor moves and not when the screen changes.

### A topic names what it is explaining

`struct mesh_ui_help_topic` carries a `subject`: the catalog id of the section's or the feature's
name, drawn on the trail above the title. The screen's own title is "Help" everywhere, so without
it the frame never says *what* it is helping with.

It is on the topic rather than read out of the nav by the backend, which is what
`fb_render_help()` used to do - `mesh_ui_settings_section_name(nav->settings_section)`, which is a
renderer knowing that help is about settings. Over the Nodes tab it would have drawn whichever
section the user last opened, confidently and wrongly. `mesh_ui_settings_section_label()` is the
id half of the section name that this needed, and the twenty-seven-case switch it replaced is now
a table beside the icons and the notes.

## The screen

### For settings, it is per section, not per row

The obvious design is a supporting line under every settings row. It is wrong twice. It doubles
the height of every section on a panel that already scrolls, and prose on a supporting line is the
one shape that cannot be scanned - the point `fb_render_devices()` already makes about an attach
line. It is also help you want once, paid for on every frame forever.

The next design is a note per row, opened by a press on that row. That is wrong once, and fatally:
most fields do not need a note, so the key would do nothing on two rows out of three. That is
exactly what `actions_settings()` refuses to do for A on a settings section - *a keycap that only
sometimes does anything is worse than one fewer*.

So a topic is **the section**, and the screen is the section's own note followed by the notes of
whichever of its rows have one. Every section has a note, so the key always does something. And
the screen **opens scrolled to the row the cursor was on**, so pressing help on `Hop limit` lands
on the hop-limit paragraph and scrolling up reaches the LoRa overview. One screen, always
populated, and still a direct answer to the row in front of you.

A feature's topic opens at the top instead, and that is the difference between the two kinds
rather than the settings answer failing to apply. A section's rows and its paragraphs correspond
one for one, so there is a row to open on; a feature's paragraphs are about the screen rather than
about the rows of it, and its third paragraph has no more claim to a place in the list than its
first.

### It is a list of wrapped paragraphs

One new widget, `fb_list_note()`, and it is the note row: a heading line at the label scale and
the sentences under it, wrapped across the list's whole width. A note is `mesh_ui_wrap_lines()`
lines long, which is however many **steps** its row is, which is exactly the shape
`fb_list_begin_heights()` already takes - the same "rows that are not all the same height"
machinery the transcript and the node detail use. The screen measures every entry with
`fb_list_note_steps()`, hands over the array, and the window, the highlight and the scroll thumb
are three sums of the same heights.

This is the one list in the client where a row's height is a property of its *words* rather than
of its kind, which is why the measure is a call rather than a constant - and why the draw takes
its height from the model rather than from the words a second time.

It is deliberately *not* `fb_card_note()`: a card note stops at `FB_CARD_NOTE_LINES` (three), which
is right for a sentence the radio wrote into a card of other rows and wrong for the only content
on a screen. Nor is it a list item's supporting line, which is one line and elided - the shape for
a reminder, not for an explanation.

### The key is SELECT

`BTN_SELECT` is already mapped to `MESH_UI_KEY_SELECT` in `input.c` and `nav.c` currently falls
straight through it. It is the one free key on the case, and it is the right one, because help
should mean the same thing everywhere rather than being contextual. `enum mesh_ui_button` gains
`MESH_UI_BUTTON_SELECT`, whose cap is `SELECT` - untranslated, like every other cap, because it is
what is printed on the plastic.

The bar offers it only where `mesh_ui_help_topic()` answers, which is the same table the press
reads, so the keycap and the press cannot disagree.

It is handled **before the overlay dispatch** rather than after it. Every overlay handler consumes
whatever key it is given, so a press reaching the bottom of `mesh_ui_nav_handle_key()` is a press
on a tab's own screen - which is why the one screen in the client made entirely of glyphs was the
one screen that could not say what its glyphs did. Hoisting it costs those handlers nothing:
`mesh_ui_nav_open_help()` answers false wherever there is no topic, so an overlay acquires the
press by acquiring a paragraph and not otherwise.

That move is also what made `help_question_armed()` necessary. Five screens arm a destructive
question - the settings discard, the Devices forget, the node remove, the waypoint delete, the
conversation delete - and each of them spends both keycaps on a yes and a no. Only the settings
one was reachable while SELECT was handled last; all five are now, and a press that opened help
over one would stand the question down where the user could not see it happen.

### It is a level, so it animates for free

`MESH_UI_ROUTE_HELP` in `enum mesh_ui_route_level`, set from `nav.help_open` in
`mesh_ui_route_of()`. The slide, the back arrow and the `B` keycap all follow from that without
being told - which is the whole of what deriving a route buys.

## The catalog, which is where the cost is

A note is prose, and prose is the expensive kind of string. Roughly 150 field notes plus 27
section notes would grow a 1101-entry catalog by fifteen percent, and `src/i18n/locale_es.c` is a
parallel table of the same length.

Three things make that affordable, and they are policy rather than luck:

- **An untranslated note falls back per id.** `mesh_str_in()` resolves against the locale's table
  and drops through to English for any entry the locale left NULL - per string, not per table. A
  locale that translates no notes at all renders English notes and correct Spanish everywhere
  else. **Note bodies are the one class of string a locale may omit.** That is not only written
  down: `i18n_spanish_catalog` holds every other id to full translation and skips these, keyed on
  the id's own `SETTINGS_NOTE_` or `HELP_NOTE_` prefix so the exemption cannot quietly widen. The
  notes ship untranslated on purpose - a confidently wrong Spanish sentence about transmit power
  is worse than a visibly English one.

  The *headings* that go with a feature's notes are deliberately not exempt. `HELP_SUBJECT_*` and
  `HELP_LABEL_*` are a few words each, drawn beside the rest of the screen's chrome, and a screen
  half in Spanish is worse than a paragraph wholly in English.
- **They ship in phases.** Sections first, because "what is Store & Forward even for" is the
  question people actually have, and 27 strings answer it. Fields follow, and only the ones that
  are genuinely opaque.
- **A field with nothing worth saying keeps `MESH_STR_NONE` forever.** `Long name` does not need a
  paragraph. The screen simply does not list it.

### What a note may say

- **One or two sentences.** What the setting does, and what goes wrong if it is set badly. The
  second half is the one worth the room: `Hop limit` is guessable, *"every extra hop costs the
  whole mesh airtime, so raise it only when a node you want is genuinely that far away"* is not.
- **No more than 200 characters.** `help_notes_fit_the_panel` enforces it over every
  `SETTINGS_NOTE_*` and `HELP_NOTE_*` id. Longer than that and the reader is being handed a manual page on a 3.2"
  panel; shorter is usually better.
- **No cross-references to other rows by name.** A note that says "see Spread factor below" breaks
  when the section is reordered and is untranslatable into a language that orders them differently.
- **The id says where it is read**, as every id does: `SETTINGS_NOTE_LORA_HOPS`, not
  `HOP_LIMIT_EXPLANATION`.
- **Never assembled from parts.** One note is one entry, %-specifiers included. This is the
  catalog's own rule and it matters more here than anywhere: a paragraph glued together from
  clauses hands a translator a word order they cannot change.

## Phases

**Phase 1 - the seam and the sections. Done.** `note` on `field_spec`, the section table, the
accessors, `help.c`, `nav.help_open`, `MESH_UI_ROUTE_HELP`, `MESH_UI_BUTTON_SELECT`,
`fb_list_note()`, the fb screen, and a note for all 27 sections.

It also ships **five field notes** - LoRa's region, spread factor, coding rate, hop limit and
transmit power - which is not scope creep but the other half of the proof: without one explained
row the field accessor, the entry ordering and "open where the cursor was" are all code no test
can reach. The LoRa section is the worked example of what a finished section looks like.

**Phase 2 - the fields the questions get asked about. Done.** Notes on the rows where the label is
not the explanation, which is the list this section used to hold as the work to do: bandwidth,
device role, rebroadcast mode, smart position and its two thresholds, position precision, the
channel key, MQTT uplink and downlink, the admin keys, Store & Forward's history window and
neighbour info's floor. Eighteen explained rows in all, counting LoRa's five from phase 1.

`help_field_notes_are_optional` names them one by one rather than counting them, because a count is
a number that goes stale on the first row anybody adds and says nothing about *which* row went
missing.

**Phase 3 - features, keyed on the route. Done.** `mesh_ui_help_topic()` now answers for the
screens that are not lists of settings: each of the five tabs, one conversation, one node, one
place, and the tapback picker. That is `k_help_features[]`, `mesh_ui_route_under_help()`, the
topic's `subject`, SELECT moving ahead of the overlay dispatch, and `help_question_armed()`.

SELECT consequently means the same thing on every tab, which it did not in phase 1: the one key on
the case with no verb printed on it worked on one tab in six.

**Store & Forward's replay**, which this section used to list here as a feature, deliberately is
not one. Its press lives in a settings section, so the section's own note and the phase 2 notes on
its history rows are where it is explained - and the destructive radio actions beside it need no
note at all, for the reason below.

**Phase 4 - the long tail. Done.** Eighty-four more rows, which with the eighteen above makes a
hundred and two explained rows out of the field table's hundred and fifty-two. Every section that has
editable rows now explains at least one of them, and `help_field_notes_reach_every_explained_section`
is what keeps that true - named by *section* rather than by row, because a hundred field names in
a test would be the field table written out a second time, and a whole section losing its notes is
the failure worth catching rather than one row of it.

There was no mechanism to build, as this section used to say: it was one line in `k_fields` and one
in the catalog per row. Phase 4 did find one thing the earlier phases could not, and it is a rule
rather than a row - see *two explained rows in one section may not share a heading* below.

What is still deliberately silent after phase 4:

- **The rows whose label repeats inside their own section.** Telemetry's five Enabled / Interval /
  Show on screen trios and External notification's three Pin / On message / On bell trios. Their
  sections say it once in the overview instead - Telemetry's names the five readings, so the
  paragraph that five rows called "Enabled" would have carried is written once.
- **The sections that are one idea each.** Status message and Canned messages have an overview and
  nothing to add to it; About, Radio and Modules have no editable rows at all; Radio actions puts a
  confirm sheet in front of every press, for the reason below.
- **The rows whose label is the whole of it.** Long name, Short name, a channel's Username and
  Password, the six canned slots, the three ambient colours. `MESH_STR_NONE` is the right answer
  forever, and a placeholder in their place would be a row of chrome saying nothing.

## Things that will look like bugs and are not

- **A note that is missing is a note nobody wrote.** There is no fallback text and deliberately no
  "no description available" row. A section lists the rows that have something to say; the rest are
  not mentioned. A placeholder would be a row of chrome saying nothing, on the screen whose entire
  job is to say something.
- **Two explained rows in one section may not share a heading, and one that would keeps
  `MESH_STR_NONE`.** A topic is a flat list: an entry carries the row's own label, and there are no
  subheadings in it - the notes are a property of the *fields* and the subheadings are a property
  of the section's *layout*, which is exactly the split that keeps `settings.c` from having to
  describe a screen. So Telemetry, whose rows are five groups of Enabled / Interval / Show on
  screen, would draw five paragraphs headed "Enabled" on the one screen whose whole job is to be
  read, and the reader would have to guess which reading each was about. Those rows carry no note
  and the section's overview names the five readings instead. `help_note_labels_are_unique_in_a_section`
  holds both halves of it - the headings, and the paragraphs, since one note is written about one
  row and two rows naming one id is a copy-paste rather than a choice. It compares the *rendered*
  label rather than the id, because "Interval" and "Interval" are two catalog entries holding one
  word and a reader sees the word.
- **The help screen does not show values.** It says what a setting *is*, never what it is set to -
  the section behind it is already saying that, and a second opinion about the radio's config is a
  second opinion that can be stale.
- **SELECT is not offered on every screen**, and that is the `actions.c` rule rather than an
  oversight: the bar names presses that do something here. It is offered on all five tabs and on
  every level opened over one; it is not offered on the Settings tab's own list of sections, and
  it is not offered by an overlay that is asking the user a question - the compose sheet, the
  keyboard, the send-to picker, the confirm dialog. The way out of a question is to answer it,
  and the screen underneath does have a topic.
- **A radio action has no note, and that is the confirm sheet's doing.** Reboot, shutdown, the
  NodeDB reset, both factory resets and the two forget rows each already put a dialog in front of
  the user saying what is about to be lost - `mesh_ui_settings_confirm_text()`, four wrapped lines
  of it, at the moment the press is made rather than on a screen they would have to think to open.
  A note beside that would be the same warning written twice, in two files, drifting apart.
  Store & Forward's history request is the one action with no confirm, because the worst a
  mistaken press costs is one small packet.
- **A feature's help opens at its first paragraph, not at the row the cursor was on.** A settings
  section's rows and its paragraphs correspond one for one; a feature's paragraphs are about the
  screen rather than about the rows of it, so there is no row to open on.
- **An overlay over a settings section is not the settings section.** `help_section_open()` asks
  the *route* whether a section is what is on the panel, not the nav whether one is open
  somewhere below - `settings_section` stays set under every overlay a section can raise. Asking
  the nav cost a bug: with SELECT ahead of the overlay dispatch, the press opened a section's
  help over a half-typed field while the keyboard's own bar said nothing about it. The feature
  half never had that failure, because a table keyed on the route cannot answer for a route
  nobody put in it - which is the argument for keying on the route, made by the half that did
  not.
- **A screen holding a destructive question open offers no help.** All five arming flags, not
  only the settings one - see `help_question_armed()`. An armed question has spent both keycaps on
  a yes and a no, and a third press that opened a screen would stand the question down where the
  user could not see it happen.
- **The help screen is not editable and has no cursor on a value.** Its cursor scrolls, nothing
  more. B leaves. That is the whole interaction, and it is why help is a level rather than an
  overlay: nothing is stacked on top of the section, so nothing has to be restored when it closes.
