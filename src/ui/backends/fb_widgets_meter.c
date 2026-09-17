#define _POSIX_C_SOURCE 200809L

/*
 * The quantities. The meter and the slider ease towards their value and take the state mutably;
 * the staircase, the sparkline, the proportion bar and the chart are const - there is nothing
 * to step, because a reading that is already history does not move.
 */

#include "fb_widgets_meter.h"
#include "fb_widgets_control.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/anim.h"
#include "mesh/ui/emoji.h"

/* ---- the meter ----------------------------------------------------------------------------- */

/*
 * How long a determinate fill takes to reach a new reading.
 *
 * Longer than the switch's, and for the opposite reason. A switch answers a press, so it has to
 * feel immediate; a meter answers a *sample*, and the samples are seconds apart - the airtime
 * figures arrive on the radio's own schedule, the download's byte count is whatever the file on
 * disk had grown to when the loop last looked. Easing across that gap is the whole trick: the
 * bar spends its time moving between two readings instead of sitting still and then jumping,
 * and what the eye gets is the rate rather than the samples.
 */
#define FB_METER_MOTION MESH_UI_MOTION_LONG

/* One pass of an indeterminate pill. Slow enough to read as travel rather than as flicker, fast
   enough that a screen with one on it does not feel stalled. */
#define FB_METER_LOOP_MOTION MESH_UI_MOTION_LOOP

/* How much of the track the pill covers. A third is the proportion Material's indeterminate bar
   settles at, and it is about the shortest that still reads as a bar rather than as a dot. */
#define FB_METER_PILL_PCT 34

/*
 * Where in its cycle the pill starts, in permille.
 *
 * A loop begins at 0, which for the travel below is the pill entirely off the leading edge -
 * so a bar drawn on the frame it first appears would be an empty track, and the one frame that
 * has to say "this is working" would say the opposite. Roughly two fifths in is where the eased
 * travel first brings the whole pill onto the track, so the widget appears with something in
 * it and loops normally from there.
 *
 * A rotation of a periodic function, not a special case for the first frame: every cycle starts
 * here, so nothing has to remember whether this is the first one.
 */
#define FB_METER_PILL_PHASE 400

int fb_meter_thickness(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int thickness = (int)fb_metrics(state)->meter_thickness * (scale > 0 ? scale : 1);
    return thickness > 1 ? thickness : 1;
}

/*
 * The fill's colour.
 *
 * A family tone and nothing else. The neutral three - normal, dim, strong - fold back to the
 * primary rather than being drawn as asked, and that is not defensiveness: a fill nobody has
 * validated against MESH_UI_COLOR_METER_TRACK is a bar that vanishes on some theme somebody has
 * not opened yet, and the primary is the one answer that is always right for "something is
 * here". Every family *is* validated against the track, so this is now a question about the
 * kind of tone rather than a list to keep in step with the validator - which is what the list
 * that used to be here was, and it went stale the moment a fourth fill existed.
 */
static enum mesh_ui_tone fb_meter_tone(enum mesh_ui_tone tone) {
    return mesh_ui_tone_family(tone) != MESH_UI_FAMILY_COUNT ? tone : MESH_UI_TONE_PRIMARY;
}

/*
 * Where a boundary is marked on the track, or -1 for one there is no point marking.
 *
 * The ends are refused deliberately. A notch at 0 or at the full width is not a threshold the
 * eye can locate against anything - it is the edge of the track, which is already drawn - and a
 * band whose boundary sits off the scale is a caller's domain and threshold disagreeing, which
 * is better shown as an unmarked bar than as a mark in the wrong place.
 */
static int fb_band_mark(const struct fb_meter *meter, int32_t boundary) {
    const int32_t permille = mesh_ui_scale_permille(meter->scale, boundary);
    if (permille <= 0 || permille >= MESH_UI_ANIM_ONE) {
        return -1;
    }
    return (int)(((int64_t)meter->rect.w * permille) / MESH_UI_ANIM_ONE);
}

void fb_draw_meter(struct mesh_ui_backend_fb_state *state, const struct fb_meter *meter) {
    if (meter == NULL || meter->rect.w <= 0 || meter->rect.h <= 0) {
        return;
    }
    const struct fb_rect r = meter->rect;
    const int damage_pad = fb_space(state, MESH_UI_SPACE_XS);
    fb_animation_damage(state, r.x - damage_pad, r.y - damage_pad, r.w + 2 * damage_pad,
                        r.h + 2 * damage_pad);
    /* A pill, always: a bar with square ends reads as a region of the screen that has been
       filled in, and one with round ends reads as a quantity in a container. */
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);

    /*
     * Its own ground under a cursor fill, for the reason the switch lays one.
     *
     * The track is validated against the body and against a card, which are the two grounds a
     * meter is drawn on - not against the cursor fill, and on two of the four themes it *is*
     * the cursor fill. Without this the track would disappear on precisely the row being
     * pointed at, leaving a fill floating in space with no length to be read against.
     */
    if (meter->selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad, radius + pad,
                           fb_color(state, meter->ground));
    }

    fb_fill_round_rect(state, r.x, r.y, r.w, r.h, radius,
                       fb_color(state, MESH_UI_COLOR_METER_TRACK));

    if (meter->kind == FB_METER_INDETERMINATE) {
        /* A band says where a reading changes meaning and an indeterminate bar has no reading,
           so nothing here consults one: the pill is drawn in the tone it was given. */
        const struct mesh_ui_rgb ink = fb_tone_color(state, fb_meter_tone(meter->tone));
        /*
         * A pill crossing the track, from entirely off the leading edge to entirely off the
         * trailing one. Both ends of the travel are off the track on purpose: the loop's wrap
         * from ONE back to 0 then happens while nothing is drawn, so a bar that never finishes
         * also never visibly restarts.
         *
         * Eased rather than linear, so it accelerates in and settles out instead of sliding at
         * one speed - which is the difference between a thing that is working and a thing on a
         * conveyor belt.
         */
        const int pill = r.w * FB_METER_PILL_PCT / 100 > 1 ? r.w * FB_METER_PILL_PCT / 100 : 1;
        /* The phase is added before the curve, not after: shifting the sawtooth rotates where
           the cycle begins and leaves the wrap exactly where it was - at the point the pill is
           off the track entirely, which is what keeps a loop that never ends from visibly
           restarting. Shifting the eased value instead would put a jump in the middle of the
           travel. */
        const int32_t phase = (mesh_ui_anim_loop(&state->anim, meter->id, state->now_ms,
                                                 fb_motion(state, FB_METER_LOOP_MOTION)) +
                               FB_METER_PILL_PHASE) %
                              MESH_UI_ANIM_ONE;
        const int32_t t = mesh_ui_ease(MESH_UI_EASE_IN_OUT, phase);
        const int travel = r.w + pill;
        int x = r.x - pill + (int)(((int64_t)travel * t) / MESH_UI_ANIM_ONE);
        int w = pill;
        /* Clipped to the track rather than drawn past it: fb_fill_round_rect() is happy to fill
           outside a container it knows nothing about, and the container here is the thing that
           gives the pill its meaning. */
        if (x < r.x) {
            w -= r.x - x;
            x = r.x;
        }
        if (x + w > r.x + r.w) {
            w = r.x + r.w - x;
        }
        if (w > 0) {
            fb_fill_round_rect(state, x, r.y, w, r.h, radius, ink);
        }
        return;
    }

    /*
     * The reading onto the track. Once, here, rather than at the call site - which is the whole
     * reason the domain travels with the reading: the fill's length and the boundary marks
     * below are then measured by one piece of arithmetic and cannot land in different places.
     */
    const int32_t value = mesh_ui_scale_permille(meter->scale, meter->value);
    /* The band is asked in the meter's own units rather than in permille, so a boundary is
       compared against the figure a caller stated rather than against a rounded position. */
    const struct mesh_ui_rgb ink = fb_tone_color(
        state, fb_meter_tone(mesh_ui_band_tone(meter->band, meter->value, meter->tone)));

    /* Where the fill has got to, which is not where the reading is: see FB_METER_MOTION. */
    const int32_t position =
        mesh_ui_anim_track(&state->anim, meter->id, state->now_ms, value,
                           fb_motion(state, FB_METER_MOTION), MESH_UI_EASE_OUT);

    int fill = (int)(((int64_t)r.w * position) / MESH_UI_ANIM_ONE);
    if (fill <= 0) {
        /* A reading that is not zero draws something, however small. Rounding a real 0.4% down
           to no pixels at all says "nothing is happening", which is the one thing the bar is
           there to distinguish from. Exactly zero draws an empty track, as it should. */
        fill = position > 0 ? 1 : 0;
    }
    if (fill > 0) {
        fb_fill_round_rect(state, r.x, r.y, fill, r.h, radius, ink);
    }

    if (meter->band == NULL) {
        return;
    }
    /*
     * The boundaries, cut *out* of the bar rather than laid on top of it.
     *
     * A notch in the ground colour is the one mark that reads the same whether or not the fill
     * has reached it: over the track it is a gap in the track, over the fill it is a gap in the
     * fill, and either way the eye sees the bar divided where the meaning divides. A mark drawn
     * in an ink of its own would need a colour validated against both, which is two more
     * contracts every theme would have to satisfy to say something the absence of ink already
     * says.
     *
     * Same ground the widget lays under a selected track above, for the same reason: that is
     * what is behind the bar on the row the cursor is on.
     */
    const int notch = fb_space(state, MESH_UI_SPACE_XS) > 0 ? fb_space(state, MESH_UI_SPACE_XS) : 1;
    /* The ground the *row* is on, not the panel's: a notch is a gap, and a gap is only a gap
       when it is the colour of what is behind the bar. On a card, BG would punch a hole. */
    const struct mesh_ui_rgb ground = fb_color(state, meter->ground);
    const int32_t bounds[] = {meter->band->warn, meter->band->bad};
    for (size_t i = 0U; i < sizeof bounds / sizeof bounds[0]; i++) {
        const int mark = fb_band_mark(meter, bounds[i]);
        if (mark < 0) {
            continue;
        }
        int x = r.x + mark - notch / 2;
        int w = notch;
        if (x < r.x) {
            w -= r.x - x;
            x = r.x;
        }
        if (x + w > r.x + r.w) {
            w = r.x + r.w - x;
        }
        if (w > 0) {
            fb_fill_rect(state, x, r.y, w, r.h, ground);
        }
    }
}

/* ---- the slider ----------------------------------------------------------------------------- */

/*
 * The handle travels to each new value rather than appearing at it, on the meter's terms and
 * for the same reason - a control the reader just pressed should be seen to move, because that
 * is what says the press landed. EASE_OUT rather than IN_OUT: this follows a button press, and
 * a press wants a control that leaves immediately and settles.
 */
#define FB_SLIDER_MOTION MESH_UI_MOTION_SHORT

/*
 * The track, as a multiple of a meter's, and the handle as a multiple of the track.
 *
 * A control is drawn heavier than a reading of the same width, and that is not a preference: a
 * meter is looked at when the eye is already on the row, while a slider has to be *found* before
 * it can be aimed at, from wherever the cursor was. At a meter's thickness across a whole row it
 * reads as a rule with a mark on it.
 *
 * Two handle heights rather than one, because the row has to reserve the taller of them whether
 * or not the cursor is here: a handle that grew the row it is on would push every row below it
 * down as the cursor arrived, which is the correction §11 made to the progress bar and §8 made
 * to a card's focus ring. So the box is always the focused size and the resting handle is drawn
 * short inside it.
 */
#define FB_SLIDER_TRACK 2
#define FB_SLIDER_HANDLE_REST 3 /* in halves of a track */
#define FB_SLIDER_HANDLE_FOCUS 4

int fb_slider_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    return fb_meter_thickness(state, scale) * FB_SLIDER_TRACK * FB_SLIDER_HANDLE_FOCUS / 2;
}

/*
 * The choices, marked on the track.
 *
 * Cut out of it in the ground colour rather than laid on it in an ink of their own - the band
 * notches' arrangement, and the same argument: a gap reads the same over the filled half as over
 * the empty one, and a mark with a colour would be two more contracts every theme has to satisfy
 * to say what an absence already says. It follows that this must run *after* whatever it is
 * cutting into, which is the one thing about it that is easy to get wrong.
 *
 * Drawn only where they can be told apart. A settings field may offer four choices or twelve,
 * and twelve notches on a narrow panel is a dashed line rather than a set of stops - so the
 * component decides, from the width the row actually gave it, and an unmarked track is the
 * honest answer for a list too long to mark. The ends are skipped for the reason the band's are:
 * a notch at the very edge of a track is the edge of the track.
 */
static void fb_slider_stops(const struct mesh_ui_backend_fb_state *state,
                            const struct fb_rect *track, int handle_w, uint32_t stops,
                            struct mesh_ui_rgb ground) {
    const int notch = fb_space(state, MESH_UI_SPACE_XS) > 0 ? fb_space(state, MESH_UI_SPACE_XS) : 1;
    const int travel = track->w - handle_w;
    if (stops < 2U || travel <= 0 || (uint32_t)travel < (stops - 1U) * (uint32_t)(notch * 4)) {
        return;
    }
    for (uint32_t stop = 1U; stop + 1U < stops; ++stop) {
        const int x = track->x + handle_w / 2 +
                      (int)(((int64_t)travel * stop) / (int64_t)(stops - 1U)) - notch / 2;
        if (x >= track->x && x + notch <= track->x + track->w) {
            fb_fill_rect(state, x, track->y, notch, track->h, ground);
        }
    }
}

void fb_draw_slider(struct mesh_ui_backend_fb_state *state, const struct fb_slider *slider) {
    if (slider == NULL || slider->rect.w <= 0 || slider->rect.h <= 0) {
        return;
    }
    const struct fb_rect box = slider->rect;
    const int damage_pad = fb_space(state, MESH_UI_SPACE_XS);
    fb_animation_damage(state, box.x - damage_pad, box.y - damage_pad, box.w + 2 * damage_pad,
                        box.h + 2 * damage_pad);

    /* The track is a slice of the box the handle has the rest of, centred in it - so the ends of
       the track and the middle of the handle are on one line however tall either is. Recovered
       from the box rather than measured again, because a row may have been given less than it
       asked for and the control has to stay inside what it got. */
    const int full = box.h * 2 / FB_SLIDER_HANDLE_FOCUS;
    const int thickness = full > 1 ? full : 1;
    const struct fb_rect track = {
        .x = box.x, .y = box.y + (box.h - thickness) / 2, .w = box.w, .h = thickness};
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);

    /*
     * Its own ground under a cursor fill, the meter's arrangement and the switch's: the track
     * and the handle are both contracted against the body, and on two of the four themes the
     * cursor fill *is* the resting track - so without this the control disappears on precisely
     * the row it is being edited from.
     *
     * The whole box rather than the track, because the handle stands outside the track and the
     * gaps that separate it from the fill are drawn in this colour.
     */
    if (slider->selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad,
                           radius + pad, ground);
    }

    fb_fill_round_rect(state, track.x, track.y, track.w, track.h, radius,
                       fb_color(state, MESH_UI_COLOR_METER_TRACK));

    /* Narrower than the track is thick, which is the shape Material settled on and the right one
       here for a reason of its own: the handle marks a *position*, and a wide one is a range. */
    const int handle_w = thickness * 3 / 4 > 2 ? thickness * 3 / 4 : 2;
    /* The handle's centre travels the track less its own width, so the control reads as full at
       the last stop and empty at the first instead of hanging off either end. */
    const int travel = track.w - handle_w;

    /* A value the track has no room for: the marks it offers, and nothing claiming to be among
       them. Drawn here because there is no fill to draw them over. */
    if (slider->unplaced) {
        fb_slider_stops(state, &track, handle_w, slider->stops, ground);
        return;
    }

    int32_t target = slider->position;
    if (target < 0) {
        target = 0;
    } else if (target > MESH_UI_ANIM_ONE) {
        target = MESH_UI_ANIM_ONE;
    }
    const int32_t position =
        slider->id != 0U ? mesh_ui_anim_track(&state->anim, slider->id, state->now_ms, target,
                                              fb_motion(state, FB_SLIDER_MOTION), MESH_UI_EASE_OUT)
                         : target;

    const struct mesh_ui_rgb ink = fb_tone_color(state, fb_meter_tone(slider->tone));
    const int handle_x =
        track.x + (travel > 0 ? (int)(((int64_t)travel * position) / MESH_UI_ANIM_ONE) : 0);
    const int active = handle_x - track.x;
    if (active > 0) {
        fb_fill_round_rect(state, track.x, track.y, active, track.h, radius, ink);
    }

    /* After both halves of the track are down, never before either.
     *
     * A notch is a gap cut out of whatever is there, which is the whole reason it needs no ink
     * of its own - and a gap painted before the fill is a gap the fill closes. Drawn early, the
     * stops behind the handle vanished one by one as the value climbed, and at the top of the
     * scale a track that offers a dozen choices showed none of them. Same order fb_draw_meter()
     * cuts a band's boundaries in, for the same reason. */
    fb_slider_stops(state, &track, handle_w, slider->stops, ground);

    /*
     * The handle, and the gap that separates it from the fill it ends.
     *
     * Material leaves that gap and it is not decoration here either: the active track and the
     * handle are one ink, so without it the two are a single shape and the control has no
     * position to read - it is a bar with a bulge. A gap in the ground is what makes the handle a
     * thing sitting *on* the track.
     */
    const int handle_h =
        thickness * (slider->selected ? FB_SLIDER_HANDLE_FOCUS : FB_SLIDER_HANDLE_REST) / 2;
    const int handle_y = box.y + (box.h - handle_h) / 2;
    /* Wide enough to be a gap rather than a seam: the handle and the fill it ends are one ink,
       so this is the whole of what separates them. */
    const int gap = fb_space(state, MESH_UI_SPACE_SM) > 1 ? fb_space(state, MESH_UI_SPACE_SM) : 1;
    fb_fill_rect(state, handle_x - gap, track.y, handle_w + 2 * gap, track.h, ground);
    fb_fill_round_rect(state, handle_x, handle_y, handle_w, handle_h, handle_w / 2, ink);
}

/* ---- the signal staircase ------------------------------------------------------------------ */

void fb_draw_signal(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                    uint8_t level, struct mesh_ui_rgb ink, struct mesh_ui_rgb unlit) {
    if (box == NULL || box->w <= 0 || box->h <= 0) {
        return;
    }
    /*
     * Rungs and the gaps between them out of the width the slot was given, rather than a stated
     * pixel size: this is drawn beside text at whatever glyph scale the theme picked, and a
     * staircase that did not grow with it would be a set of ticks next to large type on the one
     * theme somebody chose for legibility.
     *
     * The gap is taken first and floored at a pixel. A staircase whose rungs touch is a filled
     * block, and a block has no rungs to count - which is the entire content of the widget.
     */
    const int steps = (int)MESH_UI_SIGNAL_STEPS;
    int gap = box->w / (steps * 4);
    if (gap < 1) {
        gap = 1;
    }
    int rung = (box->w - (steps - 1) * gap) / steps;
    if (rung < 1) {
        rung = 1;
    }
    const int bottom = box->y + box->h;
    const int radius = fb_radius(state, MESH_UI_SHAPE_SM);

    for (int i = 0; i < steps; i++) {
        /*
         * Rising left to right, bottom aligned, the shortest rung a quarter of the tallest.
         * Every rung is drawn whether or not it is lit - see FB_TRAILING_SIGNAL: what is being
         * read is lit rungs against a constant total, and an indicator that shortened as the
         * signal fell would be claiming a proportion four buckets cannot support.
         */
        int h = box->h * (i + 1) / steps;
        if (h < 1) {
            h = 1;
        }
        const int x = box->x + i * (rung + gap);
        fb_fill_round_rect(state, x, bottom - h, rung, h, radius, i < (int)level ? ink : unlit);
    }
}

/* ---- the sparkline ------------------------------------------------------------------------- */

int fb_sparkline_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int height = fb_line_adv(state, scale) - scale;
    return height > 1 ? height : 1;
}

/*
 * How thick the line is drawn, and how thick its floor is.
 *
 * A stroke a single pixel wide is what a line on a desktop is and it is the wrong answer on a
 * panel with 1024 pixels across 3.2 inches and no anti-aliasing: a diagonal run of single pixels
 * is a dotted line held at arm's length. The glyph scale is what everything else here grows
 * with, so the stroke grows with it too - a theme picked for legibility gets a legible line
 * rather than the same hairline beside larger type.
 */
static int fb_spark_stroke(int scale) {
    const int stroke = (scale > 0 ? scale : 1) / 2;
    return stroke > 1 ? stroke : 1;
}

/*
 * One segment, drawn a pixel column at a time.
 *
 * Bresenham's is the usual answer and this is not it, deliberately: what a column-wise walk
 * gives that a line rasteriser does not is that consecutive columns are *joined* by
 * construction - each fills from where the last one ended to where this one lands - so a steep
 * segment is a connected stroke rather than a ladder of separated pixels. On a series whose x
 * axis is time, steep is the ordinary case: two readings a minute apart on a line spanning an
 * hour land within a few columns of each other.
 */
static void fb_spark_segment(const struct mesh_ui_backend_fb_state *state, int x0, int y0, int x1,
                             int y1, int stroke, struct mesh_ui_rgb color) {
    if (x1 < x0) {
        const int swap_x = x0, swap_y = y0;
        x0 = x1;
        y0 = y1;
        x1 = swap_x;
        y1 = swap_y;
    }
    const int columns = x1 - x0;
    if (columns == 0) {
        /* Two readings the clock could not separate, or a series projected onto a box narrower
           than it has samples: a vertical connector rather than nothing, so the line still
           passes through both values. */
        const int top = y0 < y1 ? y0 : y1;
        const int bottom = y0 > y1 ? y0 : y1;
        fb_fill_rect(state, x0, top, stroke, bottom - top + stroke, color);
        return;
    }
    int previous = y0;
    for (int i = 0; i <= columns; ++i) {
        const int y = y0 + (int)(((int64_t)(y1 - y0) * i) / columns);
        const int top = y < previous ? y : previous;
        const int bottom = y > previous ? y : previous;
        fb_fill_rect(state, x0 + i, top, stroke, bottom - top + stroke, color);
        previous = y;
    }
}

void fb_draw_sparkline(const struct mesh_ui_backend_fb_state *state,
                       const struct fb_sparkline *spark) {
    if (spark == NULL || spark->points == NULL || spark->rect.w <= 0 || spark->rect.h <= 0) {
        return;
    }
    const struct mesh_ui_polyline *points = spark->points;
    /* One reading is a level, not a trend - and an empty box drawn against a radio that has
       reported once says "nothing is happening", which is the claim the whole component exists
       to avoid making by accident. Nothing is drawn, floor included. */
    if (points->count < 2U) {
        return;
    }

    const struct fb_rect r = spark->rect;
    const int stroke = fb_spark_stroke(state->scale);
    /* The line is placed so that both ends of the domain are inside the box: a reading at the
       top of its scale draws its stroke against the top edge rather than half outside it. */
    const int travel = r.h > stroke ? r.h - stroke : 0;
    const int span = r.w > 1 ? r.w - 1 : 0;

    /*
     * The floor: the bottom of the domain, drawn in the same role the meter's empty track takes
     * - which is what gives a line something to be read against without adding a colour any
     * theme has to be validated for.
     *
     * Under the cursor it takes the row's quiet ink instead, exactly as an unlit rung does and
     * for the same reason: two of the four themes make the track the cursor fill, so a floor in
     * that role would vanish on precisely the row being pointed at.
     */
    const struct mesh_ui_rgb floor_ink = spark->selected
                                             ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                                             : fb_color(state, MESH_UI_COLOR_METER_TRACK);
    fb_fill_rect(state, r.x, r.y + r.h - stroke, r.w, stroke, floor_ink);

    /*
     * The line.
     *
     * Its tone on the ground, and the row's selected ink under the cursor - which is the
     * staircase's rule, not a second one. A family tone is validated against the body and
     * against a card; it is not validated against the cursor fill, and on the contrast theme
     * that fill is white while the primary is yellow, so a stroke drawn in the tone there is a
     * line nobody can see on precisely the row being pointed at. The meter answers this by
     * laying a ground of its own under its track; a line has no track to lay one under - it is
     * a stroke among the row's words - so it takes the pairing those words take.
     */
    const struct mesh_ui_rgb ink = spark->selected
                                       ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                                       : fb_tone_color(state, fb_meter_tone(spark->tone));
    int previous_x = 0;
    int previous_y = 0;
    for (uint32_t i = 0U; i < points->count && i < MESH_UI_SERIES_MAX; ++i) {
        const struct mesh_ui_point *point = &points->items[i];
        const int x = r.x + (int)(((int64_t)point->x * span) / MESH_UI_ANIM_ONE);
        /* Up from the bottom: permille of the domain is a height, and a height on a panel whose
           origin is its top corner is a subtraction. */
        const int y = r.y + travel - (int)(((int64_t)point->y * travel) / MESH_UI_ANIM_ONE);
        if (!point->gap && i > 0U) {
            fb_spark_segment(state, previous_x, previous_y, x, y, stroke, ink);
        }
        previous_x = x;
        previous_y = y;
    }

    /*
     * The newest reading, marked.
     *
     * A line has two ends and nothing about a stroke says which of them is now. On a trend that
     * is the whole reading - a line that falls left to right and one that rises are the same
     * picture read backwards - so the end that is the present carries a square three times the
     * stroke, which is the smallest mark that is still findable against the line it ends.
     */
    const int mark = stroke * 3;
    /* Centred on the stroke's own centre, then held inside the box: the newest reading is at the
       trailing edge by construction, so an uncentred square would hang over whatever the slot
       was measured to keep clear of. */
    int mark_x = previous_x + stroke / 2 - mark / 2;
    int mark_y = previous_y + stroke / 2 - mark / 2;
    if (mark_x > r.x + r.w - mark) {
        mark_x = r.x + r.w - mark;
    }
    if (mark_x < r.x) {
        mark_x = r.x;
    }
    if (mark_y > r.y + r.h - mark) {
        mark_y = r.y + r.h - mark;
    }
    if (mark_y < r.y) {
        mark_y = r.y;
    }
    fb_fill_rect(state, mark_x, mark_y, mark, mark, ink);
}

/* ---- the proportion bar --------------------------------------------------------------------- */

/*
 * The two halves of the seam layout.h keeps with theme.h, held equal where they finally meet.
 *
 * A part is layout's idea and the colour it takes is the theme's, so neither header includes the
 * other and each states its own count - the same split MESH_WAYPOINT_NAME_MAX makes across the
 * store's seam. This is the one translation unit that sees both, so this is where the two are
 * proved to agree, at compile time rather than by a test that has to be remembered.
 */
MESH_UI_STATIC_ASSERT((int)MESH_UI_PROPORTION_PARTS == (int)MESH_UI_SERIES_COLORS,
                      "a composition may have exactly as many parts as there are series colours");

int fb_proportion_thickness(const struct mesh_ui_backend_fb_state *state, int scale) {
    return fb_meter_thickness(state, scale);
}

void fb_draw_proportion(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_proportion *bar) {
    if (bar == NULL || bar->count < 2U || bar->rect.w <= 0 || bar->rect.h <= 0) {
        return;
    }
    const struct fb_rect r = bar->rect;

    int32_t widths[MESH_UI_PROPORTION_PARTS];
    const uint32_t parts = mesh_ui_proportion_split(bar->values, bar->count, r.w, widths);
    if (parts == 0U) {
        /* Nothing was heard at all, so there is no whole to divide. An empty bar here would say
           the parts were all zero, which is a reading; this is the absence of one. */
        return;
    }

    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);
    /* The meter's ground under a cursor fill, for the meter's reason - see `selected`. */
    if (bar->selected) {
        const int pad = fb_space(state, MESH_UI_SPACE_XS);
        fb_fill_round_rect(state, r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad, radius + pad,
                           fb_color(state, MESH_UI_COLOR_BG));
    }

    /*
     * Widest first, each part a pill from the bar's left edge to where that part ends.
     *
     * Not one rectangle per part, which is the obvious way and loses both caps: a plain rect over
     * the last part squares off the round end the bar shares with every other bar on the card,
     * and the first part's round end has nothing to sit in. Drawn this way the outermost fill
     * lays the right-hand cap, each narrower one lands on top with its own left-hand cap in the
     * same place, and the part drawn last owns the left end - so the two ends of the bar are the
     * meter's ends and the boundaries between parts are the only new edges on it.
     *
     * At this radius - a pill on a bar a few pixels tall clamps to a pixel or two - a boundary
     * comes out as a softened vertical edge rather than as a visible bulge.
     */
    int end = r.w;
    for (uint32_t i = parts; i-- > 0U;) {
        if (widths[i] > 0 && end > 0) {
            fb_fill_round_rect(state, r.x, r.y, end, r.h, radius,
                               mesh_ui_theme_series(state->theme, i));
        }
        end -= widths[i];
    }

    /*
     * And a gap cut at each boundary, in the ground the bar is drawn on.
     *
     * The meter's band notches, doing the same job one level along: two parts of a composition
     * are two fills meeting with nothing between them, and the palette only promises they are
     * 1.4:1 apart - which is a difference the eye finds reliably when there is an edge to find it
     * at, and less reliably across a seam it has to decide is there. A gap is that edge, and it
     * is drawn in the absence of ink for the reason the notches are: an ink of its own would be
     * one more pair every theme had to be validated for, to say what a hole already says.
     *
     * In the caller's ground rather than in MESH_UI_COLOR_BG, which is where this differs from
     * the band notch it is otherwise copying. A notch divides a bar the eye has already found;
     * these gaps have to be *invisible*, and a bar on a card whose gaps are the body's colour has
     * stripes in it rather than divisions. Under a cursor fill it is the pad above that is
     * behind the bar, so that is what the gaps take there.
     *
     * It costs each part half a pixel of length at one end. That is the same price the band marks
     * pay and it is the right way round: the boundary is what the picture is *for*.
     */
    const int gap = fb_space(state, MESH_UI_SPACE_XS) > 0 ? fb_space(state, MESH_UI_SPACE_XS) : 1;
    const struct mesh_ui_rgb ground =
        bar->selected ? fb_color(state, MESH_UI_COLOR_BG) : bar->ground;
    int boundary = 0;
    for (uint32_t i = 0; i + 1U < parts; ++i) {
        boundary += widths[i];
        if (widths[i] == 0) {
            continue; /* a part that is not there has no edge of its own */
        }
        int x = r.x + boundary - gap / 2;
        int w = gap;
        /* Held inside the bar, so the gap at the last boundary cannot eat the round end. */
        if (x < r.x) {
            w -= r.x - x;
            x = r.x;
        }
        if (x + w > r.x + r.w) {
            w = r.x + r.w - x;
        }
        if (w > 0) {
            fb_fill_rect(state, x, r.y, w, r.h, ground);
        }
    }
}

/* ---- the chart ------------------------------------------------------------------------------ */

/* The two lines of chrome under the plot: what the horizontal covers, then what the lines are.
   Both are the axis - a picture whose axes are unnamed is the sparkline, which is a different
   component with a different job. */
#define FB_CHART_FOOTER_LINES 2

/*
 * How thick a line on a chart is, which is deliberately not the sparkline's stroke.
 *
 * A series colour promises 1.4:1 against the grounds and against the other series, and that is a
 * *fill's* contract - it was measured on a bar several pixels tall, and it is the reason
 * MESH_UI_SERIES_COLORS may never be used as an ink. A stroke half the glyph scale wide is not a
 * fill; at the contrast the palette guarantees, a hairline in one of these colours is a line the
 * reader has to hunt for on the very theme that exists so nobody has to.
 *
 * So a chart's lines are drawn at least twice as thick as a row's, which is what a chart has the
 * room for and what makes the colour the palette was validated for the colour that is actually
 * on the panel.
 */
static int fb_chart_stroke(int scale) {
    const int stroke = scale > 0 ? scale : 1;
    return stroke > 2 ? stroke : 2;
}

/* The hairline the axis and the threshold rules are drawn at: a mark rather than a reading, so
   it is as thin as this panel can draw and still be seen. */
static int fb_chart_rule(int scale) {
    const int rule = (scale > 0 ? scale : 1) / 2;
    return rule > 1 ? rule : 1;
}

int fb_chart_min_height(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout) {
    (void)state;
    /* The chrome, and a plot at least as tall again as the chrome under it. Below that the
       picture is shorter than its own caption, which reads as a rendering fault rather than as a
       small chart. */
    return layout->line * (FB_CHART_FOOTER_LINES * 2);
}

/*
 * One threshold, drawn across the plot as a broken rule.
 *
 * Broken rather than solid, and that is the whole of what tells it from the axis and from the
 * data: a chart with three solid horizontals on it has three things that look like readings. The
 * meter answers the same question by cutting a notch in its own track, which is a gap in a thing
 * the eye has already found; there is no track here to cut, so the mark has to be visibly a mark.
 */
static void fb_chart_threshold(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_rect *plot, int travel, int32_t value,
                               struct mesh_ui_scale scale, int rule, struct mesh_ui_rgb ink) {
    /*
     * Outside the domain the lines are drawn on. Nothing is clamped to an edge here: a threshold
     * pinned to the top of a chart is a threshold the trend can never be seen crossing, which is
     * worse than one the reader can see is off the picture.
     *
     * Asked of the *reading* rather than of its projection, and that is not a tidy-up:
     * mesh_ui_scale_permille() clamps, so a threshold above the domain's ceiling comes back as
     * 1000 and this test - written against the projection - could never fire. It never had to
     * until the ceiling learned to contract (mesh_ui_trend_domain()), and the first quiet mesh
     * drawn on a fifth of the domain would have had a rule ruled across the top of it saying the
     * air was as busy as the picture goes.
     *
     * Both ends of the comparison rather than one, because a domain may be stated either way
     * round: a descending scale reads backwards, and its ceiling is the smaller number.
     */
    const int32_t max = scale.min == scale.max ? MESH_UI_ANIM_ONE : scale.max;
    const int32_t low = scale.min < max ? scale.min : max;
    const int32_t high = scale.min < max ? max : scale.min;
    if (value < low || value > high) {
        return;
    }
    const int32_t permille = mesh_ui_scale_permille(scale, value);
    const int y = plot->y + travel - (int)(((int64_t)permille * travel) / MESH_UI_ANIM_ONE);
    const int dash = rule * 3;
    for (int x = plot->x; x < plot->x + plot->w; x += dash * 2) {
        const int w = (x + dash > plot->x + plot->w) ? plot->x + plot->w - x : dash;
        fb_fill_rect(state, x, y, w, rule, ink);
    }
}

/*
 * The legend: a swatch and a word per line, in the order the lines were handed over.
 *
 * The swatch carries the colour and the word is in the body's own ink, which is the rule a
 * series colour never gets out of - it is a fill, so it fills a square, and the text beside it is
 * text. Naming the parts in their own colours is what every spreadsheet does and it is four more
 * contrast pairs per theme, to say what a swatch already says.
 */
static void fb_chart_legend(const struct mesh_ui_backend_fb_state *state,
                            const struct fb_layout *layout, const struct fb_chart *chart, int x,
                            int y) {
    const int scale = layout->small;
    const int adv = fb_char_adv(state, scale);
    const int cap = mesh_ui_font_cap(fb_font(state), scale);
    const int line = fb_line_adv(state, scale);
    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);

    for (uint32_t i = 0U; i < chart->count && i < FB_CHART_LINES; ++i) {
        const enum mesh_str_id label = chart->lines[i].label;
        const char *value = chart->lines[i].value;
        /* A name, a reading, or both - and an entry with neither is a swatch standing for
           nothing, which is furniture. One line drawn alone is named by the screen's own title,
           so this is where its current reading gets said. */
        if (label == MESH_STR_NONE && value == NULL) {
            continue;
        }
        const char *word = label != MESH_STR_NONE ? mesh_str(label) : NULL;
        int width = cap + adv;
        width += word != NULL ? (int)mesh_ui_text_cells(word) * adv : 0;
        width +=
            value != NULL ? (int)mesh_ui_text_cells(value) * adv + (word != NULL ? adv : 0) : 0;
        if (x + width > chart->rect.x + chart->rect.w) {
            /* Out of line. The entry is dropped whole rather than cut, for the reason a bubble's
               trailing run drops a chip rather than truncating one: half a word beside a colour
               is a legend that names the wrong thing, and the reader has no way to tell. */
            return;
        }
        /* Vertically centred on the capitals beside it rather than on the cell, which is the
           icon slot's rule - a square sized to the cell stands a seventh taller than the word it
           is labelling on any face with real descenders. */
        fb_fill_round_rect(state, x, y + (line - cap) / 2, cap, cap,
                           fb_radius(state, MESH_UI_SHAPE_SM),
                           mesh_ui_theme_series(state->theme, i));
        int text_x = x + cap + adv;
        if (word != NULL) {
            fb_draw_text(state, text_x, y, word, scale, ink, ground);
            text_x += (int)mesh_ui_text_cells(word) * adv + adv;
        }
        if (value != NULL) {
            /* The reading in the body's own ink rather than dimmed: it is the only number on
               this line and the words beside it are its label, not the other way round. */
            fb_draw_text(state, text_x, y, value, scale, fb_color(state, MESH_UI_COLOR_TEXT),
                         ground);
        }
        x += width + adv * 2;
    }
}

/*
 * One binned line: columns up from the axis, or a stroke through the bins' centres.
 *
 * A present bin whose mean is zero still gets a column one rule tall, because "the radio said
 * nothing was on the air" and "the radio said nothing" are different readings and an empty slot
 * is the second. The gap between columns is one rule, and only where a column is wide enough to
 * spare it; below that the columns touch rather than vanish.
 *
 * A stroke is drawn twice: first wider in the ground colour, then in its own. The series colours
 * are only promised apart by 1.4:1, which a column next to a column meets and a two-pixel line
 * crossing a column does not - the halo is what keeps the line legible where it passes through
 * one.
 *
 * True when it put a mark on the panel.
 */
static bool fb_chart_bins(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *plot,
                          int travel, int stroke, int rule, struct mesh_ui_scale scale,
                          const struct fb_chart_line *line, struct mesh_ui_rgb colour,
                          struct mesh_ui_rgb ground) {
    const struct mesh_ui_trend_bins *bins = line->bins;
    const uint32_t count =
        bins->count < MESH_UI_TREND_BINS_MAX ? bins->count : MESH_UI_TREND_BINS_MAX;
    if (count == 0U) {
        return false;
    }
    /* The plot less the axis rule on its left, so the first column does not sit on the axis. */
    const int left = plot->x + rule;
    const int width = plot->w - rule;
    const int base = plot->y + travel + stroke; /* the top of the axis rule */
    const int halo = stroke / 2 > rule ? stroke / 2 : rule;
    bool drawn = false;
    for (int pass = line->columns ? 1 : 0; pass < 2; ++pass) {
        const int pen = pass == 0 ? stroke + halo * 2 : stroke;
        const int offset = pass == 0 ? halo : 0;
        const struct mesh_ui_rgb ink = pass == 0 ? ground : colour;
        int previous_x = 0;
        int previous_y = 0;
        for (uint32_t i = 0U; i < count; ++i) {
            if (!bins->present[i]) {
                continue;
            }
            const int x0 = left + (int)(((int64_t)i * width) / (int64_t)count);
            const int x1 = left + (int)(((int64_t)(i + 1U) * width) / (int64_t)count);
            const int32_t permille = mesh_ui_scale_permille(scale, bins->values[i]);
            if (line->columns) {
                const int gap = (x1 - x0) >= rule * 4 ? rule : 0;
                int h = (int)(((int64_t)permille * (travel + stroke)) / MESH_UI_ANIM_ONE);
                h = h < rule ? rule : h;
                if (x1 - x0 - gap > 0) {
                    fb_fill_rect(state, x0, base - h, x1 - x0 - gap, h, ink);
                    drawn = true;
                }
                continue;
            }
            const int x = (x0 + x1) / 2 - offset;
            const int y =
                plot->y + travel - (int)(((int64_t)permille * travel) / MESH_UI_ANIM_ONE) - offset;
            if (bins->joins[i]) {
                fb_spark_segment(state, previous_x, previous_y, x, y, pen, ink);
            } else {
                /* A bin nothing joins is still a reading: a stroke's worth of dot says so, where
                   the polyline's rule would draw nothing. */
                fb_fill_rect(state, x - pen / 2 + offset, y, pen, pen, ink);
            }
            drawn = true;
            previous_x = x;
            previous_y = y;
        }
    }
    return drawn;
}

/*
 * What the span strip takes off the top of a chart's body, and how wide it is.
 *
 * Off the top before anything else is measured, and measured against what *would be left* rather
 * than against what there was: a strip drawn and then found to have taken the plot's room is a
 * control floating over nothing, which is the clipped chart this component refuses one level up.
 * The content wins the room.
 *
 * Its own function because both faces of the chart start here and must start identically. A
 * readings list that measured the strip differently from the plot would place its first row over
 * the control the reader is pressing - and only on the panels where the arithmetic happened to
 * differ, which is the kind of wrong that ships.
 *
 * Above the plot rather than below it, which is Material's placement for a filter over a view and
 * is also the only one that reads correctly here: the strip says what the picture is of, and the
 * lines under it say what came out. Put underneath, the control would be the third line of a
 * caption. Centred, because it is about the whole horizontal rather than either end of it.
 */
static int fb_chart_strip(const struct mesh_ui_backend_fb_state *state,
                          const struct fb_layout *layout, const struct fb_rect *rect,
                          const struct fb_segmented *spans, int *out_width) {
    *out_width = 0;
    if (spans == NULL) {
        return 0;
    }
    const int scale = layout->small;
    const int strip = fb_segmented_height(state, scale);
    const int width = fb_segmented_width(state, spans, scale);
    const int gap = fb_gutter(state);
    if (strip <= 0 || width <= 0 || width > rect->w ||
        rect->h - (strip + gap) < fb_chart_min_height(state, layout)) {
        return 0;
    }
    *out_width = width;
    return strip + gap;
}

uint32_t fb_chart_reading_rows(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_layout *layout, const struct fb_rect *rect,
                               const struct fb_segmented *spans) {
    if (state == NULL || layout == NULL || rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return 0U;
    }
    /* The plot's own floor, so the two faces appear and disappear together: a panel too small to
       draw a chart in is one the reader would land on by pressing Y and find blank. */
    if (rect->h < fb_chart_min_height(state, layout)) {
        return 0U;
    }
    int width = 0;
    struct fb_rect body = *rect;
    body.h -= fb_chart_strip(state, layout, rect, spans, &width);
    /* One line kept back for the caption, on the terms the plot keeps two: a column of durations
       with nothing saying what they are measured from can be read as a clock. */
    const int rows = (body.h - layout->line) / layout->line;
    return rows > 0 ? (uint32_t)rows : 0U;
}

/*
 * The readings, as two columns down the body.
 *
 * Right-aligned, both of them, which is the one layout decision here and is the table's whole
 * job: these are figures to be *compared down the column*, and a left-aligned "-8 dB" under a
 * "-110 dBm" puts the digits that differ in different places. The durations are right-aligned
 * against the value column's left edge for the same reason.
 *
 * The newest row is the one every other row is measured from, so it says so rather than "0s" -
 * a zero there is a duration the reader has to work out is not a reading.
 */
static void fb_draw_chart_readings(const struct mesh_ui_backend_fb_state *state,
                                   const struct fb_layout *layout, const struct fb_chart *chart,
                                   const struct fb_rect *body) {
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);
    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT);
    const struct mesh_ui_rgb dim = fb_color(state, MESH_UI_COLOR_TEXT_DIM);

    /*
     * Both columns are measured from their content and the pair is centred as a block.
     *
     * Centred rather than set against either edge, because that is what the rest of this screen
     * does - the span strip above is centred and so is the caption under the plot - and because a
     * table is not a list: there are no rows to line up with, only two columns of figures with
     * the whole panel around them. Pushed to one edge it reads as the remains of a layout that
     * was going to have something else in it.
     *
     * The columns themselves are right-aligned, which is the one decision here that is about the
     * content: these are figures to be compared *down* the column, and a left-aligned "-8 dB"
     * under a "-110 dBm" puts the digits that differ in different places. A cell of air between
     * them, which is the gap a list row leaves between its label and its value.
     */
    size_t when_cells = 0U;
    size_t value_cells = 0U;
    for (uint32_t i = 0U; i < chart->reading_count; ++i) {
        const struct fb_chart_reading *row = &chart->readings[i];
        const size_t when = row->when != NULL ? mesh_ui_text_cells(row->when) : 0U;
        const size_t value = row->value != NULL ? mesh_ui_text_cells(row->value) : 0U;
        if (when > when_cells) {
            when_cells = when;
        }
        if (value > value_cells) {
            value_cells = value;
        }
    }
    const int block = (int)(when_cells + 1U + value_cells) * adv;
    const int left = body->x + (body->w > block ? (body->w - block) / 2 : 0);
    const int values_x = left + (int)(when_cells + 1U) * adv;

    int y = body->y;
    for (uint32_t i = 0U; i < chart->reading_count; ++i) {
        const struct fb_chart_reading *row = &chart->readings[i];
        if (row->value != NULL) {
            const int x = values_x + (int)(value_cells - mesh_ui_text_cells(row->value)) * adv;
            fb_draw_text(state, x, y, row->value, scale, ink, ground);
        }
        if (row->when != NULL) {
            const int x = left + (int)(when_cells - mesh_ui_text_cells(row->when)) * adv;
            fb_draw_text(state, x, y, row->when, scale, dim, ground);
        }
        y += layout->line;
    }

    /*
     * And the caption, under the last row rather than at the foot of the body.
     *
     * It annotates the left-hand column, so it belongs next to it: the plot's own caption is
     * under the plot because the plot fills the body, and a list that has not filled it would
     * leave the same line stranded half a panel below the thing it is about.
     */
    if (chart->readings_note != NULL) {
        const int small = fb_char_adv(state, layout->small);
        const int width = (int)mesh_ui_text_cells(chart->readings_note) * small;
        fb_draw_text(state, body->x + (body->w > width ? (body->w - width) / 2 : 0), y,
                     chart->readings_note, layout->small, dim, ground);
    }
}

void fb_draw_chart(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_chart *chart) {
    if (chart == NULL || chart->rect.w <= 0 || chart->rect.h <= 0) {
        return;
    }
    if (chart->rect.h < fb_chart_min_height(state, layout)) {
        return; /* see fb_chart_min_height(): there is no clipped chart */
    }

    const int scale = layout->small;
    const int adv = fb_char_adv(state, scale);
    const int rule = fb_chart_rule(state->scale);

    /*
     * The span picker, off the top before the plot is measured.
     *
     * Above the plot rather than below it, which is Material's placement for a filter over a
     * view and is also the only one that reads correctly here: the strip says what the picture is
     * of, and the two lines under the plot say what came out - the axis's own span and the names
     * of the lines. Put underneath, the control the reader is pressing would be the third line of
     * a caption.
     *
     * Centred on the plot for the caption's reason, one component down: it is about the whole
     * horizontal rather than about either end of it.
     */
    struct fb_rect body = chart->rect;
    int strip_width = 0;
    const int strip_takes = fb_chart_strip(state, layout, &chart->rect, chart->spans, &strip_width);
    if (strip_takes > 0) {
        const int strip = fb_segmented_height(state, scale);
        const struct fb_rect box = {
            .x = body.x + (body.w - strip_width) / 2, .y = body.y, .w = strip_width, .h = strip};
        /* Selected, always: it is the only control on the screen and the d-pad always reaches
           it - see `spans`. Its ground is the body's, because that is what is behind it. */
        fb_draw_segmented(state, &box, chart->spans, true, MESH_UI_COLOR_BG, scale);
        body.y += strip_takes;
        body.h -= strip_takes;
    }

    /* The other face, once the strip has taken its room and before the plot claims any: the same
       window said exactly rather than drawn - see `readings`. */
    if (chart->readings != NULL && chart->reading_count > 0U) {
        fb_draw_chart_readings(state, layout, chart, &body);
        return;
    }

    /*
     * The room the vertical's two ends want, taken off the left before anything is placed.
     *
     * Measured from the labels themselves rather than reserved as a fixed column: this is the
     * only screen on the panel, so a chart of percentages should not be inset as far as one of
     * five-digit counts. The gap after them is one cell, which is the same gap a list row leaves
     * between its label column and its value.
     */
    const size_t top_cells = chart->top != NULL ? mesh_ui_text_cells(chart->top) : 0U;
    const size_t bottom_cells = chart->bottom != NULL ? mesh_ui_text_cells(chart->bottom) : 0U;
    const size_t axis_cells = top_cells > bottom_cells ? top_cells : bottom_cells;
    const int gutter = axis_cells > 0U ? (int)(axis_cells + 1U) * adv : 0;

    struct fb_rect plot = {
        .x = body.x + gutter,
        .y = body.y,
        .w = body.w - gutter,
        .h = body.h - FB_CHART_FOOTER_LINES * layout->line,
    };
    if (plot.w <= 0 || plot.h <= rule) {
        return;
    }

    const struct mesh_ui_rgb furniture = fb_color(state, MESH_UI_COLOR_METER_TRACK);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);

    /*
     * The frame: the two axes and nothing else.
     *
     * Two rather than four, because the two that are not drawn would be saying something. A rule
     * along the top of a chart reads as the domain's ceiling and this one has a label saying
     * where that is; a rule up the right-hand edge reads as the present, which is where the
     * lines end anyway. What is left is the pair that say "measured from here".
     */
    const int interior = plot.h - rule;
    fb_fill_rect(state, plot.x, plot.y, rule, plot.h, furniture);
    fb_fill_rect(state, plot.x, plot.y + interior, plot.w, rule, furniture);

    /*
     * The height a reading travels over, which is the interior less the stroke - so a reading at
     * the top of its domain draws its whole line inside the plot rather than half outside it.
     * The sparkline's arithmetic, with a thicker pen.
     */
    const int stroke = fb_chart_stroke(state->scale);
    const int travel = interior > stroke ? interior - stroke : 0;
    const int span = plot.w > 1 ? plot.w - 1 : 0;

    /* The thresholds, under the lines: a mark the data can be seen crossing has to be behind it,
       or the mark is what is on top of the reading. */
    if (chart->band != NULL) {
        fb_chart_threshold(state, &plot, travel, chart->band->warn, chart->scale, rule, furniture);
        fb_chart_threshold(state, &plot, travel, chart->band->bad, chart->scale, rule, furniture);
    }

    /* The two ends of the vertical, against the plot's own top and bottom. */
    const int label_line = fb_line_adv(state, scale);
    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
    if (chart->top != NULL) {
        fb_draw_text(state, plot.x - (int)(top_cells + 1U) * adv, plot.y, chart->top, scale, ink,
                     ground);
    }
    if (chart->bottom != NULL) {
        fb_draw_text(state, plot.x - (int)(bottom_cells + 1U) * adv, plot.y + interior - label_line,
                     chart->bottom, scale, ink, ground);
    }

    /*
     * The lines, in the order they were handed over, each in the series colour of its position.
     *
     * By position rather than by anything about the data, which is the palette's whole contract:
     * slice 0 is the same colour on every frame and every theme, so the legend under the plot
     * goes on meaning what it said the last time this screen was opened.
     *
     * Two passes: columns first, then strokes, so a line is never hidden behind a column that
     * happens to come later in the list.
     */
    bool drawn = false;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0U; i < chart->count && i < FB_CHART_LINES; ++i) {
            const struct fb_chart_line *line = &chart->lines[i];
            const bool columns = line->bins != NULL && line->columns;
            if ((pass == 0) != columns) {
                continue;
            }
            const struct mesh_ui_rgb colour = mesh_ui_theme_series(state->theme, i);
            if (line->bins != NULL) {
                drawn = fb_chart_bins(state, &plot, travel, stroke, rule, chart->scale, line,
                                      colour, ground) ||
                        drawn;
                continue;
            }
            const struct mesh_ui_polyline *points = line->points;
            if (points == NULL || points->count < 2U) {
                continue; /* one reading is a level; the sparkline's rule, unchanged */
            }
            int previous_x = 0;
            int previous_y = 0;
            for (uint32_t j = 0U; j < points->count && j < MESH_UI_SERIES_MAX; ++j) {
                const struct mesh_ui_point *point = &points->items[j];
                const int x = plot.x + (int)(((int64_t)point->x * span) / MESH_UI_ANIM_ONE);
                const int y =
                    plot.y + travel - (int)(((int64_t)point->y * travel) / MESH_UI_ANIM_ONE);
                if (!point->gap && j > 0U) {
                    fb_spark_segment(state, previous_x, previous_y, x, y, stroke, colour);
                    drawn = true;
                }
                previous_x = x;
                previous_y = y;
            }
        }
    }

    /*
     * And what to say when none of that put a mark on the panel.
     *
     * Asked of the drawing rather than of the data, which is the only place the question can be
     * answered honestly: two readings either side of a break are a polyline of two points and no
     * line at all, and a caller counting samples would offer a picture this then declines to
     * draw. `drawn` is set by the one call that actually strokes something.
     */
    if (!drawn && chart->empty != MESH_STR_NONE) {
        const char *word = mesh_str(chart->empty);
        const int width = (int)mesh_ui_text_cells(word) * adv;
        const int x = plot.x + (plot.w - width) / 2;
        fb_draw_text(state, x > plot.x ? x : plot.x, plot.y + (interior - layout->line) / 2, word,
                     scale, fb_color(state, MESH_UI_COLOR_TEXT_DIM), ground);
    }

    /*
     * And the chrome under it: what the horizontal covers, then what the lines are.
     *
     * The span is centred on the plot rather than tucked under either end, because it names the
     * whole axis rather than a point on it - "last 45m" under the left-hand end reads as a label
     * for that end, which is the one place on the axis it is not true of.
     */
    int y = plot.y + plot.h;
    if (chart->span != NULL) {
        const int width = (int)mesh_ui_text_cells(chart->span) * adv;
        const int x = plot.x + (plot.w - width) / 2;
        fb_draw_text(state, x > plot.x ? x : plot.x, y, chart->span, scale, ink, ground);
    }
    y += layout->line;
    fb_chart_legend(state, layout, chart, plot.x, y);
}
