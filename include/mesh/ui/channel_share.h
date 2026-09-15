#ifndef MESH_UI_CHANNEL_SHARE_H
#define MESH_UI_CHANNEL_SHARE_H

/*
 * What the two channel-sharing screens say.
 *
 * `src/ui/trust.c`'s shape, and here for trust.c's reason: the share screen and the sheet in
 * front of a typed link both have to put a *sentence* about a Meshtastic link in front of the
 * user, and working one out means parsing the link - which is not something a renderer may do.
 * So the renderer asks, and this answers with text out of the catalog.
 *
 * The link itself is not built here. This radio's is in the snapshot already
 * (`mesh_ui_settings.share_url`, filled at the publish boundary from the radio's own bytes),
 * and the one being imported is whatever the user typed. What this module does is read them.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The line under the QR code: how many channels it carries and what to do with it.
 *
 * Reads the link rather than counting the store's slots, and that is the point of asking it
 * here: the code on the screen is drawn from those exact characters, so a summary counted from
 * anywhere else could describe something the code does not say. False when `url` is not a
 * link, which is what an empty `share_url` reads as.
 */
bool mesh_ui_channel_share_summary(const char *url, char *out, size_t out_len);

/* True when the text is a Meshtastic channel link this client can act on. What the nav asks
   when the keyboard closes, to decide whether to raise the sheet or say it is not a link. */
bool mesh_ui_channel_link_valid(const char *text);

/*
 * The sheet in front of a typed link: a headline naming the mesh being joined and a paragraph
 * saying what joining costs.
 *
 * The headline names the *channel* rather than counting anything, because the name is the only
 * part of a link a person can read, and it is what they will have been told to look for. False
 * when the text is not a link, which the caller draws as a refusal rather than an empty panel.
 */
bool mesh_ui_channel_import_sheet(const char *text, char *headline, size_t headline_len, char *body,
                                  size_t body_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_CHANNEL_SHARE_H */
