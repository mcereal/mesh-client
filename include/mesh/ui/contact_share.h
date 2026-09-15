#ifndef MESH_UI_CONTACT_SHARE_H
#define MESH_UI_CONTACT_SHARE_H

/*
 * What the two contact-sharing screens say.
 *
 * mesh/ui/channel_share.h's shape exactly, and here for its reason: the code screen and the
 * sheet in front of a typed link both have to put a *sentence* about a Meshtastic link in front
 * of the user, and working one out means parsing the link - which is not something a renderer
 * may do. So the renderer asks, and this answers with text out of the catalog.
 *
 * Every entry here takes the link as characters rather than a decoded contact, which is what
 * keeps nanopb out of a header the screens include. It also means the words on screen are read
 * from the same bytes the QR code is drawn from, so a caption can never describe a contact the
 * code does not carry.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The line under the QR code: whose contact it is, and what to do with it.
 *
 * The name and node number are in it deliberately. This is the one screen whose content the
 * person holding the device cannot read - a QR code says nothing to an eye - so the caption is
 * the only check that the code on the panel is this radio's own and not a stale frame.
 *
 * False when `url` is not a contact link, which is what an empty `contact_url` reads as.
 */
bool mesh_ui_contact_share_summary(const char *url, char *out, size_t out_len);

/* True when the text is a Meshtastic contact link this client can act on. What the nav asks
   when the keyboard closes, to decide whether to raise the sheet or say it is not a link. */
bool mesh_ui_contact_link_valid(const char *text);

/*
 * The name the contact in a link goes by: its long name, its short name, or - for a link that
 * carried neither - a catalog stand-in. Never the node number, which is a separate fact and is
 * shown beside this rather than in place of it.
 *
 * Shared by the sheet below and by the toast the app raises after queueing the write, so that
 * the node named in the question and the node named in the answer cannot disagree. False when
 * the text is not a link.
 */
bool mesh_ui_contact_link_name(const char *text, char *out, size_t out_len);

/*
 * The sheet in front of a typed link: a headline naming the node and a paragraph saying what
 * adding it does and does not do.
 *
 * The paragraph's job is the "does not": a contact arriving by link is a key somebody handed
 * over, not a key anybody checked, and the padlock in this client means the second thing. False
 * when the text is not a link, which the caller draws as a refusal rather than an empty panel.
 */
bool mesh_ui_contact_import_sheet(const char *text, char *headline, size_t headline_len, char *body,
                                  size_t body_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_CONTACT_SHARE_H */
