#define _POSIX_C_SOURCE 200809L

/*
 * The themes.
 *
 * Each is a table of colours by role plus the geometry it wants, and that is the whole of a
 * theme: nothing here draws, and nothing that draws knows which of these it is drawing with.
 * Adding one is adding an entry to k_themes.
 *
 * Most of the table is the six families - primary, secondary, tertiary, success, warning,
 * error - four roles each. A theme states all twenty-four and every widget reaches them the
 * same way, through mesh_ui_theme_paint(), which is what stops a component from picking a fill
 * here and a label colour there and landing on a pair nothing has checked.
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
 * renderer ever said "green". mesh_ui_theme_validate() holds the three status families apart
 * numerically, so a theme cannot quietly hand two of them the same colour.
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

/*
 * The metrics the Brick's panel was tuned for. A theme that wants a different look overrides
 * the field it cares about rather than restating the struct.
 *
 * The shape steps start at two rather than one because of how big the boxes here are. A step is
 * one glyph-scale unit - four pixels at the device's scale - and four pixels off the corner of
 * a row highlight a thousand pixels wide and forty tall is not a rounded rectangle, it is a
 * rectangle somebody sanded. Two steps is where the eye starts reading a shape with ends.
 */
#define MESH_UI_METRICS_DEFAULT                                                                    \
    {                                                                                              \
        .margin = 16U, .scale = 4U, .bubble_width_pct = 75U,                                       \
        .type_offset =                                                                             \
            {                                                                                      \
                [MESH_UI_TYPE_TITLE] = 1,                                                          \
                [MESH_UI_TYPE_BODY] = 0,                                                           \
                [MESH_UI_TYPE_LABEL] = -1,                                                         \
            },                                                                                     \
        .space =                                                                                   \
            {                                                                                      \
                [MESH_UI_SPACE_NONE] = 0U, [MESH_UI_SPACE_XS] = 1U, [MESH_UI_SPACE_SM] = 2U,       \
                [MESH_UI_SPACE_MD] = 4U,   [MESH_UI_SPACE_LG] = 6U,                                \
            },                                                                                     \
        .field_label_cols = 20U, .narrow_cols = 40U, .card_pad = 2U, .meter_thickness = 1U,        \
        .motion_ms =                                                                               \
            {                                                                                      \
                [MESH_UI_MOTION_SHORT] = 140U,                                                     \
                [MESH_UI_MOTION_MEDIUM] = 220U,                                                    \
                [MESH_UI_MOTION_LONG] = 320U,                                                      \
                [MESH_UI_MOTION_LOOP] = 1400U,                                                     \
            },                                                                                     \
        .shape = {                                                                                 \
            [MESH_UI_SHAPE_NONE] = 0U,                                                             \
            [MESH_UI_SHAPE_SM] = 2U,                                                               \
            [MESH_UI_SHAPE_MD] = 3U,                                                               \
            [MESH_UI_SHAPE_LG] = 4U,                                                               \
        },                                                                                         \
    }

static const struct mesh_ui_theme
    k_themes[] =
        {
            {
                .id = "dark",
                .name = "Dark",
                .font_id = "ui",
                .dark = true,
                .colors =
                    {
                        [MESH_UI_COLOR_BG] = RGB(0x0A, 0x14, 0x1E),
                        /* The three tiers walk away from the ground in even steps. Far enough apart
                           to tell one from another indoors, near enough that body text keeps the
                           contrast it is validated for on all of them - and SURFACE_HIGH now
                           carries an inbound bubble as well as the draft box, so it is read at
                           length rather than glanced at. */
                        [MESH_UI_COLOR_SURFACE_LOW] = RGB(0x10, 0x1B, 0x28),
                        [MESH_UI_COLOR_SURFACE] = RGB(0x14, 0x22, 0x32),
                        [MESH_UI_COLOR_SURFACE_HIGH] = RGB(0x1C, 0x2E, 0x42),
                        [MESH_UI_COLOR_SURFACE_SEL] = RGB(40, 80, 120),
                        [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(60, 110, 170),
                        /* The other end of the palette: near enough to the light theme's ground
                           that a notice on it reads as a piece of another UI laid over this one,
                           which is exactly what a snackbar is meant to look like. */
                        [MESH_UI_COLOR_SURFACE_INVERSE] = RGB(226, 232, 240),
                        [MESH_UI_COLOR_TEXT] = RGB(220, 230, 240),
                        [MESH_UI_COLOR_TEXT_DIM] = RGB(140, 150, 165),
                        [MESH_UI_COLOR_TEXT_STRONG] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(190, 208, 226),
                        [MESH_UI_COLOR_TEXT_ON_INVERSE] = RGB(0x10, 0x1B, 0x28),
                        /* The brand yellow, unchanged. Dark text on it because white on that yellow
                           is unreadable at this glyph size; the container is the same hue taken
                           down to an olive that reads as "the primary, quietly", with a pale tint
                           of it for the ink. */
                        [MESH_UI_COLOR_PRIMARY] = RGB(255, 220, 120),
                        [MESH_UI_COLOR_ON_PRIMARY] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_PRIMARY_CONTAINER] = RGB(86, 68, 24),
                        [MESH_UI_COLOR_ON_PRIMARY_CONTAINER] = RGB(255, 232, 170),
                        /* The blue our own messages are drawn in. The container is the outbound
                           bubble's old fill and the ink on it the old outbound text colour, so the
                           transcript is unchanged - it is simply no longer four roles nothing else
                           could reach. */
                        [MESH_UI_COLOR_SECONDARY] = RGB(120, 175, 240),
                        [MESH_UI_COLOR_ON_SECONDARY] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_SECONDARY_CONTAINER] = RGB(34, 66, 104),
                        [MESH_UI_COLOR_ON_SECONDARY_CONTAINER] = RGB(228, 238, 248),
                        /* A lavender: adjacent to neither the yellow that means "here" nor the
                           green and red that mean "fine" and "not fine", which is the whole
                           requirement for a colour that says "not finished yet". */
                        [MESH_UI_COLOR_TERTIARY] = RGB(190, 160, 235),
                        [MESH_UI_COLOR_ON_TERTIARY] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_TERTIARY_CONTAINER] = RGB(58, 50, 92),
                        [MESH_UI_COLOR_ON_TERTIARY_CONTAINER] = RGB(222, 208, 255),
                        [MESH_UI_COLOR_SUCCESS] = RGB(120, 220, 150),
                        [MESH_UI_COLOR_ON_SUCCESS] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_SUCCESS_CONTAINER] = RGB(0x1A, 0x42, 0x2C),
                        [MESH_UI_COLOR_ON_SUCCESS_CONTAINER] = RGB(170, 240, 196),
                        /* Orange rather than the brand yellow. They are neighbours, which is why
                           this had to stop being one colour: a warning drawn in the primary is a
                           warning that cannot be told from a heading. */
                        [MESH_UI_COLOR_WARNING] = RGB(255, 167, 38),
                        [MESH_UI_COLOR_ON_WARNING] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_WARNING_CONTAINER] = RGB(78, 46, 8),
                        [MESH_UI_COLOR_ON_WARNING_CONTAINER] = RGB(255, 208, 156),
                        /* The container is the failed bubble's fill, which every theme used to
                           restate. */
                        [MESH_UI_COLOR_ERROR] = RGB(240, 120, 120),
                        [MESH_UI_COLOR_ON_ERROR] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_ERROR_CONTAINER] = RGB(84, 40, 44),
                        [MESH_UI_COLOR_ON_ERROR_CONTAINER] = RGB(255, 190, 190),
                        /* A step brighter than the rule for the outline: an edge has to be found
                           against two fills at once, where a separator only has to divide one. */
                        [MESH_UI_COLOR_RULE] = RGB(40, 80, 120),
                        [MESH_UI_COLOR_RULE_STRONG] = RGB(60, 110, 170),
                        [MESH_UI_COLOR_OUTLINE] = RGB(62, 100, 140),
                        /* The cursor fill's own colour, which is where a track wants to sit on a
                           dark palette: one step off the surface and well under every fill. */
                        [MESH_UI_COLOR_METER_TRACK] = RGB(40, 80, 120),
                    },
                /* Bright tints for a dark ground: the initials over them are the ground colour, so
                   a tint has to carry the contrast the way the accent fill does. */
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
                .font_id = "ui",
                .dark = false,
                .colors =
                    {
                        /* A paper ground. The primary goes amber-brown rather than yellow: it is a
                           fill as well as a text colour, and pale yellow under dark text is a
                           highlighter pen. */
                        [MESH_UI_COLOR_BG] = RGB(247, 248, 250),
                        /* Downwards, because here the ground is the light one: a surface rises by
                           getting further from paper, not nearer to it. */
                        [MESH_UI_COLOR_SURFACE_LOW] = RGB(238, 241, 246),
                        [MESH_UI_COLOR_SURFACE] = RGB(231, 235, 241),
                        [MESH_UI_COLOR_SURFACE_HIGH] = RGB(220, 226, 235),
                        [MESH_UI_COLOR_SURFACE_SEL] = RGB(200, 219, 242),
                        [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(154, 193, 236),
                        /* Downwards here too: on paper the far end is a slate the ink comes off,
                           not a deeper tier of the same paper. */
                        [MESH_UI_COLOR_SURFACE_INVERSE] = RGB(42, 51, 64),
                        [MESH_UI_COLOR_TEXT] = RGB(24, 32, 44),
                        [MESH_UI_COLOR_TEXT_DIM] = RGB(92, 104, 120),
                        [MESH_UI_COLOR_TEXT_STRONG] = RGB(8, 14, 24),
                        [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(12, 20, 32),
                        [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(70, 84, 104),
                        [MESH_UI_COLOR_TEXT_ON_INVERSE] = RGB(238, 241, 246),
                        /* An apricot container rather than the palest tint of the primary: on a
                           paper ground the two nearest surface tiers are already near-white, so a
                           container has to come down far enough to be a fill at all before it is a
                           quiet one. */
                        [MESH_UI_COLOR_PRIMARY] = RGB(160, 72, 0),
                        [MESH_UI_COLOR_ON_PRIMARY] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_PRIMARY_CONTAINER] = RGB(250, 192, 142),
                        [MESH_UI_COLOR_ON_PRIMARY_CONTAINER] = RGB(98, 40, 0),
                        /* The outbound bubble's blue, as on the dark theme. */
                        [MESH_UI_COLOR_SECONDARY] = RGB(26, 88, 160),
                        [MESH_UI_COLOR_ON_SECONDARY] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_SECONDARY_CONTAINER] = RGB(203, 224, 248),
                        [MESH_UI_COLOR_ON_SECONDARY_CONTAINER] = RGB(18, 32, 52),
                        /* A violet, for the same reason the dark theme's is a lavender. */
                        [MESH_UI_COLOR_TERTIARY] = RGB(98, 58, 150),
                        [MESH_UI_COLOR_ON_TERTIARY] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TERTIARY_CONTAINER] = RGB(223, 210, 248),
                        [MESH_UI_COLOR_ON_TERTIARY_CONTAINER] = RGB(58, 30, 100),
                        [MESH_UI_COLOR_SUCCESS] = RGB(20, 110, 60),
                        [MESH_UI_COLOR_ON_SUCCESS] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_SUCCESS_CONTAINER] = RGB(190, 232, 205),
                        [MESH_UI_COLOR_ON_SUCCESS_CONTAINER] = RGB(10, 64, 34),
                        /* A dark amber. It has to be tellable from the primary's burnt orange above
                           it and from the error's red below it, which is what the distinctness
                           check in mesh_ui_theme_validate() holds it to. */
                        [MESH_UI_COLOR_WARNING] = RGB(150, 100, 0),
                        [MESH_UI_COLOR_ON_WARNING] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_WARNING_CONTAINER] = RGB(250, 222, 170),
                        [MESH_UI_COLOR_ON_WARNING_CONTAINER] = RGB(92, 58, 0),
                        [MESH_UI_COLOR_ERROR] = RGB(176, 32, 40),
                        [MESH_UI_COLOR_ON_ERROR] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_ERROR_CONTAINER] = RGB(250, 214, 214),
                        [MESH_UI_COLOR_ON_ERROR_CONTAINER] = RGB(120, 16, 22),
                        [MESH_UI_COLOR_RULE] = RGB(188, 199, 213),
                        [MESH_UI_COLOR_RULE_STRONG] = RGB(120, 160, 205),
                        [MESH_UI_COLOR_OUTLINE] = RGB(160, 174, 192),
                        /* A shade under the hairline. The cursor fill is too close to a card here -
                           1.18:1, so a track drawn in it vanishes on the Status screen - and this
                           is the quietest step that still separates from both grounds. */
                        [MESH_UI_COLOR_METER_TRACK] = RGB(186, 198, 214),
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
                .font_id = "ui",
                .dark = true,
                .colors =
                    {
                        /* Inverse video for the cursor - a white bar with black text - because a
                           fill one step lighter than the ground is what stops being findable first
                           in sunlight, and it is the cue this theme exists to make loud. */
                        [MESH_UI_COLOR_BG] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_SURFACE_LOW] = RGB(18, 18, 18),
                        [MESH_UI_COLOR_SURFACE] = RGB(26, 26, 26),
                        [MESH_UI_COLOR_SURFACE_HIGH] = RGB(42, 42, 42),
                        [MESH_UI_COLOR_SURFACE_SEL] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(255, 214, 0),
                        /* The same inverse video the cursor is, because on this theme that *is* the
                           other end of the palette - there is nothing between black and white to
                           hold back to. */
                        [MESH_UI_COLOR_SURFACE_INVERSE] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TEXT] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TEXT_DIM] = RGB(196, 196, 196),
                        [MESH_UI_COLOR_TEXT_STRONG] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(72, 72, 72),
                        [MESH_UI_COLOR_TEXT_ON_INVERSE] = RGB(0, 0, 0),
                        /* The container is the primary at full strength, exactly as BASE/ON_BASE.
                           Holding a colour back is the one thing this theme exists not to do - so
                           it declines, the same way it declines four of its six avatar tints. */
                        [MESH_UI_COLOR_PRIMARY] = RGB(255, 214, 0),
                        [MESH_UI_COLOR_ON_PRIMARY] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_PRIMARY_CONTAINER] = RGB(255, 214, 0),
                        [MESH_UI_COLOR_ON_PRIMARY_CONTAINER] = RGB(0, 0, 0),
                        /* White, with the outbound bubble's navy kept as its container: the
                           transcript is the one place this theme does hold a colour back, because
                           two white blocks in a row would say nothing about who said what. */
                        [MESH_UI_COLOR_SECONDARY] = RGB(255, 214, 0),
                        [MESH_UI_COLOR_ON_SECONDARY] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_SECONDARY_CONTAINER] = RGB(0, 48, 84),
                        [MESH_UI_COLOR_ON_SECONDARY_CONTAINER] = RGB(255, 255, 255),
                        /* The yellow again for both bases rather than the white they would
                           naturally be: this theme's cursor fill *is* white, and a marker bar laid
                           under it in white is a marker bar nobody can see. Collapsing them onto
                           the one accent is what this theme does everywhere else, and their
                           containers still differ. */
                        [MESH_UI_COLOR_TERTIARY] = RGB(255, 214, 0),
                        [MESH_UI_COLOR_ON_TERTIARY] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_TERTIARY_CONTAINER] = RGB(42, 42, 42),
                        [MESH_UI_COLOR_ON_TERTIARY_CONTAINER] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_SUCCESS] = RGB(0, 230, 118),
                        [MESH_UI_COLOR_ON_SUCCESS] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_SUCCESS_CONTAINER] = RGB(0, 64, 34),
                        [MESH_UI_COLOR_ON_SUCCESS_CONTAINER] = RGB(255, 255, 255),
                        /* The same yellow as the primary, deliberately. This theme has three
                           colours and spends them on the distinction that matters: warning against
                           success and error, which the distinctness check holds - not warning
                           against a heading. */
                        [MESH_UI_COLOR_WARNING] = RGB(255, 214, 0),
                        [MESH_UI_COLOR_ON_WARNING] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_WARNING_CONTAINER] = RGB(72, 56, 0),
                        [MESH_UI_COLOR_ON_WARNING_CONTAINER] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_ERROR] = RGB(255, 120, 120),
                        [MESH_UI_COLOR_ON_ERROR] = RGB(0, 0, 0),
                        [MESH_UI_COLOR_ERROR_CONTAINER] = RGB(96, 0, 0),
                        [MESH_UI_COLOR_ON_ERROR_CONTAINER] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_RULE] = RGB(140, 140, 140),
                        [MESH_UI_COLOR_RULE_STRONG] = RGB(255, 214, 0),
                        [MESH_UI_COLOR_OUTLINE] = RGB(200, 200, 200),
                        /* Mid grey rather than the near-white the cursor fill is: on this palette
                           SURFACE_SEL and the success colour are both effectively white, so a track
                           borrowed from either would swallow the fill it is meant to contain. */
                        [MESH_UI_COLOR_METER_TRACK] = RGB(96, 96, 96),
                    },
                /* Two, not six. A palette of hues is exactly what this theme exists to do without,
                   so an avatar here is the yellow or the white and the initials carry the rest. */
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
                .font_id = "ui",
                .dark = true,
                .colors =
                    {
                        /* Okabe-Ito: sky blue for success, orange for error, yellow for warning,
                           reddish purple for the primary. All four stay distinct under deuteranopia
                           and protanopia, where the original green/yellow/red collapse into one
                           another. */
                        [MESH_UI_COLOR_BG] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_SURFACE_LOW] = RGB(0x10, 0x1B, 0x28),
                        [MESH_UI_COLOR_SURFACE] = RGB(0x14, 0x22, 0x32),
                        [MESH_UI_COLOR_SURFACE_HIGH] = RGB(0x1C, 0x2E, 0x42),
                        [MESH_UI_COLOR_SURFACE_SEL] = RGB(40, 80, 120),
                        [MESH_UI_COLOR_SURFACE_ACTIVE] = RGB(60, 110, 170),
                        /* A neutral, as on the dark theme: the notice is found by being the wrong
                           way up rather than by a hue, which is the one cue this theme can always
                           spend. */
                        [MESH_UI_COLOR_SURFACE_INVERSE] = RGB(226, 232, 240),
                        [MESH_UI_COLOR_TEXT] = RGB(226, 232, 240),
                        [MESH_UI_COLOR_TEXT_DIM] = RGB(146, 156, 170),
                        [MESH_UI_COLOR_TEXT_STRONG] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TEXT_ON_SEL] = RGB(255, 255, 255),
                        [MESH_UI_COLOR_TEXT_ON_SEL_DIM] = RGB(190, 208, 226),
                        [MESH_UI_COLOR_TEXT_ON_INVERSE] = RGB(0x0C, 0x16, 0x22),
                        /* The reddish purple taken down to a plum, with a pale tint of the same hue
                           on it. Held back in lightness rather than towards a neighbouring hue, so
                           the pair stays the primary under every dichromacy the theme is for. */
                        [MESH_UI_COLOR_PRIMARY] = RGB(204, 121, 167),
                        [MESH_UI_COLOR_ON_PRIMARY] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_PRIMARY_CONTAINER] = RGB(80, 46, 66),
                        [MESH_UI_COLOR_ON_PRIMARY_CONTAINER] = RGB(240, 194, 220),
                        /* A navy, not one of the Okabe-Ito eight: an outbound bubble is not a
                           status, so it must not spend one of the colours the statuses need to stay
                           separable in. */
                        [MESH_UI_COLOR_SECONDARY] = RGB(120, 175, 240),
                        [MESH_UI_COLOR_ON_SECONDARY] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_SECONDARY_CONTAINER] = RGB(34, 66, 104),
                        [MESH_UI_COLOR_ON_SECONDARY_CONTAINER] = RGB(228, 238, 248),
                        /* Bluish green. Decorative rather than a status, so it may sit nearer the
                           success sky blue than any two statuses are allowed to. */
                        [MESH_UI_COLOR_TERTIARY] = RGB(0, 158, 115),
                        [MESH_UI_COLOR_ON_TERTIARY] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_TERTIARY_CONTAINER] = RGB(0x10, 0x40, 0x34),
                        [MESH_UI_COLOR_ON_TERTIARY_CONTAINER] = RGB(150, 226, 200),
                        [MESH_UI_COLOR_SUCCESS] = RGB(86, 180, 233),
                        [MESH_UI_COLOR_ON_SUCCESS] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_SUCCESS_CONTAINER] = RGB(18, 58, 80),
                        [MESH_UI_COLOR_ON_SUCCESS_CONTAINER] = RGB(170, 220, 246),
                        /* Yellow. Against the error's orange it is separated by lightness rather
                           than by hue, which is the separation that survives dichromacy - and the
                           check in mesh_ui_theme_validate() is what proves it did. */
                        [MESH_UI_COLOR_WARNING] = RGB(240, 228, 66),
                        [MESH_UI_COLOR_ON_WARNING] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_WARNING_CONTAINER] = RGB(0x4A, 0x44, 0x14),
                        [MESH_UI_COLOR_ON_WARNING_CONTAINER] = RGB(244, 236, 150),
                        /* The container is a brown rather than a red: a failed bubble has to differ
                           from an outbound one without relying on the hue half the point of this
                           theme is to avoid. */
                        [MESH_UI_COLOR_ERROR] = RGB(230, 159, 0),
                        [MESH_UI_COLOR_ON_ERROR] = RGB(0x0A, 0x14, 0x1E),
                        [MESH_UI_COLOR_ERROR_CONTAINER] = RGB(96, 52, 20),
                        [MESH_UI_COLOR_ON_ERROR_CONTAINER] = RGB(250, 206, 150),
                        [MESH_UI_COLOR_RULE] = RGB(40, 80, 120),
                        [MESH_UI_COLOR_RULE_STRONG] = RGB(86, 180, 233),
                        [MESH_UI_COLOR_OUTLINE] = RGB(62, 100, 140),
                        [MESH_UI_COLOR_METER_TRACK] = RGB(40, 80, 120),
                    },
                /* The Okabe-Ito set again, this time as fills. They are the six that stay separable
                   under every common dichromacy, which is the only reason to spend six on avatars
                   at all - two tints that collapse into one for the reader are one tint. */
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

enum mesh_ui_color mesh_ui_family_role(enum mesh_ui_family family, enum mesh_ui_slot slot) {
    if ((int)family < 0 || family >= MESH_UI_FAMILY_COUNT) {
        family = MESH_UI_FAMILY_PRIMARY;
    }
    if ((int)slot < 0 || slot >= MESH_UI_SLOT_COUNT) {
        slot = MESH_UI_SLOT_BASE;
    }
    /* Arithmetic rather than a table because the enum is laid out for it, and the
       _Static_asserts in the header are what keep that true. */
    return (enum mesh_ui_color)(MESH_UI_COLOR_PRIMARY + (int)family * MESH_UI_SLOT_COUNT +
                                (int)slot);
}

struct mesh_ui_rgb mesh_ui_theme_family(const struct mesh_ui_theme *theme,
                                        enum mesh_ui_family family, enum mesh_ui_slot slot) {
    return mesh_ui_theme_color(theme, mesh_ui_family_role(family, slot));
}

enum mesh_ui_family mesh_ui_tone_family(enum mesh_ui_tone tone) {
    if (tone < MESH_UI_TONE_PRIMARY || tone >= MESH_UI_TONE_COUNT) {
        return MESH_UI_FAMILY_COUNT; /* the neutral three name no family */
    }
    return (enum mesh_ui_family)((int)tone - (int)MESH_UI_TONE_PRIMARY);
}

enum mesh_ui_tone mesh_ui_family_tone(enum mesh_ui_family family) {
    if ((int)family < 0 || family >= MESH_UI_FAMILY_COUNT) {
        return MESH_UI_TONE_NORMAL;
    }
    return (enum mesh_ui_tone)((int)MESH_UI_TONE_PRIMARY + (int)family);
}

enum mesh_ui_color mesh_ui_tone_role(enum mesh_ui_tone tone) {
    const enum mesh_ui_family family = mesh_ui_tone_family(tone);
    if (family != MESH_UI_FAMILY_COUNT) {
        /* A tone is ink, and BASE is the slot a family states for ink on the ground. */
        return mesh_ui_family_role(family, MESH_UI_SLOT_BASE);
    }
    switch (tone) {
    case MESH_UI_TONE_DIM:
        return MESH_UI_COLOR_TEXT_DIM;
    case MESH_UI_TONE_STRONG:
        return MESH_UI_COLOR_TEXT_STRONG;
    case MESH_UI_TONE_NORMAL:
    default:
        return MESH_UI_COLOR_TEXT;
    }
}

/*
 * The state layer, as a fixed-point mix.
 *
 * The percentages are the whole of the design: enough that the cursor is found without looking
 * for it, little enough that the ink the theme was validated against still reads on the result.
 * mesh_ui_theme_validate() checks the selected variant of every pair drawn this way, so these
 * two numbers cannot be raised without the tests saying which theme it broke.
 */
static const uint8_t k_state_mix_pct[MESH_UI_STATE_COUNT] = {
    [MESH_UI_STATE_REST] = 0U,
    [MESH_UI_STATE_SELECTED] = 12U,
    [MESH_UI_STATE_ACTIVE] = 20U,
};

static uint8_t mix_channel(uint8_t fill, uint8_t ink, unsigned pct) {
    const int delta = (int)ink - (int)fill;
    if (delta == 0) {
        return fill;
    }
    int scaled = (delta * (int)pct + (delta >= 0 ? 50 : -50)) / 100;
    if (scaled == 0) {
        /*
         * A twelfth of a four-step difference rounds to nothing, and a layer that resolves to
         * its own fill is a cursor that has left no mark. So a channel that differs at all
         * moves at least one step: the point of the layer is to be *found*, and one step is the
         * smallest amount of being found there is. It can never overshoot, because a delta of
         * one moved by one lands exactly on the ink and nothing here is asked for more.
         */
        scaled = delta > 0 ? 1 : -1;
    }
    return (uint8_t)((int)fill + scaled);
}

struct mesh_ui_rgb mesh_ui_theme_state_layer(struct mesh_ui_rgb fill, struct mesh_ui_rgb ink,
                                             enum mesh_ui_state state) {
    if ((int)state <= (int)MESH_UI_STATE_REST || state >= MESH_UI_STATE_COUNT) {
        return fill;
    }
    const unsigned pct = k_state_mix_pct[state];
    return (struct mesh_ui_rgb){
        .r = mix_channel(fill.r, ink.r, pct),
        .g = mix_channel(fill.g, ink.g, pct),
        .b = mix_channel(fill.b, ink.b, pct),
    };
}

struct mesh_ui_paint mesh_ui_theme_paint(const struct mesh_ui_theme *theme,
                                         enum mesh_ui_family family, enum mesh_ui_slot slot,
                                         enum mesh_ui_state state) {
    /* The pair is resolved from one slot, so an ink can only ever be the one checked against
       the fill beside it. A caller asking for ON_BASE gets the BASE pair, not an inverted one:
       there is one pair per half of a family and naming either half selects it. */
    const bool container = (slot == MESH_UI_SLOT_CONTAINER || slot == MESH_UI_SLOT_ON_CONTAINER);
    const enum mesh_ui_slot fill_slot = container ? MESH_UI_SLOT_CONTAINER : MESH_UI_SLOT_BASE;
    const enum mesh_ui_slot ink_slot = container ? MESH_UI_SLOT_ON_CONTAINER : MESH_UI_SLOT_ON_BASE;
    const struct mesh_ui_rgb fill = mesh_ui_theme_family(theme, family, fill_slot);
    const struct mesh_ui_rgb ink = mesh_ui_theme_family(theme, family, ink_slot);
    /*
     * A state layer goes on a container and never on a base, which is not a special case but
     * the definition of the two slots. A container is the colour held back so text can sit on
     * it, and the room it was held back by is exactly the room a layer has to move in; a base
     * is already the full-strength end - it is *what a pressed tonal control commits to* - so
     * there is nowhere further for it to go. Mixing its ink in anyway costs the contrast that
     * makes a saturated fill legible in the first place: it took every BASE/ON_BASE pair in
     * three of the four themes under 4.5:1, because those pairs sit near the floor by design.
     */
    return (struct mesh_ui_paint){
        .fill = container ? mesh_ui_theme_state_layer(fill, ink, state) : fill,
        .ink = ink,
    };
}

enum mesh_ui_tone mesh_ui_tone_for_load(int32_t permille, int32_t warn, int32_t bad) {
    /* Thresholds handed over the wrong way round would otherwise make the middle band
       unreachable and every reading BAD, which is the failure that hides itself: a screen
       permanently in the red looks like a mesh in trouble rather than like a caller's typo. */
    if (warn > bad) {
        const int32_t swap = warn;
        warn = bad;
        bad = swap;
    }
    if (permille >= bad) {
        return MESH_UI_TONE_ERROR;
    }
    if (permille >= warn) {
        return MESH_UI_TONE_WARNING;
    }
    return MESH_UI_TONE_SUCCESS;
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
        /* A theme that states no palette still has to answer, and the primary is a fill it
           already promises reads with the ground colour over it. */
        return theme->colors[MESH_UI_COLOR_PRIMARY];
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

uint32_t mesh_ui_theme_motion(const struct mesh_ui_theme *theme, enum mesh_ui_motion motion) {
    theme = theme_or_default(theme);
    if ((int)motion < 0 || (int)motion >= (int)MESH_UI_MOTION_COUNT) {
        return 0U;
    }
    return (uint32_t)theme->metrics.motion_ms[motion];
}

int mesh_ui_theme_radius(const struct mesh_ui_theme *theme, enum mesh_ui_shape shape, int scale) {
    theme = theme_or_default(theme);
    scale = mesh_ui_theme_clamp_scale(theme, scale);
    if (shape == MESH_UI_SHAPE_FULL) {
        /* Bigger than any panel this runs on. fb_fill_round_rect() clamps a radius to half the
           shorter side, so "as round as it goes" is answered where the box is finally known
           rather than guessed at here - a pill and a circle are the same request. */
        return INT16_MAX;
    }
    if ((int)shape < 0 || (int)shape >= (int)MESH_UI_SHAPE_FULL) {
        return 0;
    }
    return (int)theme->metrics.shape[shape] * scale;
}

int mesh_ui_theme_type_scale(const struct mesh_ui_theme *theme, enum mesh_ui_type type, int scale) {
    theme = theme_or_default(theme);
    const int body = mesh_ui_theme_clamp_scale(theme, scale);
    if ((int)type < 0 || (int)type >= (int)MESH_UI_TYPE_COUNT) {
        return body;
    }
    const int wanted = body + (int)theme->metrics.type_offset[type];
    /* Clamped rather than allowed out: at the top of the range a title collapses onto the body
       and at the bottom a label does, which is the scale degrading rather than the font
       registry being asked for a size it cannot rasterise. */
    if (wanted < MESH_UI_SCALE_MIN) {
        return MESH_UI_SCALE_MIN;
    }
    if (wanted > MESH_UI_SCALE_MAX) {
        return MESH_UI_SCALE_MAX;
    }
    return wanted;
}

int mesh_ui_theme_space(const struct mesh_ui_theme *theme, enum mesh_ui_space space, int scale) {
    theme = theme_or_default(theme);
    if ((int)space < 0 || (int)space >= (int)MESH_UI_SPACE_COUNT) {
        return 0;
    }
    const int halves = (int)theme->metrics.space[space];
    if (halves <= 0) {
        return 0;
    }
    scale = mesh_ui_theme_clamp_scale(theme, scale);
    const int pixels = (halves * scale) / 2;
    /* A theme that asked for a gap gets at least a pixel of one. Rounding a half-step down to
       nothing at a small scale is how an inset silently stops existing on exactly the themes
       that most need it to. */
    return pixels > 0 ? pixels : 1;
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
    /* A card is a SURFACE panel with the ordinary tones written on it, so every tone a card row
       can take owes that fill what it already owes the ground. Without these rows a theme could
       put its surface anywhere it liked and only the plain body text would notice. */
    {MESH_UI_COLOR_TEXT_STRONG, MESH_UI_COLOR_SURFACE, 4.5},
    {MESH_UI_COLOR_TEXT_ON_SEL, MESH_UI_COLOR_SURFACE_SEL, 4.5},
    {MESH_UI_COLOR_TEXT_ON_SEL, MESH_UI_COLOR_SURFACE_ACTIVE, 4.5},
    {MESH_UI_COLOR_TEXT_ON_SEL_DIM, MESH_UI_COLOR_SURFACE_SEL, 3.0},
    /* The raised tier carries two things now: the keyboard's draft box, and an inbound chat
       bubble. The draft box holds the text being composed and the bubble holds a message being
       read, so both want the body threshold rather than the secondary one - and TEXT is on it
       as well as TEXT_STRONG, because a bubble is written in the ordinary ink. */
    {MESH_UI_COLOR_TEXT, MESH_UI_COLOR_SURFACE_HIGH, 4.5},
    {MESH_UI_COLOR_TEXT_STRONG, MESH_UI_COLOR_SURFACE_HIGH, 4.5},
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_SURFACE_HIGH, 3.0},
    /* The clock on a resting outbound bubble: dim on the secondary container. Under the cursor
       the bubble switches to its own ink instead, which the family rules already cover. */
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_SECONDARY_CONTAINER, 3.0},
    /* The recessed tier is the tab strip's bar. Its labels are chrome - the inactive ones are
       drawn dim, and the active one is the primary container. */
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_SURFACE_LOW, 3.0},
    /* The snackbar. It is a sentence the user has four seconds to read while looking at
       something else, so it gets the body threshold rather than the secondary one - and the
       inverted fill is only worth having if what is written on it is legible. */
    {MESH_UI_COLOR_TEXT_ON_INVERSE, MESH_UI_COLOR_SURFACE_INVERSE, 4.5},
    /* Secondary: still has to be read, just not for long. */
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_BG, 3.0},
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_SURFACE, 3.0},
    /* Furniture: visible at all. An edge has to be findable from both sides - against the
       ground it sits on and against the fill it encloses - because on a theme whose surface is
       a step off the ground the edge is the whole of what says a container is there at all.
       Both containers that have one are here: the card on SURFACE, the draft box on
       SURFACE_HIGH. */
    {MESH_UI_COLOR_RULE, MESH_UI_COLOR_BG, 1.4},
    {MESH_UI_COLOR_OUTLINE, MESH_UI_COLOR_BG, 1.4},
    {MESH_UI_COLOR_OUTLINE, MESH_UI_COLOR_SURFACE, 1.4},
    {MESH_UI_COLOR_OUTLINE, MESH_UI_COLOR_SURFACE_HIGH, 1.4},
    /* The rule that closes the tab strip off meets the strip's own bar rather than the ground,
       and the active tab's pill - the primary container, and the only container drawn up
       there - sits on that same bar. Neither has to be *read*, but a tab indicator nobody can
       find is a tab strip with no current tab. */
    {MESH_UI_COLOR_RULE_STRONG, MESH_UI_COLOR_SURFACE_LOW, 1.4},
    {MESH_UI_COLOR_PRIMARY_CONTAINER, MESH_UI_COLOR_SURFACE_LOW, 1.4},
};

/*
 * Pairs whose ground is not a colour any theme states: a fill with a state layer over it.
 *
 * A selected chat bubble used to be a role of its own, matched by eye against the resting one
 * in every theme. It is derived now, which removes four roles - and derived is only safe if
 * what is written on the result is still checked, because the layer moves a fill *towards* its
 * own ink and so can only ever cost contrast. `layer` is the ink being mixed in, which is not
 * always the ink being checked: a timestamp on a selected bubble is TEXT_DIM read against a
 * fill that TEXT was mixed into.
 */
struct theme_state_pair {
    enum mesh_ui_color ink;
    enum mesh_ui_color ground;
    enum mesh_ui_color layer;
    enum mesh_ui_state state;
    double ratio;
};

static const struct theme_state_pair k_required_state[] = {
    /* The inbound bubble under the cursor: SURFACE_HIGH with its own body ink mixed in. */
    {MESH_UI_COLOR_TEXT, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, MESH_UI_STATE_SELECTED,
     4.5},
    {MESH_UI_COLOR_TEXT_DIM, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, MESH_UI_STATE_SELECTED,
     3.0},
    /* A bubble's sender line is the primary, and a critical alert heads its bubble in the error
       colour instead - so that pairing has to hold everywhere the primary one does, or the one
       message a theme must not swallow is the one it swallows. Both at rest and selected. */
    {MESH_UI_COLOR_PRIMARY, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, MESH_UI_STATE_REST,
     3.0},
    {MESH_UI_COLOR_PRIMARY, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, MESH_UI_STATE_SELECTED,
     3.0},
    {MESH_UI_COLOR_ERROR, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, MESH_UI_STATE_REST, 3.0},
    {MESH_UI_COLOR_ERROR, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, MESH_UI_STATE_SELECTED,
     3.0},
};

/*
 * How far apart two colours have to be before they are two colours.
 *
 * Contrast cannot answer this: two hues of the same lightness sit at 1.0:1 however different
 * they look, which is exactly the case that matters here - a green success and a red error are
 * near-identical by luminance and completely distinct to the eye. So this is a plain
 * channel-sum distance, out of a possible 765, and it is a crude guard rather than a
 * perceptual measure: it is here to catch a theme that has handed two *meanings* one colour,
 * not to rank palettes. 120 is roughly "these differ by half a channel somewhere".
 */
#define MESH_UI_STATUS_DISTANCE 120

static unsigned theme_distance(struct mesh_ui_rgb a, struct mesh_ui_rgb b) {
    const int dr = (int)a.r - (int)b.r;
    const int dg = (int)a.g - (int)b.g;
    const int db = (int)a.b - (int)b.b;
    return (unsigned)((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db));
}

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
    if (theme->metrics.meter_thickness == 0U) {
        if (reason != NULL) {
            snprintf(reason, reason_len, "%s", "a meter with no thickness draws nothing");
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

    for (size_t i = 0; i < sizeof k_required_state / sizeof k_required_state[0]; ++i) {
        const struct theme_state_pair *pair = &k_required_state[i];
        const struct mesh_ui_rgb ground = mesh_ui_theme_state_layer(
            theme->colors[pair->ground], theme->colors[pair->layer], pair->state);
        const double ratio = mesh_ui_theme_contrast(theme->colors[pair->ink], ground);
        if (ratio + 0.005 < pair->ratio) {
            if (reason != NULL) {
                snprintf(reason, reason_len,
                         "role %d on role %d in state %d is %.2f:1, needs %.1f:1", (int)pair->ink,
                         (int)pair->ground, (int)pair->state, ratio, pair->ratio);
            }
            return false;
        }
    }

    /*
     * Every family, the same six rules - which is the point of having families at all.
     *
     * This replaced two dozen hand-written rows, and the rows were the problem: a pair missing
     * from the table was a pair nothing checked, and that is how dim text on a selected
     * outbound bubble stayed at 1.9:1 on the dark palette for as long as it did. A family
     * cannot be added now without every one of its six contracts being checked, because there
     * is no list to forget to add it to.
     */
    for (int f = 0; f < MESH_UI_FAMILY_COUNT; ++f) {
        const enum mesh_ui_family family = (enum mesh_ui_family)f;
        static const struct {
            enum mesh_ui_slot ink; /* MESH_UI_SLOT_COUNT means "against a fixed role" */
            enum mesh_ui_slot fill;
            enum mesh_ui_color ground;
            enum mesh_ui_state state;
            double ratio;
        } k_family_rules[] = {
            /* Each half of a family is a fill with an ink checked against it. The container
               gets two more rows for the two state layers, because the layer moves a fill
               towards its own ink and can therefore only ever cost contrast - without them a
               container could be picked so close to its ink that being pressed tips it out.
               The base takes no layer, so it needs no such row; see mesh_ui_theme_paint(). */
            {MESH_UI_SLOT_ON_BASE, MESH_UI_SLOT_BASE, MESH_UI_COLOR_COUNT, MESH_UI_STATE_REST, 4.5},
            {MESH_UI_SLOT_ON_CONTAINER, MESH_UI_SLOT_CONTAINER, MESH_UI_COLOR_COUNT,
             MESH_UI_STATE_REST, 4.5},
            {MESH_UI_SLOT_ON_CONTAINER, MESH_UI_SLOT_CONTAINER, MESH_UI_COLOR_COUNT,
             MESH_UI_STATE_SELECTED, 4.5},
            {MESH_UI_SLOT_ON_CONTAINER, MESH_UI_SLOT_CONTAINER, MESH_UI_COLOR_COUNT,
             MESH_UI_STATE_ACTIVE, 4.5},
            /* BASE is ink as often as it is a fill - a status word on a card, a title on the
               body - so it owes both grounds the secondary threshold. */
            {MESH_UI_SLOT_COUNT, MESH_UI_SLOT_BASE, MESH_UI_COLOR_BG, MESH_UI_STATE_REST, 3.0},
            {MESH_UI_SLOT_COUNT, MESH_UI_SLOT_BASE, MESH_UI_COLOR_SURFACE, MESH_UI_STATE_REST, 3.0},
            /* A container is a fill on the body ground. It only has to be found, not read -
               the same "visible at all" bar the meter track gets, and for the same reason. */
            {MESH_UI_SLOT_COUNT, MESH_UI_SLOT_CONTAINER, MESH_UI_COLOR_BG, MESH_UI_STATE_REST, 1.2},
            /* The marker bar down a selected row is a family's BASE laid under the cursor
               fill, so it has to be tellable from that fill. The same "found, not read" bar,
               deliberately, rather than the 3:1 an ink would owe: the contrast theme's cursor
               fill is white and its one accent is a yellow that clears 1.4:1 by a hair, so a
               text threshold here would fail the shipped palette for a bar that is not text.
               It still catches the case that matters - a family whose base *is* the cursor
               fill, which is a marker nobody can see. */
            {MESH_UI_SLOT_COUNT, MESH_UI_SLOT_BASE, MESH_UI_COLOR_SURFACE_SEL, MESH_UI_STATE_REST,
             1.2},
        };
        for (size_t i = 0; i < sizeof k_family_rules / sizeof k_family_rules[0]; ++i) {
            const struct mesh_ui_rgb fill =
                mesh_ui_theme_family(theme, family, k_family_rules[i].fill);
            struct mesh_ui_rgb ink;
            struct mesh_ui_rgb ground;
            if (k_family_rules[i].ink == MESH_UI_SLOT_COUNT) {
                /* The family colour read *against* a neutral ground rather than written on. */
                ink = fill;
                ground = theme->colors[k_family_rules[i].ground];
            } else {
                ink = mesh_ui_theme_family(theme, family, k_family_rules[i].ink);
                ground = mesh_ui_theme_state_layer(fill, ink, k_family_rules[i].state);
            }
            const double ratio = mesh_ui_theme_contrast(ink, ground);
            if (ratio + 0.005 < k_family_rules[i].ratio) {
                if (reason != NULL) {
                    snprintf(reason, reason_len,
                             "family %d slot %d in state %d is %.2f:1, needs %.1f:1", f,
                             (int)k_family_rules[i].fill, (int)k_family_rules[i].state, ratio,
                             k_family_rules[i].ratio);
                }
                return false;
            }
        }
    }

    /*
     * The three families that carry a verdict have to be three colours.
     *
     * Nothing above catches this: success, warning and error each pass their own contrast
     * contract perfectly well while being the same colour as each other, and a theme where
     * "connected", "busy" and "failed" all read identically is worse than one that is merely
     * hard to read - it is confidently wrong. The other three families are exempt: primary,
     * secondary and tertiary are identity rather than verdict, and the contrast theme spends
     * its whole palette on making the verdicts loud.
     */
    {
        static const enum mesh_ui_family k_status[] = {
            MESH_UI_FAMILY_SUCCESS,
            MESH_UI_FAMILY_WARNING,
            MESH_UI_FAMILY_ERROR,
        };
        const size_t count = sizeof k_status / sizeof k_status[0];
        for (size_t i = 0; i < count; ++i) {
            for (size_t j = i + 1U; j < count; ++j) {
                const unsigned d =
                    theme_distance(mesh_ui_theme_family(theme, k_status[i], MESH_UI_SLOT_BASE),
                                   mesh_ui_theme_family(theme, k_status[j], MESH_UI_SLOT_BASE));
                if (d < MESH_UI_STATUS_DISTANCE) {
                    if (reason != NULL) {
                        snprintf(reason, reason_len, "families %d and %d are %u apart, needs %d",
                                 (int)k_status[i], (int)k_status[j], d, MESH_UI_STATUS_DISTANCE);
                    }
                    return false;
                }
            }
        }
    }

    /*
     * A meter is an empty track with a fill inside it, and both halves have to be visible or it
     * is not a meter: an unfindable track is a bar that vanishes when the reading is low, and a
     * fill that matches its track is one that vanishes when the reading is high. The track is
     * SURFACE_SEL on the ground and every fill is a tone, so the pairs to hold are those.
     *
     * 1.4:1 rather than a text ratio because neither half carries a word - this is the "a
     * hairline only has to be visible at all" bar, applied to something the eye is meant to
     * read as a length.
     *
     * Every family and nothing else, because a family is the whole of what a meter can be
     * filled with: fb_draw_meter() folds any tone that names no family back to the primary.
     * Checking the neutral tones instead would hold every theme to a pair nothing draws - the
     * contrast theme, whose STRONG *is* the cursor fill, would fail for a bar it will never
     * render - and checking a hand-written list of families is how the list went stale when a
     * fourth fill appeared.
     */
    {
        const struct mesh_ui_rgb track = theme->colors[MESH_UI_COLOR_METER_TRACK];
        /* Both grounds a meter is actually drawn on: the body, for the bar in a settings row,
           and a card's surface, for the one under the Status screen's airtime figures. A track
           validated against one and invisible on the other is a bar that exists on one screen. */
        static const enum mesh_ui_color k_meter_grounds[] = {MESH_UI_COLOR_BG,
                                                             MESH_UI_COLOR_SURFACE};
        for (size_t i = 0; i < sizeof k_meter_grounds / sizeof k_meter_grounds[0]; ++i) {
            const double ratio = mesh_ui_theme_contrast(track, theme->colors[k_meter_grounds[i]]);
            if (ratio + 0.005 < 1.2) {
                if (reason != NULL) {
                    snprintf(reason, reason_len,
                             "the meter track on role %d is %.2f:1, needs 1.2:1",
                             (int)k_meter_grounds[i], ratio);
                }
                return false;
            }
        }
        for (int f = 0; f < MESH_UI_FAMILY_COUNT; ++f) {
            const struct mesh_ui_rgb fill =
                mesh_ui_theme_family(theme, (enum mesh_ui_family)f, MESH_UI_SLOT_BASE);
            const double ratio = mesh_ui_theme_contrast(fill, track);
            if (ratio + 0.005 < 1.4) {
                if (reason != NULL) {
                    snprintf(reason, reason_len,
                             "meter family %d on its track is %.2f:1, needs 1.4:1", f, ratio);
                }
                return false;
            }
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
