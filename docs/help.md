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
    enum mesh_str_id title;
    enum mesh_ui_icon icon;
    struct mesh_ui_help_entry entries[MESH_UI_HELP_ENTRIES_MAX];
    uint32_t count;
};

bool mesh_ui_help_topic(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_help_topic *out);
```

This is `status.c`'s shape and it is here for `status.c`'s reason: `nav.c` walks the same list the
renderer draws and `actions.c` names the press that opens it, and three opinions about one list is
how a screen comes to disagree with itself.

`mesh_ui_help_topic()` returning **false** is how the client says there is nothing to explain
here - which is what decides whether the key is offered at all.

## The screen

### It is per section, not per row

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
  the id's own `SETTINGS_NOTE_` prefix so the exemption cannot quietly widen. The phase 1 notes
  ship untranslated on purpose - a confidently wrong Spanish sentence about transmit power is
  worse than a visibly English one.
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
  `SETTINGS_NOTE_*` id. Longer than that and the reader is being handed a manual page on a 3.2"
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

**Phase 2 - the rest of the fields worth explaining.** Notes on the other rows where the name is
not the explanation. The list to work through, in the order the questions actually get asked:
bandwidth, device role, rebroadcast mode, smart position and its two thresholds, position
precision, the channel PSK choices, MQTT uplink and downlink, the admin key rows, Store &
Forward's history window, and neighbour info's floor.

**Phase 3 - features, keyed on the route.** Waypoints, the tapback set, Store & Forward's replay,
the all-traffic transcript, the Devices tab. `mesh_ui_help_topic()` already takes a snapshot and
switches on the route, so this is entries in that switch rather than a new mechanism - and keying
on the route means a new way of reaching a screen gets the right help without being told, for the
reason `route.h` gives at length.

## Things that will look like bugs and are not

- **A note that is missing is a note nobody wrote.** There is no fallback text and deliberately no
  "no description available" row. A section lists the rows that have something to say; the rest are
  not mentioned. A placeholder would be a row of chrome saying nothing, on the screen whose entire
  job is to say something.
- **The help screen does not show values.** It says what a setting *is*, never what it is set to -
  the section behind it is already saying that, and a second opinion about the radio's config is a
  second opinion that can be stale.
- **SELECT is not offered on every screen**, and that is the `actions.c` rule rather than an
  oversight: the bar names presses that do something here.
- **The help screen is not editable and has no cursor on a value.** Its cursor scrolls, nothing
  more. B leaves. That is the whole interaction, and it is why help is a level rather than an
  overlay: nothing is stacked on top of the section, so nothing has to be restored when it closes.
