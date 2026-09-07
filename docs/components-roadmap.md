# Component roadmap

An audit of what `src/ui/backends/fb_widgets.h` already is, what it is still missing, and the
order the gaps are worth closing in. The reference point is Material 3, not because the Brick
should look like an Android phone, but because M3 is the most completely written-down answer to
the problem this component set is already solving: *a screen describes its content and a token
layer answers for everything else.*

Written against the tree at the time of the audit. Line references are indicative; the argument
is not.

## Where the set already is

This is worth stating first, because most of what follows is small next to it.

The **token layer is essentially done for colour and shape.** `include/mesh/ui/theme.h` carries
six families of four slots over a neutral spine, `enum mesh_ui_state` as a modifier rather than
a colour, a shape scale, and `mesh_ui_theme_validate()` holding every theme to a contrast
contract by looping over the families rather than over a hand-written list of pairs. That is
M3's colour system, arrived at independently and for the same reasons.

The **component set is real**, not a pile of draw calls: buttons in three variants, chips,
switches, meters (determinate and indeterminate), titles, empty states, rules, the list and its
slotted list item, conversation cells, chat bubbles, cards, text fields, dialogs and the
snackbar. The list item in particular — leading avatar or icon, marker gutter, value column,
trailing text/badge/switch/icon/meter, supporting line, accent edge, divider — is the slot model
every platform converged on, and it is the reason a new kind of row is a struct literal rather
than a new function.

`fb_screens.c` is correspondingly thin. There is now **no** `fb_fill_rect` and there are **no**
`fb_draw_text` calls anywhere in the file: at the time of the audit there were three, and all
three were inside the two pieces of chrome that had never become components (§2.2 and §2.3).
The only renderer that still names a raw glyph scale is the on-screen keyboard, which is laying
out a grid. Everything else is content.

So this is not a rewrite. It is: **three tokens the theme does not answer for yet**, and
**a handful of components whose absence is visible on screen right now.**

> **Second pass.** The three tokens have since landed, along with the two pieces of chrome that
> were not components (§2.2, §2.3) and the quantitative work in §2.11–2.12. What the re-audit
> added is §1.4, §1.5, §2.14, §2.15 and §2.16, and a reordering in §3 that puts two of them
> first. §5 says why. Sections are numbered in the order they were found rather than by tier,
> which is why the newest entries sit at the end of their tier's list.

## 1. The tokens

These come first because they are multipliers. Every component below is easier to add once they
exist, and several of them are only worth adding once they do.

### 1.1 There is no type scale

> **Landed.** `enum mesh_ui_type` in `theme.h`. Kept below as written, because the reasoning is
> what the scale has to keep answering for; the limitation it did not reach is in §3.

The theme answers for colour, for shape and for metrics. It does not answer for type. What it
has is two numbers — `metrics.scale` for the body and `metrics.chrome_scale_down` for the tab
strip and the footer — and everything on screen is drawn at one of them.

Those two are a *content / chrome* split, not a hierarchy, and the difference shows. Open
Settings or Nodes: `fb_draw_title()` draws the screen title at `state->scale` — the same size as
every list row beneath it — and separates it from them with `MESH_UI_TONE_PRIMARY` and nothing
else. A heading and its content are the same size, distinguished only by colour.

The card goes the other way: `fb_draw_card()` draws its heading at `layout->small`, deliberately
(a section label "is not something to read, it is something to find", and a body-scale heading
costs a row on a fifteen-row panel), while its labels and values stay at `state->scale`. That is
a sound call for the room available, but it means a card heading is *smaller* than the content it
heads — which is a reasonable answer to "there are only two sizes" and not an answer a type scale
would ever give.

So there is no size a renderer can reach for that means *more important*. M3 gets its hierarchy
from a type scale first and colour second, and so does every desktop UI that reads as modern;
here colour is doing the whole job alone.

The fix is the move this codebase has already made twice. A renderer should name a *type role* —
`MESH_UI_TYPE_TITLE`, `MESH_UI_TYPE_LABEL`, `MESH_UI_TYPE_BODY`, `MESH_UI_TYPE_SUPPORTING` — and
`theme.c` should answer with a scale, exactly as it answers a colour role with an RGB and a shape
with a radius. The table lives in `struct mesh_ui_metrics` beside `shape[]`, adding a theme stays
a table entry, and a screen title stops being the same size as its own list.

This is the single largest structural gap, and it is the one that would most change how the UI
looks.

> A caution the font layer imposes: the glyph scale is an integer multiplier over a 5x7 cell, so
> a type scale here is a scale *of steps*, not of points, and the useful range is narrow. That is
> an argument for four or five roles, not for M3's fifteen.

### 1.2 There is no spacing scale

> **Landed.** `enum mesh_ui_space`, in half-steps, plus `fb_gutter()` for the panel inset that
> is not glyph-relative and does not belong in it.

`margin / 2`, `2 * small`, `line / 2`, `2 * scale` and friends appear on about a dozen lines
across `fb_draw.c`, `fb_widgets.c` and `fb_screens.c`. Each is correct; collectively they are the same
thing the colour literals were before `theme.c`. A theme that wants a denser or a roomier layout
can move `metrics.margin` and nothing else, and half the spacing on screen does not move with it
because it was derived from the glyph scale instead.

M3's answer is a 4dp grid. Ours would be a small step scale in `metrics` — `NONE / XS / SM / MD /
LG` — read through a `fb_space()` the way radii are read through `fb_radius()`. It is a mechanical
change and it is worth doing before the components in §2, because each new component otherwise
adds two or three more literals to the pile.

### 1.3 There are no motion tokens

> **Landed.** `enum mesh_ui_motion` in `theme.h`; the five per-widget constants below are gone.

`src/ui/anim.c` is good: fixed-point easing, a table keyed per control, a repaint timerfd that
only runs while something is moving. What it has no answer for is *how long*. Five durations are
defined next to the five widgets that use them:

| Constant | Value | Where |
|---|---|---|
| `FB_SNACKBAR_IN_MS` | 220 | `fb_widgets.c` |
| `FB_SNACKBAR_OUT_MS` | 150 | `fb_widgets.c` |
| `FB_SWITCH_MS` | 140 | `fb_widgets.c` |
| `FB_METER_MS` | 320 | `fb_widgets.c` |
| `FB_METER_LOOP_MS` | 1400 | `fb_widgets.c` |

Those are five independent guesses that happen to agree. M3 names durations (short / medium /
long) and pairs each with a curve, so a set of controls moves as one system rather than as five
things that were each tuned alone. The same argument as the colour families, and the same fix: a
`MESH_UI_MOTION_SHORT` in the theme, and the widget names the token.

This is the cheapest of the three and the one that most affects whether the UI *feels* modern, as
distinct from looking it.

### 1.4 A list row is one height, chosen for the whole list

> **Landed.** `mesh_ui_list` counts steps rather than items; `mesh_ui_list_begin_heights()` and
> `fb_list_begin_heights()` are the entry points. Kept below as written - what it got wrong is
> in §9.

`fb_list_begin_rows()` takes a *`per_item`* — one number, for the list. The conversation cell
spends two rows and every settings row spends one, and each list picks its number once at
`fb_list_begin*()`. Rows of the same height are therefore not a limitation; rows of *different*
heights within one list are, and that is the case every one of the following wants:

- a section heading a step down from the rows it heads — §1.1's one unfinished half;
- a row carrying a sparkline, which wants two steps where its neighbours want one (§2.13);
- a banner *inside* a list rather than above it (§2.9);
- a meter row that puts its bar under the words instead of beside them, which is what a card
  row already does and a list row cannot.

The cursor is why this is not cosmetic. `struct mesh_ui_list` counts items and multiplies:
`first`, `visible` and the scroll thumb in `mesh_ui_list_scroll()` are all item arithmetic, and
`fb_list_next()` advances `y` by `line` (or by `rows * line` for the two-row item, which is the
same multiplication with a constant in it). Give one row a different height and the window's
first row, the rail's thumb and the highlight rect stop agreeing, in three different ways.

Two shapes would work and they cost very differently. A **measure pass** — the list asks the
caller how tall row *i* is before it draws anything, which is what every platform's list does —
is general and needs a callback through an API that currently has none. A **height in steps**
field on `struct fb_list_item`, with the model counting steps rather than items, is less
general, needs no second pass, and is almost certainly the right answer on a panel that holds
sixteen rows: nothing here needs to know the height of row four hundred.

Either way the change lands in `layout.c` and its tests before a pixel moves, because
`mesh_ui_list_scroll()` is unit tested and a second backend is meant to read it. Budget it as a
layout-model change with a component set on the far side, not as a widget.

### 1.5 The icon set is a prerequisite, not a detail

`include/mesh/ui/icons.def` held twenty-three entries when this was written, and four of the
components below name a glyph that is not among them: §2.5's checkbox and radio, §2.9's banner
(an `info` and an `error`), §2.15's back affordance, §2.14's star. Adding one is not a line of
code. It is a line in the `.def`, then `scripts/gen-icons.py` against a Material Symbols Rounded
font that is not in this tree, then committing the regenerated `src/ui/icon_glyphs.c` — a step
CI cannot run and a contributor without the font cannot run either (`CLAUDE.md` says as much,
under the generators that are not part of the build).

So an icon is a real line item in a component's budget, in the same way §2.2 discovered the
catalog was. Every entry below that needs one says so.

## 2. Components

Tiered by whether the absence is visible on the device today.

### Tier 1 — visible now

> **Landed.** `fb_list_rail()`, drawn from the list model with no screen asking for it.

**2.1 No scroll indicator.** There is no scroll rail anywhere in the tree — `grep -rn scroll
src/ui/backends` finds only comments. A list of forty-two nodes shows a window of eight with
nothing on screen saying where in the list that window sits, or that there is more of it in
either direction. `struct mesh_ui_list` already carries `count`, `first` and `visible`, so a rail
is three numbers it is already holding and a rounded rect. Highest ratio of "reads as a modern
list" to lines of code in the whole audit, and it needs nothing else on this list first.

> The `Nodes (8 of 42)` in that tab's title is **not** what a rail would replace, and the two must
> not be confused. That title is local rows against `my_info.nodedb_entries` — how much of the
> radio's NodeDB we are holding — and its sibling form, `NODES_TITLE_OFF_RADIO`, reports nodes the
> radio has forgotten and we have kept. Both are facts about the roster outliving the connection
> (see the note in `CLAUDE.md`), not facts about scroll position, and a rail derived from the list
> model cannot express either. The title keeps its count.

> **Landed.** `fb_draw_action_bar()`, over `mesh_ui_actions_for()` in `src/ui/actions.c`. The
> paragraph below is kept as written because its last sentence turned out to be exactly right:
> the catalog change *was* the bulk of it. §3 has what the tables do and do not say.

**2.2 The footer is not a component.** `fb_draw_footer()` is static in `fb_screens.c` and draws
two lines of plain text: `A open node  X pin  Y write  L/R tabs`, then the link summary. It is
the last screen-level renderer that lays out its own pixels, and it is also the piece of chrome
that most makes the UI read as a terminal rather than as a handheld OS. The button component
already draws a keycap (`FB_BUTTON_FILLED` at `MESH_UI_SHAPE_SM` is exactly one); a bottom action
bar of keycap-plus-label pairs is that component in a loop. Both DRY and aesthetics point the
same way here.

**This one has a prerequisite in the catalog, and it is the real work.** The hints are whole
localised sentences — `HINT_NODES` is `"A open node  X pin  Y write  L/R tabs"`, one entry — so a
keycap loop has nothing to iterate: it would either draw the entire sentence on every key or parse
translated text to find the button letters, and parsing a translation is the one thing the i18n
layer exists to prevent. The action bar therefore needs the hints expressed as *structured action
data* first — a small table of (button, label string id) per screen, with a catalog entry per
label — and the sentence hints retired as that table covers them. Budget the catalog change as the
bulk of §2.2, not the drawing.

> **Landed.** `fb_draw_nav_bar()`, over a reusable `fb_draw_chip_strip()`.

**2.3 The tab strip is not a component.** `fb_draw_tabs()` is also static in `fb_screens.c`, and
it is about thirty lines that fill a bar, run a three-step label-elision fallback, loop chips and
close with a rule. It is M3's navigation bar. Because it is private to that file, nothing else can
have a chip strip — a filter row on Nodes (*All / Direct / Favourites*) would have to re-derive
the measuring loop, which is the exact duplication `fb_chip_width()` was added to prevent. Move it
to `fb_widgets.c` as `fb_draw_nav_bar()` / `fb_draw_chip_strip()`.

> **Landed.** `enum fb_card_variant` on `struct fb_card`, `fb_card_action()` on its heading line,
> and the Status tab's cursor over the verbs its cards carry. §8 has what doing it changed —
> including the two things the entry below got wrong about where an action row goes and about
> what "focusable" turned out to mean.

**2.4 The card has one variant.** On the dark theme the card fill sits close enough to the ground
that a card reads as an outlined box; on Status, two cards of equal weight say Link and Mesh with
nothing to say which one to look at. M3 has filled, elevated and outlined and uses the difference
for exactly that. `enum fb_card_variant` is one field on `struct fb_card` and a fill choice in
`fb_draw_card()`.

Two related card gaps: there is no **action row** (a card that ends in buttons — "Radio actions"
is a list row that opens a screen because a card cannot offer a verb), and a card is never
**focusable**, which is why the whole Status tab is inert.

> **The badge half has landed.** `FB_TRAILING_BADGE` is wired into the Devices list, and doing
> it produced one rule worth more than the wiring: *anything a badge does not shout is a badge
> that should not be there.* Badging all four device states looked right on the dark theme and
> collided on two others — the contrast palette has one yellow and the colourblind palette one
> blue, so the resting `paired` capsule came out the same colour as the warning beside it on the
> first and as `connected` on the second. The resting state is a quiet word in the same
> right-aligned slot now, and the colour is left to the rows that have something to report. The
> leading-icon half, the pinned star and the bubble's padlock are still open.

> **Landed.** `FB_LEADING_ICON` has its callers - the settings root and the Modules list, over
> `mesh_ui_settings_section_icon()` - the star is `MESH_UI_ICON_PINNED` in a plain row's marker
> gutter, and the padlock is `struct fb_bubble`'s own `meta_icon`. §5's third-pass note has what
> doing it changed about the entry below.

**2.14 A slot with no caller, and a slot only one component can reach.** This is the one entry
on the list where the component is not missing — the *wiring* is — and it is therefore the
cheapest visible change left. The two slots are not in the same state, and an earlier draft of
this section flattened them into one claim that was wrong about the second:

- `FB_LEADING_ICON` is **never set**. The two branches that draw it in `fb_widgets.c` are
  the implementation; nothing anywhere assigns the kind, so they are unreachable.
- `FB_TRAILING_BADGE` has **exactly one**, and it is not a screen: `fb_draw_conversation()`
  names it internally for a thread's unread count, so a badge is on screen on the Messages tab
  and always has been. What no screen had done is reach for it while composing an
  `fb_list_item` of its own — which is a much narrower finding than "nothing draws it", and
  worth stating carefully, because a slot that is exercised through one hardcoded caller is
  covered by that caller's rendering and cannot be assumed dead.

The header's own examples for the leading slot name three screens and none of the three does it.
Nodes and Devices both took the avatar instead, and in Devices' case that is right: a disc
carrying the transport answers "which bus is this radio on" in the place the eye already looks,
and a second icon would be saying it twice. What is left for the slot is the settings section
list and the root rows, which have no leading anything and are the one list here that reads as a
column of words.

The pinned star is the sharper finding. It is still `"\xE2\xAD\x90"` prepended to the row's text
in `fb_render_nodes()` — a live exception to §4's third rule. Its comment argues the star belongs beside the name rather than in place of the
identity the disc is carrying, which is a sound layout call and not an argument for a literal:
the marker gutter is exactly the slot for "a fact about this row, one cell, before the words",
and it is already reserved on every row of a list that declares it. Moving it there costs one
entry in `icons.def` and a `gen-icons.py` run (§1.5).

It is **not** the only one, and the second is the harder half. `fb_thread_row_build()` appends a
literal `\U0001F512` to a bubble's meta line for a direct message the radio decrypted with our key
pair rather than with a channel PSK, and that padlock is saying something no other mark on the
screen says. The star can move because a list row has a marker gutter waiting for it; the meta
line has no slots at all — it is a text run assembled by the line builder out of the delivered
or pending word, the padlock and the reactions, in that order — so retiring this one means
giving the bubble a slot first, which is a change to `struct fb_bubble` rather than a struct
field. Budget it with the components, not with the star.

The badge is still the cheapest of the three, one caller or none. Devices puts `Connected` /
`Working` / `Needs pairing` on the supporting line as dim text — a state word set as prose,
where every platform draws a filled capsule, and where `FB_TRAILING_BADGE` with a family already
draws exactly that. Success for connected, warning for needs-pairing: the tones are already
chosen on that screen, they are just being spent on the row's ink instead of on a pill. That is
a struct field, not a component. That the conversation cell had been drawing one all along is an
argument *for* doing it, not against: the slot is proven, and Messages and Devices saying the
same thing the same way is the whole point of a component set.

> **Landed.** `fb_draw_app_bar()` over `struct fb_app_bar`, with `fb_draw_badge()` lifted out
> of the trailing slot on the way past - which is most of §2.8. §7 has what doing it changed.

**2.15 There is no top app bar.** `fb_draw_title()` takes a `const char *`. A screen's heading is
therefore a *string*, and everything a heading has to carry gets glued into that string:
Settings builds `"Settings > %s%s%s"` out of `SETTINGS_TITLE_SECTION` and
`SETTINGS_TRAIL_MODULES` (`"Modules > "`), and the unsaved count arrives as a third `%s`.

That is a whole-sentence string id doing structural work, and it is the same mistake §2.2 spent
most of its budget undoing for the button hints — worse here, because the `>` separators hand a
translator the breadcrumb's *grammar* along with its words, and because a badge glued into a
title with `%s` cannot be a badge.

M3's top app bar is the component that answers all of it at once: a leading slot (the back
affordance, which B already does on every screen and nothing on screen says), an overline for
the trail, the title, and a trailing slot. §2.8's standalone badge wants to live in that trailing
slot — `3 unsaved` is a fact about the screen, not about a card — and §2.10's screen-level
progress bar wants the bar's bottom edge. Three entries on this list collapse into one component,
and the catalog loses two format strings rather than gaining any. Needs one icon (§1.5).

### Tier 2 — gaps that unlock screens we do not have yet

**2.5 No selection controls besides the switch.** No checkbox, no radio. The picker signals
"this is the current target" by giving one avatar a stated accent fill, which works for one
choice and does not generalise. Anything multi-select — *forget these nodes*, *which channels to
show* — has nothing to draw. In this component set both are `FB_TRAILING_CHECKBOX` /
`FB_TRAILING_RADIO` on the existing trailing slot, and the animation table that makes the switch
slide already keys them.

**2.6 No segmented button.** `fb_draw_chip()`'s own comment says it is "the shape a tab strip, a
filter row and a segmented control all have" — the component predicted this one. An `ENUM`
setting with two to four choices is currently a value column stepped with Left/Right, which shows
one option at a time and gives no sense of how many there are. A segmented control shows the set.

**2.7 No slider.** *(Landed - see §12, including the two things this entry did not have: that
what a preset list means cannot be derived from the integers in it, and that a list can be a
scale with one value standing outside it.)* `NUMBER` settings step with Left/Right and report a
figure. The meter already
draws an animated track with a fill; a slider is that plus a knob and a focused state. Worth it
for the settings that are genuinely continuous — screen-on seconds, broadcast intervals — and not
worth it for the ones that are really enums with numeric labels.

**2.8 No standalone badge or status pill.** A badge exists only as `FB_TRAILING_BADGE` inside a
list row. A card heading cannot carry `3 unsaved`, and the Status card cannot carry a `LIVE`
pill. The drawing already exists; it needs to come out of the row.

> Two things this got half right. The drawing does exist — and `FB_TRAILING_BADGE` is not
> merely un-liftable, it is **unused** (§2.14), so the row form wants a caller before the
> standalone form wants extracting. And the place `3 unsaved` actually belongs is not a card
> heading: it is the top app bar's trailing slot (§2.15), which is why that entry should land
> first and this one should shrink to whatever is left over.
>
> **It did, and to almost nothing.** Step 7 lifted the capsule into `fb_draw_badge()` because it
> had two callers the moment the app bar had a trailing slot, and `3 unsaved` is drawn by it. All
> that is left of this entry is the Status card's `LIVE` pill — a caller, not a component.

**2.9 No persistent inline banner.** The snackbar is transient by design and correctly so. But
*radio disconnected*, *firmware mismatch* and *update available* are persistent states, and they
currently become a dim card note or a word in the footer. M3's banner sits under the app bar and
stays until dismissed or resolved. This is the component the Status tab's Link card is
half-imitating.

**2.10 No screen-level progress.** There is an indeterminate meter, but nothing draws a thin
linear indeterminate bar under the nav bar during handshake or sync. Open Settings before the
radio has answered and eight rows say `not loaded` with no sign that anything is happening. One
bar under the tab strip answers that for every screen at once.

**2.16 Nothing transitions.** §1.3 gave the theme durations and every one of them is spent on a
*control*: a switch's knob, a meter's fill, the snackbar's rise. Moving between screens is a cut.
Opening a node, entering a settings section, raising the keyboard, pressing B to go back — each
replaces the frame with a different frame in one repaint, and those four are precisely the
transitions a handheld OS animates. It is the difference between a UI that reads as a place and
one that reads as a slideshow, and it is the largest remaining gap in how the thing *feels* as
distinct from how it looks.

Two honest costs, and neither of them is in `fb_widgets.c`.

The nav has to say **what changed and which way**. Forward into a section and back out of one are
the same two frames in the opposite order, and that is the whole of what decides whether the new
screen slides in from the right or the old one slides off it. `struct mesh_ui_nav` holds where
you are, not how you got there, so this is a field and a rule about who clears it — the same
shape as `settings_parent`, which already exists for the breadcrumb and is the closest thing to
a precedent.

And there is **no alpha compositing**, so a cross-fade is out. `fb_draw.c` blends an icon against
a ground it is told about, which is not the same capability: nothing can read what is already on
the panel. What is available is an x-offset slide — render one screen at an offset for the length
of one `MESH_UI_MOTION_SHORT` — which is the transition those platforms mostly use anyway. Over
the existing repaint timerfd, at one screen per frame, that is affordable.

The reason it is Tier 2 and not Tier 1 is that unlike everything above it, no part of it is
visible as a *static* defect. Nothing on the panel is wrong right now; it is what happens between
two panels that is missing, which is also why it is the one entry here that a `make ui-capture`
GIF reviews better than a screenshot ever could.

### Tier 1.5 — the quantitative gap

> **Landed.** `struct mesh_ui_scale` and `mesh_ui_signal_level()` in
> [`layout.h`](../include/mesh/ui/layout.h), `struct mesh_ui_band` and `mesh_ui_band_tone()` in
> [`theme.h`](../include/mesh/ui/theme.h), the notches and the domain in `fb_draw_meter()`, and
> `FB_TRAILING_SIGNAL`. Kept below because the argument is what the next visualization has to
> keep answering.

**2.11 A meter had no marks on it.** This is the one the set got *nearly* right and it is worth
separating from the components that were simply absent. `fb_draw_meter()` could say how far
along a reading was and could turn amber when it crossed a threshold — but nothing on screen
said where the threshold *was*. The colour therefore reported a boundary the reader could not
locate, which is half of an answer to the question the bar exists for: the Status card's own
comment says a percentage "has to be read and then held against a threshold nobody carries
around", and a bare track is a threshold nobody can see either.

Two things were missing and they are one change. A **domain** (`struct mesh_ui_scale`), because
a bar that fills from zero cannot express a reading measured between −20 dB and +10 at all, and
because a caller normalising by hand is free to pick ends that the thresholds colouring the
number know nothing about. And a **band** (`struct mesh_ui_band`), stated in the reading's own
units and *drawn* — a notch cut into the track at each boundary — so the mark and the colour are
two readings of one statement. Order is meaning there: `bad` above `warn` climbs, `bad` below
`warn` falls, which is what lets a battery use the same component as airtime.

The band lives in `theme.h` beside `mesh_ui_tone_for_load()`, which it generalises and delegates
to; the scale lives in `layout.h` beside `mesh_ui_list_scroll()`, which is proportion arithmetic
for the same reason. What a number *means* is the theme's half; where it *goes* is layout's.

**2.12 A list had no way to show a signal.** The Nodes tab's trailing column was `4.2dB 3m` on
every row — a figure with a scale nobody carries around, forty-two times down one screen — and
the node detail's readings were text throughout. `FB_TRAILING_SIGNAL` is four rungs in the slot
the trailing text already had, and `MESH_UI_NODE_ROW_METER` is the node detail's row model
gaining the same "a fact, and a length beside it" shape the settings rows had.

Three rules came out of doing it, and they are the ones a sparkline or a stacked bar will face:

- **A picture cannot be wrong quietly.** The Nodes list already knew that a relayed node's SNR
  describes the relay and an MQTT node's describes nothing on the air — that is what the `Nhop`
  and `mqtt` branches were. Printing a number there was merely unhelpful; drawing a staircase
  would have been a claim. Each new visualization has to be checked against the branch it is
  replacing rather than dropped over it.
- **Quantise where the reading is noisy.** An SNR is measured off one packet. Four rungs is a
  claim its error bars support; a smooth bar is not, and easing between buckets would invent the
  intermediate values that bucketing was meant to refuse. `mesh_ui_signal_level()` is in
  `layout.c` so the ladder is arithmetic a test can reach.
- **Reuse the row's own pair.** A staircase takes the row's ink and the meter's track role, not
  a colour of its own. Anything else is a contract every theme has to be re-validated for, to
  say something the existing pairing already says.

**2.13 There is still no sparkline.** The gap that remains, and the only one on this list whose
cost is not in the component. A meter and a staircase both report a *level*; nothing reports a
**trend**, and "is the airtime climbing" and "is this battery going to last the night" are the
two questions the Status and node screens cannot answer at all. The drawing is a polyline in a
row's height. The work is that nothing in the store keeps history: `struct mesh_ui_snapshot`
holds the present, so this wants a small sample ring — a fixed number of readings per series,
written where the telemetry lands — before there is anything to draw. Worth doing, worth doing
last, and worth being honest that it is a data change wearing a component's clothes.

### Tier 3 — worth knowing, not worth doing yet

- **Surface tiers stop at three plus two states.** M3 has five container levels. Only worth a
  fourth once §2.4 and §2.9 land and there is something that needs to sit above a card.
- **No menu, no tooltip.** Correctly absent: both are pointer affordances, and this device has a
  d-pad and four face buttons.
- **No radial gauge, and there should not be one.** The obvious answer to "where does this
  reading sit" is a dial, and it is the wrong one here twice over. `fb_internal.h` says there is
  no anti-aliasing, deliberately, because the panel is 1024 px across 3.2 inches — and a dial
  reads through a swept needle against tick marks, which are the two things that need it most; a
  staircase and a bar are axis-aligned rectangles and lose nothing. And a dial wants a square of
  screen where a list row is one line tall, so a screen of readings would become a screen of one
  reading. The banded meter says the same sentence in a row's height. "Gauge" here means a
  meter with marks on it, not a dial.
- **No FAB.** Also arguably correct. The "start a new thread" row is a list row with a plus in
  its avatar slot, which is the right answer on a device with no touch — a floating button that
  cannot be pointed at is a button that has to be reached by scrolling past everything else.

## 3. Suggested order

Each step is independently shippable and each is visible.

| # | Work | Status | Why here |
|---|---|---|---|
| 1 | Motion tokens (§1.3) | **done** | Smallest change, no layout risk, immediately felt |
| 2 | Scroll indicator (§2.1) | **done** | Highest value per line; needs nothing else |
| 3 | Spacing scale (§1.2) | **done** | Mechanical, and every later step stops adding literals |
| 4 | Type scale (§1.1) | **done** | The big one. Do it after spacing so the two land together |
| 5 | Nav bar + action bar as components (§2.2, §2.3) | **done** | Both are moves into `fb_widgets.c`; both benefit from 3 and 4 |
| 6 | Leading icon and badge, wired up (§2.14) | **done** | No new component: a slot with no caller, one only the conversation cell reaches, and two marker characters |
| 7 | Top app bar (§2.15) | **done** | Retires the breadcrumb format strings, and is where 8 and 11 land |
| 8 | Card variants and card actions (§2.4) | **done** | Where the type scale pays off most |
| 9 | Variable-height list rows (§1.4) | **done** | Structural. §1.1's unfinished half and three components below wait on it |
| 10 | Checkbox / radio, segmented button (§2.5, §2.6) | **done** (the checkbox is held, see §10) | Additive slots on components that already exist |
| 11 | Banner and screen progress (§2.9, §2.10) | **done** (the banner's table is two entries, see §11) | New surfaces; the bar from 7 is where progress hangs |
| 12 | Slider (§2.7) | **done** (and it found what a scale is not, see §12) | Genuinely new interaction |
| 13 | Screen transitions (§2.16) |  | Wants a direction on the nav first; the only step whose work is mostly outside the backend |
| — | Meter domain and bands, signal staircase (§2.11, §2.12) | **done** | Out of order on purpose: both were visible on the device and neither needed anything above |
| 14 | Sparkline (§2.13) |  | A sample ring in the store first; the component is the small half |

Steps 1 to 4 have landed. The motion tokens are `enum mesh_ui_motion` in
[`theme.h`](../include/mesh/ui/theme.h), answered by `mesh_ui_theme_motion()`; the five
per-widget duration constants are gone. The scroll rail is drawn by `fb_list_rail()` from the
list model, with its proportion arithmetic in `mesh_ui_list_scroll()`
([`layout.c`](../src/ui/layout.c)) so it is unit tested and available to a second backend. No
screen calls either: a rail is derived entirely from `count`, `first` and `visible`, so the
first row that draws puts it up.

The spacing scale is `enum mesh_ui_space` in half-steps of the glyph scale, read through
`fb_space()` / `fb_space_at()`; the half-margin panel inset it deliberately does *not* cover is
`fb_gutter()`, because that tracks the body margin rather than the text size. The type scale is
`enum mesh_ui_type` held as offsets from the body scale — offsets rather than absolutes because
a preference and `MESHCLIENT_SCALE` both override the body scale at runtime, and a table of
absolutes would stop being a scale the moment somebody asked for larger text.
`metrics.chrome_scale_down` is gone; chrome is `MESH_UI_TYPE_LABEL`.

> One thing §1.1 asked for that was **not** done at the time, and could not be done that way: a
> section heading inside a list (`MESH_UI_SETTING_HEADING`, and the node detail's group rows)
> still carried its hierarchy in colour alone. It is a list row, and `struct mesh_ui_list`
> counted rows of one fixed height — a heading drawn a step down would put the cursor and the row
> it highlights in two different places. Giving those a type role meant variable-height list
> items first, which is a change to the list model rather than to the type scale. That became
> §1.4, promoted out of this note because three later components turned out to be waiting behind
> the same thing. Step 9 closed it, though not in the shape this note expected — see §9.

Step 5 is the first that changed what a screen can *say*, and the change is mostly in the
catalog rather than in `fb_widgets.c`. The navigation bar was a move: `fb_draw_tabs()` became
`fb_draw_nav_bar()` over a reusable `fb_draw_chip_strip()`, which is the piece §2.6's segmented
button and a Nodes filter row both want and neither could reach while it was private to
`fb_screens.c`. The action bar was not a move, because there was nothing to move: the hints
were twenty-four whole sentences, and §2.2 was right that retiring them is the bulk of the
work. What replaced them is `struct mesh_ui_button_action` — a button and a verb — built by
`mesh_ui_actions_for()` in [`src/ui/actions.c`](../src/ui/actions.c), which is in the UI layer
rather than beside the backend because *which buttons mean something in a given state* is a
fact about the nav. Three consequences worth recording, because none of them was in the audit:

- **The branch that picked a hint is gone from `fb_render_snapshot()`.** It was the same chain
  of overlay tests as the branch that picks a renderer, written out twice — so a screen growing
  a press had two places to remember and no way to notice missing one. The bar is now derived
  from the snapshot, which also makes it the first piece of chrome a unit test can assert about
  (`tests/suites/ui_actions.c` walks every overlay against every screen).
- **A keycap is not a catalog id.** `mesh_ui_button_cap()` answers with what is printed on the
  case, on the same footing as a region code, and the two directional pairs are drawn from the
  font's arrows — which meant adding U+2191 and U+2193 to `font5x7.c`'s literals beside the
  horizontal pair that was already there. `MESH_UI_BUTTON_QUIT` is the one cap that is not a
  constant, because `MESHCLIENT_QUIT_KEYS` can move it somewhere with no printed name.
- **One hint lost half of itself, deliberately.** `"Left/Right/A edit"` became `←→ edit`. A bar
  names the gesture that works on every row of a settings section; A only opens something on
  the rows that have a picker or a keyboard behind them, and a keycap that sometimes does
  nothing is worse than one fewer keycap.

One thing the audit assumed and got wrong: it treated §2.2 and §2.3 as two moves of similar
size, both "moves into `fb_widgets.c`". They are not. §2.3 is a move; §2.2 is a catalog
migration with a component on the end of it, and the ratio was about ten to one. §4's rule
against a whole-sentence string id had already spotted why — it is the one rule in this document
that was written before the work it describes and turned out to be load-bearing rather than
tidy.

The bar is a little taller than the two text lines it replaced, and that was checked rather than
assumed: the Nodes tab renders the same sixteen rows before and after at the default scale. The
same reasoning as the type scale in step 4 — the body height is a floored division carrying a
remainder of most of a row, and the extra half-step above the keycaps and quarter-step under
them come out of that remainder rather than out of a row.

Steps 1 to 3 changed no screen's *content* and were reviewable as a `make ui-capture` diff
against identical scene scripts — steps 1 and 3 were pixel-identical at the default theme, which
is what a pure token extraction should be. Step 4 moves things, as a larger title must: what it
does not do, in the end, is cost a row. The body height is a floored division, so it already
carried a remainder of most of a row that the row count never included; a title that takes some
of that remainder takes nothing a list was using. `fb_draw_title()` therefore recomputes the row
count from the body's real bottom rather than deducting what it spent — deducting charged the
title for that remainder a second time and hid a row that did in fact fit. Steps 6 onwards change
what screens can *say*, so each wants its own scene.

## 4. Rules this roadmap does not get to break

- A component takes a **tone, a family or a role** — never a colour. §2.4's card variant is a
  variant, not a fill.
- A component names a **string id**, never a sentence — and a *whole-sentence* string id is not
  a way around that when the component's job is to draw the parts separately. §2.2 is the worked
  example: the hint entries were the obstacle rather than the supply, and retiring them was most
  of that step. The successor rule, now that the action bar exists: a screen names a **(button,
  verb) pair**, and a verb is a word, not a clause.
- A component names an **icon**, never a marker character. There are no exceptions left on
  screen: the pinned node's star and the padlock on a PKI-encrypted direct message were the last
  two, and §2.14 retired both. The rule's successor, from doing it: *a slot is declared for a
  list and filled by a row*, which is what the two halves of the marker gutter and the leading
  slot have in common and what `marker_slot` exists to say on a row with no label column.
- `fb_widgets.h` stays a **component set, not a seam**: it is included only from
  `src/ui/backends/`. A type scale and a spacing scale are the opposite — they belong in
  `include/mesh/ui/theme.h` with the colours, because a second backend that grows colour should
  speak them too.
- Anything added to `struct mesh_ui_metrics` must be answered by **every** theme, and
  `mesh_ui_theme_validate()` should hold it to a contract wherever a contract exists.

## 5. What the second pass found

Steps 1 to 5 landed between the two passes, so the re-audit was mostly a check that the argument
above still describes the tree. Four things it did not describe, and they are why the order in §3
moved:

- **The set has capability it is not spending.** `FB_LEADING_ICON` has no caller at all, and
  `FB_TRAILING_BADGE` had exactly one — `fb_draw_conversation()`, internally (§2.14). The
  original audit read the component set by its header, which is the one way to miss the first:
  a slot that is implemented, documented and unused reads exactly like a slot that is in use.
  The correction is worth as much as the finding. A first draft of §2.14 called both of them
  unused, which was wrong about the badge and would have mis-scoped step 6 — so the `grep` this
  wants is over every kind in every enum, and it has to count callers *inside* `fb_widgets.c`
  as well as outside, because a component that composes another component is a caller.
- **The title is a string, and that is the last sentence-shaped string id doing structural
  work** (§2.15). §4's second rule caught the button hints and did not catch this, because
  `"Settings > %s%s%s"` looks like a format rather than like a sentence. It is both.
- **The list model's fixed row height blocks more than a heading's type role** (§1.4). It was
  written down as a footnote about one unfinished half of §1.1; it is actually the prerequisite
  for a sparkline row, an in-list banner and a stacked meter row as well.
- **Motion stops at the control** (§2.16). Every duration the theme now answers for is spent
  inside a widget, and the frame-to-frame transitions are all cuts. The original audit had no
  entry for this at all, because it audited components and a transition is not one.

One thing the first pass got right and is worth restating: the ordering heuristic is *visible on
the device today, cheapest first*, and it keeps winning. The two highest-value entries this pass
found (§2.14, §2.15) are both wiring and retirement rather than new components, which is the same
shape §2.2 turned out to have.

## 6. What doing step 6 changed

Three notes worth keeping, because none of them was in either audit and each is a cost the next
entry will meet again.

- **The marker gutter was not waiting for the star.** §2.14 said it was "already reserved on
  every row of a list that declares it", and that is true of a *label/value* row, which measures
  the gutter out of `label_cols`. A node row is a plain row - `text` is the whole line - and had
  nothing to measure a gutter against, so the slot had to grow a way of being declared:
  `marker_slot`, set on every row of the list, exactly as `FB_LEADING_ICON` is. Small, but it is
  a component change rather than the struct field the entry budgeted for, and the same will be
  true of any "one cell before the words" mark on a list that is not settings-shaped.
- **An icon per settings section is twenty-two icons.** §1.5 warned that an icon is a real line
  item, and the leading slot's own callers turned out to be the two lists with twenty-five rows
  between them. Three of those rows answer with an icon another part of the UI already owns -
  "About radio" with the Status card's `RADIO`, Bluetooth and Channels with their own runes -
  because they are saying the thing that icon already says; the other twenty-two are new
  entries and one `gen-icons.py` run. The generated table went from 8 KB to about 15 KB.
- **The harness could not show either mark.** Pinning is a `mesh_ui_action` and the store stops
  there, so no scene could put a star on screen; the capture harness grew a `pin NAME` verb
  beside `offradio`, for the same reason `offradio` exists. The padlock needed nothing - the
  demo's direct messages already carry `pki_encrypted` - which is the difference between a
  fact the harness seeds and a press it cannot make. A component whose state is only reachable
  through an action needs a scene verb before it can be reviewed as a picture, and that is
  worth budgeting alongside the icon.

## 7. What doing step 7 changed

The audit budgeted this as "retires two format strings and gains a component", and the component
was the small half. Four things it did not have, and each is a rule rather than a note.

- **An overline says only what nothing else on the frame says.** The entry describes the trail as
  the breadcrumb made structural, which reads as *the same levels, in slots*. That is wrong on a
  device with a navigation bar: the outermost level of every trail here is the tab, and the tab
  strip is already drawing it, selected, three rows above. A faithful trail would have spent a
  body row saying `Settings` under a chip reading **Settings**. So the trail is the levels
  *between* the tab and this screen, which for most sections is none — and the entry's own worked
  example, `Nodes > Bravo Creek`, turns out to want no overline at all: it becomes an arrow and a
  name, and the node detail keeps all sixteen of its rows. The overline earns itself in exactly
  two places, `Modules` and `Channels`, which are the two levels the tab strip cannot name.
- **The leading slot is derived, and that is what makes it correct.** The entry calls the back
  affordance a slot, which invites a `bool back` on the struct filled in by each screen — a
  second opinion about a fact `src/ui/actions.c` already holds, and the exact shape of the bug
  §2.2 removed from `fb_render_snapshot()`. `mesh_ui_action_bar_goes_back()` reads the bar the
  action bar is about to draw, so the arrow costs one line and is right in the case a flag would
  have got wrong: a settings section holding edits offers `B` as *discard*, not as back, and the
  arrow correctly is not there. The bar and the keycap are one table or they will drift.
- **A trailing slot made §2.8 into a caller.** The badge did not need extracting for its own sake;
  it needed a second caller, and the app bar was it. `fb_draw_badge()` is the component and the
  row's `FB_TRAILING_BADGE` is now one of its two callers — which is the shape §2.14 predicted
  for a slot with one hardcoded user, arrived at from the other end.
- **The catalog lost more than it gained, as the entry promised — and one thing it did not
  spot.** Gone: `SETTINGS_TITLE_SECTION`, `SETTINGS_TRAIL_MODULES` and `NODES_TITLE_DETAIL`.
  Changed: `SETTINGS_TITLE_CHANNEL` from `"Settings > Channel %u%s"` to `"Channel %u"`, and
  `SETTINGS_UNSAVED` from `" (unsaved)"` to `"%u unsaved"` — a marker becoming a count, which is
  what a badge slot buys and a `%s` on the end of a title could not. Added: nothing. What is left
  for a later pass is `COMPOSE_SUFFIX_CHANNEL` / `_DIRECT`, which are a second pair of entries
  saying what `THREAD_KIND_CHANNEL` / `_DIRECT` already say; collapsing them is a compose-sheet
  change rather than an app-bar one, so it stayed out of this step.

One cost, stated because the next entry with an overline will meet it: **a trail costs a body
row.** It is drawn at the label scale and advanced past by the glyph body rather than by the full
line advance, which is as tight as it goes, and the body's floored-division remainder does not
cover it the way it covered step 4's larger title. That is the whole of why the rule at the top
of this list matters — the two screens that keep an overline are paying a row for it, and the
four that would have had one for free are not paying anything.

§2.9's banner and §2.10's screen progress both hang off this bar's bottom edge, and both are
unblocked by it now.

## 8. What doing step 8 changed

The entry budgeted three things — a variant enum, an action row, and a focusable card — and read
as though the first was the work and the other two were struct fields. It is the other way round.
The variant is four lines and a switch. What the other two cost is a place to put a button, an
interaction model, and one bug that only exists once a card can offer a verb.

- **An action row does not go at the bottom.** M3 puts card actions under the content and that is
  how this was first written, buttons at the body scale in a row along the card's bottom edge. It
  looked right and it cost a row per card carrying a verb — which was measured rather than
  guessed, by rendering the same scene with the calls disabled: the Status tab lost the TX queue
  and the reboot count off the end of the Radio card, which are two of the rows that card exists
  to show, on the one screen here that can outgrow its panel. A heading is three or four cells of
  a line that is otherwise empty. The verbs went into the rest of it, at the chrome scale the
  heading is drawn at and the action bar draws every other verb on the frame at, and the whole
  thing costs nothing. **On a panel with fifteen rows, a component that wants a row of its own has
  to be worth a row of content, and an action row is not.**
- **"Focusable" is not a flag, and it is not a state layer either.** The entry's wording invites a
  `bool focused` on `struct fb_card` and `MESH_UI_STATE_SELECTED` over its fill, which is what the
  palette's own rule would suggest. Both are wrong here. The flag is a second opinion about
  something the buttons already say, so focus is *derived* — a card is focused because one of its
  actions is selected — which is the same correction step 7 made about the back arrow. And the
  state layer over an area that large is a change nobody sees from across a table, while every
  tone written on the card would then owe the layered fill its own contrast contract, on each of
  three tiers. What says which card the next press acts on is an **accent edge, drawn thicker**:
  Material's focus indicator, read at a glance, and `PRIMARY` already owes both grounds 3:1.
- **The cursor walks verbs, not cards.** There was no free axis for anything else: Left and Right
  are the tab switch on every screen including this one, so a per-card cursor with Left/Right
  inside it would have had to take a gesture that works everywhere. Up and Down walk the flat
  list of verbs instead, and a card with none is stepped over — which is also why the Mesh card
  needs no "this card has no actions" anything. The one thing this costs is that a card with two
  verbs is walked vertically through a horizontal pair; no card here has two yet, and the day one
  does is the day to look at it again.
- **A card that can be empty cannot carry a verb.** Every row on the Radio card appears only when
  the radio is in some kind of trouble, so a healthy radio leaves it with no rows — and a card
  with no rows is not drawn. That was fine while it was a readout. With a verb on it, the action
  bar was naming a press whose button was nowhere on the frame and the cursor was stepping onto
  nothing. The Mesh card already had a `no report yet` row for the same reason its counters can
  be missing, so the Radio card got the same sentence with its own id. The general form is worth
  keeping: **a verb may only be offered where the thing carrying it is guaranteed to be drawn**,
  and "guaranteed" means the renderer's condition, not the model's.
- **The third variant was already on the screen, as a colour.** `radio_tone` is a reading of the
  worst thing that card holds, and the card's variant is now that same reading rather than a
  separate decision: outlined while it has nothing to report, so a quiet card recedes into the
  ground instead of spending a panel of fill saying nothing, and raised the moment the tone says
  otherwise. Link is elevated always, because it answers the screen's first question. Mesh is
  filled. That is the audit's complaint — *two cards of equal weight with nothing to say which one
  to look at* — answered by one existing fact and one line.
- **A variant is a fill, so it extends the contrast contract.** An elevated card is
  `SURFACE_HIGH` with the same headings and row tones on it, so `k_family_rules` gained
  BASE-on-`SURFACE_HIGH` at 3:1 and every family is now checked on all three grounds a card can
  be. All four shipped themes cleared it unchanged, which is what a rule looping over the
  families rather than over a hand-written list is for.

Two things the first draft got wrong, both found by review and both worth keeping written down
because the next focusable component meets them again:

- **A focus indicator must not be part of the layout.** The ring was drawn by doubling the
  card's edge, and that edge is also in the content inset and in the box height — so selecting a
  card made it taller, moved its text, pushed every card under it down the panel, and could
  change which rows were clipped, all because the cursor arrived. The painted thickness is now
  its own number and grows *inward* into the padding; the layout edge is a fact about the card,
  never about what is selected.
- **A cursor that is an index needs a list that only appends.** Refresh was offered on a synced
  radio whether or not the link was up. A client holding a cached configuration therefore offered
  refresh alone, and auto-connect arriving slid disconnect in *underneath* a cursor still sitting
  on index 0 — so a press meant to re-read the settings would have dropped the link that had just
  come up. The fix is not to remember the verb: it is to gate both verbs on the same fact, so the
  list goes empty → `[disconnect]` → `[disconnect, refresh]` and nothing is ever inserted before
  something already on it. That is also the more honest gate, because a refresh is a request over
  the air and `mesh_session_refresh_settings()` answers `-ENOTCONN` without one. A third verb here
  has to keep the invariant or the cursor has to start carrying a verb rather than an index.

One thing deliberately left: the entry's own worked example, *"Radio actions" is a list row that
opens a screen because a card cannot offer a verb*. A card can now, and that verb still is not
there — the confirmation dialog is keyed on `nav->settings_section` and takes its strings from
`settings.c`, so a reboot raised from the Status tab would mean decoupling the dialog from the
settings model first. That is a nav change rather than a component one, and it belongs with §2.9's
banner rather than here. The two verbs the cards do carry — disconnect and refresh — are both
presses that already existed on other screens, which was the point: this step gives a card
somewhere to put a verb, and a verb invented for the occasion would have been arguing two things
at once.

## 9. What doing step 9 changed

The entry called this "a layout-model change with a component set on the far side", and that is
right. What it did not have is which of its two shapes wins, what the step is *made of*, and the
fact that the model being right is not the same as the two things reading it agreeing.

- **The measure pass and the height field are not alternatives.** §1.4 presents them as a
  choice: a callback the list asks per row, or a field on `struct fb_list_item` with the model
  counting steps. The shipped answer is both halves of the second one and neither half of the
  first. The heights are an **array the caller builds and hands over** — which is a measure pass,
  just one the screen has already done for its own reasons and does not need a function pointer
  to repeat — and the model then counts steps over it. `mesh_ui_transcript_window()` had taken
  exactly that shape since the transcript was written, for exactly this reason, and the entry
  missed it because it was reading the list model rather than the file the list model is in. A
  second entry point beside an existing one is cheaper than a new mechanism, every time.
- **A field on the item would have been the bug.** The first draft did put `height` on `struct
  fb_list_item`, because the entry says to. It is a second opinion about something the window
  has already decided — the same shape as the back-arrow flag §7 refused and the `bool focused`
  §8 refused — and here it is worse than either, because the two opinions are *both used*: the
  model places the window and the item advances the y cursor, so they disagree silently and only
  when they differ. The item's height is gone; `fb_list_row_height()` asks the model, and every
  entry point that advances — the plain row, the subheader, the item — advances by that. A screen
  that forgets to declare a tall row now draws it **short**, which is visible, rather than over
  the row beneath it, which is not.
- **A step is a body row, and there is no half of one.** The entry's first bullet — a section
  heading "a step down from the rows it heads" — reads as though variable heights would make a
  heading *cheaper*. They do not, and could not: a label-scale line is about three quarters of a
  body line at the scales this ships with, so a finer step would have to be quarters, and a list
  model dividing rows into quarters to save a fifth of one is not a trade. What the type role
  actually buys the heading is the **air**: `fb_list_subheader()` draws at `MESH_UI_TYPE_LABEL`
  and sits on the *bottom* of its step, so the space the smaller glyphs free becomes the gap
  above it — which is where a section break wants its space anyway, and it costs nothing because
  the row was already that tall. §1.1's unfinished half is closed, by the type scale alone; §1.4
  was never what was standing in its way.
- **The scroll thumb was the third thing counting items, and the only one that had to be told
  twice.** `mesh_ui_list_scroll()` moved from `count`/`first`/`visible` to `total`/`first_step`/
  `used`, which for a uniform list is the same numbers and the same pixels. What it also needed
  is `last_first_step` — where the thumb runs out of travel — because on a list of mixed heights
  the window at the bottom need not be as many *items* as the one being drawn, and measuring the
  travel against the current window overshoots the end of the rail by the difference. That is
  derived once with the window rather than inside the scroll call, because it is a walk bounded
  by the window and the alternative was one bounded by the list.
- **The unit test that poked the struct was the one that broke.** `ui_layout_scroll_reports_the_
  window` set `middle.first = 15U` on a settled list to test the halfway case. That was fine
  while `first` was the only thing the offset was derived from and became a lie the moment the
  struct carried a step count beside it. It asks for the window it wants now
  (`mesh_ui_list_begin(40, 24, 10)`), which is a better test of the same thing: a test that
  reaches past the constructor is testing a state the code cannot be in.
- **The fill had to grow, and only for the bar.** A two-step item's fill deliberately stops short
  of the second line's glyph box — a glyph's ink sits high in its cell, so text stays inside it,
  and the slack is the gap between one conversation cell and the next. A bar has no such slack:
  its ink is the whole of its box, so the cursor's highlight ended a few pixels above the bar it
  was meant to be under. The fill takes the bar in explicitly. The general form: **a fill
  measured for text is not a fill measured for a shape**, and the next slot that draws a solid
  thing on a second line meets this again.

One thing left where the entry put it. §1.4 lists four things waiting on this; two are here (the
heading's type role, the stacked meter row) and two are still ahead — §2.9's in-list banner and
§2.13's sparkline. Both are now a component and a heights array rather than a layout-model
change, which was the whole point of doing this first. What neither of them has yet is a reason
for the model to answer *where inside the window* a given row sits - a `mesh_ui_list_step_offset()`
was written, went unused because every entry point advances its own y cursor, and was deleted
again. §5's finding about capability the set is not spending applies to the model too, and the
day a component places a row rather than advancing past one is the day to put it back.

And one cost, stated because §2.13 will meet it: **a stacked row costs a row.** The node detail
screen has four gauges, so it is four rows longer than it was. That is the trade §8 refused for
a card's action row and takes here, and the difference is what the row buys — an action row moved
buttons that already had a home, while eight cells against a trailing edge genuinely cannot draw
a threshold mark, and where a reading falls between its marks is the whole of what that screen is
for. A row of content is worth spending on something the screen could not otherwise say. It is
not worth spending on somewhere else to put something it already says.

## 10. What doing step 10 changed

The entry called these "additive slots on components that already exist", and for the drawing
that is exactly right: both are one new function in `fb_widgets.c` and a kind on the trailing
slot. What it did not have is that a slot is only half of a component - the other half is a
caller - and that one of the three controls it names still has no second half.

- **The checkbox is built and is not wired, on purpose.** `FB_SELECTION_CHECKBOX` exists because
  it is the same drawing as the radio with a different corner radius, and `FB_TRAILING_CHECKBOX`
  is not a kind any screen names. §5's finding is the reason: *a slot that is implemented,
  documented and unused reads exactly like a slot that is in use*, and the way to avoid adding
  another `FB_LEADING_ICON` is to say so rather than to hope. There is nothing multi-select in
  this client - §2.5's own examples, *forget these nodes* and *which channels to show*, are both
  a nav change with a component on the end of it, which is the shape step 9 was and not the shape
  this one is. The day a list can arm more than one row is the day the kind goes in.
- **The radio's caller was a correction, not an addition.** The "send to" picker already marked
  the current target - by giving that row's avatar a stated accent fill, which §2.5 spotted does
  not generalise. What it missed is that it was also *wrong for one choice*: the disc's fill is
  the node's identity, the same two letters and the same colour the conversation list and the
  Nodes tab draw, and the mark overwrote it on precisely the row the eye was hunting for.
  Identity belongs to the leading slot and selection to the trailing one. This is the same
  correction §7 made to the back arrow and §8 made to the focused card, arriving from the other
  direction: not *two opinions about one fact*, but *one slot carrying two facts*.
- **A segmented button is not a chip strip, and the difference is a measurement.** Both are
  buttons in a row and the file predicted they were one shape, which is true of the drawing. It
  is not true of the sizing: a chip is as wide as its own words, and a segment is as wide as the
  widest of them, because the members of a segmented control are *alternatives* and three boxes
  of three different widths read as three different kinds of thing. `fb_draw_chip_strip()` was
  reused for neither reason in the end - the segments want a shared container and equal shares,
  and what they actually reuse is `fb_button_width()` and `fb_draw_button()`, one layer down.
- **The type role is what made it fit at all.** Drawn at the body scale a three-valued setting
  wants more than a value column at every scale that ships, so the control would have fallen back
  to the word it exists to replace on every theme and the step would have shipped one two-valued
  setting's worth of visible change. `MESH_UI_TYPE_LABEL` is both the Material answer and the
  practical one. Its *height* stays the switch's at the row's scale, which is the distinction
  worth keeping: the labels are chrome, the control is not.
- **The slot that can come back as words.** Every other trailing kind either fits or is dropped.
  This one has a second form because a set of choices always has the chosen one in words, so
  `struct fb_segmented` carries `value` and one function answers "how many cells, and which
  form" for the measure and the draw alike. That is the same rule `fb_trailing_cols()` was
  written for, one level further in: a slot whose *form* was decided twice would disagree with
  itself exactly as one whose width was.
- **And it found the bug in that rule.** `fb_trailing_cols()` fitted a slot against the whole
  line. A headline is clipped from its tail, so a slot too wide ate the row's value and then its
  label - and the wide slots never existed to notice: a switch is four cells and a badge is two.
  A segmented button is most of a value column and reaches it at every scale. Slots are fitted
  against what is *free* now (`reserved`), which is the number the row already knew and had never
  passed on. The capture test pins it by rendering one row at two values across every theme and
  every scale and requiring the *label* column to be pixel-identical, which is the only form of
  the assertion that does not have to know where the control ended up.

  `reserved` is the **label column only**, and the first draft got that wrong in a way worth
  recording: it reserved the plain row's marker gutter too, which is spent by `fb_item_measure()`
  advancing `g.text_x` past it *before* the columns are counted - so it is already outside the
  number and reserving it again took a cell off every row with a marker slot. Two things spend
  room on a row and only one of them spends it inside `g.cols`. The label column is the one,
  because it lives in the line `fb_item_headline()` builds and nothing has counted it yet.

- **A control that shows a set has to be able to say "not one of these".** `active` outside
  `count` is a state the radio can genuinely report - an enum value from a newer firmware, or a
  corrupt one - and the settings item already keeps it and formats it as "Unknown". Clamping it
  into range, which is the obvious defensive thing to write, turns that into the panel stating a
  configuration nobody reported: `Random PIN`, lit, for a value the radio never sent. It falls
  back to the words instead, through the same exit the narrow case takes. Nor is "every segment
  unlit" the answer - a set with nothing chosen says *none of these*, which is a different false
  claim. This is §2.11's rule arriving somewhere it was not expected: **a picture cannot be wrong
  quietly**, and a control is a picture.

One thing the entry got right that is worth recording because it is unusual: this step needed no
input work at all. Left and Right already stepped an `ENUM` field, the marker gutter already
carried the pencil that says so, and the picker already had a cursor. Both components are pure
statements about state - which is why "additive slots" was the right description of the half of
the work that is in `fb_widgets.c`, and why the argument above is all about the other half.

## 11. What doing step 11 changed

The entry called these "new surfaces" and put them together because both hang off the bar from
step 7. They do hang off it, and they are otherwise not one kind of thing at all: one is chrome
that costs nothing and the other is content that costs rows, and the whole of what makes either
readable is the line between them. What the entry did not have is that line, and that most of
the work is deciding what may go in the banner rather than drawing one.

- **The bar and the banner are the moving half and the settled half of the same idea.** §2.9 and
  §2.10 read as two components with different geometry. The distinction that matters is
  temporal: the bar says something is *moving* - it costs no row, it says nothing about what,
  and it goes away on its own when the work lands - and the banner says something has *settled*
  and stays true until somebody or something resolves it, which is what makes it worth rows. A
  state that is one is never the other, and that is not a stylistic preference: it is what
  decides that the updater's three in-flight states raise the bar and its two settled ones raise
  the banner, and it is what makes the entry's own first example, *radio disconnected*, neither
  of them.
- **A banner says only what nothing else on the frame says, and that is most of the table.**
  This is §7's overline rule - *an overline says only what nothing else on the frame says* -
  arriving somewhere it was not expected, and it refuses two of the three things §2.9 asked
  for. *Radio disconnected* is on every frame already, in the status line under the keycaps,
  and a second statement of it in a container is not more visible, it is the frame contradicting
  itself about how important the fact is. *Unsaved edits* would have been the same, and is worth
  recording because the app bar's trailing badge already carries it - which is the rule
  confirming itself from the other side. What is left is the updater, and the rule pays for
  itself once more inside that: the banner stands down inside Settings > About, because the
  section it points at states the same thing in more detail and a banner over it is the client
  telling you something while you are already reading it.
- **A banner must resolve, because there is no dismissal - and dismissal is a nav change.**
  M3's banner "stays until dismissed or resolved" and the first draft budgeted a dismissal.
  It cannot be had cheaply: it needs somewhere to remember *which* banner was dismissed, a
  press to spend on it, and a rule about when it comes back - a nav change with a component on
  the end of it, which is the shape step 9 was and not the shape this one is. Refusing it turns
  out to be the more useful constraint, because it disciplines the table: nothing may be raised
  that cannot go away on its own terms. That is what refuses the radio's own `ERROR` notice,
  which nothing clears until the link cycles, and it is what makes `update_can_install` part of
  the gate rather than a detail - an update a build is not allowed to install is a container
  nothing the user does would ever clear, on every screen, for the rest of the run.
- **The one §2.9 asked for first is the one that needs something the snapshot does not carry.**
  *Radio disconnected* is refused above for saying what the status line says, and even if it
  were not, the honest version of it needs to know whether the transport is *between*
  connections - a link that drops after a settings write is expected and comes back in seconds,
  and a banner that flashed on every reconnect would be worse than none. The only thing the
  snapshot holds about that is `transport_status`, a free-form string the transports write for
  a person to read ("scanning", "waiting-for-bluez"), and comparing against it in the UI layer
  is exactly the coupling `enum mesh_str_id` exists to prevent everywhere else. So it waits on
  the transport publishing a *state* rather than a sentence, which is a transport change, not a
  component one.
- **The bar costs no row, and that had to be arranged rather than discovered.** The navigation
  bar already leaves a gap between its rule and the first body row, and the bar hangs in it - so
  the layout is `const` in that call, a list gets the same rows whether or not anything is in
  flight, and a save going out does not reflow the screen it was saved from. This is §8's
  correction about the focus ring, one component along: **an indicator that changes the layout
  is an indicator that moves what it is pointing at.** The capture test is what says so, and it
  is the same assertion read from both ends - the busy frame and the quiet frame must stop
  differing inside the top eighth of the panel, and the banner frame must differ all the way
  down. Neither half of that is a claim about a pixel, which is what lets both drawings change.
- **`layout->nav_y` is a field rather than an arithmetic.** The bar needs the bottom edge of the
  navigation bar's rule and `body_y` is a gap further down - and has moved on by the time a
  banner or an app bar has run. Deriving it by subtracting the gap would be the navigation bar's
  own arithmetic written out a second time, in a place that cannot see when the bar changes it.
  The same reasoning `layout->back` was added by in §7.
- **The banner sits above the screen's app bar, which is not where Material puts it.** A phone's
  banner goes under the top app bar because on a phone the top app bar *is* the app-level
  chrome. Here it is not: the navigation bar is, and the top app bar is the screen's own
  heading. A statement about the client goes with the first of those, so the stack is nav bar,
  progress, banner, then whatever screen is up. The practical half of the same answer is that a
  banner under the app bar would have to be called by every screen renderer and by each of the
  four overlays, which is the duplication `fb_render_snapshot()`'s single tail exists to prevent.
- **Nothing about the banner animates, and that follows from it costing rows.** A container that
  eased its height open would reflow the list underneath it for the length of the animation. The
  snackbar animates because it arrives *over* the UI and has to be noticed to be read; a banner
  is read whenever the eye next reaches the top of the panel, which on a handheld is every time
  the screen changes.
- **The bar is the meter, and the version is not in the words.** Two reuses worth recording
  because both were the alternative to a new thing. The bar is `fb_draw_meter()` at
  `FB_METER_INDETERMINATE`, full bleed and one hairline tall - there is exactly one "a thing is
  working" motion in this UI, and a second travelling pill is a second one to keep in step with
  the theme's timings. And the banner's version number is a *slot*, drawn against the trailing
  edge of the headline at the label scale, rather than a `%s` inside the sentence: the same
  split the app bar made when `"Settings > %s%s%s"` became a trail, a title and a badge. It
  recedes by size rather than by colour, which is the type scale doing a job a second, unvalidated
  ink would otherwise have been invented for.

- **The bar is the first animated thing above `body_y`, and it needed no new machinery to be
  one.** The partial-composition work that landed alongside this
  (`fb_animation_damage()`, `state->clip`) re-composes a frame clipped to whatever the animated
  widgets declared, and it is entered exactly when the snapshot has stopped changing - which is
  the bar's whole life. Because the bar *is* `fb_draw_meter()`, it declares that region already:
  reuse paid a second time, in a mechanism that did not exist when it was chosen.
  `fb_progress_clip_matches_full_composition` pins it, because the existing clip test runs on a
  snapshot with no radio attached and so never draws one.

One thing the audit had right and worth repeating: §2.10's example is exact. Open Settings
before the radio has answered and eight sections say `not loaded`, which reads identically
whether a request is on its way back or nothing was ever sent. One hairline under the tab strip
is the difference, and because it is chrome it answers that for every screen at once rather than
for the one that happened to be waiting - `admin_busy`, `write_pending`, the config handshake and
an update check all raise it, and no screen had to be told.

And one cost, stated for §2.13, which will meet it: **the banner's gate on `config_complete` had
to be read against a connected radio, not on its own.** The node roster deliberately outlives the
connection, so a client sitting on a cached roster with no radio has an incomplete handshake for
as long as it runs - and a bar that never stopped would be a bar that had stopped saying
anything. Every derived indicator that reads a handshake field meets this, because the roster
surviving a disconnect is the one piece of state in this client that is deliberately older than
the link it came from.

## 12. What doing step 12 changed

The entry called this "genuinely new interaction", and that is the one thing it is not. Left and
Right already stepped a `NUMBER` field, the marker gutter already carried the pencil that says
so, and nothing about input changed - exactly as in step 10, and for the same reason: the
control is a *statement about state*, and the press that changes the state was already there.
What was new is that this is the first component whose correctness is a question about the data
rather than about the drawing, and the drawing was finished long before the answer was.

- **The slider is the meter's sibling and is not a variant of it.** §2.7 read them as one shape
  with a knob added, which is true of the pixels. The distinction that matters is the same
  temporal one §11 found between the progress bar and the banner, one level down: a meter reports
  a level *being told to you* and eases towards each sample; a slider reports a value you are
  *choosing between others*, marks the others on its own track, and has a focused state because
  there is a cursor on it. A component with a state the other has no word for is a second
  component.
- **It went on the second step, and the trailing slot was never in the running.** The rule that
  slot was written with settles it: *inline is for a figure the eye passes; a step is for one it
  stops on*. A settings control is by definition the second kind. The measurement says the same
  thing - eight cells could not carry a band's two boundary marks, and this needs a dozen stops.
  The cost is real and was accepted: a section of durations is now half as many rows on screen,
  which is what the node detail paid in step 9 for exactly the same reason.
- **What a preset list *is* cannot be derived from it, and that is the whole of the step.** `{0,
  1, 2, 3, 4, 5, 6, 7}` is a hop limit under one field and a GPIO pin under the next. A length
  drawn across the second says a pin is two thirds of the way to being a pin, which is §2.11's
  rule - *a picture cannot be wrong quietly* - meeting a component that has no reading of its
  own to check against. So every `NUMBER` field states which it is, in the same table entry that
  states its presets, and the five lists that name things rather than measure them (a spreading
  factor, a bandwidth, a coding rate, a pin, a count of coordinate bits) say so. The LoRa
  section is where the argument is visible in one frame: five numeric rows, two with a track
  under them and three without.
- **And the harder half is that a list can be a scale with one value that is not on it.** This
  is what the first version shipped wrong, and it was visible in the first capture: LoRa's
  transmit power reads `max` at 0, drawn with its handle hard left - at the *empty* end of its
  own bar, reporting the opposite of what it says. Every `default` is the same mistake more
  quietly: a screen timeout the firmware picks is not the shortest one this client offers, it is
  an interval nobody here knows. So `SCALE_PRESETS_AFTER_ZERO()` stands that value outside the
  track, the scale is what follows it, and such a value draws the stops with no handle anywhere -
  §10's *a control that shows a set has to be able to say "not one of these"*, arriving on an
  axis. An empty track is not ambiguous with a value at the minimum, because a value at the
  minimum has a handle sitting on it.
- **Stop space, not value space.** These lists climb geometrically, so a handle at `value/max`
  would crowd eight of screen-on's ten choices into the first sixth of the track. What is being
  chosen between is the choices, so the stops are evenly spaced. That immediately buys the thing
  the segmented button could not have: a value *between* two stops is interpolated rather than
  refused, because an axis has room between its members and a set of alternatives does not. A
  radio reporting 42 seconds - a firmware default, a phone app with a different list - lands
  where 42 seconds is.
- **The height comes from the field, never from the value.** `unplaced` is the only state in the
  client where a row would otherwise want a different height, and the tempting thing to write -
  no slider, so no second step - reflows the section under the cursor the instant somebody
  presses Right off `default`. It is step 10's rule about the row count under the cursor, a step
  further in, and it is the assertion the capture test exists for: the two frames must differ in
  the row and be pixel-identical below it. Neither half is visible in a screenshot taken one
  value at a time.
- **The measurement is one function, asked twice.** `settings_row_slider()` answers "does this
  row draw one, and where does the handle go" for the height pass and for the draw alike. This
  is `fb_trailing_cols()`'s rule one component along, and here the two answers would have been a
  *step* apart rather than a cell - a control drawn into a step the list never reserved, over the
  row beneath it.
- **The screen had to be able to measure at all, which cost an accessor and refunded it.**
  `fb_render_settings()` asked `mesh_ui_settings_item()` row by row, and that call rebuilds the
  section from the radio's config every time - so a screen of sixteen rows built its own section
  sixteen times. Measuring needs every row before the first is placed, which is
  `mesh_ui_node_detail_build()`'s shape, so `mesh_ui_settings_items()` fills the array once. The
  step that needed the batch is the step that made the screen cheaper.
- **Nothing new was needed to draw it.** The track is `MESH_UI_COLOR_METER_TRACK`, the fill is a
  tone through the same `fb_meter_tone()` fallback, the stops are notches cut out in the ground
  colour - a band boundary's own mark, for a reason that transfers exactly: a gap reads the same
  over the fill as over the track, where an ink of its own would be two more contracts per theme
  to say what an absence already says. The one thing measured rather than reused is the handle,
  which is narrower than its track is thick because it marks a *position* and a wide one is a
  range, and which stands taller under the cursor inside a box that always reserves the taller
  size - §8's focus ring rule, since a control that grew on focus would move the rows below it.

What remains of §3 is step 13 (screen transitions) and step 14 (the sparkline), and both are
still what the audit said they were: the first is mostly nav work, and the second is a data
change wearing a component's clothes.
