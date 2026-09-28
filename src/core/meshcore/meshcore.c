#include "mesh/core/meshcore.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "inkwell/base/wipe.h"

#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/proto/ble_profile.h"
#include "mesh/proto/stream_framing.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The conversation is one command at a time. The firmware answers in order and its BLE send
 * queue holds four frames, so pipelining buys nothing but a way to lose answers; a queue here
 * and one outstanding command there is the whole flow control. Pushes (0x80 and up) arrive
 * whenever they like and are handled without touching the queue.
 *
 * Connecting runs down a fixed list - DEVICE_QUERY, APP_START, the clock, the contacts, each
 * channel slot - and each step's answer queues the next, so a step the radio refuses ends the
 * walk where it stands rather than leaving a request nothing will answer.
 */

#define MESH_MESHCORE_APP_NAME "MeshClient"

/* Meshtastic's channel roles, which is what the model's channel table is written in. */
enum {
    MESH_MESHCORE_ROLE_DISABLED = 0,
    MESH_MESHCORE_ROLE_PRIMARY = 1,
    MESH_MESHCORE_ROLE_SECONDARY = 2,
};

/* And its device roles, for the one thing a MeshCore advert says a node *is*. */
enum {
    MESH_MESHCORE_DEVICE_ROLE_CLIENT = 0,
    MESH_MESHCORE_DEVICE_ROLE_REPEATER = 4,
    MESH_MESHCORE_DEVICE_ROLE_SENSOR = 6,
    MESH_MESHCORE_DEVICE_ROLE_CLIENT_BASE = 12,
};

/* ---------------------------------------------------------------------------- the queue */

static struct mesh_meshcore_request *mesh_meshcore_head(struct mesh_meshcore *meshcore) {
    return meshcore->queue_count > 0U ? &meshcore->queue[meshcore->queue_head] : NULL;
}

static uint8_t mesh_meshcore_head_cmd(const struct mesh_meshcore *meshcore) {
    if (!meshcore->awaiting || meshcore->queue_count == 0U) {
        return 0U;
    }
    return meshcore->queue[meshcore->queue_head].frame[0];
}

static void mesh_meshcore_pop(struct mesh_meshcore *meshcore) {
    if (meshcore->queue_count == 0U) {
        return;
    }
    /* A login's frame carries its password, and a slot is otherwise only overwritten when the
       ring comes round to it again. Only that one: a reply is read against its request's frame
       after the pop, and nothing reads a login's. */
    struct mesh_meshcore_request *head = &meshcore->queue[meshcore->queue_head];
    if (head->frame[0] == MESH_MESHCORE_CMD_SEND_LOGIN) {
        inkwell_wipe(head, sizeof *head);
    }
    meshcore->queue_head = (meshcore->queue_head + 1U) % MESH_MESHCORE_QUEUE_LEN;
    meshcore->queue_count -= 1U;
    meshcore->awaiting = false;
}

static void mesh_meshcore_mark(struct mesh_meshcore *meshcore, uint32_t packet_id,
                               enum mesh_message_ack ack) {
    if (packet_id != 0U) {
        (void)mesh_message_log_mark_ack(&meshcore->model->messages, packet_id, ack, 0U);
    }
}

static bool mesh_meshcore_is_settings_write(uint8_t cmd);
static void mesh_meshcore_settle_write(struct mesh_meshcore *meshcore, int32_t error);
static bool mesh_meshcore_is_remote(uint8_t cmd);
static uint8_t mesh_meshcore_adv_type(uint32_t role);
static void mesh_meshcore_request_ended(struct mesh_meshcore *meshcore, uint8_t answer);
static void mesh_meshcore_notify(struct mesh_meshcore *meshcore, uint32_t node_id, uint8_t cmd,
                                 uint8_t answer);

/* Writes the head of the queue when nothing is outstanding. A write that fails is dropped and
   the next one tried, so one refused frame cannot wedge the queue behind it - and a settings
   command so dropped is settled as refused, or its save would wait on it for ever. */
static void mesh_meshcore_pump(struct mesh_meshcore *meshcore) {
    while (!meshcore->awaiting && meshcore->queue_count > 0U && meshcore->send != NULL) {
        struct mesh_meshcore_request *request = mesh_meshcore_head(meshcore);
        const int result =
            meshcore->send(meshcore->send_ctx, request->frame, request->len, request->packet_id);
        if (result == 0) {
            meshcore->awaiting = true;
            /* Read here rather than taken from the last tick: a whole handshake runs between two
               ticks, and a stale stamp would time a command out the moment it was written. */
            meshcore->now_ms = inkwell_time_monotonic_ms();
            meshcore->awaiting_since_ms = meshcore->now_ms;
            return;
        }
        inkwell_log_warn("meshcore", "Command %u not sent: %d", (unsigned)request->frame[0],
                         result);
        meshcore->send_error = result;
        mesh_meshcore_mark(meshcore, request->packet_id, MESH_MESSAGE_ACK_FAILED);
        const uint8_t cmd = request->frame[0];
        mesh_meshcore_pop(meshcore);
        if (mesh_meshcore_is_settings_write(cmd)) {
            mesh_meshcore_settle_write(meshcore, result);
        }
        /* A request to another node the link would not take was never asked. */
        if (mesh_meshcore_is_remote(cmd) && cmd == meshcore->request_cmd) {
            mesh_meshcore_request_ended(meshcore, MESH_MESHCORE_ANSWER_UNSENT);
        }
    }
}

static int mesh_meshcore_enqueue_tagged(struct mesh_meshcore *meshcore, const uint8_t *frame,
                                        int len, uint32_t packet_id, uint8_t favorite,
                                        bool never_heard) {
    if (len < 0) {
        return len;
    }
    if (meshcore->send == NULL) {
        return -ENOTCONN;
    }
    if (meshcore->queue_count >= MESH_MESHCORE_QUEUE_LEN) {
        return -ENOBUFS;
    }
    const size_t slot = (meshcore->queue_head + meshcore->queue_count) % MESH_MESHCORE_QUEUE_LEN;
    struct mesh_meshcore_request *request = &meshcore->queue[slot];
    memcpy(request->frame, frame, (size_t)len);
    request->len = (uint8_t)len;
    request->packet_id = packet_id;
    request->favorite = favorite;
    request->never_heard = never_heard;
    meshcore->queue_count += 1U;
    /* Alone in an idle queue it is written now, and a link that refuses it outright leaves
       nothing on its way: that refusal is this call's answer, not a success. */
    const bool immediate = !meshcore->awaiting && meshcore->queue_count == 1U;
    meshcore->send_error = 0;
    mesh_meshcore_pump(meshcore);
    if (immediate && !meshcore->awaiting) {
        return meshcore->send_error < 0 ? meshcore->send_error : -EIO;
    }
    return 0;
}

static int mesh_meshcore_enqueue(struct mesh_meshcore *meshcore, const uint8_t *frame, int len,
                                 uint32_t packet_id) {
    return mesh_meshcore_enqueue_tagged(meshcore, frame, len, packet_id,
                                        MESH_MESHCORE_FAVORITE_NONE, false);
}

static bool mesh_meshcore_queued(const struct mesh_meshcore *meshcore, uint8_t cmd) {
    for (size_t i = 0; i < meshcore->queue_count; ++i) {
        if (meshcore->queue[(meshcore->queue_head + i) % MESH_MESHCORE_QUEUE_LEN].frame[0] == cmd) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------- requests to another node */

/* The commands the radio answers for with a push from the node asked: one outstanding at a
   time, since each clears whatever the last left pending (clearPendingReqs()). */
static bool mesh_meshcore_is_remote(uint8_t cmd) {
    return cmd == MESH_MESHCORE_CMD_SEND_TELEMETRY_REQ || cmd == MESH_MESHCORE_CMD_SEND_LOGIN ||
           cmd == MESH_MESHCORE_CMD_SEND_STATUS_REQ ||
           cmd == MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ ||
           cmd == MESH_MESHCORE_CMD_SEND_BINARY_REQ;
}

/* Says how something asked of another node ended, for the publish to put into words. */
static void mesh_meshcore_notify(struct mesh_meshcore *meshcore, uint32_t node_id, uint8_t cmd,
                                 uint8_t answer) {
    meshcore->notice.node_id = node_id;
    meshcore->notice.cmd = cmd;
    meshcore->notice.answer = answer;
    meshcore->notices += 1U;
}

/* The request is over: the lock is free, and `notice` says how it went. */
static void mesh_meshcore_request_ended(struct mesh_meshcore *meshcore, uint8_t answer) {
    if (meshcore->request_cmd == 0U) {
        return;
    }
    /* A route that never came is a trace that timed out, as Meshtastic's lost one is. */
    struct mesh_traceroute *trace = &meshcore->model->traceroute;
    if (meshcore->request_cmd == MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ &&
        answer != MESH_MESHCORE_ANSWER_ROUTE && trace->state == MESH_TRACEROUTE_PENDING &&
        trace->target == meshcore->request_node) {
        trace->state = MESH_TRACEROUTE_TIMEOUT;
    }
    mesh_meshcore_notify(meshcore, meshcore->request_node, meshcore->request_cmd, answer);
    meshcore->request_cmd = 0U;
    meshcore->request_until_ms = 0U;
    meshcore->request_tag = 0U;
}

/* Its deadline passed with nothing heard. */
static void mesh_meshcore_request_expire(struct mesh_meshcore *meshcore, uint64_t now_ms) {
    if (meshcore->request_until_ms != 0U && now_ms >= meshcore->request_until_ms) {
        mesh_meshcore_request_ended(meshcore, MESH_MESHCORE_ANSWER_SILENT);
    }
}

/*
 * A node answered `cmd`. Only the node asked ends it: the firmware pushes an answer only while
 * that request is its pending one, but a late answer from another node's earlier request can be
 * in flight when this one is written. And only once the request is written: while it still
 * waits in the queue, the radio's pending request - and so the answer - is the last one's.
 */
static bool mesh_meshcore_request_answered(struct mesh_meshcore *meshcore, const uint8_t *prefix,
                                           uint8_t cmd, uint8_t answer) {
    if (meshcore->request_cmd != cmd ||
        memcmp(prefix, meshcore->request_prefix, MESH_MESHCORE_PREFIX_LEN) != 0) {
        return false;
    }
    const bool written = meshcore->awaiting && mesh_meshcore_head_cmd(meshcore) == cmd;
    if (!written && mesh_meshcore_queued(meshcore, cmd)) {
        return false;
    }
    /* Ended, so a SENT still to come for it - an answer can beat one - sets no deadline. */
    mesh_meshcore_request_ended(meshcore, answer);
    return true;
}

static void mesh_meshcore_request_byte(struct mesh_meshcore *meshcore, uint8_t cmd, uint8_t value) {
    uint8_t frame[2];
    (void)mesh_meshcore_enqueue(meshcore, frame,
                                mesh_meshcore_encode_byte(cmd, value, frame, sizeof frame), 0U);
}

static void mesh_meshcore_request_plain(struct mesh_meshcore *meshcore, uint8_t cmd) {
    (void)mesh_meshcore_enqueue(meshcore, &cmd, 1, 0U);
}

/* Drains the radio's message queue. One SYNC_NEXT_MESSAGE at a time; each answer that is a
   message asks again, and NO_MORE_MESSAGES stops. */
static void mesh_meshcore_request_sync(struct mesh_meshcore *meshcore) {
    if (!mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE)) {
        mesh_meshcore_request_plain(meshcore, MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE);
    }
}

/* ---------------------------------------------------------------------------- the model */

/* Up to four bytes of whole UTF-8 characters, skipping leading spaces: a MeshCore node has one
   name, and the roster's short name is what a disc or a narrow column shows. An emoji - which
   plenty of MeshCore names lead with - is exactly four. */
static void mesh_meshcore_short_name(const char *name, char *out, size_t out_len) {
    size_t used = 0U;
    while (*name == ' ') {
        ++name;
    }
    while (*name != '\0' && *name != ' ') {
        const size_t step =
            inkwell_text_utf8_sequence_len((const uint8_t *)name, strnlen(name, 4U));
        if (step == 0U || used + step + 1U > out_len) {
            break;
        }
        memcpy(out + used, name, step);
        used += step;
        name += step;
    }
    out[used] = '\0';
}

static uint32_t mesh_meshcore_role(uint8_t adv_type) {
    switch (adv_type) {
    case MESH_MESHCORE_ADV_REPEATER:
        return MESH_MESHCORE_DEVICE_ROLE_REPEATER;
    case MESH_MESHCORE_ADV_SENSOR:
        return MESH_MESHCORE_DEVICE_ROLE_SENSOR;
    case MESH_MESHCORE_ADV_ROOM:
        return MESH_MESHCORE_DEVICE_ROLE_CLIENT_BASE;
    default:
        return MESH_MESHCORE_DEVICE_ROLE_CLIENT;
    }
}

static void mesh_meshcore_name_node(struct mesh_node_summary *node, const uint8_t *key,
                                    const char *name, uint8_t adv_type) {
    if (name != NULL && name[0] != '\0') {
        inkwell_text_sanitise_str(name, node->long_name, sizeof node->long_name);
        mesh_meshcore_short_name(node->long_name, node->short_name, sizeof node->short_name);
        node->has_user = true;
    }
    /* The key's first six bytes, which is how MeshCore's own apps print a node. */
    snprintf(node->user_id, sizeof node->user_id, "%02x%02x%02x%02x%02x%02x", key[0], key[1],
             key[2], key[3], key[4], key[5]);
    memcpy(node->public_key, key, MESH_MESHCORE_PUBKEY_LEN);
    node->public_key_len = (uint8_t)MESH_MESHCORE_PUBKEY_LEN;
    node->role = mesh_meshcore_role(adv_type);
}

/* Keeps a heard advert whole, over an older one from the same key or else the one kept longest
   ago - a node heard again is the newest, not left where it was first kept. */
static void mesh_meshcore_keep_advert(struct mesh_meshcore *meshcore,
                                      const struct mesh_meshcore_contact *contact) {
    size_t slot = 0U;
    for (size_t i = 0; i < MESH_MESHCORE_HEARD_ADVERTS; ++i) {
        if (meshcore->heard_age[i] != 0U &&
            memcmp(meshcore->heard[i].public_key, contact->public_key, MESH_MESHCORE_PUBKEY_LEN) ==
                0) {
            slot = i;
            break;
        }
        if (meshcore->heard_age[i] < meshcore->heard_age[slot]) {
            slot = i;
        }
    }
    meshcore->heard[slot] = *contact;
    meshcore->heard_age[slot] = ++meshcore->heard_clock;
}

static const struct mesh_meshcore_contact *
mesh_meshcore_heard_advert(const struct mesh_meshcore *meshcore, const uint8_t *key) {
    for (size_t i = 0; i < MESH_MESHCORE_HEARD_ADVERTS; ++i) {
        if (meshcore->heard_age[i] != 0U &&
            memcmp(meshcore->heard[i].public_key, key, MESH_MESHCORE_PUBKEY_LEN) == 0) {
            return &meshcore->heard[i];
        }
    }
    return NULL;
}

/* `imported` is a contact the user added themselves, which the roster takes without calling it
   a discovery; see mesh_session_model_contact(). */
static void mesh_meshcore_store_contact(struct mesh_meshcore *meshcore,
                                        const struct mesh_meshcore_contact *contact, bool synced,
                                        bool imported) {
    const uint32_t id = mesh_meshcore_node_id(contact->public_key, MESH_MESHCORE_PUBKEY_LEN);
    /* lastmod is the radio's clock, which we set; the advert's own stamp is the sender's. */
    const uint32_t heard = contact->lastmod != 0U ? contact->lastmod : contact->last_advert;
    /* The contact list is the radio's list, as a Meshtastic replay is: news only when heard
       after the roster's newest (mesh_session_model_listed()). */
    struct mesh_node_summary *node = imported ? mesh_session_model_contact(meshcore->model, id)
                                     : synced
                                         ? mesh_session_model_listed(meshcore->model, id, heard)
                                         : mesh_session_model_node(meshcore->model, id, false);
    if (node == NULL) {
        return;
    }
    mesh_meshcore_name_node(node, contact->public_key, contact->name, contact->type);
    node->in_nodedb = true;
    node->is_favorite = (contact->flags & MESH_MESHCORE_CONTACT_FAVORITE) != 0U;
    if (heard > node->last_heard) {
        node->last_heard = heard;
    }
    if (contact->out_path_len != MESH_MESHCORE_PATH_NONE) {
        node->has_hops_away = true;
        node->hops_away = MESH_MESHCORE_PATH_HOPS(contact->out_path_len);
    }
    if (contact->latitude_e6 != 0 || contact->longitude_e6 != 0) {
        node->position.valid = true;
        node->position.latitude_i = contact->latitude_e6 * 10;
        node->position.longitude_i = contact->longitude_e6 * 10;
    }
    /* A contact with no stamp at all is one typed in from a link, never heard. */
    if (heard != 0U) {
        const bool has_hops = contact->out_path_len != MESH_MESHCORE_PATH_NONE;
        const struct mesh_session_event event = {
            .kind = MESH_SESSION_EVENT_NODE_LISTED,
            .node = node,
            .has_hops = has_hops,
            .hops = has_hops ? (uint8_t)MESH_MESHCORE_PATH_HOPS(contact->out_path_len) : 0U};
        mesh_session_model_note_node(meshcore->model, &event);
    }
}

/* A packet from `node` arrived just now, over the air: MeshCore has no MQTT leg. */
static void mesh_meshcore_note_heard(struct mesh_meshcore *meshcore,
                                     const struct mesh_node_summary *node, bool has_hops,
                                     uint8_t hops) {
    const struct mesh_session_event event = {
        .kind = MESH_SESSION_EVENT_NODE_HEARD, .node = node, .has_hops = has_hops, .hops = hops};
    mesh_session_model_note_node(meshcore->model, &event);
}

static void mesh_meshcore_store_self(struct mesh_meshcore *meshcore) {
    const struct mesh_meshcore_self_info *self = &meshcore->self;
    meshcore->self_node = mesh_meshcore_node_id(self->public_key, MESH_MESHCORE_PUBKEY_LEN);
    mesh_session_model_adopt_radio(meshcore->model, meshcore->self_node);
    struct mesh_node_summary *node =
        mesh_session_model_node(meshcore->model, meshcore->self_node, true);
    if (node == NULL) {
        return;
    }
    mesh_meshcore_name_node(node, self->public_key, self->name, self->adv_type);
    node->in_nodedb = true;
    const uint32_t now = inkwell_time_wall_credible_s();
    if (now > node->last_heard) {
        node->last_heard = now;
    }
    if (self->latitude_e6 != 0 || self->longitude_e6 != 0) {
        node->position.valid = true;
        node->position.latitude_i = self->latitude_e6 * 10;
        node->position.longitude_i = self->longitude_e6 * 10;
    } else {
        /* No advert location: cleared on the radio, perhaps by another app, so the fix this
           record held is no longer the radio's and must not be shown or saved back. */
        memset(&node->position, 0, sizeof node->position);
    }
}

/*
 * SELF_INFO onto the settings record the Settings tab reads, in Meshtastic's shape: the name is
 * the owner, the radio parameters are a LoRa config with its preset off - MeshCore has no
 * presets and no regions, only the four numbers - and the position section is loaded so its
 * coordinate rows have something to stand on. The bandwidth goes in as Meshtastic writes it,
 * whole kHz, so 62.5 is 62 exactly as it is on a Meshtastic radio.
 */
static void mesh_meshcore_store_settings(struct mesh_meshcore *meshcore) {
    struct mesh_radio_settings *settings = mesh_session_model_settings(meshcore->model);
    if (settings == NULL) {
        return;
    }
    const struct mesh_meshcore_self_info *self = &meshcore->self;
    settings->has_owner = true;
    memset(&settings->owner, 0, sizeof settings->owner);
    inkwell_str_copy(settings->owner.long_name, sizeof settings->owner.long_name, self->name);
    mesh_meshcore_short_name(self->name, settings->owner.short_name,
                             sizeof settings->owner.short_name);

    settings->has_lora = true;
    memset(&settings->lora, 0, sizeof settings->lora);
    settings->lora.use_preset = false;
    settings->lora.tx_enabled = true;
    settings->lora.bandwidth = (uint16_t)(self->bandwidth_hz / 1000U);
    settings->lora.spread_factor = self->spreading_factor;
    settings->lora.coding_rate = self->coding_rate;
    settings->lora.tx_power = (int8_t)self->tx_power_dbm;
    settings->lora.override_frequency = (float)self->frequency_khz / 1000.0f;

    settings->has_position = true;
}

/* Every command a settings save is made of. */
static bool mesh_meshcore_is_settings_write(uint8_t cmd) {
    return cmd == MESH_MESHCORE_CMD_SET_ADVERT_NAME || cmd == MESH_MESHCORE_CMD_SET_RADIO_PARAMS ||
           cmd == MESH_MESHCORE_CMD_SET_RADIO_TX_POWER ||
           cmd == MESH_MESHCORE_CMD_SET_ADVERT_LATLON || cmd == MESH_MESHCORE_CMD_SET_CHANNEL ||
           cmd == MESH_MESHCORE_CMD_SET_OTHER_PARAMS || cmd == MESH_MESHCORE_CMD_SET_DEVICE_PIN;
}

/* One command of a save answered: `error` is 0 for OK. The last answer settles the save into
   the model's write counters, which is what the app's save toast is watching. */
static void mesh_meshcore_settle_write(struct mesh_meshcore *meshcore, int32_t error) {
    if (meshcore->writes_outstanding == 0U) {
        return;
    }
    if (error != 0 && meshcore->write_error == 0) {
        meshcore->write_error = error;
    }
    meshcore->writes_outstanding--;
    if (meshcore->writes_outstanding > 0U) {
        return;
    }
    struct mesh_radio_settings *settings = mesh_session_model_settings(meshcore->model);
    if (settings == NULL) {
        return;
    }
    if (meshcore->write_error != 0) {
        settings->writes_failed += 1U;
        settings->last_write_error = meshcore->write_error;
    } else {
        settings->writes_acked += 1U;
    }
    meshcore->write_error = 0;
}

static bool mesh_meshcore_channel_used(const struct mesh_meshcore_channel *channel) {
    if (channel->name[0] != '\0') {
        return true;
    }
    for (size_t i = 0; i < MESH_MESHCORE_SECRET_LEN; ++i) {
        if (channel->secret[i] != 0U) {
            return true;
        }
    }
    return false;
}

static void mesh_meshcore_store_channel(struct mesh_meshcore *meshcore,
                                        const struct mesh_meshcore_channel *channel) {
    struct mesh_channel_summary summary;
    memset(&summary, 0, sizeof summary);
    summary.index = channel->index;
    if (mesh_meshcore_channel_used(channel)) {
        summary.role =
            channel->index == 0U ? MESH_MESHCORE_ROLE_PRIMARY : MESH_MESHCORE_ROLE_SECONDARY;
        summary.psk_len = (uint8_t)MESH_MESHCORE_SECRET_LEN;
        inkwell_text_sanitise_str(channel->name, summary.name, sizeof summary.name);
    } else {
        summary.role = MESH_MESHCORE_ROLE_DISABLED;
    }
    if (mesh_session_model_set_channel(meshcore->model, &summary) < 0) {
        inkwell_log_debug("meshcore", "Channel %u is past the %u the client shows",
                          (unsigned)channel->index, (unsigned)MESH_SESSION_MAX_CHANNELS);
        return;
    }
    /* And the slot as a settings record, secret included, which is what the channel editor
       starts from and what a save writes back when the key is kept. The record's name is
       Meshtastic's twelve bytes; the full one is the summary's above. */
    struct mesh_radio_settings *settings = mesh_session_model_settings(meshcore->model);
    if (settings == NULL || channel->index >= MESH_RADIO_SETTINGS_MAX_CHANNELS) {
        return;
    }
    meshtastic_Channel *record = &settings->channels[channel->index];
    memset(record, 0, sizeof *record);
    record->index = (int8_t)channel->index;
    record->role = (meshtastic_Channel_Role)summary.role;
    if (summary.role != MESH_MESHCORE_ROLE_DISABLED) {
        record->has_settings = true;
        inkwell_str_copy(record->settings.name, sizeof record->settings.name, summary.name);
        memcpy(record->settings.psk.bytes, channel->secret, MESH_MESHCORE_SECRET_LEN);
        record->settings.psk.size = (pb_size_t)MESH_MESHCORE_SECRET_LEN;
    }
    settings->has_channel[channel->index] = true;
}

/* The roster entry whose key starts with `prefix`, or 0. */
static uint32_t mesh_meshcore_find_prefix(const struct mesh_meshcore *meshcore,
                                          const uint8_t *prefix, size_t len) {
    const struct mesh_handshake_status *status = &meshcore->model->handshake;
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        const struct mesh_node_summary *node = &status->nodes[i];
        if (node->public_key_len == MESH_MESHCORE_PUBKEY_LEN &&
            memcmp(node->public_key, prefix, len) == 0) {
            return node->node_id;
        }
    }
    return 0U;
}

/* The one node whose key starts with `hash`, or 0 for none or for more than one: a byte names
   one key in 256, so a guess among several would draw the wrong repeater with confidence. */
static uint32_t mesh_meshcore_find_hash(const struct mesh_meshcore *meshcore, const uint8_t *hash,
                                        size_t len) {
    const struct mesh_handshake_status *status = &meshcore->model->handshake;
    uint32_t found = 0U;
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        const struct mesh_node_summary *node = &status->nodes[i];
        if (node->public_key_len == MESH_MESHCORE_PUBKEY_LEN &&
            memcmp(node->public_key, hash, len) == 0) {
            if (found != 0U) {
                return 0U;
            }
            found = node->node_id;
        }
    }
    return found;
}

/*
 * One path out of a PATH_DISCOVERY_RESPONSE at `*at`: its length byte - the hop count in the low
 * six bits, the bytes naming each hop less one in the top two - then the hops. Each is named by
 * the roster where one node answers to it, and kept as its bytes where none or several do.
 * Past MESH_TRACEROUTE_MAX_HOPS the rest are skipped. False for a length the firmware reserves
 * or a frame too short for it.
 */
static bool mesh_meshcore_read_path(const struct mesh_meshcore *meshcore, const uint8_t *frame,
                                    size_t len, size_t *at, uint32_t *route, uint8_t *count,
                                    uint8_t hashes[][MESH_TRACEROUTE_HASH_MAX], uint8_t *size) {
    if (*at >= len) {
        return false;
    }
    const uint8_t encoded = frame[(*at)++];
    const uint8_t hops = MESH_MESHCORE_PATH_HOPS(encoded);
    const uint8_t width = (uint8_t)((encoded >> 6U) + 1U);
    if (width > MESH_TRACEROUTE_HASH_MAX || (size_t)hops * width > MESH_MESHCORE_PATH_MAX ||
        len - *at < (size_t)hops * width) {
        return false;
    }
    *size = width;
    *count = 0U;
    for (uint8_t hop = 0; hop < hops; ++hop) {
        const uint8_t *hash = frame + *at + (size_t)hop * width;
        if (*count < MESH_TRACEROUTE_MAX_HOPS) {
            route[*count] = mesh_meshcore_find_hash(meshcore, hash, width);
            memcpy(hashes[*count], hash, width);
            *count += 1U;
        }
    }
    *at += (size_t)hops * width;
    return true;
}

static const struct mesh_node_summary *
mesh_meshcore_model_find(const struct mesh_meshcore *meshcore, uint32_t node_id) {
    const struct mesh_handshake_status *status = &meshcore->model->handshake;
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (status->nodes[i].node_id == node_id) {
            return &status->nodes[i];
        }
    }
    return NULL;
}

/* A channel message's sender, when the name it starts with is a node we know by that name. */
static uint32_t mesh_meshcore_find_name(const struct mesh_meshcore *meshcore, const char *name) {
    const struct mesh_handshake_status *status = &meshcore->model->handshake;
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        const struct mesh_node_summary *node = &status->nodes[i];
        if (node->has_user && strcmp(node->long_name, name) == 0) {
            return node->node_id;
        }
    }
    return 0U;
}

static void mesh_meshcore_store_message(struct mesh_meshcore *meshcore,
                                        const struct mesh_meshcore_message *decoded) {
    struct mesh_message message;
    memset(&message, 0, sizeof message);
    message.packet_id = mesh_session_next_packet_id(meshcore->model);
    message.direction = MESH_MESSAGE_INBOUND;
    message.kind = MESH_MESSAGE_KIND_TEXT;
    message.rx_time = decoded->timestamp;
    if (decoded->has_snr) {
        message.rx_snr = (float)decoded->snr_q4 / 4.0F;
    }
    if (decoded->path_len != MESH_MESHCORE_PATH_NONE) {
        message.has_hops_away = true;
        message.hops_away = MESH_MESHCORE_PATH_HOPS(decoded->path_len);
    } else {
        /* Direct, which is zero hops as far as the screen is concerned. */
        message.has_hops_away = true;
        message.hops_away = 0U;
    }

    char text[MESH_MESSAGE_TEXT_MAX + 1U];
    inkwell_text_sanitise(decoded->text, decoded->text_len, text, sizeof text);
    const char *body = text;

    if (decoded->channel) {
        message.to = MESH_MESSAGE_BROADCAST_ADDR;
        message.channel = decoded->channel_index;
        /* "Name: text" is the only sender a channel message has. A name we hold a node for
           becomes the sender and leaves the text; one we do not stays in the text, which is
           how MeshCore's own apps would show a stranger anyway. */
        const char *colon = strstr(text, ": ");
        if (colon != NULL && (size_t)(colon - text) <= MESH_MESHCORE_NAME_LEN) {
            char name[MESH_MESHCORE_NAME_LEN + 1U];
            memcpy(name, text, (size_t)(colon - text));
            name[colon - text] = '\0';
            const uint32_t from = mesh_meshcore_find_name(meshcore, name);
            if (from != 0U) {
                message.from = from;
                body = colon + 2;
            }
        }
    } else {
        message.to = meshcore->self_node;
        message.pki_encrypted = true;
        /* A room server relays each post under its own key and names the author by four
           bytes of theirs. The room stays the sender, so its posts stay one conversation, and
           the author goes in front of the text the way a channel message carries its sender:
           by name when the roster knows the key, by those four bytes when it does not. */
        if (decoded->has_author) {
            const uint32_t author = mesh_meshcore_find_prefix(meshcore, decoded->author_prefix, 4U);
            const struct mesh_node_summary *node =
                author != 0U ? mesh_meshcore_model_find(meshcore, author) : NULL;
            char attributed[sizeof node->long_name + sizeof ": " + sizeof text];
            if (node != NULL && node->has_user) {
                snprintf(attributed, sizeof attributed, "%s: %s", node->long_name, text);
            } else {
                snprintf(attributed, sizeof attributed, "%02x%02x%02x%02x: %s",
                         decoded->author_prefix[0], decoded->author_prefix[1],
                         decoded->author_prefix[2], decoded->author_prefix[3], text);
            }
            /* A post is at most MESH_MESHCORE_TEXT_MAX bytes, so a name in front of one
               still fits a message and nothing here is cut. */
            _Static_assert(sizeof node->long_name + sizeof ": " + MESH_MESHCORE_TEXT_MAX <=
                               MESH_MESSAGE_TEXT_MAX + 1U,
                           "an attributed room post fits a message");
            inkwell_str_copy(text, sizeof text, attributed);
        }
        uint32_t from =
            mesh_meshcore_find_prefix(meshcore, decoded->sender_prefix, MESH_MESHCORE_PREFIX_LEN);
        if (from == 0U) {
            from = mesh_meshcore_node_id(decoded->sender_prefix, MESH_MESHCORE_PREFIX_LEN);
        }
        message.from = from;
        struct mesh_node_summary *node = mesh_session_model_node(meshcore->model, from, false);
        if (node != NULL) {
            const uint32_t now = inkwell_time_wall_credible_s();
            if (now > node->last_heard) {
                node->last_heard = now;
            }
            if (decoded->has_snr) {
                node->snr = message.rx_snr;
                node->snr_time = now;
            }
            mesh_meshcore_note_heard(meshcore, node, message.has_hops_away, message.hops_away);
        }
    }
    snprintf(message.text, sizeof message.text, "%s", body);
    (void)mesh_session_model_log_message(meshcore->model, &message);
}

/* ------------------------------------------------------------------------- the handshake */

static void mesh_meshcore_request_channel(struct mesh_meshcore *meshcore, uint8_t index) {
    meshcore->channel_probe = index;
    mesh_meshcore_request_byte(meshcore, MESH_MESHCORE_CMD_GET_CHANNEL, index);
}

static uint8_t mesh_meshcore_channel_limit(const struct mesh_meshcore *meshcore) {
    uint8_t limit = meshcore->has_device && meshcore->device.max_channels > 0U
                        ? meshcore->device.max_channels
                        : 1U;
    if (limit > MESH_SESSION_MAX_CHANNELS) {
        limit = (uint8_t)MESH_SESSION_MAX_CHANNELS;
    }
    return limit;
}

static void mesh_meshcore_ready_now(struct mesh_meshcore *meshcore) {
    meshcore->phase = MESH_MESHCORE_READY;
    mesh_session_model_sync_complete(meshcore->model);
    /* A favourite is a flag on the radio's contact record: a node the sync left off the list
       has no record to carry one, and one still shown pinned could never be unpinned. */
    struct mesh_handshake_status *roster = &meshcore->model->handshake;
    for (size_t i = 0; i < roster->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (!roster->nodes[i].in_nodedb) {
            roster->nodes[i].is_favorite = false;
        }
    }
    inkwell_log_info("meshcore", "Synced: %zu nodes, %zu channels",
                     meshcore->model->handshake.node_count,
                     meshcore->model->handshake.channel_count);
    mesh_meshcore_request_sync(meshcore);
    mesh_meshcore_request_plain(meshcore, MESH_MESHCORE_CMD_GET_BATT_AND_STORAGE);
}

static void mesh_meshcore_after_self(struct mesh_meshcore *meshcore) {
    /* The radio's clock only moves forward, and a Brick without network time has none worth
       giving it; either way the answer is not worth waiting on. */
    const uint32_t now = inkwell_time_wall_credible_s();
    if (now != 0U) {
        uint8_t frame[5];
        (void)mesh_meshcore_enqueue(
            meshcore, frame,
            mesh_meshcore_encode_u32(MESH_MESHCORE_CMD_SET_DEVICE_TIME, now, frame, sizeof frame),
            0U);
    } else {
        /* Then the radio's clock is the better of the two, and the one it stamps its own
           traffic with; see mesh_meshcore_timestamp(). */
        mesh_meshcore_request_plain(meshcore, MESH_MESHCORE_CMD_GET_DEVICE_TIME);
    }
    meshcore->phase = MESH_MESHCORE_CONTACTS;
    uint8_t frame[5];
    (void)mesh_meshcore_enqueue(meshcore, frame,
                                mesh_meshcore_encode_u32(MESH_MESHCORE_CMD_GET_CONTACTS,
                                                         meshcore->contacts_since, frame,
                                                         sizeof frame),
                                0U);
}

/* ---------------------------------------------------------------------------- sending */

static struct mesh_meshcore_pending *mesh_meshcore_pending_for(struct mesh_meshcore *meshcore,
                                                               uint32_t packet_id) {
    for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        if (packet_id != 0U && meshcore->pending[i].packet_id == packet_id) {
            return &meshcore->pending[i];
        }
    }
    return NULL;
}

static int mesh_meshcore_send_attempt(struct mesh_meshcore *meshcore,
                                      struct mesh_meshcore_pending *pending) {
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    const int len = mesh_meshcore_encode_text(
        pending->key, pending->command ? MESH_MESHCORE_TXT_CLI_DATA : MESH_MESHCORE_TXT_PLAIN,
        pending->attempt, pending->timestamp, pending->text, frame, sizeof frame);
    pending->deadline_ms = 0U;
    return mesh_meshcore_enqueue(meshcore, frame, len, pending->packet_id);
}

static void mesh_meshcore_pending_done(struct mesh_meshcore *meshcore,
                                       struct mesh_meshcore_pending *pending,
                                       enum mesh_message_ack ack) {
    mesh_meshcore_mark(meshcore, pending->packet_id, ack);
    memset(pending, 0, sizeof *pending);
}

/* A repeater replied to a command: the oldest one to it still waiting - the first sent, which is
   not the lowest packet id - is the one answered. One already given up on counts: its reply was
   late, not missing, so it is delivered after all and the next command keeps its own place. The
   reply names its sender by a six-byte prefix, and a command is held by the whole key. */
static void mesh_meshcore_command_answered(struct mesh_meshcore *meshcore,
                                           const uint8_t prefix[MESH_MESHCORE_PREFIX_LEN]) {
    struct mesh_meshcore_pending *oldest = NULL;
    for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        struct mesh_meshcore_pending *pending = &meshcore->pending[i];
        if (pending->packet_id != 0U && pending->command &&
            memcmp(pending->key, prefix, MESH_MESHCORE_PREFIX_LEN) == 0 &&
            (oldest == NULL || (int32_t)(pending->sequence - oldest->sequence) < 0)) {
            oldest = pending;
        }
    }
    if (oldest != NULL) {
        mesh_meshcore_pending_done(meshcore, oldest, MESH_MESSAGE_ACK_DELIVERED);
    }
}

/* A direct message whose ack did not come. Try again - the last time with the route reset, so
   the firmware floods it rather than trusting a path that has just failed twice. */
static void mesh_meshcore_retry(struct mesh_meshcore *meshcore,
                                struct mesh_meshcore_pending *pending) {
    /* A command the repeater did not answer is not sent again: it may have run and only the
       reply been lost, and "reboot" twice is not what anybody asked for. Silence from a
       repeater is usually a login that did not make us its admin, which is worth saying. It
       keeps its place in line a while longer (MESH_MESHCORE_COMMAND_LATE_MS), then goes. */
    if (pending->command) {
        if (pending->expired) {
            memset(pending, 0, sizeof *pending);
            return;
        }
        inkwell_log_info("meshcore", "Command %u: no reply", pending->packet_id);
        mesh_meshcore_notify(meshcore,
                             mesh_meshcore_node_id(pending->key, MESH_MESHCORE_PUBKEY_LEN),
                             MESH_MESHCORE_CMD_SEND_TXT_MSG, MESH_MESHCORE_ANSWER_SILENT);
        mesh_meshcore_mark(meshcore, pending->packet_id, MESH_MESSAGE_ACK_FAILED);
        pending->expired = true;
        pending->deadline_ms = meshcore->now_ms + MESH_MESHCORE_COMMAND_LATE_MS;
        return;
    }
    if (pending->attempt + 1U >= MESH_MESHCORE_SEND_ATTEMPTS) {
        inkwell_log_info("meshcore", "Message %u: no ack after %u attempts", pending->packet_id,
                         (unsigned)MESH_MESHCORE_SEND_ATTEMPTS);
        mesh_meshcore_pending_done(meshcore, pending, MESH_MESSAGE_ACK_FAILED);
        return;
    }
    pending->attempt += 1U;
    if (pending->attempt + 1U == MESH_MESHCORE_SEND_ATTEMPTS) {
        uint8_t frame[1U + MESH_MESHCORE_PUBKEY_LEN];
        (void)mesh_meshcore_enqueue(meshcore, frame,
                                    mesh_meshcore_encode_key(MESH_MESHCORE_CMD_RESET_PATH,
                                                             pending->key, frame, sizeof frame),
                                    0U);
    }
    if (mesh_meshcore_send_attempt(meshcore, pending) < 0) {
        mesh_meshcore_pending_done(meshcore, pending, MESH_MESSAGE_ACK_FAILED);
    }
}

/* Seconds since the epoch: ours when it is credible, else the radio's as read at the handshake
   and advanced since, else 0. */
static uint32_t mesh_meshcore_clock_now(const struct mesh_meshcore *meshcore) {
    uint32_t now = inkwell_time_wall_credible_s();
    if (now == 0U && meshcore->radio_clock != 0U) {
        const uint64_t elapsed = inkwell_time_monotonic_ms() - meshcore->radio_clock_at_ms;
        now = meshcore->radio_clock + (uint32_t)(elapsed / 1000U);
    }
    return now;
}

/* The roster's entry for `node_id`, or NULL; a lookup, never an add. */
static const struct mesh_node_summary *
mesh_meshcore_roster_node(const struct mesh_meshcore *meshcore, uint32_t node_id) {
    const struct mesh_handshake_status *status = &meshcore->model->handshake;
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (status->nodes[i].node_id == node_id) {
            return &status->nodes[i];
        }
    }
    return NULL;
}

/* A node's answer to a telemetry request, onto its record: what it did not send is left as it
   was, since a node reports only the sensors it has and only what it lets us see. */
static void mesh_meshcore_store_telemetry(struct mesh_meshcore *meshcore,
                                          struct mesh_node_summary *node,
                                          const struct mesh_meshcore_telemetry *telemetry) {
    const uint32_t now = mesh_meshcore_clock_now(meshcore);
    /* An answer is a packet from the node, so it was heard now. */
    if (now > node->last_heard) {
        node->last_heard = now;
    }
    if (telemetry->has_battery) {
        node->metrics.valid = true;
        node->metrics.time = now;
        node->metrics.has_voltage = true;
        node->metrics.voltage = telemetry->battery_v;
    }
    struct mesh_node_environment *env = &node->environment;
    const bool environment = telemetry->has_temperature || telemetry->has_humidity ||
                             telemetry->has_pressure || telemetry->has_lux ||
                             telemetry->has_voltage || telemetry->has_current;
    /* Each answer is the whole of what the node reports now: a reading it has stopped sending
       goes, rather than standing in the new report as though it were fresh. */
    if (environment) {
        memset(env, 0, sizeof *env);
        env->valid = true;
        env->time = now;
    }
    if (telemetry->has_temperature) {
        env->has_temperature = true;
        env->temperature = telemetry->temperature_c;
    }
    if (telemetry->has_humidity) {
        env->has_humidity = true;
        env->relative_humidity = telemetry->humidity_pct;
    }
    if (telemetry->has_pressure) {
        env->has_pressure = true;
        env->barometric_pressure = telemetry->pressure_hpa;
    }
    if (telemetry->has_lux) {
        env->has_lux = true;
        env->lux = telemetry->lux;
    }
    if (telemetry->has_voltage) {
        env->has_voltage = true;
        env->voltage = telemetry->voltage_v;
    }
    if (telemetry->has_current) {
        env->has_current = true;
        env->current = telemetry->current_a * 1000.0f; /* the record's current is in mA */
    }
    if (telemetry->has_position) {
        node->position.valid = true;
        node->position.latitude_i = telemetry->latitude_e7;
        node->position.longitude_i = telemetry->longitude_e7;
        node->position.has_altitude = true;
        node->position.altitude = telemetry->altitude_m;
        node->position.received = now;
    }
    /* Announced last, so a listener reads the fix this answer carried. */
    mesh_meshcore_note_heard(meshcore, node, false, 0U);
}

/* A repeater's or room server's status: its counters on `relay`, its battery where every
   node's is. Each answer is the whole of it, so what an older firmware left out goes. */
static void mesh_meshcore_store_status(struct mesh_meshcore *meshcore,
                                       struct mesh_node_summary *node,
                                       const struct mesh_meshcore_status *status) {
    const uint32_t now = mesh_meshcore_clock_now(meshcore);
    if (now > node->last_heard) {
        node->last_heard = now;
    }
    if (status->battery_mv != 0U) {
        node->metrics.valid = true;
        node->metrics.time = now;
        node->metrics.has_voltage = true;
        node->metrics.voltage = (float)status->battery_mv / 1000.0f;
    }
    struct mesh_node_relay *relay = &node->relay;
    memset(relay, 0, sizeof *relay);
    relay->valid = true;
    relay->time = now;
    relay->uptime_seconds = status->uptime_secs;
    relay->tx_queue_len = status->tx_queue_len;
    relay->noise_floor = status->noise_floor;
    relay->last_rssi = status->last_rssi;
    relay->last_snr = (float)status->last_snr_q4 / 4.0f;
    relay->packets_recv = status->packets_recv;
    relay->packets_sent = status->packets_sent;
    relay->recv_flood = status->recv_flood;
    relay->recv_direct = status->recv_direct;
    relay->sent_flood = status->sent_flood;
    relay->sent_direct = status->sent_direct;
    relay->flood_dups = status->flood_dups;
    relay->direct_dups = status->direct_dups;
    relay->air_time_secs = status->air_time_secs;
    relay->err_events = status->err_events;
    relay->has_rx_air_time = status->has_rx_air_time;
    relay->rx_air_time_secs = status->rx_air_time_secs;
    relay->has_recv_errors = status->has_recv_errors;
    relay->recv_errors = status->recv_errors;
    relay->has_posts = status->has_posts;
    relay->posted = status->posted;
    relay->post_pushes = status->post_pushes;
    mesh_meshcore_note_heard(meshcore, node, false, 0U);
}

/*
 * A repeater's neighbours, onto the record Meshtastic's NeighborInfo fills: each by the roster's
 * number for its key's first four bytes, which is exactly what the request asked for, so a
 * neighbour the roster holds is named and one it does not is its "!hex". Each answer is the
 * whole list as far as it goes; how long ago each was heard is the repeater's to judge, and it
 * already drops the ones it has not heard for long.
 */
static void mesh_meshcore_store_neighbours(struct mesh_meshcore *meshcore,
                                           struct mesh_node_summary *node,
                                           const struct mesh_meshcore_neighbours *neighbours) {
    _Static_assert(MESH_MESHCORE_NEIGHBOURS_MAX <= MESH_NODE_MAX_NEIGHBORS,
                   "a neighbours answer fits the node's record");
    const uint32_t now = mesh_meshcore_clock_now(meshcore);
    if (now > node->last_heard) {
        node->last_heard = now;
    }
    struct mesh_node_neighbors *record = &node->neighbors;
    memset(record, 0, sizeof *record);
    record->valid = true;
    record->time = now;
    for (uint8_t n = 0; n < neighbours->count; ++n) {
        const struct mesh_meshcore_neighbour *entry = &neighbours->entries[n];
        record->entries[record->count].node_id =
            mesh_meshcore_node_id(entry->prefix, MESH_MESHCORE_NEIGHBOUR_PREFIX_LEN);
        record->entries[record->count].snr = (float)entry->snr_q4 / 4.0f;
        record->count += 1U;
    }
    mesh_meshcore_note_heard(meshcore, node, false, 0U);
}

/* ---------------------------------------------------------------------------- receiving */

static uint32_t mesh_meshcore_u32_at(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static void mesh_meshcore_on_push(struct mesh_meshcore *meshcore, const uint8_t *frame,
                                  size_t len) {
    switch (frame[0]) {
    case MESH_MESHCORE_PUSH_MSG_WAITING:
        mesh_meshcore_request_sync(meshcore);
        break;
    case MESH_MESHCORE_PUSH_SEND_CONFIRMED:
        if (len >= 5U) {
            /* Compared as the four bytes the SENT reply carried, as the firmware does. */
            for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
                struct mesh_meshcore_pending *pending = &meshcore->pending[i];
                uint8_t expected[4];
                expected[0] = (uint8_t)(pending->expected_ack & 0xFFU);
                expected[1] = (uint8_t)((pending->expected_ack >> 8U) & 0xFFU);
                expected[2] = (uint8_t)((pending->expected_ack >> 16U) & 0xFFU);
                expected[3] = (uint8_t)((pending->expected_ack >> 24U) & 0xFFU);
                /* A command is never acked; the zero its SENT named is not an ack to match. */
                if (pending->packet_id != 0U && pending->deadline_ms != 0U && !pending->command &&
                    memcmp(expected, frame + 1, 4U) == 0) {
                    mesh_meshcore_pending_done(meshcore, pending, MESH_MESSAGE_ACK_DELIVERED);
                    break;
                }
            }
        }
        break;
    case MESH_MESHCORE_PUSH_ADVERT:
    case MESH_MESHCORE_PUSH_PATH_UPDATED:
        /* Only the key: the record itself is one question away. */
        if (len >= 1U + MESH_MESHCORE_PUBKEY_LEN) {
            uint8_t request[1U + MESH_MESHCORE_PUBKEY_LEN];
            (void)mesh_meshcore_enqueue(
                meshcore, request,
                mesh_meshcore_encode_key(MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY, frame + 1, request,
                                         sizeof request),
                0U);
        }
        break;
    case MESH_MESHCORE_PUSH_NEW_ADVERT: {
        /* Heard, but not added: the radio is in manual-add mode. Shown, and marked as a node
           the radio does not carry. */
        struct mesh_meshcore_contact contact;
        if (mesh_meshcore_decode_contact(frame, len, &contact) == 0) {
            mesh_meshcore_keep_advert(meshcore, &contact);
            mesh_meshcore_store_contact(meshcore, &contact, false, false);
            const uint32_t id = mesh_meshcore_node_id(contact.public_key, MESH_MESHCORE_PUBKEY_LEN);
            struct mesh_node_summary *node = mesh_session_model_node(meshcore->model, id, false);
            if (node != NULL) {
                node->in_nodedb = false;
                const bool has_hops = contact.out_path_len != MESH_MESHCORE_PATH_NONE;
                mesh_meshcore_note_heard(
                    meshcore, node, has_hops,
                    has_hops ? (uint8_t)MESH_MESHCORE_PATH_HOPS(contact.out_path_len) : 0U);
            }
        }
        break;
    }
    case MESH_MESHCORE_PUSH_TELEMETRY_RESPONSE:
        /* 0x00, the answering node's key prefix, then its Cayenne LPP. */
        if (len >= 2U + MESH_MESHCORE_PREFIX_LEN) {
            const uint32_t id =
                mesh_meshcore_find_prefix(meshcore, frame + 2, MESH_MESHCORE_PREFIX_LEN);
            struct mesh_node_summary *node =
                id != 0U ? mesh_session_model_node(meshcore->model, id, false) : NULL;
            struct mesh_meshcore_telemetry telemetry;
            if (node != NULL &&
                mesh_meshcore_decode_lpp(frame + 2U + MESH_MESHCORE_PREFIX_LEN,
                                         len - 2U - MESH_MESHCORE_PREFIX_LEN, &telemetry) == 0) {
                mesh_meshcore_store_telemetry(meshcore, node, &telemetry);
                inkwell_log_info("meshcore", "Readings from 0x%08x", id);
            }
            /* A late one from another node is stored, and leaves the current request its
               deadline. The same node's answer to an earlier request cannot arrive once this
               one is written - the firmware pushes one only while its tag is the pending one
               (pending_telemetry), which each new request replaces. */
            mesh_meshcore_request_answered(meshcore, frame + 2,
                                           MESH_MESHCORE_CMD_SEND_TELEMETRY_REQ,
                                           MESH_MESHCORE_ANSWER_READINGS);
        }
        break;
    case MESH_MESHCORE_PUSH_LOGIN_SUCCESS:
        /* Whether it took us as its admin, then the node's key prefix; a newer firmware
           follows with its clock, our ACL permissions and its firmware level. */
        if (len >= 2U + MESH_MESHCORE_PREFIX_LEN) {
            mesh_meshcore_request_answered(meshcore, frame + 2, MESH_MESHCORE_CMD_SEND_LOGIN,
                                           frame[1] != 0U ? MESH_MESHCORE_ANSWER_ADMIN
                                                          : MESH_MESHCORE_ANSWER_GUEST);
        }
        break;
    case MESH_MESHCORE_PUSH_LOGIN_FAIL:
        if (len >= 2U + MESH_MESHCORE_PREFIX_LEN) {
            mesh_meshcore_request_answered(meshcore, frame + 2, MESH_MESHCORE_CMD_SEND_LOGIN,
                                           MESH_MESHCORE_ANSWER_REFUSED);
        }
        break;
    case MESH_MESHCORE_PUSH_STATUS_RESPONSE:
        /* 0x00, the answering node's key prefix, then its stats as they lie in its memory. */
        if (len >= 2U + MESH_MESHCORE_PREFIX_LEN) {
            const uint32_t id =
                mesh_meshcore_find_prefix(meshcore, frame + 2, MESH_MESHCORE_PREFIX_LEN);
            struct mesh_node_summary *node =
                id != 0U ? mesh_session_model_node(meshcore->model, id, false) : NULL;
            struct mesh_meshcore_status status;
            if (node != NULL &&
                mesh_meshcore_decode_status(frame + 2U + MESH_MESHCORE_PREFIX_LEN,
                                            len - 2U - MESH_MESHCORE_PREFIX_LEN,
                                            mesh_meshcore_adv_type(node->role), &status) == 0) {
                mesh_meshcore_store_status(meshcore, node, &status);
                inkwell_log_info("meshcore", "Status from 0x%08x", id);
            }
            mesh_meshcore_request_answered(meshcore, frame + 2, MESH_MESHCORE_CMD_SEND_STATUS_REQ,
                                           MESH_MESHCORE_ANSWER_STATUS);
        }
        break;
    case MESH_MESHCORE_PUSH_PATH_DISCOVERY_RESPONSE:
        /* 0x00, the node's key prefix, then the path our flood took to it and the path its
           answer took back, each a length byte and the hops it counts. */
        if (len >= 2U + MESH_MESHCORE_PREFIX_LEN) {
            const uint32_t target = meshcore->request_node;
            if (mesh_meshcore_request_answered(meshcore, frame + 2,
                                               MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ,
                                               MESH_MESHCORE_ANSWER_ROUTE)) {
                struct mesh_traceroute *trace = &meshcore->model->traceroute;
                memset(trace, 0, sizeof *trace);
                trace->target = target;
                size_t at = 2U + MESH_MESHCORE_PREFIX_LEN;
                const bool read = mesh_meshcore_read_path(meshcore, frame, len, &at, trace->route,
                                                          &trace->route_count, trace->route_hash,
                                                          &trace->hash_size) &&
                                  mesh_meshcore_read_path(meshcore, frame, len, &at,
                                                          trace->route_back, &trace->back_count,
                                                          trace->back_hash, &trace->back_hash_size);
                if (read) {
                    trace->state = MESH_TRACEROUTE_DONE;
                    trace->completed = mesh_meshcore_clock_now(meshcore);
                    inkwell_log_info("meshcore", "Route to 0x%08x: %u out, %u back", target,
                                     (unsigned)trace->route_count, (unsigned)trace->back_count);
                } else {
                    trace->state = MESH_TRACEROUTE_TIMEOUT;
                    inkwell_log_warn("meshcore", "Unreadable route from 0x%08x", target);
                }
            }
        }
        break;
    case MESH_MESHCORE_PUSH_BINARY_RESPONSE:
        /* 0x00, then the tag the request's SENT carried, then whatever the node answered. */
        if (len >= 6U && meshcore->request_cmd == MESH_MESHCORE_CMD_SEND_BINARY_REQ &&
            meshcore->request_tag != 0U &&
            mesh_meshcore_u32_at(frame + 2) == meshcore->request_tag) {
            const uint32_t id = meshcore->request_node;
            struct mesh_node_summary *node = mesh_session_model_node(meshcore->model, id, false);
            struct mesh_meshcore_neighbours neighbours;
            if (node != NULL &&
                mesh_meshcore_decode_neighbours(frame + 6, len - 6U, &neighbours) == 0) {
                mesh_meshcore_store_neighbours(meshcore, node, &neighbours);
                inkwell_log_info("meshcore", "Neighbours from 0x%08x: %u of %u", id,
                                 (unsigned)neighbours.count, (unsigned)neighbours.total);
            }
            mesh_meshcore_request_ended(meshcore, MESH_MESHCORE_ANSWER_NEIGHBOURS);
        }
        break;
    case MESH_MESHCORE_PUSH_CONTACT_DELETED:
        if (len >= 1U + MESH_MESHCORE_PUBKEY_LEN) {
            const uint32_t id = mesh_meshcore_node_id(frame + 1, MESH_MESHCORE_PUBKEY_LEN);
            struct mesh_node_summary *node = mesh_session_model_node(meshcore->model, id, false);
            if (node != NULL) {
                node->in_nodedb = false;
                node->is_favorite = false; /* the flag went with the record */
            }
        }
        break;
    case MESH_MESHCORE_PUSH_LOG_RX_DATA:
        break; /* every packet the radio heard, raw; nothing here reads it yet */
    default:
        inkwell_log_debug("meshcore", "Push 0x%02x (%zu bytes) ignored", (unsigned)frame[0], len);
        break;
    }
}

/*
 * A settings command the radio took, onto the SELF_INFO the next save is built over. The
 * read-back is queued behind it and will say the same, but a save made before it lands would
 * otherwise carry the old values of every row it did not touch and undo this one - and a
 * read-back the link drops would leave the screens on the old values until a refresh.
 */
static void mesh_meshcore_apply_write(struct mesh_meshcore *meshcore, const uint8_t *frame,
                                      size_t len) {
    struct mesh_meshcore_self_info *self = &meshcore->self;
    switch (frame[0]) {
    case MESH_MESHCORE_CMD_SET_ADVERT_NAME: {
        const size_t n = len - 1U < MESH_MESHCORE_NAME_LEN ? len - 1U : MESH_MESHCORE_NAME_LEN;
        memcpy(self->name, frame + 1, n);
        self->name[n] = '\0';
        break;
    }
    case MESH_MESHCORE_CMD_SET_RADIO_PARAMS:
        if (len >= 11U) {
            self->frequency_khz = mesh_meshcore_u32_at(frame + 1);
            self->bandwidth_hz = mesh_meshcore_u32_at(frame + 5);
            self->spreading_factor = frame[9];
            self->coding_rate = frame[10];
        }
        break;
    case MESH_MESHCORE_CMD_SET_RADIO_TX_POWER:
        if (len >= 2U) {
            self->tx_power_dbm = frame[1];
        }
        break;
    case MESH_MESHCORE_CMD_SET_ADVERT_LATLON:
        if (len >= 9U) {
            self->latitude_e6 = (int32_t)mesh_meshcore_u32_at(frame + 1);
            self->longitude_e6 = (int32_t)mesh_meshcore_u32_at(frame + 5);
        }
        break;
    case MESH_MESHCORE_CMD_SET_OTHER_PARAMS:
        if (len >= 5U) {
            self->manual_add_contacts = frame[1];
            self->telemetry_modes = frame[2];
            self->advert_loc_policy = frame[3];
            self->multi_acks = frame[4];
        }
        break;
    case MESH_MESHCORE_CMD_SET_CHANNEL:
        if (len >= 2U + MESH_MESHCORE_NAME_LEN + MESH_MESHCORE_SECRET_LEN) {
            struct mesh_meshcore_channel channel;
            memset(&channel, 0, sizeof channel);
            channel.index = frame[1];
            memcpy(channel.name, frame + 2, MESH_MESHCORE_NAME_LEN - 1U);
            memcpy(channel.secret, frame + 2 + MESH_MESHCORE_NAME_LEN, MESH_MESHCORE_SECRET_LEN);
            mesh_meshcore_store_channel(meshcore, &channel);
        }
        return; /* nothing of SELF_INFO's changed */
    case MESH_MESHCORE_CMD_SET_DEVICE_PIN:
        if (len >= 5U) {
            meshcore->device.ble_pin = mesh_meshcore_u32_at(frame + 1);
        }
        return; /* DEVICE_INFO's, not SELF_INFO's */
    default:
        return;
    }
    /* And onto what the screens read, so the save shows even if the read-back never lands. */
    mesh_meshcore_store_self(meshcore);
    mesh_meshcore_store_settings(meshcore);
}

/* The record just read back for a favourite asked for: written whole, one bit changed. */
static void mesh_meshcore_write_favorite(struct mesh_meshcore *meshcore,
                                         const struct mesh_meshcore_contact *record,
                                         uint8_t intent) {
    struct mesh_meshcore_contact contact = *record;
    if (intent == MESH_MESHCORE_FAVORITE_SET) {
        contact.flags |= MESH_MESHCORE_CONTACT_FAVORITE;
    } else {
        contact.flags &= (uint8_t)~MESH_MESHCORE_CONTACT_FAVORITE;
    }
    /* At the head, where its lookup stood: a command accepted after the lookup - a removal,
       say - still runs after the write, rather than the write undoing it. The lookup has just
       been popped, so its slot is free, and nothing has been sent since. */
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    const int len = mesh_meshcore_encode_contact(&contact, frame, sizeof frame);
    if (len < 0 || meshcore->queue_count >= MESH_MESHCORE_QUEUE_LEN) {
        return;
    }
    meshcore->queue_head =
        (meshcore->queue_head + MESH_MESHCORE_QUEUE_LEN - 1U) % MESH_MESHCORE_QUEUE_LEN;
    struct mesh_meshcore_request *request = &meshcore->queue[meshcore->queue_head];
    memcpy(request->frame, frame, (size_t)len);
    request->len = (uint8_t)len;
    request->packet_id = 0U;
    request->favorite = MESH_MESHCORE_FAVORITE_NONE;
    request->never_heard = false;
    meshcore->queue_count += 1U;
}

/* The answer to the command at the head of the queue. */
static void mesh_meshcore_on_reply(struct mesh_meshcore *meshcore, const uint8_t *frame,
                                   size_t len) {
    const uint8_t cmd = mesh_meshcore_head_cmd(meshcore);
    struct mesh_meshcore_request *request = mesh_meshcore_head(meshcore);
    const uint32_t packet_id = request != NULL && meshcore->awaiting ? request->packet_id : 0U;
    const uint8_t code = frame[0];
    /* A favourite's record, written back once its lookup has left the queue - so the slot
       the lookup held is there for the write. */
    struct mesh_meshcore_contact favorite_record;
    uint8_t favorite_intent = MESH_MESHCORE_FAVORITE_NONE;

    /* The contact list is several frames to one command; only its end closes it. */
    if (code == MESH_MESHCORE_RESP_CONTACTS_START) {
        return;
    }
    if (code == MESH_MESHCORE_RESP_CONTACT) {
        struct mesh_meshcore_contact contact;
        if (mesh_meshcore_decode_contact(frame, len, &contact) == 0) {
            mesh_meshcore_store_contact(meshcore, &contact, cmd == MESH_MESHCORE_CMD_GET_CONTACTS,
                                        false);
            if (cmd == MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY && meshcore->awaiting &&
                request->favorite != MESH_MESHCORE_FAVORITE_NONE &&
                memcmp(contact.public_key, request->frame + 1, MESH_MESHCORE_PUBKEY_LEN) == 0) {
                favorite_record = contact;
                favorite_intent = request->favorite;
            }
        }
        if (cmd != MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY) {
            return;
        }
    }

    if (!meshcore->awaiting) {
        inkwell_log_debug("meshcore", "Reply %u with nothing outstanding", (unsigned)code);
        return;
    }
    mesh_meshcore_pop(meshcore);
    meshcore->timeouts = 0U;
    if (favorite_intent != MESH_MESHCORE_FAVORITE_NONE) {
        mesh_meshcore_write_favorite(meshcore, &favorite_record, favorite_intent);
    }

    switch (code) {
    case MESH_MESHCORE_RESP_DEVICE_INFO:
        if (mesh_meshcore_decode_device_info(frame, len, &meshcore->device) == 0) {
            meshcore->has_device = true;
            inkwell_log_info("meshcore", "%s, firmware %s (%u), %u contacts, %u channels",
                             meshcore->device.model, meshcore->device.version,
                             (unsigned)meshcore->device.firmware_version,
                             (unsigned)meshcore->device.max_contacts,
                             (unsigned)meshcore->device.max_channels);
        }
        break;
    case MESH_MESHCORE_RESP_SELF_INFO:
        if (mesh_meshcore_decode_self_info(frame, len, &meshcore->self) == 0) {
            meshcore->has_self = true;
            mesh_meshcore_store_self(meshcore);
            mesh_meshcore_store_settings(meshcore);
            inkwell_log_info("meshcore", "Radio \"%s\" is 0x%08x", meshcore->self.name,
                             meshcore->self_node);
            if (meshcore->phase == MESH_MESHCORE_HANDSHAKE) {
                mesh_meshcore_after_self(meshcore);
            }
        }
        break;
    case MESH_MESHCORE_RESP_END_OF_CONTACTS:
        if (len >= 5U) {
            const uint32_t since = (uint32_t)frame[1] | ((uint32_t)frame[2] << 8U) |
                                   ((uint32_t)frame[3] << 16U) | ((uint32_t)frame[4] << 24U);
            if (since > meshcore->contacts_since) {
                meshcore->contacts_since = since;
            }
        }
        if (meshcore->phase == MESH_MESHCORE_CONTACTS) {
            meshcore->phase = MESH_MESHCORE_CHANNELS;
            mesh_meshcore_request_channel(meshcore, 0U);
        }
        break;
    case MESH_MESHCORE_RESP_CHANNEL_INFO: {
        struct mesh_meshcore_channel channel;
        if (mesh_meshcore_decode_channel(frame, len, &channel) == 0) {
            mesh_meshcore_store_channel(meshcore, &channel);
        }
        if (meshcore->phase == MESH_MESHCORE_CHANNELS) {
            const uint8_t next = (uint8_t)(meshcore->channel_probe + 1U);
            if (next < mesh_meshcore_channel_limit(meshcore)) {
                mesh_meshcore_request_channel(meshcore, next);
            } else {
                mesh_meshcore_ready_now(meshcore);
            }
        }
        break;
    }
    case MESH_MESHCORE_RESP_CONTACT_MSG_RECV:
    case MESH_MESHCORE_RESP_CHANNEL_MSG_RECV:
    case MESH_MESHCORE_RESP_CONTACT_MSG_RECV_V3:
    case MESH_MESHCORE_RESP_CHANNEL_MSG_RECV_V3: {
        struct mesh_meshcore_message message;
        if (mesh_meshcore_decode_message(frame, len, &message) == 0) {
            /* A repeater's reply to a command: the answer that settles it, and a message in
               the conversation the command was typed into. */
            if (message.txt_type == MESH_MESHCORE_TXT_CLI_DATA && !message.channel) {
                mesh_meshcore_command_answered(meshcore, message.sender_prefix);
            }
            mesh_meshcore_store_message(meshcore, &message);
        }
        mesh_meshcore_request_sync(meshcore);
        break;
    }
    case MESH_MESHCORE_RESP_CHANNEL_DATA_RECV:
        mesh_meshcore_request_sync(meshcore); /* a datagram, not text; keep draining */
        break;
    case MESH_MESHCORE_RESP_NO_MORE_MESSAGES:
        break;
    case MESH_MESHCORE_RESP_SENT: {
        struct mesh_meshcore_sent sent;
        /* A request to another node is on its way: its answer is due within the firmware's
           estimate, and until then a second one would orphan it. */
        if (mesh_meshcore_is_remote(cmd) && cmd == meshcore->request_cmd &&
            mesh_meshcore_decode_sent(frame, len, &sent) == 0) {
            const uint32_t wait = sent.timeout_ms > 0U ? sent.timeout_ms : 10000U;
            meshcore->request_until_ms = inkwell_time_monotonic_ms() + (uint64_t)wait + 2000U;
            /* The one answer that names no node: it carries this tag instead. */
            if (cmd == MESH_MESHCORE_CMD_SEND_BINARY_REQ) {
                meshcore->request_tag = sent.expected_ack;
            }
        }
        struct mesh_meshcore_pending *pending = mesh_meshcore_pending_for(meshcore, packet_id);
        if (pending != NULL && mesh_meshcore_decode_sent(frame, len, &sent) == 0) {
            meshcore->now_ms = inkwell_time_monotonic_ms();
            pending->expected_ack = sent.expected_ack;
            /* The firmware's estimate, with room for the ack to come back the same way. A
               command's answer is a whole message rather than an ack: the repeater waits before
               it replies, and the reply reaches us only through the radio's message queue. */
            const uint32_t wait = sent.timeout_ms > 0U ? sent.timeout_ms : 10000U;
            pending->deadline_ms =
                meshcore->now_ms + (uint64_t)wait + (pending->command ? 5000U : 2000U);
        }
        break;
    }
    case MESH_MESHCORE_RESP_OK:
        /* A channel message is sent once and never acknowledged; OK is all it will get. */
        if (cmd == MESH_MESHCORE_CMD_SEND_CHANNEL_TXT_MSG) {
            mesh_meshcore_mark(meshcore, packet_id, MESH_MESSAGE_ACK_NONE);
        } else if (mesh_meshcore_is_settings_write(cmd)) {
            if (request != NULL) {
                mesh_meshcore_apply_write(meshcore, request->frame, request->len);
            }
            mesh_meshcore_settle_write(meshcore, 0);
        } else if (cmd == MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT && request != NULL &&
                   (size_t)request->len + 4U <= MESH_MESHCORE_MAX_FRAME) {
            /* Only now is it on the radio's list, and so a node a direct message can reach, and
               only now is a favourite changed. The record sent is stored as the radio now holds
               it, which also brings the node back if an advert pushed it off the roster while
               the add was waiting. */
            uint8_t record[MESH_MESHCORE_MAX_FRAME];
            memset(record, 0, sizeof record);
            memcpy(record, request->frame, request->len);
            record[0] = MESH_MESHCORE_RESP_CONTACT;
            struct mesh_meshcore_contact contact;
            const struct mesh_node_summary *was = NULL;
            uint32_t id = 0U;
            bool clash = false;
            if (mesh_meshcore_decode_contact(record, (size_t)request->len + 4U, &contact) == 0) {
                id = mesh_meshcore_node_id(contact.public_key, MESH_MESHCORE_PUBKEY_LEN);
                was = mesh_meshcore_roster_node(meshcore, id);
                /* A different key under the same four bytes - an advert heard while this add
                   waited - keeps its row: storing this one there would make it another node. */
                clash = was != NULL && (was->public_key_len != MESH_MESHCORE_PUBKEY_LEN ||
                                        memcmp(was->public_key, contact.public_key,
                                               MESH_MESHCORE_PUBKEY_LEN) != 0);
                if (clash) {
                    inkwell_log_warn("meshcore",
                                     "Contact 0x%08x shares its number with another node; "
                                     "not listed until the next contact sync",
                                     id);
                }
            }
            if (!clash && id != 0U) {
                const bool favorite = (contact.flags & MESH_MESHCORE_CONTACT_FAVORITE) != 0U;
                if (was == NULL || !was->in_nodedb) {
                    inkwell_log_info("meshcore", "Added contact 0x%08x", id);
                } else if (was->is_favorite != favorite) {
                    inkwell_log_info("meshcore", "%s contact 0x%08x",
                                     favorite ? "Pinned" : "Unpinned", id);
                }
                /* The sender's stamp is its own clock, not when we heard it. A node still on
                   the roster keeps its heard time; one brought back is heard as of now, the
                   radio's lastmod for it too, so it keeps its place among the recently heard
                   rather than sinking to the bottom of a full roster. A contact from a link
                   carries no stamp, and was never heard at all. */
                const bool heard = !request->never_heard;
                contact.last_advert = 0U;
                contact.lastmod = was == NULL && heard ? mesh_meshcore_clock_now(meshcore) : 0U;
                /* The user's add, from a link or from a heard advert: never a discovery. A heard
                   node was already counted when its advert arrived. */
                mesh_meshcore_store_contact(meshcore, &contact, false, true);
            }
        } else if (cmd == MESH_MESHCORE_CMD_REMOVE_CONTACT && request != NULL &&
                   request->len == 1U + MESH_MESHCORE_PUBKEY_LEN) {
            /* Only now: a refused or unanswered removal leaves the contact on the radio, and
               so on the list. */
            const uint32_t id =
                mesh_meshcore_find_prefix(meshcore, request->frame + 1, MESH_MESHCORE_PUBKEY_LEN);
            if (id != 0U && mesh_session_model_drop_node(meshcore->model, id) == 0) {
                inkwell_log_info("meshcore", "Removed contact 0x%08x", id);
            }
        }
        break;
    case MESH_MESHCORE_RESP_CURR_TIME:
        if (len >= 5U) {
            meshcore->radio_clock = (uint32_t)frame[1] | ((uint32_t)frame[2] << 8U) |
                                    ((uint32_t)frame[3] << 16U) | ((uint32_t)frame[4] << 24U);
            meshcore->radio_clock_at_ms = inkwell_time_monotonic_ms();
        }
        break;
    case MESH_MESHCORE_RESP_BATT_AND_STORAGE:
        if (len >= 3U) {
            meshcore->battery_mv = (uint16_t)(frame[1] | (frame[2] << 8U));
            meshcore->battery_valid = true;
        }
        break;
    case MESH_MESHCORE_RESP_ERR:
    case MESH_MESHCORE_RESP_DISABLED: {
        const unsigned error = len >= 2U ? frame[1] : 0U;
        inkwell_log_info("meshcore", "Command %u refused (%u, error %u)", (unsigned)cmd,
                         (unsigned)code, error);
        if (mesh_meshcore_is_settings_write(cmd)) {
            /* A refusal with no code still has to count as one. */
            mesh_meshcore_settle_write(meshcore, error != 0U ? (int32_t)error : -1);
        }
        struct mesh_meshcore_pending *pending = mesh_meshcore_pending_for(meshcore, packet_id);
        if (pending != NULL) {
            mesh_meshcore_pending_done(meshcore, pending, MESH_MESSAGE_ACK_FAILED);
        } else {
            mesh_meshcore_mark(meshcore, packet_id, MESH_MESSAGE_ACK_FAILED);
        }
        /* A walk step refused ends the walk where it stands. */
        if (cmd == MESH_MESHCORE_CMD_GET_CHANNEL && meshcore->phase == MESH_MESHCORE_CHANNELS) {
            mesh_meshcore_ready_now(meshcore);
        } else if (cmd == MESH_MESHCORE_CMD_GET_CONTACTS &&
                   meshcore->phase == MESH_MESHCORE_CONTACTS) {
            meshcore->phase = MESH_MESHCORE_CHANNELS;
            mesh_meshcore_request_channel(meshcore, 0U);
        }
        break;
    }
    default:
        inkwell_log_debug("meshcore", "Reply %u to command %u ignored", (unsigned)code,
                          (unsigned)cmd);
        break;
    }
    /* A request to another node whose answer was not a SENT that decoded - a refusal, or a frame
       too short to read - has no deadline for the tick to expire, and was not asked. One answered
       ahead of its SENT has already ended, and is not here. */
    if (mesh_meshcore_is_remote(cmd) && cmd == meshcore->request_cmd &&
        meshcore->request_until_ms == 0U) {
        mesh_meshcore_request_ended(meshcore, MESH_MESHCORE_ANSWER_UNSENT);
    }
    /* A direct message whose answer was not a SENT that decoded has nothing to wait for: no ack
       was named, so no deadline was set, and the tick only retires pending sends that have one.
       Left alone it would sit PENDING for good and hold one of the slots. */
    if (cmd == MESH_MESHCORE_CMD_SEND_TXT_MSG) {
        struct mesh_meshcore_pending *pending = mesh_meshcore_pending_for(meshcore, packet_id);
        if (pending != NULL && pending->deadline_ms == 0U) {
            inkwell_log_info("meshcore", "Message %u: reply %u named no ack", packet_id,
                             (unsigned)code);
            mesh_meshcore_pending_done(meshcore, pending, MESH_MESSAGE_ACK_FAILED);
        }
    }
    mesh_meshcore_pump(meshcore);
}

/* -------------------------------------------------------------------------- protocol ops */

/* The model's own send path while MeshCore holds the link. The session is Meshtastic's, and
   anything that reaches it - a verb a screen forgot to gate - would otherwise be a protobuf
   written at a radio that does not speak them. */
static int mesh_meshcore_refuse(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id) {
    (void)ctx;
    (void)frame;
    (void)len;
    (void)frame_id;
    return -EPROTONOSUPPORT;
}

static void mesh_meshcore_attach(void *self, mesh_protocol_send_fn send, void *send_ctx) {
    struct mesh_meshcore *meshcore = self;
    meshcore->send = send;
    meshcore->send_ctx = send_ctx;
    mesh_session_attach(meshcore->model, mesh_meshcore_refuse, NULL);
}

static void mesh_meshcore_detach(void *self) {
    struct mesh_meshcore *meshcore = self;
    for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        if (meshcore->pending[i].packet_id != 0U) {
            mesh_meshcore_pending_done(meshcore, &meshcore->pending[i], MESH_MESSAGE_ACK_FAILED);
        }
    }
    for (size_t i = 0; i < meshcore->queue_count; ++i) {
        mesh_meshcore_mark(
            meshcore,
            meshcore->queue[(meshcore->queue_head + i) % MESH_MESHCORE_QUEUE_LEN].packet_id,
            MESH_MESSAGE_ACK_FAILED);
    }
    /* A save the link took with it did not land; say so rather than leave it pending. */
    while (meshcore->writes_outstanding > 0U) {
        mesh_meshcore_settle_write(meshcore, MESH_RADIO_SETTINGS_WRITE_TIMEOUT);
    }
    /* And a request to another node: its answer can no longer reach us. Asked, it went
       unanswered; still queued, it was never sent. */
    mesh_meshcore_request_ended(meshcore, meshcore->request_until_ms != 0U
                                              ? MESH_MESHCORE_ANSWER_SILENT
                                              : MESH_MESHCORE_ANSWER_UNSENT);
    inkwell_wipe(meshcore->queue, sizeof meshcore->queue);
    meshcore->send = NULL;
    meshcore->send_ctx = NULL;
    meshcore->queue_head = 0U;
    meshcore->queue_count = 0U;
    meshcore->awaiting = false;
    meshcore->timeouts = 0U;
    meshcore->phase = MESH_MESHCORE_IDLE;
    meshcore->battery_valid = false;
    mesh_session_detach(meshcore->model);
}

static int mesh_meshcore_begin(void *self) {
    struct mesh_meshcore *meshcore = self;
    if (meshcore->send == NULL) {
        return -ENOTCONN;
    }
    mesh_session_model_sync_begin(meshcore->model);
    meshcore->phase = MESH_MESHCORE_HANDSHAKE;
    meshcore->has_self = false;
    meshcore->has_device = false;
    /* A fresh connection asks for every contact: the model may hold nodes from another radio's
       list, and "since" is only meaningful against the list it came from. */
    meshcore->contacts_since = 0U;
    /* And the adverts kept for adding are this connection's: one from before may be older than
       what the sender now stamps, and would hold its next advert back as a replay. */
    memset(meshcore->heard_age, 0, sizeof meshcore->heard_age);
    /* And no channel slot is editable until this walk has read it again: the table outlives
       the link, and a slot left over from the last one - a walk refused before reaching it -
       would be written back over whatever the radio holds now. */
    struct mesh_radio_settings *settings = mesh_session_model_settings(meshcore->model);
    if (settings != NULL) {
        memset(settings->has_channel, 0, sizeof settings->has_channel);
        memset(settings->channels, 0, sizeof settings->channels);
    }

    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    int result = mesh_meshcore_enqueue(
        meshcore, frame,
        mesh_meshcore_encode_device_query(MESH_MESHCORE_APP_VERSION, frame, sizeof frame), 0U);
    if (result < 0) {
        return result;
    }
    return mesh_meshcore_enqueue(
        meshcore, frame,
        mesh_meshcore_encode_app_start(MESH_MESHCORE_APP_NAME, frame, sizeof frame), 0U);
}

static void mesh_meshcore_receive(void *self, const uint8_t *frame, size_t len) {
    struct mesh_meshcore *meshcore = self;
    if (frame == NULL || len == 0U) {
        return;
    }
    if (frame[0] >= 0x80U) {
        mesh_meshcore_on_push(meshcore, frame, len);
    } else {
        mesh_meshcore_on_reply(meshcore, frame, len);
    }
    mesh_meshcore_pump(meshcore);
}

static void mesh_meshcore_frame_failed(void *self, uint32_t frame_id) {
    struct mesh_meshcore *meshcore = self;
    struct mesh_meshcore_pending *pending = mesh_meshcore_pending_for(meshcore, frame_id);
    if (pending != NULL) {
        mesh_meshcore_pending_done(meshcore, pending, MESH_MESSAGE_ACK_FAILED);
    } else {
        mesh_meshcore_mark(meshcore, frame_id, MESH_MESSAGE_ACK_FAILED);
    }
}

static void mesh_meshcore_tick(void *self, uint64_t now_ms) {
    struct mesh_meshcore *meshcore = self;
    meshcore->now_ms = now_ms;
    if (meshcore->awaiting && now_ms >= meshcore->awaiting_since_ms &&
        now_ms - meshcore->awaiting_since_ms >= MESH_MESHCORE_REPLY_TIMEOUT_MS) {
        struct mesh_meshcore_request *request = mesh_meshcore_head(meshcore);
        const uint8_t cmd = request->frame[0];
        const uint32_t packet_id = request->packet_id;
        mesh_meshcore_pop(meshcore);
        /* A reboot is never answered: the radio is gone before it could be. Where the link
           outlives the restart - a USB-serial bridge keeps the port open while the ESP32 behind
           it resets - nothing else would notice, so the conversation starts over by itself. */
        if (cmd == MESH_MESHCORE_CMD_REBOOT) {
            inkwell_log_info("meshcore", "Radio rebooted; syncing again");
            const int result = mesh_meshcore_begin(meshcore);
            if (result < 0) {
                /* Nothing is on its way to time out, so nothing would ever notice: call the
                   link silent now and let it be dropped and reconnected. */
                inkwell_log_warn("meshcore", "Handshake after reboot not sent (%d)", result);
                meshcore->timeouts = 2U;
            }
            return;
        }
        inkwell_log_warn("meshcore", "Command %u unanswered after %u ms", (unsigned)cmd,
                         (unsigned)MESH_MESHCORE_REPLY_TIMEOUT_MS);
        if (meshcore->timeouts < UINT8_MAX) {
            meshcore->timeouts += 1U;
        }
        /* A handshake step given up on leaves a sync that can never finish - a DEVICE_INFO with
           no SELF_INFO after it is a radio half known - so the link is called silent now, to be
           dropped and asked again, rather than waiting on a second timeout nothing will send. */
        if (meshcore->phase == MESH_MESHCORE_HANDSHAKE) {
            meshcore->timeouts = 2U;
        }
        if (mesh_meshcore_is_settings_write(cmd)) {
            mesh_meshcore_settle_write(meshcore, MESH_RADIO_SETTINGS_WRITE_TIMEOUT);
        }
        if (mesh_meshcore_is_remote(cmd) && cmd == meshcore->request_cmd) {
            mesh_meshcore_request_ended(meshcore, MESH_MESHCORE_ANSWER_UNSENT);
        }
        struct mesh_meshcore_pending *pending = mesh_meshcore_pending_for(meshcore, packet_id);
        if (pending != NULL) {
            mesh_meshcore_pending_done(meshcore, pending, MESH_MESSAGE_ACK_FAILED);
        } else {
            mesh_meshcore_mark(meshcore, packet_id, MESH_MESSAGE_ACK_FAILED);
        }
        mesh_meshcore_pump(meshcore);
    }
    for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        struct mesh_meshcore_pending *pending = &meshcore->pending[i];
        if (pending->packet_id != 0U && pending->deadline_ms != 0U &&
            now_ms >= pending->deadline_ms) {
            mesh_meshcore_retry(meshcore, pending);
        }
    }
    mesh_meshcore_request_expire(meshcore, now_ms);
}

static bool mesh_meshcore_silent(const void *self) {
    const struct mesh_meshcore *meshcore = self;
    return meshcore->send != NULL && meshcore->timeouts >= 2U;
}

static const struct mesh_protocol_ops k_meshcore_ops = {
    .name = "meshcore",
    .stream_framing = &mesh_stream_framing_meshcore,
    .ble_profile = &mesh_ble_profile_meshcore,
    .attach = mesh_meshcore_attach,
    .detach = mesh_meshcore_detach,
    .begin = mesh_meshcore_begin,
    .receive = mesh_meshcore_receive,
    .frame_failed = mesh_meshcore_frame_failed,
    .tick = mesh_meshcore_tick,
    .silent = mesh_meshcore_silent,
    .keepalive = NULL,
};

/* ------------------------------------------------------------------------------- public */

void mesh_meshcore_init(struct mesh_meshcore *meshcore, struct mesh_session *model) {
    if (meshcore == NULL) {
        return;
    }
    memset(meshcore, 0, sizeof *meshcore);
    meshcore->model = model;
}

struct mesh_protocol mesh_meshcore_protocol(struct mesh_meshcore *meshcore) {
    if (meshcore == NULL || meshcore->model == NULL) {
        return (struct mesh_protocol){NULL, NULL};
    }
    return (struct mesh_protocol){&k_meshcore_ops, meshcore};
}

bool mesh_meshcore_ready(const struct mesh_meshcore *meshcore) {
    return meshcore != NULL && meshcore->send != NULL && meshcore->phase == MESH_MESHCORE_READY;
}

/*
 * What a message we send says the time is. The wall clock when it is credible; otherwise the
 * radio's, as read at connect and advanced since; otherwise 0, which a recipient reads as "no
 * date" - never our uptime, which it would read as a date in 1970. Strictly increasing whenever
 * there is a clock at all, because the timestamp is part of the encrypted payload and a repeat
 * of the same text in the same second would be dropped by the mesh as a duplicate.
 */
static uint32_t mesh_meshcore_timestamp(struct mesh_meshcore *meshcore) {
    uint32_t now = mesh_meshcore_clock_now(meshcore);
    if (now == 0U) {
        return 0U;
    }
    if (now <= meshcore->last_timestamp) {
        now = meshcore->last_timestamp + 1U;
    }
    meshcore->last_timestamp = now;
    return now;
}

size_t mesh_meshcore_text_max(const struct mesh_meshcore *meshcore, uint32_t dest) {
    if (dest != MESH_MESSAGE_BROADCAST_ADDR) {
        return MESH_MESHCORE_TEXT_MAX;
    }
    const size_t name = (meshcore != NULL && meshcore->has_self) ? strlen(meshcore->self.name)
                                                                 : MESH_MESHCORE_NAME_LEN;
    const size_t prefix = name + 2U; /* ": " */
    return prefix < MESH_MESHCORE_TEXT_MAX ? MESH_MESHCORE_TEXT_MAX - prefix : 0U;
}

int mesh_meshcore_send_text(struct mesh_meshcore *meshcore, uint32_t dest, uint8_t channel,
                            const char *text, uint32_t *out_packet_id) {
    if (meshcore == NULL || text == NULL || text[0] == '\0') {
        return -EINVAL;
    }
    if (meshcore->send == NULL) {
        return -ENOTCONN;
    }
    if (strlen(text) > mesh_meshcore_text_max(meshcore, dest)) {
        return -EMSGSIZE;
    }
    /* A direct message's timestamp is part of what the recipient's ack is computed over, so
       every retry repeats this one. */
    const uint32_t timestamp = mesh_meshcore_timestamp(meshcore);

    struct mesh_message message;
    memset(&message, 0, sizeof message);
    message.packet_id = mesh_session_next_packet_id(meshcore->model);
    message.from = meshcore->self_node;
    message.to = dest;
    message.channel = channel;
    message.rx_time = inkwell_time_wall_credible_s();
    message.direction = MESH_MESSAGE_OUTBOUND;
    message.kind = MESH_MESSAGE_KIND_TEXT;
    inkwell_text_sanitise_str(text, message.text, sizeof message.text);

    int result;
    if (dest == MESH_MESSAGE_BROADCAST_ADDR) {
        uint8_t frame[MESH_MESHCORE_MAX_FRAME];
        const int len =
            mesh_meshcore_encode_channel_text(channel, timestamp, text, frame, sizeof frame);
        if (len < 0) {
            return len;
        }
        message.ack = MESH_MESSAGE_ACK_PENDING;
        (void)mesh_session_model_log_message(meshcore->model, &message);
        result = mesh_meshcore_enqueue(meshcore, frame, len, message.packet_id);
    } else {
        const struct mesh_node_summary *node = mesh_meshcore_roster_node(meshcore, dest);
        if (node == NULL || node->public_key_len != MESH_MESHCORE_PUBKEY_LEN) {
            return -ENOENT;
        }
        struct mesh_meshcore_pending *pending = NULL;
        for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
            if (meshcore->pending[i].packet_id == 0U) {
                pending = &meshcore->pending[i];
                break;
            }
        }
        if (pending == NULL) {
            return -ENOBUFS;
        }
        memset(pending, 0, sizeof *pending);
        pending->packet_id = message.packet_id;
        pending->timestamp = timestamp;
        pending->command = mesh_meshcore_is_command_peer(meshcore, dest);
        pending->sequence = ++meshcore->pending_sequence;
        memcpy(pending->key, node->public_key, MESH_MESHCORE_PUBKEY_LEN);
        snprintf(pending->text, sizeof pending->text, "%s", text);
        message.ack = MESH_MESSAGE_ACK_PENDING;
        message.pki_encrypted = true;
        (void)mesh_session_model_log_message(meshcore->model, &message);
        result = mesh_meshcore_send_attempt(meshcore, pending);
        if (result < 0) {
            memset(pending, 0, sizeof *pending);
        }
    }
    if (result < 0) {
        mesh_meshcore_mark(meshcore, message.packet_id, MESH_MESSAGE_ACK_FAILED);
        return result;
    }
    if (out_packet_id != NULL) {
        *out_packet_id = message.packet_id;
    }
    return 0;
}

int mesh_meshcore_send_advert(struct mesh_meshcore *meshcore, bool flood) {
    if (meshcore == NULL) {
        return -EINVAL;
    }
    uint8_t frame[2];
    return mesh_meshcore_enqueue(meshcore, frame,
                                 mesh_meshcore_encode_byte(MESH_MESHCORE_CMD_SEND_SELF_ADVERT,
                                                           flood ? 1U : 0U, frame, sizeof frame),
                                 0U);
}

int mesh_meshcore_remove_contact(struct mesh_meshcore *meshcore, uint32_t node_id) {
    if (meshcore == NULL || node_id == 0U || node_id == meshcore->self_node) {
        return -EINVAL;
    }
    /* Not until the handshake is through: the roster outlives a reconnect, so before then it
       can be another radio's list, and this would delete that radio's contact from this one. */
    if (!mesh_meshcore_ready(meshcore)) {
        return -ENOTCONN;
    }
    const struct mesh_node_summary *node = mesh_meshcore_roster_node(meshcore, node_id);
    /* A heard node the radio never added, and a sender known only by its key's prefix, are
       not contacts; there is nothing on the radio to remove. */
    if (node == NULL || node->public_key_len != MESH_MESHCORE_PUBKEY_LEN || !node->in_nodedb) {
        return -ENOENT;
    }
    uint8_t frame[1U + MESH_MESHCORE_PUBKEY_LEN];
    const int result =
        mesh_meshcore_enqueue(meshcore, frame,
                              mesh_meshcore_encode_key(MESH_MESHCORE_CMD_REMOVE_CONTACT,
                                                       node->public_key, frame, sizeof frame),
                              0U);
    return result < 0 ? result : 1;
}

/* The advert type a roster role came from: mesh_meshcore_role() the other way. */
static uint8_t mesh_meshcore_adv_type(uint32_t role) {
    switch (role) {
    case MESH_MESHCORE_DEVICE_ROLE_REPEATER:
        return MESH_MESHCORE_ADV_REPEATER;
    case MESH_MESHCORE_DEVICE_ROLE_SENSOR:
        return MESH_MESHCORE_ADV_SENSOR;
    case MESH_MESHCORE_DEVICE_ROLE_CLIENT_BASE:
        return MESH_MESHCORE_ADV_ROOM;
    default:
        return MESH_MESHCORE_ADV_CHAT;
    }
}

/*
 * Whether an add for `key`'s number is already waiting for its OK - it joins the roster only
 * then, so the roster alone cannot say. 1 for the same key (already asked), -EADDRINUSE for a
 * different key with the same four bytes (a second contact under one number), 0 for none.
 */
static int mesh_meshcore_add_pending(const struct mesh_meshcore *meshcore,
                                     const uint8_t key[MESH_MESHCORE_PUBKEY_LEN]) {
    const uint32_t id = mesh_meshcore_node_id(key, MESH_MESHCORE_PUBKEY_LEN);
    for (size_t i = 0; i < meshcore->queue_count; ++i) {
        const struct mesh_meshcore_request *queued =
            &meshcore->queue[(meshcore->queue_head + i) % MESH_MESHCORE_QUEUE_LEN];
        if (queued->frame[0] != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT ||
            queued->len <= MESH_MESHCORE_PUBKEY_LEN ||
            mesh_meshcore_node_id(queued->frame + 1, MESH_MESHCORE_PUBKEY_LEN) != id) {
            continue;
        }
        return memcmp(queued->frame + 1, key, MESH_MESHCORE_PUBKEY_LEN) == 0 ? 1 : -EADDRINUSE;
    }
    return 0;
}

int mesh_meshcore_add_contact(struct mesh_meshcore *meshcore, uint32_t node_id) {
    if (meshcore == NULL || node_id == 0U || node_id == meshcore->self_node) {
        return -EINVAL;
    }
    /* The roster is the last radio's until the handshake is through, as for a removal. */
    if (!mesh_meshcore_ready(meshcore)) {
        return -ENOTCONN;
    }
    const struct mesh_node_summary *node = mesh_meshcore_roster_node(meshcore, node_id);
    if (node == NULL || node->public_key_len != MESH_MESHCORE_PUBKEY_LEN) {
        return -ENOENT;
    }
    if (node->in_nodedb) {
        return -EEXIST;
    }
    const int pending = mesh_meshcore_add_pending(meshcore, node->public_key);
    if (pending != 0) {
        return pending;
    }
    struct mesh_meshcore_contact contact;
    const struct mesh_meshcore_contact *heard =
        mesh_meshcore_heard_advert(meshcore, node->public_key);
    if (heard != NULL) {
        contact = *heard;
    } else {
        /* An advert from before this run: what the roster kept, and no stamp - a zero lets the
           sender's next advert through, where a guess ahead of its clock would not. */
        memset(&contact, 0, sizeof contact);
        memcpy(contact.public_key, node->public_key, MESH_MESHCORE_PUBKEY_LEN);
        contact.type = mesh_meshcore_adv_type(node->role);
        if (node->has_user) {
            inkwell_str_copy(contact.name, sizeof contact.name, node->long_name);
        }
        if (node->position.valid) {
            contact.latitude_e6 = node->position.latitude_i / 10;
            contact.longitude_e6 = node->position.longitude_i / 10;
        }
    }
    contact.flags = 0U;
    contact.out_path_len = MESH_MESHCORE_PATH_NONE;
    memset(contact.out_path, 0, sizeof contact.out_path);
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    const int result = mesh_meshcore_enqueue(
        meshcore, frame, mesh_meshcore_encode_contact(&contact, frame, sizeof frame), 0U);
    return result < 0 ? result : 1;
}

/*
 * Queues a request to another node: `frame` names it by `key`, and the node's answer - or the
 * radio's deadline for one - ends it. One at a time, since a new one orphans the last: while it
 * is still queued, and then until its answer or deadline.
 */
static int mesh_meshcore_ask(struct mesh_meshcore *meshcore, const uint8_t *frame, size_t len,
                             uint32_t node_id, const uint8_t *key) {
    if (mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_SEND_TELEMETRY_REQ) ||
        mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_SEND_LOGIN) ||
        mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_SEND_STATUS_REQ) ||
        mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ) ||
        mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_SEND_BINARY_REQ)) {
        return -EBUSY;
    }
    const uint64_t now_ms = inkwell_time_monotonic_ms();
    mesh_meshcore_request_expire(meshcore, now_ms);
    if (meshcore->request_until_ms != 0U) {
        return -EBUSY;
    }
    const int result = mesh_meshcore_enqueue(meshcore, frame, (int)len, 0U);
    if (result < 0) {
        return result;
    }
    meshcore->request_cmd = frame[0];
    meshcore->request_node = node_id;
    meshcore->request_tag = 0U;
    memcpy(meshcore->request_prefix, key, MESH_MESHCORE_PREFIX_LEN);
    return 0;
}

/* The contact a request names: the radio looks the key up among its own, and a heard node is
   not one. */
static int mesh_meshcore_ask_whom(struct mesh_meshcore *meshcore, uint32_t node_id,
                                  const struct mesh_node_summary **out) {
    if (meshcore == NULL || node_id == 0U || node_id == meshcore->self_node) {
        return -EINVAL;
    }
    if (!mesh_meshcore_ready(meshcore)) {
        return -ENOTCONN;
    }
    const struct mesh_node_summary *node = mesh_meshcore_roster_node(meshcore, node_id);
    if (node == NULL || !node->in_nodedb || node->public_key_len != MESH_MESHCORE_PUBKEY_LEN) {
        return -ENOENT;
    }
    *out = node;
    return 0;
}

int mesh_meshcore_request_telemetry(struct mesh_meshcore *meshcore, uint32_t node_id) {
    const struct mesh_node_summary *node = NULL;
    const int whom = mesh_meshcore_ask_whom(meshcore, node_id, &node);
    if (whom < 0) {
        return whom;
    }
    uint8_t frame[4U + MESH_MESHCORE_PUBKEY_LEN];
    memset(frame, 0, sizeof frame);
    frame[0] = MESH_MESHCORE_CMD_SEND_TELEMETRY_REQ; /* then three reserved bytes */
    memcpy(frame + 4, node->public_key, MESH_MESHCORE_PUBKEY_LEN);
    return mesh_meshcore_ask(meshcore, frame, sizeof frame, node_id, node->public_key);
}

int mesh_meshcore_request_status(struct mesh_meshcore *meshcore, uint32_t node_id) {
    const struct mesh_node_summary *node = NULL;
    const int whom = mesh_meshcore_ask_whom(meshcore, node_id, &node);
    if (whom < 0) {
        return whom;
    }
    uint8_t frame[1U + MESH_MESHCORE_PUBKEY_LEN];
    frame[0] = MESH_MESHCORE_CMD_SEND_STATUS_REQ;
    memcpy(frame + 1, node->public_key, MESH_MESHCORE_PUBKEY_LEN);
    return mesh_meshcore_ask(meshcore, frame, sizeof frame, node_id, node->public_key);
}

int mesh_meshcore_discover_path(struct mesh_meshcore *meshcore, uint32_t node_id) {
    const struct mesh_node_summary *node = NULL;
    const int whom = mesh_meshcore_ask_whom(meshcore, node_id, &node);
    if (whom < 0) {
        return whom;
    }
    uint8_t frame[2U + MESH_MESHCORE_PUBKEY_LEN];
    frame[0] = MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ;
    frame[1] = 0U; /* reserved; the firmware refuses anything else */
    memcpy(frame + 2, node->public_key, MESH_MESHCORE_PUBKEY_LEN);
    const int result = mesh_meshcore_ask(meshcore, frame, sizeof frame, node_id, node->public_key);
    if (result == 0) {
        struct mesh_traceroute *trace = &meshcore->model->traceroute;
        memset(trace, 0, sizeof *trace);
        trace->state = MESH_TRACEROUTE_PENDING;
        trace->target = node_id;
        trace->sent_ms = inkwell_time_monotonic_ms();
    }
    return result;
}

int mesh_meshcore_request_neighbours(struct mesh_meshcore *meshcore, uint32_t node_id) {
    const struct mesh_node_summary *node = NULL;
    const int whom = mesh_meshcore_ask_whom(meshcore, node_id, &node);
    if (whom < 0) {
        return whom;
    }
    /* Only a repeater keeps a list of what it hears; anything else would not answer. */
    if (mesh_meshcore_adv_type(node->role) != MESH_MESHCORE_ADV_REPEATER) {
        return -EINVAL;
    }
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    const int len = mesh_meshcore_encode_neighbours_req(
        node->public_key, (uint8_t)MESH_MESHCORE_NEIGHBOURS_MAX,
        (uint32_t)inkwell_time_monotonic_ms(), frame, sizeof frame);
    if (len < 0) {
        return len;
    }
    return mesh_meshcore_ask(meshcore, frame, (size_t)len, node_id, node->public_key);
}

bool mesh_meshcore_is_command_peer(const struct mesh_meshcore *meshcore, uint32_t node_id) {
    if (meshcore == NULL || node_id == 0U || node_id == MESH_MESSAGE_BROADCAST_ADDR) {
        return false;
    }
    const struct mesh_node_summary *node = mesh_meshcore_roster_node(meshcore, node_id);
    return node != NULL && mesh_meshcore_adv_type(node->role) == MESH_MESHCORE_ADV_REPEATER;
}

int mesh_meshcore_login(struct mesh_meshcore *meshcore, uint32_t node_id, const char *password) {
    const size_t password_len = password != NULL ? strlen(password) : 0U;
    if (password_len > MESH_MESHCORE_PASSWORD_MAX) {
        return -EINVAL;
    }
    const struct mesh_node_summary *node = NULL;
    const int whom = mesh_meshcore_ask_whom(meshcore, node_id, &node);
    if (whom < 0) {
        return whom;
    }
    /* The key, then the password unterminated: the radio ends it at the frame's length. */
    uint8_t frame[1U + MESH_MESHCORE_PUBKEY_LEN + MESH_MESHCORE_PASSWORD_MAX];
    frame[0] = MESH_MESHCORE_CMD_SEND_LOGIN;
    memcpy(frame + 1, node->public_key, MESH_MESHCORE_PUBKEY_LEN);
    if (password_len > 0U) {
        memcpy(frame + 1U + MESH_MESHCORE_PUBKEY_LEN, password, password_len);
    }
    const int result = mesh_meshcore_ask(
        meshcore, frame, 1U + MESH_MESHCORE_PUBKEY_LEN + password_len, node_id, node->public_key);
    inkwell_wipe(frame, sizeof frame);
    return result;
}

int mesh_meshcore_import_contact(struct mesh_meshcore *meshcore,
                                 const uint8_t key[MESH_MESHCORE_PUBKEY_LEN], const char *name,
                                 uint8_t adv_type) {
    if (meshcore == NULL || key == NULL || adv_type < MESH_MESHCORE_ADV_CHAT ||
        adv_type > MESH_MESHCORE_ADV_SENSOR) {
        return -EINVAL;
    }
    if (!mesh_meshcore_ready(meshcore)) {
        return -ENOTCONN;
    }
    if (meshcore->has_self &&
        memcmp(key, meshcore->self.public_key, MESH_MESHCORE_PUBKEY_LEN) == 0) {
        return -EINVAL;
    }
    const uint32_t known = mesh_meshcore_find_prefix(meshcore, key, MESH_MESHCORE_PUBKEY_LEN);
    const struct mesh_node_summary *node =
        known != 0U ? mesh_meshcore_roster_node(meshcore, known) : NULL;
    if (node != NULL && node->in_nodedb) {
        return -EEXIST;
    }
    /* The roster names a node by its key's first four bytes: a different key with the same
       four - this radio's, or another node's - would be stored over that one. */
    const uint32_t id = mesh_meshcore_node_id(key, MESH_MESHCORE_PUBKEY_LEN);
    if (node == NULL &&
        (id == meshcore->self_node || mesh_meshcore_roster_node(meshcore, id) != NULL)) {
        return -EADDRINUSE;
    }
    const int pending = mesh_meshcore_add_pending(meshcore, key);
    if (pending != 0) {
        return pending;
    }
    struct mesh_meshcore_contact contact;
    memset(&contact, 0, sizeof contact);
    memcpy(contact.public_key, key, MESH_MESHCORE_PUBKEY_LEN);
    contact.type = adv_type;
    contact.out_path_len = MESH_MESHCORE_PATH_NONE;
    if (name != NULL) {
        inkwell_str_copy(contact.name, sizeof contact.name, name);
    }
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    const int result = mesh_meshcore_enqueue_tagged(
        meshcore, frame, mesh_meshcore_encode_contact(&contact, frame, sizeof frame), 0U,
        MESH_MESHCORE_FAVORITE_NONE, node == NULL);
    return result < 0 ? result : 1;
}

int mesh_meshcore_import_card(struct mesh_meshcore *meshcore, const uint8_t *packet, size_t len,
                              const uint8_t key[MESH_MESHCORE_PUBKEY_LEN]) {
    /* The firmware takes a card only past a key and a signature's worth of bytes. */
    if (meshcore == NULL || packet == NULL || key == NULL ||
        len <= 1U + MESH_MESHCORE_PUBKEY_LEN + 64U || len + 1U > MESH_MESHCORE_MAX_FRAME) {
        return -EINVAL;
    }
    if (!mesh_meshcore_ready(meshcore)) {
        return -ENOTCONN;
    }
    if (meshcore->has_self &&
        memcmp(key, meshcore->self.public_key, MESH_MESHCORE_PUBKEY_LEN) == 0) {
        return -EINVAL;
    }
    /* A card for a node already on the roster refreshes it; one whose first four bytes are
       another node's, or this radio's, would land on that entry. */
    const uint32_t id = mesh_meshcore_node_id(key, MESH_MESHCORE_PUBKEY_LEN);
    const uint32_t known = mesh_meshcore_find_prefix(meshcore, key, MESH_MESHCORE_PUBKEY_LEN);
    if (known == 0U &&
        (id == meshcore->self_node || mesh_meshcore_roster_node(meshcore, id) != NULL)) {
        return -EADDRINUSE;
    }
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    frame[0] = MESH_MESHCORE_CMD_IMPORT_CONTACT;
    memcpy(frame + 1, packet, len);
    const int result = mesh_meshcore_enqueue(meshcore, frame, (int)(len + 1U), 0U);
    return result < 0 ? result : 1;
}

int mesh_meshcore_set_favorite(struct mesh_meshcore *meshcore, uint32_t node_id, bool favorite) {
    if (meshcore == NULL || node_id == 0U || node_id == meshcore->self_node) {
        return -EINVAL;
    }
    if (!mesh_meshcore_ready(meshcore)) {
        return -ENOTCONN;
    }
    const struct mesh_node_summary *node = mesh_meshcore_roster_node(meshcore, node_id);
    if (node == NULL || !node->in_nodedb || node->public_key_len != MESH_MESHCORE_PUBKEY_LEN) {
        return -ENOENT;
    }
    if (node->is_favorite == favorite) {
        return 0;
    }
    uint8_t frame[1U + MESH_MESHCORE_PUBKEY_LEN];
    const int result = mesh_meshcore_enqueue_tagged(
        meshcore, frame,
        mesh_meshcore_encode_key(MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY, node->public_key, frame,
                                 sizeof frame),
        0U, favorite ? MESH_MESHCORE_FAVORITE_SET : MESH_MESHCORE_FAVORITE_CLEAR, false);
    return result < 0 ? result : 1;
}

int mesh_meshcore_write_settings(struct mesh_meshcore *meshcore,
                                 const struct mesh_meshcore_settings_write *write) {
    if (meshcore == NULL || write == NULL) {
        return -EINVAL;
    }
    if (meshcore->send == NULL) {
        return -ENOTCONN;
    }
    /* A save is built over SELF_INFO, so none is made while one is on its way: the radio
       parameters are one command, and a save encoded over values a queued read-back is about
       to replace would put the stale ones back. Nor behind a reboot, whose handshake starts
       the write counters over under a save still queued. */
    if (meshcore->writes_outstanding > 0U ||
        mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_APP_START) ||
        mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_REBOOT)) {
        return -EBUSY;
    }
    /* Encoded whole before anything is queued, so a value the codec refuses leaves the radio
       untouched rather than half written. */
    uint8_t frames[7][MESH_MESHCORE_MAX_FRAME];
    int lens[7];
    size_t count = 0U;
    if (write->set_name) {
        lens[count] = mesh_meshcore_encode_name(write->name, frames[count], sizeof frames[count]);
        count += 1U;
    }
    if (write->set_radio) {
        lens[count] = mesh_meshcore_encode_radio_params(write->frequency_khz, write->bandwidth_hz,
                                                        write->spreading_factor, write->coding_rate,
                                                        frames[count], sizeof frames[count]);
        count += 1U;
    }
    if (write->set_tx_power) {
        lens[count] = mesh_meshcore_encode_byte(MESH_MESHCORE_CMD_SET_RADIO_TX_POWER,
                                                (uint8_t)write->tx_power_dbm, frames[count],
                                                sizeof frames[count]);
        count += 1U;
    }
    if (write->set_position) {
        lens[count] = mesh_meshcore_encode_latlon(write->latitude_e6, write->longitude_e6,
                                                  frames[count], sizeof frames[count]);
        count += 1U;
    }
    /* All four together, every one of them the caller's: the firmware reads a shorter frame as
       "leave the rest", but a frame this client sends says what it means. */
    if (write->set_other) {
        const uint8_t other[5] = {MESH_MESHCORE_CMD_SET_OTHER_PARAMS, write->manual_add_contacts,
                                  write->telemetry_modes, write->advert_loc_policy,
                                  write->multi_acks};
        memcpy(frames[count], other, sizeof other);
        lens[count] = (int)sizeof other;
        count += 1U;
    }
    /* The firmware's own bound, refused here rather than answered with ILLEGAL_ARG. */
    if (write->set_pin) {
        if (write->ble_pin != 0U && (write->ble_pin < 100000U || write->ble_pin > 999999U)) {
            return -EINVAL;
        }
        lens[count] = mesh_meshcore_encode_u32(MESH_MESHCORE_CMD_SET_DEVICE_PIN, write->ble_pin,
                                               frames[count], sizeof frames[count]);
        count += 1U;
    }
    /* A slot the radio has, and one the handshake has read: the save starts from what it
       read, and a slot past the walk is one no screen offers. */
    const bool self_info = write->set_name || write->set_radio || write->set_tx_power ||
                           write->set_position || write->set_other;
    if (write->set_channel) {
        if (write->channel_index >= mesh_meshcore_channel_limit(meshcore)) {
            return -EINVAL;
        }
        char name[MESH_MESHCORE_NAME_LEN + 1U];
        memcpy(name, write->channel_name, MESH_MESHCORE_NAME_LEN);
        name[MESH_MESHCORE_NAME_LEN] = '\0';
        lens[count] = mesh_meshcore_encode_set_channel(
            write->channel_index, name, write->channel_secret, frames[count], sizeof frames[count]);
        count += 1U;
    }
    if (count == 0U) {
        return -EINVAL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (lens[i] < 0) {
            return -EINVAL;
        }
    }
    /* The whole save and its read-backs, or none of it. */
    const size_t reads = (self_info ? 1U : 0U) + (write->set_channel ? 1U : 0U);
    if (meshcore->queue_count + count + reads > MESH_MESHCORE_QUEUE_LEN) {
        return -ENOBUFS;
    }
    meshcore->writes_outstanding = (uint8_t)count;
    meshcore->write_error = 0;
    struct mesh_radio_settings *settings = mesh_session_model_settings(meshcore->model);
    if (settings != NULL) {
        settings->writes_sent += 1U;
    }
    const uint32_t failed_before = settings != NULL ? settings->writes_failed : 0U;
    for (size_t i = 0; i < count; ++i) {
        (void)mesh_meshcore_enqueue(meshcore, frames[i], lens[i], 0U);
    }
    /* Every command refused by the link before this returns: the save settled as failed
       already, before a caller could start watching for it, so it is this call's answer. */
    if (settings != NULL && meshcore->writes_outstanding == 0U &&
        settings->writes_failed != failed_before) {
        return settings->last_write_error < 0 ? (int)settings->last_write_error : -EIO;
    }
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    if (self_info) {
        (void)mesh_meshcore_enqueue(
            meshcore, frame,
            mesh_meshcore_encode_app_start(MESH_MESHCORE_APP_NAME, frame, sizeof frame), 0U);
    }
    if (write->set_channel) {
        mesh_meshcore_request_byte(meshcore, MESH_MESHCORE_CMD_GET_CHANNEL, write->channel_index);
    }
    return (int)count;
}

int mesh_meshcore_refresh_settings(struct mesh_meshcore *meshcore) {
    if (meshcore == NULL) {
        return -EINVAL;
    }
    if (meshcore->send == NULL) {
        return -ENOTCONN;
    }
    if (mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_APP_START)) {
        return 0;
    }
    uint8_t frame[MESH_MESHCORE_MAX_FRAME];
    const int result = mesh_meshcore_enqueue(
        meshcore, frame,
        mesh_meshcore_encode_app_start(MESH_MESHCORE_APP_NAME, frame, sizeof frame), 0U);
    return result < 0 ? result : 1;
}

int mesh_meshcore_reboot(struct mesh_meshcore *meshcore) {
    if (meshcore == NULL) {
        return -EINVAL;
    }
    /* One is enough: a second would restart the radio again ahead of the handshake the first
       one's silence starts. */
    if (mesh_meshcore_queued(meshcore, MESH_MESHCORE_CMD_REBOOT)) {
        return 0;
    }
    uint8_t frame[8];
    const int result = mesh_meshcore_enqueue(meshcore, frame,
                                             mesh_meshcore_encode_reboot(frame, sizeof frame), 0U);
    return result < 0 ? result : 1;
}

int mesh_meshcore_import_channel(struct mesh_meshcore *meshcore, const char *name,
                                 const uint8_t secret[MESH_MESHCORE_SECRET_LEN],
                                 uint8_t *out_slot) {
    if (meshcore == NULL || name == NULL || secret == NULL || name[0] == '\0' ||
        strlen(name) >= MESH_MESHCORE_NAME_LEN) {
        return -EINVAL;
    }
    if (!mesh_meshcore_ready(meshcore)) {
        return -ENOTCONN;
    }
    const struct mesh_handshake_status *status = &meshcore->model->handshake;
    const struct mesh_radio_settings *settings = mesh_session_model_settings(meshcore->model);
    if (settings == NULL) {
        return -ENOTCONN;
    }
    /* Only the slots the sync read: one never read cannot be told from a free one. */
    const uint8_t limit = mesh_meshcore_channel_limit(meshcore);
    int free_slot = -1;
    for (size_t slot = 0;
         slot < limit && slot < MESH_RADIO_SETTINGS_MAX_CHANNELS && slot < status->channel_count;
         ++slot) {
        if (!settings->has_channel[slot]) {
            continue;
        }
        const meshtastic_Channel *record = &settings->channels[slot];
        if (record->role == meshtastic_Channel_Role_DISABLED) {
            if (free_slot < 0) {
                free_slot = (int)slot;
            }
            continue;
        }
        /* The same secret under the same name is already this radio's channel. */
        if (record->settings.psk.size == MESH_MESHCORE_SECRET_LEN &&
            memcmp(record->settings.psk.bytes, secret, MESH_MESHCORE_SECRET_LEN) == 0 &&
            strcmp(status->channels[slot].name, name) == 0) {
            if (out_slot != NULL) {
                *out_slot = (uint8_t)slot;
            }
            return -EEXIST;
        }
    }
    if (free_slot < 0) {
        return -ENOSPC;
    }
    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    write.set_channel = true;
    write.channel_index = (uint8_t)free_slot;
    inkwell_str_copy(write.channel_name, sizeof write.channel_name, name);
    memcpy(write.channel_secret, secret, MESH_MESHCORE_SECRET_LEN);
    const int result = mesh_meshcore_write_settings(meshcore, &write);
    inkwell_wipe(write.channel_secret, sizeof write.channel_secret);
    if (result > 0 && out_slot != NULL) {
        *out_slot = (uint8_t)free_slot;
    }
    return result;
}
