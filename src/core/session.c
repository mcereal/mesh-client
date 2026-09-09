#define _POSIX_C_SOURCE 200809L

#include "mesh/core/session.h"

#include "mesh/geo/coords.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/channel.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "meshtastic/telemetry.pb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Our own wall clock as epoch seconds, or 0 when the Brick does not have one worth quoting.
 *
 * The handheld has no RTC battery, so an unconfigured one boots somewhere in 1970 and a
 * timestamp taken from it would date every packet to before Meshtastic existed. Anything
 * earlier than the floor is treated as no clock at all, which every caller already renders as
 * "unknown" rather than as a date.
 */
/* The wall clock when it is credibly one; see mesh_time_wall_credible_s(). The floor used to
   be spelled out here, and it is now shared with the two other places that need to know a
   Brick's 1970 clock is not a date. */
static uint32_t mesh_session_wall_clock(void) { return mesh_time_wall_credible_s(); }

/*
 * Everything the connection that just ended told us about *itself* - what was in flight, what
 * this link's admin session held, what the radio said about its own uptime. What the radio told
 * us about what it *is* survives; mesh_session_forget_radio() below is what drops that.
 *
 * The split is the roster's rule, extended. The roster was already kept across a reconnect
 * because it belongs to the client rather than to the radio, and because the radio's NodeDB
 * holds 80 entries and evicts. But the config, the channel table and MyNodeInfo were wiped on
 * every drop and re-fetched from nothing - and on a 135-node radio the replay that refills them
 * runs about seventeen seconds, of which the first four are the part the client actually needs
 * to work. A link that keeps dying at ten seconds therefore never converged: each attempt threw
 * away the four seconds the last one had earned, blanked the channel list on the way past, and
 * started again. Keeping them means the first attempt that gets four seconds in leaves the
 * client usable, and every attempt after that adds roster rather than starting over.
 *
 * What this costs is one reconnect's worth of staleness if the radio's config changed while we
 * were away. The two ways it can change are both handled: a reboot forgets the radio outright
 * (the sync after one is precisely the sync that must not trust what came before), and a swap to
 * another radio forgets it too. Short of those, the next sync's fragments overwrite each section
 * as they land, seconds in.
 */
static void mesh_session_reset_link_state(struct mesh_session *session) {
    struct mesh_handshake_status *handshake = &session->handshake;
    handshake->request_in_flight = false;
    handshake->request_id = 0U;
    handshake->config_complete = false;
    handshake->config_complete_id = 0U;

    memset(&session->stats, 0, sizeof session->stats);
    memset(&session->traceroute, 0, sizeof session->traceroute);
    /* The Store & Forward router goes the same way, and for a sharper reason than the trace
       does: it is remembered as a node number *and a channel index*, and a channel index only
       names a channel against the table of the radio we are attached to. The history cursor
       goes with it - it indexes a table inside the router, and we are no longer sure we are
       about to talk to the same one. */
    mesh_store_forward_reset(&session->store_forward);
    /* Both describe the radio that is connected right now, so they go the way `stats` does.
       The reboot counter goes with them: it counts restarts of *this* link, and a reader that
       saw it at 2 on the last radio must not read the next one's first reboot as a third. */
    memset(&session->notification, 0, sizeof session->notification);
    memset(&session->queue, 0, sizeof session->queue);
    session->reboot_notices = 0U;
    session->node_cache_warned = false;
    mesh_radio_settings_reset_session(&session->settings);
    session->admin_probe_queued = false;
}

/* Everything the radio told us about what it is. Dropped only when what it is may have changed
   underneath us: a reboot, or a move to a different radio. */
static void mesh_session_forget_radio(struct mesh_session *session) {
    struct mesh_handshake_status *handshake = &session->handshake;
    handshake->has_my_info = false;
    memset(&handshake->my_info, 0, sizeof handshake->my_info);
    handshake->has_config = false;
    memset(&handshake->config, 0, sizeof handshake->config);
    handshake->channel_count = 0U;
    memset(handshake->channels, 0, sizeof handshake->channels);
    mesh_radio_settings_reset(&session->settings);
}

/* Drops the roster outright. Only for a radio swap: another radio is another NodeDB, and its
   idea of who is on the air is not this one's. */
static void mesh_session_clear_nodes(struct mesh_session *session) {
    session->handshake.node_count = 0U;
    memset(session->handshake.nodes, 0, sizeof session->handshake.nodes);
}

void mesh_session_init(struct mesh_session *session) {
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof *session);
    mesh_message_log_reset(&session->messages);
    mesh_waypoint_book_reset(&session->waypoints);
    /*
     * want_config_id is a nonce: the node echoes it back in config_complete_id. A per-process seed
     * keeps a stale completion left in the node's FIFO by a previous session from ending ours
     * early.
     */
    session->next_config_request_id = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
    if (session->next_config_request_id == 0U) {
        session->next_config_request_id = 1U;
    }
    session->next_packet_id = 0U; /* seeded lazily on the first send */
    /* A fresh session knows no radio, so this is the one place that clears both halves. */
    mesh_session_reset_link_state(session);
    mesh_session_forget_radio(session);
}

void mesh_session_attach(struct mesh_session *session, mesh_session_send_fn send, void *ctx) {
    if (session == NULL) {
        return;
    }
    session->send = send;
    session->send_ctx = ctx;
}

void mesh_session_detach(struct mesh_session *session) {
    if (session == NULL) {
        return;
    }
    session->send = NULL;
    session->send_ctx = NULL;
    /* The link ended; the radio on the other side of it did not change. */
    mesh_session_reset_link_state(session);
}

bool mesh_session_attached(const struct mesh_session *session) {
    return session != NULL && session->send != NULL;
}

static int mesh_session_send_raw(struct mesh_session *session, const uint8_t *packet, size_t len,
                                 uint32_t packet_id) {
    if (session == NULL || packet == NULL || len == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }
    if (len > MESH_SESSION_MAX_PACKET) {
        mesh_log_warn("session", "ToRadio packet of %zu bytes exceeds %u byte limit", len,
                      (unsigned)MESH_SESSION_MAX_PACKET);
        return -EMSGSIZE;
    }
    return session->send(session->send_ctx, packet, len, packet_id);
}

int mesh_session_begin_handshake(struct mesh_session *session) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }

    uint32_t request_id = session->next_config_request_id++;
    if (session->next_config_request_id == 0U) {
        session->next_config_request_id = 1U;
    }
    if (request_id == 0U) {
        request_id = session->next_config_request_id++;
        if (session->next_config_request_id == 0U) {
            session->next_config_request_id = 1U;
        }
    }

    /* Only the link's half. The replay this is about to ask for overwrites each section as it
       lands, so blanking them first bought nothing and cost the client every screen it could
       have drawn during the seventeen seconds the replay takes. */
    mesh_session_reset_link_state(session);
    /* A fresh epoch, so nodes the coming replay does not mention can be told apart from the
       ones it does. Zero means "no sync has ever carried this node", so it is never an epoch. */
    if (++session->sync_epoch == 0U) {
        session->sync_epoch = 1U;
    }
    session->handshake.request_in_flight = true;
    session->handshake.request_id = request_id;

    meshtastic_ToRadio request = meshtastic_ToRadio_init_default;
    request.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    request.want_config_id = request_id;

    uint8_t payload[64];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &request)) {
        mesh_log_error("session", "Failed to encode want_config: %s", PB_GET_ERROR(&stream));
        session->handshake.request_in_flight = false;
        return -EIO;
    }

    int result = mesh_session_send_raw(session, payload, stream.bytes_written, 0U);
    if (result < 0) {
        mesh_log_error("session", "Failed to send want_config request: %d", result);
        session->handshake.request_in_flight = false;
        return result;
    }

    mesh_log_info("session", "Requested config sync (request_id=%u)", request_id);
    return 0;
}

static bool mesh_session_node_known(const struct mesh_session *session, uint32_t node_id) {
    const struct mesh_handshake_status *handshake = &session->handshake;
    for (size_t i = 0; i < handshake->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (handshake->nodes[i].node_id == node_id) {
            return true;
        }
    }
    return false;
}

/*
 * The identity Meshtastic's own clients show for a node whose User has never reached them: all
 * three strings are derived from the node number, which is also how the firmware names a radio
 * that has never been given a name. Without it a node heard before its NodeInfo - and there are
 * plenty, since the firmware replays its database once and the mesh keeps moving - draws as a
 * row of dashes with nothing beside it, while the phone in your other hand shows
 * "Meshtastic e54c". Overwritten the moment a real User arrives.
 */
static void mesh_session_default_identity(struct mesh_node_summary *summary) {
    const unsigned suffix = (unsigned)(summary->node_id & 0xffffU);
    snprintf(summary->user_id, sizeof summary->user_id, "!%08x", summary->node_id);
    snprintf(summary->short_name, sizeof summary->short_name, "%04x", suffix);
    snprintf(summary->long_name, sizeof summary->long_name, "Meshtastic %04x", suffix);
    summary->has_user = false;
}

/*
 * The entry to drop when the roster is full. It fills for real now that it outlives the
 * connection, so "full" has to mean "make room" rather than "stop listening": the least useful
 * entry goes, which is a node the radio has already forgotten before one it still carries, and
 * within either group the one heard longest ago. Ourselves and pinned nodes are never victims -
 * a pin is the user saying keep this one - so NULL is possible in theory and means every slot
 * is spoken for.
 */
static struct mesh_node_summary *mesh_session_evict_candidate(struct mesh_session *session) {
    struct mesh_handshake_status *handshake = &session->handshake;
    const uint32_t my_node = handshake->has_my_info ? handshake->my_info.my_node_num : 0U;
    struct mesh_node_summary *victim = NULL;
    for (size_t i = 0; i < handshake->node_count; ++i) {
        struct mesh_node_summary *node = &handshake->nodes[i];
        if (node->is_favorite || (my_node != 0U && node->node_id == my_node)) {
            continue;
        }
        if (victim == NULL) {
            victim = node;
            continue;
        }
        if (victim->in_nodedb != node->in_nodedb) {
            if (!node->in_nodedb) {
                victim = node;
            }
            continue;
        }
        if (node->last_heard < victim->last_heard) {
            victim = node;
        }
    }
    return victim;
}

/* The node's entry in the roster, adding it if it is new. NULL only when every slot holds a
   node worth more than this one. */
static struct mesh_node_summary *mesh_session_node_slot(struct mesh_session *session,
                                                        uint32_t node_id) {
    struct mesh_handshake_status *handshake = &session->handshake;
    if (handshake->node_count > MESH_SESSION_MAX_NODES) {
        handshake->node_count = MESH_SESSION_MAX_NODES;
    }

    for (size_t i = 0; i < handshake->node_count; ++i) {
        if (handshake->nodes[i].node_id == node_id) {
            return &handshake->nodes[i];
        }
    }

    struct mesh_node_summary *summary = NULL;
    if (handshake->node_count >= MESH_SESSION_MAX_NODES) {
        summary = mesh_session_evict_candidate(session);
        if (summary == NULL) {
            if (!session->node_cache_warned) {
                mesh_log_warn("session",
                              "Node roster full (%u) and nothing evictable; 0x%08x "
                              "and any further nodes dropped",
                              (unsigned)MESH_SESSION_MAX_NODES, node_id);
                session->node_cache_warned = true;
            }
            return NULL;
        }
        mesh_log_debug("session", "Roster full; node 0x%08x makes room for 0x%08x",
                       summary->node_id, node_id);
    } else {
        summary = &handshake->nodes[handshake->node_count++];
    }

    memset(summary, 0, sizeof *summary);
    summary->node_id = node_id;
    mesh_session_default_identity(summary);
    return summary;
}

uint32_t mesh_session_roster_owner(const struct mesh_session *session) {
    return session != NULL ? session->roster_node : 0U;
}

void mesh_session_set_roster_owner(struct mesh_session *session, uint32_t node_num) {
    if (session != NULL) {
        session->roster_node = node_num;
    }
}

size_t mesh_session_synced_nodes(const struct mesh_session *session) {
    if (session == NULL || session->sync_epoch == 0U) {
        return 0U;
    }
    const struct mesh_handshake_status *handshake = &session->handshake;
    size_t carried = 0U;
    for (size_t i = 0; i < handshake->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (handshake->nodes[i].sync_epoch == session->sync_epoch) {
            ++carried;
        }
    }
    return carried;
}

/* Whether a name is one the node chose or the one we derived from its number. Used to read an
   older cache, written before the roster carried the answer: the names are all it has. A node
   whose real short name happens to be its factory default reads as derived, which costs one
   informational row on the detail screen and nothing else. */
static bool mesh_session_identity_is_derived(const struct mesh_node_summary *node) {
    struct mesh_node_summary derived;
    memset(&derived, 0, sizeof derived);
    derived.node_id = node->node_id;
    mesh_session_default_identity(&derived);
    return strcmp(node->short_name, derived.short_name) == 0 &&
           strcmp(node->long_name, derived.long_name) == 0;
}

void mesh_session_seed_node(struct mesh_session *session, const struct mesh_node_summary *node) {
    if (session == NULL || node == NULL || node->node_id == 0U) {
        return;
    }
    if (mesh_session_node_known(session, node->node_id)) {
        return;
    }
    struct mesh_node_summary *slot = mesh_session_node_slot(session, node->node_id);
    if (slot == NULL) {
        return;
    }
    *slot = *node;
    /* Restored, not replayed: no sync of ours has carried it, so the first one to complete
       decides whether the radio still knows it. */
    slot->sync_epoch = 0U;
    if (slot->short_name[0] == '\0' && slot->long_name[0] == '\0') {
        mesh_session_default_identity(slot);
    } else if (!slot->has_user && !mesh_session_identity_is_derived(slot)) {
        /* A cache written before has_user existed. The node carries a name nobody derived, so
           it came from a User; without this, a node the radio has since evicted - the whole
           reason the cache is worth restoring - would claim a derived name forever. */
        slot->has_user = true;
    }
}

/*
 * The identity half of a node record. Reached from two directions: NodeInfo during the NodeDB
 * sync, and a NODEINFO_APP packet when a node introduces itself over the air afterwards - the
 * firmware only replays the database once per connection, so without the packet path a node
 * that joins mid-session stays a bare id forever.
 */
static void mesh_session_apply_user(struct mesh_node_summary *summary,
                                    const meshtastic_User *user) {
    /* Names are chosen by whoever owns that node, so they are untrusted radio input just like
       message bodies: sanitise them here rather than at each of the several places that draw
       or serialise them. A plain snprintf would also happily cut a multi-byte character in
       half at the field boundary, which matters because Meshtastic short names are routinely a
       single four-byte emoji. */
    char long_name[sizeof summary->long_name];
    char short_name[sizeof summary->short_name];
    char user_id[sizeof summary->user_id];
    mesh_text_sanitise_str(user->long_name, long_name, sizeof long_name);
    mesh_text_sanitise_str(user->short_name, short_name, sizeof short_name);
    mesh_text_sanitise_str(user->id, user_id, sizeof user_id);

    /* A NodeInfo can carry a User the radio has nothing to put in - it knows the node exists
       and no more. Blanking a name we derived from the node number for that is a step
       backwards, so an empty record leaves the derived identity standing. */
    if (long_name[0] != '\0' || short_name[0] != '\0') {
        (void)mesh_str_copy(summary->long_name, sizeof summary->long_name, long_name);
        (void)mesh_str_copy(summary->short_name, sizeof summary->short_name, short_name);
        summary->has_user = true;
    }
    if (user_id[0] != '\0') {
        (void)mesh_str_copy(summary->user_id, sizeof summary->user_id, user_id);
    }
    summary->hw_model = (uint32_t)user->hw_model;
    summary->role = (uint32_t)user->role;
    summary->is_licensed = user->is_licensed;
    summary->is_unmessagable = user->has_is_unmessagable && user->is_unmessagable;

    summary->public_key_len = 0U;
    if (user->public_key.size > 0U && user->public_key.size <= sizeof summary->public_key) {
        memcpy(summary->public_key, user->public_key.bytes, user->public_key.size);
        summary->public_key_len = (uint8_t)user->public_key.size;
    }
}

static void mesh_session_apply_position(struct mesh_node_summary *summary,
                                        const meshtastic_Position *position, uint32_t heard) {
    /* A Position with neither coordinate is a time-only or precision-only broadcast; keeping
       the last real fix beats replacing it with 0,0 in the Gulf of Guinea. */
    if (!position->has_latitude_i || !position->has_longitude_i) {
        return;
    }
    /* The wire type is four times wider than the range a coordinate can occupy, and nothing
       upstream promises the sender checked. An impossible pair is dropped rather than stored:
       the node keeps the last fix we believed, which is the same answer as for a packet that
       carried no coordinates at all. */
    if (!mesh_geo_coords_valid(position->latitude_i, position->longitude_i)) {
        mesh_log_debug("session", "Node 0x%08x sent an out-of-range fix (%d, %d); keeping the last",
                       summary->node_id, (int)position->latitude_i, (int)position->longitude_i);
        return;
    }
    summary->position.valid = true;
    summary->position.latitude_i = position->latitude_i;
    summary->position.longitude_i = position->longitude_i;
    summary->position.has_altitude = position->has_altitude;
    summary->position.altitude = position->altitude;
    /* `timestamp` is when the GPS solved; `time` is the sender's own clock, which upstream
       says is usually left off the mesh to save space. Preferring the first means the row
       answers "when was this fix taken" whenever either field can, and neither is confused
       with when we heard about it. */
    summary->position.time = position->timestamp != 0U ? position->timestamp : position->time;
    summary->position.received = heard;
    summary->position.sats_in_view =
        (uint8_t)(position->sats_in_view > 255U ? 255U : position->sats_in_view);
    summary->position.precision_bits =
        (uint8_t)(position->precision_bits > 255U ? 255U : position->precision_bits);
}

static void mesh_session_apply_device_metrics(struct mesh_node_summary *summary,
                                              const meshtastic_DeviceMetrics *metrics,
                                              uint32_t heard) {
    summary->metrics.valid = true;
    summary->metrics.time = heard;
    summary->metrics.has_battery = metrics->has_battery_level;
    summary->metrics.battery_level =
        (uint8_t)(metrics->battery_level > 255U ? 255U : metrics->battery_level);
    summary->metrics.has_voltage = metrics->has_voltage;
    summary->metrics.voltage = metrics->voltage;
    summary->metrics.has_channel_utilization = metrics->has_channel_utilization;
    summary->metrics.channel_utilization = metrics->channel_utilization;
    summary->metrics.has_air_util_tx = metrics->has_air_util_tx;
    summary->metrics.air_util_tx = metrics->air_util_tx;
    summary->metrics.has_uptime = metrics->has_uptime_seconds;
    summary->metrics.uptime_seconds = metrics->uptime_seconds;
}

static void mesh_session_apply_environment(struct mesh_node_summary *summary,
                                           const meshtastic_EnvironmentMetrics *env,
                                           uint32_t heard) {
    summary->environment.valid = true;
    summary->environment.time = heard;
    summary->environment.has_temperature = env->has_temperature;
    summary->environment.temperature = env->temperature;
    summary->environment.has_humidity = env->has_relative_humidity;
    summary->environment.relative_humidity = env->relative_humidity;
    summary->environment.has_pressure = env->has_barometric_pressure;
    summary->environment.barometric_pressure = env->barometric_pressure;
    summary->environment.has_iaq = env->has_iaq;
    summary->environment.iaq = (uint16_t)env->iaq;
    summary->environment.has_lux = env->has_lux;
    summary->environment.lux = env->lux;
    summary->environment.has_voltage = env->has_voltage;
    summary->environment.voltage = env->voltage;
    summary->environment.has_current = env->has_current;
    summary->environment.current = env->current;
}

/*
 * The four variants beyond device and environment metrics. Each follows the pattern above: the
 * whole wire message is decoded, and the fields worth a row are copied across with the sender's
 * own has_* rather than being inferred from a zero - on a sensor node "0 ug/m3" and "no
 * particulate sensor" are very different statements.
 */
static void mesh_session_apply_power_metrics(struct mesh_node_summary *summary,
                                             const meshtastic_PowerMetrics *power, uint32_t heard) {
    summary->power.valid = true;
    summary->power.time = heard;
    summary->power.channel[0].has_voltage = power->has_ch1_voltage;
    summary->power.channel[0].voltage = power->ch1_voltage;
    summary->power.channel[0].has_current = power->has_ch1_current;
    summary->power.channel[0].current = power->ch1_current;
    summary->power.channel[1].has_voltage = power->has_ch2_voltage;
    summary->power.channel[1].voltage = power->ch2_voltage;
    summary->power.channel[1].has_current = power->has_ch2_current;
    summary->power.channel[1].current = power->ch2_current;
    summary->power.channel[2].has_voltage = power->has_ch3_voltage;
    summary->power.channel[2].voltage = power->ch3_voltage;
    summary->power.channel[2].has_current = power->has_ch3_current;
    summary->power.channel[2].current = power->ch3_current;
}

static void mesh_session_apply_air_quality(struct mesh_node_summary *summary,
                                           const meshtastic_AirQualityMetrics *air,
                                           uint32_t heard) {
    summary->air_quality.valid = true;
    summary->air_quality.time = heard;
    summary->air_quality.has_pm10 = air->has_pm10_standard;
    summary->air_quality.pm10_standard = (uint16_t)air->pm10_standard;
    summary->air_quality.has_pm25 = air->has_pm25_standard;
    summary->air_quality.pm25_standard = (uint16_t)air->pm25_standard;
    summary->air_quality.has_pm100 = air->has_pm100_standard;
    summary->air_quality.pm100_standard = (uint16_t)air->pm100_standard;
    summary->air_quality.has_co2 = air->has_co2;
    summary->air_quality.co2 = (uint16_t)air->co2;
    summary->air_quality.has_voc_index = air->has_pm_voc_idx;
    summary->air_quality.voc_index = air->pm_voc_idx;
    summary->air_quality.has_nox_index = air->has_pm_nox_idx;
    summary->air_quality.nox_index = air->pm_nox_idx;
}

static void mesh_session_apply_health_metrics(struct mesh_node_summary *summary,
                                              const meshtastic_HealthMetrics *health,
                                              uint32_t heard) {
    summary->health.valid = true;
    summary->health.time = heard;
    summary->health.has_heart_bpm = health->has_heart_bpm;
    summary->health.heart_bpm = (uint8_t)health->heart_bpm;
    summary->health.has_spo2 = health->has_spO2;
    summary->health.spo2 = (uint8_t)health->spO2;
    summary->health.has_temperature = health->has_temperature;
    summary->health.temperature = health->temperature;
}

static void mesh_session_apply_host_metrics(struct mesh_node_summary *summary,
                                            const meshtastic_HostMetrics *host, uint32_t heard) {
    summary->host.valid = true;
    summary->host.time = heard;
    /* uptime, free memory and the load averages are plain scalars with no has_* on the wire, so
       a zero is indistinguishable from silence. Uptime and memory of zero are impossible on a
       host that is running, which is what the flags are set from; a load average of zero is a
       real reading, so the trio is flagged together on any of them being non-zero. */
    summary->host.has_uptime = host->uptime_seconds > 0U;
    summary->host.uptime_seconds = host->uptime_seconds;
    summary->host.has_freemem = host->freemem_bytes > 0U;
    summary->host.freemem_kib = (uint32_t)(host->freemem_bytes / 1024U);
    summary->host.has_diskfree = host->diskfree1_bytes > 0U;
    summary->host.diskfree_mib = (uint32_t)(host->diskfree1_bytes / (1024U * 1024U));
    summary->host.has_load = host->load1 > 0U || host->load5 > 0U || host->load15 > 0U;
    summary->host.load1 = host->load1;
    summary->host.load5 = host->load5;
    summary->host.load15 = host->load15;
}

/*
 * A node's list of who it can hear.
 *
 * The record belongs to `info->node_id` rather than to `packet->from`: a NeighborInfo is
 * forwarded across the mesh, and `last_sent_by_id` names whoever relayed it. Attributing the
 * list to the relayer would draw one node's neighbours on another node's screen, which is
 * exactly the kind of wrong that looks plausible.
 *
 * A report with no neighbours in it is kept as a report with no neighbours: a node that hears
 * nobody is a real and interesting state, and rejecting it would leave the last non-empty list
 * standing as though it were still true.
 */
static void mesh_session_apply_neighbors(struct mesh_node_summary *summary,
                                         const meshtastic_NeighborInfo *info, uint32_t heard) {
    summary->neighbors.valid = true;
    summary->neighbors.time = heard;
    summary->neighbors.broadcast_interval_secs = info->node_broadcast_interval_secs;
    summary->neighbors.count = 0U;
    for (pb_size_t i = 0;
         i < info->neighbors_count && summary->neighbors.count < MESH_NODE_MAX_NEIGHBORS; ++i) {
        if (info->neighbors[i].node_id == 0U) {
            continue;
        }
        struct mesh_node_neighbor *entry = &summary->neighbors.entries[summary->neighbors.count++];
        entry->node_id = info->neighbors[i].node_id;
        entry->snr = info->neighbors[i].snr;
    }
}

/*
 * LocalStats is the radio describing itself, so it lands on the session rather than on a node
 * record. Two fields get a flag rather than being trusted at face value: heap_total_bytes of
 * zero means the firmware did not fill it in (no radio has no heap), and a noise floor of
 * exactly 0 dBm is not a reading any LoRa front end produces.
 */
static void mesh_session_apply_local_stats(struct mesh_session *session,
                                           const meshtastic_LocalStats *stats, uint32_t stamp) {
    struct mesh_radio_stats *out = &session->stats;
    out->valid = true;
    out->time = stamp;
    out->uptime_seconds = stats->uptime_seconds;
    out->channel_utilization = stats->channel_utilization;
    out->air_util_tx = stats->air_util_tx;
    out->num_packets_tx = stats->num_packets_tx;
    out->num_packets_rx = stats->num_packets_rx;
    out->num_packets_rx_bad = stats->num_packets_rx_bad;
    out->num_rx_dupe = stats->num_rx_dupe;
    out->num_tx_relay = stats->num_tx_relay;
    out->num_tx_relay_canceled = stats->num_tx_relay_canceled;
    out->num_tx_dropped = stats->num_tx_dropped;
    out->num_online_nodes = stats->num_online_nodes;
    out->num_total_nodes = stats->num_total_nodes;
    out->has_heap = stats->heap_total_bytes > 0U;
    out->heap_total_bytes = stats->heap_total_bytes;
    out->heap_free_bytes = stats->heap_free_bytes;
    out->has_noise_floor = stats->noise_floor != 0;
    out->noise_floor = stats->noise_floor;
}

static void mesh_session_store_node_summary(struct mesh_session *session,
                                            const meshtastic_NodeInfo *info) {
    struct mesh_node_summary *summary = mesh_session_node_slot(session, info->num);
    if (summary == NULL) {
        return;
    }

    /*
     * A resync replaces what the radio knows, but the radio's NodeDB does not carry environment
     * telemetry, and it may send a NodeInfo with no position or metrics for a node we have
     * already heard both from. Overwriting only what this NodeInfo actually carries keeps the
     * detail screen from emptying itself every time the database is replayed.
     */
    summary->last_heard =
        info->last_heard > summary->last_heard ? info->last_heard : summary->last_heard;
    summary->snr = info->snr;
    summary->via_mqtt = info->via_mqtt;
    summary->has_hops_away = info->has_hops_away;
    summary->hops_away = info->hops_away;
    summary->is_favorite = info->is_favorite;
    summary->is_ignored = info->is_ignored;
    summary->is_muted = info->is_muted;
    summary->channel = (uint8_t)info->channel;
    /* Stamped whether or not this NodeInfo told us anything new: what it proves is that the
       radio's database still carries the node, which is what the stamp is read for. */
    summary->sync_epoch = session->sync_epoch;
    /* The same proof, said in the form the UI reads. config_complete settles the *other*
       direction below, for the nodes no NodeInfo mentioned; without this line a screen drawn
       mid-sync would mark every node as one the radio has forgotten until the sync ended. */
    summary->in_nodedb = true;

    if (info->has_user) {
        mesh_session_apply_user(summary, &info->user);
    }
    if (info->has_position) {
        /* No arrival time, and deliberately not `last_heard`. This is the radio replaying its
           NodeDB at us: the fix inside may be days old, while last_heard is the node's most
           recent packet of *any* kind, so borrowing it would date a stale fix by unrelated
           chatter - the exact conflation `received` exists to end. We did not watch this one
           arrive, so we say we do not know when it did. */
        mesh_session_apply_position(summary, &info->position, 0U);
    }
    if (info->has_device_metrics) {
        mesh_session_apply_device_metrics(summary, &info->device_metrics, info->last_heard);
    }

    mesh_log_debug("session", "Cached node %u (%s) last_heard=%u%s", summary->node_id,
                   summary->has_user ? summary->short_name : "unnamed", summary->last_heard,
                   summary->via_mqtt ? " via_mqtt" : "");
}

/*
 * The sync just ended, so the roster divides in two: the nodes the radio still carries, and the
 * ones only we remember. The second group is worth keeping - they are on the mesh, the radio's
 * small database simply pushed them out - but the Nodes screen says which is which, because a
 * message to a node the radio has forgotten has no key to encrypt with and may never leave.
 */
static void mesh_session_resolve_nodedb_membership(struct mesh_session *session) {
    struct mesh_handshake_status *handshake = &session->handshake;
    size_t remembered = 0U;
    for (size_t i = 0; i < handshake->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        struct mesh_node_summary *node = &handshake->nodes[i];
        node->in_nodedb = node->sync_epoch == session->sync_epoch;
        if (!node->in_nodedb) {
            ++remembered;
        }
    }
    if (remembered > 0U) {
        mesh_log_info("session", "Roster holds %zu node%s the radio's NodeDB no longer carries",
                      remembered, remembered == 1U ? "" : "s");
    }
}

/*
 * Every packet a node sends us is proof it is alive now. The NodeDB sync only tells us what the
 * radio knew at connect time, and a mesh of 130 nodes re-sorts constantly, so without this the
 * node you are actually talking to sinks down (or off) the UI's list while it is chatting with
 * you. A node the sync never delivered (cache full, or joined later) is added with just its id;
 * the name follows when the radio sends its NodeInfo.
 */
static void mesh_session_touch_node_from_packet(struct mesh_session *session,
                                                const meshtastic_MeshPacket *packet) {
    struct mesh_handshake_status *handshake = &session->handshake;
    if (packet->from == 0U || packet->from == MESH_MESSAGE_BROADCAST_ADDR ||
        (handshake->has_my_info && packet->from == handshake->my_info.my_node_num)) {
        return;
    }

    uint32_t heard = packet->has_rx_time ? packet->rx_time : 0U;
    if (heard == 0U) {
        /* No radio timestamp: use ours if it looks like a real clock (not 1970). */
        heard = mesh_session_wall_clock();
    }

    const bool known = mesh_session_node_known(session, packet->from);
    struct mesh_node_summary *summary = mesh_session_node_slot(session, packet->from);
    if (summary == NULL) {
        return;
    }
    if (!known) {
        mesh_log_info("session", "Node 0x%08x heard before its NodeInfo; added to the cache",
                      packet->from);
    }

    if (heard > summary->last_heard) {
        summary->last_heard = heard;
    }
    if (packet->rx_snr != 0.0f) {
        summary->snr = packet->rx_snr;
    }
    /* A packet that reached us over MQTT was not heard by this radio at all, so whatever RSSI
       rides along with it describes somebody else's antenna. The reading is stamped rather
       than cleared on the next MQTT packet: it stays a true measurement, and clearing it would
       make the row flicker on a mesh whose bridge relays traffic we also hear ourselves. */
    if (packet->has_rx_rssi && !packet->via_mqtt) {
        summary->has_rssi = true;
        summary->rx_rssi = (int16_t)packet->rx_rssi;
        summary->rssi_time = heard;
    }
    if (packet->hop_start != 0U && packet->hop_start >= packet->hop_limit) {
        summary->has_hops_away = true;
        summary->hops_away = (uint8_t)(packet->hop_start - packet->hop_limit);
    }
    summary->via_mqtt = packet->via_mqtt;
}

/*
 * The other half of a node's record, off the air rather than out of the NodeDB. The firmware
 * replays its database exactly once per connection, so everything that happens afterwards - a
 * node joining, a battery draining, a tracker moving - only reaches us as one of these three
 * app payloads. Without this the detail screen would show whatever was true at connect time
 * and then quietly rot for the rest of the session.
 *
 * Unlike the last_heard touch above this runs for our own node too: our node broadcasts its
 * own position and telemetry like any other, and it is the one node whose battery the user can
 * do something about.
 */
static void mesh_session_apply_packet_details(struct mesh_session *session,
                                              const meshtastic_MeshPacket *packet) {
    if (packet->from == 0U || packet->from == MESH_MESSAGE_BROADCAST_ADDR ||
        packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return;
    }

    const meshtastic_Data *data = &packet->decoded;
    if (data->portnum != meshtastic_PortNum_NODEINFO_APP &&
        data->portnum != meshtastic_PortNum_POSITION_APP &&
        data->portnum != meshtastic_PortNum_TELEMETRY_APP &&
        data->portnum != meshtastic_PortNum_NEIGHBORINFO_APP) {
        return;
    }

    uint32_t heard = packet->has_rx_time ? packet->rx_time : 0U;
    if (heard == 0U) {
        heard = mesh_session_wall_clock();
    }

    /* LocalStats below is about the radio rather than about a node, so a full node cache
       must not cost us the one telemetry that has nowhere else to go. */
    struct mesh_node_summary *summary = mesh_session_node_slot(session, packet->from);

    pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);
    switch (data->portnum) {
    case meshtastic_PortNum_NODEINFO_APP: {
        meshtastic_User user = meshtastic_User_init_default;
        if (!pb_decode(&stream, meshtastic_User_fields, &user)) {
            mesh_log_debug("session", "Bad NODEINFO_APP from 0x%08x: %s", packet->from,
                           PB_GET_ERROR(&stream));
            return;
        }
        if (summary == NULL) {
            return;
        }
        mesh_session_apply_user(summary, &user);
        mesh_log_debug("session", "Node 0x%08x introduced itself as %s", packet->from,
                       summary->short_name);
        break;
    }
    case meshtastic_PortNum_POSITION_APP: {
        meshtastic_Position position = meshtastic_Position_init_default;
        if (!pb_decode(&stream, meshtastic_Position_fields, &position)) {
            mesh_log_debug("session", "Bad POSITION_APP from 0x%08x: %s", packet->from,
                           PB_GET_ERROR(&stream));
            return;
        }
        if (summary == NULL) {
            return;
        }
        mesh_session_apply_position(summary, &position, heard);
        break;
    }
    case meshtastic_PortNum_NEIGHBORINFO_APP: {
        meshtastic_NeighborInfo info = meshtastic_NeighborInfo_init_default;
        if (!pb_decode(&stream, meshtastic_NeighborInfo_fields, &info)) {
            mesh_log_debug("session", "Bad NEIGHBORINFO_APP from 0x%08x: %s", packet->from,
                           PB_GET_ERROR(&stream));
            return;
        }
        /* The reporting node, not the one that handed it to us. `summary` above is the
           relayer's slot, so this looks up its own. */
        const uint32_t reporter = info.node_id != 0U ? info.node_id : packet->from;
        struct mesh_node_summary *owner =
            (reporter == packet->from) ? summary : mesh_session_node_slot(session, reporter);
        if (owner == NULL) {
            return;
        }
        mesh_session_apply_neighbors(owner, &info, heard);
        mesh_log_debug("session", "Node 0x%08x reports %u neighbour%s", reporter,
                       (unsigned)owner->neighbors.count, owner->neighbors.count == 1U ? "" : "s");
        break;
    }
    case meshtastic_PortNum_TELEMETRY_APP: {
        meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_default;
        if (!pb_decode(&stream, meshtastic_Telemetry_fields, &telemetry)) {
            mesh_log_debug("session", "Bad TELEMETRY_APP from 0x%08x: %s", packet->from,
                           PB_GET_ERROR(&stream));
            return;
        }
        const uint32_t stamp = telemetry.time != 0U ? telemetry.time : heard;
        /* LocalStats never crosses the mesh: the firmware sends it to the attached client
           only, from its own node number. Anything else claiming to be ours is not. */
        if (telemetry.which_variant == meshtastic_Telemetry_local_stats_tag) {
            if (session->handshake.has_my_info &&
                packet->from == session->handshake.my_info.my_node_num) {
                mesh_session_apply_local_stats(session, &telemetry.variant.local_stats, heard);
            }
            break;
        }
        if (summary == NULL) {
            return;
        }
        switch (telemetry.which_variant) {
        case meshtastic_Telemetry_device_metrics_tag:
            mesh_session_apply_device_metrics(summary, &telemetry.variant.device_metrics, stamp);
            break;
        case meshtastic_Telemetry_environment_metrics_tag:
            mesh_session_apply_environment(summary, &telemetry.variant.environment_metrics, stamp);
            break;
        case meshtastic_Telemetry_power_metrics_tag:
            mesh_session_apply_power_metrics(summary, &telemetry.variant.power_metrics, stamp);
            break;
        case meshtastic_Telemetry_air_quality_metrics_tag:
            mesh_session_apply_air_quality(summary, &telemetry.variant.air_quality_metrics, stamp);
            break;
        case meshtastic_Telemetry_health_metrics_tag:
            mesh_session_apply_health_metrics(summary, &telemetry.variant.health_metrics, stamp);
            break;
        case meshtastic_Telemetry_host_metrics_tag:
            mesh_session_apply_host_metrics(summary, &telemetry.variant.host_metrics, stamp);
            break;
        default:
            /* TrafficManagementStats and anything upstream adds next: decoded, counted as a
               packet from the node, and otherwise nothing we have a row for. */
            break;
        }
        break;
    }
    default:
        break;
    }
}

/*
 * A WAYPOINT_APP packet: a place somebody shared with the channel.
 *
 * Its own step rather than a case inside mesh_session_apply_packet_details(), which is about
 * the *node* a packet came from - a waypoint says nothing about its sender beyond that they
 * were there to send it, and the roster is not where a place belongs. It is also not a message:
 * mesh_message_ingest() would have to grow a second payload shape to hold one, and the log it
 * appends to is a ring of things that happened rather than a table of things that are.
 */
static void mesh_session_handle_waypoint(struct mesh_session *session,
                                         const meshtastic_MeshPacket *packet) {
    if (packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet->decoded.portnum != meshtastic_PortNum_WAYPOINT_APP) {
        return;
    }
    uint32_t heard = packet->has_rx_time ? packet->rx_time : 0U;
    if (heard == 0U) {
        heard = mesh_session_wall_clock();
    }
    mesh_waypoint_ingest(
        &session->waypoints, packet,
        session->handshake.has_my_info ? session->handshake.my_info.my_node_num : 0U, heard);
}

/*
 * A STORE_FORWARD_APP packet: a router talking to us about the history it keeps.
 *
 * Claimed outright, like the traceroute below it, because none of what arrives here is a
 * message *as it stands*: an announcement, a count, a refusal, or a message wrapped inside a
 * router's packet. The last of those becomes one, and this is where it stops being the router's
 * packet and starts being what the sender said - which is why the fold happens here and not in
 * mesh_message_ingest(), whose whole input is one MeshPacket carrying one payload.
 *
 * The de-duplication is the part that earns its keep. A replay hands back everything in the
 * router's window, which on a client that was only briefly off is mostly traffic it heard live;
 * without this, one press would put a second copy of the last four hours under the first.
 */
static void mesh_session_handle_store_forward(struct mesh_session *session,
                                              const meshtastic_MeshPacket *packet) {
    const uint32_t my_node =
        session->handshake.has_my_info ? session->handshake.my_info.my_node_num : 0U;
    struct mesh_message replayed;
    const int event =
        mesh_store_forward_ingest(&session->store_forward, packet, my_node,
                                  mesh_session_wall_clock(), mesh_time_monotonic_ms(), &replayed);
    if (event != MESH_STORE_FORWARD_EVENT_TEXT) {
        /*
         * Everything that is not a replayed message was written by the node that sent it - a
         * router announcing itself, a count, a refusal, or somebody else's request crossing our
         * radio - so the packet's SNR, hop count and arrival really do measure the link to that
         * node, and the roster is told. For a router this is the whole of what keeps it in the
         * roster at all: a heartbeat is often the only packet one ever sends.
         */
        mesh_session_touch_node_from_packet(session, packet);
        return;
    }
    /*
     * A replayed message is the exception, and deliberately not touched. `from` is the original
     * sender, but the packet carrying it came one hop from the router - so touching would file
     * the router's SNR, its hop count and this moment's arrival under a node that may not have
     * been heard from in days. Asking for history would quietly make every sender in the window
     * look freshly reachable over a link that was never measured to them, which is the same
     * mistake the message itself refuses when it declines to carry those fields.
     */
    if (mesh_message_log_holds_replay(&session->messages, &replayed)) {
        mesh_log_debug("session", "Store & Forward replayed a message we already had");
        return;
    }
    if (mesh_message_log_append(&session->messages, &replayed) == NULL) {
        return;
    }
    mesh_store_forward_stored(&session->store_forward);
    mesh_log_info("session", "Store & Forward replayed a message from 0x%08x on channel %u",
                  replayed.from, (unsigned)replayed.channel);
}

/*
 * A TRACEROUTE_APP packet answering the request we sent. The firmware replies from the target
 * with `Data.request_id` set to our packet id, which is what tells our trace from somebody
 * else's crossing the same radio - a node relaying a trace between two other nodes sees the
 * same portnum.
 *
 * Returns true when the packet was ours, so the caller can keep it out of everything else.
 */
static bool mesh_session_handle_traceroute(struct mesh_session *session,
                                           const meshtastic_MeshPacket *packet) {
    const meshtastic_Data *data = &packet->decoded;
    if (data->portnum != meshtastic_PortNum_TRACEROUTE_APP) {
        return false;
    }
    struct mesh_traceroute *trace = &session->traceroute;
    if (trace->state != MESH_TRACEROUTE_PENDING || data->request_id == 0U ||
        data->request_id != trace->packet_id) {
        /* Not an answer to ours: a trace passing through, or one we have already given up on.
           Claimed anyway - a RouteDiscovery is not a message and has no business in the log. */
        mesh_log_debug("session", "Ignoring TRACEROUTE_APP from 0x%08x (request %u)", packet->from,
                       data->request_id);
        return true;
    }

    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);
    if (!pb_decode(&stream, meshtastic_RouteDiscovery_fields, &route)) {
        mesh_log_warn("session", "Bad TRACEROUTE_APP reply: %s", PB_GET_ERROR(&stream));
        trace->state = MESH_TRACEROUTE_TIMEOUT;
        return true;
    }

    trace->route_count =
        (uint8_t)(route.route_count > MESH_TRACEROUTE_MAX_HOPS ? MESH_TRACEROUTE_MAX_HOPS
                                                               : route.route_count);
    for (uint8_t i = 0; i < trace->route_count; ++i) {
        trace->route[i] = route.route[i];
    }
    trace->snr_count = (uint8_t)(route.snr_towards_count > MESH_TRACEROUTE_MAX_HOPS + 1U
                                     ? MESH_TRACEROUTE_MAX_HOPS + 1U
                                     : route.snr_towards_count);
    for (uint8_t i = 0; i < trace->snr_count; ++i) {
        trace->snr[i] = route.snr_towards[i];
    }
    trace->back_count =
        (uint8_t)(route.route_back_count > MESH_TRACEROUTE_MAX_HOPS ? MESH_TRACEROUTE_MAX_HOPS
                                                                    : route.route_back_count);
    for (uint8_t i = 0; i < trace->back_count; ++i) {
        trace->route_back[i] = route.route_back[i];
    }
    trace->snr_back_count = (uint8_t)(route.snr_back_count > MESH_TRACEROUTE_MAX_HOPS + 1U
                                          ? MESH_TRACEROUTE_MAX_HOPS + 1U
                                          : route.snr_back_count);
    for (uint8_t i = 0; i < trace->snr_back_count; ++i) {
        trace->snr_back[i] = route.snr_back[i];
    }

    trace->completed = mesh_session_wall_clock();
    trace->state = MESH_TRACEROUTE_DONE;
    mesh_log_info("session", "Traceroute to 0x%08x: %u hops out, %u back", trace->target,
                  (unsigned)trace->route_count, (unsigned)trace->back_count);
    return true;
}

static void mesh_session_handle_log_record(const meshtastic_LogRecord *record) {
    char message[sizeof(record->message) + 1U];
    memcpy(message, record->message, sizeof record->message);
    message[sizeof record->message] = '\0';

    const char *component = "radio.log";
    switch (record->level) {
    case meshtastic_LogRecord_Level_CRITICAL:
    case meshtastic_LogRecord_Level_ERROR:
        mesh_log_error(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_WARNING:
        mesh_log_warn(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_INFO:
        mesh_log_info(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_DEBUG:
        mesh_log_debug(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_TRACE:
    case meshtastic_LogRecord_Level_UNSET:
    default:
        mesh_log_trace(component, "%s", message);
        break;
    }
}

/*
 * The radio's own explanation of something it just did or refused to do. Unlike a LogRecord -
 * which is the firmware's debug stream and goes to our log at its own level - a
 * ClientNotification is addressed *to the user*: the firmware raises one when it has taken a
 * decision the person holding the client needs to know about, and the phone apps show it.
 *
 * The message is untrusted radio text, so it is sanitised on the way in like a node name is,
 * and the slot it lands in is smaller than the 400 bytes the wire allows.
 */
static void mesh_session_handle_client_notification(struct mesh_session *session,
                                                    const meshtastic_ClientNotification *note) {
    struct mesh_client_notification *out = &session->notification;
    /* Monotonic for the life of the session: two identical notifications are two events, and a
       reader watching for "something new" cannot see that in the text. */
    out->seq++;
    out->time = note->time;
    out->received = mesh_session_wall_clock();
    out->has_reply_id = note->has_reply_id;
    out->reply_id = note->has_reply_id ? note->reply_id : 0U;
    out->level = (uint8_t)note->level;
    mesh_text_sanitise((const uint8_t *)note->message, strnlen(note->message, sizeof note->message),
                       out->text, sizeof out->text);

    /* Logged at the radio's own level as well as kept for the UI: the screen shows the newest
       one and the log is where the sequence of them can be read back afterwards. */
    const char *component = "radio.notify";
    switch (note->level) {
    case meshtastic_LogRecord_Level_CRITICAL:
    case meshtastic_LogRecord_Level_ERROR:
        mesh_log_error(component, "%s", out->text);
        break;
    case meshtastic_LogRecord_Level_WARNING:
        mesh_log_warn(component, "%s", out->text);
        break;
    default:
        mesh_log_info(component, "%s", out->text);
        break;
    }
}

/*
 * The radio reporting its outgoing queue after every ToRadio it took or refused.
 *
 * A refusal (`res` non-zero) is the one failure that produces no Routing reply at all: the
 * packet was never transmitted, so nothing in the mesh will ever answer for it, and the
 * message would otherwise sit PENDING until the ring evicted it. Marking it here is what turns
 * "still sending..." into a stated failure with the firmware's own reason on it.
 */
static void mesh_session_handle_queue_status(struct mesh_session *session,
                                             const meshtastic_QueueStatus *status) {
    struct mesh_queue_status *out = &session->queue;
    out->valid = true;
    out->time = mesh_session_wall_clock();
    out->res = status->res;
    out->free = status->free;
    out->maxlen = status->maxlen;
    out->mesh_packet_id = status->mesh_packet_id;

    if (status->res == 0) {
        return;
    }
    mesh_log_warn("session", "Radio refused packet %u (error %d); %u of %u queue slots free",
                  status->mesh_packet_id, (int)status->res, (unsigned)status->free,
                  (unsigned)status->maxlen);
    /* `res` is a Routing_Error, the same scale mesh_message_log_mark_ack() already speaks, so
       the message log needs no new failure vocabulary for this. A queue status with no packet
       id is the radio reporting depth rather than refusing anything. */
    if (status->mesh_packet_id != 0U) {
        (void)mesh_message_log_mark_ack(&session->messages, status->mesh_packet_id,
                                        MESH_MESSAGE_ACK_FAILED, (uint8_t)status->res);
    }
}

static void mesh_session_handle_channel(struct mesh_session *session,
                                        const meshtastic_Channel *channel) {
    if (channel->index < 0 || (size_t)channel->index >= MESH_SESSION_MAX_CHANNELS) {
        mesh_log_debug("session", "Ignoring channel with index %d", (int)channel->index);
        return;
    }
    mesh_radio_settings_apply_channel(&session->settings, channel);
    struct mesh_channel_summary *slot = &session->handshake.channels[channel->index];
    memset(slot, 0, sizeof *slot);
    slot->index = (uint8_t)channel->index;
    slot->role = (uint8_t)channel->role;
    if (channel->has_settings) {
        snprintf(slot->name, sizeof slot->name, "%s", channel->settings.name);
        slot->psk_len = (uint8_t)channel->settings.psk.size;
        slot->uplink_enabled = channel->settings.uplink_enabled;
        slot->downlink_enabled = channel->settings.downlink_enabled;
        if (channel->settings.has_module_settings) {
            slot->position_precision = channel->settings.module_settings.position_precision;
        }
    }
    if ((size_t)channel->index + 1U > session->handshake.channel_count) {
        session->handshake.channel_count = (size_t)channel->index + 1U;
    }
    if (channel->role != meshtastic_Channel_Role_DISABLED) {
        mesh_log_info("session", "Channel %d: %s (%s)", (int)channel->index,
                      slot->name[0] != '\0' ? slot->name : "<default>",
                      channel->role == meshtastic_Channel_Role_PRIMARY ? "primary" : "secondary");
    }
}

void mesh_session_handle_from_radio(struct mesh_session *session, const uint8_t *payload,
                                    size_t len) {
    if (session == NULL || payload == NULL || len == 0U) {
        return;
    }

    meshtastic_FromRadio message = meshtastic_FromRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(payload, len);
    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &message)) {
        mesh_log_warn("session", "Failed to decode FromRadio: %s", PB_GET_ERROR(&stream));
        return;
    }

    struct mesh_handshake_status *handshake = &session->handshake;
    switch (message.which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        /* The roster survives a reconnect, but not a move to another radio: that radio's
           NodeDB is a different view of the mesh, and half of what we remember may not be
           reachable through it. */
        if (session->roster_node != 0U && session->roster_node != message.my_info.my_node_num) {
            mesh_log_info("session", "Radio changed (0x%08x -> 0x%08x); dropping the roster",
                          session->roster_node, message.my_info.my_node_num);
            mesh_session_clear_nodes(session);
            /* And everything else the old radio told us about itself. A reconnect keeps the
               config because it is the same radio; this is the case where it is not, and the
               channel table and LoRa settings we are holding are another radio's. */
            mesh_session_forget_radio(session);
            /*
             * The places go with the channel table, which is what makes them the roster's case
             * rather than the message log's.
             *
             * A message survives a swap because its channel is only ever a label on something
             * that already happened. A waypoint's is an *index into the table just discarded*,
             * and "Share it again" broadcasts on it - so a place carried across would go out on
             * whatever slot that number names on the new radio, which is a different channel or
             * none. They are also another mesh's places, which is the roster's own argument.
             */
            mesh_waypoint_book_reset(&session->waypoints);
        }
        session->roster_node = message.my_info.my_node_num;
        handshake->has_my_info = true;
        handshake->my_info = message.my_info;
        mesh_log_info("session", "MyNodeInfo: node=%u, node_count=%u", message.my_info.my_node_num,
                      message.my_info.nodedb_count);
        break;
    case meshtastic_FromRadio_node_info_tag:
        mesh_session_store_node_summary(session, &message.node_info);
        /* Our own NodeInfo carries the owner record the User settings section shows. */
        if (message.node_info.has_user && handshake->has_my_info &&
            message.node_info.num == handshake->my_info.my_node_num) {
            mesh_radio_settings_apply_owner(&session->settings, &message.node_info.user);
        }
        break;
    case meshtastic_FromRadio_channel_tag:
        mesh_session_handle_channel(session, &message.channel);
        break;
    case meshtastic_FromRadio_config_tag:
        handshake->has_config = true;
        handshake->config = message.config;
        mesh_radio_settings_apply_config(&session->settings, &message.config);
        mesh_log_debug("session", "Received config fragment (variant %u)",
                       (unsigned)message.config.which_payload_variant);
        break;
    case meshtastic_FromRadio_moduleConfig_tag:
        mesh_radio_settings_apply_module_config(&session->settings, &message.moduleConfig);
        mesh_log_debug("session", "Received module config fragment (variant %u)",
                       (unsigned)message.moduleConfig.which_payload_variant);
        break;
    case meshtastic_FromRadio_deviceuiConfig_tag:
        /* The radio's own screen settings, streamed with the rest of the handshake. Kept here
           as well as through get_ui_config_response so the section is populated on a radio
           whose firmware predates the admin verb but still streams the fragment. */
        mesh_radio_settings_apply_ui_config(&session->settings, &message.deviceuiConfig);
        mesh_log_debug("session", "Received device UI config (version %u)",
                       (unsigned)message.deviceuiConfig.version);
        break;
    case meshtastic_FromRadio_metadata_tag:
        mesh_radio_settings_apply_metadata(&session->settings, &message.metadata);
        mesh_log_info("session", "Device metadata: firmware %s, hw_model %u",
                      message.metadata.firmware_version, (unsigned)message.metadata.hw_model);
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        handshake->config_complete_id = message.config_complete_id;
        if (handshake->request_in_flight && message.config_complete_id == handshake->request_id) {
            handshake->request_in_flight = false;
            handshake->config_complete = true;
            mesh_session_resolve_nodedb_membership(session);
            mesh_log_info("session", "Config sync complete for request %u",
                          message.config_complete_id);
        } else {
            mesh_log_debug("session", "Received config_complete_id=%u (pending=%s request=%u)",
                           message.config_complete_id, handshake->request_in_flight ? "yes" : "no",
                           handshake->request_id);
        }
        break;
    case meshtastic_FromRadio_packet_tag:
        /* Admin replies come from ourselves; they are not traffic and never a message. */
        if (mesh_radio_settings_ingest(&session->settings, &message.packet) == 1) {
            break;
        }
        /* A RouteDiscovery is not a message either, and the node it came from is already
           being touched below - so claim it after the touch, not before. */
        if (message.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            message.packet.decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP) {
            mesh_session_touch_node_from_packet(session, &message.packet);
            (void)mesh_session_handle_traceroute(session, &message.packet);
            break;
        }
        /*
         * And a Store & Forward frame, which is claimed for the same reason and touches the
         * roster on its own terms rather than here: whether the node named on the envelope is
         * the node the packet's measurements belong to depends on what is inside it, and only
         * the decode knows. See mesh_session_handle_store_forward().
         */
        if (mesh_store_forward_is_frame(&message.packet)) {
            mesh_session_handle_store_forward(session, &message.packet);
            break;
        }
        mesh_session_touch_node_from_packet(session, &message.packet);
        mesh_session_apply_packet_details(session, &message.packet);
        mesh_session_handle_waypoint(session, &message.packet);
        mesh_message_ingest(&session->messages, &message.packet,
                            handshake->has_my_info ? handshake->my_info.my_node_num : 0U);
        break;
    case meshtastic_FromRadio_log_record_tag:
        mesh_session_handle_log_record(&message.log_record);
        break;
    case meshtastic_FromRadio_clientNotification_tag:
        mesh_session_handle_client_notification(session, &message.clientNotification);
        break;
    case meshtastic_FromRadio_queueStatus_tag:
        mesh_session_handle_queue_status(session, &message.queueStatus);
        break;
    case meshtastic_FromRadio_rebooted_tag:
        /*
         * The radio restarted underneath a link that survived it. Everything the config sync
         * told us describes the process that just died - the NodeDB replay, the channel table,
         * the config fragments, and the admin session passkey above all - so the only correct
         * response is to ask for all of it again.
         *
         * The counter is bumped *after* the handshake restarts, not before:
         * mesh_session_begin_handshake() resets the per-connection state, and this counter is
         * part of that state so it clears on a detach or a radio swap. Incrementing first
         * would hand the reset its own answer to wipe.
         */
        if (message.rebooted) {
            mesh_log_info("session", "Radio reports it rebooted; re-running the config sync");
            /* A drop keeps the config because nothing changed; a reboot is the case where it
               may have. A settings write is followed by exactly this, so holding the old value
               here would show the user the number they just replaced. */
            mesh_session_forget_radio(session);
            (void)mesh_session_begin_handshake(session);
            session->reboot_notices++;
        }
        break;
    default:
        mesh_log_debug("session", "Ignoring FromRadio payload tag %" PRIu32,
                       (uint32_t)message.which_payload_variant);
        break;
    }
}

/*
 * Pushes our wall clock at the radio once per connection. A node with no GPS and no phone ever
 * attached sits at 00:00 forever, which also means every packet it hands us has rx_time 0 and
 * the UI can say nothing about when anything arrived. `set_time_only` is the firmware's
 * convenience for exactly this, and it is what the phone clients do on connect.
 *
 * Silent either way: a radio that already has a better clock (GPS, another node) is free to
 * ignore us, and a Brick whose own clock is not credible pushes nothing at all.
 */
static void mesh_session_sync_clock(struct mesh_session *session) {
    const time_t now = time(NULL);
    if (now <= 0 || (uint64_t)now > UINT32_MAX) {
        return;
    }
    const uint32_t epoch = (uint32_t)now;
    if (epoch < MESH_RADIO_CLOCK_MIN_EPOCH) {
        mesh_log_info("session", "Not setting the radio clock: our own clock reads %u", epoch);
        return;
    }
    const int queued = mesh_radio_settings_queue_time(&session->settings, epoch);
    if (queued > 0) {
        mesh_log_info("session", "Setting the radio clock to %u (%d requests)", epoch, queued);
    } else if (queued < 0) {
        mesh_log_warn("session", "Could not queue the radio clock: %d", queued);
    }
}

/* Defined next to the traceroute's send, at the bottom; the tick is the only caller that is
   not the request itself, and it needs it a few hundred lines earlier. */
static int mesh_session_send_store_forward(struct mesh_session *session, bool ping,
                                           uint64_t now_ms);

/*
 * Once the handshake has completed, ask for the metadata and the owner (proof that the
 * AdminMessage round trip and its session passkey work on this radio), push our clock at the
 * radio, then send whatever else is queued, one request at a time.
 */
void mesh_session_tick(struct mesh_session *session, uint64_t now_ms) {
    if (session == NULL) {
        return;
    }
    /* Nothing on the mesh reports a traceroute that was dropped on the way out or on the way
       back, so the clock is the only thing that can end a lost one. Checked before the link
       guards below: a trace outlives a momentary stall, and a row reading "tracing" forever
       is worse than one that says it gave up. */
    struct mesh_traceroute *trace = &session->traceroute;
    if (trace->state == MESH_TRACEROUTE_PENDING && now_ms > trace->sent_ms &&
        now_ms - trace->sent_ms > MESH_TRACEROUTE_TIMEOUT_MS) {
        trace->state = MESH_TRACEROUTE_TIMEOUT;
        mesh_log_info("session", "Traceroute to 0x%08x timed out", trace->target);
    }
    /*
     * And the places whose own deadline has passed, for the same reason: nothing on the mesh
     * re-announces a waypoint's expiry, so the clock is the only thing that can retire one.
     *
     * Above the link guards, again deliberately. An expiry is a fact about the place rather than
     * about the connection, and a list that went on offering "share it again" for somewhere that
     * stopped being a place an hour ago would be wrong whether or not a radio is attached. The
     * clock is the credible one, so a Brick that does not know the date simply retires nothing.
     */
    (void)mesh_waypoint_book_prune(&session->waypoints, mesh_time_wall_credible_s());

    /* And the Store & Forward waits, for the traceroute's reason exactly: a ping nobody
       answered and a replay that stopped arriving are both silences, and a clock is the only
       thing that can tell either of them from a reply still on its way. */
    (void)mesh_store_forward_tick(&session->store_forward, now_ms);

    if (session->send == NULL || !session->handshake.has_my_info) {
        return;
    }

    /* The history request a pong earned. Sent from here rather than from the ingest that armed
       it so the reply arriving on the link's read path does not write back down the link on the
       same turn - the admin queue's split, for the same reason. */
    if (session->store_forward.followup) {
        session->store_forward.followup = false;
        if (session->store_forward.router != 0U) {
            (void)mesh_session_send_store_forward(session, false, now_ms);
        }
    }
    if (session->handshake.config_complete && !session->admin_probe_queued) {
        session->admin_probe_queued = true;
        mesh_radio_settings_queue_probe(&session->settings);
        mesh_session_sync_clock(session);
    }

    struct mesh_admin_request request;
    if (!mesh_radio_settings_next_request(&session->settings, now_ms, &request)) {
        return;
    }
    request.my_node = session->handshake.my_info.my_node_num;
    request.packet_id = mesh_session_next_packet_id(session);

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    size_t written = 0U;
    int result = mesh_radio_settings_encode_request(&session->settings, &request, payload,
                                                    sizeof payload, &written);
    if (result < 0) {
        mesh_log_warn("session", "Admin request encode failed: %d", result);
        return;
    }
    result = mesh_session_send_raw(session, payload, written, 0U);
    if (result < 0) {
        /* A failed send may already have dropped the link (and reset the settings with it);
           the note lands in the fresh struct so the app still hears about the lost write. */
        mesh_log_warn("session", "Admin request send failed: %d", result);
        mesh_radio_settings_mark_unsent(&session->settings, &request, result);
        return;
    }
    mesh_radio_settings_mark_sent(&session->settings, request.packet_id, now_ms);
    mesh_log_info("session", "Sent admin request kind=%u type=%u id=%u", (unsigned)request.kind,
                  (unsigned)request.type, request.packet_id);
}

int mesh_session_send_packet(struct mesh_session *session, const uint8_t *packet, size_t len) {
    return mesh_session_send_raw(session, packet, len, 0U);
}

/* The node in the cache with this id, or NULL. */
static struct mesh_node_summary *mesh_session_find_node(struct mesh_session *session,
                                                        uint32_t node_id) {
    for (size_t i = 0; i < session->handshake.node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (session->handshake.nodes[i].node_id == node_id) {
            return &session->handshake.nodes[i];
        }
    }
    return NULL;
}

int mesh_session_set_node_ignored(struct mesh_session *session, uint32_t node_id, bool ignored) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    /* Ignoring the radio we are talking through would drop our own traffic. */
    if (node_id == session->handshake.my_info.my_node_num) {
        return -EINVAL;
    }

    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    if (summary->is_ignored == ignored) {
        return 0; /* already what the user asked for; nothing to send */
    }

    const int queued = mesh_radio_settings_queue_ignored(&session->settings, node_id, ignored);
    if (queued < 0) {
        return queued;
    }
    summary->is_ignored = ignored;
    mesh_log_info("session", "%s node 0x%08x in the NodeDB (%d requests)",
                  ignored ? "Ignoring" : "No longer ignoring", node_id, queued);
    return queued;
}

int mesh_session_request_node_info(struct mesh_session *session, uint32_t dest) {
    if (session == NULL || dest == 0U || dest == MESH_MESSAGE_BROADCAST_ADDR) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    if (dest == session->handshake.my_info.my_node_num) {
        return -EINVAL;
    }

    /*
     * The firmware answers a NODEINFO_APP carrying want_response with its own NodeInfo. What
     * we send is our own User record, which is also how the far end learns *our* name - the
     * exchange the phone apps offer is the same packet travelling in both directions.
     *
     * Which is exactly why there is no fallback here. A NodeInfo is applied by overwriting
     * the record wholesale - our own mesh_session_apply_user() blanks the names and drops the
     * public key when the incoming User does not carry them, and the firmware's NodeDB does
     * the same - so sending a placeholder User with nothing but an id in it would erase this
     * node's identity on every peer that received it. The owner arrives with our own NodeInfo
     * during the handshake, moments after config_complete, so refusing until then costs a
     * retry at worst.
     */
    if (!session->settings.has_owner) {
        return -EAGAIN;
    }
    const meshtastic_User user = session->settings.owner;

    uint8_t body[192];
    pb_ostream_t body_stream = pb_ostream_from_buffer(body, sizeof body);
    if (!pb_encode(&body_stream, meshtastic_User_fields, &user)) {
        mesh_log_error("session", "Failed to encode our User: %s", PB_GET_ERROR(&body_stream));
        return -EIO;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *packet = &to_radio.packet;
    packet->to = dest;
    packet->id = mesh_session_next_packet_id(session);
    packet->want_ack = false; /* the NodeInfo coming back is the answer */
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_NODEINFO_APP;
    packet->decoded.want_response = true;
    memcpy(packet->decoded.payload.bytes, body, body_stream.bytes_written);
    packet->decoded.payload.size = (pb_size_t)body_stream.bytes_written;

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("session", "Failed to encode NodeInfo request: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    const int result = mesh_session_send_raw(session, payload, stream.bytes_written, 0U);
    if (result == 0) {
        mesh_log_info("session", "Asked node 0x%08x to introduce itself", dest);
    }
    return result;
}

/*
 * Asks a node for its position or its telemetry, now, rather than waiting for its broadcast
 * interval to come round.
 *
 * Both are the same mechanism as the NodeInfo request above - an empty payload on the port
 * with `want_response` set - and both answer the question that made the node detail's readings
 * frustrating: a tracker broadcasts a position every fifteen minutes by default and telemetry
 * every half hour, so "where is it *now*" was a question the client could not ask.
 *
 * Unlike the NodeInfo request, neither carries anything of ours. A NodeInfo is applied by
 * overwriting the record wholesale, which is why that one refuses to go out with a placeholder;
 * a Position and a Telemetry are merged field by field at the far end and an empty one asserts
 * nothing, so there is nothing here to erase and no owner to wait for.
 */
static int mesh_session_request_on_port(struct mesh_session *session, uint32_t dest,
                                        meshtastic_PortNum portnum, const char *what) {
    if (session == NULL || dest == 0U || dest == MESH_MESSAGE_BROADCAST_ADDR) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    if (dest == session->handshake.my_info.my_node_num) {
        return -EINVAL;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *packet = &to_radio.packet;
    packet->to = dest;
    packet->id = mesh_session_next_packet_id(session);
    /* The reply is the acknowledgement. A want_ack on top of it would double the traffic this
       costs the mesh for a question that answers itself when it works. */
    packet->want_ack = false;
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = portnum;
    packet->decoded.want_response = true;
    packet->decoded.payload.size = 0U;

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("session", "Failed to encode %s request: %s", what, PB_GET_ERROR(&stream));
        return -EIO;
    }

    const int result = mesh_session_send_raw(session, payload, stream.bytes_written, 0U);
    if (result == 0) {
        mesh_log_info("session", "Asked node 0x%08x for its %s", dest, what);
    }
    return result;
}

int mesh_session_request_position(struct mesh_session *session, uint32_t dest) {
    return mesh_session_request_on_port(session, dest, meshtastic_PortNum_POSITION_APP, "position");
}

int mesh_session_request_telemetry(struct mesh_session *session, uint32_t dest) {
    return mesh_session_request_on_port(session, dest, meshtastic_PortNum_TELEMETRY_APP,
                                        "telemetry");
}

/*
 * Meshtastic packet ids only need to be unique per sender for a few minutes, so a cheap
 * xorshift seeded from the monotonic clock is enough. Zero is reserved by the protocol to mean
 * "no id", so it is never handed out.
 */
uint32_t mesh_session_next_packet_id(struct mesh_session *session) {
    if (session->next_packet_id == 0U) {
        uint32_t seed = (uint32_t)mesh_time_monotonic_ms();
        if (session->handshake.has_my_info) {
            seed ^= session->handshake.my_info.my_node_num;
        }
        session->next_packet_id = (seed == 0U) ? 0x9E3779B9U : seed;
    }

    uint32_t value = session->next_packet_id;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    session->next_packet_id = (value == 0U) ? 0x9E3779B9U : value;
    return session->next_packet_id;
}

/*
 * Every text packet this client originates: a plain message, a threaded reply, or a reaction.
 *
 * One function rather than three because the difference between them is two fields on the
 * wire, and splitting it would give the packet id, the log entry and the failure path three
 * copies each. The three public entry points below are the vocabulary; this is the mechanism.
 */
static int mesh_session_send_text_packet(struct mesh_session *session, uint32_t dest,
                                         uint8_t channel, const char *text, bool want_ack,
                                         uint32_t reply_id, bool is_reaction,
                                         uint32_t *out_packet_id) {
    if (session == NULL || text == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }

    /*
     * Broadcasts are never acked directly by the mesh; asking for one just wastes airtime. Nor
     * is a reaction: it has no bubble of its own - the transcript draws it as a chip on the
     * message it names - so a delivery mark it earned would be one nothing on the frame could
     * ever show, bought with a retransmit round on a shared band.
     */
    const bool broadcast = (dest == MESH_MESSAGE_BROADCAST_ADDR);
    const bool request_ack = want_ack && !broadcast && !is_reaction;

    struct mesh_message_text_request request = {
        .dest = dest,
        .packet_id = mesh_session_next_packet_id(session),
        .text = text,
        .channel = channel,
        .hop_limit = 0U,
        .want_ack = request_ack,
        .reply_id = reply_id,
        .is_reaction = is_reaction,
    };

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    size_t written = 0U;
    int encode_result = mesh_message_encode_text(&request, payload, sizeof payload, &written);
    if (encode_result < 0) {
        return encode_result;
    }

    /* Record before the send so a failure has something to mark. */
    struct mesh_message record;
    memset(&record, 0, sizeof record);
    record.packet_id = request.packet_id;
    record.from = session->handshake.has_my_info ? session->handshake.my_info.my_node_num : 0U;
    record.to = dest;
    record.channel = channel;
    record.direction = MESH_MESSAGE_OUTBOUND;
    record.ack = request_ack ? MESH_MESSAGE_ACK_PENDING : MESH_MESSAGE_ACK_NONE;
    /* The same two fields the echo will bring back, so the transcript shows the reply threaded
       and the reaction on its target from the moment the key was pressed rather than from
       whenever the radio gets round to echoing it. */
    record.reply_id = reply_id;
    record.is_reaction = is_reaction;
    snprintf(record.text, sizeof record.text, "%s", text);
    mesh_message_log_append(&session->messages, &record);

    int result = mesh_session_send_raw(session, payload, written, request.packet_id);
    if (result < 0) {
        mesh_message_log_mark_ack(&session->messages, request.packet_id, MESH_MESSAGE_ACK_FAILED,
                                  0U);
        return result;
    }

    if (out_packet_id != NULL) {
        *out_packet_id = request.packet_id;
    }
    if (reply_id != 0U) {
        mesh_log_info("session", "Queued %s id=%u about id=%u to 0x%08x on channel %u",
                      is_reaction ? "reaction" : "reply", request.packet_id, reply_id, dest,
                      (unsigned)channel);
    } else {
        mesh_log_info("session", "Queued text message id=%u to 0x%08x on channel %u",
                      request.packet_id, dest, (unsigned)channel);
    }
    return 0;
}

int mesh_session_send_text(struct mesh_session *session, uint32_t dest, uint8_t channel,
                           const char *text, bool want_ack, uint32_t *out_packet_id) {
    return mesh_session_send_text_packet(session, dest, channel, text, want_ack, 0U, false,
                                         out_packet_id);
}

int mesh_session_send_reply(struct mesh_session *session, uint32_t dest, uint8_t channel,
                            const char *text, bool want_ack, uint32_t reply_id,
                            uint32_t *out_packet_id) {
    return mesh_session_send_text_packet(session, dest, channel, text, want_ack, reply_id, false,
                                         out_packet_id);
}

int mesh_session_send_reaction(struct mesh_session *session, uint32_t dest, uint8_t channel,
                               const char *emoji, uint32_t reply_id, uint32_t *out_packet_id) {
    if (reply_id == 0U) {
        return -EINVAL;
    }
    return mesh_session_send_text_packet(session, dest, channel, emoji, false, reply_id, true,
                                         out_packet_id);
}

void mesh_session_packet_failed(struct mesh_session *session, uint32_t packet_id) {
    if (session == NULL || packet_id == 0U) {
        return;
    }
    mesh_message_log_mark_ack(&session->messages, packet_id, MESH_MESSAGE_ACK_FAILED, 0U);
}

int mesh_session_refresh_settings(struct mesh_session *session) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    return (int)mesh_radio_settings_queue_all(&session->settings);
}

int mesh_session_write_settings(struct mesh_session *session,
                                const struct mesh_admin_request *write) {
    if (session == NULL || write == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    const int queued = mesh_radio_settings_queue_write(&session->settings, write);
    if (queued > 0) {
        mesh_log_info("session", "Queued settings write kind=%u type=%u (%d requests)",
                      (unsigned)write->kind, (unsigned)write->type, queued);
    }
    return queued;
}

int mesh_session_set_node_favorite(struct mesh_session *session, uint32_t node_id, bool favorite) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }

    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    if (summary->is_favorite == favorite) {
        return 0; /* already what the user asked for; nothing to send */
    }

    const int queued = mesh_radio_settings_queue_favorite(&session->settings, node_id, favorite);
    if (queued < 0) {
        return queued;
    }
    summary->is_favorite = favorite;
    mesh_log_info("session", "%s node 0x%08x in the NodeDB (%d requests)",
                  favorite ? "Pinned" : "Unpinned", node_id, queued);
    return queued;
}

int mesh_session_toggle_node_muted(struct mesh_session *session, uint32_t node_id) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    const int queued = mesh_radio_settings_queue_toggle_muted(&session->settings, node_id);
    if (queued <= 0) {
        /* 0 is the same toggle already queued and deduplicated. The favorite and ignore pair
           can flip their cached flag anyway, because they send the state they want and a
           second request for it is a no-op; a toggle cannot. Flipping twice here for one
           toggle on the wire would leave the row stating the opposite of the truth. */
        return queued;
    }
    /* The wire verb is a toggle, so there is no wanted state to send and none to assume: the
       cached flag follows the request rather than leading it. */
    summary->is_muted = !summary->is_muted;
    mesh_log_info("session", "%s node 0x%08x in the NodeDB (%d requests)",
                  summary->is_muted ? "Muted" : "Unmuted", node_id, queued);
    return queued;
}

int mesh_session_remove_node(struct mesh_session *session, uint32_t node_id) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    /* Removing the radio we are talking through would take our own record out from under
       every screen that resolves a name through it. */
    if (node_id == session->handshake.my_info.my_node_num) {
        return -EINVAL;
    }
    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    const int queued = mesh_radio_settings_queue_remove_node(&session->settings, node_id);
    if (queued < 0) {
        return queued;
    }
    /* Drop it here too. There is no read-back, and an entry left in place would sit in the
       list looking removed-but-present until the next connection re-syncs the NodeDB. */
    const size_t index = (size_t)(summary - session->handshake.nodes);
    const size_t last = session->handshake.node_count - 1U;
    if (index < last) {
        memmove(&session->handshake.nodes[index], &session->handshake.nodes[index + 1U],
                (last - index) * sizeof session->handshake.nodes[0]);
    }
    memset(&session->handshake.nodes[last], 0, sizeof session->handshake.nodes[last]);
    session->handshake.node_count = last;
    mesh_log_info("session", "Removed node 0x%08x from the NodeDB (%d requests)", node_id, queued);
    return queued;
}

/* Our own record is the one every screen resolves a name through, and `my_info` is gone while
   the link is down - which is exactly when a roster is most likely to be cleared - so the
   roster's owner stands in for it. */
static uint32_t mesh_session_roster_self(const struct mesh_handshake_status *handshake,
                                         const struct mesh_session *session) {
    return handshake->has_my_info ? handshake->my_info.my_node_num : session->roster_node;
}

/*
 * Whether a forget would take this node. The one place that decides, so the count a row
 * advertises cannot disagree with what pressing it does: a roster whose off-radio nodes are all
 * pinned has nothing to drop, and a row saying "3 nodes" that drops none of them is worse than
 * no row at all.
 */
static bool mesh_session_node_is_forgettable(const struct mesh_node_summary *node, uint32_t my_node,
                                             bool only_off_nodedb) {
    if (node->is_favorite || (my_node != 0U && node->node_id == my_node)) {
        return false;
    }
    return !only_off_nodedb || !node->in_nodedb;
}

static size_t mesh_session_roster_len(const struct mesh_handshake_status *handshake) {
    return handshake->node_count > MESH_SESSION_MAX_NODES ? MESH_SESSION_MAX_NODES
                                                          : handshake->node_count;
}

int mesh_session_forget_nodes(struct mesh_session *session, bool only_off_nodedb) {
    if (session == NULL) {
        return -EINVAL;
    }
    struct mesh_handshake_status *handshake = &session->handshake;
    const uint32_t my_node = mesh_session_roster_self(handshake, session);
    const size_t count = mesh_session_roster_len(handshake);
    size_t kept = 0U;
    for (size_t i = 0; i < count; ++i) {
        if (mesh_session_node_is_forgettable(&handshake->nodes[i], my_node, only_off_nodedb)) {
            continue;
        }
        if (kept != i) {
            handshake->nodes[kept] = handshake->nodes[i];
        }
        ++kept;
    }
    const size_t dropped = count - kept;
    if (dropped == 0U) {
        return 0;
    }
    memset(&handshake->nodes[kept], 0, (count - kept) * sizeof handshake->nodes[0]);
    handshake->node_count = kept;
    /* The roster has room again, so the next node that does not fit is worth saying so about. */
    session->node_cache_warned = false;
    mesh_log_info(
        "session", "Forgot %zu cached node%s (%s); %zu kept", dropped, dropped == 1U ? "" : "s",
        only_off_nodedb ? "not in the radio's NodeDB" : "all but ourselves and pins", kept);
    return (int)dropped;
}

uint32_t mesh_session_forgettable_nodes(const struct mesh_session *session, bool only_off_nodedb) {
    if (session == NULL) {
        return 0U;
    }
    const struct mesh_handshake_status *handshake = &session->handshake;
    const uint32_t my_node = mesh_session_roster_self(handshake, session);
    const size_t count = mesh_session_roster_len(handshake);
    uint32_t forgettable = 0U;
    for (size_t i = 0; i < count; ++i) {
        if (mesh_session_node_is_forgettable(&handshake->nodes[i], my_node, only_off_nodedb)) {
            ++forgettable;
        }
    }
    return forgettable;
}

static int mesh_session_queue_fixed_position(struct mesh_session *session,
                                             const struct mesh_admin_request *write) {
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    return mesh_radio_settings_queue_write(&session->settings, write);
}

int mesh_session_set_fixed_position(struct mesh_session *session, int32_t latitude_i,
                                    int32_t longitude_i, bool has_altitude, int32_t altitude) {
    if (session == NULL) {
        return -EINVAL;
    }
    /* The same test the mesh's own fixes are held to - a coordinate typed on the keyboard and
       one decoded off the air are the same question, and two answers to it is how they drift. */
    if (!mesh_geo_coords_valid(latitude_i, longitude_i)) {
        return -EINVAL;
    }
    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_SET_FIXED_POSITION;
    write.type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
    write.payload.position.has_latitude_i = true;
    write.payload.position.latitude_i = latitude_i;
    write.payload.position.has_longitude_i = true;
    write.payload.position.longitude_i = longitude_i;
    write.payload.position.has_altitude = has_altitude;
    write.payload.position.altitude = has_altitude ? altitude : 0;
    /* Typed in by a person, which is exactly what LOC_MANUAL means; without it the firmware
       would report the fix as though a GPS had produced it. */
    write.payload.position.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
    const int queued = mesh_session_queue_fixed_position(session, &write);
    if (queued > 0) {
        mesh_log_info("session", "Fixed position set to %d, %d (%d requests)", (int)latitude_i,
                      (int)longitude_i, queued);
    }
    return queued;
}

int mesh_session_clear_fixed_position(struct mesh_session *session) {
    if (session == NULL) {
        return -EINVAL;
    }
    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_REMOVE_FIXED_POSITION;
    write.type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
    const int queued = mesh_session_queue_fixed_position(session, &write);
    if (queued > 0) {
        mesh_log_info("session", "Fixed position cleared (%d requests)", queued);
    }
    return queued;
}

/* For the log line only; the UI has its own labels. */
static const char *mesh_session_action_name(enum mesh_admin_request_kind kind) {
    switch (kind) {
    case MESH_ADMIN_REBOOT:
        return "reboot";
    case MESH_ADMIN_SHUTDOWN:
        return "shutdown";
    case MESH_ADMIN_RESET_NODEDB:
        return "nodedb reset";
    case MESH_ADMIN_FACTORY_RESET_CONFIG:
        return "factory reset (config)";
    case MESH_ADMIN_FACTORY_RESET_DEVICE:
        return "factory reset (device)";
    default:
        return "?";
    }
}

int mesh_session_radio_action(struct mesh_session *session, enum mesh_admin_request_kind kind) {
    if (session == NULL || !mesh_admin_request_is_action(kind)) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    const int queued =
        mesh_radio_settings_queue_action(&session->settings, kind, MESH_RADIO_ACTION_DELAY_SECONDS);
    if (queued < 0) {
        return queued;
    }
    /* Loud on purpose: this is the one thing the Settings tab does that cannot be undone by
       pressing the opposite row, and the log is what says who asked for it. */
    mesh_log_warn("session", "Requested %s of node 0x%08x (%d requests)",
                  mesh_session_action_name(kind), session->handshake.my_info.my_node_num, queued);
    return queued;
}

const struct mesh_handshake_status *mesh_session_handshake(const struct mesh_session *session) {
    return session != NULL ? &session->handshake : NULL;
}

const struct mesh_message_log *mesh_session_messages(const struct mesh_session *session) {
    return session != NULL ? &session->messages : NULL;
}

const struct mesh_waypoint_book *mesh_session_waypoints(const struct mesh_session *session) {
    return session != NULL ? &session->waypoints : NULL;
}

int mesh_session_send_waypoint(struct mesh_session *session, const struct mesh_waypoint *waypoint,
                               uint8_t channel, uint32_t *out_id) {
    if (session == NULL || waypoint == NULL) {
        return -EINVAL;
    }
    /* A place with no place is not one. Everything else about a waypoint is optional on the
       wire; this is the field the whole message exists to carry. */
    if (!waypoint->has_coords ||
        !mesh_geo_coords_valid(waypoint->latitude_i, waypoint->longitude_i)) {
        return -EINVAL;
    }

    struct mesh_waypoint outgoing = *waypoint;
    if (outgoing.id == 0U) {
        /* The same generator packet ids come from. Upstream ids are arbitrary uint32s chosen by
           whoever made the waypoint, and two clients picking the same one would be two places
           overwriting each other - which is the same collision a packet id already has to avoid,
           so it is the same seed rather than a second one. */
        outgoing.id = mesh_session_next_packet_id(session);
    }
    outgoing.channel = channel;
    /*
     * Whose place it is, and only for a place that does not have an owner yet.
     *
     * Re-sharing is a broadcast of somebody else's waypoint, unchanged: the packet goes out from
     * this radio, but the place is still theirs, and stamping it as ours here would have the
     * list say "you" under a name somebody else chose. A new place has `from` of 0, which is
     * what tells the two apart.
     */
    if (outgoing.from == 0U) {
        outgoing.from =
            session->handshake.has_my_info ? session->handshake.my_info.my_node_num : 0U;
        outgoing.ours = true;
    }
    if (outgoing.heard == 0U) {
        outgoing.heard = mesh_session_wall_clock();
    }

    struct mesh_waypoint_request request = {
        .waypoint = &outgoing,
        .packet_id = mesh_session_next_packet_id(session),
        .channel = channel,
    };

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    size_t written = 0U;
    const int encoded = mesh_waypoint_encode(&request, payload, sizeof payload, &written);
    if (encoded < 0) {
        return encoded;
    }

    /*
     * Stored before the send, and kept even if the send fails.
     *
     * A waypoint is not a message: there is no bubble to mark FAILED and nothing comes back to
     * mark it with - a broadcast is never acked. What the user did was name a place, and the
     * place is theirs whether or not this radio got the packet out; the next share re-broadcasts
     * the same id.
     */
    if (mesh_waypoint_book_store(&session->waypoints, &outgoing) == NULL) {
        return -ENOMEM;
    }
    if (out_id != NULL) {
        *out_id = outgoing.id;
    }

    /* -ENOTCONN *after* the place is in the book, which is the whole reason the link is not
       checked at the top of this function. Naming a place is not a message that failed to go
       out: it is a thing the user made, it is theirs with or without a radio, and the next
       share re-broadcasts the same id. The caller reports which of the two happened. */
    const int result = mesh_session_send_raw(session, payload, written, 0U);
    if (result < 0) {
        return result;
    }
    mesh_log_info("session", "Shared waypoint %u \"%s\" on channel %u", outgoing.id, outgoing.name,
                  (unsigned)channel);
    return 0;
}

int mesh_session_forget_waypoint(struct mesh_session *session, uint32_t id, bool *out_shared) {
    if (out_shared != NULL) {
        *out_shared = false;
    }
    if (session == NULL) {
        return -EINVAL;
    }
    struct mesh_waypoint *entry = mesh_waypoint_book_find(&session->waypoints, id);
    if (entry == NULL) {
        return -ENOENT;
    }

    /* `locked_to` of 0 means the mesh left it open to anyone; anything else names the one node
       entitled to change it. Withdrawing somebody else's locked place is not ours to do. */
    const uint32_t me =
        session->handshake.has_my_info ? session->handshake.my_info.my_node_num : 0U;
    const bool may_edit = (entry->locked_to == 0U) || (me != 0U && entry->locked_to == me);

    int result = 0;
    if (may_edit && session->send != NULL) {
        struct mesh_waypoint tombstone = *entry;
        tombstone.expire = MESH_WAYPOINT_EXPIRE_DELETED;
        struct mesh_waypoint_request request = {
            .waypoint = &tombstone,
            .packet_id = mesh_session_next_packet_id(session),
            .channel = entry->channel,
        };
        uint8_t payload[MESH_SESSION_MAX_PACKET];
        size_t written = 0U;
        result = mesh_waypoint_encode(&request, payload, sizeof payload, &written);
        if (result == 0) {
            result = mesh_session_send_raw(session, payload, written, 0U);
        }
        if (result == 0) {
            if (out_shared != NULL) {
                *out_shared = true;
            }
            mesh_log_info("session", "Withdrew waypoint %u from channel %u", id,
                          (unsigned)tombstone.channel);
        }
    }

    /* Dropped whatever the broadcast did: the user asked for this place to go, and a radio that
       could not carry the news does not put it back. */
    mesh_waypoint_book_forget(&session->waypoints, id);
    return result;
}

uint32_t mesh_session_forget_conversation(struct mesh_session *session, uint32_t peer,
                                          uint8_t channel) {
    if (session == NULL) {
        return 0U;
    }
    return mesh_message_log_forget(&session->messages, peer, channel);
}

const struct mesh_radio_settings *mesh_session_settings(const struct mesh_session *session) {
    return session != NULL ? &session->settings : NULL;
}

const struct mesh_radio_stats *mesh_session_radio_stats(const struct mesh_session *session) {
    return session != NULL ? &session->stats : NULL;
}

const struct mesh_client_notification *
mesh_session_notification(const struct mesh_session *session) {
    return session != NULL ? &session->notification : NULL;
}

const struct mesh_queue_status *mesh_session_queue_status(const struct mesh_session *session) {
    return session != NULL ? &session->queue : NULL;
}

int mesh_session_send_traceroute(struct mesh_session *session, uint32_t dest) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }
    /* A broadcast trace would ask the whole mesh to answer at once, and tracing the route to
       ourselves is a question with no links in it. */
    if (dest == 0U || dest == MESH_MESSAGE_BROADCAST_ADDR ||
        (session->handshake.has_my_info && dest == session->handshake.my_info.my_node_num)) {
        return -EINVAL;
    }
    if (session->traceroute.state == MESH_TRACEROUTE_PENDING) {
        return -EBUSY;
    }

    /* An empty RouteDiscovery: every node that forwards it appends itself, so what we send is
       the question and what comes back is the answer. */
    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_default;
    uint8_t body[64];
    pb_ostream_t body_stream = pb_ostream_from_buffer(body, sizeof body);
    if (!pb_encode(&body_stream, meshtastic_RouteDiscovery_fields, &route)) {
        mesh_log_error("session", "Failed to encode RouteDiscovery: %s",
                       PB_GET_ERROR(&body_stream));
        return -EIO;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *packet = &to_radio.packet;
    packet->to = dest;
    packet->id = mesh_session_next_packet_id(session);
    packet->want_ack = false; /* the reply is the ack; a Routing ack as well is just airtime */
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_TRACEROUTE_APP;
    packet->decoded.want_response = true;
    memcpy(packet->decoded.payload.bytes, body, body_stream.bytes_written);
    packet->decoded.payload.size = (pb_size_t)body_stream.bytes_written;

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("session", "Failed to encode traceroute: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    struct mesh_traceroute *trace = &session->traceroute;
    memset(trace, 0, sizeof *trace);
    trace->state = MESH_TRACEROUTE_PENDING;
    trace->target = dest;
    trace->packet_id = packet->id;
    trace->sent_ms = mesh_time_monotonic_ms();

    const int result = mesh_session_send_raw(session, payload, stream.bytes_written, 0U);
    if (result < 0) {
        trace->state = MESH_TRACEROUTE_TIMEOUT;
        return result;
    }
    mesh_log_info("session", "Traceroute to 0x%08x sent (id %u)", dest, trace->packet_id);
    return 0;
}

const struct mesh_traceroute *mesh_session_traceroute(const struct mesh_session *session) {
    return session != NULL ? &session->traceroute : NULL;
}

/*
 * Puts one Store & Forward request on the air: the history request when we know a router, and
 * the broadcast ping that looks for one when we do not.
 *
 * The ping is the only thing this client ever broadcasts on the port, and it is one empty
 * packet. The history request that follows is always addressed - see
 * mesh_store_forward_encode() for why a broadcast one would be an act of vandalism.
 */
static int mesh_session_send_store_forward(struct mesh_session *session, bool ping,
                                           uint64_t now_ms) {
    struct mesh_store_forward *sf = &session->store_forward;
    struct mesh_store_forward_request request = {
        .dest = ping ? MESH_MESSAGE_BROADCAST_ADDR : sf->router,
        .packet_id = mesh_session_next_packet_id(session),
        .channel = ping ? 0U : sf->router_channel,
        /* 0: the router's own configured window. See the field's comment. */
        .window_minutes = 0U,
        .cursor = ping ? 0U : sf->cursor,
        .ping = ping,
    };

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    size_t written = 0U;
    int result = mesh_store_forward_encode(&request, payload, sizeof payload, &written);
    if (result < 0) {
        mesh_store_forward_send_failed(sf);
        return result;
    }
    result = mesh_session_send_raw(session, payload, written, 0U);
    if (result < 0) {
        mesh_store_forward_send_failed(sf);
        return result;
    }
    mesh_store_forward_sent(sf, request.dest, request.channel, now_ms, ping);
    if (ping) {
        mesh_log_info("session", "Looking for a Store & Forward router");
    } else {
        mesh_log_info("session", "Asked router 0x%08x for the history from %u", request.dest,
                      request.cursor);
    }
    return 0;
}

int mesh_session_request_history(struct mesh_session *session) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }
    struct mesh_store_forward *sf = &session->store_forward;
    /*
     * A second press while one is running would restart the count against a replay that is
     * still arriving, and on a router that answers CLIENT_HISTORY by replaying its window it
     * would ask for the same window twice. -EBUSY is the traceroute's rule for the same reason:
     * the client's own half of a rate limit the firmware also has.
     */
    if (sf->state == (uint8_t)MESH_STORE_FORWARD_SEEKING ||
        sf->state == (uint8_t)MESH_STORE_FORWARD_REQUESTED ||
        sf->state == (uint8_t)MESH_STORE_FORWARD_REPLAYING) {
        return -EBUSY;
    }
    return mesh_session_send_store_forward(session, sf->router == 0U, mesh_time_monotonic_ms());
}

const struct mesh_store_forward *mesh_session_store_forward(const struct mesh_session *session) {
    return session != NULL ? &session->store_forward : NULL;
}
