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

`fb_screens.c` is correspondingly thin. Across the whole file there is one `fb_fill_rect` and
there are two `fb_draw_text` calls, and all three are inside the two pieces of chrome that never
became components (§2.2 and §2.3). The only renderer that still names a raw glyph scale is the
on-screen keyboard, which is laying out a grid. Everything else is content.

So this is not a rewrite. It is: **three tokens the theme does not answer for yet**, and
**a handful of components whose absence is visible on screen right now.**

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

**2.9 No persistent inline banner.** The snackbar is transient by design and correctly so. But
*radio disconnected*, *firmware mismatch* and *update available* are persistent states, and they
currently become a dim card note or a word in the footer. M3's banner sits under the app bar and
stays until dismissed or resolved. This is the component the Status tab's Link card is
half-imitating.

**2.10 No screen-level progress.** There is an indeterminate meter, but nothing draws a thin
linear indeterminate bar under the nav bar during handshake or sync. Open Settings before the
radio has answered and eight rows say `not loaded` with no sign that anything is happening. One
bar under the tab strip answers that for every screen at once.

### Tier 3 — worth knowing, not worth doing yet

- **Surface tiers stop at three plus two states.** M3 has five container levels. Only worth a
  fourth once §2.4 and §2.9 land and there is something that needs to sit above a card.
- **No menu, no tooltip.** Correctly absent: both are pointer affordances, and this device has a
  d-pad and four face buttons.
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
| 5 | Nav bar + action bar as components (§2.2, §2.3) |  | Both are moves into `fb_widgets.c`; both benefit from 3 and 4 |
| 6 | Card variants and card actions (§2.4) |  | Where the type scale pays off most |
| 7 | Checkbox / radio, segmented button (§2.5, §2.6) |  | Additive slots on components that already exist |
| 8 | Banner, screen progress, standalone badge (§2.8–2.10) |  | New surfaces; want the fourth tier decided first |
| 9 | Slider (§2.7) |  | Genuinely new interaction; do it last |

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
> is a change to the list model rather than to the type scale.

Steps 1 to 3 changed no screen's *content* and were reviewable as a `make ui-capture` diff
against identical scene scripts — steps 1 and 3 were pixel-identical at the default theme, which
is what a pure token extraction should be. Step 4 was the exception and always would be: a title
a step larger is a title that takes more of the panel, so every screen with one gives up a body
row. That was the trade the step was for. Steps 5 onwards change what screens can *say*, so each
wants its own scene.

## 4. Rules this roadmap does not get to break

- A component takes a **tone, a family or a role** — never a colour. §2.4's card variant is a
  variant, not a fill.
- A component names a **string id**, never a sentence — and a *whole-sentence* string id is not
  a way around that when the component's job is to draw the parts separately. See §2.2, where the
  existing hint entries are the obstacle rather than the supply.
- A component names an **icon**, never a marker character.
- `fb_widgets.h` stays a **component set, not a seam**: it is included only from
  `src/ui/backends/`. A type scale and a spacing scale are the opposite — they belong in
  `include/mesh/ui/theme.h` with the colours, because a second backend that grows colour should
  speak them too.
- Anything added to `struct mesh_ui_metrics` must be answered by **every** theme, and
  `mesh_ui_theme_validate()` should hold it to a contract wherever a contract exists.
