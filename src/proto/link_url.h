#ifndef MESH_PROTO_LINK_URL_H
#define MESH_PROTO_LINK_URL_H

/*
 * The half of a Meshtastic link that is the same for every payload, shared by the two readers
 * of one - channel_url.c and contact_url.c.
 *
 * Not public API: this is the `*_internal.h` shape CLAUDE.md describes, and it holds exactly
 * what would still be `static` if the two link formats were one file. Nothing outside
 * src/proto/ should include it.
 *
 * It is shared rather than written twice because the two have to *agree*. A link is a wrapper
 * and a payload, and which characters are the payload is a fact about the link format and not
 * about what is inside it; two copies of this rule would be two chances for one reader to start
 * accepting something the other refuses, in a pair of parsers whose whole job is to be strict
 * about strangers' bytes.
 */

#include <string.h>

/*
 * Where the payload starts in `text`, or NULL when there is no payload in it.
 *
 * The `#` is what separates the link from its payload in every spelling of both formats. Kept
 * as the fragment marker rather than matching the whole prefix, because the host in front of it
 * is not ours to have an opinion about - community sites mirror the page, and the payload is
 * the same. The *last* `#` rather than the first, so a link that has been pasted after another
 * one reads as the one it ends with.
 *
 * With no fragment at all it is a bare payload, which is what somebody typing one in will
 * produce rather than spelling out twenty-six characters of URL on an on-screen keyboard - but
 * only when there is no `/` in it, because a string with a path in it and no `#` is a URL whose
 * fragment was lost rather than a payload, and decoding the host as protobuf is not a service
 * to anybody.
 */
static inline const char *mesh_link_url_payload(const char *text) {
    const char *hash = strrchr(text, '#');
    if (hash != NULL) {
        return hash + 1;
    }
    return strchr(text, '/') == NULL ? text : NULL;
}

#endif /* MESH_PROTO_LINK_URL_H */
