#include "mesh/ui/mqtt.h"

#include "inkwell/net/reason.h"
#include "mesh/i18n/net_reason.h"
#include "mesh/i18n/strings.h"

#include <string.h>

const char *mesh_ui_mqtt_state_str(enum mesh_mqtt_proxy_state state) {
    switch (state) {
    case MESH_MQTT_PROXY_OFF:
        return inkcell_str(MESH_STR_MQTT_STATE_OFF);
    case MESH_MQTT_PROXY_RESOLVING:
        return inkcell_str(MESH_STR_MQTT_STATE_RESOLVING);
    case MESH_MQTT_PROXY_CONNECTING:
        return inkcell_str(MESH_STR_MQTT_STATE_CONNECTING);
    case MESH_MQTT_PROXY_SECURING:
        return inkcell_str(MESH_STR_MQTT_STATE_SECURING);
    case MESH_MQTT_PROXY_GREETING:
        return inkcell_str(MESH_STR_MQTT_STATE_GREETING);
    case MESH_MQTT_PROXY_READY:
        return inkcell_str(MESH_STR_MQTT_STATE_READY);
    case MESH_MQTT_PROXY_WAITING:
    case MESH_MQTT_PROXY_STATE_COUNT:
    default:
        return inkcell_str(MESH_STR_MQTT_STATE_WAITING);
    }
}

/* The broker's own answer, and the two this client refuses before asking. Each takes the host
   and nothing else, except the catch-all, which carries the CONNACK number. */
static bool mqtt_refusal_text(const struct mesh_mqtt_proxy *proxy, enum mesh_mqtt_refusal refusal,
                              uint8_t code, char *out, size_t out_len) {
    const char *const host = mesh_mqtt_proxy_host(proxy);
    switch (refusal) {
    case MESH_MQTT_REFUSAL_BAD_ADDRESS:
        /* The address, not the host: a target this could not parse never became one. */
        (void)inkcell_str_format(out, out_len, MESH_STR_LINK_MQTT_BAD_ADDRESS,
                                 mesh_mqtt_proxy_address(proxy));
        return true;
    case MESH_MQTT_REFUSAL_NO_TLS:
        (void)inkcell_str_format(out, out_len, MESH_STR_LINK_MQTT_NO_TLS, host);
        return true;
    case MESH_MQTT_REFUSAL_PROTOCOL:
        (void)inkcell_str_format(out, out_len, MESH_STR_LINK_MQTT_PROTOCOL, host);
        return true;
    case MESH_MQTT_REFUSAL_BAD_LOGIN:
        (void)inkcell_str_format(out, out_len, MESH_STR_LINK_MQTT_BAD_LOGIN, host);
        return true;
    case MESH_MQTT_REFUSAL_NOT_ALLOWED:
        (void)inkcell_str_format(out, out_len, MESH_STR_LINK_MQTT_NOT_ALLOWED, host);
        return true;
    case MESH_MQTT_REFUSAL_BROKER_BUSY:
        (void)inkcell_str_format(out, out_len, MESH_STR_LINK_MQTT_BROKER_BUSY, host);
        return true;
    case MESH_MQTT_REFUSAL_OTHER:
        (void)inkcell_str_format(out, out_len, MESH_STR_LINK_MQTT_REFUSED, host, (unsigned)code);
        return true;
    /* Named rather than defaulted, so a refusal added to the proxy fails the build here. */
    case MESH_MQTT_REFUSAL_NONE:
    case MESH_MQTT_REFUSAL_COUNT:
        break;
    }
    return false;
}

void mesh_ui_mqtt_failure_text(const struct mesh_mqtt_proxy *proxy, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (proxy == NULL) {
        return;
    }

    const struct mesh_mqtt_proxy_failure failure = mesh_mqtt_proxy_failure(proxy);
    if (mqtt_refusal_text(proxy, failure.refusal, failure.code, out, out_len)) {
        return;
    }

    inkcell_str_id text = MESH_STR_LINK_UNREACHABLE;
    if (!mesh_net_reason_str(failure.net.reason, &text)) {
        return; /* no failure, or a reason with nothing to say about it */
    }

    /*
     * The two sentences that take a second argument, and the only place the arity can go wrong.
     * "%.24s: %.20s" is the host and the C library's word for the errno; "%.24s: %.64s" is the
     * host and the TLS library's own account of the handshake. Neither second half is
     * translated, for the same reason a channel key is shown as base64.
     */
    const char *const host = mesh_mqtt_proxy_host(proxy);
    switch (failure.net.reason) {
    case INKWELL_NET_UNREACHABLE:
        (void)inkcell_str_format(out, out_len, text, host, strerror(-failure.net.detail));
        break;
    case INKWELL_NET_TLS:
        (void)inkcell_str_format(out, out_len, text, host, mesh_mqtt_proxy_tls_error(proxy));
        break;
    default:
        (void)inkcell_str_format(out, out_len, text, host);
        break;
    }
}
