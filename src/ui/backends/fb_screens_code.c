#define _POSIX_C_SOURCE 200809L

/*
 * The two code sheets: this radio's channel set, and this radio's own contact record, each as a
 * square somebody else's phone is about to read.
 *
 * Two screens and one body. They differ in their heading, their caption and what they say when
 * there is nothing to show, and not at all in the layout - which is the part that is easy to get
 * subtly wrong, so it is written once below and the two renderers are four lines each.
 *
 * A QR code is black on white on every theme; see INKCELL_COLOR_CODE in inkcell's src/theme/theme.c
 * for why that pair is the one that does not vary.
 */

#include "inkcell/ui/widgets.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/channel_share.h"
#include "mesh/ui/contact_share.h"

#include <stdio.h>
#include <string.h>

/*
 * The body of a code sheet: a QR code, what is in it, and the link itself written out.
 *
 * Shared by the two screens that show one - this radio's channels, this radio's contact - which
 * differ in their heading, their caption and what they say when there is nothing to show, and
 * not at all in this. Written once because the *layout* rule below is the part that is easy to
 * get subtly wrong, and two copies of it would drift the first time one screen gained a line.
 *
 * Three things, and the order matters more than it looks. The code is the point and takes every
 * pixel it can get, because how large its modules are is most of whether a phone across the
 * table reads it. Under it goes what is in it, and under that the link itself - last, smallest,
 * and there for the one case the code cannot serve: reading it out to somebody who will type
 * it, or checking with the eye that the thing on screen is a Meshtastic link at all.
 *
 * The link is drawn dim and wrapped, and it is deliberately not cut with an ellipsis. A cut
 * link is not a link, and somebody copying one down needs the whole of it; if the panel cannot
 * hold every line, the lines that fit are the ones that fit, and the code above is what that
 * reader was meant to use anyway.
 */
static void fb_draw_code_body(struct inkcell_draw_state *state, struct inkcell_fb_layout *layout,
                              const char *url, const char *summary, const char *no_code) {
    /*
     * The words go at the bottom of the body and the code gets everything above them, rather
     * than the code being placed first and the words taking what is left. The code's own size
     * steps in whole pixels per module (inkcell/ui/widgets.h), so where it ends depends on how many
     * modules this particular link came out as - and a caption whose position moved with that
     * would sit at a different height on every radio.
     */
    /*
     * As many rows as the words wrap to at this width, measured: the summary's (two at most, as
     * it is drawn) and the link's. A wide window holds the link on fewer lines than the Brick
     * does, and those lines go to the code. The five-row budget is the most the words may take,
     * whatever the width - a link that wraps further is cut there, as it always was, because the
     * code above is what the reader was meant to use.
     */
    enum { CODE_TEXT_ROWS_MAX = 5 };
    const uint32_t summary_rows =
        inkcell_fb_wrapped_lines(state, summary, (size_t)layout->body_w, state->scale);
    const uint32_t url_rows =
        inkcell_fb_wrapped_lines(state, url, (size_t)layout->body_w, state->scale);
    int text_rows = (int)(summary_rows > 2U ? 2U : summary_rows) + (int)url_rows;
    text_rows = text_rows > CODE_TEXT_ROWS_MAX ? CODE_TEXT_ROWS_MAX : text_rows;
    const int text_y = layout->footer_y - text_rows * layout->line;
    /* From the content column's edge rather than the panel's margin: beside a rail the two are
       not the same place, and words at the margin run under the rail. */
    const int text_x = inkcell_fb_row_box(state).text_x;

    /*
     * The code is built here rather than carried on the snapshot, and that is the right place
     * for it: it is a function of the link and of nothing else, the link is on the snapshot
     * already, and a matrix on the store would be fourteen kilobytes copied into every frame
     * for a screen that is open perhaps twice in the life of a radio.
     *
     * Kept between frames beside the link that produced it, which is the snackbar's trick and is
     * here for a sharper reason: encoding walks the whole matrix eight times to choose a mask,
     * and this screen repaints whenever anything else on the frame moves - a notice sliding in,
     * the link summary in the footer changing. The link is the whole of the input, so comparing
     * it is the whole of the cache test. Static rather than on the state because nothing else in
     * this backend has any use for it, and because there is one thread and one frame at a time.
     *
     * One cache for both screens, not one each: only one of them can be open at a time - each is
     * raised by a row of a list the other's screen is covering - so the second would never be
     * warm, and switching between them costs the one encode it should.
     *
     * LOW correction on purpose - see inkcell/utils/qr.h. More correction would push the same link
     * into a higher version and make every module smaller, and on a backlit panel with no print
     * noise to recover from, module size is what decides whether a phone reads it.
     */
    _Static_assert(MESH_UI_CONTACT_URL_MAX <= MESH_UI_CHANNEL_URL_MAX,
                   "the shared code cache is keyed by the longer of the two links");
    static struct inkcell_qr code;
    static char encoded_from[MESH_UI_CHANNEL_URL_MAX];
    if (strcmp(encoded_from, url) != 0) {
        /* A refused encode zeroes the matrix, so the failure needs no flag of its own: a code
           of no size is what inkcell_fb_qr_side() answers 0 for. */
        (void)inkcell_qr_encode((const uint8_t *)url, strlen(url), INKCELL_QR_ECC_LOW, &code);
        snprintf(encoded_from, sizeof encoded_from, "%s", url);
    }
    const struct inkcell_fb_qr qr = {
        .code = code.size > 0U ? &code : NULL,
        .box = {.x = inkcell_fb_region(state).x,
                .y = layout->body_y,
                .w = inkcell_fb_region(state).w,
                .h = text_y - layout->body_y},
    };
    if (inkcell_fb_qr_side(&qr) > 0) {
        inkcell_fb_draw_qr(state, &qr);
    } else {
        /* No code: say so where the code would have been. Both bounds are tested
           (tests/suites/channel_share.c, tests/suites/contact_share.c), so this is a frame that
           should not happen rather than one the screen pretends cannot. */
        (void)inkcell_fb_draw_wrapped_at(state, text_x, layout->body_y, no_code,
                                         (size_t)layout->body_w, 2,
                                         inkcell_fb_tone_color(state, INKCELL_TONE_DIM),
                                         inkcell_fb_color(state, INKCELL_COLOR_BG));
    }

    int y = text_y;
    y += inkcell_fb_draw_wrapped_at(state, text_x, y, summary, (size_t)layout->body_w, 2,
                                    inkcell_fb_tone_color(state, INKCELL_TONE_NORMAL),
                                    inkcell_fb_color(state, INKCELL_COLOR_BG)) *
         layout->line;
    const int left = (layout->footer_y - y) / layout->line;
    if (left > 0) {
        (void)inkcell_fb_draw_wrapped_at(state, text_x, y, url, (size_t)layout->body_w, left,
                                         inkcell_fb_tone_color(state, INKCELL_TONE_DIM),
                                         inkcell_fb_color(state, INKCELL_COLOR_BG));
    }
}

/* The share sheet: this radio's channel set as a code, for a phone that is about to join. */
void fb_render_share(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                     struct inkcell_fb_layout *layout) {
    fb_draw_app_bar(state, layout,
                    &(const struct inkcell_fb_app_bar){.title = inkcell_str(MESH_STR_SHARE_TITLE)});

    const char *const url = snapshot->settings.share_url;
    char summary[96];
    if (!mesh_ui_channel_share_summary(url, summary, sizeof summary)) {
        /* The row that opens this screen is only offered when there is a link, so getting here
           means the radio dropped its table between the press and this frame. */
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_CHANNEL,
                              inkcell_str(MESH_STR_SHARE_NOTHING));
        return;
    }
    fb_draw_code_body(state, layout, url, summary, inkcell_str(MESH_STR_SHARE_NO_CODE));
}

/* The contact code sheet: this radio's own identity as a code, for a phone that is about to add
   it. The same screen as the one above with a different thing in the square - which is the whole
   of why the body is a function and this is four lines. */
void fb_render_contact(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout) {
    fb_draw_app_bar(
        state, layout,
        &(const struct inkcell_fb_app_bar){.title = inkcell_str(MESH_STR_CONTACT_TITLE)});

    const char *const url = snapshot->settings.contact_url;
    char summary[160];
    if (!mesh_ui_contact_share_summary(url, summary, sizeof summary)) {
        /* As above: the row is only offered when there is a link, so this is the radio dropping
           its owner record between the press and this frame. */
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_RADIO,
                              inkcell_str(MESH_STR_CONTACT_NOTHING));
        return;
    }
    fb_draw_code_body(state, layout, url, summary, inkcell_str(MESH_STR_CONTACT_NO_CODE));
}
