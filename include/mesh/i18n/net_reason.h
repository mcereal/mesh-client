#pragma once

#include "inkwell/net/reason.h"
#include "mesh/i18n/strings.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What this client says about a failure inkwell reported.
 *
 * inkwell's transports hand back `enum inkwell_net_reason` and a number, and nothing else - a
 * platform layer that owned the sentence would own the language and the tone of every
 * application above it, so it deals in reasons and leaves the words here. This is the table on
 * the other side of that line, and it is the whole of this client's half.
 *
 * It lives in `i18n` rather than beside either caller because both callers need it: the TCP
 * transport and the MQTT proxy fail to reach a host in exactly the same ways, and each used to
 * write those ways down in its own set of ids. "No such host" was in the catalog twice and
 * translated twice, in every language, for as long as a string id was the only thing a
 * transport could report.
 *
 * **Collapsing happens here, never on the way down.** inkwell keeps a lookup that failed and a
 * lookup that timed out apart because it can tell them apart; this table says one sentence
 * about both because there is one thing for a reader to do. That decision is about words, so it
 * belongs with the words.
 */

/*
 * The sentence for a reason every link means the same thing by. Returns false - leaving `*out`
 * untouched - for a reason whose sentence depends on what kind of link it was, which the caller
 * answers itself before falling back to this: a target that will not parse is "not an address
 * and port" for a TCP link and "not a broker address" for the MQTT proxy, and INKWELL_NET_OK is
 * not a failure to describe at all.
 *
 * Every string it returns takes the subject as its first argument - the target or host as the
 * user wrote it - and INKWELL_NET_UNREACHABLE and INKWELL_NET_TLS take a second: the C
 * library's word for the errno, and the TLS library's own sentence. Neither is translated, for
 * the same reason a channel key is shown as base64.
 */
bool mesh_net_reason_str(enum inkwell_net_reason reason, enum inkcell_str_id *out);

#ifdef __cplusplus
}
#endif
