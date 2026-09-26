#include "mesh/core/meshcore.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

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
    }
}

static int mesh_meshcore_enqueue(struct mesh_meshcore *meshcore, const uint8_t *frame, int len,
                                 uint32_t packet_id) {
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

static bool mesh_meshcore_queued(const struct mesh_meshcore *meshcore, uint8_t cmd) {
    for (size_t i = 0; i < meshcore->queue_count; ++i) {
        if (meshcore->queue[(meshcore->queue_head + i) % MESH_MESHCORE_QUEUE_LEN].frame[0] == cmd) {
            return true;
        }
    }
    return false;
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

static void mesh_meshcore_store_contact(struct mesh_meshcore *meshcore,
                                        const struct mesh_meshcore_contact *contact, bool synced) {
    const uint32_t id = mesh_meshcore_node_id(contact->public_key, MESH_MESHCORE_PUBKEY_LEN);
    struct mesh_node_summary *node = mesh_session_model_node(meshcore->model, id, synced);
    if (node == NULL) {
        return;
    }
    mesh_meshcore_name_node(node, contact->public_key, contact->name, contact->type);
    node->in_nodedb = true;
    /* lastmod is the radio's clock, which we set; the advert's own stamp is the sender's. */
    const uint32_t heard = contact->lastmod != 0U ? contact->lastmod : contact->last_advert;
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
           cmd == MESH_MESHCORE_CMD_SET_ADVERT_LATLON;
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
    meshcore->writes_outstanding -= 1U;
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
    }
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
            char attributed[MESH_MESSAGE_TEXT_MAX + 1U];
            if (node != NULL && node->has_user) {
                snprintf(attributed, sizeof attributed, "%s: %s", node->long_name, text);
            } else {
                snprintf(attributed, sizeof attributed, "%02x%02x%02x%02x: %s",
                         decoded->author_prefix[0], decoded->author_prefix[1],
                         decoded->author_prefix[2], decoded->author_prefix[3], text);
            }
            snprintf(text, sizeof text, "%s", attributed);
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
        }
    }
    snprintf(message.text, sizeof message.text, "%s", body);
    (void)mesh_message_log_append(&meshcore->model->messages, &message);
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
    const int len = mesh_meshcore_encode_text(pending->key, pending->attempt, pending->timestamp,
                                              pending->text, frame, sizeof frame);
    pending->deadline_ms = 0U;
    return mesh_meshcore_enqueue(meshcore, frame, len, pending->packet_id);
}

static void mesh_meshcore_pending_done(struct mesh_meshcore *meshcore,
                                       struct mesh_meshcore_pending *pending,
                                       enum mesh_message_ack ack) {
    mesh_meshcore_mark(meshcore, pending->packet_id, ack);
    memset(pending, 0, sizeof *pending);
}

/* A direct message whose ack did not come. Try again - the last time with the route reset, so
   the firmware floods it rather than trusting a path that has just failed twice. */
static void mesh_meshcore_retry(struct mesh_meshcore *meshcore,
                                struct mesh_meshcore_pending *pending) {
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

/* ---------------------------------------------------------------------------- receiving */

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
                if (pending->packet_id != 0U && pending->deadline_ms != 0U &&
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
            mesh_meshcore_store_contact(meshcore, &contact, false);
            const uint32_t id = mesh_meshcore_node_id(contact.public_key, MESH_MESHCORE_PUBKEY_LEN);
            struct mesh_node_summary *node = mesh_session_model_node(meshcore->model, id, false);
            if (node != NULL) {
                node->in_nodedb = false;
            }
        }
        break;
    }
    case MESH_MESHCORE_PUSH_CONTACT_DELETED:
        if (len >= 1U + MESH_MESHCORE_PUBKEY_LEN) {
            const uint32_t id = mesh_meshcore_node_id(frame + 1, MESH_MESHCORE_PUBKEY_LEN);
            struct mesh_node_summary *node = mesh_session_model_node(meshcore->model, id, false);
            if (node != NULL) {
                node->in_nodedb = false;
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
static uint32_t mesh_meshcore_u32_at(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

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
    default:
        return;
    }
    /* And onto what the screens read, so the save shows even if the read-back never lands. */
    mesh_meshcore_store_self(meshcore);
    mesh_meshcore_store_settings(meshcore);
}

/* The answer to the command at the head of the queue. */
static void mesh_meshcore_on_reply(struct mesh_meshcore *meshcore, const uint8_t *frame,
                                   size_t len) {
    const uint8_t cmd = mesh_meshcore_head_cmd(meshcore);
    struct mesh_meshcore_request *request = mesh_meshcore_head(meshcore);
    const uint32_t packet_id = request != NULL && meshcore->awaiting ? request->packet_id : 0U;
    const uint8_t code = frame[0];

    /* The contact list is several frames to one command; only its end closes it. */
    if (code == MESH_MESHCORE_RESP_CONTACTS_START) {
        return;
    }
    if (code == MESH_MESHCORE_RESP_CONTACT) {
        struct mesh_meshcore_contact contact;
        if (mesh_meshcore_decode_contact(frame, len, &contact) == 0) {
            mesh_meshcore_store_contact(meshcore, &contact, cmd == MESH_MESHCORE_CMD_GET_CONTACTS);
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
        if (mesh_meshcore_decode_message(frame, len, &message) == 0 &&
            message.txt_type != MESH_MESHCORE_TXT_CLI_DATA) {
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
        struct mesh_meshcore_pending *pending = mesh_meshcore_pending_for(meshcore, packet_id);
        if (pending != NULL && mesh_meshcore_decode_sent(frame, len, &sent) == 0) {
            meshcore->now_ms = inkwell_time_monotonic_ms();
            pending->expected_ack = sent.expected_ack;
            /* The firmware's estimate, with room for the ack to come back the same way. */
            const uint32_t wait = sent.timeout_ms > 0U ? sent.timeout_ms : 10000U;
            pending->deadline_ms = meshcore->now_ms + (uint64_t)wait + 2000U;
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
        if (mesh_meshcore_is_settings_write(cmd)) {
            mesh_meshcore_settle_write(meshcore, MESH_RADIO_SETTINGS_WRITE_TIMEOUT);
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

static uint32_t mesh_meshcore_timestamp(struct mesh_meshcore *meshcore) {
    uint32_t now = inkwell_time_wall_credible_s();
    if (now == 0U && meshcore->radio_clock != 0U) {
        const uint64_t elapsed = inkwell_time_monotonic_ms() - meshcore->radio_clock_at_ms;
        now = meshcore->radio_clock + (uint32_t)(elapsed / 1000U);
    }
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
        (void)mesh_message_log_append(&meshcore->model->messages, &message);
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
        memcpy(pending->key, node->public_key, MESH_MESHCORE_PUBKEY_LEN);
        snprintf(pending->text, sizeof pending->text, "%s", text);
        message.ack = MESH_MESSAGE_ACK_PENDING;
        message.pki_encrypted = true;
        (void)mesh_message_log_append(&meshcore->model->messages, &message);
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
    if (meshcore->send == NULL) {
        return -ENOTCONN;
    }
    const struct mesh_node_summary *node = mesh_meshcore_roster_node(meshcore, node_id);
    if (node == NULL || node->public_key_len != MESH_MESHCORE_PUBKEY_LEN) {
        return -ENOENT;
    }
    uint8_t frame[1U + MESH_MESHCORE_PUBKEY_LEN];
    const int result =
        mesh_meshcore_enqueue(meshcore, frame,
                              mesh_meshcore_encode_key(MESH_MESHCORE_CMD_REMOVE_CONTACT,
                                                       node->public_key, frame, sizeof frame),
                              0U);
    if (result < 0) {
        return result;
    }
    (void)mesh_session_model_drop_node(meshcore->model, node_id);
    inkwell_log_info("meshcore", "Removed contact 0x%08x", node_id);
    return 1;
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
    uint8_t frames[4][MESH_MESHCORE_MAX_FRAME];
    int lens[4];
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
    if (count == 0U) {
        return -EINVAL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (lens[i] < 0) {
            return -EINVAL;
        }
    }
    /* The whole save and its read-back, or none of it. */
    if (meshcore->queue_count + count + 1U > MESH_MESHCORE_QUEUE_LEN) {
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
    (void)mesh_meshcore_enqueue(
        meshcore, frame,
        mesh_meshcore_encode_app_start(MESH_MESHCORE_APP_NAME, frame, sizeof frame), 0U);
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
