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

The theme answers for colour, for shape and for metrics. It does not answer for type. What it
has is two numbers — `metrics.scale` for the body and `metrics.chrome_scale_down` for the tab
strip and the footer — and everything on screen is drawn at one of them.

Look at the Status tab: the screen title, a card heading, a card label, a card value and the
footer hint are all the same size, and the entire visual hierarchy is carried by colour. That is
why a column of cards reads as a wall of text with tinted headings rather than as a set of
grouped panels. M3 gets its hierarchy from a type scale first and colour second, and so does
every desktop UI that reads as modern.

The fix is the move this codebase has already made twice. A renderer should name a *type role* —
`MESH_UI_TYPE_TITLE`, `MESH_UI_TYPE_LABEL`, `MESH_UI_TYPE_BODY`, `MESH_UI_TYPE_SUPPORTING` — and
`theme.c` should answer with a scale, exactly as it answers a colour role with an RGB and a shape
with a radius. The table lives in `struct mesh_ui_metrics` beside `shape[]`, adding a theme stays
a table entry, and `fb_draw_title()` stops being the only thing on screen that is bigger than
everything else by accident of being the only caller that passes `state->scale`.

This is the single largest structural gap, and it is the one that would most change how the UI
looks.

> A caution the font layer imposes: the glyph scale is an integer multiplier over a 5x7 cell, so
> a type scale here is a scale *of steps*, not of points, and the useful range is narrow. That is
> an argument for four or five roles, not for M3's fifteen.

### 1.2 There is no spacing scale

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

**2.1 No scroll indicator.** The Nodes tab says `Nodes (8 of 42)` in its title because the title
is the only place it can say it. There is no scroll rail anywhere in the tree — `grep -rn scroll
src/ui/backends` finds only comments. `struct mesh_ui_list` already carries `count`, `first` and
`visible`, so a rail is three numbers it is already holding and a rounded rect. This is the
highest ratio of "reads as a modern list" to lines of code in the whole audit, and it removes the
count from the title as a side effect.

**2.2 The footer is not a component.** `fb_draw_footer()` is static in `fb_screens.c` and draws
two lines of plain text: `A open node  X pin  Y write  L/R tabs`, then the link summary. It is
the last screen-level renderer that lays out its own pixels, and it is also the piece of chrome
that most makes the UI read as a terminal rather than as a handheld OS. The button component
already draws a keycap (`FB_BUTTON_FILLED` at `MESH_UI_SHAPE_SM` is exactly one); a bottom action
bar of keycap-plus-label pairs is that component in a loop. Both DRY and aesthetics point the
same way here.

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

| # | Work | Why here |
|---|---|---|
| 1 | Motion tokens (§1.3) | Smallest change, no layout risk, immediately felt |
| 2 | Scroll indicator (§2.1) | Highest value per line; needs nothing else |
| 3 | Spacing scale (§1.2) | Mechanical, and every later step stops adding literals |
| 4 | Type scale (§1.1) | The big one. Do it after spacing so the two land together |
| 5 | Nav bar + action bar as components (§2.2, §2.3) | Both are moves into `fb_widgets.c`; both benefit from 3 and 4 |
| 6 | Card variants and card actions (§2.4) | Where the type scale pays off most |
| 7 | Checkbox / radio, segmented button (§2.5, §2.6) | Additive slots on components that already exist |
| 8 | Banner, screen progress, standalone badge (§2.8–2.10) | New surfaces; want the fourth tier decided first |
| 9 | Slider (§2.7) | Genuinely new interaction; do it last |

Steps 1–4 change no screen's content and should be reviewable as a `make ui-capture` diff with
identical scene scripts. Steps 5 onwards change what screens can say, so each wants its own
scene.

## 4. Rules this roadmap does not get to break

- A component takes a **tone, a family or a role** — never a colour. §2.4's card variant is a
  variant, not a fill.
- A component names a **string id**, never a sentence. The action bar in §2.2 names the same
  catalog entries the footer hints already use.
- A component names an **icon**, never a marker character.
- `fb_widgets.h` stays a **component set, not a seam**: it is included only from
  `src/ui/backends/`. A type scale and a spacing scale are the opposite — they belong in
  `include/mesh/ui/theme.h` with the colours, because a second backend that grows colour should
  speak them too.
- Anything added to `struct mesh_ui_metrics` must be answered by **every** theme, and
  `mesh_ui_theme_validate()` should hold it to a contract wherever a contract exists.
