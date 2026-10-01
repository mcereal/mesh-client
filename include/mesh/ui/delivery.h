#ifndef MESH_UI_DELIVERY_H
#define MESH_UI_DELIVERY_H

#include "inkcell/ui/icon.h"

#include "mesh/i18n/strings.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the UI says about an outbound message's delivery state.
 *
 * A table rather than a switch in the renderer, for the reason src/ui/tables/status.c and
 * src/ui/tables/chrome.c are tables: which mark a state gets is a decision about the product, and a
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
    /* INKCELL_ICON_NONE when the state is not worth a mark: an inbound message, or one of ours
       sent without want_ack, where there is nothing to be waiting for. */
    enum inkcell_icon icon;
    /*
     * The same state in words, from the catalog.
     *
     * Not drawn beside the icon - the whole point of the mark is that it costs one cell where a
     * word costs nine. It is what the transcript falls back to when a failed message arrives
     * carrying no reason, and it is how a test names a state's identity without asserting on a
     * sprite.
     *
     * Deliberately *not* what the `cli` backend prints. That one and `stub` are the headless
     * developer surfaces, and they stay untranslated off mesh_message_ack_to_string() - which
     * is the split docs/i18n.md draws, and which two of main.c's four call sites make load
     * bearing: they are JSON fields, where a value that changed with the handheld's locale
     * would be a bug rather than a translation.
     */
    inkcell_str_id word;
};

/*
 * The mark for one message's `ack`, which is an `enum mesh_message_ack` widened to the byte the
 * UI snapshot carries it in. An unknown value gets no mark, the way an unknown icon draws
 * nothing: a firmware that grows a fourth state must not put a stray glyph on every bubble.
 */
struct mesh_ui_delivery mesh_ui_delivery_of(uint8_t ack);

/*
 * The share of direct messages the mesh confirmed, as a whole percent: false when none has been
 * settled either way, and nothing should be said.
 *
 * Rounded *down*, so a single failure keeps the figure under 100: 199 delivered and 1 failed is
 * 99%, never a perfect score the record does not support. And done without overflowing at any
 * count the lifetime stats can hold. One function because two screens say it - the Stats page and
 * the Status board's tile - and two roundings of one record would be two answers to one question.
 */
bool mesh_ui_delivery_rate(uint64_t delivered, uint64_t failed, unsigned *percent);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_DELIVERY_H */
