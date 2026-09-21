#include "mesh/i18n/net_reason.h"

bool mesh_net_reason_str(enum inkwell_net_reason reason, enum inkcell_str_id *out) {
    if (out == NULL) {
        return false;
    }
    switch (reason) {
    case INKWELL_NET_UNKNOWN_HOST:
        *out = MESH_STR_LINK_UNKNOWN_HOST;
        return true;
    /* One sentence for two reasons, deliberately: see the header. */
    case INKWELL_NET_LOOKUP_FAILED:
    case INKWELL_NET_LOOKUP_TIMED_OUT:
        *out = MESH_STR_LINK_LOOKUP_FAILED;
        return true;
    case INKWELL_NET_UNREACHABLE:
        *out = MESH_STR_LINK_UNREACHABLE;
        return true;
    case INKWELL_NET_TIMED_OUT:
        *out = MESH_STR_LINK_TIMEOUT;
        return true;
    case INKWELL_NET_CLOSED:
        *out = MESH_STR_LINK_CLOSED;
        return true;
    case INKWELL_NET_TLS:
        *out = MESH_STR_LINK_TLS;
        return true;
    /* Named rather than defaulted, so adding a reason to inkwell fails the build here instead
       of silently reaching whichever caller's fallback ran last. */
    case INKWELL_NET_OK:
    case INKWELL_NET_BAD_ADDRESS:
    case INKWELL_NET_REASON_COUNT:
        break;
    }
    return false;
}
