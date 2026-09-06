#define _POSIX_C_SOURCE 200809L

/*
 * The themes.
 *
 * Each is a table of colours by role plus the geometry it wants, and that is the whole of a
 * theme: nothing here draws, and nothing that draws knows which of these it is drawing with.
 * Adding one is adding an entry to k_themes.
 *
 * The four that ship cover the reasons a handheld needs a different look at all:
 *
 *   dark        the original palette, unchanged - a dark ground for a lit room
 *   light       a paper ground, for direct sun where a dark screen washes out
 *   contrast    black, white and one yellow, for the same sun with worse eyes
 *   colorblind  the dark ground with the red/green pair replaced by blue/orange
 *
 * That last one is the reason roles are named for meaning rather than for colour. Roughly one
 * man in twelve cannot separate the green of "connected" from the red of "failed"; the fix is
 * to encode the difference in hue *and* lightness with colours that stay distinct - the
 * Okabe-Ito set below is the standard choice - and that is a palette change only because no
 * renderer ever said "green".
 *
 * Every table is checked by mesh_ui_theme_validate() in the tests, so a palette that reads
 * nicely on a desktop monitor and not at all on the Brick fails before it ships.
 */

#include "mesh/ui/theme.h"

#include "mesh/utils/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGB(rr, gg, bb)                                                                            \
    { .r = (rr), .g = (gg), .b = (bb) }

/* The metrics the Brick's panel was tuned for. A theme that wants a different look overrides
   the field it cares about rather than restating the struct. */
#define MESH_UI_METRICS_DEFAULT                                                                    \
    {                                                                                              \
        .margin = 16U, .scale = 4U, .chrome_scale_down = 1U, .bubble_width_pct = 75U,              \
        .field_label_cols = 20U, .narrow_cols = 40U,                                               \
    }

static const struct mesh_ui_theme k_themes[] = {
    {
        .id = "dark",
        .name = "Dark",
        .font_id = "5x7",
        .dark = true,
        .colors =
            {
                /* Dark ground, cool greys for chrome, one warm colour for what needs the eye. */
                [MESH_UI_COLOR_BG] = RGB(0x0A, 0x14, 0x1E),
                [MESH_UI_COLOR_SURFACE] = RGB(0x14, 0x22, 0x32),
                [MESH_UI_COLOR_SURFACE_SEL] = RGB(40, 80, 120),
                [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(60, 110, 170),
                [MESH_UI_COLOR_TEXT] = RGB(220, 230, 240),
                [MESH_UI_COLOR_TEXT_DIM] = RGB(140, 150, 165),
                [MESH_UI_COLOR_TEXT_STRONG] = RGB(255, 255, 255),
                [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(255, 255, 255),
                [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(190, 208, 226),
                [MESH_UI_COLOR_ACCENT] = RGB(255, 220, 120),
                /* Dark text on the accent fill: white on that yellow is unreadable at this
                   glyph size. */
                [MESH_UI_COLOR_ON_ACCENT] = RGB(0x0A, 0x14, 0x1E),
                [MESH_UI_COLOR_GOOD] = RGB(120, 220, 150),
                [MESH_UI_COLOR_BAD] = RGB(240, 120, 120),
                [MESH_UI_COLOR_RULE] = RGB(40, 80, 120),
                [MESH_UI_COLOR_RULE_STRONG] = RGB(60, 110, 170),
                /* Bubble fills. Theirs is the neutral ground, ours is the one with colour in
                   it - the same "you are the blue one" every messenger has trained everybody
                   on. The selected pair are the same hues lifted, so the cursor reads as a
                   highlight rather than as a different kind of message. */
                [MESH_UI_COLOR_BUBBLE_IN] = RGB(30, 44, 60),
                [MESH_UI_COLOR_BUBBLE_OUT] = RGB(34, 66, 104),
                [MESH_UI_COLOR_BUBBLE_IN_SEL] = RGB(52, 72, 94),
                [MESH_UI_COLOR_BUBBLE_OUT_SEL] = RGB(58, 104, 154),
                [MESH_UI_COLOR_BUBBLE_FAILED] = RGB(84, 40, 44),
                [MESH_UI_COLOR_TEXT_INBOUND] = RGB(235, 245, 255),
                /* Bright enough to read on its own bubble fill. It used to be the dim grey
                   that said "ours" on a bare row; the bubble says that now. */
                [MESH_UI_COLOR_TEXT_OUTBOUND] = RGB(228, 238, 248),
            },
        /* Bright tints for a dark ground: the initials over them are the ground colour, so a
           tint has to carry the contrast the way the accent fill does. */
        .avatars =
            {
                RGB(120, 190, 255), /* sky */
                RGB(255, 200, 120), /* amber */
                RGB(150, 220, 170), /* mint */
                RGB(230, 160, 220), /* orchid */
                RGB(255, 162, 140), /* coral */
                RGB(180, 200, 255), /* periwinkle */
            },
        .avatar_count = 6U,
        .metrics = MESH_UI_METRICS_DEFAULT,
    },
    {
        .id = "light",
        .name = "Light",
        .font_id = "5x7",
        .dark = false,
        .colors =
            {
                /* A paper ground. The accent goes amber-brown rather than yellow: an accent is
                   a fill as well as a text colour, and pale yellow under dark text is a
                   highlighter pen, not a badge. */
                [MESH_UI_COLOR_BG] = RGB(247, 248, 250),
                [MESH_UI_COLOR_SURFACE] = RGB(231, 235, 241),
                [MESH_UI_COLOR_SURFACE_SEL] = RGB(200, 219, 242),
                [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(154, 193, 236),
                [MESH_UI_COLOR_TEXT] = RGB(24, 32, 44),
                [MESH_UI_COLOR_TEXT_DIM] = RGB(92, 104, 120),
                [MESH_UI_COLOR_TEXT_STRONG] = RGB(8, 14, 24),
                [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(12, 20, 32),
                [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(70, 84, 104),
                [MESH_UI_COLOR_ACCENT] = RGB(160, 72, 0),
                [MESH_UI_COLOR_ON_ACCENT] = RGB(255, 255, 255),
                [MESH_UI_COLOR_GOOD] = RGB(20, 110, 60),
                [MESH_UI_COLOR_BAD] = RGB(176, 32, 40),
                [MESH_UI_COLOR_RULE] = RGB(188, 199, 213),
                [MESH_UI_COLOR_RULE_STRONG] = RGB(120, 160, 205),
                [MESH_UI_COLOR_BUBBLE_IN] = RGB(219, 225, 234),
                [MESH_UI_COLOR_BUBBLE_OUT] = RGB(203, 224, 248),
                [MESH_UI_COLOR_BUBBLE_IN_SEL] = RGB(193, 208, 228),
                [MESH_UI_COLOR_BUBBLE_OUT_SEL] = RGB(176, 208, 244),
                [MESH_UI_COLOR_BUBBLE_FAILED] = RGB(250, 214, 214),
                [MESH_UI_COLOR_TEXT_INBOUND] = RGB(22, 30, 42),
                [MESH_UI_COLOR_TEXT_OUTBOUND] = RGB(18, 32, 52),
            },
        /* The dark half of each hue, because here the initials are the paper ground. */
        .avatars =
            {
                RGB(30, 90, 160),  /* deep blue */
                RGB(150, 60, 20),  /* rust */
                RGB(20, 110, 80),  /* pine */
                RGB(110, 45, 130), /* plum */
                RGB(150, 40, 80),  /* berry */
                RGB(60, 80, 130),  /* slate */
            },
        .avatar_count = 6U,
        .metrics = MESH_UI_METRICS_DEFAULT,
    },
    {
        .id = "contrast",
        .name = "High contrast",
        .font_id = "5x7",
        .dark = true,
        .colors =
            {
                /* Inverse video for the cursor - a white bar with black text - because a fill
                   one step lighter than the ground is what stops being findable first in
                   sunlight, and it is the cue this theme exists to make loud. */
                [MESH_UI_COLOR_BG] = RGB(0, 0, 0),
                [MESH_UI_COLOR_SURFACE] = RGB(26, 26, 26),
                [MESH_UI_COLOR_SURFACE_SEL] = RGB(255, 255, 255),
                [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(255, 214, 0),
                [MESH_UI_COLOR_TEXT] = RGB(255, 255, 255),
                [MESH_UI_COLOR_TEXT_DIM] = RGB(196, 196, 196),
                [MESH_UI_COLOR_TEXT_STRONG] = RGB(255, 255, 255),
                [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(0, 0, 0),
                [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(72, 72, 72),
                [MESH_UI_COLOR_ACCENT] = RGB(255, 214, 0),
                [MESH_UI_COLOR_ON_ACCENT] = RGB(0, 0, 0),
                [MESH_UI_COLOR_GOOD] = RGB(0, 230, 118),
                [MESH_UI_COLOR_BAD] = RGB(255, 120, 120),
                [MESH_UI_COLOR_RULE] = RGB(140, 140, 140),
                [MESH_UI_COLOR_RULE_STRONG] = RGB(255, 214, 0),
                [MESH_UI_COLOR_BUBBLE_IN] = RGB(28, 28, 28),
                [MESH_UI_COLOR_BUBBLE_OUT] = RGB(0, 48, 84),
                [MESH_UI_COLOR_BUBBLE_IN_SEL] = RGB(80, 80, 80),
                [MESH_UI_COLOR_BUBBLE_OUT_SEL] = RGB(0, 84, 140),
                [MESH_UI_COLOR_BUBBLE_FAILED] = RGB(96, 0, 0),
                [MESH_UI_COLOR_TEXT_INBOUND] = RGB(255, 255, 255),
                [MESH_UI_COLOR_TEXT_OUTBOUND] = RGB(255, 255, 255),
            },
        /* Two, not six. A palette of hues is exactly what this theme exists to do without, so
           an avatar here is the yellow or the white and the initials carry the rest. */
        .avatars =
            {
                RGB(255, 214, 0),
                RGB(255, 255, 255),
            },
        .avatar_count = 2U,
        .metrics = MESH_UI_METRICS_DEFAULT,
    },
    {
        .id = "colorblind",
        .name = "Colour-blind safe",
        .font_id = "5x7",
        .dark = true,
        .colors =
            {
                /* Okabe-Ito: sky blue for good, orange for bad, reddish purple for the accent.
                   The three stay distinct under deuteranopia and protanopia, where the
                   original green/yellow/red collapse into one another. */
                [MESH_UI_COLOR_BG] = RGB(0x0A, 0x14, 0x1E),
                [MESH_UI_COLOR_SURFACE] = RGB(0x14, 0x22, 0x32),
                [MESH_UI_COLOR_SURFACE_SEL] = RGB(40, 80, 120),
                [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(60, 110, 170),
                [MESH_UI_COLOR_TEXT] = RGB(226, 232, 240),
                [MESH_UI_COLOR_TEXT_DIM] = RGB(146, 156, 170),
                [MESH_UI_COLOR_TEXT_STRONG] = RGB(255, 255, 255),
                [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(255, 255, 255),
                [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(190, 208, 226),
                [MESH_UI_COLOR_ACCENT] = RGB(204, 121, 167),
                [MESH_UI_COLOR_ON_ACCENT] = RGB(0x0A, 0x14, 0x1E),
                [MESH_UI_COLOR_GOOD] = RGB(86, 180, 233),
                [MESH_UI_COLOR_BAD] = RGB(230, 159, 0),
                [MESH_UI_COLOR_RULE] = RGB(40, 80, 120),
                [MESH_UI_COLOR_RULE_STRONG] = RGB(86, 180, 233),
                [MESH_UI_COLOR_BUBBLE_IN] = RGB(30, 44, 60),
                [MESH_UI_COLOR_BUBBLE_OUT] = RGB(34, 66, 104),
                [MESH_UI_COLOR_BUBBLE_IN_SEL] = RGB(52, 72, 94),
                [MESH_UI_COLOR_BUBBLE_OUT_SEL] = RGB(58, 104, 154),
                /* Brown rather than red: a failed bubble has to differ from an outbound one
                   without relying on the hue half the point of this theme is to avoid. */
                [MESH_UI_COLOR_BUBBLE_FAILED] = RGB(96, 52, 20),
                [MESH_UI_COLOR_TEXT_INBOUND] = RGB(235, 245, 255),
                [MESH_UI_COLOR_TEXT_OUTBOUND] = RGB(228, 238, 248),
            },
        /* The Okabe-Ito set again, this time as fills. They are the six that stay separable
           under every common dichromacy, which is the only reason to spend six on avatars at
           all - two tints that collapse into one for the reader are one tint. */
        .avatars =
            {
                RGB(86, 180, 233),  /* sky blue */
                RGB(230, 159, 0),   /* orange */
                RGB(0, 158, 115),   /* bluish green */
                RGB(240, 228, 66),  /* yellow */
                RGB(213, 94, 0),    /* vermillion */
                RGB(204, 121, 167), /* reddish purple */
            },
        .avatar_count = 6U,
        .metrics = MESH_UI_METRICS_DEFAULT,
    },
};

#define MESH_UI_THEME_COUNT (sizeof k_themes / sizeof k_themes[0])

size_t mesh_ui_theme_count(void) { return MESH_UI_THEME_COUNT; }

const struct mesh_ui_theme *mesh_ui_theme_at(size_t index) {
    return index < MESH_UI_THEME_COUNT ? &k_themes[index] : NULL;
}

const struct mesh_ui_theme *mesh_ui_theme_default(void) { return &k_themes[0]; }

const struct mesh_ui_theme *mesh_ui_theme_by_id(const char *id) {
    if (id == NULL || id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < MESH_UI_THEME_COUNT; ++i) {
        if (strcmp(k_themes[i].id, id) == 0) {
            return &k_themes[i];
        }
    }
    return NULL;
}

const struct mesh_ui_theme *mesh_ui_theme_env(void) {
    const char *name = getenv("MESHCLIENT_THEME");
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    const struct mesh_ui_theme *theme = mesh_ui_theme_by_id(name);
    if (theme == NULL) {
        /* Warned rather than refused: a typo in an environment variable should not leave a
           handheld with no UI. Warned once per call site, and there are two. */
        mesh_log_warn("ui", "Unknown MESHCLIENT_THEME '%s'; using '%s'", name,
                      mesh_ui_theme_default()->id);
        return NULL;
    }
    return theme;
}

const struct mesh_ui_theme *mesh_ui_theme_from_env(void) {
    const struct mesh_ui_theme *theme = mesh_ui_theme_env();
    return theme != NULL ? theme : mesh_ui_theme_default();
}

const struct mesh_ui_theme *mesh_ui_theme_resolve(const char *id) {
    const struct mesh_ui_theme *theme = mesh_ui_theme_by_id(id);
    return theme != NULL ? theme : mesh_ui_theme_default();
}

/*
 * The next theme in the registry, wrapping.
 *
 * Cycling rather than a menu because the Brick has no pointer and About is a list of one-press
 * rows: pressing A steps to the next look and the screen is the preview. The order is the
 * table's, which is why `dark` is first - a user who has cycled somewhere unreadable presses
 * A until the familiar one comes back round.
 */
const struct mesh_ui_theme *mesh_ui_theme_next(const struct mesh_ui_theme *theme) {
    const size_t count = mesh_ui_theme_count();
    if (count == 0U) {
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (mesh_ui_theme_at(i) == theme) {
            return mesh_ui_theme_at((i + 1U) % count);
        }
    }
    return mesh_ui_theme_default();
}

static const struct mesh_ui_theme *theme_or_default(const struct mesh_ui_theme *theme) {
    return theme != NULL ? theme : mesh_ui_theme_default();
}

struct mesh_ui_rgb mesh_ui_theme_color(const struct mesh_ui_theme *theme, enum mesh_ui_color role) {
    theme = theme_or_default(theme);
    if ((int)role < 0 || role >= MESH_UI_COLOR_COUNT) {
        role = MESH_UI_COLOR_TEXT;
    }
    return theme->colors[role];
}

enum mesh_ui_color mesh_ui_tone_role(enum mesh_ui_tone tone) {
    switch (tone) {
    case MESH_UI_TONE_DIM:
        return MESH_UI_COLOR_TEXT_DIM;
    case MESH_UI_TONE_STRONG:
        return MESH_UI_COLOR_TEXT_STRONG;
    case MESH_UI_TONE_ACCENT:
        return MESH_UI_COLOR_ACCENT;
    case MESH_UI_TONE_GOOD:
        return MESH_UI_COLOR_GOOD;
    case MESH_UI_TONE_BAD:
        return MESH_UI_COLOR_BAD;
    case MESH_UI_TONE_INBOUND:
        return MESH_UI_COLOR_TEXT_INBOUND;
    case MESH_UI_TONE_OUTBOUND:
        return MESH_UI_COLOR_TEXT_OUTBOUND;
    case MESH_UI_TONE_NORMAL:
    case MESH_UI_TONE_COUNT:
    default:
        return MESH_UI_COLOR_TEXT;
    }
}

struct mesh_ui_rgb mesh_ui_theme_tone(const struct mesh_ui_theme *theme, enum mesh_ui_tone tone) {
    return mesh_ui_theme_color(theme, mesh_ui_tone_role(tone));
}

struct mesh_ui_rgb mesh_ui_theme_avatar(const struct mesh_ui_theme *theme, uint32_t seed) {
    theme = theme_or_default(theme);
    const uint32_t count = theme->avatar_count > 0U && theme->avatar_count <= MESH_UI_AVATAR_TINTS
                               ? theme->avatar_count
                               : 0U;
    if (count == 0U) {
        /* A theme that states no palette still has to answer, and the accent is the one fill
           it already promises reads with the ground colour over it. */
        return theme->colors[MESH_UI_COLOR_ACCENT];
    }
    /*
     * Knuth's multiplicative hash before the modulo. A node number is not random in its low
     * bits - a mesh is a run of consecutive ids off one vendor's block - so `seed % count`
     * alone hands neighbouring nodes neighbouring tints, which is the one thing an avatar
     * colour must not do.
     */
    return theme->avatars[(seed * 2654435761U >> 16) % count];
}

const struct mesh_ui_font *mesh_ui_theme_font(const struct mesh_ui_theme *theme) {
    theme = theme_or_default(theme);
    const struct mesh_ui_font *font = mesh_ui_font_by_id(theme->font_id);
    return font != NULL ? font : mesh_ui_font_default();
}

const struct mesh_ui_metrics *mesh_ui_theme_metrics(const struct mesh_ui_theme *theme) {
    return &theme_or_default(theme)->metrics;
}

int mesh_ui_theme_clamp_scale(const struct mesh_ui_theme *theme, int scale) {
    theme = theme_or_default(theme);
    if (scale <= 0) {
        scale = (int)theme->metrics.scale;
    }
    if (scale < MESH_UI_SCALE_MIN) {
        return MESH_UI_SCALE_MIN;
    }
    if (scale > MESH_UI_SCALE_MAX) {
        return MESH_UI_SCALE_MAX;
    }
    return scale;
}

int mesh_ui_theme_scale(const struct mesh_ui_theme *theme) {
    return mesh_ui_theme_clamp_scale(theme, 0);
}

int mesh_ui_theme_chrome_scale(const struct mesh_ui_theme *theme, int scale) {
    theme = theme_or_default(theme);
    const int down = (int)theme->metrics.chrome_scale_down;
    const int chrome = mesh_ui_theme_clamp_scale(theme, scale) - down;
    return chrome < MESH_UI_SCALE_MIN ? MESH_UI_SCALE_MIN : chrome;
}

/*
 * Relative luminance, the sRGB definition, without libm.
 *
 * A display does not emit twice the light for twice the stored value - the value is
 * gamma-encoded - so each channel is decoded first, then weighted by how much of our sense of
 * brightness comes from it: green carries most of it, blue almost none. The result runs from 0
 * for black to 65535 for white, and it is what makes "these two colours are far enough apart"
 * a measurement rather than an opinion.
 *
 * The decode is a 256-entry table rather than a pow() because pow() is the only thing in this
 * tree that would pull libm into the static aarch64 link, and there are 256 possible answers.
 * Generated as round(65535 * srgb_to_linear(v / 255)) for v in 0..255.
 */
static const uint16_t k_srgb_linear[256] = {
    0,     20,    40,    60,    80,    99,    119,   139,   159,   179,   199,   219,   241,
    264,   288,   313,   340,   367,   396,   427,   458,   491,   526,   562,   599,   637,
    677,   718,   761,   805,   851,   898,   947,   997,   1048,  1101,  1156,  1212,  1270,
    1330,  1391,  1453,  1517,  1583,  1651,  1720,  1790,  1863,  1937,  2013,  2090,  2170,
    2250,  2333,  2418,  2504,  2592,  2681,  2773,  2866,  2961,  3058,  3157,  3258,  3360,
    3464,  3570,  3678,  3788,  3900,  4014,  4129,  4247,  4366,  4488,  4611,  4736,  4864,
    4993,  5124,  5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,  6395,  6547,  6700,
    6856,  7014,  7174,  7335,  7500,  7666,  7834,  8004,  8177,  8352,  8528,  8708,  8889,
    9072,  9258,  9445,  9635,  9828,  10022, 10219, 10417, 10619, 10822, 11028, 11235, 11446,
    11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146, 14387,
    14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727,
    18001, 18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481,
    21787, 22096, 22407, 22721, 23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662,
    26001, 26344, 26688, 27036, 27386, 27739, 28094, 28452, 28813, 29176, 29542, 29911, 30282,
    30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143, 34544, 34948, 35355,
    35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903,
    47385, 47871, 48359, 48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401,
    53921, 54445, 54971, 55500, 56032, 56567, 57105, 57646, 58190, 58737, 59287, 59840, 60396,
    60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

static uint32_t theme_luminance(struct mesh_ui_rgb color) {
    /* The sRGB weights (0.2126, 0.7152, 0.0722), carried as ten-thousandths so the whole
       calculation stays in integers. The product fits comfortably in 32 bits: 65535 * 10000. */
    const uint32_t r = k_srgb_linear[color.r];
    const uint32_t g = k_srgb_linear[color.g];
    const uint32_t b = k_srgb_linear[color.b];
    return (r * 2126U + g * 7152U + b * 722U) / 10000U;
}

double mesh_ui_theme_contrast(struct mesh_ui_rgb a, struct mesh_ui_rgb b) {
    uint32_t la = theme_luminance(a);
    uint32_t lb = theme_luminance(b);
    if (la < lb) {
        const uint32_t swap = la;
        la = lb;
        lb = swap;
    }
    /* The flare term, 0.05 on a 0..1 scale: no real screen is perfectly black, so the ratio of
       two near-blacks is bounded instead of running off to infinity. */
    const double flare = 0.05 * 65535.0;
    return ((double)la + flare) / ((double)lb + flare);
}

/*
 * The readability contract every theme is held to.
 *
 * Body text on its ground gets the WCAG AA threshold for large text (4.5:1); secondary text -
 * dim rows, the clock on a bubble, a status colour - gets 3:1, which is the same standard's
 * floor for anything that is not the words you are reading. A rule only has to be visible.
 *
 * A pair belongs here when something is actually drawn that way. That cuts both ways: a pair
 * missing from this table is a pair nothing checks, which is how dim text on a *selected*
 * outbound bubble stayed at 1.9:1 on the dark palette for as long as it did. When a renderer
 * starts drawing a new combination, it comes with a row.
 */
struct theme_pair {
    enum mesh_ui_color ink;
    enum mesh_ui_color ground;
    double ratio;
};

static const struct theme_pair k_required[] = {
    {MESH_UI_COLOR_TEXT, MESH_UI_COLOR_BG, 4.5},
    {MESH_UI_COLOR_TEXT_STRONG, MESH_UI_COLOR_BG, 4.5},
    {MESH_UI_COLOR_TEXT, MESH_UI_COLOR_SURFACE, 4.5},
    {MESH_UI_COLOR_TEXT_ON_SEL, MESH_UI_COLOR_SURFACE_SEL, 4.5},
    {MESH_UI_COLOR_TEXT_ON_SEL, MESH_UI_COLOR_SURFACE_ACTIVE, 4.5},
    {MESH_UI_COLOR_TEXT_ON_SEL_DIM, MESH_UI_COLOR_SURFACE_SEL, 3.0},
    {MESH_UI_COLOR_ON_ACCENT, MESH_UI_COLOR_ACCENT, 4.5},
    /* Two colours outside the avatar palette are drawn as avatar tints, and an avatar's
       initials are the ground colour: the accent, on the conversation-list rows that are not
       somebody, and the bad tone, on a row armed to be deleted. Both owe the ground what every
       stated tint owes it - and the armed one is the disc that must not go quiet. */
    {MESH_UI_COLOR_BG, MESH_UI_COLOR_ACCENT, 4.5},
    {MESH_UI_COLOR_BG, MESH_UI_COLOR_BAD, 4.5},
    {MESH_UI_COLOR_TEXT_INBOUND, MESH_UI_COLOR_BUBBLE_IN, 4.5},
    {MESH_UI_COLOR_TEXT_INBOUND, MESH_UI_COLOR_BUBBLE_IN_SEL, 4.5},
    {MESH_UI_COLOR_TEXT_OUTBOUND, MESH_UI_COLOR_BUBBLE_OUT, 4.5},
    {MESH_UI_COLOR_TEXT_OUTBOUND, MESH_UI_COLOR_BUBBLE_OUT_SEL, 4.5},
    /* Secondary: still has to be read, just not for long. */
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_BG, 3.0},
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_SURFACE, 3.0},
    {MESH_UI_COLOR_ACCENT, MESH_UI_COLOR_BG, 3.0},
    {MESH_UI_COLOR_GOOD, MESH_UI_COLOR_BG, 3.0},
    {MESH_UI_COLOR_BAD, MESH_UI_COLOR_BG, 3.0},
    {MESH_UI_COLOR_ACCENT, MESH_UI_COLOR_BUBBLE_IN, 3.0},
    {MESH_UI_COLOR_ACCENT, MESH_UI_COLOR_BUBBLE_IN_SEL, 3.0},
    /* A critical alert heads its bubble in the bad tone rather than the accent, so that pairing
       has to hold everywhere the accent one does - otherwise the one message a theme must not
       swallow is the one it swallows. */
    {MESH_UI_COLOR_BAD, MESH_UI_COLOR_BUBBLE_IN, 3.0},
    {MESH_UI_COLOR_BAD, MESH_UI_COLOR_BUBBLE_IN_SEL, 3.0},
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_BUBBLE_IN, 3.0},
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_BUBBLE_OUT, 3.0},
    {MESH_UI_COLOR_TEXT_INBOUND, MESH_UI_COLOR_BUBBLE_FAILED, 3.0},
    {MESH_UI_COLOR_TEXT_OUTBOUND, MESH_UI_COLOR_BUBBLE_FAILED, 3.0},
    {MESH_UI_COLOR_BAD, MESH_UI_COLOR_BUBBLE_FAILED, 3.0},
    /* Furniture: visible at all. */
    {MESH_UI_COLOR_RULE, MESH_UI_COLOR_BG, 1.4},
    {MESH_UI_COLOR_RULE_STRONG, MESH_UI_COLOR_BG, 1.4},
};

bool mesh_ui_theme_validate(const struct mesh_ui_theme *theme, char *reason, size_t reason_len) {
    theme = theme_or_default(theme);
    if (reason != NULL && reason_len > 0U) {
        reason[0] = '\0';
    }

    if (theme->id == NULL || theme->id[0] == '\0' || theme->name == NULL) {
        if (reason != NULL) {
            snprintf(reason, reason_len, "%s", "the theme has no id or no name");
        }
        return false;
    }
    if (mesh_ui_theme_clamp_scale(theme, 0) != (int)theme->metrics.scale) {
        if (reason != NULL) {
            snprintf(reason, reason_len, "scale %u is outside %d..%d", theme->metrics.scale,
                     MESH_UI_SCALE_MIN, MESH_UI_SCALE_MAX);
        }
        return false;
    }
    if (theme->metrics.bubble_width_pct == 0U || theme->metrics.bubble_width_pct > 100U) {
        if (reason != NULL) {
            snprintf(reason, reason_len, "bubble width %u%% is not a fraction of the body",
                     theme->metrics.bubble_width_pct);
        }
        return false;
    }
    if (mesh_ui_font_by_id(theme->font_id) == NULL) {
        if (reason != NULL) {
            snprintf(reason, reason_len, "no font called '%s'",
                     theme->font_id != NULL ? theme->font_id : "(none)");
        }
        return false;
    }

    if (theme->avatar_count > MESH_UI_AVATAR_TINTS) {
        if (reason != NULL) {
            snprintf(reason, reason_len, "%u avatar tints, at most %u fit", theme->avatar_count,
                     (unsigned)MESH_UI_AVATAR_TINTS);
        }
        return false;
    }

    for (size_t i = 0; i < sizeof k_required / sizeof k_required[0]; ++i) {
        const struct theme_pair *pair = &k_required[i];
        const double ratio =
            mesh_ui_theme_contrast(theme->colors[pair->ink], theme->colors[pair->ground]);
        if (ratio + 0.005 < pair->ratio) {
            if (reason != NULL) {
                snprintf(reason, reason_len, "role %d on role %d is %.2f:1, needs %.1f:1",
                         (int)pair->ink, (int)pair->ground, ratio, pair->ratio);
            }
            return false;
        }
    }

    /* An avatar is a fill with the ground colour punched out of it, so every tint owes the
       ground what body text owes it. A palette entry that fails is a disc whose initials
       nobody can read - and the initials are the only part of an avatar that carries
       information. */
    for (uint8_t i = 0; i < theme->avatar_count; ++i) {
        const double ratio =
            mesh_ui_theme_contrast(theme->avatars[i], theme->colors[MESH_UI_COLOR_BG]);
        if (ratio + 0.005 < 4.5) {
            if (reason != NULL) {
                snprintf(reason, reason_len, "avatar tint %u on the ground is %.2f:1, needs 4.5:1",
                         i, ratio);
            }
            return false;
        }
    }
    return true;
}
