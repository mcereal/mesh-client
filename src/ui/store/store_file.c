#define _POSIX_C_SOURCE 200809L

/*
 * The handshake cache: the file the client writes at shutdown and reads at launch.
 *
 * It is the reason a Brick with no radio in range still opens on a roster, a transcript and a
 * trend rather than on an empty list, and it is also what seeds the session's node roster at
 * startup (docs/architecture.md#the-node-roster). What it holds and what it deliberately does
 * not is in docs/ui.md; the keys are include/mesh/ui/store_keys.def and the rules for adding
 * one are at the top of that file.
 *
 * Split out of store.c because it is a different subject with a different audience. store.c is
 * the state machine - what the store holds, what moved, who gets told - and this is a *format*,
 * with compatibility rules of its own and a user-editable text file at the far end of it.
 * Neither half ever needed much from the other; the whole seam is store_internal.h.
 *
 * The two halves here are mirrors and are meant to be read side by side:
 *
 *   mesh_ui_store_save()  spells every key through store_keys.h's writers
 *   mesh_ui_store_load()  switches on mesh_ui_store_key_lookup() and reads the value through
 *                         store_fields.h's typed field lists
 *
 * A key's spelling therefore exists once (store_keys.def) and a value's shape exists once per
 * direction - the writer's format string and the loader's field list, which read as each
 * other's reflection. The loader's switch has no `default`, deliberately: a key added to the
 * .def and never read is then a -Wswitch on this file rather than a line that goes out every
 * save and comes back as nothing.
 */

#include "mesh/ui/store.h"

#include "store_internal.h"

/* For MESH_TRACEROUTE_DONE: a logged route is a finished one, which is the whole of what this
   file has to say about the state a record loads with. */
#include "mesh/core/session.h"
#include "mesh/geo/coords.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store_fields.h"
#include "mesh/ui/store_keys.h"
#include "mesh/utils/array.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Room for the cache path plus the `.tmp` the save writes beside it. Comfortably above the 256
   struct mesh_app gives the path itself, so the guard below is a bound rather than a limit
   anything real runs into; it is here rather than shared because the save is the only caller
   that has to name a second file next to the one it was handed. */
#define MESH_UI_STORE_TEMP_PATH_MAX 1024

/* ---- saving -------------------------------------------------------------------------------- */

static int mesh_ui_store_save_handshake(FILE *file,
                                        const struct mesh_ui_handshake_state *handshake) {
    if (file == NULL || handshake == NULL) {
        return 0;
    }

    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_REQUEST, "%u,%u",
                        handshake->request_in_flight ? 1U : 0U, handshake->request_id);
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_CONFIG, "%u,%u,%u",
                        handshake->config_complete ? 1U : 0U, handshake->config_complete_id,
                        handshake->has_config ? 1U : 0U);
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_MYNODE, "%u,%u,%u,%u",
                        handshake->has_my_info ? 1U : 0U, handshake->my_info.node_num,
                        handshake->my_info.nodedb_entries, handshake->my_info.reboot_count);
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_ROSTER, "%u", handshake->roster_owner);
    mesh_ui_store_write_text(file, MESH_UI_STORE_KEY_HANDSHAKE_CHANNEL, handshake->primary_channel);
    mesh_ui_store_write_text(file, MESH_UI_STORE_KEY_HANDSHAKE_MY_SHORT, handshake->my_short_name);
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_CACHED, "%u",
                        handshake->cached ? 1U : 0U);
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_CHANNELS, "%u", handshake->channel_count);
    for (uint32_t i = 0; i < handshake->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
        const struct mesh_ui_channel *channel = &handshake->channels[i];
        mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_CHANNEL, i, "%u,%u",
                                (unsigned)channel->index, (unsigned)channel->role);
        mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_CHANNEL_NAME, i, channel->name);
    }
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_NODES, "%u", handshake->node_count);
    for (uint32_t i = 0; i < handshake->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE, i, "%u,%u,%u,%f,%u,%u", node->node_id,
                                node->last_heard, node->has_hops_away ? 1U : 0U, (double)node->snr,
                                node->via_mqtt ? 1U : 0U, node->hops_away);
        mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_NODE_LONG, i, node->long_name);
        mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_NODE_SHORT, i, node->short_name);

        /* The detail the Nodes tab drills into. Written as separate keys rather than widened
           onto node[] so an older build reading a newer cache skips what it does not know and
           a newer build reading an older one simply finds nothing to fill in. */
        mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_NODE_USER, i, node->user_id);
        mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE_IDENT, i, "%u,%u,%u,%u,%u,%u,%u",
                                node->hw_model, node->role, node->is_licensed ? 1U : 0U,
                                node->is_unmessagable ? 1U : 0U, node->is_favorite ? 1U : 0U,
                                node->is_ignored ? 1U : 0U, (unsigned)node->channel);
        /* What the roster knows that the radio did not tell us this run: whether the name is
           real and whether the radio still carried the node. Both are why a restored roster is
           worth more than a re-sync. */
        /* The verified bit is appended last so a cache written by an older build still loads,
           the way node_pos[] takes seven fields or eight. It is worth keeping across a restart
           for the same reason the roster is: the node the radio has evicted is exactly the one
           whose proven key we would otherwise have to establish again, and it is the key an
           add-contact would hand back to the radio. */
        mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE_STATE, i, "%u,%u,%u",
                                node->has_user ? 1U : 0U, node->in_nodedb ? 1U : 0U,
                                node->key_verified ? 1U : 0U);
        if (node->public_key_len > 0U) {
            char pubkey[2U * sizeof node->public_key + 1U];
            mesh_ui_settings_key_hex(node->public_key, node->public_key_len, pubkey, sizeof pubkey);
            mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_NODE_KEY, i, pubkey);
        }
        /* Its own key rather than a widened node[] line: the loader matches node[] on an exact
           field count, so a build that predates this one would drop the whole node rather than
           the one value it does not know. */
        if (node->has_rssi) {
            mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE_RSSI, i, "%d", (int)node->rx_rssi);
        }
        if (node->position.valid) {
            /* `received` is appended last so a cache written by an older build still loads:
               the reader takes seven fields or eight, and a line with seven leaves it 0. */
            mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE_POS, i, "%d,%d,%u,%d,%u,%u,%u,%u",
                                    node->position.latitude_i, node->position.longitude_i,
                                    node->position.has_altitude ? 1U : 0U, node->position.altitude,
                                    node->position.time, (unsigned)node->position.sats_in_view,
                                    (unsigned)node->position.precision_bits,
                                    node->position.received);
        }
        if (node->metrics.valid) {
            mesh_ui_store_write_row(
                file, MESH_UI_STORE_KEY_NODE_METRICS, i, "%u,%u,%u,%u,%f,%u,%f,%u,%f,%u,%u",
                node->metrics.time, node->metrics.has_battery ? 1U : 0U,
                (unsigned)node->metrics.battery_level, node->metrics.has_voltage ? 1U : 0U,
                (double)node->metrics.voltage, node->metrics.has_channel_utilization ? 1U : 0U,
                (double)node->metrics.channel_utilization, node->metrics.has_air_util_tx ? 1U : 0U,
                (double)node->metrics.air_util_tx, node->metrics.has_uptime ? 1U : 0U,
                node->metrics.uptime_seconds);
        }
        if (node->environment.valid) {
            mesh_ui_store_write_row(
                file, MESH_UI_STORE_KEY_NODE_ENV, i, "%u,%u,%f,%u,%f,%u,%f,%u,%u,%u,%f,%u,%f,%u,%f",
                node->environment.time, node->environment.has_temperature ? 1U : 0U,
                (double)node->environment.temperature, node->environment.has_humidity ? 1U : 0U,
                (double)node->environment.relative_humidity,
                node->environment.has_pressure ? 1U : 0U,
                (double)node->environment.barometric_pressure, node->environment.has_iaq ? 1U : 0U,
                (unsigned)node->environment.iaq, node->environment.has_lux ? 1U : 0U,
                (double)node->environment.lux, node->environment.has_voltage ? 1U : 0U,
                (double)node->environment.voltage, node->environment.has_current ? 1U : 0U,
                (double)node->environment.current);
        }
        /* The four groups beyond device metrics and environment. Each gets its own key for the
           reason the three above do: a cache written by a build that had them is read by one
           that does not simply by skipping a line it does not recognise. */
        /* One line per neighbour rather than one line for the list: a cache line is parsed
           against a fixed field list, and a variable-length list in one would have to be
           re-parsed by hand for a count that upstream can change. */
        if (node->neighbors.valid) {
            mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE_NBRS, i, "%u,%u,%u",
                                    node->neighbors.time, node->neighbors.broadcast_interval_secs,
                                    (unsigned)node->neighbors.count);
            for (uint8_t n = 0; n < node->neighbors.count && n < MESH_UI_MAX_NEIGHBORS; ++n) {
                mesh_ui_store_write_slot(file, MESH_UI_STORE_KEY_NODE_NBR, i, (uint32_t)n, "%u,%f",
                                         node->neighbors.entries[n].node_id,
                                         (double)node->neighbors.entries[n].snr);
            }
        }
        if (node->power.valid) {
            mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE_POWER, i,
                                    "%u,%u,%f,%u,%f,%u,%f,%u,%f,%u,%f,%u,%f", node->power.time,
                                    node->power.channel[0].has_voltage ? 1U : 0U,
                                    (double)node->power.channel[0].voltage,
                                    node->power.channel[0].has_current ? 1U : 0U,
                                    (double)node->power.channel[0].current,
                                    node->power.channel[1].has_voltage ? 1U : 0U,
                                    (double)node->power.channel[1].voltage,
                                    node->power.channel[1].has_current ? 1U : 0U,
                                    (double)node->power.channel[1].current,
                                    node->power.channel[2].has_voltage ? 1U : 0U,
                                    (double)node->power.channel[2].voltage,
                                    node->power.channel[2].has_current ? 1U : 0U,
                                    (double)node->power.channel[2].current);
        }
        if (node->air_quality.valid) {
            mesh_ui_store_write_row(
                file, MESH_UI_STORE_KEY_NODE_AIR, i, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%f,%u,%f",
                node->air_quality.time, node->air_quality.has_pm10 ? 1U : 0U,
                (unsigned)node->air_quality.pm10_standard, node->air_quality.has_pm25 ? 1U : 0U,
                (unsigned)node->air_quality.pm25_standard, node->air_quality.has_pm100 ? 1U : 0U,
                (unsigned)node->air_quality.pm100_standard, node->air_quality.has_co2 ? 1U : 0U,
                (unsigned)node->air_quality.co2, node->air_quality.has_voc_index ? 1U : 0U,
                (double)node->air_quality.voc_index, node->air_quality.has_nox_index ? 1U : 0U,
                (double)node->air_quality.nox_index);
        }
        if (node->health.valid) {
            mesh_ui_store_write_row(
                file, MESH_UI_STORE_KEY_NODE_HEALTH, i, "%u,%u,%u,%u,%u,%u,%f", node->health.time,
                node->health.has_heart_bpm ? 1U : 0U, (unsigned)node->health.heart_bpm,
                node->health.has_spo2 ? 1U : 0U, (unsigned)node->health.spo2,
                node->health.has_temperature ? 1U : 0U, (double)node->health.temperature);
        }
        if (node->host.valid) {
            mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_NODE_HOST, i,
                                    "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", node->host.time,
                                    node->host.has_uptime ? 1U : 0U, node->host.uptime_seconds,
                                    node->host.has_freemem ? 1U : 0U, node->host.freemem_kib,
                                    node->host.has_diskfree ? 1U : 0U, node->host.diskfree_mib,
                                    node->host.has_load ? 1U : 0U, node->host.load1,
                                    node->host.load5, node->host.load15);
        }
    }

    return 0;
}

/*
 * One message, as the five lines both formats spell it.
 *
 * Shared with the archive (mesh/ui/store_archive.h) rather than written out twice, which is the
 * same reason a key's text lives in store_keys.def: a record that meant one thing in the cache
 * and another in a conversation's log would be two formats wearing one name. `index` groups the
 * five lines and nothing else - the cache numbers its rows, the archive counts records within a
 * run, and neither reading depends on the other.
 */
void mesh_ui_store_write_message(FILE *file, uint32_t index,
                                 const struct mesh_ui_message *message) {
    if (file == NULL || message == NULL) {
        return;
    }
    mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_MSG, index, "%u,%u,%u,%u,%u,%u,%u",
                            message->packet_id, message->peer, message->rx_time,
                            (unsigned)message->channel, (unsigned)message->direction,
                            (unsigned)message->ack, message->broadcast ? 1U : 0U);

    /* What the message *is*, as opposed to where it came from. On its own key rather than
       widened onto msg[] for the reason the node detail's groups are: the loader matches
       msg[] on an exact field count, so a build that predates this would drop the whole
       message rather than the part it does not know.

       Losing this line is not cosmetic. A reaction reloaded without `is_reaction` is a
       bubble containing a bare emoji that also bumps the unread count - which is precisely
       the behaviour reading Data.emoji was meant to end, returning at every restart.

       `ack_error` rides here rather than on msg[] for that same reason, and it is the line's
       one field that is allowed to be absent: the loader takes four fields or five, so a
       record written before it existed still reads. Without it a message that failed came
       back carrying FAILED and no reason, and the note under the bubble fell from "No route"
       to the bare word - the transcript forgetting, at every launch, the one thing a failed
       message is worth keeping. */
    mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_MSG_META, index, "%u,%u,%u,%u,%u",
                            (unsigned)message->kind, message->pki_encrypted ? 1U : 0U,
                            message->reply_id, message->is_reaction ? 1U : 0U,
                            (unsigned)message->ack_error);

    mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_MSG_NAME, index, message->peer_name);
    /* The relay's name, written only when there is one - it is absent from most messages on
       most meshes, and an empty line each would be a third of the file.

       The resolved name rather than MeshPacket.relay_node's byte, because that is what the
       store holds: resolving needs the roster, and the roster the *next* run loads is the
       one from the cache rather than the one that was in front of us when the packet
       landed. A name written down is the route the message took; a byte re-resolved later
       would be this run's guess at it. */
    if (message->relay_name[0] != '\0') {
        mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_MSG_RELAY, index, message->relay_name);
    }
    mesh_ui_store_write_row_text(file, MESH_UI_STORE_KEY_MSG_TEXT, index, message->text);
}

static void mesh_ui_store_save_messages(FILE *file, const struct mesh_ui_message_list *messages) {
    if (file == NULL || messages == NULL) {
        return;
    }

    mesh_ui_store_write(file, MESH_UI_STORE_KEY_MESSAGES, "%u,%u", messages->count,
                        messages->dropped);
    for (uint32_t i = 0; i < messages->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        mesh_ui_store_write_message(file, i, &messages->entries[i]);
    }
}

/* One line per conversation the client remembers anything about. The stamp is not written: load
   order stands in for it, which is all the eviction ordering needs.

   The mute is a fifth field rather than a line of its own, and it is appended rather than
   inserted, so a file written before it existed still parses - see the loader, which takes four
   fields or five. */
static void mesh_ui_store_save_read_state(FILE *file, const struct mesh_ui_read_state *state) {
    if (file == NULL || state == NULL) {
        return;
    }
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_READ_MARKS, "%u", state->count);
    for (uint32_t i = 0; i < state->count && i < MESH_UI_READ_MARKS_MAX; ++i) {
        const struct mesh_ui_read_mark *mark = &state->marks[i];
        mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_READ, i, "%u,%u,%u,%u,%u",
                                (unsigned)mark->kind, (unsigned)mark->channel, mark->node,
                                mark->packet_id, mark->muted ? 1U : 0U);
    }
}

/*
 * The radio's airtime trend, as ages rather than as stamps.
 *
 * A sample's time is CLOCK_MONOTONIC, which counts from boot, so the number itself means
 * nothing to the next run - what survives a restart is how far apart the readings were. Ages
 * are taken from the newest sample, so the newest is 0 and the file reads oldest-first in the
 * order the loader pushes them.
 */
static void mesh_ui_store_save_history(FILE *file, const struct mesh_ui_history *history) {
    if (file == NULL || history == NULL) {
        return;
    }
    const struct mesh_ui_airtime_sample *newest = mesh_ui_history_airtime_newest(history);
    if (newest == NULL) {
        return;
    }
    const uint32_t count = mesh_ui_history_airtime_count(history);
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_AIRTIME_COUNT, "%u", count);
    for (uint32_t i = 0U; i < count; ++i) {
        const struct mesh_ui_airtime_sample *sample = mesh_ui_history_airtime_at(history, i);
        mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_AIRTIME, i, "%u,%d,%d,%u",
                                newest->time - sample->time, (int)sample->utilization,
                                (int)sample->tx, sample->gap ? 1U : 0U);
    }
}

/*
 * The routes this client has measured, hop by hop.
 *
 * Small enough to ride the cache rather than earn a file of its own: eight routes of ten stops
 * is a few hundred lines in the worst case and a dozen in the ordinary one, against a roster of
 * 128 nodes that is already rewritten whole on every save.
 *
 * `completed` is written as it stands because it is a *wall* clock - mesh_session_wall_clock(),
 * the radio's time - so unlike the airtime trend beside it there is nothing to convert into an
 * age: the stamp still means what it meant, and a run that never learns the time draws the age
 * as unknown exactly as it would have in the run that measured it.
 */
static void mesh_ui_store_save_traceroutes(FILE *file, const struct mesh_ui_traceroute_log *log) {
    if (file == NULL || log == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < log->count && i < MESH_UI_TRACEROUTE_LOG_MAX; ++i) {
        const struct mesh_ui_traceroute *trace = &log->entries[i];
        if (trace->target == 0U || trace->forward_count == 0U) {
            continue;
        }
        mesh_ui_store_write_row(file, MESH_UI_STORE_KEY_TRACE, i, "%u,%u", trace->target,
                                trace->completed);
        for (uint32_t direction = 0U; direction < 2U; ++direction) {
            const struct mesh_ui_traceroute_hop *path =
                direction == 0U ? trace->forward : trace->back;
            const uint8_t count = direction == 0U ? trace->forward_count : trace->back_count;
            for (uint8_t hop = 0U; hop < count && hop < MESH_UI_TRACEROUTE_MAX_HOPS; ++hop) {
                const uint32_t slot = direction * MESH_UI_TRACEROUTE_MAX_HOPS + hop;
                mesh_ui_store_write_slot(file, MESH_UI_STORE_KEY_TRACE_HOP, i, slot, "%u,%u,%d",
                                         path[hop].node_id, path[hop].has_snr ? 1U : 0U,
                                         (int)path[hop].snr_quarter_db);
                mesh_ui_store_write_slot_text(file, MESH_UI_STORE_KEY_TRACE_HOP_NAME, i, slot,
                                              path[hop].name);
            }
        }
    }
}

/*
 * Through a temporary beside it, then rename(), exactly as the archive and the trend log are
 * written - and for the reason those two gave for not doing it here.
 *
 * The old argument was that this file is "a snapshot the next publish rebuilds". That is true
 * of every section but one. The roster is the record of nodes the *radio* no longer carries -
 * a NodeDB that evicts, or one a factory reset emptied - and no publish can rebuild it, because
 * the only place those nodes still exist is this file (mesh_app_seed_nodes_from_cache()). So an
 * interrupted save cost the client the one thing in here nothing else holds.
 *
 * And it was interrupted easily. The save runs every two seconds for as long as anything is
 * moving, fopen(path, "w") empties the file before the first byte is written, and the handheld
 * at the far end is switched off with a button. A cut anywhere in that window left a truncated
 * cache; a cut near the start of it left an empty one, which reads back as handshake_valid=0 -
 * the whole roster, gone, from a radio that was only ever doing what its NodeDB does.
 */
int mesh_ui_store_save(const struct mesh_ui_store *store, const char *path) {
    if (store == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    char temp[MESH_UI_STORE_TEMP_PATH_MAX];
    const int named = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (named <= 0 || named >= (int)sizeof temp) {
        return -ENAMETOOLONG;
    }

    FILE *file = fopen(temp, "w");
    if (file == NULL) {
        return -errno;
    }

    mesh_ui_store_write(file, MESH_UI_STORE_KEY_HANDSHAKE_VALID, "%u",
                        store->handshake_valid ? 1U : 0U);
    if (store->handshake_valid) {
        mesh_ui_store_save_handshake(file, &store->handshake);
    }
    mesh_ui_store_save_messages(file, &store->messages);
    mesh_ui_store_save_read_state(file, &store->read_state);
    mesh_ui_store_save_history(file, &store->history);
    mesh_ui_store_save_traceroutes(file, &store->traceroutes);

    int result = ferror(file) ? -EIO : 0;
    /*
     * Flushed and on the card before the rename, not just handed to the kernel. A rename is
     * atomic against the *directory*, which on its own only promises that a reader sees one
     * name or the other - on the FAT volume a Brick keeps its userdata on, a rename that landed
     * ahead of the data would publish the new name over blocks that are still the old file's,
     * or zeros. This is the one place that ordering is worth a stall, because it is the one
     * file here that cannot be rebuilt.
     */
    if (result == 0 && fflush(file) != 0) {
        result = -errno;
    }
    if (result == 0 && fsync(fileno(file)) != 0) {
        result = -errno;
    }
    if (fclose(file) != 0 && result == 0) {
        result = -errno;
    }
    if (result != 0) {
        (void)unlink(temp);
        return result;
    }

    if (rename(temp, path) != 0) {
        result = -errno;
        (void)unlink(temp);
        return result;
    }
    return 0;
}

/* ---- loading ------------------------------------------------------------------------------- */

/* One saved airtime reading, between the file and the history. Its own type because a sample's
   place on the timeline is not known until the whole list is in; see mesh_ui_store_load(). */
struct mesh_ui_store_cached_airtime {
    uint32_t age_ms;
    int32_t utilization;
    int32_t tx;
    bool gap;
};

/*
 * One pass over the file, before any of it is a store.
 *
 * Collected rather than applied line by line, because the decisions the format leaves to the
 * end cannot be taken until the last line is in: how long each list really is, and where the
 * airtime trend's oldest sample sits.
 */
struct mesh_ui_store_cache {
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    /* What the header claimed against what actually arrived. The loader trusts the header for
       the length - trailing rows may legitimately be all-zero - but a header that does not
       parse leaves `claimed` unset and the rows decide. */
    uint32_t nodes_claimed;
    bool nodes_claimed_set;
    uint32_t nodes_loaded;

    struct mesh_ui_message_list messages;
    uint32_t messages_claimed;
    bool messages_claimed_set;
    uint32_t messages_loaded;

    struct mesh_ui_read_state read_state;

    struct mesh_ui_store_cached_airtime airtime[MESH_UI_HISTORY_AIRTIME_MAX];
    uint32_t airtime_loaded;

    /* The routes, in the order the file carries them, which is the order they were measured in:
       the log writes newest first and the loader fills the same slots. */
    struct mesh_ui_traceroute_log traceroutes;
};

/*
 * The row a `name[i]` key names, or NULL when the file names one past the end.
 *
 * Every row loader below takes the result and tolerates NULL, so the bounds check happens once
 * per key rather than once per field - and a hand-edited `node[9999]` is a line that does
 * nothing rather than a write past a roster.
 */
static struct mesh_ui_node_summary *cache_node(struct mesh_ui_store_cache *cache, uint32_t index) {
    return index < MESH_UI_MAX_HANDSHAKE_NODES ? &cache->handshake.nodes[index] : NULL;
}

static struct mesh_ui_channel *cache_channel(struct mesh_ui_store_cache *cache, uint32_t index) {
    return index < MESH_UI_MAX_CHANNELS ? &cache->handshake.channels[index] : NULL;
}

static struct mesh_ui_message *cache_message(struct mesh_ui_store_cache *cache, uint32_t index) {
    return index < MESH_UI_MAX_MESSAGES ? &cache->messages.entries[index] : NULL;
}

static struct mesh_ui_traceroute *cache_traceroute(struct mesh_ui_store_cache *cache,
                                                   uint32_t index) {
    return index < MESH_UI_TRACEROUTE_LOG_MAX ? &cache->traceroutes.entries[index] : NULL;
}

/*
 * The whole value, or nothing.
 *
 * Most keys are read this way, and that is the format's central rule rather than strictness for
 * its own sake: a line widens by growing a *key*, never a field, so a field count that does not
 * match is a record some other build meant something else by. The few keys that appended a
 * field on the end compare the return of mesh_ui_store_fields_read() themselves, and say so.
 */
static bool cache_fields(const char *value, const struct mesh_ui_store_field *fields,
                         size_t count) {
    return mesh_ui_store_fields_read(value, fields, count) == count;
}

/* A key whose whole value is one field. */
static bool cache_field(const char *value, struct mesh_ui_store_field field) {
    return mesh_ui_store_fields_read(value, &field, 1U) == 1U;
}

/* A value that is text rather than numbers, already unescaped by the caller. */
static void cache_text(char *dst, size_t cap, const char *value) {
    if (dst != NULL && cap > 0U) {
        snprintf(dst, cap, "%s", value);
    }
}

static void load_handshake_request(struct mesh_ui_handshake_state *handshake, const char *value) {
    bool in_flight = false;
    uint32_t request_id = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&in_flight),
        MESH_UI_STORE_FIELD(&request_id),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    handshake->request_in_flight = in_flight;
    handshake->request_id = request_id;
}

static void load_handshake_config(struct mesh_ui_handshake_state *handshake, const char *value) {
    bool complete = false;
    uint32_t complete_id = 0U;
    bool has_config = false;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&complete),
        MESH_UI_STORE_FIELD(&complete_id),
        MESH_UI_STORE_FIELD(&has_config),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    handshake->config_complete = complete;
    handshake->config_complete_id = complete_id;
    handshake->has_config = has_config;
}

static void load_handshake_mynode(struct mesh_ui_handshake_state *handshake, const char *value) {
    bool has_my_info = false;
    struct mesh_ui_my_info info = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&has_my_info),
        MESH_UI_STORE_FIELD(&info.node_num),
        MESH_UI_STORE_FIELD(&info.nodedb_entries),
        MESH_UI_STORE_FIELD(&info.reboot_count),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    handshake->has_my_info = has_my_info;
    handshake->my_info = info;
}

static void load_handshake_channels(struct mesh_ui_handshake_state *handshake, const char *value) {
    uint32_t count = 0U;
    if (!cache_field(value, MESH_UI_STORE_FIELD(&count))) {
        return;
    }
    handshake->channel_count = count > MESH_UI_MAX_CHANNELS ? MESH_UI_MAX_CHANNELS : count;
}

static void load_channel(struct mesh_ui_channel *channel, const char *value) {
    if (channel == NULL) {
        return;
    }
    uint8_t slot = 0U;
    uint8_t role = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&slot),
        MESH_UI_STORE_FIELD(&role),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    channel->index = slot;
    channel->role = role;
}

static void load_channel_name(struct mesh_ui_channel *channel, const char *value) {
    if (channel != NULL) {
        cache_text(channel->name, sizeof channel->name, value);
    }
}

/* The base row: what the Nodes list draws before anything is drilled into. Everything else a
   node has arrives on a key of its own. */
static bool load_node(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return false;
    }
    uint32_t node_id = 0U;
    uint32_t last_heard = 0U;
    bool has_hops = false;
    float snr = 0.0f;
    bool via_mqtt = false;
    uint8_t hops = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&node_id),  MESH_UI_STORE_FIELD(&last_heard),
        MESH_UI_STORE_FIELD(&has_hops), MESH_UI_STORE_FIELD(&snr),
        MESH_UI_STORE_FIELD(&via_mqtt), MESH_UI_STORE_FIELD(&hops),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return false;
    }
    node->node_id = node_id;
    node->last_heard = last_heard;
    node->has_hops_away = has_hops;
    node->snr = snr;
    node->via_mqtt = via_mqtt;
    node->hops_away = hops;
    return true;
}

static void load_node_long(struct mesh_ui_node_summary *node, const char *value) {
    if (node != NULL) {
        cache_text(node->long_name, sizeof node->long_name, value);
    }
}

static void load_node_short(struct mesh_ui_node_summary *node, const char *value) {
    if (node != NULL) {
        cache_text(node->short_name, sizeof node->short_name, value);
    }
}

static void load_node_user(struct mesh_ui_node_summary *node, const char *value) {
    if (node != NULL) {
        cache_text(node->user_id, sizeof node->user_id, value);
    }
}

static void load_node_ident(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    uint32_t hw_model = 0U;
    uint32_t role = 0U;
    bool licensed = false;
    bool unmessagable = false;
    bool favorite = false;
    bool ignored = false;
    uint8_t channel = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&hw_model), MESH_UI_STORE_FIELD(&role),
        MESH_UI_STORE_FIELD(&licensed), MESH_UI_STORE_FIELD(&unmessagable),
        MESH_UI_STORE_FIELD(&favorite), MESH_UI_STORE_FIELD(&ignored),
        MESH_UI_STORE_FIELD(&channel),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    node->hw_model = hw_model;
    node->role = role;
    node->is_licensed = licensed;
    node->is_unmessagable = unmessagable;
    node->is_favorite = favorite;
    node->is_ignored = ignored;
    node->channel = channel;
}

static void load_node_state(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    bool has_user = false;
    bool in_nodedb = false;
    bool verified = false;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&has_user),
        MESH_UI_STORE_FIELD(&in_nodedb),
        MESH_UI_STORE_FIELD(&verified),
    };
    /* Two fields or three: a cache from before the verified bit existed leaves it false, which
       is what an unverified key reads as anyway. */
    if (mesh_ui_store_fields_read(value, fields, INKCELL_ARRAY_LEN(fields)) < 2U) {
        return;
    }
    node->has_user = has_user;
    node->in_nodedb = in_nodedb;
    node->key_verified = verified;
}

static void load_node_key(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    size_t len = 0U;
    if (mesh_ui_settings_key_parse(value, node->public_key, sizeof node->public_key, &len)) {
        node->public_key_len = (uint8_t)len;
    }
}

static void load_node_rssi(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    int16_t rssi = 0;
    uint32_t stamp = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&rssi),
        MESH_UI_STORE_FIELD(&stamp),
    };
    /* The stamp joined this line after the reading did, so one field or two: a cache written
       without it still loads, and an unstamped reading reads as current - which is exactly what
       it was before the stamp existed. */
    if (mesh_ui_store_fields_read(value, fields, INKCELL_ARRAY_LEN(fields)) < 1U) {
        return;
    }
    node->has_rssi = true;
    node->rx_rssi = rssi;
    node->rssi_time = stamp;
}

static void load_node_position(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_position position = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&position.latitude_i),     MESH_UI_STORE_FIELD(&position.longitude_i),
        MESH_UI_STORE_FIELD(&position.has_altitude),   MESH_UI_STORE_FIELD(&position.altitude),
        MESH_UI_STORE_FIELD(&position.time),           MESH_UI_STORE_FIELD(&position.sats_in_view),
        MESH_UI_STORE_FIELD(&position.precision_bits), MESH_UI_STORE_FIELD(&position.received),
    };
    /* Seven fields or eight: a cache written before `received` existed still loads, and its
       fixes simply have no arrival time to fall back on.

       A coordinate that does not fit an int32_t is a field that did not read, so it never
       reaches the test below - which is the whole of what the loader's old widest-scan-then-
       bound dance was for. `%d` on text past INT32_MAX is undefined and glibc's answer is 0, so
       scanned narrowly an absurd coordinate arrived at the range check already wearing a valid
       one's clothes and was stored as a fix in the Gulf of Guinea. */
    if (mesh_ui_store_fields_read(value, fields, INKCELL_ARRAY_LEN(fields)) < 7U) {
        return;
    }
    /* The cache is a text file on a card the user can edit, so it is an ingress like the air
       is, and it is held to the same test. A line naming an impossible point leaves the node
       with no fix rather than an absurd one. */
    if (!mesh_geo_coords_valid(position.latitude_i, position.longitude_i)) {
        return;
    }
    position.valid = true;
    node->position = position;
}

static void load_node_metrics(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_metrics metrics = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&metrics.time),
        MESH_UI_STORE_FIELD(&metrics.has_battery),
        MESH_UI_STORE_FIELD(&metrics.battery_level),
        MESH_UI_STORE_FIELD(&metrics.has_voltage),
        MESH_UI_STORE_FIELD(&metrics.voltage),
        MESH_UI_STORE_FIELD(&metrics.has_channel_utilization),
        MESH_UI_STORE_FIELD(&metrics.channel_utilization),
        MESH_UI_STORE_FIELD(&metrics.has_air_util_tx),
        MESH_UI_STORE_FIELD(&metrics.air_util_tx),
        MESH_UI_STORE_FIELD(&metrics.has_uptime),
        MESH_UI_STORE_FIELD(&metrics.uptime_seconds),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    metrics.valid = true;
    node->metrics = metrics;
}

static void load_node_environment(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_environment env = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&env.time),
        MESH_UI_STORE_FIELD(&env.has_temperature),
        MESH_UI_STORE_FIELD(&env.temperature),
        MESH_UI_STORE_FIELD(&env.has_humidity),
        MESH_UI_STORE_FIELD(&env.relative_humidity),
        MESH_UI_STORE_FIELD(&env.has_pressure),
        MESH_UI_STORE_FIELD(&env.barometric_pressure),
        MESH_UI_STORE_FIELD(&env.has_iaq),
        MESH_UI_STORE_FIELD(&env.iaq),
        MESH_UI_STORE_FIELD(&env.has_lux),
        MESH_UI_STORE_FIELD(&env.lux),
        MESH_UI_STORE_FIELD(&env.has_voltage),
        MESH_UI_STORE_FIELD(&env.voltage),
        MESH_UI_STORE_FIELD(&env.has_current),
        MESH_UI_STORE_FIELD(&env.current),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    env.valid = true;
    node->environment = env;
}

/* The header of a neighbour list. The entries arrive on their own slot key below; the count
   this line carries is re-derived from the ones that actually load, so a truncated or
   hand-edited file cannot leave a list claiming rows that are not there. */
static void load_node_neighbors(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_neighbors neighbors = {0};
    uint8_t claimed = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&neighbors.time),
        MESH_UI_STORE_FIELD(&neighbors.broadcast_interval_secs),
        MESH_UI_STORE_FIELD(&claimed),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    neighbors.valid = true;
    node->neighbors = neighbors;
}

static void load_node_neighbor(struct mesh_ui_node_summary *node, uint32_t slot,
                               const char *value) {
    if (node == NULL || slot >= MESH_UI_MAX_NEIGHBORS) {
        return;
    }
    struct mesh_ui_node_neighbor entry = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&entry.node_id),
        MESH_UI_STORE_FIELD(&entry.snr),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields)) || entry.node_id == 0U) {
        return;
    }
    /* Only ever appended, and only to a list the node_nbrs line already opened: a stray entry
       for a node with no header is not half a neighbour list. */
    struct mesh_ui_node_neighbors *neighbors = &node->neighbors;
    if (neighbors->valid && neighbors->count < MESH_UI_MAX_NEIGHBORS) {
        neighbors->entries[neighbors->count] = entry;
        neighbors->count++;
    }
}

static void load_node_power(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_power power = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&power.time),
        MESH_UI_STORE_FIELD(&power.channel[0].has_voltage),
        MESH_UI_STORE_FIELD(&power.channel[0].voltage),
        MESH_UI_STORE_FIELD(&power.channel[0].has_current),
        MESH_UI_STORE_FIELD(&power.channel[0].current),
        MESH_UI_STORE_FIELD(&power.channel[1].has_voltage),
        MESH_UI_STORE_FIELD(&power.channel[1].voltage),
        MESH_UI_STORE_FIELD(&power.channel[1].has_current),
        MESH_UI_STORE_FIELD(&power.channel[1].current),
        MESH_UI_STORE_FIELD(&power.channel[2].has_voltage),
        MESH_UI_STORE_FIELD(&power.channel[2].voltage),
        MESH_UI_STORE_FIELD(&power.channel[2].has_current),
        MESH_UI_STORE_FIELD(&power.channel[2].current),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    power.valid = true;
    node->power = power;
}

static void load_node_air_quality(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_air_quality air = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&air.time),
        MESH_UI_STORE_FIELD(&air.has_pm10),
        MESH_UI_STORE_FIELD(&air.pm10_standard),
        MESH_UI_STORE_FIELD(&air.has_pm25),
        MESH_UI_STORE_FIELD(&air.pm25_standard),
        MESH_UI_STORE_FIELD(&air.has_pm100),
        MESH_UI_STORE_FIELD(&air.pm100_standard),
        MESH_UI_STORE_FIELD(&air.has_co2),
        MESH_UI_STORE_FIELD(&air.co2),
        MESH_UI_STORE_FIELD(&air.has_voc_index),
        MESH_UI_STORE_FIELD(&air.voc_index),
        MESH_UI_STORE_FIELD(&air.has_nox_index),
        MESH_UI_STORE_FIELD(&air.nox_index),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    air.valid = true;
    node->air_quality = air;
}

static void load_node_health(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_health health = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&health.time),        MESH_UI_STORE_FIELD(&health.has_heart_bpm),
        MESH_UI_STORE_FIELD(&health.heart_bpm),   MESH_UI_STORE_FIELD(&health.has_spo2),
        MESH_UI_STORE_FIELD(&health.spo2),        MESH_UI_STORE_FIELD(&health.has_temperature),
        MESH_UI_STORE_FIELD(&health.temperature),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    health.valid = true;
    node->health = health;
}

static void load_node_host(struct mesh_ui_node_summary *node, const char *value) {
    if (node == NULL) {
        return;
    }
    struct mesh_ui_node_host host = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&host.time),           MESH_UI_STORE_FIELD(&host.has_uptime),
        MESH_UI_STORE_FIELD(&host.uptime_seconds), MESH_UI_STORE_FIELD(&host.has_freemem),
        MESH_UI_STORE_FIELD(&host.freemem_kib),    MESH_UI_STORE_FIELD(&host.has_diskfree),
        MESH_UI_STORE_FIELD(&host.diskfree_mib),   MESH_UI_STORE_FIELD(&host.has_load),
        MESH_UI_STORE_FIELD(&host.load1),          MESH_UI_STORE_FIELD(&host.load5),
        MESH_UI_STORE_FIELD(&host.load15),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    host.valid = true;
    node->host = host;
}

static void load_messages_header(struct mesh_ui_store_cache *cache, const char *value) {
    uint32_t count = 0U;
    uint32_t dropped = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&count),
        MESH_UI_STORE_FIELD(&dropped),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    cache->messages_claimed = count;
    cache->messages_claimed_set = true;
    cache->messages.dropped = dropped;
}

/* Where the message came from. What it *is* arrives on msg_meta[] beside it. */
static bool load_message(struct mesh_ui_message *message, const char *value) {
    if (message == NULL) {
        return false;
    }
    uint32_t packet_id = 0U;
    uint32_t peer = 0U;
    uint32_t rx_time = 0U;
    uint8_t channel = 0U;
    uint8_t direction = 0U;
    uint8_t ack = 0U;
    bool broadcast = false;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&packet_id), MESH_UI_STORE_FIELD(&peer),
        MESH_UI_STORE_FIELD(&rx_time),   MESH_UI_STORE_FIELD(&channel),
        MESH_UI_STORE_FIELD(&direction), MESH_UI_STORE_FIELD(&ack),
        MESH_UI_STORE_FIELD(&broadcast),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return false;
    }
    message->packet_id = packet_id;
    message->peer = peer;
    message->rx_time = rx_time;
    message->channel = channel;
    message->direction = direction;
    message->ack = ack;
    message->broadcast = broadcast;
    return true;
}

static void load_message_meta(struct mesh_ui_message *message, const char *value) {
    if (message == NULL) {
        return;
    }
    uint8_t kind = 0U;
    bool pki = false;
    uint32_t reply_id = 0U;
    bool is_reaction = false;
    uint8_t ack_error = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&kind),      MESH_UI_STORE_FIELD(&pki),
        MESH_UI_STORE_FIELD(&reply_id),  MESH_UI_STORE_FIELD(&is_reaction),
        MESH_UI_STORE_FIELD(&ack_error),
    };
    /* Four fields is a record written before the failure reason was kept, five is one written
       since - the shape load_read_mark() takes for the mute it grew, and the rule an archive
       file needs rather than merely deserves: it holds records from every build that ever ran
       on this card. `ack_error` keeps its 0 for the older ones, which is what the bubble
       already reads as "no reason to give". */
    if (mesh_ui_store_fields_read(value, fields, INKCELL_ARRAY_LEN(fields)) < 4U) {
        return;
    }
    message->kind = kind;
    message->pki_encrypted = pki;
    message->reply_id = reply_id;
    message->is_reaction = is_reaction;
    message->ack_error = ack_error;
}

static void load_message_name(struct mesh_ui_message *message, const char *value) {
    if (message != NULL) {
        cache_text(message->peer_name, sizeof message->peer_name, value);
    }
}

static void load_message_relay(struct mesh_ui_message *message, const char *value) {
    if (message != NULL) {
        cache_text(message->relay_name, sizeof message->relay_name, value);
    }
}

static void load_message_text(struct mesh_ui_message *message, const char *value) {
    if (message != NULL) {
        cache_text(message->text, sizeof message->text, value);
    }
}

bool mesh_ui_store_key_is_message(enum mesh_ui_store_key key) {
    switch (key) {
    case MESH_UI_STORE_KEY_MSG:
    case MESH_UI_STORE_KEY_MSG_META:
    case MESH_UI_STORE_KEY_MSG_NAME:
    case MESH_UI_STORE_KEY_MSG_RELAY:
    case MESH_UI_STORE_KEY_MSG_TEXT:
        return true;
    default:
        return false;
    }
}

/*
 * The mirror of mesh_ui_store_write_message(), one line at a time.
 *
 * A line at a time rather than a record at a time because neither caller has a whole record in
 * hand: both are walking a file, and which lines a record turns out to have is only known when
 * the next msg[] line arrives. The return says "this was the line that opens a record, and it
 * read", which is the one thing a caller has to act on - the cache counts rows with it, the
 * archive closes the previous record with it.
 */
bool mesh_ui_store_read_message_line(struct mesh_ui_message *message, enum mesh_ui_store_key key,
                                     const char *value) {
    switch (key) {
    case MESH_UI_STORE_KEY_MSG:
        return load_message(message, value);
    case MESH_UI_STORE_KEY_MSG_META:
        load_message_meta(message, value);
        return false;
    case MESH_UI_STORE_KEY_MSG_NAME:
        load_message_name(message, value);
        return false;
    case MESH_UI_STORE_KEY_MSG_RELAY:
        load_message_relay(message, value);
        return false;
    case MESH_UI_STORE_KEY_MSG_TEXT:
        load_message_text(message, value);
        return false;
    default:
        return false;
    }
}

static void load_read_mark(struct mesh_ui_read_state *state, uint32_t index, const char *value) {
    if (index >= MESH_UI_READ_MARKS_MAX) {
        return;
    }
    struct mesh_ui_read_mark mark = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&mark.kind),  MESH_UI_STORE_FIELD(&mark.channel),
        MESH_UI_STORE_FIELD(&mark.node),  MESH_UI_STORE_FIELD(&mark.packet_id),
        MESH_UI_STORE_FIELD(&mark.muted),
    };
    /* Four fields is a file written before mutes existed, five is one written since; `muted`
       keeps its false either way. And a mark is worth keeping when it carries *either* half - a
       conversation muted before it was ever read has no packet id to name, and dropping it
       would unmute it on the next launch. */
    if (mesh_ui_store_fields_read(value, fields, INKCELL_ARRAY_LEN(fields)) < 4U) {
        return;
    }
    if (mark.packet_id == 0U && !mark.muted) {
        return;
    }
    /* Load order stands in for the stamp nothing saved, which is all the eviction ordering
       needs. */
    mark.stamp = index + 1U;
    state->marks[index] = mark;
    if (index + 1U > state->count) {
        state->count = index + 1U;
    }
}

static void load_airtime(struct mesh_ui_store_cache *cache, uint32_t index, const char *value) {
    if (index >= MESH_UI_HISTORY_AIRTIME_MAX) {
        return;
    }
    struct mesh_ui_store_cached_airtime sample = {0};
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&sample.age_ms),
        MESH_UI_STORE_FIELD(&sample.utilization),
        MESH_UI_STORE_FIELD(&sample.tx),
        MESH_UI_STORE_FIELD(&sample.gap),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields))) {
        return;
    }
    cache->airtime[index] = sample;
    if (index + 1U > cache->airtime_loaded) {
        cache->airtime_loaded = index + 1U;
    }
}

/*
 * The line that opens a route. Everything else about the record arrives on the two slot keys
 * below, and the state is not on the file at all: only a finished trace is ever logged, so a
 * record that loads is a measured route by definition.
 */
static void load_traceroute(struct mesh_ui_store_cache *cache, uint32_t index, const char *value) {
    struct mesh_ui_traceroute *trace = cache_traceroute(cache, index);
    if (trace == NULL) {
        return;
    }
    uint32_t target = 0U;
    uint32_t completed = 0U;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&target),
        MESH_UI_STORE_FIELD(&completed),
    };
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields)) || target == 0U) {
        return;
    }
    memset(trace, 0, sizeof *trace);
    trace->state = MESH_TRACEROUTE_DONE;
    trace->target = target;
    trace->completed = completed;
    if (index + 1U > cache->traceroutes.count) {
        cache->traceroutes.count = (uint8_t)(index + 1U);
    }
}

/* The stop a `trace_hop[i.n]` or `trace_name[i.n]` names, or NULL when the file names one past
   the end of either path - or one in a record no trace[] line opened, which is not half a
   route, the same rule a stray neighbour entry is held to. */
static struct mesh_ui_traceroute_hop *cache_traceroute_hop(struct mesh_ui_store_cache *cache,
                                                           uint32_t index, uint32_t slot) {
    struct mesh_ui_traceroute *trace = cache_traceroute(cache, index);
    if (trace == NULL || trace->target == 0U || slot >= 2U * MESH_UI_TRACEROUTE_MAX_HOPS) {
        return NULL;
    }
    const bool back = slot >= MESH_UI_TRACEROUTE_MAX_HOPS;
    const uint8_t at = (uint8_t)(back ? slot - MESH_UI_TRACEROUTE_MAX_HOPS : slot);
    /* The counts are re-derived here rather than read off the trace[] line, the way a neighbour
       list's count is: a truncated or hand-edited file then leaves a route as long as the stops
       it actually carries rather than one claiming stops that are not there. */
    uint8_t *count = back ? &trace->back_count : &trace->forward_count;
    if (at + 1U > *count) {
        *count = (uint8_t)(at + 1U);
    }
    return back ? &trace->back[at] : &trace->forward[at];
}

static void load_traceroute_hop(struct mesh_ui_store_cache *cache, uint32_t index, uint32_t slot,
                                const char *value) {
    uint32_t node_id = 0U;
    bool has_snr = false;
    int16_t snr = 0;
    const struct mesh_ui_store_field fields[] = {
        MESH_UI_STORE_FIELD(&node_id),
        MESH_UI_STORE_FIELD(&has_snr),
        MESH_UI_STORE_FIELD(&snr),
    };
    /* Read a field wider than it is written and bounded here, because the reading is an int8_t
       and store_fields.h has no i8: a value that does not fit is a hop that does not load. */
    if (!cache_fields(value, fields, INKCELL_ARRAY_LEN(fields)) || snr < INT8_MIN ||
        snr > INT8_MAX) {
        return;
    }
    struct mesh_ui_traceroute_hop *hop = cache_traceroute_hop(cache, index, slot);
    if (hop == NULL) {
        return;
    }
    hop->node_id = node_id;
    hop->has_snr = has_snr;
    hop->snr_quarter_db = (int8_t)snr;
}

static void load_traceroute_hop_name(struct mesh_ui_store_cache *cache, uint32_t index,
                                     uint32_t slot, const char *value) {
    struct mesh_ui_traceroute_hop *hop = cache_traceroute_hop(cache, index, slot);
    if (hop != NULL) {
        cache_text(hop->name, sizeof hop->name, value);
    }
}

/* One line: a key, an index or two, and a value. Everything the format knows how to do is one
   case of this switch, which is why it has no `default` - see the note at the top of the file. */
static void load_line(struct mesh_ui_store_cache *cache, const char *key, char *value) {
    uint32_t index = 0U;
    uint32_t slot = 0U;
    const enum mesh_ui_store_key id = mesh_ui_store_key_lookup(key, &index, &slot);
    switch (id) {
    case MESH_UI_STORE_KEY_HANDSHAKE_VALID:
        (void)cache_field(value, MESH_UI_STORE_FIELD(&cache->handshake_valid));
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_REQUEST:
        load_handshake_request(&cache->handshake, value);
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_CONFIG:
        load_handshake_config(&cache->handshake, value);
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_MYNODE:
        load_handshake_mynode(&cache->handshake, value);
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_ROSTER:
        (void)cache_field(value, MESH_UI_STORE_FIELD(&cache->handshake.roster_owner));
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_CHANNEL:
        cache_text(cache->handshake.primary_channel, sizeof cache->handshake.primary_channel,
                   value);
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_MY_SHORT:
        cache_text(cache->handshake.my_short_name, sizeof cache->handshake.my_short_name, value);
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_CACHED:
        (void)cache_field(value, MESH_UI_STORE_FIELD(&cache->handshake.cached));
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_CHANNELS:
        load_handshake_channels(&cache->handshake, value);
        break;
    case MESH_UI_STORE_KEY_HANDSHAKE_NODES:
        cache->nodes_claimed_set = cache_field(value, MESH_UI_STORE_FIELD(&cache->nodes_claimed));
        break;

    case MESH_UI_STORE_KEY_CHANNEL:
        load_channel(cache_channel(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_CHANNEL_NAME:
        load_channel_name(cache_channel(cache, index), value);
        break;

    case MESH_UI_STORE_KEY_NODE:
        if (load_node(cache_node(cache, index), value) && index + 1U > cache->nodes_loaded) {
            cache->nodes_loaded = index + 1U;
        }
        break;
    case MESH_UI_STORE_KEY_NODE_LONG:
        load_node_long(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_SHORT:
        load_node_short(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_USER:
        load_node_user(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_IDENT:
        load_node_ident(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_STATE:
        load_node_state(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_KEY:
        load_node_key(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_RSSI:
        load_node_rssi(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_POS:
        load_node_position(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_METRICS:
        load_node_metrics(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_ENV:
        load_node_environment(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_NBRS:
        load_node_neighbors(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_NBR:
        load_node_neighbor(cache_node(cache, index), slot, value);
        break;
    case MESH_UI_STORE_KEY_NODE_POWER:
        load_node_power(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_AIR:
        load_node_air_quality(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_HEALTH:
        load_node_health(cache_node(cache, index), value);
        break;
    case MESH_UI_STORE_KEY_NODE_HOST:
        load_node_host(cache_node(cache, index), value);
        break;

    case MESH_UI_STORE_KEY_MESSAGES:
        load_messages_header(cache, value);
        break;
    /* The five that make up a message, read through the codec the archive also reads: the
       return is true only for the msg[] line that opens a record, which is the one that decides
       how many rows this file actually carried. */
    case MESH_UI_STORE_KEY_MSG:
    case MESH_UI_STORE_KEY_MSG_META:
    case MESH_UI_STORE_KEY_MSG_NAME:
    case MESH_UI_STORE_KEY_MSG_RELAY:
    case MESH_UI_STORE_KEY_MSG_TEXT:
        if (mesh_ui_store_read_message_line(cache_message(cache, index), id, value) &&
            index + 1U > cache->messages_loaded) {
            cache->messages_loaded = index + 1U;
        }
        break;

    case MESH_UI_STORE_KEY_READ:
        load_read_mark(&cache->read_state, index, value);
        break;
    case MESH_UI_STORE_KEY_AIRTIME:
        load_airtime(cache, index, value);
        break;

    case MESH_UI_STORE_KEY_TRACE:
        load_traceroute(cache, index, value);
        break;
    case MESH_UI_STORE_KEY_TRACE_HOP:
        load_traceroute_hop(cache, index, slot, value);
        break;
    case MESH_UI_STORE_KEY_TRACE_HOP_NAME:
        load_traceroute_hop_name(cache, index, slot, value);
        break;

    /* Written every save and read by nobody, on purpose: both are counts, and the loader
       re-derives each from the rows that actually load so a truncated or hand-edited file
       cannot leave a list claiming rows that are not there. */
    case MESH_UI_STORE_KEY_READ_MARKS:
    case MESH_UI_STORE_KEY_AIRTIME_COUNT:
    /* Not this file's at all: a node's trend log next door writes it and store_trends.c reads
       it. mesh_ui_store_key_in_cache() is where that is said once; the case is here because the
       switch has no `default` and every key in the table has to be answered. */
    case MESH_UI_STORE_KEY_TREND:
    /* A line this build has no key for: a cache from a newer client, or one edited by hand.
       Skipped, which is what makes the format forward-compatible. */
    case MESH_UI_STORE_KEY_NONE:
    case MESH_UI_STORE_KEY_COUNT:
        break;
    }
}

/*
 * The airtime trend, put back on a timeline of its own.
 *
 * The oldest sample sits at zero and the rest follow their saved spacing, so the series is the
 * same shape it was written as. mesh_ui_history_resume() then fits the live clock to it and
 * lifts the pen over the seam - what the client did while it was not running is the one thing
 * the cache cannot say, and a Brick has no clock that could measure it.
 *
 * This is what makes the Mesh card's chart survive a relaunch. Without it the trend starts
 * empty every time, the radio's LocalStats is a quarter of an hour apart, and the card is
 * offered no verb for the first half hour of every session - which on the device is
 * indistinguishable from a card that does nothing at all.
 */
static void restore_airtime(struct mesh_ui_history *history,
                            const struct mesh_ui_store_cache *cache) {
    if (cache->airtime_loaded == 0U) {
        return;
    }
    uint32_t span_ms = 0U;
    for (uint32_t i = 0U; i < cache->airtime_loaded; ++i) {
        if (cache->airtime[i].age_ms > span_ms) {
            span_ms = cache->airtime[i].age_ms;
        }
    }
    for (uint32_t i = 0U; i < cache->airtime_loaded; ++i) {
        const struct mesh_ui_store_cached_airtime *sample = &cache->airtime[i];
        /* Above MESH_UI_HISTORY_EPOCH_MS rather than from zero: the timeline the rest of this
           run rides is fitted to these samples, and a node's trend read off the card later is
           placed *behind* the live clock, so the clock cannot start at the bottom of its range.
           Nothing here reads a stamp as anything but a difference, so the shift is free. */
        mesh_ui_history_restore_airtime(history,
                                        MESH_UI_HISTORY_EPOCH_MS + span_ms - sample->age_ms,
                                        sample->utilization, sample->tx, sample->gap);
    }
    mesh_ui_history_resume(history, MESH_UI_HISTORY_RADIO_GAP_MS);
}

/* What the file collected, made into the store the first frame draws from. */
static void commit(struct mesh_ui_store *store, struct mesh_ui_store_cache *cache) {
    /*
     * The header's count, but never more rows than actually arrived - the rule read_marks[] has
     * always followed, for the reason given beside it in store_keys.def.
     *
     * `handshake_nodes` is written before the rows it counts, so a file cut short between the
     * two claims a roster it does not have, and the difference lands in the published list as
     * rows with a node id of 0: blank lines on the Nodes screen, and a count above them that
     * disagrees with what the reader can see. A truncated cache should be a shorter roster,
     * not a roster with holes in it.
     */
    uint32_t node_count = cache->nodes_claimed_set ? cache->nodes_claimed : cache->nodes_loaded;
    if (cache->nodes_claimed_set && node_count > cache->nodes_loaded) {
        node_count = cache->nodes_loaded;
    }
    if (node_count > MESH_UI_MAX_HANDSHAKE_NODES) {
        node_count = MESH_UI_MAX_HANDSHAKE_NODES;
    }
    cache->handshake.node_count = node_count;
    /* The cache holds only the 128 that were published, so for as long as it is all we have,
       what we know and what we show are the same number. The first publish after the session
       is seeded replaces it with the roster's own total. */
    cache->handshake.nodes_known = node_count;
    /* The two forget counts are deliberately left at zero: they describe what the *session's*
       roster would lose, and the session is seeded from this cache a moment later, so the
       first publish fills them from the roster itself rather than from the 128 rows here. */

    if (cache->handshake_valid) {
        cache->handshake.cached = true;
        store->handshake = cache->handshake;
        store->handshake_valid = true;
    } else {
        memset(&store->handshake, 0, sizeof store->handshake);
        store->handshake_valid = false;
    }

    /* Clamped to the rows that arrived, for the reason the roster above is. */
    uint32_t message_count =
        cache->messages_claimed_set ? cache->messages_claimed : cache->messages_loaded;
    if (cache->messages_claimed_set && message_count > cache->messages_loaded) {
        message_count = cache->messages_loaded;
    }
    if (message_count > MESH_UI_MAX_MESSAGES) {
        message_count = MESH_UI_MAX_MESSAGES;
    }
    cache->messages.count = message_count;
    store->messages = cache->messages;

    /* File order stands in for the ordering nothing saved; the revision starts where a freshly
       initialised store's does, because what was just loaded *is* what the file holds. */
    cache->read_state.stamp = cache->read_state.count;
    cache->read_state.revision = 0U;
    store->read_state = cache->read_state;

    restore_airtime(&store->history, cache);

    /* Straight across: a route is the one thing in this file that needs no fitting to a clock,
       because its stamp is the radio's rather than ours. The revision stays at zero - what was
       just loaded *is* what the card holds, so nothing here is worth writing back. */
    store->traceroutes = cache->traceroutes;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_HANDSHAKE | MESH_UI_UPDATE_MESSAGES);
}

int mesh_ui_store_load(struct mesh_ui_store *store, const char *path) {
    if (store == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    /* On the stack, as the four records it replaced were: the roster alone is 68 KB, and the
       client is single-threaded with the main thread's own stack under it. */
    struct mesh_ui_store_cache cache;
    memset(&cache, 0, sizeof cache);

    char line[1280];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        char *value = equals + 1;
        mesh_ui_store_unescape_value(value);
        load_line(&cache, line, value);
    }

    fclose(file);

    commit(store, &cache);
    return 0;
}
