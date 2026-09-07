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

> Raised out of §3's closing note, where it sat as a caveat on the type scale. It is the last
> structural gap in the layout model, and four things below are waiting behind it.

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

`include/mesh/ui/icons.def` holds twenty-three entries, and four of the components below name a
glyph that is not among them: §2.5's checkbox and radio, §2.9's banner (an `info` and an
`error`), §2.15's back affordance, §2.14's star. Adding one is not a line of code. It is a line
in the `.def`, then `scripts/gen-icons.py` against a Material Symbols Rounded font that is not
in this tree, then committing the regenerated `src/ui/icon_glyphs.c` — a step CI cannot run and
a contributor without the font cannot run either (`CLAUDE.md` says as much, under the generators
that are not part of the build).

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

**2.7 No slider.** `NUMBER` settings step with Left/Right and report a figure. The meter already
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
| 6 | Leading icon and badge, wired up (§2.14) | badge **done** | No new component: a slot with no caller, one only the conversation cell reaches, and two marker characters |
| 7 | Top app bar (§2.15) |  | Retires the breadcrumb format strings, and is where 8 and 11 land |
| 8 | Card variants and card actions (§2.4) |  | Where the type scale pays off most |
| 9 | Variable-height list rows (§1.4) |  | Structural. §1.1's unfinished half and three components below wait on it |
| 10 | Checkbox / radio, segmented button (§2.5, §2.6) |  | Additive slots on components that already exist |
| 11 | Banner and screen progress (§2.9, §2.10) |  | New surfaces; the bar from 7 is where progress hangs |
| 12 | Slider (§2.7) |  | Genuinely new interaction |
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

> One thing §1.1 asked for that is **not** done, and cannot be done this way: a section heading
> inside a list (`MESH_UI_SETTING_HEADING`, and the node detail's group rows) still carries its
> hierarchy in colour alone. It is a list row, and `struct mesh_ui_list` counts rows of one
> fixed height — a heading drawn a step down would put the cursor and the row it highlights in
> two different places. Giving those a type role means variable-height list items first, which
> is a change to the list model rather than to the type scale. That is now §1.4, promoted out of
> this note because three later components turned out to be waiting behind the same thing.

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
- A component names an **icon**, never a marker character. Two exceptions are still on screen
  and they cost differently (§2.14): the pinned node's star is a literal in a list row's text,
  and a marker gutter is already waiting for it; the padlock on a PKI-encrypted direct message
  is a literal in a bubble's meta line, which has no slots to move it into.
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
