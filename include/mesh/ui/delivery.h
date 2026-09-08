#ifndef MESH_UI_DELIVERY_H
#define MESH_UI_DELIVERY_H

#include "mesh/i18n/strings.h"
#include "mesh/ui/icon.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the UI says about an outbound message's delivery state.
 *
 * A table rather than a switch in the renderer, for the reason src/ui/status.c and
 * src/ui/chrome.c are tables: which mark a state gets is a decision about the product, and a
 * renderer that made it would be the third place holding an opinion about it. The transcript
 * draws the icon, a backend with no sprites says the word, and the tests read both - so a state
 * that gained a mark on one screen and not on another is not expressible.
 *
 * The mark is a *shape*, not a colour. A failed message is already drawn in the error family -
 * its bubble is the error container - so a hue on the icon would be the fill said twice, and
 * the two states that are not failures have nothing to be coloured about: "gone out" and
 * "acknowledged" differ by one tick, which is what every messenger has taught everybody to
 * read. That also keeps the icon out of the theme's contrast contract, because it is drawn in
 * ink the pairing already covers.
 */
struct mesh_ui_delivery {
    /* MESH_UI_ICON_NONE when the state is not worth a mark: an inbound message, or one of ours
       sent without want_ack, where there is nothing to be waiting for. */
    enum mesh_ui_icon icon;
    /* The same thing in words. Not drawn beside the icon - the whole point of the icon is that
       it costs one cell where a word costs nine - but it is what a text backend prints and what
       a test asserts against, and it is why the states stay named in the string catalog. */
    enum mesh_str_id word;
};

/*
 * The mark for one message's `ack`, which is an `enum mesh_message_ack` widened to the byte the
 * UI snapshot carries it in. An unknown value gets no mark, the way an unknown icon draws
 * nothing: a firmware that grows a fourth state must not put a stray glyph on every bubble.
 */
struct mesh_ui_delivery mesh_ui_delivery_of(uint8_t ack);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_DELIVERY_H */
