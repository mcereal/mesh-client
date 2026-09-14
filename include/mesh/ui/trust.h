#ifndef MESH_UI_TRUST_H
#define MESH_UI_TRUST_H

#include "mesh/i18n/strings.h"
#include "mesh/ui/icon.h"
#include "mesh/ui/store_node.h"
#include "mesh/ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the UI says about the key behind a node - and therefore about the padlock on a direct
 * message to it.
 *
 * A table rather than a switch in the renderer, for the reason src/ui/delivery.c is a table:
 * which mark a state gets is a decision about the product, and a renderer that made it would be
 * a second opinion about it. Three screens read this one - the transcript's padlock, the node
 * detail's key row, and the verification sheet - and they have to agree, because the whole
 * feature is the claim that a mark on one screen means the same thing as a word on another.
 *
 * The three states are not a scale of confidence; they are three different facts:
 *
 *   NONE        We hold no key. A direct message to this node cannot be PKI-encrypted at all,
 *               so it travels under the channel key - which on a channel still using the
 *               default one is a key every node on the mesh has.
 *   UNVERIFIED  We hold a key, and it arrived over the air from whoever transmitted it. The
 *               message *is* encrypted to that key, and nothing has established whose it is.
 *   VERIFIED    Somebody proved it, out of band (mesh/core/key_verification.h).
 *
 * UNVERIFIED is the default and it is not a warning. Almost every key on a working mesh is
 * unverified, and a client that drew a caution mark on all of them would have taught its user
 * to ignore the mark by the end of the first day. It is the *verified* state that is marked out
 * of the ordinary, which is the right way round: verification is the thing somebody did.
 */
enum mesh_ui_key_trust {
    MESH_UI_KEY_TRUST_NONE = 0,
    MESH_UI_KEY_TRUST_UNVERIFIED,
    MESH_UI_KEY_TRUST_VERIFIED,
};

/* The trust in a node record: its key, and whether the radio holds the verified bit for it.
   A NULL node reads as NONE, which is what a node the roster has lost amounts to. */
enum mesh_ui_key_trust mesh_ui_key_trust_of(const struct mesh_ui_node_summary *node);

/*
 * The mark for a state, or MESH_UI_ICON_NONE for NONE - there is no glyph for the absence of
 * encryption, and one would put a mark on every broadcast on the mesh.
 *
 * The two that have one are a padlock and a shield, which is a *shape* difference rather than a
 * colour one, for the reason src/ui/delivery.c gives: the transcript's marks are drawn in the
 * ink the theme pairing already covers, so they stay out of the contrast contract.
 */
enum mesh_ui_icon mesh_ui_key_trust_icon(enum mesh_ui_key_trust trust);

/* The state in words, for a row that has room for them. */
enum mesh_str_id mesh_ui_key_trust_label(enum mesh_ui_key_trust trust);

/*
 * The ink a row saying it takes.
 *
 * VERIFIED is the success family and the other two are plain, which is the same judgement the
 * enum's comment makes: an unverified key is the ordinary case and colouring it would be a
 * warning about the mesh working normally. NONE is not coloured either - it is the answer for
 * every node that has never sent a NodeInfo, which is not that node's fault or the user's.
 */
enum mesh_ui_tone mesh_ui_key_trust_tone(enum mesh_ui_key_trust trust);

/*
 * The verification sheet: one question, assembled from the exchange the radio has open.
 *
 * Here rather than in a backend for the house rule that nothing is spelled out in a renderer,
 * and here rather than in src/ui/settings.c - whose confirm overlay this borrows its shape
 * from - because the two dialogs answer to different things. A settings confirm is about a
 * section the user is editing; this is about an exchange the *radio* is running, and its words
 * change without anybody pressing anything.
 *
 * `headline` and `text` are built into the caller's buffers because both name the other person
 * and one of them carries digits. The two button labels are catalog strings and are not.
 */
struct mesh_ui_verify_sheet {
    enum mesh_ui_icon icon;
    enum mesh_str_id accept;
    enum mesh_str_id cancel;
    /* The answer that acts is the one that says the characters did *not* match: it is the
       destructive half of a comparison, and the dialog colours itself from this. A sheet with
       nothing to refuse - the two waiting stages - is not destructive. */
    bool destructive;
};

/*
 * Fills `out`, `headline` and `text` for the stage this exchange is at. False when there is
 * nothing to show (an idle record, or a stage this build does not know), which is what tells a
 * backend to draw nothing rather than an empty panel.
 */
bool mesh_ui_verify_sheet_of(const struct mesh_ui_verification *verification,
                             struct mesh_ui_verify_sheet *out, char *headline, size_t headline_len,
                             char *text, size_t text_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_TRUST_H */
