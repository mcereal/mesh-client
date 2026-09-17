# In-client help

There is no help *system*. There is one more column on the tables that already describe a
setting, and one screen that reads it — the same shape as `theme.c` answering a colour role or
`delivery.c` answering an ack mark.

## The model

`struct field_spec` (`src/ui/settings/settings_internal.h`) carries a `note`, a catalog id.
`MESH_STR_NONE` is id 0 and the empty string, so a row that explains itself needs no change and a
field acquires help by naming one id in the row somebody was already writing. Sections get the
same treatment in a table beside `k_section_icons[]`. Two accessors return the **id**, not the
text, so a caller can ask *is there help here* without a `strlen`:

```c
enum mesh_str_id mesh_ui_settings_section_note(enum mesh_ui_settings_section section);
enum mesh_str_id mesh_ui_settings_field_note(enum mesh_ui_setting_field field);
```

`src/ui/tables/help.c` answers one question — given where the nav is, what does the help screen say —
with data rather than pixels: a title, a subject, and a list of `{label, body}` id pairs.
Returning **false** is how it says there is nothing to explain here, which is what decides whether
the key is offered at all. It takes the configuration and the handshake rather than a whole
snapshot, because its two callers are the action bar (one per frame) and the key handler (which
has a store, not a snapshot).

A settings section's topic is **built** by walking its rows. A feature has no rows to read a note
off, so those topics are a table in `help.c` keyed on the **route** — `k_help_features[]` — which
means a new way of reaching a screen arrives with the right help already attached. The route it
reads is the one *underneath* help: `mesh_ui_route_of()` puts `MESH_UI_ROUTE_HELP` on top of
everything, so `mesh_ui_route_under_help()` is the second call that stops the walk below it.

## The screen

**A topic is the section, not the row.** A note per row would leave the key dead on two rows out
of three, and `actions.c`'s rule is that a keycap that only sometimes does anything is worse than
one fewer. Every section has a note, so the key always does something — and the screen **opens
scrolled to the row the cursor was on**. A feature's topic opens at the top instead, because its
paragraphs are about the screen rather than about the rows of it.

`fb_list_note()` is the row: a heading at the label scale, sentences wrapped under it. A note is
however many `mesh_ui_wrap_lines()` lines long, which is however many *steps* its row is — the
same `fb_list_begin_heights()` machinery the transcript uses. This is the one list where a row's
height is a property of its **words** rather than of its kind.

**The key is SELECT**, the one free key on the case, and it means the same thing everywhere. It
is handled *before* the overlay dispatch, since every overlay handler consumes whatever key it is
given. `MESH_UI_ROUTE_HELP` is a route level, so the slide, the back arrow and the `B` keycap all
follow without being told.

## What a note may say

- **One or two sentences**: what the setting does, and what goes wrong if it is set badly. The
  second half is the one worth the room.
- **No more than 200 characters.** `help_notes_fit_the_panel` enforces it over every
  `SETTINGS_NOTE_*` and `HELP_NOTE_*` id.
- **No cross-references to other rows by name** — they break on a reorder and do not translate.
- **The id says where it is read**: `SETTINGS_NOTE_LORA_HOPS`, not `HOP_LIMIT_EXPLANATION`.
- **Never assembled from parts.** One note is one entry, %-specifiers included.

Notes are the one class of string a locale may leave `NULL` ([`i18n.md`](i18n.md)); the
`HELP_SUBJECT_*` and `HELP_LABEL_*` headings beside them are not.

## Things that look like bugs and are not

- **A note that is missing is a note nobody wrote.** There is no fallback text and no "no
  description available" row — that would be chrome saying nothing, on the screen whose whole job
  is to say something. `MESH_STR_NONE` is the right answer forever for a row whose label is the
  whole of it.
- **A row with no paragraph opens on the nearest one above it, but never across a subheading.**
  Without the fallback the screen opens at the overview for most rows; without the reset,
  Telemetry's Air quality rows opened on a paragraph about reading a thermometer in Fahrenheit.
  `help_opens_on_the_overview_across_a_subheading` checks both sides.
- **Two explained rows in one section may not share a heading**, and one that would keeps
  `MESH_STR_NONE`. A topic is a flat list, so Telemetry's five Enabled / Interval / Show on screen
  trios would draw five paragraphs headed "Enabled". The section's overview names the five
  readings instead. `help_note_labels_are_unique_in_a_section` compares the *rendered* label,
  because a reader sees the word and not the id.
- **The help screen does not show values.** It says what a setting is, never what it is set to;
  the section behind it is already saying that.
- **SELECT is not offered on every screen.** It is offered on all five tabs and every level over
  one; not on the Settings tab's own list of sections, and not by an overlay asking a question —
  the way out of a question is to answer it. A screen holding a destructive question armed offers
  no help either (`help_question_armed()`), because a third press would stand the question down
  where the user could not see it happen.
- **A radio action has no note.** Reboot, the resets and the forget rows each already put
  `mesh_ui_settings_confirm_text()` in front of the press. A note beside that is the same warning
  in two files, drifting apart.
- **An overlay over a settings section is not the settings section.** `help_section_open()` asks
  the *route*, not the nav — `settings_section` stays set under every overlay a section can raise.
