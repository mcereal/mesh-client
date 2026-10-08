#include "mesh/ui/delivery.h"

#include "mesh/core/message.h"

struct mesh_ui_delivery mesh_ui_delivery_of(uint8_t ack) {
    switch ((enum mesh_message_ack)ack) {
    case MESH_MESSAGE_ACK_PENDING:
        return (struct mesh_ui_delivery){.icon = INKCELL_ICON_SENDING,
                                         .word = MESH_STR_DELIVERY_SENDING};
    case MESH_MESSAGE_ACK_WAITING:
        /* The same clock as sending: the message is the radio's and not yet the air's. */
        return (struct mesh_ui_delivery){.icon = INKCELL_ICON_SENDING,
                                         .word = MESH_STR_DELIVERY_WAITING};
    case MESH_MESSAGE_ACK_DELIVERED:
        return (struct mesh_ui_delivery){.icon = INKCELL_ICON_DELIVERED,
                                         .word = MESH_STR_DELIVERY_DELIVERED};
    case MESH_MESSAGE_ACK_FAILED:
        return (struct mesh_ui_delivery){.icon = INKCELL_ICON_UNDELIVERED,
                                         .word = MESH_STR_DELIVERY_FAILED};
    case MESH_MESSAGE_ACK_NONE:
        break;
    }
    /* Nothing to report, and nothing to draw. A broadcast goes out without want_ack, so the
       common case on a channel is this one - and a tick on a message nobody was ever going to
       acknowledge would be the client inventing a delivery it never heard about. */
    return (struct mesh_ui_delivery){.icon = INKCELL_ICON_NONE, .word = INKCELL_STR_NONE};
}

bool mesh_ui_delivery_rate(uint64_t delivered, uint64_t failed, unsigned *percent) {
    const uint64_t settled = delivered + failed;
    if (settled == 0U || settled < delivered || percent == NULL) {
        return false;
    }
    const uint64_t share =
        delivered > UINT64_MAX / 100U ? delivered / (settled / 100U) : delivered * 100U / settled;
    *percent = share > 100U ? 100U : (unsigned)share;
    return true;
}
