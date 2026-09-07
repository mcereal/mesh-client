#ifndef MESH_UI_THEME_H
#define MESH_UI_THEME_H

/*
 * Themes: the colours, the metrics and the font a frame is drawn with.
 *
 * A renderer never names a colour. It names a *role* - "the ground", "the fill under the
 * cursor", "text on an accent fill" - or a *tone*, which is the same idea one level up: what a
 * piece of text means, rather than what colour it is. The theme answers, so a new look is a
 * table in src/ui/theme.c and nothing else, and every screen switches together because none of
 * them holds an opinion of its own.
 *
 * The same goes for geometry. The margin, the glyph multiplier, how much smaller chrome text
 * is, how wide a bubble may grow, when the label column gives way on a narrow body: those were
 * literals scattered through the drawing code, and a theme that wanted a roomier layout had to
 * find all of them. They are `struct mesh_ui_metrics` now.
 *
 * Nothing here knows about the framebuffer. That is deliberate: tones and metrics are the
 * vocabulary of *the UI*, and a second backend that grows colour (a colour CLI, an SDL window
 * on a desktop) should speak it too rather than inventing a parallel one.
 */

#include "mesh/ui/font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_rgb {
    uint8_t r, g, b;
};

/*
 * Every colour a frame can use, named for the job it does.
 *
 * Adding a role is how a new visual element gets themed. Adding it means every theme answers
 * for it, which is the point - a role that only one theme fills is a hardcoded colour with
 * extra steps.
 */
enum mesh_ui_color {
    MESH_UI_COLOR_BG = 0, /* the ground the whole frame is cleared to */
    /*
     * The three surface tiers, lowest first.
     *
     * A surface says how far a thing is from the ground, and the whole point of having more
     * than one is that a panel over a panel has to be tellable from it. The Brick's display
     * engine composites fb0 against its own background layer rather than against what we have
     * already drawn, so there is no alpha to shade with and no drop shadow to cast - the
     * distance is carried by the fill alone, which is how Material's tonal elevation works and
     * why it survives a light palette as well as a dark one.
     *
     * Lower is nearer the ground, so on a dark theme the tiers get lighter as they rise and on
     * a light one they get darker. A theme states all three and nothing that draws knows which
     * direction its palette went.
     */
    MESH_UI_COLOR_SURFACE_LOW,    /* recessed chrome: the ground the tab strip sits on */
    MESH_UI_COLOR_SURFACE,        /* a panel on the ground: a card */
    MESH_UI_COLOR_SURFACE_HIGH,   /* raised over the body: the keyboard's draft box */
    MESH_UI_COLOR_SURFACE_SEL,    /* the fill under the cursor, and a button at rest */
    MESH_UI_COLOR_SURFACE_ACTIVE, /* a pressed button */
    /*
     * The surface from the other end of the palette: a light fill on a dark theme, a dark one
     * on a light theme.
     *
     * The three tiers above say how far a thing is from the ground, which works for anything
     * that belongs to the screen it is on. A transient notice does not - it is over the whole
     * UI, it was not there a second ago and will not be there in four - and no tier can say
     * that on a panel with no shadow and no alpha to raise it with. Inverting the ground can:
     * a fill that reads as "not part of this screen" is found before it is read, which is the
     * whole job of a snackbar.
     *
     * Material calls this pair inverse-surface and inverse-on-surface, for the same reason and
     * with the same one user.
     */
    MESH_UI_COLOR_SURFACE_INVERSE,
    MESH_UI_COLOR_TEXT,        /* body text */
    MESH_UI_COLOR_TEXT_DIM,    /* headings, secondary lines, anything not yet loaded */
    MESH_UI_COLOR_TEXT_STRONG, /* unread, unsaved: the row the eye should land on */
    MESH_UI_COLOR_TEXT_ON_SEL, /* text drawn on SURFACE_SEL or SURFACE_ACTIVE */
    /* The secondary line of a selected item: a timestamp, a message preview. TEXT_DIM is
       chosen against the ground and says nothing about a fill over it, so a row that carries
       two tiers of text under the cursor - which the conversation list does - needs its own
       quiet colour rather than flattening to TEXT_ON_SEL. */
    MESH_UI_COLOR_TEXT_ON_SEL_DIM,
    MESH_UI_COLOR_TEXT_ON_INVERSE, /* text drawn on SURFACE_INVERSE */
    MESH_UI_COLOR_ACCENT,          /* titles, actions, channels, the current target */
    MESH_UI_COLOR_ON_ACCENT,       /* text drawn on an accent fill: the unread badge */
    /*
     * The accent's quiet half: a fill that says "this one" without shouting it, and the ink
     * that goes on it.
     *
     * A full accent fill is the right answer for a badge, which is small and has to be found
     * across the panel. It is the wrong one for anything the size of a tab, where a block of
     * saturated colour under a label becomes the loudest thing on the screen and the label
     * stops being read. The container is the same hue held back far enough to sit behind text -
     * which is what a selected tab, a tonal button and an assist chip all want.
     */
    MESH_UI_COLOR_ACCENT_CONTAINER,
    MESH_UI_COLOR_ON_ACCENT_CONTAINER,
    MESH_UI_COLOR_GOOD,        /* connected, healthy */
    MESH_UI_COLOR_BAD,         /* disconnected, failed, armed to destroy something */
    MESH_UI_COLOR_RULE,        /* hairline separators */
    MESH_UI_COLOR_RULE_STRONG, /* the rule under the tab strip */
    /*
     * The edge of a container, which is not the same job as a separator.
     *
     * A rule divides content that is already on one surface; an outline is what says a surface
     * is there at all. They were one role while a card was the only thing with an edge, and
     * they part company the moment a second container wants an edge that reads against a
     * different fill: a rule is tuned to disappear politely, an outline has to be found.
     */
    MESH_UI_COLOR_OUTLINE,
    /*
     * The empty part of a meter: the container a fill is read against.
     *
     * A role of its own because it is the one colour with a contract in *both* directions, and
     * no existing role can hold both ends of it. It has to be findable on the two grounds a bar
     * is drawn on - the body and a card - and every fill a meter can take has to be tellable
     * from it. SURFACE_SEL was the obvious borrow and fails the first half on the light theme,
     * where the cursor fill is within 1.2:1 of a card; OUTLINE passes that and fails the second
     * half on the contrast theme, where it is the same near-white as the good tone. A track
     * borrowed from a role tuned for something else is a bar that disappears on whichever theme
     * nobody happened to open.
     *
     * Quiet is the goal, not contrast: this is the part of the widget that is meant to recede,
     * and mesh_ui_theme_validate() is what stops quiet becoming absent.
     */
    MESH_UI_COLOR_METER_TRACK,
    MESH_UI_COLOR_BUBBLE_IN,     /* a message from someone else */
    MESH_UI_COLOR_BUBBLE_OUT,    /* one of ours */
    MESH_UI_COLOR_BUBBLE_IN_SEL, /* the same two under the cursor */
    MESH_UI_COLOR_BUBBLE_OUT_SEL,
    MESH_UI_COLOR_BUBBLE_FAILED, /* the radio said it did not get there */
    MESH_UI_COLOR_TEXT_INBOUND,  /* text on an inbound bubble */
    MESH_UI_COLOR_TEXT_OUTBOUND, /* text on one of ours */
    MESH_UI_COLOR_COUNT
};

/*
 * What a piece of content *is*, rather than which colour to draw it.
 *
 * This is the vocabulary screens speak. It is a level above the roles above: a screen says
 * "this row is bad news" and the theme decides both which role that maps to and what colour
 * the role holds. Same reason a stylesheet has a token called `danger` instead of a hex.
 */
enum mesh_ui_tone {
    MESH_UI_TONE_NORMAL = 0,
    MESH_UI_TONE_DIM,
    MESH_UI_TONE_STRONG,
    MESH_UI_TONE_ACCENT,
    MESH_UI_TONE_GOOD,
    MESH_UI_TONE_BAD,
    MESH_UI_TONE_INBOUND,
    MESH_UI_TONE_OUTBOUND,
    MESH_UI_TONE_COUNT
};

/* The glyph multipliers the UI will accept, whatever a theme asks for. The lower bound is
   legibility on the Brick's 3.2" panel; the upper is the buffers sized off it. */
#define MESH_UI_SCALE_MIN 2
#define MESH_UI_SCALE_MAX 6

/*
 * Avatar tints: the fills behind a conversation's initials.
 *
 * A palette rather than a role because the whole point of an avatar colour is that two of them
 * differ - it is what lets the eye find a thread in a list without reading a word of it, the
 * way every messenger's coloured discs do. Which tint a conversation gets is a hash of its
 * identity, so it is stable across restarts and across a rename.
 *
 * A theme may fill fewer than the maximum (`avatar_count`); the high-contrast one deliberately
 * offers two, because a palette of six hues is the opposite of what that theme is for.
 *
 * The initials are drawn in MESH_UI_COLOR_BG - a tint is a fill punched out of the ground - so
 * every tint owes the ground the body-text contrast, and mesh_ui_theme_validate() holds it to
 * that. Same contract as ACCENT/ON_ACCENT, one level up.
 */
#define MESH_UI_AVATAR_TINTS 6U

/*
 * The shape scale: how round a container's corners are, by what kind of container it is.
 *
 * A renderer no more names a radius than it names a colour. It names a shape - "this is a
 * pill", "this is a panel" - and the theme answers, which is what makes a squarer or a rounder
 * look one table entry instead of a hunt through every fill in the backend. It is the same
 * move as `enum mesh_ui_tone`, one axis over.
 *
 * The steps are in *glyph-scale multiples*, not pixels, for the reason `card_pad` is: a theme
 * asking for bigger text gets proportionally rounder corners, so the whole frame stays in
 * proportion rather than the corners staying put while everything around them grows.
 */
enum mesh_ui_shape {
    MESH_UI_SHAPE_NONE = 0, /* square: a rule, a bar, anything that meets an edge */
    MESH_UI_SHAPE_SM,       /* a row highlight, a keycap - a shape the eye reads as a rectangle */
    MESH_UI_SHAPE_MD,       /* a panel: a card, a text field */
    MESH_UI_SHAPE_LG,       /* a surface over the body: a dialog, a sheet */
    /* As round as the shorter side allows - a capsule, or a circle when it is square. A badge,
       a chip, an avatar. Not a step count, so it has no entry in the table below: "half of
       whatever this turns out to be" is not a length a theme can state in advance. */
    MESH_UI_SHAPE_FULL,
    MESH_UI_SHAPE_COUNT
};

/*
 * The geometry a theme owns.
 *
 * Everything here was a literal in a drawing function once. They are theme data because a
 * "large text" theme is exactly this struct with a different `scale`, and a roomier one is a
 * different `margin` - neither should need a renderer to be touched.
 */
struct mesh_ui_metrics {
    uint8_t margin;            /* pixels between the panel edge and the body */
    uint8_t scale;             /* glyph multiplier for body text */
    uint8_t chrome_scale_down; /* steps smaller the tab bar and footer are drawn */
    uint8_t bubble_width_pct;  /* how much of the body a chat bubble may fill */
    uint8_t field_label_cols;  /* preferred label column, in cells */
    uint8_t narrow_cols;       /* a body narrower than this halves the label column */
    /* A card's inset, in glyph-scale steps rather than in pixels: a theme that asks for bigger
       text gets a proportionally roomier card, the same way the switch and the chat bubble
       already grow with the scale. */
    uint8_t card_pad;
    /*
     * How thick a meter's track is, in glyph-scale steps.
     *
     * A bar is the one widget here whose whole job is to be read without being looked at, so
     * its thickness is the difference between "a line the eye finds in a column of text" and
     * "an underline somebody forgot to remove". One step - four pixels at the device's scale -
     * is Material's 4dp track at the size this panel actually is, and it grows with the glyph
     * scale like everything else so a theme asking for bigger text gets a bar to match.
     */
    uint8_t meter_thickness;
    /* The shape scale, in glyph-scale steps, indexed by enum mesh_ui_shape. Sized to stop
       before MESH_UI_SHAPE_FULL because that one is not a step count - see the enum. Read it
       through mesh_ui_theme_radius(), which does the multiply and handles the pill. All zeroes
       is a legal, entirely square theme. */
    uint8_t shape[MESH_UI_SHAPE_FULL];
};

struct mesh_ui_theme {
    const char *id;      /* what MESHCLIENT_THEME and a saved preference name it by */
    const char *name;    /* what a menu would show */
    const char *font_id; /* resolved against the font registry; NULL means the default */
    bool dark;           /* whether the ground is darker than the text; nothing draws
                            differently for it, but a caller choosing a default cares */
    struct mesh_ui_rgb colors[MESH_UI_COLOR_COUNT];
    /* Avatar fills, read through mesh_ui_theme_avatar(). Only the first `avatar_count` are
       used, so a theme states its palette and leaves the rest zeroed. */
    struct mesh_ui_rgb avatars[MESH_UI_AVATAR_TINTS];
    uint8_t avatar_count;
    struct mesh_ui_metrics metrics;
};

/* The registry. Index order is menu order, and `dark` is index 0. */
size_t mesh_ui_theme_count(void);
const struct mesh_ui_theme *mesh_ui_theme_at(size_t index);
const struct mesh_ui_theme *mesh_ui_theme_by_id(const char *id); /* NULL when unknown */
const struct mesh_ui_theme *mesh_ui_theme_default(void);

/*
 * The theme MESHCLIENT_THEME names, or the default when it is unset or names nothing.
 *
 * An unknown name warns rather than failing: a typo in an environment variable should not
 * leave a handheld with no UI.
 */
const struct mesh_ui_theme *mesh_ui_theme_from_env(void);

/*
 * The same, but NULL when the environment did not name a theme this layer knows.
 *
 * The difference matters to whoever owns the choice: an environment variable is a deliberate
 * override, so when it names a theme the Settings row shows it as a fact rather than as a
 * switch that would spring back. Same shape as the updater's MESHCLIENT_UPDATE_ALLOW_DEV.
 */
const struct mesh_ui_theme *mesh_ui_theme_env(void);

/* The theme `id` names, or the default. What a saved preference is read back through. */
const struct mesh_ui_theme *mesh_ui_theme_resolve(const char *id);

/* The next theme in registry order, wrapping - the Settings row steps through with this. */
const struct mesh_ui_theme *mesh_ui_theme_next(const struct mesh_ui_theme *theme);

/* Lookups. A NULL theme resolves to the default, so no caller has to guard. */
struct mesh_ui_rgb mesh_ui_theme_color(const struct mesh_ui_theme *theme, enum mesh_ui_color role);
struct mesh_ui_rgb mesh_ui_theme_tone(const struct mesh_ui_theme *theme, enum mesh_ui_tone tone);
enum mesh_ui_color mesh_ui_tone_role(enum mesh_ui_tone tone);

/*
 * The tone a fraction of some budget has earned: GOOD below `warn`, ACCENT from there up to
 * `bad`, and BAD at or above it. All three arguments are permille, the same scale a meter's
 * value is on (MESH_UI_ANIM_ONE).
 *
 * It is here, in the UI's vocabulary, rather than in whichever screen first needed it, because
 * two things now say the same sentence about one number: the airtime figure is coloured by it
 * and the meter beside the figure is filled by it. A screen that worked its own thresholds out
 * for the text and handed a bar something else would be drawing a number and a picture that
 * disagree - and the picture is the one people will believe.
 *
 * Thresholds are the caller's because they are domain facts, not palette ones: 25% of the air
 * is a busy mesh, 25% of a download is a slow start.
 */
enum mesh_ui_tone mesh_ui_tone_for_load(int32_t permille, int32_t warn, int32_t bad);
const struct mesh_ui_font *mesh_ui_theme_font(const struct mesh_ui_theme *theme);

/*
 * The avatar fill for `seed`, which is whatever identifies the conversation - a node number, a
 * channel index. Any seed answers: it is reduced into the theme's palette here, so no caller
 * has to know how many tints a theme offers.
 */
struct mesh_ui_rgb mesh_ui_theme_avatar(const struct mesh_ui_theme *theme, uint32_t seed);
const struct mesh_ui_metrics *mesh_ui_theme_metrics(const struct mesh_ui_theme *theme);

/* The theme's glyph multiplier, clamped into [MESH_UI_SCALE_MIN, MESH_UI_SCALE_MAX]. */
int mesh_ui_theme_scale(const struct mesh_ui_theme *theme);
/* The multiplier chrome is drawn at, given the body's. Never below the minimum. */
int mesh_ui_theme_chrome_scale(const struct mesh_ui_theme *theme, int scale);
/* Clamps any multiplier into the accepted range; 0 or less means "the theme's own". */
int mesh_ui_theme_clamp_scale(const struct mesh_ui_theme *theme, int scale);

/*
 * The corner radius `shape` wants, in pixels, at glyph multiplier `scale`.
 *
 * MESH_UI_SHAPE_FULL answers with a number larger than any panel, because the fill primitive
 * clamps a radius to half the shorter side anyway: "as round as it goes" is the one shape that
 * cannot be measured until the box it is applied to is known, and the clamp is where that is
 * already known. Every other shape is its step count times the scale.
 */
int mesh_ui_theme_radius(const struct mesh_ui_theme *theme, enum mesh_ui_shape shape, int scale);

/*
 * The WCAG contrast ratio between two colours, from 1.0 (identical) to 21.0 (black on white).
 *
 * The arithmetic is the standard one: undo the display's gamma on each channel, weight them by
 * how much the eye gets from each (green most, blue least) to get a relative luminance, then
 * compare the lighter against the darker. It is here rather than in a test because it is what
 * makes "is this theme readable?" a question with an answer - see mesh_ui_theme_validate().
 */
double mesh_ui_theme_contrast(struct mesh_ui_rgb a, struct mesh_ui_rgb b);

/*
 * Whether a theme's text is readable on the grounds it is drawn on.
 *
 * Body text needs 4.5:1 and secondary text 3.0:1, which are the WCAG AA thresholds; a hairline
 * only has to be visible at all. On failure `reason` (when given) names the pair that failed,
 * so a theme added later fails with an explanation rather than by looking wrong on a handheld
 * somebody has taken outdoors.
 */
bool mesh_ui_theme_validate(const struct mesh_ui_theme *theme, char *reason, size_t reason_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_THEME_H */
